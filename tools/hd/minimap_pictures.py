"""The opened map's pictures (DATA\\Levels\\<Day|Night|Fog|Custom1>\\<level>.min) as pictures the mod would collect.

    python -I tools/hd/minimap_pictures.py [--write] [--png DIR]

The opened map of a level (the scroll in the top-right corner) is not drawn from the level - every level brings its
own picture of the whole opened scroll, 158x246, in a .min file next to its .map: u16 w, u16 h, u32 format (2 =
bzip2), u32 size, then the compressed RGB565 pixels. The mod recognises an interface picture by its content
(ddm_hd_patch.cpp `Fnv`, the same key as res_pictures.py), so the HD versions can be made without the game.
--write puts the ones without an HD version into hd/work/menu_parts/part_<key>.raw (the mod's own format, colour key
0x07C0, shadow marker 0x001F when the picture has such pixels) for upscale_menu_parts.py; --png writes previews.
"""
import argparse
import bz2
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))      # also with python -I
from res_pictures import KEY, MARKER, NONE, OUT, ROOT, WORK, fnv_key  # noqa: E402

LEVELS = ROOT / 'DATA' / 'Levels'


def read_min(path):
    data = path.read_bytes()
    w, h, fmt, size = struct.unpack_from('<HHII', data, 0)
    body = data[12:12 + size]
    if fmt == 2:
        raw = bz2.decompress(body)
    elif fmt == 1:
        raw = zlib.decompress(body)
    elif fmt == 0:
        raw = body
    else:
        raise ValueError(f'{path}: unknown format {fmt}')
    if len(raw) != w * h * 2:
        raise ValueError(f'{path}: {len(raw)} bytes for {w}x{h}')
    return w, h, raw


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--write', action='store_true')
    ap.add_argument('--png', default='', help='write previews of the pictures into this folder')
    args = ap.parse_args()
    import numpy as np
    done = {p.stem for p in OUT.glob('*.hdp')}
    seen = {}
    for path in sorted(LEVELS.rglob('*')):
        if path.suffix.lower() != '.min':
            continue
        w, h, raw = read_min(path)
        k = f'{fnv_key(raw, w, h):016x}'
        rel = path.relative_to(LEVELS)
        first = k not in seen
        seen.setdefault(k, (w, h, raw, rel))
        print(f'{str(rel):32} {w}x{h} key {k}{"  (HD exists)" if k in done else ""}{"" if first else "  (same as " + str(seen[k][3]) + ")"}')
    print(f'{len(seen)} different pictures, {sum(k in done for k in seen)} with an HD version')
    if args.png:
        from PIL import Image
        out = Path(args.png)
        out.mkdir(parents=True, exist_ok=True)
        for k, (w, h, raw, rel) in seen.items():
            px = np.frombuffer(raw, '<u2', w * h).reshape(h, w).astype(np.uint32)
            rgb = np.stack([((px >> 11) & 31) * 255 // 31, ((px >> 5) & 63) * 255 // 63, (px & 31) * 255 // 31], -1)
            name = str(rel.with_suffix('')).replace('\\', '_').replace('/', '_')
            Image.fromarray(rgb.astype(np.uint8)).save(out / f'{name}_{k}.png')
        print(f'previews in {out}')
    if args.write:
        WORK.mkdir(parents=True, exist_ok=True)
        wrote = 0
        for k, (w, h, raw, rel) in seen.items():
            if k in done:
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
