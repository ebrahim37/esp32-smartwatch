#!/usr/bin/python3
"""Orient common switching electrodes toward each other and reserve broad copper.
Uses actual pad nets. In1 stays uninterrupted GND; In2 VSYS ends35.5mm, modem
power begins35.9mm, placing the input FET above its real input plane. A reserved
B.Cu switching corridor and filled/capped via arrays carry the inductor current.
No clearance or DRC severity is weakened; every emitted site is geometrically
screened and subsequent KiCad DRC remains authoritative.
"""
import pathlib,json,shutil,math
import pcbnew as p
from shapely.geometry import box,LineString,Point,Polygon
from shapely.ops import unary_union
R=pathlib.Path(__file__).resolve().parents[1];path=R.parents[1] / 'kicad/companion/companion.kicad_pcb';b=p.LoadBoard(str(path));mm=p.FromMM;to=p.ToMM;fs={f.GetReference():f for f in b.GetFootprints()};tracks=list(b.GetTracks());zones=list(b.Zones());cfg=json.loads((R/'scripts/layout-revision.json').read_text())
poses={'QH2':(32.5,14.5,180),'QL2':(32.5,19.65,0),'C27':(37.75,11.6,180),'C28':(39.8,11.6,180),'C35':(42.8,11.5,0),'C42':(51.25,34.25,0)}
old={r:[to(fs[r].GetX())-50,to(fs[r].GetY())-50,fs[r].GetOrientationDegrees()] for r in poses}
for r,(x,y,a) in poses.items():fs[r].SetPosition(p.VECTOR2I(mm(x+50),mm(y+50)));fs[r].SetOrientationDegrees(a);fs[r].SetLocked(True)
def rect(f):
    q=f.GetBoundingBox(False,False);return box(to(q.GetLeft()),to(q.GetTop()),to(q.GetRight()),to(q.GetBottom()))
for r in poses:
    g=rect(fs[r]).buffer(.149)
    for s,f in fs.items():
        if s!=r and g.intersects(rect(f)):raise RuntimeError(f'Power pose {r} intersects {s}')
def land(a):
    q=a.GetBoundingBox();return box(to(q.GetLeft()),to(q.GetTop()),to(q.GetRight()),to(q.GetBottom()))
padlist=[a for f in fs.values() for a in f.Pads()];newpads=[a for r in poses for a in fs[r].Pads()];sw='/Power/LTE_SW1';swcode=b.FindNet(sw).GetNetCode();swpads=[a for a in padlist if a.GetNetname()==sw]
points=[(to(a.GetX()),to(a.GetY())) for a in swpads];reached={0};edges=[]
while len(reached)<len(points):
    _,i,j=min((math.dist(points[i],points[j]),i,j) for i in reached for j in range(len(points)) if j not in reached)
    edges.append(LineString([points[i],points[j]]));reached.add(j)
# 4mm corridor; front GND reference remains on In1, never split.
corridor=unary_union([e.buffer(2,resolution=6) for e in edges]).intersection(box(50.3,50.3,131.7,88.2))
ripnets={sw,'Net-(QH2-G)','Net-(QL2-G)','Net-(U5-BOOT1)','Net-(U5-BOOT2)','Net-(U5-VCC)'};obsolete=[]
def copper(t):
    return Point(to(t.GetX()),to(t.GetY())).buffer(to(t.GetWidth(p.F_Cu))/2) if isinstance(t,p.PCB_VIA) else LineString([(to(t.GetStart().x),to(t.GetStart().y)),(to(t.GetEnd().x),to(t.GetEnd().y))]).buffer(to(t.GetWidth())/2)
for t in tracks:
    g=copper(t)
    if t.GetNetname() in ripnets or any(t.GetNetCode()!=a.GetNetCode() and g.intersects(land(a).buffer(.2)) for a in newpads) or (not isinstance(t,p.PCB_VIA) and t.GetLayer()==p.B_Cu and g.intersects(corridor.buffer(.2))):obsolete.append(t)
removed=set(t.m_Uuid.AsString() for t in obsolete);remaining=[t for t in tracks if t.m_Uuid.AsString() not in removed]
# Screen full-depth via copper and drill against all foreign pads/tracks and all
# existing holes; own-net copper may merge, holes never overlap.
added=[];rejected=[]
def addvia(a,x,y):
    net=a.GetNetCode();g=Point(x,y).buffer(.3);reason=None
    for q in padlist:
        if q.GetNetCode()!=net and g.distance(land(q))<.16-1e-6:reason='foreign pad';break
        d=q.GetDrillSize()
        if d.x and Point(x,y).distance(Point(to(q.GetX()),to(q.GetY())))<max(to(d.x),to(d.y))/2+.15+.3:reason='pad hole';break
    if reason is None:
        for t in remaining:
            tg=copper(t)
            if t.GetNetCode()!=net and g.distance(tg)<.16-1e-6:reason='foreign copper';break
            if isinstance(t,p.PCB_VIA) and Point(x,y).distance(Point(to(t.GetX()),to(t.GetY())))<.15+to(t.GetDrillValue())/2+.26:reason='via hole';break
    if reason is None and any(math.hypot(x-v[0],y-v[1])<.56 for v in added):reason='new via hole'
    if reason:rejected.append([a.GetParentFootprint().GetReference(),a.GetNumber(),x,y,reason]);return
    added.append((x,y,net,a.GetParentFootprint().GetReference(),a.GetNumber()))
