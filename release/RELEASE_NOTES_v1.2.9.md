SherwoodEngineFix - a modification for **Robin Hood: The Legend of Sherwood, GOG edition**: smooth play on today's
PCs (own DirectDraw + Direct3D 11 presentation, borderless window, Alt+Tab, extra frames between game ticks), HD mode
at 2560x1440 with HD graphics from AI-upscaled game files, 2x interface with HD fonts, full-screen menus in HD, smooth
zoom, in-between animation frames, F10 settings panel. Full description: [README](https://github.com/TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod#readme).

## Installation (short)
1. **GOG edition only** - Steam / CD editions and changed `Game.exe` files are not supported (the mod says so at start
   and stays off).
2. HD graphics: the archives of [HD graphics pack 1](https://github.com/TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod/releases/tag/hd-pack-1)
   (3 required) - unpack each **into the game folder** (next to `Game.exe`). **New in the pack:**
   `SherwoodHD-4-klatki-posrednie-2.zip` (110 MB, recommended) - download and unpack it also if you already have the
   others.
3. Then unpack `SherwoodEngineFix-v1.2.9.zip` (below) into the game folder - after the HD archives. Updating from an
   earlier version: unpack it over the old files and confirm replacing them; the HD packs stay as they are. Your
   `SherwoodFix.ini` can stay (new options get their defaults; the release's `SherwoodFix.ini` describes them).
4. Optional: new menu backgrounds - [remade menu backgrounds 2](https://github.com/TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod/releases/tag/menu-remake-2).
5. Start the game. In a mission `GRAFIKA HD: DAT2` appears at the top for 3 s = everything is in place. F10 = settings.

**Windows "Extract All" adds the archive's name to the target path** - delete that part, the files must land right
next to `Game.exe`. Keep the resolution at 2560x1440 on every monitor (the picture is scaled to the screen). Not
working? See [If it does not work](https://github.com/TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod#if-it-does-not-work)
and attach `SherwoodFix.log` from the game folder.

## What's new in 1.2.9
- **A crash fixed** (access violation at `0x0058C88B`, Nottingham): the game now and then reads a little past the end
  of the level's picture; with the original DirectDraw that went unnoticed, with the mod it could end the game. Every
  picture the mod keeps for the game now has a margin around it.
- **The mouse cursor and its action icons in HD** - the arrow, the fists, tie up, open, pick up, the cross... no
  longer have a cut-out outline of an odd colour: they are drawn from their HD versions.
- **"Quit the game?" in the main menu** (after playing a mission) and **"Save over an existing game?"** on the save
  screen show the HD menu behind them instead of the old small picture.
- **The level's own animations are smoother**: pigeons, crows, ducks, hens, flags, smoke, flies... get in-between
  frames (F9), and what flies or swims moves along a smooth path instead of jumping every 80-160 ms. Needs the new
  archive `SherwoodHD-4-klatki-posrednie-2.zip` from HD graphics pack 1 - without it everything works as in 1.2.8.
- **More in-between frames for the characters**: a second try with other settings of the frame generator gave about
  11,400 frames to moves the first pack had to skip (falls, quick strikes, bending down) - in the same new archive.

## 1.2.8
- Dialogs in a mission: the whole level behind them is darkened (a strip of the level - not darkened and larger -
  showed on the right behind a question in Nottingham).
- Green patches while **left Shift** is held are the game's own view of the places your men can reach behind
  obstacles or on top of rocks and stumps (manual p. 16) - the same with the original graphics (F8). Not a mod error.

## 1.2.7
- Sword gestures work again: with the mod's extra frames between game ticks almost every line became a normal
  strike; each point of a stroke is now recorded once, as in the original game.

## 1.2.6
- The game draws on the most powerful graphics card (computers with two graphics units; `GPU=1` = Windows' choice).
- Recording with OBS: the README's OBS section explains what gets in the way.

## Recent changes (1.2.0 - 1.2.5)
- 1.2.5: minimap in five sizes and four pictures (F10), quick save / quick load keys.
- 1.2.4: the whole interface in HD (about 940 pictures).
- 1.2.3: the camp's question backdrop whole, first briefing without a black band, portraits in an even row.
- 1.2.2: portraits evenly spaced, skills tooltip next to the pointer, black screen when loading, the Windows key works.
- 1.2.1 / 1.2.0: the camp's questions, seals and campaign map in HD, the hedge whole at every zoom, sword swings at every
  zoom, a borderless window smaller than the screen.

Uninstall: delete `winmm.dll` (and the `hd` folder).
