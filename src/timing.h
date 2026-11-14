#pragma once
#include <windows.h>
#include <cstdint>

namespace Timing {
    bool Init();
    void Shutdown();

    // True while the pacer is drawing an extra frame between two game ticks (main thread).
    bool InExtraFrame();

    // Where the frame being drawn lies inside its 40 ms game tick: 0 = the tick's start, towards
    // 1 = its end. -1 when no extra frames are drawn between ticks (every tick is one picture).
    float RenderPhase();

    // High-precision monotonic clock in milliseconds & microseconds (QPC-backed)
    double GetElapsedMilliseconds();
    uint64_t GetElapsedMicroseconds();
    double GetCurrentFPS();

    // Hybrid QPC frame limiter (Sleep + _mm_pause spin-wait)
    void FrameLimitStep(double targetFPS);
}
