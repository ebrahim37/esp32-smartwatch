#!/usr/bin/python3
"""Flare BQ25606 PMID to C2 and remove superseded crossing bootstrap remnants."""
import pathlib,shutil
import pcbnew as p
R=pathlib.Path(__file__).resolve().parents[1];P=R.parents[1]/'kicad/companion/companion.kicad_pcb';b=p.LoadBoard(str(P));mm=p.FromMM
checkpoint=R/'checkpoints/pre-charger-pmid.kicad_pcb'
if not checkpoint.exists():shutil.copy2(P,checkpoint)
net=next(t.GetNetCode() for t in b.GetTracks() if 'PMID' in t.GetNetname());obsolete=[t for t in b.GetTracks() if t.GetNetCode()==net or t.m_Uuid.AsString() in {'ae24c9b9-80a6-4df3-bf73-f5dd5dbf8159','8d52a385-2be8-464f-b60f-55d6525ceca9','9ba4cd10-baf9-4211-994b-919919279bba'}]
for coords,width in [([(73.25,73.95),(73.25,73.5),(72.75,73.0)],.3), ([(72.75,73.0),(71.975,73.0),(71.5,72.525)],.6)]:
    for a,c in zip(coords,coords[1:]):
        t=p.PCB_TRACK(b);t.SetStart(p.VECTOR2I(mm(a[0]),mm(a[1])));t.SetEnd(p.VECTOR2I(mm(c[0]),mm(c[1])));t.SetWidth(mm(width));t.SetLayer(p.F_Cu);t.SetNetCode(net);b.Add(t)
for t in obsolete:b.Remove(t)
p.SaveBoard(str(P),b);print('PMID 0.3mm pad escape -> 0.6mm C2 loop; obsolete bootstrap crossings removed')
