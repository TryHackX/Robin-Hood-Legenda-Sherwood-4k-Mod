"""In-between frames for sprite animations (switched on in the game by HDInBetween=1 / F9).

    python -I tools\\hd\\make_inbetweens.py --files Characters/ --jobs 3 --half     (all characters, 3 processes)
    python -I tools\\hd\\make_inbetweens.py --files RobinTown.rhs                   (one file, one process)
    python -I tools\\hd\\make_inbetweens.py --files Characters/ --survey 300 --sheet hd\\compare\\posrednie_proba_postacie.png
    python -I tools\\hd\\make_inbetweens.py --files Characters/ --selftest 240     (speed and precision check)
    python -I tools\\hd\\make_inbetweens.py --stats

--files: endings of .rhs file names ("RobinTown.rhs"), or folders when they end with "/" ("Characters/");
see hd\\work\\frame_index.csv. Everything can be stopped and started again: finished pairs are kept.
While the game runs (Game.exe), the work pauses and the graphics card memory is given back.

The game shows one animation frame per 40 ms tick (or longer). The mod draws extra pictures between the
ticks; with this store it shows, during the first part of a tick in which a sprite went from frame A to
frame B, a generated frame "half-way between A and B" - the animation runs at twice the frame rate.

For every pair of frames that follow each other in an animation of the chosen .rhs files:
  * both upscaled frames (hd\\sprites\\_common) are put on a common canvas using the frame offsets of the .rhs,
  * the canvas is magnified 4x (bicubic, on the graphics card) and the RIFE v4.26 network
    (tools\\hd\\rife_net.py, weights hd\\models_vfi\\rife_v4.26_dl\\flownet.pkl) gives the motion between the two;
    colour, opacity and shadow of the sprite are moved along that motion and reduced 4x again, so the result
    is a sprite frame (figure / shadow / transparent),
  * a quality check decides whether the frame is usable: the two source frames, each moved to the middle,
    must agree with each other (colour difference on the figure and difference of the silhouettes). Pairs
    that are no real motion (last -> first frame of an animation that does not loop, cuts) fail it and the
    game then shows the animation as it always did.
A pair is stored once for both directions (A->B and B->A).

Output: hd\\sprites\\_between\\between.idx   "HDBT", u32 1, u32 count, u32 0, then per pair, sorted by (a, b), a < b:
                                           { u32 a, u32 b, u64 offset, u32 size, u16 w, u16 h,
                                             i16 dxA, dyA, dxB, dyB }   (32 bytes; size 0 = rejected by the check)
                                           dx/dy: where the in-between frame's top-left corner is, relative to
                                           the top-left corner of frame A / of frame B, in HD pixels.
        hd\\sprites\\_between\\between.dat   "HDBD" + 12 bytes, then the frames (raw or row spans, as in sprites.dat).
        hd\\work\\between_log*.csv           one line per pair: the measurements of the quality check.
With --jobs N the work is split over N processes (stores _between_part0 ...), merged into _between at the
end, and then the last -> first pairs that are steps of a loop are made (--loops).
Run with "python -I": the script reads a downloaded weights file.
"""
import argparse
import csv
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

import cv2
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import interp_test as it  # noqa: E402
from extract_sprites import KEY, MARK  # noqa: E402
from upscale_sprites import encode_spans, to_565  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
SPRITES = ROOT / "hd" / "sprites"
WORK = ROOT / "hd" / "work"
MAIN = "_between"
ENTRY = np.dtype([("a", "<u4"), ("b", "<u4"), ("off", "<u8"), ("size", "<u4"), ("w", "<u2"), ("h", "<u2"),
                  ("dxa", "<i2"), ("dya", "<i2"), ("dxb", "<i2"), ("dyb", "<i2")])
MARGIN = 8
MAX_CANVAS = 400 * 400  # HD pixels; pairs on a larger canvas (frames far apart, huge figures) get no in-between frame
# Quality check, picked by looking at a sorted sample of 240 pairs of Robin (hd\compare\posrednie_proba.png,
# 2026-11-14): up to colour ~17 the frames are clean, at 27-36 soft but one coherent figure, beyond that
# smeared. Pairs seen only as "last -> first frame" are mostly cuts (another pose or direction) and look like
# a real step only when both values are small.
MAX_COLOUR = 32.0        # of 255: the two moved source frames differ by more than this on the figure -> rejected
MAX_SHAPE = 0.30         # share of the silhouette on which the two moved frames disagree -> rejected
MAX_COLOUR_WRAP = 20.0   # the same for pairs seen only as last -> first
MAX_SHAPE_WRAP = 0.10
# Thin parts (sword blades, spears, bows; thinner than ~5 HD pixels) that move fast can vanish from the
# in-between frame - the two measures above hardly notice, the parts are small. Seen on the sample of all
# characters (2026-11-14): a sword swing whose middle frame held only the hilt. The middle frame must keep at
# least half of the thin parts the two frames have on average (when they have some at all).
THIN_KERNEL = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (5, 5))
THIN_MIN_PIXELS = 40
MIN_THIN_KEPT = 0.5


def log(text):
    print(f"[{time.strftime('%H:%M:%S')}] {text}", flush=True)


