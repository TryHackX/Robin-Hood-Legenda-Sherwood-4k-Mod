"""Upscale the menu pictures of the game 2x with the AI models of the map variants.

    python -I tools\\hd\\upscale_menu.py --png-only            (look at what was collected)
    python -I tools\\hd\\upscale_menu.py                       (all variants: aomr sgi ultrasharp dat2)
    python -I tools\\hd\\upscale_menu.py --variants dat2 --redo

Input : hd\\work\\menu_frames\\<key>.mnf      written by the mod while the menus are shown with the menu zoom
        (src\\ddm\\ddm_hd_menu.cpp): "MNUF", u32 1, u16 cropW, cropH, frameW, frameH, then the part of the
        composed menu screen the menu zoom shows (cropW x cropH, RGB565, row by row)
Output: hd\\menu\\<variant>\\<key>.mhd       what the mod loads: "MNHD", u32 1, u16 cropW, cropH, l, t, w, h, then
        (2w x 2h) BGRA - the picture part (l, t, w, h: the box around everything that is not black) twice the size
        hd\\menu_png\\_oryginal\\<key>.png, hd\\menu_png\\<variant>\\<key>.png   for looking at
Colours: the HD picture gets the colours and brightness of the original (color_fix of upscale_variants.py).
Finished files are skipped unless --redo.
"""
import argparse
import struct
import sys
import time
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
FRAMES = ROOT / "hd" / "work" / "menu_frames"
STORE = ROOT / "hd" / "menu"
PNG = ROOT / "hd" / "menu_png"
DEFAULT = ["aomr", "sgi", "ultrasharp", "dat2"]


def log(text: str):
    print(f"[{time.strftime('%H:%M:%S')}] {text}", flush=True)


def read_frame(path: Path):
    d = path.read_bytes()
    magic, version, cw, ch, fw, fh = struct.unpack_from("<4sIHHHH", d, 0)
    if magic != b"MNUF" or version != 1 or len(d) != 16 + cw * ch * 2:
        raise ValueError(f"{path.name}: not a menu picture")
    px = np.frombuffer(d, "<u2", count=cw * ch, offset=16).reshape(ch, cw)
    return px


def rgb_from_565(px: np.ndarray) -> np.ndarray:
    r = ((px >> 11) & 31).astype(np.float32) * (255.0 / 31.0)
    g = ((px >> 5) & 63).astype(np.float32) * (255.0 / 63.0)
    b = (px & 31).astype(np.float32) * (255.0 / 31.0)
    return (np.dstack([r, g, b]) + 0.5).astype(np.uint8)


def picture_box(px: np.ndarray, pad: int = 4):
    """The box around everything that is not (almost) black, a little larger; None if all black."""
    rgb = rgb_from_565(px)
    lit = rgb.max(axis=2) > 6
    ys, xs = np.nonzero(lit)
    if len(xs) == 0:
        return None
    h, w = px.shape
    l, t = max(0, xs.min() - pad), max(0, ys.min() - pad)
    r, b = min(w, xs.max() + 1 + pad), min(h, ys.max() + 1 + pad)
    return l, t, r - l, b - t


def write_mhd(path: Path, crop_w, crop_h, box, hd_rgb: np.ndarray):
    l, t, w, h = box
    assert hd_rgb.shape == (h * 2, w * 2, 3)
    bgra = np.empty((h * 2, w * 2, 4), np.uint8)
    bgra[..., 0] = hd_rgb[..., 2]
    bgra[..., 1] = hd_rgb[..., 1]
    bgra[..., 2] = hd_rgb[..., 0]
    bgra[..., 3] = 255
    tmp = path.with_suffix(".tmp")
    with open(tmp, "wb") as f:
        f.write(struct.pack("<4sIHHHHHH", b"MNHD", 1, crop_w, crop_h, l, t, w, h))
        f.write(bgra.tobytes())
    tmp.replace(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--variants", nargs="*", default=DEFAULT)
    ap.add_argument("--png-only", action="store_true", help="only write the originals as PNG")
    ap.add_argument("--redo", action="store_true")
    ap.add_argument("--tile", type=int, default=192)
    ap.add_argument("--overlap", type=int, default=16)
    ap.add_argument("--colorfix", type=float, default=1.5)
    args = ap.parse_args()

    files = sorted(FRAMES.glob("*.mnf"))
    log(f"{len(files)} menu pictures in {FRAMES}")
    frames = {}
    for p in files:
        try:
            px = read_frame(p)
        except ValueError as e:
            log(str(e))
            continue
        box = picture_box(px)
        if box is None:
            log(f"{p.stem}: black - skipped")
            continue
        frames[p.stem] = (px, box)
        (PNG / "_oryginal").mkdir(parents=True, exist_ok=True)
        out = PNG / "_oryginal" / f"{p.stem}.png"
        if not out.exists() or args.redo:
            Image.fromarray(rgb_from_565(px)).save(out)
        log(f"{p.stem}: {px.shape[1]}x{px.shape[0]}, picture {box[2]}x{box[3]} at {box[0]},{box[1]}")
    if args.png_only or not frames:
        return

    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import torch
    from upscale_test import load_model
    from upscale_variants import VARIANTS, pick_precision, upscale, color_fix
    import cv2

    for name in args.variants:
        if name not in VARIANTS:
            log(f"unknown variant {name} - skipped")
            continue
        todo = [k for k in frames if args.redo or not (STORE / name / f"{k}.mhd").exists()]
        if not todo:
            log(f"{name}: all done")
            continue
        (STORE / name).mkdir(parents=True, exist_ok=True)
        (PNG / name).mkdir(parents=True, exist_ok=True)
        desc = load_model(ROOT / "hd" / "models" / VARIANTS[name][0])
        sample = np.zeros((96, 96, 3), np.uint8)
        sample[::2, ::3] = 180
        dtype, how = pick_precision(desc, sample, args.tile, args.overlap)
        log(f"{name}: {VARIANTS[name][0]} ({desc.architecture.name} x{desc.scale}, {how}); {len(todo)} pictures")
        for k in todo:
            px, box = frames[k]
            l, t, w, h = box
            crop_h, crop_w = px.shape
            orig = rgb_from_565(px[t:t + h, l:l + w])
            t0 = time.time()
            res = upscale(desc, orig, args.tile, args.overlap, dtype)
            if desc.scale != 2:
                res = cv2.resize(res, (w * 2, h * 2), interpolation=cv2.INTER_AREA if desc.scale > 2 else cv2.INTER_CUBIC)
            if args.colorfix > 0:
                res = (color_fix(res, orig, args.colorfix) + 0.5).clip(0, 255).astype(np.uint8)
            write_mhd(STORE / name / f"{k}.mhd", crop_w, crop_h, box, res)
            Image.fromarray(res).save(PNG / name / f"{k}.png")
            log(f"  {k}: {w}x{h} -> {w * 2}x{h * 2} in {time.time() - t0:.1f} s")
        del desc
        torch.cuda.empty_cache()


if __name__ == "__main__":
    main()
