#!/usr/bin/python3
"""Map supplied JLC measurements to frozen native geometry and plotted mask apertures.
Read-only: no board saves, refill, DRC or rule changes. Requires pcbnew and shapely.
"""
import argparse, collections, hashlib, json, math, pathlib, re
import pcbnew as p
from shapely.geometry import Point, LineString, Polygon, box
from shapely.ops import unary_union
from shapely.affinity import translate, rotate

ROOT=pathlib.Path(__file__).resolve().parents[3]
AP=argparse.ArgumentParser();AP.add_argument('--candidate',default=str(ROOT/'agent/companion/archive/release-candidate/rev-a'));AP.add_argument('--output',default=str(ROOT/'agent/companion/reports/jlcpcb-dfm/fabrication-findings.json'));args=AP.parse_args()
C=pathlib.Path(args.candidate);BP=C/'project/companion.kicad_pcb';b=p.LoadBoard(str(BP));mm=p.ToMM
origin=b.GetDesignSettings().GetAuxOrigin();ox,oy=mm(origin.x),mm(origin.y)
def xy(v):return [mm(v.x),mm(v.y)]
def poly(ps):
 out=[]
 for i in range(ps.OutlineCount()):
  c=ps.COutline(i);pts=[xy(c.CPoint(j)) for j in range(c.PointCount())]
  holes=[]
  for k in range(ps.HoleCount(i)):
   h=ps.CHole(i,k);holes.append([xy(h.CPoint(j)) for j in range(h.PointCount())])
  if len(pts)>2:out.append(Polygon(pts,holes))
 return unary_union(out)
def capsule(w,h):
 r=min(w,h)/2
 if abs(w-h)<1e-9:return Point(0,0).buffer(r,quad_segs=96)
 return LineString([(-(w-h)/2,0),((w-h)/2,0)]).buffer(r,quad_segs=96) if w>h else LineString([(0,-(h-w)/2),(0,(h-w)/2)]).buffer(r,quad_segs=96)
def parse_mask(path):
 text=path.read_text();mac={};aps={};out=[]
 for name,body in re.findall(r'%AM([^*]+)\*(.*?)%',text,re.S):mac[name]=body
 for n,kind,params in re.findall(r'%ADD(\d+)([^,*]+),([^*]+)\*%',text):
  vals=[float(x) for x in params.split('X')]
  if kind=='R':g=box(-vals[0]/2,-vals[1]/2,vals[0]/2,vals[1]/2)
  elif kind=='C':g=Point(0,0).buffer(vals[0]/2,quad_segs=96)
  elif kind=='O':g=capsule(*vals[:2])
  elif kind=='RoundRect':
   r=vals[0];cs=list(zip(vals[1:9:2],vals[2:9:2]));g=Polygon(cs).buffer(r,quad_segs=96)
  elif kind.startswith('FreePoly'):
   fields=mac[kind].split('*')[0].split(',');count=int(fields[2]);nums=[float(x) for x in fields[3:3+2*count]];g=Polygon(list(zip(nums[::2],nums[1::2])))
  else:raise ValueError((kind,params))
  aps[int(n)]=(g,kind,vals)
 n=None;ref=None
 for line in text.splitlines():
  if m:=re.fullmatch(r'D(\d+)\*',line):n=int(m.group(1))
  if m:=re.match(r'%TO.P,([^,*]+),([^,*]+)',line):ref=m.group(1)
  if m:=re.match(r'%TO.C,([^*]+)',line):ref=m.group(1)
  if line=='%TD*%':ref=None
  if m:=re.fullmatch(r'X(-?\d+)Y(-?\d+)D03\*',line):
   x,y=int(m.group(1))/1e6,int(m.group(2))/1e6;g,kind,vals=aps[n]
   # Gerber Y is positive up; native Y positive down.
   from shapely.affinity import scale
   g=translate(scale(g,xfact=1,yfact=-1,origin=(0,0)),ox+x,oy-y)
   out.append(dict(ref=ref,aperture=n,kind=kind,params=vals,position_mm=[ox+x,oy-y],geometry=g,gerber_line=line))
 return out
objects=[];pads=[];vias=[];tracks=[]
def add(obj,g,kind,layer,extra):
 d=dict(uuid=obj.m_Uuid.AsString(),kind=kind,layer=layer,net=obj.GetNetname(),position_mm=xy(obj.GetPosition()),**extra);row=(d,g,obj);objects.append(row);return row