def store_dir(name):
    return SPRITES / name


def log_path(name):
    return WORK / ("between_log.csv" if name == MAIN else f"between_log_{name}.csv")


def matches(file_name, patterns):
    return any(file_name.startswith(p) if p.endswith("/") else file_name.endswith(p) for p in patterns)


def read_animations(patterns):
    """{(file, profile, anim): [frames in playing order]} and {frame: (x, y)} for the chosen .rhs files."""
    anims, offs = {}, {}
    with open(WORK / "frame_index.csv", newline="", encoding="utf-8", errors="replace") as f:
        r = csv.reader(f)
        next(r)
        for row in r:
            if not matches(row[1], patterns):
                continue
            frame = int(row[0])
            anims.setdefault((row[1], row[2], int(row[3])), []).append((int(row[4]), frame))
            offs[frame] = (int(row[7]), int(row[8]))
    return {k: [fr for _, fr in sorted(v)] for k, v in anims.items()}, offs


def read_pairs(anims):
    """{(a, b) with a < b: True if only seen as "last -> first"} for the animations."""
    pairs = {}
    for seq in anims.values():
        for a, b in zip(seq, seq[1:]):
            if a != b:
                pairs[(min(a, b), max(a, b))] = False
        if len(seq) > 2 and seq[-1] != seq[0]:
            pairs.setdefault((min(seq[-1], seq[0]), max(seq[-1], seq[0])), True)
    return pairs


def load_index(name):
    d = store_dir(name)
    idx = d / "between.idx"
    if not idx.exists() or not (d / "between.dat").exists():
        return {}
    raw = idx.read_bytes()
    if raw[:4] != b"HDBT":
        return {}
    count = struct.unpack_from("<I", raw, 8)[0]
    return {(e[0], e[1]): e for e in np.frombuffer(raw, ENTRY, count, 16).tolist()}


SHARP_STORE = [0]     # 1: made with --sharp (between.idx header, last word; tools\hd\between_quality.py reads it)


def store_is_sharp(name):
    idx = store_dir(name) / "between.idx"
    if not idx.exists():
        return False
    raw = idx.read_bytes()[:16]
    return len(raw) == 16 and raw[:4] == b"HDBT" and struct.unpack_from("<I", raw, 12)[0] == 1


def save_index(name, entries):
    d = store_dir(name)
    arr = np.array([entries[k] for k in sorted(entries)], dtype=ENTRY) if entries else np.zeros(0, ENTRY)
    tmp = d / "between.tmp"
    tmp.write_bytes(b"HDBT" + struct.pack("<III", 1, len(arr), SHARP_STORE[0]) + arr.tobytes())
    for _ in range(100):
        try:
            tmp.replace(d / "between.idx")
            return
        except PermissionError:   # the game is reading it this very moment
            time.sleep(0.2)


def game_running():
    """The player's game runs: a Game.exe with a VISIBLE window. The hidden test instance (tools\\htest.ps1) keeps
    its window hidden and does not count - the generator goes on while the mod is tested in the background.
    (A window title is no sign: the hidden window has one too - 2026-11-15 the generator paused for a test.)"""
    try:
        out = subprocess.run(["tasklist", "/FO", "CSV", "/NH", "/FI", "IMAGENAME eq Game.exe"], capture_output=True,
                             text=True, errors="replace", timeout=20).stdout
    except (OSError, subprocess.SubprocessError):
        return False
    pids = {int(row[1]) for row in csv.reader(out.splitlines()) if len(row) > 1 and row[0].lower() == "game.exe"}
    if not pids:
        return False
    import ctypes
    from ctypes import wintypes
    user32 = ctypes.windll.user32
    found = []
    proc_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    def each(hwnd, _):
        pid = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        if pid.value in pids and user32.IsWindowVisible(hwnd):
            found.append(hwnd)
        return True

    user32.EnumWindows(proc_type(each), 0)
    return bool(found)


_last_game_check = [0.0]


def wait_while_game_runs(release=None, force=False):
    """Pause as long as the game runs: the generator would take the graphics card away from it."""
    if not force and time.time() - _last_game_check[0] < 15:
        return
    _last_game_check[0] = time.time()
    paused = False
    while game_running():
        if not paused:
            log("the game is running - paused, the graphics card is left to the game")
            if release:
                release()
            paused = True
        time.sleep(30)
    if paused:
        log("the game was closed - going on")


def canvas_of(pa, xa, ya, pb, xb, yb):
    """Both frames (uint16) on one canvas of planes; canvas sides are multiples of 16 (x4 -> multiples of 64)."""
    x0, y0 = min(xa, xb) - MARGIN, min(ya, yb) - MARGIN
    w = max(xa + pa.shape[1], xb + pb.shape[1]) + MARGIN - x0
    h = max(ya + pa.shape[0], yb + pb.shape[0]) + MARGIN - y0
    w, h = (w + 15) // 16 * 16, (h + 15) // 16 * 16
    out = []
    for px, x, y in ((pa, xa, ya), (pb, xb, yb)):
        c = np.zeros((h, w, 5), np.float32)
        c[y - y0:y - y0 + px.shape[0], x - x0:x - x0 + px.shape[1]] = it.to_planes(px)
        out.append(c)
    return out[0], out[1], x0, y0


