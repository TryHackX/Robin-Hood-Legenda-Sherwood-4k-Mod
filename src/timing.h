#pragma once
#include <windows.h>
#include <cstdint>

namespace Timing {
    bool Init();
    void Shutdown();

    // True while the pacer is drawing an extra frame between two game ticks (main thread).
    bool InExtraFrame();

    // True while any RHGame::RenderFrame runs: the tick's own frame, an extra frame or a dialog's
    // backdrop (main thread; used by the co-op probe to tell drawing from game logic).
    bool InRenderFrame();

    // True while a dialog renders the mission once for its backdrop (0x0053B6B0 calls
    // RHGame::RenderFrame(0, 0) at 0x0053B6CD: world and windows into the back buffer, nothing
    // presented) - not a frame of the mission the player sees (main thread).
    bool InBackdropRender();
    // Counts those renders: a dialog started after one takes the level from the back buffer.
    unsigned BackdropRenderCount();
    // The last RenderFrame was such a render (the back buffer holds it; the front buffer an older,
    // perhaps in-between frame - a different picture of the level).
    bool LastRenderWasBackdrop();

    // Where the frame being drawn lies inside its 40 ms game tick: 0 = the tick's start, towards
    // 1 = its end. -1 when no extra frames are drawn between ticks (every tick is one picture).
    float RenderPhase();

    // High-precision monotonic clock in microseconds (QPC-backed)
    uint64_t GetElapsedMicroseconds();
}
