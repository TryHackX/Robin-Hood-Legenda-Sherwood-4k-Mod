"""Upscale sprite frames of the game's bank 2x with the AI models used for the level backgrounds.

    python tools\\hd\\upscale_sprites.py --kind rle                       (lossless frames, 4 variants)
    python tools\\hd\\upscale_sprites.py --kind vq --variants sgi --store _common
    python tools\\hd\\upscale_sprites.py --variants sgi --frames 5067 5067 --sheet hd\\compare\\roof.png
    python tools\\hd\\upscale_sprites.py --variants sgi --store _common --ids-file tools\\out\\harness\\frames.txt

Input : hd\\work\\robinhood.dic / .bks      (copies of DATA\\RobinHood.*; tools\\hd\\extract_sprites.py)
Output: hd\\sprites\\<store>\\sprites.idx     "HDSI", u32 version=1, u32 nFrames, u32 0, then per bank frame
                                           { u64 offset, u32 size, u16 width, u16 height } (16 bytes; all 0 = not there)
        hd\\sprites\\<store>\\sprites.dat     "HDSD" + 12 bytes, then the frames. A frame is exactly twice the bank
                                           frame's size, RGB565 little endian, rows top to bottom, either
                                             raw   (size == width*height*2), or
                                             spans (anything else): per row "u16 first, u16 count" + count pixels;
                                                   the rest of the row is transparent.
                                           0x07C0 = transparent (the game's colour key), 0x001F = shadow marker,
                                           as in the bank; no other pixel has these two values.
<store> is the variant name unless --store is given. The mod (src\\ddm\\ddm_hd_sprites.cpp) looks a frame up
in the store named like the HD background variant, then in "_common".

Which model for what (hd\\compare\\postacie_sprites.png): the lossless frames (roofs, trees, objects) go
through every variant's own model, like the backgrounds. The block-compressed frames (characters,
animations) only look clean with sgi / ultrasharp (dat2 and aomr turn the compression noise into
texture), so they are made once, with sgi, into "_common".

How a frame is made: colours of the opaque pixels are spread into the transparent area (so the model
sees no hard edge against a key colour), the picture is upscaled like a level background, its colours
and brightness are brought back to the original's, and the three classes opaque / shadow marker /
transparent are magnified separately (cubic + slight blur) and decided per HD pixel - slanted
outlines instead of 2x2 steps.

Frames of the same size are sent through the model together. Finished frames are skipped: the run can
be stopped and continued; the index is rewritten every minute.
"""
import argparse
import struct
import sys
import time
from pathlib import Path

import cv2
import numpy as np
import torch

sys.path.insert(0, str(Path(__file__).resolve().parent))
from extract_sprites import Bank, KEY, MARK  # noqa: E402
from upscale_test import load_model  # noqa: E402
from upscale_variants import VARIANTS, DEFAULT, upscale, pick_precision  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
PAD = 8             # pixels of context around a frame while it is upscaled
BATCH_PIXELS = 280_000  # padded input pixels sent through the model at once
BAYER8 = np.array([
    [0, 32, 8, 40, 2, 34, 10, 42], [48, 16, 56, 24, 50, 18, 58, 26],
    [12, 44, 4, 36, 14, 46, 6, 38], [60, 28, 52, 20, 62, 30, 54, 22],
    [3, 35, 11, 43, 1, 33, 9, 41], [51, 19, 59, 27, 49, 17, 57, 25],
    [15, 47, 7, 39, 13, 45, 5, 37], [63, 31, 55, 23, 61, 29, 53, 21]], dtype=np.float32) / 64.0
ENTRY = np.dtype([("off", "<u8"), ("size", "<u4"), ("w", "<u2"), ("h", "<u2")])


def log(text: str):
    line = f"[{time.strftime('%H:%M:%S')}] {text}"
    print(line, flush=True)
    with open(ROOT / "hd" / "work" / "upscale_sprites.log", "a", encoding="utf-8") as f:
        f.write(line + "\n")


