#!/usr/bin/python3
"""Pre-route RF on F.Cu, declare reference/power planes, export Specctra.
Never changes schematic nets, deletes functional pads, or disables DRC rules.
"""
import pathlib,json,math,heapq,itertools,collections,sys
import numpy as np
import pcbnew as p
ROOT=pathlib.Path(__file__).resolve().parents[1]; cfg=json.loads((ROOT/'scripts/layout-revision.json').read_text())
path=ROOT.parents[1] / 'kicad/companion/companion.kicad_pcb'; b=p.LoadBoard(str(path)); mm=p.FromMM; to=p.ToMM; O=cfg['origin']; W,H=cfg['size']
fs={f.GetReference():f for f in b.GetFootprints()}
def xy(x,y): return p.VECTOR2I(mm(x+O[0]),mm(y+O[1]))
def pos(a): return (to(a.GetPosition().x)-O[0],to(a.GetPosition().y)-O[1])
def padrect(a):
    r=a.GetBoundingBox();return (to(r.GetLeft())-O[0],to(r.GetTop())-O[1],to(r.GetRight())-O[0],to(r.GetBottom())-O[1])

def track(net,points,width=.29,layer=p.F_Cu,locked=True):
    for a,c in zip(points,points[1:]):
        if math.dist(a,c)<1e-6: continue
        t=p.PCB_TRACK(b);t.SetStart(xy(*a));t.SetEnd(xy(*c));t.SetWidth(mm(width));t.SetLayer(layer);t.SetNetCode(net);t.SetLocked(locked);b.Add(t)

padlist=[a for f in b.GetFootprints() for a in f.Pads()]

def astar(start,end,net,width=.29,margin=5,step=.05):
    lo=(max(.2,min(start[0],end[0])-margin),max(.2,min(start[1],end[1])-margin))
    hi=(min(W-.2,max(start[0],end[0])+margin),min(H-.2,max(start[1],end[1])+margin))
    lo=(math.floor(lo[0]/step)*step,math.floor(lo[1]/step)*step)
    nx=int((hi[0]-lo[0])/step)+1;ny=int((hi[1]-lo[1])/step)+1
    blocked=np.zeros((ny,nx),bool); clearance=.205+width/2
    def rect(r,expand):
        x0=max(0,math.floor((r[0]-expand-lo[0])/step));x1=min(nx-1,math.ceil((r[2]+expand-lo[0])/step))
        y0=max(0,math.floor((r[1]-expand-lo[1])/step));y1=min(ny-1,math.ceil((r[3]+expand-lo[1])/step))
        if x0<=x1 and y0<=y1: blocked[y0:y1+1,x0:x1+1]=True
    for a in padlist:
        if a.GetNetCode()!=net and (a.IsOnLayer(p.F_Cu) or a.GetDrillSize().x): rect(padrect(a),clearance)
    for t in b.GetTracks():
        if t.GetLayer()!=p.F_Cu or t.GetNetCode()==net:continue
        a=pos(t);c=(to(t.GetEnd().x)-O[0],to(t.GetEnd().y)-O[1]);ex=clearance+to(t.GetWidth())/2
        # Segment distance rasterization, not an entire diagonal's bounding box.
        for q in range(max(2,int(math.dist(a,c)/step)*2)):
            f=q/max(1,max(2,int(math.dist(a,c)/step)*2)-1);x=a[0]+f*(c[0]-a[0]);y=a[1]+f*(c[1]-a[1]);rect((x,y,x,y),ex)
    conv=lambda q:(round((q[0]-lo[0])/step),round((q[1]-lo[1])/step))
    s=conv(start);e=conv(end)
    if blocked[s[1],s[0]] or blocked[e[1],e[0]]: return None
    cost={s:0};prev={};q=[(math.dist(s,e),0,s)];moves=[(a,c,math.hypot(a,c)) for a,c in ((1,0),(-1,0),(0,1),(0,-1),(1,1),(1,-1),(-1,1),(-1,-1))]
    while q:
        _,g,u=heapq.heappop(q)
        if g>cost.get(u,1e99):continue
        if u==e:break
        for dx,dy,dc in moves:
            v=(u[0]+dx,u[1]+dy)
            if not(0<=v[0]<nx and 0<=v[1]<ny) or blocked[v[1],v[0]]:continue
            if dx and dy and (blocked[u[1],v[0]] or blocked[v[1],u[0]]):continue
            ng=g+dc
            if ng>=cost.get(v,1e99):continue
            cost[v]=ng;prev[v]=u;heapq.heappush(q,(ng+math.dist(v,e),ng,v))
    if e not in cost:return None
    pts=[e]
    while pts[-1]!=s:pts.append(prev[pts[-1]])
    pts.reverse();points=[(lo[0]+x*step,lo[1]+y*step) for x,y in pts]
    # Collapse collinear points; preserve exact solder-pad endpoints.
    result=[start,points[0]]
    for i in range(1,len(points)-1):
        a,c,d=points[i-1:i+2]
        if abs((c[0]-a[0])*(d[1]-c[1])-(c[1]-a[1])*(d[0]-c[0]))>1e-8:result.append(c)
    result.extend((points[-1],end));return result

