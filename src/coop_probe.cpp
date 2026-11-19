#include "coop_probe.h"
#include "logger.h"
#include "test_harness.h"
#include "timing.h"

#include <MinHook.h>
#include <windows.h>
#include <intrin.h>
#include <share.h>
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#pragma intrinsic(_ReturnAddress, _AddressOfReturnAddress)

// Co-op feasibility probe - see coop_probe.h. Everything here runs only in the background test mode.
//
// How player input reaches the simulation (static RE, docs\RE_NOTES.md "Co-op"):
//   RHGame::Run event loop -> RHGameInputTranslator::HandleEvent 0x005460C0 (thiscall(tr, Event*), ret 4):
//   the translator digests the event (sword trail, key -> action) and broadcasts it to its listeners
//   ([tr+8]..[tr+0xC], vtbl[0]); one of them is the engine, RHEngine+0x60, HandleEvent 0x004DC2C0
//   (thiscall(E+0x60, Event*), ret 4). The engine turns pointer events into world actions (left click ->
//   RHEngine::OnWorldClick 0x004DE290, double click 0x004DE5E0, drag 0x004DDFB0) and game events
//   (category 2: code +0xC, argument +0x10) into the rest. That broadcast is the narrowest point all player
//   commands pass, so the probe records exactly the events the engine listener gets from the input side,
//   with their engine tick, and replays them into the same listener.
namespace CoopProbe {
namespace {
    // ---- Game.exe (GOG). Prologues read from the file with tools\fninfo.py -------------------------------
    constexpr uintptr_t kEnginePtr       = 0x006C0FA8; // RHEngine*
    constexpr uintptr_t kCrtTlsSlot      = 0x006BFAA0; // TLS index of the CRT per-thread data (_getptd 0x00645933)
    constexpr uintptr_t kTextBegin       = 0x00401000;
    constexpr uintptr_t kTextEnd         = 0x00676E39;
    constexpr uintptr_t kIatGetTickCount = 0x0067718C;
    constexpr uintptr_t kIatTimeGetTime  = 0x006772C8;
    // RHsound*.cpp code (strings RHSound .. RHSoundSourceManager referenced from 0x005A6684..0x005B52D2;
    // RHSprite starts at 0x005B5890). Its rand() calls pick sample variants (0x005AE380, 0x005A8220, ...).
    constexpr uint32_t  kSoundCodeBegin  = 0x005A6000;
    constexpr uint32_t  kSoundCodeEnd    = 0x005B5800;
    constexpr uint32_t  kIsolateSoundBit = 64;

    constexpr int E_Listener   = 0x60;     // the engine's listener sub-object (vtable 0x00678B34)
    constexpr int E_Translator = 0x68;     // RHGameInputTranslator* (the singleton [0x006C0C4C])
    constexpr int E_Camera     = 0x4738;   // Vec2 float
    constexpr int E_Zoom       = 0x475C;   // float
    constexpr int E_SelHead    = 0x49FC;   // selected PCs: std::list sentinel node {next, prev, value}
    constexpr int E_SelCount   = 0x4A00;
    constexpr int kElementLists[] = { 0x48D8, 0x4918, 0x49D8, 0x49B8, 0x4998 }; // live, actors, master, FX, FX

    // translator state copied into a replayed event: plain values only (+0x2C.. and +0x5C.. hold objects)
    constexpr int TR_A = 0x14, TR_A_LEN = 0x18;   // flags, run / modifier bytes, action words
    constexpr int TR_B = 0x84, TR_B_LEN = 0x28;   // pointer position, click positions, cursor mode +0xA2

    enum Ctx : int { C_NONE = 0, C_INPUT, C_LOGIC, C_RENDER, C_EXTRA, C_SOUND, C_COUNT };
    const char* const kCtxName[C_COUNT] = { "other", "input", "logic", "render", "extra", "sound" };

    enum Mode : int { M_OFF = 0, M_ARM_REC, M_ARM_PLAY, M_REC, M_PLAY };

    struct Field { uint16_t off, size; };
    // RHElement::Serialize (0x00462337): what every element keeps
    constexpr Field kBaseFields[] = {
        {0x04, 4}, {0x08, 1}, {0x0C, 4}, {0x10, 2}, {0x12, 1}, {0x70, 1}, {0x71, 1}, {0x88, 1},
        {0x8A, 2}, {0x8C, 2}, {0x94, 10}, {0xA0, 4}, {0xA4, 2}, {0xA6, 1},
    };
    // RHElementActor::Serialize (0x0046345E) and RHElementActorHuman::Serialize (0x004799EC): plain fields
    // written to the save file, humans only ((type & 7) == 4: NPC 4, PC 0xC, soldier 0x14). Not here:
    // +0x134 (a sequence pointer, saved through 0x0058AD80) and +0x1D8 (a heap pointer inside the raw
    // 0x1C-byte block +0x1D0) - both differ from run to run (first runs of 2026-11-19).
    constexpr Field kHumanFields[] = {
        {0x10C, 4}, {0x110, 1}, {0x114, 4}, {0x118, 1}, {0x119, 1}, {0x124, 4}, {0x13C, 1}, {0x140, 4},
        {0x144, 4}, {0x148, 1}, {0x149, 1}, {0x14C, 4}, {0x160, 4}, {0x164, 1}, {0x165, 1}, {0x16C, 2},
        {0x16E, 1}, {0x16F, 1}, {0x1A4, 1},
        {0x1C8, 2}, {0x1CA, 2}, {0x1CC, 2}, {0x1CE, 1}, {0x1D0, 8}, {0x1DC, 0x10}, {0x230, 4}, {0x234, 4},
        {0x238, 4}, {0x23C, 2}, {0x23E, 1}, {0x23F, 1}, {0x240, 2}, {0x288, 1}, {0x289, 1}, {0x28A, 2},
        {0x28C, 4}, {0x47C, 1}, {0x480, 4}, {0x484, 4}, {0x488, 2}, {0x48A, 1}, {0x48C, 4}, {0x490, 1},
        {0x492, 2}, {0x494, 2}, {0x496, 1}, {0x497, 1},
    };

    // ---- state (main thread unless noted) ----------------------------------------------------------
    bool      g_installed = false;
    DWORD     g_mainThread = 0;
    int       g_ctx = C_NONE;
    std::recursive_mutex g_lock;          // files and session switches (main thread vs. command thread)
    std::string g_dir;                    // ...\tools\out\harness\ (trailing backslash)

    volatile int g_mode = M_OFF;
    std::string  g_name, g_tag;
    uint32_t  g_seed = 0x5EED1234;
    uint32_t  g_tick = 0;                 // engine ticks since the session start
    uint32_t  g_seq = 0;                  // events logged / replayed in this session
    FILE*     g_hashFile = nullptr;
    FILE*     g_dumpFile = nullptr;
    FILE*     g_eventFile = nullptr;
    FILE*     g_traceFile = nullptr;
    FILE*     g_randFile = nullptr;           // every main-thread rand() call of the session: tick, context, caller
    FILE*     g_orderFile = nullptr;          // every RHOrder made in the session: tick, context, callers
    volatile bool g_dumpNext = false;
    bool      g_pinSrand = true;          // srand() inside a session: a seed derived from the session, not the clock
    uint32_t  g_isolateMask = 0;          // contexts whose rand() runs on its own generator
    uint32_t  g_cosmetic = 0x1234567;
    bool      g_camFixed = false;         // replay: keep the camera here instead of the recorded one
    float     g_camFix[2]{};
    uint32_t  g_dumpEvery = 50;           // element dump every N ticks (coop dumpevery N)
    uint32_t  g_loopPasses = 0;           // RHGame::Run loop passes in the session (a modal dialog stops them)
    int       g_lastTickResult = -1;      // what RHEngine::Tick returned last ([pGame+0x3550])
    bool      g_tickedThisLoop = false;
    std::vector<std::string> g_refHash;   // replay: the recording's "all humans" hash per tick (dump where they differ)
    uint32_t  g_refMismatch = 0;

    uint64_t  g_randCount[C_COUNT] = {};
    uint64_t  g_randSoundCode = 0;        // of those: called from the sound module
    std::atomic<uint64_t> g_randOtherThread{ 0 };
    uint64_t  g_clockCount[C_COUNT] = {};
    uint32_t  g_srandCalls = 0, g_srandPinned = 0;
    uint32_t  g_listenerDepth = 0;
    uint32_t  g_dropped = 0, g_replayed = 0, g_replaySkipped = 0, g_selMismatch = 0, g_swingMismatch = 0;
    uint32_t  g_renderEvents = 0, g_logicEvents = 0;

    struct Slot { uint32_t key; uint32_t count; };
    constexpr uint32_t kHistSize = 8192;
    Slot g_hist[kHistSize];

