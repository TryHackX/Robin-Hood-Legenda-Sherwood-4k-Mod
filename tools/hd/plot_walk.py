"""Draw a walkability probe (tools/probe_walk.ps1 CSV) over a frame dump of the same camera position.

    python -I tools/hd/plot_walk.py tools/out/walk.csv tools/out/harness/frame.bmp tools/out/walk.png [x0 y0 x1 y1]

Green: the game's cursor says "walk there"; red: the cross ("cannot go there"); blue: the interface scale gives the
pointer to one of the game's windows (the HUD); yellow: another cursor (attack, use ...). The optional rectangle crops
the picture (screen pixels).
"""
import csv
import sys

from PIL import Image, ImageDraw

WALK = "28x32_5c4bdbb6"
CROSS = "32x23_a68f2d2d"


def main():
    rows = list(csv.DictReader(open(sys.argv[1], newline="")))
    im = Image.open(sys.argv[2]).convert("RGB")
    draw = ImageDraw.Draw(im)
    for r in rows:
        x, y = int(r["sx"]), int(r["sy"])
        if r.get("hud") == "1":
            col = (40, 120, 255)
        elif r["cursor"] == WALK:
            col = (40, 230, 40)
        elif r["cursor"] == CROSS:
            col = (255, 40, 40)
        else:
            col = (255, 220, 0)
        draw.rectangle([x - 5, y - 5, x + 5, y + 5], fill=col, outline=(0, 0, 0))
    if len(sys.argv) >= 8:
        im = im.crop(tuple(int(v) for v in sys.argv[4:8]))
    im.save(sys.argv[3])
    print(sys.argv[3], im.size, len(rows), "points")


if __name__ == "__main__":
    main()
