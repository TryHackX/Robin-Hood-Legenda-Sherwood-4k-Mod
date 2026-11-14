#!/usr/bin/env python3
"""rh.py - static analysis helper for Game.exe (Robin Hood: The Legend of Sherwood, GOG).

Usage (always run from anywhere, paths are resolved relative to this file):
  python tools/rh.py imports [dllfilter]          - import table (IAT slot VA -> dll!name)
  python tools/rh.py sections                     - PE sections
  python tools/rh.py dis <va> [count]             - disassemble <count> instructions (default 40)
  python tools/rh.py func <va> [maxinsn]          - disassemble a whole function (until padding after ret)
  python tools/rh.py xrefs <va>                   - call/jmp rel32 + imm32/disp32 references to <va>
  python tools/rh.py callers <va>                 - only E8 call sites targeting <va> (with enclosing function start)
  python tools/rh.py vcalls <start> <end>         - indirect calls `call [reg+disp]` in a VA range
  python tools/rh.py str <regex>                  - search ASCII strings (regex, case-insensitive)
  python tools/rh.py wstr <regex>                 - search UTF-16LE strings
  python tools/rh.py bytes <va> [n]               - hex dump
  python tools/rh.py find <hex pattern with ??>   - byte pattern search in all sections
  python tools/rh.py fstart <va>                  - guess start of function containing <va>
  python tools/rh.py iatcalls <dll!name substr>   - all `call [IAT]` sites of an import
  python tools/rh.py dump <out.asm>               - whole-image linear disassembly to a file (then grep it)
Option:  --exe <path>  to analyse another PE (e.g. ddraw.dll).
"""
import os
import re
import struct
import sys

import capstone

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_EXE = os.path.join(os.path.dirname(HERE), "Game.exe")


class PE:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        d = self.data
        self.lfanew = struct.unpack_from("<I", d, 0x3C)[0]
        nsec = struct.unpack_from("<H", d, self.lfanew + 6)[0]
        optsize = struct.unpack_from("<H", d, self.lfanew + 20)[0]
        self.opt = self.lfanew + 24
        self.base = struct.unpack_from("<I", d, self.opt + 28)[0]
        self.entry = struct.unpack_from("<I", d, self.opt + 16)[0] + self.base
        self.sections = []
        so = self.opt + optsize
        for i in range(nsec):
            o = so + i * 40
            name = d[o:o + 8].rstrip(b"\0").decode("latin1")
            vsize, vaddr, rsize, raddr = struct.unpack_from("<IIII", d, o + 8)
            chars = struct.unpack_from("<I", d, o + 36)[0]
            self.sections.append((name, vaddr, vsize, raddr, rsize, chars))
        self._imports = None

    def va2off(self, va):
        rva = va - self.base
        for name, vaddr, vsize, raddr, rsize, _ in self.sections:
            if vaddr <= rva < vaddr + max(vsize, rsize):
                o = rva - vaddr
                if o >= rsize:
                    return None  # uninitialised data
                return raddr + o
        return None

    def off2va(self, off):
        for name, vaddr, vsize, raddr, rsize, _ in self.sections:
            if raddr <= off < raddr + rsize:
                return self.base + vaddr + (off - raddr)
        return None

    def read(self, va, n):
        o = self.va2off(va)
        if o is None:
            return b""
        return self.data[o:o + n]

    def code_sections(self):
        for name, vaddr, vsize, raddr, rsize, chars in self.sections:
            if chars & 0x20000000:  # MEM_EXECUTE
                yield name, self.base + vaddr, raddr, min(vsize, rsize) if vsize else rsize

    def cstr(self, va, maxlen=256):
        b = self.read(va, maxlen)
        i = b.find(b"\0")
        return b[:i if i >= 0 else maxlen].decode("latin1", "replace")

    def imports(self):
        if self._imports is not None:
            return self._imports
        d = self.data
        imp_rva, imp_size = struct.unpack_from("<II", d, self.opt + 96 + 8)
        res = {}
        if imp_rva:
            o = self.va2off(self.base + imp_rva)
            while True:
                oft, _, _, name_rva, ft = struct.unpack_from("<IIIII", d, o)
                if not name_rva and not ft:
                    break
                dll = self.cstr(self.base + name_rva)
                thunk = oft or ft
                i = 0
                while True:
                    to = self.va2off(self.base + thunk + i * 4)
                    v = struct.unpack_from("<I", d, to)[0]
                    if v == 0:
                        break
                    if v & 0x80000000:
                        nm = "#%d" % (v & 0xFFFF)
                    else:
                        nm = self.cstr(self.base + v + 2)
                    res[self.base + ft + i * 4] = "%s!%s" % (dll, nm)
                    i += 1
                o += 20
        self._imports = res
        return res


