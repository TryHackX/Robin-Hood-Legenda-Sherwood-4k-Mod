"""Compiles the presenter's shaders (src/ddm/ddm_present.cpp, kVS / kPS) with fxc from the Windows SDK - the DLL compiles
them only at run time (D3DCompile), so a mistake there shows only in the game (2026-11-22).

    python -I tools/check_shader.py            -> "vs: ok", "ps: ok" or fxc's errors
"""
import ast
import glob
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / 'src' / 'ddm' / 'ddm_present.cpp'


def shader_text(source, name):
    """The C string literals of `const char name[] = "..." "...";` joined (comments between them skipped)."""
    m = re.search(r'const char ' + name + r'\[\]\s*=(.*?);\s*\n', source, re.S)
    if not m:
        sys.exit(f'{name} not found in {SRC}')
    parts = []
    for line in m.group(1).splitlines():
        line = line.strip()
        if not line or line.startswith('//'):
            continue
        parts.append(ast.literal_eval(line))     # one "..." literal per line
    return ''.join(parts)


def main():
    fxc = sorted(glob.glob(r'C:\Program Files (x86)\Windows Kits\10\bin\*\x64\fxc.exe'))
    if not fxc:
        sys.exit('fxc.exe not found (Windows SDK)')
    source = SRC.read_text(encoding='utf-8')
    ok = True
    with tempfile.TemporaryDirectory() as tmp:
        for name, profile in (('kVS', 'vs_4_0'), ('kPS', 'ps_4_0')):
            path = Path(tmp) / f'{name}.hlsl'
            path.write_text(shader_text(source, name), encoding='ascii')
            r = subprocess.run([fxc[-1], '/nologo', '/T', profile, '/E', 'main', '/Fo', str(Path(tmp) / f'{name}.cso'), str(path)],
                               capture_output=True, text=True)
            print(f'{name}: ' + ('ok' if r.returncode == 0 else 'FAILED'))
            if r.returncode != 0:
                ok = False
                print(r.stdout + r.stderr)
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
