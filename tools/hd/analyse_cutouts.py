"""Tree crowns / roofs the game draws over the level (2026-11-22): how do their pixels differ from the level picture?

    python -I tools/hd/analyse_cutouts.py

Reads tools/out/harness/cutout_*.raw (the mod's dump, dbg 65536 in test mode: u32 w, h, lx, ly, marker; w*h sprite
pixels; w*h level pixels, RGB565) and prints per dump: the share of pixels equal / within small steps of the level
picture, and how far apart the rest are; writes tools/out/cutout_<n>.png: sprite | level | difference map.
"""
import struct
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
HARNESS = ROOT / "tools" / "out" / "harness"


def rgb(px):
    r = ((px >> 11) & 31).astype(np.int32) * 255 // 31
    g = ((px >> 5) & 63).astype(np.int32) * 255 // 63
    b = (px & 31).astype(np.int32) * 255 // 31
    return np.dstack([r, g, b])


def main():
    seen = {}
    for path in sorted(HARNESS.glob("cutout_*.raw")):
        d = path.read_bytes()
        w, h, lx, ly, marker = struct.unpack_from("<5I", d, 0)
        n = w * h
        spr = np.frombuffer(d, dtype=np.uint16, count=n, offset=20).reshape(h, w)
        lev = np.frombuffer(d, dtype=np.uint16, count=n, offset=20 + n * 2).reshape(h, w)
        opaque = (spr != 0x07C0) & (spr != (marker & 0xFFFF))
        a, b = rgb(spr), rgb(lev)
        diff = np.abs(a - b).max(axis=2)
        dvals = diff[opaque]
        key = (w, h, lx, ly)
        tag = "" if key not in seen else " (again)"
        seen[key] = True
        pct = lambda t: 100.0 * (dvals <= t).mean()
        print(f"{path.stem}: {w}x{h} at {lx},{ly}{tag}: opaque {opaque.sum()}, equal {pct(0):.0f}%, <=8 {pct(8):.0f}%,"
              f" <=16 {pct(16):.0f}%, <=32 {pct(32):.0f}%, median {np.median(dvals):.0f}")
        if not tag:
            vis = np.zeros((h, w, 3), dtype=np.uint8)
            vis[opaque] = np.clip(diff[opaque] * 4, 0, 255)[:, None].astype(np.uint8)
            sa = a.astype(np.uint8).copy()
            sa[~opaque] = (255, 0, 255)
            out = Image.new("RGB", (w * 3 + 16, h), (0, 0, 0))
            out.paste(Image.fromarray(sa), (0, 0))
            out.paste(Image.fromarray(b.astype(np.uint8)), (w + 8, 0))
            out.paste(Image.fromarray(vis), (2 * w + 16, 0))
            out.save(ROOT / "tools" / "out" / f"{path.stem}.png")


if __name__ == "__main__":
    main()
