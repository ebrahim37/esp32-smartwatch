#!/usr/bin/python3
"""Export unit-board manufacturing artifacts with an explicit evidence-based gate.
JLCPCB performs panelization: this script never creates a panel or panel Gerbers.
Any unresolved electrical defect marks the entire package NOT FOR ORDERING.
"""
import pathlib,shutil,subprocess,json,collections,re,hashlib,zipfile
R=pathlib.Path(__file__).resolve().parents[1];OUT=R / 'archive/release-candidate/rev-a';OUT.mkdir(parents=True,exist_ok=True)
log=[]
def run(args):
    x=subprocess.run(args,cwd=R.parent,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True);log.append({'command':args,'returncode':x.returncode,'output':x.stdout})
    (OUT/'export-log.json').write_text(json.dumps(log,indent=2))
    if x.returncode:raise RuntimeError(x.stdout)
proof_path=R/'reports/final-verification.json'
if not proof_path.exists():raise SystemExit('Run scripts/verify.py before exporting this engineering candidate')
proof=json.loads(proof_path.read_text())
for relative,digest in proof['inputs_sha256'].items():
    if relative.startswith('kicad/companion/') and hashlib.sha256((R.parents[1]/relative).read_bytes()).hexdigest()!=digest:
        raise SystemExit('Verification is stale for '+relative+'; rerun scripts/verify.py, assembly.py and write_handoff.py')
run(['/usr/bin/python3',str(R/'scripts/local_libraries.py'),'--libraries-only'])
run(['/usr/bin/python3',str(R/'scripts/assembly.py')])
shutil.copytree(R.parents[1] / 'kicad/companion',OUT/'project',dirs_exist_ok=True)
B=OUT/'project/companion.kicad_pcb';S=OUT/'project/companion.kicad_sch'
run(['kicad-cli','pcb','drc','--refill-zones','--save-board','--schematic-parity','--severity-all','--format','json','-o',str(OUT/'drc.json'),str(B)])
run(['kicad-cli','pcb','drc','--schematic-parity','--severity-all','--format','report','-o',str(OUT/'companion-drc.rpt'),str(B)])
run(['kicad-cli','sch','erc','--severity-all','--format','json','-o',str(OUT/'erc.json'),str(S)])
run(['kicad-cli','sch','erc','--severity-all','--format','report','-o',str(OUT/'companion-erc.rpt'),str(S)])
run(['kicad-cli','sch','export','pdf','-o',str(OUT/'schematic.pdf'),str(S)])
run(['kicad-cli','pcb','export','gerbers','--layers','F.Cu,In1.Cu,In2.Cu,B.Cu,F.Mask,B.Mask,F.Paste,B.Paste,F.SilkS,B.SilkS,Edge.Cuts','--use-drill-file-origin','--check-zones','-o',str(OUT/'fabrication'),str(B)])
run(['kicad-cli','pcb','export','drill','--format','excellon','--drill-origin','plot','--excellon-units','mm','--excellon-separate-th','--generate-map','--generate-report','--report-path',str(OUT/'fabrication/companion-drill.rpt'),'--map-format','pdf','-o',str(OUT/'fabrication'),str(B)])
run(['kicad-cli','pcb','export','pos','--format','csv','--units','mm','--exclude-dnp','--use-drill-file-origin','-o',str(OUT/'placement-kicad.csv'),str(B)])
run(['kicad-cli','pcb','export','svg','--layers','F.Fab,Edge.Cuts','--mode-single','--fit-page-to-board','--exclude-drawing-sheet','--sketch-pads-on-fab-layers','--crossout-DNP-footprints-on-fab-layers','-o',str(OUT/'assembly-top.svg'),str(B)])
run(['kicad-cli','pcb','export','svg','--layers','B.Fab,Edge.Cuts','--mode-single','--mirror','--fit-page-to-board','--exclude-drawing-sheet','-o',str(OUT/'assembly-bottom.svg'),str(B)])
for side in ('top','bottom'):
    run(['kicad-cli','pcb','render','--side',side,'--width','2000','--height','1200','-o',str(OUT/('pcb-'+side+'.png')),str(B)])
for source,dest in [('research/manufacturing.md','stackup-and-manufacturing.md'),('research/mechanical-revision.md','antenna-and-battery.md'),('reports/mechanical-arrangement.svg','mechanical-arrangement.svg'),('reports/mechanical-revision.json','mechanical-verification.json'),('reports/acceptance-audit.json','electrical-geometry-review.json')]:
    shutil.copy2(R/source,OUT/dest)
