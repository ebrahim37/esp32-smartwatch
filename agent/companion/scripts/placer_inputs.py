#!/usr/bin/python3
"""Functional groups and independent mechanical intent for KiCadRoutingTools."""
import pathlib,json,collections,math,xml.etree.ElementTree as E
import pcbnew as p
ROOT=pathlib.Path(__file__).resolve().parents[1];cfg=json.loads((ROOT/'scripts/layout-revision.json').read_text());b=p.LoadBoard(str(ROOT.parents[1] / 'kicad/companion/companion.kicad_pcb'));fs={f.GetReference():f for f in b.GetFootprints()};xml=E.parse(ROOT/'reports/revision-testpoints-netlist.xml')
netrefs=collections.defaultdict(set);refnets=collections.defaultdict(set)
for net in xml.findall('./nets/net'):
    name=net.attrib['name']
    if name=='GND' or name.startswith('unconnected-') or name.endswith(('3V3','3V8','VSYS','VBAT','USB_5V')):continue
    for n in net.findall('node'):netrefs[name].add(n.attrib['ref']);refnets[n.attrib['ref']].add(name)
cores={'U1':'charger','U3':'fuel','U4':'esp_power','U5':'modem_power','U7':'esp','U8':'usb_uart','U9':'uart_gate','U11':'level_shift','U10':'modem','Card1':'sim','J_SD1':'sd','USBC1':'usb','J_LTE1':'lte_rf','J_GNSS1':'gnss_rf','U2':'battery'}
forced={'L1':'U1','L3':'U4','L4':'U5','QH2':'U5','QL2':'U5','C11':'U7','C12':'U7','C13':'U7','C39':'U10','C68':'U10','TP9':'U7','TP10':'U7','R43':'J_LTE1','C48':'J_LTE1','C49':'J_LTE1','R44':'J_GNSS1','C50':'J_GNSS1','C51':'J_GNSS1','C52':'J_GNSS1'}
groups={r:[] for r in cores}
for ref,fp in fs.items():
    if ref in cores:owner=ref
    elif ref in forced:owner=forced[ref]
    else:
        scores={r:sum(1 for n in refnets[ref] if r in netrefs[n]) for r in cores}
        best=max(scores.values())
        candidates=[r for r,v in scores.items() if v==best]
        owner=min(candidates,key=lambda r:math.hypot(p.ToMM(fp.GetX()-fs[r].GetX()),p.ToMM(fp.GetY()-fs[r].GetY())))
    groups[owner].append(ref)
out={'schema':1,'board':'companion.kicad_pcb','groups':[{'name':cores[r],'anchor':r,'role':'power' if r in ('U1','U4','U5') else 'interface' if r.startswith('J') or r in ('USBC1','Card1','U2') else 'core','side':'F','refs':sorted(refs),'why':'Functional local nets; shared rails do not group unrelated circuits'} for r,refs in groups.items()]}
(ROOT/'scripts/functional-groups.json').write_text(json.dumps(out,indent=2))
intent={'schema':1,'kind':'floorplan-intent','units':'mm','envelope':{'rect':[50,50,132,88.5],'tolerance_mm':.05},'assembly':{'sides':'F','why':'LiPo directly underneath; no bottom fitted components'},'blocks':[{'name':'all_top','refs':sorted(fs),'side':'F','why':'Outward-facing assembly only'}], 'keepouts':[{'name':'ESP32_antenna_only','rect':[53.85,43.32,72.15,49.96],'sides':['F','B'],'allow':['U7'],'why':'Real overhanging antenna; 15mm external clearance deviation documented, not silently imposed on entire battery'},{'name':'battery_projection','rect':[50,58.5,132,88.5],'sides':['B'],'why':'No battery-facing fitted components'}], 'edge_connectors':[{'ref':'U7','edge':'north','overhang_mm':{'min':6.3,'max':6.8},'side':'F','why':'Intentional supported-pad antenna overhang, not a mating connector'},{'ref':'USBC1','edge':'south','side':'F','mating_face':'+y','overhang_mm':{'min':0,'max':.3},'max_setback_mm':.2},{'ref':'J_SD1','edge':'south','side':'F','mating_face':'+y','overhang_mm':{'min':0,'max':1.3},'why':'Card geometry extends outside, solder pads supported'},{'ref':'U2','edge':'south','side':'F','mating_face':'+y','overhang_mm':{'min':0,'max':.3},'max_setback_mm':2}], 'must_lock':list(cfg['anchors']), 'legality_budget':{'overlap_area':0,'oob_count':0}, 'context':{'mechanical_source':'research/mechanical-revision.md','antenna_clearance_deviation':'ESP32/LTE projection overlap and local battery proximity require physical RF qualification'},'proximity':[{'ref':'C12','near':'U7','max_mm':3,'basis':'pad_edge','pads':{'near':['2']},'why':'Local ESP supply HF decoupling'},{'ref':'C13','near':'U7','max_mm':6,'basis':'pad_edge','pads':{'near':['3']},'why':'EN RC reset timing'}]}
for claim in intent['proximity']:
    claim['pads']={claim['near']:claim['pads']['near']}
(ROOT/'scripts/floorplan-revision.json').write_text(json.dumps(intent,indent=2));print('Groups',[(g['name'],len(g['refs'])) for g in out['groups']])
