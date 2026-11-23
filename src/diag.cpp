#include "diag.h"
#include "config.h"
#include "logger.h"
#include "save_thumbs.h"

#include <MinHook.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace Diag {
namespace {
    LARGE_INTEGER g_freq{};
    DWORD  g_mainTid    = 0;
    HANDLE g_mainThread = nullptr;

    std::atomic<uint64_t> g_cnt[C_COUNT];
    std::atomic<uint64_t> g_ticks[C_COUNT];

    struct Snapshot {
        uint64_t cnt[C_COUNT]{};
        uint64_t ticks[C_COUNT]{};
    };
    Snapshot g_tickStart;   // taken when a tick's work begins
    Snapshot g_winStart;    // taken when the 5 s report window begins

    uint64_t g_winStartQpc   = 0;
    uint32_t g_winTicks      = 0;
    double   g_winWorkSumUs  = 0.0;
    double   g_winWorkMaxUs  = 0.0;
    uint64_t g_lastSpikeQpc  = 0;
    std::atomic<bool> g_inWork{ false };
    std::atomic<uint64_t> g_lastTickBeginQpc{ 0 };

    void Take(Snapshot& s) {
        for (int i = 0; i < C_COUNT; ++i) {
            s.cnt[i]   = g_cnt[i].load(std::memory_order_relaxed);
            s.ticks[i] = g_ticks[i].load(std::memory_order_relaxed);
        }
    }

    // ---------------------------------------------------------------- event-loop cost
    // RHGame::Run pops one queued input event per loop iteration (0x0050F9DE -> 0x005F1150)
    // and runs the whole input pipeline for it. The time between two consecutive pops is the
    // cost of processing one event.
    using PFN_PopEvent = uint8_t(__cdecl*)(void* evMouse, void* evKeys);
    PFN_PopEvent o_PopEvent = nullptr;
    uint64_t g_lastPopQpc = 0;

    uint8_t __cdecl Hook_PopEvent(void* evMouse, void* evKeys) {
        if (GetCurrentThreadId() == g_mainTid) {
            const uint64_t now = Now();
            uint64_t dt = 0;
            // only count gaps inside one tick (anything above 250 ms is a new tick / loading)
            if (g_lastPopQpc != 0 && now > g_lastPopQpc &&
                (now - g_lastPopQpc) < static_cast<uint64_t>(g_freq.QuadPart / 4)) {
                dt = now - g_lastPopQpc;
            }
            const uint8_t r = o_PopEvent(evMouse, evKeys);
            if (r) {
                Add(C_IN_POP, dt);
            }
            g_lastPopQpc = Now();
            return r;
        }
        return o_PopEvent(evMouse, evKeys);
    }

    // ---------------------------------------------------------------- sampling profiler
    struct Sample {
        uint32_t eip;
        uint32_t ret[3];
    };
    constexpr size_t kMaxSamples = 12000;
    Sample   g_samples[kMaxSamples];
    size_t   g_sampleCount = 0;
    HANDLE   g_profThread  = nullptr;
    std::atomic<bool> g_profStop{ false };

    constexpr uint32_t kGameTextLo = 0x00401000;
    constexpr uint32_t kGameTextHi = 0x00677000;
    constexpr size_t   kStackWords = 2048;
    uint32_t g_mainStackBase = 0;

