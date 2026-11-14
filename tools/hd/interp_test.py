"""Experiment: can in-between frames of a sprite animation be generated?

    python -I tools\\hd\\interp_test.py [--anim 112] [--file RobinTown.rhs] [--profile "Robin des bois"]
                                        [--no-rife] [--gif]

Robin's slow walk (animation 112 of Characters\\RobinTown.rhs) has 20 different frames; the faster
gaits use only some of them. That gives a test with a known answer: take every SECOND frame,
generate the frame in between, and compare it with the real one the game has.

Methods compared:
  hold      - show the previous frame (what the game does between two animation frames)
  blend     - 50/50 cross-fade of the two neighbours
  flow      - classical motion compensation: dense optical flow (OpenCV DIS) both ways, each
              neighbour moved half-way along the motion, the two mixed. No model needed.
  rife xN   - the RIFE v4.26 neural network (tools\\hd\\rife_net.py; weights in
              hd\\models_vfi\\rife_v4.26_dl\\flownet.pkl), fed with the frames magnified N times:
              the network was trained on video, where things are hundreds of pixels large, and a
              sprite is about 56 x 114 even in its HD version.
The sprite's colour, opacity and shadow planes are moved along the network's motion field, so the
result is a real sprite frame again (not a picture with a background baked in).
Works on the upscaled frames of hd\\sprites\\_common (falls back to the bank frames doubled).

Output: hd\\compare\\klatki_posrednie.png - rows: previous | methods ... | REAL middle | next
        hd\\compare\\chod_klatki_posrednie.gif (--gif) - the walk at half the frame rate, the same
        with generated in-between frames, and the game's real full-rate walk, side by side
        and the mean error of each method against the real middle frame (0 = identical), over the
        whole picture and over the figure only.
Run with "python -I": the script reads a downloaded weights file.
"""
import argparse
import csv
import struct
import sys
from pathlib import Path

import cv2
import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parent))
from extract_sprites import Bank, KEY, MARK  # noqa: E402
from upscale_sprites import ENTRY, decode_spans, rgb_from_565  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
BG = np.array([96, 120, 78], np.float32)
WEIGHTS = ROOT / "hd" / "models_vfi" / "rife_v4.26_dl" / "flownet.pkl"


def load_animation(file_suffix: str, profile: str, anim: int):
    rows = []
    with open(ROOT / "hd" / "work" / "frame_index.csv", newline="", encoding="utf-8", errors="replace") as f:
        r = csv.reader(f)
        next(r)
        for row in r:
            if row[1].endswith(file_suffix) and row[2] == profile and int(row[3]) == anim:
                rows.append((int(row[4]), int(row[0]), int(row[7]), int(row[8])))
    rows.sort()
    return [(fr, x, y) for _, fr, x, y in rows]


class HdStore:
    def __init__(self, name: str):
        d = ROOT / "hd" / "sprites" / name
        raw = (d / "sprites.idx").read_bytes()
        self.entries = np.frombuffer(raw, ENTRY, struct.unpack_from("<I", raw, 8)[0], 16)
        self.dat = open(d / "sprites.dat", "rb")

    def frame(self, i: int):
        e = self.entries[i]
        if e["size"] == 0:
            return None
        self.dat.seek(int(e["off"]))
        data = self.dat.read(int(e["size"]))
        w, h = int(e["w"]), int(e["h"])
        if len(data) == w * h * 2:
            return np.frombuffer(data, "<u2").reshape(h, w).copy()
        return decode_spans(data, w, h)


def to_planes(px: np.ndarray) -> np.ndarray:
    """565 frame -> float (h, w, 5): premultiplied R, G, B (0..255), opacity (0..1), shadow (0..1)."""
    rgb = rgb_from_565(px)
    opaque = ((px != KEY) & (px != MARK)).astype(np.float32)
    shadow = (px == MARK).astype(np.float32)
    return np.dstack([rgb * opaque[..., None], opaque, shadow])


def over_bg(p: np.ndarray) -> np.ndarray:
    """The planes drawn over the backdrop the way the game would: shadow darkens, figure covers."""
    a = np.clip(p[..., 3:4], 0, 1)
    ground = BG * (1 - 0.45 * np.clip(p[..., 4:5], 0, 1))
    return np.clip(p[..., :3] + ground * (1 - a) + 0.5, 0, 255).astype(np.uint8)