    struct LoggedEvent {
        uint32_t tick = 0, seq = 0;
        uint8_t  ev[0x18]{};
        uint8_t  trA[TR_A_LEN]{};
        uint8_t  trB[TR_B_LEN]{};
        float    cam[2]{}, zoom = 0.0f;
        int32_t  argId = -1;              // category 2: +0x10 was this element (by id)
        bool     argForeign = false;      // +0x10 looked like a pointer, but to no element
        uint32_t nWorld = 0;
        float    world[2]{};
        std::vector<uint32_t> swings;
        std::vector<uint32_t> sel;        // ids of the selected PCs before the event
    };
    LoggedEvent* g_recording = nullptr;   // the event being recorded (inside its dispatch)
    const LoggedEvent* g_replaying = nullptr;
    size_t    g_swingIndex = 0;
    std::vector<LoggedEvent> g_play;
    size_t    g_playIndex = 0;

    // ---- small helpers -----------------------------------------------------------------------------
    struct Fnv {
        uint64_t h = 1469598103934665603ULL;
        void Add(const void* p, size_t n) {
            const auto* b = static_cast<const uint8_t*>(p);
            for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ULL; }
        }
    };
    uint64_t Mix(uint64_t x) {
        x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL; x ^= x >> 33;
        return x;
    }

    bool OnMain() { return g_mainThread != 0 && GetCurrentThreadId() == g_mainThread; }

    int CurrentCtx() {
        if (Timing::InExtraFrame()) return C_EXTRA;
        if (Timing::InRenderFrame()) return C_RENDER;
        return g_ctx;
    }

    // The CRT's rand() state of the calling thread ([_getptd() + 0x14]); null before its first rand/srand.
    uint32_t* Holdrand() {
        const DWORD err = GetLastError();
        auto* ptd = static_cast<uint8_t*>(TlsGetValue(*reinterpret_cast<const DWORD*>(kCrtTlsSlot)));
        SetLastError(err);
        return ptd ? reinterpret_cast<uint32_t*>(ptd + 0x14) : nullptr;
    }

    void HistAdd(uint32_t ret, int ctx) {
        const uint32_t key = (ret & 0x00FFFFFFu) | (static_cast<uint32_t>(ctx) << 24);
        uint32_t h = (key * 2654435761u) >> 19;
        for (uint32_t i = 0; i < kHistSize; ++i) {
            Slot& s = g_hist[(h + i) & (kHistSize - 1)];
            if (s.key == key) { ++s.count; return; }
            if (s.key == 0) { s.key = key; s.count = 1; return; }
        }
    }

    // Return addresses into Game.exe's code above `from` on this thread's stack: a dword counts when
    // the bytes before it are a call (E8 rel32 into .text, or the FF /2 forms).
    int ScanCallers(const void* from, uint32_t* out, int max) {
        const auto* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
        const uintptr_t top = reinterpret_cast<uintptr_t>(tib->StackBase);
        uintptr_t p = reinterpret_cast<uintptr_t>(from);
        int n = 0;
        for (int i = 0; i < 2048 && p + 4 <= top && n < max; ++i, p += 4) {
            const uint32_t v = *reinterpret_cast<const uint32_t*>(p);
            if (v < kTextBegin + 8 || v >= kTextEnd) continue;
            const auto* c = reinterpret_cast<const uint8_t*>(v);
            bool call = false;
            if (c[-5] == 0xE8) {
                const uint32_t target = v + *reinterpret_cast<const int32_t*>(c - 4);
                call = target >= kTextBegin && target < kTextEnd;
            }
            if (!call && c[-6] == 0xFF && (c[-5] & 0x38) == 0x10) call = true;                                  // [disp32] / [reg+disp32]
            if (!call && c[-3] == 0xFF && (c[-2] & 0x38) == 0x10 && (c[-2] & 0xC0) == 0x40) call = true;        // [reg+disp8]
            if (!call && c[-2] == 0xFF && (c[-1] & 0x38) == 0x10 && ((c[-1] & 0xC0) == 0 || (c[-1] & 0xC0) == 0xC0)) call = true;
            if (call) out[n++] = v;
        }
        return n;
    }

    std::string ChainText(const void* from) {
        uint32_t c[10];
        const int n = ScanCallers(from, c, 10);
        std::string s;
        char t[16];
        for (int i = 0; i < n; ++i) {
            std::snprintf(t, sizeof(t), "%s%06X", i ? "<" : "", c[i]);
            s += t;
        }
        return s;
    }

