#!/usr/bin/python3
"""Join immutable JLC observations, manufacturer evidence and current native proof.
A classified supplier exception is not a resolved manufacturing defect or approval.
"""
import collections, hashlib, json, pathlib
import pcbnew as p
R=pathlib.Path(__file__).resolve().parents[3];A=R/'agent/companion';D=A/'reports/jlcpcb-dfm'
def load(name):return json.loads((D/name).read_text())
def key(raw):return json.dumps(raw,sort_keys=True)
inv=load('inventory.json');fab=load('fabrication-findings.json');smt=load('smt-bodies.json')
baseline=load('baseline.json');lands=load('land-repair.json');quant=load('land-quantization.json')
verification=json.loads((A/'reports/final-verification.json').read_text())
board_path=R/'kicad/companion/companion.kicad_pcb';board=p.LoadBoard(str(board_path));sha=hashlib.sha256(board_path.read_bytes()).hexdigest()
assert verification['inputs_sha256']['kicad/companion/companion.kicad_pcb']==sha,'Stale native verification'
smt_after=load('smt-bodies-after.json')
assert smt_after['source_board_sha256']==sha,'Stale current SMT extent measurements'
fab_map={}
for act in fab['reports'][0]['actions']:
 for layer in act['layers']:
  for record in layer['records']:fab_map[(act['name'],layer['layer'],key(record['source']))]=record
smt_map={key(row['raw']):row for category in ('c2c_size','comp2comp_supplied','comp2edge','comp_x_edge','pin_and_lead_supplied') for row in smt[category]}
smt_after_map={key(row['raw']):row for category in ('c2c_size','comp2comp_supplied','comp2edge','comp_x_edge','pin_and_lead_supplied') for row in smt_after[category]}
changes={(row['ref'],row['pad']):row for row in quant['changes']}
for row in inv['findings']:
 cat=row['category'];raw=row['raw'];row['severity']='information';row['proposed_repair']='None: preserve measured, electrically valid native geometry.'
 row['verification']={'native_report':'../final-verification.json','native_pcb_sha256':sha,'fresh_JLC_DFM_run':False}
 if row['action']!='jlc_analysis_smt':
  source=fab_map[(row['action'],row['layer'],key(raw))]
  objects=source.get('mapped_endpoint_objects') or [a[0] for a in source.get('nearest_objects',[]) if a]
  row['mapped_native_objects']=objects
  row['affected_refs']=sorted(set(row['affected_refs']+[a['ref'] for a in objects if a.get('ref')]))
  row['reason']=fab['category_dispositions'][cat];row['evidence']='fabrication-findings.json'
  if cat=='acute_line':
   row.update(disposition='repaired_native_geometry',severity='resolved_minor',proposed_repair='0.3mm-leg chamfer, unchanged 0.15mm B.Cu width; original 45deg vertex replaced by two 112.5deg turns.')
   row['verification']['repair_DRC']='repaired-drc.json'
  elif cat in ('via2pad','pth_ar','fills'):
   row.update(disposition='intentional_process_feature_pending_approval',severity='supplier_process_gate')
   row['proposed_repair']='Supplier must approve actual filled/planarized/copper-capped via map, small drills and annuli; preserve correct composite same-net drain lands.'
  elif cat=='neg_pad_ar':row['disposition']='likely_detector_model_false_positive'
  else:row['disposition']='within_applicable_capability'
  row['before_native_measurement']=source.get('native_object_clearance_mm',source.get('reported_distance_mm'))
 else:
  source=smt_map.get(key(raw))
  row['evidence']='smt-bodies.json'
  if source:
   row['before_native_measurement']={k:v for k,v in source.items() if k not in ('raw','disposition')}
   row['reason']=source['disposition']
  if cat=='c2c_size':
   if 'U10' in row['affected_refs']:
    row.update(disposition='unapproved_spacing_exception',severity='supplier_assembly_gate',proposed_repair='Obtain explicit assembler approval of each listed sub-3mm modem gap, or authorize a broader floorplan/package change. Do not apply the rejected 16-move layout that displaces supply bypass by about29mm.')
   else:row['disposition']='likely_detector_expansion_false_positive'
  elif cat=='comp2comp':
   if source['disposition'].startswith('GENUINE'):
    row.update(disposition='unapproved_spacing_exception',severity='supplier_assembly_gate',proposed_repair='Review measured body and outer lead/pad gaps against the stated package-specific recommendation; supplier must accept compact placement explicitly.')
   else:row['disposition']='measurement_only_no_violation_established'
  elif cat in ('comp2edge','comp_x_edge'):
   row.update(disposition='supplier_fixture_approval',severity='supplier_assembly_gate',proposed_repair='JLC-designed rails/fixture with local component, USB/SD mouth and antenna relief; all native solder lands remain supported. No unit-edge V-cut.')
  elif cat=='comp2hole':
   row.update(disposition='intentional_process_feature_pending_approval',severity='supplier_process_gate')
   row['reason']='Component-to-hole detector includes layer-transfer vias under SMT bodies; a via is not an inserted tall component. Confirm exact native via/pad overlap against the filled/capped map, not a blanket body-collision claim.'
   row['proposed_repair']='Review under-body vias, retained POFV and locating holes with CAM/assembly; no unsupported component placement inferred from a zero-length measurement.'
  elif cat=='weld2paste':
   ref=raw['name1'];pad=source.get('nearest_native_pad')
   if (ref,pad) in changes:
    row.update(disposition='repaired_manufacturer_land_geometry',severity='resolved_land_pattern',proposed_repair='Union with exact manufacturer land recommendation while preserving outward routed copper; dedicated manufacturer paste graphics instead of enlarged automatic paste.')
    row['after_native_land']=changes[(ref,pad)];row['verification']['land_report']='land-quantization.json'
   elif ref in ('QH2','QL2'):
    row['disposition']='intentional_composite_land_union';row['reason']='Drain terminals overlap the same-net large drain pad; evaluate union, not each duplicated pad separately.'
   else:raise RuntimeError('Unclassified lead overlap '+str(raw))
  elif cat.startswith('pin_'):
   row.update(disposition='supplier_package_model_review',severity='supplier_model_gate',proposed_repair='Review exact JLC library lead boundaries against selected manufacturer land pattern; do not scale copper to force generic maximum-terminal containment. Re-run DFM after the native land corrections.')
   ref=raw['name1'];pad=source.get('nearest_native_pad')
   if (ref,pad) in changes:row['after_native_land']=changes[(ref,pad)]
  else:raise RuntimeError('Unhandled SMT category '+cat)
