#!/usr/bin/python3
"""Verify repaired companion lands against independently transcribed TI drawings.

Read-only native source; reads actual KiCad metric Gerber flashes AND filled
regions (including polygon paste graphics). Does not refill, save, DRC, or ERC.
Usage: /usr/bin/python3 verify_dfm_patterns.py --board BOARD --export-dir GERBERS
   or: /usr/bin/python3 verify_dfm_patterns.py --release-dir RELEASE
Requires pcbnew and shapely. Exits nonzero for missing repaired patterns, lost old
copper, deficient nominal FET support, or uncalibrated emitted pad geometry.
Unchanged stencil deviations and supplier process exceptions remain explicit
report findings, not silently converted into a blanket manufacturer approval.
"""
import argparse
import collections
import hashlib
import json
import math
import pathlib
import re
import pcbnew as p
from shapely import make_valid
from shapely.affinity import rotate, scale, translate
from shapely.geometry import Point, LineString, Polygon, box
from shapely.ops import unary_union

ROOT = pathlib.Path(__file__).resolve().parents[3]
MM = p.ToMM
CURVE_SEGMENTS = 128  # per quadrant: analytical aperture sagitta < 0.000012 mm for R=.6
GRID_TOL = 0.000002


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def xy(v):
    return [MM(v.x), MM(v.y)]


def native_poly(ps):
    out = []
    for i in range(ps.OutlineCount()):
        c = ps.COutline(i)
        holes = []
        for j in range(ps.HoleCount(i)):
            h = ps.CHole(i, j)
            holes.append([xy(h.CPoint(k)) for k in range(h.PointCount())])
        out.append(Polygon([xy(c.CPoint(k)) for k in range(c.PointCount())], holes))
    return unary_union(out)


def capsule(w, h):
    r = min(w, h) / 2
    if abs(w - h) < 1e-10:
        return Point(0, 0).buffer(r, quad_segs=CURVE_SEGMENTS)
    ends = [(-(w-h)/2, 0), ((w-h)/2, 0)] if w > h else [(0, -(h-w)/2), (0, (h-w)/2)]
    return LineString(ends).buffer(r, quad_segs=CURVE_SEGMENTS)


def roundrect(x, y, w, h, r=0):
    if not r:
        return box(x-w/2, y-h/2, x+w/2, y+h/2)
    return box(x-w/2+r, y-h/2+r, x+w/2-r, y+h/2-r).buffer(r, quad_segs=CURVE_SEGMENTS)


def to_native(g, origin):
    return translate(scale(g, xfact=1, yfact=-1, origin=(0, 0)), *origin)


def world(fp, g):
    return translate(rotate(g, -fp.GetOrientationDegrees(), origin=(0, 0)), *xy(fp.GetPosition()))


def local(fp, g):
    return rotate(translate(g, -MM(fp.GetX()), -MM(fp.GetY())), fp.GetOrientationDegrees(), origin=(0, 0))


