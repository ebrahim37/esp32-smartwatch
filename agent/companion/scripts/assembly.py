#!/usr/bin/python3
"""Export sourced BOM plus JLCPCB CPL without inventing manufacturer identities.
Catalog identity/assembly classification from55 live JLC records; stock is not
reserved. Test points and original DNP RF shunts are NOT purchasable components.
"""
import csv,json,pathlib,xml.etree.ElementTree as ET,collections,subprocess
import pcbnew as p
ROOT=pathlib.Path(__file__).resolve().parents[1];OUT=ROOT / 'archive/release-candidate/rev-b';OUT.mkdir(parents=True,exist_ok=True)
project=ROOT.parents[1]/'kicad/companion';netlist=ROOT/'reports/assembly-netlist.xml'
subprocess.run(['kicad-cli','sch','export','netlist','--format','kicadxml','-o',str(netlist),str(project/'companion.kicad_sch')],check=True)
b=p.LoadBoard(str(project/'companion.kicad_pcb'));catalog=json.loads((ROOT/'research/catalog.json').read_text());net=ET.parse(netlist).getroot();fs={f.GetReference():f for f in b.GetFootprints()};cfg=json.loads((ROOT/'scripts/layout-revision.json').read_text());origin=cfg['origin']
rows=[];anomalies=[]
for c in net.findall('./components/comp'):
    ref=c.attrib['ref'];fields={a.attrib['name']:a.text or '' for a in c.findall('./fields/field')};f=fs[ref];code=fields.get('LCSC Part','');cat=catalog.get(code,{})
    dnp=f.IsDNP();test=ref.startswith('TP');status='DNP - RF tuning reserve' if dnp else 'Bare PCB test pad - not a fitted part' if test else 'Catalog identity verified; stock not reserved'
    if cat.get('assembly') and cat['assembly']!='SMT Assembly':status+='; assembly classification anomaly - supplier review/manual fit';anomalies.append(ref)
    mpn=cat.get('mpn') or fields.get('MPN') or '';mfr=cat.get('manufacturer') or fields.get('Manufacturer') or ''
    if mfr=='--':mfr=fields.get('Manufacturer','') or 'UNVERIFIED'
    rows.append(dict(Designator=ref,Comment=c.findtext('value'),Footprint=c.findtext('footprint'),Quantity=0 if dnp or test else 1,Manufacturer=mfr,MPN=mpn,LCSC=code,DNP='Yes' if dnp else 'No',Side='Bottom' if f.IsFlipped() else 'Top',Assembly=cat.get('pcba',''),Status=status,Source=cat.get('manufacturer_source') or cat.get('url','')))
with (OUT/'bom-detailed.csv').open('w',newline='') as out:
    w=csv.DictWriter(out,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
groups=collections.defaultdict(list)
for r in rows:
    if r['Quantity']:groups[(r['Comment'],r['Footprint'],r['LCSC'],r['MPN'],r['Manufacturer'])].append(r['Designator'])
with (OUT/'bom-jlcpcb.csv').open('w',newline='') as out:
    w=csv.writer(out);w.writerow(['Comment','Designator','Footprint','LCSC Part #','Manufacturer Part #','Manufacturer','Quantity'])
    for k,refs in groups.items():w.writerow([k[0],','.join(refs),k[1],k[2],k[3],k[4],len(refs)])
with (OUT/'cpl-jlcpcb.csv').open('w',newline='') as out:
    w=csv.writer(out);w.writerow(['Designator','Mid X','Mid Y','Layer','Rotation'])
    for r in rows:
        if not r['Quantity']:continue
        f=fs[r['Designator']];x=p.ToMM(f.GetX())-origin[0];y=origin[1]-p.ToMM(f.GetY())
        w.writerow([r['Designator'],f'{x:.5f}mm',f'{y:.5f}mm',r['Side'],f'{f.GetOrientationDegrees()%360:.3f}'])
summary=dict(footprints=len(rows),populated=sum(r['Quantity'] for r in rows),dnp=[r['Designator'] for r in rows if r['DNP']=='Yes'],bare_testpoints=[r['Designator'] for r in rows if r['Designator'].startswith('TP')],catalog_identities=len(catalog),assembly_anomalies=anomalies,missing_populated_mpn=[r['Designator'] for r in rows if r['Quantity'] and not r['MPN']],stock_verified=False,cpl_rotation='KiCad orientation in board coordinates; require JLC preview correction per exact part before ordering')
summary['missing_populated_manufacturer']=[r['Designator'] for r in rows if r['Quantity'] and r['Manufacturer'] in ('','--','UNVERIFIED')]
summary['populated_lcsc_codes']=len({r['LCSC'] for r in rows if r['Quantity']})
expected={r['Designator'] for r in rows if r['Quantity']}
with (OUT/'cpl-jlcpcb.csv').open() as source:cpl=list(csv.DictReader(source))
with (OUT/'bom-jlcpcb.csv').open() as source:bom=list(csv.DictReader(source))
assert {r['Designator'] for r in cpl}=={ref for r in bom for ref in r['Designator'].split(',')}==expected
assert len(cpl)==len(expected) and all(r['Layer']=='Top' for r in cpl)
assert not summary['missing_populated_mpn'] and not summary['missing_populated_manufacturer']
assert all(r['LCSC'] and catalog.get(r['LCSC'],{}).get('identity_verified') for r in rows if r['Quantity'])
(ROOT/'reports/assembly.json').write_text(json.dumps(summary,indent=2));print(summary)
