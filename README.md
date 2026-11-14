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
- Input fixes (high polling rate mice, mouse re-acquired after Alt+Tab), audio fixes (hearing range on wide screens),
  hang watchdog, removed thread deadlock when scrolling with the screen edge.

**Graphics**
- High resolutions in the game's own menu (buttons 1920×1080, 2560×1440, 3840×2160).
- **HD mode** (internal width 2048 or more): the game composes the picture at full resolution and shows the map at
  zoom 2.0 (framing of a 1280×720 screen) — interface, texts and cursor stay sharp; the map reaches the bottom edge.
- **HD map backgrounds**, **HD characters and objects**, **HD icons**, **HD menu backgrounds** — from pictures
  prepared with AI models (4 variants, switchable while playing), colours matched to the original. These pictures are
  NOT part of the repository — see "HD graphics" below.
- Optional in-between frames for character animations (RIFE model) — animations 2× smoother.

**Interface**
- Mission interface 2× larger (each window grows from its own screen corner, clicks land where you see them).
- Minimap in the top-right corner; dragging on it moves the camera, the map window is moved by its edges.
- Menus and game dialogs (briefing, Esc menu, save/load, options) shown full-screen instead of a small 1024×768 box
  in the middle.
- **In-game settings panel — F10** (HD graphics, interface, minimap, menus, VSync, filter, FPS counter, cursor
  speed); changes apply at once and are saved to `SherwoodFix.ini`.
- The game's options screen shows the real processor name and memory size (it showed "GenuineIntel" and at most
  4095 MB).

## Requirements

- Robin Hood: The Legend of Sherwood from **GOG** (the mod recognises the code of that `Game.exe`; other editions are
  not supported — the patches are then not applied, and the log says so).
- Windows 10 or 11, a graphics card with Direct3D 11.

## Installation

1. Download the archive from **Releases**.
2. Copy `winmm.dll` and `SherwoodFix.ini` into the game folder (where `Game.exe` is).
3. Start the game as usual (GOG Galaxy or the shortcut).

Uninstall: delete `winmm.dll` (and optionally `SherwoodFix.ini`, `SherwoodFix.log`).

By default the game renders at 2560×1440 (HD mode). On a 1080p monitor set `Width=1920`, `Height=1080` in
`SherwoodFix.ini` (or `0` and `0` = the monitor's resolution). Every option is described in the INI file itself
(comments in Polish); the most important ones can be changed in the F10 panel.

## Mod keys

| Key | Action |
|---|---|
| F10 | mod settings panel (in menus, game dialogs and missions) |
| F8 | HD background ↔ original (quick comparison) |
| F7 / F6 | next / previous HD graphics variant |
| F9 | in-between frames of character animations (when generated) |

## HD graphics

The HD pictures are made from the original game files, so they are **not distributed** (the game's artwork is
copyrighted). Anyone can make them from their own copy of the game with the tools in `tools\hd\` (Python 3 +
PyTorch, an NVIDIA card recommended):

- `extract_maps.py`, `upscale_variants.py` — map backgrounds (variants: `dat2`, `sgi`, `ultrasharp`, `aomr`),
- `extract_sprites.py`, `upscale_sprites.py`, `upscale_icons.py` — characters, objects, icons,
- `upscale_menu.py` — menu backgrounds (the mod collects them into `hd\work\menu_frames\` by itself),
- `make_inbetweens.py` + `rife_net.py` — in-between animation frames.

The AI models have to be downloaded from their authors (licences and sources: `docs\UPSCALE_MODELS.md`). The layout
of the `hd\` folder is described in `hd\CZYTAJ.txt`, the whole HD mode in `docs\HD_MODE.md` (documentation is in
Polish). Without HD pictures the mod works normally (original graphics, sharply enlarged).

## Known limitations

- The game engine has only three zoom levels (0.5 / 1 / 2). On levels narrower than 2560 px (e.g. Nottingham),
  zooming out to 1.0 leaves a black band on the right. A smooth zoom (magnifier in the presenter) is in progress.
- HD graphics work at zoom 2.0; at 1.0 the original pixels are shown.
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
