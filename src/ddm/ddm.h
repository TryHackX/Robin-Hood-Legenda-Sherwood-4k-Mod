#pragma once
#include <cstdint>

// "DDM" - in-memory DirectDraw 7 replacement.
//
// Game.exe renders in software into 16-bit DirectDraw surfaces. Measured on the GOG D3D9
// wrapper, ~95 % of the main thread's time was spent inside the wrapper and the GPU driver
// (Blt up to 94 ms and Flip 9-12 ms per 40 ms tick) while the game's own code used ~4 %.
// DDM keeps every surface as plain system memory (Lock is a pointer, Blt is an SSE2 copy) and
// shows the finished frame through DDM::Present (Direct3D 11). The GOG wrapper stays loaded
// but is never called: its DirectDrawCreate/Enumerate exports are redirected here.
namespace DDM {
    bool Install();
    void Shutdown();
    bool IsActive();

    // Crash log helper: lists the surfaces locked last and where `faultAddress` lies relative to them.
    void LogFaultContext(uintptr_t faultAddress);

    // Test harness: write the next `count` surface calls (Blt, Lock, Flip, present) to a text file,
    // one line each. TraceMark adds a line of its own (tick / frame boundaries) while a trace runs.
    void TraceCalls(const char* path, int count);
    bool TraceActive();
    void TraceMark(const char* text);

    // Presenter, main thread: the first frame row of the hedge strip along the bottom edge when the mission's
    // interface drew it since the last call (0 = not drawn) - those rows are interface, never world.
    int TakeHudBandTop();
    // test harness: the mission cursor's own picture drawn last - width << 48 | height << 32 | content hash (0 = none)
    uint64_t LastCursorKey();
    // a surface's pixel (x, y) is the colour key - false when the surface is gone (its serial: a new surface can
    // reuse an old address) or the pixel lies outside it (UiScale: the pointer over a see-through part)
    bool PixelIsKey(const void* surface, unsigned serial, int x, int y, uint16_t key);
    // ... the same without taking it: the band's top row seen last (0 = never drawn)
    int LastHudBandTop();
}
