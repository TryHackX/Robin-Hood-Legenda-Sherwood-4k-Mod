"""Contact sheets of the pictures in DATA/Interface/Default.res, grouped by size (res_pictures.py finds them).

    python -I tools/hd/res_sheet.py OUTDIR [--sizes 120x160,107x132 ...] [--all]
"""
import argparse
import os
import sys
from collections import defaultdict
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parent))
from res_pictures import scan, fnv_key, RES, OUT  # noqa: E402


def to_image(w, h, raw):
    px = np.frombuffer(raw, '<u2', w * h).reshape(h, w)
    r = ((px >> 11) & 31) * 255 // 31
    g = ((px >> 5) & 63) * 255 // 63
    b = (px & 31) * 255 // 31
    rgb = np.stack([r, g, b], -1).astype(np.uint8)
    rgb[px == 0x07C0] = [60, 60, 60]
    rgb[px == 0x001F] = [0, 0, 120]
    return Image.fromarray(rgb)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--sizes', default='')
    ap.add_argument('--all', action='store_true', help='every picture of a group, not 8')
    ap.add_argument('--cell', type=int, default=140)
    args = ap.parse_args()
    want = {tuple(int(v) for v in s.split('x')) for s in args.sizes.split(',') if s}
    done = {p.stem for p in OUT.glob('*.hdp')}
    groups = defaultdict(list)
    for o, w, h, raw in scan(RES.read_bytes()):
        if want and (w, h) not in want:
            continue
        if not want and (min(w, h) < 20 or w * h < 900 or max(w, h) > 700):
            continue
        groups[(w, h)].append((o, raw))
    os.makedirs(args.out, exist_ok=True)
    width, height = 1600, 2400
    sheets, cur, y = [], Image.new('RGB', (width, height), (25, 25, 25)), 0
    draw = ImageDraw.Draw(cur)
    for (w, h), lst in sorted(groups.items(), key=lambda t: t[1][0][0]):
        sample = lst if args.all else lst[:8]
        scale = min(1.0, args.cell / max(w, h))
        ims = [to_image(w, h, raw).resize((max(1, int(w * scale)), max(1, int(h * scale)))) for o, raw in sample]
        per_row = max(1, (width - 8) // (ims[0].width + 4))
        rows = (len(ims) + per_row - 1) // per_row
        need = 14 + rows * (ims[0].height + 4)
        if y + need > height:
            sheets.append(cur.crop((0, 0, width, max(y, 1))))
            cur, y = Image.new('RGB', (width, height), (25, 25, 25)), 0
            draw = ImageDraw.Draw(cur)
        nhd = sum(f'{fnv_key(raw, w, h):016x}' in done for o, raw in lst)
        draw.text((4, y), f'{w}x{h} n={len(lst)} hd={nhd} @{lst[0][0]}', fill=(255, 255, 0))
        for i, im in enumerate(ims):
            cur.paste(im, (4 + (i % per_row) * (im.width + 4), y + 14 + (i // per_row) * (im.height + 4)))
        y += need + 4
    sheets.append(cur.crop((0, 0, width, max(y, 1))))
    for i, s in enumerate(sheets):
        path = os.path.join(args.out, f'res_sheet{i}.png')
        s.save(path)
        print(path, s.size)


if __name__ == '__main__':
    main()
