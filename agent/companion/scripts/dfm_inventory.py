#!/usr/bin/python3
"""Parse every supplied JLC record without conflating threshold bands with failures.
JLC exports inch metadata, mil measurement geometry, and sometimes truncated lists.
The PCB is the source of native identities; supplied measurements remain immutable.
"""
import collections, hashlib, json, math, pathlib
import pcbnew as p
from shapely.geometry import Point, LineString, Polygon
R = pathlib.Path(__file__).resolve().parents[3]
A = R / 'agent/companion'
D = A / 'reports/jlcpcb-dfm'
D.mkdir(parents=True, exist_ok=True)
paths = [A / f'dfm{i}.json' for i in (1, 2)]
docs = [json.loads(f.read_text()) for f in paths]
board_path = R / 'kicad/companion/companion.kicad_pcb'
b = p.LoadBoard(str(board_path))
ox, oy = (p.ToMM(v) for v in (b.GetDesignSettings().GetAuxOrigin().x, b.GetDesignSettings().GetAuxOrigin().y))
def digest(f): return hashlib.sha256(f.read_bytes()).hexdigest()
def xy(x, y): return [ox + x * .0254, oy - y * .0254]
def geometry(s):
    if not s: return None, []
    if s[0] == 'R':
        _, x, y, w, h, angle = s
        return None, [xy(x, y)]  # Width/height remain raw; rotation is not a line endpoint.
    if len(s) >= 5 and s[2] == 'L':
        pts = [xy(s[0], s[1]), xy(s[3], s[4])]
        return math.dist(*pts), pts
    return None, []
# Correlate the asymmetric real USB-C slots, not a guessed visual translation.
slots = []
for f in b.GetFootprints():
    for pad in f.Pads():
        drill = pad.GetDrillSize()
        if drill.x and drill.x != drill.y:
            slots.append({'ref': f.GetReference(), 'pad': pad.GetNumber(), 'native_mm': [p.ToMM(pad.GetPosition().x), p.ToMM(pad.GetPosition().y)], 'drill_mm': [p.ToMM(drill.x), p.ToMM(drill.y)]})
anchors = []
for a in docs[1]['actions']:
    if a['name'] != 'jlc_analysis_drill': continue
    for result in a.get('results') or []:
        for m in result.get('meas') or []:
            if a['categories'][m['cat_num']]['name'] != 'slot_width': continue
            length, pts = geometry(m['meas_shape'])
            center = [(pts[0][i] + pts[1][i]) / 2 for i in (0, 1)]
            slot = min(slots, key=lambda s: math.dist(center, s['native_mm']))
            anchors.append({'measurement': m, 'center_native_mm': center, 'native_slot': slot, 'center_residual_mm': math.dist(center, slot['native_mm']), 'width_residual_mm': abs(length - min(slot['drill_mm']))})
assert len(anchors) == 4 and max(a['center_residual_mm'] for a in anchors) < .001, anchors
profile = docs[1]['check']['profile']['data']
profile_points = [(row[-2] * 25.4, row[-1] * -25.4) for row in profile]
profile_size = [max(q[i] for q in profile_points)-min(q[i] for q in profile_points) for i in (0,1)]
assert all(abs(a-c)<.001 for a,c in zip(profile_size, [82,38.5]))
seen = {}; categories = {}; duplicates = 0
for path, doc in zip(paths, docs):
    for action in doc['actions']:
        for result in action.get('results') or []:
            totals = {r['cat_num']:r['orig_num'] for r in result.get('truncate') or []}
            supplied = collections.Counter(m['cat_num'] for m in result.get('meas') or [])
            for ci, cat in enumerate(action['categories']):
                key = (action['name'], result['layer'], cat['name'])
                entry = categories.setdefault(key, {'action':key[0], 'layer':key[1], 'category':key[2], 'title':cat.get('language',{}).get('title',{}).get('en',cat['title']), 'threshold_bands_raw':cat.get('range'), 'reported_count':totals.get(ci, supplied[ci]), 'supplied_unique_count':supplied[ci], 'omitted_record_count':totals.get(ci,supplied[ci])-supplied[ci], 'sources':[]})
                entry['sources'].append(path.name)
            for m in result.get('meas') or []:
                cat = action['categories'][m['cat_num']]
                key = json.dumps([action['name'],result['layer'],cat['name'],m],sort_keys=True)
                if key in seen:
                    if path.name not in seen[key]['sources']:
                        seen[key]['sources'].append(path.name)
                    duplicates += 1
                    continue
                length, points = geometry(m.get('meas_shape'))
                refs = [n for n in (m.get('name1'),m.get('name2')) if b.FindFootprintByReference(n or '')]
                seen[key] = {'id':f'DFM-{len(seen)+1:04d}', 'action':action['name'], 'category':cat['name'], 'layer':result['layer'], 'sources':[path.name], 'raw':m, 'threshold_bands_raw':cat.get('range'), 'line_measurement_mm':length, 'native_points_mm':points, 'affected_refs':refs, 'disposition':'pending_evidence', 'severity':'unclassified', 'proposed_repair':None, 'verification':None}
rows = list(seen.values())
unique_counts = collections.Counter((r['action'], r['layer'], r['category']) for r in rows)
for key, category in categories.items():
    category['supplied_raw_count'] = category['supplied_unique_count']
    category['supplied_unique_count'] = unique_counts[key]
# Separate artifact identity from geometric correspondence: report contains no Gerber digest.
prior = A / 'archive/release-candidate/rev-a/project/companion.kicad_pcb'
result = {'source_reports':{str(f.relative_to(R)):digest(f) for f in paths}, 'source_board_sha256':digest(board_path), 'prior_candidate_board_sha256':digest(prior), 'source_and_prior_candidate_byte_identical':digest(board_path)==digest(prior), 'coordinate_transform':{'measurement_units':'mil', 'metadata_profile_units':'inch', 'native_x':'aux_x + 0.0254 * dfm_x', 'native_y':'aux_y - 0.0254 * dfm_y', 'aux_origin_native_mm':[ox,oy], 'profile_dimensions_mm':profile_size, 'asymmetric_slot_anchors':anchors}, 'correspondence_limit':'No upload Gerber checksum is included in JLC JSON; geometry and drill distribution can establish correspondence, not cryptographic upload identity.', 'identical_pcb_actions':docs[0]['actions'][:4]==docs[1]['actions'][:4], 'duplicate_records_removed':duplicates, 'unique_supplied_records':len(rows), 'categories':list(categories.values()), 'process_mismatches':{'reported_thickness_mm':docs[1]['check']['board_base_info']['board_thickness'], 'required_thickness_mm':.8, 'reported_finish':docs[1]['check']['pcb_quote']['adornPut'], 'required_finish':'ENIG', 'reported_via_covering':docs[1]['check']['pcb_quote']['adornBestrow'], 'required_via_covering':'Resin filled, planarized and copper capped for all retained via-in-pad; not ordinary tenting', 'reported_stackup':docs[1]['check']['pcb_quote']['cascadeStructure'], 'required_stackup':'JLC04081H-7628'}, 'unmatched_assembly_packages':docs[1]['check'].get('no_package',[]), 'findings':rows}
(D/'inventory.json').write_text(json.dumps(result,indent=2,ensure_ascii=False)+'\n')
print(json.dumps({k:v for k,v in result.items() if k not in ('findings','categories','coordinate_transform')},indent=2,ensure_ascii=False))
print('Category counts:', [(r['category'],r['layer'],r['reported_count'],r['supplied_unique_count']) for r in result['categories'] if r['reported_count']])
