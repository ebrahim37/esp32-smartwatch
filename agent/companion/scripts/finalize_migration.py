#!/usr/bin/python3
"""Lossless companion project-name migration; verify independently with KiCad CLI.
The watch schematic/layout are not modified. Referenced sheet UUIDs stay intact.
"""
import pathlib,json,re,hashlib,tarfile
from sexp_spans import parse,Node,apply
R=pathlib.Path(__file__).resolve().parents[3]; A=R/'agent/companion'; P=R/'kicad/companion'
def visit(n):
    yield n
    for c in n.items:
        if isinstance(c,Node):yield from visit(c)
changed=[]
for f in P.glob('*.kicad_sch'):
    text=f.read_text();edits=[]
    for n in visit(parse(text)):
        if n.key=='project' and n.items[1]=='watch':
            fragment=text[n.start:n.end];updated=re.sub(r'^(\(project\s+)"watch"',r'\1"companion"',fragment,count=1);edits.append((n.start,n.end,updated))
        elif n.key=='property' and n.items[1]=='Datasheet' and n.items[2].startswith('${KIPRJMOD}/../datasheets/'):
            edits.append((n.start,n.end,text[n.start:n.end].replace('${KIPRJMOD}/../datasheets/','${KIPRJMOD}/../../datasheets/')))
    if edits:f.write_text(apply(text,edits));changed.append(str(f.relative_to(R)))
f=P/'companion.kicad_pro';cfg=json.loads(f.read_text());cfg['meta']['filename']='companion.kicad_pro'
for sheet in cfg.get('sheets',[]):
    if isinstance(sheet,dict) and sheet.get('filename')=='watch.kicad_sch':sheet['filename']='companion.kicad_sch'
f.write_text(json.dumps(cfg,indent=2)+'\n')
import pcbnew as p
board=P/'companion.kicad_pcb';b=p.LoadBoard(str(board));changed_fields=False
for footprint in b.GetFootprints():
    field=footprint.GetField('Datasheet')
    if field and field.GetText().startswith('${KIPRJMOD}/../datasheets/'):
        field.SetText(field.GetText().replace('${KIPRJMOD}/../datasheets/','${KIPRJMOD}/../../datasheets/'));changed_fields=True
if changed_fields:p.SaveBoard(str(board),b)
# Compare unmodified watch circuitry/layout/library bytes to the recovery archive.
checks=[]
with tarfile.open(A/'checkpoints/watch4-before-migration-20261029.tar.gz') as archive:
    for member in archive.getmembers():
        if member.isfile() and member.name.startswith('kicad/') and '/.history/' not in member.name and not member.name.endswith('.kicad_prl'):
            target=R/'kicad/watch'/member.name.removeprefix('kicad/')
            assert target.read_bytes()==archive.extractfile(member).read(),str(target)
            checks.append(str(target.relative_to(R)))
report={'companion_project_instance_files_migrated':changed,'watch_files_byte_identical_to_recovery':checks,'historical_releases':'agent/companion/archive/jlcpcb_release*','canonical_companion':'kicad/companion/companion.kicad_pro'}
(A/'reports/migration-integrity.json').write_text(json.dumps(report,indent=2));print(json.dumps({'renamed_schematic_files':len(changed),'unchanged_watch_files':len(checks)}))
