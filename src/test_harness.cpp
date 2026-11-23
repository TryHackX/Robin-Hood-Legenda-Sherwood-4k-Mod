#include "test_harness.h"
#include "coop_probe.h"
#include "diag.h"
#include "logger.h"
#include "settings_menu.h"
#include "ui_scale.h"
#include "view_zoom.h"
#include "window_manager.h"
#include "ddm/ddm.h"
#include "ddm/ddm_hd.h"
#include "ddm/ddm_hd_font.h"
#include "ddm/ddm_hd_menu.h"
#include "ddm/ddm_hd_patch.h"
#include "ddm/ddm_hd_sprites.h"
#include "ddm/ddm_present.h"
#include "proxy_winmm.h"

#include <MinHook.h>
#include <audiopolicy.h>
#include <intrin.h>
#include <mmdeviceapi.h>
#include <share.h>
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace TestHarness {
namespace {
    constexpr uintptr_t kAddrCursorPos   = 0x006C1300; // Vec2 float: the game's cursor, internal pixels
    constexpr uintptr_t kAddrAppActive   = 0x006A6D27;
    constexpr uintptr_t kAddrEnginePtr   = 0x006C0FA8;
    constexpr uintptr_t kIatSetOutput    = 0x00677330; // fmod.dll!_FSOUND_SetOutput@4
    constexpr uintptr_t kIatBinkSoundSys = 0x006772EC; // binkw32.dll!_BinkSetSoundSystem@8
    constexpr uintptr_t kIatSetCursorPos = 0x006772A8; // user32.dll!SetCursorPos (RHGame::Run, every loop pass)
    constexpr uintptr_t kIatShowCursor   = 0x0067727C; // user32.dll!ShowCursor

    bool g_enabled = false;
    bool g_checked = false;
    volatile uint32_t g_ambiance = 0;   // command "ambiance": the next level's ambiance (0 = as the level says)
    std::atomic<unsigned> g_desktopCallsBlocked{ 0 }; // pointer / injection / foreground calls swallowed (see IsolateDesktop)
    HANDLE g_marker = nullptr; // named event: "a test instance is running"
    std::string g_dir;         // ...\tools\out\harness\ (with trailing backslash)

    struct Queued {
        uint64_t dueQpc;
        InputRecord rec;
    };
    std::mutex g_lock;
    std::deque<Queued> g_inputs;
    uint64_t g_lastDue = 0; // records are released in order, each no earlier than the previous

    // engine call wanted on the main thread: 1 = zoom in, 2 = zoom out (reader thread -> game tick)
    std::atomic<int> g_mainCmd{ 0 };
    float g_zoneqPt[2]{};           // cmd 4 "zoneq": the point asked about, and the answer
    char  g_zoneqOut[1024]{};
    int   g_gridArgs[5]{};          // cmd 6 "pointergrid": x0 y0 x1 y1 step, and the file it writes
    std::string g_gridPath;
    // the last calls of the zone-under-a-point routine and of the cursor's choice (InstallZoneProbe)
    struct ZoneCall { uint32_t caller; float x, y; uint32_t zone, cc; uint16_t c4; uint8_t alt; uint32_t charZone; float cx, cy; };
    struct CursorCall { uint32_t caller; float x, y; uint32_t zoneIn; uint16_t id; };
    ZoneCall g_zoneCalls[8]{};
    CursorCall g_cursorCalls[8]{};
    std::atomic<unsigned> g_zoneCallN{ 0 }, g_cursorCallN{ 0 };

    // frame dump request (reader thread -> presenter -> reader thread)
    std::atomic<int> g_shotState{ 0 }; // 0 none, 1 requested, 2 done ok, 3 failed
    std::string g_shotPath;
    bool g_shotScaled = false;
    int  g_shotW = 0, g_shotH = 0;

    uint64_t QpcFromMs(double ms) {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return static_cast<uint64_t>(ms * static_cast<double>(f.QuadPart) / 1000.0);
    }

    // Queue one record `gapMs` after the previously queued one (or after now, if that is later).
    void Enqueue(uint32_t type, uint32_t ofs, uint32_t data, double gapMs) {
        std::lock_guard<std::mutex> lock(g_lock);
        const uint64_t now = Diag::Now();
        const uint64_t base = (g_lastDue > now) ? g_lastDue : now;
        g_lastDue = base + QpcFromMs(gapMs);
        g_inputs.push_back(Queued{ g_lastDue, InputRecord{ type, ofs, data } });
    }

    bool WaitQueueEmpty(DWORD timeoutMs) {
        const DWORD t0 = GetTickCount();
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(g_lock);
                if (g_inputs.empty()) return true;
            }
            if (GetTickCount() - t0 > timeoutMs) return false;
            Sleep(5);
        }
    }

    // Without an exception: after a mission the engine pointer stays set to freed memory, and an
    // access violation - even a caught one - reaches the crash logger first, which then wrote a
    // "FATAL" block for a harmless "stat" (2026-11-15) and has no report left for a real crash.
    bool ReadMem(uintptr_t addr, void* out, size_t n) {
        SIZE_T done = 0;
        return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), out, n, &done) && done == n;
    }

    bool CursorPos(float& x, float& y) {
        float v[2]{};
        if (!ReadMem(kAddrCursorPos, v, sizeof(v))) return false;
        x = v[0];
        y = v[1];
        return true;
    }

    void QueueMove(int dx, int dy) {
        // like a hand: steps of at most 12 pixels, one every 8 ms
        int steps = (std::max)(std::abs(dx), std::abs(dy)) / 12 + 1;
        int doneX = 0, doneY = 0;
        for (int i = 1; i <= steps; ++i) {
            const int tx = dx * i / steps, ty = dy * i / steps;
            if (tx != doneX) Enqueue(0, 0, static_cast<uint32_t>(tx - doneX), 0.0);
            if (ty != doneY) Enqueue(0, 4, static_cast<uint32_t>(ty - doneY), 0.0);
            doneX = tx;
            doneY = ty;
            // a zero-length pause record keeps the steps 8 ms apart
            Enqueue(0, 0, 0, 8.0);
        }
    }

    void Ack(unsigned seq, const char* fmt, ...) {
        char text[2400];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(text, sizeof(text), fmt, args);
        va_end(args);
        FILE* f = _fsopen((g_dir + "ack.txt").c_str(), "a", _SH_DENYNO);
        if (f) {
            std::fprintf(f, "%u %s\n", seq, text);
            std::fclose(f);
        }
    }

    void Execute(unsigned seq, const char* cmd, const char* args) {
        if (!std::strcmp(cmd, "move")) {
            int dx = 0, dy = 0;
            if (std::sscanf(args, "%d %d", &dx, &dy) != 2) { Ack(seq, "err usage: move DX DY"); return; }
            QueueMove(dx, dy);
            const bool ok = WaitQueueEmpty(8000);
            float x = 0, y = 0;
            CursorPos(x, y);
            Ack(seq, "%s cursor=%.0f,%.0f", ok ? "ok" : "err timeout", x, y);
        } else if (!std::strcmp(cmd, "moveto")) {
            int tx = 0, ty = 0;
            if (std::sscanf(args, "%d %d", &tx, &ty) != 2) { Ack(seq, "err usage: moveto X Y"); return; }
            float x = 0, y = 0;
            bool ok = true;
            for (int pass = 0; pass < 4 && ok; ++pass) {
                if (!CursorPos(x, y)) { ok = false; break; }
                const int dx = tx - static_cast<int>(x + 0.5f), dy = ty - static_cast<int>(y + 0.5f);
                if (dx == 0 && dy == 0) break;
                QueueMove(dx, dy);
                ok = WaitQueueEmpty(8000);
                Sleep(60); // let the game's tick take the position over
            }
            CursorPos(x, y);
            const bool reached = std::abs(tx - static_cast<int>(x + 0.5f)) <= 1 && std::abs(ty - static_cast<int>(y + 0.5f)) <= 1;
            Ack(seq, "%s cursor=%.0f,%.0f", !ok ? "err" : (reached ? "ok" : "err not reached (dialog closing or loading?)"), x, y);
        } else if (!std::strcmp(cmd, "click") || !std::strcmp(cmd, "down") || !std::strcmp(cmd, "up")) {
            const uint32_t ofs = (args[0] == 'R' || args[0] == 'r') ? 0x0D : 0x0C;
            if (cmd[0] != 'u') Enqueue(0, ofs, 0x80, 20.0);
            if (cmd[0] != 'd') Enqueue(0, ofs, 0x00, cmd[0] == 'c' ? 90.0 : 20.0);
            Enqueue(0, 0, 0, 60.0);
            Ack(seq, WaitQueueEmpty(5000) ? "ok" : "err timeout");
        } else if (!std::strcmp(cmd, "hold") || !std::strcmp(cmd, "release")) {
            // a key held down until "release DIK" (shots while e.g. left Shift is held)
            unsigned dik = 0;
            if (std::sscanf(args, "%i", reinterpret_cast<int*>(&dik)) != 1 || dik == 0 || dik > 0xFF) {
                Ack(seq, "err usage: hold|release DIK");
                return;
            }
            Enqueue(1, dik, cmd[0] == 'h' ? 0x80 : 0x00, 20.0);
            Enqueue(0, 0, 0, 60.0);
            Ack(seq, WaitQueueEmpty(5000) ? "ok" : "err timeout");
        } else if (!std::strcmp(cmd, "key")) {
            unsigned dik = 0;
            int hold = 80;
            const int n = std::sscanf(args, "%i %d", reinterpret_cast<int*>(&dik), &hold);
            if (n < 1 || dik == 0 || dik > 0xFF) { Ack(seq, "err usage: key DIK [HOLD_MS]"); return; }
            Enqueue(1, dik, 0x80, 20.0);
            Enqueue(1, dik, 0x00, static_cast<double>(hold));
            Enqueue(0, 0, 0, 60.0);
            Ack(seq, WaitQueueEmpty(static_cast<DWORD>(hold) + 5000) ? "ok" : "err timeout");
        } else if (!std::strcmp(cmd, "shot") || !std::strcmp(cmd, "shotout") || !std::strcmp(cmd, "moveshot")) {
            // moveshot DX DY NAME: the output frame while that movement is still going on (a sword
            // swing's trail fades within a fraction of a second)
            char name[128]{};
            const bool moving = cmd[0] == 'm';
            int dx = 0, dy = 0;
            if (moving ? std::sscanf(args, "%d %d %127s", &dx, &dy, name) != 3 : std::sscanf(args, "%127s", name) != 1) {
                Ack(seq, moving ? "err usage: moveshot DX DY NAME" : "err usage: shot NAME");
                return;
            }
            if (moving) {
                QueueMove(dx, dy);
                Sleep(40);
            }
            g_shotPath = g_dir + name + ".bmp";
            g_shotScaled = moving || (cmd[4] == 'o');
            g_shotState.store(1);
            const DWORD t0 = GetTickCount();
            while (g_shotState.load() == 1 && GetTickCount() - t0 < 5000) Sleep(5);
            const int st = g_shotState.exchange(0);
            if (moving) WaitQueueEmpty(8000);
            if (st == 2) Ack(seq, "ok %s %dx%d", g_shotPath.c_str(), g_shotW, g_shotH);
            else Ack(seq, st == 1 ? "err no frame was presented in 5 s" : "err the frame could not be saved");
        } else if (!std::strcmp(cmd, "mem")) {
            unsigned addr = 0, len = 0;
            if (std::sscanf(args, "%i %i", reinterpret_cast<int*>(&addr), reinterpret_cast<int*>(&len)) != 2 || len == 0 || len > 256) {
                Ack(seq, "err usage: mem ADDR LEN(1..256)");
                return;
            }
            uint8_t buf[256];
            if (!ReadMem(addr, buf, len)) { Ack(seq, "err unreadable"); return; }
            char hex[256 * 3 + 1];
            int pos = 0;
            for (unsigned i = 0; i < len; ++i) pos += std::snprintf(hex + pos, sizeof(hex) - pos, "%02X ", buf[i]);
            Ack(seq, "ok %s", hex);
        } else if (!std::strcmp(cmd, "memfind")) {
            // memfind F1 [F2]: where the game keeps these float values (RHGame, RHEngine and the
            // committed private memory of the process) - e.g. a position on the screen
            float want[2]{};
            const int n = std::sscanf(args, "%f %f", &want[0], &want[1]);
            if (n < 1) { Ack(seq, "err usage: memfind FLOAT [FLOAT_NEXT]"); return; }
            std::string found;
            int hits = 0;
            MEMORY_BASIC_INFORMATION mbi{};
            for (uintptr_t a = 0x00010000; a < 0x7FFF0000 && hits < 40; a = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize) {
                if (!VirtualQuery(reinterpret_cast<const void*>(a), &mbi, sizeof(mbi))) break;
                if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
                    !(mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) || mbi.RegionSize > 256u * 1024 * 1024) {
                    continue;
                }
                const auto* p = static_cast<const float*>(mbi.BaseAddress);
                const size_t count = mbi.RegionSize / sizeof(float);
                for (size_t i = 0; i + (n > 1 ? 1 : 0) < count && hits < 40; ++i) {
                    if (p[i] == want[0] && (n < 2 || p[i + 1] == want[1])) {
                        char text[16];
                        std::snprintf(text, sizeof(text), " %08X", static_cast<unsigned>(reinterpret_cast<uintptr_t>(p + i)));
                        found += text;
                        ++hits;
                    }
                }
            }
            Ack(seq, "ok %d%s", hits, found.c_str());
        } else if (!std::strcmp(cmd, "stat")) {
            float cx = -1, cy = -1;
            CursorPos(cx, cy);
            uint8_t active = 0;
            ReadMem(kAddrAppActive, &active, 1);
            uint8_t* engine = nullptr;
            ReadMem(kAddrEnginePtr, &engine, sizeof(engine));
            float cam[2]{}, mapSize[2]{}, view[2]{}, zoom = 0;
            uint8_t camState = 0;
            if (engine) {
                const auto e = reinterpret_cast<uintptr_t>(engine);
                ReadMem(e + 0x4738, cam, sizeof(cam));
                ReadMem(e + 0x4730, mapSize, sizeof(mapSize));
                ReadMem(e + 0x4728, view, sizeof(view));
                ReadMem(e + 0x475C, &zoom, sizeof(zoom));
                ReadMem(e + 0x48C0, &camState, 1);
            }
            int iw = 0, ih = 0;
            WindowManager::GetInternalResolution(iw, ih);
            Ack(seq, "ok active=%u cursor=%.0f,%.0f internal=%dx%d engine=%d map=%.0fx%.0f viewport=%.0fx%.0f cam=%.0f,%.0f zoom=%.2f camState=%u desktopBlocked=%u hd=%s hdSprites=%u hdColourDiff=%.1f between=%s hdBetween=%u",
                active, cx, cy, iw, ih, engine ? 1 : 0, mapSize[0], mapSize[1], view[0], view[1], cam[0], cam[1], zoom, camState,
                g_desktopCallsBlocked.load(), DDM::HD::CurrentVariantName(), DDM::HD::Sprites::FramesDrawn(),
                DDM::HD::Sprites::WorstColourDiff(), DDM::HD::BetweenOn() ? "on" : "off", DDM::HD::Sprites::BetweenDrawn());
        } else if (!std::strcmp(cmd, "zoomin") || !std::strcmp(cmd, "zoomout")) {
            // the engine's own zoom requests, run on the main thread at the next game tick
            g_mainCmd.store(cmd[4] == 'i' ? 1 : 2);
            const DWORD t0 = GetTickCount();
            while (g_mainCmd.load() != 0 && GetTickCount() - t0 < 3000) Sleep(5);
            if (g_mainCmd.exchange(0) != 0) { Ack(seq, "err no game tick in 3 s (menu open?)"); return; }
            // wait for the zoom transition (camera state 3/4) to finish
            uint8_t* engine = nullptr;
            float zoom = 0;
            for (int i = 0; i < 400; ++i) {
                Sleep(25);
                uint8_t camState = 0;
                if (!ReadMem(kAddrEnginePtr, &engine, sizeof(engine)) || !engine) break;
                ReadMem(reinterpret_cast<uintptr_t>(engine) + 0x48C0, &camState, 1);
                ReadMem(reinterpret_cast<uintptr_t>(engine) + 0x475C, &zoom, sizeof(zoom));
                if (i > 4 && camState != 3 && camState != 4) break;
            }
            Ack(seq, "ok zoom=%.2f", zoom);
        } else if (!std::strcmp(cmd, "win")) {
            // win the running mission through the game's console (reaches the camp of Sherwood);
            // it changes the profile's campaign - back up DATA\Savegame first (tools\save_backup.ps1)
            g_mainCmd.store(3);
            const DWORD t0 = GetTickCount();
            while (g_mainCmd.load() != 0 && GetTickCount() - t0 < 3000) Sleep(5);
            Ack(seq, g_mainCmd.exchange(0) != 0 ? "err no game tick in 3 s (menu open?)" : "ok");
        } else if (!std::strcmp(cmd, "wheel")) {
            int notches = 0;
            if (std::sscanf(args, "%d", &notches) != 1 || notches == 0) { Ack(seq, "err usage: wheel NOTCHES"); return; }
            Enqueue(0, 8, static_cast<uint32_t>(notches * 120), 20.0);
            Enqueue(0, 0, 0, 60.0);
            Ack(seq, WaitQueueEmpty(5000) ? "ok" : "err timeout");
        } else if (!std::strcmp(cmd, "hd")) {
            // HD background variants: hd            (which one is shown)
            //                         hd next | prev | toggle | NAME | off
            char what[64]{};
            if (std::sscanf(args, "%63s", what) == 1) {
                if (!std::strcmp(what, "next")) DDM::HD::RequestStep(+1);
                else if (!std::strcmp(what, "prev")) DDM::HD::RequestStep(-1);
                else if (!std::strcmp(what, "toggle")) DDM::HD::RequestToggle();
                else DDM::HD::RequestByName(what);
                Sleep(400); // the switch and the redraw happen at the next game ticks
            }
            Ack(seq, "ok hd=%s", DDM::HD::CurrentVariantName());
        } else if (!std::strcmp(cmd, "cursor")) {
            // the mission cursor's own picture drawn last (which one the game chose: walk, can't walk, attack ...)
            const uint64_t k = DDM::LastCursorKey();
            Ack(seq, "ok cursor=%ux%u key=%08x", static_cast<unsigned>(k >> 48), static_cast<unsigned>((k >> 32) & 0xFFFF),
                static_cast<unsigned>(k & 0xFFFFFFFFu));
        } else if (!std::strcmp(cmd, "zones")) {
            // zones NAME: the level's walk zones -> tools\out\harness\NAME.txt (tools\hd\plot_zones.py). The zone map
            // is the engine + 0x11C: a grid of 64-unit cells per layer (u16 width +0x3E98, height +0x3E9A, layers
            // +0x3E9C; cells +0x403C, 40 bytes: u16 count +0x10, zone pointers +0x14); a zone has its polygon at +4
            // (vertices: begin +8, end +0xC), flags +0x30 (0x40 = found by the lookup at a point), active +0x34,
            // floor +0x36, id +0x38, bounding box +0x3C (x0 y0 x1 y1, valid +0x4C), linked +0x58, network +0x64,
            // parent +0x98, type +0xF0. Only read here.
            char name[128]{};
            if (std::sscanf(args, "%127s", name) != 1) { Ack(seq, "err usage: zones NAME"); return; }
            uintptr_t engine = 0;
            if (!ReadMem(kAddrEnginePtr, &engine, 4) || !engine) { Ack(seq, "err no engine"); return; }
            const uintptr_t zm = engine + 0x11C;
            uint16_t gw = 0, gh = 0, layers = 0;
            uintptr_t cells = 0;
            ReadMem(zm + 0x3E98, &gw, 2);
            ReadMem(zm + 0x3E9A, &gh, 2);
            ReadMem(zm + 0x3E9C, &layers, 2);
            ReadMem(zm + 0x403C, &cells, 4);
            if (!cells || !gw || !gh || !layers || static_cast<size_t>(gw) * gh * layers > 200000) {
                Ack(seq, "err no zone grid (gw=%u gh=%u layers=%u)", gw, gh, layers);
                return;
            }
            const std::string path = g_dir + name + ".txt";
            FILE* f = _fsopen(path.c_str(), "w", _SH_DENYNO);
            if (!f) { Ack(seq, "err cannot write %s", path.c_str()); return; }
            std::fprintf(f, "grid %u %u %u\n", gw, gh, layers);
            std::vector<uintptr_t> seen;
            const size_t ncells = static_cast<size_t>(gw) * gh * layers;
            for (size_t c = 0; c < ncells; ++c) {
                uint8_t rec[40]{};
                if (!ReadMem(cells + c * 40, rec, sizeof(rec))) continue;
                uint16_t n = 0;
                uintptr_t arr = 0;
                std::memcpy(&n, rec + 0x10, 2);
                std::memcpy(&arr, rec + 0x14, 4);
                if (!n || !arr || n > 512) continue;
                std::vector<uintptr_t> ptrs(n);
                if (!ReadMem(arr, ptrs.data(), n * 4u)) continue;
                const size_t layer = c / (static_cast<size_t>(gw) * gh), rest = c % (static_cast<size_t>(gw) * gh);
                std::fprintf(f, "cell %u %u %u %u", static_cast<unsigned>(layer), static_cast<unsigned>(rest % gw),
                    static_cast<unsigned>(rest / gw), n);
                for (const uintptr_t p : ptrs) {
                    std::fprintf(f, " %08X", static_cast<unsigned>(p));
                    if (std::find(seen.begin(), seen.end(), p) == seen.end()) seen.push_back(p);
                }
                std::fprintf(f, "\n");
            }
            uintptr_t listB = 0, listE = 0, special = 0;
            ReadMem(zm + 0x4064, &listB, 4);
            ReadMem(zm + 0x4068, &listE, 4);
            ReadMem(zm + 0x4184, &special, 4);
            std::fprintf(f, "list %08X %08X special %08X\n", static_cast<unsigned>(listB), static_cast<unsigned>(listE),
                static_cast<unsigned>(special));
            if (listB && listE > listB && (listE - listB) / 4 < 4096) {
                std::vector<uintptr_t> all((listE - listB) / 4);
                if (ReadMem(listB, all.data(), all.size() * 4)) {
                    for (const uintptr_t p : all)
                        if (p && std::find(seen.begin(), seen.end(), p) == seen.end()) seen.push_back(p);
                }
            }
            for (const uintptr_t z : seen) {
                uint8_t zr[0x100]{};
                if (!ReadMem(z, zr, sizeof(zr))) continue;
                auto u32 = [&](size_t o) { uint32_t v; std::memcpy(&v, zr + o, 4); return v; };
                auto u16 = [&](size_t o) { uint16_t v; std::memcpy(&v, zr + o, 2); return static_cast<unsigned>(v); };
                auto f32 = [&](size_t o) { float v; std::memcpy(&v, zr + o, 4); return v; };
                const uint32_t vb = u32(8), ve = u32(0xC);
                const unsigned nv = (vb && ve > vb && (ve - vb) / 8 < 20000) ? (ve - vb) / 8 : 0;
                std::fprintf(f, "zone %08X flags=%08X act=%u floor=%u id=%u z58=%08X z64=%08X z98=%08X f0=%u bbox=%.1f,%.1f,%.1f,%.1f,%u n=%u\n",
                    static_cast<unsigned>(z), u32(0x30), zr[0x34], u16(0x36), u16(0x38), u32(0x58), u32(0x64), u32(0x98), u32(0xF0),
                    f32(0x3C), f32(0x40), f32(0x44), f32(0x48), zr[0x4C], nv);
                if (nv) {
                    std::vector<float> v(nv * 2u);
                    if (ReadMem(vb, v.data(), v.size() * 4)) {
                        for (unsigned i = 0; i < nv; ++i) std::fprintf(f, "v %.2f %.2f\n", v[i * 2], v[i * 2 + 1]);
                    }
                }
            }
            std::fclose(f);
            Ack(seq, "ok %s grid=%ux%ux%u zones=%u", path.c_str(), gw, gh, layers, static_cast<unsigned>(seen.size()));
        } else if (!std::strcmp(cmd, "pointer")) {
            // pointer [X Y]: what the interface scale maps the pointer at a screen point to (default: the cursor)
            float x = -1.0f, y = -1.0f;
            if (std::sscanf(args, "%f %f", &x, &y) != 2) CursorPos(x, y);
            g_zoneqPt[0] = x;
            g_zoneqPt[1] = y;
            g_zoneqOut[0] = 0;
            g_mainCmd.store(5);
            const DWORD t0 = GetTickCount();
            while (g_mainCmd.load() != 0 && GetTickCount() - t0 < 3000) Sleep(5);
            if (g_mainCmd.exchange(0) != 0) { Ack(seq, "err no game tick in 3 s"); return; }
            Ack(seq, "ok %.0f,%.0f %s", x, y, g_zoneqOut);
        } else if (!std::strcmp(cmd, "pointergrid")) {
            // pointergrid X0 Y0 X1 Y1 STEP NAME: UiScale::ClassifyPointer over a grid of screen points -> NAME.csv
            // (x, y, class, seeThrough; tools\hd\plot_pointer.py), on the main thread
            char name[128]{};
            if (std::sscanf(args, "%d %d %d %d %d %127s", &g_gridArgs[0], &g_gridArgs[1], &g_gridArgs[2], &g_gridArgs[3],
                            &g_gridArgs[4], name) != 6 || g_gridArgs[4] < 2) {
                Ack(seq, "err usage: pointergrid X0 Y0 X1 Y1 STEP NAME");
                return;
            }
            g_gridPath = g_dir + name + ".csv";
            g_zoneqOut[0] = 0;
            g_mainCmd.store(6);
            const DWORD t0 = GetTickCount();
            while (g_mainCmd.load() != 0 && GetTickCount() - t0 < 5000) Sleep(5);
            if (g_mainCmd.exchange(0) != 0) { Ack(seq, "err no game tick in 5 s"); return; }
            Ack(seq, "ok %s %s", g_gridPath.c_str(), g_zoneqOut);
        } else if (!std::strcmp(cmd, "text")) {
            // text ID: the game's text routine (0x005E23D0) for one id, its code units in hex - run on this thread (a
            // read of the text table; the menus have no game tick)
            unsigned id = 0;
            if (std::sscanf(args, "%i", reinterpret_cast<int*>(&id)) != 1) { Ack(seq, "err usage: text ID"); return; }
            alignas(4) uint8_t str[16]{};
            using PFN_GetText = void*(__cdecl*)(void* out, uint32_t id);
            using PFN_Tidy = void(__fastcall*)(void* str, void* edx, uint32_t freeIt);
            reinterpret_cast<PFN_GetText>(0x005E23D0)(str, id);
            const auto* p = *reinterpret_cast<const uint16_t* const*>(str + 4);
            const uint32_t n = *reinterpret_cast<const uint32_t*>(str + 8);
            char out[1024];
            int pos = std::snprintf(out, sizeof(out), "len=%u:", n);
            for (uint32_t i = 0; p && i < n && i < 120 && pos < static_cast<int>(sizeof(out)) - 8; ++i) {
                pos += std::snprintf(out + pos, sizeof(out) - pos, " %04X", p[i]);
            }
            reinterpret_cast<PFN_Tidy>(0x004238C0)(str, nullptr, 1);
            Ack(seq, "ok %s", out);
        } else if (!std::strcmp(cmd, "zonelog")) {
            // the last calls of the zone-under-a-point routine and of the cursor's choice (newest first)
            std::string s;
            char line[256];
            const unsigned nz = g_zoneCallN.load(), nc = g_cursorCallN.load();
            for (unsigned i = 0; i < 4 && i < nz; ++i) {
                const ZoneCall& c = g_zoneCalls[(nz - 1 - i) % 8];
                std::snprintf(line, sizeof(line), " Z[%08X pt=%.1f,%.1f zone=%08X cc=%08X c4=%u alt=%u char=%08X@%.0f,%.0f]",
                    c.caller, c.x, c.y, c.zone, c.cc, c.c4, c.alt, c.charZone, c.cx, c.cy);
                s += line;
            }
            for (unsigned i = 0; i < 4 && i < nc; ++i) {
                const CursorCall& c = g_cursorCalls[(nc - 1 - i) % 8];
                std::snprintf(line, sizeof(line), " C[%08X pt=%.1f,%.1f zoneIn=%08X id=0x%X]", c.caller, c.x, c.y, c.zoneIn, c.id);
                s += line;
            }
            Ack(seq, "ok nz=%u nc=%u%s", nz, nc, s.c_str());
        } else if (!std::strcmp(cmd, "zoneq")) {
            // zoneq X Y: the game's own zone lookup at a level point (main thread) - which zone, per layer, and the
            // bounding box / polygon tests of every zone in that point's cell
            if (std::sscanf(args, "%f %f", &g_zoneqPt[0], &g_zoneqPt[1]) != 2) { Ack(seq, "err usage: zoneq X Y"); return; }
            g_zoneqOut[0] = 0;
            g_mainCmd.store(4);
            const DWORD t0 = GetTickCount();
            while (g_mainCmd.load() != 0 && GetTickCount() - t0 < 3000) Sleep(5);
            if (g_mainCmd.exchange(0) != 0) { Ack(seq, "err no game tick in 3 s"); return; }
            Ack(seq, "ok %s", g_zoneqOut);
        } else if (!std::strcmp(cmd, "pause")) {
            // the game's pause - the input translator's flag (singleton [0x006C0C4C] + 0x1B; RHGame::Run then skips
            // the engine's tick and the frame limiter): pause 1 | pause 0 | pause (state)
            int v = -1;
            std::sscanf(args, "%d", &v);
            auto* tr = *reinterpret_cast<uint8_t* volatile*>(0x006C0C4C);
            if (!tr) {
                Ack(seq, "err no input translator (no level)");
            } else {
                if (v == 0 || v == 1) tr[0x1B] = static_cast<uint8_t>(v);
                Ack(seq, "ok pause=%u", static_cast<unsigned>(tr[0x1B]));
            }
        } else if (!std::strcmp(cmd, "frames")) {
            // which bank frame every sprite surface holds right now -> text file (for upscale_sprites.py --ids-file)
            char name[128]{};
            if (std::sscanf(args, "%127s", name) != 1) { Ack(seq, "err usage: frames NAME"); return; }
            const std::string path = g_dir + name + ".txt";
            DDM::HD::Sprites::RequestDump(path.c_str());
            const DWORD t0 = GetTickCount();
            while (DDM::HD::Sprites::DumpPending() && GetTickCount() - t0 < 3000) Sleep(10);
            Ack(seq, "%s %s", DDM::HD::Sprites::DumpPending() ? "err no game tick in 3 s" : "ok", path.c_str());
        } else if (!std::strcmp(cmd, "between")) {
            // in-between frames of sprite animations: between on | off | toggle
            char what[16]{};
            if (std::sscanf(args, "%15s", what) == 1) {
                DDM::HD::RequestBetween(!std::strcmp(what, "on") ? 1 : (!std::strcmp(what, "off") ? 0 : 2));
                Sleep(300); // the switch happens at the next game tick
            }
            Ack(seq, "ok between=%s drawn=%u", DDM::HD::BetweenOn() ? "on" : "off", DDM::HD::Sprites::BetweenDrawn());
        } else if (!std::strcmp(cmd, "settings")) {
            // the mod's settings panel: open | close | up | down | left | right | click | state
            char what[32]{};
            std::sscanf(args, "%31s", what);
            static char text[1024];
            SettingsMenu::TestCommand(what, text, sizeof(text));
            Ack(seq, "ok %s", text);
        } else if (!std::strcmp(cmd, "lupa")) {
            // smooth zoom: lupa             (state)
            //              lupa Z           (jump to the effective zoom Z at the next game tick)
            //              lupa gamein | gameout  (the engine's own zoom animation, as a script starts it)
            float z = 0.0f;
            char word[16]{};
            if (std::sscanf(args, "%15s", word) == 1 && (!std::strcmp(word, "gamein") || !std::strcmp(word, "gameout"))) {
                ViewZoom::TestGameZoom(word[4] == 'i' ? +1 : -1);
                Sleep(1500);    // the animation takes about a second
            } else if (std::sscanf(args, "%f", &z) == 1 && z > 0.0f) {
                ViewZoom::TestSetZoom(z);
                Sleep(400);
            }
            char text[320];
            ViewZoom::Describe(text, sizeof(text));
            Ack(seq, "ok %s", text);
        } else if (!std::strcmp(cmd, "fonts")) {
            // HD font: fonts          (state)
            //          fonts dump     (every font strip seen from now on -> tools\out\harness\fonts\font_*.raw)
            //          fonts stop
            char what[16]{};
            if (std::sscanf(args, "%15s", what) == 1) {
                if (!std::strcmp(what, "dump")) DDM::HD::Font::SetDump((g_dir + "fonts").c_str());
                else if (!std::strcmp(what, "stop")) DDM::HD::Font::SetDump(nullptr);
            }
            char text[160];
            DDM::HD::Font::Describe(text, sizeof(text));
            Ack(seq, "ok %s", text);
        } else if (!std::strcmp(cmd, "menuhd")) {
            // HD menu pictures: counters and the key of the menu picture seen last
            char text[256];
            DDM::HD::Menu::Describe(text, sizeof(text));
            Ack(seq, "ok %s", text);
        } else if (!std::strcmp(cmd, "ambiance")) {
            // the next mission start on another picture of its town: 1 day, 2 fog, 4 night, 0 = as the level says
            unsigned v = 0;
            std::sscanf(args, "%u", &v);
            g_ambiance = v;
            Ack(seq, "ok ambiance=%u (from the next mission start)", v);
        } else if (!std::strcmp(cmd, "patch")) {
            // HD letters and pictures of menus and dialogs (ddm_hd_patch.h): counters
            char text[320];
            DDM::HD::Patch::Describe(text, sizeof(text));
            Ack(seq, "ok %s", text);
        } else if (!std::strcmp(cmd, "uiwins")) {
            // the mission's windows: rectangle and what each drew in the last frame
            static char text[8192];
            UiScale::DescribeWindows(text, sizeof(text));
            const std::string path = g_dir + "uiwins.txt";
            FILE* f = _fsopen(path.c_str(), "w", _SH_DENYNO);
            if (f) {
                for (const char* p = text; *p; ++p) {   // one window per line
                    if (p[0] == ' ' && p[1] == '[') std::fputc('\n', f); else std::fputc(*p, f);
                }
                std::fputc('\n', f);
                std::fclose(f);
            }
            Ack(seq, "ok %s", path.c_str());
        } else if (!std::strcmp(cmd, "rtdump")) {
            // all frames the game made at run time (icons, markers), as its bank holds them -> binary file
            char name[128]{};
            if (std::sscanf(args, "%127s", name) != 1) { Ack(seq, "err usage: rtdump NAME"); return; }
            const std::string path = g_dir + name + ".bin";
            DDM::HD::Sprites::RequestRunTimeDump(path.c_str());
            const DWORD t0 = GetTickCount();
            while (DDM::HD::Sprites::RunTimeDumpPending() && GetTickCount() - t0 < 5000) Sleep(10);
            Ack(seq, "%s %s", DDM::HD::Sprites::RunTimeDumpPending() ? "err no game tick in 5 s" : "ok", path.c_str());
        } else if (!std::strcmp(cmd, "miss")) {
            // what is magnified 2x without an upscaled frame during the next TICKS game ticks -> text file
            char name[128]{};
            int ticks = 25;
            if (std::sscanf(args, "%127s %d", name, &ticks) < 1 || ticks < 1 || ticks > 5000) { Ack(seq, "err usage: miss NAME [TICKS]"); return; }
            const std::string path = g_dir + name + ".txt";
            DDM::HD::Sprites::RequestMissReport(path.c_str(), ticks);
            const DWORD t0 = GetTickCount();
            const DWORD limit = static_cast<DWORD>(ticks) * 60 + 5000;
            while (DDM::HD::Sprites::MissReportPending() && GetTickCount() - t0 < limit) Sleep(10);
            Ack(seq, "%s %s", DDM::HD::Sprites::MissReportPending() ? "err the game did not tick long enough" : "ok", path.c_str());
        } else if (!std::strcmp(cmd, "presentlog")) {
            // the next N presented frames: how each was composed, into the log (a wrong frame too short for a shot)
            int count = 0;
            if (std::sscanf(args, "%d", &count) != 1 || count <= 0) { Ack(seq, "err usage: presentlog N"); return; }
            DDM::Present::LogNextFrames(count);
            Ack(seq, "ok");
        } else if (!std::strcmp(cmd, "levelpic")) {
            // the level picture the world is drawn from now (and the copy the HD picture is compared with)
            // -> tools\out\harness\NAME.raw (DDM::HD::DumpLevelPictures)
            char name[96]{};
            if (std::sscanf(args, "%95s", name) != 1) { Ack(seq, "err usage: levelpic NAME"); return; }
            const std::string path = g_dir + name + ".raw";
            Ack(seq, DDM::HD::DumpLevelPictures(path.c_str()) ? "ok %s" : "err no level picture", path.c_str());
        } else if (!std::strcmp(cmd, "dbg")) {
            // leave out drawing routines (bit mask, see DDM::HD::SetDebugFlags); dbg 0 = everything on
            unsigned flags = 0;
            std::sscanf(args, "%i", reinterpret_cast<int*>(&flags));
            DDM::HD::SetDebugFlags(flags);
            Sleep(120);
            Ack(seq, "ok dbg=%u", DDM::HD::DebugFlags());
        } else if (!std::strcmp(cmd, "trace")) {
            // the next N surface calls (Blt / Lock / Flip / present) as text: what a frame is made of
            // "trace N NAME bg" starts it and answers at once (the next commands - a key that opens a
            // dialog - land inside it); the file closes by itself after N calls.
            int count = 0;
            char name[128]{};
            char opt[16]{};
            if (std::sscanf(args, "%d %127s %15s", &count, name, opt) < 2 || count <= 0 || count > 400000) {
                Ack(seq, "err usage: trace COUNT NAME [bg]");
                return;
            }
            const std::string path = g_dir + name + ".txt";
            DDM::TraceCalls(path.c_str(), count);
            if (!std::strcmp(opt, "bg")) {
                Ack(seq, "ok started %s", path.c_str());
                return;
            }
            const DWORD t0 = GetTickCount();
            while (DDM::TraceActive() && GetTickCount() - t0 < 8000) Sleep(10);
            const bool done = !DDM::TraceActive();
            DDM::TraceCalls(nullptr, 0); // closes the file if the count was not reached
            Ack(seq, "%s %s", done ? "ok" : "ok (fewer calls than asked for in 8 s)", path.c_str());
        } else if (!std::strcmp(cmd, "coop")) {
            // co-op probe (coop_probe.h): sessions of recorded / replayed input with a state hash per tick
            static char text[1200];
            CoopProbe::Command(args, text, sizeof(text));
            Ack(seq, "%s", text);
        } else if (!std::strcmp(cmd, "quit")) {
            const HWND hwnd = WindowManager::GetGameWindow();
            Ack(seq, hwnd && PostMessageA(hwnd, WM_SYSCOMMAND, SC_CLOSE, 0) ? "ok" : "err no window");
        } else {
            Ack(seq, "err unknown command '%s'", cmd);
        }
    }

    DWORD WINAPI ReaderThread(LPVOID) {
        const std::string path = g_dir + "cmd.txt";
        // commands written before this start belong to an earlier run
        long offset = 0;
        if (FILE* f = _fsopen(path.c_str(), "rb", _SH_DENYNO)) {
            std::fseek(f, 0, SEEK_END);
            offset = std::ftell(f);
            std::fclose(f);
        }
        std::string pending;
        for (;;) {
            Sleep(15);
            FILE* f = _fsopen(path.c_str(), "rb", _SH_DENYNO);
            if (!f) continue;
            std::fseek(f, 0, SEEK_END);
            const long size = std::ftell(f);
            if (size < offset) offset = 0; // the file was replaced
            if (size > offset) {
                std::fseek(f, offset, SEEK_SET);
                std::vector<char> buf(static_cast<size_t>(size - offset));
                const size_t got = std::fread(buf.data(), 1, buf.size(), f);
                offset += static_cast<long>(got);
                pending.append(buf.data(), got);
            }
            std::fclose(f);

            size_t eol;
            while ((eol = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, eol);
                pending.erase(0, eol + 1);
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                unsigned seq = 0;
                char cmd[32]{};
                int consumed = 0;
                if (std::sscanf(line.c_str(), "%u %31s %n", &seq, cmd, &consumed) < 2) continue;
                Execute(seq, cmd, line.c_str() + consumed);
            }
        }
    }

    bool PatchIat(uintptr_t slot, void* replacement, void** original) {
        auto** p = reinterpret_cast<void**>(slot);
        DWORD oldProt = 0;
        if (!*p || !VirtualProtect(p, sizeof(void*), PAGE_READWRITE, &oldProt)) return false;
        if (original) *original = *p;
        *p = replacement;
        VirtualProtect(p, sizeof(void*), oldProt, &oldProt);
        return true;
    }

    using PFN_SetOutput = signed char(__stdcall*)(int);
    PFN_SetOutput o_SetOutput = nullptr;

    // A test build of 2026-11-22 muted this process's audio session in the Windows mixer - Windows keeps that for the
    // program, so the player's next start of Game.exe had no sound. The marker file tools\out\harness\unmute_once.txt
    // asks a test run to undo it once (only a mute a test left; the player's own mixer settings are not touched).
    void UndoTestMute() {
        char exe[MAX_PATH]{};
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        std::string marker(exe);
        marker.erase(marker.find_last_of("\\/") + 1);
        marker += "tools\\out\\harness\\unmute_once.txt";     // also for a QUIET run (g_dir is set in test mode only)
        if (GetFileAttributesA(marker.c_str()) == INVALID_FILE_ATTRIBUTES) return;
        const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // RPC_E_CHANGED_MODE: COM is there
        IMMDeviceEnumerator* en = nullptr;
        IMMDevice* dev = nullptr;
        IAudioSessionManager* mgr = nullptr;
        ISimpleAudioVolume* vol = nullptr;
        BOOL muted = FALSE;
        const bool ok = SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                                   reinterpret_cast<void**>(&en))) &&
                        SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) &&
                        SUCCEEDED(dev->Activate(__uuidof(IAudioSessionManager), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&mgr))) &&
                        SUCCEEDED(mgr->GetSimpleAudioVolume(nullptr, FALSE, &vol)) && SUCCEEDED(vol->GetMute(&muted)) &&
                        (!muted || SUCCEEDED(vol->SetMute(FALSE, nullptr)));
        if (vol) vol->Release();
        if (mgr) mgr->Release();
        if (dev) dev->Release();
        if (en) en->Release();
        (void)init;     // left initialised: the game's thread may use COM later
        if (ok) DeleteFileA(marker.c_str());
        Logger::Info("TEST MODE: mute left by an earlier test: %s.", !ok ? "could not be checked" : (muted ? "undone" : "none"));
    }

    // No sound in a test run. Not FSOUND_OUTPUT_NOSOUND (2026-11-22): with it a sound stream never ends and
    // FSOUND_Stream_Close waits for that for ever - the hidden game hung when Robin aimed his bow (a voice stream
    // closed). FMOD plays through its WINMM output into a waveOut device this DLL emulates (SilenceWaveOut: buffers
    // "played" in real time, nothing sent to Windows - zeroing the buffers at waveOutWrite was not enough, FMOD keeps
    // mixing into them: the player heard the hidden game, 2026-11-22). A muted audio session would be kept by Windows
    // for the program (see UndoTestMute). Only if the emulation cannot be set up, NOSOUND.
    signed char __stdcall Stub_SetOutput(int /*outputType*/) {
        static int silent = -1;
        if (silent < 0) {
            silent = SilenceWaveOut() ? 1 : 0;
            Logger::Info("TEST MODE: sound %s.", silent ? "through FMOD's WINMM output into an emulated device (nothing reaches Windows)" :
                                                          "without FMOD output (a voice stream may hang it)");
            if (silent) UndoTestMute();
        }
        return o_SetOutput ? o_SetOutput(silent ? 1 /* FSOUND_OUTPUT_WINMM */ : 0 /* FSOUND_OUTPUT_NOSOUND */) : 1;
    }
    int __stdcall Stub_BinkSetSoundSystem(void* /*open*/, uint32_t /*param*/) {
        return 1; // no sound system: the videos play silently
    }

    // ---- the desktop belongs to the player --------------------------------------------------
    // RHGame::Run centres the Windows pointer on the game window on EVERY pass of the mission
    // loop while the game is active (GetWindowRect + SetCursorPos at 0x0050F967..0x0050F98E).
    // With the forced activation of the test mode that pinned the player's pointer to the middle
    // of the screen for as long as a hidden test mission ran. A test instance must not be able
    // to move, clip, hide or inject anything: every such user32 entry is replaced for the whole
    // process (game, DirectInput, the driver's DLLs, this DLL), and the game's own import of
    // SetCursorPos / ShowCursor is replaced as a second, independent layer.
    std::atomic<int> g_showCursorCount{ 0 };

    BOOL WINAPI Stub_SetCursorPos(int, int)        { ++g_desktopCallsBlocked; return TRUE; }
    BOOL WINAPI Stub_ClipCursor(const RECT*)       { ++g_desktopCallsBlocked; return TRUE; }
    HCURSOR WINAPI Stub_SetCursor(HCURSOR)         { return nullptr; }
    int WINAPI Stub_ShowCursor(BOOL show)          { return show ? ++g_showCursorCount : --g_showCursorCount; }
    UINT WINAPI Stub_SendInput(UINT n, LPINPUT, int) { ++g_desktopCallsBlocked; return n; }
    void WINAPI Stub_mouse_event(DWORD, DWORD, DWORD, DWORD, ULONG_PTR) { ++g_desktopCallsBlocked; }
    void WINAPI Stub_keybd_event(BYTE, BYTE, DWORD, ULONG_PTR)          { ++g_desktopCallsBlocked; }
    BOOL WINAPI Stub_SetForegroundWindow(HWND)     { ++g_desktopCallsBlocked; return TRUE; }
    HWND WINAPI Stub_SetActiveWindow(HWND)         { ++g_desktopCallsBlocked; return nullptr; }
    BOOL WINAPI Stub_BringWindowToTop(HWND)        { ++g_desktopCallsBlocked; return TRUE; }
    HWND WINAPI Stub_SetCapture(HWND)              { ++g_desktopCallsBlocked; return nullptr; }

    // Returns the number of entries that could NOT be replaced.
    // ---- other ambiances of the level in the background test (night, fog) -------------------------
    // The first campaign mission is all the test can start. 0x004C1AB0 (thiscall, ret 0x10) reads the
    // level's header: [engine+0x4AD0] = ambiance (1 day, 2 fog, 4 night, 8 attack, 16.. custom; the map
    // comes from Data\Levels\<ambiance>\, the night / fog code tests == 4 / == 2) and [engine+0x4CC0]
    // (300 for fog and night, else 400). Its caller reads the ambiance right after it returns - set here,
    // the same mission runs on the night or fog picture of its town. Test instance only.
    using PFN_LevelHeader = uint32_t(__fastcall*)(uint8_t* engine, void* edx, void* a1, void* a2, void* a3, void* a4);
    PFN_LevelHeader o_LevelHeader = nullptr;

    uint32_t __fastcall Hook_LevelHeader(uint8_t* engine, void* edx, void* a1, void* a2, void* a3, void* a4) {
        const uint32_t r = o_LevelHeader(engine, edx, a1, a2, a3, a4);
        const uint32_t want = g_ambiance;
        if (want && engine) {
            const uint32_t was = *reinterpret_cast<uint32_t*>(engine + 0x4AD0);
            *reinterpret_cast<uint32_t*>(engine + 0x4AD0) = want;
            *reinterpret_cast<uint16_t*>(engine + 0x4CC0) = static_cast<uint16_t>((want == 2 || want == 4) ? 300 : 400);
            Logger::Info("TEST MODE: level ambiance %u replaced by %u.", was, want);
        }
        return r;
    }

    // ---- what the cursor's zone and picture come from (command "zonelog") -----------------------------
    // 0x004DEBF0 __thiscall(engine, const Vec2* pt) ret 4: the zone under a point -> [engine+0xC8] (zone map
    // lookup 0x004E9760, or 0x004E9920 while [[engine+0x68]+0x14]); 0x004CC100 __thiscall(engine, bool*, const
    // Vec2* pt) ret 8 -> the cursor's id. The last calls of each, test instance only.
    using PFN_ZoneUnder = uint32_t(__fastcall*)(uint8_t*, void*, const float*);
    using PFN_CursorId = uint16_t(__fastcall*)(uint8_t*, void*, bool*, const float*);
    PFN_ZoneUnder o_ZoneUnder = nullptr;
    PFN_CursorId o_CursorId = nullptr;

    uint32_t __fastcall Hook_ZoneUnder(uint8_t* engine, void* edx, const float* pt) {
        const uint32_t caller = reinterpret_cast<uint32_t>(_ReturnAddress());
        const uint32_t r = o_ZoneUnder(engine, edx, pt);
        ZoneCall c{};
        c.caller = caller;
        if (pt) { c.x = pt[0]; c.y = pt[1]; }
        c.zone = *reinterpret_cast<uint32_t*>(engine + 0xC8);
        c.cc = *reinterpret_cast<uint32_t*>(engine + 0xCC);
        c.c4 = *reinterpret_cast<uint16_t*>(engine + 0xC4);
        const uint8_t* e68 = *reinterpret_cast<uint8_t**>(engine + 0x68);
        c.alt = e68 ? e68[0x14] : 0xFF;
        if (*reinterpret_cast<uint32_t*>(engine + 0x4A00)) {
            const uint8_t* ch = *reinterpret_cast<uint8_t**>(**reinterpret_cast<uint8_t***>(engine + 0x49FC) + 8);
            const uint8_t* phys = ch ? *reinterpret_cast<uint8_t* const*>(ch + 0x50) : nullptr;
            if (phys) {
                c.charZone = *reinterpret_cast<const uint32_t*>(phys + 0xE0);
                c.cx = *reinterpret_cast<const float*>(phys + 0x24);
                c.cy = *reinterpret_cast<const float*>(phys + 0x28);
            }
        }
        g_zoneCalls[g_zoneCallN.fetch_add(1) % 8] = c;
        return r;
    }

    uint16_t __fastcall Hook_CursorId(uint8_t* engine, void* edx, bool* flag, const float* pt) {
        const uint32_t caller = reinterpret_cast<uint32_t>(_ReturnAddress());
        const uint32_t zoneIn = *reinterpret_cast<uint32_t*>(engine + 0xC8);
        const uint16_t id = o_CursorId(engine, edx, flag, pt);
        CursorCall c{ caller, pt ? pt[0] : -1.0f, pt ? pt[1] : -1.0f, zoneIn, id };
        g_cursorCalls[g_cursorCallN.fetch_add(1) % 8] = c;
        return id;
    }

    void InstallZoneProbe() {
        bool ok = MH_CreateHook(reinterpret_cast<void*>(0x004DEBF0), reinterpret_cast<void*>(&Hook_ZoneUnder),
                      reinterpret_cast<void**>(&o_ZoneUnder)) == MH_OK &&
                  MH_EnableHook(reinterpret_cast<void*>(0x004DEBF0)) == MH_OK;
        ok = ok && MH_CreateHook(reinterpret_cast<void*>(0x004CC100), reinterpret_cast<void*>(&Hook_CursorId),
                       reinterpret_cast<void**>(&o_CursorId)) == MH_OK &&
             MH_EnableHook(reinterpret_cast<void*>(0x004CC100)) == MH_OK;
        Logger::Info("TEST MODE: zone / cursor probe (command 'zonelog') %s.", ok ? "ready" : "NOT available");
    }

    void InstallAmbianceHook() {
        static const uint8_t kProlog[] = { 0x53, 0x56, 0x8B, 0x74, 0x24, 0x0C, 0x57, 0x6A, 0x04, 0x68, 0x54, 0x4F, 0x4F, 0x46 };
        auto* target = reinterpret_cast<void*>(0x004C1AB0);
        const bool ok = std::memcmp(target, kProlog, sizeof(kProlog)) == 0 &&
                        MH_CreateHook(target, reinterpret_cast<void*>(&Hook_LevelHeader), reinterpret_cast<void**>(&o_LevelHeader)) == MH_OK &&
                        MH_EnableHook(target) == MH_OK;
        Logger::Info("TEST MODE: level ambiance switch (command 'ambiance') %s.", ok ? "ready" : "NOT available");
    }

    int IsolateDesktop(bool& cursorPosSafe) {
        struct Entry { const char* name; void* stub; };
        static const Entry kEntries[] = {
            { "SetCursorPos",         reinterpret_cast<void*>(&Stub_SetCursorPos) },
            { "SetPhysicalCursorPos", reinterpret_cast<void*>(&Stub_SetCursorPos) },
            { "ClipCursor",           reinterpret_cast<void*>(&Stub_ClipCursor) },
            { "SetCursor",            reinterpret_cast<void*>(&Stub_SetCursor) },
            { "ShowCursor",           reinterpret_cast<void*>(&Stub_ShowCursor) },
            { "SendInput",            reinterpret_cast<void*>(&Stub_SendInput) },
            { "mouse_event",          reinterpret_cast<void*>(&Stub_mouse_event) },
            { "keybd_event",          reinterpret_cast<void*>(&Stub_keybd_event) },
            { "SetForegroundWindow",  reinterpret_cast<void*>(&Stub_SetForegroundWindow) },
            { "SetActiveWindow",      reinterpret_cast<void*>(&Stub_SetActiveWindow) },
            { "BringWindowToTop",     reinterpret_cast<void*>(&Stub_BringWindowToTop) },
            { "SetCapture",           reinterpret_cast<void*>(&Stub_SetCapture) },
        };
        int failed = 0;
        bool inlineCursorPos = false;
        const HMODULE user32 = GetModuleHandleA("user32.dll");
        for (const Entry& e : kEntries) {
            void* target = user32 ? reinterpret_cast<void*>(GetProcAddress(user32, e.name)) : nullptr;
            LPVOID original = nullptr; // never called: the stubs do not forward
            const bool ok = target && MH_CreateHook(target, e.stub, &original) == MH_OK && MH_EnableHook(target) == MH_OK;
            if (!ok) {
                ++failed;
                Logger::Warn("TEST MODE: user32!%s could not be replaced.", e.name);
            } else if (!std::strcmp(e.name, "SetCursorPos")) {
                inlineCursorPos = true;
            }
        }
        // second layer: the game's own imports (the only caller that is known to do it)
        const bool iatCursorPos = PatchIat(kIatSetCursorPos, reinterpret_cast<void*>(&Stub_SetCursorPos), nullptr);
        PatchIat(kIatShowCursor, reinterpret_cast<void*>(&Stub_ShowCursor), nullptr);
        cursorPosSafe = inlineCursorPos || iatCursorPos;
        return failed;
    }
}

