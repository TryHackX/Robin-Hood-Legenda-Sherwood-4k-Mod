"""Contact sheet of a profile's save thumbnails (DATA\\Savegame\\Profile_NNN\\<save>_t).

The game keeps a 160x120 picture of every save in <save>_t: u16 width, u16 height, u32 bytes per pixel (2 =
RGB565), u32 size of the bzip2 stream that follows. Usage:
    python tools/save_thumbs.py <profile folder> <out.png> [columns]
"""
import bz2
import os
import struct
import sys

from PIL import Image, ImageDraw


def thumb(path):
    d = open(path, 'rb').read()
    w, h, bpp, size = struct.unpack_from('<HHII', d, 0)
    raw = bz2.decompress(d[12:12 + size])
    if bpp != 2 or len(raw) < w * h * 2:
        return None
    im = Image.new('RGB', (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            v = raw[(y * w + x) * 2] | (raw[(y * w + x) * 2 + 1] << 8)
            px[x, y] = (((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31)
    return im


def main():
    folder, out = sys.argv[1], sys.argv[2]
    cols = int(sys.argv[3]) if len(sys.argv) > 3 else 12
    names = sorted(f[:-2] for f in os.listdir(folder) if f.endswith('_t'))
    tiles = []
    for n in names:
        try:
            im = thumb(os.path.join(folder, n + '_t'))
        except (OSError, ValueError, struct.error):
            im = None
        if im is not None:
            tiles.append((n.replace('Savegame_', ''), im))
    rows = (len(tiles) + cols - 1) // cols
    sheet = Image.new('RGB', (cols * 160, rows * 136), (20, 20, 20))
    draw = ImageDraw.Draw(sheet)
    for i, (label, im) in enumerate(tiles):
        x, y = (i % cols) * 160, (i // cols) * 136
        sheet.paste(im, (x, y + 16))
        draw.text((x + 3, y + 2), label, fill=(255, 255, 0))
    sheet.save(out)
    print(out, sheet.size, len(tiles))


if __name__ == '__main__':
    main()