def md():
    m = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    return m


def annotate(pe, ins):
    """Add import / string annotations for an instruction."""
    out = ""
    imps = pe.imports()
    for m in re.finditer(r"0x([0-9a-f]{5,8})", ins.op_str):
        v = int(m.group(1), 16)
        if v in imps:
            out += "  ; " + imps[v]
        elif pe.base <= v < pe.base + 0x400000:
            o = pe.va2off(v)
            if o is not None:
                raw = pe.data[o:o + 48]
                m2 = re.match(rb"[\x20-\x7e]{4,}", raw)
                if m2 and not (ins.mnemonic in ("call", "jmp") or ins.mnemonic.startswith("j")):
                    out += '  ; "%s"' % m2.group(0).decode("latin1")
    return out


def cmd_dis(pe, va, count=40):
    buf = pe.read(va, count * 16)
    n = 0
    for ins in md().disasm(buf, va):
        print("%08X: %-20s %s %s%s" % (ins.address, ins.bytes.hex(), ins.mnemonic, ins.op_str, annotate(pe, ins)))
        n += 1
        if n >= count:
            break


def cmd_func(pe, va, maxinsn=4000):
    buf = pe.read(va, maxinsn * 8)
    n = 0
    max_target = va
    for ins in md().disasm(buf, va):
        print("%08X: %-20s %s %s%s" % (ins.address, ins.bytes.hex(), ins.mnemonic, ins.op_str, annotate(pe, ins)))
        n += 1
        # track forward jump targets so we do not stop at an early ret
        if ins.mnemonic.startswith("j") and ins.op_str.startswith("0x"):
            try:
                t = int(ins.op_str, 16)
                if va <= t < va + 0x8000 and t > max_target:
                    max_target = t
            except ValueError:
                pass
        if ins.mnemonic in ("ret", "retn") or (ins.mnemonic == "jmp" and not ins.op_str.startswith("0x")):
            if ins.address >= max_target:
                nxt = pe.read(ins.address + ins.size, 1)
                if nxt in (b"\x90", b"\xcc", b""):
                    break
                # next function may start immediately without padding
                nb = pe.read(ins.address + ins.size, 3)
                if nb[:1] in (b"\x55", b"\x83", b"\x81", b"\x6a", b"\x64", b"\x8b", b"\x53", b"\x56", b"\x51", b"\xa1", b"\xb8"):
                    if ins.address + ins.size > max_target:
                        break
        if n >= maxinsn:
            print("-- maxinsn reached --")
            break


def iter_code(pe):
    for name, sva, raddr, size in pe.code_sections():
        yield sva, pe.data[raddr:raddr + size]


def find_rel_refs(pe, target, opcodes=(0xE8, 0xE9)):
    res = []
    for sva, blob in iter_code(pe):
        for op in opcodes:
            pos = 0
            needle = bytes([op])
            while True:
                i = blob.find(needle, pos)
                if i < 0 or i + 5 > len(blob):
                    break
                rel = struct.unpack_from("<i", blob, i + 1)[0]
                if (sva + i + 5 + rel) & 0xFFFFFFFF == target:
                    res.append((sva + i, "call" if op == 0xE8 else "jmp"))
                pos = i + 1
    return sorted(res)


