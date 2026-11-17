SherwoodEngineFix - a modification for **Robin Hood: The Legend of Sherwood, GOG edition**: smooth play on today's
PCs (own DirectDraw + Direct3D 11 presentation, borderless window, Alt+Tab, extra frames between game ticks), HD mode
at 2560x1440 with HD graphics from AI-upscaled game files, 2x interface with HD fonts, full-screen menus in HD, smooth
zoom, in-between animation frames, F10 settings panel. Full description: [README](https://github.com/TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod#readme).

## Installation (short)
1. **GOG edition only** - Steam / CD editions and changed `Game.exe` files are not supported (the mod says so at start
   and stays off).
2. HD graphics: the archives of [HD graphics pack 1](https://github.com/TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod/releases/tag/hd-pack-1)
   (3 required) - unpack each **into the game folder** (next to `Game.exe`).
3. Then unpack `SherwoodEngineFix-v1.2.2.zip` (below) into the game folder - after the HD archives. Updating from an
   earlier version: unpack it over the old files; the HD packs stay as they are. Keep your `SherwoodFix.ini` if you
   changed settings (the new option below works without a new line).
4. Optional: new menu backgrounds - [remade menu backgrounds 2](https://github.com/TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod/releases/tag/menu-remake-2).
5. Start the game. In a mission `GRAFIKA HD: DAT2` appears at the top for 3 s = everything is in place. F10 = settings.

**Windows "Extract All" adds the archive's name to the target path** - delete that part, the files must land right
next to `Game.exe`. Keep the resolution at 2560x1440 on every monitor (the picture is scaled to the screen). Not
working? See [If it does not work](https://github.com/TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod#if-it-does-not-work)
and attach `SherwoodFix.log` from the game folder.

## What's new in 1.2.2
From a YouTuber's second report (screenshots and a video).
- **Portraits of several selected characters**: the game spreads them evenly over the whole width; they grew from the
  screen's thirds and the last two touched. Each now grows from its own middle - evenly spaced, as the game put them.
  (A bookkeeping error since 1.0.0 also made the layout depend on earlier frames - fixed.)
- **Skills tooltip in the camp** (hover over a merry man): it sometimes flickered or appeared away from the pointer
  (it kept the place it had first appeared at). It now follows the pointer and sits to its right, as in the original.
- **Loading a saved game**: a black screen as in the original, no ragged black box over the menu background.
- **The Windows key works while playing** (the game blocked it - only Alt+Tab got out). `WindowsKey=0` in
  `SherwoodFix.ini` brings back the old behaviour.
- **Recording with OBS**: the GOG installer marks the game to run as administrator, so OBS's Game Capture stays black.
  Use Window Capture with the method "Windows 10 (1903 and up)" (only the game window is recorded), or start OBS as
  administrator - see [Recording and streaming](https://github.com/TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod#recording-and-streaming-obs).
- The "start the chosen mission?" question in the Sherwood camp was already fixed in 1.2.1 (the parchment squares
  behind the cursor in the report's video were recorded with 1.2.0).
- Audit: a 4-minute test with interface use - no errors, no warnings, ~143 frames a second.

## Recent changes (1.1.8 - 1.2.1)
- 1.2.1: the camp's questions and the level behind them fixed; seals, campaign map and briefing portraits in HD;
  faster on wooded levels (140 instead of 86 frames a second); cursor speed as in Windows also on 1080p.
- 1.2.0: the hedge along the bottom is whole at every zoom; sword swings at every zoom; borderless window smaller than
  the screen (`WindowWidth`, `WindowHeight`, `WindowX`, `WindowY`).
- 1.1.9: no Visual C++ runtime needed; the GOG edition is checked at start; instructions rewritten.
- 1.1.8: the question whether to quit shows the HD / remade main menu; credits in HD (and remade).

Uninstall: delete `winmm.dll` (and the `hd` folder).
