## What's new in 1.1.8

- **The question whether to quit shows the HD menu.** Behind the "Quit the game?" parchment the game shows the main
  menu - so far as the old picture at its original size, because the mod did not recognise the menu with a dialog
  drawn over it. Now a screen the mod has no picture of, which is mostly the last menu screen with something drawn
  over it, keeps that menu's HD or remade picture everywhere except where the dialog lies. This works for the other
  dialogs over menu screens too.
- **Credits in HD.** The credits scroll their text over a forest picture; the mod now finds that picture by the fixed
  surface the game copies it from every frame and shows its HD version - or a remade one - under the text. The HD
  versions of this picture for all four background variants are in this archive (`hd\menu\<variant>\2ef7a848bc09b885.mhd`).
- **Credits run smoothly**: 70-90 frames a second (before 40-50) - per frame only the changed column of text is
  compared, the HD picture is uploaded to the graphics card once.
- **New remade backgrounds pack** `SherwoodEngineFix-menu-remake-2.zip` (below): the five menu backgrounds of pack 1
  plus a new one for the credits - six in all, painted anew by an image AI after the originals, filling the whole
  widescreen screen. Replaces pack 1.
- `tools\hd\menu_remake.py import --only 06` imports one screen of remade backgrounds; screen `06_tworcy` = the credits.

## Installation
1. HD graphics: the archives of the **HD graphics pack 1** release (nothing new there).
2. Then unpack `SherwoodEngineFix-v1.1.8.zip` into the game folder (next to `Game.exe`) - **after** the HD pack, as it
   carries newer files: `winmm.dll`, `SherwoodFix.ini`, `hd\fonts\`, `hd\menu_parts\`, `hd\menu\<variant>\` (aliases and
   the credits' background), `hd\sprites\_icons\` and `hd\sprites\_between\between.q` / `between.v1.q` / `between.t`.
   Updating from 1.1.x: everything except `SherwoodFix.ini` (yours keeps working).
3. Optional new menu backgrounds: `SherwoodEngineFix-menu-remake-2.zip` from this release (unpack into the game folder;
   F10 -> "MENU W HD" -> "TAK, TLA Z REMAKE GDY SA").
4. Start the game as usual. Mod settings in the game: **F10**.

Uninstall: delete `winmm.dll`. On a 1080p monitor set `Width=1920` and `Height=1080` in `SherwoodFix.ini`.
