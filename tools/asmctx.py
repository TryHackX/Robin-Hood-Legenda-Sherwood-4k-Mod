#!/usr/bin/env python3
"""asmctx.py - print the linear listing tools/out/Game.exe.asm around an address.

  python tools/asmctx.py <va> [before] [after]     (defaults: 20 lines before, 40 after)
  python tools/asmctx.py <va> --func              from the nearest function start above <va>
                                                  (padding / int3 / ret before a push or sub) to its ret

The listing is linear, so it can be misaligned right after data in .text; the nearest listed
instruction at or below <va> is used.
"""
import bisect
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
LISTING = os.path.join(HERE, 'out', 'Game.exe.asm')

_cache = None


def load():
    global _cache
    if _cache is None:
        addrs, lines = [], []
        with open(LISTING, 'r', encoding='latin1') as f:
            for line in f:
                m = re.match(r'^([0-9A-F]{8}):', line)
                if m:
                    addrs.append(int(m.group(1), 16))
                    lines.append(line.rstrip('\n'))
        _cache = (addrs, lines)
    return _cache


def index_of(va):
    addrs, _ = load()
    i = bisect.bisect_right(addrs, va) - 1
    return max(i, 0)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    va = int(sys.argv[1], 16)
    addrs, lines = load()
    i = index_of(va)
    if '--func' in sys.argv:
        j = i
        while j > 0:
            prev = lines[j - 1]
            if re.search(r': (ret|int3|nop)\b', prev) or re.search(r': ret ', prev):
                break
            j -= 1
        k = i
        while k < len(lines) and not re.search(r': ret\b', lines[k]):
            k += 1
        for line in lines[j:k + 1]:
            print(line)
        return
    before = int(sys.argv[2]) if len(sys.argv) > 2 else 20
    after = int(sys.argv[3]) if len(sys.argv) > 3 else 40
    for line in lines[max(0, i - before):i + after]:
        print(line)


if __name__ == '__main__':
    main()
