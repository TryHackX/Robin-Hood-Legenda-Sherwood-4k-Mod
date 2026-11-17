SherwoodEngineFix - a modification for **Robin Hood: The Legend of Sherwood, GOG edition**: smooth play on today's
PCs (own DirectDraw + Direct3D 11 presentation, borderless window, Alt+Tab, extra frames between game ticks), HD mode
at 2560x1440 with HD graphics from AI-upscaled game files, 2x interface with HD fonts, full-screen menus in HD, smooth
zoom, in-between animation frames, F10 settings panel. Full description: [README](https://github.com/TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod#readme).

## Installation (short)
1. **GOG edition only** - Steam / CD editions and changed `Game.exe` files are not supported (the mod says so at start
   and stays off).
2. HD graphics: the archives of [HD graphics pack 1](https://github.com/TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod/releases/tag/hd-pack-1)
   (3 required) - unpack each **into the game folder** (next to `Game.exe`).
3. Then unpack `SherwoodEngineFix-v1.2.3.zip` (below) into the game folder - after the HD archives. Updating from an
   earlier version: unpack it over the old files and confirm replacing them; the HD packs stay as they are. Your
   `SherwoodFix.ini` can stay.
4. Optional: new menu backgrounds - [remade menu backgrounds 2](https://github.com/TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod/releases/tag/menu-remake-2).
5. Start the game. In a mission `GRAFIKA HD: DAT2` appears at the top for 3 s = everything is in place. F10 = settings.

**Windows "Extract All" adds the archive's name to the target path** - delete that part, the files must land right
next to `Game.exe`. Keep the resolution at 2560x1440 on every monitor (the picture is scaled to the screen). Not
working? See [If it does not work](https://github.com/TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod#if-it-does-not-work)
and attach `SherwoodFix.log` from the game folder.

## What's new in 1.2.3
From a YouTuber's third report (screenshots from 1.2.1 and their saved games - checked on those saves).
- **"Start the chosen mission?" in the Sherwood camp, zoomed out**: the tree tops and bushes behind the question came
  apart and pictures of two sizes overlapped. The game draws the camp once more for the question's backdrop, from
  inside the drawing of its own window; the mod took the translucent tree tops for parts of that window. The backdrop
  is now exactly the camp as it was on the screen - tree tops, hedge and all, at any zoom. A question that opened
  between two game ticks also runs as a normal frame now.
- **The first briefing of a mission** no longer shows a black band on the right (levels narrower than the screen), and
  the first picture of a new level is no longer shown small for a moment: the level starts at your zoom before its
  first frame.
- **Portraits of the selected characters**: an even row along the bottom without gaps - and all of them: with four
  selected, sometimes only three were shown (hidden portrait windows of the camp were counted along).
- **Loading a saved game or a mission**: black screen as in the original (checked again for both).
- **Campaign map in HD in every state of the campaign**: all five castles (drawn and coloured, with their glow), the
  flags, the path markers and the Sheriff's army arrows - taken straight from the game's interface file, so they are
  sharp also in states a playthrough had not shown yet.
- Without object smoothing (`SmoothMotion=0`) the fixes of dialog backdrops from 1.2.1 were not active - they are now.
- Audit: a 4-minute test with interface use - no errors, no warnings.

## Recent changes (1.1.9 - 1.2.2)
- 1.2.2: portraits evenly spaced, skills tooltip next to the pointer, black screen when loading, the Windows key works,
  recording with OBS explained in the README.
- 1.2.1: the camp's questions and the level behind them fixed; seals, campaign map and briefing portraits in HD;
  faster on wooded levels (140 instead of 86 frames a second); cursor speed as in Windows also on 1080p.
- 1.2.0: the hedge along the bottom is whole at every zoom; sword swings at every zoom; borderless window smaller than
  the screen (`WindowWidth`, `WindowHeight`, `WindowX`, `WindowY`).
- 1.1.9: no Visual C++ runtime needed; the GOG edition is checked at start; instructions rewritten.

Uninstall: delete `winmm.dll` (and the `hd` folder).