def flow_middle(a: np.ndarray, b: np.ndarray) -> np.ndarray:
    ga = cv2.cvtColor(over_bg(a), cv2.COLOR_RGB2GRAY)
    gb = cv2.cvtColor(over_bg(b), cv2.COLOR_RGB2GRAY)
    dis = cv2.DISOpticalFlow_create(cv2.DISOPTICAL_FLOW_PRESET_MEDIUM)
    dis.setFinestScale(0)
    fab = dis.calc(ga, gb, None)
    fba = dis.calc(gb, ga, None)
    h, w = ga.shape
    gx, gy = np.meshgrid(np.arange(w, dtype=np.float32), np.arange(h, dtype=np.float32))
    # a pixel of the middle frame came from A half a step back along A->B, and from B half a step back along B->A
    wa = cv2.remap(a, gx - 0.5 * fab[..., 0], gy - 0.5 * fab[..., 1], cv2.INTER_LINEAR, borderMode=cv2.BORDER_CONSTANT, borderValue=0)
    wb = cv2.remap(b, gx - 0.5 * fba[..., 0], gy - 0.5 * fba[..., 1], cv2.INTER_LINEAR, borderMode=cv2.BORDER_CONSTANT, borderValue=0)
    return 0.5 * wa + 0.5 * wb


class Rife:
    def __init__(self):
        import torch
        import rife_net
        self.torch, self.mod = torch, rife_net
        self.net = rife_net.load(WEIGHTS)

    def middle(self, a: np.ndarray, b: np.ndarray, magnify: int, t: float = 0.5) -> np.ndarray:
        torch = self.torch
        h, w = a.shape[:2]
        size = (w * magnify, h * magnify)

        def prep(p):
            big = cv2.resize(p, size, interpolation=cv2.INTER_CUBIC) if magnify != 1 else p
            pic = torch.from_numpy(over_bg(big)).cuda().float().div(255.0).permute(2, 0, 1)
            planes = torch.from_numpy(np.ascontiguousarray(big)).cuda().float().permute(2, 0, 1)
            return pic, planes

        pa, ea = prep(a)
        pb, eb = prep(b)
        _, extra, _, _ = self.mod.interpolate(self.net, pa, pb, t, ea, eb)
        out = extra.permute(1, 2, 0).cpu().numpy()
        if magnify != 1:
            out = cv2.resize(out, (w, h), interpolation=cv2.INTER_AREA)
        return out


def harden(p: np.ndarray) -> np.ndarray:
    """Back to what a sprite frame can hold: every pixel either figure, shadow or nothing."""
    a, s = p[..., 3], p[..., 4]
    figure = a >= 0.5
    shadow = (~figure) & (s >= 0.5)
    out = np.zeros_like(p)
    out[..., :3] = np.where(figure[..., None], p[..., :3] / np.maximum(a, 0.5)[..., None], 0)
    out[..., 3] = figure
    out[..., 4] = shadow
    return out


