SherwoodEngineFix - a modification for **Robin Hood: The Legend of Sherwood, GOG edition**: smooth play on today's
PCs (own DirectDraw + Direct3D 11 presentation, borderless window, Alt+Tab, extra frames between game ticks), HD mode
at 2560x1440 with HD graphics from AI-upscaled game files, 2x interface with HD fonts, full-screen menus in HD, smooth
zoom, in-between animation frames, F10 settings panel. Full description: [README](https://github.com/TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod#readme).

## Installation (short)
1. **GOG edition only** - Steam / CD editions and changed `Game.exe` files are not supported (the mod says so at start
   and stays off).
2. HD graphics: the archives of [HD graphics pack 1](https://github.com/TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod/releases/tag/hd-pack-1)
   (3 required) - unpack each **into the game folder** (next to `Game.exe`).
3. Then unpack `SherwoodEngineFix-v1.2.4.zip` (below) into the game folder - after the HD archives. Updating from an
   earlier version: unpack it over the old files and confirm replacing them; the HD packs stay as they are. Your
   `SherwoodFix.ini` can stay.
4. Optional: new menu backgrounds - [remade menu backgrounds 2](https://github.com/TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod/releases/tag/menu-remake-2).
5. Start the game. In a mission `GRAFIKA HD: DAT2` appears at the top for 3 s = everything is in place. F10 = settings.

**Windows "Extract All" adds the archive's name to the target path** - delete that part, the files must land right
next to `Game.exe`. Keep the resolution at 2560x1440 on every monitor (the picture is scaled to the screen). Not
working? See [If it does not work](https://github.com/TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod#if-it-does-not-work)
and attach `SherwoodFix.log` from the game folder.

## What's new in 1.2.4
- **The whole interface in HD**: every picture of the game's interface file (`DATA\Interface\Default.res`) - about
  940 of them, from the portraits on their parchments, action and item icons, buttons, seals and the hedge down to the
  smallest markers - has an AI-upscaled version in the mod archive (`hd\menu_parts\`). The 2x mission interface now
  draws them instead of smoothing the original pictures, and menus and dialogs use them as before. Pictures the game
  composes while playing (health, numbers) keep the smoothing.
- **Load / save screen after loading a game**: when the loaded game was on another level, the remade full-screen
  background of the load and save screens showed only in a box in the middle (since 1.2.2) - whole again.
- Checked on the saved games of a whole campaign: every level loads at your zoom, without a black band.
- The archive is larger because of the new pictures (see the size below).

## 1.2.3 (from a YouTuber's third report)
- "Start the chosen mission?" in the Sherwood camp: the tree tops and bushes behind the question are whole again, at
  any zoom; a question opened between two game ticks runs as a normal frame.
- The first briefing of a mission without a black band on the right; a new level starts at your zoom before its first
  frame.
- Portraits of the selected characters in an even row along the bottom - all of them.
- Loading a saved game or a mission: black screen as in the original.
- Campaign map in HD in every state of the campaign (castles, flags, path markers, the Sheriff's army arrows).
- Without object smoothing (`SmoothMotion=0`) the fixes of dialog backdrops were not active - they are now.

## Recent changes (1.2.0 - 1.2.2)
- 1.2.2: portraits evenly spaced, skills tooltip next to the pointer, black screen when loading, the Windows key works,
  recording with OBS explained in the README.
- 1.2.1: the camp's questions and the level behind them fixed; seals, campaign map and briefing portraits in HD;
  faster on wooded levels (140 instead of 86 frames a second); cursor speed as in Windows also on 1080p.
- 1.2.0: the hedge along the bottom is whole at every zoom; sword swings at every zoom; borderless window smaller than
  the screen (`WindowWidth`, `WindowHeight`, `WindowX`, `WindowY`).

Uninstall: delete `winmm.dll` (and the `hd` folder).
