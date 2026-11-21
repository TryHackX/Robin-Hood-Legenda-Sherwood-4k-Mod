"""Quality of every in-between frame, for the game: hd\\sprites\\_between\\between.q

The generator (make_inbetweens.py) accepted frames whose two moved source frames differ by up to
32 of 255 on the figure ("colour"); its own notes: up to ~17 clean, 27-36 soft. In the game the soft
ones alternate with the sharp HD frames - the figure looked "now HD, now not", with ghosts in fast
moves (user report 2026-11-15, Robin's idle animation). The DLL now uses only frames up to a limit
(HDInBetweenQuality, default 17); this file holds the measurement per pair, in the order of
between.idx, so the store itself stays as it is.

    python -I tools/hd/between_quality.py            -> hd/sprites/_between/between.q  (+ a summary)
    python -I tools/hd/between_quality.py --store _between_anim    (the level animations' store)

between.q: "HDBQ", u32 version 1, u32 count (= entries of between.idx), u32 CRC-32 of between.idx,
           then count bytes: the colour difference rounded up (0..254), 255 = not measured.
Sharp store (make_inbetweens.py --sharp, 2026-11-15; flag 1 in the last word of the between.idx header): no frame
is a mix of the two any more, the soft look is gone - the value is scaled so that the generator's own limit (32)
lands on the DLL's default 17: with HDInBetweenQuality=17 every accepted frame is shown. Players with the old
store keep its file (between.v1.q, the DLL takes the one whose CRC matches).
2026-11-15 (v1.1.7): with hd/work/between_motion.npz (tools/hd/between_motion.py) also pairs without motion are left
out (q = 255, MIN_MOTION_P90), and between.t is written: "HDBS", u32 1, u32 count, u32 CRC as above, then count
bytes = ticks the frame b stays when the animation goes a -> b, count bytes = ticks of a going b -> a (0 = unknown).
"""
import csv
import glob
import math
import os
import struct
import sys
import zlib

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
DIR = os.path.join(ROOT, 'hd', 'sprites', '_between')
ENTRY = np.dtype([("a", "<u4"), ("b", "<u4"), ("off", "<u8"), ("size", "<u4"), ("w", "<u2"), ("h", "<u2"),
                  ("dxa", "<i2"), ("dya", "<i2"), ("dxb", "<i2"), ("dyb", "<i2")])
# 2026-11-15: pairs whose 1x frames move less than this (optical flow, 90th percentile over the figure, 1x pixels)
# show no motion - left out (q = 255). Robin's idle: 0.1-0.2 (a patchwork, "his clothes kept changing"), a slow
# head turn 0.8-1.1, the smallest steps of a walk 1.6-2.1. Needs hd/work/between_motion.npz (between_motion.py).
MIN_MOTION_P90 = 1.25


def main():
    global DIR
    store = sys.argv[sys.argv.index('--store') + 1] if '--store' in sys.argv else '_between'
    DIR = os.path.join(ROOT, 'hd', 'sprites', store)
    idx_path = os.path.join(DIR, 'between.idx')
    raw = open(idx_path, 'rb').read()
    magic, ver, count, sharp = struct.unpack_from('<4sIII', raw, 0)
    if magic != b'HDBT' or ver != 1:
        sys.exit('not an in-between index: ' + idx_path)
    scale = 17.0 / 32.0 if sharp == 1 else 1.0
    print('store made %s' % ('sharp: values scaled 17/32 (limit 32 -> 17)' if sharp == 1 else 'by mixing: values as measured'))
    entries = np.frombuffer(raw, ENTRY, count, 16)
    colour = {}
    for p in glob.glob(os.path.join(ROOT, 'hd', 'work', 'between_log*.csv')):
        with open(p, newline='') as f:
            for r in csv.DictReader(f):
                a, b = int(r['a']), int(r['b'])
                key = (min(a, b), max(a, b))
                c = float(r['colour'])
                colour[key] = min(c, colour.get(key, 1e9))
    q = np.full(count, 255, np.uint8)
    used = entries['size'] != 0
    missing = 0
    for i in range(count):
        c = colour.get((int(entries['a'][i]), int(entries['b'][i])))
        if c is None:
            missing += used[i]
            continue
        q[i] = min(254, int(math.ceil(c * scale - 1e-9)))
    h = zlib.crc32(raw) & 0xFFFFFFFF            # the index this file belongs to
    # motion and step lengths (between_motion.py), measured for exactly this index
    motion = None
    mpath = os.path.join(ROOT, 'hd', 'work', 'between_motion.npz' if store == '_between' else 'between_motion%s.npz' % store)
    if os.path.exists(mpath):
        m = np.load(mpath)
        if int(m['crc']) == h and len(m['p90']) == count:
            motion = m
        else:
            print('between_motion.npz belongs to another index - run tools/hd/between_motion.py again')
    if motion is not None:
        # no motion to show: the pair's in-between is only a patchwork of the two frames (see between_motion.py)
        still = used & (motion['p90'] >= 0) & (motion['p90'] < MIN_MOTION_P90)
        print('no motion (flow p90 < %.2f px): %d pairs left out' % (MIN_MOTION_P90, still.sum()))
        q[still] = 255
        with open(os.path.join(DIR, 'between.t'), 'wb') as f:
            f.write(struct.pack('<4sIII', b'HDBS', 1, count, h))
            f.write(motion['hold_ab'].astype(np.uint8).tobytes())
            f.write(motion['hold_ba'].astype(np.uint8).tobytes())
        print('->', os.path.join(DIR, 'between.t'), 16 + 2 * count, 'bytes (step lengths in ticks)')
    with open(os.path.join(DIR, 'between.q'), 'wb') as f:
        f.write(struct.pack('<4sIII', b'HDBQ', 1, count, h))
        f.write(q.tobytes())
    qu = q[used]
    print('pairs %d, usable %d, measured usable %d (missing %d)' % (count, used.sum(), (qu != 255).sum(), missing))
    for lim in (12, 15, 17, 20, 24, 28, 32):
        print('  limit %2d: %6d usable frames stay (%.0f%%)' % (lim, (qu <= lim).sum(), 100.0 * (qu <= lim).sum() / max(1, used.sum())))
    print('->', os.path.join(DIR, 'between.q'), 16 + count, 'bytes')


if __name__ == '__main__':
    main()
