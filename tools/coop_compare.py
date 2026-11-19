#!/usr/bin/env python3
"""coop_compare.py - compare two co-op probe runs tick by tick (src\\coop_probe.h).

  python tools/coop_compare.py NAME A B       e.g.  coop_compare.py c1 rec play1
      reads tools/out/harness/coop_NAME_A_hash.txt / _dump.txt and the same for B

Prints how many ticks both runs have, the first tick where the state hash of all elements, of the
humans only, the rand() generator state or the rand() call counts differ, and - from the element
dumps (every 50 ticks) - which elements differ first and in which fields.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
HARNESS = os.path.join(HERE, 'out', 'harness')

# the human fields of a dump line, in the order of kHumanFields in src\coop_probe.cpp (since 2026-11-19 01:00)
HUMAN_FIELDS = [
    (0x10C, 4), (0x110, 1), (0x114, 4), (0x118, 1), (0x119, 1), (0x124, 4), (0x13C, 1), (0x140, 4), (0x144, 4),
    (0x148, 1), (0x149, 1), (0x14C, 4), (0x160, 4), (0x164, 1), (0x165, 1), (0x16C, 2), (0x16E, 1), (0x16F, 1),
    (0x1A4, 1), (0x1C8, 2), (0x1CA, 2), (0x1CC, 2), (0x1CE, 1), (0x1D0, 8), (0x1DC, 0x10), (0x230, 4), (0x234, 4),
    (0x238, 4), (0x23C, 2), (0x23E, 1), (0x23F, 1), (0x240, 2), (0x288, 1), (0x289, 1), (0x28A, 2), (0x28C, 4),
    (0x47C, 1), (0x480, 4), (0x484, 4), (0x488, 2), (0x48A, 1), (0x48C, 4), (0x490, 1), (0x492, 2), (0x494, 2),
    (0x496, 1), (0x497, 1),
]
# drawn-frame effects kept in human fields: the selection fade +0x480/+0x488/+0x48A/+0x48C is stepped by the
# logic (0x0047A8A0) AND by every draw of the character (PC 0x00491645, NPC 0x00486AEF..) - extra frames change it
COSMETIC = {0x480, 0x488, 0x48A, 0x48C}


def parse_element(line):
    """id -> dict of comparable values (simple fields + human fields by offset)"""
    p = line.split()
    out = {}
    hexs = ''
    for tok in p[1:]:
        if '=' in tok:
            k, v = tok.split('=', 1)
            if k != 'h':
                out[k] = v
        elif tok.count('.') > 10:
            hexs = tok
    if hexs:
        for (off, size), val in zip(HUMAN_FIELDS, hexs.split('.')):
            out[f'+0x{off:X}'] = val
    return out


def load_hash(path):
    rows = {}
    with open(path, 'r', encoding='latin1') as f:
        for line in f:
            if line.startswith('#'):
                continue
            p = line.split()
            if len(p) < 4:
                continue
            tick = int(p[0])
            kv = {}
            for tok in p[4:]:
                if '=' in tok:
                    k, v = tok.split('=', 1)
                    kv[k] = v
            rows[tick] = {'all': p[1], 'humans': p[2], 'holdrand': p[3], **kv}
    return rows


def load_dump(path):
    dumps = {}
    if not os.path.exists(path):
        return dumps
    cur = None
    with open(path, 'r', encoding='latin1') as f:
        for line in f:
            if line.startswith('== tick'):
                cur = int(line.split()[2])
                dumps[cur] = {}
                continue
            if cur is None:
                continue
            p = line.split()
            ident = next((t for t in p if t.startswith('id=')), None)
            if ident:
                dumps[cur][ident] = line.rstrip('\n')
    return dumps


def first_diff(a, b, key, ticks):
    for t in ticks:
        if a[t].get(key) != b[t].get(key):
            return t
    return None


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return
    name, ta, tb = sys.argv[1:4]
    base = os.path.join(HARNESS, f'coop_{name}_')
    a, b = load_hash(base + ta + '_hash.txt'), load_hash(base + tb + '_hash.txt')
    ticks = sorted(set(a) & set(b))
    print(f'{ta}: {len(a)} ticks, {tb}: {len(b)} ticks, common: {len(ticks)} (0..{ticks[-1] if ticks else -1})')
    if not ticks:
        return
    result = {}
    for key in ('all', 'humans', 'holdrand', 'logic', 'render', 'sound', 'soundcode', 'ev', 'n', 'ctr'):
        result[key] = first_diff(a, b, key, ticks)
        t = result[key]
        if t is None:
            print(f'  {key:9s}: identical in all {len(ticks)} common ticks')
        else:
            print(f'  {key:9s}: first differs at tick {t}: {a[t].get(key)} vs {b[t].get(key)}')
    da, db = load_dump(base + ta + '_dump.txt'), load_dump(base + tb + '_dump.txt')
    common = sorted(set(da) & set(db))
    for label, ignore in (('all fields', set()), ('without the drawn-frame fade', COSMETIC)):
        first = None
        cosmetic_only = 0
        for t in common:
            ea, eb = da[t], db[t]
            diffs = []
            for ident in sorted(set(ea) | set(eb), key=lambda s: int(s[3:])):
                if ident not in ea or ident not in eb:
                    diffs.append((ident, ['missing']))
                    continue
                pa, pb = parse_element(ea[ident]), parse_element(eb[ident])
                keys = [k for k in pa if pa.get(k) != pb.get(k) and not (k.startswith('+0x') and int(k[1:], 16) in ignore)]
                if keys:
                    diffs.append((ident, [f'{k}: {pa.get(k)} -> {pb.get(k)}' for k in keys]))
            if diffs:
                first = (t, diffs)
                break
        if first is None:
            print(f'\nelement dumps ({label}): identical at all {len(common)} common dump ticks'
                  + (f' ({common[0]}..{common[-1]})' if common else ''))
        else:
            t, diffs = first
            print(f'\nelement dumps ({label}): first difference at tick {t}, {len(diffs)} elements:')
            for ident, keys in diffs[:10]:
                print(f'  {ident}: ' + '; '.join(keys)[:300])
    # rand() call logs: "tick ctx caller value", ctx lower case = shared generator, upper case = isolated
    ra, rb = base + ta + '_rand.txt', base + tb + '_rand.txt'
    if os.path.exists(ra) and os.path.exists(rb):
        last = min(ticks[-1], max(ticks))

        def shared(path):
            rows = []
            with open(path, 'r', encoding='latin1') as f:
                for line in f:
                    p = line.split()
                    if len(p) == 4 and p[1].islower() and int(p[0]) <= last:
                        rows.append(p)
            return rows
        sa, sb = shared(ra), shared(rb)
        n = min(len(sa), len(sb))
        k = next((i for i in range(n) if sa[i] != sb[i]), None)
        if k is None:
            print(f'\nshared-generator rand() calls: {len(sa)} vs {len(sb)} up to tick {last}, identical over the first {n}')
        else:
            print(f'\nshared-generator rand() calls part at call #{k} (tick {sa[k][0]} vs {sb[k][0]}):')
            for i in range(max(0, k - 6), min(n, k + 6)):
                mark = '>>' if i >= k and sa[i] != sb[i] else '  '
                print(f'  {mark} {ta}: {" ".join(sa[i]):28s}   {tb}: {" ".join(sb[i])}')
    # RHOrder creations: "tick ctx kind callers..."
    oa, ob = base + ta + '_orders.txt', base + tb + '_orders.txt'
    if os.path.exists(oa) and os.path.exists(ob):
        last = ticks[-1]

        def orders(path):
            with open(path, 'r', encoding='latin1') as f:
                return [line.split() for line in f if line.split() and int(line.split()[0]) <= last]
        xa, xb = orders(oa), orders(ob)
        n = min(len(xa), len(xb))
        k = next((i for i in range(n) if xa[i] != xb[i]), None)
        if k is None:
            print(f'\nRHOrder creations: {len(xa)} vs {len(xb)} up to tick {last}, identical over the first {n}')
        else:
            print(f'\nRHOrder creations part at #{k} (tick {xa[k][0]} vs {xb[k][0]}):')
            for i in range(max(0, k - 4), min(n, k + 5)):
                mark = '>>' if i >= k and xa[i] != xb[i] else '  '
                print(f'  {mark} {ta}: {" ".join(xa[i]):40s}   {tb}: {" ".join(xb[i])}')


if __name__ == '__main__':
    main()