def parse_gerber(path, origin):
    """Parse KiCad 4.6 metric aperture flashes/linear regions; fail on unknown modes.

    Native track/zone polygons are used for mask exposure, not Gerber strokes.
    Strokes are counted, explicitly excluded from returned flash/region objects.
    Region contours obey even-odd fill. X2 reference/pad/net and byte line ranges
    are retained so an independently plotted pad cannot be confused with a zone.
    """
    text = path.read_text()
    if '%FSLAX46Y46*%' not in text or '%MOMM*%' not in text:
        raise ValueError(f'Unsupported coordinate format/units: {path}')
    macros = dict(re.findall(r'%AM([^*]+)\*(.*?)%', text, re.S))
    aps = {}
    for n, kind, params in re.findall(r'%ADD(\d+)([^,*]+),([^*]+)\*%', text):
        vals = [float(x) for x in params.split('X')]
        if kind == 'R':
            g = roundrect(0, 0, *vals[:2])
        elif kind == 'C':
            g = Point(0, 0).buffer(vals[0]/2, quad_segs=CURVE_SEGMENTS) if vals[0] else Point(0, 0)
        elif kind == 'O':
            g = capsule(*vals[:2])
        elif kind == 'RoundRect':
            g = Polygon(list(zip(vals[1:9:2], vals[2:9:2]))).buffer(vals[0], quad_segs=CURVE_SEGMENTS)
        elif kind.startswith('FreePoly'):
            fields = macros[kind].split('*')[0].split(',')
            count = int(fields[2])
            nums = [float(x) for x in fields[3:3+2*count]]
            g = Polygon(list(zip(nums[::2], nums[1::2])))
            if vals[0]:
                g = rotate(g, vals[0], origin=(0, 0))
        else:
            raise ValueError(f'Unsupported aperture {kind} in {path}')
        aps[int(n)] = (g, kind, vals)
    objects = []
    ref = pad = net = None
    aperture = None
    dark = True
    in_region = False
    contours = []
    points = []
    x = y = 0.
    stroke_count = 0
    start = None
    for lineno, line in enumerate(text.splitlines(), 1):
        if line.startswith(('%LM', '%LR', '%LS', '%SR')):
            raise ValueError(f'Unsupported image transformation at {path}:{lineno}')
        if line.startswith(('G02', 'G03')):
            raise ValueError(f'Unsupported curved draw at {path}:{lineno}')
        if line == '%LPD*%':
            dark = True
        if line == '%LPC*%':
            dark = False
        if m := re.fullmatch(r'D(\d+)\*', line):
            aperture = int(m[1])
        if m := re.match(r'%TO\.P,([^,*]+),([^,*]+)', line):
            ref, pad = m.groups()
        if m := re.match(r'%TO\.C,([^*]+)', line):
            ref, pad = m[1], None
        if m := re.match(r'%TO\.N,([^*]+)', line):
            net = m[1]
        if line == '%TD*%':
            ref = pad = net = None
        if line == 'G36*':
            in_region, contours, points, start = True, [], [], lineno
        if line == 'G37*':
            if points:
                contours.append(points)
            g = Polygon()
            normalized = False
            for contour in contours:
                if len(contour) < 3:
                    raise ValueError(f'Degenerate contour at {path}:{start}')
                pg = Polygon(contour)
                if not pg.is_valid:
                    # KiCad copper pours use self-touching keyhole contours.
                    # GEOS linework normalization implements even-odd fill,
                    # without the material loss of arbitrary buffer(0).
                    pg = make_valid(pg)
                    normalized = True
                g = g.symmetric_difference(pg)
            objects.append(dict(ref=ref, pad=pad, net=net, kind='region', dark=dark,
                                line_range=[start, lineno], keyhole_normalized=normalized,
                                geometry=to_native(g, origin)))
            in_region = False
        if m := re.fullmatch(r'(?:X(-?\d+))?(?:Y(-?\d+))?D0([123])\*', line):
            if m[1] is not None:
                x = int(m[1])/1e6
            if m[2] is not None:
                y = int(m[2])/1e6
            operation = int(m[3])
            if in_region:
                if operation == 2 and points:
                    contours.append(points)
                    points = []
                if operation == 3:
                    raise ValueError('Flash inside a Gerber region')
                points.append((x, y))
            elif operation == 3:
                g, kind, vals = aps[aperture]
                objects.append(dict(ref=ref, pad=pad, net=net, kind='flash', dark=dark,
                                    aperture=aperture, aperture_kind=kind, aperture_parameters=vals,
                                    native_flash_position_mm=[origin[0]+x,origin[1]-y],
                                    line_range=[lineno, lineno], geometry=to_native(translate(g, x, y), origin)))
            elif operation == 1:
                stroke_count += 1
    if in_region:
        raise ValueError(f'Unterminated region in {path}')
    return dict(path=str(path), sha256=digest(path), objects=objects,
                flash_count=sum(x['kind']=='flash' for x in objects),
                region_count=sum(x['kind']=='region' for x in objects),
                native_exposure_uses_stored_tracks_instead_of_gerber_strokes=stroke_count)


def evidence(obj):
    return {k: v for k, v in obj.items() if k != 'geometry'}


def compose(rows):
    dark = unary_union([r['geometry'] for r in rows if r['dark']])
    return dark.difference(unary_union([r['geometry'] for r in rows if not r['dark']]))


def target_specs():
    # Derived from TI drawings, NOT read from repair report or repair script.
    # CSD16340Q3 p10: 3.500 outer copper span => bottom outer 1.750;
    # .630 land length => center1.435, .650 pitch and .500 width.
    # Stencil outer span3.320 => center1.660-.225=1.435; width.340.
    for ref in ('QH2', 'QL2'):
        for n in range(1, 5):
            x = (n-2.5)*.65
            yield ref, str(n), [x, 1.435, .5, .63, 0], [x, 1.435, .34, .45, 0]
    # BQ25606 RGE0024H pp44-45: 3.825 center-center, .5 pitch,
    # perimeter .58x.24, R.05 (same stencil sizes).
    for n in range(1, 25):
        if n <= 6:
            vals = [-3.825/2, -1.25+(n-1)*.5, .58, .24, .05]
        elif n <= 12:
            vals = [-1.25+(n-7)*.5, 3.825/2, .24, .58, .05]
        elif n <= 18:
            vals = [3.825/2, 1.25-(n-13)*.5, .58, .24, .05]
        else:
            vals = [1.25-(n-19)*.5, -3.825/2, .24, .58, .05]
        yield 'U1', str(n), vals, vals
    # All22 rectangular RPM0026A recommendations, independently transcribed.
    for n, vals in u5_rectangular_stencil_specs():
        yield 'U5', n, vals, vals


