#pragma once
#include "ddm_blit.h"
#include "ddm_hd_menu.h"
#include <cstdint>
#include <vector>

// HD letters and pictures in menus and dialogs (2026-11-15, "lecisz te wszystkie rzeczy").
//
// With the menu zoom the game draws its menus and dialogs at their own size (1x) in the middle of
// the frame and the presenter enlarges that part (1.875x at 1440p): letters, buttons, the
// briefing's parchment were magnified from 1x pixels. Here every letter and picture the game draws
// there is drawn a second time, twice as large, into a "shadow" of the surface (only over the
// part the menu zoom shows): the letter from the HD font (ddm_hd_font.h), a picture from its
// upscaled version (hd\menu_parts\<16 hex digits>.hdp, tools\hd\upscale_menu_parts.py), over the
// HD version of what was there (the shadow itself, the HD menu picture, or the old pixels
// enlarged). Every shadow pixel remembers the frame pixel the game drew there; 1:1 copies between
// surfaces copy the shadows along (the game composes its dialogs in surfaces of the screen's size
// and copies them to the screen). The presenter shows the shadow of the presented surface wherever
// the frame still holds that pixel - the cursor, highlights and anything not known here stay the
// game's own pixels, enlarged as before.
namespace DDM::HD::Patch {
    // Main thread: shadows are kept now (menu zoom on, a dialog loop runs, HDMenu=1).
    bool Active();

    // DirectDraw 1:1 copy of rect `s` of `src` to (dx, dy) of `dst`, after the copy. A whole copy of
    // the screen into another surface of its size starts a dialog: the game composes it there
    // (the briefing draws its parchment before its dialog loop runs) - such surfaces take
    // patches from then on, until the screen gets a whole picture from elsewhere again.
    void OnCopy(const Buf& dst, int dx, int dy, const Buf& src, const RECT& s, bool dstIsScreen, bool srcIsScreen);
    // Anything else wrote rect `r` of `dst` (fill, stretch): the shadow there no longer applies.
    void OnOverwrite(const Buf& dst, const RECT& r);

    // A picture or letter drawn 1:1: Begin...() before the game draws it (the pixels under it are
    // kept), End() after. Begin returns false when there is nothing to do.
    struct Op {
        Buf dst{};
        int x = 0, y = 0, w = 0, h = 0;     // destination rectangle (surface pixels)
        int kind = 0;                       // 1 glyph, 2 picture, 3 something without an HD version
        bool glyph = false;                 // a letter (its HD edge may reach into pixels it leaves alone)
        const uint16_t* src = nullptr;      // source pixels (top-left of the drawn part)
        int srcPitch = 0;                   // bytes
        const uint16_t* alpha = nullptr;    // alpha glyph: its alpha strip (else nullptr)
        int alphaPitch = 0;
        uint32_t key = 0xFFFFFFFFu;         // transparent source colour (0xFFFFFFFF: none)
        uint32_t marker = 0xFFFFFFFFu;      // source colour that darkens the destination (shadows)
        uint32_t factor = 256;              // ... by factor / 256
        // HD glyph: its strip and the HD pixel of the 1x top-left
        const uint16_t* hdColour = nullptr;
        const uint8_t*  hdAlpha = nullptr;
        int hdW = 0, hdH = 0, hx = 0, hy = 0;
        const void* picture = nullptr;      // HD picture (see the .cpp)
        std::vector<uint16_t> before;
        void Clear() {                      // all fields back, the buffer's memory kept
            std::vector<uint16_t> keep;
            keep.swap(before);
            *this = Op{};
            keep.clear();
            before.swap(keep);
        }
    };
    bool BeginAlphaGlyph(const uint16_t* src, uint16_t* dst, const uint16_t* alpha, int w, int h, Op& op);
    bool BeginKeyed(const Buf& dst, int dx, int dy, const Buf& src, const RECT& s, uint16_t key, Op& op);
    bool BeginShadowPicture(const uint16_t* src, uint16_t* dst, int w, int h, uint32_t factor, uint32_t marker, bool keyed, Op& op);
    void End(Op& op);

    // The game's cursor is being drawn (0x005F1700): its pictures are not patches.
    void SetCursorDrawing(bool on);

    // Text the game draws with GDI (TextOutA / TextOutW into a surface's DC: lists of saved games, key
    // bindings, Arial): drawn again with the same font twice as large, smoothed. Import slots of Game.exe.
    bool InstallGdiText();

    // The last dialog loop ended: all shadows go.
    void Reset();

    // Presenter, main thread: the shadow of the presented surface (pixels at `frameBits`) over
    // `crop`: orig = 0 / 0x10000 | the frame pixel it belongs to, hd = 2x BGRA; false = none.
    // `identity` tells the shadows apart (a new one: upload everything).
    bool GetPatch(const uint8_t* frameBits, const RECT& crop, Menu::Layer& out, uintptr_t& identity);

    // Time spent here since the last call, in microseconds (the presenter's hitch report)
    double TakeTimeUs();

    // The mission's interface enlarged 2x (ddm_blit.cpp, SmoothPicture2x16 - the HUD, portraits, icons,
    // the hedge): the HD version of the whole picture in rect `r` of `src`, found by its content - the
    // same files as the menus' (hd\menu_parts\<key>.hdp; most made from DATA\Interface\Default.res by
    // tools\hd\res_pictures.py, sides >= 4): 2w x 2h BGRA (alpha = coverage) and the shadow coverage.
    // Main thread only, HDMenu=1; false = none.
    bool InterfacePicture(const Buf& src, const RECT& r, int& w2, int& h2, const uint32_t*& bgra, const uint8_t*& shade);

    // A picture of the opened map at its own size (DDM::ScaledPicture16): by [Display] MinimapHD - 3 the one made
    // from the level's HD background and tinted like the game's parchment (hd\minimap\parchment\<key>.hdp), 2 the
    // same without the tint (hd\minimap\clean\), each 2..4 times the game's picture (the other kind, then the
    // upscaled game's picture when one is missing); 1 the game's picture upscaled (InterfacePicture); 0 none.
    // The Sherwood camp's own picture shows another place (a forest path); with MinimapCamp=0 (MinimapHD >= 1) it is
    // kept, upscaled 4x (hd\minimap\classic\). wk x hk BGRA (alpha = coverage) and the shadow coverage. Main thread.
    bool MapPicture(const Buf& src, const RECT& r, int& wk, int& hk, const uint32_t*& bgra, const uint8_t*& shade);

    // Test harness
    int Describe(char* out, int size);
}
