"""In-between frames: the "every second frame" test of interp_test.py over several animations.

    python -I tools\\hd\\interp_survey.py [--file RobinTown.rhs] [--profile "Robin des bois"]
                                          [--anims 112 224 320 432 672 752] [--sheet]

For every animation: drop every second frame, regenerate it (hold / blend / classical flow /
RIFE at several input magnifications) and compare with the real frame - error of 255 over the
figure only. Note that this is HARDER than the real use: in the game an in-between frame would
sit between two consecutive frames (half the motion tested here), except for animation 112,
whose frames are twice as dense as the gaits that are actually played.
Also prints the time RIFE needs per pair, and the "noise floor": how much two frames of the idle
animation (nearly the same pose) differ - no method can get below that.
--sheet writes hd\\compare\\klatki_posrednie_<anim>.png for every animation.
"""
import argparse
import sys
import time
from pathlib import Path

import cv2
import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parent))
import interp_test as it  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]


def build_canvas(store, bank, seq):
    uniq, seen = [], set()
    for fr, x, y in seq:
        if fr not in seen:
            seen.add(fr)
            uniq.append((fr, x, y))
    frames = []
    for fr, x, y in uniq:
        px = store.frame(fr) if store is not None else None
        if px is None:
            px = np.repeat(np.repeat(bank.decode16(fr), 2, 0), 2, 1)
        frames.append((x * 2, y * 2, it.to_planes(px)))
    x0, y0 = min(f[0] for f in frames), min(f[1] for f in frames)
    cw = max(f[0] - x0 + f[2].shape[1] for f in frames) + 16
    ch = max(f[1] - y0 + f[2].shape[0] for f in frames) + 16
    out = []
    for x, y, planes in frames:
        c = np.zeros((ch, cw, 5), np.float32)
        c[y - y0 + 8:y - y0 + 8 + planes.shape[0], x - x0 + 8:x - x0 + 8 + planes.shape[1]] = planes
        out.append(c)
    return out, cw, ch


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--file", default="RobinTown.rhs")
    ap.add_argument("--profile", default="Robin des bois")
    ap.add_argument("--anims", type=int, nargs="*", default=[112, 224, 320, 432, 672, 752])
    ap.add_argument("--magnify", type=int, nargs="*", default=[2, 4, 8])
    ap.add_argument("--sheet", action="store_true")
    args = ap.parse_args()

    bank = it.Bank()
    store = it.HdStore("_common")
    rife = it.Rife()
    methods = ["hold", "blend", "flow"] + [f"rife x{m}" for m in args.magnify]
    print(f"{'anim':>5s} {'frames':>6s} {'canvas':>9s}  " + "  ".join(f"{m:>8s}" for m in methods) + "   move/px")
    timing = {m: [] for m in args.magnify}
    for anim in args.anims:
        seq = it.load_animation(args.file, args.profile, anim)
        if len(seq) < 3:
            continue
        canvas, cw, ch = build_canvas(store, bank, seq)
        errs = {m: [] for m in methods}
        rows = []
        moves = []
        for i in range(0, len(canvas) - 2, 2):
            a, real, b = canvas[i], canvas[i + 1], canvas[i + 2]
            figure = cv2.dilate(((a[..., 3] + a[..., 4] + real[..., 3] + real[..., 4] + b[..., 3] + b[..., 4]) > 0).astype(np.uint8),
                                np.ones((5, 5), np.uint8)).astype(bool)
            made = {"hold": a, "blend": 0.5 * a + 0.5 * b, "flow": it.flow_middle(a, b)}
            for m in args.magnify:
                rife.torch.cuda.synchronize()
                t0 = time.perf_counter()
                made[f"rife x{m}"] = it.harden(rife.middle(a, b, m))
                rife.torch.cuda.synchronize()
                timing[m].append(time.perf_counter() - t0)
            for name in methods:
                errs[name].append(it.errors(made[name], real, figure)[1])
            # how far the figure moves between the two given frames (centroid of the opaque part), in HD pixels
            ya, xa = np.nonzero(a[..., 3] > 0.5)
            yb, xb = np.nonzero(b[..., 3] > 0.5)
            if len(xa) and len(xb):
                moves.append(float(np.hypot(xa.mean() - xb.mean(), ya.mean() - yb.mean())))
            rows.append([a] + [made[m] for m in methods] + [real, b])
        print(f"{anim:5d} {len(canvas):6d} {cw:4d}x{ch:<4d}  " + "  ".join(f"{np.mean(errs[m]):8.2f}" for m in methods) +
              f"   {np.mean(moves) if moves else 0:5.1f}")
        if args.sheet:
            labels = ["poprzednia"] + [{"hold": "hold", "blend": "blend", "flow": "flow (OpenCV)"}.get(m, m.upper()) for m in methods] + ["PRAWDZIWA", "nastepna"]
            z, pad, bar = 3, 4, 16
            sheet = Image.new("RGB", (len(labels) * (cw * z + pad) + pad, bar + len(rows) * (ch * z + pad) + pad), (24, 24, 24))
            draw = ImageDraw.Draw(sheet)
            for c, label in enumerate(labels):
                draw.text((pad + c * (cw * z + pad) + 2, 2), label, fill=(235, 235, 235))
            for r, row in enumerate(rows):
                for c, img in enumerate(row):
                    sheet.paste(Image.fromarray(it.over_bg(img), "RGB").resize((cw * z, ch * z), Image.NEAREST),
                                (pad + c * (cw * z + pad), bar + r * (ch * z + pad)))
            sheet.save(ROOT / "hd" / "compare" / f"klatki_posrednie_{anim}.png")
    print("RIFE time per pair: " + ", ".join(f"x{m}: {np.median(timing[m][3:] or timing[m]) * 1000:.0f} ms" for m in args.magnify))

    idle, _, _ = build_canvas(store, bank, it.load_animation(args.file, args.profile, 0))
    fig = cv2.dilate((sum(p[..., 3] + p[..., 4] for p in idle) > 0).astype(np.uint8), np.ones((5, 5), np.uint8)).astype(bool)
    floor = [it.errors(idle[i], idle[i + 1], fig)[1] for i in range(len(idle) - 1)]
    print(f"noise floor (consecutive frames of the idle animation): {np.mean(floor):.1f}")


if __name__ == "__main__":
    main()
