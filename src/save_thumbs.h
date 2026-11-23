#pragma once
#include <windows.h>
#include <cstdint>

// The pictures of the saved games (2026-11-24, user: "the saves have a wrong, ugly picture with black bands; it could
// also show the time and date it was made when clicked").
//
// The game makes a save's picture in RHEngine 0x004DA2F0: the whole frame (2560x1440 here, 16:9) into an SBPicture
// (engine +0xE4), resized to 160x120 - squeezed to 4:3, and at the engine's zoom 1.0 on a small level (the camp of
// Sherwood, 1920x1088, which the smooth zoom magnifies on the screen) the level in its top-left corner with black
// beside and below it, the hedge and the portraits at the bottom. After the game's own picture it is drawn again from
// the world the player saw: the part the smooth zoom showed (its copy of the world without the windows), else the
// frame's world above the hedge; the middle 4:3 of it, every picture pixel the mean of the pixels it covers.
//
// The game's "Load" / "Save" screens read the picture file <save>_t of the entry clicked (0x00531BD0) and draw it
// 160x120: the date and time of that save's file appear under it (presenter overlay, the F10 panel's letters).
namespace SaveThumbs {
    void Install();     // DllMain, after MinHook is initialised

    // Diag's hook of the game's CreateFileA (main thread): a file the game opened
    void NoteFileOpened(const char* name, DWORD access);
    // DDM (main thread): a picture of `srcW` x `srcH` (surface `serial`) drawn whole onto the screen at `d`
    void NoteScreenBlit(unsigned serial, int srcW, int srcH, const RECT& d);
    // Presenter (main thread): the date under the save's picture into the mapped frame texture (see
    // SettingsMenu::Draw for the arguments)
    void Draw(uint8_t* dst, LONG dstPitch, bool dst8888, int w, int h, int originX, int originY);

    // test harness: what was done last; the engine's save picture -> path (u32 w, h, RGB565 rows), in a mission only
    int Describe(char* out, int size);
    bool DumpPicture(const char* path);
}
