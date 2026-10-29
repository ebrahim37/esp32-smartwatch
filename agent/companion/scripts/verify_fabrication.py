#!/usr/bin/python3
"""Independently parse exported Gerber/Excellon/CPL and compare native geometry.
Proves shared aux-origin alignment, every hole/slot centre and size, every fitted
part placement/angle, and unit outline. Does not claim supplier DFM acceptance.
"""
import pathlib,sys,re,json,csv,math
import pcbnew as p
OUT=pathlib.Path(sys.argv[1]).resolve();b=p.LoadBoard(str(OUT/'project/companion.kicad_pcb'));to=p.ToMM;origin=b.GetDesignSettings().GetAuxOrigin();ox,oy=to(origin.x),to(origin.y)
def xy(q):return(to(q.x)-ox,oy-to(q.y))
edge=next((OUT/'fabrication').glob('*Edge_Cuts*'));text=edge.read_text();fmt=re.search(r'%FS[LT]A?X\d(\d)Y\d\d\*%',text);scale=10**int(fmt.group(1));points=[(int(x)/scale,int(y)/scale) for x,y in re.findall(r'(?:^|\n)X(-?\d+)Y(-?\d+)',text)];bounds=[min(x for x,y in points),min(y for x,y in points),max(x for x,y in points),max(y for x,y in points)]
assert all(abs(a-c)<.001 for a,c in zip(bounds,[0,-38.5,82,0])),('Gerber must share auxiliary placement origin, not absolute PCB coordinates',bounds)
expected={'PTH':[],'NPTH':[]}
for v in b.GetTracks():
    if isinstance(v,p.PCB_VIA):expected['PTH'].append((*xy(v.GetPosition()),to(v.GetDrillValue()),to(v.GetDrillValue())))
for f in b.GetFootprints():
    for a in f.Pads():
        drill=a.GetDrillSize()
        if not drill.x:continue
        kind='NPTH' if a.GetAttribute()==p.PAD_ATTRIB_NPTH else 'PTH';expected[kind].append((*xy(a.GetPosition()),min(to(drill.x),to(drill.y)),max(to(drill.x),to(drill.y))))
counts={}
for kind in ['PTH','NPTH']:
    file=OUT/'fabrication'/('companion-'+kind+'.drl');tools={};tool=None;actual=[]
    for line in file.read_text().splitlines():
        match=re.match(r'T(\d+)C([\d.]+)',line)
        if match:tools[match.group(1)]=float(match.group(2));continue
        match=re.fullmatch(r'T(\d+)',line)
        if match:tool=match.group(1);continue
        coords=[(float(x),float(y)) for x,y in re.findall(r'X(-?[\d.]+)Y(-?[\d.]+)',line)]
        if not coords:continue
        assert tool in tools
        if 'G85' in line:
            assert len(coords)==2;start,end=coords;center=((start[0]+end[0])/2,(start[1]+end[1])/2);major=math.dist(start,end)+tools[tool]
        else:assert len(coords)==1;center=coords[0];major=tools[tool]
        actual.append((*center,tools[tool],major))
    assert len(actual)==len(expected[kind]),(kind,len(actual),len(expected[kind]))
    remaining=expected[kind].copy()
    for row in actual:
        matches=[i for i,target in enumerate(remaining) if math.dist(row[:2],target[:2])<.0008 and abs(row[2]-target[2])<.00051 and abs(row[3]-target[3])<.0008]
        assert len(matches)==1,(kind,'drill/slot mismatch or duplicate',row,matches)
        remaining.pop(matches[0])
    assert not remaining;counts[kind]=len(actual)
with (OUT/'cpl-jlcpcb.csv').open() as source:rows=list(csv.DictReader(source))
expected_parts={f.GetReference():f for f in b.GetFootprints() if not f.IsDNP() and not f.GetReference().startswith('TP')}
assert len(rows)==len(expected_parts) and {r['Designator'] for r in rows}==set(expected_parts)
for row in rows:
    f=expected_parts[row['Designator']];actual=(float(row['Mid X'].removesuffix('mm')),float(row['Mid Y'].removesuffix('mm')))
    assert math.dist(actual,xy(f.GetPosition()))<.00001,(row,xy(f.GetPosition()))
    angle=float(row['Rotation']);assert abs((angle-f.GetOrientationDegrees()+180)%360-180)<.001
    assert row['Layer']==('Bottom' if f.IsFlipped() else 'Top')
result=dict(Gerber_bounds_relative_aux_origin_mm=bounds,aux_origin_native_mm=[ox,oy],drill_features=counts,native_vias=sum(isinstance(v,p.PCB_VIA) for v in b.GetTracks()),fitted_placements_matched=len(rows),drill_xy_tolerance_mm=.0008,all_hole_centres_sizes_and_slots_matched=True,gerber_drill_and_CPL_share_origin=True,supplier_DFM_approval=False)
(OUT/'coordinate-verification.json').write_text(json.dumps(result,indent=2));print(json.dumps(result,indent=2))
