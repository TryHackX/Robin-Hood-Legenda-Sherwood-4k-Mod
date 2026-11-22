"""A look at bushes painted into the strip below the gate on the carriage map (Croisement02) - where the player's
characters cannot go (user 2026-11-22: "put a log or an obstacle there so the player does not think he can go
there"). Only a preview: tools/out/corner_mock.png (before | after, the HD picture, 2x the level).

    python -I tools/hd/mock_corner_bushes.py
"""
import struct
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
KEY = "450c0ad1696a52aa"            # Croisement02 (the carriage mission)


def load_hdmap(path):
    d = path.read_bytes()
    _, _, w, h, _ = struct.unpack_from("<4sIIII", d, 0)
    px = np.frombuffer(d, dtype=np.uint16, count=w * h, offset=20).reshape(h, w)
    r = ((px >> 11) & 31).astype(np.float32) * 255 / 31
    g = ((px >> 5) & 63).astype(np.float32) * 255 / 63
    b = (px & 31).astype(np.float32) * 255 / 31
    return np.dstack([r, g, b])


def paste(img, patch, x, y):
    h, w = patch.shape[:2]
    yy, xx = np.mgrid[0:h, 0:w]
    # an oval, soft at its edge: the bush and the grass around it go over the grass of the strip
    d = np.sqrt(((xx - w / 2) / (w / 2)) ** 2 + ((yy - h / 2) / (h / 2)) ** 2)
    a = np.clip((1.0 - d) / 0.35, 0.0, 1.0)[..., None]
    img[y:y + h, x:x + w] = patch * a + img[y:y + h, x:x + w] * (1 - a)


def main():
    hd = load_hdmap(ROOT / "hd" / "maps" / "dat2" / f"{KEY}.hdmap")
    region = (2860, 1940, 3584, 2304)               # x0, y0, x1, y1 in HD pixels: the gate and the strip below it
    before = hd[region[1]:region[3], region[0]:region[2]].copy()
    bush = hd[2060:2240, 3380:3580].copy()          # the bush right of the gate
    out = hd.copy()
    paste(out, bush, 3040, 2105)
    paste(out, bush[:, ::-1], 3195, 2120)           # mirrored: not the same bush twice
    paste(out, bush, 2900, 2118)
    after = out[region[1]:region[3], region[0]:region[2]]
    w = region[2] - region[0]
    canvas = np.zeros((region[3] - region[1], w * 2 + 16, 3), dtype=np.float32)
    canvas[:, :w] = before
    canvas[:, w + 16:] = after
    Image.fromarray(np.clip(canvas, 0, 255).astype(np.uint8)).save(ROOT / "tools" / "out" / "corner_mock.png")
    print("tools/out/corner_mock.png")


if __name__ == "__main__":
    main()
