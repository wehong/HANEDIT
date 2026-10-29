#!/usr/bin/env python3
"""Build HANJA.DIC (Hanja dictionary) for HANEDIT.

Usage: python3 genhanja.py hanja.txt freq-hanjaeo.txt HANJA.DIC

Input : libhangul data/hanja/hanja.txt       ("key:hanja:meaning" per line)
        libhangul data/hanja/freq-hanjaeo.txt ("hanja-word:NXXXXXX", optional)
Output: little-endian binary, two parts

  header (24 bytes)
    "HJD2"
    u32 syl_off, u32 syl_len        one-syllable part (loaded into memory)
    u32 widx_off, u32 nwords        word index (searched on disk)
    u32 0
  one-syllable part (same layout as the old HJD1 file)
    "HJD1", u16 nkeys, u16 0
    nkeys x { u16 syllable, u16 count, u32 offset }   sorted by syllable
    data: per candidate { u16 hanja, u8 len, len bytes UTF-8 meaning }
  word index
    nwords x u32 record offset (from file start), sorted by key (code units)
  word records
    u8 keylen, keylen x u16 key, u8 count, count x (keylen x u16 hanja)

Only Hangul-syllable keys are kept, and only Hanja the built-in font can
show (U+4E00..U+9FFF, U+F900..U+FAFF). One-syllable candidates keep the
order of hanja.txt (most common first); word candidates are sorted by
freq-hanjaeo.txt (most common first).
"""
import struct
import sys
from collections import OrderedDict

MAX_WORD = 12


def displayable(cp):
    return 0x4E00 <= cp <= 0x9FFF or 0xF900 <= cp <= 0xFAFF


def is_syl(c):
    return 0xAC00 <= ord(c) <= 0xD7A3


def main(src, freq_path, dst):
    freq = {}
    if freq_path != '-':
        for line in open(freq_path, encoding='utf-8'):
            p = line.strip().split(':')
            if len(p) == 2 and p[1].isdigit():
                freq[p[0]] = int(p[1]) % 1000000

    syl = OrderedDict()
    words = OrderedDict()
    for line in open(src, encoding='utf-8'):
        if line.startswith('#'):
            continue
        p = line.rstrip('\n').split(':')
        if len(p) < 2 or not p[0] or len(p[0]) != len(p[1]):
            continue
        key, hz = p[0], p[1]
        if not all(is_syl(c) for c in key) or not all(displayable(ord(c)) for c in hz):
            continue
        if len(key) == 1:
            meaning = p[2].strip() if len(p) > 2 else ''
            lst = syl.setdefault(ord(key), [])
            if any(h == ord(hz) for h, _ in lst):
                continue
            m = meaning.encode('utf-8')
            while len(m) > 255:
                meaning = meaning[:-1]
                m = meaning.encode('utf-8')
            lst.append((ord(hz), m))
        elif len(key) <= MAX_WORD:
            lst = words.setdefault(key, [])
            if hz not in lst and len(lst) < 255:
                lst.append(hz)

    # --- one-syllable part (HJD1 layout)
    keys = sorted(syl)
    data = bytearray()
    index = bytearray()
    for k in keys:
        index += struct.pack('<HHI', k, len(syl[k]), len(data))
        for hz, m in syl[k]:
            data += struct.pack('<HB', hz, len(m)) + m
    sylpart = b'HJD1' + struct.pack('<HH', len(keys), 0) + index + data

    # --- word part
    wkeys = sorted(words, key=lambda k: [ord(c) for c in k])
    hdr_len = 24
    syl_off = hdr_len
    widx_off = syl_off + len(sylpart)
    rec_base = widx_off + 4 * len(wkeys)
    recs = bytearray()
    widx = bytearray()
    ncand = 0
    for k in wkeys:
        cands = words[k]
        order = {h: i for i, h in enumerate(cands)}
        cands = sorted(cands, key=lambda h: (-freq.get(h, 0), order[h]))
        widx += struct.pack('<I', rec_base + len(recs))
        recs += struct.pack('<B', len(k)) + b''.join(struct.pack('<H', ord(c)) for c in k)
        recs += struct.pack('<B', len(cands))
        for h in cands:
            recs += b''.join(struct.pack('<H', ord(c)) for c in h)
        ncand += len(cands)

    with open(dst, 'wb') as o:
        o.write(b'HJD2' + struct.pack('<IIIII', syl_off, len(sylpart), widx_off, len(wkeys), 0))
        o.write(sylpart)
        o.write(widx)
        o.write(recs)
    total = hdr_len + len(sylpart) + len(widx) + len(recs)
    print('syllables=%d (%d candidates)  words=%d (%d candidates)  bytes=%d'
          % (len(keys), sum(len(v) for v in syl.values()), len(wkeys), ncand, total))


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2], sys.argv[3])
