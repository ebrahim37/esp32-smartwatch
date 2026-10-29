#!/usr/bin/python3
"""Remove electrically dead copper, never a schematic-required connection.
Native connectivity identifies pad-free signal components. Then exact KiCad
 dangling-item findings are trial-pruned one at a time with DRC rollback.
"""
import pathlib,json,subprocess,shutil,sys
import pcbnew as p
from sexp_spans import parse,apply
def remove_copper(ids):
    text=path.read_text();root=parse(text);edits=[]
    for node in root.items:
        if not hasattr(node,'key') or node.key not in ('segment','via','arc'):continue
        uid=node.child('uuid')
        if uid and uid.items[1] in ids:edits.append((node.start,node.end,''))
    assert len(edits)==len(ids),(len(edits),len(ids))
    path.write_text(apply(text,edits))
R=pathlib.Path(__file__).resolve().parents[1];D=R/'routing/prune-resume';D.mkdir(exist_ok=True);source=R.parents[1] / 'kicad/companion/companion.kicad_pcb';T=D/'design';shutil.copytree(R.parents[1] / 'kicad/companion',T,dirs_exist_ok=True,ignore=shutil.ignore_patterns('.history','*.lck'));path=T/'companion.kicad_pcb';log=[]
def drc(label):
    f=D/(label+'.json');subprocess.run(['kicad-cli','pcb','drc','--refill-zones','--save-board','--schematic-parity','--severity-all','--format','json','-o',str(f),str(path)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL);return json.loads(f.read_text())
def errors(r):return {(v['type'],tuple(sorted(i['uuid'] for i in v['items']))) for v in r['violations'] if v['severity']=='error'}
def good(n,r):return errors(n)<=errors(r) and len(n['unconnected_items'])<=len(r['unconnected_items']) and len(n['schematic_parity'])<=len(r['schematic_parity'])
b=p.LoadBoard(str(path));b.BuildConnectivity();conn=b.GetConnectivity();tracks=list(b.GetTracks());pads=[a for f in b.GetFootprints() for a in f.Pads()];lookup={q.m_Uuid.AsString():q for q in tracks+pads};seen=set();remove=[];dead=[]
def protected(n):return n=='GND' or n.startswith('/Power/') or any(z in n for z in ('MODEM_3V8','SYS_3V3','_ANT','J_LTE1','J_GNSS1','C50-Pad1'))
for root in tracks:
    if root.m_Uuid.AsString() in seen or protected(root.GetNetname()) or root.IsLocked():continue
    pending=[root];component=[];haspad=False;haszone=False
    while pending:
        q=pending.pop();uid=q.m_Uuid.AsString()
        if uid in seen:continue
        seen.add(uid);component.append(q);haspad|=isinstance(q,p.PAD)
        for raw in conn.GetConnectedItems(q):
            typed=lookup.get(raw.m_Uuid.AsString())
            if typed is not None and typed.GetNetCode()==root.GetNetCode():pending.append(typed)
            elif isinstance(raw,p.ZONE):haszone=True
    if not haspad and not haszone:
        remove.extend(component);dead.append(dict(net=root.GetNetname(),items=[q.m_Uuid.AsString() for q in component]))
report=drc('baseline');backup=path.read_bytes()
if remove:remove_copper({q.m_Uuid.AsString() for q in remove})
if remove:
    new=drc('floating-copper-pruned')
    if good(new,report):report=new;shutil.copy2(path,source);print('Removed pad-free signal copper',len(remove),'items;',len(report['unconnected_items']),'opens',flush=True)
    else:path.write_bytes(backup);dead=[]
# Each accepted deletion is real DRC-proved. Never deleting unconnected PADs.
tried=set()
for _ in range(200):
    rows=[v for v in report['violations'] if v['type'] in ('track_dangling','via_dangling') and v['items'][0]['uuid'] not in tried]
    if not rows:break
    uid=rows[0]['items'][0]['uuid'];tried.add(uid);b=p.LoadBoard(str(path));tracks=list(b.GetTracks());q=next((t for t in tracks if t.m_Uuid.AsString()==uid),None)
    if q is None or q.IsLocked():continue
    backup=path.read_bytes();name=q.GetNetname();remove_copper({uid});new=drc('candidate');ok=good(new,report)
    log.append(dict(uuid=uid,net=name,accepted=ok))
    if ok:report=new;shutil.copy2(path,source);print('PRUNE',name,'opens',len(report['unconnected_items']),flush=True)
    else:path.write_bytes(backup)
    (D/'prune-log.json').write_text(json.dumps(dict(dead_components=dead,dangling_trials=log),indent=2))
(D/'final-drc.json').write_text(json.dumps(report,indent=2));(D/'prune-log.json').write_text(json.dumps(dict(dead_components=dead,dangling_trials=log),indent=2));print('Copper pruning final:',len(report['unconnected_items']),'opens;',len(report['violations']),'other violations',flush=True)
