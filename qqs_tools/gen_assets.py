#!/usr/bin/env python3
"""
QQS-Pro v3 UI asset generator.

Builds Marlin/src/lcd/tft/qqs/qqs_assets.{h,cpp}:
  * 4 bitmap fonts (2 bpp, anti-aliased) from Noto Sans CJK TC Medium, holding only the
    characters the UI source actually uses (plus printable ASCII),
  * Tabler icons (MIT) at 28 px and 18 px, 2 bpp,
  * the two 1 bpp boot pictures.

Run from the firmware root:  python3 qqs_tools/gen_assets.py
Needs: Pillow, numpy, cairosvg, and the Noto Sans CJK fonts (fonts-noto-cjk).
"""
import io, os, re, sys
import numpy as np
from PIL import Image, ImageFont
import cairosvg

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(ROOT, 'qqs_tools')
OUT_DIR = os.path.join(ROOT, 'Marlin', 'src', 'lcd', 'tft', 'qqs')
UI_SOURCES = ['qqs_ui.cpp', 'qqs_draw.cpp', 'qqs_motion.cpp']

FONT_FILE = '/usr/share/fonts/opentype/noto/NotoSansCJK-Medium.ttc'
FONT_INDEX = 3   # Noto Sans CJK TC Medium

# name, pixel size, line height, baseline (from top of line)
FONTS = [
    ('s',  12, 16, 13),
    ('m',  16, 21, 17),
    ('l',  22, 28, 23),
    ('xl', 40, 46, 39),
]
XL_CHARS = ' 0123456789%°.+-−:m?'

ICONS = sorted(f[:-4] for f in os.listdir(os.path.join(TOOLS, 'icons')) if f.endswith('.svg'))
ICON_SIZES = [(28, 2.0), (18, 2.4)]   # (px, stroke width in the 24-unit viewBox)


L_EXTRA = '−關°%'   # used by F_L through variables (editor buttons, "off" values)


def collect_chars():
    """All non-ASCII chars in string literals; separately those on lines that use F_L."""
    chars, large = set(), set(L_EXTRA)
    lit_re = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
    for fn in UI_SOURCES:
        for line in open(os.path.join(OUT_DIR, fn), encoding='utf-8'):
            for lit in lit_re.findall(line):
                na = set(c for c in lit if ord(c) > 0x7E)
                chars |= na
                if 'F_L' in line: large |= na
    bad = [c for c in chars if ord(c) > 0xFFFF]
    if bad:
        sys.exit('characters outside the BMP: %r' % bad)
    return chars, large


def pack(levels, w, h, bpp):
    """levels: h x w array of ints in [0, 2**bpp - 1]; MSB first, rows padded to bytes."""
    per = 8 // bpp
    out = bytearray()
    for r in range(h):
        row = list(levels[r]) + [0] * ((-w) % per)
        for i in range(0, len(row), per):
            v = 0
            for k in range(per):
                v = (v << bpp) | int(row[i + k])
            out.append(v)
    return out


def quant2(alpha):
    return np.clip(np.rint(alpha.astype(float) * 3 / 255), 0, 3).astype(int)


def build_font(name, size, line_h, baseline, chars):
    font = ImageFont.truetype(FONT_FILE, size, index=FONT_INDEX)
    glyphs, blob = [], bytearray()
    for ch in sorted(chars, key=ord):
        adv = int(round(font.getlength(ch)))
        bbox = font.getbbox(ch, anchor='ls')
        mask = font.getmask(ch, mode='L', anchor='ls')
        w, h = mask.size
        if w == 0 or h == 0 or bbox[2] <= bbox[0]:
            glyphs.append((ord(ch), 0, 0, 0, 0, adv, 0))
            continue
        a = np.array(mask, dtype=np.uint8).reshape(h, w)
        lv = quant2(a)
        # trim empty rows/cols left by the rasteriser
        rows = np.nonzero(lv.any(1))[0]; cols = np.nonzero(lv.any(0))[0]
        if len(rows) == 0:
            glyphs.append((ord(ch), 0, 0, 0, 0, adv, 0)); continue
        lv = lv[rows[0]:rows[-1] + 1, cols[0]:cols[-1] + 1]
        top = baseline + bbox[1] + rows[0]
        xoff = bbox[0] + cols[0]
        if top < 0:                      # clip anything above the line box
            lv = lv[-top:]; top = 0
        if top + lv.shape[0] > line_h:   # and below it
            lv = lv[:line_h - top]
        gh, gw = lv.shape
        if gh <= 0:
            glyphs.append((ord(ch), 0, 0, 0, 0, adv, 0)); continue
        off = len(blob)
        blob += pack(lv, gw, gh, 2)
        glyphs.append((ord(ch), gw, gh, xoff, top, adv, off))
    if len(blob) > 0xFFFF:
        sys.exit('font %s data too large for 16-bit offsets' % name)
    return glyphs, blob


