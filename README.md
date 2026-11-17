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
  scaling by shader (sharp pixels / sharp bilinear / soft). Optionally a borderless window smaller than the screen
  (`WindowWidth`, `WindowHeight`, `WindowX`, `WindowY` in `SherwoodFix.ini`).
- Extra frames between game ticks (the game runs its logic 25 times per second): smooth map scrolling, smooth cursor,
  smooth movement of characters and projectiles — at the monitor's refresh rate; game speed unchanged.
- The enlarged interface is filtered once and kept (it was filtered again in every frame): a frame at 2560×1440 costs
  about 4.5 ms of processor time instead of 10 ms, so the game holds the monitor's refresh rate. HD characters,
  trees and objects are drawn 8 pixels at a time (SSE2): on wooded levels a frame takes ~7 ms instead of ~11 ms
  (140 instead of 86 frames a second).
- If the graphics driver resets (update, sleep, a GPU hiccup), Direct3D 11 comes back by itself after a few seconds.
- The Windows key works while playing (the game blocked it; `WindowsKey` in `SherwoodFix.ini`).
- Input fixes (high polling rate mice, mouse re-acquired after Alt+Tab), audio fixes (hearing range on wide screens),
  hang watchdog, removed thread deadlock when scrolling with the screen edge.

**Graphics**
- High resolutions in the game's own menu (buttons 1920×1080, 2560×1440, 3840×2160).
- **HD mode** (internal width 2048 or more): the game composes the picture at full resolution and shows the map at
  zoom 2.0 (framing of a 1280×720 screen) — interface, texts and cursor stay sharp; the map reaches the bottom edge.
- **HD map backgrounds**, **HD characters and objects**, **HD icons**, **HD menu backgrounds** — from pictures
  prepared with AI models (4 variants, switchable while playing), colours matched to the original. Download them as
  ready-made packs — see "HD graphics" below.