for ref,num,maxcount in [('L4','1',4),('QH2','1',1),('QH2','2',1),('QL2','8',3),('QH2','8',3),('C27','2',1),('U5','23',1)]:
    pads=[a for a in fs[ref].Pads() if a.GetNumber()==num];a=max(pads,key=lambda a:land(a).area);x,y=to(a.GetX()),to(a.GetY());count=0
    candidates=[(x,y),(x,y-.8),(x,y+.8),(x-.7,y),(x+.7,y),(x,y-1.6),(x,y+1.6)]
    for xx,yy in candidates:
        if (xx,yy)!=(x,y) and not land(a).buffer(-.29).covers(Point(xx,yy)):continue
        n=len(added);addvia(a,xx,yy)
        if len(added)>n:count+=1
        if count>=maxcount:break
# The top common-electrode bridge is compact; the B corridor carries the longer
# inductor connection without exposing its high-current route to signal rip-up.
source=[a for a in swpads if a.GetParentFootprint().GetReference() in ('QH2','QL2')]
front=unary_union([land(a) for a in source]).convex_hull.buffer(.2)
shutil.copy2(path,R/'checkpoints/pre-switch-power-geometry.kicad_pcb')
for t in obsolete:b.Remove(t)
for z in zones:
    if not z.GetIsRuleArea() and z.GetLayer()==p.In2_Cu and z.GetNetname() in ('/Power/VSYS','/Modem/MODEM_3V8'):b.Remove(z)
p.SaveBoard(str(path),b);b=p.LoadBoard(str(path))
def polygon_zone(net,layer,geom,priority=0,keepout=False):
    z=p.ZONE(b);z.SetLayer(layer);z.SetAssignedPriority(priority)
    if keepout:z.SetIsRuleArea(True);z.SetDoNotAllowTracks(True);z.SetDoNotAllowVias(False);z.SetDoNotAllowPads(False);z.SetDoNotAllowZoneFills(False);z.SetDoNotAllowFootprints(False)
    else:z.SetNetCode(b.FindNet(net).GetNetCode());z.SetLocalClearance(mm(.2));z.SetMinThickness(mm(.2));z.SetPadConnection(p.ZONE_CONNECTION_FULL);z.SetIslandRemovalMode(p.ISLAND_REMOVAL_MODE_ALWAYS)
    ps=z.Outline();ps.NewOutline()
    for x,y in list(geom.exterior.coords)[:-1]:ps.Append(mm(x),mm(y))
    for hole in geom.interiors:
        ps.NewHole()
        for x,y in list(hole.coords)[:-1]:ps.Append(mm(x),mm(y))
    b.Add(z)
polygon_zone('/Power/VSYS',p.In2_Cu,box(50.25,50.25,85.5,88.25))
polygon_zone('/Modem/MODEM_3V8',p.In2_Cu,box(85.9,50.25,131.75,88.25))
polygon_zone(sw,p.B_Cu,corridor,3);polygon_zone(sw,p.F_Cu,front,3);polygon_zone(sw,p.B_Cu,corridor,0,True)
for x,y,net,ref,num in added:
    v=p.PCB_VIA(b);v.SetPosition(p.VECTOR2I(mm(x),mm(y)));v.SetWidth(mm(.6));v.SetDrill(mm(.3));v.SetViaType(p.VIATYPE_THROUGH);v.SetLayerPair(p.F_Cu,p.B_Cu);v.SetNetCode(net);v.SetFillingMode(p.FILLING_MODE_FILLED);v.SetCappingMode(p.CAPPING_MODE_CAPPED);b.Add(v)
p.SaveBoard(str(path),b)
for r,(x,y,a) in poses.items():cfg['anchors'][r]=[x,y,a,'top']
cfg['power_planes']={'VSYS_In2_xmax_mm':35.5,'MODEM_In2_xmin_mm':35.9,'SW1_B_reserved_corridor_width_mm':4}
(R/'scripts/layout-revision.json').write_text(json.dumps(cfg,indent=2));(R/'reports/switch-power-geometry.json').write_text(json.dumps(dict(old=old,new=poses,removed_obsolete_copper=len(obsolete),added_vias=added,rejected_via_sites=rejected,back_corridor_area_mm2=corridor.area,front_common_bridge_area_mm2=front.area,power_planes=cfg['power_planes']),indent=2));print('Power geometry corrected; removed',len(obsolete),'items, added',len(added),'filled/capped vias')
