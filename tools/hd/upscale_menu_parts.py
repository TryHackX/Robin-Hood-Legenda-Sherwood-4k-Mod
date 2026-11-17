"""HD pictures of menus and dialogs (buttons, the briefing's parchment and seal, portraits ...) twice the size.

    python -I tools/hd/upscale_menu_parts.py [--model sgi] [--redo]

Input : hd/work/menu_parts/part_<key>.raw - pictures the mod collected while menus and dialogs were shown
        (only where hd/work exists): "SBMP", u32 w, h, colour key, shadow marker (0xFFFFFFFF = none), then
        w*h RGB565. The key is the mod's (the picture's content).
Output: hd/menu_parts/<key>.hdp  "HDMP", u32 1, u32 2w, u32 2h, then 2w*2h BGRA (alpha = how much of the picture's
        colour covers the pixel), then 2w*2h bytes: how much of the pixel is the picture's shadow (the game darkens
        what lies under such pixels). Previews over a checkerboard: hd/menu_parts_png/<key>.png.
The colour goes through the model with its transparent and shadow parts filled from the pixels next to them; the
colours are brought back to the original's (as the menu backgrounds); the transparent / shadow edges get a smooth
contour from the 0/1 masks (as the colour-keyed fonts).
"""
import argparse
import os
import struct
import sys
import time
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
WORK = ROOT / 'hd' / 'work' / 'menu_parts'
OUT = ROOT / 'hd' / 'menu_parts'
PNG = ROOT / 'hd' / 'menu_parts_png'
NONE = 0xFFFFFFFF


def log(text):
    print(f"[{time.strftime('%H:%M:%S')}] {text}", flush=True)


def read_part(path):
    d = path.read_bytes()
    magic, w, h, key, marker = struct.unpack_from('<5I', d, 0)
    if magic != 0x504D4253 or len(d) < 20 + w * h * 2:
        raise ValueError(f'{path.name}: not a picture of the mod')
    px = np.frombuffer(d, '<u2', w * h, 20).reshape(h, w)
    return px, key, marker


def rgb_from_565(px):
    r = ((px >> 11) & 31).astype(np.uint32)
    g = ((px >> 5) & 63).astype(np.uint32)
    b = (px & 31).astype(np.uint32)
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], -1).astype(np.uint8)


def contour(mask, sigma=0.6, half=0.05):
    """A 0/1 mask twice the size with a smooth contour (blurred a little, 4x bicubic, a narrow soft step at 0.5,
    area-reduced to 2x). sigma 0.6 keeps lines one pixel wide (0.8, as for the fonts, would lose them)."""
    import cv2
    m = mask.astype(np.float32)
    b = cv2.GaussianBlur(m, (0, 0), sigma) if sigma > 0 else m
    up = cv2.resize(b, (m.shape[1] * 4, m.shape[0] * 4), interpolation=cv2.INTER_CUBIC)
    t = np.clip((up - (0.5 - half)) / (2 * half), 0, 1)
    t = t * t * (3 - 2 * t)
    return np.clip(cv2.resize(t, (m.shape[1] * 2, m.shape[0] * 2), interpolation=cv2.INTER_AREA), 0, 1)


