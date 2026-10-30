#!/usr/bin/python3
"""Union deficient copper lands with exact selected-manufacturer recommendations.
NOT RUN by the author. Parent integration owner applies --apply, then runs DRC.
Existing pad UUIDs/nets/FPIDs and all old copper are preserved. No routed item
is moved/deleted. Separate manufacturer F.Paste graphics prevent copper growth
from inadvertently increasing paste volume. Original thermal/drain lands stay.
"""
import argparse, hashlib, json, math, pathlib, shutil
import pcbnew as p
from shapely.geometry import box, Polygon
from shapely.affinity import rotate
from shapely.ops import unary_union
ROOT=pathlib.Path(__file__).resolve().parents[3]
TO=p.ToMM

def world(fp,x,y):
 a=math.radians(fp.GetOrientationDegrees())
 return (TO(fp.GetX())+x*math.cos(a)+y*math.sin(a),TO(fp.GetY())-x*math.sin(a)+y*math.cos(a))
def local(fp,x,y):
 a=math.radians(fp.GetOrientationDegrees());x-=TO(fp.GetX());y-=TO(fp.GetY())
 return x*math.cos(a)-y*math.sin(a),x*math.sin(a)+y*math.cos(a)
def vec(x,y):return p.VECTOR2I(round(x*1_000_000),round(y*1_000_000))
def copper_box(pad):
 r=pad.GetBoundingBox();return box(TO(r.GetLeft()),TO(r.GetTop()),TO(r.GetRight()),TO(r.GetBottom()))
def terminal(fp,x,y,w,h):
 xx,yy=world(fp,x,y)
 return rotate(box(xx-w/2,yy-h/2,xx+w/2,yy+h/2),-fp.GetOrientationDegrees(),origin=(xx,yy))
def paste(fp,ref,n,x,y,w,h,radius):
 # Filled polygon, not a copperless SMD pad. Circle quadrants discretized
 # at11.25degrees have <0.00025mm radial error forR=.05, below1umKiCad grid.
 # Identify our aperture geometrically: KiCad exposes item UUIDs read-only.
 expected=terminal(fp,x,y,w,h).bounds
 legacy=terminal(fp,x,-1.6375,w,h).bounds if ref=='U5' and n in ('20','23') else expected
 for g in list(fp.GraphicalItems()):
  if g.GetLayer()!=p.F_Paste:continue
  bb=g.GetBoundingBox()
  bounds=[TO(bb.GetLeft()),TO(bb.GetTop()),TO(bb.GetRight()),TO(bb.GetBottom())]
  if any(all(abs(a-b)<.0005 for a,b in zip(bounds,target)) for target in (expected,legacy)):fp.Remove(g)
 points=[]
 for cx,cy,start in [(x+w/2-radius,y+h/2-radius,0),(x-w/2+radius,y+h/2-radius,90),(x-w/2+radius,y-h/2+radius,180),(x+w/2-radius,y-h/2+radius,270)]:
  for j in range(9 if radius else 1):
   a=math.radians(start+j*11.25);points.append(world(fp,cx+radius*math.cos(a),cy+radius*math.sin(a)))
 poly=p.SHAPE_POLY_SET();poly.NewOutline()
 for xx,yy in points:poly.Append(vec(xx,yy))
 shape=p.PCB_SHAPE(fp);shape.SetShape(p.SHAPE_T_POLY);shape.SetPolyShape(poly);shape.SetLayer(p.F_Paste);shape.SetFilled(True);shape.SetWidth(0);fp.Add(shape)
def overlap(shape,copper):return shape.intersection(copper).area/shape.area

