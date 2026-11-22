SherwoodEngineFix - a modification for **Robin Hood: The Legend of Sherwood, GOG edition**: smooth play on today's
PCs (own DirectDraw + Direct3D 11 presentation, borderless window, Alt+Tab, extra frames between game ticks), HD mode
at 2560x1440 with HD graphics from AI-upscaled game files, **maps in native 4K**, 2x interface with HD fonts,
full-screen menus in HD, smooth zoom, in-between animation frames, F10 settings panel. Full description:
[README](https://github.com/TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod#readme).

## Installation (short)
1. **GOG edition only** - Steam / CD editions and changed `Game.exe` files are not supported (the mod says so at start
   and stays off).
2. HD graphics: the archives of [HD graphics pack 1](https://github.com/TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod/releases/tag/hd-pack-1)
   (3 required) - unpack each **into the game folder** (next to `Game.exe`). **New and optional:**
   `SherwoodHD-5-mapy-4k-a.zip` and `SherwoodHD-5-mapy-4k-b.zip` (about 1.2 GB each) - the maps in native 4K.
3. Then unpack `SherwoodEngineFix-v1.3.1.zip` (below) into the game folder - after the HD archives. Updating from an
   earlier version: unpack it over the old files and confirm replacing them; the HD packs stay as they are. Your
   `SherwoodFix.ini` can stay (new options get their defaults; the release's `SherwoodFix.ini` describes them).
4. Optional: new menu backgrounds - [remade menu backgrounds 2](https://github.com/TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod/releases/tag/menu-remake-2).
5. Start the game. In a mission `GRAFIKA HD: DAT2` appears at the top for 3 s = everything is in place. F10 = settings.

**Windows "Extract All" adds the archive's name to the target path** - delete that part, the files must land right
next to `Game.exe`. Keep the resolution at 2560x1440 on every monitor (the picture is scaled to the screen). Not
working? See [If it does not work](https://github.com/TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod#if-it-does-not-work)
and attach `SherwoodFix.log` from the game folder.

## What's new in 1.3.1
- **The last two pieces of the mission interface in HD**: the red seal on the right and the corner with the hunting
  horn at the bottom right were still magnified from the original pixels (the horn even looked faded). The game
  magnifies them with its own sprite routine, which now takes them from their HD pictures as well.

## What's new in 1.3.0
- **Maps in native 4K.** All 20 map backgrounds were upscaled 4x (`dat2`) - with the two new packs the game shows
  them wherever the screen shows the level larger than its HD picture: zoomed in beyond 2.0 (up to 4.0), and on 4K
  monitors already at 2.0. Blades of grass, stones and planks instead of a magnified picture. Characters, trees,
  shadows and the interface stay as the game draws them. `HDMaps4k=1` in `SherwoodFix.ini` (0 = off, 2 = also when
  zoomed out). The 4K map loads in the background for about a second after a level starts.
- **Zoomed out below 2.0 the ground is HD too.** The game draws the level in its original pixels below 2.0; now the
  terrain and everything painted on the map comes straight from the HD picture at every zoom (`HDLevelLayer=1`).
  Characters and objects below 2.0 stay as the game draws them there.
- **The flash before a scroll is gone** - at the end of a raid (and with other questions in a mission) a scroll cut
  off on the right over the bright level showed for a split second before the right one.
- **Pause:** the paused game no longer keeps a CPU core at 100 % (it ran some 47,000 loops a second); F6-F9 and the
  F10 panel work while paused.
- **F6 / F7 in the carriage mission** (and other missions that change their map while you play) no longer leave the
  level in the original pixels.
- **A full code audit, and its fixes:** the game's own quick save / quick load keys are saved into the profile even
  when you quit with Alt+F4 in a level; a rare memory error when a question ends the level; smooth scrolling no longer
  switches itself off after one slow frame (VSync=0); the HUD's HD pictures no longer switch off after a long session;
  the F10 switches for HD menus and HD characters work even when they were off at the start; the mod no longer makes a copy
  `Game.exe.bak` (it can be deleted) and nothing is written into an `hd\work` folder on players' PCs any more (it can be
  deleted too); crashes inside the mod are now written to the log; the
  fallback presenter shows the F10 panel; unused code removed.

## 1.2.10
- A crash fixed for good (access violation at `0x0058C88B`, enemies' fields of view past the level's edge); the mouse
  cursor in menus and dialogs in HD; Robin's eyebrow in the top right corner whole.

## 1.2.9
- A crash fixed (the first case of the one above); the mouse cursor and its action icons in a mission in HD.
- "Quit the game?" in the main menu and "Save over an existing game?" over the HD menu.
- In-between frames and a smooth flight path for the level's own animations (birds, flags, smoke...) and ~11,400 more
  for the characters - in `SherwoodHD-4-klatki-posrednie-2.zip` of HD graphics pack 1.

## Recent changes (1.2.0 - 1.2.8)
- 1.2.8: dialogs in a mission darken the whole level; the green patches with left Shift explained.
- 1.2.7: sword gestures work again (each point of a stroke recorded once, as in the original game).
- 1.2.6: the game draws on the most powerful graphics card; recording with OBS explained in the README.
- 1.2.5: minimap in five sizes and four pictures (F10), quick save / quick load keys.
- 1.2.4: the whole interface in HD (about 940 pictures).
- 1.2.3: the camp's question backdrop whole, first briefing without a black band, portraits in an even row.
- 1.2.2 / 1.2.1 / 1.2.0: portraits evenly spaced, black screen when loading, the camp's questions, seals and campaign
  map in HD, the hedge whole at every zoom, a borderless window smaller than the screen.

Uninstall: delete `winmm.dll` (and the `hd` folder).
