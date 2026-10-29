#!/usr/bin/python3
"""Add clearance-filled copper to actual high-current trunks and dogbone vias.
No component movement or rule changes. No claim that polygon declarations alone
meet ampacity: final saved fill and voltage/current paths require audit.
"""
import pathlib,json,math,shutil
import pcbnew as p
from shapely.geometry import Polygon,Point,LineString,box
from shapely.ops import unary_union
R=pathlib.Path(__file__).resolve().parents[1];file=R.parents[1] / 'kicad/companion/companion.kicad_pcb';b=p.LoadBoard(str(file));to=p.ToMM;mm=p.FromMM
fs={f.GetReference():f for f in b.GetFootprints()};pads=[a for f in fs.values() for a in f.Pads()];tracks=list(b.GetTracks());board=box(50.25,50.25,131.75,88.25)
def xy(q):return p.VECTOR2I(mm(q[0]),mm(q[1]))
def pos(a):return (to(a.GetPosition().x),to(a.GetPosition().y))
def land(a):
    r=a.GetBoundingBox();return box(to(r.GetLeft()),to(r.GetTop()),to(r.GetRight()),to(r.GetBottom()))
def cu(t):
    return Point(pos(t)).buffer(to(t.GetWidth(p.F_Cu))/2) if isinstance(t,p.PCB_VIA) else LineString([pos(t),(to(t.GetEnd().x),to(t.GetEnd().y))]).buffer(to(t.GetWidth())/2)
obstacles=[(land(a),a.GetNetCode(),bool(a.GetDrillSize().x)) for a in pads];copper=[(cu(t),t.GetNetCode(),t) for t in tracks];holes=[(pos(t),to(t.GetDrillValue())/2) for t in tracks if isinstance(t,p.PCB_VIA)]
# These are power terminals, not arbitrary net endpoints/bias pullups.
trunks={
'/Power/VBAT':[('U2','1'),('U1','13'),('U1','14'),('C7','2')],
'/Power/VSYS':[('L3','2'),('U1','15'),('U1','16'),('QH2','8'),('C25','1'),('C26','1'),('U4','1'),('U4','10')],
'/Power/USB_5V':[('USBC1','A4B9'),('USBC1','B4A9'),('U1','1'),('U1','24'),('C1','2'),('C15','2')],
'/ESP32/SYS_3V3':[('U4','6'),('C10','1'),('U7','2')],
'/Modem/MODEM_3V8':[('U5','11'),('U5','12'),('U5','13'),('U10','32'),('U10','33'),('U10','52'),('U10','53'),('C39','1'),('C68','1'),('C32','1'),('C33','1'),('C34','1')],
'/Power/LTE_SW2':[('L4','2'),('U5','21'),('U5','25'),('C28','2')]}
added=[];rejected=[]
for name,targets in trunks.items():
    code=b.FindNet(name).GetNetCode()
    for ref,num in targets:
        candidates=[a for a in fs[ref].Pads() if a.GetNumber()==num and a.GetNetCode()==code]
        if not candidates:raise RuntimeError(f'Wrong terminal {name} {ref}.{num}')
        a=max(candidates,key=lambda a:land(a).area);x,y=pos(a)
        # Existing full-depth pad/nearby via already supplies this terminal.
        if any(t.GetNetCode()==code and Point(pos(t)).distance(land(a))<.05 for t in tracks if isinstance(t,p.PCB_VIA)):continue
        good=None
        for radius in (.8,1.,1.2,1.4,1.6):
            for angle in range(0,360,45):
                xx=x+radius*math.cos(math.radians(angle));yy=y+radius*math.sin(math.radians(angle));q=Point(xx,yy);vg=q.buffer(.3);line=LineString([(x,y),(xx,yy)]);width=min(.6,max(.20,min(to(a.GetBoundingBox().GetWidth()),to(a.GetBoundingBox().GetHeight()))*.8));tg=line.buffer(width/2)
                if not board.covers(vg) or any(vg.distance(g)<.04 for g,n,h in obstacles):continue
                if any(n!=code and (vg.distance(g)<.127 or tg.distance(g)<(.2 if h else .105)) for g,n,h in obstacles):continue
                if any(n!=code and (vg.distance(g)<.205 or (not isinstance(t,p.PCB_VIA) and t.GetLayer()==p.F_Cu and tg.distance(g)<.095) or (isinstance(t,p.PCB_VIA) and tg.distance(g)<.205)) for g,n,t in copper):continue
                if any(math.dist((xx,yy),qq)<.15+rr+.26 for qq,rr in holes):continue
                good=(xx,yy,width);break
            if good:break
        if good is None:rejected.append([ref,num,name]);continue
        xx,yy,width=good;v=p.PCB_VIA(b);v.SetPosition(xy((xx,yy)));v.SetWidth(mm(.6));v.SetDrill(mm(.3));v.SetViaType(p.VIATYPE_THROUGH);v.SetLayerPair(p.F_Cu,p.B_Cu);v.SetNetCode(code);b.Add(v)
        t=p.PCB_TRACK(b);t.SetStart(a.GetPosition());t.SetEnd(v.GetPosition());t.SetWidth(mm(width));t.SetLayer(p.F_Cu);t.SetNetCode(code);b.Add(t);holes.append(((xx,yy),.15));copper.extend([(cu(v),code,v),(cu(t),code,t)]);added.append(dict(ref=ref,pad=num,net=name,pos=[xx,yy],stub_width_mm=width))
