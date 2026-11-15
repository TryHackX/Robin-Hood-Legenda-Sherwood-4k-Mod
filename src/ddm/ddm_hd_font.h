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

    // Test harness: write every strip seen from now on to `dir` (font_*.raw), until stopped.
    void SetDump(const char* dir);
    int  Describe(char* out, int size);
}
