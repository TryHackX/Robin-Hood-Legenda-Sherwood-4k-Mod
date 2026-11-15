#pragma once
#include "ddm_blit.h"
#include <vector>

// HD level backgrounds.
//
// With an internal resolution of 2560x1440 and the game's 2x zoom the engine composes the frame
// natively (sharp HUD, cursor, view cones) and only magnifies its bitmaps. The level background is
// magnified by an exact 2x stretch from the "map surface" (a 16-bit picture of the whole level)
// into the engine's world surface - on a full redraw and for the strips uncovered by scrolling.
// That stretch is replaced here: when an upscaled version of the picture exists, its pixels are
// copied 1:1 instead of doubling the low-resolution ones.
//
// Several upscaled versions ("variants", one per AI model) can be installed side by side:
//     hd\maps\<variant>\<16 hex digits>.hdmap      (tools\hd\upscale_variants.py)
// The player switches between them, and back to the original pixels, with hot keys while
// playing. The file name is a 64-bit FNV-1a hash over every 7th pixel of the original picture:
// the level is recognised by content, no knowledge about the game's files is needed.
namespace DDM {
    // The surface, among those the game locked most recently, whose pixels contain `address`
    // (the game draws sprites itself into locked memory: this tells which surfaces are involved).
    bool FindLockedBuf(const void* address, Buf& out);

    // True while the surface with this serial number exists and its pixels are at `bits`.
    bool SurfaceAlive(uintptr_t serial, const uint8_t* bits);

    // The picture on the screen now (the front surface: the frame presented last), RGB565.
    bool CopyFront(std::vector<uint16_t>& out, int& w, int& h);
    // The frame being drawn (the back surface of the flip chain), RGB565.
    bool CopyBack(std::vector<uint16_t>& out, int& w, int& h);
    // ... its pixels in place (main thread: valid until the game draws again)
    bool BackBits(const uint8_t*& bits, int& pitch, int& w, int& h);
}

namespace DDM::HD {
    // Returns true if `dst` was filled from an HD picture (rect `d` of dst <- rect `s` of src,
    // exactly 2x, restricted to `clip`). `identity` is the source surface; `lockGeneration`
    // changes whenever the game locked it (it reads its map that way every frame),
    // `writeGeneration` whenever it was the target of a blit.
    bool TryBackgroundBlit(const Buf& dst, const RECT& d, const RECT& clip,
                           const Buf& src, const RECT& s, const void* identity,
                           unsigned lockGeneration, unsigned writeGeneration);

    // A colour-keyed sprite is about to be magnified 2x onto the frame `dst` (rect `s` of `src`
    // to rect `d`, visible part `clip`). Returns true if it is a ground animation (water, chimney
    // smoke) that was handled here: its difference to the level picture over the HD background,
    // or left out for still HD water (see ddm_hd.cpp).
    bool DrawGroundAnimation(const Buf& dst, const RECT& d, const RECT& clip, const Buf& src, const RECT& s);

    // Main thread, once per game tick: hot keys, variant switches, the redraw they need.
    void OnGameTick();

    // Variant control, any thread (hot keys and the test harness). The switch itself happens at
    // the next game tick. step: +1 next, -1 previous (both cycle through "off" as well).
    void RequestStep(int step);
    void RequestToggle();                    // original <-> the variant used last
    void RequestByName(const char* name);    // folder name, or "off"
    const char* CurrentVariantName();        // "off" when the original pixels are shown
    const char* MenuVariantName();           // main thread: the same, chosen already in the menus
    bool VariantOn();                        // false with "off": sprites are then drawn as the game does, too

    // Main thread: the game starts drawing a tick's own frame (sprite animation steps are counted).
    void OnTickFrame();

    // In-between frames of sprite animations (experimental, see ddm_hd_sprites.h), any thread:
    // mode 0 = off, 1 = on, 2 = the other state. The switch happens at the next game tick.
    void RequestBetween(int mode);
    bool BetweenOn();

    // Hooks the game's own 2x drawing routines (tinted polygons, sprites with shadows): see ddm_hd.cpp.
    void InstallHooks();

    // Test harness: leave out drawing routines to see what they draw. Bit 0: keyed 2x sprites,
    // bit 1: plain 2x sprites, bit 2: tinted polygons, bit 3: DirectDraw keyed 2x stretch blits.
    // Bit 4 (16): every picture of a tick shows the in-between frame of a sprite that has one.
    // Bit 5 (32): ground overlays (chimney smoke) are drawn as animated water (HDWater=2/3).
    void SetDebugFlags(unsigned flags);
    unsigned DebugFlags();
}
