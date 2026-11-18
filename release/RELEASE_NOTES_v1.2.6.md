SherwoodEngineFix - a modification for **Robin Hood: The Legend of Sherwood, GOG edition**: smooth play on today's
PCs (own DirectDraw + Direct3D 11 presentation, borderless window, Alt+Tab, extra frames between game ticks), HD mode
at 2560x1440 with HD graphics from AI-upscaled game files, 2x interface with HD fonts, full-screen menus in HD, smooth
zoom, in-between animation frames, F10 settings panel. Full description: [README](https://github.com/TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod#readme).

## Installation (short)
1. **GOG edition only** - Steam / CD editions and changed `Game.exe` files are not supported (the mod says so at start
   and stays off).
2. HD graphics: the archives of [HD graphics pack 1](https://github.com/TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod/releases/tag/hd-pack-1)
   (3 required) - unpack each **into the game folder** (next to `Game.exe`).
3. Then unpack `SherwoodEngineFix-v1.2.6.zip` (below) into the game folder - after the HD archives. Updating from an
   earlier version: unpack it over the old files and confirm replacing them; the HD packs stay as they are. Your
   `SherwoodFix.ini` can stay (new options get their defaults; the release's `SherwoodFix.ini` describes them).
4. Optional: new menu backgrounds - [remade menu backgrounds 2](https://github.com/TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod/releases/tag/menu-remake-2).
5. Start the game. In a mission `GRAFIKA HD: DAT2` appears at the top for 3 s = everything is in place. F10 = settings.

**Windows "Extract All" adds the archive's name to the target path** - delete that part, the files must land right
next to `Game.exe`. Keep the resolution at 2560x1440 on every monitor (the picture is scaled to the screen). Not
working? See [If it does not work](https://github.com/TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod#if-it-does-not-work)
and attach `SherwoodFix.log` from the game folder.

## What's new in 1.2.6
- **The game draws on the most powerful graphics card.** On computers with two graphics units - a processor with
  graphics of its own (e.g. AMD Ryzen 7000/9000) or a laptop - Windows often gave the game the weaker one: slower, and
  OBS's Game Capture stayed black. When the game's monitor is connected to that card (or the same model), the picture
  goes straight to it. `SherwoodFix.log` lists every card and names the one used (`Present: Direct3D 11 on ...`);
  `GPU=1` in `SherwoodFix.ini` = Windows' choice as before.
- **Recording with OBS** - tested with OBS 33: Game Capture records the mod's picture. The README's OBS section explains
  what gets in the way: start OBS as administrator (the game runs as administrator), the game pauses while its window
  is not in front (OBS's preview is black then - use OBS's hotkeys), untick "Use anti-cheat compatibility hook" in the
  Game Capture's properties, and "SLI/Crossfire Capture Mode" when the game and OBS run on different cards.

## 1.2.5
- Minimap: five sizes (classic 2x, 1.5x, 2.5x, 3x, 5x) and four pictures (classic, classic upscaled, new from the
  level's HD background, new on parchment - the default), F10 → "MINIMAPA - WIELKOSC" / "MINIMAPA - OBRAZ".
- Quick save / quick load keys (F10, or `QuickSaveKey` / `QuickLoadKey`), also "none".
- First briefing without a black band after a mouse wheel turn; zoomed out, the bottom of the level ends above the
  hedge; the log records the first 60 sword gestures (`Swing:`).

## Recent changes (1.2.0 - 1.2.4)
- 1.2.4: the whole interface in HD (about 940 pictures).
- 1.2.3: the camp's question backdrop whole, first briefing without a black band, portraits in an even row.
- 1.2.2: portraits evenly spaced, skills tooltip next to the pointer, black screen when loading, the Windows key works.
- 1.2.1 / 1.2.0: the camp's questions, seals and campaign map in HD, the hedge whole at every zoom, sword swings at every
  zoom, a borderless window smaller than the screen.

Uninstall: delete `winmm.dll` (and the `hd` folder).