    bool LooksLikeReturnAddress(uint32_t v) {
        if (v < kGameTextLo + 8 || v >= kGameTextHi) return false;
        const auto* p = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(v));
        if (p[-5] == 0xE8) return true;                                   // call rel32
        if (p[-6] == 0xFF && (p[-5] == 0x15 || (p[-5] & 0xF8) == 0x90)) return true; // call [disp32] / [reg+disp32]
        if (p[-3] == 0xFF && ((p[-2] & 0xF8) == 0x50 || p[-2] == 0x14)) return true; // call [reg+disp8] / [sib]
        if (p[-2] == 0xFF && ((p[-1] & 0xF8) == 0xD0 || (p[-1] & 0xF8) == 0x10)) return true; // call reg / [reg]
        return false;
    }

    // Copies stack words of the suspended thread. Separate function: SEH and C++ unwinding
    // must not mix in one frame.
    size_t CopyStackWords(uint32_t esp, uint32_t* out, size_t maxWords) {
        __try {
            std::memcpy(out, reinterpret_cast<const void*>(static_cast<uintptr_t>(esp)), maxWords * sizeof(uint32_t));
            return maxWords;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    void TakeSample() {
        if (g_sampleCount >= kMaxSamples) return;
        if (SuspendThread(g_mainThread) == static_cast<DWORD>(-1)) return;

        // No allocation, locking or logging until ResumeThread: the suspended thread may hold
        // the heap or logger lock.
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        static uint32_t stack[kStackWords]; // only this thread uses it
        size_t words = 0;
        const BOOL ok = GetThreadContext(g_mainThread, &ctx);
        if (ok) {
            // deep enough to get past driver frames to the first Game.exe return addresses
            size_t want = kStackWords;
            if (g_mainStackBase > ctx.Esp) {
                want = std::min<size_t>(want, (g_mainStackBase - ctx.Esp) / sizeof(uint32_t));
            }
            words = CopyStackWords(ctx.Esp, stack, want);
        }
        ResumeThread(g_mainThread);
        if (!ok) return;

        Sample s{};
        s.eip = ctx.Eip;
        int found = 0;
        for (size_t i = 0; i < words && found < 3; ++i) {
            if (LooksLikeReturnAddress(stack[i])) {
                s.ret[found++] = stack[i];
            }
        }
        g_samples[g_sampleCount++] = s;
    }

    struct ModInfo {
        uintptr_t base = 0;
        std::string name;
    };

    const ModInfo& ModuleOf(uint32_t addr, std::unordered_map<uintptr_t, ModInfo>& cache) {
        static const ModInfo kUnknown{ 0, "?" };
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(addr)), &mbi, sizeof(mbi)) != sizeof(mbi) ||
            !mbi.AllocationBase) {
            return kUnknown;
        }
        const auto key = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
        auto it = cache.find(key);
        if (it != cache.end()) return it->second;

        ModInfo mi;
        mi.base = key;
        char path[MAX_PATH]{};
        if (GetModuleFileNameA(reinterpret_cast<HMODULE>(mbi.AllocationBase), path, MAX_PATH) > 0) {
            const char* slash = path;
            for (const char* p = path; *p; ++p) {
                if (*p == '\\' || *p == '/') slash = p + 1;
            }
            mi.name = slash;
        } else {
            mi.name = "<jit/heap>";
        }
        return cache.emplace(key, std::move(mi)).first->second;
    }

    template <typename K>
    std::vector<std::pair<K, uint32_t>> TopN(const std::unordered_map<K, uint32_t>& m, size_t n) {
        std::vector<std::pair<K, uint32_t>> v(m.begin(), m.end());
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        if (v.size() > n) v.resize(n);
        return v;
    }

    void ReportSamples() {
        const size_t n = g_sampleCount;
        g_sampleCount = 0;
        if (n < 200) return; // too few work-phase samples to mean anything

        std::unordered_map<uintptr_t, ModInfo> modCache;
        std::unordered_map<std::string, uint32_t> byModule;
        std::unordered_map<uint64_t, uint32_t> byBucket;   // (module base << 32) | rva of 64-byte bucket
        std::unordered_map<uint32_t, uint32_t> byCaller1;
        std::unordered_map<uint32_t, uint32_t> byCaller2;
        std::unordered_map<uint32_t, uint32_t> byCaller3;

        for (size_t i = 0; i < n; ++i) {
            const Sample& s = g_samples[i];
            const ModInfo& mi = ModuleOf(s.eip, modCache);
            ++byModule[mi.name];
            const uint32_t rva = (mi.base != 0) ? (s.eip - static_cast<uint32_t>(mi.base)) : s.eip;
            ++byBucket[(static_cast<uint64_t>(mi.base) << 32) | (rva & ~0x3Fu)];
            if (s.ret[0]) ++byCaller1[s.ret[0]];
            if (s.ret[1]) ++byCaller2[s.ret[1]];
            if (s.ret[2]) ++byCaller3[s.ret[2]];
        }

        char line[1800];
        int pos = std::snprintf(line, sizeof(line), "Prof: samples=%zu |", n);
        for (const auto& [name, c] : TopN(byModule, 8)) {
            pos += std::snprintf(line + pos, sizeof(line) - pos, " %s %.1f%%", name.c_str(), 100.0 * c / n);
            if (pos > 1700) break;
        }
        Logger::Info("%s", line);

        pos = std::snprintf(line, sizeof(line), "Prof.hot:");
        for (const auto& [key, c] : TopN(byBucket, 16)) {
            const auto base = static_cast<uintptr_t>(key >> 32);
            const auto rva  = static_cast<uint32_t>(key & 0xFFFFFFFFu);
            const char* name = "?";
            auto it = modCache.find(base);
            if (it != modCache.end()) name = it->second.name.c_str();
            if (base == 0x00400000) {
                pos += std::snprintf(line + pos, sizeof(line) - pos, " %08X %.1f%% |", static_cast<uint32_t>(base) + rva, 100.0 * c / n);
            } else {
                pos += std::snprintf(line + pos, sizeof(line) - pos, " %s+%06X %.1f%% |", name, rva, 100.0 * c / n);
            }
            if (pos > 1700) break;
        }
        Logger::Info("%s", line);

        auto printCallers = [&](const char* tag, const std::unordered_map<uint32_t, uint32_t>& m) {
            int p = std::snprintf(line, sizeof(line), "%s", tag);
            for (const auto& [addr, c] : TopN(m, 14)) {
                p += std::snprintf(line + p, sizeof(line) - p, " %08X %.1f%% |", addr, 100.0 * c / n);
                if (p > 1700) break;
            }
            Logger::Info("%s", line);
        };
        printCallers("Prof.ret1:", byCaller1);
        printCallers("Prof.ret2:", byCaller2);
        printCallers("Prof.ret3:", byCaller3);
    }

    // ---------------------------------------------------------------- file open failures
    // The game loads sprite frames from its data files on demand and does not check every
    // result. A file it cannot open - seen: Data\RobinHood.bks, which the game opens
    // exclusively, still held by a leftover Game.exe - turns into garbage sprite data and an
    // access violation in the sprite decoder (0x005E399B) seconds into a mission, far away from
    // the cause. Failed opens are logged; "not found" / "access denied" probes of optional
    // files and of directories are normal and skipped.
    using PFN_CreateFileA = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
    PFN_CreateFileA o_CreateFileA = nullptr;
    std::atomic<int> g_fileFailLogged{ 0 };

    HANDLE WINAPI Hook_CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                                   DWORD disposition, DWORD flags, HANDLE templ) {
        const HANDLE h = o_CreateFileA(name, access, share, sa, disposition, flags, templ);
        if (h != INVALID_HANDLE_VALUE) {
            const DWORD err = GetLastError();
            SaveThumbs::NoteFileOpened(name, access);   // a save's picture read: the load screen shows its date
            SetLastError(err);
        }
        if (h == INVALID_HANDLE_VALUE) {
            const DWORD err = GetLastError();
            if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND && err != ERROR_ACCESS_DENIED &&
                g_fileFailLogged.fetch_add(1) < 30) {
                Logger::Warn("File open FAILED (error %lu%s): %s [access=0x%08X share=0x%X]", err,
                             err == ERROR_SHARING_VIOLATION ? " = in use by another process" : "",
                             name ? name : "(null)", access, share);
            }
            SetLastError(err);
        }
        return h;
    }

    // The process exit path lives in watchdog.cpp (ExitProcess hook); it calls StopProfiler()
    // first so that the profiler can never be killed while it holds the main thread suspended.
    DWORD WINAPI ProfilerThread(LPVOID) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        uint64_t lastReport = Now();
        while (!g_profStop.load(std::memory_order_relaxed)) {
            ::SleepEx(1, FALSE);
            // sample only while game ticks are flowing (not in menus, while paused in the
            // background, or during shutdown)
            if (g_inWork.load(std::memory_order_relaxed) &&
                (Now() - g_lastTickBeginQpc.load(std::memory_order_relaxed)) < static_cast<uint64_t>(g_freq.QuadPart) / 4) {
                TakeSample();
            }
            const uint64_t now = Now();
            if (now - lastReport >= static_cast<uint64_t>(g_freq.QuadPart) * 5ULL) {
                lastReport = now;
                ReportSamples();
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

    const auto& cfg = SherwoodConfig::Get();

    // Game.exe's own import of CreateFileA only (0x00677194): other DLLs open thousands of
    // unrelated files and must not be slowed down or logged.
    {
        // Whatever the loader (or a compatibility shim) put into the slot stays the target.
        auto** slot = reinterpret_cast<void**>(0x00677194);
        void* current = *slot;
        MEMORY_BASIC_INFORMATION mbi{};
        const bool looksLikeCode = current && VirtualQuery(current, &mbi, sizeof(mbi)) == sizeof(mbi) &&
                                   mbi.State == MEM_COMMIT && mbi.Type == MEM_IMAGE;
        DWORD oldProt = 0;
        if (looksLikeCode && VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProt)) {
            o_CreateFileA = reinterpret_cast<PFN_CreateFileA>(current);
            *slot = reinterpret_cast<void*>(&Hook_CreateFileA);
            VirtualProtect(slot, sizeof(void*), oldProt, &oldProt);
        }
    }

    if (!cfg.perfLog && !cfg.profiler) {
        return;
    }

    // RHGame::Run input event pop @ 0x005F1150: sub esp,18h / push ebx / mov ebx,[EnterCriticalSection]
    auto* popAddr = reinterpret_cast<uint8_t*>(0x005F1150);
    static const uint8_t kPopProlog[] = { 0x83, 0xEC, 0x18, 0x53, 0x8B, 0x1D };
    if (std::memcmp(popAddr, kPopProlog, sizeof(kPopProlog)) == 0) {
        if (MH_CreateHook(popAddr, reinterpret_cast<LPVOID>(&Hook_PopEvent),
                          reinterpret_cast<LPVOID*>(&o_PopEvent)) == MH_OK) {
            MH_EnableHook(popAddr);
        }
    } else {
        Logger::Warn("Diag: input PopEvent prologue mismatch @ 0x005F1150 - event-loop cost not measured.");
    }

    if (cfg.profiler && g_mainThread) {
        g_profThread = CreateThread(nullptr, 0, ProfilerThread, nullptr, 0, nullptr);
        Logger::Info("Diag: sampling profiler enabled (1 kHz, main thread TID=%lu).", g_mainTid);
    }
}

