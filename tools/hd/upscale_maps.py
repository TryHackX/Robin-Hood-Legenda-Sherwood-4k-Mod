"""Upscale level backgrounds 2x with an AI model and write them in the format the mod loads.

    python tools\\hd\\upscale_maps.py --model 4xNomos2_hq_dat2.pth [--only nottingham_day] [--tile 192]

Input : hd\\maps_png\\<map>_<variant>.png   (tools\\hd\\extract_maps.py)
Output: hd\\maps_hd\\<map>_<variant>.png     (2x, 8-bit, for looking at)
        hd\\maps\\<key>.hdmap                (what the game mod loads; see below)

<key> identifies the ORIGINAL background by content, because the mod only sees pixels, not file
names: 16 hex digits of a 64-bit FNV-1a hash over the original 16-bit pixels (little endian),
exactly what src\\ddm computes over the game's map surface.

.hdmap layout: "HDMP" u32 version=1, u32 width, u32 height (of the HD image), u32 reserved,
then width*height RGB565 pixels (little endian), rows top to bottom. 8-bit -> 565 uses ordered
dithering so that gradients do not band.
"""
import argparse
import struct
import sys
import time
from pathlib import Path

import numpy as np
import torch
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from upscale_test import load_model, upscale  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
BAYER8 = np.array([
    [0, 32, 8, 40, 2, 34, 10, 42], [48, 16, 56, 24, 50, 18, 58, 26],
    [12, 44, 4, 36, 14, 46, 6, 38], [60, 28, 52, 20, 62, 30, 54, 22],
    [3, 35, 11, 43, 1, 33, 9, 41], [51, 19, 59, 27, 49, 17, 57, 25],
    [15, 47, 7, 39, 13, 45, 5, 37], [63, 31, 55, 23, 61, 29, 53, 21]], dtype=np.float32) / 64.0


def fnv1a64(data: bytes) -> int:
    """64-bit FNV-1a over 16-bit words (same loop as the C++ side, vectorised in blocks)."""
    h = 0xCBF29CE484222325
    prime = 0x100000001B3
    mask = 0xFFFFFFFFFFFFFFFF
    words = np.frombuffer(data, dtype="<u2")
    # sample every 7th pixel: 16 MB hashed in milliseconds on the game side
    for w in words[::7].tolist():
        h = ((h ^ w) * prime) & mask
    return h


def rgb888_to_565_dithered(rgb: np.ndarray) -> np.ndarray:
    h, w, _ = rgb.shape
    thr = np.tile(BAYER8, (h // 8 + 1, w // 8 + 1))[:h, :w]
    f = rgb.astype(np.float32)
    r = np.clip(np.floor(f[..., 0] * (31.0 / 255.0) + thr), 0, 31).astype(np.uint16)
    g = np.clip(np.floor(f[..., 1] * (63.0 / 255.0) + thr), 0, 63).astype(np.uint16)
    b = np.clip(np.floor(f[..., 2] * (31.0 / 255.0) + thr), 0, 31).astype(np.uint16)
    return (r << 11) | (g << 5) | b


def original_pixels_565(png: Path) -> bytes:
    """The exact 16-bit pixels the game has: invert the bit expansion done by extract_maps.py."""
    rgb = np.asarray(Image.open(png).convert("RGB")).astype(np.uint16)
    r = (rgb[..., 0] * 31 + 127) // 255
    g = (rgb[..., 1] * 63 + 127) // 255
    b = (rgb[..., 2] * 31 + 127) // 255
    return ((r << 11) | (g << 5) | b).astype("<u2").tobytes()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", required=True)
    ap.add_argument("--only", nargs="*", default=None, help="map names without .png")
    ap.add_argument("--tile", type=int, default=192)
    ap.add_argument("--overlap", type=int, default=16)
    args = ap.parse_args()

    src_dir, png_dir, out_dir = ROOT / "hd" / "maps_png", ROOT / "hd" / "maps_hd", ROOT / "hd" / "maps"
    png_dir.mkdir(parents=True, exist_ok=True)
    out_dir.mkdir(parents=True, exist_ok=True)
    desc = load_model(ROOT / "hd" / "models" / args.model)
    print(f"model {args.model}: {desc.architecture.name} x{desc.scale}")

    for png in sorted(src_dir.glob("*.png")):
        if args.only and png.stem not in args.only:
            continue
        img = Image.open(png).convert("RGB")
        w, h = img.size
        key = f"{fnv1a64(original_pixels_565(png)):016x}"
        t0 = time.perf_counter()
        res = upscale(desc, np.asarray(img).copy(), tile=args.tile, overlap=args.overlap)
        hd = Image.fromarray(res, "RGB")
        if desc.scale != 2:
            hd = hd.resize((w * 2, h * 2), Image.LANCZOS)
        hd.save(png_dir / png.name)
        px = rgb888_to_565_dithered(np.asarray(hd))
        with open(out_dir / f"{key}.hdmap", "wb") as f:
            f.write(b"HDMP" + struct.pack("<IIII", 1, w * 2, h * 2, 0))
            f.write(px.astype("<u2").tobytes())
        dt = time.perf_counter() - t0
        print(f"{png.stem:22s} {w}x{h} -> {w * 2}x{h * 2}  key={key}  {dt:6.0f} s  ({w * h / 1e6 / dt:.3f} Mpx/s)")
        torch.cuda.empty_cache()


if __name__ == "__main__":
    main()
