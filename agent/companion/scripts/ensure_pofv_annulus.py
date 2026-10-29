#!/usr/bin/python3
"""Prefer drill+0.15mm via lands for filled/capped construction; DRC rollback.
Hard board annular-ring rules remain unchanged. This implements the larger
2026 POFV-guide land where the existing clearance geometry permits it.
"""
import pathlib,json,subprocess,shutil
import pcbnew as p
A=pathlib.Path(__file__).resolve().parents[1];P=A.parents[1]/'kicad/companion/companion.kicad_pcb';D=A/'routing/pofv-annulus';D.mkdir(exist_ok=True)
def drc(name):
    out=D/(name+'.json');subprocess.run(['kicad-cli','pcb','drc','--refill-zones','--save-board','--severity-all','--schematic-parity','--format','json','-o',str(out),str(P)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL);return json.loads(out.read_text())
b=p.LoadBoard(str(P));ids=[t.m_Uuid.AsString() for t in b.GetTracks() if isinstance(t,p.PCB_VIA) and t.GetFillingMode()==p.FILLING_MODE_FILLED and t.GetWidth(p.F_Cu)-t.GetDrillValue()<p.FromMM(.15)-1];log=[]
if ids:
    baseline=drc('baseline');shutil.copy2(P,A/'checkpoints/pre-pofv-annulus.kicad_pcb')
    for uid in ids:
        backup=P.read_bytes();b=p.LoadBoard(str(P));via=next(t for t in b.GetTracks() if t.m_Uuid.AsString()==uid);before=p.ToMM(via.GetWidth(p.F_Cu));via.SetWidth(via.GetDrillValue()+p.FromMM(.15));p.SaveBoard(str(P),b);report=drc('candidate');accepted=not any(v['severity']=='error' for v in report['violations']) and len(report['unconnected_items'])<=len(baseline['unconnected_items']) and not report['schematic_parity'];log.append(dict(uuid=uid,before_diameter_mm=before,accepted=accepted))
        if accepted:baseline=report
        else:P.write_bytes(backup)
(A/'reports/pofv-annulus.json').write_text(json.dumps(log,indent=2));print(json.dumps(log,indent=2))
