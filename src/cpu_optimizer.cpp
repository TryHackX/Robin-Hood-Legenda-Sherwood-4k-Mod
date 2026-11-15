#include "cpu_optimizer.h"
#include "config.h"
#include "logger.h"
#include "pattern.h"
#include <MinHook.h>
#include <windows.h>
#include <intrin.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <cstdint>

// ---- what the game's options menu says about the computer (2026-11-14, user report) ----------
// 0x00538F90 composes L"%s : %S, %i %s\n%s : %i %s": the processor as the CPUID vendor
// ("GenuineIntel": the CPU info object of 0x0060F2B0 copies it from 0x006C1894), the clock the game
// measured, and the memory from GlobalMemoryStatus (0x0060F8F0, plain ret: dwTotalPhys >> 20), which
// stops at 4 GB ("4095 Mb"; the menu keeps the low 16 bits of the value, the game's log 0x0045C82A
// prints it too). Shown instead: the processor's name (CPUID brand string, shortened) and the real
// memory (GlobalMemoryStatusEx, at most 65535 MB so that the 16 bits hold it).
namespace {
    char g_cpuName[64] = "";

    void ReadCpuName() {
        int r[4];
        __cpuid(r, static_cast<int>(0x80000000u));
        if (static_cast<unsigned>(r[0]) < 0x80000004u) return;
        char brand[49] = {};
        for (int i = 0; i < 3; ++i) {
            __cpuid(r, static_cast<int>(0x80000002u + i));
            std::memcpy(brand + 16 * i, r, 16);
        }
        std::string s(brand);
        const size_t at = s.find(" @");        // "@ 3.60GHz": the menu shows the clock itself
        if (at != std::string::npos) s.erase(at);
        for (const char* word : { "(R)", "(r)", "(TM)", "(tm)", " CPU", " Processor" }) {
            for (size_t p; (p = s.find(word)) != std::string::npos;) s.erase(p, std::strlen(word));
        }
        std::string out;
        for (char c : s) {
            if (c == ' ' && (out.empty() || out.back() == ' ')) continue;
            out += c;
        }
        while (!out.empty() && out.back() == ' ') out.pop_back();
        if (out.size() >= 4) std::snprintf(g_cpuName, sizeof(g_cpuName), "%s", out.c_str());
    }

    using PFN_TotalPhys = uint32_t(__cdecl*)();
    PFN_TotalPhys o_TotalPhys = nullptr;

    uint32_t __cdecl Hook_TotalPhys() {
        MEMORYSTATUSEX ms{};
        ms.dwLength = sizeof(ms);
        if (GlobalMemoryStatusEx(&ms)) {
            const unsigned long long mb = ms.ullTotalPhys >> 20;
            return static_cast<uint32_t>(mb > 65535ull ? 65535ull : mb);
        }
        return o_TotalPhys();
    }

    // ---- the clock the options menu shows (2026-11-15, user report: "entering Options stutters for a moment")
    // 0x0060F250 (plain ret, returns MHz; called by the CPU info object's constructor 0x0060F2B0 - at game start
    // 0x0045C5D0 and EVERY time the options menu opens, 0x00538FB7) waits for a timeGetTime edge and then spins
    // 1000 ms between two rdtsc reads: the game stood still for a whole second (no picture, measured with a call
    // trace: 1069 ms without a frame; tools\stack_sample.ps1 caught the main thread in that loop). Measured here
    // once, over 25 ms of the performance counter (more exact than the game's 1 ms timeGetTime over 1 s), and kept.
    using PFN_CpuMhz = uint32_t(__cdecl*)();
    PFN_CpuMhz o_CpuMhz = nullptr;
    uint32_t g_cpuMhz = 0;           // main thread only (the game's own calls)

    uint32_t MeasureMhz() {
        LARGE_INTEGER f{}, q0{}, q1{};
        if (!QueryPerformanceFrequency(&f) || f.QuadPart <= 0) return 0;
        QueryPerformanceCounter(&q0);
        const unsigned long long t0 = __rdtsc();
        do {
            QueryPerformanceCounter(&q1);
        } while (q1.QuadPart - q0.QuadPart < f.QuadPart / 40);
        const unsigned long long t1 = __rdtsc();
        const double seconds = static_cast<double>(q1.QuadPart - q0.QuadPart) / static_cast<double>(f.QuadPart);
        const double mhz = static_cast<double>(t1 - t0) / seconds / 1e6;
        return (mhz >= 1.0 && mhz < 65535.0) ? static_cast<uint32_t>(mhz) : 0;
    }

