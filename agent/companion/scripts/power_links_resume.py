#!/usr/bin/python3
"""Repair the connector-pose discontinuity in 5V and improve VBUS TVS locality.
The original 1mm charger trunk stays intact; two short 0.65mm B.Cu bridges join
the new Type-C dogbones to it. TVS U6 moves next to VBUS.
"""
import pathlib,json,shutil,math
import pcbnew as p
from shapely.geometry import box
R=pathlib.Path(__file__).resolve().parents[1];file=R.parents[1] / 'kicad/companion/companion.kicad_pcb';b=p.LoadBoard(str(file));mm=p.FromMM;to=p.ToMM;fs={f.GetReference():f for f in b.GetFootprints()};tracks=list(b.GetTracks());lookup={t.m_Uuid.AsString():t for t in tracks}
def vec(x,y):return p.VECTOR2I(mm(x),mm(y))
def bridge(net,layer,coords,width):
    for (x,y),(xx,yy) in zip(coords,coords[1:]):
        existing=next((t for t in tracks if not isinstance(t,p.PCB_VIA) and t.GetLayer()==layer and t.GetNetname()==net and t.GetStart()==vec(x,y) and t.GetEnd()==vec(xx,yy)),None)
        if existing:existing.SetWidth(mm(width));continue
        t=p.PCB_TRACK(b);t.SetStart(vec(x,y));t.SetEnd(vec(xx,yy));t.SetWidth(mm(width));t.SetLayer(layer);t.SetNetCode(b.FindNet(net).GetNetCode());b.Add(t)
# Filled cap copper need not form tangent contacts to nearby same-net objects.
for uid,diameter in [('79dff439-d299-437d-8c69-edd02d15be77',.45),('217c6bf3-ce51-423f-a9b3-1816314da774',.55)]:
    v=lookup.get(uid)
    if v:v.SetWidth(mm(diameter))
old=(to(fs['U6'].GetX()),to(fs['U6'].GetY()),fs['U6'].GetOrientationDegrees());fs['U6'].SetPosition(vec(129,81));fs['U6'].SetOrientationDegrees(180)
q=fs['U6'].GetBoundingBox(False,False);g=box(to(q.GetLeft()),to(q.GetTop()),to(q.GetRight()),to(q.GetBottom()))
for ref,f in fs.items():
    if ref=='U6':continue
    z=f.GetBoundingBox(False,False);other=box(to(z.GetLeft()),to(z.GetTop()),to(z.GetRight()),to(z.GetBottom()))
    if g.buffer(.149).intersects(other):raise RuntimeError('TVS pose overlaps '+ref)
for x in (122.6,127.4):
    bridge('/Power/USB_5V',p.B_Cu,[(x,80.05),(x,81.44)],.65)
    if not any(isinstance(v,p.PCB_VIA) and v.GetPosition()==vec(x,80.05) and v.GetNetname()=='/Power/USB_5V' for v in tracks):
        v=p.PCB_VIA(b);v.SetPosition(vec(x,80.05));v.SetWidth(mm(.6));v.SetDrill(mm(.3));v.SetViaType(p.VIATYPE_THROUGH);v.SetLayerPair(p.F_Cu,p.B_Cu);v.SetNetCode(b.FindNet('/Power/USB_5V').GetNetCode());b.Add(v)
checkpoint=R/'checkpoints/pre-resume-power-links.kicad_pcb'
if not checkpoint.exists():shutil.copy2(file,checkpoint)
p.SaveBoard(str(file),b)
cfg=json.loads((R/'scripts/layout-revision.json').read_text());cfg['anchors']['U6']=[79,31,180,'top'];(R/'scripts/layout-revision.json').write_text(json.dumps(cfg,indent=2))
record=R/'reports/resume-power-links.json';previous=json.loads(record.read_text()) if record.exists() else {}
record.write_text(json.dumps(dict(TVS_old=previous.get('TVS_old',old),TVS_new=[129,81,180],rationale='TVS U6 was 24mm from Type-C VBUS; now <4mm, all other footprints unchanged',USB_back_bridge_width_mm=.65,bridge_resistance_mohm_35um=1.05,bridge_loss_mw_at_2A=4.2),indent=2));print('VBUS pose-gap bridges and local TVS applied')
