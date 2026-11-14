#include "watchdog.h"
#include "config.h"
#include "diag.h"
#include "logger.h"
#include "test_harness.h"
#include "window_manager.h"

#include <MinHook.h>
#include <tlhelp32.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace Watchdog {
namespace {
    constexpr uintptr_t kAddrAppActive = 0x006A6D27;   // Game.exe: 1 while the game considers itself active
    constexpr double kHangSeconds      = 3.0;          // silence in front of the user before a report
    // Time the game gets to exit after its window is gone. A normal exit takes a fraction of a
    // second; closing the window from the main menu (Alt+F4, taskbar) never ends by itself -
    // that loop waits in GetMessage for a window that no longer exists.
    constexpr double kExitGraceSeconds = 2.0;
    constexpr size_t kStackWords       = 4096;
    constexpr int    kMaxFrames        = 40;

    std::atomic<uint64_t> g_lastBeat{ 0 };
    std::atomic<uint64_t> g_windowGoneAt{ 0 };
    std::atomic<HWND>     g_window{ nullptr };
    std::atomic<bool>     g_stop{ false };
    std::atomic<bool>     g_exiting{ false };

    HANDLE   g_thread = nullptr;
    HANDLE   g_mainThread = nullptr;
    DWORD    g_mainTid = 0;
    uint32_t g_mainStackBase = 0;
    LARGE_INTEGER g_freq{};
    uint32_t g_stack[kStackWords]; // watchdog thread only

    using PFN_ExitProcess = VOID(WINAPI*)(UINT);
    PFN_ExitProcess o_ExitProcess = nullptr;

    using PFN_NtQueryInformationThread = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    PFN_NtQueryInformationThread g_fnQueryThread = nullptr;

    double Seconds(uint64_t ticks) {
        return static_cast<double>(ticks) / static_cast<double>(g_freq.QuadPart);
    }