bool Enabled() {
    if (!g_checked) {
        char v[8]{};
        g_enabled = GetEnvironmentVariableA("SHERWOODFIX_TEST", v, sizeof(v)) > 0 && v[0] == '1';
        g_checked = true;
    }
    return g_enabled;
}

void Init() {
    if (!Enabled()) {
        // SHERWOODFIX_QUIET=1: an ordinary, visible run without sound (tools\gamectl.ps1 -Quiet),
        // for checks at night. Nothing else of the test mode is active.
        char quiet[8]{};
        if (GetEnvironmentVariableA("SHERWOODFIX_QUIET", quiet, sizeof(quiet)) > 0 && quiet[0] == '1') {
            const bool noSound = PatchIat(kIatSetOutput, reinterpret_cast<void*>(&Stub_SetOutput), reinterpret_cast<void**>(&o_SetOutput));
            const bool noVideoSound = PatchIat(kIatBinkSoundSys, reinterpret_cast<void*>(&Stub_BinkSetSoundSystem), nullptr);
            Logger::Warn("QUIET RUN (SHERWOODFIX_QUIET=1): sound off: FMOD=%d videos=%d.", noSound ? 1 : 0, noVideoSound ? 1 : 0);
        }
        return;
    }

    char exe[MAX_PATH]{};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir(exe);
    dir.erase(dir.find_last_of("\\/") + 1);
    CreateDirectoryA((dir + "tools").c_str(), nullptr);
    CreateDirectoryA((dir + "tools\\out").c_str(), nullptr);
    g_dir = dir + "tools\\out\\harness\\";
    CreateDirectoryA(g_dir.c_str(), nullptr);

    g_marker = CreateEventA(nullptr, TRUE, FALSE, "Local\\SherwoodFixTestInstance");

    bool cursorPosSafe = false;
    const int notIsolated = IsolateDesktop(cursorPosSafe);
    InstallAmbianceHook();
    InstallZoneProbe();
    if (!cursorPosSafe) {
        // a hidden instance that can grab the player's pointer must not run at all
        Logger::Error("TEST MODE: SetCursorPos could not be taken away from the game - ending the test instance.");
        Logger::Shutdown();
        TerminateProcess(GetCurrentProcess(), 3);
    }
    Logger::Info("TEST MODE: pointer, input injection and foreground calls are cut off for this process (%d entries not replaced).",
                 notIsolated);

    const bool noSound = PatchIat(kIatSetOutput, reinterpret_cast<void*>(&Stub_SetOutput), reinterpret_cast<void**>(&o_SetOutput));
    const bool noVideoSound = PatchIat(kIatBinkSoundSys, reinterpret_cast<void*>(&Stub_BinkSetSoundSystem), nullptr);

    CreateThread(nullptr, 0, ReaderThread, nullptr, 0, nullptr);
    Logger::Warn("TEST MODE (SHERWOODFIX_TEST=1): hidden window, forced activation, injected input, commands in %scmd.txt; sound off: FMOD=%d videos=%d.",
                 g_dir.c_str(), noSound ? 1 : 0, noVideoSound ? 1 : 0);
}

