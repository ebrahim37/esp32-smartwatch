#!/usr/bin/python3
"""Saved-artifact geometry, electrical parity and copper continuity audit.
The CLI DRC JSON is authoritative for opens and clearances. Geometry assertions
are independent measurements of the saved PCB, not counts of generator intent.
"""
import pathlib,json,math,collections,xml.etree.ElementTree as ET
import pcbnew as p
from shapely.geometry import Polygon,box,Point,LineString
from shapely.ops import unary_union
ROOT=pathlib.Path(__file__).resolve().parents[1];cfg=json.loads((ROOT/'scripts/layout-revision.json').read_text());b=p.LoadBoard(str(ROOT.parents[1] / 'kicad/companion/companion.kicad_pcb'));to=p.ToMM;mm=p.FromMM;ox,oy=cfg['origin'];W,H=cfg['size']

def polyset(ps):
    polys=[]
    for i in range(ps.OutlineCount()):
        o=ps.COutline(i);pts=[(to(o.CPoint(j).x),to(o.CPoint(j).y)) for j in range(o.PointCount())]
        if len(pts)<3:continue
        holes=[]
        for k in range(ps.HoleCount(i)):
            o=ps.CHole(i,k);holes.append([(to(o.CPoint(j).x),to(o.CPoint(j).y)) for j in range(o.PointCount())])
        polys.append(Polygon(pts,holes))
    return unary_union(polys)
outline=p.SHAPE_POLY_SET();valid=b.GetBoardPolygonOutlines(outline,False);shape=polyset(outline);bounds=shape.bounds
parts=[];courts={};pad_outside=[];antenna=box(*(v+(ox if i%2==0 else oy) for i,v in enumerate(cfg['antenna'])));rfmetal=[]
for f in b.GetFootprints():
    ref=f.GetReference();r=f.GetBoundingBox(False,False);env=box(to(r.GetLeft()),to(r.GetTop()),to(r.GetRight()),to(r.GetBottom()));s='bottom' if f.IsFlipped() else 'top';court=polyset(f.GetCourtyard(p.B_CrtYd if f.IsFlipped() else p.F_CrtYd));courts[ref]=(s,court)
    distance=env.distance(antenna)
    if ref!='U7' and distance<15-1e-6:rfmetal.append(dict(ref=ref,clearance_mm=distance))
    for a in f.Pads():
        ar=a.GetBoundingBox();land=box(to(ar.GetLeft()),to(ar.GetTop()),to(ar.GetRight()),to(ar.GetBottom()))
        if not shape.covers(land):pad_outside.append(dict(ref=ref,pin=a.GetNumber(),bounds=land.bounds))
    parts.append(dict(ref=ref,side=s,x_mm=to(f.GetX())-ox,y_mm=to(f.GetY())-oy,rotation_deg=f.GetOrientationDegrees(),antenna_distance_mm=distance,bounds=env.bounds))
overlaps=[];keys=list(courts)
for i,a in enumerate(keys):
    for c in keys[i+1:]:
        sa,pa=courts[a];sc,pc=courts[c]
        if sa==sc and not pa.is_empty and not pc.is_empty and pa.intersection(pc).area>1e-6:overlaps.append([a,c])
root=ET.parse(ROOT/'reports/final-netlist.xml').getroot();sch={(n.attrib['ref'],n.attrib['pin']):net.attrib['name'].replace('{slash}','/') for net in root.findall('./nets/net') for n in net.findall('node')};pcb={(f.GetReference(),a.GetNumber()):a.GetNetname().replace('{slash}','/') for f in b.GetFootprints() for a in f.Pads() if a.GetNumber()};mismatches=[dict(ref=k[0],pin=k[1],schematic=v,pcb=pcb.get(k)) for k,v in sch.items() if pcb.get(k)!=v]
zones=[];ground=None
for z in b.Zones():
    if z.GetIsRuleArea():continue
    shapez=polyset(z.GetFilledPolysList(z.GetLayer()));zones.append(dict(net=z.GetNetname(),layer=b.GetLayerName(z.GetLayer()),filled_area_mm2=shapez.area,islands=len(shapez.geoms) if hasattr(shapez,'geoms') else (0 if shapez.is_empty else 1)))
    if z.GetLayer()==p.In1_Cu and z.GetNetname()=='GND':ground=shapez
rf_nets={'/Modem/GNSS_ANT','/Modem/LTE_ANT','Net-(J_GNSS1-SIN)','Net-(J_LTE1-SIN)','Net-(C50-Pad1)'};rf=[];unreferenced=[]
for t in b.GetTracks():
    if t.GetNetname() not in rf_nets:continue
    if isinstance(t,p.PCB_VIA):rf.append(dict(net=t.GetNetname(),via=True));continue
    a=(to(t.GetStart().x),to(t.GetStart().y));c=(to(t.GetEnd().x),to(t.GetEnd().y));line=LineString([a,c]);rf.append(dict(net=t.GetNetname(),layer=t.GetLayerName(),width_mm=to(t.GetWidth()),length_mm=line.length))
    if ground is not None and not ground.buffer(.005).covers(line):unreferenced.append(dict(net=t.GetNetname(),start=a,end=c))
report=dict(outline_valid=valid,board_dimensions_mm=[bounds[2]-bounds[0],bounds[3]-bounds[1]],board_outline_area_mm2=shape.area,
    board_thickness_mm=to(b.GetDesignSettings().GetBoardThickness()),copper_layers=b.GetCopperLayerCount(),footprints=len(parts),
    side_counts=dict(collections.Counter(r['side'] for r in parts)),unsupported_solder_pad_bounding_boxes=pad_outside,
    courtyard_overlaps=overlaps,antenna_15mm_external_metal_deviations=rfmetal,antenna_deviation_document='research/mechanical-revision.md',
    schematic_nodes=len(sch),pcb_nodes=len(pcb),parity_mismatches=mismatches,
    tracks_by_layer=dict(collections.Counter(b.GetLayerName(t.GetLayer()) for t in b.GetTracks() if not isinstance(t,p.PCB_VIA))),
    vias=sum(isinstance(t,p.PCB_VIA) for t in b.GetTracks()),rf=rf,rf_reference_exceptions=unreferenced,zones=zones,parts=parts,
    battery=cfg['battery'])
(ROOT/'reports/acceptance-audit.json').write_text(json.dumps(report,indent=2))
print(json.dumps({k:v for k,v in report.items() if k not in ('parts','rf')},indent=2))
