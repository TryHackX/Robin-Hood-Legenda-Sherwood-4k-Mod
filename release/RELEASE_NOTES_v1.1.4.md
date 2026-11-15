## What's new in 1.1.4

- **HD font in the mission interface.** The game's fonts are small bitmaps (16 x 20 px cells); with the interface
  enlarged 2x every letter was magnified into blocks. The mod now recognises the game's font pictures by their
  content and draws the letters from versions made twice as large with an AI model (SGI) - tooltips, messages, the
  money counter, names: sharp letters with the same shape, colour and outline as the original.
  The HD fonts are small files in `hd\fonts\` and come with this archive.
- Fonts the mod has not seen yet (other missions, other situations) are collected while playing (only where the HD
  work folder `hd\work` exists) for the next update.
- Menus and dialogs (main menu, briefing, in-game menu) still show the original letters, enlarged with the rest of
  the dialog - that needs a separate text layer in the presenter (planned).

## Installation
1. Unpack this archive into the game folder (next to `Game.exe`): `winmm.dll`, `SherwoodFix.ini`,
   `hd\fonts\*.hdfont` and `hd\sprites\_between\between.q`. Updating from 1.1.x: everything except
   `SherwoodFix.ini` (yours keeps working).
2. For HD graphics download the archives of the **HD graphics pack** release and unpack them into the game folder
   (unchanged, no new download needed).
3. Start the game as usual. Mod settings in the game: **F10**.

Uninstall: delete `winmm.dll`. On a 1080p monitor set `Width=1920` and `Height=1080` in `SherwoodFix.ini`.
