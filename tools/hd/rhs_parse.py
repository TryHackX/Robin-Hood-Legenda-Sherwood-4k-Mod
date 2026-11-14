"""Heuristic parser for DATA/**/*.rhs sprite descriptions (layout inferred from data, see docs/SPRITE_BANK.md).

  python tools/hd/rhs_parse.py               -> check every .rhs parses to EOF, print summary
  python tools/hd/rhs_parse.py FILE [--dump] -> profiles/animations/frame ids of one file
"""
import glob, os, struct, sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'DATA'))


def parse(path):
    d = open(path, 'rb').read()
    stamp, nprof = struct.unpack_from('<IH', d, 0); p = 6
    profiles = []
    for _ in range(nprof):
        name = d[p:p + 32].split(b'\0')[0].decode('latin1'); p += 32
        nanim, a, b, w, h = struct.unpack_from('<HHHII', d, p); p += 14
        anims = []
        for _ in range(nanim):
            nfr, u1, aw, ah, u2 = struct.unpack_from('<HHIIH', d, p); p += 14
            frames = [struct.unpack_from('<IHhhhH', d, p + 14 * k) for k in range(nfr)]; p += 14 * nfr
            anims.append(dict(n=nfr, u1=u1, w=aw, h=ah, u2=u2, frames=frames))
        profiles.append(dict(name=name, a=a, b=b, w=w, h=h, anims=anims))
    return stamp, profiles, p, len(d)


def write_index(out):
    """CSV: one row per frame reference (a bank frame can be referenced many times)."""
    import csv
    with open(out, 'w', newline='') as fo:
        wr = csv.writer(fo)
        wr.writerow(['frame', 'file', 'profile', 'anim', 'idx', 'flags', 's16', 'x', 'y', 'extra'])
        for f in sorted(glob.glob(os.path.join(ROOT, '**', '*.rhs'), recursive=True)):
            rel = os.path.relpath(f, ROOT).replace('\\', '/')
            for pr in parse(f)[1]:
                for k, a in enumerate(pr['anims']):
                    for j, (fid, fl, s16, x, y, ex) in enumerate(a['frames']):
                        wr.writerow([fid, rel, pr['name'], k, j, fl, s16, x, y, ex])


def main():
    if len(sys.argv) > 2 and sys.argv[1] == '--index':
        write_index(sys.argv[2]); return
    if len(sys.argv) > 1:
        stamp, profs, p, n = parse(sys.argv[1])
        print('stamp %d consumed %d/%d' % (stamp, p, n))
        for pr in profs:
            ids = [f[0] for a in pr['anims'] for f in a['frames']]
            print('%-24s anims %3d  a,b=%d,%d  canvas %dx%d  frames %d ids %d..%d' % (
                pr['name'], len(pr['anims']), pr['a'], pr['b'], pr['w'], pr['h'], len(ids), min(ids), max(ids)))
            if '--dump' in sys.argv:
                for k, a in enumerate(pr['anims']):
                    print('  anim %d n=%d u1=%d %dx%d u2=%d first %s' % (k, a['n'], a['u1'], a['w'], a['h'], a['u2'], a['frames'][:2]))
        return
    ok = bad = 0; allids = set(); nprof = 0
    for f in glob.glob(os.path.join(ROOT, '**', '*.rhs'), recursive=True):
        try:
            stamp, profs, p, n = parse(f)
            if p != n: raise ValueError('consumed %d of %d' % (p, n))
            ok += 1; nprof += len(profs)
            for pr in profs:
                for a in pr['anims']:
                    allids.update(fr[0] for fr in a['frames'])
        except Exception as e:
            bad += 1
            if bad < 6: print('FAIL', os.path.relpath(f, ROOT), e)
    print('parsed %d, failed %d, profiles %d, distinct frame ids %d (min %d max %d)' % (
        ok, bad, nprof, len(allids), min(allids), max(allids)))


if __name__ == '__main__':
    main()
