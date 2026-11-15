"""Prototype: animated water over the HD background (the "realistic water" of HDWater=2).

The game's own water animation holds no waves - only compression shimmer (water_motion_study.py).
Here the still water of the HD picture is moved instead: a few slow wave trains (periodic in time,
so the loop is seamless) displace the picture by about a pixel - the reflections of trees and
sky wobble as on a real pond - and tilt its brightness a little towards a light from the top
left. The water region is where the game's water sprite is opaque in every frame; the effect
fades out towards its edge (shore, reeds).

    python -I tools/hd/water_anim_proto.py [--variant dat2] [--frames 24] [--preset NAME]
        -> hd/compare/water_anim/<preset>_sheet.png   4 frames side by side (1:1 HD pixels)
        -> hd/compare/water_anim/<preset>.gif          the loop (for watching the motion)
        -> hd/compare/water_anim/<preset>_zoom.png     a detail 2x (nearest), frames 0 and N/4

The same arithmetic is in src/ddm/ddm_hd.cpp (WaterAnimation) - keep the two in step.
"""
import argparse, math, os, sys
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
from extract_sprites import Bank, KEY  # noqa: E402

# wave trains: (wavelength in HD px, direction in degrees (0 = +x, 90 = +y on screen), displacement
# amplitude in HD px, cycles per loop). The vertical wavelength is halved (isometric view).
PRESETS = {
    'calm':   dict(waves=[(72, 15, 0.55, 1), (46, 105, 0.40, 2), (29, 200, 0.28, 3), (19, 300, 0.18, 4)], shade=0.05, glint=0.0),
    'gentle': dict(waves=[(64, 20, 0.85, 1), (41, 110, 0.60, 2), (27, 200, 0.42, 3), (17, 290, 0.26, 4)], shade=0.07, glint=0.10),
    'breeze': dict(waves=[(56, 25, 1.20, 1), (36, 115, 0.85, 2), (23, 205, 0.55, 3), (15, 295, 0.35, 5)], shade=0.09, glint=0.16),
    # what the DLL uses (HDWater=2 / 3): only the smooth part moves (sigma 1.2 HD px), the finest
    # texture stays - the water keeps all its detail; slowest-moving short waves (<= 3 cycles a loop)
    # so that 24 frames a loop cross-fade without visible steps
    'waves':        dict(waves=[(64, 20, 0.85, 1), (41, 110, 0.60, 2), (27, 200, 0.42, 2), (17, 290, 0.26, 3)],
                         shade=0.07, glint=0.10, split=1.2),
    'waves_strong': dict(waves=[(56, 25, 1.20, 1), (36, 115, 0.85, 2), (23, 205, 0.55, 3), (15, 295, 0.35, 3)],
                         shade=0.09, glint=0.16, split=1.2),
}
for _name in ('calm', 'gentle', 'breeze'):      # the same with the smooth part only moving
    PRESETS[_name + '_split'] = dict(PRESETS[_name], split=1.2)
LIGHT = (-0.6, -0.8)        # light from the top left (screen)
EDGE = 7                    # HD px over which the effect fades in from the edge of the water
LOOP_S = 4.0                # seconds per loop


def erode_distance(mask, limit):
    """Chessboard distance to the nearest non-water pixel, up to `limit`."""
    dist = np.zeros(mask.shape, np.float32)
    cur = mask.copy()
    for d in range(1, limit + 1):
        dist[cur] = d
        p = np.pad(cur, 1, constant_values=False)
        cur = (p[1:-1, 1:-1] & p[:-2, 1:-1] & p[2:, 1:-1] & p[1:-1, :-2] & p[1:-1, 2:] &
               p[:-2, :-2] & p[:-2, 2:] & p[2:, :-2] & p[2:, 2:])
        if not cur.any():
            break
    return dist


def bilinear(img, x, y):
    h, w = img.shape[:2]
    x = np.clip(x, 0, w - 1.001)
    y = np.clip(y, 0, h - 1.001)
    x0 = np.floor(x).astype(np.int32); y0 = np.floor(y).astype(np.int32)
    fx = (x - x0)[..., None]; fy = (y - y0)[..., None]
    a = img[y0, x0]; b = img[y0, x0 + 1]; c = img[y0 + 1, x0]; d = img[y0 + 1, x0 + 1]
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy


def gauss_blur(img, sigma):
    r = max(1, int(math.ceil(3 * sigma)))
    k = np.exp(-0.5 * (np.arange(-r, r + 1) / sigma) ** 2).astype(np.float32)
    k /= k.sum()
    pad = np.pad(img, ((r, r), (r, r), (0, 0)), mode='edge')
    tmp = sum(pad[:, i:i + img.shape[1]] * k[i] for i in range(2 * r + 1))
    return sum(tmp[i:i + img.shape[0]] * k[i] for i in range(2 * r + 1))


def cubic_weights(f):
    """Catmull-Rom weights for taps -1, 0, 1, 2 at fraction f (keeps the picture sharp)."""
    f2 = f * f; f3 = f2 * f
    return (-0.5 * f3 + f2 - 0.5 * f, 1.5 * f3 - 2.5 * f2 + 1.0, -1.5 * f3 + 2.0 * f2 + 0.5 * f, 0.5 * f3 - 0.5 * f2)


