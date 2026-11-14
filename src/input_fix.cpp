#include "input_fix.h"
#include "config.h"
#include "diag.h"
#include "logger.h"
#include "pattern.h"
#include "settings_menu.h"
#include "test_harness.h"
#include "ui_scale.h"
#include "window_manager.h"

#include <windows.h>
#include <MinHook.h>
#include <cstdint>
#include <cstring>

namespace {
    // DirectInput 7 DIDEVICEOBJECTDATA (the game passes cbObjectData = 0x10)
    struct DiRecord {
        uint32_t ofs;       // 0 = X, 4 = Y, 8 = wheel, 0x0C.. = buttons
        uint32_t data;
        uint32_t timeStamp;
        uint32_t sequence;
    };

    // Addresses inside SBInputThreadProc (0x005F0A40) and the input module
    constexpr uintptr_t kAddrWaitCall       = 0x005F0B0F; // call [WaitForMultipleObjects]
    constexpr uintptr_t kAddrMouseLoop      = 0x005F0B22; // do { ReadMouseData; ProcessInputEvent; ... } while (more)
    constexpr uintptr_t kAddrMouseLoopExit  = 0x005F0B82; // push csCursor / EnterCriticalSection / ...
    constexpr uintptr_t kAddrInputMgrPtr    = 0x006C1320; // SBInput*, mouse IDirectInputDevice at +0x0C
    constexpr uintptr_t kAddrEventCount     = 0x006C1268; // number of queued events
    constexpr uintptr_t kAddrCursorEnabled  = 0x006C12AC; // 1 = software cursor tracking active

    using PFN_ProcessInputEvent = uint8_t(__cdecl*)(uint32_t type, const DiRecord* rec);
    using PFN_PeekEvent         = uint8_t(__cdecl*)(int index, void* evMouse, void* evKeys);
    using PFN_UpdateCursor      = void(__cdecl*)(void* evMouse);

    const auto g_fnProcessInputEvent = reinterpret_cast<PFN_ProcessInputEvent>(0x005F0C20);
    const auto g_fnPeekEvent         = reinterpret_cast<PFN_PeekEvent>(0x005F0D20);
    const auto g_fnUpdateCursor      = reinterpret_cast<PFN_UpdateCursor>(0x005F45D0);

    // State below is touched only by the game's input thread.
    int32_t  g_accX = 0;
    int32_t  g_accY = 0;
    bool     g_pending = false;
    uint32_t g_lastTimeStamp = 0;
    uint32_t g_lastSequence = 0;
    uint64_t g_lastEmitQpc = 0;
    uint64_t g_intervalTicks = 0;
    LARGE_INTEGER g_freq{};

    // A left click on a rolled-up end of the opened map closes it: the game closes the map with
    // the right button, so the press (and its release) go to the game as right button.
    bool g_leftAsRight = false;     // input thread
    // The map's window is dragged by its edges with the right button too: the game drags with the
    // left one, so a right press on an edge goes to the game as a left press, and its release as
    // a left release - followed by a right click when the mouse did not move (a right click on
    // the map closes it, as in the game).
    bool  g_rightAsLeft = false;    // input thread
    float g_rightPressX = 0.0f, g_rightPressY = 0.0f;

    void Queue(const DiRecord& rec, uint8_t* evMouse, void* evKeys);

    void Emit(const DiRecord& recIn, uint8_t* evMouse, void* evKeys) {
        if (SettingsMenu::OnMouseButton(recIn.ofs, recIn.data)) {
            return;     // a click on the mod's settings panel, not for the game
        }
        DiRecord rec = recIn;
        const float cx = *reinterpret_cast<const float*>(0x006C1300);
        const float cy = *reinterpret_cast<const float*>(0x006C1304);
        if (rec.ofs == 0x0C) {
            if (rec.data & 0x80) {
                g_leftAsRight = UiScale::OnMapScrollEnd(cx, cy);
                if (g_leftAsRight) rec.ofs = 0x0D;
            } else if (g_leftAsRight) {
                g_leftAsRight = false;
                rec.ofs = 0x0D;
            }
        } else if (rec.ofs == 0x0D) {
            if (rec.data & 0x80) {
                g_rightAsLeft = UiScale::OnMapEdge(cx, cy);
                if (g_rightAsLeft) {
                    g_rightPressX = cx;
                    g_rightPressY = cy;
                    UiScale::NoteEdgePress(true);
                    rec.ofs = 0x0C;
                }
            } else if (g_rightAsLeft) {
                g_rightAsLeft = false;
                rec.ofs = 0x0C;
                Queue(rec, evMouse, evKeys);
                UiScale::NoteEdgePress(false);
                const float moved = (cx > g_rightPressX ? cx - g_rightPressX : g_rightPressX - cx) +
                                    (cy > g_rightPressY ? cy - g_rightPressY : g_rightPressY - cy);
                if (moved < 6.0f) {     // a right click on the edge: closes the map as anywhere on it
                    Queue(DiRecord{ 0x0D, 0x80, rec.timeStamp, rec.sequence }, evMouse, evKeys);
                    Queue(DiRecord{ 0x0D, 0x00, rec.timeStamp, rec.sequence }, evMouse, evKeys);
                }
                return;
            }
        }
        Queue(rec, evMouse, evKeys);
    }