# Broad B.Cu trunk corridors supply parallel copper to the narrow autorouter
# paths. Clearance refill retains every foreign route and the solid In1 GND.
for priority,(name,targets) in enumerate(trunks.items(),20):
    code=b.FindNet(name).GetNetCode();ps=list(dict.fromkeys(pos(a) for ref,num in targets for a in fs[ref].Pads() if a.GetNumber()==num and a.GetNetCode()==code));reached={0};edges=[]
    while len(reached)<len(ps):
        _,i,j=min((math.dist(ps[i],ps[j]),i,j) for i in reached for j in range(len(ps)) if j not in reached);edges.append(LineString([ps[i],ps[j]]));reached.add(j)
    radius=1.5 if name!='/ESP32/SYS_3V3' else .6
    g=unary_union([e.buffer(radius,quad_segs=4) for e in edges]+[Point(q).buffer(radius,quad_segs=4) for q in ps]).intersection(board)
    for layer in [p.B_Cu,p.F_Cu]:
        shape=g if layer==p.B_Cu else unary_union([land(a).buffer(.45) for ref,num in targets for a in fs[ref].Pads() if a.GetNumber()==num and a.GetNetCode()==code]).intersection(board)
        z=p.ZONE(b);z.SetLayer(layer);z.SetNetCode(code);z.SetAssignedPriority(priority);z.SetLocalClearance(mm(.2));z.SetMinThickness(mm(.2));z.SetPadConnection(p.ZONE_CONNECTION_FULL);z.SetIslandRemovalMode(p.ISLAND_REMOVAL_MODE_ALWAYS)
        poly=z.Outline()
        for geo in ([shape] if shape.geom_type=='Polygon' else shape.geoms):
            poly.NewOutline()
            for x,y in list(geo.exterior.coords)[:-1]:poly.Append(mm(x),mm(y))
            for ring in geo.interiors:
                poly.NewHole()
                for x,y in list(ring.coords)[:-1]:poly.Append(mm(x),mm(y))
        b.Add(z)
shutil.copy2(file,R/'checkpoints/pre-resume-reinforcement.kicad_pcb');p.SaveBoard(str(file),b)
(R/'reports/resume-reinforcement.json').write_text(json.dumps(dict(added_dogbone_vias=added,rejected_dogbone_terminals=rejected,trunks=trunks),indent=2));print('Power reinforcement:',len(added),'non-via-in-pad dogbones;',len(rejected),'blocked sites; no ampacity-pass claim')
