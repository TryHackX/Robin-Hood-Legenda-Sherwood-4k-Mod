#pragma once
#include "ddm_blit.h"
#include <string>
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

    // The surface whose GDI device context (GetDC) is `dc`: its pixels.
    bool SurfaceOfDC(void* dc, Buf& out);

    // The picture on the screen now (the front surface: the frame presented last), RGB565.
    bool CopyFront(std::vector<uint16_t>& out, int& w, int& h);
    // The frame being drawn (the back surface of the flip chain), RGB565.
    bool CopyBack(std::vector<uint16_t>& out, int& w, int& h);
    // ... its pixels in place (main thread: valid until the game draws again)
    bool BackBits(const uint8_t*& bits, int& pitch, int& w, int& h);
    // ... rect `r` of it filled with one colour (main thread)
    bool FillBack(const RECT& r, uint16_t colour);
}

namespace DDM::HD {
    // Returns true if `dst` was filled from an HD picture (rect `d` of dst <- rect `s` of src,
    // exactly 2x, restricted to `clip`). `identity` is the source surface; `lockGeneration`
    // changes whenever the game locked it (it reads its map that way every frame),
    // `writeGeneration` whenever it was the target of a blit.
    bool TryBackgroundBlit(const Buf& dst, const RECT& d, const RECT& clip,
                           const Buf& src, const RECT& s, const void* identity,
                           unsigned lockGeneration, unsigned writeGeneration);

    // DDM, before a blit into `surf` (serial `serial`) - a large surface that is not the screen: the first one into
    // a surface keeps a copy of it - a level picture as the game loaded it, before the mission put its objects into
    // it (fallen branches, heaps of leaves). A level picture without an HD picture of its own finds it by that copy.
    void NoteBlitInto(const Buf& surf, unsigned serial);

    // A colour-keyed sprite is about to be magnified 2x onto the frame `dst` (rect `s` of `src`
    // to rect `d`, visible part `clip`). Returns true if it is a ground animation (water, chimney
    // smoke) that was handled here: its difference to the level picture over the HD background,
    // or left out for still HD water (see ddm_hd.cpp).
    bool DrawGroundAnimation(const Buf& dst, const RECT& d, const RECT& clip, const Buf& src, const RECT& s);
    // Destination pixel (0, 0) of the world surface shows HD level pixel (x, y) (twice the camera position);
    // false while no HD background was drawn in this frame.
    bool LevelOrigin(int& x, int& y);

    // The presenter's level layer (2026-11-22): the HD picture of the level in use (2x, RGB565, mapped) and the
    // game's level picture it was made from (1x, RGB565, the copy taken when it was found); `serial` changes with
    // either (another level, another state of it, another variant). Main thread; false = none (variant off ...).
    struct LevelPictures {
        const uint16_t* hd = nullptr;
        int hdW = 0, hdH = 0;
        const uint16_t* level = nullptr;
        int levelW = 0, levelH = 0;
        unsigned serial = 0;
        uint64_t key = 0;               // the level picture's key (hd\maps4k\<variant>\<key>.hdm4: its 4x picture)
        const char* variant = "";       // the variant folder in use
    };
    bool LevelPicturesNow(LevelPictures& out);
    // the game's folder (where Game.exe lies), with a trailing backslash
    std::string GameFolder();
    // hd\work\ (trailing backslash) if that folder and tools\hd exist - the machine the HD pictures are made on; ""
    // for every player. The collectors of pictures without an HD version (fonts, menu parts, menu frames, sprite frames) write
    // only there and never create it (audit 2026-11-22: two of them created it in every player's game folder).
    const std::string& WorkFolder();

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
    // The presenter's bits: 64 HD patches off, 128 HD menu layer off, 256 dialog backdrop layer off,
    // 512 HD menu cursor off, 1024 test: the vision-cone span clip sees a shorter level, 2048 level
    // layer (HD / 4x) off, 4096 level layer pixels tinted (test), 8192 the windows' pictures the game's 2x sprite
    // routine magnifies not from their HD versions (Sprite2xWindowHd), 16384 (test mode) each such picture, once per
    // size, written to tools\out\harness\sprite2x_<w>x<h>.raw as the game has it.
    void SetDebugFlags(unsigned flags);
    unsigned DebugFlags();

    // Test harness: the level picture the game draws the world from now and the copy the HD picture is compared
    // with (g_base) -> path: "LVPC", u32 w, h, then w*h RGB565 of the picture, w*h of the copy (zeros without one).
    bool DumpLevelPictures(const char* path);

    // Main thread: the colour table (RGB565 -> RGB565) of the game's last colouring of a dialog's
    // backdrop (0x0053B330, hue and factor as the game passed them), nullptr when none was made yet.
    const uint16_t* LastBackdropColouring();
}
