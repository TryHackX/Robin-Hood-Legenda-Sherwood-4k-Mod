"""Every picture of the interface resource file (DATA\\Interface\\Default.res) as a picture the mod would collect.

    python -I tools/hd/res_pictures.py [--write] [--min 16] [--sheet PNG]

The mod recognises a picture of a menu or dialog by its content (ddm_hd_patch.cpp `Fnv`: FNV-1a 64 over the
RGB565 bytes row by row, then the size mixed in) and looks for hd/menu_parts/<key>.hdp. The resource file keeps
its pictures as RGB565 too (res_parse.py), so their keys can be worked out without the game showing them - every
castle of the campaign map, every flag and briefing portrait, not only the ones a playthrough happened to show.

Found by a scan for picture headers (u16 w, u16 h, u32 format 0/1, u32 size, data whose length is w*h*2).
--write [--sizes WxH,...] puts the ones without an HD version into hd/work/menu_parts/part_<key>.raw (the mod's own format;
colour key 0x07C0, shadow marker 0x001F when the picture has such pixels) for upscale_menu_parts.py.
"""
import argparse
import struct
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RES = ROOT / 'DATA' / 'Interface' / 'Default.res'
WORK = ROOT / 'hd' / 'work' / 'menu_parts'
OUT = ROOT / 'hd' / 'menu_parts'
KEY = 0x07C0
MARKER = 0x001F
NONE = 0xFFFFFFFF


def fnv_key(raw, w, h):
    v = 1469598103934665603
    for b in raw[:w * h * 2]:
        v = ((v ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    v ^= (w << 40) ^ (h << 20)
    return (v * 1099511628211) & 0xFFFFFFFFFFFFFFFF


def scan(data):
    """(offset, w, h, raw RGB565) of every picture in the file."""
    found = []
    o, end = 12, len(data) - 12
    while o < end:
        w, h, fmt, size = struct.unpack_from('<HHII', data, o)
        if 2 <= w <= 1024 and 2 <= h <= 1024 and size > 0 and o + 12 + size <= len(data):
            body = data[o + 12:o + 12 + size]
            raw = None
            if fmt == 1 and body[:1] == b'\x78':
                try:
                    raw = zlib.decompress(body)
                except zlib.error:
                    raw = None
            elif fmt == 0 and size == w * h * 2:
                raw = body
            if raw is not None and len(raw) == w * h * 2:
                found.append((o, w, h, raw))
                o += 12 + size
                continue
        o += 1
    return found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--res', default=str(RES))
    ap.add_argument('--write', action='store_true')
    ap.add_argument('--min', type=int, default=16, help='skip pictures with a side shorter than this')
    ap.add_argument('--sizes', default='', help='only pictures of these sizes: 126x111,125x112,...')
    ap.add_argument('--max-area', type=int, default=640 * 480 - 1,
                    help='skip larger pictures (the full-screen menu backgrounds have HD versions of their own)')
    args = ap.parse_args()
    want = {tuple(int(v) for v in t.split('x')) for t in args.sizes.split(',') if t}
    import numpy as np
    pics = scan(Path(args.res).read_bytes())
    collected = {p.stem[5:] for p in WORK.glob('part_*.raw')}
    done = {p.stem for p in OUT.glob('*.hdp')}
    keys = []
    for o, w, h, raw in pics:
        k = f'{fnv_key(raw, w, h):016x}'
        keys.append(k)
    known = [k for k in keys if k in collected]
    print(f'{len(pics)} pictures; {len(known)} of them collected by the mod so far, '
          f'{sum(k in done for k in keys)} with an HD version')
    print(f'collected pictures found in the file: {len(known)} of {len(collected)}')
    if not args.write:
        return
    WORK.mkdir(parents=True, exist_ok=True)
    wrote = 0
    for (o, w, h, raw), k in zip(pics, keys):
        if min(w, h) < args.min or w * h > args.max_area or k in done or k in collected or (want and (w, h) not in want):
            continue
        px = np.frombuffer(raw, '<u2', w * h)
        marker = MARKER if (px == MARKER).any() else NONE
        with open(WORK / f'part_{k}.raw', 'wb') as f:
            f.write(struct.pack('<5I', 0x504D4253, w, h, KEY, marker))
            f.write(raw)
        wrote += 1
    print(f'wrote {wrote} pictures to {WORK}')


if __name__ == '__main__':
    main()
