#!/usr/bin/python3
"""Move solder-wicking via holes off component lands with legal dogbones.
Each single-via move is checked by actual refilled KiCad DRC; rollback if any
new mandatory error, open or parity issue appears. No footprint/rule edits.
"""
import pathlib,json,subprocess,shutil,math,collections
import pcbnew as p
from shapely.geometry import Point,LineString,box
R=pathlib.Path(__file__).resolve().parents[1];D=R/'routing/via-repair';D.mkdir(exist_ok=True);source=R.parents[1] / 'kicad/companion/companion.kicad_pcb';T=D/'design';shutil.copytree(R.parents[1] / 'kicad/companion',T,dirs_exist_ok=True,ignore=shutil.ignore_patterns('.history','*.lck'));path=T/'companion.kicad_pcb';mm=p.FromMM;to=p.ToMM
layers=[p.F_Cu,p.In1_Cu,p.In2_Cu,p.B_Cu];log=[]
def drc(label):
    out=D/(label+'.json');subprocess.run(['kicad-cli','pcb','drc','--refill-zones','--save-board','--schematic-parity','--severity-all','--format','json','-o',str(out),str(path)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL);return json.loads(out.read_text())
def errors(r):return {(v['type'],tuple(sorted(i['uuid'] for i in v['items']))) for v in r['violations'] if v['severity']=='error'}
def pos(a):return(to(a.GetPosition().x),to(a.GetPosition().y))
def land(a):
    r=a.GetBoundingBox();return box(to(r.GetLeft()),to(r.GetTop()),to(r.GetRight()),to(r.GetBottom()))
def cu(t):return Point(pos(t)).buffer(to(t.GetWidth(p.F_Cu))/2) if isinstance(t,p.PCB_VIA) else LineString([pos(t),(to(t.GetEnd().x),to(t.GetEnd().y))]).buffer(to(t.GetWidth())/2)
def vec(q):return p.VECTOR2I(mm(q[0]),mm(q[1]))
report=drc('baseline');b=p.LoadBoard(str(path));allpads=[a for f in b.GetFootprints() for a in f.Pads()];ids=[]
for v in b.GetTracks():
    if isinstance(v,p.PCB_VIA) and any(a.GetAttribute()==p.PAD_ATTRIB_SMD and Point(pos(v)).buffer(to(v.GetDrillValue())/2).intersects(land(a)) for a in allpads):ids.append(v.m_Uuid.AsString())
for uid in ids:
    b=p.LoadBoard(str(path));tracks=list(b.GetTracks());v=next(t for t in tracks if t.m_Uuid.AsString()==uid);padlist=[a for f in b.GetFootprints() for a in f.Pads()];obstacles=[(land(a),a.GetNetCode(),a) for a in padlist];x,y=pos(v);net=v.GetNetCode();rad=to(v.GetWidth(p.F_Cu))/2;dr=to(v.GetDrillValue())/2
    hitpads=[a for g,n,a in obstacles if a.GetAttribute()==p.PAD_ATTRIB_SMD and Point(x,y).buffer(dr).intersects(g)];name=v.GetNetname();power=any(q in name for q in ('VSYS','VBAT','MODEM_3V8','USB_5V','LTE_SW'))
    # Do not shrink power vias to disguise a current-capacity problem.
    width=.6 if power else .15;used=set()
    trackgeoms=[(t,cu(t)) for t in tracks]
    for t in tracks:
        if t is v or t.m_Uuid.AsString()==uid or t.GetNetCode()!=net:continue
        if not isinstance(t,p.PCB_VIA) and cu(t).distance(Point(x,y))<rad+.005:used.add(t.GetLayer())
    for a in hitpads:
        used.update(l for l in layers if a.IsOnLayer(l))
    if not used:continue
    options=[]
    for radius in (.4,.6,.8,1.,1.2,1.5):
        for angle in range(0,360,30):
            xx=round(x+radius*math.cos(math.radians(angle)),3);yy=round(y+radius*math.sin(math.radians(angle)),3);q=Point(xx,yy);vg=q.buffer(rad);line=LineString([(x,y),(xx,yy)]);tg=line.buffer(width/2)
            if not box(50.2,50.2,131.8,88.3).covers(vg):continue
            if any(q.buffer(dr+.075).intersects(g) for g,n,a in obstacles if a.GetAttribute()==p.PAD_ATTRIB_SMD):continue
            if any(n!=net and (vg.distance(g)<.13 or any(a.IsOnLayer(l) for l in used) and tg.distance(g)<(.20 if a.GetDrillSize().x else .105)) for g,n,a in obstacles):continue
            if any(a.GetDrillSize().x and q.distance(Point(pos(a)))<dr+max(to(a.GetDrillSize().x),to(a.GetDrillSize().y))/2+.45 for a in padlist):continue
            bad=False
            for t,g in trackgeoms:
                if t.m_Uuid.AsString()==uid:continue
                if isinstance(t,p.PCB_VIA) and q.distance(Point(pos(t)))<dr+to(t.GetDrillValue())/2+.26:bad=True;break
                if t.GetNetCode()!=net and (vg.distance(g)<.205 or (isinstance(t,p.PCB_VIA) or t.GetLayer() in used) and tg.distance(g)<.205):bad=True;break
            if bad:continue
            options.append((xx,yy))
        if options:break
    row=dict(uuid=uid,net=name,old=[x,y],hitpads=[a.GetParentFootprint().GetReference()+'.'+a.GetNumber() for a in hitpads],options=len(options),accepted=False)
    if not options:log.append(row);continue
    backup=path.read_bytes()
    for xx,yy in options[:4]:
        b=p.LoadBoard(str(path));v=next(t for t in b.GetTracks() if t.m_Uuid.AsString()==uid);v.SetPosition(vec((xx,yy)))
        for layer in used:
            if layer==p.In1_Cu and name!='GND':continue
            t=p.PCB_TRACK(b);t.SetStart(vec((x,y)));t.SetEnd(vec((xx,yy)));t.SetWidth(mm(width));t.SetLayer(layer);t.SetNetCode(net);b.Add(t)
        p.SaveBoard(str(path),b);new=drc('candidate');good=errors(new)<=errors(report) and len(new['unconnected_items'])<=len(report['unconnected_items']) and len(new['schematic_parity'])<=len(report['schematic_parity'])
        if good:
            row.update(accepted=True,new=[xx,yy]);report=new;shutil.copy2(path,source);print('DOGBONE',name,row['hitpads'],flush=True);break
        path.write_bytes(backup)
    log.append(row);(D/'repair-log.json').write_text(json.dumps(log,indent=2))
(D/'repair-log.json').write_text(json.dumps(log,indent=2));(D/'final-drc.json').write_text(json.dumps(report,indent=2));print('Moved',sum(r['accepted'] for r in log),'of',len(ids),'via-in-pad holes',flush=True)
