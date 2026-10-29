#!/usr/bin/python3
"""Run real saved-project KiCad checks and geometry/current screens.
Companion zone fill is saved. Original watch PCB is checked WITHOUT refill/save.
No checks are disabled and no manufacturing publication is performed.
"""
import pathlib,subprocess,json,hashlib,collections,sys,time
A=pathlib.Path(__file__).resolve().parents[1];R=A.parents[1];D=A/'reports';log=[]
def run(args):
    result=subprocess.run([str(a) for a in args],cwd=R,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT);log.append(dict(command=[str(a) for a in args],returncode=result.returncode,output=result.stdout));(D/'final-verification-log.json').write_text(json.dumps(log,indent=2));print(result.stdout,flush=True)
    if result.returncode:raise SystemExit(result.returncode)
for project in ['companion','watch']:
    P=R/'kicad'/project;B=P/(project+'.kicad_pcb');S=P/(project+'.kicad_sch')
    run(['kicad-cli','pcb','drc',*(['--refill-zones','--save-board'] if project=='companion' else []),'--schematic-parity','--severity-all','--format','json','-o',D/('final-'+project+'-drc.json'),B])
    run(['kicad-cli','sch','erc','--severity-all','--format','json','-o',D/('final-'+project+'-erc.json'),S])
    if project=='companion':
        run(['kicad-cli','pcb','drc','--schematic-parity','--severity-all','--format','report','-o',D/'companion-drc.rpt',B])
        run(['kicad-cli','sch','erc','--severity-all','--format','report','-o',D/'companion-erc.rpt',S])
        run(['kicad-cli','sch','export','netlist','--format','kicadxml','-o',D/'final-netlist.xml',S])
for script in ['audit_manufacturing_geometry.py','audit_usb_resume.py','power_mesh_resume.py','validate.py','mechanical_revision.py','draw_mechanical_revision.py']:
    run([sys.executable,A/'scripts'/script])
summary={}
for project in ['companion','watch']:
    drc=json.loads((D/('final-'+project+'-drc.json')).read_text());erc=json.loads((D/('final-'+project+'-erc.json')).read_text());erc_rows=[row for sheet in erc['sheets'] for row in sheet['violations']]
    summary[project]=dict(drc_errors=sum(row['severity']=='error' for row in drc['violations']),drc_warnings=sum(row['severity']=='warning' for row in drc['violations']),unconnected_items=len(drc['unconnected_items']),schematic_parity=len(drc['schematic_parity']),erc_errors=sum(row['severity']=='error' for row in erc_rows),erc_warnings=sum(row['severity']=='warning' for row in erc_rows),erc_categories=dict(collections.Counter(row['type'] for row in erc_rows)))
summary['inputs_sha256']={str(f.relative_to(R)):hashlib.sha256(f.read_bytes()).hexdigest() for project in ['companion','watch'] for f in (R/'kicad'/project).iterdir() if f.suffix in ('.kicad_pcb','.kicad_sch','.kicad_pro','.kicad_dru')}
(D/'final-verification.json').write_text(json.dumps(summary,indent=2));print(json.dumps(summary,indent=2))
