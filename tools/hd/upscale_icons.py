"""Upscale the frames the game makes at run time: icons above heads, markers, the "Z" of a sleeper ...

    python tools\\hd\\upscale_icons.py --sheet hd\\compare\\ikony_oryginalne.png      (only look at them)
    python tools\\hd\\upscale_icons.py --model sgi [--sheet hd\\compare\\ikony.png]   (make the store)
    python tools\\hd\\upscale_icons.py --compare sgi ultrasharp dat2 --sheet hd\\compare\\ikony_modele.png

These frames are not in the bank file (RobinHood.dic ends at frame 404854): the game builds them when it
loads its interface pictures, so their numbers depend on the order of loading. The mod recognises such a
frame by its content: FNV-1a 64 over "u16 width, u16 height" and the frame's row data.

Input : hd\\work\\runtime_frames\\*.bin   dumps written by the test command "rtdump NAME" (tools\\hctl.ps1; the file
                                        appears in tools\\out\\harness and is copied here by --collect, the default)
        "RTFR", u32 1, u32 count, u32 first number; per frame: u32 number, u16 w, u16 h, u8 flag, u8 0,
        u16 palette, u32 size field, u32 bytes, then the row data ("u16 first, u16 last, pixels"; last = 0xFFFF:
        empty row) - exactly what the game holds in memory.
Output: hd\\sprites\\_icons\\icons.idx    "HDIC", u32 1, u32 count, u32 0, then per frame
                                        { u64 key, u64 offset, u32 size, u16 width, u16 height } (24 bytes)
        hd\\sprites\\_icons\\icons.dat    "HDID" + 12 bytes, then the frames: twice the size of the original, RGB565,
                                        raw or row spans - the same frame layout as sprites.dat (upscale_sprites.py)
Dumps from several levels can be collected; frames with the same content are made once. A frame that is in the
store already is kept (--redo makes all again).
"""
import argparse
import shutil
import struct
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
DUMPS = ROOT / "hd" / "work" / "runtime_frames"
HARNESS = ROOT / "tools" / "out" / "harness"
STORE = ROOT / "hd" / "sprites" / "_icons"
KEY, MARK = 0x07C0, 0x001F
ENTRY = np.dtype([("key", "<u8"), ("off", "<u8"), ("size", "<u4"), ("w", "<u2"), ("h", "<u2")])


def fnv1a64(data: bytes) -> int:
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def content_key(w: int, h: int, rows: bytes) -> int:
    return fnv1a64(struct.pack("<HH", w, h) + rows) or 1


def decode_rows(rows: bytes, w: int, h: int) -> np.ndarray:
    words = np.frombuffer(rows, "<u2")
    img = np.full((h, w), KEY, np.uint16)
    p = 0
    for y in range(h):
        first, last = int(words[p]), int(words[p + 1])
        p += 2
        if last == 0xFFFF:
            continue
        n = last - first + 1
        img[y, first:last + 1] = words[p:p + n]
        p += n
    if p != len(words):
        raise ValueError("row data not consumed exactly")
    return img


def read_dumps(paths):
    """{key: (w, h, flag, image, where)} over all dumps; `where` = "dump:frame number" of the first sighting."""
    frames = {}
    for path in paths:
        d = path.read_bytes()
        magic, version, count, _first = struct.unpack_from("<4sIII", d, 0)
        if magic != b"RTFR" or version != 1:
            print(f"{path.name}: not a run-time frame dump - skipped")
            continue
        p = 16
        n = 0
        # count 0xFFFFFFFF: a file the mod appends to while the game is played - read to its end
        while n < count and p + 20 <= len(d):
            frame, w, h, flag, _pad, palette, _size, nbytes = struct.unpack_from("<IHHBBHII", d, p)
            p += 20
            if p + nbytes > len(d):
                break   # the last record is still being written
            rows = d[p:p + nbytes]
            p += nbytes
            n += 1
            if nbytes == 0 or palette != 0xFFFF or w == 0 or h == 0:
                continue
            key = content_key(w, h, rows)
            if key not in frames:
                try:
                    frames[key] = (w, h, flag, decode_rows(rows, w, h), f"{path.stem}:{frame}")
                except (ValueError, IndexError):
                    print(f"{path.name}: frame {frame} is damaged - skipped")
    return frames


def collect():
    DUMPS.mkdir(parents=True, exist_ok=True)
    for src in sorted(HARNESS.glob("rt_*.bin")):
        dst = DUMPS / src.name
        if not dst.exists() or dst.stat().st_mtime < src.stat().st_mtime:
            shutil.copy2(src, dst)
            print(f"collected {src.name}")


