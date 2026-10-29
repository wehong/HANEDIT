#!/usr/bin/env python3
"""Generate the built-in font for HANEDIT (src/font.c, src/fontd0..4.c).

Usage:
  python3 genfont.py UNIFONT.hex GALMURI14.bdf GALMURIMONO11.bdf GALMURI11-CONDENSED.bdf OUT/font.c
  python3 genfont.py ... --preview preview.png "sample text"

Glyph sources (each cell is 8x16 or 16x16):
  * Hangul syllables and compatibility jamo (narrow cells, same width as ASCII)
        -> Galmuri11-Condensed (7px wide design), placed in the 8x16 cell
  * Hanja, kana, CJK punctuation, fullwidth forms (wide cells)
        -> Galmuri14 (14x14 design), placed in the 16x16 cell
  * ASCII 0x21-0x7E (narrow cells)
        -> GalmuriMono11 (5px wide design), placed in the 8x16 cell
  * everything else (box drawing, arrows, Latin-1, Greek, ...)
        -> GNU Unifont, so that the UI frame lines still connect
Cell width follows Unifont, except Hangul, which is made narrow (1 cell)
so a Hangul syllable takes the same width as a Latin letter.
If Galmuri has no glyph, Unifont is used.

A second, small table (src/fonta*.c, "font_alt_*") holds the same Hangul
from Galmuri14 in wide (2-cell) form; the editor switches to it at run
time from the font menu.
"""
import sys

NFILES = 5   # src/fontd0.c .. fontd4.c

BLOCKS = [
    (0x0020, 0x007E),  # ASCII
    (0x00A0, 0x017F),  # Latin-1 Supplement, Latin Extended-A
    (0x0370, 0x03FF),  # Greek
    (0x0400, 0x045F),  # Cyrillic (basic)
    (0x2010, 0x206F),  # General Punctuation
    (0x20A0, 0x20BF),  # Currency
    (0x2100, 0x215F),  # Letterlike, number forms
    (0x2160, 0x218F),  # Roman numerals
    (0x2190, 0x22FF),  # Arrows, Math operators
    (0x2460, 0x24FF),  # Enclosed alphanumerics
    (0x2500, 0x25FF),  # Box drawing, blocks, geometric shapes
    (0x2600, 0x26FF),  # Misc symbols
    (0x3000, 0x303F),  # CJK symbols & punctuation
    (0x3040, 0x30FF),  # Hiragana, Katakana
    (0x3130, 0x318F),  # Hangul Compatibility Jamo
    (0x3200, 0x33FF),  # Enclosed CJK, CJK compatibility
    (0x4E00, 0x9FFF),  # CJK Unified Ideographs (Hanja)
    (0xAC00, 0xD7A3),  # Hangul Syllables
    (0xF900, 0xFAFF),  # CJK Compatibility Ideographs
    (0xFF01, 0xFFEE),  # Halfwidth & Fullwidth forms
]

# narrow (1-cell) Hangul taken from Galmuri11-Condensed
HANGUL_NARROW = [(0xAC00, 0xD7A3), (0x3131, 0x318E)]

# wide cells taken from Galmuri14
GALMURI_WIDE = [
    (0x3000, 0x303F), (0x3040, 0x30FF), (0x3130, 0x318F), (0x3200, 0x33FF),
    (0x4E00, 0x9FFF), (0xAC00, 0xD7A3), (0xF900, 0xFAFF), (0xFF01, 0xFF60),
    (0xFFE0, 0xFFE6),
]
WIDE_BASELINE = 15     # rows above baseline in the 16x16 cell (Hangul: rows 1..14)
NARROW_BASELINE = 14   # GalmuriMono11 ASCII: caps rows 3..13, descenders 14..15


def load_hex(path):
    g = {}
    for line in open(path):
        line = line.strip()
        if ':' not in line:
            continue
        cp, data = line.split(':', 1)
        cp = int(cp, 16)
        if cp <= 0xFFFF and len(data) in (32, 64):
            g[cp] = bytes.fromhex(data)
    return g


def load_bdf(path):
    g = {}
    rows = None
    for line in open(path, encoding='latin-1'):
        t = line.split()
        if not t:
            continue
        k = t[0]
        if k == 'ENCODING':
            enc = int(t[-1])
        elif k == 'DWIDTH':
            dw = int(t[1])
        elif k == 'BBX':
            bbx = tuple(int(x) for x in t[1:5])
        elif k == 'BITMAP':
            rows = []
        elif k == 'ENDCHAR':
            if 0 <= enc <= 0xFFFF:
                g[enc] = (dw, bbx, rows)
            rows = None
        elif rows is not None:
            rows.append((int(k, 16), len(k) * 4))
    return g


def bdf_cell(glyph, cw, baseline, xoff):
    """Render a BDF glyph into a cw x 16 cell; returns bytes or None if it doesn't fit."""
    dw, (bw, bh, bx, by), rows = glyph
    out = [0] * 16
    top = baseline - (by + bh)
    for r, (v, nbits) in enumerate(rows):
        y = top + r
        for c in range(bw):
            if v >> (nbits - 1 - c) & 1:
                x = xoff + bx + c
                if not (0 <= y < 16 and 0 <= x < cw):
                    return None
                out[y] |= 1 << (cw - 1 - x)
    nb = cw // 8
    return b''.join(v.to_bytes(nb, 'big') for v in out)