class Maker:
    def __init__(self, magnify, half=False, card_share=0.0):
        import torch
        import torch.nn.functional as F
        import rife_net
        self.torch, self.F, self.rife, self.m, self.half = torch, F, rife_net, magnify, half
        if card_share > 0:
            # Several processes side by side: without a cap each one keeps memory it no longer needs (canvases
            # of many sizes), together they overfill the card and Windows starts moving its memory to the main
            # memory - everything crawls (measured 2026-11-14: 3 processes, 11.5 of 12 GB, two of them stalled).
            torch.cuda.set_per_process_memory_fraction(card_share)
        self.net = rife_net.load(it.WEIGHTS)
        self.bg = torch.tensor(it.BG, device="cuda").view(1, 3, 1, 1)
        # sharp: every pixel of the middle frame from ONE of the two moved frames (the one the network trusts more
        # there) instead of their mix - where the two disagree the mix was soft / doubled (2026-11-15)
        self.sharp = False

    def release(self):
        self.torch.cuda.empty_cache()

    def picture(self, p):
        """Planes -> what the network sees: the sprite over a plain backdrop (like interp_test.over_bg)."""
        torch = self.torch
        alpha, shadow = p[:, 3:4].clamp(0, 1), p[:, 4:5].clamp(0, 1)
        pic = p[:, :3] + self.bg * (1 - 0.45 * shadow) * (1 - alpha)
        return torch.floor(pic.clamp(0, 255) + 0.5) / 255.0

    def run(self, canvases):
        """canvases: (a, b) planes of one size -> [(middle planes at canvas size, colour, shape)]. All on the card:
        4x bicubic magnification, network, moving the planes, quality check, 4x reduction (= area average)."""
        torch, F, m = self.torch, self.F, self.m
        with torch.inference_mode(), torch.autocast("cuda", dtype=torch.float16, enabled=self.half):
            a = torch.from_numpy(np.stack([c[0] for c in canvases])).cuda().permute(0, 3, 1, 2)
            b = torch.from_numpy(np.stack([c[1] for c in canvases])).cuda().permute(0, 3, 1, 2)
            big_a = F.interpolate(a, scale_factor=m, mode="bicubic", align_corners=False).float()
            big_b = F.interpolate(b, scale_factor=m, mode="bicubic", align_corners=False).float()
            ta, tb = self.picture(big_a), self.picture(big_b)
            _, flow, mask = self.net(ta, tb, 0.5)
            flow, mask = flow.float(), mask.float()
            wa, wb = self.rife.warp(big_a, flow[:, :2]), self.rife.warp(big_b, flow[:, 2:4])
            mid = torch.where(mask > 0.5, wa, wb) if self.sharp else wa * mask + wb * (1 - mask)
            # quality check: do the two source frames, each moved to the middle, agree?
            pa, pb = self.rife.warp(ta, flow[:, :2]), self.rife.warp(tb, flow[:, 2:4])
            figure = (mid[:, 3:4] > 0.5).float()
            colour = ((pa - pb).abs().mean(1, keepdim=True) * figure).sum((1, 2, 3)) / figure.sum((1, 2, 3)).clamp(min=1) * 255.0
            sa, sb = (wa[:, 3:4] > 0.5).float(), (wb[:, 3:4] > 0.5).float()
            # silhouettes made of shadow only (characters out of sight, "Blip00"): compare the shadows instead
            only_shadow = ((sa.sum((1, 2, 3)) + sb.sum((1, 2, 3))) < 200).view(-1, 1, 1, 1)
            sa = torch.where(only_shadow, (wa[:, 4:5] > 0.5).float(), sa)
            sb = torch.where(only_shadow, (wb[:, 4:5] > 0.5).float(), sb)
            shape = (sa - sb).abs().sum((1, 2, 3)) / torch.maximum(sa, sb).sum((1, 2, 3)).clamp(min=1)
            small = F.avg_pool2d(mid, m).permute(0, 2, 3, 1).float().cpu().numpy()
            colour, shape = colour.float().cpu().numpy(), shape.float().cpu().numpy()
        return [(small[k], float(colour[k]), float(shape[k])) for k in range(len(canvases))]

    def run_reference(self, canvases):
        """The first version (magnification on the processor with OpenCV) - only for --selftest."""
        torch, m = self.torch, self.m
        h, w = canvases[0][0].shape[:2]
        size = (w * m, h * m)
        pics_a, pics_b, ex_a, ex_b = [], [], [], []
        for a, b in canvases:
            for p, pics, ex in ((a, pics_a, ex_a), (b, pics_b, ex_b)):
                big = cv2.resize(p, size, interpolation=cv2.INTER_CUBIC)
                pics.append(it.over_bg(big))
                ex.append(np.ascontiguousarray(big))
        with torch.inference_mode():
            ta = torch.from_numpy(np.stack(pics_a)).cuda().float().div(255.0).permute(0, 3, 1, 2)
            tb = torch.from_numpy(np.stack(pics_b)).cuda().float().div(255.0).permute(0, 3, 1, 2)
            ea = torch.from_numpy(np.stack(ex_a)).cuda().float().permute(0, 3, 1, 2)
            eb = torch.from_numpy(np.stack(ex_b)).cuda().float().permute(0, 3, 1, 2)
            _, flow, mask = self.net(ta, tb, 0.5)
            wa, wb = self.rife.warp(ea, flow[:, :2]), self.rife.warp(eb, flow[:, 2:4])
            mid = wa * mask + wb * (1 - mask)
            pa, pb = self.rife.warp(ta, flow[:, :2]), self.rife.warp(tb, flow[:, 2:4])
            figure = (mid[:, 3:4] > 0.5).float()
            colour = ((pa - pb).abs().mean(1, keepdim=True) * figure).sum((1, 2, 3)) / figure.sum((1, 2, 3)).clamp(min=1) * 255.0
            sa, sb = (wa[:, 3:4] > 0.5).float(), (wb[:, 3:4] > 0.5).float()
            shape = (sa - sb).abs().sum((1, 2, 3)) / torch.maximum(sa, sb).sum((1, 2, 3)).clamp(min=1)
            mid = mid.permute(0, 2, 3, 1).cpu().numpy()
            colour, shape = colour.cpu().numpy(), shape.cpu().numpy()
        return [(cv2.resize(mid[k], (w, h), interpolation=cv2.INTER_AREA), float(colour[k]), float(shape[k]))
                for k in range(len(canvases))]


