#!/usr/bin/python3
"""Preserve copper land patterns; fix demonstrably shifted 3D model origins.
Provide project-local, artwork-adjusted libraries under the SAME schematic IDs.
No global KiCad library and no schematic connectivity is modified.
"""
import pathlib,json,sys
import pcbnew as p
ROOT=pathlib.Path(__file__).resolve().parents[1];path=ROOT.parents[1] / 'kicad/companion/companion.kicad_pcb';b=p.LoadBoard(str(path))
# Native VRML coordinate envelopes were measured by mechanical.py. Gold feet and
# the asymmetric USB locating pegs calibrate these transforms, not visual guesses.
corrections={'U7':(-7.557008,0,0),'USBC1':(0,-2.24,0),'U2':(0,-1.055,0)}
if '--libraries-only' not in sys.argv:
    for ref,offset in corrections.items():
        f=b.FindFootprintByReference(ref)
        models=f.Models()
        for index in range(len(models)):
            model=models[index]
            model.m_Offset=p.VECTOR3D(*offset)
            models[index]=model
    # Missing vendor solid: use the documented mechanical clearance envelope.
    f=b.FindFootprintByReference('U10')
    if not list(f.Models()):
        model=p.FP_3DMODEL();model.m_Filename='${KIPRJMOD}/libs/jlcpcb.3dshapes/EG912U-mechanical-envelope.wrl';f.Add3DModel(model)
    p.SaveBoard(str(path),b)
seen=set();libs={};writer=p.PCB_IO_KICAD_SEXPR()
for f in b.GetFootprints():
    lib=str(f.GetFPID().GetLibNickname());item=str(f.GetFPID().GetLibItemName());key=(lib,item)
    if key in seen:continue
    seen.add(key);libpath=ROOT.parents[1] / 'kicad/companion/layout-footprints'/(lib+'.pretty');libpath.mkdir(parents=True,exist_ok=True);libs[lib]='${KIPRJMOD}/layout-footprints/'+lib+'.pretty'
    clone=p.FOOTPRINT(f)
    if clone.IsFlipped():clone.Flip(clone.GetPosition(),False)
    clone.SetOrientationDegrees(0);clone.SetPosition(p.VECTOR2I(0,0))
    writer.FootprintSave(str(libpath),clone)
(ROOT/'reports/local-libraries.json').write_text(json.dumps(dict(libraries=libs,unique_footprints=len(seen),model_corrections_mm=corrections,modem_model='official29x25x2.4mm rectangular envelope; not detailed vendor solid'),indent=2))
print('Local libraries',libs,'footprints',len(seen),flush=True)