    // Raw copy of another thread's stack; SEH and C++ objects must not share a frame.
    size_t CopyStackWords(uint32_t esp, uint32_t* out, size_t maxWords) {
        __try {
            std::memcpy(out, reinterpret_cast<const void*>(static_cast<uintptr_t>(esp)), maxWords * sizeof(uint32_t));
            return maxWords;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    // true if `v` points just behind a call instruction inside the code of a loaded module
    bool IsReturnAddress(uint32_t v, uint32_t* moduleBase) {
        if (v < 0x10000) return false;
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(v)), &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
        if (mbi.State != MEM_COMMIT || mbi.Type != MEM_IMAGE) return false;
        const DWORD prot = mbi.Protect & 0xFF;
        if (prot != PAGE_EXECUTE && prot != PAGE_EXECUTE_READ && prot != PAGE_EXECUTE_READWRITE && prot != PAGE_EXECUTE_WRITECOPY) {
            return false;
        }
        if (v - static_cast<uint32_t>(reinterpret_cast<uintptr_t>(mbi.BaseAddress)) < 8) return false;
        bool call = false;
        __try {
            const auto* p = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(v));
            call = p[-5] == 0xE8 ||                                                        // call rel32
                   (p[-2] == 0xFF && ((p[-1] >> 3) & 7) == 2) ||                           // call reg / [reg]
                   (p[-3] == 0xFF && ((p[-2] >> 3) & 7) == 2 && (p[-2] >> 6) != 3) ||      // call [reg+d8] / [sib]
                   (p[-4] == 0xFF && ((p[-3] >> 3) & 7) == 2 && (p[-3] & 7) == 4) ||       // call [sib+d8]
                   (p[-6] == 0xFF && ((p[-5] >> 3) & 7) == 2) ||                           // call [d32] / [reg+d32]
                   (p[-7] == 0xFF && ((p[-6] >> 3) & 7) == 2 && (p[-6] & 7) == 4);         // call [sib+d32]
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            call = false;
        }
        if (!call) return false;
        *moduleBase = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(mbi.AllocationBase));
        return true;
    }

    uint32_t ModuleBaseOf(uint32_t addr) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (addr < 0x10000 || VirtualQuery(reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(addr)), &mbi, sizeof(mbi)) != sizeof(mbi)) {
            return 0;
        }
        return (mbi.Type == MEM_IMAGE) ? static_cast<uint32_t>(reinterpret_cast<uintptr_t>(mbi.AllocationBase)) : 0;
    }

    // "Game.exe 005F1234" or "KERNELBASE.dll+0x1600C8". Takes the loader lock (GetModuleFileName),
    // so it is only used after the raw addresses have been written to the log.
    void Describe(uint32_t addr, uint32_t base, char* out, size_t outSize) {
        char path[MAX_PATH]{};
        if (base == 0 || GetModuleFileNameA(reinterpret_cast<HMODULE>(static_cast<uintptr_t>(base)), path, MAX_PATH) == 0) {
            std::snprintf(out, outSize, "%08X", addr);
            return;
        }
        const char* name = path;
        for (const char* p = path; *p; ++p) {
            if (*p == '\\' || *p == '/') name = p + 1;
        }
        if (base == 0x00400000) {
            std::snprintf(out, outSize, "%s %08X", name, addr);
        } else {
            std::snprintf(out, outSize, "%s+0x%X", name, addr - base);
        }
    }

    struct Frame {
        uint32_t offset;
        uint32_t addr;
        uint32_t base;
    };

    // Where is the main thread right now? Suspends it only for the copy of its stack.
    void ReportMainThread() {
        CONTEXT ctx{};
        size_t words = 0;
        bool ok = false;
        if (g_mainThread && SuspendThread(g_mainThread) != static_cast<DWORD>(-1)) {
            // nothing that allocates, locks or logs until ResumeThread
            ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            ok = GetThreadContext(g_mainThread, &ctx) != FALSE;
            if (ok) {
                size_t want = kStackWords;
                if (g_mainStackBase > ctx.Esp) {
                    want = std::min<size_t>(want, (g_mainStackBase - ctx.Esp) / sizeof(uint32_t));
                }
                words = CopyStackWords(ctx.Esp, g_stack, want);
            }
            ResumeThread(g_mainThread);
        }
        if (!ok) {
            Logger::Warn("Watchdog:   main thread context not available.");
            return;
        }

        Frame frames[kMaxFrames];
        int n = 0;
        for (size_t i = 0; i < words && n < kMaxFrames; ++i) {
            uint32_t base = 0;
            if (IsReturnAddress(g_stack[i], &base)) {
                frames[n++] = Frame{ static_cast<uint32_t>(i * sizeof(uint32_t)), g_stack[i], base };
            }
        }

        // raw first: needs no lock the main thread could be holding
        char line[1024];
        int pos = std::snprintf(line, sizeof(line), "Watchdog:   raw EIP=%08X ESP=%08X EBP=%08X stack:", ctx.Eip, ctx.Esp, ctx.Ebp);
        for (int i = 0; i < n && pos < static_cast<int>(sizeof(line)) - 12; ++i) {
            pos += std::snprintf(line + pos, sizeof(line) - pos, " %08X", frames[i].addr);
        }
        Logger::Warn("%s", line);

        char where[MAX_PATH + 32];
        Describe(ctx.Eip, ModuleBaseOf(ctx.Eip), where, sizeof(where));
        Logger::Warn("Watchdog:   at        %s", where);
        for (int i = 0; i < n; ++i) {
            Describe(frames[i].addr, frames[i].base, where, sizeof(where));
            Logger::Warn("Watchdog:   [esp+%04X] %s", frames[i].offset, where);
        }
    }

    // Threads that somebody left suspended (a thread waiting for one of those never wakes up).
    void ReportSuspendedThreads() {
        const DWORD pid = GetCurrentProcessId();
        const DWORD self = GetCurrentThreadId();
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE) return;
        THREADENTRY32 te{};
        te.dwSize = sizeof(te);
        int total = 0;
        int suspended = 0;
        if (Thread32First(snap, &te)) {
            do {
                if (te.th32OwnerProcessID != pid) continue;
                ++total;
                if (te.th32ThreadID == self) continue;
                HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
                if (!h) continue;
                const DWORD prev = SuspendThread(h);
                if (prev != static_cast<DWORD>(-1)) {
                    ResumeThread(h);
                }
                if (prev != static_cast<DWORD>(-1) && prev > 0) {
                    ++suspended;
                    void* start = nullptr;
                    if (g_fnQueryThread) {
                        g_fnQueryThread(h, 9 /* ThreadQuerySetWin32StartAddress */, &start, sizeof(start), nullptr);
                    }
                    const auto startAddr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(start));
                    char where[MAX_PATH + 32];
                    Describe(startAddr, ModuleBaseOf(startAddr), where, sizeof(where));
                    Logger::Warn("Watchdog:   thread %lu%s is SUSPENDED (count %lu), started at %s",
                                 te.th32ThreadID, te.th32ThreadID == g_mainTid ? " (main)" : "", prev, where);
                }
                CloseHandle(h);
            } while (Thread32Next(snap, &te));
        }
        CloseHandle(snap);
        Logger::Warn("Watchdog:   %d threads in the process, %d suspended.", total, suspended);
    }

    void Report(const char* what, double silentSeconds, bool withThreads) {
        const HWND hwnd = g_window.load(std::memory_order_relaxed);
        const bool exists = hwnd && IsWindow(hwnd);
        Logger::Warn("Watchdog: %s - main thread (TID %lu) silent for %.1f s; game active flag=%u, window: %s, foreground=%d, visible=%d, minimised=%d.",
                     what, g_mainTid, silentSeconds,
                     static_cast<unsigned>(*reinterpret_cast<const uint8_t*>(kAddrAppActive)),
                     exists ? "exists" : "gone",
                     (exists && GetForegroundWindow() == hwnd) ? 1 : 0,
                     (exists && IsWindowVisible(hwnd)) ? 1 : 0,
                     (exists && WindowManager::IsReallyMinimised()) ? 1 : 0);
        ReportMainThread();
        if (withThreads) {
            ReportSuspendedThreads();
        }
    }

    [[noreturn]] void EndProcess(UINT code) {
        ClipCursor(nullptr);
        Logger::Shutdown();
        TerminateProcess(GetCurrentProcess(), code);
        for (;;) {
            Sleep(1000);
        }
    }

    // The game leaves through the CRT's exit(), which has already run its own clean-up when it
    // gets here. What would follow - DllMain(PROCESS_DETACH) of every DLL after all other
    // threads were killed wherever they stood - is where a 2002 game and its drivers get stuck,
    // and nothing in it is needed: end the process now.
    VOID WINAPI Hook_ExitProcess(UINT code) {
        if (!g_exiting.exchange(true)) {
            g_stop.store(true, std::memory_order_relaxed);
            Diag::StopProfiler();
            Logger::Info("ExitProcess(%u) on thread %lu - ending the process directly.", code, GetCurrentThreadId());
            EndProcess(code);
        }
        o_ExitProcess(code);
    }

    DWORD WINAPI WatchdogThread(LPVOID) {
        bool hangReported = false;
        double nextReportAt = 0.0;
        int reports = 0;
        int inactiveInFront = 0;
        uint64_t lastResendAt = 0;
        int resendLogged = 0;
        while (!g_stop.load(std::memory_order_relaxed)) {
            Sleep(250);
            if (g_stop.load(std::memory_order_relaxed)) break;
            const uint64_t now = Diag::Now();

            const uint64_t goneAt = g_windowGoneAt.load(std::memory_order_relaxed);
            if (goneAt != 0) {
                if (Seconds(now - goneAt) >= kExitGraceSeconds && !g_exiting.exchange(true)) {
                    Report("the process is still running after the game window was destroyed", Seconds(now - goneAt), true);
                    Logger::Warn("Watchdog: ending the process so that no windowless Game.exe is left behind.");
                    EndProcess(0);
                }
                continue;
            }

            const HWND hwnd = g_window.load(std::memory_order_relaxed);
            if (!hwnd) continue;

            // The game keeps its own "active" flag and, while it is 0, blocks in GetMessage until
            // WM_ACTIVATEAPP(TRUE) arrives. The flag can be 0 although the window is in front
            // (start-up without focus: the flag is the result of the first DirectInput acquire;
            // or a deactivation Windows never matched with an activation). Windows will not send
            // another activation to a window that already is active, so do it for the game.
            // (Background test mode: the hidden window is never in front, the game runs anyway.)
            const bool testMode = TestHarness::Enabled();
            if ((testMode || GetForegroundWindow() == hwnd) && *reinterpret_cast<const uint8_t*>(kAddrAppActive) == 0) {
                if (testMode) {
                    PostMessageA(hwnd, WM_ACTIVATEAPP, TRUE, 0);
                } else if (++inactiveInFront >= 3 && (lastResendAt == 0 || Seconds(now - lastResendAt) >= 2.0)) {
                    lastResendAt = now;
                    inactiveInFront = 0;
                    PostMessageA(hwnd, WM_ACTIVATEAPP, TRUE, 0);
                    if (resendLogged < 20) {
                        ++resendLogged;
                        Logger::Warn("Watchdog: the game window is in front but the game considers itself inactive - activation re-sent.");
                    }
                }
            } else {
                inactiveInFront = 0;
            }

            const uint64_t last = g_lastBeat.load(std::memory_order_relaxed);
            if (last == 0 || now <= last) continue;
            const double silent = Seconds(now - last);

            // Only while the player is looking at the game: in the background the game blocks
            // in GetMessage on purpose.
            if (silent >= kHangSeconds && GetForegroundWindow() == hwnd) {
                if (!hangReported) {
                    hangReported = true;
                    reports = 1;
                    nextReportAt = 10.0;
                    Report("no frame and no message handled (hang, or a long load)", silent, true);
                } else if (silent >= nextReportAt && reports < 3) {
                    ++reports;
                    nextReportAt *= 3.0;
                    Report("still silent", silent, false);
                }
            } else if (hangReported && silent < 1.0) {
                hangReported = false;
                Logger::Info("Watchdog: main thread is responsive again.");
            }
        }
        return 0;
    }
}