def run_safe(maker, canvases):
    """maker.run, splitting the batch when the graphics card runs out of memory (None = could not be done)."""
    torch = maker.torch
    try:
        return maker.run(canvases)
    except torch.cuda.OutOfMemoryError:
        torch.cuda.empty_cache()
        if len(canvases) == 1:
            log(f"  out of graphics card memory for one pair on a {canvases[0][0].shape[1]}x{canvases[0][0].shape[0]} canvas - skipped")
            return [None]
        half = len(canvases) // 2
        return run_safe(maker, canvases[:half]) + run_safe(maker, canvases[half:])


def to_frame(planes, need_figure=True):
    """Planes of the in-between picture -> (uint16 frame cropped to its content, x, y of the crop) or None.
    need_figure=False: a silhouette made of shadow only is a frame too (both source frames are such)."""
    p = it.harden(planes)
    figure, shadow = p[..., 3] > 0.5, p[..., 4] > 0.5
    solid = figure | shadow
    if not solid.any() or (need_figure and not figure.any()):
        return None
    ys, xs = np.nonzero(solid)
    y0, y1, x0, x1 = ys.min(), ys.max() + 1, xs.min(), xs.max() + 1
    px = to_565(np.clip(p[y0:y1, x0:x1, :3], 0, 255))
    px[px == KEY] = KEY | 1
    px[px == MARK] = MARK - 1
    px[shadow[y0:y1, x0:x1]] = MARK
    px[~solid[y0:y1, x0:x1]] = KEY
    return px, int(x0), int(y0)


def thin_kept(a, b, mid):
    """Thin parts of the middle frame / the mean of those of the two frames (1.0 when they have hardly any)."""
    counts = []
    for alpha in (a[..., 3], b[..., 3], it.harden(mid)[..., 3]):
        fig = (alpha > 0.5).astype(np.uint8)
        counts.append(int(np.count_nonzero(fig > cv2.morphologyEx(fig, cv2.MORPH_OPEN, THIN_KERNEL))))
    ref = (counts[0] + counts[1]) / 2
    return counts[2] / ref if ref >= THIN_MIN_PIXELS else 1.0


def accepted(colour, shape, wrap_only=False, thin=1.0):
    if thin < MIN_THIN_KEPT:
        return False
    if wrap_only:
        return colour <= MAX_COLOUR_WRAP and shape <= MAX_SHAPE_WRAP
    return colour <= MAX_COLOUR and shape <= MAX_SHAPE


def loop_closures(anims):
    """Pairs seen only as "last -> first frame" that are real steps of a loop: their two values are no worse
    than 1.25 x the median of the steps INSIDE the same animation (a walk cycle closes with a step like any
    other; a cut to another pose is far beyond the animation's own steps). Uses the measurements in the logs."""
    measured = {}
    for path in sorted(WORK.glob("between_log*.csv")):
        with open(path, newline="", encoding="utf-8") as f:
            for row in csv.DictReader(f):
                measured[(int(row["a"]), int(row["b"]))] = (float(row["colour"]), float(row["shape"]))
    good = set()
    for seq in anims.values():
        if len(seq) < 4 or seq[-1] == seq[0]:
            continue
        wrap = (min(seq[-1], seq[0]), max(seq[-1], seq[0]))
        inner = [measured.get((min(a, b), max(a, b))) for a, b in zip(seq, seq[1:]) if a != b]
        inner = [v for v in inner if v is not None]
        if wrap not in measured or len(inner) < 3:
            continue
        colour, shape = measured[wrap]
        med_c, med_s = float(np.median([v[0] for v in inner])), float(np.median([v[1] for v in inner]))
        if colour <= min(MAX_COLOUR, 1.25 * med_c) and shape <= min(MAX_SHAPE, 1.25 * med_s + 0.02):
            good.add(wrap)
    return good