void StopProfiler() {
    g_profStop.store(true, std::memory_order_relaxed);
    // On the main thread the profiler cannot be holding us suspended right now (we are running),
    // and it takes no further sample once the flag is set - a short wait lets it finish its log line.
    if (g_profThread && GetCurrentThreadId() == g_mainTid) {
        WaitForSingleObject(g_profThread, 300);
    }
}

void Shutdown() {
    g_profStop.store(true, std::memory_order_relaxed);
    if (g_profThread) {
        WaitForSingleObject(g_profThread, 200);
        CloseHandle(g_profThread);
        g_profThread = nullptr;
    }
}

uint64_t Now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<uint64_t>(t.QuadPart);
}

double TicksToMs(uint64_t ticks) {
    return static_cast<double>(ticks) * 1000.0 / static_cast<double>(g_freq.QuadPart);
}

bool IsMainThread() {
    return GetCurrentThreadId() == g_mainTid;
}

void Add(Counter c, uint64_t ticks) {
    g_cnt[c].fetch_add(1, std::memory_order_relaxed);
    if (ticks) {
        g_ticks[c].fetch_add(ticks, std::memory_order_relaxed);
    }
}

void AddValue(Counter c, uint64_t value) {
    g_ticks[c].fetch_add(value, std::memory_order_relaxed);
}