def preview(rgb2, a2, s2, path):
    from PIL import Image
    h, w = a2.shape
    yy, xx = np.mgrid[0:h, 0:w]
    checker = np.where(((yy // 16 + xx // 16) & 1)[..., None] == 0, 200, 140).astype(np.float32) * np.ones(3)
    under = checker * (1 - 0.5 * s2[..., None])
    out = rgb2.astype(np.float32) * a2[..., None] + under * (1 - a2[..., None])
    Image.fromarray(out.clip(0, 255).astype(np.uint8)).save(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--model', default='sgi')   # aomr made the parchment crunchy (hd/compare/menu_parts_*.png)
    ap.add_argument('--redo', action='store_true')
    ap.add_argument('--tile', type=int, default=192)
    ap.add_argument('--overlap', type=int, default=16)
    ap.add_argument('--colorfix', type=float, default=1.5)
    args = ap.parse_args()

    parts = sorted(WORK.glob('part_*.raw'))
    todo = [p for p in parts if args.redo or not (OUT / (p.stem[5:] + '.hdp')).exists()]
    log(f'{len(parts)} pictures in {WORK}, {len(todo)} to do')
    if not todo:
        return
    sys.path.insert(0, str(HERE))
    import cv2
    import torch
    from upscale_sprites import bleed
    from upscale_test import load_model
    from upscale_variants import VARIANTS, pick_precision, upscale, color_fix
    desc = load_model(ROOT / 'hd' / 'models' / VARIANTS[args.model][0])
    sample = np.zeros((96, 96, 3), np.uint8)
    sample[::2, ::3] = 180
    dtype, how = pick_precision(desc, sample, args.tile, args.overlap)
    log(f'{args.model}: {VARIANTS[args.model][0]} (x{desc.scale}, {how})')
    OUT.mkdir(parents=True, exist_ok=True)
    PNG.mkdir(parents=True, exist_ok=True)
    pad = 8
    for p in todo:
        key = p.stem[5:]
        try:
            px, ckey, marker = read_part(p)
        except ValueError as e:
            log(str(e))
            continue
        h, w = px.shape
        trans = (px == (ckey & 0xFFFF)) if ckey != NONE else np.zeros_like(px, bool)
        shade = ((px == (marker & 0xFFFF)) & ~trans) if marker != NONE else np.zeros_like(px, bool)
        colour = ~trans & ~shade
        if not colour.any():
            log(f'  {key}: no colour pixels - skipped')
            continue
        orig = rgb_from_565(px)
        filled = orig if colour.all() else bleed(orig.astype(np.float32), colour).clip(0, 255).astype(np.uint8)
        t0 = time.time()
        padded = np.pad(filled, ((pad, pad), (pad, pad), (0, 0)), mode='edge')
        res = upscale(desc, padded, args.tile, args.overlap, dtype)
        if desc.scale != 2:
            res = cv2.resize(res, (padded.shape[1] * 2, padded.shape[0] * 2), interpolation=cv2.INTER_AREA)
        small = min(h, w) < 16      # the colour fix's blur needs more pixels than a tiny picture has
        if args.colorfix > 0 and small:
            res = (color_fix(res, padded, args.colorfix) + 0.5).clip(0, 255).astype(np.uint8)
        res = res[pad * 2:pad * 2 + h * 2, pad * 2:pad * 2 + w * 2]
        if args.colorfix > 0 and not small:
            res = (color_fix(res, filled, args.colorfix) + 0.5).clip(0, 255).astype(np.uint8)
        a2 = np.ones((h * 2, w * 2), np.float32) if colour.all() else contour(colour)
        s2 = contour(shade) if shade.any() else np.zeros((h * 2, w * 2), np.float32)
        bgra = np.empty((h * 2, w * 2, 4), np.uint8)
        bgra[..., 0] = res[..., 2]
        bgra[..., 1] = res[..., 1]
        bgra[..., 2] = res[..., 0]
        bgra[..., 3] = (a2 * 255 + 0.5).astype(np.uint8)
        with open(OUT / f'{key}.hdp', 'wb') as f:
            f.write(struct.pack('<4sIII', b'HDMP', 1, w * 2, h * 2))
            f.write(bgra.tobytes())
            f.write((s2 * 255 + 0.5).astype(np.uint8).tobytes())
        preview(res, a2, s2, PNG / f'{key}.png')
        log(f'  {key}: {w}x{h} -> {w * 2}x{h * 2} (transparent {int(trans.sum())}, shadow {int(shade.sum())}) '
            f'in {time.time() - t0:.1f} s')
    del desc
    torch.cuda.empty_cache()


if __name__ == '__main__':
    main()