def catmull_rom(img, x, y):
    h, w = img.shape[:2]
    x = np.clip(x, 0, w - 1.001)
    y = np.clip(y, 0, h - 1.001)
    x0 = np.floor(x).astype(np.int32); y0 = np.floor(y).astype(np.int32)
    wx = cubic_weights(x - x0); wy = cubic_weights(y - y0)
    out = np.zeros(x.shape + (img.shape[2],), np.float32)
    for j in range(4):
        yy = np.clip(y0 + j - 1, 0, h - 1)
        row = np.zeros_like(out)
        for i in range(4):
            xx = np.clip(x0 + i - 1, 0, w - 1)
            row += img[yy, xx] * wx[i][..., None]
        out += row * wy[j][..., None]
    return out


def water_frame(hd, weight, f, nframes, p, origin):
    """One frame of the loop. `origin` = HD position of the region (waves are fixed to the level)."""
    H, W = weight.shape
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
    gx = xx + origin[0]; gy = yy + origin[1]
    dx = np.zeros((H, W), np.float32); dy = np.zeros((H, W), np.float32); slope = np.zeros((H, W), np.float32)
    for lam, ang, amp, n in p['waves']:
        ux, uy = math.cos(math.radians(ang)), math.sin(math.radians(ang))
        kx, ky = ux / lam, 2.0 * uy / lam                  # vertical wavelength halved (isometric)
        ph = 2 * math.pi * (kx * gx + ky * gy - n * f / nframes) + (lam * 0.37)
        c = np.cos(ph)
        dx += amp * c * ux
        dy += amp * c * uy * 0.5
        slope += amp * c * (ux * LIGHT[0] + uy * LIGHT[1])
    tot = sum(a for _, _, a, _ in p['waves'])
    s = slope / tot                                       # -1 .. 1
    sample = catmull_rom if p.get('cubic', True) else bilinear
    if p.get('split', 0) > 0:
        # only the smooth part of the picture moves (no detail lost by resampling it); the finest
        # texture (the rest) stays in place
        low = gauss_blur(hd, p['split'])
        out = sample(low, xx + weight * dx, yy + weight * dy) + (hd - low)
    else:
        out = sample(hd, xx + weight * dx, yy + weight * dy)
    out = out * (1.0 + (weight * p['shade'] * s)[..., None])
    if p['glint'] > 0:
        g = np.clip((s - 0.55) / 0.45, 0, 1) ** 2 * p['glint'] * weight
        out = out + g[..., None] * (255.0 - out)
    return np.clip(out, 0, 255)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--variant', default='dat2')
    ap.add_argument('--frames', type=int, default=24)
    ap.add_argument('--preset', default='gentle', choices=sorted(PRESETS))
    args = ap.parse_args()
    p = PRESETS[args.preset]

    bank = Bank()
    level, (mx, my), (w, h) = 'nottingham_day', (1196, 2632), (272, 178)
    frames = [bank.decode16(i) for i in range(4579, 4587)]
    opaque = np.logical_and.reduce([f != KEY for f in frames])
    mask = np.repeat(np.repeat(opaque, 2, 0), 2, 1)
    dist = erode_distance(mask, EDGE)
    t = np.clip(dist / EDGE, 0, 1)
    weight = (t * t * (3 - 2 * t)).astype(np.float32)        # smoothstep

    hdimg = np.asarray(Image.open(os.path.join(ROOT, 'hd', 'maps_hd', args.variant, level + '.png')).convert('RGB'), np.float32)
    x0, y0 = 2 * mx, 2 * my
    hd = hdimg[y0:y0 + 2 * h, x0:x0 + 2 * w]

    out = os.path.join(ROOT, 'hd', 'compare', 'water_anim')
    os.makedirs(out, exist_ok=True)
    seq = [water_frame(hd, weight, f, args.frames, p, (x0, y0)).astype(np.uint8) for f in range(args.frames)]
    q = args.frames // 4
    Image.fromarray(np.concatenate([hd.astype(np.uint8)] + [seq[i * q] for i in range(3)], 1)).save(
        os.path.join(out, args.preset + '_sheet.png'))
    imgs = [Image.fromarray(s) for s in seq]
    imgs[0].save(os.path.join(out, args.preset + '.gif'), save_all=True, append_images=imgs[1:],
                 duration=int(LOOP_S * 1000 / args.frames), loop=0)
    crop = (slice(130, 330), slice(130, 430))
    z = np.concatenate([hd[crop].astype(np.uint8), seq[0][crop], seq[q][crop]], 1)
    Image.fromarray(z).resize((z.shape[1] * 2, z.shape[0] * 2), Image.NEAREST).save(os.path.join(out, args.preset + '_zoom.png'))
    m = np.abs(seq[0].astype(np.float32) - seq[q].astype(np.float32)).mean(-1)

    def detail(img):    # high-frequency energy inside the water (Laplacian)
        g = img.astype(np.float32).mean(-1)
        lap = np.abs(4 * g[1:-1, 1:-1] - g[:-2, 1:-1] - g[2:, 1:-1] - g[1:-1, :-2] - g[1:-1, 2:])
        return lap[mask[1:-1, 1:-1]].mean()
    print('%s: mask %.0f%% of the rectangle, mean change frame 0 -> %d: %.2f (water only: %.2f), detail kept %.0f%%' % (
        args.preset, 100 * mask.mean(), q, m.mean(), m[mask].mean(),
        100 * np.mean([detail(seq[i]) for i in range(0, args.frames, q)]) / detail(hd)))


if __name__ == '__main__':
    main()