    // Queue one record exactly the way the original loop body did (0x005F0B34..0x005F0B7B).
    void Queue(const DiRecord& rec, uint8_t* evMouse, void* evKeys) {
        const int before = *reinterpret_cast<const int*>(kAddrEventCount);
        g_fnProcessInputEvent(0, &rec);
        const int after = *reinterpret_cast<const int*>(kAddrEventCount);

        // The original re-read the newest queued event and applied it to the cursor even when
        // nothing had been queued, which re-applied the previous movement. Only do it for a
        // record that really went in.
        if (after == before + 1) {
            Diag::Add(Diag::C_IN_EMIT);
            if (*reinterpret_cast<const uint8_t*>(kAddrCursorEnabled) == 1) {
                g_fnPeekEvent(after - 1, evMouse, evKeys);
                g_fnUpdateCursor(evMouse);
            }
        }
        // x, y, wheel deltas are per-record; the button bytes at +6..+8 persist on purpose
        std::memset(evMouse, 0, 6);
    }

    // Cursor speed. The game moves its own cursor one internal pixel per mouse count. When the
    // internal resolution is lower than the window (1280x720 shown at 2560x1440), each internal
    // pixel covers several screen pixels and the same hand movement would carry the cursor
    // that many times further. MouseSensitivity=0 scales the counts by internal width / window
    // width, so the pointer covers the same distance on the screen at any internal resolution.
    float g_fracX = 0.0f; // input thread only: part of a pixel not handed to the game yet
    float g_fracY = 0.0f;

    float SensitivityScale() {
        const auto& cfg = SherwoodConfig::Get();
        // menu zoom: the middle of the frame is shown larger - same speed on the screen
        const float zoom = UiScale::MenuZoomMouseScale();
        if (cfg.mouseSensitivity > 0) {
            return static_cast<float>(cfg.mouseSensitivity) / 100.0f * zoom;
        }
        int internalW = 0, internalH = 0, monitorW = 0, monitorH = 0;
        WindowManager::GetInternalResolution(internalW, internalH);
        WindowManager::GetMonitorResolution(monitorW, monitorH);
        if (internalW <= 0 || monitorW <= 0 || internalW >= monitorW) {
            return zoom;
        }
        return static_cast<float>(internalW) / static_cast<float>(monitorW) * zoom;
    }

    int32_t ScaleCounts(int32_t counts, float scale, float& frac) {
        if (scale == 1.0f) {
            frac = 0.0f;
            return counts;
        }
        const float value = static_cast<float>(counts) * scale + frac;
        const auto whole = static_cast<int32_t>(value); // towards zero; the remainder keeps its sign
        frac = value - static_cast<float>(whole);
        return whole;
    }

    void FlushMovement(uint8_t* evMouse, void* evKeys) {
        const float scale = SensitivityScale();
        const int32_t outX = ScaleCounts(g_accX, scale, g_fracX);
        const int32_t outY = ScaleCounts(g_accY, scale, g_fracY);
        if (outX != 0) {
            Emit(DiRecord{ 0, static_cast<uint32_t>(outX), g_lastTimeStamp, g_lastSequence }, evMouse, evKeys);
        }
        if (outY != 0) {
            Emit(DiRecord{ 4, static_cast<uint32_t>(outY), g_lastTimeStamp, g_lastSequence }, evMouse, evKeys);
        }
        g_accX = 0;
        g_accY = 0;
        g_pending = false;
        g_lastEmitQpc = Diag::Now();
    }

