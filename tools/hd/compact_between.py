"""Only the in-between frames the game shows (2026-11-22, before release 1.2.9).

make_inbetweens.py keeps every pair it tried: the rejected ones as entries without data, the accepted ones that
between_quality.py later leaves out (no motion: q 255) with their data. In the level animations' store that is more
than half of the data file (278 MB, 131 MB shown); in the second try's store 125 000 of its 137 000 entries are
pairs it rejected again. Compacted:

  - between.dat: only the frames shown at the DLL's default limit (size != 0 and q <= --limit, 17), in the index's order;
  - between.idx: with --keep-all every entry stays, the left-out ones without data (the level animations: the DLL lets
    a level sprite glide only on a step its store knows - also one without a frame, AnimateLevelSprite); otherwise only
    the shown entries (the second try: its pairs are steps of the characters' store, which knows them all);
  - between.q / between.t: the entries kept, under the new index's CRC; hd\\work\\between_motion<store>.npz as well,
    so between_quality.py still works on the compacted store.

The old files go to hd\\work\\between_full\\<store>\\ - delete them when the game has been checked with the new ones.

    python -I tools/hd/compact_between.py --store _between_anim --keep-all
    python -I tools/hd/compact_between.py --store _between_more
"""
import argparse
import shutil
import struct
import sys
import zlib
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
ENTRY = np.dtype([("a", "<u4"), ("b", "<u4"), ("off", "<u8"), ("size", "<u4"), ("w", "<u2"), ("h", "<u2"),
                  ("dxa", "<i2"), ("dya", "<i2"), ("dxb", "<i2"), ("dyb", "<i2")])
FILES = ('between.idx', 'between.dat', 'between.q', 'between.t')


def read_side(path, magic, count, crc, per_entry):
    """between.q (1 byte per entry) or between.t (2) - only for exactly this index."""
    raw = path.read_bytes()
    m, v, c, h = struct.unpack_from('<4sIII', raw, 0)
    if m != magic or v != 1 or c != count or h != crc or len(raw) != 16 + per_entry * count:
        sys.exit(f'{path} does not belong to this index - run between_motion.py and between_quality.py first')
    return np.frombuffer(raw, np.uint8, per_entry * count, 16)


def motion_file(store):
    return ROOT / 'hd' / 'work' / ('between_motion.npz' if store == '_between' else f'between_motion{store}.npz')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--store', required=True, help='folder under hd/sprites (_between_anim, _between_more)')
    ap.add_argument('--keep-all', action='store_true', help='keep every entry of the index (the left-out ones without data)')
    ap.add_argument('--limit', type=int, default=17, help='frames up to this quality value stay (HDInBetweenQuality default)')
    args = ap.parse_args()
    if args.store == '_between':
        sys.exit("the characters' store comes with HD pack 1 - the mod's between.q / between.t belong to its index as it is")
    d = ROOT / 'hd' / 'sprites' / args.store
    if any(d.glob('between.dat.*')):
        sys.exit('the data file is in parts - not handled here')
    raw = (d / 'between.idx').read_bytes()
    magic, ver, count, sharp = struct.unpack_from('<4sIII', raw, 0)
    if magic != b'HDBT' or ver != 1 or len(raw) != 16 + ENTRY.itemsize * count:
        sys.exit(f'not an in-between index: {d / "between.idx"}')
    crc = zlib.crc32(raw) & 0xFFFFFFFF
    e = np.frombuffer(raw, ENTRY, count, 16)
    q = read_side(d / 'between.q', b'HDBQ', count, crc, 1)
    t = read_side(d / 'between.t', b'HDBS', count, crc, 2).reshape(2, count)      # ticks a -> b, ticks b -> a
    shown = (e['size'] != 0) & (q <= args.limit)
    keep = np.ones(count, bool) if args.keep_all else shown
    order = np.nonzero(keep)[0]

    full = ROOT / 'hd' / 'work' / 'between_full' / args.store
    if full.exists() and any(full.iterdir()):
        sys.exit(f'{full} still holds an earlier full store - check the game with the compacted one and delete it first')
    full.mkdir(parents=True, exist_ok=True)

    ne = e[order].copy()
    sums = {}
    new_dat = d / 'between.dat.new'
    with open(d / 'between.dat', 'rb') as src, open(new_dat, 'wb') as out:
        out.write(b'HDBD' + struct.pack('<III', 1, 0, 0))
        for k, i in enumerate(order):
            if not shown[i]:
                ne['off'][k] = 0
                ne['size'][k] = 0
                continue
            src.seek(int(e['off'][i]))
            data = src.read(int(e['size'][i]))
            if len(data) != int(e['size'][i]):
                new_dat.unlink()
                sys.exit(f'pair {e["a"][i]}-{e["b"][i]} is cut short in the data file - nothing changed')
            ne['off'][k] = out.tell()
            sums[k] = zlib.crc32(data)
            out.write(data)
    # read back: every frame where the new index says, with the same bytes
    with open(new_dat, 'rb') as f:
        for k, s in sums.items():
            f.seek(int(ne['off'][k]))
            if zlib.crc32(f.read(int(ne['size'][k]))) != s:
                new_dat.unlink()
                sys.exit(f'entry {k}: the copied frame differs - nothing changed')

    new_idx = b'HDBT' + struct.pack('<III', 1, len(ne), sharp) + ne.tobytes()
    ncrc = zlib.crc32(new_idx) & 0xFFFFFFFF
    new_q = struct.pack('<4sIII', b'HDBQ', 1, len(ne), ncrc) + q[order].tobytes()
    new_t = struct.pack('<4sIII', b'HDBS', 1, len(ne), ncrc) + np.ascontiguousarray(t[:, order]).tobytes()
    motion = None
    mp = motion_file(args.store)
    if mp.exists():
        with np.load(mp) as m:
            if int(m['crc']) == crc and len(m['p90']) == count:
                motion = {k: m[k] for k in m.files}
    for n in FILES:
        shutil.move(str(d / n), str(full / n))
    new_dat.replace(d / 'between.dat')
    (d / 'between.idx').write_bytes(new_idx)
    (d / 'between.q').write_bytes(new_q)
    (d / 'between.t').write_bytes(new_t)
    if motion is not None:
        shutil.copy2(mp, full / mp.name)
        np.savez_compressed(mp, **{k: (v[order] if k != 'crc' else np.uint32(ncrc)) for k, v in motion.items()})

    old_mb = (full / 'between.dat').stat().st_size / 1e6
    new_mb = (d / 'between.dat').stat().st_size / 1e6
    print(f'{args.store}: {count} entries -> {len(ne)} ({int(shown.sum())} with a frame shown at limit {args.limit}); '
          f'data {old_mb:.1f} MB -> {new_mb:.1f} MB, index {len(raw) / 1e6:.2f} MB -> {len(new_idx) / 1e6:.2f} MB'
          f'{"" if motion is not None else " (no motion file of this index - not carried over)"}')
    print(f'old files: {full}')


if __name__ == '__main__':
    main()
