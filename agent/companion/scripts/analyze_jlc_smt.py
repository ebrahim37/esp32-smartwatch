#!/usr/bin/python3
"""Reproduce supplied SMT measurements without changing a PCB or weakening rules.
Needs distro pcbnew, numpy, shapely. Body dimensions are NOT courtyards.
Defaults to immutable uploaded rev-a; --board can select the integrated board.
"""
import argparse, collections, hashlib, json, math, pathlib, re, sys
import numpy as np
import pcbnew as p
from shapely.geometry import box, Polygon, Point
from shapely.affinity import rotate, translate
from shapely.ops import unary_union
ROOT=pathlib.Path(__file__).resolve().parents[3]
BASE=ROOT/'agent/companion'
SPACING='https://jlcpcb.com/help/article/minimum-spacing-for-smd-components'
QUECTEL='https://quectel.com/content/uploads/2024/02/Quectel_EG912U-GL_Hardware_Design_V1.1.pdf'
HIROSE='https://media.digikey.com/pdf/data%20sheets/hirose%20pdfs/ufl%20series.pdf'
SOURCES={
 'U10':{'url':QUECTEL,'pages':'13,33,42,76,78','nominal_global_mm':[29,25,2.4],'size_plusminus_mm':[.2,.2,.2],'spacing_mm':3,'spacing_status':'manufacturer recommendation for soldering/maintenance, not a proven capability hard limit'},
 'UFL':{'url':HIROSE,'official_url_attempt':'https://www.hirose.com/en/product/document?clcode=&documentid=ed_U.FL_CAT&documenttype=Catalog&lang=en&productname=&series=U.FL','official_fetch_status':'403; manufacturer catalogue obtained from distributor; engineering drawing/tolerance still missing','page':3,'housing_mm':[2.6,2.6,1.25],'metal_mm':[3.1,3.0],'metal_3_1_is_reference_only':True,'native_housing_center_mm':[.62,0],'ground_pad_centers_mm':[[.62,-1.5],[.62,1.5]],'signal_pad_center_mm':[-.62,0],'maximum_body_tolerance_mm':None},
 'C1525':{'url':'https://product.samsungsem.com/mlcc/CL05B104KO5NNN.do','nominal_mm':[1,.5,.5],'size_plusminus_mm':[.05,.05,.05]},
 'C19702':{'url':'https://product.samsungsem.com/mlcc/CL10A106KP8NNN.do','nominal_mm':[1.6,.8,.8],'size_plusminus_mm':[.1,.1,.1]},
 'C12891':{'url':'https://product.samsungsem.com/mlcc/CL31A226KAHNNN.do','nominal_mm':[3.2,1.6,1.6],'size_plusminus_mm':[.2,.2,.2]},
 'FH':{'url':'https://datasheet.lcsc.com/datasheet/pdf/09fe2daf22580a99fa7fea93ccb20fb7.pdf?productCode=C40059','page':5,'0402_family_possible_plusminus_mm':[.05,.15,.2],'selected_size_code_not_supplied':True},
 'UNI':{'url':'https://datasheet.lcsc.com/datasheet/pdf/0a975aaa49b7c97f38a963127be4a823.pdf?productCode=C25744','page':4,'nominal_mm':[1,.5],'size_plusminus_mm':[.1,.05]},
 'C148107':{'url':'https://www.lcsc.com/product-detail/C148107.html','manufacturer_url_attempt':'https://datasheets.kyocera-avx.com/TPS.pdf','manufacturer_fetch_status':'406/HTTP failure; exact maximum tolerance not qualified','nominal_case_mm':[3.5,2.8],'selected':'TPSB107M010R0400, not a Kemet capacitor despite KiCad footprint name'},
 'U2':{'url':'https://www.molex.com/en-us/products/part-detail/2053380002','local_pdf':'datasheets/Molex_2053380002_drawing.pdf','page':1,'housing_mm':[7.95,7.6],'flanges_each_side_mm':.4,'full_width_mm':8.75,'general_size_plusminus_mm':.2},
 'J_SD1':{'url':'https://www.lcsc.com/product-detail/C266620.html','local_pdf':'datasheets/XUNPU_TF-115_microSD_C266620.pdf','main_mm':[14.75,14.5],'general_two_decimal_plusminus_mm':.25,'note':'full mesh includes projecting contacts, shell and locating pegs; used as conservative occupied envelope, not exact solid'},
 'CARD1':{'url':'https://www.lcsc.com/product-detail/C53200251.html','local_pdf':'datasheets/XYECONN_Nano_SIM_C53200251.pdf','main_mm':[14.1,12.0],'general_two_decimal_plusminus_mm':.2,'note':'full mesh conservative 14.100x12.400 includes side tabs; hinge-open and card-sweep not certified'},
 'D1':{'url':'https://www.ti.com/lit/ds/symlink/tpd4e05u06.pdf','page':28,'package':'DQA0010A','nominal_mm':[2.5,1.0],'size_plusminus_mm':[.1,.1]},
 'U6':{'url':'https://www.ti.com/lit/ds/symlink/tpd1e10b06.pdf','page':19,'package':'DPY0002A','nominal_mm':[1,.6],'size_plusminus_mm':[.1,.1]},
 'U4':{'url':'https://www.ti.com/lit/ds/symlink/tps63802.pdf','page':35,'nominal_mm':[3,2],'size_plusminus_mm':[.1,.1]},
 'U1':{'url':'https://www.ti.com/lit/ds/symlink/bq25606.pdf','pages':[43,44],'nominal_mm':[4,4],'size_plusminus_mm':[.1,.1],'recommended_land_mm':[.58,.24],'recommended_row_center_offset_mm':1.9125},
 'U5':{'url':'https://www.ti.com/lit/ds/symlink/tps552882.pdf','pages':[38,39],'nominal_mm':[4,3.5],'size_plusminus_mm':[.1,.1]},
 'U11':{'url':'https://www.ti.com/lit/ds/symlink/txu0204.pdf','page':35,'nominal_mm':[2,1.7],'size_plusminus_mm':[.1,.1]},
 'QH2':{'url':'https://www.ti.com/lit/ds/symlink/csd16340q3.pdf','pages':[9,10],'nominal_mm':[3.3,3.3],'size_plusminus_mm':[.1,.1],'nominal_terminal_width_length_mm':[.34,.45],'minimum_terminal_width_length_mm':[.28,.35],'maximum_terminal_width_length_mm':[.4,.55],'recommended_source_land_width_length_mm':[.5,.63],'recommended_source_row_center_offset_mm':1.435,'existing_mesh_wrong_body_mm':[3.0,3.02]},
 'USBC1':{'url':'https://www.lcsc.com/product-detail/C165948.html','local_pdf':'datasheets/HRO_TYPE-C-31-M-12_C165948.pdf','note':'supplier drawing and asymmetric locating holes calibrate shell/board-origin; mesh nominal envelope only; exact maximum occupied solid not qualified'},
 'U7':{'url':'https://www.espressif.com/sites/default/files/documentation/esp32-wroom-32e_esp32-wroom-32ue_datasheet_en.pdf','nominal_mm':[18,25.5],'note':'all supported solder lands on board; antenna intentionally overhangs north edge; RF exception is not an assembly-spacing waiver'}
}
# Explicit attempted XY-only arrangement, rejected for supply/RF/USB proximity.
ATTEMPT={'U10':[116.9,64.7,90],'C39':[97.25,56.75,0],'C68':[97.25,52.5,0],'J_GNSS1':[92.75,52.5,0],'J_LTE1':[97.25,66.5,0],'C31':[98.5,60.25,90],'C34':[98,71.75,0],'C32':[94.75,60,0],'D2':[92,57,-90],'C1':[99.5,80.25,90],'C22':[98.5,62.5,0],'C52':[98.5,63.5,0],'C37':[92,59.75,0],'R43':[98.75,73,0],'R32':[98.75,69.5,0],'R44':[90.25,52,0]}