for f in b.GetFootprints():
 for a in f.Pads():
  for layer in [p.F_Cu,p.B_Cu]:
   if a.IsOnLayer(layer):pads.append(add(a,poly(a.GetEffectivePolygon(layer)),'pad',b.GetLayerName(layer),dict(ref=f.GetReference(),pad=a.GetNumber(),size_mm=xy(a.GetSize()),drill_mm=xy(a.GetDrillSize()),mask_expansion_mm=mm(a.GetSolderMaskExpansion(p.F_Mask if layer==p.F_Cu else p.B_Mask)),mask_layer_present=a.IsOnLayer(p.F_Mask if layer==p.F_Cu else p.B_Mask))))
for t in b.GetTracks():
 if isinstance(t,p.PCB_VIA):
  for layer in [p.F_Cu,p.B_Cu]:
   vias.append(add(t,Point(xy(t.GetPosition())).buffer(mm(t.GetWidth(layer))/2,quad_segs=96),'via',b.GetLayerName(layer),dict(diameter_mm=mm(t.GetWidth(layer)),drill_mm=mm(t.GetDrillValue()),filled=t.GetFillingMode()==p.FILLING_MODE_FILLED,capped=t.GetCappingMode()==p.CAPPING_MODE_CAPPED)))
 elif t.GetLayer() in [p.F_Cu,p.B_Cu]:
  g=LineString([xy(t.GetStart()),xy(t.GetEnd())]).buffer(mm(t.GetWidth())/2,quad_segs=96)
  tracks.append(add(t,g,'track',b.GetLayerName(t.GetLayer()),dict(width_mm=mm(t.GetWidth()),start_mm=xy(t.GetStart()),end_mm=xy(t.GetEnd()))))
# Native effective polygons use coarse inscribed arcs; plotted pad flashes retain
# the exact Gerber aperture parameters for comparisons against mask flashes.
plotted_pad_geometry=[]
for side in ['F','B']:
 flashes=parse_mask(C/'fabrication'/f'companion-{side}_Cu.{"gtl" if side=="F" else "gbl"}')
 for row in list(pads):
  d,g,o=row
  if d['layer']!=side+'.Cu':continue
  matches=[f for f in flashes if f['ref']==d['ref'] and math.dist(f['position_mm'],d['position_mm'])<.000002]
  if matches:
   best=min(matches,key=lambda f:f['geometry'].symmetric_difference(g).area)
   plotted_pad_geometry.append(dict(uuid=d['uuid'],side=side,aperture=best['aperture'],gerber_line=best['gerber_line']))
   idx=objects.index(row);replacement=(d,best['geometry'],o);objects[idx]=replacement;pads[pads.index(row)]=replacement
for z in b.Zones():
 if not z.GetIsRuleArea() and z.GetLayer() in [p.F_Cu,p.B_Cu]:
  add(z,poly(z.GetFilledPolysList(z.GetLayer())),'filled_zone',b.GetLayerName(z.GetLayer()),{})
mask={side:parse_mask(C/'fabrication'/f'companion-{side}_Mask.{"gts" if side=="F" else "gbs"}') for side in ['F','B']}
# Match every plotted mask flash using both component X2 attribute and actual geometry.
mask_matches=[]
for side,rows in mask.items():
 for m in rows:
  poss=[r for r in pads if r[0]['layer']==side+'.Cu' and r[0]['ref']==m['ref']]
  ranked=sorted(poss,key=lambda r:r[1].symmetric_difference(m['geometry']).area)
  m['pad']=ranked[0] if ranked else None
  if ranked:mask_matches.append(dict(side=side,ref=m['ref'],pad=ranked[0][0]['pad'],uuid=ranked[0][0]['uuid'],aperture=m['aperture'],symmetric_difference_mm2=ranked[0][1].symmetric_difference(m['geometry']).area,mask_area_mm2=m['geometry'].area,copper_area_mm2=ranked[0][1].area,unopened_copper_area_mm2=ranked[0][1].difference(m['geometry']).area,gerber_line=m['gerber_line']))
bylayer={layer:[r for r in objects if r[0]['layer']==layer] for layer in ['F.Cu','B.Cu']}
def nearest(q,layer,limit=4):
 pt=Point(q);rows=bylayer.get(layer,objects)
 ranked=sorted(rows,key=lambda r:r[1].boundary.distance(pt))[:limit]
 return [dict(**d,distance_to_boundary_mm=g.boundary.distance(pt),point_in_copper=g.covers(pt)) for d,g,o in ranked]
