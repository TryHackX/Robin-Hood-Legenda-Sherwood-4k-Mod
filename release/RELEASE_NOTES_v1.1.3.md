## What's new in 1.1.3

Fixes from a player's report on 1.1.2:

- **Character animations "now HD, now not"**: the generated in-between frames (RIFE) were accepted by the generator up
  to a fairly large difference between the two source frames; half of them came out soft and alternated with the sharp
  HD frames - the figure flickered between sharp and soft, with ghosts in fast moves (e.g. Robin's idle animation, or
  fights at a large zoom looking "drawn in another place"). Only the clean in-between frames are shown now
  (`HDInBetweenQuality=17`, about a third of them); fast moves run as in the original. Needs the small quality list
  `hd\sprites\_between\between.q`, included in this archive.
- **Spots in the dark parts of the interface at a large zoom, and in the menus**: a pixel of a translucent interface
  picture can be equal to the world under it by chance (black stays black when darkened); it was then taken for the
  world and showed a dot of the magnified picture. A pixel now counts as world only when most of its neighbours do.
- **Settings panel (F10) at a large zoom**: the picture behind the panel showed through at another scale - the panel
  now has a plain dark background whenever the screen is magnified.
- **Opening the in-game menu (Esc) at a large zoom**: the hitch is shorter - about 55 ms instead of about 200 ms (the
  magnified backdrop is built with integer arithmetic on several threads).
- Items on the ground (arrows, scrolls ...) are drawn from the HD pictures already - nothing changed there.

## Installation
1. Unpack this archive into the game folder (next to `Game.exe`): `winmm.dll`, `SherwoodFix.ini` and
   `hd\sprites\_between\between.q`. Updating from 1.1.x: `winmm.dll` and `between.q` are enough (your
   `SherwoodFix.ini` keeps working; the limit can be set there with `HDInBetweenQuality`).
2. For HD graphics download the archives of the **HD graphics pack** release and unpack them into the game folder
   (unchanged, no new download needed).
3. Start the game as usual. Mod settings in the game: **F10**.

Uninstall: delete `winmm.dll`. On a 1080p monitor set `Width=1920` and `Height=1080` in `SherwoodFix.ini`.
