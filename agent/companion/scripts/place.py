#!/usr/bin/python3
"""Deterministic bounded placer using actual footprint/pad bounding envelopes.

All source footprints and pad nets survive. No courtyard or clearance rules are
suppressed. Backside is reserved for sub-mm ICs and low-profile R/C parts;
mechanical spacer requirements are recorded separately, not inferred from DRC.
"""
import json, pathlib, math, collections, time
import pcbnew as p
ROOT=pathlib.Path(__file__).resolve().parents[1]
cfg=json.loads((ROOT/'scripts/layout.json').read_text())
b=p.LoadBoard(str(ROOT/'backups/source-kicad/watch.kicad_pcb'))
mm=p.FromMM; to=p.ToMM; O=cfg['origin']; W,H=cfg['size']; gap=cfg['courtyard_gap']
fs={f.GetReference():f for f in b.GetFootprints()}

def xy(x,y): return p.VECTOR2I(mm(x+O[0]),mm(y+O[1]))
def side(f): return 'bottom' if f.IsFlipped() else 'top'
def bounds(f):
    r=f.GetBoundingBox(False,False)
    return (to(r.GetLeft())-O[0],to(r.GetTop())-O[1],to(r.GetRight())-O[0],to(r.GetBottom())-O[1])
def intersect(a,c,g=gap): return a[0]<c[2]+g and a[2]>c[0]-g and a[1]<c[3]+g and a[3]>c[1]-g
def rf_clear(r):
    a=cfg['antenna']
    dx=max(a[0]-r[2],r[0]-a[2],0)
    dy=max(a[1]-r[3],r[1]-a[3],0)
    return math.hypot(dx,dy)>=cfg['external_metal_clearance']
def put(f,x,y,a,s):
    if f.IsFlipped() != (s=='bottom'): f.Flip(f.GetPosition(),False)
    f.SetOrientationDegrees(a); f.SetPosition(xy(x,y))

def pad_support(f):
    return all(to(a.GetBoundingBox().GetLeft()) >= O[0]+.2-1e-6 and to(a.GetBoundingBox().GetRight()) <= O[0]+W-.2+1e-6 and to(a.GetBoundingBox().GetTop()) >= O[1]+.2-1e-6 and to(a.GetBoundingBox().GetBottom()) <= O[1]+H-.2+1e-6 for a in f.Pads())

for f in fs.values():
    # Library silk has only 0.10mm pad clearance versus the required 0.15mm.
    # Retain the geometry in assembly drawings rather than printing it on lands.
    for graphic in f.GraphicalItems():
        if graphic.GetLayer()==p.F_SilkS: graphic.SetLayer(p.F_Fab)
        elif graphic.GetLayer()==p.B_SilkS: graphic.SetLayer(p.B_Fab)
    # References on fabrication layer retain readable assembly identity without
    # printing artwork onto fine-pitch lands or other component bodies.
    f.Reference().SetLayer(p.F_Fab); f.Reference().SetVisible(True)
    f.Value().SetVisible(False)
    f.SetLocked(False)

occupied={'top':[],'bottom':[]}; placed=set(); log=[]
for ref,params in cfg['anchors'].items():
    f=fs[ref]; put(f,*params); r=bounds(f); s=side(f)
    conflicts=[n for n,v in occupied[s] if intersect(r,v)]
    if conflicts: raise RuntimeError(f'Anchor {ref} collides with {conflicts}: {r}')
    if not pad_support(f): raise RuntimeError(f'Unsupported copper pad on {ref}')
    if ref!='U7' and not rf_clear(r): raise RuntimeError(f'External metal in antenna clearance: {ref} {r}')
    occupied[s].append((ref,r)); placed.add(ref); log.append([ref,*params,'anchor'])
for ref in placed:
    r=bounds(fs[ref])
    for cr in ('USBC1','J_SD1'):
        if ref==cr: continue
        for a in fs[cr].Pads():
            if not a.GetDrillSize().x: continue
            ar=a.GetBoundingBox()
            c=(to(ar.GetLeft())-O[0],to(ar.GetTop())-O[1],to(ar.GetRight())-O[0],to(ar.GetBottom())-O[1])
            if intersect(r,c,.25): raise RuntimeError(f'Anchor {ref} overlaps drilled keepout of {cr}')

netpads=collections.defaultdict(list)
for ref in placed:
    for a in fs[ref].Pads():
        netpads[a.GetNetname()].append((ref,to(a.GetX())-O[0],to(a.GetY())-O[1]))

# Constraint targets: nearest pin of the principal functional IC; shared supply
# rails are deliberately weaker than local feedback, switching and signal nets.
primary={'U1','U3','U4','U5','U7','U8','U9','U10','U11','Card1','USBC1','J_SD1'}
def target(f):
    pts=[]
    for a in f.Pads():
        net=a.GetNetname()
        if not net or net=='GND' or net.startswith('unconnected-'): continue
        candidates=netpads[net]
        local=[z for z in candidates if z[0] in primary]
        if local: candidates=local
        weight=1 if net in ('/Power/VBAT','/Power/VSYS','/Power/USB_5V','/ESP32/SYS_3V3','/Modem/MODEM_3V8') else 8
        for _,x,y in candidates: pts.append((x,y,weight/max(1,len(candidates))))
    if not pts: return (40,15)
    return (sum(x*w for x,y,w in pts)/sum(w for x,y,w in pts),sum(y*w for x,y,w in pts)/sum(w for x,y,w in pts))