    void Trace(const char* fmt, ...) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        if (!g_traceFile) return;
        char text[1600];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(text, sizeof(text), fmt, args);
        va_end(args);
        std::fprintf(g_traceFile, "[%u] %s\n", g_tick, text);
        std::fflush(g_traceFile);
    }

    FILE* OpenOut(const std::string& leaf, const char* mode) {
        return _fsopen((g_dir + leaf).c_str(), mode, _SH_DENYNO);
    }

    // ---- reading the world (SEH: only raw pointers inside) ---------------------------------------------
    constexpr int kMaxElements = 6000;
    uint8_t* g_el[kMaxElements];

    int CollectElements(uint8_t* E) {
        int n = 0;
        __try {
            for (int off : kElementLists) {
                uint8_t** it  = *reinterpret_cast<uint8_t***>(E + off + 4);
                uint8_t** end = *reinterpret_cast<uint8_t***>(E + off + 8);
                if (!it || end < it || end - it > 4096) continue;
                for (; it < end && n < kMaxElements; ++it) {
                    if (*it) g_el[n++] = *it;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return -1;
        }
        std::sort(g_el, g_el + n);
        return static_cast<int>(std::unique(g_el, g_el + n) - g_el);
    }

    struct ElemInfo {
        uint32_t id, type;
        uint8_t  active;
        float    pos[3];
        uint16_t facing, anim, frame, sub;
        bool     human;
        uint64_t h;
    };

    bool ReadElement(uint8_t* el, ElemInfo& o) {
        __try {
            Fnv f;
            for (const Field& fl : kBaseFields) f.Add(el + fl.off, fl.size);
            o.type = *reinterpret_cast<const uint32_t*>(el + 0x04);
            o.id = *reinterpret_cast<const uint32_t*>(el + 0x0C);
            o.active = el[0x12];
            if (el[0x70]) f.Add(el + 0x74, 12);       // pending 3D position (valid while the flag is set)
            if (el[0x71]) f.Add(el + 0x80, 8);        // pending projected position
            o.pos[0] = o.pos[1] = o.pos[2] = 0.0f;
            o.facing = o.anim = o.frame = o.sub = 0;
            const uint8_t* S = *reinterpret_cast<uint8_t* const*>(el + 0x50);
            if (S) {
                const uint8_t* P = S + 4;
                std::memcpy(o.pos, P + 0x0C, 12);
                o.facing = *reinterpret_cast<const uint16_t*>(P + 0xD4);
                o.anim = *reinterpret_cast<const uint16_t*>(S + 0x184);
                o.frame = *reinterpret_cast<const uint16_t*>(S + 0x186);
                o.sub = *reinterpret_cast<const uint16_t*>(S + 0x188);
                f.Add(P + 0x0C, 12);
                f.Add(P + 0xD4, 2);
                f.Add(S + 0x184, 6);
            }
            o.human = (o.type & 7) == 4;
            if (o.human) {
                for (const Field& fl : kHumanFields) f.Add(el + fl.off, fl.size);
            }
            o.h = f.h;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
        return true;
    }

    bool ReadHumanHex(uint8_t* el, char* out, size_t outSize) {
        __try {
            size_t pos = 0;
            for (const Field& fl : kHumanFields) {
                for (int i = 0; i < fl.size && pos + 3 < outSize; ++i) {
                    static const char kHex[] = "0123456789ABCDEF";
                    out[pos++] = kHex[el[fl.off + i] >> 4];
                    out[pos++] = kHex[el[fl.off + i] & 15];
                }
                if (pos + 2 < outSize) out[pos++] = '.';
            }
            out[pos] = 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            out[0] = 0;
            return false;
        }
        return true;
    }

    int ReadSelection(uint8_t* E, uint32_t* ids, int max) {
        int n = 0;
        __try {
            const uint32_t count = *reinterpret_cast<const uint32_t*>(E + E_SelCount);
            uint8_t* head = *reinterpret_cast<uint8_t**>(E + E_SelHead);
            if (!head) return 0;
            uint8_t* node = *reinterpret_cast<uint8_t**>(head);
            for (uint32_t i = 0; i < count && node && node != head && n < max; ++i) {
                uint8_t* pc = *reinterpret_cast<uint8_t**>(node + 8);
                ids[n++] = pc ? *reinterpret_cast<const uint32_t*>(pc + 0x0C) : 0xFFFFFFFFu;
                node = *reinterpret_cast<uint8_t**>(node);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return -1;
        }
        return n;
    }

    uint8_t* FindElement(uint8_t* E, uint32_t id) {
        const int n = CollectElements(E);
        for (int i = 0; i < n; ++i) {
            uint32_t eid = 0;
            __try { eid = *reinterpret_cast<const uint32_t*>(g_el[i] + 0x0C); } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
            if (eid == id) return g_el[i];
        }
        return nullptr;
    }

    // which element is this pointer, if any (-1: none)
    int32_t ElementId(uint8_t* E, uint32_t ptr) {
        const int n = CollectElements(E);
        for (int i = 0; i < n; ++i) {
            if (reinterpret_cast<uintptr_t>(g_el[i]) == ptr) {
                uint32_t eid = 0;
                __try { eid = *reinterpret_cast<const uint32_t*>(g_el[i] + 0x0C); } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
                return static_cast<int32_t>(eid);
            }
        }
        return -1;
    }

    // SEH helpers for the hooks (a function with __try may hold no C++ objects)
    void ReadWords(const uint8_t* p, uint32_t* w, int n) {
        __try {
            std::memcpy(w, p, static_cast<size_t>(n) * 4);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            std::memset(w, 0, static_cast<size_t>(n) * 4);
        }
    }

    bool CaptureForLog(uint8_t* E, const uint8_t* ev, uint8_t* evOut, size_t evLen, uint8_t* trA, uint8_t* trB,
                       float* cam, float* zoom) {
        __try {
            std::memcpy(evOut, ev, evLen);
            const uint8_t* tr = *reinterpret_cast<uint8_t* const*>(E + E_Translator);
            if (tr) {
                std::memcpy(trA, tr + TR_A, TR_A_LEN);
                std::memcpy(trB, tr + TR_B, TR_B_LEN);
            }
            std::memcpy(cam, E + E_Camera, 8);
            std::memcpy(zoom, E + E_Zoom, 4);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
        return true;
    }

    // the translator's listeners ([tr+8]..[tr+0xC]): object, vtable, vtbl[0] = its HandleEvent
    void DescribeListeners(char* out, size_t outSize) {
        size_t pos = 0;
        out[0] = 0;
        __try {
            const uint8_t* tr = *reinterpret_cast<uint8_t* const*>(0x006C0C4C);
            if (!tr) { std::snprintf(out, outSize, "no translator"); return; }
            uint8_t* const* it = *reinterpret_cast<uint8_t* const* const*>(tr + 8);
            uint8_t* const* end = *reinterpret_cast<uint8_t* const* const*>(tr + 0xC);
            for (int i = 0; it && it < end && i < 32 && pos + 48 < outSize; ++it, ++i) {
                const uint8_t* obj = *it;
                const uint32_t vt = obj ? *reinterpret_cast<const uint32_t*>(obj) : 0;
                const uint32_t fn = vt ? *reinterpret_cast<const uint32_t*>(vt) : 0;
                pos += static_cast<size_t>(std::snprintf(out + pos, outSize - pos, " %p:vt=%06X:fn=%06X", static_cast<const void*>(obj), vt, fn));
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            std::snprintf(out + pos, outSize - pos, " (fault)");
        }
    }

    void DescribeAi(char* out, size_t outSize) {
        size_t pos = 0;
        out[0] = 0;
        __try {
            uint8_t* const* it = *reinterpret_cast<uint8_t* const* const*>(0x006C06D4);
            uint8_t* const* end = *reinterpret_cast<uint8_t* const* const*>(0x006C06D8);
            uint32_t vts[16]{}, counts[16]{};
            int nvt = 0, total = 0;
            for (; it && it < end && total < 4096; ++it, ++total) {
                const uint32_t vt = *it ? *reinterpret_cast<const uint32_t*>(*it) : 0;
                int k = 0;
                while (k < nvt && vts[k] != vt) ++k;
                if (k == nvt && nvt < 16) vts[nvt++] = vt;
                if (k < 16) ++counts[k];
            }
            pos += static_cast<size_t>(std::snprintf(out, outSize, " ai=%d", total));
            for (int k = 0; k < nvt && pos + 64 < outSize; ++k) {
                const uint32_t ser = vts[k] ? reinterpret_cast<const uint32_t*>(vts[k])[3] : 0;
                pos += static_cast<size_t>(std::snprintf(out + pos, outSize - pos, " vt=%06X x%u ser=%06X", vts[k], counts[k], ser));
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            std::snprintf(out + pos, outSize - pos, " (fault)");
        }
    }

    bool LooksLikePointer(uint32_t v) {
        if (v < 0x00100000 || v >= 0x80000000u) return false;
        MEMORY_BASIC_INFORMATION mbi{};
        return VirtualQuery(reinterpret_cast<const void*>(v), &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT;
    }

    // ---- per-tick state hash ----------------------------------------------------------------------------
    ElemInfo g_info[kMaxElements];

    void WriteTickHash(uint8_t* E) {
        const int n = CollectElements(E);
        uint64_t all = 0, humans = 0;
        int nHumans = 0, nBad = 0;
        for (int i = 0; i < n && i >= 0; ++i) {
            if (!ReadElement(g_el[i], g_info[i])) { g_info[i].h = 0; ++nBad; continue; }
            const uint64_t m = Mix(g_info[i].h);
            all += m;
            if (g_info[i].human) { humans += m; ++nHumans; }
        }
        const uint32_t* hr = Holdrand();
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        if (g_hashFile) {
            std::fprintf(g_hashFile, "%u %016llX %016llX %08X n=%d h=%d bad=%d rand", g_tick,
                         static_cast<unsigned long long>(all), static_cast<unsigned long long>(humans),
                         hr ? *hr : 0u, n, nHumans, nBad);
            for (int c = 0; c < C_COUNT; ++c) std::fprintf(g_hashFile, " %s=%llu", kCtxName[c], static_cast<unsigned long long>(g_randCount[c]));
            // global creation counters: elements [0x006C081C] (id at el+0xC), orders [0x006C0C6C] (RHOrder+0x34) -
            // an object made in one run only shows here first ([0x006C1034], read by 0x005866A0, is a pointer)
            const uint32_t ctr[2] = { *reinterpret_cast<const uint32_t*>(0x006C081C), *reinterpret_cast<const uint32_t*>(0x006C0C6C) };
            std::fprintf(g_hashFile, " soundcode=%llu clock.logic=%llu ev=%u ctr=%X/%X\n", static_cast<unsigned long long>(g_randSoundCode),
                         static_cast<unsigned long long>(g_clockCount[C_LOGIC]), g_seq, ctr[0], ctr[1]);
        }
        // replay: does this tick's state match the recording's? (a mismatch is dumped in full)
        bool mismatch = false;
        if (g_mode == M_PLAY && g_tick < g_refHash.size() && !g_refHash[g_tick].empty()) {
            char mine[48];
            std::snprintf(mine, sizeof(mine), "%016llX %016llX", static_cast<unsigned long long>(all), static_cast<unsigned long long>(humans));
            mismatch = g_refHash[g_tick] != mine;
            if (mismatch) ++g_refMismatch;
        }
        const bool dump = g_dumpFile && (g_dumpNext || (g_dumpEvery && g_tick % g_dumpEvery == 0) || (mismatch && g_refMismatch <= 40));
        if (dump) {
            g_dumpNext = false;
            std::fprintf(g_dumpFile, "== tick %u elements %d\n", g_tick, n);
            static char hex[1024];
            for (int i = 0; i < n; ++i) {
                const ElemInfo& o = g_info[i];
                hex[0] = 0;
                if (o.human) ReadHumanHex(g_el[i], hex, sizeof(hex));
                std::fprintf(g_dumpFile, "%u id=%u type=%X act=%u pos=%.3f,%.3f,%.3f face=%u anim=%u/%u/%u h=%016llX %s\n",
                             g_tick, o.id, o.type, o.active, o.pos[0], o.pos[1], o.pos[2], o.facing, o.anim, o.frame, o.sub,
                             static_cast<unsigned long long>(o.h), hex);
            }
            std::fflush(g_dumpFile);
        }
        if (g_hashFile && (g_tick % 25 == 0)) std::fflush(g_hashFile);
    }

    // ---- the event log ----------------------------------------------------------------------------------
    void Hex(std::string& s, const uint8_t* p, size_t n) {
        static const char kHex[] = "0123456789ABCDEF";
        for (size_t i = 0; i < n; ++i) { s += kHex[p[i] >> 4]; s += kHex[p[i] & 15]; }
    }
    bool Unhex(const char* s, uint8_t* out, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            auto nib = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                return -1;
            };
            const int hi = nib(s[2 * i]), lo = nib(s[2 * i + 1]);
            if (hi < 0 || lo < 0) return false;
            out[i] = static_cast<uint8_t>(hi * 16 + lo);
        }
        return true;
    }
    uint32_t Bits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
    float FromBits(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }

    void WriteEvent(const LoggedEvent& le) {
        std::string s;
        char t[96];
        std::snprintf(t, sizeof(t), "E %u %u ev=", le.tick, le.seq);
        s += t;
        Hex(s, le.ev, sizeof(le.ev));
        s += " trA=";
        Hex(s, le.trA, sizeof(le.trA));
        s += " trB=";
        Hex(s, le.trB, sizeof(le.trB));
        std::snprintf(t, sizeof(t), " cam=%08X,%08X zoom=%08X arg=%d foreign=%d world=%u,%08X,%08X",
                      Bits(le.cam[0]), Bits(le.cam[1]), Bits(le.zoom), le.argId, le.argForeign ? 1 : 0,
                      le.nWorld, Bits(le.world[0]), Bits(le.world[1]));
        s += t;
        s += " swing=";
        for (size_t i = 0; i < le.swings.size(); ++i) { std::snprintf(t, sizeof(t), "%s%u", i ? "," : "", le.swings[i]); s += t; }
        s += " sel=";
        for (size_t i = 0; i < le.sel.size(); ++i) { std::snprintf(t, sizeof(t), "%s%u", i ? "," : "", le.sel[i]); s += t; }
        // readable part (ignored by the reader)
        uint32_t cat = 0, param = 0, code = 0, arg = 0;
        std::memcpy(&cat, le.ev + 4, 4); std::memcpy(&param, le.ev + 8, 4); std::memcpy(&code, le.ev + 12, 4); std::memcpy(&arg, le.ev + 16, 4);
        uint16_t mode = 0; std::memcpy(&mode, le.trB + (0xA2 - TR_B), 2);
        std::snprintf(t, sizeof(t), " # cat=%u param=%X code=%u arg=%X mode=%u world=%.1f,%.1f", cat, param, code, arg, mode,
                      le.world[0], le.world[1]);
        s += t;
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        if (g_eventFile) {
            std::fprintf(g_eventFile, "%s\n", s.c_str());
            std::fflush(g_eventFile);
        }
    }

    bool ParseEvent(const char* line, LoggedEvent& le) {
        if (line[0] != 'E' || line[1] != ' ') return false;
        if (std::sscanf(line + 2, "%u %u", &le.tick, &le.seq) != 2) return false;
        const char* p = std::strstr(line, "ev=");
        if (!p || !Unhex(p + 3, le.ev, sizeof(le.ev))) return false;
        p = std::strstr(line, "trA=");
        if (!p || !Unhex(p + 4, le.trA, sizeof(le.trA))) return false;
        p = std::strstr(line, "trB=");
        if (!p || !Unhex(p + 4, le.trB, sizeof(le.trB))) return false;
        uint32_t c0 = 0, c1 = 0, z = 0, w0 = 0, w1 = 0;
        int foreign = 0;
        p = std::strstr(line, " cam=");
        if (!p || std::sscanf(p, " cam=%X,%X zoom=%X arg=%d foreign=%d world=%u,%X,%X", &c0, &c1, &z, &le.argId, &foreign,
                              &le.nWorld, &w0, &w1) != 8) {
            return false;
        }
        le.cam[0] = FromBits(c0); le.cam[1] = FromBits(c1); le.zoom = FromBits(z);
        le.world[0] = FromBits(w0); le.world[1] = FromBits(w1);
        le.argForeign = foreign != 0;
        auto list = [](const char* at, std::vector<uint32_t>& out) {
            if (!at) return;
            at = std::strchr(at, '=') + 1;
            while (*at && *at != ' ') {
                char* end = nullptr;
                const unsigned long v = std::strtoul(at, &end, 10);
                if (end == at) break;
                out.push_back(static_cast<uint32_t>(v));
                at = (*end == ',') ? end + 1 : end;
            }
        };
        list(std::strstr(line, " swing="), le.swings);
        list(std::strstr(line, " sel="), le.sel);
        return true;
    }

    // ---- sessions ---------------------------------------------------------------------------------------
    void CloseSession() {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        if (g_recording && g_eventFile) {
            // An event whose dispatch has not returned: it opened a modal loop (a dialog) inside the engine's
            // HandleEvent and the main thread is still in it (2026-11-19, recording s2: the release of a sword
            // stroke). Without this line the replay would lack exactly the event that stopped the recording.
            WriteEvent(*g_recording);
            std::fprintf(g_eventFile, "# seq %u: its dispatch had not returned when the session stopped (a modal loop?)\n",
                         g_recording->seq);
        }
        for (FILE** f : { &g_hashFile, &g_dumpFile, &g_eventFile, &g_randFile, &g_orderFile }) {
            if (*f) { std::fclose(*f); *f = nullptr; }
        }
        g_mode = M_OFF;
        g_play.clear();
        g_playIndex = 0;
    }

    void StartSession(uint8_t* E) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        const bool play = g_mode == M_ARM_PLAY;
        g_tick = 0;
        g_seq = 0;
        g_dropped = g_replayed = g_replaySkipped = g_selMismatch = g_swingMismatch = 0;
        g_renderEvents = g_logicEvents = 0;
        g_srandCalls = g_srandPinned = 0;
        g_loopPasses = 0;
        g_lastTickResult = -1;
        std::memset(g_randCount, 0, sizeof(g_randCount));
        g_randSoundCode = 0;
        std::memset(g_clockCount, 0, sizeof(g_clockCount));
        std::memset(g_hist, 0, sizeof(g_hist));
        if (uint32_t* hr = Holdrand()) *hr = g_seed;        // = srand(seed): the same generator state in every run
        g_cosmetic = g_seed ^ 0xA5A5A5A5u;
        const std::string base = "coop_" + g_name + "_";
        g_hashFile = OpenOut(base + (play ? g_tag : std::string("rec")) + "_hash.txt", "w");
        g_dumpFile = OpenOut(base + (play ? g_tag : std::string("rec")) + "_dump.txt", "w");
        g_randFile = OpenOut(base + (play ? g_tag : std::string("rec")) + "_rand.txt", "w");
        g_orderFile = OpenOut(base + (play ? g_tag : std::string("rec")) + "_orders.txt", "w");
        if (play) {
            g_play.clear();
            if (FILE* f = OpenOut(base + "events.txt", "r")) {
                static char line[4096];
                while (std::fgets(line, sizeof(line), f)) {
                    LoggedEvent le;
                    if (ParseEvent(line, le)) g_play.push_back(std::move(le));
                }
                std::fclose(f);
            }
            g_playIndex = 0;
            g_refHash.clear();
            g_refMismatch = 0;
            if (FILE* f = OpenOut(base + "rec_hash.txt", "r")) {
                static char line[512];
                while (std::fgets(line, sizeof(line), f)) {
                    unsigned t = 0;
                    char a[24]{}, h[24]{};
                    if (line[0] == '#' || std::sscanf(line, "%u %23s %23s", &t, a, h) != 3 || t > 200000) continue;
                    if (g_refHash.size() <= t) g_refHash.resize(t + 1);
                    g_refHash[t] = std::string(a) + " " + h;
                }
                std::fclose(f);
            }
            g_mode = M_PLAY;
        } else {
            g_eventFile = OpenOut(base + "events.txt", "w");
            g_mode = M_REC;
        }
        Logger::Warn("COOP PROBE: %s session '%s' started at engine tick 0 (seed %08X, isolate %X, engine %p)%s.",
                     play ? "replay" : "record", g_name.c_str(), g_seed, g_isolateMask, static_cast<void*>(E),
                     play ? (" - " + std::to_string(g_play.size()) + " events to replay").c_str() : "");
        if (g_hashFile) {
            std::fprintf(g_hashFile, "# coop %s '%s' seed=%08X isolate=%X pinSrand=%d events=%u\n", play ? "play" : "rec",
                         g_name.c_str(), g_seed, g_isolateMask, g_pinSrand ? 1 : 0, static_cast<unsigned>(g_play.size()));
        }
    }

    // ---- hooks ------------------------------------------------------------------------------------------
    using PFN_Listener = uint32_t(__fastcall*)(uint8_t* self, void* edx, uint8_t* ev);
    PFN_Listener o_Listener = nullptr;

    void ReplayOne(uint8_t* E, const LoggedEvent& le) {
        uint8_t* tr = *reinterpret_cast<uint8_t**>(E + E_Translator);
        if (!tr) { ++g_replaySkipped; return; }
        alignas(8) uint8_t ev[0x20]{};
        std::memcpy(ev, le.ev, sizeof(le.ev));
        uint32_t cat = 0;
        std::memcpy(&cat, ev + 4, 4);
        if (le.argForeign) { ++g_replaySkipped; Trace("REPLAY skip seq=%u: argument was a pointer to no element", le.seq); return; }
        if (cat == 2 && le.argId >= 0) {
            uint8_t* el = FindElement(E, static_cast<uint32_t>(le.argId));
            if (!el) { ++g_replaySkipped; Trace("REPLAY skip seq=%u: element %d gone", le.seq, le.argId); return; }
            std::memcpy(ev + 0x10, &el, 4);
        }
        uint32_t sel[16];
        const int nSel = ReadSelection(E, sel, 16);
        bool same = nSel == static_cast<int>(le.sel.size());
        for (int i = 0; same && i < nSel; ++i) same = sel[i] == le.sel[i];
        if (!same) {
            if (g_selMismatch++ < 20) Trace("REPLAY seq=%u: selection differs (%d now, %u logged)", le.seq, nSel, static_cast<unsigned>(le.sel.size()));
        }
        std::memcpy(tr + TR_A, le.trA, TR_A_LEN);
        std::memcpy(tr + TR_B, le.trB, TR_B_LEN);
        std::memcpy(E + E_Camera, g_camFixed ? g_camFix : le.cam, 8);
        if (g_traceFile) {
            uint32_t w[4];
            std::memcpy(w, ev + 4, 16);
            Trace("REPLAY seq=%u logged-tick=%u cat=%u param=%X code=%u arg=%X world=%u:%.1f,%.1f swings=%u", le.seq, le.tick, w[0], w[1], w[2],
                  w[3], le.nWorld, le.world[0], le.world[1], static_cast<unsigned>(le.swings.size()));
        }
        g_replaying = &le;
        g_swingIndex = 0;
        ++g_listenerDepth;
        o_Listener(E + E_Listener, nullptr, ev);
        --g_listenerDepth;
        g_replaying = nullptr;
        if (g_swingIndex != le.swings.size()) ++g_swingMismatch;
        ++g_replayed;
        ++g_seq;
    }

    // The event loop of RHGame::Run runs before the engine tick: events of tick T go in right before it.
    void DispatchDue(uint8_t* E) {
        while (g_mode == M_PLAY && g_playIndex < g_play.size() && g_play[g_playIndex].tick <= g_tick) {
            ReplayOne(E, g_play[g_playIndex]);
            ++g_playIndex;
        }
    }

    uint32_t __fastcall Hook_Listener(uint8_t* self, void* edx, uint8_t* ev) {
        const int ctx = CurrentCtx();
        const bool fromInput = OnMain() && (ctx == C_INPUT || ctx == C_NONE) && g_listenerDepth == 0 && !g_replaying;
        uint32_t w[4];
        ReadWords(ev + 4, w, 4);
        const uint32_t cat = w[0], param = w[1], code = w[2], arg = w[3];
        if (OnMain() && g_mode >= M_REC) {
            if (ctx == C_RENDER || ctx == C_EXTRA) ++g_renderEvents;
            if (ctx == C_LOGIC) ++g_logicEvents;
        }
        if (g_traceFile && OnMain() && !(cat == 1 && code == 1)) {
            Trace("ENGINE-EV ctx=%s depth=%u cat=%u param=%X code=%u arg=%X%s chain=%s", kCtxName[ctx], g_listenerDepth, cat, param,
                  code, arg, g_replaying ? " (replay)" : "", ChainText(_AddressOfReturnAddress()).c_str());
        }
        if (fromInput && g_mode == M_PLAY) {
            ++g_dropped;          // live input is not part of the replay
            return 0;
        }
        if (!(fromInput && g_mode == M_REC)) {
            ++g_listenerDepth;
            const uint32_t r = o_Listener(self, edx, ev);
            --g_listenerDepth;
            return r;
        }
        // record
        uint8_t* E = self - E_Listener;
        LoggedEvent le;
        le.tick = g_tick;
        le.seq = g_seq++;
        CaptureForLog(E, ev, le.ev, sizeof(le.ev), le.trA, le.trB, le.cam, &le.zoom);
        if (cat == 2 && LooksLikePointer(arg)) {
            le.argId = ElementId(E, arg);
            le.argForeign = le.argId < 0;
        }
        uint32_t sel[16];
        const int nSel = ReadSelection(E, sel, 16);
        for (int i = 0; i < nSel; ++i) le.sel.push_back(sel[i]);
        g_recording = &le;
        ++g_listenerDepth;
        const uint32_t r = o_Listener(self, edx, ev);
        --g_listenerDepth;
        g_recording = nullptr;
        WriteEvent(le);
        return r;
    }

    using PFN_Translator = uint32_t(__fastcall*)(uint8_t* tr, void* edx, uint8_t* ev);
    PFN_Translator o_Translator = nullptr;

    uint32_t __fastcall Hook_Translator(uint8_t* tr, void* edx, uint8_t* ev) {
        if (!OnMain()) return o_Translator(tr, edx, ev);
        const int prev = g_ctx;
        const bool top = g_ctx == C_NONE && !Timing::InRenderFrame();
        if (top) g_ctx = C_INPUT;
        if (g_traceFile && top) {
            uint32_t w[3];
            ReadWords(ev + 4, w, 3);
            if (!(w[0] == 1 && w[2] == 1)) Trace("INPUT-EV cat=%u param=%X code=%u mode=%u", w[0], w[1], w[2], *reinterpret_cast<const uint16_t*>(tr + 0xA2));
        }
        const uint32_t r = o_Translator(tr, edx, ev);
        g_ctx = prev;
        return r;
    }

    using PFN_EngineTick = int(__fastcall*)(uint8_t* E, void* edx);
    PFN_EngineTick o_EngineTick = nullptr;

    int __fastcall Hook_EngineTick(uint8_t* E, void* edx) {
        if (!g_mainThread) g_mainThread = GetCurrentThreadId();
        if (g_mode == M_ARM_REC || g_mode == M_ARM_PLAY) StartSession(E);
        if (g_mode == M_PLAY && g_camFixed) std::memcpy(E + E_Camera, g_camFix, 8);   // another player's view
        if (g_mode == M_PLAY) DispatchDue(E);
        const int prev = g_ctx;
        g_ctx = C_LOGIC;
        const int r = o_EngineTick(E, edx);
        g_ctx = prev;
        g_tickedThisLoop = true;
        if (g_mode == M_REC || g_mode == M_PLAY) {
            if (r != g_lastTickResult) {
                // the game state RHGame::Run keeps in [pGame+0x3550]; 4 and 7 stop the engine ticks
                std::lock_guard<std::recursive_mutex> lock(g_lock);
                if (g_hashFile) std::fprintf(g_hashFile, "# tick %u: RHEngine::Tick returned %d (was %d)\n", g_tick, r, g_lastTickResult);
            }
            g_lastTickResult = r;
            WriteTickHash(E);
            ++g_tick;
        }
        return r;
    }

    using PFN_SoundUpdate = void(__fastcall*)(void* snd, void* edx);
    PFN_SoundUpdate o_SoundUpdate = nullptr;
    void __fastcall Hook_SoundUpdate(void* snd, void* edx) {
        if (!OnMain()) { o_SoundUpdate(snd, edx); return; }
        const int prev = g_ctx;
        g_ctx = C_SOUND;
        o_SoundUpdate(snd, edx);
        g_ctx = prev;
    }

    using PFN_rand = int(__cdecl*)();
    PFN_rand o_rand = nullptr;
    int __cdecl Hook_rand() {
        const uint32_t ret = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress()));
        if (!OnMain()) {
            g_randOtherThread.fetch_add(1, std::memory_order_relaxed);
            return o_rand();
        }
        const int c = CurrentCtx();
        ++g_randCount[c];
        HistAdd(ret, c);
        const bool soundCode = ret >= kSoundCodeBegin && ret < kSoundCodeEnd;
        if (soundCode) ++g_randSoundCode;
        const bool isolated = (g_isolateMask & (1u << c)) || (soundCode && (g_isolateMask & kIsolateSoundBit));
        int value;
        if (isolated) {
            g_cosmetic = g_cosmetic * 0x343FDu + 0x269EC3u;
            value = static_cast<int>((g_cosmetic >> 16) & 0x7FFF);
        } else {
            value = o_rand();
        }
        if (g_randFile) {
            // one line per call: two runs part at the first line that differs (caller = where the branch went)
            std::fprintf(g_randFile, "%u %c %06X %d\n", g_tick, isolated ? kCtxName[c][0] - 32 : kCtxName[c][0], ret, value);
        }
        return value;
    }

    using PFN_srand = void(__cdecl*)(unsigned);
    PFN_srand o_srand = nullptr;
    void __cdecl Hook_srand(unsigned seed) {
        const uint32_t ret = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress()));
        const bool session = OnMain() && (g_mode == M_REC || g_mode == M_PLAY);
        if (session) ++g_srandCalls;
        unsigned use = seed;
        if (session && g_pinSrand) {
            use = g_seed ^ (g_tick * 0x9E3779B9u);
            ++g_srandPinned;
        }
        Logger::Info("COOP PROBE: srand(%u) from %06X%s (session tick %u).", seed, ret, use != seed ? " - pinned for the session" : "", g_tick);
        Trace("SRAND %u from %06X -> %u", seed, ret, use);
        o_srand(use);
    }

    // RHOrder constructors (0x0054E510 thiscall(this), 0x0054E5E0 thiscall(this, src), ret 4): each takes the next
    // number from [0x006C0C6C] - one order made in a single run only moves every later number
    void LogOrderMade(char kind, const void* from) {
        if (!g_orderFile || !OnMain() || !(g_mode == M_REC || g_mode == M_PLAY)) return;
        uint32_t c[4];
        const int n = ScanCallers(from, c, 4);
        std::fprintf(g_orderFile, "%u %c %c", g_tick, kCtxName[CurrentCtx()][0], kind);
        for (int i = 0; i < n; ++i) std::fprintf(g_orderFile, " %06X", c[i]);
        std::fputc('\n', g_orderFile);
    }
    using PFN_OrderCtor = void*(__fastcall*)(void* self, void* edx);
    using PFN_OrderCopy = void*(__fastcall*)(void* self, void* edx, void* src);
    PFN_OrderCtor o_OrderCtor = nullptr;
    PFN_OrderCopy o_OrderCopy = nullptr;
    void* __fastcall Hook_OrderCtor(void* self, void* edx) {
        LogOrderMade('n', _AddressOfReturnAddress());
        return o_OrderCtor(self, edx);
    }
    void* __fastcall Hook_OrderCopy(void* self, void* edx, void* src) {
        LogOrderMade('c', _AddressOfReturnAddress());
        return o_OrderCopy(self, edx, src);
    }

    // RHSequence::AddOrder 0x00585970 - trace only: who gives which order
    using PFN_AddOrder = void(__fastcall*)(uint8_t* seq, void* edx, uint8_t* order);
    PFN_AddOrder o_AddOrder = nullptr;
    void __fastcall Hook_AddOrder(uint8_t* seq, void* edx, uint8_t* order) {
        if (g_traceFile && OnMain() && order) {
            uint32_t w[13];       // +0x04 .. +0x34 of the RHOrder
            ReadWords(order + 4, w, 13);
            float v[3];
            std::memcpy(v, &w[1], 12);   // +0x08 vec3
            Trace("ORDER ctx=%s seq=%p type=0x%X f4=%X v=%.1f,%.1f,%.1f flags=%08X,%04X target=%X id=%u chain=%s", kCtxName[CurrentCtx()],
                  static_cast<void*>(seq), w[8], w[0], v[0], v[1], v[2], w[9], w[10] & 0xFFFF, w[11], w[12],
                  ChainText(_AddressOfReturnAddress()).c_str());
        }
        o_AddOrder(seq, edx, order);
    }

    // sequence manager post 0x0058A940 (thiscall(mgr, a, b, element), ret 0xC) - trace only
    using PFN_SeqPost = uint32_t(__fastcall*)(void* mgr, void* edx, uint32_t a, uint32_t b, uint8_t* element);
    PFN_SeqPost o_SeqPost = nullptr;
    uint32_t __fastcall Hook_SeqPost(void* mgr, void* edx, uint32_t a, uint32_t b, uint8_t* element) {
        if (g_traceFile && OnMain()) {
            uint32_t w[8]{};
            if (element) ReadWords(element, w, 8);
            Trace("SEQPOST ctx=%s mgr=%p a=%X b=%X el=%p [%X %X %X %X %X %X %X %X] chain=%s", kCtxName[CurrentCtx()], mgr, a, b,
                  static_cast<void*>(element), w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], ChainText(_AddressOfReturnAddress()).c_str());
        }
        return o_SeqPost(mgr, edx, a, b, element);
    }

    // RHEngine::OnWorldClick 0x004DE290 and the cursor-mode handlers it dispatches to - trace only
    using PFN_Void = void(__fastcall*)(uint8_t* E, void* edx);
    PFN_Void o_WorldClick = nullptr;
    void __fastcall Hook_WorldClick(uint8_t* E, void* edx) {
        if (g_traceFile && OnMain()) {
            uint8_t* tr = *reinterpret_cast<uint8_t**>(E + E_Translator);
            Trace("WORLDCLICK mode=%u flag=%u%s", tr ? *reinterpret_cast<const uint16_t*>(tr + 0xA2) : 0xFFFF, tr ? tr[0x20] : 0,
                  g_replaying ? " (replay)" : "");
        }
        o_WorldClick(E, edx);
    }

    struct ModeHandler { uintptr_t addr; int nargs; const char* name; void* orig; };
    ModeHandler g_modes[] = {
        { 0x004CCAD0, 2, "Click.default", nullptr }, { 0x004DF070, 2, "Click.mode1", nullptr },
        { 0x004E0550, 3, "Click.mode2/3", nullptr }, { 0x004DF950, 2, "Click.mode4", nullptr },
        { 0x004DFB50, 2, "Click.mode5", nullptr },   { 0x004E0FB0, 4, "Click.mode6/7", nullptr },
        { 0x004DF330, 3, "Click.mode8", nullptr },   { 0x004E0770, 3, "Click.mode9", nullptr },
        { 0x004E0D80, 3, "Click.mode10", nullptr },  { 0x004DF7B0, 2, "Click.mode11", nullptr },
        { 0x004E0030, 3, "Click.mode12", nullptr },  { 0x004E14A0, 3, "Click.mode13/14", nullptr },
        { 0x004E0BF0, 3, "Click.mode15", nullptr },  { 0x004DF550, 4, "Click.mode16", nullptr },
        { 0x004DFE70, 2, "Click.mode17", nullptr },  { 0x004E09D0, 3, "Click.mode18", nullptr },
        { 0x004DFCF0, 2, "Click.mode19", nullptr },  { 0x004E1630, 2, "Click.mode20", nullptr },
        { 0x004E1A40, 2, "ApplySwordGesture", nullptr },
    };

    template <int I>
    uint32_t __fastcall ModeHook2(uint8_t* E, void* edx, const float* pos, uint32_t a2) {
        if (g_traceFile && OnMain()) Trace("%s pos=%.1f,%.1f a2=%X", g_modes[I].name, pos ? pos[0] : 0.0f, pos ? pos[1] : 0.0f, a2);
        const uint32_t r = reinterpret_cast<uint32_t(__fastcall*)(uint8_t*, void*, const float*, uint32_t)>(g_modes[I].orig)(E, edx, pos, a2);
        if (g_traceFile && OnMain()) Trace("%s -> %X", g_modes[I].name, r);
        return r;
    }
    template <int I>
    uint32_t __fastcall ModeHook3(uint8_t* E, void* edx, const float* pos, uint32_t a2, uint32_t a3) {
        if (g_traceFile && OnMain()) Trace("%s pos=%.1f,%.1f a2=%X a3=%X", g_modes[I].name, pos ? pos[0] : 0.0f, pos ? pos[1] : 0.0f, a2, a3);
        const uint32_t r = reinterpret_cast<uint32_t(__fastcall*)(uint8_t*, void*, const float*, uint32_t, uint32_t)>(g_modes[I].orig)(E, edx, pos, a2, a3);
        if (g_traceFile && OnMain()) Trace("%s -> %X", g_modes[I].name, r);
        return r;
    }
    template <int I>
    uint32_t __fastcall ModeHook4(uint8_t* E, void* edx, const float* pos, uint32_t a2, uint32_t a3, uint32_t a4) {
        if (g_traceFile && OnMain()) Trace("%s pos=%.1f,%.1f a2=%X a3=%X a4=%X", g_modes[I].name, pos ? pos[0] : 0.0f, pos ? pos[1] : 0.0f, a2, a3, a4);
        const uint32_t r = reinterpret_cast<uint32_t(__fastcall*)(uint8_t*, void*, const float*, uint32_t, uint32_t, uint32_t)>(g_modes[I].orig)(E, edx, pos, a2, a3, a4);
        if (g_traceFile && OnMain()) Trace("%s -> %X", g_modes[I].name, r);
        return r;
    }

    template <int I>
    void* ModeDetour() {
        switch (g_modes[I].nargs) {
            case 2: return reinterpret_cast<void*>(&ModeHook2<I>);
            case 3: return reinterpret_cast<void*>(&ModeHook3<I>);
            default: return reinterpret_cast<void*>(&ModeHook4<I>);
        }
    }
    template <int... Is>
    void FillModeDetours(void** out, std::integer_sequence<int, Is...>) {
        ((out[Is] = ModeDetour<Is>()), ...);
    }

    // clock reads of the game (IAT), counted per context
    using PFN_Clock = DWORD(WINAPI*)();
    PFN_Clock o_GetTickCount = nullptr, o_timeGetTime = nullptr;
    uint32_t g_clockCallersLogged = 0;
    void CountClock(uint32_t ret) {
        if (!OnMain()) return;
        const int c = CurrentCtx();
        ++g_clockCount[c];
        if (c == C_LOGIC && g_clockCallersLogged < 32) {
            ++g_clockCallersLogged;
            Logger::Info("COOP PROBE: the engine tick read the clock at %06X.", ret);
        }
    }
    DWORD WINAPI Iat_GetTickCount() {
        CountClock(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress())));
        return o_GetTickCount();
    }
    DWORD WINAPI Iat_timeGetTime() {
        CountClock(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress())));
        return o_timeGetTime();
    }

    // ScreenToWorld / swing recognition when no other module hooked them (no HD view, no interface hooks)
    using PFN_ToWorld = void(__fastcall*)(uint8_t* e, void* edx, float* out);
    PFN_ToWorld o_ToWorld = nullptr;
    void __fastcall Hook_ToWorld(uint8_t* e, void* edx, float* out) {
        o_ToWorld(e, edx, out);
        OnScreenToWorld(out);
    }
    using PFN_Swing = uint32_t(__fastcall*)(void* mgr, void* edx, void* a, void* b);
    PFN_Swing o_Swing = nullptr;
    uint32_t __fastcall Hook_Swing(void* mgr, void* edx, void* a, void* b) {
        return OnSwingRecognised(o_Swing(mgr, edx, a, b));
    }

    bool PatchIat(uintptr_t slot, void* replacement, void** original) {
        auto** p = reinterpret_cast<void**>(slot);
        DWORD old = 0;
        if (!*p || !VirtualProtect(p, sizeof(void*), PAGE_READWRITE, &old)) return false;
        *original = *p;
        *p = replacement;
        VirtualProtect(p, sizeof(void*), old, &old);
        return true;
    }

    bool HookAt(uintptr_t addr, const char* hexProlog, void* detour, void** original, const char* name) {
        uint8_t want[32];
        const size_t n = std::strlen(hexProlog) / 2;
        Unhex(hexProlog, want, n);
        if (std::memcmp(reinterpret_cast<const void*>(addr), want, n) != 0) {
            Logger::Warn("COOP PROBE: %s @ %08X does not match - not hooked.", name, static_cast<unsigned>(addr));
            return false;
        }
        if (MH_CreateHook(reinterpret_cast<LPVOID>(addr), detour, original) != MH_OK ||
            MH_EnableHook(reinterpret_cast<LPVOID>(addr)) != MH_OK) {
            Logger::Warn("COOP PROBE: %s @ %08X could not be hooked.", name, static_cast<unsigned>(addr));
            return false;
        }
        return true;
    }

    void WriteRandDump(const std::string& name) {
        struct Row { uint32_t ret; int ctx; uint32_t count; };
        std::vector<Row> rows;
        for (const Slot& s : g_hist) {
            if (s.key) rows.push_back({ s.key & 0x00FFFFFFu, static_cast<int>(s.key >> 24), s.count });
        }
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.ctx != b.ctx ? a.ctx < b.ctx : a.count > b.count; });
        if (FILE* f = OpenOut("coop_rand_" + name + ".txt", "w")) {
            std::fprintf(f, "# rand() calls on the main thread since the session start, by context and return address\n");
            for (int c = 0; c < C_COUNT; ++c) std::fprintf(f, "# %s=%llu\n", kCtxName[c], static_cast<unsigned long long>(g_randCount[c]));
            std::fprintf(f, "# other threads=%llu\n", static_cast<unsigned long long>(g_randOtherThread.load()));
            for (const Row& r : rows) std::fprintf(f, "%-6s %06X %u\n", kCtxName[r.ctx], r.ret, r.count);
            std::fclose(f);
        }
    }
} // namespace