def describe_pad(fp, pad, layer):
    return dict(uuid=pad.m_Uuid.AsString(), ref=fp.GetReference(), pad=pad.GetNumber(),
                net=pad.GetNetname(), layer=layer, native_center_mm=xy(pad.GetPosition()),
                size_mm=xy(pad.GetSize()), shape=int(pad.GetShape()))


def exact_pad(pad):
    # Rectangular repairs and original pads must not use KiCad bounding boxes,
    # which are orientation-dependent and can truncate at1nm.
    if pad.GetShape() == p.PAD_SHAPE_RECT:
        w, h = xy(pad.GetSize())
        return translate(rotate(roundrect(0, 0, w, h), -pad.GetOrientationDegrees(), origin=(0, 0)), *xy(pad.GetPosition()))
    return native_poly(pad.GetEffectivePolygon(p.F_Cu))


def additional_stencil_specs():
    # Independent transcription; do not import repair generator constants.
    for ref in ('QH2', 'QL2'):
        for n in range(5, 9):
            yield ref, 'drain-'+str(n), [(6.5-n)*.65,-1.435,.34,.45,0]
        for x in (-.635,.635):
            yield ref, 'drain-window-'+str(x), [x,-.2625,1.02,1.445,0]
    for x in (-.694,.694):
        for y in (-.694,.694):
            yield 'U1', f'EP-{x}-{y}', [x,y,1.188,1.188,.05]
    for n,x in [('24',-.625),('25',0),('26',.625)]:
        yield 'U5', 'EP-'+n, [x,0,.375,1.425,.05]


def u5_rectangular_stencil_specs():
    for n in (2,3,4,5,6,14,15,16,17,18):
        sign=-1 if n<7 else 1
        y={2:-1,3:-.5,4:0,5:.5,6:1,14:1,15:.5,16:0,17:-.5,18:-1}[n]
        w=.6 if abs(y)==1 else .725
        yield str(n), [sign*(1.9 if w==.6 else 1.8375),y,w,.25,.05]
    for n,x in [(8,-1.125),(9,-.625),(10,-.125),(11,.625),(12,1.125)]:
        yield str(n), [x,1.6375,.25,.625,.05]
    for n,x in [(20,1.05),(21,.25),(22,-.25),(23,-1.05)]:
        yield str(n), [x,-1.6125 if n in (20,23) else -1.65,.25,.675 if n in (20,23) else .6,.05]
    for n,x in [(24,-.625),(25,0),(26,.625)]:
        yield str(n), [x,0,.375,1.425,.05]


