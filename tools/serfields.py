#!/usr/bin/env python3
"""serfields.py - which fields a Game.exe Serialize method writes / reads.

  python tools/serfields.py <va-inside-serialize> [maxinsn]

The game's archive call is 0x005F52B0: __thiscall(archive, void* data, uint32 size), used as
  lea REG, [this + OFF] / push SIZE / push REG / mov ecx, archive / call 0x5f52b0
(order of the instructions varies). The script walks the function from its start (rh.py fstart)
and prints every (this-offset, size) pair it can pair with such a call, plus calls into other
serializers (calls that get the archive as their only argument), so a class's persistent state
can be listed without reading the whole function by hand.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rh  # noqa: E402

ARCHIVE_IO = 0x005F52B0


def main():
    va = int(sys.argv[1], 16)
    maxinsn = int(sys.argv[2]) if len(sys.argv) > 2 else 3000
    pe = rh.PE(rh.DEFAULT_EXE)
    start = rh.guess_func_start(pe, va) if hasattr(rh, 'guess_func_start') else None
    if start is None:
        start = va
    md = rh.md()
    import capstone
    if md is None:
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    code = pe.read(start, maxinsn * 8)
    lea = {}      # register -> (base, offset)
    pushes = []   # recent pushes (text)
    count = 0
    for ins in md.disasm(code, start):
        count += 1
        if count > maxinsn:
            break
        m = re.match(r'(e[a-d]x|e[sd]i|ebp), \[(e[a-d]x|e[sd]i|ebp|esp)(?: \+ (0x[0-9a-f]+|\d+))?\]$', ins.op_str)
        if ins.mnemonic == 'lea' and m:
            # a stack address (esp) is a local of the serializer, not a field of the object
            lea[m.group(1)] = (m.group(2), int(m.group(3), 0) if m.group(3) else 0)
            continue
        # any other write to a register forgets what it pointed at (2026-11-19: a stale lea made a stack
        # local look like the field +0x134, which really is a sequence pointer saved by 0x0058AD80)
        w = re.match(r'(e[a-d]x|e[sd]i|ebp),', ins.op_str)
        if w and ins.mnemonic not in ('cmp', 'test', 'push'):
            lea.pop(w.group(1), None)
        if ins.mnemonic == 'pop' and ins.op_str in lea:
            lea.pop(ins.op_str, None)
        if ins.mnemonic == 'push':
            pushes.append((ins.op_str, lea.get(ins.op_str)))   # what the register pointed at when pushed
            pushes = pushes[-4:]
            continue
        if ins.mnemonic == 'call':
            tgt = ins.op_str
            if tgt.startswith('0x') and int(tgt, 16) == ARCHIVE_IO and len(pushes) >= 2:
                (reg, where), size = pushes[-1], pushes[-2][0]
                if where:
                    print(f'{ins.address:08X}  field {where[0]}+0x{where[1]:X}  size {size}')
                else:
                    print(f'{ins.address:08X}  field ({reg})  size {size}')
            elif tgt.startswith('0x'):
                print(f'{ins.address:08X}  call {tgt}')
            pushes = []
            for r in ('eax', 'ecx', 'edx'):      # a call leaves these undefined
                lea.pop(r, None)
            continue
        if ins.mnemonic in ('ret', 'retn') and count > 20:
            # the function may have several returns; keep going a little past small ones
            pass


if __name__ == '__main__':
    main()
