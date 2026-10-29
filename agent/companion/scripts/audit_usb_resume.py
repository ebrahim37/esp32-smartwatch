#!/usr/bin/python3
"""Saved-board USB copper audit, independent of router success status.
Measure actual trace lengths, vias, widths and continuous In1 reference coverage.
The CP2102N is full-speed USB; short connector/ESD branches are separately listed.
No compliance, eye-diagram or fabricated-impedance claim.
"""
import pathlib,json,collections,math,heapq
import pcbnew as p
from shapely.geometry import Point,LineString,Polygon,box
from shapely.ops import unary_union
R=pathlib.Path(__file__).resolve().parents[1];b=p.LoadBoard(str(R.parents[1] / 'kicad/companion/companion.kicad_pcb'));to=p.ToMM
nets=['/USB/USB_DP','/USB/USB_DM'];result={}
def polyset(ps):
    out=[]
    for i in range(ps.OutlineCount()):
        c=ps.COutline(i);points=[(to(c.CPoint(j).x),to(c.CPoint(j).y)) for j in range(c.PointCount())]
        if len(points)<3:continue
        holes=[]
        for k in range(ps.HoleCount(i)):
            h=ps.CHole(i,k);holes.append([(to(h.CPoint(j).x),to(h.CPoint(j).y)) for j in range(h.PointCount())])
        out.append(Polygon(points,holes))
    return unary_union(out)
plane=unary_union([polyset(z.GetFilledPolysList(p.In1_Cu)) for z in b.Zones() if not z.GetIsRuleArea() and z.GetLayer()==p.In1_Cu and z.GetNetname()=='GND'])
for net in nets:
    tracks=[t for t in b.GetTracks() if t.GetNetname()==net and not isinstance(t,p.PCB_VIA)];vias=[t for t in b.GetTracks() if t.GetNetname()==net and isinstance(t,p.PCB_VIA)];lines=[];rows=[]
    for t in tracks:
        a=(to(t.GetStart().x),to(t.GetStart().y));c=(to(t.GetEnd().x),to(t.GetEnd().y));g=LineString([a,c]);lines.append(g);rows.append(dict(uuid=t.m_Uuid.AsString(),start=a,end=c,width_mm=to(t.GetWidth()),length_mm=g.length,layer=b.GetLayerName(t.GetLayer()),full_width_reference_covered=plane.buffer(.005).covers(g.buffer(to(t.GetWidth())/2,cap_style=2))))
    # Node same-layer trace intersections, include all pad centres by projection.
    segments=unary_union(lines);pieces=list(segments.geoms) if hasattr(segments,'geoms') else [segments];graph=collections.defaultdict(list)
    key=lambda q:(round(q[0],6),round(q[1],6))
    for line in pieces:
        coords=list(line.coords)
        for a,c in zip(coords,coords[1:]):
            a=key(a);c=key(c);d=math.dist(a,c);graph[a].append((c,d));graph[c].append((a,d))
    pads=[]
    for f in b.GetFootprints():
        for a in f.Pads():
            if a.GetNetname()!=net:continue
            q=(to(a.GetX()),to(a.GetY()));r=a.GetBoundingBox();g=box(to(r.GetLeft()),to(r.GetTop()),to(r.GetRight()),to(r.GetBottom()));node=('pad',f.GetReference(),a.GetNumber());pads.append((node,q))
            for v in list(graph):
                if len(v)==2 and g.buffer(.12).covers(Point(v)):
                    d=math.dist(v,q);graph[node].append((v,d));graph[v].append((node,d))
    source=next(n for n,q in pads if n[1]=='U8');dist={source:0};queue=[(0,0,source)];seq=0
    while queue:
        d,_,u=heapq.heappop(queue)
        if d!=dist[u]:continue
        for v,cost in graph[u]:
            nd=d+cost
            if nd>=dist.get(v,float('inf')):continue
            dist[v]=nd;seq+=1;heapq.heappush(queue,(nd,seq,v))
    result[net]=dict(total_copper_centerline_length_mm=sum(r['length_mm'] for r in rows),trace_widths_mm=sorted(set(r['width_mm'] for r in rows)),layers=dict(collections.Counter(r['layer'] for r in rows)),via_count=len(vias),pads=[dict(ref=n[1],pin=n[2],pos_mm=q,graph_path_from_U8_mm=dist.get(n)) for n,q in pads],reference_exceptions=[r for r in rows if not r['full_width_reference_covered']],tracks=rows)
(R/'reports/usb-resume-audit.json').write_text(json.dumps(result,indent=2));print(json.dumps({n:{k:v for k,v in d.items() if k!='tracks'} for n,d in result.items()},indent=2))
