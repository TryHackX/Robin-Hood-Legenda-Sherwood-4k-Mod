"""Is a level's HD background where the game's own picture is? (2026-11-22, YouTuber's report: the green fields of
left Shift looked shifted over a rock on the first crossroads map.)

Compares the HD background (hd\\maps\\<variant>\\<key>.hdmap, 2x) shrunk to the level's size with the original
DATA\\Levels\\<time>\\<level>.map: phase correlation over the whole level and in tiles, so a local shift shows too.
    python tools/hd/map_alignment.py croisement01 day [variant]
"""
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from extract_maps import read_map, rgb565_to_rgb    # noqa: E402
from hdmap_to_png import fnv1a64, read_hdmap        # noqa: E402

ROOT = Path(__file__).resolve().parents[2]


def grey(a):
    a = a.astype(np.float32)
    return a[..., 0] * 0.299 + a[..., 1] * 0.587 + a[..., 2] * 0.114 if a.ndim == 3 else a


def shift_of(a, b):
    """(dx, dy) to add to b's coordinates to land on a, and the peak's strength."""
    a = a - a.mean()
    b = b - b.mean()
    win = np.outer(np.hanning(a.shape[0]), np.hanning(a.shape[1]))
    fa, fb = np.fft.fft2(a * win), np.fft.fft2(b * win)
    r = fa * np.conj(fb)
    r /= np.abs(r) + 1e-6
    c = np.fft.ifft2(r).real
    y, x = np.unravel_index(np.argmax(c), c.shape)
    if y > a.shape[0] // 2:
        y -= a.shape[0]
    if x > a.shape[1] // 2:
        x -= a.shape[1]
    return x, y, float(c.max())


def main():
    level, time = sys.argv[1], sys.argv[2]
    variant = sys.argv[3] if len(sys.argv) > 3 else 'dat2'
    src = next(p for p in (ROOT / 'DATA' / 'Levels').iterdir()
               if p.is_dir() and p.name.lower() == time.lower())
    mp = next(p for p in src.iterdir() if p.name.lower() == level.lower() + '.map')
    width, height, _, _, data, _, _ = read_map(mp)
    orig = np.asarray(rgb565_to_rgb(data, width, height))
    key = '%016x' % fnv1a64(data[: width * height * 2])     # the mod's key of the level's HD background
    hd = np.asarray(read_hdmap(ROOT / 'hd' / 'maps' / variant / (key + '.hdmap')))
    if hd.ndim == 2:                       # RGB565 words
        hd = np.asarray(rgb565_to_rgb(hd.astype('<u2').tobytes(), hd.shape[1], hd.shape[0]))
    h, w = orig.shape[:2]
    print('original', mp.name, '%dx%d' % (w, h), ' HD', key, '%dx%d' % (hd.shape[1], hd.shape[0]))
    small = hd[: 2 * h, : 2 * w].astype(np.float32)
    small = (small[0::2, 0::2] + small[1::2, 0::2] + small[0::2, 1::2] + small[1::2, 1::2]) / 4.0
    a, b = grey(orig), grey(small)
    dx, dy, p = shift_of(a, b)
    print('whole level: HD lies %+d,%+d original pixels from the game picture (peak %.3f)' % (dx, dy, p))
    t = 256
    worst = 0
    for y0 in range(0, h - t + 1, t):
        row = []
        for x0 in range(0, w - t + 1, t):
            dx, dy, p = shift_of(a[y0:y0 + t, x0:x0 + t], b[y0:y0 + t, x0:x0 + t])
            row.append('%+d,%+d' % (dx, dy) if p > 0.05 else ' ? ')
            if p > 0.05:
                worst = max(worst, abs(dx), abs(dy))
        print('  y%4d: ' % y0 + '  '.join('%7s' % s for s in row))
    print('largest local shift: %d original pixels' % worst)


if __name__ == '__main__':
    main()
