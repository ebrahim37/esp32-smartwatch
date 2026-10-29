#!/usr/bin/python3
"""Measure source footprints and reconcile schematic nodes with PCB pads."""
import json, pathlib, collections, xml.etree.ElementTree as ET
import pcbnew as p
from shapely.geometry import Polygon, box
ROOT = pathlib.Path(__file__).resolve().parents[1]
b = p.LoadBoard(str(ROOT.parents[1] / 'kicad/companion/companion.kicad_pcb'))
m = p.ToMM

def polygon(ps):
    out=[]
    for i in range(ps.OutlineCount()):
        c=ps.COutline(i)
        pts=[(m(c.CPoint(j).x),m(c.CPoint(j).y)) for j in range(c.PointCount())]
        if len(pts)>2: out.append(Polygon(pts))
    from shapely.ops import unary_union
    return unary_union(out)

def envelope(f):
    # Courtyards in supplied custom library omit protruding pads on some parts.
    # Union real courtyard, real pad bounding geometry and body/silk outline.
    ps=polygon(f.GetCourtyard(p.F_CrtYd))
    r=f.GetBoundingBox(False,False)
    return ps.union(box(m(r.GetLeft()),m(r.GetTop()),m(r.GetRight()),m(r.GetBottom())))

if __name__=='__main__':
    rows=[]
    for f in sorted(b.GetFootprints(),key=lambda f:f.GetReference()):
        g=envelope(f); x,y=f.GetPosition()
        rows.append(dict(ref=f.GetReference(),value=f.GetValue(),footprint=str(f.GetFPID()),x=m(x),y=m(y),angle=f.GetOrientationDegrees(),bounds=g.bounds,area=g.area,courtyard_area=polygon(f.GetCourtyard(p.F_CrtYd)).area,pads=[dict(number=a.GetNumber(),x=m(a.GetX()),y=m(a.GetY()),net=a.GetNetname(),bounds=[m(a.GetBoundingBox().GetLeft()),m(a.GetBoundingBox().GetTop()),m(a.GetBoundingBox().GetRight()),m(a.GetBoundingBox().GetBottom())]) for a in f.Pads()]))
    root=ET.parse(ROOT/'reports/baseline/netlist.xml').getroot()
    sch={(n.attrib['ref'],n.attrib['pin']):net.attrib['name'] for net in root.findall('./nets/net') for n in net.findall('node')}
    pcb={(f.GetReference(),a.GetNumber()):a.GetNetname() for f in b.GetFootprints() for a in f.Pads() if a.GetNumber()}
    mismatches=[dict(ref=k[0],pin=k[1],schematic=v,pcb=pcb.get(k)) for k,v in sch.items() if pcb.get(k)!=v]
    summary=dict(footprints=len(rows),tracks=len(list(b.GetTracks())),zones=len(list(b.Zones())),net_count=b.GetNetCount(),envelope_area=sum(r['area'] for r in rows),courtyard_area=sum(r['courtyard_area'] for r in rows),schematic_nodes=len(sch),pcb_nodes=len(pcb),parity_mismatches=mismatches,parts=rows)
    (ROOT/'reports/inventory.json').write_text(json.dumps(summary,indent=2))
    print(json.dumps({k:v for k,v in summary.items() if k!='parts'},indent=2))
    for f in b.GetFootprints():
        if f.GetReference().startswith('U') or f.GetReference().startswith('J'):
            print(f.GetReference(),f.GetValue(),[(a.GetNumber(),a.GetNetname()) for a in f.Pads() if not a.GetNetname().startswith('unconnected-')])
