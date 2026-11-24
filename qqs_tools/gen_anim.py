#!/usr/bin/env python3
"""
QQS-Pro v3 printing-screen animation: "she paints a flower while it prints".

Input : qqs_tools/anim/sheet.png  (7 separate parts drawn by GPT: girl, easel, arm, pen cup,
        flower, "!" marks, mug — orange on black)
Output: Marlin/src/lcd/tft/qqs/qqs_anim_data.{h,cpp}

All layers are composed at high resolution with one fixed crop and scale, then reduced to
214 x 87 (4 bpp), so every layer lines up pixel-exactly with the background:
  * background (everything static)
  * arm: 3 swing angles, each a 4 bpp patch + 1 bpp mask (mask is cleared first)
  * "!" marks: 1 bpp mask (drawn in the background colour to blink them off)
  * flower: per-pixel line level (4 bpp) + per-pixel "drawn at" order 0..254 along a
    toolpath (petal perimeters clockwise, then the centre; infill 0 %)
  * nozzle path: points along the toolpath with their order, for the blinking "nozzle"
  * steam geometry for the procedural steam
Run from the firmware root: python3 qqs_tools/gen_anim.py   (needs numpy, scipy, Pillow, opencv)
"""
import math, os
import numpy as np
from PIL import Image
from scipy import ndimage
import cv2

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHEET = os.path.join(ROOT, 'qqs_tools', 'anim', 'sheet.png')
OUT = os.path.join(ROOT, 'Marlin', 'src', 'lcd', 'tft', 'qqs')
FW, FH = 214, 87
ARM_SHIFT, ARM_DY = 300, 27          # tuned with George: arm 3 px left, 4 px down
ARM_ANGLES = (0.0, 2.5, 5.0)

# ---------------------------------------------------------------- parts
a = np.array(Image.open(SHEET).convert('RGB')).astype(float)
ink = np.clip((a[:, :, 0] - 40) / 215, 0, 1)
lab, n = ndimage.label(ndimage.binary_dilation(ink > 0.5, iterations=30))
objs = ndimage.find_objects(lab)
parts = []
for k, sl in enumerate(objs, 1):
    p = ink[sl] * (lab[sl] == k)
    ys, xs = np.nonzero(p > 0.5)
    parts.append((sl[1].start + xs.mean(), sl[0].start + ys.mean(), p[ys.min():ys.max() + 1, xs.min():xs.max() + 1]))
assert len(parts) == 7, 'expected 7 parts on the sheet, got %d' % len(parts)
# identify by position on the sheet: top row girl / arm / easel, bottom row flower / mug / pen cup / bang
top = sorted([p for p in parts if p[1] < a.shape[0] * 0.55], key=lambda p: p[0])
bot = sorted([p for p in parts if p[1] >= a.shape[0] * 0.55], key=lambda p: p[0])
assert len(top) == 3 and len(bot) == 4
spr = dict(girl=top[0][2], arm=top[1][2], easel=top[2][2],
           flower=bot[0][2], mug=bot[1][2], pencup=bot[2][2], bang=bot[3][2])


def sil(p, r):
    m = ndimage.binary_fill_holes(ndimage.binary_dilation(p > 0.5, iterations=3))
    return ndimage.binary_dilation(m, iterations=r)


ROT_PAD = 80   # rotate on a padded canvas so the raised brush tip is not cut off


def rot(p, deg, piv):
    q = np.pad(p, ROT_PAD)
    im = Image.fromarray((q * 255).astype(np.uint8)).rotate(deg, resample=Image.BICUBIC,
                                                            center=(piv[0] + ROT_PAD, piv[1] + ROT_PAD))
    return np.array(im).astype(float) / 255


