#pragma once
#include <windows.h>

namespace WindowManager {
    void EnforceHighDPIAwareness();
    bool InitHooks();
    void ShutdownHooks();

    HWND GetGameWindow();
    bool IsAppActive();
    bool IsReallyMinimised(); // what Windows says; the game itself is always told "not minimised"

    // Makes `hWnd` a borderless window covering the primary monitor (used by the in-memory
    // DirectDraw when the game asks for exclusive fullscreen).
    void ApplyBorderless(HWND hWnd);
    // Once per run: puts the freshly created game window in front if it came up behind another one.
    void BringToFrontAtStart(HWND hWnd);
    // Re-hides the Windows pointer while the game window is in the foreground (main thread only).
    void KeepCursorHidden();

    void SetInternalResolution(int width, int height);
    void GetInternalResolution(int& outWidth, int& outHeight);
    void GetMonitorResolution(int& outWidth, int& outHeight);
    RECT GetScaledViewportRect();
    void UpdateCursorClipping(bool active);
}
