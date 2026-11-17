#pragma once
#include "ddm_blit.h"

// HD menu pictures (2026-11-14, with the menu zoom of ui_scale.h).
//
// The game composes every menu screen (main menu, options, load, ...) in a surface as large as the
// screen and copies it to the screen, whole when the screen appears and in parts to repair what
// buttons, texts and the cursor covered. Such a composed picture is recognised by its content
// (FNV-1a 64 over the part the menu zoom shows); an upscaled version of it lies in
//     hd\menu\<variant>\<16 hex digits>.mhd          (tools\hd\upscale_menu.py)
// Pictures without one are written once to hd\work\menu_frames\<key>.mnf for that tool.
// While the menu zoom shows the middle of the frame, the presenter takes the HD pixel wherever the
// frame still holds the original pixel of the picture (texts, buttons and the cursor drawn over it
// stay the game's).
namespace DDM::HD::Menu {
    // DoBlt, main thread: a 1:1 copy into the screen from a surface of the screen's size.
    // `s` = source rectangle (inside src), (dx, dy) = where it lands on the screen.
    void OnScreenCopy(const Buf& src, unsigned serial, unsigned gen, unsigned genWrite, const RECT& s, int dx, int dy);
    // DoBlt, main thread: a whole 1:1 copy between two surfaces of the screen's size, neither of them
    // the screen (the credits copy their fixed forest into the surface the scrolling text goes into).
    void OnSurfaceCopy(const Buf& dst, const Buf& src, unsigned serial, unsigned gen, unsigned genWrite);
    // DoBlt, main thread: the screen (or its back buffer) was filled with one colour at rect `r` -
    // over the whole surface the menu picture is gone (loading a saved game blanks the screen).
    void OnScreenFill(const Buf& dst, const RECT& r);

    struct Layer {
        const uint32_t* orig = nullptr;   // w x h: 0 = no HD picture, else 0x10000 | original RGB565
        const uint32_t* hd = nullptr;     // 2w x 2h, BGRA
        int w = 0, h = 0;                 // the shown part of the frame (crop)
        RECT dirty{};                     // `orig` changed since the last call (crop pixels); empty = nothing
        RECT hdDirty{};                   // `hd` changed since the last call (crop pixels); empty = nothing
        unsigned generation = 0;          // changes when everything has to be uploaded again
    };
    // Presenter, main thread: the layer for the part `crop` of the frame; false = none.
    bool GetLayer(const RECT& crop, Layer& out);

    // A dialog in a mission (in-game menu, briefing) lies over a backdrop the game composed from
    // the frozen picture of the level. With the menu zoom only the dialog is to be enlarged: the
    // presenter shows the backdrop at its size wherever the frame still holds the backdrop's
    // pixel. The backdrop as copied last (RGB565, frame-sized); false = none.
    // BGRA: the backdrop without the dialog's own pictures (the frozen level picture, darkened as
    // the game does it), alpha = 255 where the game's backdrop shows the level, 0 where it holds
    // a picture of the dialog (the briefing's parchment) - those stay enlarged.
    // show: what is shown where the backdrop shows (the world magnified as the smooth zoom showed
    // it); nullptr = bgra itself. bgra stays what the frame is compared with.
    struct Backdrop {
        const uint32_t* bgra = nullptr;
        const uint32_t* show = nullptr;
        int w = 0, h = 0;
        unsigned generation = 0;    // changes with the backdrop's content
    };
    bool GetBackdrop(Backdrop& out);

    // UiScale, main thread: the game's dialog loop starts - in a mission the picture on the
    // screen is still the level (the backdrop is made from it). `fromBack`: a dialog opened while
    // the mission's frame was being drawn - the level is that frame in the back buffer (the one
    // the smooth zoom's world copy belongs to), not the older one on the screen.
    void NoteDialogStart(bool fromBack = false);

    // The HD menu picture under crop pixel (cx, cy) of `crop`, when the canvas there still holds the
    // picture's pixel `px` (RGB565): its 2x2 block (BGRA, rows `stride` pixels apart); else nullptr.
    const uint32_t* HdUnder(const RECT& crop, int cx, int cy, uint16_t px, int& stride);

    // test harness: counters
    int Describe(char* out, int size);
}