// ---- public -------------------------------------------------------------------------------------------
void OnLoopEnd(void* pGame) {
    if (!g_installed || !OnMain() || !(g_mode == M_REC || g_mode == M_PLAY)) return;
    ++g_loopPasses;
    // RHGame::Run skips RHEngine::Tick (0x0050FEB4..0x0050FEE1) while the translator pauses ([tr+0x1B]), a
    // window object asks for it ([[pGame+0x29A8]+0xC8]) or the game state [pGame+0x3550] is 4 or 7
    static uint32_t s_stalled = 0;
    if (g_tickedThisLoop) {
        if (s_stalled) {
            std::lock_guard<std::recursive_mutex> lock(g_lock);
            if (g_hashFile) std::fprintf(g_hashFile, "# tick %u: engine ticks again after %u loop passes without\n", g_tick, s_stalled);
        }
        s_stalled = 0;
        g_tickedThisLoop = false;
        return;
    }
    if (s_stalled++ != 0 || !pGame) return;
    uint32_t pause = 0xFF, window = 0xFF, state = 0xFFFFFFFF;
    uint32_t w[1];
    const auto* g = static_cast<const uint8_t*>(pGame);
    ReadWords(g + 0x3550, &state, 1);
    uint8_t* E = *reinterpret_cast<uint8_t**>(kEnginePtr);
    if (E) {
        ReadWords(E + E_Translator, w, 1);
        if (w[0]) { uint32_t b[8]; ReadWords(reinterpret_cast<const uint8_t*>(w[0]) + 0x18, b, 1); pause = (b[0] >> 24) & 0xFF; }
    }
    ReadWords(g + 0x29A8, w, 1);
    if (w[0]) { uint32_t b[1]; ReadWords(reinterpret_cast<const uint8_t*>(w[0]) + 0xC8, b, 1); window = b[0] & 0xFF; }
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_hashFile) {
        std::fprintf(g_hashFile, "# tick %u: no engine tick in this loop pass - translator pause %u, window %u, state %u\n", g_tick,
                     pause, window, state);
        std::fflush(g_hashFile);
    }
}

