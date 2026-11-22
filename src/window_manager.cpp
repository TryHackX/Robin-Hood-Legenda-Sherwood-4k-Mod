#include "window_manager.h"
#include "config.h"
#include "input_fix.h"
#include "logger.h"
#include "pattern.h"
#include "test_harness.h"
#include "watchdog.h"
#include <MinHook.h>
#include <cstring>
#include <algorithm>

namespace WindowManager {
    namespace {
        HWND g_hGameWnd = nullptr;
        WNDPROC g_origWndProc = nullptr;
        bool g_isAppActive = true;
        bool g_hasLocalDDrawWrapper = false;

        int g_internalWidth = 800;
        int g_internalHeight = 600;
        int g_monitorWidth = 1920;
        int g_monitorHeight = 1080;

        int  g_savedMouseParams[3] = { 0, 0, 0 };
        bool g_mouseParamsSaved = false;

        using PFN_CreateWindowExA = HWND(WINAPI*)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
        using PFN_SetWindowLongA  = LONG(WINAPI*)(HWND, int, LONG);
        using PFN_SetWindowPos    = BOOL(WINAPI*)(HWND, HWND, int, int, int, int, UINT);
        using PFN_MoveWindow      = BOOL(WINAPI*)(HWND, int, int, int, int, BOOL);
        using PFN_ShowWindow      = BOOL(WINAPI*)(HWND, int);
        using PFN_IsIconic        = BOOL(WINAPI*)(HWND);

        PFN_CreateWindowExA o_CreateWindowExA = nullptr;
        PFN_SetWindowLongA  o_SetWindowLongA  = nullptr;
        PFN_SetWindowPos    o_SetWindowPos    = nullptr;
        PFN_MoveWindow      o_MoveWindow      = nullptr;
        PFN_ShowWindow      o_ShowWindow      = nullptr;
        PFN_IsIconic        o_IsIconic        = nullptr;

        // The game runs in a borderless window but was written for exclusive fullscreen, where the
        // GDI pointer is not shown. It only calls ShowCursor(FALSE) inside the in-game loop, and
        // DirectInput holds the mouse exclusively, so in the menus a frozen white arrow stays on
        // top of the game's own cursor. Never let Windows set a pointer over the client area.
        LRESULT CALLBACK GameWndProcHook(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
            Watchdog::Beat();
            switch (msg) {
                case WM_CLOSE:
                    // Alt+F4 in a level: the game's own quick save / load keys back before it may write the profile
                    // (the next tick of a level applies the mod's again if the game goes on; audit 2026-11-22)
                    InputFix::RestoreQuickKeys();
                    break;
                case WM_DESTROY:
                    InputFix::RestoreQuickKeys();
                    Watchdog::OnMainWindowDestroyed();
                    break;
                case WM_NCDESTROY: {
                    // last message of this window: forget it so a new one can be subclassed
                    const WNDPROC orig = g_origWndProc;
                    g_origWndProc = nullptr;
                    if (g_hGameWnd == hWnd) {
                        g_hGameWnd = nullptr;
                    }
                    return CallWindowProcA(orig, hWnd, msg, wParam, lParam);
                }
                case WM_SETCURSOR:
                    if (LOWORD(lParam) == HTCLIENT) {
                        SetCursor(nullptr);
                        return TRUE;
                    }
                    break;
                case WM_ACTIVATEAPP:
                    if (TestHarness::Enabled() && wParam == FALSE) {
                        return 0; // background test: the game must keep running without the foreground
                    }
                    g_isAppActive = (wParam != FALSE);
                    if (g_isAppActive) {
                        SetCursor(nullptr);
                        // If the window was minimised by the user (Win+D, taskbar) bring it back
                        // for real: the game cannot tell, IsIconic is forced to FALSE for it.
                        if (o_IsIconic && o_IsIconic(hWnd) && o_ShowWindow) {
                            o_ShowWindow(hWnd, SW_RESTORE);
                        }
                    }
                    Logger::Info("WindowManager: game window %s.", g_isAppActive ? "activated" : "deactivated");
                    break;
                case WM_SETFOCUS:
                    SetCursor(nullptr);
                    break;
                default:
                    break;
            }
            return CallWindowProcA(g_origWndProc, hWnd, msg, wParam, lParam);
        }

