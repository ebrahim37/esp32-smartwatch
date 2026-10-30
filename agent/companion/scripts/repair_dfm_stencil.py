#!/usr/bin/python3
"""Restore TI FET drain, charger EP, converter EP stencil openings only.

Parent integration owner applies --apply; author does not execute this repair.
No copper, net, pad UUID, footprint pose/FPID, track, via, or zone changes.
FET recommendations: CSD16340Q3 SLPS247E p10 (0.34x0.45 drain
row plus two1.02x1.445 windows). Charger: BQ25606 RGE0024H p45
(four1.188x1.188 R.05 at+/-.694, .125mm stencil). Converter:
TPS552882 RPM0026A p40 (three.375x1.425 R.05, no subdivision,
.1mm stencil). Distinct device stencil thickness examples do not establish
supplier agreement on the actual common/stepped stencil process.
Geometry-matched graphics replacement makes repeated application idempotent.
"""
import argparse
import hashlib
import json
import math
import pathlib
import shutil
import tempfile
import pcbnew as p
from shapely.ops import unary_union
from verify_dfm_patterns import exact_pad, local, native_poly, roundrect, world, xy
from sexp_spans import parse, apply

ROOT = pathlib.Path(__file__).resolve().parents[3]


def specs():
    for ref in ('QH2', 'QL2'):
        for n in range(5,9):
            yield ref, f'drain-{n}', [(6.5-n)*.65,-1.435,.34,.45,0]
        for side,x in [('left',-.635),('right',.635)]:
            yield ref, 'drain-window-'+side, [x,-.2625,1.02,1.445,0]
    for x in (-.694,.694):
        for y in (-.694,.694):
            yield 'U1', f'EP-{x}-{y}', [x,y,1.188,1.188,.05]
    for n,x in [('24',-.625),('25',0),('26',.625)]:
        yield 'U5', 'EP-'+n, [x,0,.375,1.425,.05]


def polygon_points(x,y,w,h,r):
    if not r:
        return [(x+w/2,y+h/2),(x-w/2,y+h/2),(x-w/2,y-h/2),(x+w/2,y-h/2)]
    pts=[]
    for cx,cy,start in [(x+w/2-r,y+h/2-r,0),(x-w/2+r,y+h/2-r,90),(x-w/2+r,y-h/2+r,180),(x+w/2-r,y-h/2+r,270)]:
        for j in range(17):
            angle=math.radians(start+j*90/16)
            pts.append((cx+r*math.cos(angle),cy+r*math.sin(angle)))
    return pts


