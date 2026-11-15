## What's new in 1.1.2

- **Water with waves on HD backgrounds** (new default). The game's own water animation is a tiny, heavily compressed
  picture without any real waves - drawn over the HD background it looked like a mosaic, which is why 1.1.0 showed the
  still water of the HD picture. Now that still water is moved by slow waves: the reflections of trees and sky wobble
  and the light shifts with the waves, while the fine texture of the water stays sharp (only the smooth part of the
  picture moves). Reeds standing in a pond sway a little with it. The loop is prepared in the background when water
  comes into view (about half a second for a pond) and fades in; drawing it costs next to nothing.
- F10 panel → water: **waves** (default), **stronger waves**, **still HD water**, or **the game's own frames**
  (`HDWater=2 / 3 / 1 / 0` in `SherwoodFix.ini`).
- Chimney smoke as in 1.1.1 (over the HD picture, cross-faded between frames).

## Installation
1. Unpack this archive and copy `winmm.dll` and `SherwoodFix.ini` into the game folder (next to `Game.exe`).
   Updating from 1.1.x: copying `winmm.dll` is enough; to get the waves with your own `SherwoodFix.ini`, set
   `HDWater=2` there or choose it in the F10 panel.
2. For HD graphics download the archives of the **HD graphics pack** release and unpack them into the game folder
   (unchanged, no new download needed).
3. Start the game as usual. Mod settings in the game: **F10**.

Uninstall: delete `winmm.dll`. On a 1080p monitor set `Width=1920` and `Height=1080` in `SherwoodFix.ini`.
