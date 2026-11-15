"""HD fonts: the game's font strips twice the size (the mod draws the interface's letters from them).

    python -I tools/hd/upscale_fonts.py --compare ultrasharp sgi dat2      -> hd/compare/czcionki_modele.png
    python -I tools/hd/upscale_fonts.py --model ultrasharp                 -> hd/fonts/<key>.hdfont

Input : hd/work/fonts/font_<key>.raw - strips dumped by the game (test command "fonts dump",
        copied by tools/hd/font_strips.py collect). The key is the mod's: the strip's content.
Output: hd/fonts/<key>.hdfont  "HDFT", u32 1, u32 width, u32 height (2x), then width*height RGB565 colour,
        then width*height bytes of coverage (0..255).
The colour (transparent parts filled from the letters next to them, so that nothing dark bleeds in)
and the coverage (as a grey picture) go through the model each; 4x models are brought to 2x by area.
"""
import argparse
import glob
import os
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
from font_strips import read_raw, to_rgb  # noqa: E402

WORK = os.path.join(ROOT, 'hd', 'work', 'fonts')
OUT = os.path.join(ROOT, 'hd', 'fonts')
KEY_BANK, KEY_GREEN = 0x07C0, 0x07E0


def coverage_of(col, alp):
    if alp is not None:
        return np.where(col == KEY_BANK, 0.0, np.minimum(1.0, 2.0 * (alp & 31) / 64.0)).astype(np.float32)
    key = KEY_GREEN if (col == KEY_GREEN).any() else KEY_BANK
    return (col != key).astype(np.float32)


def model_2x(desc, dtype, rgb, pad):
    """float HxWx3 (0..255) through the model -> 2x float (area-reduced when the model is 4x)."""
    import cv2
    from upscale_sprites import run_batch
    h, w = rgb.shape[:2]
    padded = np.pad(rgb, ((pad, pad), (pad, pad), (0, 0)), mode='edge')
    # wide strips in pieces (the model's memory), overlapping by 2*pad
    step = 512
    out = np.zeros(((h + 2 * pad) * 2, (w + 2 * pad) * 2, 3), np.float32)
    for x0 in range(0, w + 2 * pad, step):
        a = max(0, x0 - pad)
        b = min(w + 2 * pad, x0 + step + pad)
        res = run_batch(desc, dtype, [padded[:, a:b]])[0]
        s = desc.scale
        if s != 2:
            res = cv2.resize(res, ((b - a) * 2, (h + 2 * pad) * 2), interpolation=cv2.INTER_AREA)
        keep0 = (x0 - a) * 2
        keep1 = keep0 + (min(x0 + step, w + 2 * pad) - x0) * 2
        out[:, x0 * 2:x0 * 2 + (keep1 - keep0)] = res[:, keep0:keep1]
    return out[pad * 2:pad * 2 + h * 2, pad * 2:pad * 2 + w * 2]


def upscale(name, strips):
    import torch
    from upscale_sprites import bleed, PAD
    from upscale_test import load_model
    from upscale_variants import VARIANTS, pick_precision
    desc = load_model(os.path.join(ROOT, 'hd', 'models', VARIANTS[name][0]))
    sample = np.zeros((96, 96, 3), np.uint8)
    sample[::2, ::3] = 180
    dtype, how = pick_precision(desc, sample, 192, 16)
    print('%s: %s (x%d, %s)' % (name, VARIANTS[name][0], desc.scale, how))
    out = []
    for col, alp in strips:
        cov = coverage_of(col, alp)
        rgb = to_rgb(col).astype(np.float32)
        filled = bleed(rgb, cov > 0)
        c2 = model_2x(desc, dtype, filled, PAD)
        a2 = model_2x(desc, dtype, np.repeat((cov * 255.0)[..., None], 3, -1), PAD).mean(-1)
        out.append((np.clip(c2, 0, 255), np.clip(a2 / 255.0, 0, 1)))
    del desc
    torch.cuda.empty_cache()
    return out


def bicubic(strips):
    import cv2
    from upscale_sprites import bleed
    out = []
    for col, alp in strips:
        cov = coverage_of(col, alp)
        rgb = bleed(to_rgb(col).astype(np.float32), cov > 0)
        h, w = cov.shape
        c2 = cv2.resize(rgb, (w * 2, h * 2), interpolation=cv2.INTER_CUBIC)
        a2 = cv2.resize(cov, (w * 2, h * 2), interpolation=cv2.INTER_CUBIC)
        out.append((np.clip(c2, 0, 255), np.clip(a2, 0, 1)))
    return out


def to565(rgb):
    r = (rgb[..., 0] * 31 / 255 + 0.5).astype(np.uint16)
    g = (rgb[..., 1] * 63 / 255 + 0.5).astype(np.uint16)
    b = (rgb[..., 2] * 31 / 255 + 0.5).astype(np.uint16)
    return (r << 11) | (g << 5) | b


def composite(rgb, cov, bg=(70, 56, 40)):
    return (rgb * cov[..., None] + np.array(bg, np.float32) * (1 - cov[..., None])).astype(np.uint8)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--model', default='')
    ap.add_argument('--compare', nargs='*', default=[])
    ap.add_argument('--cells', type=int, default=40, help='cells of each strip on the sheet')
    args = ap.parse_args()
    paths = sorted(glob.glob(os.path.join(WORK, 'font_*.raw')))
    keys = [os.path.basename(p)[5:-4] for p in paths]
    strips = [read_raw(p) for p in paths]
    if not strips:
        sys.exit('no dumps in ' + WORK)
    if args.compare:
        from PIL import Image
        rows = []
        results = {'bicubic': bicubic(strips)}
        for m in args.compare:
            results[m] = upscale(m, strips)
        for (col, alp), k, i in zip(strips, keys, range(len(strips))):
            cov = coverage_of(col, alp)
            cell = col.shape[1] // 180 if col.shape[1] % 180 == 0 else 16
            wpx = min(col.shape[1], args.cells * cell)
            orig = np.repeat(np.repeat(composite(to_rgb(col).astype(np.float32), cov), 2, 0), 2, 1)[:, :wpx * 2]
            panels = [orig] + [composite(*results[m][i])[:, :wpx * 2] for m in results]
            rows.append(np.concatenate(panels, 0))
            print(k, 'rows on the sheet: original x2,', ', '.join(results))
        sheet = np.concatenate(rows, 0)
        Image.fromarray(sheet).resize((sheet.shape[1] * 2, sheet.shape[0] * 2), Image.NEAREST).save(
            os.path.join(ROOT, 'hd', 'compare', 'czcionki_modele.png'))
        print('->', os.path.join(ROOT, 'hd', 'compare', 'czcionki_modele.png'))
    if args.model:
        os.makedirs(OUT, exist_ok=True)
        made = upscale(args.model, strips) if args.model != 'bicubic' else bicubic(strips)
        for k, (c2, a2) in zip(keys, made):
            h, w = a2.shape
            with open(os.path.join(OUT, k + '.hdfont'), 'wb') as f:
                f.write(struct.pack('<4sIII', b'HDFT', 1, w, h))
                f.write(to565(c2).astype('<u2').tobytes())
                f.write((a2 * 255 + 0.5).astype(np.uint8).tobytes())
            print('->', k + '.hdfont', w, 'x', h)


if __name__ == '__main__':
    main()
