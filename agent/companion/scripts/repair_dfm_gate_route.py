#!/usr/bin/python3
"""Keep corrected manufacturer lands; clear adjacent gate, VSYS and COMP routing.
No current-path neckdown, component movement, net or design-rule change.
"""
import json, pathlib, shutil
import pcbnew as p
R=pathlib.Path(__file__).resolve().parents[3]
P=R/'kicad/companion/companion.kicad_pcb'
checkpoint=R/'agent/companion/checkpoints/pre-dfm-gate-route.kicad_pcb'
if not checkpoint.exists():shutil.copy2(P,checkpoint)
b=p.LoadBoard(str(P));to=p.ToMM
items={t.m_Uuid.AsString():t for t in b.GetTracks()}
def xy(x,y):return p.VECTOR2I(p.FromMM(x),p.FromMM(y))
via=items['4681ea79-0345-4e49-8ac9-5e88a07a2925']
trunk=items['cbecec30-5b6e-47ab-970b-6207062cb83b']
gate=items['6888dbe6-c44c-435a-8b4b-68fedadad1d3']
launch=items['6dcd9b93-af45-43e3-911f-600bcbf296d1']
assert via.GetNetname()==trunk.GetNetname()=='/Power/VSYS'
assert gate.GetNetname()==launch.GetNetname()=='Net-(QH2-G)'
assert to(trunk.GetWidth())==.6 and to(via.GetWidth(p.F_Cu))==.6
via.SetPosition(xy(81.05,63.85));trunk.SetEnd(xy(81.05,63.85))
for t in list(b.GetTracks()):
 if isinstance(t,p.PCB_VIA):continue
 if t.GetNetCode()==via.GetNetCode() and t.GetStart()==xy(80.8,64.94) and t.GetEnd()==xy(80.8,63.641):b.Remove(t)
gate.SetStart(xy(81.75,63.6));gate.SetEnd(xy(86.1,63.6));launch.SetEnd(xy(81.75,63.6))
# The corrected TI U5.20 inner edge requires another 25um at this COMP escape.
comp=items['e5d70952-5309-4178-955c-41fd92b2f883']
assert comp.GetNetname()=='Net-(U5-COMP)'
assert comp.GetPosition() in (xy(89.0125,63.4875),xy(89.0125,63.5125))
old=comp.GetPosition();new=xy(89.0125,63.5125)
for track in b.GetTracks():
 if isinstance(track,p.PCB_VIA) or track.GetNetCode()!=comp.GetNetCode():continue
 if track.GetStart()==old:track.SetStart(new)
 if track.GetEnd()==old:track.SetEnd(new)
comp.SetPosition(new)
p.SaveBoard(str(P),b)
report={'VSYS_via_native_mm':[81.05,63.85],'VSYS_front_copper_width_mm':.6,'gate_horizontal_y_mm':63.6,'gate_width_mm':.15,'COMP_via_native_mm':[89.0125,63.5125],'COMP_reason':'25um shift clears independently corrected TI U5.20 inward copper; both incident tracks follow via','component_poses_changed':False,'rules_changed':False,'status':'Requires refilled KiCad DRC and power continuity screen'}
(R/'agent/companion/reports/jlcpcb-dfm/gate-route-repair.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