def find_imm_refs(pe, target):
    needle = struct.pack("<I", target)
    res = []
    for name, vaddr, vsize, raddr, rsize, chars in pe.sections:
        blob = pe.data[raddr:raddr + rsize]
        pos = 0
        while True:
            i = blob.find(needle, pos)
            if i < 0:
                break
            res.append((pe.base + vaddr + i, name))
            pos = i + 1
    return res


def guess_func_start(pe, va, maxback=0x4000):
    """Walk back to the nearest padding (CC/90 run or ret) followed by a plausible prologue."""
    o = pe.va2off(va)
    if o is None:
        return None
    d = pe.data
    for back in range(0, maxback):
        p = o - back
        if p <= 0:
            break
        # function start heuristics: preceded by CC/90/C3/C2 xx xx and aligned to 16 or follows padding
        prev = d[p - 1]
        if prev in (0xCC, 0x90) and d[p] not in (0xCC, 0x90):
            return pe.off2va(p)
        if (pe.off2va(p) & 0xF) == 0 and (prev == 0xC3 or (p >= 3 and d[p - 3] == 0xC2)):
            return pe.off2va(p)
    return None


def cmd_xrefs(pe, target):
    for a, kind in find_rel_refs(pe, target):
        fs = guess_func_start(pe, a)
        print("%08X: %s 0x%08X   (in func ~%s)" % (a, kind, target, ("%08X" % fs) if fs else "?"))
    for a, sec in find_imm_refs(pe, target):
        ctx = pe.read(a - 4, 12).hex()
        print("%08X: imm/disp32 ref in %s  ctx=%s" % (a, sec, ctx))


def cmd_callers(pe, target):
    for a, kind in find_rel_refs(pe, target, opcodes=(0xE8,)):
        fs = guess_func_start(pe, a)
        print("%08X: call 0x%08X   (in func ~%s)" % (a, target, ("%08X" % fs) if fs else "?"))


def cmd_vcalls(pe, start, end):
    buf = pe.read(start, end - start)
    imps = pe.imports()
    for ins in md().disasm(buf, start):
        if ins.mnemonic == "call" and "[" in ins.op_str:
            print("%08X: call %s%s" % (ins.address, ins.op_str, annotate(pe, ins)))


def cmd_str(pe, rx, wide=False):
    r = re.compile(rx.encode("latin1"), re.I)
    for name, vaddr, vsize, raddr, rsize, chars in pe.sections:
        blob = pe.data[raddr:raddr + rsize]
        if wide:
            it = re.finditer(rb"(?:[\x20-\x7e]\x00){4,}", blob)
        else:
            it = re.finditer(rb"[\x20-\x7e]{4,}", blob)
        for m in it:
            s = m.group(0)
            if wide:
                s = s[::2]
            if r.search(s):
                print("%08X [%s]: %s" % (pe.base + vaddr + m.start(), name, s.decode("latin1")))


def cmd_bytes(pe, va, n=64):
    b = pe.read(va, n)
    for i in range(0, len(b), 16):
        chunk = b[i:i + 16]
        print("%08X: %-48s %s" % (va + i, chunk.hex(" "), "".join(chr(c) if 32 <= c < 127 else "." for c in chunk)))


def cmd_find(pe, pat):
    toks = pat.split()
    rx = b"".join(b"." if t in ("?", "??") else re.escape(bytes([int(t, 16)])) for t in toks)
    r = re.compile(rx, re.S)
    for name, vaddr, vsize, raddr, rsize, chars in pe.sections:
        blob = pe.data[raddr:raddr + rsize]
        for m in r.finditer(blob):
            print("%08X [%s]" % (pe.base + vaddr + m.start(), name))