for category in inv['categories']:
 selected=[r for r in inv['findings'] if (r['action'],r['layer'],r['category'])==(category['action'],category['layer'],category['category'])]
 category['classification_counts']=dict(collections.Counter(r['disposition'] for r in selected))
 category['severity_counts']=dict(collections.Counter(r['severity'] for r in selected))
 category['measurement_units']='fractional expansion factor' if category['category']=='c2c_size' else 'overlap fraction' if category['category']=='weld2paste' else 'mil for distance bins; raw shapes retained'
 category['omitted_records_disposition']='Unavailable coordinates: retain reported count, do not claim individually checked or repaired' if category['omitted_record_count'] else 'All supplied records classified'
inv['baseline_source_correspondence']=baseline
inv['source_board_sha256']=sha;inv['native_after_verification']=verification['companion']
inv['classification_counts']=dict(collections.Counter(r['disposition'] for r in inv['findings']))
inv['classification_complete_for_supplied_records']=all(r['disposition']!='pending_evidence' for r in inv['findings'])
inv['supplier_approval_obtained']=False
inv['before_after']={'baseline':baseline,'after':verification['companion'],'native_land_changes':len(changes),'component_positions_changed':False,'schematic_connectivity_changed':False,'global_rules_weakened':False,'before_FET_nominal_min_terminal_support':min(r['before_union_support_fraction'] for r in lands['lead_support_sensitivity'] if r['case']=='nominal' and r['ref'] in ('QH2','QL2')),'after_FET_nominal_min_terminal_support':min(r['after_union_support_fraction'] for r in quant['lead_support_sensitivity'] if r['case']=='nominal' and r['ref'] in ('QH2','QL2')),'note':'Fresh native checks and exported-pattern proof do not constitute a fresh JLC DFM pass.'}
spacing={}
for r in smt_after['c2c_size']+smt_after['comp2comp_supplied']:
 if r.get('recommendation_mm') is None or not r['disposition'].startswith('GENUINE'):continue
 pair=tuple(sorted((r['raw']['name1'],r['raw']['name2'])))
 spacing[pair]={'refs':list(pair),'nominal_body_gap_mm':r['native_nominal_occupied_envelope_gap_mm'],'manufacturer_max_gap_mm':r.get('manufacturer_max_gap_mm'),'outer_pad_lead_extent_gap_mm':r['native_outer_pad_lead_extent_gap_mm'],'recommended_mm':r['recommendation_mm'],'basis':r['recommendation_basis'],'approved':False}
