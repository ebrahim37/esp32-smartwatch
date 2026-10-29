#!/usr/bin/python3
"""Inspect actual VRML solids independently of footprint courtyards.
VRML lengths are KiCad's2.54mm unit. Native envelopes reveal model-origin errors;
none of these body bounds is a replacement for pad/copper support checks.
"""
import pathlib,re,json,numpy as np
ROOT=pathlib.Path(__file__).resolve().parents[1]
names=['WIFI-SMD_L25.5-W18.0-H3.1-ESP32-WROOM-32E.wrl','USB-C_SMD-TYPE-C-31-M-12_1.wrl','CONN-SMD_2053380002.wrl','TF-SMD_L14.8-W14.5-H1.9-P1.1.wrl','IND-SMD_L6.5-W6.5-IHLP2525CZER6R8M01.wrl']
rows={}
for fn in names:
    shapes=[]
    for i,s in enumerate((ROOT.parents[1] / 'kicad/companion/libs/jlcpcb.3dshapes'/fn).read_text().split('Shape{')[1:]):
        for raw in re.findall(r'point\s*\[([^]]+)\]',s):
            a=np.array([float(v) for v in re.findall(r'-?\d+\.?\d*(?:[eE][-+]?\d+)?',raw)]).reshape(-1,3)*2.54
            color=re.search(r'diffuseColor\s+([^\n]+)',s)
            shapes.append(dict(shape=i,color=color.group(1).strip() if color else None,minimum=a.min(axis=0).tolist(),maximum=a.max(axis=0).tolist()))
    rows[fn]=shapes
    print(fn)
    for s in shapes[:25]:print(s)
(ROOT/'reports/model-envelopes.json').write_text(json.dumps(rows,indent=2))
