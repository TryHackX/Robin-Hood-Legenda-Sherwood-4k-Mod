#pragma once
#include <windows.h>
#include <cstdint>

// Diagnostics: per-tick cost accounting (DirectDraw / audio / input-event loop) and an
// optional sampling profiler of the game's main thread. Everything here is measurement
// only - it never changes game behaviour.
namespace Diag {
    enum Counter : int {
        // DirectDraw calls made on the main thread
        C_DD_LOCK = 0,
        C_DD_UNLOCK,
        C_DD_BLT,
        C_DD_FLIP,
        // any DirectDraw call made on another thread (should stay ~0)
        C_DD_OTHER_THREAD,
        // full-frame presents (Blt/Flip to the primary surface)
        C_PRESENT,
        // input pipeline
        C_IN_RAW,        // raw DirectInput mouse records read
        C_IN_EMIT,       // records queued for the game after coalescing
        C_IN_POP,        // events consumed by RHGame::Run; time = processing between pops
        // FMOD
        C_SND_PLAY,
        C_SND_LOAD,
        C_SND_FAIL,      // FSOUND_PlaySound(Ex) returned -1 (no channel / error)
        C_SND_EXTRA,     // sound started while an extra frame was being drawn
        // sub-frame camera renders done by the pacer
        C_SUBFRAME,
        // in-memory DirectDraw: blits by kind (count + time) and pixels moved (value)
        C_BLT_COPY,
        C_BLT_KEY,
        C_BLT_FILL,
        C_BLT_STRETCH,
        C_PX_COPY,
        C_PX_KEY,
        C_PX_FILL,
        C_PX_STRETCH,
        // motion smoothing: ticks captured (count) and objects that moved (value)
        C_MOTION,
        C_COUNT
    };

    void Init();
    void Shutdown();
    void StopProfiler();                  // process is about to end: take no more samples

    uint64_t Now();                       // raw QPC ticks
    double   TicksToMs(uint64_t ticks);
    bool     IsMainThread();
    DWORD    MainThreadId();

    void Add(Counter c, uint64_t ticks = 0);
    void AddValue(Counter c, uint64_t value); // accumulates `value` without counting a call

    struct Scope {
        Counter  c;
        uint64_t t0;
        explicit Scope(Counter counter);
        ~Scope();
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    };

    // Called by the game-loop pacer: end of a tick's CPU work / start of the next one.
    void OnTickWorkEnd(double mainWorkUs);
    void OnTickWorkBegin();
}
