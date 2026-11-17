SherwoodEngineFix - a modification for **Robin Hood: The Legend of Sherwood, GOG edition**: smooth play on today's
PCs (own DirectDraw + Direct3D 11 presentation, borderless window, Alt+Tab, extra frames between game ticks), HD mode
at 2560x1440 with HD graphics from AI-upscaled game files, 2x interface with HD fonts, full-screen menus in HD, smooth
zoom, in-between animation frames, F10 settings panel. Full description: [README](https://github.com/TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod#readme).

## Installation (short)
1. **GOG edition only** - Steam / CD editions and changed `Game.exe` files are not supported (the mod says so at start
   and stays off).
2. HD graphics: the archives of [HD graphics pack 1](https://github.com/TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod/releases/tag/hd-pack-1)
   (3 required) - unpack each **into the game folder** (next to `Game.exe`).
3. Then unpack `SherwoodEngineFix-v1.2.0.zip` (below) into the game folder - after the HD archives. Updating: keep
   your `SherwoodFix.ini` if you changed settings (the new window options below work without new lines).
4. Optional: new menu backgrounds - [remade menu backgrounds 2](https://github.com/TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod/releases/tag/menu-remake-2).
5. Start the game. In a mission `GRAFIKA HD: DAT2` appears at the top for 3 s = everything is in place. F10 = settings.

**Windows "Extract All" adds the archive's name to the target path** - delete that part, the files must land right
next to `Game.exe`. Keep the resolution at 2560x1440 on every monitor (the picture is scaled to the screen). Not
working? See [If it does not work](https://github.com/TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod#if-it-does-not-work)
and attach `SherwoodFix.log` from the game folder.

## What's new in 1.2.0
Thanks to a YouTuber who tried the mod and reported what he saw.
- **The hedge along the bottom is whole again.** The game draws one middle piece of the hedge per character in the
  team - with fewer characters the bar had a hole in the middle (black in the original, the map with the mod), and the
  map showed through the gaps between the leaves; zooming the view, the bushes seemed to dissolve. Now the bar runs
  along the whole width with a dark base, as in the original, and stays the same at every zoom.
- **Sword swings at any zoom.** In a fight you hold the left button and draw the swing with the mouse. At zooms other
  than 2x the orange trail appeared somewhere else and the swing was not recognised properly (the game compared the
  pointer's screen position with the fighter's position in the zoomed picture). Both are now measured in the same
  picture.
- **Portraits no longer rearrange** when another character of the team is selected (the selected portrait is wider -
  the interface 2x switched between two layouts of the row).
- **Borderless window smaller than the screen** (for recording, or next to other windows): `WindowWidth`,
  `WindowHeight` (and optionally `WindowX`, `WindowY`) in `SherwoodFix.ini`, `0` = the whole monitor (default).
- Audit: a 4-minute test with fights, map, menus, F10 and background variants - no errors, no warnings, ~143 frames a
  second.

## Recent changes (1.1.5 - 1.1.9)
- 1.1.9: no Visual C++ runtime needed; the GOG edition is checked at start; a missing settings file is written with
  the release's settings; instructions rewritten.
- 1.1.8: the question whether to quit shows the HD / remade main menu; credits in HD (and remade); credits run smoothly.
- 1.1.7: the options open at once; calm idle animations and smoother long animation steps; sharper smooth zoom.
- 1.1.6: sharp in-between frames; lists in HD; night and fog checked.
- 1.1.5: menus and dialogs in HD; remade menu backgrounds; Esc menu opens in ~0.08 s.

Known: after choosing the next mission in the Sherwood camp the screen may stop refreshing (reported, not reproduced
yet - a saved game from the camp would help).

Uninstall: delete `winmm.dll` (and the `hd` folder).