def drill_nearest(q,layer):
 pt=Point(q);rows=[]
 for d,g,o in bylayer[layer]:
  if d['kind']=='via':
   hole=Point(d['position_mm']).buffer(d['drill_mm']/2,quad_segs=96)
  elif d['kind']=='pad' and min(d['drill_mm'])>0:
   hole=translate(rotate(capsule(*d['drill_mm']),-o.GetOrientationDegrees(),origin=(0,0)),*d['position_mm'])
  else:continue
  rows.append(dict(**d,distance_to_drill_boundary_mm=hole.boundary.distance(pt)))
 return sorted(rows,key=lambda d:d['distance_to_drill_boundary_mm'])[:3]
def native(v):return [ox+v[0]*.0254,oy-v[1]*.0254]
def shape_points(s):
 if s[0]=='CIRCLE':return [native(s[1:3])]
 if s[0]=='R':return [native(s[1:3])]
 if len(s)>=5 and s[2]=='L':return [native(s[:2]),native(s[3:5])]
 return []
reports=[]
for name in ['dfm1.json','dfm2.json']:
 src=ROOT/'agent/companion'/name;doc=json.loads(src.read_text());acts=[]
 for a in doc['actions']:
  if a['name'] not in ['jlc_analysis_signal','jlc_analysis_sm','jlc_analysis_drill','jlc_analysis_ss']:continue
  layers=[]
  for r in a['results'] or []:
   counts=collections.Counter(m['cat_num'] for m in r['meas'] or []);orig={x['cat_num']:x['orig_num'] for x in r['truncate'] or []};records=[]
   side='B' if 'companion-B_' in r['layer'] else 'F';layer=side+'.Cu'
   for idx,m in enumerate(r['meas'] or []):
    pts=shape_points(m['meas_shape']);row=dict(index=idx,category=a['categories'][m['cat_num']]['name'],source=m,native_points_mm=pts,nearest_objects=[nearest(q,layer) for q in pts])
    if row['category'] in ['pth_ar','pth2line','via2pad']:
     row['drill_candidates_at_first_point']=drill_nearest(pts[0],layer)
    cat=row['category']
    if cat=='fills':
     s=m['meas_shape'];q=pts[0];bounds=[q[0],q[1],q[0]+s[3]*.0254,q[1]+s[4]*.0254];region=box(*bounds)
     row['rectangle_native_bounds_mm']=bounds
     row['overlapping_native_lands']=[d for d,g,o in bylayer[layer] if d['kind']=='pad' and g.intersection(region).area>1e-6]
    if cat in ['pad2pad','neg_pad_ar','line2line','lines','bridge','coverage']:
     selected=[]
     for i,q in enumerate(pts):
      if cat in ['bridge','coverage','neg_pad_ar'] and (i==0 or cat!='coverage'):
       rows=[(x['pad'][0],x['geometry'],x['pad'][2]) for x in mask[side] if x.get('pad')]
      else:
       kinds=['pad','via'] if cat=='pad2pad' else ['track'] if cat in ['line2line','lines'] else ['track','via','filled_zone']
       rows=[o for o in bylayer[layer] if o[0]['kind'] in kinds]
      ranked=sorted(rows,key=lambda x:x[1].boundary.distance(Point(q)))
      if ranked:selected.append(ranked[0])
     row['mapped_endpoint_objects']=[x[0] for x in selected]
     if len(selected)==2:
      row['same_net']=selected[0][0]['net']==selected[1][0]['net']
      row['native_object_clearance_mm']=selected[0][1].distance(selected[1][1])
    if len(pts)==2:row['reported_distance_mm']=math.dist(*pts);row['reported_distance_mil']=math.dist(*pts)/.0254
    records.append(row)
   layers.append(dict(layer=r['layer'],category_counts=[dict(name=c['name'],range_mil=c['range'],retained=counts[i],original=orig.get(i,counts[i]),omitted=max(0,orig.get(i,counts[i])-counts[i])) for i,c in enumerate(a['categories'])],records=records))
  acts.append(dict(name=a['name'],category_definitions=a['categories'],layers=layers))
 reports.append(dict(path=str(src.relative_to(ROOT)),sha256=hashlib.sha256(src.read_bytes()).hexdigest(),actions=acts))
