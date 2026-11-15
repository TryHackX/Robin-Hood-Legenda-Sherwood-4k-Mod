"""Remade menu backgrounds: export the menu pictures for an image AI (ChatGPT ...), import what it painted.

    python -I tools/hd/menu_remake.py export [--variant aomr]
        -> hd/remake/NN_<name>.png         the menu picture (HD, the band the game shows it in)
           hd/remake/NN_<name>_uklad.png   the screen as the player sees it: where the game puts its buttons and texts
           hd/remake/PROMPTY.txt           what to do and a prompt for every picture
           hd/remake/gotowe/               put the painted pictures here, named NN_<name>.png (.jpg / .webp)
    python -I tools/hd/menu_remake.py import [--model sgi]
        <- hd/remake/gotowe/NN_*.(png|jpg|jpeg|webp)
        -> hd/menu/remake/<key>.mhd (+ <inner key>.alias), previews hd/menu_png/remake/<key>.png

A remade picture fills the whole 16:9 part of the screen the menu zoom shows (1365x768 frame pixels, at 2x: 2730x1536):
the black around the old picture is covered too. The mod takes it before the upscaled original when
HDMenuRemake=1 (F10: "MENU W HD" -> "TAK, TLA Z REMAKE GDY SA"). The game's buttons and texts stay on top.
"""
import argparse
import struct
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
REMAKE = ROOT / 'hd' / 'remake'
READY = REMAKE / 'gotowe'
STORE = ROOT / 'hd' / 'menu' / 'remake'
PREVIEW = ROOT / 'hd' / 'menu_png' / 'remake'
FRAMES = ROOT / 'hd' / 'work' / 'menu_frames'
LAYOUTS = ROOT / 'tools' / 'out' / 'menus_layout'
CROP_W, CROP_H = 1365, 768

COMMON = (
    "I am remastering the menus of an old PC game (Robin Hood: The Legend of Sherwood, 2002). "
    "Attached: (1) the original menu background, (2) a screenshot of the menu showing where the game draws its "
    "buttons and texts on top of the background. Paint a NEW background for this screen in the same spirit, "
    "setting and colour mood, but modern and highly detailed: a hand-painted, cinematic medieval fantasy "
    "illustration, soft volumetric light, rich natural textures, crisp detail, no blur, no noise. "
    "Format: 16:9 widescreen landscape, as large as possible, filling the WHOLE frame - no black bars; continue the "
    "scene to the top, bottom and sides where the old picture had black. "
    "Keep the composition so that the game's buttons and texts still fit exactly where the screenshot shows them: "
    "{layout} "
    "Do NOT draw any text, letters, numbers, buttons, frames, icons, logos, signatures or watermarks - only the "
    "background scene itself."
)

SCREENS = [
    # file name, key of the original picture (the mod's), layout screenshot, where the game uses it, layout part of the prompt
    ('01_menu_glowne', '93d9d88fc44170d0', 'main', 'menu główne',
     "On the left a tall medieval stone castle tower and a curtain wall with autumn bushes in front. In the "
     "middle-left a tall, dark wall of ivy and hedge leaves framed by hanging vines - the player's name and "
     "statistics are printed on it in light letters, so keep this central panel dark, plain and low in contrast. "
     "On the right a large old oak whose branches frame a warm, hazy sunset sky over meadows with a small castle "
     "in the distance; a column of seven buttons stands in the lower right half, keep that area calm."),
    ('02_opcje', 'fc66cf702de88229', 'options', 'opcje, wybór gracza, filmy',
     "A misty ancient forest: tall old trunks and mossy rocks on the left. In the centre a dark wall of ivy framed "
     "by hanging vines - the title and information texts are printed on it in light letters, keep it dark, plain "
     "and low in contrast. On the right sun rays falling through the trees; a column of four buttons stands in "
     "the lower right, keep that area calm."),
    ('03_opcje_grafiki', '2188aad19229888c', 'graphics', 'opcje grafiki i dźwięku',
     "A forest clearing. In the centre a dark wall of ivy framed by hanging vines - long buttons and sliders are "
     "drawn over it, keep it dark and plain. On the right an abandoned broken wooden cart with a lute and a round "
     "shield lying in soft misty light; two buttons stand in the lower right, keep that corner calm."),
    ('04_wczytywanie', '45018a3b37cec15f', 'load', 'wczytywanie i zapis gry, klawiatura',
     "A medieval dungeon. In the centre-left a large iron portcullis set in a stone wall - the list of saved games "
     "(or the key bindings) is printed over it, so keep it regular, dark and low in contrast; a thin vertical "
     "scroll bar stands at its right edge. Rough stone walls with a little moss on the far left. On the right a "
     "curved brick vault lit by dim red torchlight; a column of buttons stands in the lower right, keep that area calm."),
    ('05_pergamin', '94cc757eae713dbe', None, 'ekran z pergaminem (twórcy / filmy)',
     "A misty forest with tall trees and ivy. In the centre a large old parchment scroll hanging between two wooden "
     "rollers - text is printed on it, so leave the parchment plain and evenly lit with no writing on it."),
    # 2026-11-15 (user: "new pictures for the credits ... too"): the credits screen - its text scrolls over it
    ('06_tworcy', '2ef7a848bc09b885', 'credits', 'twórcy gry (napisy przewijają się środkiem ekranu)',
     "A dark, mystical ancient forest: huge old gnarled tree trunks, a burst of golden sunlight breaking through "
     "the canopy left of centre, teal-green mist and mossy rocks on the right, ferns and roots on the ground; deep "
     "shadows and a soft dark vignette towards all edges. The credits are printed in light letters in a column "
     "about a third of the screen wide that scrolls slowly from the bottom to the top through the middle of the "
     "screen, and the studio logo appears near the bottom: keep the middle column dark and calm without bright "
     "spots, and let the lowest quarter of the picture fade to almost black."),
]


