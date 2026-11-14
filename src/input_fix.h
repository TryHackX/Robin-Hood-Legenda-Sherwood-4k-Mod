#pragma once

// Input fixes.
//
// 1. Mouse report coalescing ("emulate a 125 Hz mouse"). Game.exe queues every DirectInput mouse
//    record (SBInputThreadProc @ 0x005F0A40) and RHGame::Run runs its input pipeline once per
//    queued report. The mouse branch of the input thread is replaced: pending records are
//    drained, X/Y movement is summed and released at most MouseReportRateHz times per second.
//    Buttons and wheel are never delayed, and pending movement is flushed in front of them.
//    (Measured: not the cause of the game's lag - 0.01 ms per event - kept as a safety net for
//    very high polling rates.)
//
// 2. DirectInput re-acquire. When the game window is activated the game acquires keyboard and
//    mouse exactly once (SBInput::Acquire @ 0x005F9EA0, called from the window procedure) and
//    ignores the result. If Windows has not finished the foreground switch at that moment the
//    acquire fails and the game is left without input - it looks frozen - until the next
//    Alt+Tab. Failed acquires are retried from the main thread until they succeed.
namespace InputFix {
    void Initialize();
    void Shutdown();

    // Main thread, called every tick / presented frame: retries a failed acquire.
    void Poll();
}