    // Replaces the mouse do/while loop. Runs on the input thread with csInput (0x006C1230) held.
    void __cdecl MouseBranchImpl(uint8_t* evMouse, void* evKeys) {
        auto* mgr = *reinterpret_cast<uint8_t**>(kAddrInputMgrPtr);
        void* dev = mgr ? *reinterpret_cast<void**>(mgr + 0x0C) : nullptr;
        if (dev) {
            // IDirectInputDevice vtable: +0x1C Acquire, +0x28 GetDeviceData
            using PFN_GetDeviceData = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, DiRecord*, DWORD*, DWORD);
            using PFN_Acquire       = HRESULT(STDMETHODCALLTYPE*)(void*);
            void** vtbl = *reinterpret_cast<void***>(dev);
            const auto getData = reinterpret_cast<PFN_GetDeviceData>(vtbl[0x28 / 4]);
            const auto acquire = reinterpret_cast<PFN_Acquire>(vtbl[0x1C / 4]);

            bool reacquired = false;
            for (int guard = 0; guard < 64; ++guard) {
                DiRecord recs[32];
                DWORD count = 32;
                const HRESULT hr = getData(dev, sizeof(DiRecord), recs, &count, 0);
                if (FAILED(hr)) {
                    // DIERR_INPUTLOST / DIERR_NOTACQUIRED: same recovery as the original (0x005FBE01)
                    if (!reacquired && (hr == static_cast<HRESULT>(0x8007001E) || hr == static_cast<HRESULT>(0x8007000C)) &&
                        SUCCEEDED(acquire(dev))) {
                        reacquired = true;
                        continue;
                    }
                    break;
                }
                for (DWORD i = 0; i < count; ++i) {
                    const DiRecord& r = recs[i];
                    Diag::Add(Diag::C_IN_RAW);
                    g_lastTimeStamp = r.timeStamp;
                    g_lastSequence  = r.sequence;
                    if (r.ofs == 0) {
                        g_accX += static_cast<int32_t>(r.data);
                        g_pending = true;
                    } else if (r.ofs == 4) {
                        g_accY += static_cast<int32_t>(r.data);
                        g_pending = true;
                    } else {
                        // wheel / button: movement first so the click happens at the right spot
                        if (g_pending) {
                            FlushMovement(evMouse, evKeys);
                        }
                        Emit(r, evMouse, evKeys);
                    }
                }
                if (count < 32) {
                    break;
                }
            }
        }

        if (g_pending && (Diag::Now() - g_lastEmitQpc) >= g_intervalTicks) {
            FlushMovement(evMouse, evKeys);
        }

        // Background test mode: records from the command file enter the game here, on the input
        // thread with csInput held - the same place and state as records read from DirectInput.
        if (TestHarness::Enabled()) {
            static uint32_t s_sequence = 1;
            TestHarness::InputRecord in{};
            for (int guard = 0; guard < 64 && TestHarness::NextInput(in); ++guard) {
                DiRecord rec{ in.ofs, in.data, GetTickCount(), s_sequence++ };
                if (in.type == 1) {
                    g_fnProcessInputEvent(1, &rec); // keyboard: what the branch at 0x005F0BA0 does
                } else if (in.ofs > 4 || in.data != 0) {
                    Emit(rec, evMouse, evKeys);
                }
            }
        }
    }

    // SBInputThreadProc frame at 0x005F0B22: [esp+0x10] = mouse event struct, [esp+0x50] = key state array
    __declspec(naked) void MouseBranchStub() {
        __asm {
            lea eax, [esp + 0x50]
            push eax
            lea eax, [esp + 0x14]
            push eax
            call MouseBranchImpl
            add esp, 8
            mov eax, kAddrMouseLoopExit
            jmp eax
        }
    }

    // Replaces `call [WaitForMultipleObjects]` at 0x005F0B0F. While movement is being held back
    // the wait gets a timeout so the remainder is delivered even if the mouse stops.
    DWORD WINAPI InputWait(DWORD count, const HANDLE* handles, BOOL waitAll, DWORD timeoutMs) {
        if (TestHarness::Enabled()) {
            // no device events arrive without the foreground: wake up regularly for injected records
            const DWORD r = WaitForMultipleObjects(count, handles, waitAll, 4);
            return (r == WAIT_TIMEOUT) ? WAIT_OBJECT_0 : r;
        }
        if (g_pending) {
            const uint64_t elapsed = Diag::Now() - g_lastEmitQpc;
            if (elapsed >= g_intervalTicks) {
                return WAIT_OBJECT_0; // run the mouse branch now
            }
            const uint64_t remTicks = g_intervalTicks - elapsed;
            timeoutMs = static_cast<DWORD>((remTicks * 1000ULL + static_cast<uint64_t>(g_freq.QuadPart) - 1) /
                                           static_cast<uint64_t>(g_freq.QuadPart));
            if (timeoutMs == 0) timeoutMs = 1;
        }
        const DWORD r = WaitForMultipleObjects(count, handles, waitAll, timeoutMs);
        if (r == WAIT_TIMEOUT) {
            return WAIT_OBJECT_0;
        }
        return r;
    }
}

