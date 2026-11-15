"""Look at an animation as the game shows it with in-between frames: frame, in-between, frame, in-between ...
taken from the stores (hd/sprites/_common = HD frames, hd/sprites/_between = in-betweens).

    python -I tools/hd/between_view.py --file Characters/RobinTown.rhs --list            animations, steps, ticks
    python -I tools/hd/between_view.py --file Characters/RobinTown.rhs --anim 12 [--profile NAME] [--sheet PNG]

Sheet (2x): one row per animation; under each in-between its quality byte (between.q) and the step's ticks.
"""
import argparse
import csv
import struct
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
WORK = ROOT / 'hd' / 'work'
BETWEEN = ROOT / 'hd' / 'sprites' / '_between'
ENTRY = np.dtype([("a", "<u4"), ("b", "<u4"), ("off", "<u8"), ("size", "<u4"), ("w", "<u2"), ("h", "<u2"),
                  ("dxa", "<i2"), ("dya", "<i2"), ("dxb", "<i2"), ("dyb", "<i2")])


def read_index(path_file, profile=None):
    anims = {}
    with open(WORK / 'frame_index.csv', newline='', encoding='utf-8', errors='replace') as f:
        r = csv.reader(f)
        next(r)
        for row in r:
            if row[1] != path_file or (profile and row[2] != profile):
                continue
            anims.setdefault((row[2], int(row[3])), []).append((int(row[4]), int(row[0]), int(row[5]), int(row[7]), int(row[8])))
    return {k: sorted(v) for k, v in anims.items()}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--file', required=True)
    ap.add_argument('--profile', default=None)
    ap.add_argument('--anim', type=int, nargs='*', default=[])
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--sheet', default='hd/compare/posrednie_animacja.png')
    ap.add_argument('--zoom', type=int, default=2)
    ap.add_argument('--stats', action='store_true',
                    help='per step: how much the original 1x frame changes, how much the HD frame changes where the '
                         '1x one does not ("boil" = details the upscaler drew differently), and for the in-between '
                         'how patchy it is (neighbouring pixels taken from different frames) and how much is new')
    args = ap.parse_args()
    anims = read_index(args.file, args.profile)
    if args.list:
        for (prof, a), steps in sorted(anims.items()):
            ticks = [s[2] for s in steps]
            print(f'{prof!r:40} anim {a:3}: {len(steps):3} steps, ticks per step {min(ticks)}..{max(ticks)} (mean {np.mean(ticks):.1f})')
        return
    import interp_test as it
    from upscale_sprites import decode_spans
    from extract_sprites import KEY, MARK
    from PIL import Image, ImageDraw
    store = it.HdStore('_common')
    raw = (BETWEEN / 'between.idx').read_bytes()
    count = struct.unpack_from('<I', raw, 8)[0]
    idx = np.frombuffer(raw, ENTRY, count, 16)
    keys = idx['a'].astype(np.uint64) << np.uint64(32) | idx['b'].astype(np.uint64)
    qfile = BETWEEN / 'between.q'
    q = np.frombuffer(qfile.read_bytes(), np.uint8, count, 16) if qfile.exists() else np.full(count, 255, np.uint8)
    dat = open(BETWEEN / 'between.dat', 'rb')

    def between(a, b):
        lo, hi = (a, b) if a < b else (b, a)
        k = np.uint64(lo) << np.uint64(32) | np.uint64(hi)
        i = int(np.searchsorted(keys, k))
        if i >= count or keys[i] != k or idx['size'][i] == 0:
            return None
        e = idx[i]
        dat.seek(int(e['off']))
        data = dat.read(int(e['size']))
        w, h = int(e['w']), int(e['h'])
        px = np.frombuffer(data, '<u2').reshape(h, w) if len(data) == w * h * 2 else decode_spans(data, w, h)
        dx, dy = (int(e['dxa']), int(e['dya'])) if a < b else (int(e['dxb']), int(e['dyb']))
        return px, dx, dy, int(q[i])

    def rgb(px):
        r = ((px >> 11) & 31).astype(np.float32) * 255 / 31
        g = ((px >> 5) & 63).astype(np.float32) * 255 / 63
        b = (px & 31).astype(np.float32) * 255 / 31
        out = np.stack([r, g, b], -1)
        bg = np.array([96, 112, 80], np.float32)
        out[px == KEY] = bg
        out[px == MARK] = bg * 0.55
        return out.astype(np.uint8)

    def figure(px):
        """RGB float planes and the figure mask of a 565 frame (marker = shadow counts as figure)."""
        return rgb(px).astype(np.float32), px != KEY

    def place(canvas_wh, origin, pic, x, y):
        cw, ch = canvas_wh
        rgbp, m = figure(pic)
        c = np.zeros((ch, cw, 3), np.float32)
        cm = np.zeros((ch, cw), bool)
        ox, oy = x - origin[0], y - origin[1]
        c[oy:oy + pic.shape[0], ox:ox + pic.shape[1]] = rgbp
        cm[oy:oy + pic.shape[0], ox:ox + pic.shape[1]] = m
        return c, cm

    def stats(steps):
        from extract_sprites import Bank
        bank = Bank()
        print(f'{"step":>18} {"ticks":>5} {"1x px":>6} {"1x d":>6} {"HD d":>6} {"boil":>6} {"mid q":>5} '
              f'{"patchy":>6} {"new":>6}')
        for i in range(len(steps) - 1):
            (_, fa, ta, xa, ya), (_, fb, tb, xb, yb) = steps[i], steps[i + 1]
            oa, ob = bank.decode16(fa), bank.decode16(fb)
            ha, hb = store.frame(fa), store.frame(fb)
            if ha is None or hb is None:
                continue
            m = between(fa, fb)
            # 1x: aligned by the hotspot offsets; the canvas also holds the in-between (HD box halved)
            boxes = [(-xa, -ya, oa.shape[1], oa.shape[0]), (-xb, -yb, ob.shape[1], ob.shape[0])]
            if m is not None:
                mx, my = -2 * xa + m[1], -2 * ya + m[2]
                boxes.append((mx // 2, my // 2, (m[0].shape[1] + 3) // 2, (m[0].shape[0] + 3) // 2))
            x0 = min(b[0] for b in boxes); y0 = min(b[1] for b in boxes)
            W = max(b[0] + b[2] for b in boxes) - x0
            Hh = max(b[1] + b[3] for b in boxes) - y0
            ca, ma = place((W, Hh), (x0, y0), oa, -xa, -ya)
            cb, mb = place((W, Hh), (x0, y0), ob, -xb, -yb)
            fig1 = ma | mb
            d1 = np.abs(ca - cb).sum(-1) * fig1 + (ma != mb) * 255
            changed1 = d1 > 12
            # HD: the same alignment doubled
            ca2, ma2 = place((2 * W, 2 * Hh), (2 * x0, 2 * y0), ha, -2 * xa, -2 * ya)
            cb2, mb2 = place((2 * W, 2 * Hh), (2 * x0, 2 * y0), hb, -2 * xb, -2 * yb)
            fig2 = ma2 | mb2
            d2 = np.abs(ca2 - cb2).sum(-1) * fig2 + (ma2 != mb2) * 255
            # where the 1x frame (and its 3x3 neighbourhood) did not change, doubled
            import cv2
            still1 = cv2.erode((~changed1 & fig1).astype(np.uint8), np.ones((3, 3), np.uint8)) > 0
            still2 = np.repeat(np.repeat(still1, 2, 0), 2, 1) & ma2 & mb2
            boil = float(d2[still2].mean()) if still2.any() else 0.0
            patchy = new = float('nan'); qq = -1
            if m is not None:
                px, dx, dy, qq = m
                cm, mm = place((2 * W, 2 * Hh), (2 * x0, 2 * y0), px, -2 * xa + dx, -2 * ya + dy)
                ea = np.abs(cm - ca2).sum(-1); eb = np.abs(cm - cb2).sum(-1)
                both = mm & ma2 & mb2 & (d2 > 24)          # pixels where the two frames differ
                src = ea < eb
                # neighbouring pixel pairs inside `both` that come from different frames
                hpair = both[:, 1:] & both[:, :-1]
                vpair = both[1:, :] & both[:-1, :]
                sw = (src[:, 1:] != src[:, :-1])[hpair].sum() + (src[1:, :] != src[:-1, :])[vpair].sum()
                patchy = 100.0 * sw / max(1, hpair.sum() + vpair.sum())
                new = 100.0 * float((np.minimum(ea, eb)[mm & (ma2 | mb2)] > 60).mean())
            # motion of the 1x figure: dense optical flow on the frames magnified 4x, in 1x pixels
            ga = cv2.resize(cv2.cvtColor(ca.astype(np.uint8), cv2.COLOR_RGB2GRAY), None, fx=4, fy=4, interpolation=cv2.INTER_CUBIC)
            gb = cv2.resize(cv2.cvtColor(cb.astype(np.uint8), cv2.COLOR_RGB2GRAY), None, fx=4, fy=4, interpolation=cv2.INTER_CUBIC)
            dis = cv2.DISOpticalFlow_create(cv2.DISOPTICAL_FLOW_PRESET_MEDIUM)
            fl = dis.calc(ga, gb, None)
            mag = np.hypot(fl[..., 0], fl[..., 1]) / 4.0
            fm = cv2.resize(fig1.astype(np.uint8), (ga.shape[1], ga.shape[0]), interpolation=cv2.INTER_NEAREST) > 0
            p50, p90 = np.percentile(mag[fm], [50, 90]) if fm.any() else (0.0, 0.0)
            print(f'{fa:>8}->{fb:<8} {ta:>5} {100 * changed1[fig1].mean():6.1f} {d1[fig1].mean():6.1f} '
                  f'{d2[fig2].mean():6.1f} {boil:6.1f} {qq:>5} {patchy:6.1f} {new:6.1f}   flow {p50:4.1f} {p90:4.1f}')

    rows = []
    for a_no in args.anim:
        key = next((k for k in anims if k[1] == a_no), None)
        if not key:
            print(f'anim {a_no}: not in {args.file}')
            continue
        steps = anims[key]
        if args.stats:
            print(f'anim {a_no} ({key[0]}):')
            stats(steps)
            continue
        items = []      # (picture, x, y, label)
        for i, (st, fr, ticks, x, y) in enumerate(steps):
            hd = store.frame(fr)
            if hd is None:
                continue
            items.append((rgb(hd), -2 * x, -2 * y, f'{fr} t{ticks}'))
            if i + 1 < len(steps):
                nxt = steps[i + 1][1]
                m = between(fr, nxt)
                if m is not None:
                    px, dx, dy, qq = m
                    items.append((rgb(px), -2 * x + dx, -2 * y + dy, f'mid q{qq}'))
                else:
                    items.append((None, 0, 0, 'brak'))
        pics = [p for p in items if p[0] is not None]
        x0 = min(p[1] for p in pics); y0 = min(p[2] for p in pics)
        x1 = max(p[1] + p[0].shape[1] for p in pics); y1 = max(p[2] + p[0].shape[0] for p in pics)
        cw, ch = x1 - x0, y1 - y0
        z = args.zoom
        row = Image.new('RGB', (len(items) * (cw * z + 4), ch * z + 16), (30, 30, 30))
        d = ImageDraw.Draw(row)
        for k, (pic, x, y, label) in enumerate(items):
            cell = Image.new('RGB', (cw, ch), (96, 112, 80))
            if pic is not None:
                cell.paste(Image.fromarray(pic), (x - x0, y - y0))
            row.paste(cell.resize((cw * z, ch * z), Image.NEAREST), (k * (cw * z + 4), 16))
            d.text((k * (cw * z + 4) + 2, 2), label, fill=(255, 255, 120) if label.startswith('mid') else (200, 200, 200))
        rows.append(row)
        print(f'anim {a_no} ({key[0]}): {len(steps)} steps')
    if rows:
        W = max(r.width for r in rows)
        sheet = Image.new('RGB', (W, sum(r.height + 6 for r in rows)), (20, 20, 20))
        yy = 0
        for r in rows:
            sheet.paste(r, (0, yy)); yy += r.height + 6
        out = ROOT / args.sheet
        out.parent.mkdir(parents=True, exist_ok=True)
        sheet.save(out)
        print('->', out, sheet.size)


if __name__ == '__main__':
    main()
