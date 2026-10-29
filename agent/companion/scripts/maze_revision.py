#!/usr/bin/python3
"""DRC-oracle repair using the interrupted C++ multilayer maze backend.
Conservative pad bounding boxes, exact segment-distance rasters, full-depth via
obstacles, continuous In1 reference (never signal-routed). Every accepted edge
must reduce actual KiCad open count without introducing any copper DRC error.
Power and RF are deliberately excluded from this generic signal repair.
"""
import pathlib,json,subprocess,struct,math,time,sys,shutil,collections,argparse
import numpy as np
import pcbnew as p
R=pathlib.Path(__file__).resolve().parents[1];BIN=R/'routing/maze';SRC=R/'scripts/maze.cpp'
if not BIN.exists() or BIN.stat().st_mtime<SRC.stat().st_mtime:subprocess.run(['g++','-O3','-std=c++20',str(SRC),'-o',str(BIN)],check=True)
def search(block,via,starts,goals):
    nl,ny,nx=block.shape;data=struct.pack('<5I',nx,ny,nl,len(starts),len(goals))+block.astype(np.uint8).tobytes()+via.astype(np.uint8).tobytes()+np.array(starts,dtype='<u4').tobytes()+np.array(goals,dtype='<u4').tobytes()
    r=subprocess.run([str(BIN)],input=data,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=45,check=True);n=struct.unpack('<i',r.stdout[:4])[0]
    return np.frombuffer(r.stdout[4:],dtype='<u4').tolist() if n else [],r.stderr.decode()
