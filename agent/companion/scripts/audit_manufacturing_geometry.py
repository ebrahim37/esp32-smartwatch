#!/usr/bin/python3
"""Audit saved native pad polygons, drilled holes, zones and placement.
Hole overlap with an SMT copper land is counted as via-in-pad, including off-centre
holes. Annular overhang is measured separately; it is not itself a DRC failure.
"""
import pathlib,json,hashlib,collections
import pcbnew as p
from shapely.geometry import Polygon,Point,LineString
from shapely.ops import unary_union
R=pathlib.Path(__file__).resolve().parents[1];P=R.parents[1]/'kicad/companion/companion.kicad_pcb';b=p.LoadBoard(str(P));to=p.ToMM

def polyset(ps):
    out=[]
    for i in range(ps.OutlineCount()):
        c=ps.COutline(i);pts=[(to(c.CPoint(j).x),to(c.CPoint(j).y)) for j in range(c.PointCount())]
        if len(pts)<3:continue
        holes=[]
        for k in range(ps.HoleCount(i)):
            h=ps.CHole(i,k);holes.append([(to(h.CPoint(j).x),to(h.CPoint(j).y)) for j in range(h.PointCount())])
        out.append(Polygon(pts,holes))
    return unary_union(out)
outline=p.SHAPE_POLY_SET();valid=b.GetBoardPolygonOutlines(outline,False);shape=polyset(outline);bounds=shape.bounds
pads=[(a,polyset(a.GetEffectivePolygon(p.F_Cu))) for f in b.GetFootprints() for a in f.Pads() if a.IsOnLayer(p.F_Cu)];vip=[];unsupported=[];models=[]
for a,g in pads:
    if not shape.buffer(.000001).covers(g):unsupported.append([a.GetParentFootprint().GetReference(),a.GetNumber(),g.bounds])
for v in b.GetTracks():
    if not isinstance(v,p.PCB_VIA):continue
    xy=[to(v.GetX()),to(v.GetY())];center=Point(xy);hole=center.buffer(to(v.GetDrillValue())/2);annulus=center.buffer(to(v.GetWidth(p.F_Cu))/2)
    hits=[(a,g) for a,g in pads if a.GetAttribute()==p.PAD_ATTRIB_SMD and hole.intersection(g).area>1e-8]
    if not hits:continue
    vip.append(dict(uuid=v.m_Uuid.AsString(),net=v.GetNetname(),pos_mm=xy,diameter_mm=to(v.GetWidth(p.F_Cu)),drill_mm=to(v.GetDrillValue()),centered=any(center.distance(Point(to(a.GetX()),to(a.GetY())))<.001 for a,g in hits),pads=[a.GetParentFootprint().GetReference()+'.'+a.GetNumber() for a,g in hits],annulus_outside_land_area_mm2=annulus.difference(unary_union([g for a,g in hits])).area,filled=v.GetFillingMode()==p.FILLING_MODE_FILLED,capped=v.GetCappingMode()==p.CAPPING_MODE_CAPPED))
zones=[];ground=None
for z in b.Zones():
    if z.GetIsRuleArea():continue
    g=polyset(z.GetFilledPolysList(z.GetLayer()));zones.append(dict(net=z.GetNetname(),layer=b.GetLayerName(z.GetLayer()),priority=z.GetAssignedPriority(),filled_area_mm2=g.area,islands=len(g.geoms) if hasattr(g,'geoms') else int(not g.is_empty)))
    if z.GetNetname()=='GND' and z.GetLayer()==p.In1_Cu:ground=g
courts=[(f,polyset(f.GetCourtyard(p.F_CrtYd))) for f in b.GetFootprints()];overlap=[]
for i,(a,g) in enumerate(courts):
    for c,h in courts[i+1:]:
        if a.IsFlipped()==c.IsFlipped() and g.intersection(h).area>1e-6:overlap.append([a.GetReference(),c.GetReference()])
report=dict(source_sha256=hashlib.sha256(P.read_bytes()).hexdigest(),outline_valid=valid,dimensions_mm=[bounds[2]-bounds[0],bounds[3]-bounds[1]],thickness_mm=to(b.GetDesignSettings().GetBoardThickness()),copper_layers=b.GetCopperLayerCount(),footprints=len(list(b.GetFootprints())),top=sum(not f.IsFlipped() for f in b.GetFootprints()),bottom=sum(f.IsFlipped() for f in b.GetFootprints()),unsupported_native_pad_polygons=unsupported,courtyard_overlaps=overlap,total_vias=sum(isinstance(v,p.PCB_VIA) for v in b.GetTracks()),via_in_pad_count=len(vip),centered_via_in_pad_count=sum(v['centered'] for v in vip),annulus_overhang_count=sum(v['annulus_outside_land_area_mm2']>.0001 for v in vip),unfilled_or_uncapped_via_in_pad=[v['uuid'] for v in vip if not v['filled'] or not v['capped']],via_in_pad=vip,zones=zones,In1_GND_polygon_count=(len(ground.geoms) if hasattr(ground,'geoms') else int(not ground.is_empty)) if ground is not None else 0,battery_connector=[dict(pin=a.GetNumber(),net=a.GetNetname(),pos_mm=[to(a.GetX()),to(a.GetY())]) for f in b.GetFootprints() if f.GetReference()=='U2' for a in f.Pads()])
report['POFV_below_2026_preferred_diameter']=[v['uuid'] for v in vip if v['diameter_mm']-v['drill_mm']<.15-1e-6]
prior_path=R/'checkpoints/pre-local-power-layout.kicad_pcb'
if prior_path.exists():
    prior=p.LoadBoard(str(prior_path));poses=[]
    for f in b.GetFootprints():
        old=prior.FindFootprintByReference(f.GetReference())
        before=[to(old.GetX()),to(old.GetY()),old.GetOrientationDegrees()%360];after=[to(f.GetX()),to(f.GetY()),f.GetOrientationDegrees()%360]
        if any(abs(a-c)>.001 for a,c in zip(before,after)):poses.append(dict(ref=f.GetReference(),before=before,after=after))
    report['small_passive_pose_changes']=poses
    report['major_component_pose_changes']=[r for r in poses if r['ref'] not in {'C10','C25','C33','C30','C29'}]
import csv
with (R/'reports/via-in-pad-map.csv').open('w',newline='') as out:
    origin=b.GetDesignSettings().GetAuxOrigin();ox,oy=to(origin.x),to(origin.y)
    writer=csv.writer(out);writer.writerow(['UUID','Net','Plot X mm','Plot Y mm','Native PCB X mm','Native PCB Y mm','Via diameter mm','Drill mm','SMT lands','Filled','Capped'])
    for v in vip:writer.writerow([v['uuid'],v['net'],v['pos_mm'][0]-ox,oy-v['pos_mm'][1],*v['pos_mm'],v['diameter_mm'],v['drill_mm'],';'.join(v['pads']),v['filled'],v['capped']])
(R/'reports/manufacturing-geometry.json').write_text(json.dumps(report,indent=2));print(json.dumps({k:v for k,v in report.items() if k not in ('via_in_pad','zones')},indent=2))