exceptions={'native_pcb_sha256':sha,'spacing':list(spacing.values()),'edge_fixture':smt['comp2edge'],'intentional_overhangs':smt['comp_x_edge'],'package_model_review':{'pin_records_supplied':sum(r['category'].startswith('pin_') for r in inv['findings']),'R19':'See r19-package.json; local identity and package geometry do not establish supplier-side model assignment.'},'process_requirements':load('required-process.json'),'missing_source_measurements':sum(c['omitted_record_count'] for c in inv['categories']),'supplier_approval_obtained':False,'rejected_floorplan':smt['rejected_strict_spacing_candidate']['reason']}
(D/'inventory.json').write_text(json.dumps(inv,indent=2,ensure_ascii=False)+'\n')
(D/'approval-exceptions.json').write_text(json.dumps(exceptions,indent=2,ensure_ascii=False)+'\n')
print(json.dumps({'classified_supplied_records':len(inv['findings']),'classification_counts':inv['classification_counts'],'spacing_pairs_requiring_approval':len(spacing),'omitted_original_records':exceptions['missing_source_measurements'],'native_after':verification['companion']},indent=2))
pattern=load('pattern-verification.json');r19=load('r19-package.json')
assert pattern['board_sha256']==sha,'Stale emitted-pattern verification'
inv['emitted_pattern_verification']={'report':'pattern-verification.json','summary':pattern['summary'],'verdict':pattern['verdict'],'errors':pattern['errors']}
for row in inv['findings']:
 if row['action']=='jlc_analysis_smt' and key(row['raw']) in smt_after_map:
  current=smt_after_map[key(row['raw'])]
  row['after_native_measurement']={k:v for k,v in current.items() if k not in ('raw','disposition')}
  row['verification']['current_SMT_report']='smt-bodies-after.json'
inv['R19_package_status']=r19['status']
(D/'inventory.json').write_text(json.dumps(inv,indent=2,ensure_ascii=False)+'\n')
def number(value):return 'not established' if value is None else f'{value:.4f}'
lines=['# Rev B DFM before/after','',
 '**Engineering candidate; no fresh supplier DFM pass or assembly approval.**',
 '',f'Current native PCB SHA-256: `{sha}`.',
 f'Baseline PCB SHA-256: `{baseline["source_board_sha256"]}`.',
 '', '## Scope and correlation','',
 'JLC profile metadata uses inches; measurement shapes use mils. Native conversion is x = 50 + 0.0254x, y = 50 - 0.0254y mm. The 82mm profile and four asymmetric USB slots establish geometric correspondence; no upload checksum was supplied.',
 f'The reports contain {len(inv["findings"])} unique supplied records. Duplicate PCB observations are not counted twice. The reports omit {exceptions["missing_source_measurements"]} original measurement rows; unavailable coordinates are not claimed repaired.',
 '', '## Native before/after','',
 '| Check | Before | After |','|---|---:|---:|',
 f'| Copper DRC violations | {baseline["drc_violations"]} | {verification["companion"]["drc_errors"] + verification["companion"]["drc_warnings"]} |',
 f'| Unconnected items | {baseline["unconnected"]} | {verification["companion"]["unconnected_items"]} |',
 f'| Schematic parity findings | {baseline["parity"]} | {verification["companion"]["schematic_parity"]} |',
 f'| ERC errors | {baseline["erc_errors"]} | {verification["companion"]["erc_errors"]} |',
 f'| ERC warnings | {sum(baseline["erc_warning_categories"].values())} | {verification["companion"]["erc_warnings"]} |',
 f'| Minimum nominal FET source/gate terminal support | {inv["before_after"]["before_FET_nominal_min_terminal_support"]:.6%} | {inv["before_after"]["after_FET_nominal_min_terminal_support"]:.6%} |',
 '', f'The pre-existing zero-error DRC did not detect manufacturer land/stencil defects. Existing pad copper, pad identities, nets, component poses and global rule limits are preserved. The B.Cu MODE trace acute corner is chamfered; {len(changes)} lands are corrected against manufacturer recommendations, with separate paste geometry. Adjacent gate/VSYS/COMP routing follows the corrected copper. Stencil corrections are detailed in `stencil-repair.json`; emitted copper/paste are independently parsed in `pattern-verification.json`.',
 '', '## Every nonempty supplied category','',
 '| Action / layer / category | Reported rows | Supplied raw | Unique | Omitted | Disposition counts |',
 '|---|---:|---:|---:|---:|---|']
for c in inv['categories']:
 if not c['reported_count']:continue
 layer=c['layer'].replace('fabrication\\companion-','')
 dispositions='; '.join(f'{k}: {n}' for k,n in c['classification_counts'].items())
 lines.append(f'| {c["action"]} / {layer} / {c["category"]} | {c["reported_count"]} | {c["supplied_raw_count"]} | {c["supplied_unique_count"]} | {c["omitted_record_count"]} | {dispositions} |')
