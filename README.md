# Robin Hood: The Legend of Sherwood — 4K / HD mod (SherwoodEngineFix)

An unofficial modification for **Robin Hood: The Legend of Sherwood** (Spellbound, 2002), **GOG** edition. The mod is
a single `winmm.dll` placed next to `Game.exe` — the game loads it by itself, no game files are changed.

Goal: smooth and sharp gameplay on today's PCs and monitors (1080p, 1440p, 4K, 144 Hz), without the stutter of the
original DirectDraw wrapper, plus optional HD graphics prepared with AI models from the original game files.

## Features

**Performance and stability**
- Own in-memory DirectDraw + Direct3D 11 presentation (flip model) instead of the slow wrapper — no more lag in
  fights and while scrolling the camera.
- Borderless full-screen window, working Alt+Tab (the game no longer freezes after switching back), VSync, image
  scaling by shader (sharp pixels / sharp bilinear / soft).
- Extra frames between game ticks (the game runs its logic 25 times per second): smooth map scrolling, smooth cursor,
  smooth movement of characters and projectiles — at the monitor's refresh rate; game speed unchanged.
- The enlarged interface is filtered once and kept (it was filtered again in every frame): a frame at 2560×1440 costs
  about 4.5 ms of processor time instead of 10 ms, so the game holds the monitor's refresh rate.
- Input fixes (high polling rate mice, mouse re-acquired after Alt+Tab), audio fixes (hearing range on wide screens),
  hang watchdog, removed thread deadlock when scrolling with the screen edge.

**Graphics**
- High resolutions in the game's own menu (buttons 1920×1080, 2560×1440, 3840×2160).
- **HD mode** (internal width 2048 or more): the game composes the picture at full resolution and shows the map at
  zoom 2.0 (framing of a 1280×720 screen) — interface, texts and cursor stay sharp; the map reaches the bottom edge.
- **HD map backgrounds**, **HD characters and objects**, **HD icons**, **HD menu backgrounds** — from pictures
  prepared with AI models (4 variants, switchable while playing), colours matched to the original. Download them as
  ready-made packs — see "HD graphics" below.
- Optional in-between frames for character animations (RIFE model) — animations 2× smoother. Only the clean ones are
  shown (`HDInBetweenQuality`, quality list `hd\sprites\_between\between.q` included in the mod archive); fast moves
  whose generated frames came out soft run as in the original.
- **Smooth zoom** (magnifier): zoom in and out in small steps (game keys, mouse wheel — about the cursor, the buttons
  in the corner), from "the whole level fills the screen" (no black bands) up to 4× closer. The engine renders at 1.0
  or 2.0 (HD pictures) and the graphics card magnifies the rest — only the world, the interface keeps its size; clicks
  land where you see them, the view reaches every edge of the level. Zooms started by the game itself (scripted
  scenes) are followed.
- **Water with waves and chimney smoke on HD backgrounds**: the game's water and smoke animations are tiny, heavily
  compressed pictures that repaint the ground under them. The water of the HD picture is moved by slow waves instead —
  reflections wobble and the light shifts with the waves, while the fine texture stays sharp (only the smooth part of
  the picture moves); the loop is prepared in the background when water comes into view. The smoke is drawn as its
  difference to the ground, blended over the HD picture and cross-faded between animation frames (no blurry patch, no
  jumps). F10 / `HDWater`: waves, stronger waves, still HD water, or the game's own frames.

**Interface**
- Mission interface 2× larger (each window grows from its own screen corner, clicks land where you see them).
- Minimap in the top-right corner; dragging on it moves the camera, the map window is moved by its edges.
- Menus and game dialogs (briefing, Esc menu, save/load, options) shown full-screen instead of a small 1024×768 box
  in the middle.
- **In-game settings panel — F10** (HD graphics, interface, minimap, smooth zoom, menus, VSync, filter, FPS counter,
  cursor speed); changes apply at once and are saved to `SherwoodFix.ini`.
- The game's options screen shows the real processor name and memory size (it showed "GenuineIntel" and at most
  4095 MB).

## Requirements

- Robin Hood: The Legend of Sherwood from **GOG** (the mod recognises the code of that `Game.exe`; other editions are
  not supported — the patches are then not applied, and the log says so).
- Windows 10 or 11, a graphics card with Direct3D 11.

## Installation — the complete HD version, step by step

