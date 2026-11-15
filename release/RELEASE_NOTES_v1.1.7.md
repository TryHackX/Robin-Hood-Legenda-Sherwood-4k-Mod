## What's new in 1.1.7

- **The options open at once.** Every time the options menu opened (from the main menu or in a mission) the game
  measured the processor's clock by waiting a whole second - the picture stood still. The mod measures it once, in
  25 ms: the options now open in about 85 ms instead of 1.1 s, and the game starts a second sooner.
- **In-between frames: smoother where it matters, calm where it does not.**
  - Most animation steps last 2-3 game ticks (crouching, getting out from under the cloak, many fight moves). The
    in-between frame was shown only in the first ~20 ms of such a step, so these moves still looked choppy. It now
    stays for the first half of every step (the steps' lengths come from the animations themselves:
    `hd\sprites\_between\between.t`, in this archive).
  - Steps without real movement - Robin standing and breathing, for example - no longer get an in-between frame:
    there it was only a patchwork of the two frames and made the clothes seem to flicker. 35 597 such transitions are
    left out (measured by optical flow); 285 049 in-between frames remain.
- **Smooth zoom: sharper.** Between zoom 1x and 2x (and between 2x and 4x) the picture is enlarged with a Catmull-Rom
  filter instead of a bilinear one: walls, animals and clothes are visibly sharper. Below zoom 2 the game itself draws
  the world at its original size, so the HD pictures are only shown from zoom 2 up.
- **Credits screen**: the mod wrote every frame of the scrolling credits to `hd\work\menu_frames` (2 MB each, up to
  1 GB per visit) - fixed, and the credits run smoothly again. If that folder holds hundreds of `.mnf` files on your
  PC, you can delete them: they are only work files.
- **Sword swing line**: the first lines of every kind the game draws are now noted in `SherwoodFix.log` (who draws them
  and where, next to the pointer). If the orange swing line is still away from the pointer at some zoom, the log
  shows why.

## Installation
1. HD graphics: the archives of the **HD graphics pack 1** release (nothing new there since 1.1.6).
2. Then unpack `SherwoodEngineFix-v1.1.7.zip` into the game folder (next to `Game.exe`) - **after** the HD pack, as it
   carries newer small files: `winmm.dll`, `SherwoodFix.ini`, `hd\fonts\`, `hd\menu_parts\`, `hd\menu\<variant>\*.alias`,
   `hd\sprites\_icons\` and `hd\sprites\_between\between.q` / `between.v1.q` / `between.t`. Updating from 1.1.x:
   everything except `SherwoodFix.ini` (yours keeps working).
3. Optional new menu backgrounds: `SherwoodEngineFix-menu-remake-1.zip` from the 1.1.5 release.
4. Start the game as usual. Mod settings in the game: **F10**.

Uninstall: delete `winmm.dll`. On a 1080p monitor set `Width=1920` and `Height=1080` in `SherwoodFix.ini`.
