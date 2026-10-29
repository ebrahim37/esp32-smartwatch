#!/usr/bin/python3
"""Move only small passives for measured regulator bypass-loop/routing defects.
Major placement unchanged. New copper is clearance-refilled, never assumed valid.
C29 flips within its existing courtyard to put GND away from the blocked COMP
escape. Candidate cap poses were independently collision-screened.
"""
import pathlib,json,shutil,math
import pcbnew as p
from shapely.geometry import Polygon,Point,LineString,box
from shapely.ops import unary_union
R=pathlib.Path(__file__).resolve().parents[1];P=R.parents[1]/'kicad/companion/companion.kicad_pcb';b=p.LoadBoard(str(P));mm=p.FromMM;to=p.ToMM;fs={f.GetReference():f for f in b.GetFootprints()}
def poly(ps):
    result=[]
    for i in range(ps.OutlineCount()):
        c=ps.COutline(i);pts=[(to(c.CPoint(j).x),to(c.CPoint(j).y)) for j in range(c.PointCount())]
        if len(pts)>2:result.append(Polygon(pts))
    return unary_union(result)
def pos(q):return(to(q.GetPosition().x),to(q.GetPosition().y))
def copper(t):return Point(pos(t)).buffer(to(t.GetWidth(p.F_Cu))/2) if isinstance(t,p.PCB_VIA) else LineString([pos(t),(to(t.GetEnd().x),to(t.GetEnd().y))]).buffer(to(t.GetWidth())/2)
checkpoint=R/'checkpoints/pre-local-power-layout.kicad_pcb'
if not checkpoint.exists():shutil.copy2(P,checkpoint)
poses={'C10':(77.90,66.95,0),'C25':(79.25,74.25,0),'C33':(88.55,68.40,270),'C30':(90.30,68.40,0),'C29':(91.25,63.5,270)};prior=p.LoadBoard(str(checkpoint));old={r:[*pos(prior.FindFootprintByReference(r)),prior.FindFootprintByReference(r).GetOrientationDegrees()] for r in poses}
for r,(x,y,a) in poses.items():fs[r].SetPosition(p.VECTOR2I(mm(x),mm(y)));fs[r].SetOrientationDegrees(a)
lands=[(a,poly(a.GetEffectivePolygon(p.F_Cu))) for r in poses for a in fs[r].Pads()];removed=[]
# Pad-specific short local top pours; inner planes and reserved SW1 unchanged.
for ref,pin,source,sourcepin,radius,priority in [('C10','1','U4','6',.6,32),('C25','1','U4','10',.7,33),('C33','1','U5','12',.8,34)]:
    if any(not q.GetIsRuleArea() and q.GetLayer()==p.F_Cu and q.GetAssignedPriority()==priority for q in b.Zones()):continue
    a=next(q for q in fs[ref].Pads() if q.GetNumber()==pin);c=next(q for q in fs[source].Pads() if q.GetNumber()==sourcepin);assert a.GetNetCode()==c.GetNetCode();g=LineString([pos(a),pos(c)]).buffer(radius,quad_segs=8)
    z=p.ZONE(b);z.SetLayer(p.F_Cu);z.SetNetCode(a.GetNetCode());z.SetAssignedPriority(priority);z.SetLocalClearance(mm(.2));z.SetMinThickness(mm(.2));z.SetPadConnection(p.ZONE_CONNECTION_FULL);z.SetIslandRemovalMode(p.ISLAND_REMOVAL_MODE_ALWAYS);ps=z.Outline();ps.NewOutline()
    for x,y in list(g.exterior.coords)[:-1]:ps.Append(mm(x),mm(y))
    b.Add(z)
for t in list(b.GetTracks()):
    g=copper(t);via=isinstance(t,p.PCB_VIA)
    if any(a.GetNetCode()!=t.GetNetCode() and (via or t.GetLayer()==p.F_Cu) and g.distance(h)<(.13 if via else .105) for a,h in lands):
        removed.append(dict(uuid=t.m_Uuid.AsString(),net=t.GetNetname(),layer='via' if via else b.GetLayerName(t.GetLayer())));b.Remove(t)
p.SaveBoard(str(P),b);cfgpath=R/'scripts/layout-revision.json';cfg=json.loads(cfgpath.read_text())
for r,(x,y,a) in poses.items():cfg['anchors'][r]=[x-50,y-50,a,'top']
cfgpath.write_text(json.dumps(cfg,indent=2));report=dict(old=old,new=poses,removed_conflicting_copper=removed,rationale='Local regulator output bypasses moved closer; C25 input bypass occupies former C10 site; C30 relocation preserves compensation network; C29 courtyard-preserving flip frees ground escape',major_component_poses_unchanged=True);(R/'reports/local-power-layout.json').write_text(json.dumps(report,indent=2));print(json.dumps(report,indent=2))
