#!/usr/bin/python3
"""Idempotently chamfer the supplied JLC B.Cu acute corner; parent runs/refills/DRCs.
Preserves both original UUIDs, changes no pad/net/rule, checkpoints canonical once.
Collision screen measures proposed copper versus local different-net native items;
stored filled zones are recorded separately because they must be refilled afterward.
"""
import argparse,json,math,pathlib,shutil
import pcbnew as p
from shapely.geometry import LineString,Point,Polygon
from shapely.ops import unary_union
ROOT=pathlib.Path(__file__).resolve().parents[3]
A=argparse.ArgumentParser();A.add_argument('--board',default=str(ROOT/'kicad/companion/companion.kicad_pcb'));A.add_argument('--apply',action='store_true');a=A.parse_args();path=pathlib.Path(a.board);b=p.LoadBoard(str(path));mm=p.ToMM
IDS=['1e4c519b-7c8e-4426-8111-d85e72e39f99','2e4efc9a-f50d-4e17-be86-f8f38f6c1464']
def xy(q):return [mm(q.x),mm(q.y)]
def vec(q):return p.VECTOR2I(p.FromMM(q[0]),p.FromMM(q[1]))
def poly(ps):
 shapes=[]
 for i in range(ps.OutlineCount()):
  c=ps.COutline(i);outer=[xy(c.CPoint(j)) for j in range(c.PointCount())];holes=[]
  for k in range(ps.HoleCount(i)):
   h=ps.CHole(i,k);holes.append([xy(h.CPoint(j)) for j in range(h.PointCount())])
  if len(outer)>2:shapes.append(Polygon(outer,holes))
 return unary_union(shapes)
byid={x.m_Uuid.AsString():x for x in b.GetTracks()};ts=[byid[x] for x in IDS]
old=[[94.,60.8],[91.7,63.1],[94.,63.1]];v1=[93.787868,61.012132];v2=[94.,61.1]
report=dict(original_corner_mm=old[0],original_segments=[dict(uuid=IDS[0],start_mm=old[0],end_mm=old[1],length_mm=math.dist(old[0],old[1])),dict(uuid=IDS[1],start_mm=old[2],end_mm=old[0],length_mm=math.dist(old[2],old[0]))],original_interior_angle_deg=45,new_interior_angles_deg=[112.5,112.5],shortened_endpoints_mm=[v1,v2],chamfer_length_mm=math.dist(v1,v2),track_width_mm=.15,net='Net-(U5-MODE)',layer='B.Cu',minimum_clearance_mm=.1)
chamfers=[t for uid,t in byid.items() if uid not in IDS and not isinstance(t,p.PCB_VIA) and t.GetLayer()==p.B_Cu and t.GetNetCode()==ts[0].GetNetCode() and math.dist(xy(t.GetStart()),v1)<.000002 and math.dist(xy(t.GetEnd()),v2)<.000002]
if chamfers:
 if len(chamfers)!=1 or math.dist(xy(ts[0].GetStart()),v1)>.000002 or math.dist(xy(ts[1].GetEnd()),v2)>.000002:raise RuntimeError('Existing chamfer geometry differs; refusing to overwrite')
 print(json.dumps(dict(report,status='already_applied',chamfer_uuid=chamfers[0].m_Uuid.AsString()),indent=2));raise SystemExit(0)
for t,s,e in [(ts[0],old[0],old[1]),(ts[1],old[2],old[0])]:
 if math.dist(xy(t.GetStart()),s)>.000002 or math.dist(xy(t.GetEnd()),e)>.000002 or t.GetLayer()!=p.B_Cu or abs(mm(t.GetWidth())-.15)>.000001 or t.GetNetname()!='Net-(U5-MODE)':raise RuntimeError('Original source junction changed; refusing to overwrite')
new=unary_union([LineString([v1,old[1]]).buffer(.075,quad_segs=96),LineString([old[2],v2]).buffer(.075,quad_segs=96),LineString([v1,v2]).buffer(.075,quad_segs=96)])
neighbors=[];zone_gaps=[]
for t in b.GetTracks():
 if t.m_Uuid.AsString() in IDS or t.GetNetCode()==ts[0].GetNetCode() or not t.IsOnLayer(p.B_Cu):continue
 if isinstance(t,p.PCB_VIA):g=Point(xy(t.GetPosition())).buffer(mm(t.GetWidth(p.B_Cu))/2,quad_segs=96)
 else:g=LineString([xy(t.GetStart()),xy(t.GetEnd())]).buffer(mm(t.GetWidth())/2,quad_segs=96)
 distance=new.distance(g)
 if distance<1:neighbors.append(dict(uuid=t.m_Uuid.AsString(),net=t.GetNetname(),clearance_mm=distance,kind='via' if isinstance(t,p.PCB_VIA) else 'track'))
for f in b.GetFootprints():
 for pad in f.Pads():
  if not pad.IsOnLayer(p.B_Cu) or pad.GetNetCode()==ts[0].GetNetCode():continue
  g=poly(pad.GetEffectivePolygon(p.B_Cu));distance=new.distance(g)
  if distance<1:neighbors.append(dict(uuid=pad.m_Uuid.AsString(),ref=f.GetReference(),pad=pad.GetNumber(),net=pad.GetNetname(),clearance_mm=distance,kind='pad'))
for z in b.Zones():
 if z.GetIsRuleArea() or z.GetLayer()!=p.B_Cu or z.GetNetCode()==ts[0].GetNetCode():continue
 g=poly(z.GetFilledPolysList(p.B_Cu));distance=new.distance(g)
 if distance<1:zone_gaps.append(dict(uuid=z.m_Uuid.AsString(),net=z.GetNetname(),stored_fill_clearance_mm=distance))
report['local_different_net_copper']=sorted(neighbors,key=lambda x:x['clearance_mm']);report['stored_zones_require_refill']=zone_gaps
if any(x['clearance_mm']<.1-.00001 for x in neighbors):raise RuntimeError(json.dumps(dict(report,status='blocked_by_local_copper'),indent=2))
if a.apply:
 checkpoint=ROOT/'agent/companion/checkpoints/pre-dfm-acute-corner.kicad_pcb'
 if not checkpoint.exists():shutil.copy2(path,checkpoint)
 ts[0].SetStart(vec(v1));ts[1].SetEnd(vec(v2));t=p.PCB_TRACK(b);t.SetStart(vec(v1));t.SetEnd(vec(v2));t.SetWidth(p.FromMM(.15));t.SetLayer(p.B_Cu);t.SetNetCode(ts[0].GetNetCode());b.Add(t);p.SaveBoard(str(path),b);report['status']='applied_requires_parent_zone_refill_and_DRC';report['chamfer_uuid']=t.m_Uuid.AsString()
else:report['status']='read_only_collision_screen_passed'
print(json.dumps(report,indent=2))
