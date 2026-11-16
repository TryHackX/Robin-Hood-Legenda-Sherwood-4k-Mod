"""Check the layout of the release archives: every entry must lie under the game folder as it is meant to
(top level = winmm.dll / SherwoodFix.ini / README.md / hd), with forward slashes (a name with a backslash is one file
for some unpackers), and the HD pack's archives together must hold the files the mod needs.

    python -I tools/check_archives.py [archives...]      (default: release/hdpack/*.zip, the newest mod and remake archives)
"""
import glob
import os
import sys
import zipfile

ALLOWED_TOP = {'winmm.dll', 'SherwoodFix.ini', 'README.md', 'hd'}


def main():
    root = os.path.normpath(os.path.join(os.path.dirname(__file__), '..'))
    paths = sys.argv[1:] or (sorted(glob.glob(os.path.join(root, 'release', 'hdpack', '*.zip'))) +
                             sorted(glob.glob(os.path.join(root, 'release', 'SherwoodEngineFix-v*.zip')))[-1:] +
                             sorted(glob.glob(os.path.join(root, 'release', 'SherwoodEngineFix-menu-remake-*.zip')))[-1:])
    names = set()
    ok = True
    for p in paths:
        z = zipfile.ZipFile(p)
        n = [x for x in z.namelist() if not x.endswith('/')]
        tops = sorted({x.split('/')[0] for x in n})
        backslash = [x for x in n if '\\' in x]
        wrong = [t for t in tops if t not in ALLOWED_TOP]
        print(f'{os.path.basename(p)}: {len(n)} files, top level {tops}'
              f'{", BACKSLASH NAMES %d" % len(backslash) if backslash else ""}{", UNEXPECTED %s" % wrong if wrong else ""}')
        ok &= not backslash and not wrong
        names.update(n)
    for need in ('hd/sprites/_common/sprites.idx', 'hd/sprites/_common/sprites.dat', 'hd/sprites/_common/sprites.dat.2',
                 'hd/sprites/_between/between.idx', 'hd/sprites/_between/between.dat'):
        if any(os.path.basename(p).startswith('SherwoodHD') for p in paths):
            print(f'  {need}: {"yes" if need in names else "MISSING"}')
            ok &= need in names
    maps = [x for x in names if x.startswith('hd/maps/dat2/') and x.endswith('.hdmap')]
    if any(os.path.basename(p).startswith('SherwoodHD-2') for p in paths):
        print(f'  hd/maps/dat2/*.hdmap: {len(maps)}')
    print('OK' if ok else 'PROBLEMS FOUND')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
