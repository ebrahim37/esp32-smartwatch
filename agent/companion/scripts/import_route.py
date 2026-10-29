#!/usr/bin/python3
"""Import actual SES, preserve footprint positions, add grounded surface pours."""
import pathlib,json,collections
import pcbnew as p
ROOT=pathlib.Path(__file__).resolve().parents[1];path=ROOT.parents[1] / 'kicad/companion/companion.kicad_pcb';b=p.LoadBoard(str(path));mm=p.FromMM
before={f.GetReference():(f.GetX(),f.GetY(),f.GetOrientationDegrees(),f.IsFlipped()) for f in b.GetFootprints()}
assert p.ImportSpecctraSES(b,str(ROOT/'routing/companion.ses'))
after={f.GetReference():(f.GetX(),f.GetY(),f.GetOrientationDegrees(),f.IsFlipped()) for f in b.GetFootprints()}
if before!=after:raise RuntimeError('Session changed component placement')
cfg=json.loads((ROOT/'scripts/layout.json').read_text());ox,oy=cfg['origin'];w,h=cfg['size']
for layer,clearance in ((p.F_Cu,.25),(p.B_Cu,.2)):
    z=p.ZONE(b);z.SetLayer(layer);z.SetNetCode(b.FindNet('GND').GetNetCode());z.SetLocalClearance(mm(clearance));z.SetMinThickness(mm(.15));z.SetPadConnection(p.ZONE_CONNECTION_FULL);z.SetIslandRemovalMode(p.ISLAND_REMOVAL_MODE_ALWAYS)
    poly=z.Outline();poly.NewOutline()
    for x,y in ((0,0),(w,0),(w,h),(0,h)):poly.Append(mm(x+ox),mm(y+oy))
    b.Add(z)
p.SaveBoard(str(path),b);b=p.LoadBoard(str(path));zones=p.ZONES()
for z in b.Zones():zones.push_back(z)
p.ZONE_FILLER(b).Fill(zones);p.SaveBoard(str(path),b)
print('Tracks and vias',len(list(b.GetTracks())),'zones',len(list(b.Zones())),flush=True)