You need two releases from the **Releases** page (right side of the repository page):
[**SherwoodEngineFix 1.1.3**](https://github.com/TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod/releases/tag/v1.1.3)
(the mod itself, 0.2 MB) and
[**HD graphics pack 1**](https://github.com/TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod/releases/tag/hd-pack-1)
(the HD pictures, 7 archives). Download the files listed under **Assets** — not "Source code (zip)" / "Source code
(tar.gz)": GitHub adds those two to every release by itself, they hold only the source code (~0.4 MB) and are not
needed to play.

**Step 1 — find the game folder.** The folder with `Game.exe`, by default
`C:\Program Files (x86)\GOG Galaxy\Games\Robin Hood - The Legend of Sherwood`
(in GOG Galaxy: the game → ⚙ → Manage installation → Show folder).

**Step 2 — the mod.** Unpack `SherwoodEngineFix-v1.1.3.zip` into the game folder (next to `Game.exe`): `winmm.dll`,
`SherwoodFix.ini` and the small `hd\sprites\_between\between.q` (the quality list of the in-between frames).

**Step 3 — the HD pictures.** Download these archives from *HD graphics pack 1*:

| Archive | Download | Unpacked | Contents | |
|---|---|---|---|---|
| `SherwoodHD-1-postacie-a.zip` | 1.1 GB | 2.0 GB | characters and all their animations (part 1), icons | **required** |
| `SherwoodHD-1-postacie-b.zip` | 1.1 GB | 2.0 GB | characters and all their animations (part 2) | **required** |
| `SherwoodHD-2-tla-dat2.zip` | 0.7 GB | 1.1 GB | all 20 map backgrounds, roofs, trees, objects, menu backgrounds (default variant `dat2`) | **required** |
| `SherwoodHD-3-klatki-posrednie.zip` | 1.6 GB | 3.0 GB | in-between frames: character animations 2× smoother (F9) | recommended |
| `SherwoodHD-wariant-aomr.zip` | 0.7 GB | 1.1 GB | another look of the backgrounds (F6 / F7) | optional |
| `SherwoodHD-wariant-sgi.zip` | 0.7 GB | 1.4 GB | another look of the backgrounds (F6 / F7) | optional |
| `SherwoodHD-wariant-ultrasharp.zip` | 0.7 GB | 1.1 GB | another look of the backgrounds (F6 / F7) | optional |

Required only: 2.9 GB to download, 5.1 GB on disk. Everything: 6.4 GB to download, 11.6 GB on disk.

Unpack **each** archive **into the game folder** — in Windows: right-click the archive → *Extract All…* → choose the
game folder → *Extract* (7-Zip / WinRAR work too). The archives contain the `hd` folder, so afterwards the game folder
holds `hd\maps\…`, `hd\sprites\…` and `hd\menu\…`. Check: `hd\sprites\_common\` must contain `sprites.idx`,
`sprites.dat` **and** `sprites.dat.2` (from the two character archives). If Windows asks to replace files, answer yes.
The downloaded `.zip` files can be deleted afterwards.

**Step 4 — play.** Start the game as usual (GOG Galaxy or the shortcut). At the start of a mission the name of the HD
variant appears at the top of the screen for 3 s. F8 compares with the original graphics, F10 opens the mod's
settings.

**Monitor resolution.** By default the game renders at 2560×1440 (HD mode). On a 1080p monitor set `Width=1920`,
`Height=1080` in `SherwoodFix.ini` (or `0` and `0` = the monitor's resolution); on a 4K monitor 2560×1440 is
recommended (sharp HD pictures, scaled up). Every option is described in the INI file itself (comments in Polish);
the most important ones can be changed in the F10 panel.

**Uninstall:** delete `winmm.dll` and the `hd` folder (and optionally `SherwoodFix.ini`, `SherwoodFix.log`).

## Mod keys

| Key | Action |
|---|---|
| F10 | mod settings panel (in menus, game dialogs and missions) |
| F8 | HD background ↔ original (quick comparison) |
| F7 / F6 | next / previous HD graphics variant |
| F9 | in-between frames of character animations (when generated) |

## HD graphics — how they were made

The HD packs (installation above) need mod version 1.1.0 or newer (the characters' store comes in two parts). Without
HD pictures the mod works normally (original graphics, sharply enlarged).

The pictures were made from the original game files with the tools in `tools\hd\` (Python 3 + PyTorch), which can
also rebuild them from your own copy of the game:

- `extract_maps.py`, `upscale_variants.py` — map backgrounds (variants: `dat2`, `sgi`, `ultrasharp`, `aomr`),
- `extract_sprites.py`, `upscale_sprites.py`, `upscale_icons.py` — characters, objects, icons,
- `upscale_menu.py` — menu backgrounds (the mod collects them into `hd\work\menu_frames\` by itself),
- `make_inbetweens.py` + `rife_net.py` — in-between animation frames,
- `tools\make_hd_packs.py` — packs them into the archives above.

AI models used (they are not included; licences and sources: `docs\UPSCALE_MODELS.md`): 4xNomos2_hq_dat2 and
2xAoMR_mosr by Phhofm (CC-BY-4.0), 4x SGI by ChrisNonyminus (GPL-3.0), 4x-UltraSharp by Kim2091 (CC-BY-NC-SA-4.0 —
the `ultrasharp` variant and the icons are for non-commercial use), RIFE by hzwer et al. (MIT). The layout of the
`hd\` folder is described in `hd\CZYTAJ.txt`, the whole HD mode in `docs\HD_MODE.md` (documentation is in Polish).

## Known limitations

- The engine itself has only three zoom levels (0.5 / 1 / 2); the smooth zoom magnifies its 1.0 or 2.0 picture. Below
  2.0 the original pixels are shown (HD pictures need 2.0), above 2.0 the HD picture is magnified further.
- Night levels and fog have not been checked with HD graphics yet.

## Building from source

Visual Studio 2022 or newer (C++, CMake), configuration **Win32 / Release**. MinHook is fetched by CMake
(FetchContent).

```
cmake -S src -B src/build -A Win32
cmake --build src/build --config Release
```

Output: `src\build\Release\winmm.dll`. The script `tools\deploy.ps1` builds and copies the library into the game
folder; `tools\make_release.ps1` packs the release archive.

## Documentation (in Polish)

- `docs\PLAN.md` — plan, course of work and test results,
- `docs\HD_MODE.md` — HD mode, 2× interface, menus, F10 panel, background test mode,
- `docs\RE_NOTES.md`, `docs\DDRAW_USAGE.md`, `docs\OBJECT_RENDER.md`, `docs\ZOOM_CAMERA.md`, `docs\HUD_STRIP.md`,
  `docs\SPRITE_BANK.md` — verified addresses and structure of `Game.exe` (partly in English),
- `tools\` — disassembler helper (`rh.py`), control of a hidden game instance for background tests, HD tools.

## Disclaimer

A fan project, not affiliated with Spellbound Entertainment, GOG or the rights holders of the game. The repository
contains no game files and no graphics derived from them. You need a legal copy of the game. Use at your own risk —
back up your save games.
