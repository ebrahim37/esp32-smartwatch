#!/usr/bin/python3
"""Recover compatible checkpoint copper only through real KiCad DRC acceptance.
Preserves current placement, power geometry, RF, constraints and all existing copper
except individually identified violating segments. All trials isolated; no rule edits.
"""
import pathlib, json, shutil, subprocess, collections, time
import pcbnew as p
R=pathlib.Path(__file__).resolve().parents[1]; D=R/'routing/resume'; D.mkdir(exist_ok=True)
source=R.parents[1] / 'kicad/companion/companion.kicad_pcb'; trial=D/'design';shutil.copytree(R.parents[1] / 'kicad/companion',trial,dirs_exist_ok=True,ignore=shutil.ignore_patterns('.history','*.lck')); dest=trial/'companion.kicad_pcb'
mm=p.FromMM; to=p.ToMM
history=[]
def drc(name):
    out=D/(name+'.json')
    subprocess.run(['kicad-cli','pcb','drc','--refill-zones','--save-board','--schematic-parity','--severity-all','--format','json','-o',str(out),str(dest)],check=True,stdout=subprocess.DEVNULL)
    return json.loads(out.read_text())
def errs(r):return {(v['type'],tuple(sorted(i['uuid'] for i in v['items']))) for v in r['violations'] if v['severity']=='error'}
def counts(r):return dict(errors=len(errs(r)),warnings=sum(v['severity']=='warning' for v in r['violations']),opens=len(r['unconnected_items']),parity=len(r['schematic_parity']))
b=p.LoadBoard(str(dest)); oldreport=json.loads((R/'reports/power-geometry-drc.json').read_text())
# Exact existing violations, retain high-current VSYS track; remove signal pieces.
remove={'82365df1-9c38-467e-b4ed-7382b491b85f','79387e5f-2850-4bd0-a106-7250014d0241','df19b2b4-5e0e-41e3-802b-d7ae2ae3e65b'}
tracks=list(b.GetTracks())
# Replace a tangent pad/via contact by a full-width bridge, not a rule waiver.
lookup={x.m_Uuid.AsString():x for x in [*tracks,*(a for f in b.GetFootprints() for a in f.Pads())]}
a=lookup['2fff87ea-1afc-4558-a71a-320d60c5942d'];v=lookup['79dff439-d299-437d-8c69-edd02d15be77']
if not any(not isinstance(t,p.PCB_VIA) and t.GetStart()==a.GetPosition() and t.GetEnd()==v.GetPosition() and t.GetNetCode()==a.GetNetCode() for t in tracks):
    t=p.PCB_TRACK(b);t.SetStart(a.GetPosition());t.SetEnd(v.GetPosition());t.SetLayer(p.F_Cu);t.SetWidth(mm(.15));t.SetNetCode(a.GetNetCode());b.Add(t)
for obsolete in tracks:
    if obsolete.m_Uuid.AsString() in remove:b.Remove(obsolete)
p.SaveBoard(str(dest),b);report=drc('copper-errors-repaired');shutil.copy2(dest,source)
history.append(dict(stage='exact copper repair',before=counts(oldreport),after=counts(report)))
# Earlier checkpoint contains useful signal work but obsolete component/power poses.
protected=lambda n:n=='GND' or n.startswith('/Power/') or n in ('/Modem/MODEM_3V8','/ESP32/SYS_3V3') or any(x in n for x in ('_ANT','J_LTE1','J_GNSS1','C50-Pad1'))
for cp in ['pre-switch-power-geometry.kicad_pcb','pre-usb-pose-1793250969.kicad_pcb']:
    old=p.LoadBoard(str(R/'checkpoints'/cp)); cur=p.LoadBoard(str(dest)); currentids={t.m_Uuid.AsString() for t in cur.GetTracks()}; groups=collections.defaultdict(list)
    oldposes={f.GetReference():(f.GetX(),f.GetY(),f.GetOrientationDegrees(),f.GetLayer()) for f in old.GetFootprints()}
    moved=[f.GetReference() for f in cur.GetFootprints() if oldposes.get(f.GetReference())!=(f.GetX(),f.GetY(),f.GetOrientationDegrees(),f.GetLayer())]
    for t in old.GetTracks():
        if t.m_Uuid.AsString() not in currentids and not protected(t.GetNetname()):groups[t.GetNetname()].append(t)
    history.append(dict(checkpoint=cp,moved_footprints=moved,recoverable_net_candidates={k:len(v) for k,v in groups.items()}))
    for n,items in sorted(groups.items(),key=lambda kv:-len(kv[1])):
        b=p.LoadBoard(str(dest)); backup=dest.read_bytes();net=b.FindNet(n)
        if not net:continue
        for t in items:
            clone=p.Cast_to_BOARD_ITEM(t.Clone());q=p.Cast_to_PCB_VIA(clone) if isinstance(t,p.PCB_VIA) else p.Cast_to_PCB_TRACK(clone);q.SetNetCode(net.GetNetCode());b.Add(q)
        p.SaveBoard(str(dest),b);new=drc('recovery-candidate')
        good=errs(new)<=errs(report) and len(new['unconnected_items'])<len(report['unconnected_items']) and len(new['schematic_parity'])<=len(report['schematic_parity'])
        history.append(dict(checkpoint=cp,net=n,added_items=len(items),accepted=good,before=counts(report),after=counts(new)))
        if good:
            report=new;shutil.copy2(dest,source);print('RECOVER',n,counts(report),flush=True)
        else:dest.write_bytes(backup)
        (D/'recovery-log.json').write_text(json.dumps(history,indent=2))
(D/'recovered-drc.json').write_text(json.dumps(report,indent=2));(D/'recovery-log.json').write_text(json.dumps(history,indent=2));print('Recovered baseline',counts(report),flush=True)
