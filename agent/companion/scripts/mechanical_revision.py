#!/usr/bin/python3
"""Measure saved PCB pad support and calibrated VRML body envelopes in 3D.
AABB exclusion is conservative; it does not certify RF or unprovided cell tolerances.
"""
import pathlib,re,json,math,collections,os,sys,shutil
import numpy as np
import pcbnew as p
from shapely.geometry import box,Polygon
ROOT=pathlib.Path(__file__).resolve().parents[1]
tool=ROOT/'tools/KiCadRoutingTools'
for required in ('py_router/kicad_parser.py','py_placer/placement/heights.py'):
    if not (tool/required).is_file():
        raise SystemExit(f'Missing KiCadRoutingTools dependency: {tool/required}. Restore the real tool checkout under agent/companion/tools/KiCadRoutingTools.')
sys.path[:0]=[str(tool/'py_router'),str(tool/'py_placer')]
try:
    from kicad_parser import parse_kicad_pcb
    from placement.heights import for_board
except ImportError as error:
    raise SystemExit(f'Cannot load KiCadRoutingTools mechanical reader: {error}. Install the dependencies in {tool/"requirements.txt"} using this Python interpreter.') from error
cli=shutil.which('kicad-cli')
if cli is None:
    raise SystemExit('Missing kicad-cli: install KiCad and put kicad-cli on PATH for real GLB height extraction.')
model_root=pathlib.Path(cli).resolve().parents[1]/'share/kicad/3dmodels'
cfg=json.loads((ROOT/'scripts/layout-revision.json').read_text());b=p.LoadBoard(str(ROOT.parents[1] / 'kicad/companion/companion.kicad_pcb'));to=p.ToMM;O=cfg['origin'];W,H=cfg['size']
parts=[];missing=[];unsupported=[]
for f in b.GetFootprints():
    ref=f.GetReference();world=[]
    for mod in f.Models():
        name=mod.m_Filename.replace('${KIPRJMOD}',str(ROOT.parents[1] / 'kicad/companion'))
        for env in ('KICAD9_3DMODEL_DIR','KICAD10_3DMODEL_DIR'):
            name=name.replace('${'+env+'}',os.environ.get(env,str(model_root)))
        filename=pathlib.Path(os.path.expandvars(name))
        if not filename.is_file():
            raise SystemExit(f'Missing 3D model for {ref}: {mod.m_Filename} resolves to {filename}. Install the matching KiCad model library or configure its KICAD*_3DMODEL_DIR environment variable.')
        if filename.suffix.lower()!='.wrl':continue
        pts=[]
        for raw in re.findall(r'point\s*\[([^]]+)\]',filename.read_text()):
            vals=[float(v) for v in re.findall(r'[-+]?(?:\d*\.\d+|\d+\.?\d*)(?:[eE][-+]?\d+)?',raw)]
            if vals and len(vals)%3==0:pts.extend(np.array(vals).reshape(-1,3)*2.54)
        if not pts:continue
        pts=np.array(pts)*np.array([mod.m_Scale.x,mod.m_Scale.y,mod.m_Scale.z])
        # Models in this repository are already-meshed WRL. Z rotations and
        # physical offsets were calibrated against USB asymmetric locating pegs.
        if abs(mod.m_Rotation.x)>1e-6 or abs(mod.m_Rotation.y)>1e-6:raise RuntimeError(f'Uncalibrated model tilt {ref}')
        a=math.radians(mod.m_Rotation.z);rz=np.array([[math.cos(a),-math.sin(a),0],[math.sin(a),math.cos(a),0],[0,0,1]])
        pts=pts@rz.T+np.array([mod.m_Offset.x,mod.m_Offset.y,mod.m_Offset.z]);pts[:,1]*=-1
        a=math.radians(f.GetOrientationDegrees());rz=np.array([[math.cos(a),math.sin(a),0],[-math.sin(a),math.cos(a),0],[0,0,1]])
        pts=pts@rz.T+np.array([to(f.GetX())-O[0],to(f.GetY())-O[1],0]);world.extend(pts)
    if world:
        arr=np.array(world);parts.append(dict(ref=ref,side='B' if f.IsFlipped() else 'F',min=arr.min(axis=0).tolist(),max=arr.max(axis=0).tolist(),model='approximate documented bounding solid' if ref=='U10' else 'library meshed model'))
    elif not ref.startswith('TP'):missing.append(ref)
    for pad in f.Pads():
        r=pad.GetBoundingBox();land=[to(r.GetLeft())-O[0],to(r.GetTop())-O[1],to(r.GetRight())-O[0],to(r.GetBottom())-O[1]]
        if land[0]<0 or land[1]<0 or land[2]>W or land[3]>H:unsupported.append([ref,pad.GetNumber(),land])
