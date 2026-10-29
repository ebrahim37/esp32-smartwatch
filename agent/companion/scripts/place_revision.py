#!/usr/bin/python3
"""Repack the authoritative companion board, preserving footprints, nets and models.
Existing copper is checkpointed: every footprint is moved or flipped, therefore
all previous routes and fills are electrically obsolete and intentionally rerouted.
No bottom components or global antenna-to-battery gap is introduced.
"""
import pathlib,json,math,collections,sys
import pcbnew as p
ROOT=pathlib.Path(__file__).resolve().parents[1]
cfg=json.loads((ROOT/'scripts/layout-revision.json').read_text());O=cfg['origin'];W,H=cfg['size'];gap=cfg['courtyard_gap'];mm=p.FromMM;to=p.ToMM
b=p.LoadBoard(str(ROOT.parents[1] / 'kicad/companion/companion.kicad_pcb'));fs={f.GetReference():f for f in b.GetFootprints()}
def xy(x,y):return p.VECTOR2I(mm(x+O[0]),mm(y+O[1]))
def put(f,x,y,a):
    if f.IsFlipped():f.Flip(f.GetPosition(),False)
    f.SetOrientationDegrees(a);f.SetPosition(xy(x,y));f.SetLocked(False)
def bounds(f):
    r=f.GetBoundingBox(False,False)
    return tuple(to(v)-O[i%2] for i,v in enumerate((r.GetLeft(),r.GetTop(),r.GetRight(),r.GetBottom())))
def intersect(a,c,g=gap):return a[0]<c[2]+g and a[2]>c[0]-g and a[1]<c[3]+g and a[3]>c[1]-g
occupied=[];placed=set();log=[]
for f in fs.values():
    if f.IsFlipped():f.Flip(f.GetPosition(),False)
    f.Reference().SetLayer(p.F_Fab);f.Reference().SetVisible(False);f.Value().SetVisible(False);f.SetLocked(False)
for ref,(x,y,a,side) in cfg['anchors'].items():
    f=fs[ref];put(f,x,y,a);r=bounds(f)
    collisions=[n for n,v in occupied if intersect(r,v)]
    if collisions:raise RuntimeError(f'Anchor {ref} {r} overlaps {collisions}')
    # Actual pad support, not module courtyard, decides antenna edge placement.
    for pad in f.Pads():
        q=pad.GetBoundingBox();rpad=[to(v)-O[i%2] for i,v in enumerate((q.GetLeft(),q.GetTop(),q.GetRight(),q.GetBottom()))]
        if rpad[0]<.2 or rpad[1]<.2 or rpad[2]>W-.2 or rpad[3]>H-.2:raise RuntimeError(f'Unsupported pad {ref}.{pad.GetNumber()} {rpad}')
    occupied.append((ref,r));placed.add(ref);f.SetLocked(True);log.append([ref,x,y,a,'top','anchor'])
if '--anchors-only' in sys.argv:print(json.dumps(occupied,indent=2));sys.exit()
netpads=collections.defaultdict(list)
for ref in placed:
    for a in fs[ref].Pads():netpads[a.GetNetname()].append((ref,to(a.GetX())-O[0],to(a.GetY())-O[1]))
primary={r for r in placed if r.startswith('U') or r in ('USBC1','J_SD1','Card1','QH2','QL2','L1','L3','L4')}
rails={'GND','/Power/VBAT','/Power/VSYS','/Power/USB_5V','/ESP32/SYS_3V3','/Modem/MODEM_3V8'}
def target(f):
    pts=[]
    for a in f.Pads():
        name=a.GetNetname()
        if not name or name=='GND' or name.startswith('unconnected-'):continue
        candidates=netpads[name];local=[z for z in candidates if z[0] in primary]
        if local:candidates=local
        weight=1 if name in rails else 10
        for _,x,y in candidates:pts.append((x,y,weight/max(1,len(candidates))))
    if not pts:return (40,19)
    return tuple(sum(z[i]*z[2] for z in pts)/sum(z[2] for z in pts) for i in (0,1))
