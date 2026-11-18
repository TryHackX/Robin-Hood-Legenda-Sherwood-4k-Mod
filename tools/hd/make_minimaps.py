"""HD pictures of the opened map (the scroll in the top-right corner), made from the HD picture of the level itself.

    python -I tools/hd/make_minimaps.py [--variant dat2] [--only KEY,...]

The game's map picture (DATA\\Levels\\<Day|Night|Fog|Custom1>\\<level>.min, see minimap_pictures.py) is the level's
own background, shrunk into the scroll's paper (the window rectangle minus 14 / 24 px - the same rectangle the game
maps the camera and the people's markers into), tinted like old paper and fading into the scroll at its edges. The
level part is made again from the HD background (hd/maps_hd/<variant>/<level>_<ambiance>.png, 2x the level) at four
times the picture's size - sharp up to the map's largest size in the settings (5x, nearly):
  clean      the HD background area-reduced to the paper, in its own colours;
  parchment  the same in the colours of the game's picture - the linear map that gives the background's colours the
             mean and covariance of the picture's, then per channel the picture's own distribution, fitted on the
             middle of the picture - with a paper grain and a slightly painted look (an old map on parchment).
The scroll and the faded edge come from the game's picture upscaled 4x by the model (upscale_menu_parts.py --scale 4
--out hd/work/minimap_x4); the level part is blended into it over the width where the game's picture
fades. The fog pictures are a colourless sketch on the paper: their parchment version takes the sketch's colours
from the background's brightness alone (sepia, no coloured spots). The Sherwood camp's pictures show another picture
than the camp level: the colours of a forest crossroads by day / a town by night.
Output: hd/minimap/<clean|parchment>/<key>.hdp ("HDMP", 1, 4w, 4h, BGRA, shade bytes) - the mod draws the opened
map from them at any size (MinimapHD=2 / 3); previews hd/minimap_png/<kind>_<level>_<ambiance>_<key>.png.
"""
import argparse
import struct
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))      # also with python -I
from minimap_pictures import LEVELS, read_min  # noqa: E402
from res_pictures import ROOT, fnv_key  # noqa: E402

X4 = ROOT / 'hd' / 'work' / 'minimap_x4'
OUT = ROOT / 'hd' / 'minimap'
PNG = ROOT / 'hd' / 'minimap_png'
MX, MY = 14, 24          # the paper rectangle inside the window (Game.exe 0x006C0C58)
S = 4                    # the map's sizes go up to 5x (MapScale=500): four times keeps that nearly sharp
FADE = 5                 # the game's picture fades into the paper over about this many of its pixels
FALLBACK = {('sherwood', 'day'): ('croisement01', 'day'), ('sherwood', 'night'): ('leicester', 'night')}


def rgb565(raw, w, h):
    px = np.frombuffer(raw, '<u2', w * h).reshape(h, w).astype(np.uint32)
    r, g, b = (px >> 11) & 31, (px >> 5) & 63, px & 31
    rgb = np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], -1).astype(np.float32)
    return rgb, px == 0x07C0


def read_hdp(path):
    d = path.read_bytes()
    magic, ver, w, h = struct.unpack_from('<4sIII', d, 0)
    if magic != b'HDMP':
        raise ValueError(path)
    return np.frombuffer(d, np.uint8, w * h * 4, 16).reshape(h, w, 4)


def load_rgb(path):
    from PIL import Image
    return np.asarray(Image.open(path).convert('RGB'), dtype=np.float32)


def _sqrtm(c, inverse=False):
    w, v = np.linalg.eigh(c)
    w = np.clip(w, 1e-6, None)
    return (v * (1.0 / np.sqrt(w) if inverse else np.sqrt(w))) @ v.T


