#!/usr/bin/python3
"""Benchmark native working DSN/SES round trip on an isolated revision copy."""
import pathlib,subprocess,shutil,json,collections,sys
import pcbnew as p
R=pathlib.Path(__file__).resolve().parents[1];S=R/'routing/freerouting-benchmark';S.mkdir(exist_ok=True)
# A self-contained project copy ensures its CLI sees the exact same .dru, .pro,
# footprints, hierarchical schematics and models. Authoritative board not edited.
shutil.copytree(R.parents[1] / 'kicad/companion',S/'design',dirs_exist_ok=True)
result=subprocess.run([sys.executable,str(R/'scripts/run_router.py'),'--revision'],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
(S/'runner.log').write_text(result.stdout)
if result.returncode:print('FreeRouting failed; see',S/'runner.log');raise SystemExit(result.returncode)
b=p.LoadBoard(str(S/'design/companion.kicad_pcb'));poses={f.GetReference():(f.GetX(),f.GetY(),f.GetOrientationDegrees(),f.IsFlipped()) for f in b.GetFootprints()}
assert p.ImportSpecctraSES(b,str(R/'routing/revision.ses'))
assert poses=={f.GetReference():(f.GetX(),f.GetY(),f.GetOrientationDegrees(),f.IsFlipped()) for f in b.GetFootprints()}
p.SaveBoard(str(S/'design/companion.kicad_pcb'),b)
subprocess.run(['kicad-cli','pcb','drc','--refill-zones','--save-board','--schematic-parity','--severity-all','--format','json','-o',str(S/'drc.json'),str(S/'design/companion.kicad_pcb')],check=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
r=json.loads((S/'drc.json').read_text());print(json.dumps({'comparison':'FreeRouting2.5 native export/import','errors':sum(v['severity']=='error' for v in r['violations']),'warnings':sum(v['severity']=='warning' for v in r['violations']),'unconnected':len(r['unconnected_items']),'parity':len(r['schematic_parity'])},indent=2))
