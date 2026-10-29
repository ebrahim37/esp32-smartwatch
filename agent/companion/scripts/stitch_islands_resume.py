#!/usr/bin/python3
"""Join actually unreferenced surface-fill polygons to their inner/back net.
Uses saved filled polygons, never zone origins as defect locations. Through-via
sites screened against foreign copper/drills and antenna rule areas. No pad/site
is declared connected until the caller refills and reruns actual KiCad DRC.
"""
import pathlib,sys,math,json
import pcbnew as p
from shapely.geometry import Polygon,Point,LineString,box
from shapely.ops import unary_union
R=pathlib.Path(__file__).resolve().parents[1];path=pathlib.Path(sys.argv[1]) if len(sys.argv)>1 else R.parents[1] / 'kicad/companion/companion.kicad_pcb';b=p.LoadBoard(str(path));to=p.ToMM;mm=p.FromMM;tracks=list(b.GetTracks());pads=[a for f in b.GetFootprints() for a in f.Pads()]
def pos(a):return(to(a.GetPosition().x),to(a.GetPosition().y))
def land(a):
    r=a.GetBoundingBox();return box(to(r.GetLeft()),to(r.GetTop()),to(r.GetRight()),to(r.GetBottom()))
def polygons(ps):
    result=[]
    for i in range(ps.OutlineCount()):
        c=ps.COutline(i);pts=[(to(c.CPoint(j).x),to(c.CPoint(j).y)) for j in range(c.PointCount())]
        if len(pts)<3:continue
        holes=[]
        for k in range(ps.HoleCount(i)):
            h=ps.CHole(i,k);holes.append([(to(h.CPoint(j).x),to(h.CPoint(j).y)) for j in range(h.PointCount())])
        result.append(Polygon(pts,holes))
    return result
copper=[];holes=[]
for t in tracks:
    if isinstance(t,p.PCB_VIA):
        copper.append((Point(pos(t)).buffer(to(t.GetWidth(p.F_Cu))/2),t.GetNetCode(),None));holes.append((Point(pos(t)),to(t.GetDrillValue())/2))
    else:copper.append((LineString([pos(t),(to(t.GetEnd().x),to(t.GetEnd().y))]).buffer(to(t.GetWidth())/2),t.GetNetCode(),t.GetLayer()))
lands=[(land(a),a.GetNetCode(),a) for a in pads];keepouts=unary_union([g for z in b.Zones() if z.GetIsRuleArea() and z.GetDoNotAllowVias() for g in polygons(z.Outline())]);allzones=list(b.Zones());added=[]
for z in allzones:
    if z.GetIsRuleArea() or z.GetLayer() not in (p.F_Cu,p.B_Cu):continue
    net=z.GetNetCode();name=z.GetNetname();others=unary_union([g for q in allzones if not q.GetIsRuleArea() and q.GetNetCode()==net and q.GetLayer()!=z.GetLayer() for g in polygons(q.GetFilledPolysList(q.GetLayer()))])
    if others.is_empty:continue
    for geo in polygons(z.GetFilledPolysList(z.GetLayer())):
        anchors=[Point(pos(v)).buffer(to(v.GetWidth(p.F_Cu))/2) for v in tracks if isinstance(v,p.PCB_VIA) and v.GetNetCode()==net]
        anchors.extend(g for g,n,a in lands if n==net and a.GetDrillSize().x)
        if any(geo.intersects(g) for g in anchors):continue
        found=False
        for diameter,drill,allow_pad in [(.6,.3,False),(.35,.2,False),(.3,.15,True),(.25,.15,True)]:
            radius=diameter/2;dr=drill/2;area=geo.intersection(others).buffer(-radius-.025)
            if area.is_empty:continue
            x0,y0,x1,y1=area.bounds;represent=area.representative_point();candidates=[(represent.x,represent.y)]+[(x,y) for x in list(__import__('numpy').arange(x0,x1+.01,.05)) for y in list(__import__('numpy').arange(y0,y1+.01,.05))]
            for x,y in candidates:
                q=Point(x,y);g=q.buffer(radius)
                if not area.covers(q) or keepouts.intersects(g):continue
                if not allow_pad and any(g.distance(a)<.03 for a,n,pad in lands):continue
                if any(n!=net and g.distance(a)<.13 for a,n,pad in lands):continue
                if any(pad.GetDrillSize().x and q.distance(Point(pos(pad)))<dr+max(to(pad.GetDrillSize().x),to(pad.GetDrillSize().y))/2+.45 for a,n,pad in lands):continue
                if any(n!=net and g.distance(c)<.205 for c,n,l in copper):continue
                if any(q.distance(h)<dr+r+.26 for h,r in holes):continue
                v=p.PCB_VIA(b);v.SetPosition(p.VECTOR2I(mm(float(x)),mm(float(y))));v.SetWidth(mm(diameter));v.SetDrill(mm(drill));v.SetViaType(p.VIATYPE_THROUGH);v.SetLayerPair(p.F_Cu,p.B_Cu);v.SetNetCode(net)
                if allow_pad:v.SetFillingMode(p.FILLING_MODE_FILLED);v.SetCappingMode(p.CAPPING_MODE_CAPPED)
                b.Add(v);tracks.append(v);holes.append((q,dr));copper.append((g,net,None));added.append(dict(net=name,layer=b.GetLayerName(z.GetLayer()),pos_mm=[x,y],diameter_mm=diameter,drill_mm=drill,filled_capped=allow_pad));found=True;break
            if found:break
if added:p.SaveBoard(str(path),b)
print(json.dumps({'added_stitching_vias':added}),flush=True)
