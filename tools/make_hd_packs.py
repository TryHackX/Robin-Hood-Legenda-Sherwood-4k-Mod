"""Packs the HD data the game uses into release archives (ZIP64, deflate), each under GitHub's
2 GiB limit per release file. Paths inside the archives start at the game folder (hd/...), so an
archive is unpacked straight into the game folder.

    python -I tools/make_hd_packs.py                 all packs into release/hdpack/
    python -I tools/make_hd_packs.py --only 1 3      some of them (numbers or names)
    python -I tools/make_hd_packs.py --jobs 3        how many packs are made at the same time
"""
import argparse
import os
import sys
import time
import zipfile
from concurrent.futures import ProcessPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "release", "hdpack")
LIMIT = 2 * 1024 ** 3 - 16 * 1024 ** 2          # a little under 2 GiB

# A data file too large for one pack is cut into parts: "<name>" (part 1), "<name>.2", ... - the
# mod (ddm_hd_sprites.cpp, DataFile) reads the parts as one file. ("file", part, of N)
COMMON = "hd/sprites/_common/sprites.dat"

PACKS = [
    ("1", "SherwoodHD-1-postacie-a.zip", ["hd/sprites/_common/sprites.idx", (COMMON, 1, 2), "hd/sprites/_icons", "hd/CZYTAJ.txt"]),
    ("1b", "SherwoodHD-1-postacie-b.zip", [(COMMON, 2, 2)]),
    ("2", "SherwoodHD-2-tla-dat2.zip", ["hd/maps/dat2", "hd/sprites/dat2", "hd/menu/dat2"]),
    ("3", "SherwoodHD-3-klatki-posrednie.zip", ["hd/sprites/_between"]),
    # 2026-11-22 (mod 1.2.9): the level's animations (birds, flags, smoke ...) and the second try of the characters'
    # rejected pairs - compacted by tools/hd/compact_between.py
    ("4", "SherwoodHD-4-klatki-posrednie-2.zip", ["hd/sprites/_between_more", "hd/sprites/_between_anim"]),
    # 2026-11-22 (mod 1.3.0): the maps in native 4K (4x the original, dat2; tools/hd/make_maps4k.py) - 3.3 GB, two packs:
    # a = crossroads, Sherwood camp, Derby, Leicester, Lincoln; b = Nottingham, York
    ("5", "SherwoodHD-5-mapy-4k-a.zip", [f"hd/maps4k/dat2/{k}.hdm4" for k in (
        "7eca0fd691643f04", "450c0ad1696a52aa", "40f626704a738ad6",                         # Croisement 01..03
        "9b2a8382a5c927fe", "fda5b877cc13782d",                                             # Sherwood day / night
        "a702fa9a13ba6dff", "2f19b01428dfa8fe", "459bc2fc54dc6b76",                         # Derby day / fog / night
        "cfd16e074ca0d117", "d01c5a96dd7ac61d",                                             # Leicester day / night
        "7d8592c1fe401703", "e319bb678a9b2d2e", "511ea802b2113319")]),                      # Lincoln day / fog / night
    ("5b", "SherwoodHD-5-mapy-4k-b.zip", [f"hd/maps4k/dat2/{k}.hdm4" for k in (
        "b0dd5dd8977d158b", "49d9e68652428bf2", "bc6c4a7382b6092a", "79be7097af890505",     # Nottingham day / fog / night / custom
        "759b475d9852e1d2", "8eef9fd5a7a5784a", "ce004d47aaea21c6")]),                      # York day / fog / night
    ("aomr", "SherwoodHD-wariant-aomr.zip", ["hd/maps/aomr", "hd/sprites/aomr", "hd/menu/aomr"]),
    ("sgi", "SherwoodHD-wariant-sgi.zip", ["hd/maps/sgi", "hd/sprites/sgi", "hd/menu/sgi"]),
    ("ultrasharp", "SherwoodHD-wariant-ultrasharp.zip", ["hd/maps/ultrasharp", "hd/sprites/ultrasharp", "hd/menu/ultrasharp"]),
]


def files_of(rel):
    path = os.path.join(ROOT, rel)
    if os.path.isfile(path):
        yield path, rel
        return
    for base, _dirs, names in os.walk(path):
        for name in sorted(names):
            full = os.path.join(base, name)
            yield full, os.path.relpath(full, ROOT).replace(os.sep, "/")


def part_range(full, part, count):
    size = os.path.getsize(full)
    step = (size // count + 2**20 - 1) // 2**20 * 2**20     # whole MB
    start = step * (part - 1)
    end = size if part == count else min(size, start + step)
    return start, end


def write_part(z, full, arc, start, end):
    info = zipfile.ZipInfo(arc, date_time=time.localtime(os.path.getmtime(full))[:6])
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = 0o644 << 16
    with open(full, "rb") as src, z.open(info, "w", force_zip64=True) as dst:
        src.seek(start)
        left = end - start
        while left > 0:
            chunk = src.read(min(left, 8 * 2**20))
            if not chunk:
                raise IOError(f"{full}: ended early")
            dst.write(chunk)
            left -= len(chunk)


def make(pack):
    key, name, parts = pack
    target = os.path.join(OUT, name)
    tmp = target + ".part"
    raw = 0
    with zipfile.ZipFile(tmp, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6, allowZip64=True) as z:
        for rel in parts:
            if isinstance(rel, tuple):
                path, part, count = rel
                full = os.path.join(ROOT, path)
                start, end = part_range(full, part, count)
                arc = path if part == 1 else f"{path}.{part}"
                write_part(z, full, arc, start, end)
                raw += end - start
                continue
            for full, arc in files_of(rel):
                raw += os.path.getsize(full)
                z.write(full, arc)
    size = os.path.getsize(tmp)
    os.replace(tmp, target)
    return name, raw, size


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", nargs="*", default=None)
    ap.add_argument("--jobs", type=int, default=3)
    args = ap.parse_args()
    os.makedirs(OUT, exist_ok=True)
    todo = [p for p in PACKS if not args.only or p[0] in args.only or p[1] in args.only]
    for p in todo:
        for rel in p[2]:
            path = rel[0] if isinstance(rel, tuple) else rel
            if not os.path.exists(os.path.join(ROOT, path)):
                sys.exit(f"missing: {path}")
    with ProcessPoolExecutor(max_workers=max(1, args.jobs)) as ex:
        for name, raw, size in ex.map(make, todo):
            flag = "  TOO LARGE for one GitHub release file" if size > LIMIT else ""
            print(f"{name}: {raw / 2**20:,.0f} MB -> {size / 2**20:,.0f} MB{flag}", flush=True)


if __name__ == "__main__":
    main()