void OnScreenToWorld(float* out) {
    if (!g_installed || !out || !OnMain()) return;
    if (g_recording) {
        g_recording->world[0] = out[0];
        g_recording->world[1] = out[1];
        ++g_recording->nWorld;
    } else if (g_replaying && g_replaying->nWorld > 0) {
        out[0] = g_replaying->world[0];
        out[1] = g_replaying->world[1];
    }
}

uint32_t OnSwingRecognised(uint32_t result) {
    if (!g_installed || !OnMain()) return result;
    if (g_recording) {
        g_recording->swings.push_back(result);
    } else if (g_replaying) {
        if (g_swingIndex < g_replaying->swings.size()) return g_replaying->swings[g_swingIndex++];
        ++g_swingIndex;
    }
    if (g_traceFile) Trace("SWING result %u", result);
    return result;
}

void Install() {
    if (!TestHarness::Enabled()) return;   // the player's game never gets these hooks
    char exe[MAX_PATH]{};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    g_dir = exe;
    g_dir.erase(g_dir.find_last_of("\\/") + 1);
    g_dir += "tools\\out\\harness\\";

    bool ok = true;
    ok &= HookAt(0x004C6EF0, "6AFF6810C4650064A100000000", reinterpret_cast<void*>(&Hook_EngineTick), reinterpret_cast<void**>(&o_EngineTick), "RHEngine::Tick");
    ok &= HookAt(0x004DC2C0, "64A1000000006AFF6863E46500", reinterpret_cast<void*>(&Hook_Listener), reinterpret_cast<void**>(&o_Listener), "RHEngine listener");
    ok &= HookAt(0x005460C0, "6AFF68D87F660064A100000000", reinterpret_cast<void*>(&Hook_Translator), reinterpret_cast<void**>(&o_Translator), "Translator::HandleEvent");
    ok &= HookAt(0x00642A7D, "E8B12E00008B481469C9FD430300", reinterpret_cast<void*>(&Hook_rand), reinterpret_cast<void**>(&o_rand), "rand");
    ok &= HookAt(0x00642A70, "E8BE2E00008B4C2404894814C3", reinterpret_cast<void*>(&Hook_srand), reinterpret_cast<void**>(&o_srand), "srand");
    HookAt(0x005A9420, "83EC48568BF18A860B020000", reinterpret_cast<void*>(&Hook_SoundUpdate), reinterpret_cast<void**>(&o_SoundUpdate), "RHSound::Update");
    HookAt(0x00585970, "53558B6C240C8BD9817D2423010000", reinterpret_cast<void*>(&Hook_AddOrder), reinterpret_cast<void**>(&o_AddOrder), "RHSequence::AddOrder");
    HookAt(0x0054E510, "6AFF68EB87660064A100000000", reinterpret_cast<void*>(&Hook_OrderCtor), reinterpret_cast<void**>(&o_OrderCtor), "RHOrder ctor");
    HookAt(0x0054E5E0, "6AFF6816886600" "64A100000000", reinterpret_cast<void*>(&Hook_OrderCopy), reinterpret_cast<void**>(&o_OrderCopy), "RHOrder copy");
    HookAt(0x0058A940, "6AFF680BC7660064A100000000", reinterpret_cast<void*>(&Hook_SeqPost), reinterpret_cast<void**>(&o_SeqPost), "sequence post");
    HookAt(0x004DE290, "6AFF6838E5650064A100000000", reinterpret_cast<void*>(&Hook_WorldClick), reinterpret_cast<void**>(&o_WorldClick), "RHEngine::OnWorldClick");

    constexpr int kModes = static_cast<int>(sizeof(g_modes) / sizeof(g_modes[0]));
    void* detours[kModes]{};
    FillModeDetours(detours, std::make_integer_sequence<int, kModes>{});
    int modesHooked = 0;
    for (int i = 0; i < kModes; ++i) {
        // the prologue differs per function: compare the first 6 bytes against the two SEH forms
        const auto* p = reinterpret_cast<const uint8_t*>(g_modes[i].addr);
        const bool sehA = p[0] == 0x6A && p[1] == 0xFF && p[2] == 0x68;
        const bool sehB = p[0] == 0x64 && p[1] == 0xA1 && p[6] == 0x6A && p[7] == 0xFF;
        if ((sehA || sehB) && MH_CreateHook(reinterpret_cast<LPVOID>(g_modes[i].addr), detours[i], &g_modes[i].orig) == MH_OK &&
            MH_EnableHook(reinterpret_cast<LPVOID>(g_modes[i].addr)) == MH_OK) {
            ++modesHooked;
        }
    }
    const bool clocks = PatchIat(kIatGetTickCount, reinterpret_cast<void*>(&Iat_GetTickCount), reinterpret_cast<void**>(&o_GetTickCount)) &&
                        PatchIat(kIatTimeGetTime, reinterpret_cast<void*>(&Iat_timeGetTime), reinterpret_cast<void**>(&o_timeGetTime));
    // ScreenToWorld 0x004DEE70 (view_zoom.cpp) and the swing recognition 0x00547600 (ui_scale.cpp) call the
    // hook points themselves when those modules hooked them (MinHook leaves a jmp, 0xE9); else hook them here.
    const char* toWorld = "view_zoom";
    if (*reinterpret_cast<const uint8_t*>(0x004DEE70) != 0xE9) {
        toWorld = HookAt(0x004DEE70, "D9059C746700D8B15C470000", reinterpret_cast<void*>(&Hook_ToWorld), reinterpret_cast<void**>(&o_ToWorld),
                         "ScreenToWorld") ? "probe" : "NONE";
    }
    const char* swing = "ui_scale";
    if (*reinterpret_cast<const uint8_t*>(0x00547600) != 0xE9) {
        swing = HookAt(0x00547600, "6AFF68EA80660064A100000000", reinterpret_cast<void*>(&Hook_Swing), reinterpret_cast<void**>(&o_Swing),
                       "swing recognition") ? "probe" : "NONE";
    }
    g_installed = ok;
    Logger::Warn("COOP PROBE (test mode only): %s; %d of %d click handlers traced, clock reads counted=%d, ScreenToWorld via %s, "
                 "swing via %s. Command: coop.",
                 ok ? "engine tick, engine listener, translator, rand/srand hooked" : "NOT complete - sessions unavailable",
                 modesHooked, kModes, clocks ? 1 : 0, toWorld, swing);
}