def build(uni_path, gal14_path, mono11_path, cond11_path):
    uni = load_hex(uni_path)
    g14 = load_bdf(gal14_path)
    m11 = load_bdf(mono11_path)
    c11 = load_bdf(cond11_path)
    glyphs, src = {}, {'unifont': 0, 'galmuri14': 0, 'galmurimono11': 0,
                       'galmuri11-condensed': 0}
    for cp in sorted(uni):
        if not any(a <= cp <= b for a, b in BLOCKS):
            continue
        base = uni[cp]
        wide = len(base) == 32
        cell = None
        if cp in c11 and any(a <= cp <= b for a, b in HANGUL_NARROW):
            cell = bdf_cell(c11[cp], 8, NARROW_BASELINE, 0)
            if cell:
                src['galmuri11-condensed'] += 1
                glyphs[cp] = cell
                continue
        if wide and cp in g14 and any(a <= cp <= b for a, b in GALMURI_WIDE):
            dw, bbx = g14[cp][0], g14[cp][1]
            xoff = (16 - dw) // 2 if dw < 15 else 0     # center narrower glyphs
            cell = bdf_cell(g14[cp], 16, WIDE_BASELINE, xoff)
            if cell:
                src['galmuri14'] += 1
        elif not wide and 0x21 <= cp <= 0x7E and cp in m11:
            dw = m11[cp][0]
            cell = bdf_cell(m11[cp], 8, NARROW_BASELINE, 1)   # 5px design + 1px margin
            if cell:
                src['galmurimono11'] += 1
        if cell is None:
            cell = base
            src['unifont'] += 1
        glyphs[cp] = cell

    alt = {}                            # wide Hangul (Galmuri14) for the font menu
    for cp in sorted(glyphs):
        if any(a <= cp <= b for a, b in HANGUL_NARROW) and cp in g14:
            dw = g14[cp][0]
            xoff = (16 - dw) // 2 if dw < 15 else 0
            cell = bdf_cell(g14[cp], 16, WIDE_BASELINE, xoff)
            if cell:
                alt[cp] = cell
    src['alt-galmuri14-hangul'] = len(alt)
    return glyphs, alt, src


def write_c(glyphs, dst, prefix='font', chunk='fd', nfiles=NFILES):
    cps = sorted(glyphs)
    ranges = []
    for cp in cps:
        wide = len(glyphs[cp]) == 32
        if ranges and ranges[-1][1] == cp - 1 and ranges[-1][2] == wide:
            ranges[-1][1] = cp
        else:
            ranges.append([cp, cp, wide])

    data = bytearray()
    out_ranges = []
    for a, b, wide in ranges:
        if wide and len(data) % 32:
            data += bytes(16)          # keep 16x16 glyphs inside one 64K chunk
        out_ranges.append((a, b, wide, len(data)))
        for cp in range(a, b + 1):
            data += glyphs[cp]

    CH = 65536
    nch = (len(data) + CH - 1) // CH
    with open(dst, 'w') as o:
        o.write('/* Generated by tools/genfont.py from Galmuri14 / GalmuriMono11\n'
                ' * (SIL OFL 1.1) and GNU Unifont (GPLv2+ w/ font embedding\n'
                ' * exception / SIL OFL 1.1). Do not edit. */\n')
        o.write('#include "font.h"\n\n')
        o.write('const FontRange %s_ranges[%d] = {\n' % (prefix, len(out_ranges)))
        for a, b, wide, off in out_ranges:
            o.write('  {0x%04X,0x%04X,%d,%luUL},\n' % (a, b, 1 if wide else 0, off))
        o.write('};\n')
        o.write('const int %s_nranges = %d;\n\n' % (prefix, len(out_ranges)))
        for c in range(nch):
            o.write('extern const unsigned char %s%d[];\n' % (chunk, c))
        o.write('const unsigned char *const %s_chunks[%d] = {%s};\n'
                % (prefix, nch, ','.join('%s%d' % (chunk, c) for c in range(nch))))

    per = (nch + nfiles - 1) // nfiles
    base = dst[:-2]
    for fi in range(nfiles):
        with open('%sd%d.c' % (base, fi), 'w') as o:
            o.write('/* Generated by tools/genfont.py. Do not edit. */\n')
            for c in range(fi * per, min(nch, (fi + 1) * per)):
                part = data[c * CH:(c + 1) * CH]
                o.write('const unsigned char %s%d[%d] = {\n' % (chunk, c, len(part)))
                for i in range(0, len(part), 24):
                    o.write(','.join(str(x) for x in part[i:i + 24]) + ',\n')
                o.write('};\n')
    return len(out_ranges), len(data)


def preview(glyphs, png, text, scale=2):
    from PIL import Image
    lines = text.split('\\n')
    w = max(sum(len(glyphs.get(ord(c), b'\0' * 16)) // 2 for c in l) for l in lines)
    img = Image.new('L', (w, 16 * len(lines)), 255)
    px = img.load()
    for li, l in enumerate(lines):
        x = 0
        for ch in l:
            b = glyphs.get(ord(ch), bytes(16))
            cw = len(b) // 2
            for r in range(16):
                v = int.from_bytes(b[r * cw // 8:(r + 1) * cw // 8], 'big')
                for c in range(cw):
                    if v >> (cw - 1 - c) & 1:
                        px[x + c, li * 16 + r] = 0
            x += cw
    img.resize((img.width * scale, img.height * scale), Image.NEAREST).save(png)


if __name__ == '__main__':
    a = sys.argv[1:]
    glyphs, alt, src = build(a[0], a[1], a[2], a[3])
    print('glyphs=%d sources=%s' % (len(glyphs), src))
    if len(a) > 5 and a[5] == '--preview':
        preview(glyphs, a[6], a[7])
    else:
        nr, nb = write_c(glyphs, a[4])
        print('main: ranges=%d bytes=%d' % (nr, nb))
        altdst = a[4][:-2] + 'a.c'          # src/fonta.c, fontad0.c, fontad1.c
        nr, nb = write_c(alt, altdst, prefix='font_alt', chunk='fa', nfiles=2)
        print('alt:  ranges=%d bytes=%d' % (nr, nb))