void Init() {
    QueryPerformanceFrequency(&g_freq);
    g_mainTid = GetCurrentThreadId();
    g_mainStackBase = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(reinterpret_cast<NT_TIB*>(NtCurrentTeb())->StackBase));
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_mainThread,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    if (HMODULE ntdll = GetModuleHandleA("ntdll.dll")) {
        g_fnQueryThread = reinterpret_cast<PFN_NtQueryInformationThread>(GetProcAddress(ntdll, "NtQueryInformationThread"));
    }

    if (HMODULE hKernel32 = GetModuleHandleA("kernel32.dll")) {
        auto pExit = reinterpret_cast<LPVOID>(GetProcAddress(hKernel32, "ExitProcess"));
        if (pExit && MH_CreateHook(pExit, reinterpret_cast<LPVOID>(&Hook_ExitProcess),
                                   reinterpret_cast<LPVOID*>(&o_ExitProcess)) == MH_OK &&
            MH_EnableHook(pExit) == MH_OK) {
            Logger::Info("Watchdog: ExitProcess hooked (direct process end, no detach phase).");
        }
    }

    if (!SherwoodConfig::Get().hangWatchdog) {
        Logger::Info("Watchdog: hang reports and exit guard disabled (HangWatchdog=0).");
        return;
    }
    g_thread = CreateThread(nullptr, 0, WatchdogThread, nullptr, 0, nullptr);
    Logger::Info("Watchdog: hang reports after %.0f s of silence, exit guard %.0f s after the window is gone.",
                 kHangSeconds, kExitGraceSeconds);
}

