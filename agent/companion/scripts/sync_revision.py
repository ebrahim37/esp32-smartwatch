#!/usr/bin/python3
"""Synchronize canonical net names without changing non-testpoint connectivity."""
import pathlib,xml.etree.ElementTree as E,collections,json
import pcbnew as p
from sexp_spans import parse,apply
ROOT=pathlib.Path(__file__).resolve().parents[1]
# Recovery pads are PCB artwork, not purchased or pick-and-place parts.
f=ROOT.parents[1] / 'kicad/companion/02_esp32.kicad_sch';text=f.read_text();root=parse(text);edits=[]
for s in root.children('symbol'):
    if any(x.items[1:3] in (['Reference','TP9'],['Reference','TP10']) for x in s.children('property')):
        for key in ('in_bom','in_pos_files'):
            n=s.child(key);edits.append((n.start,n.end,f'({key} no)'))
if edits:f.write_text(apply(text,edits))
new=E.parse(ROOT/'reports/revision-testpoints-netlist.xml');old=E.parse(ROOT/'reports/revision-baseline/netlist.xml')
def sets(root):return {frozenset((n.attrib['ref'],n.attrib['pin']) for n in net.findall('node') if not n.attrib['ref'].startswith('TP')) for net in root.findall('./nets/net')}-{frozenset()}
assert sets(new)==sets(old),'Functional connectivity changed; refusing synchronization'
nodes={(n.attrib['ref'],n.attrib['pin']):net.attrib['name'].replace('{slash}','/') for net in new.findall('./nets/net') for n in net.findall('node')}
b=p.LoadBoard(str(ROOT.parents[1] / 'kicad/companion/companion.kicad_pcb'));changes=[]
for fp in b.GetFootprints():
    ref=fp.GetReference()
    if ref in ('TP9','TP10'):
        fp.SetAttributes(p.FP_EXCLUDE_FROM_BOM|p.FP_EXCLUDE_FROM_POS_FILES)
        for name in ('MPN','Manufacturer','LCSC Part'):
            if not fp.HasField(name):
                field=p.PCB_FIELD(fp,p.FIELD_T_USER,name);field.SetText('');field.SetVisible(False);fp.Add(field)
    for pad in fp.Pads():
        key=(ref,pad.GetNumber())
        if key not in nodes:continue
        name=nodes[key];current=pad.GetNetname().replace('{slash}','/')
        if name==current:continue
        net=b.FindNet(name)
        if not net:
            net=p.NETINFO_ITEM(b,name);b.Add(net)
        changes.append((ref,pad.GetNumber(),pad.GetNetname(),name));pad.SetNet(net)
p.SaveBoard(str(ROOT.parents[1] / 'kicad/companion/companion.kicad_pcb'),b)
(ROOT/'reports/revision-parity-sync.json').write_text(json.dumps(dict(non_testpoint_connectivity_unchanged=True,schematic_nodes=len(nodes),canonical_net_changes=changes),indent=2))
print('Functional graph unchanged; canonical updates',changes)
