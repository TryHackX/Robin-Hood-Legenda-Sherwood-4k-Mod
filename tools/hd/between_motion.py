"""Motion and timing of every in-between pair, for the game (2026-11-15, user report: Robin standing still looked
"as if his clothes kept changing", the crouch still looked choppy).

    python -I tools/hd/between_motion.py [--workers 6] [--store _between_anim]
        -> hd/work/between_motion.npz (store _between) or hd/work/between_motion_<store>.npz (+ a summary)

1. MOTION. Many animation steps are no movement at all: in Robin's idle animation the original 1x frames differ in a
   quarter of the figure's pixels, but only by shading (cloth folds) - the optical flow between them is ~0.1 px. An
   in-between frame adds nothing there; the sharp store builds it pixel by pixel from one of the two moved frames,
   which for a pair without motion is a patchwork of the two (30 % of neighbouring pixels from different frames).
   Shown in the first part of every step it made the figure flicker. Measured here per pair: dense optical flow
   (OpenCV DIS) between the two 1x frames placed by their hotspot offsets, magnified 4x; p50 / p90 of its length over
   the figure, in 1x pixels. between_quality.py leaves out pairs whose p90 stays under MIN_MOTION_P90.
2. TIMING. A step of an animation lasts flags + 1 game ticks (the .rhs frame record, confirmed with a draw trace:
   flags 2 -> the frame stayed 3 ticks = 120 ms). The DLL showed an in-between only in the first ~45 % of the first
   tick after a change - for steps of 2-3 ticks (crouching, many fight moves) that is 20 ms of 80-120 ms, the motion
   stayed choppy. Kept here per pair and direction: how many ticks the frame the animation changes TO is shown
   (hold_ab: a -> b, the ticks of b; hold_ba: b -> a, the ticks of a; 0 = unknown); between_quality.py writes them to
   between.t and the DLL shows the in-between for half of that step.
"""
import argparse
import csv
import struct
import sys
import time
from multiprocessing import Pool
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
WORK = ROOT / 'hd' / 'work'
BETWEEN = ROOT / 'hd' / 'sprites' / '_between'
ENTRY = np.dtype([("a", "<u4"), ("b", "<u4"), ("off", "<u8"), ("size", "<u4"), ("w", "<u2"), ("h", "<u2"),
                  ("dxa", "<i2"), ("dya", "<i2"), ("dxb", "<i2"), ("dyb", "<i2")])
OUT = WORK / 'between_motion.npz'


def motion_file(store):
    return WORK / ('between_motion.npz' if store == '_between' else f'between_motion{store}.npz')


def read_csv():
    """frame -> (x, y) of its first record; (lo, hi) -> [ticks of hi coming from lo, ticks of lo coming from hi]."""
    offsets = {}
    anims = {}
    with open(WORK / 'frame_index.csv', newline='', encoding='utf-8', errors='replace') as f:
        r = csv.reader(f)
        next(r)
        for row in r:
            fr = int(row[0])
            offsets.setdefault(fr, (int(row[7]), int(row[8])))
            anims.setdefault((row[1], row[2], int(row[3])), []).append((int(row[4]), fr, int(row[5])))
    hold = {}

    def note(a, b, ticks_b):
        if a == b:
            return
        key = (min(a, b), max(a, b))
        slot = 0 if a < b else 1
        h = hold.setdefault(key, [0, 0])
        h[slot] = ticks_b if h[slot] == 0 else min(h[slot], ticks_b)

    for steps in anims.values():
        steps.sort()
        for i in range(len(steps)):
            a = steps[i][1]
            nb = steps[(i + 1) % len(steps)]          # the last step goes back to the first (looping animations)
            note(a, nb[1], min(255, nb[2] + 1))
    return offsets, hold


_bank = None
_offsets = None


def _init(offsets):
    global _bank, _offsets
    from extract_sprites import Bank
    _bank = Bank()
    _offsets = offsets