# Exact exposure intersections: use actual plotted mask aperture, not expanded boxes.
exposures=[]
for side,rows in mask.items():
 for m in rows:
  if not m['pad']:continue
  pd,pg,po=m['pad']
  for d,g,o in bylayer[side+'.Cu']:
   if d['uuid']==pd['uuid'] or d['net']==pd['net']:continue
   if not g.intersects(m['geometry']):continue
   area=g.intersection(m['geometry']).area
   if area>1e-7:exposures.append(dict(opening_pad=pd,opening_aperture=m['aperture'],other=d,exposed_other_net_area_mm2=area,pad_to_other_copper_gap_mm=pg.distance(g)))
# Mask union checks functional pad coverage; same-net POFV annulus can intentionally extend beyond land.
functional_coverage=[]
for side in ['F','B']:
 union=unary_union([m['geometry'] for m in mask[side]])
 for d,g,o in pads:
  if d['layer']!=side+'.Cu' or not d['mask_layer_present']:continue
  missing=g.difference(union).area
  if missing>1e-5:functional_coverage.append(dict(**d,copper_area_mm2=g.area,covered_area_mm2=missing,covered_fraction=missing/g.area))
# Source process exceptions: drill intersecting SMT land, not annulus-only overlap.
vip=[]
for d,g,o in vias:
 if d['layer']!='F.Cu':continue
 hole=Point(d['position_mm']).buffer(d['drill_mm']/2,quad_segs=96)
 hits=[pd for pd,pg,po in pads if pd['layer']=='F.Cu' and po.GetAttribute()==p.PAD_ATTRIB_SMD and hole.intersection(pg).area>1e-8]
 if hits:vip.append(dict(**d,lands=hits))