def merge(target, parts):
    """Stores `parts` into `target` (one data file, one sorted index); a usable entry wins over a rejected one."""
    tdir = store_dir(target)
    tdir.mkdir(parents=True, exist_ok=True)
    sources = [(target, load_index(target))] + [(p, load_index(p)) for p in parts]
    merged = {}
    new_dat = tdir / "between.dat.new"
    with open(new_dat, "wb") as out:
        out.write(b"HDBD" + struct.pack("<III", 1, 0, 0))
        for name, ent in sources:
            if not ent:
                continue
            with open(store_dir(name) / "between.dat", "rb") as src:
                for key, e in sorted(ent.items(), key=lambda kv: kv[1][2]):   # in the order of the data file
                    old = merged.get(key)
                    if old is not None and (old[3] or not e[3]):
                        continue
                    if e[3]:
                        src.seek(e[2])
                        data = src.read(e[3])
                        if len(data) != e[3]:
                            log(f"  {name}: pair {key} cut short in the data file - left out")
                            continue
                        e = (e[0], e[1], out.tell(), e[3]) + tuple(e[4:])
                        out.write(data)
                    merged[key] = e
    for _ in range(100):
        try:
            new_dat.replace(tdir / "between.dat")
            break
        except PermissionError:
            time.sleep(0.5)
    save_index(target, merged)
    usable = sum(1 for e in merged.values() if e[3])
    log(f"merged into {target}: {len(merged)} pairs, {usable} usable, data file {(tdir / 'between.dat').stat().st_size / 2**20:.0f} MB")
    return merged


def write_sheet(path, items, zoom=2, width=2400):
    """items: (a planes, middle planes, b planes, label, ok) - sorted by the caller."""
    from PIL import Image, ImageDraw
    cells = []
    for a, mid, b, label, ok in items:
        parts = [it.over_bg(a), it.over_bg(it.harden(mid)), it.over_bg(b)]
        h, w = parts[0].shape[:2]
        cell = Image.new("RGB", (3 * w * zoom + 8, h * zoom + 14), (20, 20, 20))
        for k, part in enumerate(parts):
            cell.paste(Image.fromarray(part, "RGB").resize((w * zoom, h * zoom), Image.NEAREST), (k * (w * zoom + 4), 14))
        ImageDraw.Draw(cell).text((2, 1), label, fill=(120, 255, 120) if ok else (255, 110, 110))
        cells.append(cell)
    x = y = 4
    row_h = 0
    pos = []
    for c in cells:
        if x + c.width + 4 > width and x > 4:
            x, y, row_h = 4, y + row_h + 4, 0
        pos.append((x, y))
        x += c.width + 4
        row_h = max(row_h, c.height)
    sheet = Image.new("RGB", (width, y + row_h + 4), (48, 48, 48))
    for c, (cx, cy) in zip(cells, pos):
        sheet.paste(c, (cx, cy))
    path.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(path)
    log(f"sheet: {path} {sheet.size}")


def prepare(store, offs, keys):
    """[(key, a planes, b planes, x0, y0)] for the pairs whose frames exist and whose canvas is not too large."""
    out = []
    for key in keys:
        pa, pb = store.frame(key[0]), store.frame(key[1])
        if pa is None or pb is None:
            continue
        (ax, ay), (bx, by) = offs[key[0]], offs[key[1]]
        a, b, x0, y0 = canvas_of(pa, 2 * ax, 2 * ay, pb, 2 * bx, 2 * by)
        if a.shape[0] * a.shape[1] <= MAX_CANVAS:
            out.append((key, a, b, x0, y0))
    return out


