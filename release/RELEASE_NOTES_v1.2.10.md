SherwoodEngineFix - a modification for **Robin Hood: The Legend of Sherwood, GOG edition**: smooth play on today's
PCs (own DirectDraw + Direct3D 11 presentation, borderless window, Alt+Tab, extra frames between game ticks), HD mode
at 2560x1440 with HD graphics from AI-upscaled game files, 2x interface with HD fonts, full-screen menus in HD, smooth
zoom, in-between animation frames, F10 settings panel. Full description: [README](https://github.com/TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod#readme).

## Installation (short)
1. **GOG edition only** - Steam / CD editions and changed `Game.exe` files are not supported (the mod says so at start
   and stays off).
2. HD graphics: the archives of [HD graphics pack 1](https://github.com/TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod/releases/tag/hd-pack-1)
   (3 required) - unpack each **into the game folder** (next to `Game.exe`). Since 1.2.9 the pack also has
   `SherwoodHD-4-klatki-posrednie-2.zip` (110 MB, recommended: smoother birds, flags, smoke and more in-between frames).
3. Then unpack `SherwoodEngineFix-v1.2.10.zip` (below) into the game folder - after the HD archives. Updating from an
   earlier version: unpack it over the old files and confirm replacing them; the HD packs stay as they are. Your
   `SherwoodFix.ini` can stay (new options get their defaults; the release's `SherwoodFix.ini` describes them).
4. Optional: new menu backgrounds - [remade menu backgrounds 2](https://github.com/TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod/releases/tag/menu-remake-2).
5. Start the game. In a mission `GRAFIKA HD: DAT2` appears at the top for 3 s = everything is in place. F10 = settings.

**Windows "Extract All" adds the archive's name to the target path** - delete that part, the files must land right
next to `Game.exe`. Keep the resolution at 2560x1440 on every monitor (the picture is scaled to the screen). Not
working? See [If it does not work](https://github.com/TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod#if-it-does-not-work)
and attach `SherwoodFix.log` from the game folder.

## What's new in 1.2.10
- **A crash fixed for good** (access violation at `0x0058C88B`): the enemies' fields of view (the eye button in the
  top right corner, then a soldier) read the level's picture past its edge when the view is larger than the level -
  zoomed out on the crossroads maps or in Nottingham. 1.2.9 only gave the game more room there; now every field of
  view is cut to the level.
- **The mouse cursor in menus and dialogs in HD** - on the scrolls in the camp, in the Esc menu, in questions and on
  the save screen it was still the old small picture enlarged (in a mission it was HD since 1.2.9).
- **Robin's eyebrow and the leaves around his eye** (top right corner) no longer show the ground through them: on a
  small level zoomed out, black pixels of the interface over the band beyond the level were taken for the level.

## 1.2.9
- A crash fixed (the first case of the one above); the mouse cursor and its action icons in a mission in HD.
- "Quit the game?" in the main menu and "Save over an existing game?" over the HD menu.
- In-between frames and a smooth flight path for the level's own animations (birds, flags, smoke...) and ~11,400 more
  for the characters - in `SherwoodHD-4-klatki-posrednie-2.zip` of HD graphics pack 1.

## 1.2.8
- Dialogs in a mission: the whole level behind them is darkened.
- Green patches while **left Shift** is held are the game's own view of the places your men can reach (manual p. 16).

## 1.2.7
- Sword gestures work again (each point of a stroke recorded once, as in the original game).

## Recent changes (1.2.0 - 1.2.6)
- 1.2.6: the game draws on the most powerful graphics card; recording with OBS explained in the README.
- 1.2.5: minimap in five sizes and four pictures (F10), quick save / quick load keys.
- 1.2.4: the whole interface in HD (about 940 pictures).
- 1.2.3: the camp's question backdrop whole, first briefing without a black band, portraits in an even row.
- 1.2.2 / 1.2.1 / 1.2.0: portraits evenly spaced, black screen when loading, the camp's questions, seals and campaign
  map in HD, the hedge whole at every zoom, a borderless window smaller than the screen.

Uninstall: delete `winmm.dll` (and the `hd` folder).