def rgba(px: np.ndarray, zoom: int) -> np.ndarray:
    import cv2
    r = ((px >> 11) & 31).astype(np.float32) * (255.0 / 31.0)
    g = ((px >> 5) & 63).astype(np.float32) * (255.0 / 63.0)
    b = (px & 31).astype(np.float32) * (255.0 / 31.0)
    out = np.dstack([r, g, b, np.where(px == KEY, 0, 255)]).astype(np.uint8)
    out[px == MARK, :3] = (40, 40, 60)
    if zoom != 1:
        out = cv2.resize(out, (out.shape[1] * zoom, out.shape[0] * zoom), interpolation=cv2.INTER_NEAREST)
    return out


def write_sheet(path: Path, columns, labels, zoom: int, width: int = 2400):
    """columns: list of lists of pictures (uint16, same order); column k is shown at zoom/scale so that all
    columns of one item have the same size on the sheet (originals 2x the zoom of the upscaled ones)."""
    from PIL import Image, ImageDraw
    n = len(columns[0])
    cells = []
    for i in range(n):
        parts = []
        for k, col in enumerate(columns):
            parts.append(rgba(col[i], zoom * 2 if (len(columns) > 1 and k == 0) else zoom))
        h = max(p.shape[0] for p in parts)
        w = sum(p.shape[1] for p in parts) + 4 * (len(parts) - 1)
        cell = np.zeros((h, w, 4), np.uint8)
        x = 0
        for p in parts:
            cell[:p.shape[0], x:x + p.shape[1]] = p
            x += p.shape[1] + 4
        cells.append(cell)
    # shelf packing, tallest first
    order = sorted(range(n), key=lambda i: -cells[i].shape[0])
    shelves, x, y, shelf_h = [], 6, 22, 0
    for i in order:
        c = cells[i]
        if x + c.shape[1] + 6 > width and x > 6:
            y += shelf_h + 6
            x, shelf_h = 6, 0
        shelves.append((i, x, y))
        x += c.shape[1] + 6
        shelf_h = max(shelf_h, c.shape[0])
    sheet = Image.new("RGB", (width, y + shelf_h + 6), (86, 110, 78))
    ImageDraw.Draw(sheet).text((6, 4), "   |   ".join(labels), fill=(255, 255, 255))
    for i, x, y in shelves:
        c = Image.fromarray(cells[i], "RGBA")
        sheet.paste(c, (x, y), c)
    path.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(path)
    print(f"sheet: {path} {sheet.size}")


def load_store():
    idx, dat = STORE / "icons.idx", STORE / "icons.dat"
    if not idx.exists() or not dat.exists():
        return {}
    raw = idx.read_bytes()
    if raw[:4] != b"HDIC":
        return {}
    count = struct.unpack_from("<I", raw, 8)[0]
    data = dat.read_bytes()
    out = {}
    for e in np.frombuffer(raw, ENTRY, count, 16):
        out[int(e["key"])] = (int(e["w"]), int(e["h"]), data[int(e["off"]):int(e["off"]) + int(e["size"])])
    return out


def save_store(frames):
    """frames: {key: (w, h, bytes)}; written whole (the store is small), index sorted by key."""
    STORE.mkdir(parents=True, exist_ok=True)
    entries = np.zeros(len(frames), ENTRY)
    blob = bytearray(b"HDID" + struct.pack("<III", 1, 0, 0))
    for k, key in enumerate(sorted(frames)):
        w, h, data = frames[key]
        entries[k] = (key, len(blob), len(data), w, h)
        blob += data
    for name, payload in (("icons.dat", bytes(blob)),
                          ("icons.idx", b"HDIC" + struct.pack("<III", 1, len(frames), 0) + entries.tobytes())):
        tmp = STORE / (name + ".tmp")
        tmp.write_bytes(payload)
        tmp.replace(STORE / name)
    print(f"store: {len(frames)} frames, {len(blob) / 1024:.0f} KB in {STORE}")


def keep_black(px: np.ndarray, img: np.ndarray) -> np.ndarray:
    """Pure black with pure black all around stays pure black. Rings and light effects are pictures on black
    that the game blends by brightness (alpha = green): noise the upscaler leaves in the black area would
    darken the ground beneath the whole rectangle."""
    import cv2
    black = (img == 0).astype(np.uint8)
    if not black.any():
        return px
    core = cv2.erode(black, np.ones((3, 3), np.uint8), borderType=cv2.BORDER_CONSTANT, borderValue=1)
    px[cv2.resize(core, (img.shape[1] * 2, img.shape[0] * 2), interpolation=cv2.INTER_NEAREST).astype(bool)] = 0
    return px