// ---------------------------------------------------------------------- DirectInput re-acquire
namespace {
    constexpr uintptr_t kAddrInputAcquire = 0x005F9EA0; // SBInput::Acquire(bool), thiscall, ret 4, al = success
    constexpr uintptr_t kAddrAppActive    = 0x006A6D27; // 1 while the game considers itself active
    constexpr int kOffAcquired = 0x2A9;                 // SBInput: devices acquired
    constexpr int kOffState    = 0x2AA;                 // SBInput: 2 = last DirectInput call failed

    using PFN_InputAcquire = uint8_t(__fastcall*)(uint8_t* self, void* edx, uint32_t acquire);
    PFN_InputAcquire o_InputAcquire = nullptr;

    // main thread only
    uint8_t* g_inputObj = nullptr;
    bool     g_retryPending = false;
    int      g_retries = 0;
    uint64_t g_firstFailQpc = 0;
    uint64_t g_lastRetryQpc = 0;

    bool GameWindowIsForeground() {
        const HWND hwnd = WindowManager::GetGameWindow();
        return hwnd && GetForegroundWindow() == hwnd;
    }

    uint8_t __fastcall Hook_InputAcquire(uint8_t* self, void* edx, uint32_t acquire) {
        const uint8_t ok = o_InputAcquire(self, edx, acquire);
        // The game calls this from its window procedure and message pump (main thread). Keep
        // any other caller out of the bookkeeping and out of the log.
        if (!Diag::IsMainThread()) {
            return ok;
        }
        g_inputObj = self;
        if ((acquire & 0xFF) == 1) {
            if (!ok) {
                if (!g_retryPending) {
                    g_retryPending = true;
                    g_retries = 0;
                    g_firstFailQpc = Diag::Now();
                    g_lastRetryQpc = g_firstFailQpc;
                    Logger::Warn("InputFix: DirectInput acquire failed on activation (game window in foreground=%d) - retrying.",
                                 GameWindowIsForeground() ? 1 : 0);
                }
            } else if (g_retryPending) {
                g_retryPending = false;
                Logger::Info("InputFix: DirectInput acquired again by the game after %.0f ms.",
                             Diag::TicksToMs(Diag::Now() - g_firstFailQpc));
            }
        } else {
            g_retryPending = false; // released on deactivation: the next activation acquires again
        }
        return ok;
    }
}

// ------------------------------------------------------------- the game's own Alt+Tab detector
namespace {
    // 0x005E17A0, cdecl(uint8_t* dinputKeyState): called every pass of the menu, video, slideshow
    // and briefing loops (not the in-game loop). When it sees Alt+Tab, Ctrl+Esc or Ctrl+Alt+Del
    // held down it clears the game's "active" flag and minimises the window itself, as an
    // exclusive-fullscreen game had to. A borderless window is not minimised (Hook_ShowWindow),
    // so the game stayed in front, believing it was inactive and blocking in GetMessage for a
    // WM_ACTIVATEAPP that never comes - most often right after switching BACK with Alt+Tab,
    // while Alt is still down when the keyboard is re-acquired. Windows does the task switch
    // by itself; the game only needs to forget the keys.
    constexpr uintptr_t kAddrTaskSwitchKeys = 0x005E17A0;

    using PFN_TaskSwitchKeys = uint32_t(__cdecl*)(uint8_t* keys);
    PFN_TaskSwitchKeys o_TaskSwitchKeys = nullptr;

