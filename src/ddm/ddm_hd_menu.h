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

    struct Layer {
        const uint32_t* orig = nullptr;   // w x h: 0 = no HD picture, else 0x10000 | original RGB565
        const uint32_t* hd = nullptr;     // 2w x 2h, BGRA
        int w = 0, h = 0;                 // the shown part of the frame (crop)
        RECT dirty{};                     // changed since the last call (crop pixels); empty = nothing
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
    struct Backdrop {
        const uint32_t* bgra = nullptr;
        int w = 0, h = 0;
        unsigned generation = 0;    // changes with the backdrop's content
    };
    bool GetBackdrop(Backdrop& out);

    // UiScale, main thread: the game's dialog loop starts - in a mission the picture on the
    // screen is still the level (the backdrop is made from it).
    void NoteDialogStart();

    // test harness: counters
    int Describe(char* out, int size);
}
