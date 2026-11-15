"""Explore the game's bitmap font files (DATA/Interface/Fonts/*.bfn, *.fnt; "SBFONT").

    python -I tools/hd/bfn_explore.py [File.bfn]
"""
import os
import struct
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
FONTS = os.path.join(ROOT, 'DATA', 'Interface', 'Fonts')


def main():
    names = sys.argv[1:] or sorted(f for f in os.listdir(FONTS) if f.lower().endswith(('.bfn', '.fnt')))
    for name in names:
        d = open(os.path.join(FONTS, name), 'rb').read()
        print('==', name, len(d), 'bytes')
        print('  magic', d[:6], 'bytes 6..9', d[6:10].hex(), 'name', d[10:46].split(b'\0')[0])
        ints = struct.unpack_from('<12i', d, 46)
        print('  ints from 46:', ints)
        # look for the per-character table: (u16 code, u16/u32 x, ...) - print the bytes after the header
        print('  bytes 94..160:', d[94:160].hex(' '))


if __name__ == '__main__':
    main()
