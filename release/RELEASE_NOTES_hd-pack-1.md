Ready-made HD graphics for the SherwoodEngineFix mod — **no AI processing on your PC needed**. For the **GOG edition**
of Robin Hood: The Legend of Sherwood; use them with the
[**newest version of the mod**](https://github.com/TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod/releases/latest).

> **New on 2026-11-22 (mod 1.3.0 or newer): the maps in native 4K** — `SherwoodHD-5-mapy-4k-a.zip` and
> `SherwoodHD-5-mapy-4k-b.zip` (about 1.2 GB each): all 20 map backgrounds at 4× the original (`dat2`). The game uses
> them where the screen shows the level larger than the HD picture — zoomed in beyond 2.0 (up to 4.0), and on 4K
> monitors already at 2.0. Optional; without them everything works as before.

> **New on 2026-11-22 (mod 1.2.9 or newer):** `SherwoodHD-4-klatki-posrednie-2.zip` (110 MB) — in-between frames of
> the level's own animations (pigeons, crows, ducks, hens, flags, smoke, flies…; what flies or swims also moves along
> a smooth path) and a second try for the characters' moves the first pack had to skip (about 11,400 frames). If you
> already have the pack, download just this one and unpack it into the game folder like the others.

> `SherwoodHD-3-klatki-posrednie.zip` was replaced on 2026-11-15 with **sharp in-between frames**. If you downloaded
> the older one, download it again and unpack it over the old files.

The HD files are the 10 `SherwoodHD-*.zip` archives under **Assets** below (0.1–1.7 GB each). "Source code (zip)" /
"Source code (tar.gz)" are added by GitHub to every release automatically — only the mod's source code, not needed
to play.

## Installation
1. Download the archives below — at least the three **required** ones (2.9 GB); everything is 8.9 GB.
2. Unpack **each** archive **into the game folder** (the folder with `Game.exe`, by default
   `C:\Program Files (x86)\GOG Galaxy\Games\Robin Hood - The Legend of Sherwood`). Windows: right-click → *Extract
   All…* → *Browse…* → the game folder. **Windows adds the archive's name to the path** — delete that part, so that
   the path ends with the game folder. The archives contain the `hd` folder; afterwards `hd\sprites\_common\` must
   hold `sprites.idx`, `sprites.dat` and `sprites.dat.2`, and `hd\maps\dat2\` the `.hdmap` files.
3. **Then** install the mod: unpack `SherwoodEngineFix-v….zip` from the
   [newest mod release](https://github.com/TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod/releases/latest) into the game
   folder — after the HD archives, as it carries newer versions of some small HD files.
4. Optional: [remade menu backgrounds](https://github.com/TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod/releases/tag/menu-remake-2).
5. Start the game. In a mission `GRAFIKA HD: DAT2` appears at the top for 3 s — everything is in place. F6 / F7 switch
   the background variant, F8 compares with the original, F9 turns the in-between animation frames on and off, F10
   opens the mod's settings. Keep the resolution at 2560x1440 on every monitor (the picture is scaled to the screen).

Full step-by-step guide and help: [README → Installation](https://github.com/TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod#installation--the-complete-hd-version-step-by-step),
[If it does not work](https://github.com/TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod#if-it-does-not-work).

| Archive | Contents | |
|---|---|---|
| `SherwoodHD-1-postacie-a.zip` + `SherwoodHD-1-postacie-b.zip` | characters and all their animations, icons and markers | **required** (both) |
| `SherwoodHD-2-tla-dat2.zip` | map backgrounds, roofs, trees, objects, menu backgrounds — variant `dat2` (default, most faithful) | **required** |
| `SherwoodHD-3-klatki-posrednie.zip` | in-between frames of character animations (2× smoother animation, F9) | recommended |
| `SherwoodHD-4-klatki-posrednie-2.zip` | in-between frames of the level's animations (birds, flags, smoke… with a smooth flight path) and a second try for the characters — needs mod 1.2.9 or newer | recommended |
| `SherwoodHD-5-mapy-4k-a.zip` + `SherwoodHD-5-mapy-4k-b.zip` | the maps in native 4K (`dat2`, 4× the original): sharp when zoomed in beyond 2.0 and on 4K monitors — needs mod 1.3.0 or newer (`HDMaps4k=1`) | optional (both) |
| `SherwoodHD-wariant-aomr.zip` | background variant `aomr` (contrasty, game-texture model) | optional |
| `SherwoodHD-wariant-sgi.zip` | background variant `sgi` (model for 90s pre-rendered game graphics) | optional |
| `SherwoodHD-wariant-ultrasharp.zip` | background variant `ultrasharp` (most added detail) | optional |

## What it is
The game's own pictures (all 20 maps, every sprite frame of the characters and objects, icons, menu backgrounds)
upscaled 2× with AI models (the maps also 4×), colours and brightness matched back to the original; the in-between frames were generated
with RIFE and checked automatically (pairs that failed the checks run as in the original game).

AI models: 4xNomos2_hq_dat2 and 2xAoMR_mosr by Phhofm (CC-BY-4.0), 4x SGI by ChrisNonyminus (GPL-3.0),
4x-UltraSharp by Kim2091 (CC-BY-NC-SA-4.0 — the `ultrasharp` variant and the icons: non-commercial use only),
RIFE by hzwer et al. (MIT).

Fan-made, non-commercial, for owners of the game. The original artwork belongs to its rights holders; this pack is
of no use without the game.

