#!/usr/bin/python3
"""Finite-volume DC screening of actual saved copper and through-via barrels.
Not an electrothermal/AC/IPC-2152 qualification. 0.1mm grid; nodes require copper
at their centre. Narrow-feature discretisation is a stated limitation. Reports
unreachable terminals instead of substituting a fictitious supply.
"""
import pathlib,json,math,sys,collections
import pcbnew as p
import numpy as np
from shapely.geometry import Polygon,Point,LineString,box
from shapely.ops import unary_union
from shapely import contains_xy
from scipy.sparse import coo_matrix,diags
from scipy.sparse.csgraph import connected_components
from scipy.sparse.linalg import cg
R=pathlib.Path(__file__).resolve().parents[1];b=p.LoadBoard(str(R.parents[1] / 'kicad/companion/companion.kicad_pcb'));mm=p.FromMM;to=p.ToMM;step=.10;layers=[p.F_Cu,p.In1_Cu,p.In2_Cu,p.B_Cu];thick=[.035,.0152,.0152,.035];rho=1.724e-5;zpos=[0,.2454,.5106,.756]
fs={f.GetReference():f for f in b.GetFootprints()};pads=[a for f in fs.values() for a in f.Pads()];tracks=list(b.GetTracks())
def polyset(ps):
    gs=[]
    for i in range(ps.OutlineCount()):
        c=ps.COutline(i);pts=[(to(c.CPoint(j).x),to(c.CPoint(j).y)) for j in range(c.PointCount())]
        if len(pts)<3:continue
        holes=[]
        for k in range(ps.HoleCount(i)):
            h=ps.CHole(i,k);holes.append([(to(h.CPoint(j).x),to(h.CPoint(j).y)) for j in range(h.PointCount())])
        gs.append(Polygon(pts,holes))
    return unary_union(gs)
def land(a):return polyset(a.GetEffectivePolygon(p.F_Cu))
def location(a):return(to(a.GetPosition().x),to(a.GetPosition().y))
cases={
'/Power/VBAT':([('U2','1')],[('U1','13',2.),('U1','14',2.)]),
'/Power/VSYS':([('U1','15'),('U1','16')],[('QH2','8',3.5),('U4','10',.6)]),
'/Power/USB_5V':([('USBC1','A4B9'),('USBC1','B4A9')],[('U1','1',1.),('U1','24',1.)]),
'/Modem/MODEM_3V8':([('U5','11'),('U5','12'),('U5','13'),('U5','26')],[('U10','32',.5),('U10','33',.5),('U10','52',1.),('U10','53',1.)]),
'/ESP32/SYS_3V3':([('U4','6')],[('U7','2',.6)])}
if len(sys.argv)>1:cases={n:cases[n] for n in sys.argv[1:]}
report={}
for name,(sources,sinks) in cases.items():
    polys=[]
    for layer in layers:
        gs=[land(a) for a in pads if a.GetNetname()==name and a.IsOnLayer(layer)]
        for t in tracks:
            if t.GetNetname()!=name:continue
            if isinstance(t,p.PCB_VIA):gs.append(Point(location(t)).buffer(to(t.GetWidth(layer))/2))
            elif t.GetLayer()==layer:gs.append(LineString([location(t),(to(t.GetEnd().x),to(t.GetEnd().y))]).buffer(to(t.GetWidth())/2))
        gs.extend(polyset(z.GetFilledPolysList(layer)) for z in b.Zones() if not z.GetIsRuleArea() and z.GetNetname()==name and z.GetLayer()==layer)
        polys.append(unary_union(gs))
    bounds=unary_union(polys).bounds;x0=math.floor(bounds[0]/step)*step;y0=math.floor(bounds[1]/step)*step;nx=math.ceil((bounds[2]-x0)/step)+1;ny=math.ceil((bounds[3]-y0)/step)+1;xx,yy=np.meshgrid(x0+np.arange(nx)*step,y0+np.arange(ny)*step)
    occupied=np.array([contains_xy(g.buffer(.00001),xx,yy) for g in polys]);idx=np.full(occupied.shape,-1,np.int32);idx[occupied]=np.arange(np.count_nonzero(occupied));N=np.count_nonzero(occupied);us=[];vs=[];gs=[]
    for li in range(4):
        for a,c in [(idx[li,:,:-1],idx[li,:,1:]),(idx[li,:-1,:],idx[li,1:,:])]:
            ok=(a>=0)&(c>=0);us.extend(a[ok]);vs.extend(c[ok]);gs.extend([thick[li]/rho]*np.count_nonzero(ok))
    for t in tracks:
        if not isinstance(t,p.PCB_VIA) or t.GetNetname()!=name:continue
        x,y=location(t);ix=round((x-x0)/step);iy=round((y-y0)/step);drill=to(t.GetDrillValue());area=math.pi*drill*.020
        for li in range(3):
            a=idx[li,iy,ix];c=idx[li+1,iy,ix]
            if a>=0 and c>=0:us.append(a);vs.append(c);gs.append(area/(rho*(zpos[li+1]-zpos[li])))
    u=np.array(us,dtype=np.int32);v=np.array(vs,dtype=np.int32);g=np.array(gs);A=coo_matrix((np.r_[g,g,-g,-g],(np.r_[u,v,u,v],np.r_[u,v,v,u])),shape=(N,N)).tocsr()
    def terminal(ref,pin):
        a=[a for a in fs[ref].Pads() if a.GetNumber()==pin and a.GetNetname()==name]
        if not a:return np.array([],dtype=np.int32)
        pg=unary_union([land(q) for q in a]);mask=contains_xy(pg.buffer(.001),xx,yy);ns=idx[0][mask];return ns[ns>=0]
    src=np.unique(np.concatenate([terminal(*s) for s in sources]));ncomp,labels=connected_components(A,directed=False);reachable=np.isin(labels,labels[src]);sinkrows=[];rhs=np.zeros(N);unreachable=[]
    for ref,pin,amps in sinks:
        ns=terminal(ref,pin);ns=ns[reachable[ns]]
        sinkrows.append((ref,pin,amps,ns))
        if len(ns):rhs[ns]+=amps/len(ns)
        else:unreachable.append(ref+'.'+pin)
    row=dict(grid_mm=step,nodes=int(N),mesh_components=int(ncomp),source_mesh_nodes=len(src),unreachable_terminals=unreachable,source_pads=sources,barrel_plating_assumed_mm=.020,scope='DC screening only; discretisation excludes sub-grid necks, no thermal/AC qualification')
    if len(src) and not unreachable:
        active=reachable.copy();active[src]=False;AA=A[active][:,active];rr=rhs[active];scale=AA.diagonal();M=diags(1/np.maximum(scale,1e-20));sol,info=cg(AA,rr,M=M,rtol=1e-7,maxiter=12000);volts=np.zeros(N);volts[active]=sol
        row.update(solver_info=int(info),sink_drops_mV=[dict(ref=r,pad=n,current_A=a,drop_mV=float(np.mean(volts[ns])*1e3)) for r,n,a,ns in sinkrows],max_drop_mV=float(np.max(volts)*1e3),joule_power_W=float(np.dot(volts,rhs)))
    report[name]=row;print(name,json.dumps(row),flush=True)
(R/'reports/power-mesh-resume.json').write_text(json.dumps(report,indent=2))