def fit_colours(src, dst, mask):
    """Monge-Kantorovich linear colour map (mean and covariance of dst), then per channel dst's quantiles. A
    least-squares fit shrinks the contrast where the picture is only a faint drawing of the level."""
    a = src[mask].reshape(-1, 3).astype(np.float64)
    b = dst[mask].reshape(-1, 3).astype(np.float64)
    mu_a, mu_b = a.mean(0), b.mean(0)
    ca = np.cov(a.T) + np.eye(3) * 1e-3
    cb = np.cov(b.T) + np.eye(3) * 1e-3
    ha, iha = _sqrtm(ca), _sqrtm(ca, True)
    A = iha @ _sqrtm(ha @ cb @ ha) @ iha
    lin = (a - mu_a) @ A.T + mu_b
    q = np.linspace(0, 100, 129)
    curves = [(np.percentile(lin[:, c], q), np.percentile(b[:, c], q)) for c in range(3)]
    return (A, mu_a, mu_b), curves


def apply_colours(img, fit):
    (A, mu_a, mu_b), curves = fit
    h, w, _ = img.shape
    lin = (img.reshape(-1, 3).astype(np.float64) - mu_a) @ A.T + mu_b
    out = np.empty_like(lin)
    for c in range(3):
        xs, ys = curves[c]
        xs = np.maximum.accumulate(xs + np.arange(len(xs)) * 1e-6)
        out[:, c] = np.interp(lin[:, c], xs, ys)
    return out.reshape(h, w, 3).clip(0, 255).astype(np.float32)


def lum(rgb):
    return rgb[..., 0] * 0.299 + rgb[..., 1] * 0.587 + rgb[..., 2] * 0.114


def fit_tone(src, dst, mask):
    """The fog pictures: a colourless sketch on the paper. Per channel a curve from the background's brightness to
    the sketch's colours (matching quantiles) - a sepia picture in the sketch's own colours, no coloured spots."""
    a = lum(src)[mask].astype(np.float64)
    b = dst[mask].reshape(-1, 3).astype(np.float64)
    q = np.linspace(0, 100, 129)
    return ('tone', np.percentile(a, q), [np.percentile(b[:, c], q) for c in range(3)])


def apply_tone(img, fit):
    _, qa, qb = fit
    l = lum(img).astype(np.float64)
    qa = np.maximum.accumulate(qa + np.arange(len(qa)) * 1e-6)
    return np.stack([np.interp(l, qa, qb[c]) for c in range(3)], -1).clip(0, 255).astype(np.float32)


def fit_error(game, b1s, rect):
    """mean |colour-matrix-fitted background - picture| over the middle of `rect`, and the correlation of their
    fine structure there (Laplacian of the grey picture): about 0.5 and more when the picture shows the level"""
    import cv2
    x0, y0, x1, y1 = rect
    cw, ch = x1 - x0, y1 - y0
    if cw < 20 or ch < 20:
        return 1e9, 0.0
    small = cv2.resize(b1s, (cw, ch), interpolation=cv2.INTER_AREA)
    inner = game[y0:y1, x0:x1]
    sy, sx = slice(int(ch * 0.2), int(ch * 0.8)), slice(int(cw * 0.2), int(cw * 0.8))
    a = small[sy, sx].reshape(-1, 3)
    b = inner[sy, sx].reshape(-1, 3)
    A = np.concatenate([a, np.ones((len(a), 1), np.float32)], 1)
    M, *_ = np.linalg.lstsq(A, b, rcond=None)
    err = float(np.abs(A @ M - b).mean())
    la = cv2.Laplacian(small[sy, sx].mean(-1), cv2.CV_32F).ravel()
    lb = cv2.Laplacian(inner[sy, sx].mean(-1), cv2.CV_32F).ravel()
    corr = float(np.dot(la - la.mean(), lb - lb.mean()) / (np.linalg.norm(la - la.mean()) * np.linalg.norm(lb - lb.mean()) + 1e-6))
    return err, corr


