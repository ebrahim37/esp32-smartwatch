#!/usr/bin/python3
"""Remove only nonessential test-point symbols/lands, never functional net nodes.
Retain TP9 EN and TP10 BOOT as 0.6 mm exposed recovery pads. Preserve all
wire segments where a removed TP was placed on a through connection.
"""
import pathlib,json
import pcbnew as p
from sexp_spans import parse,apply
ROOT=pathlib.Path(__file__).resolve().parents[1]
keep={'TP9','TP10'}; removed=[]
for filename in (ROOT.parents[1] / 'kicad/companion').glob('*.kicad_sch'):
    text=filename.read_text();root=parse(text);edits=[]
    for s in root.children('symbol'):
        lib=s.child('lib_id')
        if not lib or lib.items[1]!='Connector:TestPoint':continue
        ref=next(x.items[2] for x in s.children('property') if x.items[1]=='Reference')
        if ref in keep:
            fp=next(x for x in s.children('property') if x.items[1]=='Footprint')
            old=text[fp.start:fp.end];new=old.replace('TestPoint:TestPoint_Pad_D1.0mm','libs:TestPoint_Recovery_D0.6mm').replace('libs:Recovery_Pad_D0.6mm','libs:TestPoint_Recovery_D0.6mm')
            edits.append((fp.start,fp.end,new));continue
        removed.append(ref);edits.append((s.start,s.end,''))
        pos=s.child('at').items[1:3]
        wires=[w for w in root.children('wire') if any(q.items[1:3]==pos for q in w.child('pts').children('xy'))]
        if len(wires)!=1:continue # through-wire junctions remain electrically intact
        w=wires[0];ends=[q.items[1:3] for q in w.child('pts').children('xy')];other=ends[1] if ends[0]==pos else ends[0]
        labels=[x for key in ('label','global_label','hierarchical_label') for x in root.children(key) if x.child('at').items[1:3]==other]
        if len(labels)==1:
            # The removed branch terminates in a named label; no functional wire
            # is removed. Refuse if another wire also touches its terminal.
            adjacent=[v for v in root.children('wire') if v is not w and any(q.items[1:3]==other for q in v.child('pts').children('xy'))]
            if not adjacent:
                edits.extend([(w.start,w.end,''),(labels[0].start,labels[0].end,'')])
    if edits:filename.write_text(apply(text,edits))
b=p.LoadBoard(str(ROOT.parents[1] / 'kicad/companion/companion.kicad_pcb'))
fs={f.GetReference():f for f in b.GetFootprints()}
removed=sorted({f'TP{i}' for i in range(1,15)}-keep)
obsolete=[fs[r] for r in removed if r in fs]; additions=[]
for ref in sorted(keep):
    old=fs.get(ref)
    if not old:raise RuntimeError(ref)
    if str(old.GetFPID().GetLibItemName())=='TestPoint_Recovery_D0.6mm':continue
    new=p.FOOTPRINT(b);new.SetFPID(p.LIB_ID('libs','TestPoint_Recovery_D0.6mm'));new.SetReference(ref);new.SetValue('TestPoint');new.SetPath(old.GetPath());new.SetPosition(old.GetPosition());new.SetAttributes(p.FP_EXCLUDE_FROM_BOM|p.FP_EXCLUDE_FROM_POS_FILES)
    pad=p.PAD(new);pad.SetNumber('1');pad.SetAttribute(p.PAD_ATTRIB_SMD);pad.SetShape(p.PAD_SHAPE_CIRCLE);pad.SetSize(p.VECTOR2I(p.FromMM(.6),p.FromMM(.6)));layers=p.LSET();layers.AddLayer(p.F_Cu);layers.AddLayer(p.F_Mask);pad.SetLayerSet(layers);pad.SetPosition(old.GetPosition());pad.SetNetCode(next(iter(old.Pads())).GetNetCode());new.Add(pad)
    # Courtyard is a deliberate 0.8mm square; no paste, no purchased component.
    x,y=old.GetPosition()
    for a,c in [((-.4,-.4),(.4,-.4)),((.4,-.4),(.4,.4)),((.4,.4),(-.4,.4)),((-.4,.4),(-.4,-.4))]:
        sh=p.PCB_SHAPE(new);sh.SetShape(p.SHAPE_T_SEGMENT);sh.SetStart(p.VECTOR2I(x+p.FromMM(a[0]),y+p.FromMM(a[1])));sh.SetEnd(p.VECTOR2I(x+p.FromMM(c[0]),y+p.FromMM(c[1])));sh.SetLayer(p.F_CrtYd);sh.SetWidth(p.FromMM(.05));new.Add(sh)
    new.Reference().SetLayer(p.F_Fab);new.Reference().SetVisible(False);new.Value().SetVisible(False);obsolete.append(old);additions.append(new)
for old in obsolete:b.Remove(old)
for new in additions:b.Add(new)
p.SaveBoard(str(ROOT.parents[1] / 'kicad/companion/companion.kicad_pcb'),b)
(ROOT/'reports/testpoint-revision.json').write_text(json.dumps({'removed':sorted(removed),'retained':sorted(keep),'access':{'flashing':'USB-C / CP2102N UART and auto EN/BOOT','EN':'TP9 0.6mm exposed pad','BOOT':'TP10 0.6mm exposed pad','GND/rails':'existing capacitor and connector lands','modem UART/debug':'TXU0204 lands and ESP32 firmware bridge; direct probe access before enclosure closure'}},indent=2))
print('Removed',removed,'retained',sorted(keep),'footprints',len(fs)-len(obsolete)+len(additions))