bool NextInput(InputRecord& out) {
    if (!g_enabled) return false;
    std::lock_guard<std::mutex> lock(g_lock);
    if (g_inputs.empty() || g_inputs.front().dueQpc > Diag::Now()) return false;
    out = g_inputs.front().rec;
    g_inputs.pop_front();
    return true;
}

void OnGameTick() {
    if (!g_enabled) return;
    const int cmd = g_mainCmd.load(std::memory_order_relaxed);
    if (cmd == 0) return;
    if (cmd == 3) {
        // The game's developer console (created on demand by 0x0045BBE0, which returns it; the
        // mission start puts the game object into its first field at 0x0050F850). Its command
        // WIN = 0x0045CDA0, __thiscall(console, int, int), ret 8: "Mission won !" and the
        // campaign goes on as after a won mission (in the camp of Sherwood after mission 1).
        using PFN_GetConsole = void* (__cdecl*)();
        using PFN_ConsoleCommand = void(__thiscall*)(void* console, int, int);
        void* console = reinterpret_cast<PFN_GetConsole>(0x0045BBE0)();
        if (console && *reinterpret_cast<void**>(console)) {
            reinterpret_cast<PFN_ConsoleCommand>(0x0045CDA0)(console, 0, 0);
            Logger::Warn("TEST MODE: console command WIN - the mission counts as won.");
        }
        g_mainCmd.store(0);
        return;
    }
    if (cmd == 6) {
        FILE* f = _fsopen(g_gridPath.c_str(), "w", _SH_DENYNO);
        unsigned counts[5]{}, see = 0;
        if (f) {
            std::fprintf(f, "x,y,class,seeThrough\n");
            for (int y = g_gridArgs[1]; y <= g_gridArgs[3]; y += g_gridArgs[4]) {
                for (int x = g_gridArgs[0]; x <= g_gridArgs[2]; x += g_gridArgs[4]) {
                    int st = 0;
                    const int c = UiScale::ClassifyPointer(static_cast<float>(x), static_cast<float>(y), &st);
                    std::fprintf(f, "%d,%d,%d,%d\n", x, y, c, st);
                    if (c >= 0 && c < 5) ++counts[c];
                    if (st) ++see;
                }
            }
            std::fclose(f);
        }
        std::snprintf(g_zoneqOut, sizeof(g_zoneqOut), "world=%u window=%u worldOffPoint=%u interface=%u seeThroughSkipped=%u",
                      counts[0], counts[1], counts[2], counts[4], see);
        g_mainCmd.store(0);
        return;
    }
    if (cmd == 5) {
        UiScale::DescribePointer(g_zoneqPt[0], g_zoneqPt[1], g_zoneqOut, sizeof(g_zoneqOut));
        g_mainCmd.store(0);
        return;
    }
    if (cmd == 4) {
        // zone map = engine + 0x11C. 0x004E9760 __thiscall(zm, const Vec2* pt, const Vec2* from, Zone** out,
        // u16* layerOut) ret 0x10 -> id or < 0; 0x004E8FC0 the same for one layer (arg 3) -> id, -1 blocked,
        // -2 nothing in that layer's cell; 0x00608400 bbox test __thiscall(box, pt); 0x0060C660 polygon test
        // __thiscall(poly, pt).
        using PFN_Find = short(__thiscall*)(void*, const float*, const float*, void**, uint16_t*);
        using PFN_FindLayer = short(__thiscall*)(void*, const float*, const float*, uint32_t, void**);
        using PFN_Test = bool(__thiscall*)(void*, const float*);
        auto* engine = *reinterpret_cast<uint8_t**>(kAddrEnginePtr);
        int pos = 0;
        auto out = [&](const char* fmt, auto... a) {
            if (pos < static_cast<int>(sizeof(g_zoneqOut)) - 1)
                pos += std::snprintf(g_zoneqOut + pos, sizeof(g_zoneqOut) - pos, fmt, a...);
        };
        if (!engine) {
            out("no engine");
        } else {
            uint8_t* zm = engine + 0x11C;
            const float* pt = g_zoneqPt;
            void* zone = nullptr;
            uint16_t layer = 0xFFFF;
            const short id = reinterpret_cast<PFN_Find>(0x004E9760)(zm, pt, pt, &zone, &layer);
            out("find=%d zone=%08X layer=%d |", id, static_cast<unsigned>(reinterpret_cast<uintptr_t>(zone)), static_cast<short>(layer));
            const unsigned layers = *reinterpret_cast<uint16_t*>(zm + 0x3E9C);
            const unsigned gw = *reinterpret_cast<uint16_t*>(zm + 0x3E98), gh = *reinterpret_cast<uint16_t*>(zm + 0x3E9A);
            for (unsigned l = 0; l < layers && l < 8; ++l) {
                void* z = nullptr;
                const short r = reinterpret_cast<PFN_FindLayer>(0x004E8FC0)(zm, pt, pt, l, &z);
                out(" L%u=%d/%08X", l, r, static_cast<unsigned>(reinterpret_cast<uintptr_t>(z)));
                const int cx = static_cast<int>(pt[0]) >> 6, cy = static_cast<int>(pt[1]) >> 6;
                if (cx < 0 || cy < 0 || cx >= static_cast<int>(gw) || cy >= static_cast<int>(gh)) continue;
                const uint8_t* cell = *reinterpret_cast<uint8_t**>(zm + 0x403C) + ((l * gh + cy) * gw + cx) * 40u;
                const unsigned n = *reinterpret_cast<const uint16_t*>(cell + 0x10);
                auto* const* zs = *reinterpret_cast<uint8_t* const* const*>(cell + 0x14);
                for (unsigned i = 0; i < n && i < 24; ++i) {
                    uint8_t* zz = zs[i];
                    const uint32_t flags = *reinterpret_cast<uint32_t*>(zz + 0x30);
                    if (!(flags & 0x40)) continue;
                    const bool inBox = reinterpret_cast<PFN_Test>(0x00608400)(zz + 0x3C, pt);
                    const bool inPoly = reinterpret_cast<PFN_Test>(0x0060C660)(zz + 4, pt);
                    out(" [%08X f=%X id=%u act=%u box=%d poly=%d]", static_cast<unsigned>(reinterpret_cast<uintptr_t>(zz)), flags,
                        *reinterpret_cast<uint16_t*>(zz + 0x38), zz[0x34], inBox ? 1 : 0, inPoly ? 1 : 0);
                }
            }
        }
        g_mainCmd.store(0);
        return;
    }
    // RHEngine::ZoomInRequest 0x004DBE40 / ZoomOutRequest 0x004DBDB0: __thiscall(E, Event*), ret 4;
    // [evt+8] != 0 = zoom about the screen centre instead of the cursor (docs\ZOOM_CAMERA.md)
    using PFN_ZoomRequest = void(__thiscall*)(void* engine, void* evt);
    auto* engine = *reinterpret_cast<void**>(kAddrEnginePtr);
    if (engine) {
        uint32_t evt[8]{};
        evt[2] = 1;
        const auto fn = reinterpret_cast<PFN_ZoomRequest>(cmd == 1 ? 0x004DBE40 : 0x004DBDB0);
        fn(engine, evt);
    }
    g_mainCmd.store(0);
}

bool TakeShotRequest(std::string& path, bool& scaled) {
    if (!g_enabled || g_shotState.load(std::memory_order_relaxed) != 1) return false;
    path = g_shotPath;
    scaled = g_shotScaled;
    return true;
}

void ShotDone(bool ok, int width, int height) {
    g_shotW = width;
    g_shotH = height;
    g_shotState.store(ok ? 2 : 3);
}

} // namespace TestHarness