def align(game, b1):
    """the rectangle the level really lies in on the game's picture (one pixel off the game's own mapping
    rectangle in most pictures): coordinate descent over the four edges"""
    import cv2
    h, w, _ = game.shape
    b1s = cv2.resize(b1, (w * 3, h * 3), interpolation=cv2.INTER_AREA)
    rect = [MX, MY, w - MX, h - MY]
    best, corr = fit_error(game, b1s, rect)
    improved = True
    while improved:
        improved = False
        for i in range(4):
            for step in (-1, 1):
                trial = list(rect)
                trial[i] += step
                if abs(trial[i] - (MX, MY, w - MX, h - MY)[i]) > 3:
                    continue
                e, c = fit_error(game, b1s, trial)
                if e < best - 1e-3:
                    best, corr, rect, improved = e, c, trial, True
    return rect, best, corr


def paper(h, w, seed):
    """paper grain: blotches, fibres and fine grain, mean 0, about unit deviation"""
    import cv2
    rng = np.random.default_rng(seed)
    tex = np.zeros((h, w), np.float32)
    for cell, amp in ((96, 0.55), (24, 0.3), (6, 0.2)):
        n = rng.standard_normal((h // cell + 3, w // cell + 3)).astype(np.float32)
        tex += amp * cv2.resize(n, (w + 3 * cell, h + 3 * cell), interpolation=cv2.INTER_CUBIC)[cell:cell + h, cell:cell + w]
    fibres = cv2.GaussianBlur(rng.standard_normal((h, w)).astype(np.float32), (0, 0), sigmaX=6.0, sigmaY=0.6)
    tex += 0.25 * fibres / (fibres.std() + 1e-6)
    tex += 0.12 * rng.standard_normal((h, w)).astype(np.float32)
    return tex / (tex.std() + 1e-6)


def parchment_look(rgb, seed):
    """an old map on parchment: slightly painted (edge-preserving smoothing), paper grain in the brightness"""
    import cv2
    img = rgb.clip(0, 255).astype(np.uint8)
    painted = cv2.edgePreservingFilter(img, flags=cv2.RECURS_FILTER, sigma_s=10, sigma_r=0.18).astype(np.float32)
    out = 0.55 * painted + 0.45 * rgb
    tex = paper(out.shape[0], out.shape[1], seed)
    out *= (1.0 + 0.055 * tex)[..., None]
    return out.clip(0, 255)


def main():
    import cv2
    from PIL import Image
    ap = argparse.ArgumentParser()
    ap.add_argument('--variant', default='dat2', help='HD background variant (folder of hd/maps_hd)')
    ap.add_argument('--only', default='')
    args = ap.parse_args()
    only = {k.strip().lower() for k in args.only.split(',') if k.strip()}
    for kind in ('clean', 'parchment'):
        (OUT / kind).mkdir(parents=True, exist_ok=True)
    PNG.mkdir(parents=True, exist_ok=True)

    # every different picture, its background and the colour fit where the picture shows the level
    items, fits = [], {}
    seen = {}       # key -> (level, ambiance) of its first file; the same picture elsewhere shares its fit
    aliases = []
    for path in sorted(LEVELS.rglob('*')):
        if path.suffix.lower() != '.min':
            continue
        w, h, raw = read_min(path)
        key = f'{fnv_key(raw, w, h):016x}'
        rel = path.relative_to(LEVELS)
        level, amb = rel.stem.lower(), rel.parts[0].lower()
        if key in seen:
            aliases.append(((level, amb), seen[key]))
            continue
        seen[key] = (level, amb)
        names = [f'{level}_{amb}', f'{level}_day']
        lo = next((p for p in (ROOT / 'hd' / 'maps_png' / f'{n}.png' for n in names) if p.exists()), None)
        hi = next((p for p in (ROOT / 'hd' / 'maps_hd' / args.variant / f'{n}.png' for n in names) if p.exists()), None)
        if lo is None or hi is None or not (X4 / f'{key}.hdp').exists():
            print(f'{rel}: background or 4x scroll missing - skipped')
            continue
        game, _ = rgb565(raw, w, h)
        b1 = load_rgb(lo)
        (ax0, ay0, ax1, ay1), err, corr = align(game, b1)
        own = corr >= 0.3
        if own:
            small = cv2.resize(b1, (ax1 - ax0, ay1 - ay0), interpolation=cv2.INTER_AREA)
            yy, xx = np.mgrid[0:ay1 - ay0, 0:ax1 - ax0]
            d = np.minimum(np.minimum(xx, ax1 - ax0 - 1 - xx), np.minimum(yy, ay1 - ay0 - 1 - yy))
            middle = d >= max(8.0, 0.18 * min(ax1 - ax0, ay1 - ay0))
            inner = game[ay0:ay1, ax0:ax1]
            fits[(level, amb)] = fit_tone(small, inner, middle) if amb == 'fog' else fit_colours(small, inner, middle)
        items.append((rel, level, amb, key, w, h, hi, own, err, corr))
    for name, first in aliases:
        if first in fits and name not in fits:
            fits[name] = fits[first]

    for rel, level, amb, key, w, h, hi, own, err, corr in items:
        if only and key not in only:
            continue
        ref = (level, amb) if own else (FALLBACK.get((level, amb)) or (level, 'day'))
        fit = fits.get(ref)
        if fit is None:
            print(f'{rel}: no colour fit for {ref} - parchment skipped')
        cw, ch = w - 2 * MX, h - 2 * MY
        yy, xx = np.mgrid[0:ch, 0:cw]
        dist = np.minimum(np.minimum(xx, cw - 1 - xx), np.minimum(yy, ch - 1 - yy)).astype(np.float32)
        d3 = cv2.resize(dist, (cw * S, ch * S), interpolation=cv2.INTER_LINEAR)
        t = np.clip((d3 - 0.5) / FADE, 0, 1)
        alpha = (t * t * (3 - 2 * t))[..., None]
        b2 = load_rgb(hi)
        level_hd = cv2.resize(b2, (cw * S, ch * S), interpolation=cv2.INTER_AREA)
        base4 = read_hdp(X4 / f'{key}.hdp').astype(np.float32)
        base = cv2.resize(base4, (w * S, h * S), interpolation=cv2.INTER_AREA)
        seed = int(key[:8], 16)
        for kind in ('clean', 'parchment'):
            if kind == 'clean':
                gen = level_hd
            else:
                if fit is None:
                    continue
                toned = apply_tone(level_hd, fit) if fit[0] == 'tone' else apply_colours(level_hd, fit)
                gen = parchment_look(toned, seed)
            out = base.copy()
            rgb = out[..., [2, 1, 0]]
            y0, x0 = MY * S, MX * S
            rgb[y0:y0 + ch * S, x0:x0 + cw * S] = rgb[y0:y0 + ch * S, x0:x0 + cw * S] * (1 - alpha) + gen * alpha
            out[..., 0], out[..., 1], out[..., 2] = rgb[..., 2], rgb[..., 1], rgb[..., 0]
            bgra = (out + 0.5).clip(0, 255).astype(np.uint8)
            with open(OUT / kind / f'{key}.hdp', 'wb') as f:
                f.write(struct.pack('<4sIII', b'HDMP', 1, w * S, h * S))
                f.write(bgra.tobytes())
                f.write(np.zeros((h * S, w * S), np.uint8).tobytes())
            a = bgra[..., 3:4].astype(np.float32) / 255
            prev = bgra[..., [2, 1, 0]].astype(np.float32) * a + 60 * (1 - a)
            Image.fromarray(prev.astype(np.uint8)).save(PNG / f'{kind}_{level}_{amb}_{key}.png')
        how = 'its own picture' if own else f'colours of {ref[0]} {ref[1]}'
        print(f'{str(rel):28} {w}x{h} -> {w * S}x{h * S} from {hi.name} ({how}; structure correlation {corr:.2f})')


if __name__ == '__main__':
    main()