rf_names=['/Modem/LTE_ANT','Net-(J_LTE1-SIN)','/Modem/GNSS_ANT','Net-(C50-Pad1)','Net-(J_GNSS1-SIN)']
route_log=[]
for name in rf_names:
    net=b.FindNet(name).GetNetCode(); pads=[a for a in padlist if a.GetNetCode()==net]
    if any(not a.IsOnLayer(p.F_Cu) for a in pads):raise RuntimeError(f'RF pad on wrong side: {name}')
    connected={0};edges=[]
    while len(connected)<len(pads):
        _,i,j=min((math.dist(pos(pads[i]),pos(pads[j])),i,j) for i in connected for j in range(len(pads)) if j not in connected)
        start,end=pos(pads[i]),pos(pads[j]);points=astar(start,end,net)
        if points is None:points=astar(start,end,net,margin=15)
        if points is None:raise RuntimeError(f'No legal top-only RF path: {name}: {start}->{end}')
        track(net,points);length=sum(math.dist(a,c) for a,c in zip(points,points[1:]));route_log.append(dict(net=name,start=start,end=end,length_mm=length,segments=len(points)-1));print(name,length,flush=True)
        connected.add(j)

# Ground plane and modem distribution plane. L2 contains no routed signals.
def zone(name,layer,rect,clearance=.2):
    z=p.ZONE(b);z.SetLayer(layer);z.SetNetCode(b.FindNet(name).GetNetCode());z.SetLocalClearance(mm(clearance));z.SetMinThickness(mm(.15));z.SetPadConnection(p.ZONE_CONNECTION_FULL);z.SetIslandRemovalMode(p.ISLAND_REMOVAL_MODE_ALWAYS)
    poly=z.Outline();poly.NewOutline()
    for x,y in ((rect[0],rect[1]),(rect[2],rect[1]),(rect[2],rect[3]),(rect[0],rect[3])):poly.Append(int(mm(x+O[0])),int(mm(y+O[1])))
    b.Add(z);return z
zone('GND',p.In1_Cu,(0,0,W,H),.15)
zone('/Modem/MODEM_3V8',p.In2_Cu,(28,.25,W-.25,H-.25),.2)
zone('/Power/VSYS',p.In2_Cu,(.25,.25,27.6,H-.25),.2)
zone('GND',p.F_Cu,(0,0,W,H),.25)
zone('GND',p.B_Cu,(0,0,W,H),.2)
# The actual unsupported antenna only. External-metal clearance deviations are
# individually documented in mechanical-revision.md; no global battery gap.
for layer in (p.F_Cu,p.In1_Cu,p.In2_Cu,p.B_Cu):
    z=p.ZONE(b);z.SetLayer(layer);z.SetIsRuleArea(True);z.SetDoNotAllowTracks(True);z.SetDoNotAllowVias(True);z.SetDoNotAllowPads(True);z.SetDoNotAllowFootprints(False);z.SetDoNotAllowZoneFills(True)
    ps=z.Outline();ps.NewOutline();a=cfg['antenna']
    for x,y in ((a[0],a[1]),(a[2],a[1]),(a[2],a[3]),(a[0],a[3])):ps.Append(mm(x+O[0]),mm(y+O[1]))
    b.Add(z)
# Description-layer battery envelope, outside the local antenna projection.
bx0,by0,bx1,by1=cfg['battery']['rect']
for a,c in [((bx0,by0),(bx1,by0)),((bx1,by0),(bx1,by1)),((bx1,by1),(bx0,by1)),((bx0,by1),(bx0,by0))]:
    sh=p.PCB_SHAPE();sh.SetShape(p.SHAPE_T_SEGMENT);sh.SetLayer(p.User_1);sh.SetStart(xy(*a));sh.SetEnd(xy(*c));sh.SetWidth(mm(.1));b.Add(sh)
p.SaveBoard(str(path),b)
(ROOT/'reports/critical-routing.json').write_text(json.dumps(route_log,indent=2))
# Reload before fill/export: the saved KiCad file is the downstream contract.
b=p.LoadBoard(str(path)); zones=p.ZONES()
for z in b.Zones():zones.push_back(z)
p.ZONE_FILLER(b).Fill(zones);p.SaveBoard(str(path),b)
(ROOT/'routing').mkdir(exist_ok=True)
assert p.ExportSpecctraDSN(b,str(ROOT/'routing/revision.dsn'))
print('Exported',ROOT/'routing/revision.dsn',flush=True)
