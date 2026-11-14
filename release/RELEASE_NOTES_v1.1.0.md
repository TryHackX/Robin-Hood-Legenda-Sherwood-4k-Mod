## What's new in 1.1.0

- **Smooth zoom (magnifier)** — zoom in and out in small steps (game keys, mouse wheel, the buttons in the corner),
  from "the whole level fills the screen" (no more black band on narrow levels such as Nottingham) up to 4× closer.
  Only the world is magnified, the interface keeps its size; clicks land where you see them and the view reaches every
  edge of the level. Settings in the F10 panel: on/off, step size (110–200 %), largest zoom (3×–8×).
- **Minimap**: dragging on its picture moves the camera; the map window is dragged by its edges with the left or the
  right button and follows the cursor smoothly; a right click on it still closes it.
- **Briefing with several pages**: from the second page the parchment stayed small and the text spilled over the
  background — fixed.
- **Options screen** shows the real processor name and memory size (instead of "GenuineIntel" and 4095 MB).
- HD stores may come in parts (`sprites.dat`, `sprites.dat.2`, …) — needed for the HD graphics pack.
- README in English.

## Installation
1. Unpack this archive and copy `winmm.dll` and `SherwoodFix.ini` into the game folder (next to `Game.exe`).
2. For HD graphics download the archives of the **HD graphics pack** release and unpack them into the game folder.
3. Start the game as usual. Mod settings in the game: **F10**.

Uninstall: delete `winmm.dll`. On a 1080p monitor set `Width=1920` and `Height=1080` in `SherwoodFix.ini`.
