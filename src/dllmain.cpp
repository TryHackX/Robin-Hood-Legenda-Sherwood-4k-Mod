#include <windows.h>
#include <string>
#include <algorithm>
#include <cstring>
#include <iterator>
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
#include "coop_probe.h"
#include "settings_menu.h"
#include "ui_scale.h"
#include "view_zoom.h"
#include "watchdog.h"
#include "ddm/ddm.h"
#include "ddm/ddm_hd.h"

#include <atomic>
#include <cstdio>

namespace {
    bool g_isGameProcess = false;
    PVOID g_vehHandle = nullptr;
    std::atomic<int> g_crashLogs{0};     // exceptions logged so far (the first few are)

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

        const uintptr_t eip = reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
        // This handler sees every exception first, also the ones caught afterwards: the mod's own
        // guarded reads (__try around game memory that may be gone) are expected and never logged,
        // and the first few others are (one of them used to be the only report - a later real
        // crash went unlogged, code review 2026-11-17).
        {
            HMODULE self = nullptr;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(&CrashVectoredHandler), &self) &&
                self && code == EXCEPTION_ACCESS_VIOLATION) {
                HMODULE at = nullptr;
                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       reinterpret_cast<LPCSTR>(eip), &at) && at == self) {
                    return EXCEPTION_CONTINUE_SEARCH;
                }
            }
        }
        if (g_crashLogs.fetch_add(1, std::memory_order_acq_rel) >= 3) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

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
            Logger::Error("EXCEPTION %s (0x%08lX) %s 0x%08lX at %s+0x%08X (EIP=0x%08X, TID=%lu)",
                          codeName, code, opStr, static_cast<unsigned long>(addr),
                          modName, static_cast<uint32_t>(rva), static_cast<uint32_t>(eip),
                          GetCurrentThreadId());
        } else {
            Logger::Error("EXCEPTION %s (0x%08lX) at %s+0x%08X (EIP=0x%08X, TID=%lu)",
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

    // ---- which Game.exe (2026-11-16: a player followed the instructions and "nothing worked") ----------
    // The patches are made for the code of the GOG edition's Game.exe (file of 2 899 968 bytes, PE time stamp
    // 3E257C94, image at 0x00400000): fixed addresses, checked prologues. Another edition, or a file changed by
    // another tool, would get some of them and not others. So first: are the routines the mod changes the
    // ones it knows? If not, the mod stays off (the game runs as without it) and says so once at the start.
    struct Signature { uintptr_t addr; uint8_t bytes[10]; };
    const Signature kSignatures[] = {
        { 0x0060F250, { 0x53, 0x56, 0x57, 0x8B, 0x3D, 0xC8, 0x72, 0x67, 0x00, 0xFF } },   // CPU clock measurement
        { 0x005ECBD0, { 0x6A, 0xFF, 0x68, 0x78, 0x29, 0x67, 0x00, 0x64, 0xA1, 0x00 } },   // DrawLine
        { 0x0053B330, { 0x6A, 0xFF, 0x68, 0x38, 0x6C, 0x66, 0x00, 0x64, 0xA1, 0x00 } },   // dialog backdrop colouring
        { 0x004D0400, { 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x6A, 0xFF, 0x68, 0x20 } },   // RHEngine::DrawSelection
        { 0x005BBA00, { 0x6A, 0xFF, 0x68, 0x7B, 0xF5, 0x66, 0x00, 0x64, 0xA1, 0x00 } },   // sprite: decode frame
        { 0x004C1AB0, { 0x53, 0x56, 0x8B, 0x74, 0x24, 0x0C, 0x57, 0x6A, 0x04, 0x68 } },   // level header
        { 0x004DEE70, { 0xD9, 0x05, 0x9C, 0x74, 0x67, 0x00, 0xD8, 0xB1, 0x5C, 0x47 } },   // ScreenToWorld
        { 0x0060F8F0, { 0x83, 0xEC, 0x20, 0x8D, 0x44, 0x24, 0x00, 0x50, 0xFF, 0x15 } },   // memory size
        { 0x005F1CA0, { 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x6A, 0xFF, 0x68, 0x20 } },   // cursor drawing
        { 0x0053C390, { 0x83, 0xEC, 0x10, 0x53, 0x55, 0x56, 0x8B, 0xF1, 0x57, 0x8B } },   // dialog loop
        { 0x00622EB4, { 0x55, 0x89, 0xE5, 0x60, 0x8B, 0x55, 0x18, 0x8B, 0x4D, 0x14 } },   // text drawing
    };
    bool g_modOff = false;           // this Game.exe is not the one the mod is made for

    bool GameExeSupported(char* why, size_t whySize) {
        const auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleA(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        const uint32_t stamp = nt->FileHeader.TimeDateStamp, image = nt->OptionalHeader.SizeOfImage;
        char path[MAX_PATH]{};
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        const unsigned long long fileSize = GetFileAttributesExA(path, GetFileExInfoStandard, &fa)
            ? (static_cast<unsigned long long>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow : 0ull;
        if (reinterpret_cast<uintptr_t>(base) != 0x00400000 || image < 0x002C7000) {
            std::snprintf(why, whySize, "file %llu bytes, time stamp %08X, image %08X at %p - another program",
                          fileSize, stamp, image, static_cast<const void*>(base));
            return false;
        }
        int bad = 0;
        uintptr_t firstBad = 0;
        for (const Signature& s : kSignatures) {
            if (std::memcmp(reinterpret_cast<const void*>(s.addr), s.bytes, sizeof(s.bytes)) != 0) {
                if (!bad) firstBad = s.addr;
                ++bad;
            }
        }
        std::snprintf(why, whySize, "file %llu bytes, time stamp %08X, %d of %zu known routines differ%s%08X",
                      fileSize, stamp, bad, std::size(kSignatures), bad ? ", first at 0x" : "", static_cast<unsigned>(firstBad));
        if (!bad) std::snprintf(why, whySize, "file %llu bytes, time stamp %08X", fileSize, stamp);
        return bad == 0;
    }

    // Entry point of a Game.exe the mod is not made for: tell the player once, then the game as it is.
    void __cdecl Hook_EntryPointUnsupported() {
        MessageBoxW(nullptr,
            L"SherwoodEngineFix: ten plik Game.exe nie jest wersją GOG gry albo został zmieniony.\n"
            L"Mod działa tylko z wersją GOG - zostaje wyłączony, gra uruchomi się bez niego.\n"
            L"Szczegóły: SherwoodFix.log w folderze gry. Usunięcie moda: skasuj winmm.dll.\n\n"
            L"This Game.exe is not the GOG edition of the game, or it was modified.\n"
            L"The mod only works with the GOG edition - it stays off and the game starts without it.\n"
            L"Details: SherwoodFix.log in the game folder. To remove the mod, delete winmm.dll.",
            L"SherwoodEngineFix", MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
        g_origEntryPoint();
    }

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
        {
            char why[200]{};
            if (!GameExeSupported(why, sizeof(why))) {
                g_modOff = true;
                Logger::Error("Game.exe is not the GOG edition this mod is made for (%s). The mod stays OFF - the game "
                              "runs as without it (GOG: 2899968 bytes, time stamp 3E257C94).", why);
                if (MH_Initialize() == MH_OK) {
                    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleA(nullptr));
                    auto* nt = reinterpret_cast<PIMAGE_NT_HEADERS>(base + reinterpret_cast<PIMAGE_DOS_HEADER>(base)->e_lfanew);
                    void* entry = base + nt->OptionalHeader.AddressOfEntryPoint;
                    if (MH_CreateHook(entry, reinterpret_cast<LPVOID>(&Hook_EntryPointUnsupported),
                                      reinterpret_cast<LPVOID*>(&g_origEntryPoint)) == MH_OK) {
                        MH_EnableHook(entry);
                    }
                }
                return;
            }
            Logger::Info("Game.exe: the GOG edition (%s).", why);
        }
        Logger::Info("Config loaded: Target Resolution=%dx%d, Borderless=%d, MaxFPS=%d",
                     cfg.width, cfg.height, cfg.windowedBorderless ? 1 : 0, cfg.maxFPS);
        {
            // The GOG installer marks Game.exe "run as administrator" (compatibility flags RUNASADMIN): a
            // recorder or overlay without those rights can neither capture the game nor see its hot keys while
            // the game is in front (Windows keeps lower programs out of an elevated one) - OBS, 2026-11-18.
            HANDLE token = nullptr;
            TOKEN_ELEVATION elevation{};
            DWORD got = 0;
            if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
                if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &got)) {
                    Logger::Info(elevation.TokenIsElevated
                                     ? "Process: runs as administrator (GOG sets this for Game.exe) - OBS and other recorders "
                                       "must run as administrator too to capture the game and react to their hot keys."
                                     : "Process: runs without administrator rights.");
                }
                CloseHandle(token);
            }
        }
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
        ViewZoom::Install();        // after UiScale: it checks the original bytes of CenterCameraOn
        SettingsMenu::Install();
        CpuOptimizer::Initialize();
        MemoryOpt::Initialize();
        InputFix::Initialize();
        CoopProbe::Install();       // co-op research probe: background test mode only (SHERWOODFIX_TEST=1)
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
        if (g_isGameProcess && g_modOff) {          // nothing of the mod was started
            Logger::Info("SherwoodEngineFix: process exit (mod was off).");
            Logger::Shutdown();
            if (!processTerminating) ShutdownWinMMProxy();
            return;
        }
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
