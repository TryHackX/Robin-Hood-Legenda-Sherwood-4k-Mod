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

    // Main thread, called every tick / presented frame: retries a failed acquire; while a level runs,
    // puts the keys of QuickSaveKey / QuickLoadKey into the game's key settings.
    void Poll();

    // UiScale, main thread: a dialog loop of the game starts (its own key screen too) - the game's quick
    // save / load keys are back until a level ticks again.
    void OnDialogLoop(bool entering);
    // The game window closes (WM_CLOSE / WM_DESTROY): the game's own quick keys back before it may save the profile.
    void RestoreQuickKeys();

    // The game's own key of quick save (false) / quick load (true), DirectInput code; -1 = not known yet.
    int GameQuickKey(bool load);

    // Why a DirectInput key should not be the quick save (false) / quick load (true) key - one of the mod's
    // own keys (F6..F10), the other quick key, or a key of another action in the game's key settings;
    // nullptr = free. The F10 panel skips such keys, a clash set in the INI is logged.
    const char* QuickKeyClash(int dik, bool load);

    // Who in the game has this DirectInput key: one of its actions in its key settings, or the quick save / load
    // key as the mod uses it; nullptr = nobody. The mod's own keys must not sit on such a key (ModKeys).
    const char* GameKeyUse(int dik);
}
