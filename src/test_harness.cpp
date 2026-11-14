#include "test_harness.h"
#include "diag.h"
#include "logger.h"
#include "settings_menu.h"
#include "ui_scale.h"
#include "view_zoom.h"
#include "window_manager.h"
#include "ddm/ddm.h"
#include "ddm/ddm_hd.h"
#include "ddm/ddm_hd_menu.h"
#include "ddm/ddm_hd_sprites.h"

#include <MinHook.h>
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

    bool ReadMem(uintptr_t addr, void* out, size_t n) {
        __try {
            std::memcpy(out, reinterpret_cast<const void*>(addr), n);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
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
        char text[1200];
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
        } else if (!std::strcmp(cmd, "key")) {
            unsigned dik = 0;
            int hold = 80;
            const int n = std::sscanf(args, "%i %d", reinterpret_cast<int*>(&dik), &hold);
            if (n < 1 || dik == 0 || dik > 0xFF) { Ack(seq, "err usage: key DIK [HOLD_MS]"); return; }
            Enqueue(1, dik, 0x80, 20.0);
            Enqueue(1, dik, 0x00, static_cast<double>(hold));
            Enqueue(0, 0, 0, 60.0);
            Ack(seq, WaitQueueEmpty(static_cast<DWORD>(hold) + 5000) ? "ok" : "err timeout");
        } else if (!std::strcmp(cmd, "shot") || !std::strcmp(cmd, "shotout")) {
            char name[128]{};
            if (std::sscanf(args, "%127s", name) != 1) { Ack(seq, "err usage: shot NAME"); return; }
            g_shotPath = g_dir + name + ".bmp";
            g_shotScaled = (cmd[4] == 'o');
            g_shotState.store(1);
            const DWORD t0 = GetTickCount();
            while (g_shotState.load() == 1 && GetTickCount() - t0 < 5000) Sleep(5);
            const int st = g_shotState.exchange(0);
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
            float z = 0.0f;
            if (std::sscanf(args, "%f", &z) == 1 && z > 0.0f) {
                ViewZoom::TestSetZoom(z);
                Sleep(400);
            }
            char text[320];
            ViewZoom::Describe(text, sizeof(text));
            Ack(seq, "ok %s", text);
        } else if (!std::strcmp(cmd, "menuhd")) {
            // HD menu pictures: counters and the key of the menu picture seen last
            char text[256];
            DDM::HD::Menu::Describe(text, sizeof(text));
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
        } else if (!std::strcmp(cmd, "dbg")) {
            // leave out drawing routines (bit mask, see DDM::HD::SetDebugFlags); dbg 0 = everything on
            unsigned flags = 0;
            std::sscanf(args, "%i", reinterpret_cast<int*>(&flags));
            DDM::HD::SetDebugFlags(flags);
            Sleep(120);
            Ack(seq, "ok dbg=%u", DDM::HD::DebugFlags());
        } else if (!std::strcmp(cmd, "trace")) {
            // the next N surface calls (Blt / Lock / Flip / present) as text: what a frame is made of
            int count = 0;
            char name[128]{};
            if (std::sscanf(args, "%d %127s", &count, name) != 2 || count <= 0 || count > 200000) {
                Ack(seq, "err usage: trace COUNT NAME");
                return;
            }
            const std::string path = g_dir + name + ".txt";
            DDM::TraceCalls(path.c_str(), count);
            const DWORD t0 = GetTickCount();
            while (DDM::TraceActive() && GetTickCount() - t0 < 8000) Sleep(10);
            const bool done = !DDM::TraceActive();
            DDM::TraceCalls(nullptr, 0); // closes the file if the count was not reached
            Ack(seq, "%s %s", done ? "ok" : "ok (fewer calls than asked for in 8 s)", path.c_str());
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
    signed char __stdcall Stub_SetOutput(int /*outputType*/) {
        return o_SetOutput ? o_SetOutput(0 /* FSOUND_OUTPUT_NOSOUND */) : 1;
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
