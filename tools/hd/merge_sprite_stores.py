"""Append the frames of sprite stores made side by side into one store.

    python tools\\hd\\merge_sprite_stores.py --into _common _common_p0 _common_p1 _common_p2 _common_p3 [--delete]

Every frame a part has and the target lacks is copied to the end of the target's sprites.dat and
entered in its sprites.idx (formats: see upscale_sprites.py). --delete removes the part folders
afterwards. Safe to run again: frames already in the target are skipped.
"""
import argparse
import shutil
import struct
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
ENTRY = np.dtype([("off", "<u8"), ("size", "<u4"), ("w", "<u2"), ("h", "<u2")])


def read_index(d: Path):
    raw = (d / "sprites.idx").read_bytes()
    assert raw[:4] == b"HDSI"
    n = struct.unpack_from("<I", raw, 8)[0]
    return np.frombuffer(raw, ENTRY, n, 16).copy()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--into", required=True)
    ap.add_argument("parts", nargs="+")
    ap.add_argument("--delete", action="store_true")
    args = ap.parse_args()

    base = ROOT / "hd" / "sprites"
    target = base / args.into
    target.mkdir(parents=True, exist_ok=True)
    first = read_index(base / args.parts[0])
    if (target / "sprites.idx").exists():
        entries = read_index(target)
    else:
        entries = np.zeros(len(first), ENTRY)
        (target / "sprites.dat").write_bytes(b"HDSD" + struct.pack("<III", 1, 0, 0))
    copied = 0
    with open(target / "sprites.dat", "r+b") as out:
        out.seek(0, 2)
        for name in args.parts:
            part = base / name
            idx = read_index(part)
            todo = np.nonzero((idx["size"] != 0) & (entries["size"] == 0))[0]
            todo = todo[np.argsort(idx["off"][todo])]  # read the part front to back
            with open(part / "sprites.dat", "rb") as src:
                for i in todo:
                    src.seek(int(idx["off"][i]))
                    data = src.read(int(idx["size"][i]))
                    if len(data) != int(idx["size"][i]):
                        print(f"{name}: frame {i} is cut off - skipped")
                        continue
                    entries[i] = (out.tell(), len(data), idx["w"][i], idx["h"][i])
                    out.write(data)
                    copied += 1
            print(f"{name}: {len(todo)} frames taken")
    tmp = target / "sprites.tmp"
    with open(tmp, "wb") as f:
        f.write(b"HDSI" + struct.pack("<III", 1, len(entries), 0))
        f.write(entries.tobytes())
    tmp.replace(target / "sprites.idx")
    print(f"{args.into}: {int((entries['size'] != 0).sum())} frames in total ({copied} new), "
          f"data file {(target / 'sprites.dat').stat().st_size / 2 ** 20:.0f} MB")
    if args.delete:
        for name in args.parts:
            shutil.rmtree(base / name, ignore_errors=True)
        print("part folders removed")


if __name__ == "__main__":
    main()