def main():
 ap=argparse.ArgumentParser(description=__doc__)
 ap.add_argument('--board',type=pathlib.Path,default=BASE/'archive/release-candidate/rev-a/project/companion.kicad_pcb')
 ap.add_argument('--output',type=pathlib.Path,default=BASE/'reports/jlcpcb-dfm/smt-bodies.json')
 args=ap.parse_args(); board=p.LoadBoard(str(args.board)); fps={f.GetReference().upper():f for f in board.GetFootprints()}
 data=json.loads((BASE/'dfm2.json').read_text()); smt=next(a for a in data['actions'] if a['name']=='jlc_analysis_smt')
 bom=json.loads(json.loads(data['boms']['bom_merge_src'])['data'])
 ds={d['des']:d for g in bom['designator_info'] for side in ('top','bottom') for d in (g.get(side) or [])}
 measurements=[m for result in (smt.get('results') or []) for m in (result.get('meas') or [])]
 cats={i:c['name'] for i,c in enumerate(smt['categories'])}; nominal={}; screening={}; qualified={}; source={}; groups={}; lands={}; extents={}
 def rect(f,w,h):
  x,y=p.ToMM(f.GetX()),p.ToMM(f.GetY());return rotate(box(x-w/2,y-h/2,x+w/2,y+h/2),-f.GetOrientationDegrees(),origin=(x,y))
 def padshape(pad):
  bb=pad.GetBoundingBox();return box(p.ToMM(bb.GetLeft()),p.ToMM(bb.GetTop()),p.ToMM(bb.GetRight()),p.ToMM(bb.GetBottom()))
 def mesh(f):
  points=[]
  for m in f.Models():
   filename=pathlib.Path(m.m_Filename.replace('${KIPRJMOD}',str(args.board.parent)))
   if filename.suffix.lower()!='.wrl' or not filename.is_file():continue
   for raw in re.findall(r'point\s*\[([^]]+)\]',filename.read_text()):
    v=[float(n) for n in re.findall(r'[-+]?(?:\d*\.\d+|\d+\.?\d*)(?:[eE][-+]?\d+)?',raw)]
    if not v or len(v)%3:continue
    q=np.array(v).reshape(-1,3)*2.54*np.array([m.m_Scale.x,m.m_Scale.y,m.m_Scale.z]);a=math.radians(m.m_Rotation.z)
    q=q@np.array([[math.cos(a),-math.sin(a),0],[math.sin(a),math.cos(a),0],[0,0,1]]).T+np.array([m.m_Offset.x,m.m_Offset.y,m.m_Offset.z]);q[:,1]*=-1
    a=math.radians(f.GetOrientationDegrees());q=q@np.array([[math.cos(a),math.sin(a),0],[-math.sin(a),math.cos(a),0],[0,0,1]]).T+np.array([p.ToMM(f.GetX()),p.ToMM(f.GetY()),0]);points.extend(q)
  if not points:raise RuntimeError('Missing real mesh or explicit manufacturer dimensions: '+f.GetReference())
  q=np.array(points);return box(*q[:,:2].min(axis=0),*q[:,:2].max(axis=0))
 for ref,d in ds.items():
  f=fps[ref];name=str(f.GetFPID().GetLibItemName());code=d['lc_code'];tol=None;src=None;group='unlisted';dims=None
  if '0402' in name:dims=(1,.5);group='0402';src='UNI' if ref.startswith('R') and bom['comp_info'][code]['lc_ma'].startswith('UNI') else 'C1525' if code=='C1525' else 'FH' if ref.startswith('C') else None;tol=(.1,.05) if src=='UNI' else .05 if src=='C1525' else .2 if src=='FH' else None
  elif '0603' in name:dims=(1.6,.8);group='0603';src=code if code in SOURCES else None;tol=.1 if src else None
  elif '1206' in name:dims=(3.2,1.6);group='1206';src=code if code in SOURCES else None;tol=.2 if src else None
  elif name=='C0805':dims=(2,1.25);group='0805';src='FH';tol=.2
  elif ref in ('C39','C68'):dims=(3.5,2.8);src='C148107'
  aliases={'D2':'D1','QL2':'QH2'};key=aliases.get(ref,ref)
  if key in SOURCES and key in ('D1','U6','U4','U1','U5','U11','QH2'):
   dims=SOURCES[key]['nominal_mm'];tol=SOURCES[key]['size_plusminus_mm'][0];src=key;group='QFN-family'
   if key=='U4':dims=(2,3)  # DLA long axis is footprint-localY; calibrated by asymmetric pins/DFM rightedgeX76.5.
  if dims:
   nominal[ref]=rect(f,*dims);qualified[ref]=tol is not None and src!='FH'
   tx,ty=tol if isinstance(tol,tuple) else (tol,tol) if tol is not None else (.2,.2)
   screening[ref]=rect(f,dims[0]+tx,dims[1]+ty)
  elif ref=='U10':
   nominal[ref]=rect(f,25,29);screening[ref]=rect(f,25.2,29.2);qualified[ref]=True;src='U10'
  elif ref in ('J_GNSS1','J_LTE1'):
   x,y=p.ToMM(f.GetX()),p.ToMM(f.GetY());nominal[ref]=rotate(box(x-1.18,y-1.5,x+1.92,y+1.5),-f.GetOrientationDegrees(),origin=(x,y));screening[ref]=nominal[ref].buffer(.1,join_style=2);qualified[ref]=False;src='UFL'
  else:
   nominal[ref]=mesh(f);screening[ref]=nominal[ref].buffer(.125 if ref in ('J_SD1','USBC1') else .1,join_style=2);qualified[ref]=ref in ('U2','CARD1','J_SD1');src=ref if ref in SOURCES else None
  if any(x in name for x in ('QFN','VSON','USON','TDFN','X1SON')):group='QFN-family'
  elif 'SOT-23' in name:group='SOT'
  elif 'VSSOP' in name:group='SOP'
  groups[ref]=group;source[ref]=src
  lands[ref]=unary_union([padshape(pd) for pd in f.Pads()]);extents[ref]=screening[ref].union(lands[ref]).envelope
 def threshold(a,b):
  ga,gb=groups[a],groups[b]
  if 'U10' in (a,b):return 3.,'Quectel body spacing recommendation'
  if 'unlisted' in (ga,gb):return None,'package not in JLC table; supplier decision required, no invented generic minimum'
  if 'QFN-family' in (ga,gb):return 1.,'JLC QFN-family mapping; DFN/USON/VSON analogy must be confirmed with assembler'
  if 'SOP' in (ga,gb):return (.5 if ga==gb else .4),'JLC SOP'
  if 'SOT' in (ga,gb):return (.4 if ga==gb else .2),'JLC SOT'
  return (.35 if ga==gb=='1206' else .25 if '1206' in (ga,gb) else .15 if ga==gb=='0402' else .18),'JLC chip table'
 def nativepoint(x,y):return [50+.0254*x,50-.0254*y]
 def pose(ref):
  f=fps[ref];return [p.ToMM(f.GetX()),p.ToMM(f.GetY()),f.GetOrientationDegrees()]
 def minimum_body_gap(a,b):return nominal[a].distance(nominal[b])
 def pair(m):
  a,b=m['name1'],m['name2'];t,why=threshold(a,b);bodygap=minimum_body_gap(a,b);sg=screening[a].distance(screening[b]);eg=extents[a].distance(extents[b]);is_mod='U10' in (a,b)
  out={'raw':m,'native_poses_mm_deg':{a:pose(a),b:pose(b)},'body_nominal_bounds_native_mm':{a:list(nominal[a].bounds),b:list(nominal[b].bounds)},'native_nominal_occupied_envelope_gap_mm':bodygap,'manufacturer_max_gap_mm':sg if qualified[a] and qualified[b] else None,'screening_gap_mm':sg,'screening_includes_unqualified_allowance':not(qualified[a] and qualified[b]),'native_outer_pad_lead_extent_gap_mm':eg,'recommendation_mm':t,'recommendation_basis':why,'source_keys':[source[a],source[b]],'nominal_body_collision':nominal[a].intersection(nominal[b]).area>1e-10}
  if is_mod:out['disposition']='GENUINE manufacturer assembly/maintenance-spacing recommendation shortfall; retain electrical placement only as explicitly UNAPPROVED exception, not a false positive';out['minimum_isolated_separation_increase_mm']=max(0,3-bodygap);out['repair']='No electrically safe local XY-only cutover established. See rejected strict-spacing candidate; supplier approval or user-authorized floorplan/package redesign required.'
  elif m['cat_num']==1:out['disposition']='14-percent scale-expansion warning, not physical collision; conservative occupied-envelope gap positive; no body move justified';out['repair']='No placement change. Keep assembler review; connector categories absent JLC table.'
  elif t is not None and eg<t:out['disposition']='GENUINE recommended extent-spacing shortfall or conservative QFN-family screen; not a hard capability failure';out['minimum_isolated_extent_separation_increase_mm']=t-eg;out['repair']='Requires assembly approval or local placement/routing redesign; body-only clearance is insufficient evidence.'
  else:out['disposition']='No supplied-spacing violation established against applicable published category; do not confuse JLC threshold with physical collision'
  s=m['meas_shape']
  if len(s)>3 and s[2]=='L':out['reported_gap_mm']=math.hypot(s[3]-s[0],s[4]-s[1])*.0254;out['reported_native_endpoints_mm']=[nativepoint(s[0],s[1]),nativepoint(s[3],s[4])]
  return out
 warning_pairs=[pair(m) for m in measurements if m['cat_num']==1];comp=[pair(m) for m in measurements if m['cat_num']==2];edges=[];cross=[]
 boardbox=box(50,50,132,88.5)
 def edgegap(poly):
  x0,y0,x1,y1=poly.bounds;return min(x0-50,y0-50,132-x1,88.5-y1)
 for m in measurements:
  if m['cat_num'] not in (3,7):continue
  ref=m['name1'];out={'raw':m,'native_pose_mm_deg':pose(ref),'body_nominal_bounds_native_mm':list(nominal[ref].bounds),'body_nominal_edge_clearance_signed_mm':edgegap(nominal[ref]),'manufacturer_max_edge_clearance_mm':edgegap(screening[ref]) if qualified[ref] else None,'pad_bounds_native_mm':list(lands[ref].bounds),'all_solder_land_bounding_boxes_supported':boardbox.covers(lands[ref]),'source_key':source[ref]}
  if m['cat_num']==3:out['disposition']='Body is inside board but assembly rail proximity is real; no circuit placement move. Require panel/fixture with5mm process rails and component/antenna cutouts, not a silent rule waiver.';edges.append(out)
  else:out['disposition']='Intentional connector mouth/antenna overhang; supported lands do not remove conveyor/reflow fixture collision risk. Require matching fixture/rail relief and assembler approval.';cross.append(out)
 # Every supplied remaining pin/lead record is retained with native pad evidence.
 # Bounding-box nearest-pad association is diagnostic, not a solderability proof.
 pins=[]
 for m in measurements:
  if m['cat_num'] not in (8,9,10,11,12,17):continue
  ref=m['name1'];s=m['meas_shape'];point=nativepoint(s[1],s[2]) if s[0]=='R' else nativepoint(s[0],s[1]);nearest=min(fps[ref].Pads(),key=lambda pd:Point(point).distance(padshape(pd)));bb=padshape(nearest)
  item={'raw':m,'category':cats[m['cat_num']],'native_pose_mm_deg':pose(ref),'embedded_ec':ds[ref]['ec'],'native_diagnostic_point_mm':point,'nearest_native_pad':nearest.GetNumber(),'nearest_native_pad_center_mm':[p.ToMM(nearest.GetX()),p.ToMM(nearest.GetY())],'nearest_native_pad_size_mm':[p.ToMM(nearest.GetSize().x),p.ToMM(nearest.GetSize().y)],'nearest_native_pad_angle_deg':nearest.GetOrientationDegrees(),'source_key':source[ref]}
  if m['cat_num']==17:
   item['meaning']='lead-overlap fraction, NOT stencil paste ratio'
   if ref in ('QH2','QL2'):item['disposition']='Source pads1-4 differ TI recommended pattern and leave nominallead heel .05mm unsupported; drain pads5-8 physically merge with mainpad8 and must be evaluated as copper union. Exact nominal calculations in lead_support.'
   elif ref=='U1':item['disposition']='Native innerland edge1.640 vsTIrecommended1.6225 differs .0175mm; generic-lead .87-.90 overlap not proof solderability failure. Use manufacturer land example and supplier approval, no automatic scaling.'
   else:item['disposition']='U5 selected outer land dimensions match TIexample within .01mm; JLC partialoverlap model does not justify changing manufacturer-recommended pattern.'
  else:
   item['reported_segment_length_mm']=math.hypot(s[3]-s[0],s[4]-s[1])*.0254 if len(s)>4 and s[2]=='L' else None
   item['disposition']='Native pad geometry and manufacturer package must control; tiny boundary residuals are not proof unsupported solder. Nonzero larger discrepancies remain assembler-model/footprint review items, not closed solely by courtyard/3D mesh.'
  pins.append(item)
 lead_support=[]
 for ref in ('QH2','QL2'):
  f=fps[ref];x,y=p.ToMM(f.GetX()),p.ToMM(f.GetY())
  for n in range(1,9):
   # QL2 orientation0 has source at+Y; QH2 orientation180 reverses this.
   xx=(-.975+(n-1)*.65) if n<=4 else (.975-(n-5)*.65);yy=1.425 if n<=4 else -1.425
   shape=rotate(box(x+xx-.17,y+yy-.225,x+xx+.17,y+yy+.225),-f.GetOrientationDegrees(),origin=(x,y))
   own=unary_union([padshape(pd) for pd in f.Pads() if pd.GetNumber()==str(n)])
   union_fraction=shape.intersection(lands[ref]).area/shape.area
   lead_support.append({'reference':ref,'pad':str(n),'manufacturer_nominal_terminal_bounds_native_mm':list(shape.bounds),'own_number_copper_overlap_fraction':shape.intersection(own).area/shape.area,'all_same_footprint_copper_union_overlap_fraction':union_fraction,'disposition':'Nominal terminal fully supported by native copper union; tolerance acceptance remains separate' if union_fraction>=1-1e-7 else 'Measured nominal terminal support deficit; compare independent manufacturer-pattern proof before release'})
 # Candidate uses conservative screening margins where exact max tolerance absent.
 moved={};movedext={}
 for ref in nominal:
  old=pose(ref);new=ATTEMPT.get(ref,old);dx,dy=new[0]-old[0],new[1]-old[1];moved[ref]=translate(screening[ref],xoff=dx,yoff=dy);movedext[ref]=translate(extents[ref],xoff=dx,yoff=dy)
 candidate_shortfalls=[{'ref':k,'gap_mm':moved['U10'].distance(v)} for k,v in moved.items() if k!='U10' and moved['U10'].distance(v)<3-1e-7]
 collisions=[{'a':a,'b':b} for i,a in enumerate(moved) for b in list(moved)[i+1:] if moved[a].intersection(moved[b]).area>1e-8 and (a in ATTEMPT or b in ATTEMPT)]
 changed_nets=sorted({pd.GetNetname() for ref in ATTEMPT for pd in fps[ref].Pads() if pd.GetNetCode()})
 invalidated=[str(t.m_Uuid.AsString()) for t in board.GetTracks() if t.GetNetname() in changed_nets]
 mapping=[]
 for ref,d in ds.items():
  x,y,a=pose(ref);ec=d['ec'];mapping.append({'ref':ref,'native_pose_mm_deg':[x,y,a],'ec_pose':ec,'position_residual_mm':[x-50-ec['x'],50-y-ec['y']],'rotation_residual_mod360_deg':(a-ec['ang'])%360,'lc_code':d['lc_code'],'mpn':bom['comp_info'][d['lc_code']]['part_name'],'package':d['ft_name']})
 out={'scope':'Every supplied dfm2 SMT measurement parsed; no claim to original records absent from capped JSON','source_board':str(args.board.relative_to(ROOT)) if args.board.is_relative_to(ROOT) else str(args.board),'source_board_sha256':hashlib.sha256(args.board.read_bytes()).hexdigest(),'units':{'meas_shape':'mil','profile_and_file_boundbox':'inch','native':'mm','transform':'xNative=50+.0254*xDFM; yNative=50-.0254*yDFM; corroborated by all123 embedded placements and82x38.5mm board'},'sources':SOURCES,'jlc_current_spacing':{'url':SPACING,'last_updated':'2026-09-09','measurement_illustration':'https://rs.jlcpcb.com/static/image/help/pcb-assembly/smd-component-spacing-measurement-method.png','method':'outermost lead/pad extents in lead direction, body perpendicular; NOT body-to-body for every pair; matrix recommendations are not hard capability limits','process_rail_url':'https://jlcpcb.com/help/article/specifications-for-adding-process-edges-and-positioning-holes','process_rails_recommended_mm':5},'counts_supplied':dict(collections.Counter(cats[m['cat_num']] for m in measurements)),'truncation':[{**t,'category':cats[t['cat_num']]} for result in smt['results'] for t in (result.get('truncate') or [])],'missing_comp2comp_measurements':720-len(comp),'c2c_size':warning_pairs,'comp2edge':edges,'comp_x_edge':cross,'comp2comp_supplied':comp,'pin_and_lead_supplied':pins,'lead_support':lead_support,'bom_cpl_correlation':mapping,'R19':{'check_no_package':data['check']['no_package'],'embedded':ds['R19'],'selected_component':bom['comp_info'][ds['R19']['lc_code']],'disposition':'Not a missing BOM row, wrong selected resistor, or unprovided CPL. R19 explicitly lacks a recognized JLC assembly package/model despite valid0402 package string; request model assignment/confirmation for C2117579. Preserve part selection.'},'model_correction':{'refs':['J_GNSS1','J_LTE1'],'rotation_xyz_deg':[0,0,0],'offset_xyz_mm':[-.011952,0,0],'calibration':'rawWRL housing[-.668020,-1.299972]..[1.931924,1.299972], centerX.631952; groundpad-centerX.62','nominal_physical_override_local_xy_mm':[-1.18,-1.5,1.92,1.5],'limitations':'3.1mm dimension reference only; fullmodel metal not manufacturer maximumtol solid; source scripts fixed, parent applies native/normalized metadata'},'rejected_strict_spacing_candidate':{'native_poses_mm_deg':ATTEMPT,'method':'nearest-grid slots constrained by bodymodem3mm, lead/pad obstacle spacing, supported pad extents; no routing performed','modem_shortfalls_with_current_model':candidate_shortfalls,'moved_body_collisions_with_current_model':collisions,'invalidated_nets':changed_nets,'invalidated_route_item_uuids':invalidated,'accepted':False,'reason':'Quectel p33 requires MLCCarray close toVBAT_BB/RF and star traces >=2/2.5mm. FixedUSB plus3mm bodygap forces modem top to52.1mm; top/right strips cannot hold caps. Top powerpins move to~Y55.5, any allowed lowerbody region startsY80.1 or leftbody region endsX99.3: >=24mm geometric distance. Candidate movesC68~29mm left, incompatible with preserving compact supply loop; no claim reroutable/electrically validated.','alternative_attempt':{'U10_pose_mm_deg':[116.9,66.2,90],'U6_pose_mm_deg':[130.8,82.5,180],'result':'D1 and all south caps still below3mm, D1 southmove collides U8/USB; rejected domino rather than unroute routed design'}},'verdict':'Retain routed XY as best engineering candidate, NOT manufacture-approved. Correct proven U.FL rendering metadata. Manufacturer/assembler spacing exceptions, missing R19assembly model, source MOSFET lands and fixture relief require resolution before release.','measurement_limits':['Nominal mesh bounding envelopes are not detailed or manufacturer-max solids.','FHselected size subcode not supplied; family maximum conservatively used.','Unknown body tolerances are explicitly null; screening .1mm/side is an assumption, not manufacturer evidence.','No DRC/ERC, routing, production placement or solderability test run.']}
 args.output.parent.mkdir(parents=True,exist_ok=True);args.output.write_text(json.dumps(out,indent=2));print(json.dumps({'output':str(args.output),'collisions':len(warning_pairs),'edge':len(edges),'cross':len(cross),'comp_supplied':len(comp),'pins_and_leads':len(pins),'candidate_shortfalls':candidate_shortfalls,'candidate_body_collisions':collisions},indent=2))
if __name__=='__main__':main()
