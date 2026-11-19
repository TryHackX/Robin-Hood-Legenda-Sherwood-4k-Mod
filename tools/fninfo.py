#!/usr/bin/env python3
"""fninfo.py - prologue bytes and return forms of Game.exe functions (for hooks).

  python tools/fninfo.py <va> [<va> ...]

For each function: the first 16 bytes, the first instructions, and every distinct `ret N`
found by a linear walk until the next padding (0x90/0xCC run) after a ret.
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rh

pe = rh.PE(rh.DEFAULT_EXE)
md = rh.md()
for a in sys.argv[1:]:
    va = int(a, 16)
    b = pe.read(va, 16)
    rets = set()
    last_ret = None
    code = pe.read(va, 0x4000)
    for ins in md.disasm(code, va):
        if ins.mnemonic == 'ret':
            rets.add(ins.op_str or '0')
            last_ret = ins.address + ins.size
            nxt = pe.read(last_ret, 2)
            if nxt in (b'\x90\x90', b'\xcc\xcc'):
                break
        if ins.address - va > 0x3F00:
            break
    first = [f'{i.mnemonic} {i.op_str}' for i in md.disasm(pe.read(va, 24), va)][:4]
    print(f'{va:08X}  {b.hex(" ")}  ret={sorted(rets)}  | ' + ' ; '.join(first))
