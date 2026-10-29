#!/usr/bin/python3
"""Short flared all-F.Cu BQ25606 SW load path; preserve compact bootstrap branch.
Removes obsolete SW layer detour and reroutes the crossing low-current bootstrap
launch separately. Does not change footprints, pin nets or design rules.
"""
import pathlib,shutil
import pcbnew as p
R=pathlib.Path(__file__).resolve().parents[1];P=R.parents[1]/'kicad/companion/companion.kicad_pcb';b=p.LoadBoard(str(P));mm=p.FromMM;net=b.FindNet('Net-(C4-Pad1)').GetNetCode();boot=b.FindNet('Net-(U1-BTST)').GetNetCode();checkpoint=R/'checkpoints/pre-charger-switch.kicad_pcb'
if not checkpoint.exists():shutil.copy2(P,checkpoint)
obsolete=[t for t in b.GetTracks() if t.GetNetCode()==net or t.m_Uuid.AsString() in {'501cb10c-473f-4dcb-8c65-76ba0e35938d','e4748060-0712-402a-8be0-8d09bc7ecee2'}]
def trace(points,width):
    for a,c in zip(points,points[1:]):
        t=p.PCB_TRACK(b);t.SetStart(p.VECTOR2I(mm(a[0]),mm(a[1])));t.SetEnd(p.VECTOR2I(mm(c[0]),mm(c[1])));t.SetWidth(mm(width));t.SetLayer(p.F_Cu);t.SetNetCode(net);b.Add(t)
trace([(75.25,73.95),(74.75,73.95),(74.75,73.2)],.3)
trace([(74.75,73.2),(74.75,71.75),(74.7,71.7)],.6)
trace([(74.75,72.23),(76.25,72.23)],.3)
if not any(isinstance(t,p.PCB_VIA) and t.GetNetCode()==boot and t.GetPosition()==p.VECTOR2I(mm(74.25),mm(73.95)) for t in b.GetTracks()):
    v=p.PCB_VIA(b);v.SetPosition(p.VECTOR2I(mm(74.25),mm(73.95)));v.SetWidth(mm(.3));v.SetDrill(mm(.15));v.SetViaType(p.VIATYPE_THROUGH);v.SetLayerPair(p.F_Cu,p.B_Cu);v.SetNetCode(boot);v.SetFillingMode(p.FILLING_MODE_FILLED);v.SetCappingMode(p.CAPPING_MODE_CAPPED);b.Add(v)
for t in obsolete:b.Remove(t)
p.SaveBoard(str(P),b);print('Charger SW replaced by direct F.Cu 0.3mm paired launch -> 0.6mm trunk; bootstrap launch needs route/DRC')
