#!/usr/bin/python3
"""Export repair snapshot with DRC-illegal tracks ripped up, RF fixed.
Export-only surface planes are omitted because DSN cannot encode KiCad's zone
priority overlaps. They remain in the electrical source PCB and are refilled.
"""
import pathlib,json,re,sys
import pcbnew as p
ROOT=pathlib.Path(__file__).resolve().parents[1];path=ROOT.parents[1] / 'kicad/companion/companion.kicad_pcb';b=p.LoadBoard(str(path))
report=json.loads((ROOT/'reports/routed-first-drc.json').read_text())
bad={it['uuid'] for v in report['violations'] if v['severity']=='error' for it in v['items'] if it['description'].startswith('Track ')}
removed=[]
if '--export-only' not in sys.argv:
    for t in list(b.GetTracks()):
        if t.m_Uuid.AsString() in bad:removed.append(t.GetNetname());b.Remove(t)
        else:t.SetLocked(t.GetNetname() in ('/Modem/GNSS_ANT','/Modem/LTE_ANT','Net-(J_GNSS1-SIN)','Net-(J_LTE1-SIN)','Net-(C50-Pad1)'))
    p.SaveBoard(str(path),b)
    b=p.LoadBoard(str(path))
output=ROOT/'routing/repair.dsn';assert p.ExportSpecctraDSN(b,str(output))
# Parse DSN structurally so class labels and stronger PTH clearances are explicit,
# not broad regex replacements across copper/net names.
tokens=re.findall(r'"(?:\\.|[^"\\])*"|[()]|[^\s()]+',output.read_text());stack=[];tree=None
for tok in tokens:
    if tok=='(':
        n=[]
        if stack:stack[-1].append(n)
        else:tree=n
        stack.append(n)
    elif tok==')':stack.pop()
    else:stack[-1].append(tok)
for node in tree:
    if not isinstance(node,list):continue
    if node[0]=='structure':
        node[:]=[x for x in node if not (isinstance(x,list) and x and x[0]=='plane' and len(x)>2 and isinstance(x[2],list) and len(x[2])>1 and x[2][1] in ('F.Cu','B.Cu'))]
        for rule in node:
            if isinstance(rule,list) and rule and rule[0]=='rule':
                rule.append(['clearance','200',['type','pin_wire']])
                # Existing pads are fixed; do not export KiCad's37.5um shortcut.
                for r in rule:
                    if isinstance(r,list) and len(r)>2 and r[0]=='clearance' and isinstance(r[-1],list) and r[-1]==['type','smd_smd']:r[1]='150'
    if node[0]=='network':
        for cls in node:
            if isinstance(cls,list) and cls and cls[0]=='class':cls[1]=cls[1].split(',')[0]
def dump(n,level=0):
    if not isinstance(n,list):return n
    if all(not isinstance(a,list) for a in n):return '('+' '.join(n)+')'
    return '('+'\n'.join(('  '*(level+1) if i else '')+dump(a,level+1) for i,a in enumerate(n))+')'
output.write_text(dump(tree)+'\n')
print('Removed DRC-illegal track segments',removed,'exported',output)