    uint32_t __cdecl Hook_CpuMhz() {
        if (!g_cpuMhz) {
            g_cpuMhz = MeasureMhz();
            if (!g_cpuMhz) return o_CpuMhz();
            Logger::Info("SystemInfo: processor clock %u MHz (measured in 25 ms instead of the game's 1 s wait).", g_cpuMhz);
        }
        return g_cpuMhz;
    }

    void PatchSystemInfo() {
        // 0x0060F250: push ebx / push esi / push edi / mov edi,[timeGetTime] / call edi / mov ebx,eax
        static const uint8_t kCpuMhz[] = { 0x53, 0x56, 0x57, 0x8B, 0x3D, 0xC8, 0x72, 0x67, 0x00, 0xFF, 0xD7, 0x8B, 0xD8 };
        const bool clock = std::memcmp(reinterpret_cast<const void*>(0x0060F250), kCpuMhz, sizeof(kCpuMhz)) == 0 &&
                           MH_CreateHook(reinterpret_cast<LPVOID>(0x0060F250), reinterpret_cast<LPVOID>(&Hook_CpuMhz),
                                         reinterpret_cast<LPVOID*>(&o_CpuMhz)) == MH_OK &&
                           MH_EnableHook(reinterpret_cast<LPVOID>(0x0060F250)) == MH_OK;
        if (!clock) Logger::Warn("SystemInfo: the game's 1 s clock measurement (0x0060F250) not replaced - options open slowly.");
        // 0x0060F8F0: sub esp,20h / lea eax,[esp] / push eax / call [GlobalMemoryStatus]
        static const uint8_t kTotalPhys[] = { 0x83, 0xEC, 0x20, 0x8D, 0x44, 0x24, 0x00, 0x50, 0xFF, 0x15, 0x28, 0x72, 0x67, 0x00 };
        const bool mem = std::memcmp(reinterpret_cast<const void*>(0x0060F8F0), kTotalPhys, sizeof(kTotalPhys)) == 0 &&
                         MH_CreateHook(reinterpret_cast<LPVOID>(0x0060F8F0), reinterpret_cast<LPVOID>(&Hook_TotalPhys),
                                       reinterpret_cast<LPVOID*>(&o_TotalPhys)) == MH_OK &&
                         MH_EnableHook(reinterpret_cast<LPVOID>(0x0060F8F0)) == MH_OK;
        // 0x00539066: mov ebp,"" / je +2 / mov ebp,eax (the vendor) -> mov ebp,g_cpuName / nop x4
        static const uint8_t kVendor[] = { 0xBD, 0xD0, 0x73, 0x67, 0x00, 0x74, 0x02, 0x8B, 0xE8 };
        ReadCpuName();
        bool cpu = false;
        if (g_cpuName[0] && std::memcmp(reinterpret_cast<const void*>(0x00539066), kVendor, sizeof(kVendor)) == 0) {
            uint8_t patch[9] = { 0xBD, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90 };
            const uint32_t name = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_cpuName));
            std::memcpy(patch + 1, &name, 4);
            cpu = Memory::PatchBytes(0x00539066, patch, sizeof(patch));
        }
        Logger::Info("SystemInfo: options menu shows the processor as \"%s\"%s and the memory %s.",
                     cpu ? g_cpuName : "(the game's CPUID vendor)", cpu ? "" : " - not patched",
                     mem ? "from GlobalMemoryStatusEx" : "as the game reads it (not patched)");
    }
}

namespace {
    DWORD_PTR g_mainCoreMask   = 0;
    DWORD_PTR g_inputCoreMask  = 0;
    DWORD_PTR g_workerCoreMask = 0;
    DWORD     g_mainThreadId   = 0;

    using CreateThread_t = HANDLE (WINAPI*)(
        LPSECURITY_ATTRIBUTES lpThreadAttributes,
        SIZE_T dwStackSize,
        LPTHREAD_START_ROUTINE lpStartAddress,
        LPVOID lpParameter,
        DWORD dwCreationFlags,
        LPDWORD lpThreadId);