def errors(x: np.ndarray, ref: np.ndarray, figure: np.ndarray):
    d = np.abs(over_bg(x).astype(np.float32) - over_bg(ref).astype(np.float32))
    return float(d.mean()), float(d[figure].mean())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--file", default="RobinTown.rhs")
    ap.add_argument("--profile", default="Robin des bois")
    ap.add_argument("--anim", type=int, default=112)
    ap.add_argument("--zoom", type=int, default=3)
    ap.add_argument("--no-rife", action="store_true")
    ap.add_argument("--magnify", type=int, nargs="*", default=[1, 2, 4])
    ap.add_argument("--gif", action="store_true")
    args = ap.parse_args()

    seq = load_animation(args.file, args.profile, args.anim)
    uniq, seen = [], set()
    for fr, x, y in seq:  # the first occurrence of every frame, in playing order
        if fr not in seen:
            seen.add(fr)
            uniq.append((fr, x, y))
    print(f"animation {args.anim}: {len(seq)} steps, {len(uniq)} different frames: {uniq[0][0]}..{uniq[-1][0]}")

    bank = Bank()
    try:
        store = HdStore("_common")
    except Exception:
        store = None
    frames = []
    for fr, x, y in uniq:
        px = store.frame(fr) if store is not None else None
        if px is None:
            px = np.repeat(np.repeat(bank.decode16(fr), 2, 0), 2, 1)
        frames.append((x * 2, y * 2, to_planes(px)))
    # common canvas: the per-frame offsets of the .rhs place every frame relative to the same origin
    x0, y0 = min(f[0] for f in frames), min(f[1] for f in frames)
    cw = max(f[0] - x0 + f[2].shape[1] for f in frames) + 16
    ch = max(f[1] - y0 + f[2].shape[0] for f in frames) + 16
    canvas = []
    for x, y, planes in frames:
        c = np.zeros((ch, cw, 5), np.float32)
        c[y - y0 + 8:y - y0 + 8 + planes.shape[0], x - x0 + 8:x - x0 + 8 + planes.shape[1]] = planes
        canvas.append(c)
    print(f"canvas {cw}x{ch} (HD pixels)")

    rife = None if args.no_rife else Rife()
    methods = ["hold", "blend", "flow"] + ([f"rife x{m}" for m in args.magnify] if rife else [])
    errs = {m: [] for m in methods}
    rows = []
    for i in range(0, len(canvas) - 2, 2):
        a, real, b = canvas[i], canvas[i + 1], canvas[i + 2]
        figure = cv2.dilate(((a[..., 3] + a[..., 4] + real[..., 3] + real[..., 4] + b[..., 3] + b[..., 4]) > 0).astype(np.uint8),
                            np.ones((5, 5), np.uint8)).astype(bool)
        made = {"hold": a, "blend": 0.5 * a + 0.5 * b, "flow": flow_middle(a, b)}
        if rife:
            for m in args.magnify:
                made[f"rife x{m}"] = harden(rife.middle(a, b, m))
        for name in methods:
            errs[name].append(errors(made[name], real, figure))
        rows.append([a] + [made[m] for m in methods] + [real, b])
    print("mean error against the real middle frame, of 255 (whole picture | figure only):")
    for name in methods:
        whole = np.mean([e[0] for e in errs[name]])
        fig = np.mean([e[1] for e in errs[name]])
        print(f"  {name:8s} {whole:5.2f} | {fig:5.2f}    per step (figure): {' '.join(f'{e[1]:.1f}' for e in errs[name])}")

    labels = ["poprzednia"] + [{"hold": "hold (jak w grze)", "blend": "blend 50/50", "flow": "flow (OpenCV)"}.get(m, m.upper()) for m in methods] + ["PRAWDZIWA srodkowa", "nastepna"]
    z = args.zoom
    pad, bar = 4, 16
    sheet = Image.new("RGB", (len(labels) * (cw * z + pad) + pad, bar + len(rows) * (ch * z + pad) + pad), (24, 24, 24))
    draw = ImageDraw.Draw(sheet)
    for c, label in enumerate(labels):
        draw.text((pad + c * (cw * z + pad) + 2, 2), label, fill=(235, 235, 235))
    for r, row in enumerate(rows):
        for c, img in enumerate(row):
            im = Image.fromarray(over_bg(img), "RGB").resize((cw * z, ch * z), Image.NEAREST)
            sheet.paste(im, (pad + c * (cw * z + pad), bar + r * (ch * z + pad)))
    out = ROOT / "hd" / "compare" / "klatki_posrednie.png"
    sheet.save(out)
    print(f"sheet: {out} {sheet.size}")

    if args.gif and rife:
        # The walk three ways, in step: every second frame held twice | the same with a generated
        # frame in between | the game's own full-rate frames.
        best = min((m for m in args.magnify), key=lambda m: np.mean([e[1] for e in errs[f"rife x{m}"]]))
        n = len(canvas) - (len(canvas) % 2)
        gz = 4
        gif_frames = []
        for k in range(n):
            base = canvas[k - (k % 2)]
            if k % 2 == 0:
                mid = canvas[k]
            else:
                mid = harden(rife.middle(canvas[k - 1], canvas[(k + 1) % n], best))
            panels = [base, mid, canvas[k]]
            strip = Image.new("RGB", (3 * (cw * gz + 6) + 6, ch * gz + 22), (24, 24, 24))
            d = ImageDraw.Draw(strip)
            for c, (label, img) in enumerate(zip(("co druga klatka", f"+ klatki z RIFE (x{best})", "prawdziwe klatki gry"), panels)):
                x = 6 + c * (cw * gz + 6)
                d.text((x + 2, 4), label, fill=(235, 235, 235))
                strip.paste(Image.fromarray(over_bg(img), "RGB").resize((cw * gz, ch * gz), Image.NEAREST), (x, 20))
            gif_frames.append(strip)
        gif = ROOT / "hd" / "compare" / "chod_klatki_posrednie.gif"
        gif_frames[0].save(gif, save_all=True, append_images=gif_frames[1:], duration=70, loop=0)
        print(f"animation: {gif} ({len(gif_frames)} frames, RIFE fed x{best})")


if __name__ == "__main__":
    main()