result=dict(scope='Full supplied fabrication records, frozen release-candidate native source and exact Gerber mask flashes; not a DRC or supplier approval.',source_board=str(BP.relative_to(ROOT)),source_board_sha256=hashlib.sha256(BP.read_bytes()).hexdigest(),transform=dict(dfm_measurement_unit='mil',native_mm=['aux_x + 0.0254*x','aux_y - 0.0254*y'],aux_origin_mm=[ox,oy],gerber_unit='mm',gerber_to_native=['aux_x + x','aux_y - y']),counts=dict(footprints=len(list(b.GetFootprints())),native_pads=len(set(d['uuid'] for d,g,o in pads)),native_vias=len(vias)//2,native_outer_tracks=len(tracks),gerber_front_mask_flashes=len(mask['F']),gerber_back_mask_flashes=len(mask['B'])),reports=reports,mask_flash_matches=mask_matches,functional_pad_coverage_deficits=functional_coverage,cross_net_copper_inside_mask_openings=exposures,filled_capped_via_in_pad=vip)
result['plotted_copper_pad_flash_mapping']=plotted_pad_geometry
result['precision_notes']=['Rectangles, circles, obrounds, RoundRect and FreePoly apertures parsed; no bounding-box pad substitution.','Curves use384 segments/circle, sagitta<=.000021mm at radius.6mm; source Gerber precision1um.','Native effective polygons use coarse inscribed arcs; final pad/mask comparisons instead use independently matched plotted copper flashes.','JLC R x,y denotes native top-left bounding corner, not center.','All retained records mapped; original omitted positions are not recoverable from supplied truncated JSON.']
result['transform']['calibration']=[dict(object='USBC1.4 asymmetric plated slot',native_center_mm=[120.67,83.2],gerber_endpoints_mm=[[70.67,-32.65],[70.67,-33.75]],drill_width_mm=.6,total_slot_length_mm=1.7,dfm_width_endpoints_mil=[2794.094,-1307.086,2770.472,-1307.086]),dict(object='Outline',native_bounds_mm=[50,50,132,88.5],gerber_bounds_mm=[0,-38.5,82,0],dfm_profile_x_inches=3.228346)]
result['actual_process_capabilities']=dict(source_urls=['https://jlcpcb.com/capabilities/pcb-capabilities/','https://jlcpcb.com/help/article/instructions-for-ordering','https://jlcpcb.com/help/article/via-in-pad-pofv-guide'],observation='JLC primary table indexed by web search2026-10-29; static reader omits dynamic table. POFV guide as retained project manufacturer evidence.',four_layer_1oz_min_track_and_spacing_mm=.09,green_1oz_mask_bridge_pad_spacing_mm=.1,multilayer_min_plated_slot_width_mm=.35,min_slot_length_width_ratio=2,POFV_preferred_diameter_minus_drill_mm=.15,template_warning='range arrays are generic reporting bins, not measured values or necessarily applicable multilayer limits. slot_width24/32/40mil=.6096/.8128/1.016mm, versus measured .600mm slots.')
dispositions={
 'acute_line':'Real45degree B.Cu junction; .3mm leg chamfer preserves width/net/UUIDs.',
 'bga_pad':'No supplied measurements; no inferred package conclusion.',
 'hole_on_pad':'No detector records, but independent geometry identifies retained planar filled/capped POFV.',
 'line2edge':'No supplied measurements.',
 'line2line':'All7 retained gaps exceed.09mm four-layer capability; minimum.1125mm U5 VCC/ILIM. Preserve rules.',
 'line_stub':'No supplied measurements.',
 'lines':'Retained.1-.15mm widths exceed.09mm actual multilayer capability despite generic4/5/8mil bins; preserve intentional routing.',
 'marks':'No records; not proof supplier-panel fiducials exist.',
 'pad2edge':'Minimum.175mm to.05mm outline stroke=.200mm to route centerline. USB shell lands.220mm to actual edge; preserve manufacturer lands/mechanics.',
 'pad2pad':'Very small same-net via/land gaps are not shorts. Retained different-net minimum.1375mm exceeds.09mm capability; tented vias are not separate solder joints. No land resizing.',
 'pth2line':'Hole-boundary distances are not copper-annulus clearance. Drill UUID candidates supplied explicitly; generic7/9/12mil bins are not copper limits.',
 'pth_ar':'Reported.05/.075mm via annuli are not component-slot ring failures. POFV drill+diameter margin>=.15mm; ordinary.05mm via rings intentional process exception requiring supplier acceptance.',
 'tht2smd':'No supplied measurements.',
 'via2pad':'Zero-distance same-net drill/land intersections require retained resin fill/planarization/copper cap; not ordinary tenting. All intersecting via/land UUIDs supplied.',
 'bridge':'Retained.175-.200mm bridges exceed actual.1mm green-mask capability; preserve fine-pitch dams and exact lands.',
 'coverage':'Small front gaps are same-net outgoing copper/POFV. No different-net copper inside exact openings. Retained different-net near-trace minimum.1025mm exceeds3mil=.0762mm low template bin. Bottom GND USB-shell apertures have about.2005mm clearance to USB_5V filled-zone boundaries, not covered functional lands.',
 'fills':'QL2/QH2 drain pads5-8 overlap their large duplicated pad8; same-net contiguous drain aperture. Rectangle bounds and every overlapping land UUID supplied. Preserve exact manufacturer lands.',
 'neg_pad_ar':'All28 are matched RoundRect copper/mask corners; independently identical plotted apertures fully expose functional pads at zero expansion. No real negative expansion.',
 'ss2pad':'No records; both supplied silk Gerbers have zero drawn primitives.',
 'ss2pth':'No records; both supplied silk Gerbers have zero drawn primitives.',
 'ssl_w':'No records; intentionally blank silk; artwork remains on fabrication layers.',
 'blind_distance':'No records; native board has through-vias, not blind/buried vias.',
 'miss_pth':'No supplied measurements.',
 'near_blind_distance':'No records; native board uses through-vias.',
 'pth_2_pth':'No supplied measurements.',
 'short_slot':'No records; .6x1.4/.6x1.7mm USB slots exceed2:1.',
 'slot_width':'Four.600mm plated shell slots exceed actual.35mm multilayer minimum and2:1 length/width. Preserve exact manufacturer holes rather than resize to generic24mil=.6096mm.',
 'via_2_pth':'No supplied measurements.',
 'via_stub':'No supplied measurements.'}
result['category_dispositions']=dispositions
result['category_measurement_summary']=[]
for act in reports[0]['actions']:
 for layer in act['layers']:
  for c in layer['category_counts']:
   rows=[r for r in layer['records'] if r['category']==c['name']];vals=[r['reported_distance_mm'] for r in rows if 'reported_distance_mm' in r]
   result['category_measurement_summary'].append(dict(action=act['name'],layer=layer['layer'],**c,measured_retained_range_mm=[min(vals),max(vals)] if vals else None,disposition=dispositions[c['name']]))
v1=[93.787868,61.012132];v2=[94.,61.1]
new_corner=unary_union([LineString([v1,[91.7,63.1]]).buffer(.075,quad_segs=96),LineString([[94,63.1],v2]).buffer(.075,quad_segs=96),LineString([v1,v2]).buffer(.075,quad_segs=96)])
corner_neighbors=[dict(**d,proposed_clearance_mm=new_corner.distance(g)) for d,g,o in bylayer['B.Cu'] if d['net']!='Net-(U5-MODE)' and new_corner.distance(g)<1]
result['actionable_repairs']=[dict(id='FAB-ACUTE-01',severity='MINOR',source='dfm1/dfm2 signal B.Cu acute_line record0',net='Net-(U5-MODE)',layer='B.Cu',native_corner_mm=[94,60.8],original_angle_deg=45,original_segments=[dict(uuid='1e4c519b-7c8e-4426-8111-d85e72e39f99',start_mm=[94,60.8],end_mm=[91.7,63.1],length_mm=math.sqrt(2)*2.3),dict(uuid='2e4efc9a-f50d-4e17-be86-f8f38f6c1464',start_mm=[94,63.1],end_mm=[94,60.8],length_mm=2.3)],repair='Shorten original legs by.300mm; add.15mm chamfer between listed endpoints, preserving original UUIDs and all rules.',chamfer_endpoints_mm=[v1,v2],new_interior_angles_deg=[112.5,112.5],chamfer_length_mm=math.dist(v1,v2),local_collision_screen=sorted(corner_neighbors,key=lambda x:x['proposed_clearance_mm']),stored_fill_policy='Stored filled zones are stale after reroute: parent must refill. Non-zone copper must retain>=.1mm clearance.',repair_script='agent/companion/scripts/repair_dfm_corner.py',script_execution='Not run by fabrication analysis agent')]
result['native_via_annulus_census']=[dict(diameter_mm=k[0],drill_mm=k[1],radial_ring_mm=(k[0]-k[1])/2,count=v) for k,v in sorted(collections.Counter((d['diameter_mm'],d['drill_mm']) for d,g,o in vias if d['layer']=='F.Cu').items())]
result['annular_exception_objects']=[d for d,g,o in vias if d['layer']=='F.Cu' and d['diameter_mm']-d['drill_mm']<.15-.000001]
negative_uuids={r['mapped_endpoint_objects'][0]['uuid'] for act in reports[0]['actions'] for layer in act['layers'] for r in layer['records'] if r['category']=='neg_pad_ar'}
result['negative_mask_aperture_evidence']=dict(reported_records=28,unique_pads=len(negative_uuids),maximum_exact_plotted_copper_mask_symmetric_difference_mm2=max(x['symmetric_difference_mm2'] for x in mask_matches if x['uuid'] in negative_uuids),records=[x for x in mask_matches if x['uuid'] in negative_uuids])
exposed_pair_candidates=[]
for side,openings in mask.items():
 for i,m in enumerate(openings):
  if not m['pad']:continue
  for n in openings[i+1:]:
   if not n['pad'] or m['pad'][0]['net']==n['pad'][0]['net']:continue
   distance=m['geometry'].distance(n['geometry'])
   if distance<.201:exposed_pair_candidates.append(dict(layer=side+'.Cu',aperture_gap_mm=distance,pad1=m['pad'][0],pad2=n['pad'][0]))
result['smallest_cross_net_exposed_pad_clearances']=sorted(exposed_pair_candidates,key=lambda x:x['aperture_gap_mm'])[:20]
result['scope_limitations']=['All conclusions apply to source_board_sha256 frozen release candidate, not the parent-edited board.','Only outer copper report actions are supplied; no inner-layer JLC action records exist.','No DRC, ERC, zone refill, CLI export or repair execution performed here.','Exact aperture/copper exposure comparisons cover plotted pads, native tracks/vias and stored filled zones, not manufacturing registration tolerances. Supplier process approval remains mandatory.','Unplated locating holes have mask openings but no copper flash; their coarse native hole polygons explain residual aggregate mask/copper area difference, not a covered copper land.']
result['verdict']='FIX FIRST FAB-ACUTE-01. No measured covered functional pads, cross-net mask exposure, undersized plated slots or sub-capability track/spacing in retained findings. Preserve exact lands and filled/capped POFV. Supplier process confirmation and fresh post-edit export remain parent responsibilities.'
path=pathlib.Path(args.output);path.parent.mkdir(parents=True,exist_ok=True);path.write_text(json.dumps(result,indent=2));print(json.dumps(dict(output=str(path),counts=result['counts'],functional_coverage_deficits=len(functional_coverage),cross_net_mask_exposures=len(exposures),POFV_count=len(vip),maximum_mask_copper_flash_difference_mm2=max(x['symmetric_difference_mm2'] for x in mask_matches),corner_collision_screen=result['actionable_repairs'][0]['local_collision_screen']),indent=2))