remaining=[f for r,f in fs.items() if r not in placed]
critical={'C4','C8','C12','C16','C17','C18','C19','C24','C27','C28','C29','C30','C41','C42','C43','C44','C58'}
remaining.sort(key=lambda f:(0 if f.GetReference().startswith(('U','D','Q')) else 1 if f.GetReference() in critical else 4 if f.GetReference().startswith('TP') else 2,-(bounds(f)[2]-bounds(f)[0])*(bounds(f)[3]-bounds(f)[1]),f.GetReference()))
grid=[(x/4,y/4) for x in range(2,int(W*4)-1) for y in range(2,int(H*4)-1)]
for f in remaining:
    ref=f.GetReference();tx,ty=target(f);sites=sorted(grid,key=lambda q:(q[0]-tx)**2+(q[1]-ty)**2);best=None
    for angle in (0,90,180,270):
        put(f,0,0,angle);local=bounds(f)
        for x,y in sites:
            r=(local[0]+x,local[1]+y,local[2]+x,local[3]+y)
            if r[0]<.35 or r[1]<.35 or r[2]>W-.35 or r[3]>H-.35:continue
            if any(intersect(r,c) for _,c in occupied):continue
            score=(x-tx)**2+(y-ty)**2
            if best is None or score<best[0]:best=(score,x,y,angle,r)
            break
    if best is None:raise RuntimeError(f'No legal all-top site: {ref}')
    _,x,y,a,r=best;put(f,x,y,a);occupied.append((ref,r));placed.add(ref);log.append([ref,x,y,a,'top','packed'])
    for pad in f.Pads():netpads[pad.GetNetname()].append((ref,to(pad.GetX())-O[0],to(pad.GetY())-O[1]))
    print(ref,x,y,a,flush=True)
# Snapshot containers before removal: pcbnew 10 invalidates SWIG iteration types.
tracks=list(b.GetTracks());zones=list(b.Zones());drawings=list(b.GetDrawings())
for t in tracks:b.Remove(t)
for z in zones:b.Remove(z)
for d in drawings:
    if d.GetLayer() in (p.Edge_Cuts,p.User_1):b.Remove(d)
r=cfg['corner_radius']
for a,c in [((r,0),(W-r,0)),((W,r),(W,H-r)),((W-r,H),(r,H)),((0,H-r),(0,r))]:
    sh=p.PCB_SHAPE();sh.SetShape(p.SHAPE_T_SEGMENT);sh.SetStart(xy(*a));sh.SetEnd(xy(*c));sh.SetLayer(p.Edge_Cuts);sh.SetWidth(mm(.05));b.Add(sh)
for a,m,c in [((W-r,0),(W-r+r/math.sqrt(2),r-r/math.sqrt(2)),(W,r)),((W,H-r),(W-r+r/math.sqrt(2),H-r+r/math.sqrt(2)),(W-r,H)),((r,H),(r-r/math.sqrt(2),H-r+r/math.sqrt(2)),(0,H-r)),((0,r),(r-r/math.sqrt(2),r-r/math.sqrt(2)),(r,0))]:
    sh=p.PCB_SHAPE();sh.SetShape(p.SHAPE_T_ARC);sh.SetArcGeometry(xy(*a),xy(*m),xy(*c));sh.SetLayer(p.Edge_Cuts);sh.SetWidth(mm(.05));b.Add(sh)
b.SetCopperLayerCount(4);b.GetDesignSettings().SetBoardThickness(mm(.8))
p.SaveBoard(str(ROOT.parents[1] / 'kicad/companion/companion.kicad_pcb'),b)
(ROOT/'reports/placement-revision.json').write_text(json.dumps(dict(config=cfg,placements=log,envelopes=occupied,top_count=len(placed),bottom_count=0,obsolete_routing_archived_items=len(tracks),obsolete_zones=len(zones)),indent=2))
print('All top:',len(placed),'board',W,H,'old copper archived',len(tracks))
