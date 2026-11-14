"""Checks the HD pack archives in release/hdpack/: every file's CRC (by reading it whole), and
that the parts of a cut data file ("<name>", "<name>.2", ...) put together are byte for byte
the original in hd/.

    python -I tools/check_hd_packs.py
"""
import hashlib
import os
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "release", "hdpack")


def main():
    parts = {}          # data file -> {part number: (archive, entry)}
    bad = 0
    for name in sorted(os.listdir(OUT)):
        if not name.endswith(".zip"):
            continue
        with zipfile.ZipFile(os.path.join(OUT, name)) as z:
            broken = z.testzip()
            print(f"{name}: {len(z.namelist())} files, CRC {'OK' if broken is None else 'BROKEN at ' + broken}", flush=True)
            bad += broken is not None
            for entry in z.namelist():
                base, _, num = entry.rpartition(".")
                if num.isdigit() and base.endswith(".dat"):
                    parts.setdefault(base, {})[int(num)] = (name, entry)
                elif entry.endswith(".dat") and entry in [p for p in z.namelist()]:
                    parts.setdefault(entry, {}).setdefault(1, (name, entry))
    for base, pieces in sorted(parts.items()):
        if len(pieces) < 2:
            continue
        joined = hashlib.sha256()
        for num in sorted(pieces):
            name, entry = pieces[num]
            with zipfile.ZipFile(os.path.join(OUT, name)) as z, z.open(entry) as f:
                for chunk in iter(lambda: f.read(8 << 20), b""):
                    joined.update(chunk)
        original = hashlib.sha256()
        with open(os.path.join(ROOT, base.replace("/", os.sep)), "rb") as f:
            for chunk in iter(lambda: f.read(8 << 20), b""):
                original.update(chunk)
        same = joined.hexdigest() == original.hexdigest()
        print(f"{base}: {len(pieces)} parts joined {'== original' if same else '!= ORIGINAL'}", flush=True)
        bad += not same
    print("RESULT:", "OK" if bad == 0 else f"{bad} problem(s)")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