    CreateThread_t g_OrigCreateThread = nullptr;

    // Fix ABBA Critical Section Deadlock in RHGame::RenderFrame (0x005106F0 & 0x00510732):
    // SBInputThreadProc (0x005F0B15) locks csInput (0x006C1230) FIRST and csCursorRender (0x006C12C8) SECOND.
    // RHGame::RenderFrame (0x005106F0) originally locked csCursorRender (0x006C12C8) FIRST and then called
    // 0x005F4900 -> 0x005F0FA0 which locks csInput (0x006C1230) SECOND, causing a hard freeze on mouse edge scroll!
    // Unifying both to lock csInput (0x006C1230) makes ABBA deadlocks impossible (CRITICAL_SECTION is recursive).
    void PatchCursorCriticalSectionDeadlock() {
        HMODULE hGame = GetModuleHandleA(nullptr);
        uintptr_t addrEnter = Memory::FindPattern(hGame, "68 C8 12 6C 00 FF 15 ? ? ? ? 8A 4C 24 30");
        if (!addrEnter) {
            addrEnter = 0x005106F0;
        }
        uintptr_t addrLeave = addrEnter + 0x42; // 0x00510732
        if (*reinterpret_cast<uint8_t*>(addrEnter) == 0x68 &&
            *reinterpret_cast<uint32_t*>(addrEnter + 1) == 0x006C12C8 &&
            *reinterpret_cast<uint8_t*>(addrLeave) == 0x68 &&
            *reinterpret_cast<uint32_t*>(addrLeave + 1) == 0x006C12C8) {
            const uint32_t csInputAddr = 0x006C1230;
            Memory::Write<uint32_t>(addrEnter + 1, csInputAddr);
            Memory::Write<uint32_t>(addrLeave + 1, csInputAddr);
            Logger::Info("CpuOptimizer: Fixed ABBA CriticalSection deadlock in RHGame::RenderFrame @ 0x%08X / 0x%08X (0x6C12C8 -> 0x6C1230).",
                         static_cast<uint32_t>(addrEnter), static_cast<uint32_t>(addrLeave));
        } else {
            Logger::Warn("CpuOptimizer: Could not verify RHGame::RenderFrame CriticalSection bytes at 0x%08X.",
                         static_cast<uint32_t>(addrEnter));
        }
    }