    uint32_t __cdecl Hook_TaskSwitchKeys(uint8_t* keys) {
        if (keys && SherwoodConfig::Get().windowedBorderless) {
            // DirectInput scan codes: 0x0F Tab, 0x38/0xB8 Alt, 0x01 Esc, 0x1D/0x9D Ctrl, 0x53/0xD3 Del
            const bool alt     = keys[0x38] != 0 || keys[0xB8] != 0;
            const bool ctrl    = keys[0x1D] != 0 || keys[0x9D] != 0;
            const bool altTab  = keys[0x0F] != 0 && alt;
            const bool ctrlEsc = keys[0x01] != 0 && ctrl;
            const bool ctrlAltDel = ctrl && alt && (keys[0x53] != 0 || keys[0xD3] != 0);
            if (altTab || ctrlEsc || ctrlAltDel) {
                // what the original does to the key states, minus the self-deactivation
                keys[0x0F] = 0; keys[0x38] = 0; keys[0xB8] = 0; keys[0x1D] = 0;
                keys[0x9D] = 0; keys[0x01] = 0; keys[0x53] = 0; keys[0xD3] = 0;
                static uint64_t s_lastLog = 0;
                const uint64_t now = Diag::Now();
                if (s_lastLog == 0 || Diag::TicksToMs(now - s_lastLog) > 2000.0) {
                    s_lastLog = now;
                    Logger::Info("InputFix: task-switch keys seen in a menu/video loop (%s) - left to Windows, game stays active.",
                                 altTab ? "Alt+Tab" : (ctrlEsc ? "Ctrl+Esc" : "Ctrl+Alt+Del"));
                }
                return (altTab || ctrlEsc) ? 1u : 0u;
            }
        }
        return o_TaskSwitchKeys(keys);
    }
}

