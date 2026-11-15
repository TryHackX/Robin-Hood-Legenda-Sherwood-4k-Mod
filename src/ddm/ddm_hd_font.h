#pragma once
#include "ddm_blit.h"
#include <cstdint>

// HD font (2026-11-15, the player's wish "czcionka HD").
//
// The game's interface text comes from bitmap fonts (DATA\Interface\Fonts\*.bfn / *.fnt, "SBFONT"):
// every font is a strip of fixed-width character cells (ToolTips: 180 cells of 16 px = a strip of
// 2880x20), the "Lydian" ones with a second strip of coverage (alpha, the green channel), the
// others colour-keyed. At the 2x interface scale every glyph was magnified 2x from the strip.
// Here the game's strips are recognised by content and, where an upscaled version exists
// (hd\fonts\<16 hex digits>.hdfont, tools\hd\upscale_fonts.py), the glyph is drawn from it.
namespace DDM::HD::Font {
    // A glyph of a strip with an alpha strip is about to be drawn 2x (the game's 0x00622FD4):
    // `src` / `alpha` point at its top-left pixel in the strips, `dst` at the destination.
    // True: drawn from the HD strip (the game's routine is then left out).
    bool AlphaGlyph2x(const uint16_t* src, const uint16_t* alpha, uint16_t* dst, int dstW, int dstH, int dstPitch);

    // A colour-keyed glyph of a strip about to be drawn 2x (DirectDraw stretch, interface scale):
    // rect `s` of `strip` onto `d` of `dst` (visible part `clip`). True: drawn from the HD strip.
    bool KeyedGlyph2x(const Buf& dst, const RECT& d, const RECT& clip, const Buf& strip, const RECT& s, uint16_t key);

    // A glyph drawn at its size (menus and dialogs with the menu zoom, ddm_hd_patch.h): where its
    // HD version lies. colour / alpha: the HD strip (w x h), (hx, hy): the HD pixel that belongs to
    // the 1x pixel the game starts the glyph at (twice its position in the strip).
    struct HdGlyph {
        const uint16_t* colour = nullptr;   // RGB565
        const uint8_t*  alpha = nullptr;    // 0..255
        int w = 0, h = 0;                   // HD strip size
        int hx = 0, hy = 0;
    };
    // `src` / `alpha` point at the glyph's top-left pixel in a strip and its alpha strip (locked).
    bool FindAlphaGlyph(const uint16_t* src, const uint16_t* alpha, HdGlyph& out);
    // rect `s` of a colour-keyed strip
    bool FindKeyedGlyph(const Buf& strip, const RECT& s, HdGlyph& out);

    // Test harness: write every strip seen from now on to `dir` (font_*.raw), until stopped.
    void SetDump(const char* dir);
    int  Describe(char* out, int size);
}
