SherwoodEngineFix - a modification for **Robin Hood: The Legend of Sherwood, GOG edition**: smooth play on today's
PCs (own DirectDraw + Direct3D 11 presentation, borderless window, Alt+Tab, extra frames between game ticks), HD mode
at 2560x1440 with HD graphics from AI-upscaled game files, 2x interface with HD fonts, full-screen menus in HD, smooth
zoom, in-between animation frames, F10 settings panel. Full description: [README](https://github.com/TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod#readme).

## Installation (short)
1. **GOG edition only** - Steam / CD editions and changed `Game.exe` files are not supported (the mod says so at start
   and stays off).
2. HD graphics: the archives of [HD graphics pack 1](https://github.com/TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod/releases/tag/hd-pack-1)
   (3 required) - unpack each **into the game folder** (next to `Game.exe`).
3. Then unpack `SherwoodEngineFix-v1.2.5.zip` (below) into the game folder - after the HD archives. Updating from an
   earlier version: unpack it over the old files and confirm replacing them; the HD packs stay as they are. Your
   `SherwoodFix.ini` can stay (the new options get their defaults; the release's `SherwoodFix.ini` describes them).
4. Optional: new menu backgrounds - [remade menu backgrounds 2](https://github.com/TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod/releases/tag/menu-remake-2).
5. Start the game. In a mission `GRAFIKA HD: DAT2` appears at the top for 3 s = everything is in place. F10 = settings.

**Windows "Extract All" adds the archive's name to the target path** - delete that part, the files must land right
next to `Game.exe`. Keep the resolution at 2560x1440 on every monitor (the picture is scaled to the screen). Not
working? See [If it does not work](https://github.com/TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod#if-it-does-not-work)
and attach `SherwoodFix.log` from the game folder.

## What's new in 1.2.5
- **Minimap - size and picture to choose** (F10 → "MINIMAPA - WIELKOSC" / "MINIMAPA - OBRAZ", or `MapScale` /
  `MinimapHD` in `SherwoodFix.ini`):
  - size: classic 2x (as the rest of the interface), smaller 1.5x, larger 2.5x, large 3x, very large 5x (the tall
    maps of Nottingham and Derby stop at what fits above the hedge, about 4.4x); clicking and dragging on the map work
    at every size;
  - picture: the classic one, the classic one upscaled by AI, a **new picture made from the level's HD background**
    (sharp at every size), or the new one **tinted like the old map on parchment** - the default. Every level in
    day, night and fog has its own (the fog maps keep their sketch look).
  - The Sherwood camp's own map picture shows a forest path, not the camp - perhaps on purpose (the camp itself shows
    no minimap), so it stays, upscaled 4x (`MinimapCamp=1` in the INI: the real camp like the other levels).
  - The pictures are ready files in the archive (`hd\minimap\`) - nothing is generated while playing; the mod reads
    the level's picture once when the map is opened and keeps it at the chosen size.
- **Quick save / quick load keys** (F10 → "KLAWISZ SZYBKIEGO ZAPISU" / "... ODCZYTU", or `QuickSaveKey` /
  `QuickLoadKey`): another key, or **no key at all** (`none` - no more loading a game by accident). The game's own
  key settings stay as they are. Keys the mod itself uses (F6-F10) and keys of other actions are skipped.
- The first briefing of a level no longer shows a black band on the right after the mouse wheel was turned in the
  menu or while loading.
- Zoomed out, the bottom of the level is no longer hidden behind the hedge with the portraits: the view goes down
  until the level's bottom edge meets the hedge.
- **Recording with OBS**: the GOG installer makes the game run as administrator, so OBS must run as administrator
  too - then Game Capture, Window Capture and the OBS hotkeys work (without it only Display Capture works, and the
  hotkeys only after Alt+Tab). `SherwoodFix.log` now says at the start whether the game runs as administrator.
- Fighting: the log records the first 60 sword gestures of a session (`Swing:` lines) - for a report when drawn
  moves are not recognised. (Lines and half circles are drawn relative to the fighter, see the game's manual p. 27.)
- The archive is larger because of the minimap pictures (see the size below).

## 1.2.4
- The whole interface in HD: every picture of the game's interface file (about 940) has an AI-upscaled version; the
  2x mission interface draws them.
- Load / save screen after loading a game: the remade background is whole again.

## Recent changes (1.2.0 - 1.2.3)
- 1.2.3: the camp's question backdrop whole, first briefing without a black band, portraits in an even row,
  campaign map in HD in every state.
- 1.2.2: portraits evenly spaced, skills tooltip next to the pointer, black screen when loading, the Windows key works.
- 1.2.1: the camp's questions fixed; seals, campaign map and briefing portraits in HD; faster on wooded levels.
- 1.2.0: the hedge along the bottom is whole at every zoom; sword swings at every zoom; borderless window smaller than
  the screen.

Uninstall: delete `winmm.dll` (and the `hd` folder).