def _measure(job):
    import cv2
    from extract_sprites import KEY
    i, a, b = job
    try:
        oa, ob = _bank.decode16(a), _bank.decode16(b)
    except Exception:
        return i, -1.0, -1.0, -1.0
    xa, ya = _offsets.get(a, (0, 0))
    xb, yb = _offsets.get(b, (0, 0))
    x0 = min(-xa, -xb); y0 = min(-ya, -yb)
    W = max(-xa + oa.shape[1], -xb + ob.shape[1]) - x0
    H = max(-ya + oa.shape[0], -yb + ob.shape[0]) - y0
    if W <= 0 or H <= 0 or W * H > 600 * 600:
        return i, -1.0, -1.0, -1.0

    def place(px, x, y):
        g = np.zeros((H, W), np.float32)
        m = np.zeros((H, W), bool)
        r = ((px >> 11) & 31).astype(np.float32) * (255 / 31)
        gg = ((px >> 5) & 63).astype(np.float32) * (255 / 63)
        bb = (px & 31).astype(np.float32) * (255 / 31)
        lum = 0.299 * r + 0.587 * gg + 0.114 * bb
        fig = px != KEY
        lum[~fig] = 0.0
        g[y - y0:y - y0 + px.shape[0], x - x0:x - x0 + px.shape[1]] = lum
        m[y - y0:y - y0 + px.shape[0], x - x0:x - x0 + px.shape[1]] = fig
        return g, m

    ga, ma = place(oa, -xa, -ya)
    gb, mb = place(ob, -xb, -yb)
    fig = ma | mb
    if not fig.any():
        return i, -1.0, -1.0, -1.0
    changed = float(((np.abs(ga - gb) > 6) | (ma != mb))[fig].mean())
    big_a = cv2.resize(ga.astype(np.uint8), None, fx=4, fy=4, interpolation=cv2.INTER_CUBIC)
    big_b = cv2.resize(gb.astype(np.uint8), None, fx=4, fy=4, interpolation=cv2.INTER_CUBIC)
    if min(big_a.shape) < 12:
        # sparks, drops - smaller than DIS can work on (2026-11-22, the level animations): not measured
        return i, -1.0, -1.0, changed
    dis = cv2.DISOpticalFlow_create(cv2.DISOPTICAL_FLOW_PRESET_MEDIUM)
    fl = dis.calc(big_a, big_b, None)
    mag = np.hypot(fl[..., 0], fl[..., 1]) / 4.0
    fm = cv2.resize(fig.astype(np.uint8), (big_a.shape[1], big_a.shape[0]), interpolation=cv2.INTER_NEAREST) > 0
    p50, p90 = np.percentile(mag[fm], [50, 90])
    return i, float(p50), float(p90), changed


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--workers', type=int, default=6)
    ap.add_argument('--limit', type=int, default=0, help='only the first N usable pairs (a test)')
    ap.add_argument('--store', default='_between', help='folder under hd/sprites (the level animations: _between_anim)')
    args = ap.parse_args()
    global OUT
    OUT = motion_file(args.store)
    raw = (ROOT / 'hd' / 'sprites' / args.store / 'between.idx').read_bytes()
    count = struct.unpack_from('<I', raw, 8)[0]
    idx = np.frombuffer(raw, ENTRY, count, 16)
    t0 = time.time()
    offsets, hold = read_csv()
    print(f'frame index read: {len(offsets)} frames, {len(hold)} animation steps ({time.time() - t0:.0f} s)')
    hold_ab = np.zeros(count, np.uint8)
    hold_ba = np.zeros(count, np.uint8)
    for i in range(count):
        h = hold.get((int(idx['a'][i]), int(idx['b'][i])))
        if h:
            hold_ab[i], hold_ba[i] = h
    jobs = [(i, int(idx['a'][i]), int(idx['b'][i])) for i in range(count) if idx['size'][i] != 0]
    if args.limit:
        jobs = jobs[:args.limit]
    p50 = np.full(count, -1.0, np.float32)
    p90 = np.full(count, -1.0, np.float32)
    changed = np.full(count, -1.0, np.float32)
    t0 = time.time()
    with Pool(args.workers, initializer=_init, initargs=(offsets,)) as pool:
        for n, (i, a, b, c) in enumerate(pool.imap_unordered(_measure, jobs, chunksize=256)):
            p50[i], p90[i], changed[i] = a, b, c
            if n % 20000 == 0:
                print(f'  {n}/{len(jobs)} ({time.time() - t0:.0f} s)', flush=True)
    np.savez_compressed(OUT, p50=p50, p90=p90, changed=changed, hold_ab=hold_ab, hold_ba=hold_ba,
                        crc=np.uint32(__import__('zlib').crc32(raw) & 0xFFFFFFFF))
    used = p90 >= 0
    print(f'measured {used.sum()} pairs in {time.time() - t0:.0f} s -> {OUT}')
    for t in (0.5, 0.75, 1.0, 1.25, 1.5, 2.0):
        print(f'  flow p90 < {t:4.2f} px: {(p90[used] < t).sum():7d} ({100.0 * (p90[used] < t).mean():.1f} %)')
    known = (hold_ab > 0).sum() + (hold_ba > 0).sum()
    print(f'step lengths known: {known} of {2 * count} directions; ticks histogram (a->b):',
          np.bincount(hold_ab, minlength=8)[:8].tolist())


if __name__ == '__main__':
    main()
