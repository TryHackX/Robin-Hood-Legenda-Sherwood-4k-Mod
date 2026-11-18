"""Search the memory of the running Game.exe (the hidden test instance) for a byte pattern.

    python -I tools/memscan.py HEXBYTES [--pid N] [--max 40]
    e.g. the key bindings as 32-bit values:  python -I tools/memscan.py 4e0000004a000000c8000000d0000000

Prints the addresses where the pattern lies (committed, readable private / image memory) and the 64 bytes there.
Read-only (ReadProcessMemory).
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import subprocess

PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010
MEM_COMMIT = 0x1000
PAGE_NOACCESS = 0x01
PAGE_GUARD = 0x100


class MBI(ctypes.Structure):
    _fields_ = [('BaseAddress', ctypes.c_void_p), ('AllocationBase', ctypes.c_void_p), ('AllocationProtect', wt.DWORD),
                ('RegionSize', ctypes.c_size_t), ('State', wt.DWORD), ('Protect', wt.DWORD), ('Type', wt.DWORD)]


def game_pid():
    out = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq Game.exe', '/FO', 'CSV', '/NH'], capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        if len(parts) > 1 and parts[0].lower().strip('"') == 'game.exe':
            return int(parts[1])
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('pattern')
    ap.add_argument('--pid', type=int, default=0)
    ap.add_argument('--max', type=int, default=40)
    args = ap.parse_args()
    pat = bytes.fromhex(args.pattern)
    pid = args.pid or game_pid()
    if not pid:
        print('no Game.exe running')
        return
    k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    k32.OpenProcess.restype = wt.HANDLE
    h = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
    if not h:
        print('OpenProcess failed', ctypes.get_last_error())
        return
    addr, hits = 0x10000, 0
    mbi = MBI()
    while addr < 0x7FFF0000 and hits < args.max:
        if not k32.VirtualQueryEx(h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
            break
        base, size = mbi.BaseAddress or 0, mbi.RegionSize
        if mbi.State == MEM_COMMIT and not (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) and size < 512 * 1024 * 1024:
            buf = ctypes.create_string_buffer(size)
            got = ctypes.c_size_t(0)
            if k32.ReadProcessMemory(h, ctypes.c_void_p(base), buf, size, ctypes.byref(got)):
                data = buf.raw[:got.value]
                i = data.find(pat)
                while i != -1 and hits < args.max:
                    print('%08X type=%X: %s' % (base + i, mbi.Type, data[i:i + 64].hex(' ')))
                    hits += 1
                    i = data.find(pat, i + 1)
        addr = base + size
    k32.CloseHandle(h)
    print(hits, 'hits')


if __name__ == '__main__':
    main()
