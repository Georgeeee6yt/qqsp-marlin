import cv2, numpy as np, json, math
from scipy import ndimage
from g29_base import comp, s2, s3, W, H, HEAD, keep as GROUPKEEP, sil, cut
import os
A=json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)),'align_g29.json')))
FW,FH=160,120
SC=FW/W
# ---- sextant mask in composite coords: the sheet part warped to where it matched
x0,y0,x1,y1=A['parts']['sextant']['src']; s=A['parts']['sextant']['scale']; cx,cy=A['parts']['sextant']['comp_xy']
sp=s2[y0:y1,x0:x1]; spm=(sp>0.3).astype(np.float32)          # the ink itself, NOT hole-filled: the frame stays open
M=np.float32([[s,0,cx],[0,s,cy]])
ink=cv2.warpAffine(spm,M,(W,H))>0.5
SEXT=ndimage.binary_dilation(ink,iterations=10) & ndimage.binary_dilation(comp>0.5,iterations=10) & GROUPKEEP
SEXT=ndimage.binary_opening(SEXT,iterations=2)
# ---- arms: the rest of the group, split left / right by component centre
arms=GROUPKEEP & ~SEXT
lab,n=ndimage.label(arms)
LEFT=np.zeros_like(arms); RIGHT=np.zeros_like(arms)
for i,sl in enumerate(ndimage.find_objects(lab)):
    r=lab[sl]==i+1
    if r.sum()<400: continue
    yc=(sl[0].start+sl[0].stop)/2
    if yc<650: continue                                   # bits of the eye lines next to the scope
    xc=(sl[1].start+sl[1].stop)/2
    (LEFT if xc<960 else RIGHT)[sl]|=r
LEFT=ndimage.binary_fill_holes(LEFT); RIGHT=ndimage.binary_fill_holes(RIGHT)
SCOPE=np.zeros_like(SEXT); SCOPE[480:610,700:878]=True; SCOPE&=SEXT
def layer(mask, src=comp):
    c=np.where(mask,src,0).astype(np.float32); m=mask.astype(np.float32)
    return c,m
L={'left':layer(LEFT),'sext':layer(SEXT),'sext_ns':layer(SEXT&~SCOPE),'right':layer(RIGHT)}
STAR=cut(s2,(49,817,133,902)); STAR2=cut(s2,(166,826,235,898)); DOT=cut(s2,(268,853,289,873)); AHOGE=cut(s2,(1261,786,1402,914))
def down(c,m):
    """full-res layer -> final res content + binary mask"""
    cs=cv2.resize(c,(FW,FH),interpolation=cv2.INTER_AREA); ms=cv2.resize(m,(FW,FH),interpolation=cv2.INTER_AREA)
    mb=ms>0.5
    cs=np.where(mb, cs/np.maximum(ms,1e-3), 0)              # un-premultiply the edge pixels
    return np.clip(cs,0,1), mb
DL={k:down(*v) for k,v in L.items()}
DH={k:down(*v) for k,v in HEAD.items()}
def _eye_only():
    """The wink sheet is a separate drawing: hair, flower and outline differ by a pixel here and there.
    Take only the eye from it (the densest block of differences), so the wink patch stays small."""
    (oc,om),(wc,wm)=DH['open'],DH['wink']
    d=(np.round(np.where(om,oc,0)*3)!=np.round(np.where(wm,wc,0)*3))|(om!=wm)
    lab,n=ndimage.label(ndimage.binary_closing(d,iterations=2))
    k=1+int(np.argmax(ndimage.sum(d,lab,range(1,n+1))))
    sl=ndimage.find_objects(lab)[k-1]
    y0,y1,x0,x1=max(sl[0].start-2,0),sl[0].stop+2,max(sl[1].start-2,0),sl[1].stop+2
    c,m=oc.copy(),om.copy(); c[y0:y1,x0:x1]=wc[y0:y1,x0:x1]; m[y0:y1,x0:x1]=wm[y0:y1,x0:x1]
    DH['wink']=(c,m); return (x0,y0,x1,y1)
EYE=_eye_only()
def place_full(p,m,cx,cy,s=1.0,ang=0.0):
    h,w=p.shape; M=cv2.getRotationMatrix2D((w/2,h/2),ang,s); M[0,2]+=cx-w/2; M[1,2]+=cy-h/2
    return cv2.warpAffine(p,M,(W,H)), cv2.warpAffine(m,M,(W,H))
def put(canvas, cm, dx=0, dy=0, outline=True):
    c,m=cm
    if dx or dy:
        c=np.roll(np.roll(c,dy,0),dx,1); m=np.roll(np.roll(m,dy,0),dx,1)
        if dy>0: c[:dy]=0; m[:dy]=False
        if dx>0: c[:,:dx]=0; m[:,:dx]=False
    if outline:
        ring=ndimage.binary_dilation(m) & ~m
        canvas[ring]=0                                        # 1 px black border around the part
    canvas[m]=c[m]
def frame(head='open', lift=1.0, ahoge=0.0, tw=(1,1,1,1)):
    cv_=np.zeros((FH,FW),np.float32)
    put(cv_,DH[head],outline=False)
    a=down(*place_full(*AHOGE,394,110,1.52,ahoge)); put(cv_,a)
    dy=int(round((1-lift)*200*SC)); dx=int(round((1-lift)*25*SC))
    put(cv_,DL['left'],dx,dy)
    put(cv_,DL['sext'],dx,dy)                         # eyepiece kept in every frame
    put(cv_,DL['right'],dx,dy)
    for (p,m),(x,y,s),f in zip([STAR,STAR2,DOT,DOT],[(1198,112,1.24),(1330,247,1.24),(1384,91,1.05),(1066,153,1.05)],tw):
        if f>0.05: put(cv_,down(*place_full(p,m,x,y,s*f)),outline=False)
    return cv_
OR=np.array([0xFF,0x8C,0x28],np.float32)
def q2(l): return np.round(np.clip(l,0,1)*3)/3
def rgb(l): return (q2(l)[:,:,None]*OR).astype(np.uint8)
