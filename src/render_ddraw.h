#pragma once
#include <windows.h>
#include <cstdint>
#include <cstddef>

namespace RenderDDraw {
    bool Init();
    void Shutdown();

    // SIMD SSE2 RGB565 -> XRGB8888 fast frame converter for GPU upload
    void ConvertRGB565ToXRGB8888_SSE2(
        const uint16_t* src, int srcPitchBytes,
        uint32_t* dst, int dstPitchBytes,
        int width, int height);

    // Draws the FPS overlay into a raw RGB565 frame (used by the in-memory DirectDraw presenter).
    // `src`: the same picture in ordinary memory (the text boxes are darkened from it, `base` -
    // a mapped texture - is only written); nullptr = read `base` itself.
    void DrawOverlayRGB565(uint8_t* base, LONG pitch, int width, int height, const uint8_t* src = nullptr, LONG srcPitch = 0);

    // A short message from the mod (5x7 font: letters, digits, . : | ( ) -), drawn into the frame
    // for `seconds`, independent of ShowFPSOverlay. Main thread.
    void ShowNotice(const char* text, double seconds);
}