if '--smoke' in sys.argv:
    blocked=np.zeros((3,20,20),dtype=bool);blocked[0,:,10]=True;blocked[2,:,10]=True
    via=np.ones((20,20),dtype=bool);path,log=search(blocked,via,[21],[378]);assert path and any(q//400==1 for q in path),log
    assert not any(blocked.ravel()[q] for q in path)
    print('Maze smoke: obstacle avoided via alternate copper layer;',log);raise SystemExit()
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--nets',nargs='*',default=[])
parser.add_argument('--width',type=float,default=.15)
parser.add_argument('--max-iterations',type=int,default=160)
parser.add_argument('--allow-power',action='store_true',help='Explicitly permit named power nets; never automatic')
parser.add_argument('--allow-via-in-pad',action='store_true',help='Last-resort filled/capped via process')
parser.add_argument('--step',type=float,default=.05)
parser.add_argument('--top-only',action='store_true',help='Keep impedance-sensitive repairs on F.Cu')
parser.add_argument('--via-size',type=float,default=.35)
parser.add_argument('--via-drill',type=float,default=.20)
parser.add_argument('--phase',type=float,default=0.,help='Grid phase in mm; avoids sub-grid false blockages')
parser.add_argument('--raster-margin',type=float,default=None,help='Sampling guard only; actual KiCad rules remain authoritative')
parser.add_argument('--connect-uuids',nargs=2,help='Join these two actual copper objects instead of the DRC minimum-airwire pair')
parser.add_argument('--back-preferred',action='store_true',help='Restrict front routing to short launch regions and reserve In2 for power')
args=parser.parse_args()
D=R/'routing/maze-revision';D.mkdir(exist_ok=True)
history=D/'attempts'/str(time.time_ns());history.mkdir(parents=True)
for old in D.glob('*.json'):shutil.copy2(old,history/old.name)
(history/'invocation.json').write_text(json.dumps({'argv':sys.argv,'started':time.time()},indent=2))
shutil.copytree(R.parents[1] / 'kicad/companion',D/'design',dirs_exist_ok=True,ignore=shutil.ignore_patterns('.history','*.lck'));candidate=D/'design/companion.kicad_pcb';source=R.parents[1] / 'kicad/companion/companion.kicad_pcb';step=args.step;ox=oy=50.;W=82.;H=38.5;nx=round(W/step)+1;ny=round(H/step)+1;plane=nx*ny;layers=[p.F_Cu,p.In2_Cu,p.B_Cu];mm=p.FromMM;to=p.ToMM
VR=args.via_size/2;DR=args.via_drill/2;phase=args.phase;guard=step*.72 if args.raster_margin is None else args.raster_margin
assert args.via_size>=.25 and args.via_drill>=.15 and args.via_size-args.via_drill>=.10-1e-8
log=[]
def drc(path,name):
    out=D/(name+'.json');r=subprocess.run(['kicad-cli','pcb','drc','--refill-zones','--save-board','--severity-all','--schematic-parity','--format','json','-o',str(out),str(path)],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,check=True)
    return json.loads(out.read_text())
def errors(report):return sum(v['severity']=='error' for v in report['violations'])
def error_ids(report):
    return {(v['type'],tuple(sorted(i['uuid'] for i in v['items']))) for v in report['violations'] if v['severity']=='error'}
def addshape(mask,x0,y0,x1,y1,radius,segment=False):
    # All coordinates relative to carrier, masks are a single layer each.
    lo=max(0,math.floor((min(x0,x1)-radius-phase)/step));hi=min(nx-1,math.ceil((max(x0,x1)+radius-phase)/step));bot=max(0,math.floor((min(y0,y1)-radius-phase)/step));top=min(ny-1,math.ceil((max(y0,y1)+radius-phase)/step))
    if lo>hi or bot>top:return
    x=np.arange(lo,hi+1)*step+phase;y=np.arange(bot,top+1)*step+phase;xx,yy=np.meshgrid(x,y)
    if segment:
        dx=x1-x0;dy=y1-y0;t=np.clip(((xx-x0)*dx+(yy-y0)*dy)/(dx*dx+dy*dy+1e-30),0,1);occupied=(xx-x0-dx*t)**2+(yy-y0-dy*t)**2<=radius*radius
    else:occupied=(xx>=x0-radius)&(xx<=x1+radius)&(yy>=y0-radius)&(yy<=y1+radius)
    mask[bot:top+1,lo:hi+1]|=occupied

def raster(b,net,width=.15):
    block=np.zeros((3,ny,nx),dtype=bool);via_block=np.zeros((ny,nx),dtype=bool)
    margin=.2+width/2+guard;vm=.2+VR+guard
    xs=np.arange(nx)*step+phase;ys=np.arange(ny)*step+phase
    for li in range(3):block[li]|=(xs[None,:]<margin)|(xs[None,:]>W-margin)|(ys[:,None]<margin)|(ys[:,None]>H-margin)
    via_block|=(xs[None,:]<vm)|(xs[None,:]>W-vm)|(ys[:,None]<vm)|(ys[:,None]>H-vm)
    for f in b.GetFootprints():
        for a in f.Pads():
            r=a.GetBoundingBox();rect=(to(r.GetLeft())-ox,to(r.GetTop())-oy,to(r.GetRight())-ox,to(r.GetBottom())-oy);drill=a.GetDrillSize();same=a.GetNetCode()==net
            if not same:
                for li,layer in enumerate(layers):
                    clearance=.2 if drill.x else (.10 if layer in (p.F_Cu,p.B_Cu) else .15)
                    if a.IsOnLayer(layer) or drill.x:addshape(block[li],*rect,clearance+width/2+guard)
                addshape(via_block,*rect,max(.127+VR,.2+DR)+guard)
            if not args.allow_via_in_pad:
                # Even a same-net solder land is a via obstacle: prefer dogbones.
                addshape(via_block,*rect,VR+.04+guard)
            if drill.x and (not same or a.GetAttribute()==p.PAD_ATTRIB_NPTH):
                x=to(a.GetX())-ox;y=to(a.GetY())-oy;rad=max(to(drill.x),to(drill.y))/2
                for li in range(3):addshape(block[li],x,y,x,y,rad+.3+width/2+guard,True)
                addshape(via_block,x,y,x,y,max(rad+VR+.2,rad+DR+.254)+guard,True)
    for t in b.GetTracks():
        if t.GetNetCode()==net:
            if isinstance(t,p.PCB_VIA):
                x,y=to(t.GetX())-ox,to(t.GetY())-oy
                addshape(via_block,x,y,x,y,to(t.GetDrillValue())/2+DR+.254+guard,True)
            continue
        x0,y0=to(t.GetStart().x)-ox,to(t.GetStart().y)-oy;x1,y1=to(t.GetEnd().x)-ox,to(t.GetEnd().y)-oy
        if isinstance(t,p.PCB_VIA):
            rad=max(to(t.GetWidth(p.F_Cu))/2+.2,to(t.GetDrillValue())/2+.2)
            for li in range(3):addshape(block[li],x0,y0,x0,y0,rad+width/2+guard,True)
            addshape(via_block,x0,y0,x0,y0,max(to(t.GetWidth(p.F_Cu))/2+VR+.10,to(t.GetDrillValue())/2+DR+.254)+guard,True)
        else:
            if t.GetLayer() in layers:addshape(block[layers.index(t.GetLayer())],x0,y0,x1,y1,to(t.GetWidth())/2+.0889+width/2+guard,True)
            addshape(via_block,x0,y0,x1,y1,to(t.GetWidth())/2+max(.2+VR,.2+DR)+guard,True)
    # Via holes cross In1 too: ground pads on that layer are already captured.
    # RF tracks are locked and must retain their geometry; expanded copper
    # obstacles prevent foreign routing from touching them.
    from shapely.geometry import Polygon
    from shapely import contains_xy
    for z in b.Zones():
        if not z.GetIsRuleArea():continue
        ps=z.Outline()
        for k in range(ps.OutlineCount()):
            c=ps.COutline(k);g=Polygon([(to(c.CPoint(j).x)-ox,to(c.CPoint(j).y)-oy) for j in range(c.PointCount())]).buffer(width/2+guard)
            x0,y0,x1,y1=g.bounds;lo=max(0,math.floor((x0-phase)/step));hi=min(nx-1,math.ceil((x1-phase)/step));bot=max(0,math.floor((y0-phase)/step));top=min(ny-1,math.ceil((y1-phase)/step))
            if lo>hi or bot>top:continue
            xx,yy=np.meshgrid(np.arange(lo,hi+1)*step+phase,np.arange(bot,top+1)*step+phase);mask=contains_xy(g,xx,yy)
            if z.GetDoNotAllowTracks():
                for li,l in enumerate(layers):
                    if z.GetLayerSet().Contains(l):block[li,bot:top+1,lo:hi+1]|=mask
            if z.GetDoNotAllowVias():via_block[bot:top+1,lo:hi+1]|=mask
    return block,~via_block

def endpoints(b,item):
    uuid=item.get('uuid');obj=next((a for a in [*b.GetTracks(),*(q for f in b.GetFootprints() for q in f.Pads())] if a.m_Uuid.AsString()==uuid),None)
    if obj is None or not hasattr(obj,'GetNetCode') or isinstance(obj,p.ZONE):return None
    x=item['pos']['x']-ox;y=item['pos']['y']-oy
    if isinstance(obj,p.PCB_VIA):ls=list(range(3))
    elif isinstance(obj,p.PAD):ls=[i for i,l in enumerate(layers) if obj.IsOnLayer(l)]
    else:ls=[layers.index(obj.GetLayer())] if obj.GetLayer() in layers else []
    return obj.GetNetCode(),obj.GetNetname(),x,y,ls,obj
def anchors(b,endpoint,other,block):
    # DRC reports an item's anchor, not necessarily its unconnected end.
    # Search all accessible endpoints of its native connected component.
    b.BuildConnectivity();conn=b.GetConnectivity()
    lookup={q.m_Uuid.AsString():q for q in [*b.GetTracks(),*(a for f in b.GetFootprints() for a in f.Pads())]}
    pending=[endpoint[5]];seen=set();points=[]
    while pending:
        obj=pending.pop();uid=obj.m_Uuid.AsString()
        if uid in seen:continue
        seen.add(uid)
        if isinstance(obj,p.PCB_VIA):
            ls=range(3);positions=[obj.GetPosition()]
        elif isinstance(obj,p.PAD):
            ls=[i for i,l in enumerate(layers) if obj.IsOnLayer(l)];positions=[obj.GetPosition()]
        else:
            ls=[layers.index(obj.GetLayer())] if obj.GetLayer() in layers else []
            positions=[obj.GetStart(),obj.GetEnd()]
        for q in positions:
            x=to(q.x)-ox;y=to(q.y)-oy;ix=round((x-phase)/step);iy=round((y-phase)/step)
            if 0<=ix<nx and 0<=iy<ny:
                for l in ls:
                    if not block[l,iy,ix]:points.append((x,y,l))
        for q in conn.GetConnectedItems(obj):
            typed=lookup.get(q.m_Uuid.AsString())
            if typed is not None and typed.GetNetCode()==endpoint[0]:pending.append(typed)
    return sorted(set(points),key=lambda q:math.hypot(q[0]-other[2],q[1]-other[3]))[:12]
report=drc(candidate,'baseline');accepted=0;tried=set();iteration=0
while iteration<args.max_iterations:
    b=p.LoadBoard(str(candidate));pairs=[]
    rows=report['unconnected_items']
    if args.connect_uuids:rows=[{'items':[{'uuid':uid,'pos':{'x':to(obj.GetPosition().x),'y':to(obj.GetPosition().y)}} for uid in args.connect_uuids for obj in [next(q for q in b.GetTracks() if q.m_Uuid.AsString()==uid)]]}]
    for row in rows:
        if len(row['items'])!=2:continue
        a=endpoints(b,row['items'][0]);c=endpoints(b,row['items'][1])
        if not a or not c or not a[4] or not c[4] or a[0]!=c[0]:continue
        name=a[1]
        if args.nets and name not in args.nets:continue
        if name=='GND' or any(z in name for z in ('_ANT','J_LTE1','J_GNSS1','C50-Pad1')):continue
        if not args.allow_power and any(z in name for z in ('MODEM_3V8','SYS_3V3','/Power/','USB_D')):continue
        key=(name,round(a[2],3),round(a[3],3),round(c[2],3),round(c[3],3))
        if key in tried:continue
        pairs.append((math.hypot(a[2]-c[2],a[3]-c[3]),key,a,c))
    if not pairs:break
    _,key,a,c=min(pairs);tried.add(key);iteration+=1;block,via=raster(b,a[0],args.width);encode=lambda x,y,l:l*plane+round((y-phase)/step)*nx+round((x-phase)/step)
    if args.back_preferred:
        launch=np.zeros((ny,nx),dtype=bool)
        for obj in [*b.GetTracks(),*(q for f in b.GetFootprints() for q in f.Pads())]:
            if obj.GetNetCode()!=a[0]:continue
            for q in ([obj.GetPosition()] if isinstance(obj,(p.PAD,p.PCB_VIA)) else [obj.GetStart(),obj.GetEnd()]):
                x,y=to(q.x)-ox,to(q.y)-oy;addshape(launch,x,y,x,y,.35,True)
        block[0]|=~launch;block[1]=True
    sp=anchors(b,a,c,block);gp=anchors(b,c,a,block)
    sm={encode(*q):q for q in sp};gm={encode(*q):q for q in gp}
    starts=list(sm);goals=list(gm)
    if not starts or not goals:
        log.append({'iteration':iteration,'net':a[1],'accepted':False,'reason':'No accessible component anchor','start_count':len(starts),'goal_count':len(goals)})
        continue
    if args.top_only:
        block[1:]=True;via[:]=False
    path,stats=search(block,via,starts,goals)
    row={'iteration':iteration,'net':a[1],'before_opens':len(report['unconnected_items']),'path_states':len(path),'search':stats.strip()}
    if not path:row['accepted']=False;log.append(row);continue
    decode=lambda q:(q%nx*step+phase,((q%plane)//nx)*step+phase,q//plane)
    pts=[decode(q) for q in path];compact=[pts[0]]
    for i in range(1,len(pts)-1):
        aa,cc,dd=pts[i-1:i+2]
        if aa[2]!=cc[2] or cc[2]!=dd[2] or abs((cc[0]-aa[0])*(dd[1]-cc[1])-(cc[1]-aa[1])*(dd[0]-cc[0]))>1e-8:compact.append(cc)
    compact.append(pts[-1]);compact.insert(0,sm[path[0]]);compact.append(gm[path[-1]]);vias=set()
    def xy(x,y):return p.VECTOR2I(mm(x+ox),mm(y+oy))
    for aa,cc in zip(compact,compact[1:]):
        if aa[2]!=cc[2]:
            pos=(aa[0],aa[1])
            if pos in vias:continue
            vias.add(pos);v=p.PCB_VIA(b);v.SetPosition(xy(*pos));v.SetWidth(mm(2*VR));v.SetDrill(mm(2*DR));v.SetViaType(p.VIATYPE_THROUGH);v.SetLayerPair(p.F_Cu,p.B_Cu);v.SetNetCode(a[0])
            if args.allow_via_in_pad:v.SetFillingMode(p.FILLING_MODE_FILLED);v.SetCappingMode(p.CAPPING_MODE_CAPPED)
            b.Add(v)
        elif math.dist(aa[:2],cc[:2])>1e-7:
            t=p.PCB_TRACK(b);t.SetStart(xy(*aa[:2]));t.SetEnd(xy(*cc[:2]));t.SetWidth(mm(args.width));t.SetLayer(layers[aa[2]]);t.SetNetCode(a[0]);b.Add(t)
    backup=candidate.read_bytes();p.SaveBoard(str(candidate),b);new=drc(candidate,'candidate')
    if not (error_ids(new)-error_ids(report)) and len(new['unconnected_items'])>=len(report['unconnected_items']):
        subprocess.run(['/usr/bin/python3',str(R/'scripts/stitch_islands_resume.py'),str(candidate)],check=True)
        new=drc(candidate,'candidate')
    good=error_ids(new)<=error_ids(report) and len(new['unconnected_items'])<len(report['unconnected_items']) and len(new['schematic_parity'])<=len(report['schematic_parity'])
    row.update(accepted=good,after_opens=len(new['unconnected_items']),after_errors=errors(new));log.append(row)
    if not good:
        (D/('rejected-'+str(iteration)+'.json')).write_text(json.dumps(new,indent=2))
        shutil.copy2(candidate,D/('rejected-'+str(iteration)+'.kicad_pcb'))
    if good:
        report=new;accepted+=1;shutil.copy2(candidate,source);print('ACCEPT',a[1],len(report['unconnected_items']),'opens',flush=True)
    else:candidate.write_bytes(backup)
    (D/'repair-log.json').write_text(json.dumps(log,indent=2))
(D/'repair-log.json').write_text(json.dumps(log,indent=2));(D/'final-drc.json').write_text(json.dumps(report,indent=2));print('Maze accepted',accepted,'edges; final opens',len(report['unconnected_items']),'errors',errors(report),flush=True)