- Optional in-between frames for character animations (RIFE model) — animations 2× smoother. Since 2026-11-15 the
  frames are **sharp** (every pixel taken from one of the two moved frames instead of mixing them), so all accepted
  ones are shown — about 3× more transitions than before (`SherwoodHD-3-klatki-posrednie.zip`, updated in HD graphics
  pack 1; the quality lists for both the new and the first pack come with the mod archive).
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
- Mission interface 2× larger (each window grows from its own screen corner, clicks land where you see them), with an
  **HD font**: the letters of tooltips, messages, money and names come from upscaled versions of the game's own fonts
  (`hd\fonts\`, included in the mod archive) instead of being magnified 2×. The hedge along the bottom runs along the
  whole width with any number of characters; sword swings (hold the left button and draw) work at every zoom.
- Minimap in the top-right corner; dragging on it moves the camera, the map window is moved by its edges.
- Menus and game dialogs (briefing, Esc menu, save/load, options) shown full-screen instead of a small 1024×768 box
  in the middle — **in HD**: menu backgrounds upscaled by AI (HD pack), and the letters, buttons, the briefing's
  parchment, seals (also highlighted under the cursor), portraits and the campaign map drawn from HD versions
  (`hd\fonts\`, `hd\menu_parts\`, in the mod archive). Questions asked during a mission (mission won, start the chosen
  mission in the Sherwood camp) show the level behind them as it was on the screen. The portraits of several
  selected characters stay evenly spaced along the bottom; the skills tooltip of a merry man sits next to the pointer.
  Loading a saved game shows a black screen as in the original (no ragged black box over the menu background). Optional
  **remade menu backgrounds** that fill the whole widescreen (`hd\menu\remake\`, made with `tools\hd\menu_remake.py`
  from pictures painted by an image AI; F10 → "MENU W HD"). The in-game menu (Esc) opens in ~0.08 s instead of ~0.4 s.
- **In-game settings panel — F10** (HD graphics, interface, minimap, smooth zoom, menus, VSync, filter, FPS counter,
  cursor speed); changes apply at once and are saved to `SherwoodFix.ini`.
- The game's options screen shows the real processor name and memory size (it showed "GenuineIntel" and at most
  4095 MB).

## Requirements

- Robin Hood: The Legend of Sherwood from **GOG** (installed with GOG Galaxy or with the installer from gog.com). The
  mod is made for the code of exactly that `Game.exe` (2 899 968 bytes). **Other editions (Steam, CD / DVD releases)
  and changed `Game.exe` files (cracks, other patches) are not supported**: the mod then shows a message at start,
  stays off and the game runs as without it.
- Windows 10 or 11, a graphics card with Direct3D 11. Nothing else to install (since 1.1.9 the mod needs no Visual C++
  redistributable).

## Installation — the complete HD version, step by step

You need three things from the **Releases** page (right side of the repository page). Download the files listed
under **Assets** — not "Source code (zip)" / "Source code (tar.gz)": GitHub adds those two to every release by
itself, they hold only the source code and are not needed to play.
- [**the mod — newest version**](https://github.com/TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod/releases/latest):
  `SherwoodEngineFix-v….zip` (about 29 MB),
- [**HD graphics pack 1**](https://github.com/TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod/releases/tag/hd-pack-1):
  the HD pictures (7 archives, 3 of them required),
- optional: [**remade menu backgrounds 2**](https://github.com/TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod/releases/tag/menu-remake-2)
  (`SherwoodEngineFix-menu-remake-2.zip`, 54 MB).

**Step 1 — find the game folder.** The folder with `Game.exe`, by default
`C:\Program Files (x86)\GOG Galaxy\Games\Robin Hood - The Legend of Sherwood`
(in GOG Galaxy: the game → ⚙ → Manage installation → Show folder).

**How to unpack into the game folder (all steps below).** Right-click the archive → *Extract All…* → *Browse…* →
choose the game folder → *Extract*. **Careful: Windows adds the archive's name to the path** (for example
`…\Robin Hood - The Legend of Sherwood\SherwoodHD-1-postacie-a`) — delete that last part, so that the path ends
with the game folder itself. The archives already contain the folder `hd` and the files of the mod; they must land
**right next to `Game.exe`**, not in a folder of their own. 7-Zip ("Extract here") or WinRAR work too. If Windows asks
whether to replace files, answer yes.

**Step 2 — the HD pictures.** Download these archives from *HD graphics pack 1*:

| Archive | Download | Unpacked | Contents | |
|---|---|---|---|---|
| `SherwoodHD-1-postacie-a.zip` | 1.1 GB | 2.0 GB | characters and all their animations (part 1), icons | **required** |
| `SherwoodHD-1-postacie-b.zip` | 1.1 GB | 2.0 GB | characters and all their animations (part 2) | **required** |
| `SherwoodHD-2-tla-dat2.zip` | 0.7 GB | 1.1 GB | all 20 map backgrounds, roofs, trees, objects, menu backgrounds (default variant `dat2`) | **required** |
| `SherwoodHD-3-klatki-posrednie.zip` | 1.6 GB | 3.0 GB | in-between frames: character animations 2× smoother (F9); new sharp frames since 2026-11-15 | recommended |
| `SherwoodHD-wariant-aomr.zip` | 0.7 GB | 1.1 GB | another look of the backgrounds (F6 / F7) | optional |
| `SherwoodHD-wariant-sgi.zip` | 0.7 GB | 1.4 GB | another look of the backgrounds (F6 / F7) | optional |
| `SherwoodHD-wariant-ultrasharp.zip` | 0.7 GB | 1.1 GB | another look of the backgrounds (F6 / F7) | optional |

Required only: 2.9 GB to download, 5.1 GB on disk. Everything: 6.4 GB to download, 11.6 GB on disk.

Unpack **each** archive into the game folder (see above). Afterwards the game folder holds `hd\maps\…`,
`hd\sprites\…` and `hd\menu\…`. Check: `hd\sprites\_common\` must contain `sprites.idx`, `sprites.dat` **and**
`sprites.dat.2` (from the two character archives). The downloaded `.zip` files can be deleted afterwards.

**Step 3 — the mod (after the HD pictures).** Unpack `SherwoodEngineFix-v….zip` (the newest release) into the game
folder — **after** the HD archives: it carries newer versions of some small HD files of the pack. Afterwards
`winmm.dll` and `SherwoodFix.ini` lie **right next to `Game.exe`**. (The archive holds: `winmm.dll`, `SherwoodFix.ini`,
`README.md`, the HD fonts `hd\fonts\`, HD pictures of menus and dialogs `hd\menu_parts\`, run-time icons
`hd\sprites\_icons\`, small files and the credits' background in `hd\menu\<variant>\`, and the lists of the in-between
frames `hd\sprites\_between\between.q`, `between.v1.q`, `between.t`.) Updating from an older version: unpack the new
archive over it; keep your `SherwoodFix.ini` if you changed settings (answer "skip" for that one file).

**Step 4 (optional) — new menu backgrounds.** `SherwoodEngineFix-menu-remake-2.zip` from
[remade menu backgrounds 2](https://github.com/TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod/releases/tag/menu-remake-2):
six backgrounds (main menu, options, sound and graphics options, load / save, the parchment screen and the credits)
painted anew by an image AI after the original ones, filling the whole widescreen screen; the question whether to
quit shows the main menu's. Unpack into the game folder (it adds `hd\menu\remake\`). In the game: F10 →
"MENU W HD (TLA, NAPISY)" switches between the new backgrounds, the upscaled originals and the original menus.

**Step 5 — play.** Start the game as usual (GOG Galaxy or the shortcut). At the start of a mission the name of the HD
variant appears at the top of the screen for 3 s (for example `GRAFIKA HD: DAT2`) — that is the sign that everything
is in place. F8 compares with the original graphics, F10 opens the mod's settings.

**Monitor resolution — leave it as it is.** The game renders at 2560×1440 (HD mode) and the picture is scaled to your
screen, whatever its resolution (1080p, 1440p, 4K, other aspect ratios with black bars). Do **not** set a smaller
`Width` in `SherwoodFix.ini` and do not choose 1920×1080 in the game's own Options → Graphics: below a width of 2048
the HD mode (HD graphics, 2× interface, smooth zoom) switches off.
Every option is described in the INI file itself (comments in Polish); the most important ones can be changed in the
F10 panel.

**Uninstall:** delete `winmm.dll` and the `hd` folder (and optionally `SherwoodFix.ini`, `SherwoodFix.log`).

## If it does not work

1. **Start the game once, then look into the game folder: is there a file `SherwoodFix.log`?**
   - **No** — the mod is not loaded. `winmm.dll` is not right next to `Game.exe` (most often it was unpacked into a
     folder of its own, see "How to unpack" above), or an antivirus program removed it (the mod is an unsigned DLL
     that changes the game's code in memory; restore it from the quarantine and allow it).
   - **Yes** — open it: the first lines say what the mod found.
2. **A message "this Game.exe is not the GOG edition"** (log: `Game.exe is not the GOG edition`) — the game is not the
   GOG edition, or `Game.exe` was changed by another program. The mod works only with the original GOG `Game.exe`
   (2 899 968 bytes); reinstall the game from GOG.
3. **The game starts, but looks like the original** (small picture, no HD): check that `Width=2560` and `Height=1440`
   are in `SherwoodFix.ini` (or at least a width of 2048; the game's Options → Graphics → 2560*1440 sets it back), and
   that the `hd` folder lies next to `Game.exe` (not
   `hd\hd\…` or `SherwoodHD-…\hd\…`): `hd\maps\dat2\` must hold `.hdmap` files and `hd\sprites\_common\` the files
   named in step 2. F8 must not be switched to "ORYGINAL".
4. **When asking for help, attach `SherwoodFix.log`** from the game folder (it is written anew at every start).

## Recording and streaming (OBS)

The GOG installer marks `game.exe` to **run as administrator** (a Windows compatibility setting — that is why Windows
asks for permission when the game starts). OBS started normally cannot hook into a program running as administrator,
so **Game Capture stays black**. Either:
- in OBS use **Window Capture** of the game window with the capture method **"Windows 10 (1903 and up)"** — it records
  only the game window (not the desktop), or
- start **OBS as administrator** as well, then Game Capture works.

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
  2.0 the original pixels are shown (HD pictures need 2.0; since 1.1.7 they are enlarged with a sharper filter), above
  2.0 the HD picture is magnified further.
- On small levels (smaller than the screen at zoom 1.0) the very first briefing of a mission shows the level behind
  it at its own size with black bands; from the first moment of play the level fills the screen as usual.

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