for source in ['final-verification.json','manufacturing-geometry.json','via-in-pad-map.csv','assembly.json','power-mesh-resume.json','usb-resume-audit.json','independent-electrical-review.md','sourcing-recheck.md','manufacturing-readiness.md']:
    shutil.copy2(R/'reports'/source,OUT/source)
drc=json.loads((OUT/'drc.json').read_text());erc=json.loads((OUT/'erc.json').read_text());ev=[v for s in erc['sheets'] for v in s['violations']]
counts={'drc_errors':sum(v['severity']=='error' for v in drc['violations']),'drc_warnings':sum(v['severity']=='warning' for v in drc['violations']),'unconnected_items':len(drc['unconnected_items']),'parity_findings':len(drc['schematic_parity']),'erc_errors':sum(v['severity']=='error' for v in ev),'erc_warnings':sum(v['severity']=='warning' for v in ev)}
electrical_clean=not any(counts[k] for k in ('drc_errors','drc_warnings','unconnected_items','parity_findings','erc_errors'))
status={'candidate_revision':'rev-a','electrical_gate_passed':electrical_clean,'ready_for_ordering':False,'reason':'Filled/capped via-in-pad four-layer DFM/quote, stock and BOM/CPL assembly preview, JLC panelization/overhang approval, untyped-symbol ERC review and assembled battery/RF/thermal/USB qualification still required' if electrical_clean else 'Unresolved electrical/layout defects: engineering preview only, NOT FOR ORDERING','counts':counts,'panelization':'JLCPCB supplied; unit-board files only','publication':'Candidate only; never copied automatically into manufacturing/'}
(OUT/'release-status.json').write_text(json.dumps(status,indent=2))
(OUT/'ORDERING-STATUS.txt').write_text(('ELECTRICAL GATE PASSED; MANUFACTURER REVIEW REQUIRED\n' if electrical_clean else 'NOT FOR ORDERING — INCOMPLETE ENGINEERING PREVIEW\n')+status['reason']+'\n'+json.dumps(counts,indent=2)+'\nNo panel files provided. JLCPCB to panelize around supported-pad ESP32 antenna overhang and connector openings.\n')
# Independent checks of exported manufacturing bytes, not generator assertions.
files=list((OUT/'fabrication').iterdir());copper=[];edge=None
for file in files:
    if file.suffix not in ('.gbr','.gtl','.gbl','.g1','.g2','.gm1'):continue
    text=file.read_text(errors='replace')
    if '%TF.FileFunction,Copper,' in text:copper.append(file.name)
    if 'Profile,NP' in text or 'Edge_Cuts' in file.name:edge=file
assert len(copper)==4,('Four exported copper layers required',copper)
assert edge is not None
text=edge.read_text();fmt=re.search(r'%FS[LT]A?X\d(\d)Y\d\d\*%',text);dec=int(fmt.group(1)) if fmt else 6
points=[(int(x)/10**dec,int(y)/10**dec) for x,y in re.findall(r'(?:^|\n)X(-?\d+)Y(-?\d+)',text)]
assert points,'No Gerber outline coordinates'
size=[max(v[i] for v in points)-min(v[i] for v in points) for i in (0,1)]
assert all(abs(a-c)<.01 for a,c in zip(size,(82,38.5))),size
assert any(f.suffix=='.drl' and 'METRIC' in f.read_text() for f in files),'No metric Excellon file'
(OUT/'fabrication-verification.json').write_text(json.dumps({'copper_files':copper,'gerber_outline_size_mm':size,'metric_drill_present':True,'panelized':False},indent=2))
with zipfile.ZipFile(OUT/'unit-board-gerbers-drills.zip','w',zipfile.ZIP_DEFLATED) as z:
    for file in files:z.write(file,'fabrication/'+file.name)
run(['/usr/bin/python3',str(R/'scripts/verify_fabrication.py'),str(OUT)])
manifest={str(f.relative_to(OUT)):hashlib.sha256(f.read_bytes()).hexdigest() for f in OUT.rglob('*') if f.is_file() and f.name!='sha256.json'}
(OUT/'sha256.json').write_text(json.dumps(manifest,indent=2));print(json.dumps(status,indent=2));print('Exported files',len(manifest),'verified unit geometry',size)
