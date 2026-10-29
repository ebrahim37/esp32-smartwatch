#!/usr/bin/python3
"""Correct inherited reference-to-function placement, using actual pad nets.
L1 .47uH belongs to TPS63802; L3 1uH belongs to BQ25606. The schematic is
already correct. Remove invalidated switching routes, not functional parts.
"""
import pathlib,json,shutil
import pcbnew as p
from shapely.geometry import box,LineString,Point
R=pathlib.Path(__file__).resolve().parents[1];path=R.parents[1] / 'kicad/companion/companion.kicad_pcb';b=p.LoadBoard(str(path));mm=p.FromMM;to=p.ToMM;fs={f.GetReference():f for f in b.GetFootprints()};tracks=list(b.GetTracks());cfg=json.loads((R/'scripts/layout-revision.json').read_text())
assert {a.GetNetname() for a in fs['L1'].Pads()}=={'Net-(U4-L1)','Net-(U4-L2)'}
assert {a.GetNetname() for a in fs['L3'].Pads()}=={'Net-(C4-Pad1)','/Power/VSYS'}
poses={'L1':(25,9.4,0),'L3':(24,21.7,180),'R21':(27.35,9.25,90)};old={r:[to(fs[r].GetX())-50,to(fs[r].GetY())-50,fs[r].GetOrientationDegrees()] for r in poses}
for r,(x,y,a) in poses.items():fs[r].SetPosition(p.VECTOR2I(mm(x+50),mm(y+50)));fs[r].SetOrientationDegrees(a);fs[r].SetLocked(True)
def bounds(f):
    r=f.GetBoundingBox(False,False);return (to(r.GetLeft()),to(r.GetTop()),to(r.GetRight()),to(r.GetBottom()))
for r in poses:
    a=box(*bounds(fs[r])).buffer(.149)
    for s,f in fs.items():
        if s!=r and a.intersects(box(*bounds(f))):raise RuntimeError(f'{r} conflicts with {s}')
newpads=[]
for r in poses:
    for a in fs[r].Pads():
        q=a.GetBoundingBox();newpads.append((a.GetNetCode(),box(to(q.GetLeft()),to(q.GetTop()),to(q.GetRight()),to(q.GetBottom())).buffer(.2)))
ripnames={'Net-(U4-L1)','Net-(U4-L2)','Net-(C4-Pad1)'};obsolete=[]
for t in tracks:
    if isinstance(t,p.PCB_VIA):geom=Point(to(t.GetX()),to(t.GetY())).buffer(to(t.GetWidth(p.F_Cu))/2)
    else:geom=LineString([(to(t.GetStart().x),to(t.GetStart().y)),(to(t.GetEnd().x),to(t.GetEnd().y))]).buffer(to(t.GetWidth())/2)
    if t.GetNetname() in ripnames or any(t.GetNetCode()!=net and geom.intersects(g) for net,g in newpads):obsolete.append(t)
# Probe pads carry the same descriptive fields as their actual schematic.
for ref in ('TP9','TP10'):fs[ref].GetField('Description').SetText('test point')
shutil.copy2(path,R/'checkpoints/pre-inductor-mapping-repair.kicad_pcb')
for t in obsolete:b.Remove(t)
p.SaveBoard(str(path),b)
for r,(x,y,a) in poses.items():cfg['anchors'][r]=[x,y,a,'top']
(R/'scripts/layout-revision.json').write_text(json.dumps(cfg,indent=2));(R/'reports/inductor-placement-repair.json').write_text(json.dumps({'old':old,'new':poses,'removed_obsolete_copper':len(obsolete),'schematic_unchanged':True,'evidence':'actual L1 pad nets Net-(U4-L1/L2), actual L3 pad nets switch Net-(C4-Pad1) and VSYS'},indent=2));print('Corrected inductor mapping; obsolete copper items',len(obsolete))