    // Pick one logical processor belonging to a dedicated physical core for the main thread,
    // and place all worker threads (audio/GPU) on the remaining cores.
    void ComputeOptimalAffinityMasks() {
        DWORD_PTR procMask = 0;
        DWORD_PTR sysMask  = 0;
        if (!GetProcessAffinityMask(GetCurrentProcess(), &procMask, &sysMask) || procMask == 0) {
            procMask = 1;
        }

        DWORD returnLength = 0;
        GetLogicalProcessorInformation(nullptr, &returnLength);
        std::vector<DWORD_PTR> physicalCoreMasks;

        if (returnLength > 0) {
            std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> buffer(
                returnLength / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
            if (GetLogicalProcessorInformation(buffer.data(), &returnLength)) {
                for (const auto& info : buffer) {
                    if (info.Relationship == RelationProcessorCore) {
                        DWORD_PTR mask = info.ProcessorMask & procMask;
                        if (mask != 0) {
                            physicalCoreMasks.push_back(mask);
                        }
                    }
                }
            }
        }

        if (physicalCoreMasks.size() >= 3) {
            // Core 1 -> dedicated main game thread (no SMT sibling sharing)
            // Core 2 -> dedicated physical core for internal Game.exe threads (SBInputThreadProc mouse/combat sampling)
            // Remaining cores -> external worker threads (D3D9 presenter, FMOD audio)
            const DWORD_PTR mainCore  = physicalCoreMasks[1];
            const DWORD_PTR inputCore = physicalCoreMasks[2];
            g_mainCoreMask  = mainCore & (~mainCore + 1);
            g_inputCoreMask = inputCore & (~inputCore + 1);
            if (physicalCoreMasks.size() >= 4) {
                g_workerCoreMask = procMask & ~(mainCore | inputCore);
            } else {
                g_workerCoreMask = procMask & ~mainCore;
            }
        } else if (physicalCoreMasks.size() >= 2) {
            DWORD_PTR coreMask = physicalCoreMasks[1];
            DWORD_PTR singleLogical = coreMask & (~coreMask + 1);
            g_mainCoreMask   = singleLogical;
            g_workerCoreMask = procMask & ~coreMask;
            if (g_workerCoreMask == 0) {
                g_workerCoreMask = procMask & ~singleLogical;
            }
            g_inputCoreMask = g_workerCoreMask & (~g_workerCoreMask + 1);
        } else {
            // Fallback: find the lowest two set bits in procMask
            DWORD_PTR firstBit = procMask & (~procMask + 1);
            DWORD_PTR remaining = procMask & ~firstBit;
            if (remaining != 0) {
                DWORD_PTR secondBit = remaining & (~remaining + 1);
                g_mainCoreMask   = secondBit;
                g_workerCoreMask = procMask & ~secondBit;
                g_inputCoreMask  = firstBit;
            } else {
                g_mainCoreMask   = firstBit;
                g_inputCoreMask  = firstBit;
                g_workerCoreMask = firstBit;
            }
        }
    }

    HANDLE WINAPI Hook_CreateThread(
        LPSECURITY_ATTRIBUTES lpThreadAttributes,
        SIZE_T dwStackSize,
        LPTHREAD_START_ROUTINE lpStartAddress,
        LPVOID lpParameter,
        DWORD dwCreationFlags,
        LPDWORD lpThreadId)
    {
        HANDLE hThread = g_OrigCreateThread(
            lpThreadAttributes, dwStackSize, lpStartAddress, lpParameter, dwCreationFlags, lpThreadId);
        if (hThread && g_mainCoreMask != 0 && g_workerCoreMask != 0) {
            HMODULE hOwner = nullptr;
            GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(lpStartAddress),
                &hOwner);

            if (hOwner && hOwner == GetModuleHandleA(nullptr)) {
                // Route internal Game.exe threads (e.g. SBInputThreadProc @ 0x005F0A40) to a dedicated
                // core separate from GameMain so heavy rendering never starves combat mouse sampling.
                // Cursor blitting & RHGame::RenderFrame are already serialized via csInput (0x006C1230).
                SetThreadAffinityMask(hThread, g_inputCoreMask ? g_inputCoreMask : g_workerCoreMask);
            } else {
                // External worker thread (FMOD audio mixer, DirectDraw/D3D9 GPU presenter):
                SetThreadAffinityMask(hThread, g_workerCoreMask);
            }
        }
        return hThread;
    }
}

namespace CpuOptimizer {

void Initialize() {
    // Always patch the ABBA CriticalSection deadlock in Game.exe regardless of affinity settings
    PatchCursorCriticalSectionDeadlock();
    PatchSystemInfo();

    const auto& cfg = SherwoodConfig::Get();
    if (!cfg.threadOptimization) {
        Logger::Info("CpuOptimizer: Thread affinity optimization disabled via config.");
        return;
    }

    g_mainThreadId = GetCurrentThreadId();
    ComputeOptimalAffinityMasks();

    if (g_mainCoreMask != 0) {
        SetThreadAffinityMask(GetCurrentThread(), g_mainCoreMask);
    }

    HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
    if (hKernel32) {
        auto pCreateThread = reinterpret_cast<LPVOID>(GetProcAddress(hKernel32, "CreateThread"));
        if (pCreateThread &&
            MH_CreateHook(pCreateThread,
                          reinterpret_cast<LPVOID>(&Hook_CreateThread),
                          reinterpret_cast<LPVOID*>(&g_OrigCreateThread)) == MH_OK) {
            MH_EnableHook(pCreateThread);
        }
    }

    Logger::Info("CpuOptimizer: Main thread (TID=%u) core=0x%08X; Game input threads core=0x%08X; worker threads=0x%08X.",
                 g_mainThreadId,
                 static_cast<uint32_t>(g_mainCoreMask),
                 static_cast<uint32_t>(g_inputCoreMask),
                 static_cast<uint32_t>(g_workerCoreMask));
}

void Shutdown() {
    // Hooks cleaned up globally by MH_Uninitialize
}

} // namespace CpuOptimizer
