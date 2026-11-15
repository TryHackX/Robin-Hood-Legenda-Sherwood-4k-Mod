"""Study: ground animations (water, chimney smoke) over the HD background.

The game animates water and smoke with large sprites that repaint the ground they lie on: every
frame is the level picture of that spot plus ripples / smoke, low resolution and block-compressed
(4x1 vector quantisation). Magnified 2x over an HD background they show as a blurry patch. Here
the difference "frame - level picture beneath" is added to the HD picture instead, in several
variants, and contact sheets are written for a visual comparison:

    python -I tools/hd/ground_anim_study.py [--variant dat2]
        -> hd/compare/ground_anim/<name>.png   (one sheet per animation)

Rows of a sheet: frames. Columns: HD background (still) | the game's frame 2x over it (what the
mod drew for smoke) | HD + difference | HD + smoothed difference with a soft threshold.
"""
import argparse, os, sys
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
from extract_sprites import Bank, KEY  # noqa: E402

# (name, level picture, level position, frame size, frame ids or None = search by size and ground)
CASES = [
    ('pond_nottingham', 'nottingham_day', (1196, 2632), (272, 178), list(range(4579, 4587))),
    ('smoke_lincoln_a', 'lincoln_day', (1349, 1274), (52, 106), None),
    ('smoke_lincoln_b', 'lincoln_day', (1609, 1182), (52, 111), None),
]


def rgb565_to_float(img):
    r = ((img >> 11) & 31).astype(np.float32) * (255.0 / 31.0)
    g = ((img >> 5) & 63).astype(np.float32) * (255.0 / 63.0)
    b = (img & 31).astype(np.float32) * (255.0 / 31.0)
    return np.stack([r, g, b], -1)


def box_blur(a, r):
    if r <= 0:
        return a
    k = 2 * r + 1
    pad = np.pad(a, ((r, r), (r, r), (0, 0)), mode='edge')
    c = np.cumsum(np.cumsum(pad, 0), 1)
    c = np.pad(c, ((1, 0), (1, 0), (0, 0)))
    return (c[k:, k:] - c[:-k, k:] - c[k:, :-k] + c[:-k, :-k]) / (k * k)


def up2_bilinear(a):
    """2x like the DLL's smooth filter: destination pixel = 1/4 - 3/4 mix of the two nearest."""
    def axis(x, ax):
        n = x.shape[ax]
        idx = np.arange(2 * n)
        k = idx // 2
        near = np.where(idx % 2 == 1, k, k - 1)
        far = near + 1
        wn = np.where(idx % 2 == 1, 0.75, 0.25)
        near = np.clip(near, 0, n - 1)
        far = np.clip(far, 0, n - 1)
        xa = np.take(x, near, ax)
        xb = np.take(x, far, ax)
        shape = [1] * x.ndim
        shape[ax] = 2 * n
        wn = wn.reshape(shape)
        return xa * wn + xb * (1 - wn)
    return axis(axis(a, 0), 1)


def find_frames(bank, size, ground, limit=12):
    w, h = size
    fr = bank.frames
    ids = np.nonzero((fr['w'] == w) & (fr['h'] == h))[0]
    scored = []
    for i in ids:
        try:
            img = bank.decode16(int(i))
        except Exception:
            continue
        op = img != KEY
        if op.sum() < 0.8 * w * h:
            continue
        d = np.abs(rgb565_to_float(img) - ground)[op].mean()
        scored.append((d, int(i)))
    scored.sort()
    # frames of one animation lie next to each other in the bank: keep the run around the best
    if not scored:
        return []
    best = scored[0][1]
    near = sorted(i for d, i in scored if abs(i - best) < 40 and d < 40)
    return near[:limit]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--variant', default='dat2')
    ap.add_argument('--thr', type=float, nargs=2, default=(6.0, 18.0), help='soft threshold of the smoothed difference')
    ap.add_argument('--blur', type=int, default=1, help='box blur radius of the difference (low-res pixels)')
    args = ap.parse_args()

    bank = Bank()
    outdir = os.path.join(ROOT, 'hd', 'compare', 'ground_anim')
    os.makedirs(outdir, exist_ok=True)
    for name, level, (mx, my), (w, h), ids in CASES:
        lo = np.asarray(Image.open(os.path.join(ROOT, 'hd', 'maps_png', level + '.png')).convert('RGB'), np.float32)
        hd = np.asarray(Image.open(os.path.join(ROOT, 'hd', 'maps_hd', args.variant, level + '.png')).convert('RGB'), np.float32)
        ground = lo[my:my + h, mx:mx + w]
        hdg = hd[2 * my:2 * (my + h), 2 * mx:2 * (mx + w)]
        if ids is None:
            ids = find_frames(bank, (w, h), ground)
        if not ids:
            print(name, ': no frames found')
            continue
        rows = []
        for i in ids:
            img = bank.decode16(i)
            if img.shape != (h, w):
                print(name, 'frame', i, 'has size', img.shape[::-1], '- skipped')
                continue
            op = (img != KEY)
            f = rgb565_to_float(img)
            d = np.where(op[..., None], f - ground, 0.0)
            ad = np.abs(d).mean(-1)
            print('%s frame %d: opaque %.0f%%, |frame - ground| mean %.1f, p50 %.1f, p90 %.1f, p99 %.1f' % (
                name, i, 100.0 * op.mean(), ad[op].mean(), *np.percentile(ad[op], [50, 90, 99])))
            # what was drawn: the frame 2x (nearest) over the HD picture
            f2 = np.repeat(np.repeat(f, 2, 0), 2, 1)
            op2 = np.repeat(np.repeat(op, 2, 0), 2, 1)
            patch = np.where(op2[..., None], f2, hdg)
            # HD + difference
            plain = np.clip(hdg + up2_bilinear(d), 0, 255)
            # HD + smoothed difference, soft threshold on its size
            sd = box_blur(d, args.blur)
            mag = np.abs(sd).max(-1, keepdims=True)
            t1, t2 = args.thr
            wgt = np.clip((mag - t1) / max(1e-3, t2 - t1), 0.0, 1.0)
            soft = np.clip(hdg + up2_bilinear(sd * wgt), 0, 255)
            rows.append(np.concatenate([hdg, patch, plain, soft], 1))
        sheet = np.concatenate(rows, 0).astype(np.uint8)
        path = os.path.join(outdir, name + '.png')
        Image.fromarray(sheet).save(path)
        print('->', path, sheet.shape[1], 'x', sheet.shape[0], 'frames', ids)


if __name__ == '__main__':
    main()