def rgb_from_565(img: np.ndarray) -> np.ndarray:
    r = ((img >> 11) & 31).astype(np.float32) * (255.0 / 31.0)
    g = ((img >> 5) & 63).astype(np.float32) * (255.0 / 63.0)
    b = (img & 31).astype(np.float32) * (255.0 / 31.0)
    return np.dstack([r, g, b])


def bleed(rgb: np.ndarray, valid: np.ndarray) -> np.ndarray:
    """Fill the pixels that are not `valid` with colours spread from the valid ones (pull-push
    over an image pyramid): the valid pixels stay exactly as they are."""
    weight = valid.astype(np.float32)
    levels = [(rgb * weight[..., None], weight)]
    while min(levels[-1][1].shape) > 1:
        c, wt = levels[-1]
        nh, nw = max(1, c.shape[0] // 2), max(1, c.shape[1] // 2)
        levels.append((cv2.resize(c, (nw, nh), interpolation=cv2.INTER_AREA).reshape(nh, nw, 3),
                       cv2.resize(wt, (nw, nh), interpolation=cv2.INTER_AREA).reshape(nh, nw)))
    c, wt = levels[-1]
    fill = c / np.maximum(wt, 1e-6)[..., None]
    for c, wt in reversed(levels[:-1]):
        up = cv2.resize(fill, (c.shape[1], c.shape[0]), interpolation=cv2.INTER_LINEAR).reshape(c.shape)
        cover = np.clip(wt * 4.0, 0.0, 1.0)[..., None]  # trust a level once a quarter of it is valid
        fill = np.where(cover > 0, (c / np.maximum(wt, 1e-6)[..., None]) * cover + up * (1 - cover), up)
    fill[valid] = rgb[valid]
    return fill


def color_fix_cpu(hd: np.ndarray, orig: np.ndarray, sigma: float) -> np.ndarray:
    h, w, _ = orig.shape
    low = cv2.resize(hd, (w, h), interpolation=cv2.INTER_AREA)
    diff = cv2.GaussianBlur(orig - low, (0, 0), sigma, borderType=cv2.BORDER_REFLECT)
    return np.clip(hd + cv2.resize(diff, (w * 2, h * 2), interpolation=cv2.INTER_LINEAR), 0, 255)


def to_565(rgb: np.ndarray) -> np.ndarray:
    h, w, _ = rgb.shape
    thr = np.tile(BAYER8, (h // 8 + 1, w // 8 + 1))[:h, :w]
    r = np.clip(np.floor(rgb[..., 0] * (31.0 / 255.0) + thr), 0, 31).astype(np.uint16)
    g = np.clip(np.floor(rgb[..., 1] * (63.0 / 255.0) + thr), 0, 63).astype(np.uint16)
    b = np.clip(np.floor(rgb[..., 2] * (31.0 / 255.0) + thr), 0, 31).astype(np.uint16)
    return (r << 11) | (g << 5) | b


def soft_mask(mask: np.ndarray, size) -> np.ndarray:
    """A 1-bit mask magnified 2x: cubic, then a slight blur, so that a staircase of whole pixels
    becomes a slanted line when it is cut at 0.5 again."""
    m = cv2.resize(mask.astype(np.float32), size, interpolation=cv2.INTER_CUBIC)
    return cv2.GaussianBlur(m, (0, 0), 0.8, borderType=cv2.BORDER_REPLICATE)


def encode_spans(px: np.ndarray) -> bytes:
    """Rows as "u16 first, u16 count, count pixels": only the part of each row between its first
    and last non-transparent pixel is stored. The reader tells this from the raw layout by size."""
    h, w = px.shape
    solid = px != KEY
    any_row = solid.any(axis=1)
    first = np.where(any_row, solid.argmax(axis=1), 0).astype(np.int64)
    last = np.where(any_row, w - 1 - solid[:, ::-1].argmax(axis=1), -1).astype(np.int64)
    count = last - first + 1
    cols = np.arange(w)[None, :]
    inside = (cols >= first[:, None]) & (cols <= last[:, None])
    total = int(count.sum())
    out = np.empty(2 * h + total, np.uint16)
    row_start = 2 * np.arange(h) + np.concatenate(([0], np.cumsum(count)[:-1]))
    out[row_start] = first
    out[row_start + 1] = count
    if total:
        row_of = np.repeat(np.arange(h), count)
        out[np.arange(total) + 2 * (row_of + 1)] = px[inside]
    return out.astype("<u2").tobytes()


def decode_spans(data: bytes, w: int, h: int) -> np.ndarray:
    words = np.frombuffer(data, "<u2")
    px = np.full((h, w), KEY, np.uint16)
    p = 0
    for y in range(h):
        first, count = int(words[p]), int(words[p + 1])
        p += 2
        px[y, first:first + count] = words[p:p + count]
        p += count
    assert p == len(words)
    return px


def prepare(img: np.ndarray):
    """(opaque mask, marker mask, padded float picture for the model) or None for an empty frame."""
    marker = img == MARK
    opaque = (img != KEY) & ~marker
    if not opaque.any():
        return (opaque, marker, None) if marker.any() else None
    base = bleed(rgb_from_565(img), opaque)
    return opaque, marker, np.pad(base, ((PAD, PAD), (PAD, PAD), (0, 0)), mode="edge")


def finish(prep, res: np.ndarray, scale: int, sigma: float) -> np.ndarray:
    """prep from prepare(), res = the model's output for the padded picture -> (2h, 2w) uint16."""
    opaque, marker, padded = prep
    h, w = opaque.shape
    size = (w * 2, h * 2)
    if padded is None:  # a shadow and nothing else
        out = np.full((h * 2, w * 2), KEY, np.uint16)
        out[soft_mask(marker, size) >= 0.5] = MARK
        return out
    res = res.astype(np.float32)
    ph, pw = padded.shape[:2]
    if scale != 2:
        res = cv2.resize(res, (pw * 2, ph * 2), interpolation=cv2.INTER_LANCZOS4)
    if sigma > 0:
        res = color_fix_cpu(res, padded, sigma)
    hd = res[PAD * 2:PAD * 2 + h * 2, PAD * 2:PAD * 2 + w * 2]
    # classes, each magnified by itself; the largest wins
    op2 = soft_mask(opaque, size)
    mk2 = soft_mask(marker, size) if marker.any() else np.zeros_like(op2)
    tr2 = 1.0 - op2 - mk2
    px = to_565(hd)
    px[px == KEY] = KEY | 1      # real colours must not look like the key or the marker
    px[px == MARK] = MARK - 1
    px[(mk2 > op2) & (mk2 >= tr2)] = MARK
    px[(tr2 > op2) & (tr2 > mk2)] = KEY
    return px


def make_hd(desc, dtype, img: np.ndarray, tile: int, overlap: int, sigma: float):
    """One frame, start to end (used by sprite_model_test.py and for frames larger than a tile)."""
    prep = prepare(img)
    if prep is None:
        return None
    res = None
    if prep[2] is not None:
        res = upscale(desc, np.clip(prep[2] + 0.5, 0, 255).astype(np.uint8), tile, overlap, dtype)
    return finish(prep, res, desc.scale, sigma)


@torch.inference_mode()
def run_batch(desc, dtype, padded_list):
    """Same-size padded pictures (float, 0..255) through the model in one go -> list of float arrays."""
    arr = np.stack([np.clip(p + 0.5, 0, 255).astype(np.uint8) for p in padded_list])
    t = torch.from_numpy(arr).permute(0, 3, 1, 2).cuda().float().div_(255.0).to(dtype)
    out = desc(t).float().clamp_(0, 1).mul_(255.0).permute(0, 2, 3, 1).cpu().numpy()
    return [out[k] for k in range(out.shape[0])]


def frame_to_rgba(px: np.ndarray, scale_up: int = 1) -> np.ndarray:
    rgb = rgb_from_565(px).astype(np.uint8)
    a = np.where(px == KEY, 0, 255).astype(np.uint8)
    rgb[px == MARK] = (40, 40, 60)
    out = np.dstack([rgb, a])
    if scale_up != 1:
        out = cv2.resize(out, (out.shape[1] * scale_up, out.shape[0] * scale_up), interpolation=cv2.INTER_NEAREST)
    return out


def write_index(path: Path, entries: np.ndarray):
    tmp = path.with_suffix(".tmp")
    with open(tmp, "wb") as f:
        f.write(b"HDSI" + struct.pack("<III", 1, len(entries), 0))
        f.write(entries.tobytes())
    for _ in range(50):
        try:
            tmp.replace(path)
            return
        except PermissionError:  # the game is reading it this very moment
            time.sleep(0.2)


def write_sheet(path: Path, items):
    from PIL import Image
    rows = []
    for _, img, px in items:
        a = frame_to_rgba(img, 2)
        b = frame_to_rgba(px, 1)
        row = np.zeros((a.shape[0], a.shape[1] * 2 + 8, 4), np.uint8)
        row[..., :3] = (70, 110, 70)
        row[..., 3] = 255
        for k, part in enumerate((a, b)):
            x0 = k * (a.shape[1] + 8)
            alpha = part[..., 3:4].astype(np.float32) / 255.0
            region = row[:, x0:x0 + a.shape[1], :3].astype(np.float32)
            row[:, x0:x0 + a.shape[1], :3] = (region * (1 - alpha) + part[..., :3] * alpha).astype(np.uint8)
        rows.append(row)
    width = max(r.shape[1] for r in rows)
    sheet = np.zeros((sum(r.shape[0] + 8 for r in rows), width, 4), np.uint8)
    sheet[..., :3] = (30, 30, 30)
    sheet[..., 3] = 255
    y = 0
    for r in rows:
        sheet[y:y + r.shape[0], :r.shape[1]] = r
        y += r.shape[0] + 8
    path.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(sheet, "RGBA").convert("RGB").save(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--variants", nargs="*", default=DEFAULT)
    ap.add_argument("--store", default="", help="folder under hd\\sprites (default: the variant's name); needs one variant")
    ap.add_argument("--kind", choices=["rle", "vq", "all"], default="rle")
    ap.add_argument("--frames", type=int, nargs=2, default=None, help="first and last bank frame")
    ap.add_argument("--ids-file", default="", help="text file with bank frame numbers (first number of each line)")
    ap.add_argument("--palettes-of", default="", help="like --ids-file, but takes every frame that shares a palette "
                                                      "(= belongs to the same character or object family) with a listed one")
    ap.add_argument("--min-area", type=int, default=0)
    ap.add_argument("--max-area", type=int, default=10 ** 9)
    ap.add_argument("--limit", type=int, default=0, help="stop after this many frames per variant")
    ap.add_argument("--tile", type=int, default=192)
    ap.add_argument("--overlap", type=int, default=16)
    ap.add_argument("--colorfix", type=float, default=1.5)
    ap.add_argument("--redo", action="store_true")
    ap.add_argument("--sheet", default="", help="PNG with before/after of some frames done in this run")
    ap.add_argument("--shard", default="", help="I/N: only frames whose number modulo N is I (several processes side "
                                                "by side, each with its own --store; merge with merge_sprite_stores.py)")
    ap.add_argument("--batch-pixels", type=int, default=0, help="input pixels per batch (default 280000; less when "
                                                                 "several processes share the graphics card)")
    ap.add_argument("--skip-store", nargs="*", default=[], help="stores whose frames need not be made again")
    args = ap.parse_args()
    if args.store and len(args.variants) != 1:
        ap.error("--store needs exactly one variant (the model to use)")

    bank = Bank()
    fr = bank.frames
    area = fr["w"].astype(np.int64) * fr["h"]
    sel = (area >= args.min_area) & (area <= args.max_area)
    if args.ids_file or args.palettes_of:
        wanted = np.zeros(len(fr), bool)
        for line in Path(args.ids_file or args.palettes_of).read_text(encoding="utf-8", errors="replace").splitlines():
            first = line.split()[0] if line.split() else ""
            if first.isdigit() and int(first) < len(fr):
                wanted[int(first)] = True
        if args.palettes_of:
            pals = set(int(p) for p in fr["pal"][wanted].tolist()) - {0xFFFF}
            wanted = np.isin(fr["pal"], sorted(pals))
        sel &= wanted
    elif args.kind == "rle":
        sel &= fr["pal"] == 0xFFFF
    elif args.kind == "vq":
        sel &= fr["pal"] != 0xFFFF
    if args.frames:
        rng = np.zeros(len(fr), bool)
        rng[args.frames[0]:args.frames[1] + 1] = True
        sel &= rng
    if args.shard:
        part, parts = [int(v) for v in args.shard.split("/")]
        sel &= (np.arange(len(fr)) % parts) == part
    for name in args.skip_store:  # already made elsewhere
        raw = (ROOT / "hd" / "sprites" / name / "sprites.idx").read_bytes()
        have = np.frombuffer(raw, ENTRY, struct.unpack_from("<I", raw, 8)[0], 16)["size"] != 0
        sel &= ~have[:len(fr)]
    ids = np.nonzero(sel)[0]
    ids = ids[np.argsort(-area[ids], kind="stable")]  # big frames first: roofs, trees
    (ROOT / "hd" / "work").mkdir(parents=True, exist_ok=True)
    log(f"start: {len(ids)} frames, {area[ids].sum() / 1e6:.1f} Mpx, variants {args.variants}" + (f", store {args.store}" if args.store else ""))

    for variant in args.variants:
        if variant not in VARIANTS:
            log(f"unknown variant '{variant}'")
            continue
        store = args.store or variant
        out_dir = ROOT / "hd" / "sprites" / store
        out_dir.mkdir(parents=True, exist_ok=True)
        idx_path, dat_path = out_dir / "sprites.idx", out_dir / "sprites.dat"
        entries = np.zeros(len(fr), ENTRY)
        if idx_path.exists() and dat_path.exists():
            raw = idx_path.read_bytes()
            if raw[:4] == b"HDSI" and struct.unpack_from("<I", raw, 8)[0] == len(fr):
                entries = np.frombuffer(raw, ENTRY, len(fr), 16).copy()
        if not dat_path.exists():
            dat_path.write_bytes(b"HDSD" + struct.pack("<III", 1, 0, 0))
        todo = [int(i) for i in ids if args.redo or entries["size"][i] == 0]
        if args.limit:
            todo = todo[:args.limit]
        if not todo:
            log(f"{store}: nothing to do")
            continue
        model_file = VARIANTS[variant][0]
        desc = load_model(ROOT / "hd" / "models" / model_file)
        first_img = bank.decode16(todo[0])
        sample = np.clip(bleed(rgb_from_565(first_img), first_img != KEY)[:192, :192] + 0.5, 0, 255).astype(np.uint8)
        if min(sample.shape[:2]) < 64:
            sample = np.pad(sample, ((0, 64), (0, 64), (0, 0)), mode="edge")
        dtype, how = pick_precision(desc, sample, args.tile, args.overlap)
        log(f"{store}: {model_file} ({desc.architecture.name} x{desc.scale}, {how}); {len(todo)} frames, {area[todo].sum() / 1e6:.1f} Mpx")

        # Attention models (DAT, HAT ...) need far more memory per pixel than convolutional ones:
        # with 280 000 pixels per batch DAT2 filled the 12 GB card, spilled into system memory and
        # crawled (220 frames in 5 minutes, measured 2026-11-14). They get small batches.
        heavy = desc.architecture.name.upper() not in ("ESRGAN", "SPAN", "MOSR", "COMPACT", "SRVGGNETCOMPACT", "REALPLKSR")
        batch_pixels = 20_000 if heavy else (args.batch_pixels or BATCH_PIXELS)
        log(f"  batches of up to {batch_pixels} input pixels ({'attention model' if heavy else 'convolutional model'})")

        # work list: frames that fit a tile are grouped by size and batched, the rest go one by one
        limit = args.tile - 2 * PAD
        groups = {}
        order = []
        for i in todo:
            w, h = int(fr["w"][i]), int(fr["h"][i])
            if w <= limit and h <= limit:
                key = (h, w)
                if key not in groups:
                    groups[key] = []
                    order.append(key)
                groups[key].append(i)
            else:
                order.append(i)

        t0 = time.perf_counter()
        last_save = t0
        state = {"px": 0, "n": 0, "bytes": 0}
        sheet_items = []

        with open(dat_path, "r+b") as dat:
            dat.seek(0, 2)

            def store_frame(i, img, px):
                if px is None:
                    return
                raw = px.astype("<u2").tobytes()
                spans = encode_spans(px)
                data = spans if len(spans) < len(raw) else raw
                entries[i] = (dat.tell(), len(data), px.shape[1], px.shape[0])
                dat.write(data)
                state["bytes"] += len(data)
                if args.sheet and len(sheet_items) < 8:
                    sheet_items.append((i, img, px))

            def progress(force=False):
                nonlocal last_save
                now = time.perf_counter()
                if not force and now - last_save < 60:
                    return
                last_save = now
                dat.flush()
                write_index(idx_path, entries)
                el = now - t0
                left = (area[todo].sum() - state["px"]) / max(state["px"] / el, 1)
                log(f"  {store:10s} {state['n']}/{len(todo)} frames, {state['px'] / 1e6 / el:.3f} Mpx/s, "
                    f"{state['bytes'] / 2 ** 20:.0f} MB written, left ~{left / 60:.0f} min")

            for item in order:
                if isinstance(item, tuple):
                    h, w = item
                    members = groups[item]
                    per_batch = max(1, batch_pixels // ((h + 2 * PAD) * (w + 2 * PAD)))
                    for b0 in range(0, len(members), per_batch):
                        chunk = members[b0:b0 + per_batch]
                        imgs = [bank.decode16(i) for i in chunk]
                        preps = [prepare(img) for img in imgs]
                        need = [k for k, p in enumerate(preps) if p is not None and p[2] is not None]
                        results = run_batch(desc, dtype, [preps[k][2] for k in need]) if need else []
                        res_of = dict(zip(need, results))
                        for k, i in enumerate(chunk):
                            if preps[k] is not None:
                                store_frame(i, imgs[k], finish(preps[k], res_of.get(k), desc.scale, args.colorfix))
                            state["px"] += imgs[k].size
                            state["n"] += 1
                        progress()
                else:
                    img = bank.decode16(item)
                    store_frame(item, img, make_hd(desc, dtype, img, args.tile, args.overlap, args.colorfix))
                    state["px"] += img.size
                    state["n"] += 1
                    progress()
            progress(force=True)
        del desc
        torch.cuda.empty_cache()
        log(f"{store}: finished in {(time.perf_counter() - t0) / 60:.1f} min, data file {dat_path.stat().st_size / 2 ** 20:.0f} MB")

        if args.sheet and sheet_items:
            p = ROOT / (args.sheet if args.sheet.lower().endswith(".png") else args.sheet + ".png")
            p = p.with_name(p.stem + f"_{store}.png")
            write_sheet(p, sheet_items)
            log(f"sheet: {p}")
    log("all done")


if __name__ == "__main__":
    main()
