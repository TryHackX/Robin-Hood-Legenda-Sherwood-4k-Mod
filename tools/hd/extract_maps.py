"""Extract the level backgrounds from DATA\\Levels\\*\\*.map (read-only on the game files).

File layout (found 2026-11-14 by looking at the headers):
    u16 width, u16 height, u32 2, u32 packed_size, then a bzip2 stream.
What the stream unpacks to is reported by this script (expected: width*height 16-bit RGB565
pixels, possibly followed by more data).

Usage:
    python tools\\hd\\extract_maps.py --info                 sizes and unpacked length of every map
    python tools\\hd\\extract_maps.py --out hd\\maps_png      write one PNG per distinct map file
"""
import argparse
import bz2
import hashlib
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LEVELS = ROOT / "DATA" / "Levels"


def read_map(path: Path):
    raw = path.read_bytes()
    width, height, kind, packed = struct.unpack_from("<HHII", raw, 0)
    dec = bz2.BZ2Decompressor()
    data = dec.decompress(raw[12:])
    return width, height, kind, packed, data, len(raw) - 12 - len(dec.unused_data), dec.unused_data


def rgb565_to_rgb(data: bytes, width: int, height: int):
    import numpy as np

    px = np.frombuffer(data, dtype="<u2", count=width * height).reshape(height, width)
    r = ((px >> 11) & 0x1F).astype(np.uint16)
    g = ((px >> 5) & 0x3F).astype(np.uint16)
    b = (px & 0x1F).astype(np.uint16)
    out = np.empty((height, width, 3), dtype=np.uint8)
    out[..., 0] = (r * 527 + 23) >> 6
    out[..., 1] = (g * 259 + 33) >> 6
    out[..., 2] = (b * 527 + 23) >> 6
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--info", action="store_true")
    ap.add_argument("--out", type=str, default="")
    args = ap.parse_args()

    files = sorted(LEVELS.rglob("*.map"), key=lambda p: (p.name.lower(), p.parent.name.lower()))
    if not files:
        sys.exit(f"no .map files under {LEVELS}")
    seen = {}
    for f in files:
        width, height, kind, packed, data, used, rest = read_map(f)
        digest = hashlib.sha1(data).hexdigest()[:10]
        variant = f.parent.name
        expect = width * height * 2
        note = "== w*h*2" if len(data) == expect else f"w*h*2={expect}, extra={len(data) - expect}"
        dup = f" (same pixels as {seen[digest]})" if digest in seen else ""
        print(f"{variant:8s} {f.name:18s} {width}x{height} kind={kind} packed={packed} used={used} "
              f"trailing={len(rest)} unpacked={len(data)} {note} sha={digest}{dup}")
        if digest in seen:
            continue
        seen[digest] = f"{variant}/{f.name}"
        if args.out and len(data) >= expect:
            from PIL import Image

            out_dir = ROOT / args.out
            out_dir.mkdir(parents=True, exist_ok=True)
            name = f"{f.stem.lower()}_{variant.lower()}.png"
            Image.fromarray(rgb565_to_rgb(data, width, height), "RGB").save(out_dir / name, optimize=False)
            print(f"    -> {args.out}\\{name}")


if __name__ == "__main__":
    main()
