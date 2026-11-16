SherwoodEngineFix - a modification for **Robin Hood: The Legend of Sherwood, GOG edition**: smooth play on today's
PCs (own DirectDraw + Direct3D 11 presentation, borderless window, Alt+Tab, extra frames between game ticks), HD mode
at 2560x1440 with HD graphics from AI-upscaled game files, 2x interface with HD fonts, full-screen menus in HD, smooth
zoom, in-between animation frames, F10 settings panel. Full description: [README](https://github.com/TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod#readme).

## Installation (short)
1. **GOG edition only** - Steam / CD editions and changed `Game.exe` files are not supported (the mod says so at start
   and stays off).
2. HD graphics: the archives of [HD graphics pack 1](https://github.com/TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod/releases/tag/hd-pack-1)
   (3 required) - unpack each **into the game folder** (next to `Game.exe`).
3. Then unpack `SherwoodEngineFix-v1.1.9.zip` (below) into the game folder - after the HD archives.
4. Optional: new menu backgrounds - [remade menu backgrounds 2](https://github.com/TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod/releases/tag/menu-remake-2).
5. Start the game. In a mission `GRAFIKA HD: DAT2` appears at the top for 3 s = everything is in place. F10 = settings.

**Windows "Extract All" adds the archive's name to the target path** (`…\Robin Hood - The Legend of Sherwood\SherwoodHD-…`)
- delete that part, the files must land right next to `Game.exe`. Keep the resolution at 2560x1440 on every monitor
(the picture is scaled to the screen; a width below 2048 switches the HD mode off). Not working? See
[If it does not work](https://github.com/TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod#if-it-does-not-work) and attach
`SherwoodFix.log` from the game folder.

## What's new in 1.1.9
- **No Visual C++ redistributable needed any more**: the C/C++ runtime is built into `winmm.dll`. Before, a PC without
  the 32-bit Visual C++ 2015-2022 runtime could not load the mod - the game did not start at all.
- **The edition is checked at start**: the mod compares the code of the routines it changes with the GOG `Game.exe`.
  Another edition or a changed file: a message (Polish / English), the mod stays off, the game runs as without it, and
  `SherwoodFix.log` names the file (size, time stamp). Before, such a game got some patches and not others.
- **A missing `SherwoodFix.ini` is written with the release's settings** (HD mode 2560x1440). Before, the mod wrote a
  short file with 1920x1080 - the HD mode stayed off for anyone who unpacked only `winmm.dll`.
- **Instructions rewritten**: the "Extract All" trap, the resolution (the old advice "1920x1080 on a 1080p monitor"
  switched the HD mode off - the picture is scaled to any monitor), a "If it does not work" section.

## Recent changes (1.1.5 - 1.1.8)
- 1.1.8: the question whether to quit shows the HD / remade main menu; credits in HD (and remade); credits run smoothly.
- 1.1.7: the options open at once (the game waited 1 s to measure the processor); calm idle animations and smoother
  long animation steps with in-between frames; sharper smooth zoom below 2x.
- 1.1.6: sharp in-between frames (3x more of them); lists (key bindings, saved games) in HD; night and fog checked.
- 1.1.5: menus and dialogs in HD (letters, buttons, parchments, seals, portraits); remade menu backgrounds; Esc menu
  opens in ~0.08 s.

Uninstall: delete `winmm.dll` (and the `hd` folder).