def vector(x,y):
    return p.VECTOR2I(round(x*1_000_000),round(y*1_000_000))


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--board',type=pathlib.Path,default=ROOT/'kicad/companion/companion.kicad_pcb')
    ap.add_argument('--report',type=pathlib.Path,default=ROOT/'agent/companion/reports/jlcpcb-dfm/stencil-repair.json')
    ap.add_argument('--apply',action='store_true')
    args=ap.parse_args()
    input_board=p.LoadBoard(str(args.board))
    text=args.board.read_text()
    original=hashlib.sha256(args.board.read_bytes()).hexdigest()
    before={pd.m_Uuid.AsString():(pd.GetNetname(),xy(pd.GetPosition()),xy(pd.GetSize()),int(pd.GetShape()),exact_pad(pd).wkb_hex)
            for f in input_board.GetFootprints() for pd in f.Pads()}
    planned=list(specs())
    refs=('QH2','QL2','U1','U5')
    def affected(fp):
        ref=fp.GetReference()
        return [pd for pd in fp.Pads() if (ref in ('QH2','QL2') and int(pd.GetNumber())>=5)
                or (ref=='U1' and pd.GetNumber()=='25') or (ref=='U5' and pd.GetNumber() in ('24','25','26'))]
    report=[]
    remove_uuids=set()
    for ref in refs:
        fp=input_board.FindFootprintByReference(ref)
        prior=[native_poly(pd.GetEffectivePolygon(p.F_Paste)) for pd in affected(fp) if pd.IsOnLayer(p.F_Paste)]
        removed=[]
        targets=[(label,vals) for rr,label,vals in planned if rr==ref]
        for graphic in fp.GraphicalItems():
            if graphic.GetLayer()!=p.F_Paste:
                continue
            g=native_poly(graphic.GetPolyShape()) if graphic.GetShape()==p.SHAPE_T_POLY else None
            if g is None:
                raise RuntimeError(f'Unexpected nonpolygon paste graphic in {ref}; refusing ambiguous removal')
            matches=[label for label,vals in targets if world(fp,roundrect(*vals)).hausdorff_distance(g)<.00026]
            if matches:
                uid=graphic.m_Uuid.AsString()
                prior.append(g);removed.append(dict(uuid=uid,matches=matches));remove_uuids.add(uid)
        report.append(dict(ref=ref,affected_pad_uuids=[pd.m_Uuid.AsString() for pd in affected(fp)],
                           old_affected_paste_area_mm2=unary_union(prior).area,
                           new_ideal_affected_paste_area_mm2=unary_union([world(fp,roundrect(*vals)) for label,vals in targets]).area,
                           removed_geometry_matched_graphics=removed,added_apertures=[]))
    # KiCad10 SWIG Remove(graphic) can invalidate its parent wrapper. Remove
    # ONLY the geometrically selected fp_poly spans, then load a fresh board.
    edits=[]
    found=set()
    for fpnode in parse(text).children('footprint'):
        for graphic in fpnode.children('fp_poly'):
            uidnode=graphic.child('uuid')
            layernode=graphic.child('layer')
            uid=uidnode.items[1] if uidnode else None
            if uid in remove_uuids:
                if not layernode or layernode.items[1]!='F.Paste':
                    raise RuntimeError('Refusing removal outside F.Paste')
                edits.append((graphic.start,graphic.end,''));found.add(uid)
    if found!=remove_uuids:
        raise RuntimeError(f'Cannot locate exact saved polygon UUIDs: {remove_uuids-found}')
    with tempfile.TemporaryDirectory(prefix='companion-stencil-') as scratch:
        scratchpath=pathlib.Path(scratch)/args.board.name
        scratchpath.write_text(apply(text,edits))
        # Load the actual project rules; a bare scratch PCB loads defaults.
        # Save only inside scratch, then copy PCB bytes back, never project data.
        for suffix in ('.kicad_pro','.kicad_dru'):
            source=args.board.with_suffix(suffix)
            if source.exists():
                shutil.copy2(source,scratchpath.with_suffix(suffix))
        board=p.LoadBoard(str(scratchpath))
        for row in report:
            ref=row['ref'];fp=board.FindFootprintByReference(ref)
            for pd in affected(fp):
                ls=pd.GetLayerSet();ls.RemoveLayer(p.F_Paste);pd.SetLayerSet(ls)
            angle=math.radians(fp.GetOrientationDegrees())
            c,s=math.cos(angle),math.sin(angle)
            ox,oy=xy(fp.GetPosition())
            for rr,label,vals in planned:
                if rr!=ref:
                    continue
                poly=p.SHAPE_POLY_SET();poly.NewOutline()
                for xx,yy in polygon_points(*vals):
                    poly.Append(vector(ox+xx*c+yy*s,oy-xx*s+yy*c))
                shape=p.PCB_SHAPE(fp);shape.SetShape(p.SHAPE_T_POLY);shape.SetPolyShape(poly)
                shape.SetLayer(p.F_Paste);shape.SetFilled(True);shape.SetWidth(0);fp.Add(shape)
                row['added_apertures'].append(dict(label=label,local_mm=vals,uuid=shape.m_Uuid.AsString()))
        after={pd.m_Uuid.AsString():(pd.GetNetname(),xy(pd.GetPosition()),xy(pd.GetSize()),int(pd.GetShape()),exact_pad(pd).wkb_hex)
               for f in board.GetFootprints() for pd in f.Pads()}
        if before!=after:
            raise RuntimeError('Copper, identity, net, or pose unexpectedly changed; refusing save')
        if args.apply:
            p.SaveBoard(str(scratchpath),board)
            args.board.write_bytes(scratchpath.read_bytes())
    doc=dict(applied=args.apply,input_board=str(args.board),input_sha256=original,changes=report,
             new_graphics_count=len(planned),copper_identity_net_pose_unchanged=True,
             FET_derivation='Stencil span3.320 => rowcenters+/-1.435; two windows width1.020 separated.250 => x+/-.635; upperwindowedge-1.210+.225=-.985; length1.445 => center-.2625.',
             U5_scope='24/25/26 each1.425x.375 R.05 atx+/-.625; no required subdivided EP windows. Remaining perimeter library deviations are independently reported by verify_dfm_patterns.',
             scope_limits=['No DRC/ERC/refill/export performed by this repair.','Not supplier stencil process approval; TI thickness examples differ (.125mm U1,.1mm U5).'])
    args.report.parent.mkdir(parents=True,exist_ok=True);args.report.write_text(json.dumps(doc,indent=2)+'\n')
    print(json.dumps(dict(applied=args.apply,new_graphics=len(planned),report=str(args.report)),indent=2))


if __name__=='__main__':
    main()