remaining=[f for ref,f in fs.items() if ref not in placed]
# Principal ICs first, then local decoupling/feedback before large generic
# components; access pads last so they cannot displace critical decoupling.
critical={'C4','C8','C12','C16','C17','C18','C19','C24','C27','C28','C29','C30','C41','C42','C43','C44','C58'}
remaining.sort(key=lambda f: (0 if f.GetReference().startswith(('U','D','Q')) else 1 if f.GetReference() in critical else 4 if f.GetReference().startswith('TP') else 2,-(bounds(f)[2]-bounds(f)[0])*(bounds(f)[3]-bounds(f)[1]),f.GetReference()))
# Dense sites are on a .25 mm placement grid, finer than physical clearances.
grid=[(x/4,y/4) for x in range(2,int(W*4)-1) for y in range(2,int(H*4)-1)]
for f in remaining:
    ref=f.GetReference(); tx,ty=target(f)
    sites=sorted(grid,key=lambda v:(v[0]-tx)**2+(v[1]-ty)**2)
    permitted=['top']
    if ref.startswith(('R','C','TP')): permitted.append('bottom')
    best=None
    for s in permitted:
        for angle in (0,90,180,270):
            put(f,0,0,angle,s); local=bounds(f)
            for x,y in sites:
                r=(local[0]+x,local[1]+y,local[2]+x,local[3]+y)
                if r[0]<.3 or r[1]<.3 or r[2]>W-.3 or r[3]>H-.3: continue
                if not rf_clear(r): continue
                if any(intersect(r,c) for _,c in occupied[s]): continue
                # Avoid plated/NPTH connector pins on the reverse side too.
                bad=False
                for cr in ('USBC1','J_SD1'):
                    for a in fs[cr].Pads():
                        if a.GetDrillSize().x:
                            ar=a.GetBoundingBox(); c=(to(ar.GetLeft())-O[0],to(ar.GetTop())-O[1],to(ar.GetRight())-O[0],to(ar.GetBottom())-O[1])
                            if intersect(r,c,.3): bad=True; break
                    if bad: break
                if bad: continue
                score=(x-tx)**2+(y-ty)**2+(25 if s=='bottom' else 0)
                if best is None or score<best[0]: best=(score,x,y,angle,s,r)
                break
        if best and best[0]<25: break
    if best is None: raise RuntimeError(f'No safe site found for {ref}; nothing silently removed')
    _,x,y,a,s,r=best; put(f,x,y,a,s); occupied[s].append((ref,r)); placed.add(ref)
    log.append([ref,x,y,a,s,'packed'])
    for pad in f.Pads(): netpads[pad.GetNetname()].append((ref,to(pad.GetX())-O[0],to(pad.GetY())-O[1]))
    print(ref,round(x,2),round(y,2),a,s,flush=True)

# Replace obsolete outline in the new project only. R0.5 is a modest production
# corner and keeps the narrow board-end space available for connector shells.
for d in list(b.GetDrawings()):
    if d.GetLayer()==p.Edge_Cuts: b.Remove(d)
r=cfg['corner_radius']
for a,c in [((r,0),(W-r,0)),((W,r),(W,H-r)),((W-r,H),(r,H)),((0,H-r),(0,r))]:
    sh=p.PCB_SHAPE(); sh.SetShape(p.SHAPE_T_SEGMENT); sh.SetStart(xy(*a)); sh.SetEnd(xy(*c)); sh.SetLayer(p.Edge_Cuts); sh.SetWidth(mm(.05)); b.Add(sh)
for a,mid,c in [((W-r,0),(W-r+r/math.sqrt(2),r-r/math.sqrt(2)),(W,r)),((W,H-r),(W-r+r/math.sqrt(2),H-r+r/math.sqrt(2)),(W-r,H)),((r,H),(r-r/math.sqrt(2),H-r+r/math.sqrt(2)),(0,H-r)),((0,r),(r-r/math.sqrt(2),r-r/math.sqrt(2)),(r,0))]:
    sh=p.PCB_SHAPE(); sh.SetShape(p.SHAPE_T_ARC); sh.SetArcGeometry(xy(*a),xy(*mid),xy(*c)); sh.SetLayer(p.Edge_Cuts); sh.SetWidth(mm(.05)); b.Add(sh)
b.SetCopperLayerCount(4)
b.GetDesignSettings().SetAuxOrigin(xy(0,0))
p.SaveBoard(str(ROOT.parents[1] / 'kicad/companion/companion.kicad_pcb'),b)
(ROOT/'reports/placement.json').write_text(json.dumps(dict(config=cfg,placements=log,envelopes=occupied,top_count=len(occupied['top']),bottom_count=len(occupied['bottom'])),indent=2))
print('Placed',len(placed),'top',len(occupied['top']),'bottom',len(occupied['bottom']))
