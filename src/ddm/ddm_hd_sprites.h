#pragma once
#include "ddm_blit.h"

// HD sprites.
//
// The game keeps every sprite frame in a "bank" (DATA\RobinHood.bks) and decodes a frame into a
// small surface right before it draws it; at the 2x zoom the surface is then magnified - by a
// colour-keyed DirectDraw stretch (roofs, trees, objects, background animations) or by the
// game's own shadow-aware routine (characters). Both are places where an upscaled version of
// the frame can be drawn 1:1 instead.
//
// Which frame a surface holds is learnt from the bank's decode function (0x00504CD0): it is told
// the frame number and the memory it decodes into. The upscaled frames come from
//     hd\sprites\<variant>\sprites.idx / sprites.dat      (tools\hd\upscale_sprites.py)
// with the same variant names as the HD backgrounds. A frame that is not in the store, or whose
// surface no longer looks like it, is drawn the old way.
namespace DDM::HD::Sprites {
    void InstallHooks();

    // Background variant switched ("off" = original pixels): use the sprite store of that name.
    void SetVariant(const char* name);

    // Colour-keyed exact 2x stretch of rect `s` of `src` onto rect `d` of `dst` (pixels inside
    // `clip`). Returns true if the HD frame was drawn instead.
    bool TryStretch(const Buf& dst, const RECT& d, const RECT& clip, const Buf& src, const RECT& s, uint16_t key);

    // The game's own 2x sprite routine (see ddm_hd.cpp): `src` points into a locked sprite
    // surface, `dst` into the locked frame; cols x rows source pixels. `marker` pixels darken
    // the destination by factor/256. Returns true if the HD frame was drawn instead.
    bool TryShadowBlit(const uint16_t* src, uint16_t* dst, uint32_t cols, uint32_t rows,
                       int srcPitch, int dstPitch, uint32_t factor, uint32_t marker, bool keyed);

    // The game's stretched alpha blit (0x005E8B10, see ddm_hd.cpp; the ring under the selected
    // character, light effects): colour-keyed, alpha from the picture's own green channel;
    // cols x rows source pixels magnified exactly 2x. Returns true if the HD frame was drawn.
    bool TryBlendBlit(const uint16_t* src, uint16_t* dst, uint32_t cols, uint32_t rows, int srcPitch, int dstPitch, uint32_t key);

    // Something was drawn into the surface whose pixels start at `bits` (a blit, GDI): whatever
    // frame the decoder put there is no longer what it holds. Main thread.
    void OnSurfaceWritten(const uint8_t* bits);

    // In-between frames of sprite animations (hd\sprites\_between, tools\hd\make_inbetweens.py):
    // a sprite that changed its frame in this game tick is drawn with a generated frame half-way
    // between the old and the new one during the first part of the tick. OnTickFrame: the game
    // starts drawing a tick's own frame (main thread). BetweenAvailable: main thread (opens the store).
    void OnTickFrame();
    void SetBetween(bool on);
    bool BetweenOn();
    bool BetweenAvailable();
    unsigned BetweenDrawn();

    // For the log / test harness.
    unsigned FramesDrawn();
    double WorstColourDiff();   // largest colour difference (of 255) between an accepted frame and the game's surface

    // Test harness: write "frame size surface kind ..." for every sprite surface to a text file
    // (done on the main thread at the next game tick; DumpPending() turns false when written).
    void RequestDump(const char* path);
    bool DumpPending();
    void OnGameTick();

    // Frames the game makes at run time (icons above heads, markers ...; numbers beyond the bank
    // file's) have no place in those stores: they are recognised by content and their upscaled
    // versions come from hd\sprites\_icons\icons.idx / icons.dat (tools\hd\upscale_icons.py).
    // Test harness: write all run-time frames the bank holds now to a binary file.
    void RequestRunTimeDump(const char* path);
    bool RunTimeDumpPending();

    // Test harness: count, over `ticks` game ticks, every 2x magnification that was done WITHOUT
    // an upscaled frame (which frame, by which routine, why) -> text file, most frequent first.
    void RequestMissReport(const char* path, int ticks);
    bool MissReportPending();
    bool MissActive();
    // path: 0 DirectDraw stretch, 1 the game's own routine, 2 DirectDraw mirrored stretch, 3 the game's alpha blit
    void NoteFallback(const uint8_t* bits, int w, int h, int path);
}
