"""Study: what moves in a water animation (frame - mean of all frames), over the HD background.

    python -I tools/hd/water_motion_study.py [--variant dat2] [--blur 1] [--gain 1.0]
        -> hd/compare/ground_anim/pond_motion.png       HD + motion, every frame (rows)
        -> hd/compare/ground_anim/pond_motion_diff.png  the motion itself, x4 around grey
"""
import argparse, os, sys
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
from extract_sprites import Bank, KEY  # noqa: E402
from ground_anim_study import rgb565_to_float, box_blur, up2_bilinear  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--variant', default='dat2')
    ap.add_argument('--blur', type=int, default=1)
    ap.add_argument('--gain', type=float, default=1.0)
    args = ap.parse_args()
    bank = Bank()
    level, (mx, my), (w, h) = 'nottingham_day', (1196, 2632), (272, 178)
    ids = list(range(4579, 4587))
    hd = np.asarray(Image.open(os.path.join(ROOT, 'hd', 'maps_hd', args.variant, level + '.png')).convert('RGB'), np.float32)
    hdg = hd[2 * my:2 * (my + h), 2 * mx:2 * (mx + w)]
    frames = [bank.decode16(i) for i in ids]
    op = np.logical_and.reduce([f != KEY for f in frames])
    fs = [rgb565_to_float(f) for f in frames]
    mean = sum(fs) / len(fs)
    rows, drows = [], []
    for i, f in zip(ids, fs):
        d = np.where(op[..., None], f - mean, 0.0)
        ad = np.abs(d).mean(-1)[op]
        print('frame %d: |frame - mean| mean %.2f p50 %.2f p90 %.2f p99 %.2f' % (i, ad.mean(), *np.percentile(ad, [50, 90, 99])))
        sd = box_blur(d, args.blur) * args.gain
        rows.append(np.clip(hdg + up2_bilinear(sd), 0, 255))
        drows.append(np.clip(128 + 4 * np.concatenate([d, box_blur(d, args.blur)], 1), 0, 255))
    out = os.path.join(ROOT, 'hd', 'compare', 'ground_anim')
    Image.fromarray(np.concatenate(rows, 0).astype(np.uint8)).save(os.path.join(out, 'pond_motion.png'))
    Image.fromarray(np.concatenate(drows, 0).astype(np.uint8)).save(os.path.join(out, 'pond_motion_diff.png'))
    # consecutive frames differ by
    for a, b, i in zip(fs, fs[1:] + fs[:1], ids):
        print('frame %d -> next: mean |diff| %.2f' % (i, np.abs(a - b).mean(-1)[op].mean()))


if __name__ == '__main__':
    main()