def main():
 ap=argparse.ArgumentParser(description=__doc__)
 ap.add_argument('--board',type=pathlib.Path,default=ROOT/'kicad/companion/companion.kicad_pcb')
 ap.add_argument('--report',type=pathlib.Path,default=ROOT/'agent/companion/reports/jlcpcb-dfm/land-repair.json')
 ap.add_argument('--apply',action='store_true',help='Save the canonical board only after copper-short guard passes')
 args=ap.parse_args();board=p.LoadBoard(str(args.board));original=hashlib.sha256(args.board.read_bytes()).hexdigest();fps={f.GetReference():f for f in board.GetFootprints()}
 if args.apply:
  checkpoint=ROOT/'agent/companion/checkpoints/pre-dfm-land-repair.kicad_pcb'
  if not checkpoint.exists():shutil.copy2(args.board,checkpoint)
 changes=[];changed_pads=[];evidence=[]
 # Each target is a manufacturer copper rectangle in unrotated footprint
 # coordinates plus the exact manufacturer stencil aperture. The union with
 # the existing pad is a rectangle here; no existing copper is lost.
 targets=[]
 for ref in ('QH2','QL2'):
  for n,x in enumerate((-.975,-.325,.325,.975),1):targets.append((ref,str(n),x,1.435,.5,.63,.34,.45,0))
 for n in range(1,25):
  if n<=6:x,y,w,h=-1.9125,-1.25+(n-1)*.5,.58,.24
  elif n<=12:x,y,w,h=-1.25+(n-7)*.5,1.9125,.24,.58
  elif n<=18:x,y,w,h=1.9125,1.25-(n-13)*.5,.58,.24
  else:x,y,w,h=1.25-(n-19)*.5,-1.9125,.24,.58
  targets.append(('U1',str(n),x,y,w,h,w,h,.05))
 # TI RPM0026A pp39-40, all22 rectangular pads. Preserve four custom L
 # lands; their envelopes are not a defensible exact terminal-area model.
 # .675 top lands at1.6125; 1.6375 belongs to the BOTTOM .625 row.
 for n in (2,3,4,5,6,14,15,16,17,18):
  sign=-1 if n<7 else 1;y={2:-1,3:-.5,4:0,5:.5,6:1,14:1,15:.5,16:0,17:-.5,18:-1}[n]
  w=.6 if abs(y)==1 else .725;x=sign*(1.9 if w==.6 else 1.8375)
  targets.append(('U5',str(n),x,y,w,.25,w,.25,.05))
 for n,x in [(8,-1.125),(9,-.625),(10,-.125),(11,.625),(12,1.125)]:
  targets.append(('U5',str(n),x,1.6375,.25,.625,.25,.625,.05))
 for n,x in [(20,1.05),(21,.25),(22,-.25),(23,-1.05)]:
  y=-1.6125 if n in (20,23) else -1.65;h=.675 if n in (20,23) else .6
  targets.append(('U5',str(n),x,y,.25,h,.25,h,.05))
 for n,x in [(24,-.625),(25,0),(26,.625)]:
  targets.append(('U5',str(n),x,0,.375,1.425,.375,1.425,.05))
 before_union={ref:unary_union([copper_box(pd) for pd in fps[ref].Pads()]) for ref in ('QH2','QL2','U1','U5')}
 for ref,n,x,y,w,h,pw,ph,radius in targets:
  fp=fps[ref];pads=[pd for pd in fp.Pads() if pd.GetNumber()==n]
  if len(pads)!=1:raise RuntimeError(f'Expected one isolated target pad {ref}.{n}, got {len(pads)}')
  pd=pads[0]
  if pd.GetShape()!=p.PAD_SHAPE_RECT:raise RuntimeError(f'Refusing nonrectangular target {ref}.{n}')
  prior=copper_box(pd);corners=[local(fp,xx,yy) for xx,yy in [(prior.bounds[0],prior.bounds[1]),(prior.bounds[2],prior.bounds[3])]]
  x0=min(q[0] for q in corners);y0=min(q[1] for q in corners);x1=max(q[0] for q in corners);y1=max(q[1] for q in corners)
  new=(min(x0,x-w/2),min(y0,y-h/2),max(x1,x+w/2),max(y1,y+h/2))
  if ref in ('QH2','QL2'):
   # Original outer edge is exactly 1.950mm; recover any earlier 1nm truncation.
   new=(min(new[0],x-.25),min(new[1],1.12),max(new[2],x+.25),max(new[3],1.95))
  cx,cy=(new[0]+new[2])/2,(new[1]+new[3])/2
  olduuid=str(pd.m_Uuid.AsString());oldnet=pd.GetNetname();oldid=(str(fp.GetFPID().GetLibNickname()),str(fp.GetFPID().GetLibItemName()));oldpose=[TO(pd.GetX()),TO(pd.GetY())];oldsize=[TO(pd.GetSize().x),TO(pd.GetSize().y)]
  pd.SetPosition(vec(*world(fp,cx,cy)));pd.SetOrientationDegrees(fp.GetOrientationDegrees());pd.SetSize(vec(new[2]-new[0],new[3]-new[1]))
  # Remove the automatic enlarged-copper aperture; add only recommendedpaste.
  layers=pd.GetLayerSet();layers.RemoveLayer(p.F_Paste);pd.SetLayerSet(layers)
  paste(fp,ref,n,x,y,pw,ph,radius)
  now=copper_box(pd)
  if not now.buffer(.000002).covers(prior):raise RuntimeError(f'Oldcopper loss {ref}.{n}')
  if str(pd.m_Uuid.AsString())!=olduuid or pd.GetNetname()!=oldnet or (str(fp.GetFPID().GetLibNickname()),str(fp.GetFPID().GetLibItemName()))!=oldid:raise RuntimeError('Identity changed')
  changes.append({'ref':ref,'pad':n,'uuid':olduuid,'net':oldnet,'before_center_native_mm':oldpose,'before_size_mm':oldsize,'after_center_native_mm':[TO(pd.GetX()),TO(pd.GetY())],'after_size_mm':[TO(pd.GetSize().x),TO(pd.GetSize().y)],'copper_target_local_mm':[x,y,w,h],'paste_target_local_mm':[x,y,pw,ph],'paste_corner_radius_mm':radius,'old_copper_preserved':True})
  changed_pads.append(pd)
 after_union={ref:unary_union([copper_box(pd) for pd in fps[ref].Pads()]) for ref in before_union}
 # Independent dimensional extremes are sensitivity bounds, not a claim that
 # every terminal simultaneously reaches all independent extrema.
 for ref in ('QH2','QL2'):
  fp=fps[ref]
  for n in range(1,9):
   x=(-.975+(n-1)*.65) if n<=4 else (.975-(n-5)*.65);sign=1 if n<=4 else -1
   for label,body,width,length in [('nominal',3.3,.34,.45),('largest_terminal_smallest_body',3.2,.4,.55),('largest_terminal_largest_body',3.4,.4,.55)]:
    lead=terminal(fp,x,sign*(body/2-length/2),width,length)
    evidence.append({'ref':ref,'pad':str(n),'case':label,'before_union_support_fraction':overlap(lead,before_union[ref]),'after_union_support_fraction':overlap(lead,after_union[ref]),'manufacturer_land_note':'TI source .5x.63 at1.435; drain union unchanged'})
 for n in range(1,25):
  row='left' if n<=6 else 'bottom' if n<=12 else 'right' if n<=18 else 'top';along=-1.25+(n-1)*.5 if n<=6 else -1.25+(n-7)*.5 if n<=12 else 1.25-(n-13)*.5 if n<=18 else 1.25-(n-19)*.5
  for label,body,width,length in [('nominal',4,.24,.38),('largest_terminal_smallest_body',3.9,.3,.48),('largest_terminal_largest_body',4.1,.3,.48)]:
   off=body/2-length/2
   x,y,w,h=(-off,along,length,width) if row=='left' else (along,off,width,length) if row=='bottom' else (off,along,length,width) if row=='right' else (along,-off,width,length)
   lead=terminal(fps['U1'],x,y,w,h);evidence.append({'ref':'U1','pad':str(n),'case':label,'before_union_support_fraction':overlap(lead,before_union['U1']),'after_union_support_fraction':overlap(lead,after_union['U1']),'manufacturer_land_note':'TI recommended inneredge1.6225, not arbitrary100%containment ofallmaxterminals'})
 allpads=[pd for f in board.GetFootprints() for pd in f.Pads() if pd.IsOnLayer(p.F_Cu)];clearances=[];shorts=[]
 for a in changed_pads:
  aa=copper_box(a);neighbors=[]
  for b in allpads:
   if a.m_Uuid==b.m_Uuid or a.GetNetCode()==b.GetNetCode():continue
   bb=copper_box(b);d=aa.distance(bb)
   if d<.4:neighbors.append({'ref':b.GetParentFootprint().GetReference(),'pad':b.GetNumber(),'net':b.GetNetname(),'copper_bbox_gap_mm':d})
   if aa.intersection(bb).area>1e-9:shorts.append({'a':[a.GetParentFootprint().GetReference(),a.GetNumber()],'b':[b.GetParentFootprint().GetReference(),b.GetNumber()]})
  clearances.append({'ref':a.GetParentFootprint().GetReference(),'pad':a.GetNumber(),'crossnet_nearby_pad_copper_bboxes':sorted(neighbors,key=lambda q:q['copper_bbox_gap_mm'])})
 report={'applied':bool(args.apply and not shorts),'input_board':str(args.board),'input_sha256':original,'manufacturer_sources':{'QH2_QL2':'https://www.ti.com/lit/ds/symlink/csd16340q3.pdf pp9-10','U1':'https://www.ti.com/lit/ds/symlink/bq25606.pdf pp43-45','U5':'https://www.ti.com/lit/ds/symlink/tps552882.pdf pp38-40'},'changes':changes,'lead_support_sensitivity':evidence,'crossnet_pad_clearance_screen':clearances,'crossnet_pad_bbox_overlaps':shorts,'U5_disposition':'Independent TI drawing correction: top20/23 centerY-1.6125, NOT-1.6375 (bottom-row callout). .675 length gives y[-1.950,-1.275]. Copper union preserves old outward edge; dedicated paste matches TI only. Generic JLC overlap ratio is not a100%maximum-terminal criterion.','scope_limits':['No track/via changes; parent must DRC andrepairnewcopper-to-track/via conflicts.','Clearance screen uses conservative pad boundingboxes, not complete Gerber polygon DRC.','Thermalpad paste patterns preserved here; repair_dfm_stencil.py separately restores manufacturer EP/drain windows.','Sourcepad copperpitchgap becomes.150mm; pastepitchgap.310mm. Boardmaskexpansion mayremoveverythinmaskbridges andneedsassemblerreview.']}
 args.report.parent.mkdir(parents=True,exist_ok=True);args.report.write_text(json.dumps(report,indent=2))
 if shorts:raise SystemExit('Refusing save: cross-net pad bbox overlap; see '+str(args.report))
 if args.apply:p.SaveBoard(str(args.board),board)
 print(json.dumps({'applied':args.apply,'changed_pads':len(changes),'report':str(args.report)},indent=2))
if __name__=='__main__':main()
