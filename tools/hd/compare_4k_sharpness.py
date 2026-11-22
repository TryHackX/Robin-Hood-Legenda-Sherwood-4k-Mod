"""Is a 4K map picture sharper than the HD (2x) one magnified? (user 2026-11-22: "the 4K map does not look sharp")

    python -I tools/hd/compare_4k_sharpness.py croisement02_day 450c0ad1696a52aa [x y w h]

Compares, over a region given in ORIGINAL pixels (default: a block of trees), at the 4K scale:
  orig4  - the original picture magnified 4x (bicubic)
  hd2x2  - the HD picture (hd/maps/dat2/<key>.hdmap) magnified 2x (bicubic)
  k4     - the 4K picture (hd/maps_png/4k/<map>_dat2.png)
Sharpness = mean absolute Laplacian (fine detail) and mean gradient; writes a side-by-side PNG at 100 % to
tools/out/sharp_<map>.png (left to right: orig4, hd2x2, k4).
"""
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]


def load_hdmap(key):
    d = (ROOT / "hd" / "maps" / "dat2" / f"{key}.hdmap").read_bytes()
    magic, ver, w, h, _ = struct.unpack_from("<4sIIII", d, 0)
    px = np.frombuffer(d, dtype=np.uint16, count=w * h, offset=20).reshape(h, w)
    r = ((px >> 11) & 31).astype(np.uint16) * 255 // 31
    g = ((px >> 5) & 63).astype(np.uint16) * 255 // 63
    b = (px & 31).astype(np.uint16) * 255 // 31
    return np.dstack([r, g, b]).astype(np.uint8)


def measures(a):
    f = a.astype(np.float32).mean(axis=2)
    lap = np.abs(4 * f[1:-1, 1:-1] - f[:-2, 1:-1] - f[2:, 1:-1] - f[1:-1, :-2] - f[1:-1, 2:])
    gx = np.abs(np.diff(f, axis=1)).mean()
    gy = np.abs(np.diff(f, axis=0)).mean()
    return lap.mean(), (gx + gy) / 2


def main():
    name, key = sys.argv[1], sys.argv[2]
    x, y, w, h = (int(v) for v in sys.argv[3:7]) if len(sys.argv) >= 7 else (900, 150, 220, 150)
    orig = np.asarray(Image.open(ROOT / "hd" / "maps_png" / f"{name}.png").convert("RGB"))
    k4img = Image.open(ROOT / "hd" / "maps_png" / "4k" / f"{name}_dat2.png")
    k4 = np.asarray(k4img.crop((x * 4, y * 4, (x + w) * 4, (y + h) * 4)).convert("RGB"))
    hd = load_hdmap(key)
    o4 = np.asarray(Image.fromarray(orig[y:y + h, x:x + w]).resize((w * 4, h * 4), Image.BICUBIC))
    h22 = np.asarray(Image.fromarray(hd[y * 2:(y + h) * 2, x * 2:(x + w) * 2]).resize((w * 4, h * 4), Image.BICUBIC))
    for label, a in (("orig4", o4), ("hd2x2", h22), ("k4", k4)):
        lap, grad = measures(a)
        print(f"{label:6s} detail (|Laplacian|) {lap:6.2f}   gradient {grad:6.2f}")
    # the 4K picture brought down to 2x against the HD picture: how close (it should be about the same picture)
    k4half = np.asarray(Image.fromarray(k4).resize((w * 2, h * 2), Image.BOX)).astype(np.float32)
    hdreg = hd[y * 2:(y + h) * 2, x * 2:(x + w) * 2].astype(np.float32)
    print(f"4K reduced to 2x vs HD 2x: mean difference {np.abs(k4half - hdreg).mean():.2f} (0-255)")
    out = Image.new("RGB", (w * 4 * 3 + 24, h * 4), (30, 30, 30))
    for i, a in enumerate((o4, h22, k4)):
        out.paste(Image.fromarray(a), (i * (w * 4 + 12), 0))
    target = ROOT / "tools" / "out" / f"sharp_{name}.png"
    out.save(target)
    print("side by side:", target)


if __name__ == "__main__":
    main()
