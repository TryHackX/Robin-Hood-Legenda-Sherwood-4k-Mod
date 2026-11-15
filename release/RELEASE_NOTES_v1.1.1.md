## What's new in 1.1.1

Fixes from a player's report on 1.1.0 (smooth zoom and HD mode):

- **Zoom with the mouse wheel** (it zooms about the cursor): characters, roofs and the green hints no longer drift away
  from the background while zooming — the engine kept the old background until Alt+Tab.
- **Tooltips and messages over the magnified world**: no more "dotted" boxes and no "old map" under the letters. The
  game blended its 2× enlarged text once per 2×2 block, which turned the world under every letter into blocks; it is
  now blended pixel by pixel.
- **Zoom-outs started by the game itself** (scripted scenes that show you something) are followed — the picture no
  longer looks low-resolution until you touch the zoom.
- **Chimney smoke is back on HD backgrounds** (it was taken for water and left out). The game's smoke frames are tiny
  and heavily compressed and repaint the ground under them, so the smoke is drawn as its difference to the ground,
  blended over the HD picture and cross-faded between animation frames: a soft column of smoke, no blurry patch, no
  jumps.
- **Water**: new option `HDWater=2` (F10 panel → water) — the HD water with the shimmer of the game's own water
  animation on top. The default stays the still water of the HD picture; `HDWater=0` shows the game's frames as they are.
- **Performance**: the enlarged interface is no longer filtered again in every frame. At 2560×1440 a frame costs
  about 4.5 ms of processor time instead of 10 ms, so the game holds the monitor's refresh rate (the drops to about
  110 fps are gone).
- Fixed a crash when leaving a mission to the main menu.

## Installation
1. Unpack this archive and copy `winmm.dll` and `SherwoodFix.ini` into the game folder (next to `Game.exe`).
   Updating from 1.1.0: copying `winmm.dll` is enough (your `SherwoodFix.ini` keeps working).
2. For HD graphics download the archives of the **HD graphics pack** release and unpack them into the game folder
   (unchanged, no new download needed).
3. Start the game as usual. Mod settings in the game: **F10**.

Uninstall: delete `winmm.dll`. On a 1080p monitor set `Width=1920` and `Height=1080` in `SherwoodFix.ini`.
