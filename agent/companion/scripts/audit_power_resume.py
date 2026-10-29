#!/usr/bin/python3
"""Measure saved copper geometry and via-in-pad, not a physical PI qualification.
IPC-2221 isolated-trace screening is conservative, not IPC-2152 simulation.
Short pad escapes need separate resistance review; parallel planes are reported.
"""
import pathlib,json,math,collections
import pcbnew as p
from shapely.geometry import Polygon,Point,LineString,box
from shapely.ops import unary_union
R=pathlib.Path(__file__).resolve().parents[1];b=p.LoadBoard(str(R.parents[1] / 'kicad/companion/companion.kicad_pcb'));to=p.ToMM
nets={'/Power/VBAT':3.,'/Power/VSYS':3.,'/Power/USB_5V':2.,'/ESP32/SYS_3V3':.6,'/Modem/MODEM_3V8':2.,'/Power/LTE_SW1':3.,'/Power/LTE_SW2':3.}
def polyset(ps):
    gs=[]
    for i in range(ps.OutlineCount()):
        c=ps.COutline(i);pts=[(to(c.CPoint(j).x),to(c.CPoint(j).y)) for j in range(c.PointCount())]
        if len(pts)<3:continue
        holes=[]
        for k in range(ps.HoleCount(i)):
            h=ps.CHole(i,k);holes.append([(to(h.CPoint(j).x),to(h.CPoint(j).y)) for j in range(h.PointCount())])
        gs.append(Polygon(pts,holes))
    return unary_union(gs)
pads=[(f,a) for f in b.GetFootprints() for a in f.Pads()]
def land(a):
    try:return polyset(a.GetEffectivePolygon(p.F_Cu))
    except (AttributeError,TypeError):
        r=a.GetBoundingBox();return box(to(r.GetLeft()),to(r.GetTop()),to(r.GetRight()),to(r.GetBottom()))
lands=[(f,a,land(a)) for f,a in pads]
traces=list(b.GetTracks());vias=[v for v in traces if isinstance(v,p.PCB_VIA)];vip=[]
for v in vias:
    xy=(to(v.GetX()),to(v.GetY()));g=Point(xy).buffer(to(v.GetDrillValue())/2)
    hits=[dict(ref=f.GetReference(),pad=a.GetNumber(),net=a.GetNetname()) for f,a,z in lands if a.GetAttribute()==p.PAD_ATTRIB_SMD and g.intersects(z)]
    if hits:vip.append(dict(uuid=v.m_Uuid.AsString(),pos_mm=xy,net=v.GetNetname(),drill_mm=to(v.GetDrillValue()),diameter_mm=to(v.GetWidth(p.F_Cu)),pads=hits,filled=str(v.GetFillingMode()),capped=str(v.GetCappingMode())))
report={'assumptions':{'screening_temperature_rise_C':10,'copper_thickness_outer_mm':.035,'copper_thickness_inner_mm':.0152,'resistivity_ohm_mm':1.724e-5,'currents_are_design_envelopes_not_measured':True,'no_zone_bottleneck_or_thermal_simulation_claim':True},'nets':{},'via_in_pad':vip,'via_count':len(vias)}
for name,amps in nets.items():
    rows=[]
    for t in traces:
        if isinstance(t,p.PCB_VIA) or t.GetNetname()!=name:continue
        w=to(t.GetWidth());length=to(t.GetLength());inner=t.GetLayer() in (p.In1_Cu,p.In2_Cu);thick=.0152 if inner else .035;k=.024 if inner else .048
        area=w*thick/(.0254**2);capacity=k*10**.44*area**.725;res=1.724e-5*length/(w*thick)
        rows.append(dict(uuid=t.m_Uuid.AsString(),layer=b.GetLayerName(t.GetLayer()),width_mm=w,length_mm=length,start_mm=[to(t.GetStart().x),to(t.GetStart().y)],end_mm=[to(t.GetEnd().x),to(t.GetEnd().y)],isolated_IPC2221_10C_A=capacity,resistance_mohm=res*1e3,voltage_drop_at_envelope_mV=res*amps*1e3))
    zs=[dict(layer=b.GetLayerName(z.GetLayer()),area_mm2=polyset(z.GetFilledPolysList(z.GetLayer())).area,islands=z.GetFilledPolysList(z.GetLayer()).OutlineCount()) for z in b.Zones() if not z.GetIsRuleArea() and z.GetNetname()==name]
    req={}
    for layer,k,thick in [('outer',.048,.035),('inner',.024,.0152)]:req[layer]=(amps/(k*10**.44))**(1/.725)*.0254**2/thick
    report['nets'][name]=dict(screening_current_A=amps,isolated_trace_width_10C_mm=req,tracks=rows,zones=zs,pads=[dict(ref=f.GetReference(),pad=a.GetNumber(),pos_mm=[to(a.GetX()),to(a.GetY())]) for f,a in pads if a.GetNetname()==name],via_count=sum(v.GetNetname()==name for v in vias))
(R/'reports/power-resume-audit.json').write_text(json.dumps(report,indent=2));print('Power copper audit:',len(traces)-len(vias),'tracks;',len(vias),'vias;',len(vip),'via holes intersect SMD solder lands')
for n,d in report['nets'].items():
    bad=[q for q in d['tracks'] if q['isolated_IPC2221_10C_A']<d['screening_current_A'] and q['length_mm']>.5]
    print(n,'required isolated widths',d['isolated_trace_width_10C_mm'],'long under-screening traces',len(bad),'filled zones',d['zones'])
