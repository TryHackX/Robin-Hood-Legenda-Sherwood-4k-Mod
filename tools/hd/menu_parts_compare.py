"""Side-by-side sheet of menu pictures upscaled by several models (to choose one for upscale_menu_parts.py).

    python -I tools/hd/menu_parts_compare.py KEY [KEY ...] --models aomr sgi dat2 [--box x y w h]

Writes hd/compare/menu_parts_<first key>.png: rows = models (first row: the original, bicubic), the part
`box` of the picture (1x pixels) shown at 2x size.
"""
import argparse
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('keys', nargs='+')
    ap.add_argument('--models', nargs='+', default=['aomr', 'sgi', 'dat2'])
    ap.add_argument('--box', nargs=4, type=int, default=None)
    args = ap.parse_args()
    import cv2
    import torch
    from PIL import Image
    from upscale_menu_parts import read_part, rgb_from_565, WORK
    from upscale_sprites import bleed
    from upscale_test import load_model
    from upscale_variants import VARIANTS, pick_precision, upscale, color_fix
    pics = []
    for k in args.keys:
        px, ckey, marker = read_part(WORK / f'part_{k}.raw')
        orig = rgb_from_565(px)
        valid = np.ones(px.shape, bool) if ckey == 0xFFFFFFFF else px != (ckey & 0xFFFF)
        filled = orig if valid.all() else bleed(orig.astype(np.float32), valid).clip(0, 255).astype(np.uint8)
        pics.append((k, filled))
    rows = {'original': [cv2.resize(f, (f.shape[1] * 2, f.shape[0] * 2), interpolation=cv2.INTER_CUBIC) for _, f in pics]}
    for m in args.models:
        desc = load_model(ROOT / 'hd' / 'models' / VARIANTS[m][0])
        sample = np.zeros((96, 96, 3), np.uint8)
        sample[::2, ::3] = 180
        dtype, _ = pick_precision(desc, sample, 192, 16)
        out = []
        for _, f in pics:
            p = np.pad(f, ((8, 8), (8, 8), (0, 0)), mode='edge')
            r = upscale(desc, p, 192, 16, dtype)
            if desc.scale != 2:
                r = cv2.resize(r, (p.shape[1] * 2, p.shape[0] * 2), interpolation=cv2.INTER_AREA)
            r = r[16:16 + f.shape[0] * 2, 16:16 + f.shape[1] * 2]
            out.append((color_fix(r, f, 1.5) + 0.5).clip(0, 255).astype(np.uint8))
        rows[m] = out
        del desc
        torch.cuda.empty_cache()
    tiles = []
    for name, imgs in rows.items():
        parts = []
        for img in imgs:
            if args.box:
                x, y, w, h = args.box
                img = img[y * 2:(y + h) * 2, x * 2:(x + w) * 2]
            parts.append(img)
        row = np.concatenate([cv2.copyMakeBorder(p, 4, 4, 4, 4, cv2.BORDER_CONSTANT, value=(30, 30, 30)) for p in parts], 1)
        cv2.putText(row, name, (10, 24), cv2.FONT_HERSHEY_SIMPLEX, 0.7, (255, 0, 0), 2)
        tiles.append(row)
    w = max(t.shape[1] for t in tiles)
    sheet = np.concatenate([np.pad(t, ((0, 0), (0, w - t.shape[1]), (0, 0))) for t in tiles], 0)
    path = ROOT / 'hd' / 'compare' / f'menu_parts_{args.keys[0]}.png'
    path.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(sheet).save(path)
    print('->', path)


if __name__ == '__main__':
    main()
