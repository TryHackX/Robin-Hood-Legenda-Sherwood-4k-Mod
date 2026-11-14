"""PNG previews of the HD maps the game really uses.

    python tools\\hd\\hdmap_to_png.py [--variants dat2 sgi ... | --variants all] [--only nottingham_day ...] [--redo]

Input : hd\\maps\\<variant>\\<key>.hdmap   what the mod loads (written by upscale_variants.py)
        hd\\maps_png\\<map>.png            the game's own maps - used only to find <key> of every map
Output: hd\\maps_hd\\<variant>\\<map>.png  the same pixels as in the .hdmap file, for looking at

The .hdmap pictures are 16-bit (RGB565, ordered dither) because that is what the game draws; the PNG
holds exactly those pixels, so it shows what is seen in the game. No model and no graphics card needed.
Without --variants: the variant chosen in SherwoodFix.ini (HDVariant=), else the first folder in hd\\maps.
"""
import argparse
import shutil
import struct
import sys
import time
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
HEADER = struct.Struct("<4sIIII")  # "HDMP", version, width, height, reserved


def fnv1a64(data: bytes) -> int:
    """Same key as upscale_maps.py / the mod: 64-bit FNV-1a over every 7th 16-bit pixel."""
    h = 0xCBF29CE484222325
    prime = 0x100000001B3
    mask = 0xFFFFFFFFFFFFFFFF
    for w in np.frombuffer(data, dtype="<u2")[::7].tolist():
        h = ((h ^ w) * prime) & mask
    return h


def original_pixels_565(png: Path) -> bytes:
    """The exact 16-bit pixels the game has (inverse of the bit expansion in extract_maps.py)."""
    rgb = np.asarray(Image.open(png).convert("RGB")).astype(np.uint16)
    r = (rgb[..., 0] * 31 + 127) // 255
    g = (rgb[..., 1] * 63 + 127) // 255
    b = (rgb[..., 2] * 31 + 127) // 255
    return ((r << 11) | (g << 5) | b).astype("<u2").tobytes()


def read_hdmap(path: Path) -> np.ndarray:
    with open(path, "rb") as f:
        magic, version, width, height, _ = HEADER.unpack(f.read(HEADER.size))
        if magic != b"HDMP" or version != 1:
            raise ValueError(f"{path.name}: not an HD map (header {magic!r}, version {version})")
        px = np.fromfile(f, dtype="<u2", count=width * height)
    if px.size != width * height:
        raise ValueError(f"{path.name}: file is shorter than its header says")
    px = px.reshape(height, width)
    out = np.empty((height, width, 3), dtype=np.uint8)
    out[..., 0] = (((px >> 11) & 0x1F).astype(np.uint16) * 527 + 23) >> 6
    out[..., 1] = (((px >> 5) & 0x3F).astype(np.uint16) * 259 + 33) >> 6
    out[..., 2] = ((px & 0x1F).astype(np.uint16) * 527 + 23) >> 6
    return out


def chosen_variant(all_variants):
    ini = ROOT / "SherwoodFix.ini"
    if ini.exists():
        for line in ini.read_text(encoding="utf-8", errors="replace").splitlines():
            name, _, value = line.partition("=")
            if name.strip().lower() == "hdvariant" and value.strip() in all_variants:
                return value.strip()
    return all_variants[0] if all_variants else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--variants", nargs="*", default=None, help="folder names in hd\\maps, or: all")
    ap.add_argument("--only", nargs="*", default=None, help="map names without .png")
    ap.add_argument("--redo", action="store_true", help="write the pictures again even if they exist")
    args = ap.parse_args()

    maps_dir = ROOT / "hd" / "maps"
    all_variants = sorted(p.name for p in maps_dir.iterdir() if p.is_dir())
    if not args.variants:
        variants = [chosen_variant(all_variants)]
    elif args.variants == ["all"]:
        variants = all_variants
    else:
        variants = args.variants
    unknown = [v for v in variants if v not in all_variants]
    if unknown or not all_variants:
        sys.exit(f"unknown variant(s) {unknown}; there are: {all_variants}")

    originals = [p for p in sorted((ROOT / "hd" / "maps_png").glob("*.png")) if not args.only or p.stem in args.only]
    if not originals:
        sys.exit("no maps in hd\\maps_png match")
    keys = {p.stem: f"{fnv1a64(original_pixels_565(p)):016x}" for p in originals}

    for variant in variants:
        out_dir = ROOT / "hd" / "maps_hd" / variant
        out_dir.mkdir(parents=True, exist_ok=True)
        todo = [p for p in originals if args.redo or not (out_dir / p.name).exists()]
        # a PNG of a dithered 16-bit picture takes about as much room as the .hdmap file itself
        need = sum((maps_dir / variant / f"{keys[p.stem]}.hdmap").stat().st_size
                   for p in todo if (maps_dir / variant / f"{keys[p.stem]}.hdmap").exists())
        free = shutil.disk_usage(out_dir).free
        print(f"{variant}: {len(todo)} of {len(originals)} maps to write, about {need / 2**20:.0f} MB "
              f"({free / 2**30:.1f} GB free)", flush=True)
        if need + 4 * 2**30 > free:
            sys.exit("not enough free disk space (4 GB are kept in reserve) - nothing written")
        written = 0
        for p in todo:
            src = maps_dir / variant / f"{keys[p.stem]}.hdmap"
            if not src.exists():
                print(f"  {p.stem}: no HD map in this variant ({src.name})", flush=True)
                continue
            t0 = time.perf_counter()
            rgb = read_hdmap(src)
            tmp = out_dir / (p.stem + ".tmp")
            Image.fromarray(rgb, "RGB").save(tmp, format="PNG", compress_level=6)
            tmp.replace(out_dir / p.name)
            size = (out_dir / p.name).stat().st_size
            written += size
            print(f"  {p.stem:22s} {rgb.shape[1]}x{rgb.shape[0]}  {size / 2**20:6.1f} MB  "
                  f"{time.perf_counter() - t0:4.1f} s  <- {src.name}", flush=True)
        print(f"{variant}: done, {written / 2**20:.0f} MB in {out_dir}", flush=True)


if __name__ == "__main__":
    main()