def render_icon(svg_path, px, stroke):
    svg = open(svg_path, encoding='utf-8').read()
    svg = svg.replace('currentColor', '#000000')
    svg = re.sub(r'stroke-width="[0-9.]+"', 'stroke-width="%s"' % stroke, svg)
    png = cairosvg.svg2png(bytestring=svg.encode(), output_width=px, output_height=px)
    a = np.array(Image.open(io.BytesIO(png)).convert('RGBA'))[:, :, 3]
    return quant2(a)


def boot_1bpp(path, width, thresh, ink_is_dark):
    im = Image.open(path).convert('RGBA')
    a = np.array(im).astype(float)
    lum = a[:, :, :3].mean(2); alpha = a[:, :, 3] / 255
    if ink_is_dark:
        ink = ((lum < 128) & (alpha > 0.5)).astype(float)
    else:
        ink = ((a[:, :, 0] > 128) & (a[:, :, 1] > 60)).astype(float)
    ys, xs = np.nonzero(ink)
    ink = ink[ys.min():ys.max() + 1, xs.min():xs.max() + 1]
    h = round(ink.shape[0] * width / ink.shape[1])
    small = Image.fromarray((ink * 255).astype(np.uint8)).resize((width, h), Image.BOX)
    return (np.array(small) > 255 * thresh).astype(int)


def c_bytes(b, indent='  '):
    lines = []
    for i in range(0, len(b), 20):
        lines.append(indent + ','.join('0x%02X' % v for v in b[i:i + 20]) + ',')
    return '\n'.join(lines)


def main():
    used, large = collect_chars()
    ascii_set = set(chr(c) for c in range(0x20, 0x7F))
    font_chars = {
        's': ascii_set | used,
        'm': ascii_set | used,
        'l': ascii_set | large,
        'xl': set(XL_CHARS),
    }

    h = ['// Generated by qqs_tools/gen_assets.py — do not edit.', '#pragma once', '#include "qqs_font.h"', '']
    c = ['// Generated by qqs_tools/gen_assets.py — do not edit.',
         '// Fonts: Noto Sans CJK TC (SIL OFL 1.1). Icons: Tabler Icons (MIT).',
         '#include "../../../inc/MarlinConfigPre.h"', '#if ENABLED(QQS_UI)', '#include "qqs_assets.h"', '']
    total = 0
    for name, size, lh, base in FONTS:
        glyphs, blob = build_font(name, size, lh, base, font_chars[name])
        total += len(blob) + 9 * len(glyphs)
        c.append('static const uint8_t font_%s_data[] = {\n%s\n};' % (name, c_bytes(blob)))
        c.append('static const qqs_glyph_t font_%s_glyphs[] = {' % name)
        for g in glyphs:
            c.append('  { 0x%04X, %d, %d, %d, %d, %d, %d },' % g)
        c.append('};')
        c.append('const qqs_font_t qqs_font_%s = { %d, %d, font_%s_glyphs, font_%s_data };\n' % (name, lh, len(glyphs), name, name))
        h.append('extern const qqs_font_t qqs_font_%s;' % name)
        print('font %-2s %2dpx: %3d glyphs, %6d bytes' % (name, size, len(glyphs), len(blob) + 9 * len(glyphs)))

    h.append('')
    icon_bytes = 0
    for px, stroke in ICON_SIZES:
        for n in ICONS:
            lv = render_icon(os.path.join(TOOLS, 'icons', n + '.svg'), px, stroke)
            b = pack(lv, px, px, 2)
            icon_bytes += len(b)
            ident = 'qqs_i%d_%s' % (px, n.replace('-', '_'))
            c.append('static const uint8_t %s_data[] = {\n%s\n};' % (ident, c_bytes(b)))
            c.append('const qqs_bitmap_t %s = { %d, %d, 2, %s_data };\n' % (ident, px, px, ident))
            h.append('extern const qqs_bitmap_t %s;' % ident)
    print('icons: %d bytes' % icon_bytes)
    total += icon_bytes

    h.append('')
    for ident, fn, w, th, dark in [('qqs_boot_flsun', 'flsun.png', 150, 0.45, True),
                                   ('qqs_boot_girl', 'girl.png', 128, 0.60, False)]:
        lv = boot_1bpp(os.path.join(TOOLS, 'boot', fn), w, th, dark)
        b = pack(lv, lv.shape[1], lv.shape[0], 1)
        total += len(b)
        c.append('static const uint8_t %s_data[] = {\n%s\n};' % (ident, c_bytes(b)))
        c.append('const qqs_bitmap_t %s = { %d, %d, 1, %s_data };\n' % (ident, lv.shape[1], lv.shape[0], ident))
        h.append('extern const qqs_bitmap_t %s;' % ident)
        print('boot %s: %dx%d, %d bytes' % (ident, lv.shape[1], lv.shape[0], len(b)))

    c.append('#endif // QQS_UI')
    open(os.path.join(OUT_DIR, 'qqs_assets.h'), 'w', encoding='utf-8').write('\n'.join(h) + '\n')
    open(os.path.join(OUT_DIR, 'qqs_assets.cpp'), 'w', encoding='utf-8').write('\n'.join(c) + '\n')
    print('CJK/symbol chars used: %d; total asset data ≈ %d bytes' % (len(used), total))


if __name__ == '__main__':
    main()