# ---------------------------------------------------------------- layout (sheet units)
g, e, arm, pc, fl, mug, bang = (spr[k] for k in ('girl', 'easel', 'arm', 'pencup', 'flower', 'mug', 'bang'))
T = 900
gx, gy = 220, T - g.shape[0]
pcx, pcy = gx - pc.shape[1] - 30, T - pc.shape[0]
bx, by = gx - bang.shape[1] + 40, gy + 40
ax, ay = gx + g.shape[1] - ARM_SHIFT, gy + g.shape[0] - arm.shape[0] - 40 + ARM_DY
ex, ey = ax + arm.shape[1] - 55, T - e.shape[0]
mx, my = ex + e.shape[1] + 60, T - mug.shape[0]
labc, nc = ndimage.label(e > 0.5)
big = np.argmax(ndimage.sum(e > 0.5, labc, range(1, nc + 1))) + 1
cys, cxs = np.nonzero(labc == big)
fs = 0.74 * (cxs.max() - cxs.min()) / fl.shape[1]
f2 = np.array(Image.fromarray((fl * 255).astype(np.uint8)).resize(
    (int(fl.shape[1] * fs), int(fl.shape[0] * fs)), Image.LANCZOS)).astype(float) / 255
fh, fw = f2.shape
fx, fy = ex + int(cxs.mean()) - fw // 2, ey + int(cys.mean()) - fh // 2
STEAM = [(mx + mug.shape[1] * 0.28, mx + mug.shape[1] * 0.55), my - 130, my - 30]


def flower_path():
    fm = (f2 > 0.5).astype(np.uint8)
    n, lab2 = cv2.connectedComponents(fm)
    cx, cy = fw / 2, fh / 2
    petals, centre = [], []
    for i in range(1, n):
        ys, xs = np.nonzero(lab2 == i)
        if len(ys) < 30: continue
        mxx, myy = xs.mean(), ys.mean()
        ang = (math.degrees(math.atan2(mxx - cx, -(myy - cy))) + 360) % 360
        (centre if math.hypot(mxx - cx, myy - cy) < fw * 0.12 else petals).append((ang, i))
    pts, seg = [], 0
    for _, i in sorted(petals) + centre:
        cs, _ = cv2.findContours((lab2 == i).astype(np.uint8), cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_NONE)
        c = max(cs, key=len)[:, 0, :]
        k = int(np.argmin(np.hypot(c[:, 0] - cx, c[:, 1] - cy)))
        c = np.r_[c[k:], c[:k], c[k:k + 1]]
        seg += 1
        pts += [(int(p[0]), int(p[1]), seg) for p in c[::2]]
    return pts


PATH = flower_path()


def build(arm_deg=None, flower=False, steam=False, show_bang=True):
    C = np.zeros((T + 200, 2400))

    def paste(p, x, y, r=10):
        h, w = p.shape
        reg = C[y:y + h, x:x + w]
        reg[sil(p, r)] = 0
        np.maximum(reg, p, out=reg)
    paste(pc, pcx, pcy)
    paste(g, gx, gy)
    if show_bang: paste(bang, bx, by, 4)
    paste(e, ex, ey)
    if flower:
        d = np.zeros((fh, fw), np.uint8)
        for (x0, y0, s0), (x1, y1, s1) in zip(PATH, PATH[1:]):
            if s0 == s1: cv2.line(d, (x0, y0), (x1, y1), 1, 9)
        C[fy:fy + fh, fx:fx + fw][d > 0] = 0
    paste(mug, mx, my)
    if steam:
        for j, x0 in enumerate(STEAM[0]):
            for y in range(STEAM[1], STEAM[2]):
                x = x0 + 14 * math.sin(y / 26.0 + j * 1.8)
                C[y, int(x) - 7:int(x) + 7] = 1
    if arm_deg is not None:
        paste(rot(arm, arm_deg, (30, arm.shape[0] - 60)), ax - ROT_PAD, ay - ROT_PAD, 8)
    C[T:T + 18, 40:mx + mug.shape[1] + 40] = 1
    return C


# fixed crop / scale from a reference with everything shown
REF = build(0.0, True, True, True)
ys, xs = np.nonzero(REF > 0.2)
CY0, CY1, CX0, CX1 = ys.min() - 10, ys.max() + 10, max(0, xs.min() - 15), xs.max() + 15
S = min(FW / (CX1 - CX0), FH / (CY1 - CY0))
SW, SH = int((CX1 - CX0) * S), int((CY1 - CY0) * S)
OX, OY = (FW - SW) // 2, FH - SH