def upscale_all(model_name, images, colorfix):
    """images: list of uint16 originals -> list of uint16 pictures twice the size."""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import torch
    from upscale_sprites import prepare, finish, run_batch
    from upscale_test import load_model
    from upscale_variants import VARIANTS, pick_precision

    desc = load_model(ROOT / "hd" / "models" / VARIANTS[model_name][0])
    sample = np.zeros((96, 96, 3), np.uint8)
    sample[::2, ::3] = 180
    dtype, how = pick_precision(desc, sample, 192, 16)
    print(f"{model_name}: {VARIANTS[model_name][0]} ({desc.architecture.name} x{desc.scale}, {how}); {len(images)} frames")
    out = []
    groups = {}
    preps = []
    for i, img in enumerate(images):
        prep = prepare(img)
        preps.append(prep)
        if prep is not None and prep[2] is not None:
            groups.setdefault(prep[2].shape[:2], []).append(i)
    results = {}
    for shape, members in groups.items():
        per_batch = max(1, 20_000 // (shape[0] * shape[1]))
        for b0 in range(0, len(members), per_batch):
            chunk = members[b0:b0 + per_batch]
            for i, res in zip(chunk, run_batch(desc, dtype, [preps[i][2] for i in chunk])):
                results[i] = res
    for i, img in enumerate(images):
        out.append(None if preps[i] is None else keep_black(finish(preps[i], results.get(i), desc.scale, colorfix), img))
    del desc
    torch.cuda.empty_cache()
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", default="", help="variant name of upscale_variants.py (sgi, ultrasharp, dat2, aomr ...)")
    ap.add_argument("--compare", nargs="*", default=[], help="several models side by side on a sheet; nothing is stored")
    ap.add_argument("--sheet", default="", help="PNG to write (originals alone, before/after, or the comparison)")
    ap.add_argument("--zoom", type=int, default=3)
    ap.add_argument("--colorfix", type=float, default=1.5)
    ap.add_argument("--min-side", type=int, default=0, help="only frames at least this wide and high (for sheets)")
    ap.add_argument("--redo", action="store_true")
    ap.add_argument("--no-collect", action="store_true")
    args = ap.parse_args()

    if not args.no_collect:
        collect()
    frames = read_dumps(sorted(DUMPS.glob("*.bin")))
    keys = sorted(frames, key=lambda k: (-frames[k][1] * frames[k][0], k))
    if args.min_side:
        keys = [k for k in keys if min(frames[k][0], frames[k][1]) >= args.min_side]
    print(f"{len(frames)} different run-time frames in {len(list(DUMPS.glob('*.bin')))} dump(s); {len(keys)} selected")
    if not keys:
        return

    def original(k):
        img = frames[k][3].copy()
        if frames[k][2] == 0:
            img[img == MARK] = MARK - 1   # flag 0: the game copies 0x001F as a plain colour, it is no shadow marker
        return img

    originals = [original(k) for k in keys]
    sheet = ROOT / args.sheet if args.sheet else None

    if args.compare:
        columns, labels = [originals], ["oryginal (x2)"]
        for name in args.compare:
            columns.append(upscale_all(name, originals, args.colorfix))
            labels.append(name)
        ok = [i for i in range(len(keys)) if all(col[i] is not None for col in columns)]
        if sheet:
            write_sheet(sheet, [[col[i] for i in ok] for col in columns], labels, args.zoom)
        return

    if not args.model:
        if sheet:
            write_sheet(sheet, [originals], ["klatki tworzone przez gre w locie (ikony, znaczniki)"], args.zoom * 2)
        return

    store = {} if args.redo else load_store()
    todo = [i for i, k in enumerate(keys) if k not in store]
    print(f"{len(todo)} to make, {len(keys) - len(todo)} in the store already")
    made = upscale_all(args.model, [originals[i] for i in todo], args.colorfix) if todo else []
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from upscale_sprites import encode_spans
    for i, px in zip(todo, made):
        if px is None:
            continue
        raw = px.astype("<u2").tobytes()
        spans = encode_spans(px)
        store[keys[i]] = (px.shape[1], px.shape[0], spans if len(spans) < len(raw) else raw)
    save_store(store)
    if sheet and todo:
        ok = [(originals[i], px) for i, px in zip(todo, made) if px is not None]
        write_sheet(sheet, [[a for a, _ in ok], [b for _, b in ok]], ["oryginal (x2)", args.model], args.zoom)


if __name__ == "__main__":
    main()
