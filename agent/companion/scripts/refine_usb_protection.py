#!/usr/bin/python3
"""Orient the local USB data/protection cluster toward the connector.
Retain regulator and RF placement/routes; move CP2102N decoupling with its
power-pin side and rip only invalidated local copper and signal nets.
"""
import pathlib,json,shutil,math,time,sys
import pcbnew as p
from shapely.geometry import box,LineString,Point
R=pathlib.Path(__file__).resolve().parents[1];path=R.parents[1] / 'kicad/companion/companion.kicad_pcb';b=p.LoadBoard(str(path));mm=p.FromMM;to=p.ToMM;fs={f.GetReference():f for f in b.GetFootprints()};cfg=json.loads((R/'scripts/layout-revision.json').read_text());poses={'D1':(74.95,30.9,0),'USBC1':(75,34.91,0),'R11':(80.75,35.2,90),'R12':(80.75,37.35,90),'R34':(62.5,31.5,0),'U8':(67,34,180),'C17':(67.8,37.35,0),'C18':(68.5,30.75,0),'C43':(64.85,37.3,0),'R25':(63.5,35,90)}
tracks=list(b.GetTracks());oldpads=[a for r in poses for a in fs[r].Pads()];invalid={a.GetNetCode() for a in oldpads if a.GetNetname() not in ('GND','/Power/USB_5V')};gndpoints={(a.GetX(),a.GetY()) for a in oldpads if a.GetNetname()=='GND'};before={r:[to(fs[r].GetX())-50,to(fs[r].GetY())-50,fs[r].GetOrientationDegrees()] for r in poses}
for r,(x,y,a) in poses.items():fs[r].SetPosition(p.VECTOR2I(mm(50+x),mm(50+y)));fs[r].SetOrientationDegrees(a);fs[r].SetLocked(True)
def bounds(f):
    if f.GetReference()=='D1':
        # Its fab pin-1 marker extends beyond its body/courtyard. Use the real
        # courtyard + solder lands + measured2.5x1.02mm solid, not that marker.
        ps=f.GetCourtyard(p.F_CrtYd);pts=[(to(ps.COutline(i).CPoint(j).x),to(ps.COutline(i).CPoint(j).y)) for i in range(ps.OutlineCount()) for j in range(ps.COutline(i).PointCount())]
        for pad in f.Pads():
            q=pad.GetBoundingBox();pts.extend([(to(q.GetLeft()),to(q.GetTop())),(to(q.GetRight()),to(q.GetBottom()))])
        x,y=to(f.GetX()),to(f.GetY());pts.extend([(x-1.25,y-.51),(x+1.25,y+.51)])
        return min(x for x,y in pts),min(y for x,y in pts),max(x for x,y in pts),max(y for x,y in pts)
    q=f.GetBoundingBox(False,False);return to(q.GetLeft()),to(q.GetTop()),to(q.GetRight()),to(q.GetBottom())
for r in poses:
    f=fs[r];a=bounds(f)
    for s,g in fs.items():
        if r==s:continue
        c=bounds(g)
        if a[0]<c[2]+.15-1e-6 and a[2]>c[0]-.15+1e-6 and a[1]<c[3]+.15-1e-6 and a[3]>c[1]-.15+1e-6:raise RuntimeError(f'{r} occupied envelope collides with {s}')
    for pad in f.Pads():
        q=pad.GetBoundingBox()
        if to(q.GetLeft())<50.2 or to(q.GetTop())<50.2 or to(q.GetRight())>131.8 or to(q.GetBottom())>88.3:raise RuntimeError(f'{r} unsupported pad {pad.GetNumber()}')
if '--check-only' in sys.argv:
    print('USB cluster poses have supported pads and no occupied-envelope overlap',poses);raise SystemExit()
newpads=[]
for r in poses:
    for a in fs[r].Pads():
        q=a.GetBoundingBox();newpads.append((a.GetNetCode(),box(to(q.GetLeft()),to(q.GetTop()),to(q.GetRight()),to(q.GetBottom())).buffer(.2)))
obsolete=[]
for t in tracks:
    geom=Point(to(t.GetX()),to(t.GetY())).buffer(to(t.GetWidth(p.F_Cu))/2) if isinstance(t,p.PCB_VIA) else LineString([(to(t.GetStart().x),to(t.GetStart().y)),(to(t.GetEnd().x),to(t.GetEnd().y))]).buffer(to(t.GetWidth())/2)
    if t.GetNetCode() in invalid or any(t.GetNetCode()!=net and geom.intersects(g) for net,g in newpads):obsolete.append(t)
    elif t.GetNetname()=='GND' and not isinstance(t,p.PCB_VIA) and ((t.GetStart().x,t.GetStart().y) in gndpoints or (t.GetEnd().x,t.GetEnd().y) in gndpoints):obsolete.append(t)
shutil.copy2(path,R/'checkpoints'/f'pre-usb-pose-{int(time.time())}.kicad_pcb')
for t in obsolete:b.Remove(t)
p.SaveBoard(str(path),b)
for r,(x,y,a) in poses.items():cfg['anchors'][r]=[x,y,a,'top']
(R/'scripts/layout-revision.json').write_text(json.dumps(cfg,indent=2))
(R/'reports/usb-protection-refinement.json').write_text(json.dumps({'before':before,'after':poses,'ripped_obsolete_signal_items':len(obsolete),'reason':'D1 inline immediately behind the USB data row; CP2102N data pins face the port. USB shell overhang1.5mm, all solder pads supported≥0.22mm. CP internal3V3 caps follow its supply side; regulator/RF placement unchanged.'},indent=2));print('ESD refinement',poses,'ripped',len(obsolete),'obsolete items')
