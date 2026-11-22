"""The 4x level pictures as the mod reads them (2026-11-22, user: "dat2 in 4K came out best - do it for the other maps
so it is native 4K"): hd\\maps_png\\4k\\<map>_<variant>.png (tools\\hd\\upscale_4k.py) -> hd\\maps4k\\<variant>\\<key>.hdm4

    python -I tools/hd/make_maps4k.py [--variants dat2] [--only nottingham_day]

<key> = the key of the level picture (as the 2x .hdmap files, upscale_maps.fnv1a64 over the game's 16-bit pixels).
.hdm4: "HDM4", u32 version 1, u32 width, u32 height (4x the level), u32 0, then RGB565 rows top to bottom
(ordered dither, as the 2x pictures). The presenter shows it where the screen has more than two pixels per level
pixel (zoomed in beyond 2.0, a 4K screen) - ddm_present.cpp, the level layer.
Finished files are skipped (--redo writes them again).
"""
import argparse
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from upscale_maps import fnv1a64, original_pixels_565, rgb888_to_565_dithered  # noqa: E402

ROOT = HERE.parents[1]
Image.MAX_IMAGE_PIXELS = None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--variants", nargs="*", default=["dat2"])
    ap.add_argument("--only", nargs="*", default=None)
    ap.add_argument("--redo", action="store_true")
    args = ap.parse_args()
    src_dir = ROOT / "hd" / "maps_png"
    for variant in args.variants:
        out_dir = ROOT / "hd" / "maps4k" / variant
        out_dir.mkdir(parents=True, exist_ok=True)
        for one in sorted(src_dir.glob("*.png")):
            if args.only and one.stem not in args.only:
                continue
            big = src_dir / "4k" / f"{one.stem}_{variant}.png"
            if not big.exists():
                print(f"{one.stem}: no 4x picture ({big.name})")
                continue
            key = f"{fnv1a64(original_pixels_565(one)):016x}"
            target = out_dir / f"{key}.hdm4"
            if target.exists() and not args.redo:
                print(f"{one.stem}: {target.name} there already")
                continue
            w1, h1 = Image.open(one).size
            with Image.open(big) as im:
                w4, h4 = im.size
                if (w4, h4) != (4 * w1, 4 * h1):
                    print(f"{one.stem}: {big.name} is {w4}x{h4}, not 4x {w1}x{h1} - skipped")
                    continue
                rgb = np.asarray(im.convert("RGB"))
            tmp = target.with_suffix(".tmp")
            with open(tmp, "wb") as f:
                f.write(b"HDM4" + struct.pack("<IIII", 1, w4, h4, 0))
                step = 512                      # rows at a time: the dither and the 16-bit copy stay small
                for y in range(0, h4, step):
                    f.write(rgb888_to_565_dithered(rgb[y:y + step]).astype("<u2").tobytes())
            tmp.replace(target)
            print(f"{one.stem}: {target.name} {w4}x{h4} ({target.stat().st_size / 2**20:.0f} MB)", flush=True)


if __name__ == "__main__":
    main()