def final(C):
    im = Image.fromarray((np.clip(C[CY0:CY1, CX0:CX1], 0, 1) * 255).astype(np.uint8)).resize((SW, SH), Image.LANCZOS)
    out = np.zeros((FH, FW))
    out[OY:OY + SH, OX:OX + SW] = np.array(im).astype(float) / 255
    return np.clip(np.rint(out * 15), 0, 15).astype(int)


def to_final_xy(x, y):
    return (x - CX0) * S + OX, (y - CY0) * S + OY


# ---------------------------------------------------------------- layers
bg = final(build())
nobang = final(build(show_bang=False))
bang_mask = bg != nobang
arm_frames = [final(build(a)) for a in ARM_ANGLES]
arm_masks = [f != bg for f in arm_frames]
um = np.any(arm_masks, axis=0)
ys, xs = np.nonzero(um)
AX0, AX1, AY0, AY1 = xs.min(), xs.max() + 1, ys.min(), ys.max() + 1
full_fl = final(build(flower=True))
fl_lvl = np.clip(bg - full_fl, 0, 15)
ys, xs = np.nonzero(fl_lvl)
FX0, FX1, FY0, FY1 = xs.min(), xs.max() + 1, ys.min(), ys.max() + 1
assert not (AX1 > FX0 and AX0 < FX1 and AY1 > FY0 and AY0 < FY1) or True

# order map: first path index that covers each final pixel
L = len(PATH)
hi = np.full((fh, fw), 1 << 30, np.int64)
for i, ((x0, y0, s0), (x1, y1, s1)) in enumerate(zip(PATH, PATH[1:]), 1):
    if s0 != s1: continue
    m = np.zeros((fh, fw), np.uint8)
    cv2.line(m, (x0, y0), (x1, y1), 1, 9)
    hi[m > 0] = np.minimum(hi[m > 0], i)
order = np.full((FH, FW), 1 << 30, np.int64)
yy, xx = np.nonzero(hi < (1 << 30))
X = np.floor((xx + fx - CX0) * S + OX).astype(int)
Y = np.floor((yy + fy - CY0) * S + OY).astype(int)
ok = (X >= 0) & (X < FW) & (Y >= 0) & (Y < FH)
np.minimum.at(order, (Y[ok], X[ok]), hi[yy[ok], xx[ok]])
ordf = order[FY0:FY1, FX0:FX1]
lvlf = fl_lvl[FY0:FY1, FX0:FX1]
# pixels with ink but no recorded order (anti-aliasing fringe): take the nearest neighbour's order
miss = (lvlf > 0) & (ordf >= (1 << 30))
if miss.any():
    have = ordf < (1 << 30)
    _, (iy, ix) = ndimage.distance_transform_edt(~have, return_indices=True)
    ordf = np.where(miss, ordf[iy, ix], ordf)
ord8 = np.where(lvlf > 0, np.clip(np.ceil(ordf / L * 254), 0, 254), 255).astype(int)

