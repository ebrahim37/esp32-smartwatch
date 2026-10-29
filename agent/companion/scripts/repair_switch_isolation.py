#!/usr/bin/python3
"""Restore the reserved SW1 switching corridor above overlapping B.Cu supply fills.
In2 remains VSYS/MODEM power; In1 remains GND. No placement/rule changes.
Remove the superseded open-ended USB bridge on the unused second launch only.
"""
import pathlib,shutil,json
import pcbnew as p
R=pathlib.Path(__file__).resolve().parents[1];P=R.parents[1]/'kicad/companion/companion.kicad_pcb'
b=p.LoadBoard(str(P));mm=p.FromMM;to=p.ToMM
shutil.copy2(P,R/'checkpoints/pre-switch-isolation.kicad_pcb');changed=[]
for z in b.Zones():
    if not z.GetIsRuleArea() and z.GetNetname()=='/Power/LTE_SW1' and z.GetLayer()==p.B_Cu and z.GetAssignedPriority()==3:
        z.SetAssignedPriority(30);changed.append({'net':z.GetNetname(),'layer':'B.Cu','old_priority':3,'new_priority':30})
removed=[]
for t in list(b.GetTracks()):
    if t.m_Uuid.AsString()=='131472b5-4559-4eca-a4df-fba3433f3c62' or (not isinstance(t,p.PCB_VIA) and t.GetLayer()==p.B_Cu and t.GetNetname()=='/Power/USB_5V' and t.GetStart()==p.VECTOR2I(mm(127.4),mm(80.05)) and t.GetEnd()==p.VECTOR2I(mm(127.4),mm(81.44))):
        removed.append(t.m_Uuid.AsString());b.Remove(t)
p.SaveBoard(str(P),b);print(json.dumps({'priority_changes':changed,'obsolete_open_bridge_removed':removed}))
