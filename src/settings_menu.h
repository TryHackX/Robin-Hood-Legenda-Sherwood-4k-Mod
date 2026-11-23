#pragma once
#include <windows.h>
#include <cstdint>

// The mod's settings inside the game (2026-11-14): F10 ([Display] SettingsKey) opens a panel over
// the picture - menu, dialogs or mission - with the options most worth changing (HD pictures,
// interface size, menu zoom, VSync, filter, FPS counter, cursor speed). A click or the arrow keys
// change a value; it takes effect at once where the game allows it and is written to
// SherwoodFix.ini. While the panel is open the game gets no clicks and no key presses (the game
// itself keeps running in a mission: Esc first pauses it).
namespace SettingsMenu {
    void Install();     // DllMain, after MinHook is initialised (keyboard filter)

    // Main thread, before every presented frame: hot key, navigation, clicks.
    void Poll();

    // Presenter (main thread): the panel into the mapped frame texture (RGB565, or BGRX8888 with
    // `dst8888`), w x h. `src` = the game's frame pixels (RGB565) the texture shows, `originX/Y` =
    // where the texture's top-left pixel lies in the game's frame (menu zoom shows only a part).
    // Only writes to `dst` (a mapped texture is slow to read).
    void Draw(uint8_t* dst, LONG dstPitch, bool dst8888, int w, int h, int originX, int originY,
              const uint8_t* src, int srcPitch);

    // Presenter (main thread): a line of the panel's letters (scale `scale`) on a dark plate into the mapped frame
    // texture, its middle at column `centreX`, its top at row `top` - kept inside the texture; nothing when it
    // does not fit. Colour RGB565.
    void DrawLabel(uint8_t* dst, LONG dstPitch, bool dst8888, int w, int h, int centreX, int top, const char* text,
                   int scale, uint16_t colour);

    // Input thread: a mouse button record (DirectInput offset 0x0C.. and data). True = the panel
    // took it, the game must not see it.
    bool OnMouseButton(uint32_t ofs, uint32_t data);

    // Test harness: open | close | up | down | left | right | enter | click | state -> text
    void TestCommand(const char* what, char* out, int size);
}
