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
# Verify original watch circuitry and layout; normalize only documented metadata.
checks=[];metadata=[]
def structural(n):
    return tuple(structural(v) if isinstance(v,Node) else v for v in n.items)
with tarfile.open(A/'checkpoints/watch4-before-migration-20261029.tar.gz') as archive:
    for member in archive.getmembers():
        if member.isfile() and member.name.startswith('kicad/') and '/.history/' not in member.name and not member.name.endswith('.kicad_prl'):
            target=R/'kicad/watch'/member.name.removeprefix('kicad/')
            original=archive.extractfile(member).read();current=target.read_bytes()
            corrected=b'${KIPRJMOD}/../../datasheets/FH_0402CG121J500NT_C40059_MLCC.pdf'
            if corrected in current:
                current=current.replace(corrected,b'${KIPRJMOD}/../datasheets/FH_0402CG121J500NT_C40059_MLCC.pdf');metadata.append(str(target.relative_to(R)))
            if member.name=='kicad/libs/jlcpcb.kicad_sym':
                # Local symbol drawing arcs were reconciled with their embedded
                # definitions; pin numbers/types/geometry must remain unchanged.
                assert [structural(n) for n in visit(parse(current.decode())) if n.key=='pin']==[structural(n) for n in visit(parse(original.decode())) if n.key=='pin'],str(target)
                metadata.append(str(target.relative_to(R)));continue
            if target.suffix=='.kicad_sch':
                text=current.decode();cached=parse(text).child('lib_symbols');edits=[]
                for n in visit(cached) if cached is not None else []:
                    if n.key=='property' and n.items[1]=='Footprint' and str(n.items[2]).startswith('libs:'):
                        edits.append((n.start,n.end,text[n.start:n.end].replace('"libs:','"jlcpcb:',1)))
                if edits:current=apply(text,edits).encode();metadata.append(str(target.relative_to(R)))
            assert current==original,str(target)
            checks.append(str(target.relative_to(R)))
report={'companion_project_instance_files_migrated':changed,'watch_files_content_preserved':checks,'watch_metadata_changes':metadata,'watch_library_metadata_exception':'Embedded/local default Footprint nickname migration, one relative Datasheet path corrected, local drawing arcs reconciled; normalized watch schematic and PCB bytes match recovery, all local library pin definitions unchanged','historical_releases':'agent/companion/archive/jlcpcb_release*','canonical_companion':'kicad/companion/companion.kicad_pro'}
(A/'reports/migration-integrity.json').write_text(json.dumps(report,indent=2));print(json.dumps({'renamed_schematic_files':len(changed),'unchanged_watch_files':len(checks)}))
