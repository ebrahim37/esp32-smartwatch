#!/usr/bin/python3
"""Free CP2102N pin15's only escape from an unrelated In2 VBUS-sense diagonal.
Reroute only that local segment; add one filled/capped through-via in pin15.
All component locations, net assignments, rules and In1 GND are unchanged.
"""
import pathlib,shutil,json
import pcbnew as p
R=pathlib.Path(__file__).resolve().parents[1];P=R.parents[1]/'kicad/companion/companion.kicad_pcb';b=p.LoadBoard(str(P));mm=p.FromMM
shutil.copy2(P,R/'checkpoints/pre-uart-active-launch.kicad_pcb')
t=next(q for q in b.GetTracks() if q.m_Uuid.AsString()=='8ab0dbb7-15dc-4e06-a237-a8437a2eef2a');net=t.GetNetCode();b.Remove(t)
coords=[(114.5,84.15),(115.45,84.15),(117.4,82.2),(117.4,81.25)]
for a,c in zip(coords,coords[1:]):
    q=p.PCB_TRACK(b);q.SetStart(p.VECTOR2I(mm(a[0]),mm(a[1])));q.SetEnd(p.VECTOR2I(mm(c[0]),mm(c[1])));q.SetWidth(mm(.15));q.SetLayer(p.In2_Cu);q.SetNetCode(net);b.Add(q)
v=p.PCB_VIA(b);v.SetPosition(p.VECTOR2I(mm(115),mm(83.75)));v.SetWidth(mm(.25));v.SetDrill(mm(.15));v.SetLayerPair(p.F_Cu,p.B_Cu);v.SetViaType(p.VIATYPE_THROUGH);v.SetNetCode(b.FindNet('/USB/CP_UART_ACTIVE').GetNetCode());v.SetFillingMode(p.FILLING_MODE_FILLED);v.SetCappingMode(p.CAPPING_MODE_CAPPED);b.Add(v)
p.SaveBoard(str(P),b);print('Local VBUS-sense diagonal detoured; CP_UART_ACTIVE pin15 has filled/capped 0.25/0.15 via launch')
