import os
HERE=os.path.dirname(os.path.abspath(__file__))
def _ink(n):
    import cv2, numpy as np
    return cv2.imread(os.path.join(HERE,"src",n))[:,:,2].astype(np.float32)/255.0
import cv2, numpy as np, json, math
from scipy import ndimage
c2=_ink('pid_parts.png'); base=_ink('pid_base.png'); A=json.load(open(os.path.join(HERE,'align_pid.json')))
W,H=1448,1086
CROP=(175,30,1275,855)            # 4:3 window over the scene
FW,FH=160,120
def sil(a,it=4): return ndimage.binary_fill_holes(ndimage.binary_closing(a>0.3,iterations=it))
def cutp(k, fill=True):
    x0,y0,x1,y1=A['parts'][k]['src']; p=c2[y0:y1,x0:x1].copy()
    m=sil(p) if fill else ndimage.binary_dilation(p>0.3,iterations=3)
    p[~m]=0; return p,m.astype(np.float32)
PARTS={k:cutp(k) for k in ('hand','rod','flame','steam','star_a','star_b','dot','ahoge')}
PARTS['steam']=cutp('steam',fill=False)
def centre(k):
    p=A['parts'][k]; return p['x']+p['w']/2, p['y']+p['h']/2, p['s']
POS={k:centre(k) for k in ('hand','flame','steam','star_a','ahoge')}
POS['rod']=(935,545,1.05)
DOTS=[(938,205),(1047,186),(1072,441),(1117,412)]
STAR2=(982,165)
def place_full(p,m,cx,cy,s=1.0,ang=0.0,flip=False):
    if flip: p=p[:,::-1].copy(); m=m[:,::-1].copy()
    h,w=p.shape; M=cv2.getRotationMatrix2D((w/2,h/2),ang,s); M[0,2]+=cx-w/2; M[1,2]+=cy-h/2
    return cv2.warpAffine(p,M,(W,H)), cv2.warpAffine(m,M,(W,H))
def down(c,m):
    x0,y0,x1,y1=CROP
    c=c[y0:y1,x0:x1]; m=m[y0:y1,x0:x1]
    cs=cv2.resize(c,(FW,FH),interpolation=cv2.INTER_AREA); ms=cv2.resize(m,(FW,FH),interpolation=cv2.INTER_AREA)
    mb=ms>0.5; cs=np.where(mb,cs/np.maximum(ms,1e-3),0); return np.clip(cs,0,1),mb
def put(cv_,cm,outline=True):
    c,m=cm
    if outline: cv_[ndimage.binary_dilation(m)&~m]=0
    cv_[m]=c[m]
BASE=down(base,np.ones_like(base))[0]
def frame(ahoge=0.0, ang=0.0, flame=None, flip=False, steam=None, s1=1.0, s2=1.0):
    """flame: size 0..1 or None; steam: rise phase 0..1 or None; s1/s2: star sizes"""
    cv_=BASE.copy()
    x,y,s=POS['ahoge']; put(cv_,down(*place_full(*PARTS['ahoge'],x,y,s,ahoge)))
    WX,WY=852,500                                                         # wrist: hand + rod turn around it
    R=cv2.getRotationMatrix2D((WX,WY),ang,1.0)
    rx,ry,rs=POS['rod']; pr,mr=place_full(*PARTS['rod'],rx,ry,rs)
    put(cv_,down(cv2.warpAffine(pr,R,(W,H)),cv2.warpAffine(mr,R,(W,H))))
    hx,hy,hs=POS['hand']; ph_,mh_=place_full(*PARTS['hand'],hx,hy,hs)
    put(cv_,down(cv2.warpAffine(ph_,R,(W,H)),cv2.warpAffine(mh_,R,(W,H))))
    if flame:
        fx,fy,fs=POS['flame']
        put(cv_,down(*place_full(*PARTS['flame'],fx,fy+(1-flame)*30,fs*flame,0,flip=flip)))
    if steam is not None:
        sx,sy,ss=POS['steam']
        put(cv_,down(*place_full(*PARTS['steam'],sx+6*math.sin(steam*6.28),sy-40*steam,ss*(0.8+0.3*steam))),outline=False)
    sp,sm=PARTS['star_a']
    for (x,y),f in [((POS['star_a'][0],POS['star_a'][1]),s1),(STAR2,s2)]:
        put(cv_,down(*place_full(sp,sm,x,y,f*1.1)),outline=False)
    dp,dm=PARTS['dot']
    for (x,y) in DOTS: put(cv_,down(*place_full(dp,dm,x,y,1.1)),outline=False)
    return cv_
OR=np.array([0xFF,0x8C,0x28],np.float32)
def q2(l): return np.round(np.clip(l,0,1)*3)/3
def rgb(l): return (q2(l)[:,:,None]*OR).astype(np.uint8)
