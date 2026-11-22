"""Draw the interface scale's pointer classes (test command "pointergrid") over a frame dump.

    python -I tools/hd/plot_pointer.py tools/out/harness/grid.csv tools/out/harness/frame.bmp tools/out/grid.png [x0 y0 x1 y1]

Nothing drawn: the world gets the real position. Blue: one of the game's windows takes the pointer. Red: the world
gets a point that is not under the cursor (since 1.3.5 only while the map is dragged or over the moved map's old
place). Magenta (old CSVs): the same over a picture whose pixels are unknown. Orange: the interface takes the pointer
although none of the game's windows lies there (its visible pictures, the hedge strip). A white dot: a see-through
picture was skipped there. The optional rectangle crops the picture (screen pixels).
"""
import csv
import sys

from PIL import Image, ImageDraw

COLOURS = {1: (40, 120, 255), 2: (255, 40, 40), 3: (255, 0, 255), 4: (255, 150, 0)}


def main():
    rows = list(csv.DictReader(open(sys.argv[1], newline="")))
    im = Image.open(sys.argv[2]).convert("RGB")
    draw = ImageDraw.Draw(im)
    for r in rows:
        x, y, c, see = int(r["x"]), int(r["y"]), int(r["class"]), int(r["seeThrough"])
        if c in COLOURS:
            draw.rectangle([x - 3, y - 3, x + 3, y + 3], fill=COLOURS[c])
        if see:
            draw.rectangle([x - 1, y - 1, x + 1, y + 1], fill=(255, 255, 255))
    if len(sys.argv) >= 8:
        im = im.crop(tuple(int(v) for v in sys.argv[4:8]))
    im.save(sys.argv[3])
    print(sys.argv[3], im.size, len(rows), "points")


if __name__ == "__main__":
    main()