# KiCad's own GLB export resolves the standard STEP models that are not VRML.
# Reuse KRT's calibrated mesh-height reader; XY remains a conservative occupied
# envelope for these ordinary SMT passives, not a fabricated detailed solid.
heights=for_board(parse_kicad_pcb(str(ROOT.parents[1] / 'kicad/companion/companion.kicad_pcb')),str(ROOT.parents[1] / 'kicad/companion/companion.kicad_pcb'))
remaining=[]
for ref in missing:
    fp=b.FindFootprintByReference(ref)
    if fp.IsDNP():continue
    height=2.4 if ref=='U10' else heights.height(ref)
    if height is None:remaining.append(ref);continue
    r=fp.GetBoundingBox(False,False)
    parts.append({'ref':ref,'side':'F','min':[to(r.GetLeft())-O[0],to(r.GetTop())-O[1],0],'max':[to(r.GetRight())-O[0],to(r.GetBottom())-O[1],height],'model':'manufacturer body envelope' if ref=='U10' else 'KiCad GLB top height; conservative occupied XY envelope'})
missing=remaining
x0,y0,x1,y1=cfg['battery']['rect'];gap=cfg['battery']['clear_gap_nominal_mm'];cell_h=cfg['battery']['thickness_mm']
battery={'min':[x0,y0,-.8-gap-cell_h],'max':[x1,y1,-.8-gap]}
collisions=[]
for r in parts:
    penetration=[min(r['max'][i],battery['max'][i])-max(r['min'][i],battery['min'][i]) for i in range(3)]
    if min(penetration)>0:collisions.append([r['ref'],penetration])
# Explicit front/rear enclosure planes, local antenna solids, not global gaps.
front_inner=3.4;rear_inner=front_inner+.8-16+.8
ants={}
for key,a in cfg['external_antennas'].items():
    x0,y0,x1,y1=a['rect_relative_pcb_mm'];t=a['thickness_mm'];z0=rear_inner if a['face']=='rear' else front_inner-t
    ants[key]={'min':[x0,y0,z0],'max':[x1,y1,z0+t]}
ants['ESP32']={'min':[3.85,-6.68,0],'max':[22.15,-.04,3.1]}
def distance(a,c):return math.sqrt(sum(max(a['min'][i]-c['max'][i],c['min'][i]-a['max'][i],0)**2 for i in range(3)))
report={'board_mm':[W,H,to(b.GetDesignSettings().GetBoardThickness())],'layers':b.GetCopperLayerCount(),'top':sum(not f.IsFlipped() for f in b.GetFootprints()),'bottom':sum(f.IsFlipped() for f in b.GetFootprints()),'unsupported_pad_boxes':unsupported,'model_envelopes':parts,'missing_or_unparsed_models':missing,'battery':battery,'body_battery_collisions':collisions,'highest_component_mm':max(r['max'][2] for r in parts),'deepest_lead_below_top_mm':min(r['min'][2] for r in parts),'front_inner_z_mm':front_inner,'rear_inner_z_mm':rear_inner,'enclosure_nominal_mm':[84.6,54,16],'cell_swelling_and_tolerance_not_qualified':True,'antennas':ants,'nearest_antenna_envelope_distances_mm':{'ESP32_to_battery':distance(ants['ESP32'],battery),'ESP32_to_LTE':distance(ants['ESP32'],ants['LTE']),'ESP32_to_GNSS':distance(ants['ESP32'],ants['GNSS']),'LTE_to_GNSS':distance(ants['LTE'],ants['GNSS'])},'RF_clearance_deviation':'ESP32 manufacturer15mm external clearance not met; enclosure/antenna RF testing required','bottom_barrier':'0.25mm continuous abrasion/electrical shield within1.75mm nominal gap, rigid independent PCB supports; reserve1mm unqualified swelling/tolerance'}
(ROOT/'reports/mechanical-revision.json').write_text(json.dumps(report,indent=2));print(json.dumps({k:v for k,v in report.items() if k!='model_envelopes'},indent=2))
assert report['bottom']==0 and not unsupported and not collisions
assert report['highest_component_mm']<=front_inner,'Front body collision'
