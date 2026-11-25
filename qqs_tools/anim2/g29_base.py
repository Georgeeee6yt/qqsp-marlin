import os
HERE=os.path.dirname(os.path.abspath(__file__))
def _ink(n):
    import cv2, numpy as np
    return cv2.imread(os.path.join(HERE,"src",n))[:,:,2].astype(np.float32)/255.0
import cv2, numpy as np
from scipy import ndimage
comp=_ink('g29_composite.png'); s2=_ink('g29_parts_open.png'); s3=_ink('g29_parts_wink.png')
W,H=1448,1086
HS,HX,HY=1.46,-105,97          # sheet -> comp: comp = sheet*1.46 + (-105, 97)
def sil(a,it=4): return ndimage.binary_fill_holes(ndimage.binary_closing(a>0.3,iterations=it))
def warp(a,s,dx,dy):
    M=np.float32([[s,0,dx],[0,s,dy]])
    return cv2.warpAffine(a.astype(np.float32),M,(W,H),flags=cv2.INTER_LINEAR)
def head_layer(src):
    h=src.copy(); h[668:,:]=0; h[:,735:]=0            # the head block of the sheet
    m=sil(h).astype(np.float32); h[m<0.5]=0
    return warp(h,HS,HX,HY), np.clip(warp(m,HS,HX,HY),0,1)
HEAD={'open':head_layer(s2),'wink':head_layer(s3)}
# sextant + hands group: the composite's orange fill regions (between the black lines) that belong to the
# sextant / hands / sleeves, whole, even where they lie over the body -> fully opaque arms.
hm=HEAD['open'][1]>0.5
reg,nr=ndimage.label(comp>0.5)
keep=np.zeros(comp.shape,bool)
for i,sl in enumerate(ndimage.find_objects(reg)):
    r=reg[sl]==i+1
    area=r.sum()
    if area<150: continue
    ys,xs=sl
    cx=(xs.start+xs.stop)/2; cy=(ys.start+ys.stop)/2
    if cy<300 or cx<640: continue                      # stars, ahoge, hair
    if xs.start>1150 and area<3000: continue             # star dots on the right
    out=(~hm[sl])[r].mean()
    if out>0.25: keep[sl]|=r
keep[495:600,748:935]|=comp[495:600,748:935]>0.5   # scope tube over the head
keep=ndimage.binary_dilation(keep,iterations=9)      # take the black outlines with the fills
keep&=ndimage.binary_fill_holes(ndimage.binary_closing(comp>0.3,iterations=5))
keep=ndimage.binary_fill_holes(keep)
GROUP=np.where(keep,comp,0).astype(np.float32); GMASK=keep.astype(np.float32)
# without the scope tube (lowered pose: it would poke the mouth)
GROUP_NS=GROUP.copy(); GMASK_NS=GMASK.copy()
GROUP_NS[480:610,730:878]=0; GMASK_NS[480:610,730:878]=0   # the tube only, left of the sextant leg
# parts from the sheet for stars / ahoge
def cut(src,box):
    x0,y0,x1,y1=box; p=src[y0:y1,x0:x1].copy(); m=sil(p); p[~m]=0; return p,m.astype(np.float32)
STAR=cut(s2,(49,817,133,902)); STAR2=cut(s2,(166,826,235,898)); DOT=cut(s2,(268,853,289,873)); AHOGE=cut(s2,(1261,786,1402,914))
def paste(c,p,m,cx,cy,s=1.0,ang=0.0):
    h,w=p.shape; M=cv2.getRotationMatrix2D((w/2,h/2),ang,s); M[0,2]+=cx-w/2; M[1,2]+=cy-h/2
    pw=cv2.warpAffine(p,M,(W,H)); mw=np.clip(cv2.warpAffine(m,M,(W,H)),0,1)
    c[:]=c*(1-mw)+pw*mw
def shift(a,dx,dy,ang=0.0,cx=1000,cy=700):
    M=cv2.getRotationMatrix2D((cx,cy),ang,1.0); M[0,2]+=dx; M[1,2]+=dy
    return cv2.warpAffine(a,M,(W,H),flags=cv2.INTER_LINEAR)
def frame(head='open', lift=1.0, ahoge=0.0, tw=(1,1,1,1), spark=0.0):
    c=np.zeros((H,W),np.float32)
    hp,hm_=HEAD[head]; c[:]=c*(1-hm_)+hp*hm_
    paste(c,*AHOGE,394,110,1.52,ahoge)               # ahoge on top of the head (composite position)
    dy=(1-lift)*200; dx=(1-lift)*25; ang=0.0
    G,GM=(GROUP,GMASK) if lift>=0.6 else (GROUP_NS,GMASK_NS)
    gp=shift(G,dx,dy,ang); gm=np.clip(shift(GM,dx,dy,ang),0,1)
    c[:]=c*(1-gm)+gp*gm
    for (p,m),(x,y,s),f in zip([STAR,STAR2,DOT,DOT],[(1198,112,1.24),(1330,247,1.24),(1384,91,1.05),(1066,153,1.05)],tw):
        if f>0.05: paste(c,p,m,x,y,s*f)
    if spark>0: paste(c,*STAR,1000,430,1.5*spark,45*(1-spark))
    return c
def small(c,w=160,bits=2):
    l=cv2.resize(c,(w,round(w*H/W)),interpolation=cv2.INTER_AREA); n=(1<<bits)-1
    return np.round(np.clip(l,0,1)*n)/n
OR=np.array([0xFF,0x8C,0x28],np.float32)
def rgb(l): return (l[:,:,None]*OR).astype(np.uint8)
