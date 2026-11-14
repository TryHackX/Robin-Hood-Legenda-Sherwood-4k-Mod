"""In-between frames as the game would use them: an animation as it is played, next to the same
animation with one generated frame between every two steps.

    python -I tools\\hd\\interp_demo.py --anim 128 [--file RobinTown.rhs] [--profile "Robin des bois"]
                                        [--magnify 4] [--ms 80] [--name chod]

Left: the game's frames, one per step (the game shows one per 40 ms tick while walking).
Right: frame, generated in-between, frame, ... - twice the frame rate (RIFE v4.26, tools\\hd\\rife_net.py).
The picture runs at half speed by default (--ms 80 per game step) so that the difference can be seen
in a viewer that does not play 50 frames per second.
Output: hd\\compare\\<name>_25_vs_50.gif  and  hd\\compare\\<name>_wszystkie_klatki.png (every frame of
the doubled animation on one sheet; generated ones are marked).
Run with "python -I": the script reads a downloaded weights file.
"""
import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parent))
import interp_test as it  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--file", default="RobinTown.rhs")
    ap.add_argument("--profile", default="Robin des bois")
    ap.add_argument("--anim", type=int, default=128)
    ap.add_argument("--magnify", type=int, default=4)
    ap.add_argument("--ms", type=int, default=80, help="display time of one game step")
    ap.add_argument("--zoom", type=int, default=4)
    ap.add_argument("--name", default="chod")
    args = ap.parse_args()

    seq = it.load_animation(args.file, args.profile, args.anim)  # steps in playing order, repeats included
    bank = it.Bank()
    store = it.HdStore("_common")
    planes = []
    for fr, x, y in seq:
        px = store.frame(fr)
        if px is None:
            px = np.repeat(np.repeat(bank.decode16(fr), 2, 0), 2, 1)
        planes.append((x * 2, y * 2, it.to_planes(px)))
    x0, y0 = min(p[0] for p in planes), min(p[1] for p in planes)
    cw = max(p[0] - x0 + p[2].shape[1] for p in planes) + 16
    ch = max(p[1] - y0 + p[2].shape[0] for p in planes) + 16
    steps = []
    for x, y, pl in planes:
        c = np.zeros((ch, cw, 5), np.float32)
        c[y - y0 + 8:y - y0 + 8 + pl.shape[0], x - x0 + 8:x - x0 + 8 + pl.shape[1]] = pl
        steps.append(c)
    n = len(steps)
    print(f"animation {args.anim}: {n} steps, frames {[s[0] for s in seq]}")

    rife = it.Rife()
    between = []
    for i in range(n):
        a, b = steps[i], steps[(i + 1) % n]
        same = seq[i][0] == seq[(i + 1) % n][0] and seq[i][1:] == seq[(i + 1) % n][1:]
        between.append(a if same else it.harden(rife.middle(a, b, args.magnify)))

    z = args.zoom
    gif_frames = []
    for i in range(n):
        for half in (0, 1):
            left = steps[i]
            right = steps[i] if half == 0 else between[i]
            strip = Image.new("RGB", (2 * (cw * z + 6) + 6, ch * z + 22), (24, 24, 24))
            d = ImageDraw.Draw(strip)
            for c, (label, img) in enumerate(((f"gra: {1000 // 40} kl./s", left), (f"+ klatki posrednie: {2000 // 40} kl./s", right))):
                x = 6 + c * (cw * z + 6)
                d.text((x + 2, 4), label, fill=(235, 235, 235))
                strip.paste(Image.fromarray(it.over_bg(img), "RGB").resize((cw * z, ch * z), Image.NEAREST), (x, 20))
            gif_frames.append(strip)
    gif = ROOT / "hd" / "compare" / f"{args.name}_25_vs_50.gif"
    gif_frames[0].save(gif, save_all=True, append_images=gif_frames[1:], duration=args.ms // 2, loop=0)
    print(f"animation: {gif} ({len(gif_frames)} pictures, {args.ms // 2} ms each = half speed at --ms 80)")

    # every frame of the doubled animation on one sheet
    cols = min(2 * n, 12)
    rows = (2 * n + cols - 1) // cols
    sz = 3
    sheet = Image.new("RGB", (cols * (cw * sz + 4) + 4, rows * (ch * sz + 18) + 4), (24, 24, 24))
    d = ImageDraw.Draw(sheet)
    for k in range(2 * n):
        img = steps[k // 2] if k % 2 == 0 else between[k // 2]
        x = 4 + (k % cols) * (cw * sz + 4)
        y = 4 + (k // cols) * (ch * sz + 18)
        d.text((x + 2, y + 2), f"klatka gry {seq[k // 2][0]}" if k % 2 == 0 else "WYGENEROWANA", fill=(235, 235, 235) if k % 2 == 0 else (255, 220, 90))
        sheet.paste(Image.fromarray(it.over_bg(img), "RGB").resize((cw * sz, ch * sz), Image.NEAREST), (x, y + 16))
    out = ROOT / "hd" / "compare" / f"{args.name}_wszystkie_klatki.png"
    sheet.save(out)
    print(f"sheet: {out} {sheet.size}")


if __name__ == "__main__":
    main()
