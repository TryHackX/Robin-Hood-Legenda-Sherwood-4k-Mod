#pragma once
#include <windows.h>
#include <cstdint>

// Software blitters for 16-bit (RGB565) surfaces. Callers pass rectangles that are already
// clipped to both buffers.
namespace DDM {
    struct Buf {
        uint8_t* bits;
        int      pitch;
        int      w;
        int      h;
    };

    void Fill16(const Buf& dst, const RECT& r, uint16_t color);

    // 1:1 copy of src rect `sr` to (dx, dy). Safe when dst and src are the same buffer.
    void Copy16(const Buf& dst, int dx, int dy, const Buf& src, const RECT& sr);

    // 1:1 copy skipping pixels equal to `key`.
    void CopyKey16(const Buf& dst, int dx, int dy, const Buf& src, const RECT& sr, uint16_t key);

    // Nearest-neighbour stretch. `sr` maps onto the whole of `dr`; only pixels inside `clip`
    // (a sub-rectangle of dr that lies inside dst) are written.
    void Stretch16(const Buf& dst, const RECT& dr, const Buf& src, const RECT& sr, const RECT& clip,
                   bool keyed, uint16_t key, bool mirrorX, bool mirrorY);

    // Exact 2x magnification of a sprite with bilinear filtering instead of 2x2 blocks.
    // `sr` maps onto `dr` (twice its size); pixels inside `clip` are written. Pixels equal to
    // `key`, and everything outside the source SURFACE, count as transparent, so the silhouette
    // gets a soft edge against whatever is in dst already; source pixels outside `sr` but inside
    // the surface are still used as neighbours (no seam where a sprite is clipped).
    // Pixels equal to `marker` are the game's shadow: they darken dst (dst * factor / 256, the
    // game's own formula with the channel masks `maskBGR`). kNoColor disables key or marker.
    constexpr uint32_t kNoColor = 0x10000;
    // SmoothSprite2x16 on this thread: true = beyond the source SURFACE the edge pixel continues
    // (instead of transparent) - pictures that are tiles of a larger one
    void SetSmoothEdgeClamp(bool on);
    void SmoothSprite2x16(const Buf& dst, const RECT& dr, const Buf& src, const RECT& sr, const RECT& clip,
                          uint32_t key, uint32_t marker, uint32_t factor, const uint16_t* maskBGR,
                          bool mirrorX, bool mirrorY);
    // The same for interface pictures (no shadow, not mirrored, `sr` inside the surface), main
    // thread only: the filtered picture is kept and drawn again while its source pixels stay the
    // same - the HUD is not filtered anew in every frame. Same pixels as SmoothSprite2x16.
    void SmoothPicture2x16(const Buf& dst, const RECT& dr, const Buf& src, const RECT& sr, const RECT& clip, uint32_t key);
}
