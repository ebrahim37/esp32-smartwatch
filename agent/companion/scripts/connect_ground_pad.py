#!/usr/bin/python3
"""Connect a local SMT power/ground pad using a screened dogbone and CLI DRC.
Usage: connect_ground_pad.py BOARD REF PIN [--allow-connected] [--power].
Zero new copper errors, no extra opens; default requires fewer actual opens.
"""
import pathlib,sys,math,json,subprocess
import pcbnew as p
from shapely.geometry import Polygon,Point,LineString
from shapely.ops import unary_union
path=pathlib.Path(sys.argv[1]).resolve();ref,pin=sys.argv[2:4];mm=p.FromMM;to=p.ToMM
D=pathlib.Path(__file__).resolve().parents[1]/'routing/local-stitch';D.mkdir(exist_ok=True);width=.6 if '--power' in sys.argv else .15;allow_connected='--allow-connected' in sys.argv

def poly(ps):
    out=[]
    for i in range(ps.OutlineCount()):
        c=ps.COutline(i);pts=[(to(c.CPoint(j).x),to(c.CPoint(j).y)) for j in range(c.PointCount())]
        if len(pts)>2:out.append(Polygon(pts))
    return unary_union(out)
def pos(q):return(to(q.GetPosition().x),to(q.GetPosition().y))
def cu(t):return Point(pos(t)).buffer(to(t.GetWidth(p.F_Cu))/2) if isinstance(t,p.PCB_VIA) else LineString([pos(t),(to(t.GetEnd().x),to(t.GetEnd().y))]).buffer(to(t.GetWidth())/2)
def drc(name):
    output=D/(name+'.json');subprocess.run(['kicad-cli','pcb','drc','--refill-zones','--save-board','--severity-all','--schematic-parity','--format','json','-o',str(output),str(path)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL);return json.loads(output.read_text())
report=drc('ground-baseline');backup=path.read_bytes();b=p.LoadBoard(str(path));pad=next(a for f in b.GetFootprints() if f.GetReference()==ref for a in f.Pads() if a.GetNumber()==pin);net=pad.GetNetCode();origin=pos(pad);pads=[(a,poly(a.GetEffectivePolygon(p.F_Cu))) for f in b.GetFootprints() for a in f.Pads()];tracks=[(t,cu(t)) for t in b.GetTracks()];outline=p.SHAPE_POLY_SET();b.GetBoardPolygonOutlines(outline,False);board=poly(outline);keepout=unary_union([poly(z.Outline()) for z in b.Zones() if z.GetIsRuleArea() and z.GetDoNotAllowVias()]);options=[]
pads=[(a,g) for a,g in pads if g.distance(Point(origin))<4.];tracks=[(t,g) for t,g in tracks if g.distance(Point(origin))<4.]
for diameter,drill in [(.6,.3),(.35,.2),(.3,.15),(.25,.15)]:
    vr=diameter/2;dr=drill/2
    for radius in [.5,.75,1.,1.25,1.5,2.,2.5,3.]:
        for angle in range(0,360,15):
            q=(round(origin[0]+radius*math.cos(math.radians(angle)),4),round(origin[1]+radius*math.sin(math.radians(angle)),4));center=Point(q);vg=center.buffer(vr);tg=LineString([origin,q]).buffer(width/2)
            if not board.buffer(-.2).covers(vg) or keepout.intersects(vg):continue
            if any(center.buffer(dr+.075).intersects(g) for a,g in pads if a.GetAttribute()==p.PAD_ATTRIB_SMD):continue
            if any(a.GetNetCode()!=net and (vg.distance(g)<.13 or tg.distance(g)<.105) for a,g in pads):continue
            if any(a.GetDrillSize().x and center.distance(Point(pos(a)))<dr+max(to(a.GetDrillSize().x),to(a.GetDrillSize().y))/2+.45 for a,g in pads):continue
            bad=False
            for t,g in tracks:
                via=isinstance(t,p.PCB_VIA)
                if via and center.distance(Point(pos(t)))<dr+to(t.GetDrillValue())/2+.26:bad=True;break
                if t.GetNetCode()==net:continue
                if via:
                    if vg.distance(g)<.1 or tg.distance(g)<.205:bad=True;break
                else:
                    line=LineString([pos(t),(to(t.GetEnd().x),to(t.GetEnd().y))]);tr=to(t.GetWidth())/2
                    if any(s in t.GetNetname() for s in ('ANT','J_LTE','J_GNSS','C50-Pad1')) and (center.distance(line)<vr+tr+.58 or tg.distance(g)<.25):bad=True;break
                    if center.distance(line)<max(vr+tr+.105,dr+tr+.205) or t.GetLayer()==p.F_Cu and tg.distance(g)<.105:bad=True;break
            if not bad:options.append((diameter,drill,q))
print('Screened local ground options',len(options),flush=True)
for diameter,drill,q in options[:40]:
    b=p.LoadBoard(str(path));v=p.PCB_VIA(b);v.SetPosition(p.VECTOR2I(mm(q[0]),mm(q[1])));v.SetWidth(mm(diameter));v.SetDrill(mm(drill));v.SetLayerPair(p.F_Cu,p.B_Cu);v.SetViaType(p.VIATYPE_THROUGH);v.SetNetCode(net);b.Add(v);t=p.PCB_TRACK(b);t.SetStart(p.VECTOR2I(mm(origin[0]),mm(origin[1])));t.SetEnd(v.GetPosition());t.SetWidth(mm(width));t.SetLayer(p.F_Cu);t.SetNetCode(net);b.Add(t);p.SaveBoard(str(path),b);new=drc('ground-candidate')
    if not any(r['severity']=='error' for r in new['violations']) and len(new['unconnected_items'])<len(report['unconnected_items'])+int(allow_connected) and not new['schematic_parity']:
        record=dict(ref=ref,pin=pin,pos_mm=q,via_diameter_mm=diameter,drill_mm=drill,stub_width_mm=width,before_opens=len(report['unconnected_items']),after_opens=len(new['unconnected_items']))
        logfile=D/'accepted-dogbones.json';records=json.loads(logfile.read_text()) if logfile.exists() else [];records.append(record);logfile.write_text(json.dumps(records,indent=2))
        print('ACCEPT local dogbone',ref,pin,q,diameter,drill,'opens',len(new['unconnected_items']),flush=True);break
    path.write_bytes(backup)
else:raise SystemExit('No legal ground dogbone accepted; trial preserved')
