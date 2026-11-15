"""Font strips dumped by the game (test harness "fonts dump" -> tools/out/harness/fonts/font_<key>.raw).

    python -I tools/hd/font_strips.py show      -> hd/work/fonts/<key>_colour.png, _alpha.png, _preview.png (x4)
    python -I tools/hd/font_strips.py collect   -> copies the dumps to hd/work/fonts/ (kept for upscale_fonts.py)

.raw: u32 "SBFS", u32 w, u32 h, u32 has_alpha, w*h RGB565 colour, [w*h RGB565 coverage picture].
The coverage of a "Lydian" strip is its blue channel, 0..31 -> weight 2*b of 64 (the game's 0x00622FD4).
A strip without coverage is colour-keyed: 0x07C0 = transparent.
"""
import glob
import os
import shutil
import struct
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
DUMPS = os.path.join(ROOT, 'tools', 'out', 'harness', 'fonts')
WORK = os.path.join(ROOT, 'hd', 'work', 'fonts')
KEY = 0x07C0


def read_raw(path):
    d = open(path, 'rb').read()
    magic, w, h, has_alpha = struct.unpack_from('<4I', d, 0)
    if magic != 0x53464253:
        raise ValueError('not a strip dump: ' + path)
    col = np.frombuffer(d, '<u2', w * h, 16).reshape(h, w)
    alp = np.frombuffer(d, '<u2', w * h, 16 + w * h * 2).reshape(h, w) if has_alpha else None
    return col, alp


def to_rgb(img):
    r = (img >> 11) & 31; g = (img >> 5) & 63; b = img & 31
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], -1).astype(np.uint8)


def coverage(col, alp):
    """0..1 coverage of every pixel (as the game blends it)."""
    if alp is not None:
        return np.where(col == KEY, 0.0, np.minimum(1.0, 2.0 * (alp & 31) / 64.0))
    return (col != KEY).astype(np.float64)


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else 'show'
    os.makedirs(WORK, exist_ok=True)
    for path in sorted(glob.glob(os.path.join(DUMPS, 'font_*.raw'))):
        key = os.path.basename(path)[5:-4]
        if cmd == 'collect':
            shutil.copy2(path, os.path.join(WORK, os.path.basename(path)))
            continue
        col, alp = read_raw(path)
        cov = coverage(col, alp)
        rgb = to_rgb(col)
        Image.fromarray(rgb).save(os.path.join(WORK, key + '_colour.png'))
        Image.fromarray((cov * 255).astype(np.uint8)).save(os.path.join(WORK, key + '_alpha.png'))
        # preview: the first 40 cells over a mid grey, x4
        bg = np.full(rgb.shape, 96.0)
        comp = (rgb * cov[..., None] + bg * (1 - cov[..., None])).astype(np.uint8)
        part = comp[:, :min(comp.shape[1], 16 * 48)]
        Image.fromarray(part).resize((part.shape[1] * 4, part.shape[0] * 4), Image.NEAREST).save(os.path.join(WORK, key + '_preview.png'))
        print(key, col.shape[1], 'x', col.shape[0], 'alpha' if alp is not None else 'keyed',
              'coverage>0: %.1f%%' % (100 * (cov > 0).mean()))


if __name__ == '__main__':
    main()
