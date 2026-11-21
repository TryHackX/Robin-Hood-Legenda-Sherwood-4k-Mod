"""How many steps of a character's animations get an in-between frame in the game (2026-11-22, user: "picking up,
tying and so on have none?"), per action (a group of 16 animations = 16 directions), and a sheet of the actions
with the lowest share - frames of one direction, to see what they are.

    python -I tools/hd/between_coverage.py [--file Characters/RobinHood.rhs] [--sheet out.png] [--groups 12]

Shown = the pair is in hd/sprites/_between with a frame (size != 0) and its quality (between.q) is at most 17
(HDInBetweenQuality's default); "with the second try" also counts hd/sprites/_between_more. Steps between equal
frames are not counted. --file Characters/ = every character file.
"""
import argparse
import csv
import struct
import sys
from collections import defaultdict
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
import interp_test as it  # noqa: E402

ENTRY = np.dtype([("a", "<u4"), ("b", "<u4"), ("off", "<u8"), ("size", "<u4"), ("w", "<u2"), ("h", "<u2"),
                  ("dxa", "<i2"), ("dya", "<i2"), ("dxb", "<i2"), ("dyb", "<i2")])


def shown_pairs(store, limit):
    """(a, b) of the pairs a store shows: a frame (size != 0) and quality up to `limit`."""
    d = ROOT / "hd" / "sprites" / store
    if not (d / "between.idx").exists() or not (d / "between.q").exists():
        return set()
    raw = (d / "between.idx").read_bytes()
    idx = np.frombuffer(raw, ENTRY, struct.unpack_from("<I", raw, 8)[0], 16)
    q = np.frombuffer((d / "between.q").read_bytes(), np.uint8, len(idx), 16)
    ok = (idx["size"] > 0) & (q <= limit)
    return set(zip(idx["a"][ok].tolist(), idx["b"][ok].tolist()))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--file", default="Characters/RobinHood.rhs", help='one .rhs, or a folder ending in "/" (all of it)')
    ap.add_argument("--sheet", default="")
    ap.add_argument("--groups", type=int, default=12, help="actions with the lowest share on the sheet")
    ap.add_argument("--limit", type=int, default=17)
    args = ap.parse_args()
    # the characters' store, and the second try of the pairs it rejected (2026-11-22, _between_more)
    first = shown_pairs("_between", args.limit)
    more = shown_pairs("_between_more", args.limit)
    anims = defaultdict(list)
    with open(ROOT / "hd" / "work" / "frame_index.csv", newline="", encoding="utf-8", errors="replace") as f:
        for r in csv.reader(f):
            if r[1] == args.file or (args.file.endswith("/") and r[1].startswith(args.file)):
                anims[(r[1], int(r[3]))].append((int(r[4]), int(r[0])))
    groups = defaultdict(lambda: [0, 0, 0])           # steps, with a frame of the first store, with the second try
    for (name, a), steps in anims.items():
        fr = [f for _, f in sorted(steps)]
        for x, y in zip(fr, fr[1:]):
            if x == y:
                continue
            g = groups[(name, a // 16)]
            g[0] += 1
            k = (min(x, y), max(x, y))
            g[1] += k in first
            g[2] += k in first or k in more
    total = np.array(list(groups.values())).sum(0)
    print(f"{args.file}: {len(groups)} actions, {total[0]} steps with an in-between frame: {total[1]} "
          f"({100.0 * total[1] / max(1, total[0]):.0f} %), with the second try {total[2]} "
          f"({100.0 * total[2] / max(1, total[0]):.0f} %)")
    low = sorted((v[2] / v[0], key, v[0], v[1] / v[0]) for key, v in groups.items() if v[0] >= 20)
    for share, (name, g), n, share1 in low[: args.groups]:
        print(f"  {name.split('/')[-1]} action {g:3d}: {100 * share:3.0f} % of {n} steps (first store {100 * share1:3.0f} %)")
    if not args.sheet:
        return
    store = it.HdStore("_common")
    rows = []
    for share, (name, g), n, _ in low[: args.groups]:
        fr = [f for _, f in sorted(anims[(name, g * 16 + 4)])]          # one direction
        picks = [fr[int(i * (len(fr) - 1) / 5)] for i in range(6)] if len(fr) >= 6 else fr
        rows.append((g, share, [store.frame(f) for f in picks]))
    cell = 150
    sheet = Image.new("RGB", (40 + 6 * cell, len(rows) * cell), (40, 40, 40))
    draw = ImageDraw.Draw(sheet)
    for r, (g, share, frames) in enumerate(rows):
        draw.text((4, r * cell + 4), f"{g}\n{100 * share:.0f}%", fill=(255, 255, 0))
        for c, px in enumerate(frames):
            if px is None:
                continue
            rgb = it.rgb_from_565(px).astype(np.uint8)
            alpha = ((px != 0x07C0) & (px != 0x001F)).astype(np.uint8) * 255
            im = Image.fromarray(np.dstack([rgb, alpha]), "RGBA")
            im.thumbnail((cell - 6, cell - 6))
            sheet.paste(im, (40 + c * cell + (cell - im.width) // 2, r * cell + (cell - im.height) // 2), im)
    sheet.save(args.sheet)
    print("->", args.sheet)


if __name__ == "__main__":
    main()
