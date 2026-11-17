"""The game's interface resource file DATA\\Interface\\Default.res ("SRES") - what it holds and its pictures as PNG.

    python -I tools/hd/res_parse.py [--png DIR] [--res PATH]

Layout (worked out 2026-11-17): "SRES", u32 0x100, u32 entry count; every entry = 4-letter type ("BTTN" ...),
u32 id, u32 0, u32 group, then its pictures one after another until the next entry: u16 w, u16 h, u32 format,
u32 size, data: format 0 = w*h RGB565 as is, format 1 = the same zlib-compressed.
"""
import argparse
import re
import struct
import zlib
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TAG = re.compile(rb'[A-Z][A-Z0-9_ ]{3}')


def entries(data):
    magic, ver, count = struct.unpack_from('<4sII', data, 0)
    if magic != b'SRES':
        raise ValueError('not an SRES file')
    off = 12
    while off + 16 <= len(data):
        tag = data[off:off + 4]
        if not TAG.fullmatch(tag):
            raise ValueError(f'no entry type at {off}: {tag!r}')
        ident, zero, group = struct.unpack_from('<III', data, off + 4)
        start = off
        single = tag == b'PIC '          # one picture right after "PIC ", id, 0
        if single:
            group = 1
        off += 12 if single else 16
        pics = []
        while off + 12 <= len(data) and not TAG.fullmatch(data[off:off + 4]) and not (single and pics):
            w, h, fmt, comp = struct.unpack_from('<HHII', data, off)
            body = data[off + 12:off + 12 + comp]
            raw = zlib.decompress(body) if fmt == 1 else body
            pics.append((w, h, fmt, raw))
            off += 12 + comp
        yield start, tag.decode(), ident, zero, group, pics


def rgb565_png(w, h, raw, path):
    import numpy as np
    from PIL import Image
    px = np.frombuffer(raw, '<u2', w * h).reshape(h, w)
    r = ((px >> 11) & 31).astype(np.uint32)
    g = ((px >> 5) & 63).astype(np.uint32)
    b = (px & 31).astype(np.uint32)
    rgb = np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], -1).astype(np.uint8)
    Image.fromarray(rgb).save(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--res', default=str(ROOT / 'DATA' / 'Interface' / 'Default.res'))
    ap.add_argument('--png', default='')
    ap.add_argument('--quiet', action='store_true')
    args = ap.parse_args()
    data = Path(args.res).read_bytes()
    types = Counter()
    out = Path(args.png) if args.png else None
    if out:
        out.mkdir(parents=True, exist_ok=True)
    n = 0
    for start, tag, ident, zero, group, pics in entries(data):
        types[tag] += 1
        n += 1
        if not args.quiet:
            sizes = ', '.join(f'{w}x{h}/{fmt}' for w, h, fmt, raw in pics)
            print(f'{start:8d} {tag} id={ident} group={group} pictures={len(pics)}: {sizes}')
        if out:
            for i, (w, h, fmt, raw) in enumerate(pics):
                if len(raw) >= w * h * 2 > 0:
                    rgb565_png(w, h, raw, out / f'{tag}_{ident:04d}_{i}.png')
    print(f'{n} entries: {dict(types)}')


if __name__ == '__main__':
    main()