lines += ['', 'Threshold bands are retained verbatim in `inventory.json`, not treated as automatic failure limits. `weld2paste` means lead-to-land overlap, not stencil paste coverage. `c2c_size` is an expansion factor, not a millimetre clearance.',
 '', '## Independent emitted-pattern result','',f'Verdict: {pattern["verdict"]}.',
 '```json',json.dumps(pattern['summary'],indent=2),'```','',
 'See manufacturer URLs, drawing pages, aperture bounds, exact source hashes and unchanged stencil deviations in `pattern-verification.json`. Nominal support is not a tolerance, solderability or assembly acceptance certification.',
 '', '## R19 package binding','',f'Status: `{r19["status"]}`.',
 'JLC already parsed the exact C2117579 / MCS04020C5492FE000 SMT 0402 identity and placement. A local BOM string edit cannot establish supplier model assignment. No remote assignment, stock reservation or supplier approval occurred. `r19-package.json` records native/manufacturer dimensional evidence and the required closeout; `r19-supplier-request.txt` is ready for supplier review.',
 '', '## Required supplier closeout','',
 'Use `approval-exceptions.md` and `approval-exceptions.json` for exact gaps, fixture reliefs and process requirements. Re-run JLC DFM against this Rev B ZIP/BOM/CPL with 0.8mm JLC04081H-7628, ENIG and resin-filled/planarized/copper-capped via-in-pad—not the supplied reports’ 1.6mm/HASL/tented defaults. Obtain explicit R19 package binding, spacing, mask/stencil, POFV and panel/fixture approval before ordering.']
(D/'before-after.md').write_text('\n'.join(lines)+'\n')
lines=['# Rev B supplier approval exceptions','',
 '**All exceptions below are unapproved.** They are not hidden waivers or a claim of manufacturer acceptance.',
 '', '## Component spacing','',
 'All dimensions are mm. Nominal bodies, manufacturer-maximum envelopes and outer lead/pad extents are separate measurements. Missing maximum dimensions remain unestablished. The recommendation column uses the basis shown, not a universal collision threshold.',
 '', '| Pair | Nominal body gap | Maximum-body gap | Outer lead/pad gap | Recommendation | Basis |',
 '|---|---:|---:|---:|---:|---|']
for row in exceptions['spacing']:
 lines.append('| '+' / '.join(row['refs'])+' | '+' | '.join(number(row[k]) for k in ('nominal_body_gap_mm','manufacturer_max_gap_mm','outer_pad_lead_extent_gap_mm','recommended_mm'))+' | '+row['basis']+' |')
lines += ['', 'The compact modem arrangement does not meet Quectel’s 3mm soldering/maintenance spacing recommendation. The rejected strict-spacing candidate displaced a supply bypass by about29mm; no such electrically worse placement was applied. Approval or an explicitly broader floorplan/package redesign is required.',
 '', '## Edge and fixture relief','',
 '| Ref | Signed nominal body-to-edge gap mm | All solder lands supported | Disposition |',
 '|---|---:|---|---|']
for row in exceptions['edge_fixture']+exceptions['intentional_overhangs']:
 lines.append(f'| {row["raw"]["name1"]} | {number(row["body_nominal_edge_clearance_signed_mm"])} | {row["all_solder_land_bounding_boxes_supported"]} | {row["disposition"]} |')
lines += ['', 'Negative values denote intentional body/mouth/antenna overhang, not unsupported solder lands. JLC supplies the Standard-PCBA panel and fixture, process rails and fiducials with local antenna/connector relief; no panel Gerbers are supplied and unit edges must not be V-cut.',
 '', '## Process and package conditions','',
 '- Four layers, 82 × 38.5 × 0.8mm, JLC04081H-7628, ENIG. Exact construction is in `required-process.json`.',
 '- All retained via-in-pad sites require resin fill, planarization and plated copper cap. Ordinary tenting/plugging is not equivalent. Review 0.15mm drills and partial/off-centre overlaps against the exact via map and a four-layer quote.',
 '- Review actual fine-pitch mask dams and dedicated stencil apertures from the new Gerbers. Native clearance success is not supplier mask registration or print-process acceptance.',
 '- R19: supplier must assign and preview the exact C2117579 SMT 0402 model and clear the missing-package result against the new artifact hashes. No substitute part or speculative string change was made.',
 f'- {exceptions["package_model_review"]["pin_records_supplied"]} supplied pin-boundary records remain supplier-model review items; omitted source coordinates cannot be individually closed.',
 '- Confirm every BOM line’s PCBA stock allocation, orientation and assembly preview; catalog identity is not allocation.',
 '- Physical RF/USB, loaded thermal/transient performance, battery swelling/insulation and enclosure tolerances remain hardware qualification gates.',
 '', 'No order, payment, supplier request submission or supplier approval is implied by these files.']
(D/'approval-exceptions.md').write_text('\n'.join(lines)+'\n')