def read_mhd(path):
    d = path.read_bytes()
    magic, version, cw, ch, l, t, w, h = struct.unpack_from('<4sIHHHHHH', d, 0)
    if magic != b'MNHD' or version != 1:
        raise ValueError(f'{path}: not an HD menu picture')
    bgra = np.frombuffer(d, np.uint8, w * 2 * h * 2 * 4, 20).reshape(h * 2, w * 2, 4)
    return (cw, ch, l, t, w, h), bgra[..., 2::-1].copy()


def write_mhd(path, rgb):
    """The whole shown part (1365x768) at 2x."""
    assert rgb.shape == (CROP_H * 2, CROP_W * 2, 3)
    bgra = np.empty((CROP_H * 2, CROP_W * 2, 4), np.uint8)
    bgra[..., 0] = rgb[..., 2]
    bgra[..., 1] = rgb[..., 1]
    bgra[..., 2] = rgb[..., 0]
    bgra[..., 3] = 255
    tmp = path.with_suffix('.tmp')
    with open(tmp, 'wb') as f:
        f.write(struct.pack('<4sIHHHHHH', b'MNHD', 1, CROP_W, CROP_H, 0, 0, CROP_W, CROP_H))
        f.write(bgra.tobytes())
    tmp.replace(path)


def export(args):
    from PIL import Image
    REMAKE.mkdir(parents=True, exist_ok=True)
    READY.mkdir(parents=True, exist_ok=True)
    lines = []
    for name, key, layout, where, part in SCREENS:
        mhd = ROOT / 'hd' / 'menu' / args.variant / f'{key}.mhd'
        if not mhd.exists():
            print(f'{name}: no {mhd} - skipped')
            continue
        (cw, ch, l, t, w, h), rgb = read_mhd(mhd)
        Image.fromarray(rgb).save(REMAKE / f'{name}.png')
        files = [f'{name}.png']
        shot = LAYOUTS / f'{layout}.png' if layout else None
        if shot and shot.exists():
            im = np.asarray(Image.open(shot).convert('RGB')).copy()
            im[:48] = 0                                  # the mod's frame counter
            im[-80:, -540:] = 0                          # "F10 - USTAWIENIA MODA"
            Image.fromarray(im).save(REMAKE / f'{name}_uklad.png')
            files.append(f'{name}_uklad.png')
        print(f'{name}: {" + ".join(files)} ({where})')
        lines.append((name, where, files, COMMON.format(layout=part)))
    with open(REMAKE / 'PROMPTY.txt', 'w', encoding='utf-8') as f:
        f.write(
            "NOWE TŁA MENU (REMAKE) - JAK TO ZROBIĆ\n"
            "=====================================\n\n"
            "1. Dla każdego ekranu poniżej: otwórz ChatGPT (albo inny program AI do obrazów), dołącz oba pliki\n"
            "   z tego folderu (obraz tła i zrzut z układem przycisków) i wklej prompt (po angielsku - modele\n"
            "   obrazów rozumieją go najdokładniej).\n"
            "2. Gotowy obraz zapisz w podfolderze 'gotowe' pod tą samą nazwą, np. gotowe\\01_menu_glowne.png\n"
            "   (może też być .jpg albo .webp; najlepiej poziomy 16:9 i jak największy).\n"
            "3. Daj znać - uruchomię import (albo sam: python -I tools\\hd\\menu_remake.py import). Narzędzie\n"
            "   dopasuje obraz do ekranu 16:9, powiększy go modelem AI do 2730x1536 i wstawi do hd\\menu\\remake\\.\n"
            "4. W grze F10 -> 'MENU W HD (TLA, NAPISY)' -> 'TAK, TLA Z REMAKE GDY SA'. Przyciski i napisy gry\n"
            "   zostają na wierzchu (w HD). Nie podoba się - usuń plik z hd\\menu\\remake\\ albo przełącz w F10.\n\n"
            "Wskazówki: jeśli ChatGPT doda napisy albo przyciski, poproś: 'remove all text and buttons'. Jeśli\n"
            "środkowy panel wyjdzie za jasny (napisy będą słabo widoczne), poproś: 'make the central panel darker\n"
            "and plainer'. Kilka wersji jednego ekranu? Zapisz je jako 01_menu_glowne_a.png, _b.png - zaimportuję\n"
            "wybraną.\n"
            "O 4K: tła HD mają już 2730x1536 pikseli na część ekranu z menu - więcej niż ekran 2560x1440. Wersje 4K\n"
            "miałyby sens dopiero na monitorze 4K.\n\n")
        for name, where, files, prompt in lines:
            f.write(f"---- {name}  ({where})\n")
            f.write(f"Pliki do dołączenia: {', '.join(files)}\n")
            f.write(f"Zapisz wynik jako: gotowe\\{name}.png\n\n")
            f.write(prompt + "\n\n")
    print('->', REMAKE / 'PROMPTY.txt')