Scope::Scope(Counter counter) : c(counter), t0(Now()) {
    if (c <= C_DD_FLIP && GetCurrentThreadId() != g_mainTid) {
        c = C_DD_OTHER_THREAD;
    }
}

Scope::~Scope() {
    Add(c, Now() - t0);
}

void OnTickWorkEnd(double mainWorkUs) {
    g_inWork.store(false, std::memory_order_relaxed);
    if (!SherwoodConfig::Get().perfLog || mainWorkUs <= 0.0) {
        return;
    }

    Snapshot now;
    Take(now);
    const uint64_t nowQpc = Now();
    auto dc = [&](Counter c, const Snapshot& from) { return now.cnt[c] - from.cnt[c]; };
    auto dms = [&](Counter c, const Snapshot& from) { return TicksToMs(now.ticks[c] - from.ticks[c]); };

    ++g_winTicks;
    g_winWorkSumUs += mainWorkUs;
    g_winWorkMaxUs = std::max(g_winWorkMaxUs, mainWorkUs);

    // One slow tick: where did it go? (rate-limited to one line per second, loading screens skipped)
    if (mainWorkUs > 60000.0 && mainWorkUs < 1000000.0 &&
        (nowQpc - g_lastSpikeQpc) > static_cast<uint64_t>(g_freq.QuadPart)) {
        g_lastSpikeQpc = nowQpc;
        Logger::Warn("Spike: tick=%.1fms | events=%llu evtProc=%.1fms | Lock n=%llu %.1fms, Unlock %.1fms, Blt n=%llu %.1fms, Flip %.1fms | snd n=%llu %.1fms | ddOtherThr n=%llu",
                     mainWorkUs / 1000.0,
                     dc(C_IN_POP, g_tickStart), dms(C_IN_POP, g_tickStart),
                     dc(C_DD_LOCK, g_tickStart), dms(C_DD_LOCK, g_tickStart),
                     dms(C_DD_UNLOCK, g_tickStart),
                     dc(C_DD_BLT, g_tickStart), dms(C_DD_BLT, g_tickStart),
                     dms(C_DD_FLIP, g_tickStart),
                     dc(C_SND_PLAY, g_tickStart) + dc(C_SND_LOAD, g_tickStart),
                     dms(C_SND_PLAY, g_tickStart) + dms(C_SND_LOAD, g_tickStart),
                     dc(C_DD_OTHER_THREAD, g_tickStart));
    }

    if (g_winStartQpc == 0) {
        g_winStartQpc = nowQpc;
        g_winStart = now;
        g_winTicks = 0;
        g_winWorkSumUs = 0.0;
        g_winWorkMaxUs = 0.0;
        return;
    }

    if ((nowQpc - g_winStartQpc) >= static_cast<uint64_t>(g_freq.QuadPart) * 5ULL && g_winTicks > 0) {
        const double sec = static_cast<double>(nowQpc - g_winStartQpc) / static_cast<double>(g_freq.QuadPart);
        const double t = static_cast<double>(g_winTicks);
        const uint64_t pops = dc(C_IN_POP, g_winStart);
        Logger::Info("Perf(5s): Tick=%.1fHz Work=%.1fms (max %.0f) Present=%.1fHz | per tick: events=%.1f evtProc=%.1fms (%.2f ms/evt) | "
                     "Lock n=%.1f %.2fms, Unlock %.2fms, Blt n=%.1f %.2fms, Flip %.2fms | snd %.2fms | sub n=%llu %.1fms",
                     t / sec, g_winWorkSumUs / (1000.0 * t), g_winWorkMaxUs / 1000.0,
                     static_cast<double>(dc(C_PRESENT, g_winStart)) / sec,
                     static_cast<double>(pops) / t, dms(C_IN_POP, g_winStart) / t,
                     pops ? dms(C_IN_POP, g_winStart) / static_cast<double>(pops) : 0.0,
                     static_cast<double>(dc(C_DD_LOCK, g_winStart)) / t, dms(C_DD_LOCK, g_winStart) / t,
                     dms(C_DD_UNLOCK, g_winStart) / t,
                     static_cast<double>(dc(C_DD_BLT, g_winStart)) / t, dms(C_DD_BLT, g_winStart) / t,
                     dms(C_DD_FLIP, g_winStart) / t,
                     (dms(C_SND_PLAY, g_winStart) + dms(C_SND_LOAD, g_winStart)) / t,
                     dc(C_SUBFRAME, g_winStart),
                     dc(C_SUBFRAME, g_winStart) ? dms(C_SUBFRAME, g_winStart) / static_cast<double>(dc(C_SUBFRAME, g_winStart)) : 0.0);
        const uint64_t motionTicks = dc(C_MOTION, g_winStart);
        Logger::Info("Input(5s): mouse raw=%.0f/s queued=%.0f/s | ddraw calls from other threads=%llu (%.1fms) | sounds started=%llu failed=%llu inExtraFrame=%llu | smoothed objects/tick=%.1f",
                     static_cast<double>(dc(C_IN_RAW, g_winStart)) / sec,
                     static_cast<double>(dc(C_IN_EMIT, g_winStart)) / sec,
                     dc(C_DD_OTHER_THREAD, g_winStart), dms(C_DD_OTHER_THREAD, g_winStart),
                     dc(C_SND_PLAY, g_winStart), dc(C_SND_FAIL, g_winStart), dc(C_SND_EXTRA, g_winStart),
                     motionTicks ? static_cast<double>(now.ticks[C_MOTION] - g_winStart.ticks[C_MOTION]) / static_cast<double>(motionTicks) : 0.0);
        if (dc(C_BLT_COPY, g_winStart) + dc(C_BLT_KEY, g_winStart) + dc(C_BLT_FILL, g_winStart) + dc(C_BLT_STRETCH, g_winStart) > 0) {
            auto mpx = [&](Counter c) { return static_cast<double>(now.ticks[c] - g_winStart.ticks[c]) / (1.0e6 * t); };
            Logger::Info("Blit(5s) per tick: copy n=%.1f %.2fMpx %.2fms | key n=%.1f %.2fMpx %.2fms | fill n=%.1f %.2fMpx %.2fms | stretch n=%.1f %.2fMpx %.2fms",
                         static_cast<double>(dc(C_BLT_COPY, g_winStart)) / t, mpx(C_PX_COPY), dms(C_BLT_COPY, g_winStart) / t,
                         static_cast<double>(dc(C_BLT_KEY, g_winStart)) / t, mpx(C_PX_KEY), dms(C_BLT_KEY, g_winStart) / t,
                         static_cast<double>(dc(C_BLT_FILL, g_winStart)) / t, mpx(C_PX_FILL), dms(C_BLT_FILL, g_winStart) / t,
                         static_cast<double>(dc(C_BLT_STRETCH, g_winStart)) / t, mpx(C_PX_STRETCH), dms(C_BLT_STRETCH, g_winStart) / t);
        }
        g_winStartQpc = nowQpc;
        g_winStart = now;
        g_winTicks = 0;
        g_winWorkSumUs = 0.0;
        g_winWorkMaxUs = 0.0;
    }
}

void OnTickWorkBegin() {
    Take(g_tickStart);
    g_lastPopQpc = 0; // the gap up to the first pop of a tick is rendering + pacing, not event work
    g_lastTickBeginQpc.store(Now(), std::memory_order_relaxed);
    g_inWork.store(true, std::memory_order_relaxed);
}
}
