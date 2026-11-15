## What's new in 1.1.6

- **Sharp in-between frames, 3x more of them.** The AI-made frames between two animation frames were soft wherever the
  two frames disagreed (the network mixed them), so only about a third of them were shown. They are now made anew so
  that every pixel comes from one of the two moved frames instead of their mix: as sharp as the HD frames, no ghosts,
  swords and spears kept better. All 320 646 accepted frames are shown (before: 100 722) - fights and fast moves too.
  **Download the new `SherwoodHD-3-klatki-posrednie.zip`** from the HD graphics pack 1 release (replaced 2026-11-15)
  and unpack it into the game folder. With the old pack the mod keeps showing only its clean frames.
- **Lists in HD**: the list of key bindings and the list of saved games (written by Windows in Arial) are drawn again
  twice as large and smoothed, like the rest of the menus.
- **Night and fog** checked: all night and fog maps have HD backgrounds; 17 small pictures the game makes while playing
  (lights at night and others) now have HD versions too (`hd\sprites\_icons\`, in this archive).

## Installation
1. HD graphics: the archives of the **HD graphics pack 1** release (the in-between pack `SherwoodHD-3-...` is new).
2. Then unpack `SherwoodEngineFix-v1.1.6.zip` into the game folder (next to `Game.exe`) - **after** the HD pack, as it
   carries newer small files: `winmm.dll`, `SherwoodFix.ini`, `hd\fonts\`, `hd\menu_parts\`, `hd\menu\<variant>\*.alias`,
   `hd\sprites\_icons\` and the quality lists `hd\sprites\_between\between.q` / `between.v1.q`. Updating from 1.1.x:
   everything except `SherwoodFix.ini` (yours keeps working).
3. Optional new menu backgrounds: `SherwoodEngineFix-menu-remake-1.zip` from the 1.1.5 release.
4. Start the game as usual. Mod settings in the game: **F10**.

Uninstall: delete `winmm.dll`. On a 1080p monitor set `Width=1920` and `Height=1080` in `SherwoodFix.ini`.