def cmd_iatcalls(pe, sub):
    imps = pe.imports()
    slots = {va: nm for va, nm in imps.items() if sub.lower() in nm.lower()}
    for va, nm in sorted(slots.items()):
        needle_call = b"\xff\x15" + struct.pack("<I", va)
        needle_jmp = b"\xff\x25" + struct.pack("<I", va)
        hits = []
        for sva, blob in iter_code(pe):
            for needle, kind in ((needle_call, "call"), (needle_jmp, "jmp")):
                pos = 0
                while True:
                    i = blob.find(needle, pos)
                    if i < 0:
                        break
                    hits.append((sva + i, kind))
                    pos = i + 1
        # also mov reg,[IAT] (8B /r disp32) patterns
        needle = struct.pack("<I", va)
        indirect = 0
        for sva, blob in iter_code(pe):
            indirect += blob.count(needle)
        print("%08X %s: %d direct sites, %d total refs" % (va, nm, len(hits), indirect))
        for a, kind in sorted(hits)[:60]:
            fs = guess_func_start(pe, a)
            print("    %08X: %s   (in func ~%s)" % (a, kind, ("%08X" % fs) if fs else "?"))


def cmd_dump(pe, out_path):
    """Linear-sweep disassembly of all code sections into a text file (grep it afterwards).
    Bytes capstone cannot decode (jump tables, padding data) are emitted as `db` and skipped."""
    m = md()
    n = 0
    with open(out_path, "w", encoding="ascii", errors="replace") as f:
        for name, sva, raddr, size in pe.code_sections():
            blob = pe.data[raddr:raddr + size]
            off = 0
            while off < size:
                last = off
                for ins in m.disasm(blob[off:], sva + off):
                    f.write("%08X: %s %s\n" % (ins.address, ins.mnemonic, ins.op_str))
                    last = ins.address - sva + ins.size
                    n += 1
                if last >= size:
                    break
                f.write("%08X: db 0x%02x\n" % (sva + last, blob[last]))
                off = last + 1
    print("wrote %d instructions to %s" % (n, out_path))


def main():
    args = sys.argv[1:]
    exe = DEFAULT_EXE
    if "--exe" in args:
        i = args.index("--exe")
        exe = args[i + 1]
        del args[i:i + 2]
    if not args:
        print(__doc__)
        return
    pe = PE(exe)
    c = args[0]
    h = lambda s: int(s, 16)
    if c == "imports":
        flt = args[1].lower() if len(args) > 1 else ""
        for va, nm in sorted(pe.imports().items()):
            if flt in nm.lower():
                print("%08X %s" % (va, nm))
    elif c == "sections":
        print("base=%08X entry=%08X" % (pe.base, pe.entry))
        for name, vaddr, vsize, raddr, rsize, chars in pe.sections:
            print("%-8s va=%08X vsize=%08X raw=%08X rsize=%08X chars=%08X" % (name, pe.base + vaddr, vsize, raddr, rsize, chars))
    elif c == "dis":
        cmd_dis(pe, h(args[1]), int(args[2]) if len(args) > 2 else 40)
    elif c == "func":
        cmd_func(pe, h(args[1]), int(args[2]) if len(args) > 2 else 4000)
    elif c == "xrefs":
        cmd_xrefs(pe, h(args[1]))
    elif c == "callers":
        cmd_callers(pe, h(args[1]))
    elif c == "vcalls":
        cmd_vcalls(pe, h(args[1]), h(args[2]))
    elif c == "str":
        cmd_str(pe, args[1])
    elif c == "wstr":
        cmd_str(pe, args[1], wide=True)
    elif c == "bytes":
        cmd_bytes(pe, h(args[1]), int(args[2]) if len(args) > 2 else 64)
    elif c == "find":
        cmd_find(pe, " ".join(args[1:]))
    elif c == "fstart":
        print("%08X" % (guess_func_start(pe, h(args[1])) or 0))
    elif c == "iatcalls":
        cmd_iatcalls(pe, args[1])
    elif c == "dump":
        cmd_dump(pe, args[1])
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