namespace InputFix {

void Poll() {
    if (!g_retryPending) {
        return;
    }
    const uint64_t now = Diag::Now();
    if (Diag::TicksToMs(now - g_lastRetryQpc) < 100.0) {
        return;
    }
    g_lastRetryQpc = now;
    if (!g_inputObj || !o_InputAcquire ||
        *reinterpret_cast<const uint8_t*>(kAddrAppActive) != 1 || !GameWindowIsForeground()) {
        return; // not active (yet): the game acquires by itself when it is activated
    }
    ++g_retries;
    // "acquired" is set as soon as the keyboard is in, even if the mouse then failed; release
    // first so that the complete acquire path runs again.
    if (g_inputObj[kOffAcquired] == 1) {
        o_InputAcquire(g_inputObj, nullptr, 0);
    }
    if (o_InputAcquire(g_inputObj, nullptr, 1)) {
        g_retryPending = false;
        Logger::Info("InputFix: DirectInput re-acquired after %d retr%s (%.0f ms without input).",
                     g_retries, g_retries == 1 ? "y" : "ies", Diag::TicksToMs(now - g_firstFailQpc));
    } else if (g_retries == 50) {
        Logger::Warn("InputFix: DirectInput still not acquired after %d retries (state=%u) - continuing to retry.",
                     g_retries, static_cast<unsigned>(g_inputObj[kOffState]));
    }
}

void Initialize() {
    const auto& cfg = SherwoodConfig::Get();

    // SBInput::Acquire prologue: mov eax,fs:[0] / push -1 / push 0x6734D8 / push eax
    static const uint8_t kAcquireProlog[] = { 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x6A, 0xFF, 0x68, 0xD8, 0x34, 0x67, 0x00, 0x50 };
    if (std::memcmp(reinterpret_cast<const void*>(kAddrInputAcquire), kAcquireProlog, sizeof(kAcquireProlog)) == 0 &&
        MH_CreateHook(reinterpret_cast<LPVOID>(kAddrInputAcquire), reinterpret_cast<LPVOID>(&Hook_InputAcquire),
                      reinterpret_cast<LPVOID*>(&o_InputAcquire)) == MH_OK &&
        MH_EnableHook(reinterpret_cast<LPVOID>(kAddrInputAcquire)) == MH_OK) {
        Logger::Info("InputFix: DirectInput acquire @ 0x%08X is retried when it fails on activation.",
                     static_cast<uint32_t>(kAddrInputAcquire));
    } else {
        o_InputAcquire = nullptr;
        Logger::Warn("InputFix: SBInput::Acquire not recognised - no re-acquire after Alt+Tab.");
    }

    // task-switch key detector: push ecx / mov eax,[esp+8] / push ebx / xor ebx,ebx / cmp [eax+0xF],bl
    static const uint8_t kTaskKeysProlog[] = { 0x51, 0x8B, 0x44, 0x24, 0x08, 0x53, 0x33, 0xDB, 0x38, 0x58, 0x0F };
    if (std::memcmp(reinterpret_cast<const void*>(kAddrTaskSwitchKeys), kTaskKeysProlog, sizeof(kTaskKeysProlog)) == 0 &&
        MH_CreateHook(reinterpret_cast<LPVOID>(kAddrTaskSwitchKeys), reinterpret_cast<LPVOID>(&Hook_TaskSwitchKeys),
                      reinterpret_cast<LPVOID*>(&o_TaskSwitchKeys)) == MH_OK &&
        MH_EnableHook(reinterpret_cast<LPVOID>(kAddrTaskSwitchKeys)) == MH_OK) {
        Logger::Info("InputFix: the game's Alt+Tab self-minimise @ 0x%08X is disabled (borderless window).",
                     static_cast<uint32_t>(kAddrTaskSwitchKeys));
    } else {
        Logger::Warn("InputFix: Alt+Tab detector not recognised - the game may freeze after Alt+Tab in menus.");
    }

    if (cfg.mouseReportRateHz <= 0) {
        Logger::Info("InputFix: mouse report coalescing disabled (MouseReportRateHz=0).");
        return;
    }

    QueryPerformanceFrequency(&g_freq);
    const int hz = (cfg.mouseReportRateHz < 30) ? 30 : (cfg.mouseReportRateHz > 1000 ? 1000 : cfg.mouseReportRateHz);
    g_intervalTicks = static_cast<uint64_t>(g_freq.QuadPart) / static_cast<uint64_t>(hz);
    g_lastEmitQpc = Diag::Now();

    static const uint8_t kWaitCall[]  = { 0xFF, 0x15, 0xF4, 0x71, 0x67, 0x00 };
    static const uint8_t kLoopHead[]  = { 0x8B, 0x0D, 0x20, 0x13, 0x6C, 0x00, 0x8D, 0x54, 0x24, 0x38, 0x52, 0xE8 };
    static const uint8_t kLoopExit[]  = { 0x68, 0xC8, 0x12, 0x6C, 0x00, 0xFF, 0xD7, 0x6A, 0x01, 0xE8 };
    if (std::memcmp(reinterpret_cast<const void*>(kAddrWaitCall), kWaitCall, sizeof(kWaitCall)) != 0 ||
        std::memcmp(reinterpret_cast<const void*>(kAddrMouseLoop), kLoopHead, sizeof(kLoopHead)) != 0 ||
        std::memcmp(reinterpret_cast<const void*>(kAddrMouseLoopExit), kLoopExit, sizeof(kLoopExit)) != 0) {
        Logger::Warn("InputFix: SBInputThreadProc bytes do not match this build - mouse coalescing NOT installed.");
        return;
    }

    // 0x005F0B22: jmp MouseBranchStub (the old loop body up to 0x005F0B81 becomes dead code)
    uint8_t jmpPatch[6];
    jmpPatch[0] = 0xE9;
    int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&MouseBranchStub) - (kAddrMouseLoop + 5));
    std::memcpy(&jmpPatch[1], &rel, 4);
    jmpPatch[5] = 0x90;

    // 0x005F0B0F: call InputWait ; nop   (stdcall, same 4 arguments as WaitForMultipleObjects)
    uint8_t callPatch[6];
    callPatch[0] = 0xE8;
    rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&InputWait) - (kAddrWaitCall + 5));
    std::memcpy(&callPatch[1], &rel, 4);
    callPatch[5] = 0x90;

    if (Memory::PatchBytes(kAddrMouseLoop, jmpPatch, sizeof(jmpPatch)) &&
        Memory::PatchBytes(kAddrWaitCall, callPatch, sizeof(callPatch))) {
        Logger::Info("InputFix: mouse reports coalesced to %d Hz (SBInputThreadProc @ 0x%08X / 0x%08X).",
                     hz, static_cast<uint32_t>(kAddrMouseLoop), static_cast<uint32_t>(kAddrWaitCall));
        if (cfg.mouseSensitivity > 0) {
            Logger::Info("InputFix: cursor speed %d %% (MouseSensitivity).", cfg.mouseSensitivity);
        } else {
            Logger::Info("InputFix: cursor speed follows internal width / window width (MouseSensitivity=0).");
        }
    } else {
        Logger::Error("InputFix: failed to patch SBInputThreadProc.");
    }
}

void Shutdown() {
}

} // namespace InputFix