def u5_nominal_leads():
    # RPM0026A p38 bottom view reflected to top/native local coordinates.
    # End caps are semicircles at inward ends, not rectangular bboxes.
    for n,vals in u5_rectangular_stencil_specs():
        num=int(n);x,y,w,h,_=vals
        if num in (24,25,26):
            yield n,roundrect(x,y,.375,1.425), 'rectangular EP'
        elif num in (2,3,4,5,6,14,15,16,17,18):
            length=.4 if num in (2,6,14,18) else .525
            sign=-1 if num<7 else 1
            inner=2-length
            g=unary_union([box(inner+.125,y-.125,2,y+.125),Point(inner+.125,y).buffer(.125,quad_segs=CURVE_SEGMENTS)])
            yield n,scale(g,xfact=sign,yfact=1,origin=(0,0)), 'semicircular inward toe'
        else:
            sign=-1 if num>=20 else 1
            length=.475 if num in (20,23) else .4 if num in (21,22) else .425
            inner=1.75-length
            g=unary_union([box(x-.125,inner+.125,x+.125,1.75),Point(x,inner+.125).buffer(.125,quad_segs=CURVE_SEGMENTS)])
            yield n,scale(g,xfact=1,yfact=sign,origin=(0,0)), 'semicircular inward toe'


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--board', type=pathlib.Path)
    ap.add_argument('--release-dir', type=pathlib.Path)
    ap.add_argument('--export-dir', type=pathlib.Path)
    ap.add_argument('--baseline-board', type=pathlib.Path, default=ROOT/'agent/companion/checkpoints/pre-dfm-land-repair.kicad_pcb')
    ap.add_argument('--output', type=pathlib.Path, default=ROOT/'agent/companion/reports/jlcpcb-dfm/pattern-verification.json')
    args = ap.parse_args()
    board_path = args.board or (args.release_dir/'project/companion.kicad_pcb' if args.release_dir else ROOT/'kicad/companion/companion.kicad_pcb')
    export_dir = args.export_dir or (args.release_dir/'fabrication' if args.release_dir else None)
    if export_dir is None:
        ap.error('--export-dir or --release-dir is required; this checker never exports implicitly')
    b = p.LoadBoard(str(board_path))
    old = p.LoadBoard(str(args.baseline_board))
    fps = {f.GetReference(): f for f in b.GetFootprints()}
    oldfps = {f.GetReference(): f for f in old.GetFootprints()}
    origin = xy(b.GetDesignSettings().GetAuxOrigin())
    layer_files = {'F.Cu': 'F_Cu.gtl', 'B.Cu': 'B_Cu.gbl', 'F.Paste': 'F_Paste.gtp',
                   'B.Paste': 'B_Paste.gbp', 'F.Mask': 'F_Mask.gts', 'B.Mask': 'B_Mask.gbs'}
    plotted = {}
    for layer, suffix in layer_files.items():
        matches = list(export_dir.glob('*-'+suffix))
        if len(matches) != 1:
            raise ValueError(f'Expected exactly one {suffix} in {export_dir}')
        plotted[layer] = parse_gerber(matches[0], origin)
    pads = []
    objects = []
    calibration = []
    unmatched = []
    for fp in b.GetFootprints():
        for pad in fp.Pads():
            for layer, layerid, maskid in [('F.Cu', p.F_Cu, p.F_Mask), ('B.Cu', p.B_Cu, p.B_Mask)]:
                if not pad.IsOnLayer(layerid):
                    continue
                d = describe_pad(fp, pad, layer)
                ng = native_poly(pad.GetEffectivePolygon(layerid))
                candidates = [r for r in plotted[layer]['objects'] if r['kind']=='flash' and r['ref']==fp.GetReference()
                              and r['pad']==pad.GetNumber() and math.dist(r['native_flash_position_mm'],xy(pad.GetPosition())) < .000002]
                if candidates:
                    obj = min(candidates, key=lambda r: r['geometry'].symmetric_difference(ng).area)
                    g = obj['geometry']
                    if fp.GetReference() in ('QH2', 'QL2', 'U1', 'U5', 'USBC1'):
                        calibration.append(dict(**d, emitted=evidence(obj), native_vs_plot_center_error_mm=math.dist(xy(pad.GetPosition()), obj['native_flash_position_mm']),
                                                native_polygon_vs_exact_aperture_difference_mm2=ng.symmetric_difference(g).area,
                                                local_emitted_bounds_mm=list(local(fp, g).bounds), footprint_rotation_deg=fp.GetOrientationDegrees(),
                                                native_rect_vs_emitted_difference_mm2=exact_pad(pad).symmetric_difference(g).area if pad.GetShape()==p.PAD_SHAPE_RECT else None))
                else:
                    g = ng
                    # NPTH/mechanical objects are not functional copper pads.
                    if pad.GetAttribute()!=p.PAD_ATTRIB_NPTH:
                        unmatched.append(d)
                row = dict(meta=d, geometry=g, native=pad, has_mask=pad.IsOnLayer(maskid))
                pads.append(row)
                objects.append(row)
    rows = []
    errors = []
    for ref, num, cuvals, pastevals in target_specs():
        fp = fps[ref]
        target = next(r for r in pads if r['meta']['ref']==ref and r['meta']['pad']==num and r['meta']['layer']=='F.Cu')
        oldpad = next(pd for pd in oldfps[ref].Pads() if pd.GetNumber()==num)
        oldg = exact_pad(oldpad)
        cu = target['geometry']
        expected_cu = world(fp, roundrect(*cuvals))
        expected_paste = world(fp, roundrect(*pastevals))
        parts = [r for r in plotted['F.Paste']['objects'] if r['ref']==ref and r['geometry'].intersection(expected_paste).area > expected_paste.area/2]
        paste = compose(parts)
        missing = expected_cu.difference(cu).area
        lost = oldg.difference(cu).area
        hd = expected_paste.hausdorff_distance(paste) if not paste.is_empty else None
        ok = (cu.buffer(GRID_TOL).covers(expected_cu) and cu.buffer(GRID_TOL).covers(oldg)
              and hd is not None and hd < .00026 and len(parts)==1 and not target['native'].IsOnLayer(p.F_Paste))
        if not ok:
            errors.append(f'{ref}.{num}: repaired pattern mismatch')
        rows.append(dict(ref=ref, pad=num, uuid=target['meta']['uuid'], footprint_rotation_deg=fp.GetOrientationDegrees(),
                         copper_recommendation_local_mm=cuvals, copper_emitted_local_bounds_mm=list(local(fp, cu).bounds),
                         copper_emitted_area_mm2=cu.area, manufacturer_copper_missing_mm2=missing,
                         old_copper_lost_mm2=lost, old_copper_emitted_support_fraction=cu.intersection(oldg).area/oldg.area,
                         paste_recommendation_local_mm=pastevals, paste_emitted_local_bounds_mm=list(local(fp, paste).bounds),
                         paste_emitted_area_mm2=paste.area, paste_recommendation_area_mm2=expected_paste.area,
                         paste_symmetric_difference_mm2=paste.symmetric_difference(expected_paste).area,
                         paste_boundary_max_deviation_mm=hd, paste_objects=[evidence(x) for x in parts],
                         enlarged_copper_automatic_paste_disabled=not target['native'].IsOnLayer(p.F_Paste), passes=ok))
    supports = []
    for ref in ('QH2', 'QL2', 'U1'):
        finalcu = unary_union([r['geometry'] for r in pads if r['meta']['ref']==ref and r['meta']['layer']=='F.Cu'])
        priorcu = unary_union([exact_pad(pd) for pd in oldfps[ref].Pads()])
        cases = [('nominal', 3.3, .34, .45), ('largest_terminal_smallest_body', 3.2, .4, .55), ('largest_terminal_largest_body', 3.4, .4, .55)] if ref!='U1' else [('nominal',4,.24,.38), ('largest_terminal_smallest_body',3.9,.3,.48), ('largest_terminal_largest_body',4.1,.3,.48)]
        for n in range(1, 9 if ref!='U1' else 25):
            for label, body, width, length in cases:
                off = body/2-length/2
                if ref!='U1':
                    x = (n-2.5)*.65 if n<=4 else (6.5-n)*.65
                    vals = [x, off if n<=4 else -off, width, length]
                elif n<=6:
                    vals = [-off, -1.25+(n-1)*.5, length, width]
                elif n<=12:
                    vals = [-1.25+(n-7)*.5, off, width, length]
                elif n<=18:
                    vals = [off, 1.25-(n-13)*.5, length, width]
                else:
                    vals = [1.25-(n-19)*.5, -off, width, length]
                lead = world(fps[ref], roundrect(*vals))
                before = lead.intersection(priorcu).area/lead.area
                after = lead.intersection(finalcu).area/lead.area
                supports.append(dict(ref=ref, pad=str(n), case=label, lead_local_mm=vals,
                                     before_support_fraction=before, emitted_support_fraction=after,
                                     emitted_missing_lead_area_mm2=lead.difference(finalcu).area))
                if ref!='U1' and n<=4 and label=='nominal' and after<1-1e-8:
                    errors.append(f'{ref}.{n}: nominal terminal not supported')
    # Independent exact aperture/native stored-copper intersections, all functional pads.
    for t in b.GetTracks():
        if isinstance(t, p.PCB_VIA):
            for side, lid in [('F',p.F_Cu),('B',p.B_Cu)]:
                meta = dict(uuid=t.m_Uuid.AsString(), kind='via', layer=side+'.Cu', net=t.GetNetname(),
                            native_center_mm=xy(t.GetPosition()), diameter_mm=MM(t.GetWidth(lid)), drill_mm=MM(t.GetDrillValue()),
                            filled=t.GetFillingMode()==p.FILLING_MODE_FILLED, capped=t.GetCappingMode()==p.CAPPING_MODE_CAPPED)
                objects.append(dict(meta=meta, geometry=Point(xy(t.GetPosition())).buffer(meta['diameter_mm']/2, quad_segs=CURVE_SEGMENTS), native=t))
        elif t.GetLayer() in (p.F_Cu,p.B_Cu):
            if isinstance(t, p.PCB_ARC):
                raise ValueError('Native curved track needs explicit exposure geometry support')
            objects.append(dict(meta=dict(uuid=t.m_Uuid.AsString(),kind='track', layer=b.GetLayerName(t.GetLayer()),net=t.GetNetname()),
                                geometry=LineString([xy(t.GetStart()),xy(t.GetEnd())]).buffer(MM(t.GetWidth())/2,quad_segs=CURVE_SEGMENTS),native=t))
    for z in b.Zones():
        if not z.GetIsRuleArea() and z.GetLayer() in (p.F_Cu,p.B_Cu):
            objects.append(dict(meta=dict(uuid=z.m_Uuid.AsString(),kind='stored_filled_zone',layer=b.GetLayerName(z.GetLayer()),net=z.GetNetname()),
                                geometry=native_poly(z.GetFilledPolysList(z.GetLayer())),native=z))
    coverage = []
    exposures = []
    mask_matches = []
    for side in ('F', 'B'):
        maskrows = plotted[side+'.Mask']['objects']
        union = compose(maskrows)
        for row in pads:
            d, g = row['meta'], row['geometry']
            if d['layer']!=side+'.Cu' or row['native'].GetAttribute()==p.PAD_ATTRIB_NPTH:
                continue
            missing = g.difference(union).area
            coverage.append(dict(**d,mask_layer_present=row['has_mask'],copper_area_mm2=g.area,
                                 unopened_copper_area_mm2=missing,unopened_fraction=missing/g.area))
        for m in maskrows:
            candidates = [r for r in pads if r['meta']['layer']==side+'.Cu' and r['meta']['ref']==m['ref']]
            if not candidates:
                continue
            pad = min(candidates,key=lambda r:r['geometry'].symmetric_difference(m['geometry']).area)
            pd, pg = pad['meta'], pad['geometry']
            mask_matches.append(dict(side=side,ref=pd['ref'],pad=pd['pad'],uuid=pd['uuid'],emitted=evidence(m),
                                     symmetric_difference_mm2=pg.symmetric_difference(m['geometry']).area,
                                     unopened_pad_area_mm2=pg.difference(m['geometry']).area))
            for obj in objects:
                d, g = obj['meta'],obj['geometry']
                if d['layer']!=side+'.Cu' or d['uuid']==pd['uuid'] or d['net']==pd['net']:
                    continue
                if not g.intersects(m['geometry']):
                    continue
                area = g.intersection(m['geometry']).area
                if area>1e-7:
                    exposures.append(dict(opening_pad=pd,opening=evidence(m),other=d,exposed_other_net_area_mm2=area,copper_gap_mm=pg.distance(g)))
    vip = []
    for row in objects:
        d = row['meta']
        if d.get('kind')!='via' or d['layer']!='F.Cu':
            continue
        hole = Point(d['native_center_mm']).buffer(d['drill_mm']/2,quad_segs=CURVE_SEGMENTS)
        hits = [dict(**r['meta'],drill_land_overlap_mm2=hole.intersection(r['geometry']).area) for r in pads
                if r['meta']['layer']=='F.Cu' and r['native'].GetAttribute()==p.PAD_ATTRIB_SMD
                and hole.intersection(r['geometry']).area>1e-8]
        if hits:
            vip.append(dict(**d,lands=hits))
    old_findings = json.loads((ROOT/'agent/companion/reports/jlcpcb-dfm/fabrication-findings.json').read_text())
    neg_uuids = {r['uuid'] for r in old_findings.get('negative_mask_aperture_evidence',{}).get('records',[])}
    neg_now = [r for r in mask_matches if r['uuid'] in neg_uuids]
    stencil_deviations = []
    # Retained FET drain apertures and U1 EP were deliberately not repaired.
    for ref in ('QH2','QL2'):
        expected = []
        for n in range(5,9):
            expected.append(world(fps[ref], roundrect((6.5-n)*.65,-1.435,.34,.45)))
        for x in (-.635,.635):
            expected.append(world(fps[ref],roundrect(x,-.2625,1.02,1.445)))
        actual = compose([r for r in plotted['F.Paste']['objects'] if r['ref']==ref])
        source = unary_union([world(fps[ref],roundrect((n-2.5)*.65,1.435,.34,.45)) for n in range(1,5)])
        expected_union=unary_union(expected+[source])
        baseline_paste=unary_union([exact_pad(pd) for pd in oldfps[ref].Pads() if pd.IsOnLayer(p.F_Paste)])
        stencil_deviations.append(dict(ref=ref,description='TI p10 full pattern:8row apertures .34x.45 and two1.02x1.445 windows at(+/-.635,-.2625).',
                                      before_original_full_paste_area_mm2=baseline_paste.area,
                                      actual_total_paste_area_mm2=actual.area,recommended_total_paste_area_mm2=expected_union.area,
                                      excess_mm2=actual.difference(expected_union).area,missing_mm2=expected_union.difference(actual).area,
                                      actual_objects=[dict(**evidence(r),local_bounds_mm=list(local(fps[ref],r['geometry']).bounds)) for r in plotted['F.Paste']['objects'] if r['ref']==ref]))
    u1ep=[r for r in plotted['F.Paste']['objects'] if r['ref']=='U1' and local(fps['U1'],r['geometry']).within(box(-1.36,-1.36,1.36,1.36))]
    ep=compose(u1ep)
    recommended=unary_union([world(fps['U1'],roundrect(x,y,1.188,1.188,.05)) for x in (-.694,.694) for y in (-.694,.694)])
    stencil_deviations.append(dict(ref='U1',description='TI four1.188x1.188 R.05 windows centered(+/-.694,+/-.694), .125mm stencil example.',
                                  before_original_EP_paste_area_mm2=7.29,
                                  actual_paste_area_mm2=ep.area,recommended_paste_area_mm2=recommended.area,
                                  excess_mm2=ep.difference(recommended).area,missing_mm2=recommended.difference(ep).area,actual_objects=[evidence(r) for r in u1ep]))
    additional=[]
    for ref,label,vals in additional_stencil_specs():
        expected=world(fps[ref],roundrect(*vals))
        parts=[r for r in plotted['F.Paste']['objects'] if r['ref']==ref and r['geometry'].intersection(expected).area>expected.area/2]
        actual=compose(parts)
        hd=expected.hausdorff_distance(actual) if not actual.is_empty else None
        ok=bool(len(parts)==1 and hd is not None and hd<.00026)
        additional.append(dict(ref=ref,label=label,recommendation_local_mm=vals,
                               actual_objects=[evidence(r) for r in parts],actual_area_mm2=actual.area,
                               recommendation_area_mm2=expected.area,paste_boundary_max_deviation_mm=hd,
                               paste_symmetric_difference_mm2=expected.symmetric_difference(actual).area,passes=ok))
        if not ok:
            errors.append(f'{ref} {label}: additional manufacturer stencil mismatch')
    u5perimeter=[]
    for num,vals in u5_rectangular_stencil_specs():
        expected=world(fps['U5'],roundrect(*vals))
        parts=[r for r in plotted['F.Paste']['objects'] if r['ref']=='U5' and r['geometry'].intersection(expected).area>expected.area/2]
        actual=compose(parts)
        center=local(fps['U5'],actual).centroid
        u5perimeter.append(dict(pad=num,recommendation_local_mm=vals,actual_objects=[evidence(r) for r in parts],
                                actual_local_bounds_mm=list(local(fps['U5'],actual).bounds),
                                aperture_center_offset_mm=[center.x-vals[0],center.y-vals[1]],
                                symmetric_difference_mm2=actual.symmetric_difference(expected).area))
    u5support=[]
    u5cu=unary_union([r['geometry'] for r in pads if r['meta']['ref']=='U5' and r['meta']['layer']=='F.Cu'])
    recommended_u5=world(fps['U5'],unary_union([roundrect(*vals) for num,vals in u5_rectangular_stencil_specs()]))
    for num,lead,model in u5_nominal_leads():
        ng=world(fps['U5'],lead)
        support=ng.intersection(u5cu).area/ng.area
        vendor_support=ng.intersection(recommended_u5).area/ng.area if 'envelope' not in model else None
        u5support.append(dict(pad=num,terminal_model=model,nominal_lead_local_bounds_mm=list(lead.bounds),
                              emitted_copper_support_fraction=support,manufacturer_recommended_copper_support_fraction=vendor_support,
                              missing_terminal_area_mm2=ng.difference(u5cu).area))
        if int(num) not in (24,25,26) and support<1-1e-7:
            errors.append(f'U5.{num}: measured nominal rectangular/rounded-tip terminal support below100%')
    u5custom=[dict(pad=r['meta']['pad'],native_anchor_local_mm=list(local(fps['U5'],Point(r['meta']['native_center_mm'])).coords)[0],
                   emitted_local_bounds_mm=list(local(fps['U5'],r['geometry']).bounds),emitted_area_mm2=r['geometry'].area,
                   disposition='Retained L geometry, including library polygon/stroke differences. Exact nominal contact-area model is not established for corner-relief L terminals; no unsupported-lead or100%compliance claim from a bounding envelope.')
              for r in pads if r['meta']['ref']=='U5' and r['meta']['pad'] in ('1','7','13','19') and r['meta']['layer']=='F.Cu']
    if unmatched:
        errors.append('Functional copper pad(s) could not be calibrated to emitted flashes')
    badcal=[r for r in calibration if r['native_rect_vs_emitted_difference_mm2'] is not None and r['native_rect_vs_emitted_difference_mm2']>1e-5]
    if badcal:
        errors.append('Native/plot rectangular geometry calibration mismatch')
    report=dict(scope='Independent manufacturer drawing transcription and actual emitted repair copper/paste; fresh native/mask intersections, not DRC/ERC or supplier approval.',
                board=str(board_path),board_sha256=digest(board_path),baseline_board=str(args.baseline_board),baseline_sha256=digest(args.baseline_board),
                manufacturer_sources={
                    'QH2_QL2':dict(url='https://www.ti.com/lit/ds/symlink/csd16340q3.pdf',pages=[9,10],revision='SLPS247E Aug2014',derivation='Copper outer3.5/2-.63/2=1.435; stencil outer3.32/2-.45/2=1.435; pitch.65, copperwidth.5, stencilwidth.34.'),
                    'U1':dict(url='https://www.ti.com/lit/ds/symlink/bq25606.pdf',pages=[43,44,45],drawing='RGE0024H 4219016/A Aug2017',derivation='Perimeter center3.825/2=1.9125; dimensions.58x.24; R.05; pitch.5.'),
                    'U5':dict(url='https://www.ti.com/lit/ds/symlink/tps552882.pdf',pages=[38,39,40],drawing='RPM0026A 4224618/A Oct2018',derivation='20/23 centers x+/-1.05,y-1.6125 (=1.95-.675/2); .25x.675;R.05. Separate1.6375callout is bottom.625row, NOT top20/23.' )},
                input_reports_read={name:digest(ROOT/'agent/companion/reports/jlcpcb-dfm'/name) for name in ('land-repair.json','land-quantization.json')},
                emitted_files={k:{x:y for x,y in v.items() if x!='objects'} for k,v in plotted.items()},
                transforms=dict(aux_origin_native_mm=origin,footprint_local_to_native='rotate by minus footprint angle, then translate footprint center',
                                gerber_to_native='(aux_x+plot_x,aux_y-plot_y)',calibration=calibration,unmatched_functional_pads=unmatched,
                                asymmetric_guard='USBC1 slotted shell pads and complete labeled pad arrangement; QH2 180deg and QL2 0deg sources/gates checked with ref+pad-specific X2 attributes, not symmetric bbox-only registration.'),
                repaired_patterns=rows,lead_support=supports,
                lead_support_caveat='Independent body/width/length sensitivity extrema, not simultaneous tolerance guarantee. 100% applies only to explicitly measured nominal leads; largest terminals need not be100% supported by TI recommended lands.',
                mask=dict(functional_pad_coverage=coverage,functional_pad_coverage_deficits=[r for r in coverage if r['unopened_copper_area_mm2']>1e-5],
                          cross_net_exposures=exposures,aperture_matches=mask_matches,
                          old_negative_mask_records_current_matches=neg_now,old_negative_mask_unique_uuid_count=len(neg_uuids),
                          old_report_sha256=digest(ROOT/'agent/companion/reports/jlcpcb-dfm/fabrication-findings.json')),
                via_in_pad=vip,via_annulus_census=[dict(diameter_mm=k[0],drill_mm=k[1],ring_mm=(k[0]-k[1])/2,count=v) for k,v in sorted(collections.Counter((r['meta']['diameter_mm'],r['meta']['drill_mm']) for r in objects if r['meta'].get('kind')=='via' and r['meta']['layer']=='F.Cu').items())],
                full_stencil_area_comparisons=stencil_deviations,additional_stencil_patterns=additional,
                U5_remaining_stencil_apertures=u5perimeter,U5_nominal_lead_support=u5support,U5_retained_L_geometry=u5custom,
                U5_remaining_stencil_max_axis_center_offset_mm=max(abs(v) for r in u5perimeter for v in r['aperture_center_offset_mm']),
                precision=dict(gerber_coordinate_quantum_mm=.000001,containment_tolerance_mm=GRID_TOL,paste_boundary_tolerance_mm=.00026,
                               aperture_curves_segments_per_circle=4*CURVE_SEGMENTS,manufacturer_paste_rounding='Emitted polygon chords approximate R.05 with32 segments/circle; maximum inward sagitta~.000241mm, not exact circular fillet.',
                               exposure_limit='Native tracks/vias/stored filled zones, actual plotted pad and mask aperture geometry; manufacturing registration tolerance not modeled.'),
                summary=dict(intended_repairs=len(rows),original_intended_repairs=34,extended_U5_rectangular_repairs=20,proven_repairs=sum(r['passes'] for r in rows),dedicated_paste_regions=sum(r['kind']=='region' for x in rows for r in x['paste_objects']),
                             intended_additional_stencil_apertures=len(additional),proven_additional_stencil_apertures=sum(r['passes'] for r in additional),
                             functional_pads_checked=len(coverage),mask_deficits=sum(r['unopened_copper_area_mm2']>1e-5 for r in coverage),
                             cross_net_mask_exposures=len(exposures),via_in_pad_count=len(vip),all_vip_filled_capped=all(r['filled'] and r['capped'] for r in vip),
                             source_gate_nominal_before_range=[min(r['before_support_fraction'] for r in supports if r['ref']!='U1' and int(r['pad'])<=4 and r['case']=='nominal'),max(r['before_support_fraction'] for r in supports if r['ref']!='U1' and int(r['pad'])<=4 and r['case']=='nominal')],
                             source_gate_nominal_after_range=[min(r['emitted_support_fraction'] for r in supports if r['ref']!='U1' and int(r['pad'])<=4 and r['case']=='nominal'),max(r['emitted_support_fraction'] for r in supports if r['ref']!='U1' and int(r['pad'])<=4 and r['case']=='nominal')]),
                errors=errors,verdict=f'PASS{len(rows)} repaired copper/paste and{len(additional)} additional stencil apertures; four retained U5 L-lands and supplier exceptions explicitly reported, NOT blanket26-pad TI/supplier approval.' if not errors else 'FAIL: '+ '; '.join(errors))
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(dict(output=str(args.output),summary=report['summary'],errors=errors),indent=2))
    return bool(errors)


if __name__ == '__main__':
    raise SystemExit(main())
