"""In-between frames: the mix of the two moved frames (as made) against the sharp choice per pixel (Maker.sharp),
on pairs the game leaves out today because the two frames disagree (colour above HDInBetweenQuality).

    python -I tools/hd/inbetween_sharp_test.py [--n 120] [--range 17 32] [--sheet hd/compare/posrednie_ostre.png]

Sheet: per pair  frame A | mix (today) | sharp (new) | frame B, label = pair, colour, shape, thin parts of both.
"""
import argparse
import csv
import glob
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--n', type=int, default=120)
    ap.add_argument('--range', type=float, nargs=2, default=[17.0, 32.0])
    ap.add_argument('--sheet', default='hd/compare/posrednie_ostre.png')
    ap.add_argument('--seed', type=int, default=7)
    args = ap.parse_args()
    import make_inbetweens as mi
    from PIL import Image, ImageDraw

    rows = []
    for p in glob.glob(str(ROOT / 'hd' / 'work' / 'between_log*.csv')):
        with open(p, newline='', encoding='utf-8') as f:
            for r in csv.DictReader(f):
                try:
                    c = float(r['colour'])
                except (KeyError, ValueError):
                    continue
                if r.get('usable') == '1' and args.range[0] < c <= args.range[1]:
                    rows.append((int(r['a']), int(r['b'])))
    rows = sorted(set(rows))
    rng = np.random.default_rng(args.seed)
    keys = [rows[i] for i in rng.choice(len(rows), min(args.n, len(rows)), replace=False)]
    print(f'{len(rows)} usable pairs with colour in {args.range}; {len(keys)} taken')
    anims, offs = mi.read_animations(['Characters/'])
    store = mi.it.HdStore('_common')
    items = mi.prepare(store, offs, sorted(keys))
    maker = mi.Maker(4)
    groups = {}
    for it_ in items:
        groups.setdefault(it_[1].shape[:2], []).append(it_)
    cells = []
    stats = []
    for gk, members in groups.items():
        canv = [(a, b) for _, a, b, _, _ in members]
        maker.sharp = False
        mixed = mi.run_safe(maker, canv)
        maker.sharp = True
        sharp = mi.run_safe(maker, canv)
        for (key, a, b, _, _), rm, rs in zip(members, mixed, sharp):
            if rm is None or rs is None:
                continue
            tm, ts = mi.thin_kept(a, b, rm[0]), mi.thin_kept(a, b, rs[0])
            stats.append((rm[1], tm, ts))
            parts = [mi.it.over_bg(a), mi.it.over_bg(mi.it.harden(rm[0])), mi.it.over_bg(mi.it.harden(rs[0])), mi.it.over_bg(b)]
            hh, ww = parts[0].shape[:2]
            z = 2
            cell = Image.new('RGB', (4 * ww * z + 12, hh * z + 14), (20, 20, 20))
            for k, part in enumerate(parts):
                cell.paste(Image.fromarray(part, 'RGB').resize((ww * z, hh * z), Image.NEAREST), (k * (ww * z + 4), 14))
            ImageDraw.Draw(cell).text((2, 1), f'{key[0]}-{key[1]} kolor {rm[1]:.1f} ksztalt {rm[2]:.2f} cienkie {tm:.2f}/{ts:.2f}',
                                      fill=(220, 220, 120))
            cells.append(cell)
    width = 2400
    x = y = 4
    row_h = 0
    pos = []
    for c in cells:
        if x + c.width + 4 > width and x > 4:
            x, y, row_h = 4, y + row_h + 4, 0
        pos.append((x, y))
        x += c.width + 4
        row_h = max(row_h, c.height)
    sheet = Image.new('RGB', (width, y + row_h + 4), (48, 48, 48))
    for c, (cx, cy) in zip(cells, pos):
        sheet.paste(c, (cx, cy))
    out = ROOT / args.sheet
    out.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(out)
    s = np.array(stats)
    print(f'-> {out} {sheet.size}; thin parts kept, median mix {np.median(s[:, 1]):.2f} / sharp {np.median(s[:, 2]):.2f}')


if __name__ == '__main__':
    main()
