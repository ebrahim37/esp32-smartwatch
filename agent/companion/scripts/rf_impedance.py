#!/usr/bin/python3
"""2-D quasi-static grounded-coplanar-waveguide estimate, not RF certification.
Finite-volume Laplace solve, C and vacuum C0, Z=1/(c*sqrt(C*C0)).
JLC04081H-7628: h=.2104mm, t=.035mm, Dk=4.4; gap=.25mm.
Grid convergence and finite-boundary error remain engineering limitations.
"""
import json, pathlib, math
import numpy as np
from scipy.sparse import coo_matrix
from scipy.sparse.linalg import spsolve
ROOT=pathlib.Path(__file__).resolve().parents[1]
EPS=8.8541878128e-12; C=299792458

def capacitance(w,g=.25,step=.005,vacuum=False):
    xs=np.arange(-1.5,1.5+step/2,step); ys=np.arange(0,1.0+step/2,step)
    x,y=np.meshgrid(xs,ys); h=.2104;t=.035
    signal=(abs(x)<=w/2+1e-8)&(y>=h-step/2)&(y<=h+t-step/2)
    ground=((abs(x)>=w/2+g-1e-8)&(y>=h-step/2)&(y<=h+t-step/2))
    fixed=signal|ground; fixed[0,:]=True; fixed[-1,:]=True; fixed[:,0]=True; fixed[:,-1]=True
    eps=np.ones(x.shape)
    if not vacuum:
        eps[y<h]=4.4
        # Uniform coating approximation; actual JLC mask contour is not a rectangle.
        eps[(y>=h)&(y<h+t+.01524)]=3.8
    v=np.zeros(x.shape);v[signal]=1
    ids=np.full(x.shape,-1,dtype=int);ids[~fixed]=np.arange(np.count_nonzero(~fixed))
    rhs=np.zeros(np.count_nonzero(~fixed));diag=np.zeros_like(rhs);rows=[];cols=[];vals=[]
    for dy,dx in ((1,0),(-1,0),(0,1),(0,-1)):
        iy,ix=np.where(~fixed); jy=iy+dy;jx=ix+dx
        weight=2*eps[iy,ix]*eps[jy,jx]/(eps[iy,ix]+eps[jy,jx])
        a=ids[iy,ix]; c=ids[jy,jx];diag[a]+=weight
        free=c>=0
        rows.extend(a[free]);cols.extend(c[free]);vals.extend(-weight[free])
        rhs[a[~free]]+=weight[~free]*v[jy[~free],jx[~free]]
    rows.extend(np.arange(len(rhs)));cols.extend(np.arange(len(rhs)));vals.extend(diag)
    mat=coo_matrix((vals,(rows,cols)),shape=(len(rhs),len(rhs))).tocsr()
    v[~fixed]=spsolve(mat,rhs)
    energy=0
    for axis in (0,1):
        a=np.take(eps,np.arange(eps.shape[axis]-1),axis=axis);bb=np.take(eps,np.arange(1,eps.shape[axis]),axis=axis)
        ef=2*a*bb/(a+bb)
        energy+=np.sum(ef*np.diff(v,axis=axis)**2)
    return EPS*energy

if __name__=='__main__':
    rows=[]
    for w in (.25,.28,.30,.32,.35):
        c=capacitance(w);c0=capacitance(w,vacuum=True);z=1/(C*math.sqrt(c*c0))
        row=dict(width_mm=w,gap_mm=.25,z_ohm=z,c_pf_per_m=c*1e12,c0_pf_per_m=c0*1e12)
        rows.append(row);print(row,flush=True)
    (ROOT/'reports/rf_impedance.json').write_text(json.dumps(dict(model='2D finite-volume quasi-static GCPW; 5um grid; rectangular mask approximation',substrate_height_mm=.2104,substrate_dk=4.4,copper_mm=.035,mask_above_trace_mm=.01524,mask_dk=3.8,results=rows),indent=2))
