"""Side-by-side look at the HD background variants, and their colour agreement with the original.

    python tools\\hd\\variant_sheet.py [--map nottingham_day] [--crop 1030,1330,300,220] [--name dom]

Reads hd\\maps_png\\<map>.png and hd\\maps\\<variant>\\<key>.hdmap for every variant folder. Prints
mean colour / saturation of each variant against the original (after reducing the HD picture to
the original size) and writes hd\\compare\\warianty_<name>.png: the original doubled, then every
variant, the same crop at 100 %.
"""
import argparse
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parent))
from upscale_maps import fnv1a64, original_pixels_565  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
Image.MAX_IMAGE_PIXELS = None


def load_hdmap(path: Path) -> np.ndarray:
    d = np.memmap(path, dtype=np.uint8, mode="r")
    _, w, h, _ = struct.unpack_from("<IIII", d, 4)
    px = np.frombuffer(d, dtype="<u2", count=w * h, offset=20).reshape(h, w)
    return np.dstack([((px >> 11) & 31).astype(np.float32) * 255 / 31,
                      ((px >> 5) & 63).astype(np.float32) * 255 / 63,
                      (px & 31).astype(np.float32) * 255 / 31])


def describe(img: np.ndarray):
    m = img.reshape(-1, 3).mean(0, dtype=np.float64)
    mx, mn = img.max(2), img.min(2)
    sat = np.where(mx > 0, (mx - mn) / np.maximum(mx, 1e-6), 0).mean(dtype=np.float64) * 100
    return m, sat


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--map", default="nottingham_day")
    ap.add_argument("--crop", default="1030,1330,300,220", help="x,y,w,h in original pixels")
    ap.add_argument("--name", default="dom")
    args = ap.parse_args()

    png = ROOT / "hd" / "maps_png" / f"{args.map}.png"
    orig = np.asarray(Image.open(png).convert("RGB")).astype(np.float32)
    key = f"{fnv1a64(original_pixels_565(png)):016x}"
    x, y, w, h = [int(v) for v in args.crop.split(",")]
    m, sat = describe(orig)
    print(f"{'original':11s} mean RGB {m[0]:6.2f} {m[1]:6.2f} {m[2]:6.2f}  saturation {sat:5.2f} %  warm (R-B) {m[0] - m[2]:5.2f}")
    tiles = [("oryginal x2", Image.fromarray(orig[y:y + h, x:x + w].astype(np.uint8), "RGB").resize((w * 2, h * 2), Image.NEAREST))]
    for folder in sorted((ROOT / "hd" / "maps").iterdir()):
        f = folder / f"{key}.hdmap"
        if not f.exists():
            continue
        hd = load_hdmap(f)
        low = (hd[0::2, 0::2] + hd[1::2, 0::2] + hd[0::2, 1::2] + hd[1::2, 1::2]) / 4
        m2, sat2 = describe(hd)
        diff = low - orig
        bh, bw = orig.shape[0] // 32, orig.shape[1] // 32
        blk = diff[:bh * 32, :bw * 32].reshape(bh, 32, bw, 32, 3).mean((1, 3), dtype=np.float64)
        print(f"{folder.name:11s} mean RGB {m2[0]:6.2f} {m2[1]:6.2f} {m2[2]:6.2f}  saturation {sat2:5.2f} %  warm (R-B) {m2[0] - m2[2]:5.2f}"
              f"  | vs original: mean {np.round(m2 - m, 2)}, 32-px blocks mean abs {np.abs(blk).mean():.2f}, worst {np.abs(blk).max():.2f}")
        tiles.append((folder.name, Image.fromarray(np.clip(hd[y * 2:(y + h) * 2, x * 2:(x + w) * 2] + 0.5, 0, 255).astype(np.uint8), "RGB")))

    pad, bar = 6, 18
    cols = 3
    rows = (len(tiles) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * (w * 2 + pad) + pad, rows * (h * 2 + bar + pad) + pad), (24, 24, 24))
    draw = ImageDraw.Draw(sheet)
    for i, (label, im) in enumerate(tiles):
        cx = pad + (i % cols) * (w * 2 + pad)
        cy = pad + (i // cols) * (h * 2 + bar + pad)
        draw.text((cx + 2, cy + 3), label, fill=(235, 235, 235))
        sheet.paste(im, (cx, cy + bar))
    out = ROOT / "hd" / "compare" / f"warianty_{args.name}.png"
    sheet.save(out)
    print(f"sheet: {out} {sheet.size}")


if __name__ == "__main__":
    main()