void Shutdown() {
    g_stop.store(true, std::memory_order_relaxed);
}

namespace {
    struct WindowSearch {
        DWORD pid;
        bool  found;
    };

    BOOL CALLBACK FindVisibleWindowProc(HWND hwnd, LPARAM lp) {
        auto* s = reinterpret_cast<WindowSearch*>(lp);
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == s->pid && IsWindowVisible(hwnd)) {
            s->found = true;
            return FALSE;
        }
        return TRUE;
    }

    bool HasVisibleWindow(DWORD pid) {
        WindowSearch s{ pid, false };
        EnumWindows(FindVisibleWindowProc, reinterpret_cast<LPARAM>(&s));
        return s.found;
    }
}

// The game opens its sprite bank (Data\RobinHood.bks) exclusively. A previous Game.exe that
// lost its window but never exited keeps that file locked; the next start then gets garbage
// sprite data and crashes a few seconds into the mission (access violation in the sprite
// decoder). Runs at the EXE entry point, before the game opens anything.
bool EndLeftoverInstances() {
    char self[MAX_PATH]{};
    GetModuleFileNameA(nullptr, self, MAX_PATH);
    const DWORD myPid = GetCurrentProcessId();
    bool leftoverRemains = false;
    bool otherRunning = false;

    // A background test instance (test_harness.cpp) announces itself with a named event. It has
    // no window by design and must never stand in the way of a real start, however young it is.
    bool testInstanceAlive = false;
    if (!TestHarness::Enabled()) {
        if (HANDLE marker = OpenEventA(SYNCHRONIZE, FALSE, "Local\\SherwoodFixTestInstance")) {
            testInstanceAlive = true;
            CloseHandle(marker);
        }
    }

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return true;
    PROCESSENTRY32 pe{};
    pe.dwSize = sizeof(pe);
    if (Process32First(snap, &pe)) {
        do {
            if (pe.th32ProcessID == myPid || _stricmp(pe.szExeFile, "Game.exe") != 0) continue;
            const DWORD pid = pe.th32ProcessID;
            const bool hasWindow = HasVisibleWindow(pid);
            HANDLE h = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
            if (!h) {
                // typically: the other one runs elevated and this one does not
                if (hasWindow) {
                    otherRunning = true;
                } else {
                    leftoverRemains = true;
                    Logger::Warn("Leftover Game.exe (PID %lu, no window) cannot be ended from here (error %lu) - it keeps the game's data files locked.",
                                 pid, GetLastError());
                }
                continue;
            }
            char path[MAX_PATH]{};
            DWORD len = MAX_PATH;
            if (QueryFullProcessImageNameA(h, 0, path, &len) && _stricmp(path, self) != 0) {
                CloseHandle(h); // a Game.exe of some other game
                continue;
            }
            FILETIME created{}, exited{}, kernel{}, user{};
            double ageSeconds = 1e9;
            if (GetProcessTimes(h, &created, &exited, &kernel, &user)) {
                FILETIME nowFt{};
                GetSystemTimeAsFileTime(&nowFt);
                const uint64_t c = (static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
                const uint64_t n = (static_cast<uint64_t>(nowFt.dwHighDateTime) << 32) | nowFt.dwLowDateTime;
                ageSeconds = (n > c) ? static_cast<double>(n - c) / 1e7 : 0.0;
            }
            if (hasWindow || (ageSeconds < 15.0 && !testInstanceAlive)) {
                otherRunning = true; // a real second copy (or one that is just starting)
            } else if (TerminateProcess(h, 0)) {
                WaitForSingleObject(h, 3000);
                Logger::Warn("Ended a leftover Game.exe (PID %lu, no window, started %.0f s ago) that kept the game's data files locked.",
                             pid, ageSeconds);
            } else {
                leftoverRemains = true;
                Logger::Warn("Leftover Game.exe (PID %lu) could not be ended (error %lu).", pid, GetLastError());
            }
            CloseHandle(h);
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);

    if (!leftoverRemains && !otherRunning) {
        return true;
    }
    char ignore[8]{};
    if (GetEnvironmentVariableA("SHERWOODFIX_IGNORE_LEFTOVER", ignore, sizeof(ignore)) > 0) {
        Logger::Warn("Another Game.exe holds the data files; continuing anyway (SHERWOODFIX_IGNORE_LEFTOVER is set) - expect a crash when a mission starts.");
        return true;
    }
    Logger::Error("Another Game.exe is %s - this start would crash when a mission loads. Exiting.",
                  otherRunning ? "already running" : "left over in the background and could not be ended");
    if (TestHarness::Enabled()) {
        return false; // a background test never puts a message box on the user's screen
    }
    const wchar_t* text = otherRunning
        ? L"Gra jest już uruchomiona (inny proces Game.exe).\n\nDrugie uruchomienie nie może otworzyć plików gry i wysypałoby się przy starcie misji."
        : L"W tle wciąż działa poprzednie uruchomienie gry (Game.exe bez okna) i blokuje pliki gry.\n\n"
          L"Zakończ je: Menedżer zadań → Szczegóły → Game.exe → Zakończ zadanie "
          L"(albo uruchom ponownie komputer), a potem włącz grę jeszcze raz.";
    MessageBoxW(nullptr, text, L"Robin Hood – SherwoodEngineFix", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST);
    return false;
}

void Beat() {
    g_lastBeat.store(Diag::Now(), std::memory_order_relaxed);
}


void OnMainWindowCreated(HWND hwnd) {
    g_windowGoneAt.store(0, std::memory_order_relaxed);
    g_window.store(hwnd, std::memory_order_relaxed);
    Beat();
}

void OnMainWindowDestroyed() {
    g_window.store(nullptr, std::memory_order_relaxed);
    g_windowGoneAt.store(Diag::Now(), std::memory_order_relaxed);
    Logger::Info("Watchdog: game window destroyed - the process has %.0f s to exit on its own.", kExitGraceSeconds);
}

} // namespace Watchdog