def fit_16_9(img):
    """Crop the middle of `img` (HxWx3) to the 16:9 shape of the shown part."""
    h, w = img.shape[:2]
    want = CROP_W / CROP_H
    if w / h > want:
        nw = int(round(h * want))
        x0 = (w - nw) // 2
        return img[:, x0:x0 + nw]
    nh = int(round(w / want))
    y0 = (h - nh) // 2
    return img[y0:y0 + nh]


def import_(args):
    import cv2
    from PIL import Image
    from upscale_menu import read_frame, inner_key
    files = sorted(p for p in READY.glob('*') if p.suffix.lower() in ('.png', '.jpg', '.jpeg', '.webp'))
    if not files:
        print(f'nothing in {READY}')
        return
    by_prefix = {name[:2]: (name, key) for name, key, *_ in SCREENS}
    STORE.mkdir(parents=True, exist_ok=True)
    PREVIEW.mkdir(parents=True, exist_ok=True)
    desc = None
    done = set()
    for p in files:
        hit = by_prefix.get(p.name[:2])
        if not hit:
            print(f'{p.name}: the name must start with the number of a screen (01..05) - skipped')
            continue
        name, key = hit
        if key in done:
            print(f'{p.name}: {name} was imported from an earlier file already - skipped (one picture per screen)')
            continue
        img = np.asarray(Image.open(p).convert('RGB'))
        img = fit_16_9(img)
        h, w = img.shape[:2]
        tw, th = CROP_W * 2, CROP_H * 2
        if w < tw:
            if desc is None:
                import torch  # noqa: F401
                from upscale_test import load_model
                from upscale_variants import VARIANTS, pick_precision, upscale
                desc = load_model(ROOT / 'hd' / 'models' / VARIANTS[args.model][0])
                sample = np.zeros((96, 96, 3), np.uint8)
                sample[::2, ::3] = 180
                dtype, how = pick_precision(desc, sample, args.tile, args.overlap)
                print(f'{args.model}: x{desc.scale}, {how}')
            res = upscale(desc, np.ascontiguousarray(img), args.tile, args.overlap, dtype)
            res = cv2.resize(res, (tw, th), interpolation=cv2.INTER_AREA)
            how_made = f'{w}x{h} enlarged by the model ({args.model})'
        else:
            res = cv2.resize(np.ascontiguousarray(img), (tw, th), interpolation=cv2.INTER_AREA)
            how_made = f'{w}x{h} reduced'
        write_mhd(STORE / f'{key}.mhd', res)
        Image.fromarray(res).resize((CROP_W, CROP_H), Image.LANCZOS).save(PREVIEW / f'{key}.png')
        mnf = FRAMES / f'{key}.mnf'
        if mnf.exists():
            ik = inner_key(read_frame(mnf))
            if ik is not None:
                (STORE / f'{ik:016x}.alias').write_text(key, encoding='ascii')
        done.add(key)
        print(f'{p.name} -> hd/menu/remake/{key}.mhd ({name}; {how_made})')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('what', choices=['export', 'import'])
    ap.add_argument('--variant', default='aomr', help='export: which HD pictures to give as the reference')
    ap.add_argument('--model', default='sgi', help='import: the model that enlarges small pictures')
    ap.add_argument('--tile', type=int, default=192)
    ap.add_argument('--overlap', type=int, default=16)
    args = ap.parse_args()
    if args.what == 'export':
        export(args)
    else:
        import_(args)


if __name__ == '__main__':
    main()
