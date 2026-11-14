"""Compare AI upscaling models on crops of the game's graphics (exact 2x output).

    python tools\\hd\\upscale_test.py --image hd\\maps_png\\nottingham_day.png \
        --crop 1030,1330,320,240 --name house --out hd\\compare [--models a.pth b.safetensors ...]

For every model in hd\\models (or the ones listed) the crop is upscaled; 4x models are reduced
to 2x with a Lanczos filter. Writes hd\\compare\\<name>\\<model>.png plus a labelled contact sheet
hd\\compare\\<name>_sheet.png, and prints the speed in megapixels of INPUT per second.
"""
import argparse
import time
from pathlib import Path

import numpy as np
import torch
from PIL import Image, ImageDraw

import spandrel

ROOT = Path(__file__).resolve().parents[2]
MODEL_DIR = ROOT / "hd" / "models"


def load_model(path: Path):
    desc = spandrel.ModelLoader(device="cuda").load_from_file(str(path))
    if not isinstance(desc, spandrel.ImageModelDescriptor):
        raise TypeError(f"{path.name}: not a single-image model")
    desc.eval()
    return desc


@torch.inference_mode()
def upscale(desc, img: np.ndarray, tile: int = 192, overlap: int = 16) -> np.ndarray:
    """img: HxWx3 uint8 -> (H*s)x(W*s)x3 uint8, tiled with feathered seams."""
    s = desc.scale
    h, w, _ = img.shape
    src = torch.from_numpy(img).permute(2, 0, 1).float().div(255.0).unsqueeze(0).cuda()
    out = torch.zeros((1, 3, h * s, w * s), device="cuda")
    acc = torch.zeros((1, 1, h * s, w * s), device="cuda")
    step = tile - overlap
    ys = list(range(0, max(h - overlap, 1), step))
    xs = list(range(0, max(w - overlap, 1), step))
    for y0 in ys:
        for x0 in xs:
            y1, x1 = min(y0 + tile, h), min(x0 + tile, w)
            y0c, x0c = max(0, y1 - tile), max(0, x1 - tile)
            patch = src[:, :, y0c:y1, x0c:x1]
            res = desc(patch).float().clamp(0, 1)
            ph, pw = res.shape[2], res.shape[3]
            # feather: weight falls off towards the tile border
            wy = torch.minimum(torch.arange(1, ph + 1, device="cuda"), torch.arange(ph, 0, -1, device="cuda")).float()
            wx = torch.minimum(torch.arange(1, pw + 1, device="cuda"), torch.arange(pw, 0, -1, device="cuda")).float()
            wgt = (wy.clamp(max=overlap * s)[:, None] * wx.clamp(max=overlap * s)[None, :])[None, None]
            out[:, :, y0c * s:y1 * s, x0c * s:x1 * s] += res * wgt
            acc[:, :, y0c * s:y1 * s, x0c * s:x1 * s] += wgt
    out = (out / acc.clamp(min=1e-6)).clamp(0, 1)
    return (out[0].permute(1, 2, 0).cpu().numpy() * 255.0 + 0.5).astype(np.uint8)


def to_2x(arr: np.ndarray, scale: int, w: int, h: int) -> Image.Image:
    im = Image.fromarray(arr, "RGB")
    if scale != 2:
        im = im.resize((w * 2, h * 2), Image.LANCZOS)
    return im


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", required=True)
    ap.add_argument("--crop", required=True, help="x,y,w,h in source pixels")
    ap.add_argument("--name", required=True)
    ap.add_argument("--out", default="hd\\compare")
    ap.add_argument("--models", nargs="*", default=None)
    ap.add_argument("--sheet-crop", default="", help="x,y,w,h inside the crop shown on the sheet (source px)")
    ap.add_argument("--cols", type=int, default=4)
    args = ap.parse_args()

    x, y, w, h = [int(v) for v in args.crop.split(",")]
    src = Image.open(ROOT / args.image).convert("RGB").crop((x, y, x + w, y + h))
    arr = np.asarray(src)
    out_dir = ROOT / args.out / args.name
    out_dir.mkdir(parents=True, exist_ok=True)

    results = []  # (label, PIL image 2x)
    results.append(("nearest x2 (as now)", src.resize((w * 2, h * 2), Image.NEAREST)))
    results.append(("bicubic x2", src.resize((w * 2, h * 2), Image.BICUBIC)))
    results.append(("lanczos x2", src.resize((w * 2, h * 2), Image.LANCZOS)))

    files = sorted(MODEL_DIR.iterdir()) if not args.models else [MODEL_DIR / m for m in args.models]
    for f in files:
        if f.suffix not in (".pth", ".safetensors"):
            continue
        try:
            desc = load_model(f)
            torch.cuda.synchronize()
            t0 = time.perf_counter()
            res = upscale(desc, arr)
            torch.cuda.synchronize()
            dt = time.perf_counter() - t0
            mpx = (w * h) / 1e6 / dt
            label = f"{f.stem} [{desc.architecture.name} x{desc.scale}] {mpx:.2f} Mpx/s"
            print(f"{f.name:44s} {desc.architecture.name:10s} x{desc.scale}  {dt:6.2f} s  {mpx:6.2f} Mpx in/s  "
                  f"vram {torch.cuda.max_memory_allocated() / 2**20:.0f} MB")
            results.append((label, to_2x(res, desc.scale, w, h)))
            del desc
            torch.cuda.empty_cache()
            torch.cuda.reset_peak_memory_stats()
        except Exception as exc:  # keep going: one unsupported model must not stop the comparison
            print(f"{f.name:44s} FAILED: {type(exc).__name__}: {exc}")

    for label, im in results:
        im.save(out_dir / (label.split(" [")[0].replace(" ", "_").replace("(", "").replace(")", "") + ".png"))

    # contact sheet (optionally only a detail of each result, at 100 %)
    if args.sheet_crop:
        sx, sy, sw, sh = [int(v) * 2 for v in args.sheet_crop.split(",")]
    else:
        sx, sy, sw, sh = 0, 0, w * 2, h * 2
    cols = args.cols
    rows = (len(results) + cols - 1) // cols
    pad, bar = 6, 18
    sheet = Image.new("RGB", (cols * (sw + pad) + pad, rows * (sh + bar + pad) + pad), (24, 24, 24))
    draw = ImageDraw.Draw(sheet)
    for i, (label, im) in enumerate(results):
        cx = pad + (i % cols) * (sw + pad)
        cy = pad + (i // cols) * (sh + bar + pad)
        draw.text((cx + 2, cy + 3), label[:70], fill=(235, 235, 235))
        sheet.paste(im.crop((sx, sy, sx + sw, sy + sh)), (cx, cy + bar))
    sheet_path = ROOT / args.out / f"{args.name}_sheet.png"
    sheet.save(sheet_path)
    print(f"sheet: {sheet_path} {sheet.size}")


if __name__ == "__main__":
    main()
