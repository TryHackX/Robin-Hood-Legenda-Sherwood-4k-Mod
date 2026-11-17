SherwoodEngineFix - a modification for **Robin Hood: The Legend of Sherwood, GOG edition**: smooth play on today's
PCs (own DirectDraw + Direct3D 11 presentation, borderless window, Alt+Tab, extra frames between game ticks), HD mode
at 2560x1440 with HD graphics from AI-upscaled game files, 2x interface with HD fonts, full-screen menus in HD, smooth
zoom, in-between animation frames, F10 settings panel. Full description: [README](https://github.com/TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod#readme).

## Installation (short)
1. **GOG edition only** - Steam / CD editions and changed `Game.exe` files are not supported (the mod says so at start
   and stays off).
2. HD graphics: the archives of [HD graphics pack 1](https://github.com/TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod/releases/tag/hd-pack-1)
   (3 required) - unpack each **into the game folder** (next to `Game.exe`).
3. Then unpack `SherwoodEngineFix-v1.2.1.zip` (below) into the game folder - after the HD archives. Updating from an
   earlier version: unpack it over the old files; the HD packs stay as they are. Keep your `SherwoodFix.ini` if you
   changed settings.
4. Optional: new menu backgrounds - [remade menu backgrounds 2](https://github.com/TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod/releases/tag/menu-remake-2).
5. Start the game. In a mission `GRAFIKA HD: DAT2` appears at the top for 3 s = everything is in place. F10 = settings.

**Windows "Extract All" adds the archive's name to the target path** - delete that part, the files must land right
next to `Game.exe`. Keep the resolution at 2560x1440 on every monitor (the picture is scaled to the screen). Not
working? See [If it does not work](https://github.com/TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod#if-it-does-not-work)
and attach `SherwoodFix.log` from the game folder.

## What's new in 1.2.1
A full audit of the mod's code and a play-through of the campaign up to the Sherwood camp in the background test.
- **The "freeze" after choosing a mission in the Sherwood camp is fixed.** The question "start the chosen mission?"
  (and "mission won - leave the level?") is opened by a window of the mission while the mission's interface is being
  drawn 2x larger - the question was enlarged along with it: the parchment appeared twice, and every move of the cursor
  left a square of parchment behind ("you can draw with pixels"), so the screen looked frozen. The level behind such
  questions is now shown as it was on the screen, also when zoomed out, without the smeared stream at the right edge
  of the camp.
- **Statistics page after a mission**: no more dark dots in the parchment.
- **Seals in HD also when highlighted** under the cursor, plus 27 more menu pictures in HD: the campaign map, the
  portraits of the briefings, the parchment of the questions, map markers.
- **Faster on wooded levels**: HD characters, trees and objects are drawn 8 pixels at a time - in the forest of the
  first mission from the camp a frame takes ~7 ms instead of ~11 ms (140 instead of 86 frames a second).
- **Cursor speed**: the same as Windows' own pointer also with 2560x1440 on a 1080p monitor or in a smaller window
  (it was up to 40 % slower).
- Robustness: Direct3D 11 comes back by itself after a graphics driver reset (it stayed in a plain fallback mode
  until a restart); Esc that closes the F10 panel no longer opens the game's menu as well; safer handling of the
  minimap dragged while a mission ends, of the hearing range between levels and of memory for enlarged pictures.
- Audit: a 4-minute test with interface use - no errors, no warnings, ~143 frames a second.

## Recent changes (1.1.7 - 1.2.0)
- 1.2.0: the hedge along the bottom is whole at every zoom; sword swings at every zoom; portraits no longer rearrange;
  borderless window smaller than the screen (`WindowWidth`, `WindowHeight`, `WindowX`, `WindowY`).
- 1.1.9: no Visual C++ runtime needed; the GOG edition is checked at start; instructions rewritten.
- 1.1.8: the question whether to quit shows the HD / remade main menu; credits in HD (and remade).
- 1.1.7: the options open at once; calm idle animations and smoother long animation steps; sharper smooth zoom.

Uninstall: delete `winmm.dll` (and the `hd` folder).
