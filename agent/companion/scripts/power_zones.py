#!/usr/bin/python3
"""Reinforce real supply/switching copper with connected, clearance-filled pours.
Actual KiCad connectivity/DRC, not polygon declarations, determines completion.
"""
import pathlib,json,math
import pcbnew as p
from shapely.geometry import LineString,Point,box
from shapely.ops import unary_union
ROOT=pathlib.Path(__file__).resolve().parents[1];path=ROOT.parents[1] / 'kicad/companion/companion.kicad_pcb';b=p.LoadBoard(str(path));mm=p.FromMM;to=p.ToMM
padlist=[a for f in b.GetFootprints() for a in f.Pads()]
def pos(a):return(to(a.GetPosition().x),to(a.GetPosition().y))
def polygon(z,geom):
    ps=z.Outline()
    for poly in ([geom] if geom.geom_type=='Polygon' else geom.geoms):
        ps.NewOutline()
        for x,y in list(poly.exterior.coords)[:-1]:ps.Append(mm(x),mm(y))

def mst(points):
    tree={0};lines=[]
    while len(tree)<len(points):
        _,i,j=min((math.dist(points[i],points[j]),i,j) for i in tree for j in range(len(points)) if j not in tree)
        lines.append(LineString([points[i],points[j]]));tree.add(j)
    return lines

rails={'/Modem/MODEM_3V8':1.25,'/Power/VSYS':1.0,'/Power/VBAT':.8,'/Power/USB_5V':.8,'/ESP32/SYS_3V3':.5,'/Power/LTE_SW1':.6,'/Power/LTE_SW2':.6}
for priority,(name,radius) in enumerate(rails.items(),10):
    code=b.FindNet(name).GetNetCode();pts=list(dict.fromkeys(pos(a) for a in padlist if a.GetNetCode()==code and a.IsOnLayer(p.B_Cu)))
    pts+=list(dict.fromkeys(pos(v) for v in b.GetTracks() if isinstance(v,p.PCB_VIA) and v.GetNetCode()==code))
    if not pts:continue
    geoms=[Point(q).buffer(radius,quad_segs=4) for q in pts]+[l.buffer(radius,quad_segs=4,join_style=2) for l in mst(pts)]
    shape=unary_union(geoms).intersection(box(50.25,50.25,131.75,79.75)).simplify(.005,preserve_topology=True)
    z=p.ZONE(b);z.SetLayer(p.B_Cu);z.SetNetCode(code);z.SetAssignedPriority(priority);z.SetLocalClearance(mm(.2));z.SetMinThickness(mm(.15));z.SetPadConnection(p.ZONE_CONNECTION_FULL);z.SetIslandRemovalMode(p.ISLAND_REMOVAL_MODE_ALWAYS);polygon(z,shape);b.Add(z)
    print('Power copper',name,'declared area',shape.area,flush=True)
# Stitch only in genuinely empty, legal sites. Same-net copper is not an obstacle,
# but all solder lands are excluded so ordinary vias do not wick component solder.
tracks=list(b.GetTracks());obstacles=[]
for a in padlist:
    r=a.GetBoundingBox();obstacles.append((box(to(r.GetLeft()),to(r.GetTop()),to(r.GetRight()),to(r.GetBottom())),a.GetNetname()))
for t in tracks:
    if isinstance(t,p.PCB_VIA):g=Point(pos(t)).buffer(to(t.GetWidth(p.F_Cu))/2,quad_segs=8)
    else:g=LineString([pos(t),(to(t.GetEnd().x),to(t.GetEnd().y))]).buffer(to(t.GetWidth())/2,quad_segs=4)
    obstacles.append((g,t.GetNetname()))
placed=[];gnd=b.FindNet('GND').GetNetCode()
for x in range(51,132,4):
    for y in range(51,80,4):
        q=Point(x,y)
        if any(q.distance(g)<.5 for g,n in obstacles if n!='GND'):continue
        if any(q.distance(g)<.35 for g,n in obstacles[:len(padlist)]):continue
        if any(q.distance(obstacles[i][0])<.61 for i,a in enumerate(padlist) if a.GetDrillSize().x):continue
        if any(q.distance(g)<1.03 for g,n in obstacles if n in ('/Modem/GNSS_ANT','/Modem/LTE_ANT','Net-(J_LTE1-SIN)','Net-(J_GNSS1-SIN)','Net-(C50-Pad1)')):continue
        if any(math.dist((x,y),pos(v))<.7 for v in tracks if isinstance(v,p.PCB_VIA)):continue
        v=p.PCB_VIA(b);v.SetPosition(p.VECTOR2I(mm(x),mm(y)));v.SetWidth(p.F_Cu,mm(.6));v.SetWidth(p.B_Cu,mm(.6));v.SetDrill(mm(.3));v.SetViaType(p.VIATYPE_THROUGH);v.SetLayerPair(p.F_Cu,p.B_Cu);v.SetNetCode(gnd);b.Add(v);placed.append((x,y))
p.SaveBoard(str(path),b);b=p.LoadBoard(str(path));zones=p.ZONES()
for z in b.Zones():zones.push_back(z)
p.ZONE_FILLER(b).Fill(zones);p.SaveBoard(str(path),b)
(ROOT/'reports/power-reinforcement.json').write_text(json.dumps(dict(stitching_vias=placed,rail_radii_mm=rails),indent=2));print('Ground stitching vias',len(placed))
