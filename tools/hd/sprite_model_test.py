"""Compare AI models on sprite frames of the bank: one row per frame, one column per method.

    python tools\\hd\\sprite_model_test.py --frames 293528 293534 166942 --name postacie
           [--variants sgi ultrasharp aomr dat2 span hat mosr] [--zoom 2]

Columns: the frame doubled as the game does it (2x2 blocks), then one column per variant (see
VARIANTS in upscale_variants.py), made exactly as tools\\hd\\upscale_sprites.py makes them. Shadow
marker pixels are shown as a dark translucent patch. Output: hd\\compare\\<name>_sprites.png
"""
import argparse
import sys
import time
from pathlib import Path

import cv2
import numpy as np
import torch
from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parent))
from extract_sprites import Bank, KEY, MARK  # noqa: E402
from upscale_test import load_model  # noqa: E402
from upscale_variants import VARIANTS, pick_precision  # noqa: E402
from upscale_sprites import make_hd, rgb_from_565  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
BG = np.array([96, 120, 78], np.float32)  # grass-like backdrop


def render(px: np.ndarray, zoom: int) -> np.ndarray:
    rgb = rgb_from_565(px)
    out = np.empty(px.shape + (3,), np.float32)
    out[:] = BG
    opaque = (px != KEY) & (px != MARK)
    out[opaque] = rgb[opaque]
    out[px == MARK] = BG * 0.55
    if zoom != 1:
        out = cv2.resize(out, (out.shape[1] * zoom, out.shape[0] * zoom), interpolation=cv2.INTER_NEAREST)
    return np.clip(out + 0.5, 0, 255).astype(np.uint8)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", type=int, nargs="+", required=True)
    ap.add_argument("--variants", nargs="*", default=["sgi", "ultrasharp", "aomr", "dat2"])
    ap.add_argument("--name", required=True)
    ap.add_argument("--zoom", type=int, default=2, help="magnify the whole sheet (nearest) for looking at it")
    ap.add_argument("--colorfix", type=float, default=1.5)
    args = ap.parse_args()

    bank = Bank()
    frames = [(i, bank.decode16(i)) for i in args.frames]
    columns = [("oryginal x2", [np.repeat(np.repeat(img, 2, 0), 2, 1) for _, img in frames])]
    for variant in args.variants:
        desc = load_model(ROOT / "hd" / "models" / VARIANTS[variant][0])
        sample = np.zeros((96, 96, 3), np.uint8) + 90
        dtype, how = pick_precision(desc, sample, 192, 16)
        t0 = time.perf_counter()
        col = [make_hd(desc, dtype, img, 192, 16, args.colorfix) for _, img in frames]
        px = sum(img.size for _, img in frames)
        print(f"{variant:10s} {VARIANTS[variant][0]:38s} {how.split(' ')[0]:9s} {px / 1e6 / (time.perf_counter() - t0):.3f} Mpx/s")
        columns.append((variant, col))
        del desc
        torch.cuda.empty_cache()

    z = args.zoom
    pad, bar = 6, 16
    col_w = max(img.shape[1] for _, img in frames) * 2 * z
    row_h = [img.shape[0] * 2 * z for _, img in frames]
    sheet = Image.new("RGB", (len(columns) * (col_w + pad) + pad, sum(h + pad for h in row_h) + bar + pad), (26, 26, 26))
    draw = ImageDraw.Draw(sheet)
    for c, (label, col) in enumerate(columns):
        x = pad + c * (col_w + pad)
        draw.text((x + 2, 2), label, fill=(235, 235, 235))
        y = bar
        for r, px in enumerate(col):
            if px is not None:
                sheet.paste(Image.fromarray(render(px, z), "RGB"), (x, y))
            y += row_h[r] + pad
    out = ROOT / "hd" / "compare" / f"{args.name}_sprites.png"
    out.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(out)
    print(f"sheet: {out} {sheet.size}")


if __name__ == "__main__":
    main()
