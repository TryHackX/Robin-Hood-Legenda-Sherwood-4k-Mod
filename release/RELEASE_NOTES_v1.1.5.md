## What's new in 1.1.5

- **Menus and dialogs in HD.** With the full-screen menus the game draws its menus at their original size and the mod
  enlarged them - letters, buttons and pictures were blurry. Now everything the game draws there is drawn a second
  time from HD versions: the letters of all menu fonts, the buttons (all states), the briefing's parchment, the wax
  seals and the portraits - in the main menu, the options, save/load, the in-game menu (Esc) and the briefing. The HD
  pictures are small files in `hd\fonts\` and `hd\menu_parts\` and come with this archive.
- **Smooth black outline of the HD font** (the money and portrait-name letters had pixelated dark edges).
- **The in-game menu opens without the hitch**: ~0.08 s instead of ~0.4 s. The game colours the level behind its
  dialogs green pixel by pixel with slow floating-point code (290 ms at 2560x1440); the mod now does the same through
  a table of all colours - checked to give exactly the same picture.
- HD menu backgrounds also in menus opened during a mission (options, save/load from the in-game menu), and a fix for
  dots of the level showing through those screens.
- **Remade menu backgrounds** (optional): new pictures that fill the whole widescreen instead of the old picture with
  black bars. `tools\hd\menu_remake.py export` prepares the backgrounds, layout screenshots and prompts for an image
  AI; `import` fits the painted pictures and puts them into `hd\menu\remake\`. F10 -> "MENU W HD (TLA, NAPISY)" chooses
  between the original, HD and remade backgrounds (`HDMenuRemake=1` in `SherwoodFix.ini`).

## Installation
1. Unpack this archive into the game folder (next to `Game.exe`): `winmm.dll`, `SherwoodFix.ini`, `hd\fonts\*.hdfont`,
   `hd\menu_parts\*.hdp`, `hd\menu\<variant>\*.alias` and `hd\sprites\_between\between.q`. Updating from 1.1.x:
   everything except `SherwoodFix.ini` (yours keeps working; the new option `HDMenuRemake` is on by default).
2. For HD graphics download the archives of the **HD graphics pack** release and unpack them into the game folder
   (unchanged, no new download needed).
3. Start the game as usual. Mod settings in the game: **F10**.

Uninstall: delete `winmm.dll`. On a 1080p monitor set `Width=1920` and `Height=1080` in `SherwoodFix.ini`.
