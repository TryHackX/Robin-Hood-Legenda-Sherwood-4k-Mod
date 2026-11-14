"""Upscale every level background 2x with several AI models: one "variant" folder per model.

    python tools\\hd\\upscale_variants.py                       (all variants below, all maps)
    python tools\\hd\\upscale_variants.py --variants sgi aomr --only nottingham_day
    python tools\\hd\\upscale_variants.py --list

Input : hd\\maps_png\\<map>_<variant>.png            (tools\\hd\\extract_maps.py)
Output: hd\\maps\\<variant>\\<key>.hdmap              what the mod loads; the player switches between
                                                     the variant folders with a key while playing
        hd\\maps_hd\\<variant>\\<map>.png  (--png)     2x picture for looking at
        hd\\work\\upscale_variants.log               progress (appended)

Finished files are skipped, so the script can be stopped and started again at any time.
<key> and the .hdmap layout: see upscale_maps.py (same functions are used).
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
from upscale_test import load_model  # noqa: E402
from upscale_maps import fnv1a64, original_pixels_565, rgb888_to_565_dithered  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]

# folder name -> (model file in hd\models, what it is)   -- fast ones first
VARIANTS = {
    "aomr":       ("2xAoMR_mosr.pth",               "MoSR 2x, game textures (Age of Mythology Retold)"),
    "sgi":        ("4x_SGI.pth",                    "ESRGAN 4x, pre-rendered CGI of 90s/2000s games, dedither"),
    "ultrasharp": ("4x-UltraSharp.safetensors",     "ESRGAN 4x, adds the most detail (non-commercial licence)"),
    "dat2":       ("4xNomos2_hq_dat2.pth",          "DAT2 4x, most faithful, slowest"),
    # extra candidates, not built unless named with --variants
    "hat":        ("4xNomos8kSCHAT-L.safetensors",  "HAT-L 4x, smooth"),
    "span":       ("2xNomosUni_span_multijpg.safetensors", "SPAN 2x, fast and soft"),
    "mosr":       ("4xNomos2_hq_mosr.pth",          "MoSR 4x, fast relative of dat2"),
}
DEFAULT = ["aomr", "sgi", "ultrasharp", "dat2"]


def log(text: str):
    line = f"[{time.strftime('%H:%M:%S')}] {text}"
    print(line, flush=True)
    with open(ROOT / "hd" / "work" / "upscale_variants.log", "a", encoding="utf-8") as f:
        f.write(line + "\n")


@torch.inference_mode()
def upscale(desc, img: np.ndarray, tile: int, overlap: int, dtype) -> np.ndarray:
    """img HxWx3 uint8 -> (H*s)x(W*s)x3 uint8. Tiles with feathered seams; the big output is
    accumulated on the CPU so that even a 4x pass of the largest map fits beside the model."""
    s = desc.scale
    h, w, _ = img.shape
    src = torch.from_numpy(img).permute(2, 0, 1).float().div(255.0).unsqueeze(0)
    out = torch.zeros((3, h * s, w * s), dtype=torch.float32)
    acc = torch.zeros((1, h * s, w * s), dtype=torch.float32)
    step = tile - overlap
    ys = list(range(0, max(h - overlap, 1), step))
    xs = list(range(0, max(w - overlap, 1), step))
    for y0 in ys:
        for x0 in xs:
            y1, x1 = min(y0 + tile, h), min(x0 + tile, w)
            y0c, x0c = max(0, y1 - tile), max(0, x1 - tile)
            patch = src[:, :, y0c:y1, x0c:x1].cuda().to(dtype)
            res = desc(patch).float()
            if not torch.isfinite(res).all():
                raise FloatingPointError("non-finite output (reduced precision is not usable for this model)")
            res = res.clamp(0, 1)
            ph, pw = res.shape[2], res.shape[3]
            wy = torch.minimum(torch.arange(1, ph + 1), torch.arange(ph, 0, -1)).float().clamp(max=overlap * s)
            wx = torch.minimum(torch.arange(1, pw + 1), torch.arange(pw, 0, -1)).float().clamp(max=overlap * s)
            wgt = (wy[:, None] * wx[None, :])[None]
            out[:, y0c * s:y1 * s, x0c * s:x1 * s] += res[0].cpu() * wgt
            acc[:, y0c * s:y1 * s, x0c * s:x1 * s] += wgt
    out = (out / acc.clamp(min=1e-6)).clamp(0, 1)
    return (out.permute(1, 2, 0).numpy() * 255.0 + 0.5).astype(np.uint8)


@torch.inference_mode()
def color_fix(hd: np.ndarray, orig: np.ndarray, sigma: float) -> np.ndarray:
    """Give the upscaled picture the colours and brightness of the original.

    The models shift colour a little (measured on nottingham_day: dat2 loses red and gains blue
    - "less warm, less saturated" -, aomr is darker and over-saturated). Seen from a distance
    the HD picture must be the original, so: reduce the HD picture to the original size, take
    the difference to the original, keep only its smooth part (Gaussian, `sigma` original
    pixels) and add that, magnified, to the HD picture. Detail finer than a few pixels stays
    the model's; everything coarser is the original's.
    hd: (2H, 2W, 3) uint8, orig: (H, W, 3) uint8 -> (2H, 2W, 3) float32 0..255
    """
    import torch.nn.functional as F
    dev = "cuda"
    out = np.empty(hd.shape, dtype=np.float32)
    radius = max(1, int(sigma * 3 + 0.5))
    xs = torch.arange(-radius, radius + 1, dtype=torch.float32, device=dev)
    kernel = torch.exp(-(xs * xs) / (2 * sigma * sigma))
    kernel = (kernel / kernel.sum())[None, None, :]
    for c in range(3):  # one channel at a time: the largest map is 32 Mpx at 2x
        h = torch.from_numpy(np.ascontiguousarray(hd[..., c])).to(dev).float()[None, None]
        o = torch.from_numpy(np.ascontiguousarray(orig[..., c])).to(dev).float()[None, None]
        diff = o - F.avg_pool2d(h, 2)
        diff = F.conv2d(F.pad(diff, (radius, radius, 0, 0), mode="reflect"), kernel[:, :, None, :])
        diff = F.conv2d(F.pad(diff, (0, 0, radius, radius), mode="reflect"), kernel[:, :, :, None])
        h = (h + F.interpolate(diff, scale_factor=2, mode="bilinear", align_corners=False)).clamp_(0, 255)
        out[..., c] = h[0, 0].cpu().numpy()
        del h, o, diff
    torch.cuda.empty_cache()
    return out


def pick_precision(desc, sample: np.ndarray, tile: int, overlap: int):
    """Half precision roughly doubles the speed. Use it only if the model declares support AND a
    test tile differs from the full-precision result by less than one 8-bit step on average."""
    ref = upscale(desc, sample, tile, overlap, torch.float32)
    for name, flag, switch, dtype in (("float16", getattr(desc, "supports_half", False), desc.half, torch.float16),
                                      ("bfloat16", getattr(desc, "supports_bfloat16", False), desc.bfloat16, torch.bfloat16)):
        if not flag:
            continue
        try:
            switch()
            got = upscale(desc, sample, tile, overlap, dtype)
            diff = np.abs(got.astype(np.int16) - ref.astype(np.int16))
            if diff.mean() < 0.6 and diff.max() <= 12:
                return dtype, f"{name} (mean diff {diff.mean():.2f}, max {diff.max()} of 255)"
            log(f"    {name}: differs too much from float32 (mean {diff.mean():.2f}, max {diff.max()}) - not used")
        except Exception as exc:  # fall back to full precision
            log(f"    {name}: {type(exc).__name__}: {exc} - not used")
        desc.float()
    desc.float()
    return torch.float32, "float32"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--variants", nargs="*", default=DEFAULT)
    ap.add_argument("--only", nargs="*", default=None, help="map names without .png")
    ap.add_argument("--tile", type=int, default=192)
    ap.add_argument("--overlap", type=int, default=16)
    ap.add_argument("--png", action="store_true", help="also write the 2x pictures as PNG")
    ap.add_argument("--colorfix", type=float, default=3.0,
                    help="colours/brightness from the original: Gaussian sigma in original pixels (0 = off)")
    ap.add_argument("--redo", action="store_true", help="build the pictures again even if they exist")
    ap.add_argument("--list", action="store_true")
    args = ap.parse_args()

    if args.list:
        for name, (file, what) in VARIANTS.items():
            print(f"{name:11s} {file:40s} {what}")
        return

    src_dir = ROOT / "hd" / "maps_png"
    (ROOT / "hd" / "work").mkdir(parents=True, exist_ok=True)
    maps = [p for p in sorted(src_dir.glob("*.png")) if not args.only or p.stem in args.only]
    # smallest first: complete variants of the small levels appear early
    maps.sort(key=lambda p: p.stat().st_size)
    keys = {}
    for png in maps:
        keys[png.stem] = f"{fnv1a64(original_pixels_565(png)):016x}"
    total_mpx = 0.0
    for png in maps:
        with Image.open(png) as im:
            total_mpx += im.size[0] * im.size[1] / 1e6
    log(f"start: variants {args.variants}, {len(maps)} maps, {total_mpx:.0f} Mpx each")

    for variant in args.variants:
        if variant not in VARIANTS:
            log(f"unknown variant '{variant}' (see --list)")
            continue
        model_file, what = VARIANTS[variant]
        out_dir = ROOT / "hd" / "maps" / variant
        out_dir.mkdir(parents=True, exist_ok=True)
        todo = [p for p in maps if args.redo or not (out_dir / f"{keys[p.stem]}.hdmap").exists()]
        if not todo:
            log(f"{variant}: all {len(maps)} maps are there already")
            continue
        desc = load_model(ROOT / "hd" / "models" / model_file)
        sample = np.asarray(Image.open(todo[0]).convert("RGB").crop((0, 0, 256, 256))).copy()
        dtype, how = pick_precision(desc, sample, args.tile, args.overlap)
        log(f"{variant}: {model_file} ({desc.architecture.name} x{desc.scale}, {how}); {len(todo)} maps to do")
        (out_dir / "variant.txt").write_text(
            f"{variant}\nmodel: {model_file}\n{what}\ncolour fix: sigma {args.colorfix} original pixels\n", encoding="utf-8")
        t_variant = time.perf_counter()
        done_mpx = 0.0
        for png in todo:
            img = Image.open(png).convert("RGB")
            w, h = img.size
            t0 = time.perf_counter()
            src_arr = np.asarray(img).copy()
            res = upscale(desc, src_arr, args.tile, args.overlap, dtype)
            hd = Image.fromarray(res, "RGB")
            if desc.scale != 2:
                hd = hd.resize((w * 2, h * 2), Image.LANCZOS)
            hd_arr = np.asarray(hd)
            if args.colorfix > 0:
                hd_arr = color_fix(hd_arr, src_arr, args.colorfix)
            if args.png:
                png_dir = ROOT / "hd" / "maps_hd" / variant
                png_dir.mkdir(parents=True, exist_ok=True)
                Image.fromarray((hd_arr + 0.5).clip(0, 255).astype(np.uint8), "RGB").save(png_dir / png.name)
            px = rgb888_to_565_dithered(hd_arr)
            tmp = out_dir / f"{keys[png.stem]}.tmp"
            with open(tmp, "wb") as f:
                f.write(b"HDMP" + struct.pack("<IIII", 1, w * 2, h * 2, 0))
                f.write(px.astype("<u2").tobytes())
            final = out_dir / f"{keys[png.stem]}.hdmap"
            for attempt in range(120):  # never leaves a half-written file behind
                try:
                    tmp.replace(final)
                    break
                except PermissionError:
                    # the game has this picture mapped (a level that uses it is running)
                    if attempt == 0:
                        log(f"  {final.name} is in use by the game - waiting for it to be released")
                    time.sleep(5)
            else:
                log(f"  {final.name} still in use after 10 minutes - new version left as {tmp.name}")
            dt = time.perf_counter() - t0
            done_mpx += w * h / 1e6
            left = sum(Image.open(p).size[0] * Image.open(p).size[1] for p in todo[todo.index(png) + 1:]) / 1e6
            speed = done_mpx / (time.perf_counter() - t_variant)
            log(f"  {variant:10s} {png.stem:20s} {w}x{h}  {dt:5.0f} s  ({w * h / 1e6 / dt:.3f} Mpx/s)  "
                f"left in this variant: {left:.0f} Mpx ~ {left / max(speed, 1e-6) / 60:.0f} min")
            del res, hd, px
            torch.cuda.empty_cache()
        del desc
        torch.cuda.empty_cache()
        log(f"{variant}: finished in {(time.perf_counter() - t_variant) / 60:.1f} min")
    log("all done")


if __name__ == "__main__":
    main()
