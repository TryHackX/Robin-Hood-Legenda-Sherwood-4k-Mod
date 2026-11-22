"""Level backgrounds at 4x as PNG pictures to look at (2026-11-22, user: "the forest maps look low on detail - maybe
upscale them to 4K? put them in hd\\maps_png\\4k, I will look at the PNGs and say if they can go in").

    python -I tools/hd/upscale_4k.py --only croisement01_day croisement02_day [--variants dat2 ultrasharp]

Input : hd\\maps_png\\<map>.png (1x, tools\\hd\\extract_maps.py)
Output: hd\\maps_png\\4k\\<map>_<variant>.png - the model's own 4x picture (the HD maps the mod loads are this
        reduced to 2x), with the colours and brightness of the original as in upscale_variants.py (colour fix at 4x:
        the difference to the original, kept smooth, added back).
Finished pictures are skipped (--redo builds them again).
"""
import argparse
import sys
import time
from pathlib import Path

import numpy as np
import torch
from PIL import Image

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from make_inbetweens import game_running  # noqa: E402
from upscale_test import load_model  # noqa: E402
from upscale_variants import VARIANTS, pick_precision, upscale  # noqa: E402

ROOT = HERE.parents[1]


def wait_for_game():
    """The player's game (a visible Game.exe window) gets the graphics card: wait until it is closed."""
    said = False
    while game_running():
        if not said:
            print("the game is running - paused until it is closed", flush=True)
            said = True
        time.sleep(30)
    if said:
        print("the game was closed - going on", flush=True)


@torch.inference_mode()
def color_fix(hd: np.ndarray, orig: np.ndarray, scale: int, sigma: float) -> np.ndarray:
    """As upscale_variants.color_fix, for any whole scale: what the picture reduced to the original's size lacks,
    smoothed (Gaussian, `sigma` original pixels) and magnified, is added to it."""
    import torch.nn.functional as F
    dev = "cuda"
    out = np.empty(hd.shape, dtype=np.uint8)
    radius = max(1, int(sigma * 3 + 0.5))
    xs = torch.arange(-radius, radius + 1, dtype=torch.float32, device=dev)
    kernel = torch.exp(-(xs * xs) / (2 * sigma * sigma))
    kernel = (kernel / kernel.sum())[None, None, :]
    for c in range(3):
        h = torch.from_numpy(np.ascontiguousarray(hd[..., c])).to(dev).float()[None, None]
        o = torch.from_numpy(np.ascontiguousarray(orig[..., c])).to(dev).float()[None, None]
        diff = o - F.avg_pool2d(h, scale)
        diff = F.conv2d(F.pad(diff, (radius, radius, 0, 0), mode="reflect"), kernel[:, :, None, :])
        diff = F.conv2d(F.pad(diff, (0, 0, radius, radius), mode="reflect"), kernel[:, :, :, None])
        h = (h + F.interpolate(diff, scale_factor=scale, mode="bilinear", align_corners=False)).clamp_(0, 255)
        out[..., c] = (h[0, 0] + 0.5).clamp_(0, 255).byte().cpu().numpy()
        del h, o, diff
    torch.cuda.empty_cache()
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", nargs="*", default=None, help="map names (hd/maps_png/<name>.png)")
    ap.add_argument("--variants", nargs="*", default=["dat2"])
    ap.add_argument("--tile", type=int, default=192)
    ap.add_argument("--overlap", type=int, default=16)
    ap.add_argument("--colorfix", type=float, default=3.0)
    ap.add_argument("--redo", action="store_true")
    args = ap.parse_args()
    src_dir = ROOT / "hd" / "maps_png"
    out_dir = src_dir / "4k"
    out_dir.mkdir(parents=True, exist_ok=True)
    maps = [p for p in sorted(src_dir.glob("*.png")) if not args.only or p.stem in args.only]
    for variant in args.variants:
        model_file, _ = VARIANTS[variant]
        desc = None
        for png in maps:
            target = out_dir / f"{png.stem}_{variant}.png"
            if target.exists() and not args.redo:
                print(f"{target.name}: there already")
                continue
            wait_for_game()
            src = np.asarray(Image.open(png).convert("RGB")).copy()
            if desc is None:
                desc = load_model(ROOT / "hd" / "models" / model_file)
                dtype, how = pick_precision(desc, src[:256, :256].copy(), args.tile, args.overlap)
                print(f"{variant}: {model_file} x{desc.scale}, {how}", flush=True)
            t0 = time.perf_counter()
            res = upscale(desc, src, args.tile, args.overlap, dtype)
            h, w = src.shape[:2]
            if desc.scale != 4:
                res = np.asarray(Image.fromarray(res, "RGB").resize((w * 4, h * 4), Image.LANCZOS))
            if args.colorfix > 0:
                res = color_fix(res, src, 4, args.colorfix)
            Image.fromarray(res, "RGB").save(target, optimize=False, compress_level=6)
            print(f"{target.name}: {w * 4}x{h * 4} in {time.perf_counter() - t0:.0f} s", flush=True)
        if desc is not None:
            del desc
            torch.cuda.empty_cache()


if __name__ == "__main__":
    main()