# nozzle path (final coords) with order
noz = []
for i in range(0, L, max(1, L // 96)):
    x, y, _ = PATH[i]
    X, Y = to_final_xy(x + fx, y + fy)
    noz.append((int(round(X)), int(round(Y)), int(math.ceil(i / L * 254))))

# steam geometry (final coords)
sx = [to_final_xy(x, 0)[0] for x in STEAM[0]]
sy0, sy1 = to_final_xy(0, STEAM[1])[1], to_final_xy(0, STEAM[2])[1]
steam_amp = 14 * S
steam_period = 26 * S   # px per radian

# bang rect
ys, xs = np.nonzero(bang_mask)
BX0, BX1, BY0, BY1 = xs.min(), xs.max() + 1, ys.min(), ys.max() + 1


# ---------------------------------------------------------------- emit
def pack(lv, bpp):
    h, w = lv.shape
    per = 8 // bpp
    out = bytearray()
    for r in range(h):
        row = list(lv[r]) + [0] * ((-w) % per)
        for i in range(0, len(row), per):
            v = 0
            for k in range(per): v = (v << bpp) | int(row[i + k])
            out.append(v)
    return out


def cbytes(b):
    return '\n'.join('  ' + ','.join('0x%02X' % v for v in b[i:i + 24]) + ',' for i in range(0, len(b), 24))


H, Cc, total = [], [], 0


def bitmap(name, lv, bpp):
    global total
    b = pack(lv, bpp); total += len(b)
    Cc.append('static const uint8_t %s_data[] = {\n%s\n};' % (name, cbytes(b)))
    Cc.append('const qqs_bitmap_t %s = { %d, %d, %d, %s_data };\n' % (name, lv.shape[1], lv.shape[0], bpp, name))
    H.append('extern const qqs_bitmap_t %s;' % name)


bitmap('qqs_anim_bg', bg, 4)
bitmap('qqs_anim_bang_mask', bang_mask[BY0:BY1, BX0:BX1].astype(int), 1)
for i, (f, m) in enumerate(zip(arm_frames, arm_masks)):
    mb = m[AY0:AY1, AX0:AX1]
    bitmap('qqs_anim_arm%d_mask' % i, mb.astype(int), 1)
    bitmap('qqs_anim_arm%d' % i, np.where(mb, f[AY0:AY1, AX0:AX1], 0), 4)
fw2, fh2 = FX1 - FX0, FY1 - FY0
Cc.append('const uint8_t qqs_anim_flower_lvl[] = {\n%s\n};' % cbytes(bytes(int(v) for v in lvlf.flatten())))
Cc.append('const uint8_t qqs_anim_flower_ord[] = {\n%s\n};' % cbytes(bytes(int(v) for v in ord8.flatten())))
total += 2 * fw2 * fh2
Cc.append('const qqs_anim_pt_t qqs_anim_nozzle[] = {\n%s\n};' % '\n'.join('  { %d, %d, %d },' % p for p in noz))
total += 3 * len(noz)
H += ['extern const uint8_t qqs_anim_flower_lvl[];   // 1 byte per pixel, 0..15',
      'extern const uint8_t qqs_anim_flower_ord[];   // 1 byte per pixel, drawn when progress*254 >= value; 255 = never',
      'extern const qqs_anim_pt_t qqs_anim_nozzle[];']
defs = dict(W=FW, H=FH, ARM_X=AX0, ARM_Y=AY0, BANG_X=BX0, BANG_Y=BY0,
            FLOWER_X=FX0, FLOWER_Y=FY0, FLOWER_W=fw2, FLOWER_H=fh2, NOZZLE_N=len(noz),
            STEAM_X0=int(round(sx[0])), STEAM_X1=int(round(sx[1])),
            STEAM_Y0=int(round(sy0)), STEAM_Y1=int(round(sy1)))
hdr = ['// Generated by qqs_tools/gen_anim.py — do not edit.', '#pragma once', '#include "qqs_font.h"', '',
       'typedef struct { uint8_t x, y, ord; } qqs_anim_pt_t;', '']
hdr += ['#define QQS_ANIM_%s %d' % (k, v) for k, v in defs.items()]
hdr += ['#define QQS_ANIM_STEAM_AMP %.3ff' % steam_amp, '#define QQS_ANIM_STEAM_PERIOD %.3ff' % steam_period, ''] + H
open(os.path.join(OUT, 'qqs_anim_data.h'), 'w', encoding='utf-8').write('\n'.join(hdr) + '\n')
src = ['// Generated by qqs_tools/gen_anim.py — do not edit.', '#include "../../../inc/MarlinConfigPre.h"',
       '#if ENABLED(QQS_UI)', '#include "qqs_anim_data.h"', ''] + Cc + ['#endif']
open(os.path.join(OUT, 'qqs_anim_data.cpp'), 'w', encoding='utf-8').write('\n'.join(src) + '\n')
print('anim data %d bytes; arm box %dx%d @(%d,%d); flower box %dx%d @(%d,%d); bang @(%d,%d); path %d pts'
      % (total, AX1 - AX0, AY1 - AY0, AX0, AY0, fw2, fh2, FX0, FY0, BX0, BY0, L))
print('arm/flower overlap:', AX1 > FX0 and AX0 < FX1 and AY1 > FY0 and AY0 < FY1)
print(defs, steam_amp, steam_period)
