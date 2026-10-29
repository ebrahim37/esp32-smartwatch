#!/usr/bin/python3
"""Measure current USB copper and native same-net terminal connectivity.
Total length includes connector/ESD branches: not end-to-end pair skew. Projection
onto In1 is a reference-coverage screen, not a field solver for inner/back traces.
"""
import pathlib,json,collections,math
import pcbnew as p
from shapely.geometry import LineString,Polygon
from shapely.ops import unary_union
R=pathlib.Path(__file__).resolve().parents[1];b=p.LoadBoard(str(R.parents[1]/'kicad/companion/companion.kicad_pcb'));to=p.ToMM

def polygons(ps):
    out=[]
    for i in range(ps.OutlineCount()):
        c=ps.COutline(i);pts=[(to(c.CPoint(j).x),to(c.CPoint(j).y)) for j in range(c.PointCount())]
        if len(pts)<3:continue
        holes=[]
        for k in range(ps.HoleCount(i)):
            h=ps.CHole(i,k);holes.append([(to(h.CPoint(j).x),to(h.CPoint(j).y)) for j in range(h.PointCount())])
        out.append(Polygon(pts,holes))
    return out
plane=unary_union([g for z in b.Zones() if not z.GetIsRuleArea() and z.GetLayer()==p.In1_Cu and z.GetNetname()=='GND' for g in polygons(z.GetFilledPolysList(p.In1_Cu))])
b.BuildConnectivity();conn=b.GetConnectivity();lookup={q.m_Uuid.AsString():q for q in [*b.GetTracks(),*(a for f in b.GetFootprints() for a in f.Pads())]};result={}
for net in ['/USB/USB_DP','/USB/USB_DM']:
    pads=[a for f in b.GetFootprints() for a in f.Pads() if a.GetNetname()==net];source=next(a for a in pads if a.GetParentFootprint().GetReference()=='U8');pending=[source];seen=set()
    while pending:
        q=pending.pop();uid=q.m_Uuid.AsString()
        if uid in seen:continue
        seen.add(uid)
        for a in conn.GetConnectedItems(q):
            item=lookup.get(a.m_Uuid.AsString())
            if item is not None and item.GetNetCode()==source.GetNetCode():pending.append(item)
    rows=[];vias=[]
    for t in b.GetTracks():
        if t.GetNetname()!=net:continue
        if isinstance(t,p.PCB_VIA):
            vias.append(dict(uuid=t.m_Uuid.AsString(),pos_mm=[to(t.GetX()),to(t.GetY())],diameter_mm=to(t.GetWidth(p.F_Cu)),drill_mm=to(t.GetDrillValue())));continue
        start=[to(t.GetStart().x),to(t.GetStart().y)];end=[to(t.GetEnd().x),to(t.GetEnd().y)];line=LineString([start,end]);width=to(t.GetWidth())
        rows.append(dict(uuid=t.m_Uuid.AsString(),start=start,end=end,width_mm=width,length_mm=line.length,layer=b.GetLayerName(t.GetLayer()),In1_projection_covered=plane.buffer(.005).covers(line.buffer(width/2,cap_style=2))))
    result[net]=dict(total_copper_centerline_length_mm=sum(r['length_mm'] for r in rows),length_scope='All branches, not end-to-end pair skew',trace_widths_mm=sorted({r['width_mm'] for r in rows}),layers=dict(collections.Counter(r['layer'] for r in rows)),via_count=len(vias),vias=vias,pads=[dict(ref=a.GetParentFootprint().GetReference(),pin=a.GetNumber(),pos_mm=[to(a.GetX()),to(a.GetY())],native_connected_from_U8=a.m_Uuid.AsString() in seen) for a in pads],reference_exceptions=[r for r in rows if not r['In1_projection_covered']],tracks=rows)
(R/'reports/usb-resume-audit.json').write_text(json.dumps(result,indent=2));print(json.dumps({n:{k:v for k,v in r.items() if k not in ('tracks','vias')} for n,r in result.items()},indent=2))