void Command(const char* args, char* out, size_t outSize) {
    char verb[32]{}, a1[128]{}, a2[64]{};
    std::sscanf(args, "%31s %127s %63s", verb, a1, a2);
    if (!g_installed) { std::snprintf(out, outSize, "err the probe is not installed (test mode only)"); return; }
    if (!std::strcmp(verb, "stat") || !verb[0]) {
        std::snprintf(out, outSize,
                      "mode=%d name=%s tick=%u seq=%u replayed=%u/%u dropped=%u skipped=%u selMismatch=%u swingMismatch=%u "
                      "rand: input=%llu logic=%llu render=%llu extra=%llu sound=%llu other=%llu thread=%llu srand=%u pinned=%u "
                      "clock: logic=%llu render=%llu other=%llu ev(render)=%u ev(logic)=%u isolate=%X refMismatch=%u loops=%u",
                      g_mode, g_name.c_str(), g_tick, g_seq, g_replayed, static_cast<unsigned>(g_play.size()), g_dropped, g_replaySkipped,
                      g_selMismatch, g_swingMismatch,
                      static_cast<unsigned long long>(g_randCount[C_INPUT]), static_cast<unsigned long long>(g_randCount[C_LOGIC]),
                      static_cast<unsigned long long>(g_randCount[C_RENDER]), static_cast<unsigned long long>(g_randCount[C_EXTRA]),
                      static_cast<unsigned long long>(g_randCount[C_SOUND]), static_cast<unsigned long long>(g_randCount[C_NONE]),
                      static_cast<unsigned long long>(g_randOtherThread.load()), g_srandCalls, g_srandPinned,
                      static_cast<unsigned long long>(g_clockCount[C_LOGIC]), static_cast<unsigned long long>(g_clockCount[C_RENDER]),
                      static_cast<unsigned long long>(g_clockCount[C_NONE] + g_clockCount[C_INPUT]), g_renderEvents, g_logicEvents,
                      g_isolateMask, g_refMismatch, g_loopPasses);
    } else if (!std::strcmp(verb, "rec") || !std::strcmp(verb, "play")) {
        if (!a1[0]) { std::snprintf(out, outSize, "err usage: coop rec NAME | coop play NAME [TAG]"); return; }
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        CloseSession();
        g_name = a1;
        g_tag = a2[0] ? a2 : "play";
        g_mode = verb[0] == 'r' ? M_ARM_REC : M_ARM_PLAY;
        std::snprintf(out, outSize, "ok armed %s '%s' - starts at the next engine tick", verb, a1);
    } else if (!std::strcmp(verb, "stop")) {
        CloseSession();
        std::snprintf(out, outSize, "ok stopped at tick %u (seq %u, replayed %u, dropped %u)", g_tick, g_seq, g_replayed, g_dropped);
    } else if (!std::strcmp(verb, "trace")) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        if (g_traceFile) { std::fclose(g_traceFile); g_traceFile = nullptr; }
        if (!std::strcmp(a1, "on") && a2[0]) {
            g_traceFile = OpenOut(std::string("coop_trace_") + a2 + ".txt", "w");
            std::snprintf(out, outSize, g_traceFile ? "ok tracing to coop_trace_%s.txt" : "err cannot open coop_trace_%s.txt", a2);
        } else {
            std::snprintf(out, outSize, "ok trace off");
        }
    } else if (!std::strcmp(verb, "isolate")) {
        g_isolateMask = static_cast<uint32_t>(std::strtoul(a1, nullptr, 0));
        std::snprintf(out, outSize, "ok isolate=%X", g_isolateMask);
    } else if (!std::strcmp(verb, "dumpevery")) {
        g_dumpEvery = static_cast<uint32_t>(std::strtoul(a1, nullptr, 0));
        std::snprintf(out, outSize, "ok element dump every %u ticks (0 = only at 'coop dump' and, in a replay, where the state differs from the recording)", g_dumpEvery);
    } else if (!std::strcmp(verb, "ai")) {
        // the AI objects of RHArtificialIntelligence ([0x006C06D4]..[0x006C06D8], serialized through vtbl[3]):
        // count and the distinct vtables with their vtbl[3]
        char text[900];
        DescribeAi(text, sizeof(text));
        std::snprintf(out, outSize, "ok%s", text);
    } else if (!std::strcmp(verb, "listeners")) {
        char text[900];
        DescribeListeners(text, sizeof(text));
        std::snprintf(out, outSize, "ok%s", text);
    } else if (!std::strcmp(verb, "camera")) {
        // replay with the camera held at X Y (world pixels) every tick, not where the recording had it
        float x = 0.0f, y = 0.0f;
        g_camFixed = std::sscanf(args, "%*s %f %f", &x, &y) == 2;
        g_camFix[0] = x;
        g_camFix[1] = y;
        std::snprintf(out, outSize, g_camFixed ? "ok replay camera held at %.0f,%.0f" : "ok replay camera as recorded", x, y);
    } else if (!std::strcmp(verb, "pinsrand")) {
        g_pinSrand = std::strcmp(a1, "off") != 0;
        std::snprintf(out, outSize, "ok pinsrand=%d", g_pinSrand ? 1 : 0);
    } else if (!std::strcmp(verb, "seed")) {
        g_seed = static_cast<uint32_t>(std::strtoul(a1, nullptr, 0));
        std::snprintf(out, outSize, "ok seed=%08X", g_seed);
    } else if (!std::strcmp(verb, "randdump")) {
        WriteRandDump(a1[0] ? a1 : "last");
        std::snprintf(out, outSize, "ok coop_rand_%s.txt", a1[0] ? a1 : "last");
    } else if (!std::strcmp(verb, "dump")) {
        g_dumpNext = true;
        std::snprintf(out, outSize, "ok dump at the next tick");
    } else {
        std::snprintf(out, outSize, "err unknown: coop stat|rec|play|stop|trace|isolate|pinsrand|seed|randdump|dump");
    }
}

} // namespace CoopProbe
