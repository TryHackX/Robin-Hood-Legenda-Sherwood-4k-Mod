"""Make a contact sheet (checkerboard bg, scaled) of PNGs: contact_sheet.py out.png scale in1.png in2.png ..."""
import sys
from PIL import Image
out, sc, files = sys.argv[1], int(sys.argv[2]), sys.argv[3:]
ims = [Image.open(f).convert('RGBA') for f in files]
ims = [i.resize((i.width * sc, i.height * sc), Image.NEAREST) for i in ims]
W = sum(i.width for i in ims) + 8 * len(ims); H = max(i.height for i in ims)
bg = Image.new('RGBA', (W, H), (90, 90, 140, 255))
x = 0
for i in ims:
    bg.alpha_composite(i, (x, 0)); x += i.width + 8
bg.save(out)
