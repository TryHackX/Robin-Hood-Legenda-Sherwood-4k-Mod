#include <windows.h>
#include <string>
#include <algorithm>
#include <MinHook.h>

#include "proxy_winmm.h"
#include "config.h"
#include "logger.h"
#include "pattern.h"
#include "window_manager.h"
#include "render_ddraw.h"
#include "timing.h"
#include "widescreen_patch.h"
#include "cpu_optimizer.h"
#include "memory_opt.h"
#include "audio_fix.h"
#include "diag.h"
#include "input_fix.h"
#include "test_harness.h"
#include "settings_menu.h"
#include "ui_scale.h"
#include "watchdog.h"
#include "ddm/ddm.h"
#include "ddm/ddm_hd.h"

#include <atomic>
#include <cstdio>

namespace {
    bool g_isGameProcess = false;
    PVOID g_vehHandle = nullptr;
    std::atomic<bool> g_crashLogged{false};

    bool ResolveModuleOffset(uintptr_t addr, char* outBuf, size_t outSize) {
        HMODULE mod = nullptr;
        char modPath[MAX_PATH] = {};
        if (addr >= 0x10000 &&
            GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(addr), &mod) &&
            mod && GetModuleFileNameA(mod, modPath, MAX_PATH) > 0) {
            const char* slash = modPath;
            for (const char* p = modPath; *p; ++p) {
                if (*p == '\\' || *p == '/') {
                    slash = p + 1;
                }
            }
            const uintptr_t rva = addr - reinterpret_cast<uintptr_t>(mod);
            std::snprintf(outBuf, outSize, "%s+0x%08X", slash, static_cast<uint32_t>(rva));
            return true;
        }
        std::snprintf(outBuf, outSize, "<unknown>");
        return false;
    }

    LONG CALLBACK CrashVectoredHandler(PEXCEPTION_POINTERS ep) {
        if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        const DWORD code = ep->ExceptionRecord->ExceptionCode;
        const char* codeName = nullptr;
        switch (code) {
            case EXCEPTION_ACCESS_VIOLATION:     codeName = "ACCESS_VIOLATION"; break;
            case EXCEPTION_ILLEGAL_INSTRUCTION:  codeName = "ILLEGAL_INSTRUCTION"; break;
            case EXCEPTION_INT_DIVIDE_BY_ZERO:   codeName = "INT_DIVIDE_BY_ZERO"; break;
            case EXCEPTION_STACK_OVERFLOW:       codeName = "STACK_OVERFLOW"; break;
            case EXCEPTION_PRIV_INSTRUCTION:     codeName = "PRIV_INSTRUCTION"; break;
            default: return EXCEPTION_CONTINUE_SEARCH;
        }

        if (g_crashLogged.exchange(true, std::memory_order_acq_rel)) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        const uintptr_t eip = reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
        HMODULE mod = nullptr;
        char modPath[MAX_PATH] = {};
        const char* modName = "<unknown>";
        uintptr_t rva = 0;

        if (GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(eip), &mod) &&
            mod && GetModuleFileNameA(mod, modPath, MAX_PATH) > 0) {
            const char* slash = modPath;
            for (const char* p = modPath; *p; ++p) {
                if (*p == '\\' || *p == '/') {
                    slash = p + 1;
                }
            }
            modName = slash;
            rva = eip - reinterpret_cast<uintptr_t>(mod);
        }

        if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
            const ULONG_PTR op = ep->ExceptionRecord->ExceptionInformation[0];
            const ULONG_PTR addr = ep->ExceptionRecord->ExceptionInformation[1];
            const char* opStr = (op == 0) ? "READ" : (op == 1) ? "WRITE" : (op == 8) ? "DEP" : "OTHER";
            Logger::Error("FATAL %s (0x%08lX) %s 0x%08lX at %s+0x%08X (EIP=0x%08X, TID=%lu)",
                          codeName, code, opStr, static_cast<unsigned long>(addr),
                          modName, static_cast<uint32_t>(rva), static_cast<uint32_t>(eip),
                          GetCurrentThreadId());
        } else {
            Logger::Error("FATAL %s (0x%08lX) at %s+0x%08X (EIP=0x%08X, TID=%lu)",
                          codeName, code, modName, static_cast<uint32_t>(rva),
                          static_cast<uint32_t>(eip), GetCurrentThreadId());
        }

        const CONTEXT* ctx = ep->ContextRecord;
        Logger::Error("  Regs: EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX ESI=%08lX EDI=%08lX EBP=%08lX ESP=%08lX",
                      ctx->Eax, ctx->Ebx, ctx->Ecx, ctx->Edx,
                      ctx->Esi, ctx->Edi, ctx->Ebp, ctx->Esp);
        if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
            DDM::LogFaultContext(static_cast<uintptr_t>(ep->ExceptionRecord->ExceptionInformation[1]));
        }

        void* trace[12] = {};
        const USHORT traceCount = CaptureStackBackTrace(0, 12, trace, nullptr);
        for (USHORT i = 0; i < traceCount; ++i) {
            char loc[MAX_PATH + 32] = {};
            const uintptr_t addr = reinterpret_cast<uintptr_t>(trace[i]);
            ResolveModuleOffset(addr, loc, sizeof(loc));
            Logger::Error("  Backtrace[%u]: 0x%08X (%s)", i, static_cast<uint32_t>(addr), loc);
        }

        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(ctx->Esp)), &mbi, sizeof(mbi)) == sizeof(mbi) &&
            mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
            const uintptr_t sp = static_cast<uintptr_t>(ctx->Esp);
            const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
            const size_t maxDwords = (regionEnd > sp) ? ((regionEnd - sp) / sizeof(uint32_t)) : 0;
            const size_t count = (maxDwords < 20) ? maxDwords : 20;
            if (count > 0) {
                char stackBuf[260] = {};
                int pos = 0;
                const uint32_t* stack = reinterpret_cast<const uint32_t*>(sp);
                for (size_t i = 0; i < count && pos < static_cast<int>(sizeof(stackBuf) - 12); ++i) {
                    const int n = std::snprintf(stackBuf + pos, sizeof(stackBuf) - pos,
                                                (i == 0) ? "%08X" : " %08X", stack[i]);
                    if (n > 0) {
                        pos += n;
                    }
                }
                Logger::Error("  Stack[0..%zu]: %s", count - 1, stackBuf);
                for (size_t i = 0; i < count; ++i) {
                    char loc[MAX_PATH + 32] = {};
                    if (ResolveModuleOffset(stack[i], loc, sizeof(loc))) {
                        Logger::Error("  StackMod[%zu]: 0x%08X -> %s", i, stack[i], loc);
                    }
                }
            }
        }

        return EXCEPTION_CONTINUE_SEARCH;
    }

    using PFN_EntryPoint = void(__cdecl*)();
    PFN_EntryPoint g_origEntryPoint = nullptr;

    // Runs on the main thread after every statically linked DLL has been initialised.
    void __cdecl Hook_EntryPoint() {
        if (!Watchdog::EndLeftoverInstances()) {
            ExitProcess(0); // hooked: ends the process directly
        }
        AudioFix::Initialize();
        g_origEntryPoint();
    }

    std::string GetExecutableDir() {
        char path[MAX_PATH]{};
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        std::string s(path);
        size_t pos = s.find_last_of("\\/");
        return (pos != std::string::npos) ? s.substr(0, pos + 1) : "";
    }

    std::string GetExecutableName() {
        char path[MAX_PATH]{};
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        std::string s(path);
        size_t pos = s.find_last_of("\\/");
        std::string name = (pos != std::string::npos) ? s.substr(pos + 1) : s;
        std::transform(name.begin(), name.end(), name.begin(), ::tolower);
        return name;
    }

    void InitSherwoodEngineFix() {
        std::string exeDir = GetExecutableDir();
        auto& cfg = SherwoodConfig::Get();
        // Test runs (tools\gamectl.ps1 -Ini) use their own settings file, so that experiments never
        // touch the player's SherwoodFix.ini.
        std::string iniPath = exeDir + "SherwoodFix.ini";
        char iniOverride[MAX_PATH]{};
        if (GetEnvironmentVariableA("SHERWOODFIX_INI", iniOverride, MAX_PATH) > 0 &&
            GetFileAttributesA(iniOverride) != INVALID_FILE_ATTRIBUTES) {
            iniPath = iniOverride;
        }
        cfg.Load(iniPath);

        Logger::Init(exeDir + "SherwoodFix.log", cfg.enableLogging);
        Logger::Info("Attached to Game.exe (Base: 0x%08X)", reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr)));
        Logger::Info("Config loaded: Target Resolution=%dx%d, Borderless=%d, MaxFPS=%d",
                     cfg.width, cfg.height, cfg.windowedBorderless ? 1 : 0, cfg.maxFPS);
        if (iniOverride[0]) {
            Logger::Warn("Settings file overridden by SHERWOODFIX_INI: %s", iniPath.c_str());
        }

        // Phase 2.3: Enforce Per-Monitor V2 High-DPI awareness before any window creation (sharp 1440p/4K)
        WindowManager::EnforceHighDPIAwareness();

        MH_STATUS status = MH_Initialize();
        if (status != MH_OK) {
            Logger::Error("MinHook initialization failed: %s", MH_StatusToString(status));
            return;
        }
        Logger::Info("MinHook initialized successfully.");

        // Measurement only (per-tick cost breakdown, optional sampling profiler)
        Diag::Init();

        // Hang reports, and a process exit that cannot leave a windowless Game.exe behind
        Watchdog::Init();

        // Background test mode (only with SHERWOODFIX_TEST=1): hidden window, injected input
        TestHarness::Init();

        // Phase 2: Initialize Window Management & DirectDraw 7 -> GPU modernization
        WindowManager::InitHooks();
        if (cfg.renderer == 1) {
            // Own in-memory DirectDraw + Direct3D 11 presenter; the GOG wrapper is bypassed.
            if (!DDM::Install()) {
                Logger::Warn("In-memory DirectDraw could not be installed - falling back to the GOG wrapper.");
                RenderDDraw::Init();
            } else {
                DDM::HD::InstallHooks(); // water on HD backgrounds
            }
        } else {
            RenderDDraw::Init();
        }

        // Phase 3: Real-Time Loop, QPC Frame Pacing & Smooth Camera Scroll
        Timing::Init();

        // Phase 4: Widescreen/HD/1440p/4K Memory Patching, CPU Affinity, LAA/SSE2 Memory & Audio Fixes
        WidescreenPatch::Initialize();
        UiScale::Install();
        SettingsMenu::Install();
        CpuOptimizer::Initialize();
        MemoryOpt::Initialize();
        InputFix::Initialize();
        g_vehHandle = AddVectoredExceptionHandler(1, CrashVectoredHandler);
        if (g_vehHandle) {
            Logger::Info("CrashVectoredHandler registered.");
        }

        // Anything that touches other DLLs' code (Fmod.dll is UPX-packed and unpacks in its own
        // DllMain, which runs after ours) has to wait until the loader is done: run it from the
        // Game.exe entry point.
        auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleA(nullptr));
        auto* dos  = reinterpret_cast<PIMAGE_DOS_HEADER>(base);
        auto* nt   = reinterpret_cast<PIMAGE_NT_HEADERS>(base + dos->e_lfanew);
        void* entry = base + nt->OptionalHeader.AddressOfEntryPoint;
        if (MH_CreateHook(entry, reinterpret_cast<LPVOID>(&Hook_EntryPoint),
                          reinterpret_cast<LPVOID*>(&g_origEntryPoint)) == MH_OK &&
            MH_EnableHook(entry) == MH_OK) {
            Logger::Info("LateInit scheduled at Game.exe entry point 0x%08X.", reinterpret_cast<uintptr_t>(entry));
        } else {
            Logger::Warn("Could not hook the Game.exe entry point - FMOD hooks disabled.");
        }
    }

    void ShutdownSherwoodEngineFix(bool processTerminating) {
        if (g_isGameProcess && processTerminating) {
            // Every other thread has already been killed wherever it stood, possibly inside a
            // lock. Removing hooks (MinHook walks and suspends threads) or tearing modules down
            // here serves no purpose and can only hang - the memory goes away with the process.
            Watchdog::Shutdown();
            ClipCursor(nullptr);
            Logger::Info("SherwoodEngineFix: process exit.");
            Logger::Shutdown();
            return;
        }
        if (g_isGameProcess) {
            if (g_vehHandle) {
                RemoveVectoredExceptionHandler(g_vehHandle);
                g_vehHandle = nullptr;
            }
            Diag::Shutdown();
            InputFix::Shutdown();
            AudioFix::Shutdown();
            MemoryOpt::Shutdown();
            CpuOptimizer::Shutdown();
            WidescreenPatch::Shutdown();
            Timing::Shutdown();
            DDM::Shutdown();
            RenderDDraw::Shutdown();
            WindowManager::ShutdownHooks();
            MH_DisableHook(MH_ALL_HOOKS);
            MH_Uninitialize();
            Logger::Info("SherwoodEngineFix detached cleanly.");
            Logger::Shutdown();
        }
        ShutdownWinMMProxy();
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);

        if (!InitWinMMProxy()) {
            return FALSE;
        }

        std::string exeName = GetExecutableName();
        if (exeName == "game.exe") {
            g_isGameProcess = true;
            InitSherwoodEngineFix();
        }
    } else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        // lpReserved != NULL: the process is terminating (as opposed to FreeLibrary)
        ShutdownSherwoodEngineFix(lpReserved != nullptr);
    }
    return TRUE;
}