        void InstallCursorHider(HWND hWnd) {
            if (!hWnd || g_origWndProc) {
                return;
            }
            SetClassLongPtrA(hWnd, GCLP_HCURSOR, 0);
            g_origWndProc = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrA(hWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&GameWndProcHook)));
            SetCursor(nullptr);
            if (g_origWndProc) {
                g_hGameWnd = hWnd;
                Watchdog::OnMainWindowCreated(hWnd);
            }
        }

        bool DetectLocalDDrawWrapper() {
            // With our own in-memory DirectDraw the GOG wrapper is never used, so the window is ours.
            if (SherwoodConfig::Get().renderer == 1) {
                return false;
            }
            char path[MAX_PATH]{};
            GetModuleFileNameA(nullptr, path, MAX_PATH);
            std::string dir(path);
            size_t pos = dir.find_last_of("\\/");
            dir = (pos != std::string::npos) ? dir.substr(0, pos + 1) : "";
            DWORD attrDxcfg = GetFileAttributesA((dir + "dxcfg.ini").c_str());
            DWORD attrDDraw = GetFileAttributesA((dir + "ddraw.dll").c_str());
            return (attrDxcfg != INVALID_FILE_ATTRIBUTES && attrDDraw != INVALID_FILE_ATTRIBUTES);
        }

        // g_monitorWidth/Height: the size of the window the game is shown in - the whole monitor, or the smaller
        // borderless window of WindowWidth x WindowHeight (2026-11-17, a YouTuber's wish) at g_winX, g_winY
        int g_winX = 0, g_winY = 0;

        void RefreshMonitorDimensions() {
            HMONITOR hMon = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            RECT mon{ 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
            if (GetMonitorInfoA(hMon, &mi)) mon = mi.rcMonitor;
            const int mw = mon.right - mon.left, mh = mon.bottom - mon.top;
            const auto& cfg = SherwoodConfig::Get();
            if (cfg.windowWidth > 0 && cfg.windowHeight > 0 && (cfg.windowWidth < mw || cfg.windowHeight < mh)) {
                g_monitorWidth  = (std::min)(cfg.windowWidth, mw);
                g_monitorHeight = (std::min)(cfg.windowHeight, mh);
                const int x = cfg.windowX >= 0 ? (std::min)(cfg.windowX, mw - g_monitorWidth) : (mw - g_monitorWidth) / 2;
                const int y = cfg.windowY >= 0 ? (std::min)(cfg.windowY, mh - g_monitorHeight) : (mh - g_monitorHeight) / 2;
                g_winX = mon.left + x;
                g_winY = mon.top + y;
            } else {
                g_monitorWidth  = mw;
                g_monitorHeight = mh;
                g_winX = mon.left;
                g_winY = mon.top;
            }
        }

        HWND WINAPI Hook_CreateWindowExA(
            DWORD dwExStyle, LPCSTR lpClassName, LPCSTR lpWindowName, DWORD dwStyle,
            int X, int Y, int nWidth, int nHeight,
            HWND hWndParent, HMENU hMenu, HINSTANCE hInstance, LPVOID lpParam)
        {
            auto& cfg = SherwoodConfig::Get();
            bool isMainGameWindow = (lpWindowName && std::strstr(lpWindowName, "SBDrawManager") != nullptr) ||
                                    (lpClassName && (reinterpret_cast<uintptr_t>(lpClassName) > 0xFFFF) &&
                                     std::strstr(lpClassName, "Robin Hood") != nullptr);

            if (isMainGameWindow && TestHarness::Enabled()) {
                // invisible, never activated, no taskbar button
                dwStyle &= ~static_cast<DWORD>(WS_VISIBLE);
                dwExStyle |= WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
            }
            if (isMainGameWindow) {
                RefreshMonitorDimensions();
                if (cfg.windowedBorderless) {
                    // Always strip WS_EX_TOPMOST so Alt+Tab and Task Manager can appear above the game window
                    dwExStyle &= ~WS_EX_TOPMOST;
                    if (!g_hasLocalDDrawWrapper) {
                        dwStyle = WS_POPUP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN |
                                  (TestHarness::Enabled() ? 0u : static_cast<DWORD>(WS_VISIBLE));
                        X = g_winX;
                        Y = g_winY;
                        nWidth  = g_monitorWidth;
                        nHeight = g_monitorHeight;
                    }
                }
                Logger::Info("CreateWindowExA intercepted for main window: Monitor=%dx%d, Style=0x%08X, ExStyle=0x%08X, LocalWrapper=%d",
                             g_monitorWidth, g_monitorHeight, dwStyle, dwExStyle, g_hasLocalDDrawWrapper ? 1 : 0);
            }

            HWND hWnd = o_CreateWindowExA(dwExStyle, lpClassName, lpWindowName, dwStyle,
                                          X, Y, nWidth, nHeight, hWndParent, hMenu, hInstance, lpParam);

            if (isMainGameWindow && hWnd) {
                g_hGameWnd = hWnd;
                if (cfg.windowedBorderless) {
                    InstallCursorHider(hWnd);
                }
            }
            return hWnd;
        }

        LONG WINAPI Hook_SetWindowLongA(HWND hWnd, int nIndex, LONG dwNewLong) {
            auto& cfg = SherwoodConfig::Get();
            if (hWnd && hWnd == g_hGameWnd && !g_hasLocalDDrawWrapper) {
                if (nIndex == GWL_STYLE && cfg.windowedBorderless) {
                    dwNewLong = static_cast<LONG>(WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN);
                } else if (nIndex == GWL_EXSTYLE && cfg.windowedBorderless) {
                    dwNewLong &= ~WS_EX_TOPMOST;
                }
            }
            if (hWnd && hWnd == g_hGameWnd && TestHarness::Enabled()) {
                if (nIndex == GWL_STYLE) {
                    dwNewLong &= ~static_cast<LONG>(WS_VISIBLE);
                } else if (nIndex == GWL_EXSTYLE) {
                    dwNewLong |= static_cast<LONG>(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW);
                }
            }
            return o_SetWindowLongA(hWnd, nIndex, dwNewLong);
        }

        BOOL WINAPI Hook_SetWindowPos(HWND hWnd, HWND hWndInsertAfter, int X, int Y, int cx, int cy, UINT uFlags) {
            auto& cfg = SherwoodConfig::Get();
            if (hWnd && hWnd == g_hGameWnd && cfg.windowedBorderless && !g_hasLocalDDrawWrapper) {
                RefreshMonitorDimensions();
                if (hWndInsertAfter == HWND_TOPMOST) {
                    hWndInsertAfter = HWND_NOTOPMOST;
                }
                X = g_winX;
                Y = g_winY;
                cx = g_monitorWidth;
                cy = g_monitorHeight;
                uFlags &= ~(SWP_NOMOVE | SWP_NOSIZE);
                uFlags |= SWP_NOZORDER | SWP_NOACTIVATE;
            }
            if (hWnd && hWnd == g_hGameWnd && TestHarness::Enabled()) {
                uFlags &= ~static_cast<UINT>(SWP_SHOWWINDOW);
                uFlags |= SWP_NOACTIVATE | SWP_NOZORDER;
            }
            return o_SetWindowPos(hWnd, hWndInsertAfter, X, Y, cx, cy, uFlags);
        }

        BOOL WINAPI Hook_MoveWindow(HWND hWnd, int X, int Y, int nWidth, int nHeight, BOOL bRepaint) {
            auto& cfg = SherwoodConfig::Get();
            if (hWnd && hWnd == g_hGameWnd && cfg.windowedBorderless && !g_hasLocalDDrawWrapper) {
                RefreshMonitorDimensions();
                X = g_winX;
                Y = g_winY;
                nWidth  = g_monitorWidth;
                nHeight = g_monitorHeight;
            }
            return o_MoveWindow(hWnd, X, Y, nWidth, nHeight, bRepaint);
        }

        BOOL WINAPI Hook_ShowWindow(HWND hWnd, int nCmdShow) {
            auto& cfg = SherwoodConfig::Get();
            if (hWnd && hWnd == g_hGameWnd && TestHarness::Enabled()) {
                return TRUE; // stays hidden
            }
            if (hWnd && hWnd == g_hGameWnd && cfg.windowedBorderless) {
                if (nCmdShow == SW_MINIMIZE || nCmdShow == SW_FORCEMINIMIZE) {
                    return TRUE;
                }
            }
            return o_ShowWindow(hWnd, nCmdShow);
        }

        // When the game loses focus RHGame::Run (0x0050F941 -> 0x00513070) calls CloseWindow, i.e.
        // minimises its own window, as an exclusive-fullscreen game would. IsIconic is forced to
        // FALSE below, so after a few Alt+Tabs the game believed it was restored while the window
        // was still minimised and kept drawing into nothing. A borderless window simply stays
        // where it is, behind the other windows.
        using PFN_CloseWindow = BOOL(WINAPI*)(HWND);
        PFN_CloseWindow o_CloseWindow = nullptr;

        BOOL WINAPI Hook_CloseWindow(HWND hWnd) {
            if (hWnd && hWnd == g_hGameWnd && SherwoodConfig::Get().windowedBorderless) {
                return TRUE;
            }
            return o_CloseWindow(hWnd);
        }

        BOOL WINAPI Hook_IsIconic(HWND hWnd) {
            auto& cfg = SherwoodConfig::Get();
            if (hWnd && hWnd == g_hGameWnd && cfg.windowedBorderless) {
                // Prevents infinite spin-wait loop at 0x00409633 inside WM_ACTIVATEAPP on Windows 11!
                return FALSE;
            }
            return o_IsIconic(hWnd);
        }
    }

    void EnforceHighDPIAwareness() {
        HMODULE hUser32 = GetModuleHandleA("user32.dll");
        if (hUser32) {
            using PFN_SetProcessDpiAwarenessContext = BOOL(WINAPI*)(HANDLE);
            auto pSetCtx = reinterpret_cast<PFN_SetProcessDpiAwarenessContext>(
                GetProcAddress(hUser32, "SetProcessDpiAwarenessContext"));
            if (pSetCtx) {
                // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = (HANDLE)-4
                if (pSetCtx(reinterpret_cast<HANDLE>(-4))) {
                    Logger::Info("High-DPI: DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 enabled (1:1 pixel mapping for 1440p/4K).");
                    return;
                }
            }
        }

        HMODULE hShcore = LoadLibraryA("shcore.dll");
        if (hShcore) {
            using PFN_SetProcessDpiAwareness = HRESULT(WINAPI*)(int);
            auto pSetDpi = reinterpret_cast<PFN_SetProcessDpiAwareness>(
                GetProcAddress(hShcore, "SetProcessDpiAwareness"));
            if (pSetDpi && SUCCEEDED(pSetDpi(2))) { // PROCESS_PER_MONITOR_DPI_AWARE = 2
                Logger::Info("High-DPI: PROCESS_PER_MONITOR_DPI_AWARE enabled via shcore.dll.");
                return;
            }
        }

        if (hUser32) {
            using PFN_SetProcessDPIAware = BOOL(WINAPI*)();
            auto pSetAware = reinterpret_cast<PFN_SetProcessDPIAware>(
                GetProcAddress(hUser32, "SetProcessDPIAware"));
            if (pSetAware) {
                pSetAware();
                Logger::Info("High-DPI: Fallback SetProcessDPIAware enabled.");
            }
        }
    }

    bool InitHooks() {
        RefreshMonitorDimensions();
        g_hasLocalDDrawWrapper = DetectLocalDDrawWrapper();
        Logger::Info("Detected primary monitor native resolution: %dx%d (Local DDraw Wrapper=%d)",
                     g_monitorWidth, g_monitorHeight, g_hasLocalDDrawWrapper ? 1 : 0);

        HMODULE hUser32 = GetModuleHandleA("user32.dll");
        if (!hUser32) return false;

        auto HookAPI = [&](const char* name, LPVOID detour, LPVOID* orig) {
            FARPROC target = GetProcAddress(hUser32, name);
            if (target && MH_CreateHook(reinterpret_cast<LPVOID>(target), detour, orig) == MH_OK) {
                MH_EnableHook(reinterpret_cast<LPVOID>(target));
            }
        };

        HookAPI("CreateWindowExA", reinterpret_cast<LPVOID>(&Hook_CreateWindowExA), reinterpret_cast<LPVOID*>(&o_CreateWindowExA));
        HookAPI("ShowWindow",      reinterpret_cast<LPVOID>(&Hook_ShowWindow),      reinterpret_cast<LPVOID*>(&o_ShowWindow));
        HookAPI("IsIconic",        reinterpret_cast<LPVOID>(&Hook_IsIconic),        reinterpret_cast<LPVOID*>(&o_IsIconic));
        HookAPI("CloseWindow",     reinterpret_cast<LPVOID>(&Hook_CloseWindow),     reinterpret_cast<LPVOID*>(&o_CloseWindow));

        if (!g_hasLocalDDrawWrapper) {
            HookAPI("SetWindowLongA", reinterpret_cast<LPVOID>(&Hook_SetWindowLongA), reinterpret_cast<LPVOID*>(&o_SetWindowLongA));
            HookAPI("SetWindowPos",   reinterpret_cast<LPVOID>(&Hook_SetWindowPos),   reinterpret_cast<LPVOID*>(&o_SetWindowPos));
            HookAPI("MoveWindow",     reinterpret_cast<LPVOID>(&Hook_MoveWindow),     reinterpret_cast<LPVOID*>(&o_MoveWindow));
        }

        Logger::Info("WindowManager hooks installed successfully.");
        return true;
    }

    void ShutdownHooks() {
        ClipCursor(nullptr);
    }

    HWND GetGameWindow() {
        return g_hGameWnd;
    }

    void ApplyBorderless(HWND hWnd) {
        if (!hWnd || !SherwoodConfig::Get().windowedBorderless) {
            return;
        }
        RefreshMonitorDimensions();
        // call the real APIs: our own hooks on them only exist to stop the game undoing this
        const auto setLong = o_SetWindowLongA ? o_SetWindowLongA : &SetWindowLongA;
        const auto setPos  = o_SetWindowPos ? o_SetWindowPos : &SetWindowPos;

        const bool hidden = TestHarness::Enabled(); // background test: the window never shows up
        LONG style = GetWindowLongA(hWnd, GWL_STYLE);
        style &= ~static_cast<LONG>(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
        style |= static_cast<LONG>(WS_POPUP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN);
        if (hidden) {
            style &= ~static_cast<LONG>(WS_VISIBLE);
        } else {
            style |= static_cast<LONG>(WS_VISIBLE);
        }
        setLong(hWnd, GWL_STYLE, style);
        LONG exStyle = GetWindowLongA(hWnd, GWL_EXSTYLE);
        exStyle &= ~static_cast<LONG>(WS_EX_TOPMOST | WS_EX_CLIENTEDGE | WS_EX_WINDOWEDGE | WS_EX_DLGMODALFRAME);
        if (hidden) {
            exStyle |= static_cast<LONG>(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW);
        }
        setLong(hWnd, GWL_EXSTYLE, exStyle);
        setPos(hWnd, HWND_NOTOPMOST, g_winX, g_winY, g_monitorWidth, g_monitorHeight,
               SWP_FRAMECHANGED | (hidden ? (SWP_NOACTIVATE | SWP_NOZORDER | SWP_HIDEWINDOW) : SWP_SHOWWINDOW));
        InstallCursorHider(hWnd);
        Logger::Info("WindowManager: borderless window %dx%d at %d,%d applied to hwnd=0x%08X%s.",
                     g_monitorWidth, g_monitorHeight, g_winX, g_winY, reinterpret_cast<uintptr_t>(hWnd),
                     hidden ? " (hidden: test mode)" : "");
        if (!hidden) {
            BringToFrontAtStart(hWnd);
        }
    }

    // The game takes its "active" state from the first DirectInput acquire, about a second after
    // the window appears, and DirectInput only gives the devices to the foreground window. A
    // game started through the UAC prompt or a launcher often comes up BEHIND the window the
    // player was in: it then sat there frozen until clicked (29 s in the log of 2026-11-14).
    // The player has just started the game, so put it in front - once.
    void BringToFrontAtStart(HWND hWnd) {
        static bool s_done = false;
        if (s_done) return;
        s_done = true;
        char skip[8]{};
        if (GetEnvironmentVariableA("SHERWOODFIX_NO_FOREGROUND", skip, sizeof(skip)) > 0) {
            return; // scripted start (tools\gamectl.ps1): the script decides about the foreground
        }
        if (GetForegroundWindow() == hWnd) return;
        const HWND before = GetForegroundWindow();
        const char* how = "SetForegroundWindow";
        SetForegroundWindow(hWnd);
        if (GetForegroundWindow() != hWnd && before) {
            // allowed for the thread that shares its input state with the foreground thread
            how = "attached input";
            const DWORD fgThread = GetWindowThreadProcessId(before, nullptr);
            const DWORD self = GetCurrentThreadId();
            if (fgThread && fgThread != self && AttachThreadInput(self, fgThread, TRUE)) {
                BringWindowToTop(hWnd);
                SetForegroundWindow(hWnd);
                AttachThreadInput(self, fgThread, FALSE);
            }
        }
        if (GetForegroundWindow() != hWnd) {
            // Windows lets the process that produced the latest input event take the foreground
            how = "Alt key";
            INPUT alt{};
            alt.type = INPUT_KEYBOARD;
            alt.ki.wVk = VK_MENU;
            SendInput(1, &alt, sizeof(INPUT));
            SetForegroundWindow(hWnd);
            alt.ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(1, &alt, sizeof(INPUT));
        }
        if (GetForegroundWindow() != hWnd) {
            // what Alt+Tab itself calls
            how = "SwitchToThisWindow";
            using PFN_Switch = void(WINAPI*)(HWND, BOOL);
            if (const auto fn = reinterpret_cast<PFN_Switch>(GetProcAddress(GetModuleHandleA("user32.dll"), "SwitchToThisWindow"))) {
                fn(hWnd, TRUE);
                Sleep(50);
            }
        }
        // (Measured 2026-11-14 with the workstation locked: every one of these is refused then -
        // nothing can take the foreground behind the lock screen. Not a failure of the methods.)
        const bool front = GetForegroundWindow() == hWnd;
        if (!front) {
            // at least be the window the player sees: one click on it then activates the game
            const auto setPos = o_SetWindowPos ? o_SetWindowPos : &SetWindowPos;
            setPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            setPos(hWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        Logger::Info("WindowManager: game window was not in front at start - brought to the foreground: %s (%s).",
                     front ? "ok" : "refused by Windows, window raised above the others instead", how);
    }

    void KeepCursorHidden() {
        if (g_hGameWnd && g_origWndProc && GetForegroundWindow() == g_hGameWnd) {
            SetCursor(nullptr);
        }
    }

    bool IsReallyMinimised() {
        // the game itself is always told "no" (Hook_IsIconic); this asks Windows
        return g_hGameWnd && o_IsIconic && o_IsIconic(g_hGameWnd) != FALSE;
    }

    void SetInternalResolution(int width, int height) {
        if (width > 0 && height > 0) {
            g_internalWidth  = width;
            g_internalHeight = height;
            Logger::Info("WindowManager: Internal viewport resolution updated to %dx%d (Monitor Output: %dx%d)",
                         g_internalWidth, g_internalHeight, g_monitorWidth, g_monitorHeight);
        }
    }

    void GetInternalResolution(int& outWidth, int& outHeight) {
        outWidth  = g_internalWidth;
        outHeight = g_internalHeight;
    }

    void GetMonitorResolution(int& outWidth, int& outHeight) {
        outWidth  = g_monitorWidth;
        outHeight = g_monitorHeight;
    }
}
