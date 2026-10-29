#!/usr/bin/python3
"""Generate a dimensioned packaging drawing, not a fitted enclosure CAD model."""
import json,pathlib,html
R=pathlib.Path(__file__).resolve().parents[1];cfg=json.loads((R/'scripts/layout-revision.json').read_text());a=cfg['anchors'];W,H=cfg['size'];s=9;ox=60;oy=190
out=['<svg xmlns="http://www.w3.org/2000/svg" width="960" height="860" viewBox="0 0 960 860" style="color:var(--fg,currentColor);font:14px sans-serif">']
def text(x,y,t,size=14):out.append(f'<text x="{x}" y="{y}" fill="currentColor" font-size="{size}">{html.escape(t)}</text>')
def rect(x,y,w,h,color='--border',dash=False):out.append(f'<rect x="{ox+x*s}" y="{oy+y*s}" width="{w*s}" height="{h*s}" fill="none" stroke="var({color},currentColor)" stroke-width="1.5"'+(' stroke-dasharray="5 4"' if dash else '')+'/>')
text(60,30,'Companion PCB / battery / antenna arrangement',22);text(60,56,'Planning envelope 84.6 × 54 × 16 mm; tolerances and cell swelling require supplier qualification')
rect(-.5,-13.65,83,52.4,'--border');rect(0,0,W,H,'--accent');bx0,by0,bx1,by1=cfg['battery']['rect'];rect(bx0,by0,bx1-bx0,by1-by0,'--warning',True)
for key,c in cfg['external_antennas'].items():
    x0,y0,x1,y1=c['rect_relative_pcb_mm'];rect(x0,y0,x1-x0,y1-y0,'--c1' if key=='LTE' else '--c2');text(ox+x0*s+6,oy+y0*s+22,key+' / '+c['model']);text(ox+x0*s+6,oy+y0*s+43,c['face']+' inside face')
rect(3.85,-6.68,18.3,6.64,'--error',True);text(ox+3.85*s,oy-10,'ESP32 antenna: 6.53 mm body overhang',12)
for ref,label in [('U7','ESP32'),('U10','EG912U'),('USBC1','USB-C ↓'),('J_SD1','microSD ↓'),('U2','Battery cable ↓'),('J_LTE1','LTE coax'),('J_GNSS1','GNSS coax')]:
    x,y,rot,_=a[ref];text(ox+x*s-20,oy+y*s,label,12)
text(60,oy+H*s+28,'82 × 38.5 mm carrier; 82 × 30 mm battery below dashed region. All fitted electronics on F.Cu.')
text(60,oy+H*s+52,'Outside arrows: separate SD/USB apertures; battery connector is assembly access, not a user socket.')
text(60,oy+H*s+76,'Coax: 100 mm cables, clip excess outside ESP antenna area; route LTE connection toward X>23 mm.')
# Vertical section, exaggerated horizontal scale only; actual Z dimensions labelled.
y=660
text(60,y,'Vertical section / nominal stack',18)
for i,(name,t) in enumerate([('Front printed wall',.8),('Component space',3.4),('Carrier',.8),('Underside gap incl. 0.25 mm shield',1.75),('LiPo nominal',7.2),('Rear support/tolerance space',1.25),('Rear printed wall',.8)]):
    text(60,y+26+i*21,f'{name}: {t:g} mm')
text(450,y+30,'No pouch contact; PCB supported independently.')
text(450,y+56,'Nominal antenna envelope separation:')
text(450,y+80,'ESP/LTE 9.85 mm; ESP/battery 8.91 mm.')
text(450,y+104,'15 mm Espressif clearance is NOT met.')
text(450,y+128,'Opposite plastic faces are not RF shielding.')
text(450,y+152,'Not an RF approval or an order-ready enclosure.')
out.append('</svg>');(R/'reports/mechanical-arrangement.svg').write_text('\n'.join(out));print(R/'reports/mechanical-arrangement.svg')