def selftest(args, pairs, offs):
    """Speed of the old and the new way, single and half precision, and how much their frames differ."""
    import torch
    store = it.HdStore("_common")
    rng = np.random.default_rng(2)
    keys = sorted(pairs)
    keys = [keys[i] for i in rng.choice(len(keys), min(len(keys), args.selftest), replace=False)]
    items = prepare(store, offs, keys)
    groups = {}
    for item in items:
        groups.setdefault(item[1].shape[:2], []).append(item)
    results = {}
    for name, half, how, batch in (("reference fp32", False, "ref", 2_500_000), ("card fp32", False, "gpu", 2_500_000),
                                   ("card fp16", True, "gpu", 2_500_000), ("card fp16, big batches", True, "gpu", 6_000_000)):
        maker = Maker(args.magnify, half)
        run = maker.run_reference if how == "ref" else maker.run
        warm = items[:2]
        run([(a, b) for _, a, b, _, _ in warm[:1]])
        torch.cuda.synchronize()
        torch.cuda.reset_peak_memory_stats()
        t0 = time.perf_counter()
        out = {}
        for shape, members in groups.items():
            per = max(1, batch // (shape[0] * shape[1] * args.magnify ** 2))
            for i in range(0, len(members), per):
                chunk = members[i:i + per]
                for (key, a, b, _, _), res in zip(chunk, run([(a, b) for _, a, b, _, _ in chunk])):
                    out[key] = res
        torch.cuda.synchronize()
        dt = time.perf_counter() - t0
        results[name] = out
        log(f"{name:24s}: {len(out) / dt:5.1f} pairs/s, peak card memory {torch.cuda.max_memory_allocated() / 2**30:.2f} GB")
        del maker
        torch.cuda.empty_cache()
    ref = results["reference fp32"]
    for name, out in results.items():
        if name == "reference fp32":
            continue
        diffs, dc, ds, agree = [], [], [], 0
        for key, (mid, colour, shape) in out.items():
            rmid, rcol, rshape = ref[key]
            x, y = it.over_bg(it.harden(mid)).astype(np.float32), it.over_bg(it.harden(rmid)).astype(np.float32)
            fig = (it.harden(rmid)[..., 3] > 0.5) | (it.harden(mid)[..., 3] > 0.5)
            if fig.any():
                diffs.append(float(np.abs(x - y)[fig].mean()))
            dc.append(abs(colour - rcol))
            ds.append(abs(shape - rshape))
            wrap = pairs[key]
            agree += accepted(colour, shape, wrap) == accepted(rcol, rshape, wrap)
        log(f"{name:24s} vs reference: frame difference on the figure {np.mean(diffs):.2f} of 255 (max {np.max(diffs):.1f}), "
            f"colour value +-{np.mean(dc):.2f}, shape value +-{np.mean(ds):.3f}, same decision {agree}/{len(out)}")


def run_jobs(args):
    """Several processes side by side, each with its own part of the pairs and its own store; then merge and
    the second pass for loops. Parts already made are kept when the whole is started again.
    With --redo everything is made anew; the old store stays in use until the new one is complete."""
    script = str(Path(__file__).resolve())
    base = [sys.executable, "-I", script, "--files", *args.files, "--magnify", str(args.magnify),
            "--batch-pixels", str(args.batch_pixels)]
    if args.half:
        base.append("--half")
    if args.sharp:
        base.append("--sharp")
    parts = [f"{MAIN}_part{i}" for i in range(args.jobs)]
    old_log = log_path(MAIN)
    if args.redo and old_log.exists() and not any(store_dir(p).exists() for p in parts):
        # the measurements of the old store must not decide the loops of the new one
        old_log.replace(WORK / f"between_log_old_{time.strftime('%Y%m%d_%H%M%S')}.txt")
    procs = []
    import os
    env = dict(os.environ)
    # give cached blocks back early and do not split big ones (less waste with canvases of many sizes)
    env["PYTORCH_CUDA_ALLOC_CONF"] = "garbage_collection_threshold:0.6,max_split_size_mb:64"
    for i, name in enumerate(parts):
        out = open(WORK / f"make_inbetweens_{name}.log", "a", encoding="utf-8")
        cmd = base + ["--store", name, "--shard", f"{i}/{args.jobs}", "--threads", str(max(1, 12 // args.jobs)),
                      "--card-share", f"{args.card_share or 0.7 / args.jobs:.3f}"]
        if not args.redo:
            cmd += ["--skip-store", MAIN]
        procs.append((subprocess.Popen(cmd, stdout=out, stderr=subprocess.STDOUT, env=env), out, name))
        log(f"started {name} (log: hd\\work\\make_inbetweens_{name}.log)")
        time.sleep(15)   # the model loads one after the other
    failed = []
    for p, out, name in procs:
        p.wait()
        out.close()
        if p.returncode:
            failed.append(name)
    if failed:
        log(f"parts {failed} ended with an error - nothing merged; start the same command again to continue")
        sys.exit(1)
    wait_while_game_runs(force=True)
    if args.redo:
        fresh = f"{MAIN}_new"
        shutil.rmtree(store_dir(fresh), ignore_errors=True)
        merge(fresh, parts)
        old = store_dir(f"{MAIN}_old")
        shutil.rmtree(old, ignore_errors=True)
        if store_dir(MAIN).exists():
            store_dir(MAIN).rename(old)
        store_dir(fresh).rename(store_dir(MAIN))
        shutil.rmtree(old, ignore_errors=True)
        log(f"the new store replaced the old one in hd\\sprites\\{MAIN}")
    else:
        merge(MAIN, parts)
    for name in parts:
        shutil.rmtree(store_dir(name), ignore_errors=True)
    log("second pass: last -> first pairs that are steps of a loop")
    subprocess.run(base + ["--store", MAIN, "--loops"], check=False)
    entries = load_index(MAIN)
    log(f"all done: {len(entries)} pairs, {sum(1 for e in entries.values() if e[3])} usable, "
        f"data file {(store_dir(MAIN) / 'between.dat').stat().st_size / 2**20:.0f} MB")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--files", nargs="*", default=["RobinTown.rhs"],
                    help='endings of .rhs file names, or folders ending with "/" (see hd\\work\\frame_index.csv)')
    ap.add_argument("--magnify", type=int, default=4)
    ap.add_argument("--half", action="store_true", help="half precision for the network (checked with --selftest)")
    ap.add_argument("--sharp", action="store_true",
                    help="each pixel from one of the two moved frames instead of their mix (no soft / doubled frames; "
                         "tools\\hd\\inbetween_sharp_test.py); the store is marked as such")
    ap.add_argument("--jobs", type=int, default=1, help="processes side by side (each needs its share of card memory)")
    ap.add_argument("--card-share", type=float, default=0.0,
                    help="cap on the graphics card memory of one process, share of the card (0 = none; --jobs N: 0.7/N)")
    ap.add_argument("--store", default=MAIN, help="folder under hd\\sprites")
    ap.add_argument("--shard", default="", help="I/N: only this part of the pairs")
    ap.add_argument("--skip-store", nargs="*", default=[], help="pairs already in these stores are not made")
    ap.add_argument("--threads", type=int, default=0, help="processor threads for OpenCV / torch (0 = all)")
    ap.add_argument("--survey", type=int, default=0, help="only this many random pairs, nothing stored; use with --sheet")
    ap.add_argument("--selftest", type=int, default=0, help="speed and precision check on this many random pairs")
    ap.add_argument("--sheet", default="")
    ap.add_argument("--sheet-range", type=float, nargs=2, default=[0.0, 99.0],
                    help="only pairs whose quality ratio (1 = at the limit) lies in this range go on the sheet")
    ap.add_argument("--sheet-thin", type=float, default=0.0,
                    help="instead: the pairs whose middle frame keeps at most this share of the thin parts")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--redo", action="store_true")
    ap.add_argument("--stats", action="store_true")
    ap.add_argument("--loops", action="store_true", help="second pass: make the last -> first pairs that are real steps "
                                                         "of a loop (decided from the measurements of the first pass)")
    ap.add_argument("--retry-shadow", action="store_true",
                    help="make again the rejected pairs whose frames are shadow-only silhouettes (before 2026-11-14 15:00 "
                         "these were always rejected)")
    ap.add_argument("--batch-pixels", type=int, default=2_500_000, help="magnified pixels per batch")
    args = ap.parse_args()
    SHARP_STORE[0] = 1 if args.sharp else 0

    if args.stats:
        entries = load_index(args.store)
        ok = sum(1 for e in entries.values() if e[3])
        dat = store_dir(args.store) / "between.dat"
        size = dat.stat().st_size if dat.exists() else 0
        print(f"{len(entries)} pairs in {args.store}: {ok} usable, {len(entries) - ok} rejected; data file {size / 2**20:.1f} MB")
        return
    if args.jobs > 1:
        run_jobs(args)
        return
    if args.threads:
        cv2.setNumThreads(args.threads)
        import torch
        torch.set_num_threads(args.threads)

    anims, offs = read_animations(args.files)
    pairs = read_pairs(anims)
    if args.selftest:
        selftest(args, pairs, offs)
        return
    store = it.HdStore("_common")
    entries = {} if (args.redo or args.survey) else load_index(args.store)
    skip = set()
    for name in args.skip_store:
        skip |= set(load_index(name))
    todo = sorted(k for k in pairs if k not in entries and k not in skip)
    if args.shard:
        part, parts = (int(v) for v in args.shard.split("/"))
        todo = [k for k in todo if (k[0] * 31 + k[1]) % parts == part]
    forced = set()
    if args.retry_shadow:
        # pairs rejected before shadow-only silhouettes were understood: both frames without any figure pixel
        def shadow_only(frame):
            px = store.frame(frame)
            return px is not None and not ((px != KEY) & (px != MARK)).any() and (px == MARK).any()
        cand = [k for k in pairs if k in entries and entries[k][3] == 0]
        todo = sorted(k for k in cand if shadow_only(k[0]) and shadow_only(k[1]))
        log(f"{len(todo)} rejected pairs of shadow-only silhouettes - made again")
    if args.loops:
        forced = {k for k in loop_closures(anims) if k in entries and entries[k][3] == 0}
        todo = sorted(forced)
        log(f"{len(todo)} last -> first pairs are steps of a loop and were rejected by the stricter limits - made now")
    log(f"{args.files} {args.shard}: {len(anims)} animations, {len(pairs)} pairs of neighbouring frames "
        f"({sum(pairs.values())} only as last -> first); {len(todo)} to do")
    if args.survey:
        rng = np.random.default_rng(1)
        wraps = [k for k in todo if pairs[k]]
        inner = [k for k in todo if not pairs[k]]
        pick = [inner[i] for i in rng.choice(len(inner), min(len(inner), args.survey * 3 // 4), replace=False)]
        pick += [wraps[i] for i in rng.choice(len(wraps), min(len(wraps), args.survey // 4), replace=False)]
        todo = sorted(pick)
    if args.limit:
        todo = todo[:args.limit]
    if not todo:
        return

    maker = Maker(args.magnify, args.half, args.card_share)
    maker.sharp = args.sharp
    sdir = store_dir(args.store)
    sdir.mkdir(parents=True, exist_ok=True)
    dat_path = sdir / "between.dat"
    if not args.survey and (args.redo or not dat_path.exists()):
        dat_path.write_bytes(b"HDBD" + struct.pack("<III", 1, 0, 0))
    lpath = log_path(args.store)
    new_log = not lpath.exists()
    logf = None if args.survey else open(lpath, "a", newline="", encoding="utf-8")
    logw = csv.writer(logf) if logf else None
    if logw and new_log:
        logw.writerow(["a", "b", "last_to_first_only", "colour", "shape", "usable", "thin_kept"])

    groups = {}
    t0 = time.perf_counter()
    last_save = t0
    stats = {"done": 0, "ok": 0, "bytes": 0}
    survey_items = []
    dat = None if args.survey else open(dat_path, "r+b")
    if dat:
        dat.seek(0, 2)

    def flush(group_key):
        nonlocal last_save
        wait_while_game_runs(maker.release)
        members = groups.pop(group_key)
        results = run_safe(maker, [(a, b) for _, a, b, _, _ in members])
        for (key, a, b, x0, y0), res in zip(members, results):
            if res is None:
                continue
            mid, colour, shape = res
            thin = thin_kept(a, b, mid)
            ok = thin >= MIN_THIN_KEPT and (key in forced or accepted(colour, shape, pairs[key]))
            frame = to_frame(mid, bool((a[..., 3] > 0.5).any() or (b[..., 3] > 0.5).any())) if ok else None
            ok = ok and frame is not None
            stats["done"] += 1
            stats["ok"] += ok
            if args.survey:
                survey_items.append((colour, shape, thin, a, mid, b, f"{key[0]}-{key[1]} kolor {colour:.1f} ksztalt {shape:.2f} "
                                     f"cienkie {thin:.2f}" + (" [ostatnia-pierwsza]" if pairs[key] else ""), ok))
                continue
            e = (key[0], key[1], 0, 0, 0, 0, 0, 0, 0, 0)
            if ok:
                px, mx, my = frame
                raw = px.astype("<u2").tobytes()
                spans = encode_spans(px)
                data = spans if len(spans) < len(raw) else raw
                (ax, ay), (bx, by) = offs[key[0]], offs[key[1]]
                e = (key[0], key[1], dat.tell(), len(data), px.shape[1], px.shape[0],
                     x0 + mx - 2 * ax, y0 + my - 2 * ay, x0 + mx - 2 * bx, y0 + my - 2 * by)
                dat.write(data)
                stats["bytes"] += len(data)
            entries[key] = e
            logw.writerow([key[0], key[1], int(pairs[key]), f"{colour:.2f}", f"{shape:.3f}", int(ok), f"{thin:.2f}"])
        now = time.perf_counter()
        if dat and now - last_save > 60:
            last_save = now
            dat.flush()
            logf.flush()
            save_index(args.store, entries)
            rate = stats["done"] / (now - t0)
            log(f"  {stats['done']}/{len(todo)} pairs, {stats['ok']} usable, {rate:.1f} pairs/s, {stats['bytes'] / 2**20:.0f} MB, "
                f"card {maker.torch.cuda.max_memory_allocated() / 2**30:.1f} GB, left ~{(len(todo) - stats['done']) / max(rate, 0.01) / 60:.0f} min")

    chunk = 400
    for i in range(0, len(todo), chunk):
        for item in prepare(store, offs, todo[i:i + chunk]):
            gk = item[1].shape[:2]
            groups.setdefault(gk, []).append(item)
            if len(groups[gk]) * gk[0] * gk[1] * args.magnify ** 2 >= args.batch_pixels:
                flush(gk)
    for gk in list(groups):
        flush(gk)

    if dat:
        dat.flush()
        dat.close()
        logf.close()
        save_index(args.store, entries)
    el = time.perf_counter() - t0
    log(f"done: {stats['done']} pairs in {el / 60:.1f} min ({stats['done'] / max(el, 1e-6):.1f}/s), {stats['ok']} usable"
        + ("" if args.survey else f", data file {dat_path.stat().st_size / 2**20:.1f} MB"))
    if args.survey and args.sheet:
        def ratio(s):   # 1.0 = exactly at the limit of the quality check
            return max(s[0] / MAX_COLOUR, s[1] / MAX_SHAPE)
        lo, hi = args.sheet_range
        if args.sheet_thin:
            # the pairs whose middle frame keeps the least of the thin parts, worst first
            shown = sorted((s for s in survey_items if s[2] <= args.sheet_thin), key=lambda s: s[2])
            log(f"sheet: {len(shown)} of {len(survey_items)} pairs keep at most {args.sheet_thin} of the thin parts")
        else:
            shown = sorted((s for s in survey_items if lo <= ratio(s) <= hi), key=ratio)
            log(f"sheet: {len(shown)} of {len(survey_items)} pairs (quality ratio {lo}..{hi}; 1 = limit)")
        write_sheet(ROOT / args.sheet, [s[3:] for s in shown])
        cs = np.array([s[0] for s in survey_items])
        ss = np.array([s[1] for s in survey_items])
        ts = np.array([s[2] for s in survey_items])
        print("colour percentiles 10/50/90/99:", np.percentile(cs, [10, 50, 90, 99]).round(1))
        print("shape  percentiles 10/50/90/99:", np.percentile(ss, [10, 50, 90, 99]).round(3))
        print("thin kept percentiles 1/5/10/50:", np.percentile(ts, [1, 5, 10, 50]).round(2),
              f"- below {MIN_THIN_KEPT}: {int((ts < MIN_THIN_KEPT).sum())}, of them passing the other checks: "
              f"{sum(1 for s in survey_items if s[2] < MIN_THIN_KEPT and accepted(s[0], s[1]))}")


if __name__ == "__main__":
    main()
