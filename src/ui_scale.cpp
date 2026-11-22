#include "ui_scale.h"
#include "config.h"
#include "coop_probe.h"
#include "input_fix.h"
#include "logger.h"
#include "pattern.h"
#include "test_harness.h"
#include "timing.h"
#include "window_manager.h"
#include "view_zoom.h"
#include "ddm/ddm.h"
#include "ddm/ddm_hd_menu.h"
#include "ddm/ddm_hd_patch.h"

#include <MinHook.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>
#include <intrin.h>
#include <vector>

#pragma intrinsic(_ReturnAddress)

// See ui_scale.h.
//
// How (2026-11-14). The game places its windows in screen pixels from several sources (the
// graphics configuration is read for that in some 60 places, the mission object keeps its own
// copy), so laying them out for a smaller screen is not practical. Instead every window picture
// is drawn twice as large AWAY FROM ITS CORNER: the screen is cut into a 3x3 grid (25 % / 50 % /
// 25 %); a window whose centre lies in the bottom-left cell grows from the bottom-left corner,
// one in the top-middle cell from the top edge about its own centre, one in the middle cell
// about its own centre, and so on. Windows of moderate size (the portrait, the map) get one
// anchor for all their pictures; the two full-screen HUD windows are scaled picture by picture
// (the left hedge from the left corner, the right one from the right corner). Pictures drawn
// outside a window (the tooltip) use the anchor the pointer was mapped with. The mouse position
// the windows see is mapped back with the anchor of the enlarged picture under it, so a click on
// the large picture reaches the small window the game knows.
//
// Game.exe addresses (static RE):
//   0x006189E0 WindowManager::Draw(int)      thiscall, ret 4 - every window of a manager. The
//              mission's manager (RHGame+0x68) is drawn by RHGame::RenderFrame (returns to
//              0x00510618); dialogs with a loop of their own (briefing, menus: 0x0053C565) have
//              their own managers and are left alone here
//   0x00628210 Pointer::Update(InputState*)  thiscall, ret 4 - a manager's pointer (+0x1758) takes
//              the mouse position from the state (s16 x at +0, s16 y at +2) and keeps it at +0x80
//              for every hit test, hover and tooltip
//   0x005C84A0 on-screen message text        thiscall, ret 0xC - from RenderFrame (returns to
//              0x0051068F), centred above the bottom edge: grows from the bottom centre
namespace UiScale {
namespace {
    int  g_factor = 1;
    bool g_uiInstalled = false;     // the mission hooks are in place: the scale can be switched
    bool g_pointerResync = false;   // scale switched off: the pointer goes back under the cursor
    int  g_drawDepth = 0;           // main thread only
    bool g_fixedAnchor = false;     // the message text: one anchor for all its letters
    float g_fixedAx = 0.0f, g_fixedAy = 0.0f;

    using PFN_Draw          = uint32_t(__fastcall*)(void* wm, void* edx, uint32_t arg);
    using PFN_PointerUpdate = uint32_t(__fastcall*)(void* ptr, void* edx, int16_t* state);
    using PFN_Message       = uint32_t(__fastcall*)(void* obj, void* edx, uint32_t a, uint32_t b, uint32_t c);
    PFN_Draw          o_Draw = nullptr;
    PFN_PointerUpdate o_PointerUpdate = nullptr;
    PFN_Message       o_Message = nullptr;

    constexpr uintptr_t kAddrEnginePtr = 0x006C0FA8;
    bool InMission() {
        return *reinterpret_cast<void* const*>(kAddrEnginePtr) != nullptr;
    }

    void ScreenSize(float& w, float& h) {
        int iw = 0, ih = 0;
        WindowManager::GetInternalResolution(iw, ih);
        w = static_cast<float>(iw > 0 ? iw : 2560);
        h = static_cast<float>(ih > 0 ? ih : 1440);
    }

    // anchor of the grid cell (25 % / 50 % / 25 %) that contains (x, y): the HUD lies along the
    // edges, dialogs and the map in the middle
    void Anchor(float x, float y, float w, float h, float& ax, float& ay) {
        ax = x < w * 0.25f ? 0.0f : (x > w * 0.75f ? w : w * 0.5f);
        ay = y < h * 0.25f ? 0.0f : (y > h * 0.75f ? h : h * 0.5f);
    }

    // What was drawn where in the last frame (enlarged rectangle and its anchor): the mouse is
    // mapped back with the anchor of the picture it is over - an enlarged picture may reach into
    // the next grid cell.
    struct Piece {
        float l, t, r, b, ax, ay, f;        // f: the factor it was enlarged with (the map has its own)
        // the picture it shows (NoteSource): the pointer over its colour key is not over the picture
        const void* src = nullptr;
        unsigned srcSerial = 0;
        float sl = 0.0f, st = 0.0f, sr = 0.0f, sb = 0.0f;
        uint16_t key = 0;
        bool keyed = false, mirrorX = false, mirrorY = false;
    };
    std::vector<Piece> g_pieces[2];
    int g_piecesCur = 0;            // being filled now; the other one is the last complete frame
    int g_lastPiece = -1;           // main thread: the piece the last Transform added (NoteSource), -1 = none

    // The opened map's own size ([Display] MapScale, 2026-11-18, user: "an option to choose the minimap's size"):
    // its window is enlarged by this factor instead of the interface's 2 (main thread).
    float g_mapFactor = 2.0f;
    float g_mapFactorUsed = 2.0f;   // what the opened map got last (a tall map at 5x: what fits above the hedge)
    bool  g_lastWasMap = false;     // Transform: the last rectangle was the opened map's
    float g_lastF = 1.0f;           // ... and the factor it got
    float MapF() { return g_factor > 1 ? g_mapFactor : static_cast<float>(g_factor); }

    struct Box { float l = 1e9f, t = 1e9f, r = -1e9f, b = -1e9f; bool any = false; };

    // The window the window loop of WindowManager::Draw is drawing now (0x00618A34 sets it,
    // 0x00618A3F clears it after the window's Draw): a window's rectangle (floats l, t, r, b at
    // +0x70, valid byte at +0x80) gives all its pictures one anchor, so the map or a dialog grows
    // in one piece about its own centre.
    void* volatile g_curWindow = nullptr;
    void* o_WindowLoop = nullptr;
    void* o_WindowDone = nullptr;

    __declspec(naked) void Hook_WindowLoop() {
        __asm {
            push eax
            mov eax, dword ptr [esi + 8]
            mov eax, dword ptr [eax + edi * 4]
            mov dword ptr [g_curWindow], eax
            pop eax
            jmp dword ptr [o_WindowLoop]
        }
    }

    __declspec(naked) void Hook_WindowDone() {
        __asm {
            mov dword ptr [g_curWindow], 0
            jmp dword ptr [o_WindowDone]
        }
    }

    bool Readable(const void* p, size_t n) {
        auto a = reinterpret_cast<uintptr_t>(p);
        const uintptr_t end = a + n;
        while (a < end) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(reinterpret_cast<const void*>(a), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
                (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
                !(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                                 PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
                return false;
            }
            a = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        }
        return true;
    }

    // What each window drew in the last frame (original coordinates) - for the test harness.
    struct Drawn { const void* win; Box box; };
    std::vector<Drawn> g_drawn[2];

    // the anchor the mouse was mapped back with: pictures drawn outside the windows (the tooltip
    // next to the pointer) use it too, so they appear next to the real cursor
    float g_pointerAx = 0.0f, g_pointerAy = 0.0f;
    bool  g_pointerAnchor = false;

    // The opened map moved to the corner (MapPosition=1): its window still lies at its own place
    // for the game (original coordinates) - a pointer there would act on the map although the
    // world is shown there.
    float g_mapOrig[4] = {};
    bool  g_mapOrigValid = false;

    // the opened map as it was drawn last (screen pixels): the whole window and the picture of
    // the level in it - read by the input thread
    std::atomic<float> g_mapShownRect[4];
    std::atomic<float> g_mapContentRect[4];
    std::atomic<bool>  g_mapShown{ false };
    bool               g_mapSeen = false;      // main thread: drawn in the frame being made

    // main thread: the anchor the opened map was drawn with, and a drag of it in progress (see
    // "the opened map: dragging" below)
    float    g_mapAx = 0.0f, g_mapAy = 0.0f;
    bool     g_mapAnchorValid = false;
    uint8_t* g_camDragMap = nullptr;    // the drag started on the map's picture: it moves the camera
    uint8_t* g_winDragMap = nullptr;    // the drag moves the window

    // A point in the game's own coordinates lies on one of the mission's shown windows (rectangle +0x70, valid
    // +0x80, shown +0x5C): a window takes the pointer there whatever the pixel (the game's hit tests are rectangles).
    bool OnMissionWindow(float x, float y) {
        auto* game = *reinterpret_cast<uint8_t* const*>(0x006C0B3C);
        if (!game || !Readable(game + 0x68, 0x10)) return false;
        void** begin = *reinterpret_cast<void***>(game + 0x68 + 8);
        void** end = *reinterpret_cast<void***>(game + 0x68 + 0xC);
        if (!begin || end <= begin || end - begin > 256 || !Readable(begin, static_cast<size_t>(end - begin) * sizeof(void*))) return false;
        for (void** it = begin; it != end; ++it) {
            const auto* win = static_cast<const uint8_t*>(*it);
            if (!win || !Readable(win, 0x84) || !win[0x80] || !win[0x5C]) continue;
            const float* rc = reinterpret_cast<const float*>(win + 0x70);
            if (x >= rc[0] && x < rc[2] && y >= rc[1] && y < rc[3]) return true;
        }
        return false;
    }

    // The pointer at (x, y) on this enlarged picture sees through it: keyed, and its source pixel there is the key.
    bool SeeThrough(const Piece& p, float x, float y) {
        if (!p.keyed || !p.src || p.r <= p.l || p.b <= p.t || p.sr <= p.sl || p.sb <= p.st) return false;
        float u = (x - p.l) / (p.r - p.l), v = (y - p.t) / (p.b - p.t);
        if (p.mirrorX) u = 1.0f - u;
        if (p.mirrorY) v = 1.0f - v;
        const int sx = (std::max)(static_cast<int>(p.sl), (std::min)(static_cast<int>(p.sr) - 1, static_cast<int>(std::floor(p.sl + u * (p.sr - p.sl)))));
        const int sy = (std::max)(static_cast<int>(p.st), (std::min)(static_cast<int>(p.sb) - 1, static_cast<int>(std::floor(p.st + v * (p.sb - p.st)))));
        return DDM::PixelIsKey(p.src, p.srcSerial, sx, sy, p.key);
    }

    // The enlarged picture the pointer at (x, y) is over - the topmost; nullptr: the world. Its see-through pixels
    // are no part of it unless the point they map to lies on a window: the hedge's right end (320x165, 2x from the
    // corner) reaches high above its leaves - the grass below the gate on the carriage map (Croisement02) lay
    // under that empty part, the world got the mapped point beyond the map's edge and the cursor showed "can't
    // go" (user 2026-11-22). The leaves themselves stay as before: the map's very edge behind them is not walked to.
    const Piece* PieceUnder(float x, float y, int* skipped) {
        const auto& last = g_pieces[g_piecesCur ^ 1];
        for (auto it = last.rbegin(); it != last.rend(); ++it) {   // the topmost first
            if (!(x >= it->l && x < it->r && y >= it->t && y < it->b)) continue;
            if (it->keyed && it->src && it->f > 0.0f) {
                const float ox = it->ax + (x - it->ax) / it->f, oy = it->ay + (y - it->ay) / it->f;
                if (!OnMissionWindow(ox, oy) && SeeThrough(*it, x, y)) {
                    if (skipped) ++*skipped;
                    continue;
                }
            }
            return &*it;
        }
        return nullptr;
    }

    void MapBack(float x, float y, float w, float h, float& ox, float& oy) {
        // Over the world (no enlarged picture under the mouse) the windows see the real position:
        // edge scrolling, the selection frame and the like work as without the scale.
        float ax = x, ay = y;
        float f = static_cast<float>(g_factor);
        bool found = false;
        if ((g_camDragMap || g_winDragMap) && g_mapAnchorValid) {
            // the map is being dragged: the pointer belongs to it wherever the cursor is (next to
            // the window the mapping changed, and the dragged window jumped back and forth)
            ax = g_mapAx;
            ay = g_mapAy;
            f = g_mapFactorUsed;
            found = true;
        }
        if (!found) {
            if (const Piece* p = PieceUnder(x, y, nullptr)) {
                ax = p->ax;
                ay = p->ay;
                f = p->f;
                found = true;
            }
        }
        if (!found && g_mapOrigValid && x >= g_mapOrig[0] && x < g_mapOrig[2] && y >= g_mapOrig[1] && y < g_mapOrig[3]) {
            ax = w * 0.5f;      // where the moved map would be for the game: away from it, about the centre
            ay = h * 0.5f;
        }
        ox = ax + (x - ax) / f;
        oy = ay + (y - ay) / f;
        const float g = static_cast<float>(g_factor);
        if (f != g && g > 1.0f) {
            // over the map at its own size: a tooltip drawn after the windows (at the interface's factor) must
            // still appear at the cursor - the anchor that maps the pointer's point there with that factor
            ax = (ox * g - x) / (g - 1.0f);
            ay = (oy * g - y) / (g - 1.0f);
        }
        g_pointerAx = ax;
        g_pointerAy = ay;
        g_pointerAnchor = true;
    }

    // ---- menu zoom (see ui_scale.h)
    // The game's cursor is kept inside a rectangle (floats minX, minY, maxX, maxY at 0x006C12E8;
    // the input thread clamps the cursor to it after every movement, 0x005F4700).
    constexpr uintptr_t kAddrCursorBounds = 0x006C12E8;
    constexpr uintptr_t kAddrCursor       = 0x006C1300;
    bool           g_dialogShown = false;       // the game's dialog loop drew last
    bool           g_missionFramed = true;      // the mission drew a frame since the last dialog loop started
    bool           g_menuZoomPossible = false;
    bool           g_menuZoomEnabled = false;
    bool           g_zoomOn = false;            // main thread
    float          g_savedBounds[4] = {};
    float          g_zoomBounds[4] = {};
    volatile float g_zoomMouse = 1.0f;          // read by the input thread
    volatile bool  g_zoomUnavailable = false;

    // the middle part of a frame, 768 lines high, with the frame's aspect ratio
    bool CropFor(int w, int h, RECT& c) {
        if (w < 1024 || h < 960) return false;
        const int ch = 768;
        int cw = static_cast<int>((static_cast<long long>(w) * ch + h / 2) / h);
        if (cw < 1024) cw = 1024;
        if (cw > w) cw = w;
        c.left = (w - cw) / 2;
        c.top = (h - ch) / 2;
        c.right = c.left + cw;
        c.bottom = c.top + ch;
        return true;
    }

    void MenuZoomOff() {
        if (!g_zoomOn) return;
        auto* bounds = reinterpret_cast<float*>(kAddrCursorBounds);
        if (std::memcmp(bounds, g_zoomBounds, sizeof(g_zoomBounds)) == 0) {
            std::memcpy(bounds, g_savedBounds, sizeof(g_savedBounds));     // unless the game set new ones
        }
        g_zoomOn = false;
        g_zoomMouse = 1.0f;
    }

    // every pass of the game's dialog loop (main menu, briefing, in-game menu, load/save, options)
    void MenuZoomDialog() {
        if (!g_menuZoomEnabled) return;
        if (g_zoomUnavailable) {
            g_menuZoomEnabled = false;
            MenuZoomOff();
            Logger::Warn("UiScale: menu zoom needs the Direct3D 11 presenter - off.");
            return;
        }
        int w = 0, h = 0;
        WindowManager::GetInternalResolution(w, h);
        RECT c{};
        if (!CropFor(w, h, c)) {
            MenuZoomOff();
            return;
        }
        auto* bounds = reinterpret_cast<float*>(kAddrCursorBounds);
        const float mine[4] = { static_cast<float>(c.left), static_cast<float>(c.top),
                                static_cast<float>(c.right), static_cast<float>(c.bottom) };
        const bool entering = !g_zoomOn || std::memcmp(g_zoomBounds, mine, sizeof(mine)) != 0;
        if (!g_zoomOn) {
            std::memcpy(g_savedBounds, bounds, sizeof(g_savedBounds));
        } else if (std::memcmp(bounds, g_zoomBounds, sizeof(g_zoomBounds)) != 0) {
            std::memcpy(g_savedBounds, bounds, sizeof(g_savedBounds));    // the game set new bounds meanwhile
        }
        std::memcpy(g_zoomBounds, mine, sizeof(mine));
        std::memcpy(bounds, mine, sizeof(mine));
        if (entering) {
            // the cursor into the part that is shown (the game clamps it only when it moves)
            auto* cur = reinterpret_cast<float*>(kAddrCursor);
            cur[0] = (std::max)(mine[0], (std::min)(cur[0], mine[2] - 1.0f));
            cur[1] = (std::max)(mine[1], (std::min)(cur[1], mine[3] - 1.0f));
        }
        g_zoomOn = true;
        g_zoomMouse = static_cast<float>(c.bottom - c.top) / static_cast<float>(h);
    }

    void CollectWindows(const uint8_t* wm);
    void FollowMapDrag();
    void PublishMapAtSize(const uint8_t* wm);

    uint32_t __fastcall Hook_Draw(void* wm, void* edx, uint32_t arg) {
        const uintptr_t from = reinterpret_cast<uintptr_t>(_ReturnAddress());
        if (from == 0x0053C565) {
            g_dialogShown = true;
            MenuZoomDialog();
        } else if (from == 0x00510618) {
            if (InMission()) ViewZoom::ClearOutsideLevel();     // the world is drawn: nothing stale beside it
            // A dialog renders the mission once for its backdrop (Timing::InBackdropRender): no frame
            // the player sees - the dialog-after-a-dialog guard, the menu zoom, the smooth zoom's copy
            // (it must stay the one of the frame on the screen, which the dialog takes as the level)
            // and the dragged map stay as they are (code review 2026-11-17).
            if (!Timing::InBackdropRender()) {
                g_dialogShown = false;
                g_missionFramed = true;
                MenuZoomOff();
                if (InMission()) {
                    ViewZoom::OnWorldDrawn();   // the world is drawn, the windows not yet: smooth zoom's copy
                    FollowMapDrag();        // the dragged map where the cursor is now, in every frame
                } else {
                    g_camDragMap = nullptr;
                    g_winDragMap = nullptr;
                }
            } else if (InMission()) {
                // the backdrop's own world, the windows not yet drawn: the dialog's level (taken from
                // this render) is compared with it pixel for pixel
                ViewZoom::OnBackdropWorldDrawn(Timing::BackdropRenderCount());
            }
        }
        const bool scale = g_factor > 1 && InMission() && from == 0x00510618;
        if (DDM::TraceActive()) {
            char text[96];
            std::snprintf(text, sizeof(text), "UI DRAW begin wm=%p from %08X scaled=%d", wm,
                          static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress())), scale ? 1 : 0);
            DDM::TraceMark(text);
        }
        if (scale) {
            ++g_drawDepth;
            g_pieces[g_piecesCur].clear();
            g_drawn[g_piecesCur].clear();
            g_mapSeen = false;
            CollectWindows(static_cast<const uint8_t*>(wm));
        } else if (from == 0x00510618 && InMission()) {
            PublishMapAtSize(static_cast<const uint8_t*>(wm));
        } else if (from == 0x00510618 || from == 0x0053C565) {
            g_mapShown.store(false, std::memory_order_relaxed);
            g_mapAnchorValid = false;
        }
        void* const outerWindow = g_curWindow;      // a window may draw a manager of its own
        g_curWindow = nullptr;
        const uint32_t r = o_Draw(wm, edx, arg);
        g_curWindow = outerWindow;
        if (scale) {
            --g_drawDepth;
            g_piecesCur ^= 1;       // this frame's pictures are now the ones the mouse is mapped with
            g_mapShown.store(g_mapSeen, std::memory_order_relaxed);
            if (!g_mapSeen) g_mapAnchorValid = false;
        }
        if (DDM::TraceActive()) DDM::TraceMark("UI DRAW end");
        return r;
    }

    uint32_t __fastcall Hook_Message(void* obj, void* edx, uint32_t a, uint32_t b, uint32_t c) {
        const bool scale = g_factor > 1 && InMission() && reinterpret_cast<uintptr_t>(_ReturnAddress()) == 0x0051068F;
        if (!scale) {
            return o_Message(obj, edx, a, b, c);
        }
        float w, h;
        ScreenSize(w, h);
        ++g_drawDepth;
        g_fixedAnchor = true;
        g_fixedAx = w * 0.5f;
        g_fixedAy = h;
        const uint32_t r = o_Message(obj, edx, a, b, c);
        g_fixedAnchor = false;
        --g_drawDepth;
        return r;
    }

    // Drawn by RHGame::RenderFrame after the windows, outside any window manager:
    //   0x004C8140 the money text in the top-left corner        thiscall, from 0x0051061B
    //   0x00511B90 the mission clock (missions with a time limit) thiscall, from 0x005106B9
    //   0x005F1CA0 the mouse cursor (cdecl, 1 argument)         from 0x0051070C
    // The money text grows from the top-left corner, the clock in one piece from its grid cell,
    // the cursor about its hot spot (it keeps pointing at the same pixel).
    using PFN_Overlay = void(__fastcall*)(void* obj, void* edx);
    using PFN_Cursor  = void(__cdecl*)(uint32_t hidden);
    PFN_Overlay o_Money = nullptr;
    PFN_Overlay o_Clock = nullptr;
    PFN_Cursor  o_Cursor = nullptr;
    bool     g_cursorDrawing = false;
    unsigned g_cursorSerial = 0;
    bool  g_groupAnchor = false;    // one anchor for everything drawn now: the first picture's cell
    bool  g_groupSet = false;
    float g_groupAx = 0.0f, g_groupAy = 0.0f;

    void __fastcall Hook_Money(void* obj, void* edx) {
        if (g_factor <= 1 || !InMission() || reinterpret_cast<uintptr_t>(_ReturnAddress()) != 0x00510620) {
            o_Money(obj, edx);
            return;
        }
        ++g_drawDepth;
        g_fixedAnchor = true;
        g_fixedAx = 0.0f;
        g_fixedAy = 0.0f;
        o_Money(obj, edx);
        g_fixedAnchor = false;
        --g_drawDepth;
    }

    void __fastcall Hook_Clock(void* obj, void* edx) {
        if (g_factor <= 1 || !InMission() || reinterpret_cast<uintptr_t>(_ReturnAddress()) != 0x005106BE) {
            o_Clock(obj, edx);
            return;
        }
        ++g_drawDepth;
        g_groupAnchor = true;
        g_groupSet = false;
        o_Clock(obj, edx);
        g_groupAnchor = false;
        --g_drawDepth;
    }

    void __cdecl Hook_Cursor(uint32_t hidden) {
        if (g_factor <= 1 || !InMission() || reinterpret_cast<uintptr_t>(_ReturnAddress()) != 0x00510711) {
            o_Cursor(hidden);
            return;
        }
        ++g_drawDepth;
        ++g_cursorSerial;
        g_cursorDrawing = true;
        g_fixedAnchor = true;
        g_fixedAx = static_cast<float>(static_cast<int>(*reinterpret_cast<const float*>(0x006C1300)));
        g_fixedAy = static_cast<float>(static_cast<int>(*reinterpret_cast<const float*>(0x006C1304)));
        o_Cursor(hidden);
        g_fixedAnchor = false;
        g_cursorDrawing = false;
        --g_drawDepth;
    }

    // The mission's window manager is RHGame+0x68, its pointer object +0x1758.
    // RHGame* [0x6C0B3C] (and the engine pointer) stay set when a mission ends: in the menus they
    // point to freed memory. Reading through it crashed the game on leaving a mission (user's
    // session 2026-11-14 22:03, ACCESS_VIOLATION READ game+0x17C0 from a dialog's pointer update):
    // used only while the memory is there and still carries RHGame's vtable (0x00678CA0).
    bool IsMissionPointer(const void* ptr) {
        auto* game = *reinterpret_cast<uint8_t* const*>(0x006C0B3C);
        if (!game || !InMission() || !Readable(game, 4) || !Readable(game + 0x68 + 0x1758, 4) ||
            *reinterpret_cast<const uint32_t*>(game) != 0x00678CA0u) {
            return false;
        }
        return ptr == *reinterpret_cast<void* const*>(game + 0x68 + 0x1758);
    }

    void AfterMissionPointer(const uint8_t* ptr);
    uint32_t PointerUpdate(void* ptr, void* edx, int16_t* state);

    uint32_t __fastcall Hook_PointerUpdate(void* ptr, void* edx, int16_t* state) {
        const uint32_t r = PointerUpdate(ptr, edx, state);
        if (IsMissionPointer(ptr)) AfterMissionPointer(static_cast<const uint8_t*>(ptr));
        return r;
    }

    uint32_t PointerUpdate(void* ptr, void* edx, int16_t* state) {
        if (DDM::TraceActive() && state) {
            char text[128];
            std::snprintf(text, sizeof(text), "POINTER ptr=%p move=%d,%d cursor=%.0f,%.0f mission=%d", ptr, state[0], state[1],
                          *reinterpret_cast<const float*>(0x006C1300), *reinterpret_cast<const float*>(0x006C1304),
                          IsMissionPointer(ptr) ? 1 : 0);
            DDM::TraceMark(text);
        }
        if (g_zoomOn && state && !IsMissionPointer(ptr)) {
            // Menu zoom: the cursor is kept inside the part that is shown, a dialog's pointer only
            // inside the screen - pushed against an edge they would part, and the dialog would
            // see the mouse somewhere else than the cursor is drawn. Its movement is therefore
            // taken from where it is to the real cursor (whole pixels), then set exactly.
            auto* pos = reinterpret_cast<float*>(static_cast<uint8_t*>(ptr) + 0x80);
            const float cx = *reinterpret_cast<const float*>(0x006C1300);
            const float cy = *reinterpret_cast<const float*>(0x006C1304);
            const int16_t dx = state[0], dy = state[1];
            state[0] = static_cast<int16_t>((std::max)(-32000.0f, (std::min)(32000.0f, cx - pos[0] + (cx >= pos[0] ? 0.5f : -0.5f))));
            state[1] = static_cast<int16_t>((std::max)(-32000.0f, (std::min)(32000.0f, cy - pos[1] + (cy >= pos[1] ? 0.5f : -0.5f))));
            const uint32_t r = o_PointerUpdate(ptr, edx, state);
            state[0] = dx;
            state[1] = dy;
            pos[0] = cx;
            pos[1] = cy;
            return r;
        }
        if (g_factor <= 1 && g_pointerResync && state && IsMissionPointer(ptr)) {
            // the scale was switched off: the pointer is at the mapped-back position - put it
            // under the real cursor again (without adding this movement twice)
            g_pointerResync = false;
            auto* pos = reinterpret_cast<float*>(static_cast<uint8_t*>(ptr) + 0x80);
            pos[0] = *reinterpret_cast<const float*>(0x006C1300);
            pos[1] = *reinterpret_cast<const float*>(0x006C1304);
            const int16_t dx = state[0], dy = state[1];
            state[0] = 0;
            state[1] = 0;
            const uint32_t r = o_PointerUpdate(ptr, edx, state);
            state[0] = dx;
            state[1] = dy;
            return r;
        }
        if (g_factor <= 1 || !state || !IsMissionPointer(ptr)) {
            return o_PointerUpdate(ptr, edx, state);
        }
        // The state carries the mouse MOVEMENT (s16 dx, dy); the pointer adds it to its position
        // (+0x80, floats), clamps it, handles the buttons and keeps the state (+0xB0: the last
        // movement - dragging on the map uses it). The windows get the movement from where the
        // pointer was to where the real cursor (0x006C1300) lies on the small window pictures;
        // afterwards the exact position is set (the movement is whole pixels). The state itself
        // is put back: it belongs to the main loop.
        float w, h, px, py;
        ScreenSize(w, h);
        const float cx = *reinterpret_cast<const float*>(0x006C1300);
        const float cy = *reinterpret_cast<const float*>(0x006C1304);
        MapBack(cx, cy, w, h, px, py);
        auto* pos = reinterpret_cast<float*>(static_cast<uint8_t*>(ptr) + 0x80);
        auto whole = [](float v) {
            const float r = v < 0.0f ? v - 0.5f : v + 0.5f;
            return static_cast<int16_t>((std::max)(-32000.0f, (std::min)(32000.0f, r)));
        };
        const int16_t dx = state[0], dy = state[1];
        state[0] = whole(px - pos[0]);
        state[1] = whole(py - pos[1]);
        const uint32_t r = o_PointerUpdate(ptr, edx, state);
        state[0] = dx;
        state[1] = dy;
        pos[0] = px;
        pos[1] = py;
        return r;
    }

    void Transform(float& l, float& t, float& r, float& b);

    // Two drawing routines of the surface manager that write the pixels themselves instead of
    // blitting (surface id 0 = the screen, 1 = its back buffer):
    //   0x005ECDC0 DrawRect(id, const float rect[4], u16 colour, flags)  thiscall, ret 0x10 - the
    //              outline of the camera view on the map
    //   0x005EC5F0 BlendBlit(srcId, const float* srcRect, dstId, const float* dstRect, key,
    //              percent, flags)  thiscall, ret 0x1C - translucent pictures (it stretches when
    //              the rectangles differ in size)
    using PFN_DrawRect  = uint32_t(__fastcall*)(void* mgr, void* edx, int id, const float* rect, uint32_t colour, uint32_t flags);
    using PFN_BlendBlit = uint32_t(__fastcall*)(void* mgr, void* edx, int srcId, const float* srcRect, int dstId,
                                                const float* dstRect, uint32_t key, uint32_t percent, uint32_t flags);
    PFN_DrawRect  o_DrawRect = nullptr;
    PFN_BlendBlit o_BlendBlit = nullptr;

    uint32_t __fastcall Hook_DrawRect(void* mgr, void* edx, int id, const float* rect, uint32_t colour, uint32_t flags) {
        if ((id != 0 && id != 1) || !rect) {
            return o_DrawRect(mgr, edx, id, rect, colour, flags);
        }
        float rc[4] = { rect[0], rect[1], rect[2], rect[3] };
        // the map's camera outline (drawn from 0x0054B3A5): the part the smooth zoom shows
        const bool view = reinterpret_cast<uintptr_t>(_ReturnAddress()) == 0x0054B3AA && ViewZoom::MapViewRect(rc);
        if (DDM::TraceActive()) {
            char text[200];
            std::snprintf(text, sizeof(text), "UI RECT by %08X id=%d (%.1f,%.1f,%.1f,%.1f) -> (%.1f,%.1f,%.1f,%.1f) view=%d depth=%d",
                          static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress())), id, rect[0], rect[1], rect[2], rect[3],
                          rc[0], rc[1], rc[2], rc[3], view ? 1 : 0, g_drawDepth);
            DDM::TraceMark(text);
        }
        if (g_drawDepth <= 0) {
            return o_DrawRect(mgr, edx, id, view ? rc : rect, colour, flags);
        }
        Transform(rc[0], rc[1], rc[2], rc[3]);
        if (rc[2] <= rc[0] || rc[3] <= rc[1]) return 1;     // left out (a dialog's backdrop render)
        const uint32_t r = o_DrawRect(mgr, edx, id, rc, colour, flags);
        // lines as thick as the rest of the enlarged picture (the routine clips to the surface; the map at
        // its own size: its factor, rounded)
        const int thick = (std::max)(1, static_cast<int>(g_lastF + 0.5f));
        for (int i = 1; i < thick; ++i) {
            const float in[4] = { rc[0] + i, rc[1] + i, rc[2] - i, rc[3] - i };
            if (in[2] > in[0] && in[3] > in[1]) o_DrawRect(mgr, edx, id, in, colour, flags);
        }
        return r;
    }

    uint32_t __fastcall Hook_BlendBlit(void* mgr, void* edx, int srcId, const float* srcRect, int dstId,
                                       const float* dstRect, uint32_t key, uint32_t percent, uint32_t flags) {
        if (DDM::TraceActive()) {
            char text[96];
            std::snprintf(text, sizeof(text), "UI BLEND win=%p depth=%d %u%%", g_curWindow, g_drawDepth, percent & 0xFFFF);
            DDM::TraceMark(text);
        }
        if ((dstId != 0 && dstId != 1) || !dstRect || g_drawDepth <= 0) {
            return o_BlendBlit(mgr, edx, srcId, srcRect, dstId, dstRect, key, percent, flags);
        }
        float d[4] = { dstRect[0], dstRect[1], dstRect[2], dstRect[3] };
        float s[4];
        if (srcRect) {
            std::memcpy(s, srcRect, sizeof(s));
        } else {    // the whole picture, drawn at its size
            s[0] = 0.0f;
            s[1] = 0.0f;
            s[2] = d[2] - d[0];
            s[3] = d[3] - d[1];
        }
        if (DDM::TraceActive()) {
            char text[160];
            std::snprintf(text, sizeof(text), "UI BLEND src=%d dst=%d d=(%.0f,%.0f,%.0f,%.0f) s=(%.0f,%.0f,%.0f,%.0f) %u%%", srcId, dstId,
                          d[0], d[1], d[2], d[3], s[0], s[1], s[2], s[3], percent & 0xFFFF);
            DDM::TraceMark(text);
        }
        Transform(d[0], d[1], d[2], d[3]);
        // the routine does not clip: cut both rectangles at the screen edge in proportion
        float w, h;
        ScreenSize(w, h);
        if (d[2] <= d[0] || d[3] <= d[1]) return 1;
        const float sx = (s[2] - s[0]) / (d[2] - d[0]), sy = (s[3] - s[1]) / (d[3] - d[1]);
        if (d[0] < 0.0f) { s[0] -= d[0] * sx; d[0] = 0.0f; }
        if (d[1] < 0.0f) { s[1] -= d[1] * sy; d[1] = 0.0f; }
        if (d[2] > w) { s[2] -= (d[2] - w) * sx; d[2] = w; }
        if (d[3] > h) { s[3] -= (d[3] - h) * sy; d[3] = h; }
        if (d[2] <= d[0] || d[3] <= d[1] || s[2] <= s[0] || s[3] <= s[1]) return 1;
        return o_BlendBlit(mgr, edx, srcId, s, dstId, d, key, percent, flags);
    }

    // The selection frame (dragging with the left button over the world). RHEngine::DrawSelection
    // (0x004D0400, thiscall, ret) draws four lines between the start (engine+0x78) and the end
    // (engine+0x80), both world points (screen = (world - camera at +0x4738) * zoom at +0x475C);
    // +0x74 = the frame is up. The game moves the end only on its 25 ticks a second, the cursor is
    // drawn in every frame - the frame's corner trailed behind the cursor. In the frames between
    // ticks the corner is put under the cursor for the drawing, then the game's value goes back.
    using PFN_DrawSelection = void(__fastcall*)(uint8_t* engine, void* edx);
    PFN_DrawSelection o_DrawSelection = nullptr;

    void __fastcall Hook_DrawSelection(uint8_t* engine, void* edx) {
        if (!engine || !engine[0x74] || !Timing::InExtraFrame()) {
            o_DrawSelection(engine, edx);
            return;
        }
        auto* end = reinterpret_cast<float*>(engine + 0x80);
        const auto* cam = reinterpret_cast<const float*>(engine + 0x4738);
        const float zoom = *reinterpret_cast<const float*>(engine + 0x475C);
        const float saved[2] = { end[0], end[1] };
        if (zoom > 0.01f) {
            float x = *reinterpret_cast<const float*>(0x006C1300);
            float y = *reinterpret_cast<const float*>(0x006C1304);
            ViewZoom::ScreenToFrame(x, y);      // the smooth zoom shows another part of the frame there
            end[0] = cam[0] + x / zoom;
            end[1] = cam[1] + y / zoom;
        }
        o_DrawSelection(engine, edx);
        end[0] = saved[0];
        end[1] = saved[1];
    }

    // The sword swing (2026-11-17, user and a YouTuber: at the smooth zoom's magnifications - in and out - the
    // orange line of a swing was drawn far away from the pointer, and swings did not come out right). In a fight
    // the player holds the button and draws the swing; the event manager ([0x6C0C4C] = RHEngine+0x68) records the
    // pointer's SCREEN positions meanwhile (list +0x5C, filled from the mouse events of the main loop 0x0050FB7C).
    // The engine draws that trail at the end of its object render (0x004D0A10 -> 0x00548800, pixels written into
    // the frame, together with the world - so the smooth zoom magnified and moved it like the world), and
    // recognises the swing from it (0x004E1A40 -> 0x00547600) against the fighter's position from WorldToScreen
    // (frame pixels). While the smooth zoom shows another part of the frame, both routines get the points mapped
    // into the frame (ViewZoom::ScreenToFrame, as the selection frame above), then the game's values are back.
    using PFN_TrailDraw = uint32_t(__fastcall*)(void* mgr, void* edx, uint32_t colour);
    using PFN_SwingRecognise = uint32_t(__fastcall*)(void* mgr, void* edx, void* a, void* b);
    using PFN_ListAt = float*(__fastcall*)(void* list, void* edx, uint32_t index);
    PFN_TrailDraw o_TrailDraw = nullptr;
    PFN_SwingRecognise o_SwingRecognise = nullptr;
    const auto g_fnListAt = reinterpret_cast<PFN_ListAt>(0x00610CB0);   // list::at(i) -> Vec2* (thiscall, ret 4)

    struct TrailInFrame {
        float* p[512];
        float was[512][2];
        int n = 0;
        explicit TrailInFrame(void* mgr) {
            if (!mgr) return;
            auto* list = static_cast<uint8_t*>(mgr) + 0x5C;
            const int count = *reinterpret_cast<const int*>(list + 8);        // 0x0053BF20: [list+8]
            if (count <= 0) return;
            for (int i = 0; i < count && n < 512; ++i) {
                float* v = g_fnListAt(list, nullptr, static_cast<uint32_t>(i));
                if (!v) continue;
                float x = v[0], y = v[1];
                if (!ViewZoom::ScreenToFrame(x, y)) continue;
                p[n] = v;
                was[n][0] = v[0];
                was[n][1] = v[1];
                ++n;
                v[0] = x;
                v[1] = y;
            }
        }
        ~TrailInFrame() {
            for (int i = 0; i < n; ++i) {
                p[i][0] = was[i][0];
                p[i][1] = was[i][1];
            }
        }
    };

    uint32_t __fastcall Hook_TrailDraw(void* mgr, void* edx, uint32_t colour) {
        TrailInFrame inFrame(mgr);
        return o_TrailDraw(mgr, edx, colour);
    }

    using PFN_TrailCrosses = uint32_t(__fastcall*)(void* list, void* edx);
    const auto g_fnTrailCrosses = reinterpret_cast<PFN_TrailCrosses>(0x006107E0);  // al: two segments that are not neighbours meet
    std::atomic<int> g_trailRepeatsLeftOut{ 0 };

    uint32_t __fastcall Hook_SwingRecognise(void* mgr, void* edx, void* a, void* b) {
        TrailInFrame inFrame(mgr);
        // The log part (2026-11-18, YouTuber: "the character does not react to what I draw, normal strikes take
        // a lot of waving, the spin attack works"): what the game recognised, from which stroke - the first
        // 60 swings of a session, and the points themselves of the first 10 (2026-11-19: a stroke from a
        // player's log can be replayed against the game's code). Result 0 = no attack, 1 = a normal strike;
        // the shapes the game knows (Manual.pdf p. 27): lines forward / back / left / right, half circles,
        // circles, the infinity sign. "crossing" = the game's own test whether the stroke crosses itself
        // (0x006107E0); a crossing stroke is never taken for a line.
        static int s_logged = 0;
        const bool log = s_logged < 60 && mgr;
        uint32_t crosses = 0;
        if (log) {
            auto* list = static_cast<uint8_t*>(mgr) + 0x5C;
            const int count = *reinterpret_cast<const int*>(list + 8);
            crosses = g_fnTrailCrosses(list, nullptr) & 0xFF;
            if (s_logged < 10 && count >= 2) {
                char line[1900];
                int len = 0, inLine = 0, part = 0;
                for (int i = 0; i < count && i < 512; ++i) {
                    const float* v = g_fnListAt(list, nullptr, static_cast<uint32_t>(i));
                    if (v) len += std::snprintf(line + len, sizeof(line) - len, " %.0f,%.0f", v[0], v[1]);
                    if (++inLine == 120 || i + 1 == count || i + 1 == 512) {
                        if (len > 0) Logger::Info("Swing %d points %d:%s", s_logged + 1, ++part, line);
                        len = 0;
                        inLine = 0;
                    }
                }
            }
        }
        const uint32_t r = o_SwingRecognise(mgr, edx, a, b);
        if (log) {
            ++s_logged;
            auto* list = static_cast<uint8_t*>(mgr) + 0x5C;
            const int count = *reinterpret_cast<const int*>(list + 8);
            float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f, length = 0.0f, px = 0.0f, py = 0.0f;
            int repeats = 0;
            for (int i = 0; i < count && i < 512; ++i) {
                const float* v = g_fnListAt(list, nullptr, static_cast<uint32_t>(i));
                if (!v) continue;
                x0 = (std::min)(x0, v[0]);
                y0 = (std::min)(y0, v[1]);
                x1 = (std::max)(x1, v[0]);
                y1 = (std::max)(y1, v[1]);
                if (i > 0) {
                    length += std::sqrt((v[0] - px) * (v[0] - px) + (v[1] - py) * (v[1] - py));
                    if (v[0] == px && v[1] == py) ++repeats;
                }
                px = v[0];
                py = v[1];
            }
            const float* who = static_cast<const float*>(a);      // the fighter on the screen (WorldToScreen)
            Logger::Info("Swing: %d points (%d repeated in a row), %.0fx%.0f px, drawn %.0f px (frame pixels; %d of them "
                         "mapped from the screen), crossing %u, fighter at (%.0f,%.0f) -> result %u%s. Repeats left out so far: %d.",
                         count, repeats, count > 0 ? x1 - x0 : 0.0f, count > 0 ? y1 - y0 : 0.0f, length, inFrame.n, crosses,
                         who ? who[0] : 0.0f, who ? who[1] : 0.0f, r, r == 0 ? " (no attack)" : "", g_trailRepeatsLeftOut.load());
        }
        return CoopProbe::OnSwingRecognised(r);   // background test only (co-op probe); r unchanged otherwise
    }

    // Repeated points (2026-11-19, the YouTuber's recordings with and without the mod, and the hidden test: a
    // perfectly straight 600 px stroke had 55 points, 38 of them repeats of the one before -> result 1, a
    // normal strike, for every line). The main loop (0x0050FB33) hands the translator one pointer-move event
    // per pass; with the mod's frames between ticks there are several passes per tick, and the position the
    // event carries moves only with the tick, so the move branch (0x00546A3D) appended the same point 4-5 times
    // in a row. The game's crossing test (0x006107E0: do two segments that are not neighbours meet - touching
    // ends count) then saw the neighbours of each zero-length segment touch, took the stroke for a loop, and
    // the recognition (0x00547600) gave every line a normal strike. Without the mod there is one pass per tick
    // and no repeats. A point equal to the trail's last one is no longer appended, nor its intensity (+0x2C):
    // the list is the one the game has without the mod. The trail is drawn with the intensity running from
    // point to point (0x00548800), so the last point gets the fresh intensity a repeat brought (112.5) - the
    // drawn trail and its fading stay exactly as before.
    constexpr uintptr_t kAddrTrailAppend = 0x00546A68;    // lea ebx,[esi+5Ch] / lea edx,[esi+84h]
    uintptr_t g_trailAppendGo   = 0x00546A71;             // push edx / mov ecx,ebx / call list::push_back (0x00610A60)
    uintptr_t g_trailAppendSkip = 0x00546AC8;             // previous / current pointer position update

    int __stdcall TrailPointRepeats(uint8_t* tr) {
        auto* list = tr + 0x5C;
        const int count = *reinterpret_cast<const int*>(list + 8);
        if (count <= 0) return 0;
        const float* last = g_fnListAt(list, nullptr, static_cast<uint32_t>(count - 1));
        const float* pos = reinterpret_cast<const float*>(tr + 0x84);   // the point the branch would append
        if (!last || last[0] != pos[0] || last[1] != pos[1]) return 0;
        // the intensities: std::list<float> at +0x2C (head node +0x30, size +0x34; node = next, prev, value)
        uint8_t* head = *reinterpret_cast<uint8_t**>(tr + 0x30);
        if (head && *reinterpret_cast<const uint32_t*>(tr + 0x34) == static_cast<uint32_t>(count)) {
            uint8_t* lastNode = *reinterpret_cast<uint8_t**>(head + 4);
            if (lastNode && lastNode != head) *reinterpret_cast<float*>(lastNode + 8) = 112.5f;
        }
        g_trailRepeatsLeftOut.fetch_add(1, std::memory_order_relaxed);
        return 1;
    }

    __declspec(naked) void TrailAppendStub() {
        __asm {
            pushad
            push esi
            call TrailPointRepeats
            test eax, eax
            popad                       // flags stay
            lea ebx, [esi + 0x5C]       // the two instructions the jump replaced
            lea edx, [esi + 0x84]
            jnz skip
            jmp dword ptr [g_trailAppendGo]
        skip:
            jmp dword ptr [g_trailAppendSkip]
        }
    }

    // The game's dialog loop (0x0053C390, thiscall, ret): main menu screens, briefing, in-game
    // menu. Known from its first instruction on - it copies its backdrop to the screen before
    // its window manager draws for the first time.
    using PFN_DialogLoop = void(__fastcall*)(void* dlg, void* edx);
    PFN_DialogLoop o_DialogLoop = nullptr;
    volatile int   g_dialogLoopDepth = 0;

    // 0x0053BB80 shows a dialog (thiscall(manager, dialog, loop, backdrop), ret 0xC; only from
    // DoModal 0x00544B80): it puts the dialog up (0x0053C650), makes the backdrop when asked
    // (0x0053B1F0) and runs the dialog loop when asked. It may start from inside the mission's window manager:
    // a window's click handler runs during its Draw ("mission won - leave the level?", the mission
    // chosen on the camp's map). The dialog and the cursor's background copies are not the
    // mission's interface - with the enlarging still on, every blit of the dialog grew 2x about
    // the screen centre: the parchment twice, the cursor's background put back 2x elsewhere
    // (squares wherever the cursor went, the cursor itself never wiped; 2026-11-17, the camp's
    // mission choice in a YouTuber's game). The mission's drawing state is set aside for the
    // dialog and comes back for the rest of the mission's windows.
    using PFN_ShowDialog = uint32_t(__fastcall*)(void* mgr, void* edx, void* dlg, uint32_t loop, uint32_t backdrop);
    PFN_ShowDialog o_ShowDialog = nullptr;
    bool g_nestedNoted = false;

    uint32_t __fastcall Hook_ShowDialog(void* mgr, void* edx, void* dlg, uint32_t loop, uint32_t backdrop) {
        const int savedDepth = g_drawDepth;
        void* const savedWindow = g_curWindow;
        const bool savedFixed = g_fixedAnchor;
        if (savedDepth > 0 && !g_nestedNoted) {
            g_nestedNoted = true;
            Logger::Info("UiScale: a dialog opened while the mission's windows were drawn (window vtable %08X) - shown at its own size.",
                         savedWindow && Readable(savedWindow, 4) ? *static_cast<const uint32_t*>(savedWindow) : 0u);
        }
        g_drawDepth = 0;
        g_curWindow = nullptr;
        g_fixedAnchor = false;
        if (savedDepth > 0 && g_dialogLoopDepth == 0 && loop) {
            // the level for the dialog's backdrop: the frame being drawn, before the dialog puts
            // itself up and presents (the dialog loop below then keeps it)
            DDM::HD::Menu::NoteDialogStart(true);
            g_missionFramed = false;
        }
        const uint32_t r = o_ShowDialog(mgr, edx, dlg, loop, backdrop);
        g_drawDepth = savedDepth;
        g_curWindow = savedWindow;
        g_fixedAnchor = savedFixed;
        return r;
    }

    void __fastcall Hook_DialogLoop(void* dlg, void* edx) {
        // The level is still on the screen - unless another dialog was shown last (the pages of
        // a briefing follow each other without a frame of the mission in between: the picture of
        // the level taken at the first page stays; taken from the screen, the next page's
        // parchment would count as the level and stay small)
        if (g_dialogLoopDepth == 0 && g_missionFramed) DDM::HD::Menu::NoteDialogStart();
        if (g_dialogLoopDepth == 0) InputFix::OnDialogLoop(true);  // the game's own quick save / load keys
        g_missionFramed = false;
        ++g_dialogLoopDepth;
        o_DialogLoop(dlg, edx);
        --g_dialogLoopDepth;
        // HD letters and pictures of the dialogs (ddm_hd_patch.h): the next one draws itself anew
        // (not at the start: the briefing has drawn its parchment by then)
        if (g_dialogLoopDepth == 0) DDM::HD::Patch::Reset();
    }

    // 0x005ECBD0 DrawLine(id, a, b, c) thiscall, ret 0x10 - who draws which line (the selection frame
    // is four of them): written to the draw trace (test harness) and, once per kind, to the log.
    // The log part (2026-11-16): the orange line of a sword swing (press and move the mouse in a fight)
    // was drawn away from the pointer at the smooth zoom's magnifications - the background test cannot
    // make the game draw a swing (its input reaches only the buffered DirectInput data), so the first
    // lines of every kind the player's game draws say who draws them and where, next to the pointer.
    using PFN_DrawLine = uint32_t(__fastcall*)(void* mgr, void* edx, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4);
    PFN_DrawLine o_DrawLine = nullptr;

    uint32_t __fastcall Hook_DrawLine(void* mgr, void* edx, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4) {
        auto vec = [](uint32_t p, float out[2]) {
            out[0] = out[1] = -1.0f;
            if (p > 0x10000 && Readable(reinterpret_cast<const void*>(p), 8)) std::memcpy(out, reinterpret_cast<const void*>(p), 8);
        };
        if (DDM::TraceActive()) {
            char text[200];
            float v2[2], v3[2];
            vec(a2, v2);
            vec(a3, v3);
            std::snprintf(text, sizeof(text), "LINE by %08X id=%u a=%08X(%.1f,%.1f) b=%08X(%.1f,%.1f) c=%08X",
                          static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress())), a1, a2, v2[0], v2[1], a3, v3[0], v3[1], a4);
            DDM::TraceMark(text);
        }
        static std::vector<uint64_t> seen;          // main thread
        static unsigned s_calls = 0;                // the stack is captured for the first 20000 lines only (a game
        if (seen.size() < 24 && ++s_calls <= 20000) {   // that draws fewer than 24 kinds paid it for every line)
            void* frames[4] = {};
            const USHORT n = RtlCaptureStackBackTrace(1, 4, frames, nullptr);
            uint64_t key = static_cast<uint64_t>(a4 & 0xFFFF) << 48;
            for (USHORT i = 0; i < n && i < 3; ++i) key ^= static_cast<uint64_t>(reinterpret_cast<uintptr_t>(frames[i])) << (i * 16);
            if (std::find(seen.begin(), seen.end(), key) == seen.end()) {
                seen.push_back(key);
                float v2[2], v3[2];
                vec(a2, v2);
                vec(a3, v3);
                float cx = *reinterpret_cast<const float*>(0x006C1300), cy = *reinterpret_cast<const float*>(0x006C1304);
                float fx = cx, fy = cy;
                const bool mapped = ViewZoom::ScreenToFrame(fx, fy);
                Logger::Info("Lines: colour %04X by %08X<%08X<%08X<%08X from (%.0f,%.0f) to (%.0f,%.0f); pointer (%.0f,%.0f), in the frame "
                             "(%.0f,%.0f)%s.", a4 & 0xFFFF, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress())),
                             static_cast<uint32_t>(reinterpret_cast<uintptr_t>(frames[1])), static_cast<uint32_t>(reinterpret_cast<uintptr_t>(frames[2])),
                             static_cast<uint32_t>(reinterpret_cast<uintptr_t>(frames[3])), v2[0], v2[1], v3[0], v3[1], cx, cy, fx, fy,
                             mapped ? " (smooth zoom magnifies)" : "");
            }
        }
        return o_DrawLine(mgr, edx, a1, a2, a3, a4);
    }

    bool Hook(uintptr_t addr, const uint8_t* prolog, size_t n, void* detour, void** original) {
        return std::memcmp(reinterpret_cast<const void*>(addr), prolog, n) == 0 &&
               MH_CreateHook(reinterpret_cast<LPVOID>(addr), detour, original) == MH_OK &&
               MH_EnableHook(reinterpret_cast<LPVOID>(addr)) == MH_OK;
    }

    // The map: one window (vtable 0x00679B70) - the scroll button in the top-right corner, which
    // becomes the opened map (158x246 at 1039,90 on a 2560x1440 screen, the top middle).
    constexpr uint32_t kMapVtable = 0x00679B70;
    constexpr uint32_t kTooltipVtable = 0x00678D18;     // the tooltip window that follows the pointer
    constexpr float kMapMargin = 8.0f;     // original pixels from the right edge
    constexpr float kMapTop = 104.0f;      // original pixels: below the face and the tower buttons
    bool g_mapCorner = false;

    bool IsOpenMap(const void* win, const float rc[4]) {
        return *static_cast<const uint32_t*>(win) == kMapVtable && rc[3] - rc[1] > 120.0f;
    }

    // ---- the opened map: dragging (2026-11-14, user wish) ----------------------------------
    // The map window handles the pointer in 0x0054A640. Pointer flags (Pointer::Update, +0xA4):
    // 0x01 left click (when let go), 0x08 left double click, 0x40 left held, 0x02 right click,
    // 0x80 right held. Over the opened map a left click on the picture of the level centres the
    // camera on that point (0x0054BFB0); with the left button held the window is dragged
    // (0x0054C1E0, thiscall(map, const Vec2* pointer), ret 4: map+0xB0 = drag started, +0xB4 the
    // pointer at the start, +0xBC the window position at the start, +0xB1 = moved - only once
    // the pointer is 15 px from the start); a right click closes the map. The opened window's
    // rectangle is map+0xE4, set by 0x0054A0F0 (thiscall(map, const Vec2* topLeft), ret 4: kept on
    // the screen) and given to the window by 0x0061AD20 (thiscall(win, const Rect*), ret 4). The
    // picture fills the window's rectangle (+0x70) minus the margin at 0x006C0C58 (Vec2: 14, 24)
    // and shows the whole level; map+0xAC = the engine. Changes:
    //  * a drag that starts on the picture moves the camera: the point under the pointer is
    //    centred continuously (0x004C7F60 CenterCameraOn, as the click does), also when the pointer
    //    leaves the map (clamped to its edge). The window is dragged by its edges - with the left
    //    button, or with the right one (input_fix.cpp makes a right press on an edge a left one);
    //  * the dragged window follows the cursor in every drawn frame (the game moves it in its
    //    ticks, 25 times a second, after a dead zone of 15 px - 30 screen pixels with the x2 UI);
    //  * while the map is dragged the pointer is mapped back with the map's anchor (MapBack);
    //  * a drag ends with the button (AfterMissionPointer): let go outside the window, the game
    //    kept its drag going, and the next drag anywhere moved the map.
    constexpr uintptr_t kAddrMapMargin = 0x006C0C58;
    using PFN_MapDrag      = void(__fastcall*)(uint8_t* map, void* edx, const float* pointer);
    using PFN_MapSetPos    = uint8_t(__fastcall*)(uint8_t* map, void* edx, const float* topLeft);
    using PFN_WinSetRect   = void(__fastcall*)(uint8_t* win, void* edx, const float* rect);
    using PFN_EngineTest   = uint32_t(__fastcall*)(uint8_t* engine, void* edx);
    using PFN_CenterCamera = void(__fastcall*)(uint8_t* engine, void* edx, const float* point);
    PFN_MapDrag o_MapDrag = nullptr;
    const auto g_fnMapSetPos    = reinterpret_cast<PFN_MapSetPos>(0x0054A0F0);
    const auto g_fnWinSetRect   = reinterpret_cast<PFN_WinSetRect>(0x0061AD20);
    const auto g_fnCanStartZoom = reinterpret_cast<PFN_EngineTest>(0x004C7C90);
    const auto g_fnCenterCamera = reinterpret_cast<PFN_CenterCamera>(0x004C7F60);
    bool  g_mapDragCamera = true;
    float g_camLast[2] = { -1.0f, -1.0f };
    float g_winLast[2] = { -1e9f, -1e9f };
    std::atomic<bool> g_edgePress{ false };     // input thread: this left press is a right one on an edge

    bool MapContent(const uint8_t* map, float c[4]) {
        const auto* rc = reinterpret_cast<const float*>(map + 0x70);
        const auto* m = reinterpret_cast<const float*>(kAddrMapMargin);
        c[0] = rc[0] + m[0];
        c[1] = rc[1] + m[1];
        c[2] = rc[2] - m[0];
        c[3] = rc[3] - m[1];
        return c[2] - c[0] > 1.0f && c[3] - c[1] > 1.0f;
    }

    // the window and its picture as drawn (anchor and factor of the drawing) - for the input thread
    void PublishMap(const uint8_t* map, float ax, float ay, float f) {
        const auto* rc = reinterpret_cast<const float*>(map + 0x70);
        float c[4];
        if (!MapContent(map, c)) std::memcpy(c, rc, sizeof(c));
        g_mapShownRect[0].store(ax + (rc[0] - ax) * f, std::memory_order_relaxed);
        g_mapShownRect[1].store(ay + (rc[1] - ay) * f, std::memory_order_relaxed);
        g_mapShownRect[2].store(ax + (rc[2] - ax) * f, std::memory_order_relaxed);
        g_mapShownRect[3].store(ay + (rc[3] - ay) * f, std::memory_order_relaxed);
        g_mapContentRect[0].store(ax + (c[0] - ax) * f, std::memory_order_relaxed);
        g_mapContentRect[1].store(ay + (c[1] - ay) * f, std::memory_order_relaxed);
        g_mapContentRect[2].store(ax + (c[2] - ax) * f, std::memory_order_relaxed);
        g_mapContentRect[3].store(ay + (c[3] - ay) * f, std::memory_order_relaxed);
    }

    // the mission's windows drawn at their size: the opened map where the game has it
    void PublishMapAtSize(const uint8_t* wm) {
        g_mapAnchorValid = false;
        bool shown = false;
        if (Readable(wm + 8, 8)) {
            void* const* begin = *reinterpret_cast<void* const* const*>(wm + 8);
            void* const* end = *reinterpret_cast<void* const* const*>(wm + 0xC);
            if (begin && end > begin && end - begin <= 64 && Readable(begin, (end - begin) * sizeof(void*))) {
                for (void* const* it = begin; it < end && !shown; ++it) {
                    auto* win = static_cast<const uint8_t*>(*it);
                    if (!win || !Readable(win, 0x130) || !win[0x80]) continue;
                    if (IsOpenMap(win, reinterpret_cast<const float*>(win + 0x70))) {
                        PublishMap(win, 0.0f, 0.0f, 1.0f);
                        shown = true;
                    }
                }
            }
        }
        g_mapShown.store(shown, std::memory_order_relaxed);
    }

    void MapCentreCamera(uint8_t* map, const float* p) {
        auto* engine = *reinterpret_cast<uint8_t* const*>(map + 0xAC);
        float c[4];
        if (!engine || engine != *reinterpret_cast<uint8_t* const*>(kAddrEnginePtr) || !MapContent(map, c)) return;
        const float u = (std::max)(0.0f, (std::min)(1.0f, (p[0] - c[0]) / (c[2] - c[0])));
        const float v = (std::max)(0.0f, (std::min)(1.0f, (p[1] - c[1]) / (c[3] - c[1])));
        const float pt[2] = { *reinterpret_cast<const float*>(engine + 0x4730) * u,
                              *reinterpret_cast<const float*>(engine + 0x4734) * v };
        if (pt[0] == g_camLast[0] && pt[1] == g_camLast[1]) return;
        if (!(g_fnCanStartZoom(engine, nullptr) & 0xFF)) return;    // a zoom is running: as the click
        g_camLast[0] = pt[0];
        g_camLast[1] = pt[1];
        g_fnCenterCamera(engine, nullptr, pt);
    }

    void NoteDragLevel();

    void __fastcall Hook_MapDrag(uint8_t* map, void* edx, const float* pointer) {
        if (!map || !pointer || !map[0x120]) {      // closed: the original does nothing either
            o_MapDrag(map, edx, pointer);
            return;
        }
        if (!map[0xB0]) {           // the drag starts
            g_camDragMap = nullptr;
            g_winDragMap = nullptr;
            NoteDragLevel();
            g_winLast[0] = g_winLast[1] = -1e9f;
            float c[4];
            if (g_mapDragCamera && !g_edgePress.load(std::memory_order_relaxed) && MapContent(map, c) &&
                pointer[0] >= c[0] && pointer[0] < c[2] && pointer[1] >= c[1] && pointer[1] < c[3]) {
                g_camDragMap = map;
                map[0xB0] = 1;      // the game goes on calling this while the button is held, also off the window
                map[0xB1] = 0;      // letting go over the picture centres once more (the same point)
                map[0xE0] = 0;
                g_camLast[0] = g_camLast[1] = -1.0f;
                MapCentreCamera(map, pointer);
                return;
            }
            o_MapDrag(map, edx, pointer);
            if (map[0xB0]) g_winDragMap = map;
            return;
        }
        if (map == g_camDragMap) {
            MapCentreCamera(map, pointer);
            map[0xE0] = 0;
            return;
        }
        o_MapDrag(map, edx, pointer);
        if (map[0xB0]) g_winDragMap = map;
    }

    // The map window a drag started on belongs to the level of that moment: after the mission ended
    // (a quick load, a scripted end while the button was held) the pointer is freed memory - still
    // committed, so Readable() passes (code review 2026-11-17). The level's surface and the map's
    // vtable are compared before the map is touched again.
    void* g_dragLevel = nullptr;
    void NoteDragLevel() {
        const auto* engine = *reinterpret_cast<const uint8_t* const*>(kAddrEnginePtr);
        g_dragLevel = engine && Readable(engine + 0x48C4, 4) ? *reinterpret_cast<void* const*>(engine + 0x48C4) : nullptr;
    }
    bool DragMapAlive(const uint8_t* map) {
        if (!map || !Readable(map, 0x130) || *reinterpret_cast<const uint32_t*>(map) != kMapVtable) return false;
        const auto* engine = *reinterpret_cast<const uint8_t* const*>(kAddrEnginePtr);
        return engine && Readable(engine + 0x48C4, 4) && g_dragLevel &&
               *reinterpret_cast<void* const*>(engine + 0x48C4) == g_dragLevel;
    }

    // every drawn frame, before the mission's windows: the dragged window where the cursor is now
    void FollowMapDrag() {
        uint8_t* map = g_winDragMap;
        if (!map) return;
        if (!DragMapAlive(map) || !map[0xB0] || !map[0x120]) {
            g_winDragMap = nullptr;
            return;
        }
        float p[2] = { *reinterpret_cast<const float*>(kAddrCursor), *reinterpret_cast<const float*>(kAddrCursor + 4) };
        if (g_factor > 1 && g_mapAnchorValid) {     // as MapBack maps it for the pointer
            const float f = g_mapFactorUsed;
            p[0] = g_mapAx + (p[0] - g_mapAx) / f;
            p[1] = g_mapAy + (p[1] - g_mapAy) / f;
        }
        const auto* start = reinterpret_cast<const float*>(map + 0xB4);
        const auto* from = reinterpret_cast<const float*>(map + 0xBC);
        const float dx = p[0] - start[0], dy = p[1] - start[1];
        if (!map[0xB1] && dx * dx + dy * dy < 4.0f) return;    // 2 px instead of the game's 15
        const float pos[2] = { from[0] + dx, from[1] + dy };
        if (pos[0] == g_winLast[0] && pos[1] == g_winLast[1]) return;
        g_winLast[0] = pos[0];
        g_winLast[1] = pos[1];
        g_fnMapSetPos(map, nullptr, pos);                                           // as the game's drag (0x0054C2CE)
        g_fnWinSetRect(map, nullptr, reinterpret_cast<const float*>(map + 0xE4));    // and its next event (0x0054A77C)
        map[0xB1] = 1;
    }

    // main thread, after every update of the mission's pointer: a drag ends with the button
    void AfterMissionPointer(const uint8_t* ptr) {
        if (!g_camDragMap && !g_winDragMap) return;
        if (*reinterpret_cast<const uint16_t*>(ptr + 0xA4) & 0x40) return;     // still held
        uint8_t* const maps[2] = { g_camDragMap, g_winDragMap };
        for (uint8_t* map : maps) {
            if (DragMapAlive(map)) map[0xB0] = 0;
        }
        g_camDragMap = nullptr;
        g_winDragMap = nullptr;
    }

    // The mission manager's windows, read when its drawing begins. A window of moderate size is
    // anchored by its rectangle - or by the rectangle around all windows of its class (the
    // portraits of the selected characters form a row from the bottom-left corner: they grow
    // together, the row stays closed). A full-width window is scaled picture by picture: its
    // ends belong to different corners.
    struct WinInfo { const void* win; uint32_t vt; float rc[4]; };
    // perWindow: the windows of the class lie so far apart that each can grow from its own place
    // without covering the next (otherwise the whole row grows together)
    struct Group { uint32_t vt; Box box; int count = 0; float minGap = 1e9f, maxW = 0.0f; bool perWindow = false; };
    WinInfo g_wins[64];
    int     g_winCount = 0;
    Group   g_groups[64];
    int     g_groupCount = 0;
    int     g_lastWin = 0;

    bool Moderate(float l, float t, float r, float b, float w, float h) {
        return r > l && b > t && r - l <= w * 0.5f && b - t <= h * 0.5f;
    }

    void ForgetGoneAnchors();

    void CollectWindows(const uint8_t* wm) {
        g_winCount = 0;
        g_groupCount = 0;
        g_lastWin = 0;
        g_mapOrigValid = false;
        if (!Readable(wm + 8, 8)) return;
        void* const* begin = *reinterpret_cast<void* const* const*>(wm + 8);
        void* const* end = *reinterpret_cast<void* const* const*>(wm + 0xC);
        if (!begin || end <= begin || end - begin > 64 || !Readable(begin, (end - begin) * sizeof(void*))) return;
        float w, h;
        ScreenSize(w, h);
        for (void* const* it = begin; it < end; ++it) {
            auto* win = static_cast<const uint8_t*>(*it);
            // +0x5C: the window is shown (vtable +0x44 returns it; the portraits' Draw stops at once
            // without it). In the camp every merry man has a portrait window, the hidden ones stacked
            // at one place - counted along, the gap between portraits was 0 and the row was enlarged
            // from the corner: portraits off the screen ("4 selected, 3 portraits", YouTuber 2026-11-17)
            if (!win || !Readable(win, 0x84) || !win[0x80] || !win[0x5C]) continue;
            WinInfo& info = g_wins[g_winCount];
            info.win = win;
            info.vt = *reinterpret_cast<const uint32_t*>(win);
            std::memcpy(info.rc, win + 0x70, sizeof(info.rc));
            if (!Moderate(info.rc[0], info.rc[1], info.rc[2], info.rc[3], w, h)) continue;
            if (g_mapCorner && IsOpenMap(win, info.rc)) {
                std::memcpy(g_mapOrig, info.rc, sizeof(g_mapOrig));
                g_mapOrigValid = true;
            }
            ++g_winCount;
            Group* g = nullptr;
            for (int i = 0; i < g_groupCount; ++i) {
                if (g_groups[i].vt == info.vt) { g = &g_groups[i]; break; }
            }
            if (!g) {
                // a whole new group: the slot still held last frame's numbers - the count of windows
                // grew without end, the smallest gap and the widest window were remembered from any
                // earlier frame, so the row-or-each decision (and its "same number of windows" guard
                // of 1.2.0) depended on history: portraits in odd places (YouTuber, 2026-11-17)
                g = &g_groups[g_groupCount++];
                *g = Group{};
                g->vt = info.vt;
            }
            g->box.l = (std::min)(g->box.l, info.rc[0]);
            g->box.t = (std::min)(g->box.t, info.rc[1]);
            g->box.r = (std::max)(g->box.r, info.rc[2]);
            g->box.b = (std::max)(g->box.b, info.rc[3]);
            g->box.any = true;
            ++g->count;
            g->maxW = (std::max)(g->maxW, info.rc[2] - info.rc[0]);
        }
        // distances between the windows of each class (left edges, in order)
        for (int gi = 0; gi < g_groupCount; ++gi) {
            Group& g = g_groups[gi];
            if (g.count < 2) continue;
            float lefts[64];
            int n = 0;
            for (int i = 0; i < g_winCount; ++i) {
                if (g_wins[i].vt == g.vt && n < 64) lefts[n++] = g_wins[i].rc[0];
            }
            std::sort(lefts, lefts + n);
            for (int i = 1; i < n; ++i) g.minGap = (std::min)(g.minGap, lefts[i] - lefts[i - 1]);
            g.perWindow = g.minGap >= g.maxW * static_cast<float>(g_factor);
            // The decision stays while the class has the same number of windows (2026-11-17, a YouTuber: "the
            // portraits rearrange"): the selected character's portrait is wider (its action buttons), and with it
            // the row switched between "each from its own place" and "the whole row from the corner" whenever the
            // selection changed - the portraits jumped. Decided anew only when a character joins or leaves.
            struct Kept { uint32_t vt; int count; bool perWindow; };
            static Kept kept[16];
            static int keptN = 0;
            Kept* k = nullptr;
            for (int i = 0; i < keptN; ++i) {
                if (kept[i].vt == g.vt) { k = &kept[i]; break; }
            }
            if (k && k->count == g.count) {
                g.perWindow = k->perWindow;
            } else if (k) {
                *k = Kept{ g.vt, g.count, g.perWindow };
            } else if (keptN < 16) {
                kept[keptN++] = Kept{ g.vt, g.count, g.perWindow };
            }
        }
        ForgetGoneAnchors();
    }

    // A window keeps the anchor it got when it appeared (at this size): the game lets the player
    // drag some windows (the map) - computed anew from the moving rectangle every frame, the
    // enlarged window would not follow the mouse and only shake in place.
    struct KeptAnchor { const void* win; float w, h, ax, ay, f; };
    std::vector<KeptAnchor> g_kept;

    KeptAnchor* FindKept(const void* win) {
        for (KeptAnchor& k : g_kept) {
            if (k.win == win) return &k;
        }
        return nullptr;
    }

    // after CollectWindows: anchors of windows that are gone are forgotten
    void ForgetGoneAnchors() {
        g_kept.erase(std::remove_if(g_kept.begin(), g_kept.end(), [](const KeptAnchor& k) {
            for (int i = 0; i < g_winCount; ++i) {
                if (g_wins[i].win == k.win) return false;
            }
            return true;
        }), g_kept.end());
    }

    // the rectangle that anchors the window (its group's or its own); false: picture by picture
    bool WindowRect(const void* win, float w, float h, float rc[4]) {
        if (!win || g_winCount == 0) return false;
        const WinInfo* info = nullptr;
        if (g_lastWin < g_winCount && g_wins[g_lastWin].win == win) {
            info = &g_wins[g_lastWin];
        } else {
            for (int i = 0; i < g_winCount; ++i) {
                if (g_wins[i].win == win) { info = &g_wins[i]; g_lastWin = i; break; }
            }
        }
        if (!info) return false;
        std::memcpy(rc, info->rc, sizeof(info->rc));
        if (info->vt == kMapVtable) return true;    // the map: its own rectangle (see IsOpenMap)
        for (int i = 0; i < g_groupCount; ++i) {
            const Box& b = g_groups[i].box;
            if (g_groups[i].vt == info->vt && !g_groups[i].perWindow && Moderate(b.l, b.t, b.r, b.b, w, h)) {
                rc[0] = b.l;
                rc[1] = b.t;
                rc[2] = b.r;
                rc[3] = b.b;
                break;
            }
        }
        return true;
    }

    // the window's class (vtable) from this frame's list; 0 when it is not a window of the list
    uint32_t WindowVt(const void* win) {
        if (!win) return 0;
        if (g_lastWin < g_winCount && g_wins[g_lastWin].win == win) return g_wins[g_lastWin].vt;
        for (int i = 0; i < g_winCount; ++i) {
            if (g_wins[i].win == win) return g_wins[i].vt;
        }
        return 0;
    }

    // several windows of this class lie far apart, each grows from its own place
    bool PerWindowClass(uint32_t vt) {
        for (int i = 0; i < g_groupCount; ++i) {
            if (g_groups[i].vt == vt) return g_groups[i].perWindow && g_groups[i].count > 1;
        }
        return false;
    }

    void NoteDrawn(const void* win, float l, float t, float r, float b) {
        auto& list = g_drawn[g_piecesCur];
        Drawn* d = nullptr;
        for (auto it = list.rbegin(); it != list.rend(); ++it) {
            if (it->win == win) { d = &*it; break; }
        }
        if (!d) {
            if (list.size() >= 256) return;
            list.push_back(Drawn{ win, Box{} });
            d = &list.back();
        }
        d->box.l = (std::min)(d->box.l, l);
        d->box.t = (std::min)(d->box.t, t);
        d->box.r = (std::max)(d->box.r, r);
        d->box.b = (std::max)(d->box.b, b);
        d->box.any = true;
    }

    void Transform(float& l, float& t, float& r, float& b) {
        float w, h, ax, ay;
        ScreenSize(w, h);
        g_lastPiece = -1;
        const void* win = g_curWindow;
        float rc[4];
        float fw = static_cast<float>(g_factor);    // this rectangle's factor (the opened map: its own size)
        bool isMap = false;
        g_lastWasMap = false;
        g_lastF = fw;
        // A dialog's backdrop render (Timing::InBackdropRender): the windows in the middle of the
        // screen are left out (an empty rectangle: the callers draw nothing). The dialog lies there,
        // and the window that opened it had drawn the same parchment 2x there, which stuck out
        // around the dialog shown 1.875x - a doubled title above "mission won - leave the level?" and
        // "start the chosen mission?" (2026-11-17). The edges' windows stay as the player sees them.
        if (Timing::InBackdropRender()) {
            float cx = (l + r) * 0.5f, cy = (t + b) * 0.5f;
            if (WindowRect(win, w, h, rc)) {
                cx = (rc[0] + rc[2]) * 0.5f;
                cy = (rc[1] + rc[3]) * 0.5f;
            }
            float mx = 0.0f, my = 0.0f;
            Anchor(cx, cy, w, h, mx, my);
            if (mx == w * 0.5f && my == h * 0.5f) {
                r = l;
                b = t;
                return;
            }
        }
        NoteDrawn(win, l, t, r, b);
        if (g_fixedAnchor) {
            ax = g_fixedAx;
            ay = g_fixedAy;
        } else if (g_groupAnchor) {
            if (!g_groupSet) {
                Anchor((l + r) * 0.5f, (t + b) * 0.5f, w, h, g_groupAx, g_groupAy);
                g_groupSet = true;
            }
            ax = g_groupAx;
            ay = g_groupAy;
        } else if (WindowRect(win, w, h, rc)) {
            const bool openMap = IsOpenMap(win, rc);
            float f = openMap ? MapF() : static_cast<float>(g_factor);
            if (openMap) {
                // a large map stays above the hedge along the bottom edge (the tall maps - Nottingham, Derby,
                // 246 px - at 5x: about 4.4x)
                const float g = static_cast<float>(g_factor);
                const float fit = (h - 80.0f * g - kMapTop * g) / (std::max)(1.0f, rc[3] - rc[1]);
                if (f > fit) f = (std::max)(1.5f, std::floor(fit * 100.0f) / 100.0f);
                g_mapFactorUsed = f;
            }
            fw = f;
            isMap = openMap;
            // Only the map keeps the anchor it got when it appeared (the player drags it). Windows
            // the game itself moves - the skills tooltip next to the pointer in the camp, the
            // portraits of a new selection - took the old anchor along and were drawn away from
            // their place or not at all on the screen (YouTuber's report 2026-11-17).
            const uint32_t vt = WindowVt(win);
            KeptAnchor* kept = vt == kMapVtable ? FindKept(win) : nullptr;
            if (kept && kept->w == rc[2] - rc[0] && kept->h == rc[3] - rc[1] && kept->f == f) {
                ax = kept->ax;      // the anchor it got when it appeared (it may have been dragged since)
                ay = kept->ay;
            } else if (vt == kTooltipVtable) {
                // the tooltip next to the pointer (the skills of a merry man in the camp, the names of
                // the buttons): it grows right and down from its corner at the pointer, where the game
                // put it (grown from its middle it slid under the pointer); kept on the screen
                const float wd = (rc[2] - rc[0]) * f, ht = (rc[3] - rc[1]) * f;
                const float left = (std::max)(0.0f, (std::min)(w - wd, rc[0]));
                const float top = (std::max)(0.0f, (std::min)(h - ht, rc[1]));
                ax = static_cast<float>(static_cast<int>((f * rc[0] - left) / (f - 1.0f)));
                ay = static_cast<float>(static_cast<int>((f * rc[1] - top) / (f - 1.0f)));
            } else if (g_mapCorner && openMap) {
                // the opened map in the top-right corner, below the corner's pictures (those keep the
                // interface's factor): the anchor that puts its enlarged top-right corner there (f >= 1.5)
                const float g = static_cast<float>(g_factor);
                const float x0 = std::floor(w - kMapMargin * g - f * (rc[2] - rc[0]));
                const float y0 = kMapTop * g;
                ax = (f * rc[0] - x0) / (f - 1.0f);
                ay = (f * rc[1] - y0) / (f - 1.0f);
            } else {
                // one anchor for the whole window: its corner, edge or - in the middle - its
                // centre (whole pixels: the enlarged rectangles stay exactly twice as large)
                const float cx = (rc[0] + rc[2]) * 0.5f, cy = (rc[1] + rc[3]) * 0.5f;
                Anchor(cx, cy, w, h, ax, ay);
                if (ax == w * 0.5f) ax = static_cast<float>(static_cast<int>(cx));
                if (ay == h * 0.5f) ay = static_cast<float>(static_cast<int>(cy));
                if (ay == h && PerWindowClass(vt)) {
                    // Portraits of several selected characters: the game gives the team 5 fixed places
                    // spread over the whole width (at 2560: x = 225 + 499 * place) - at 1024 they lie
                    // close together, at 2560 doubled the gaps were wider than the portraits ("gaps",
                    // YouTuber 2026-11-17). The places stay where they are relative to each other (a
                    // portrait never jumps when the selection changes), the distances shrink to the
                    // proportions of a 2048 wide screen (1024 doubled); the first place stays where a
                    // single portrait is (225); kept on the screen.
                    const float wd = (rc[2] - rc[0]) * f;
                    const float first = 0.1f * (w - 65.0f) - 24.5f;
                    const float k = (std::min)(1.0f, 2048.0f / w);
                    float left = first + (rc[0] - first) * k;
                    left = (std::max)(0.0f, (std::min)(w - wd, left));
                    ax = static_cast<float>(static_cast<int>((f * rc[0] - left) / (f - 1.0f)));
                } else if (ay == h && ax == 0.0f) {
                    // the row of portraits at the bottom left: it keeps the distance from the
                    // screen edge the game gave it (grown from the corner, the portrait sat twice
                    // as far in - "Robin pulled to the right"), unless the row would then run off
                    // the screen
                    ax = static_cast<float>(static_cast<int>(rc[0]));
                    if (ax + (rc[2] - ax) * f > w - 8.0f) ax = static_cast<float>(static_cast<int>((f * rc[2] - (w - 8.0f)) / (f - 1.0f)));
                    if (ax + (rc[0] - ax) * f < 0.0f) ax = 0.0f;
                }
            }
            if (vt == kMapVtable && (!kept || kept->w != rc[2] - rc[0] || kept->h != rc[3] - rc[1] || kept->f != f)) {
                if (!kept && g_kept.size() < 64) {
                    g_kept.push_back(KeptAnchor{ win, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f });
                    kept = &g_kept.back();
                }
                if (kept) *kept = KeptAnchor{ win, rc[2] - rc[0], rc[3] - rc[1], ax, ay, f };
            }
            if (openMap) {
                // where the enlarged map lies (input thread: its rolled-up ends close it, its
                // edges drag it); while it is dragged the pointer is mapped with its anchor
                g_mapAx = ax;
                g_mapAy = ay;
                g_mapAnchorValid = true;
                PublishMap(static_cast<const uint8_t*>(win), ax, ay, f);
                g_mapSeen = true;
            }
        } else if (!win && g_pointerAnchor) {
            // drawn by the manager after its windows: the tooltip, placed at the pointer
            ax = g_pointerAx;
            ay = g_pointerAy;
        } else {
            Anchor((l + r) * 0.5f, (t + b) * 0.5f, w, h, ax, ay);
        }
        l = ax + (l - ax) * fw;
        r = ax + (r - ax) * fw;
        t = ay + (t - ay) * fw;
        b = ay + (b - ay) * fw;
        auto& cur = g_pieces[g_piecesCur];
        if (cur.size() < 4096) {
            cur.push_back(Piece{ l, t, r, b, ax, ay, fw });
            g_lastPiece = static_cast<int>(cur.size()) - 1;
        }
        g_lastWasMap = isMap;
        g_lastF = fw;
    }

    // The mission's drawing state while a dialog's backdrop render runs (BeginBackdropRender).
    struct DrawState {
        int   depth = 0;
        void* window = nullptr;
        bool  fixedAnchor = false, groupAnchor = false, groupSet = false, cursorDrawing = false;
        float fixedAx = 0.0f, fixedAy = 0.0f, groupAx = 0.0f, groupAy = 0.0f;
        int   piecesCur = 0;
        std::vector<Piece> pieces[2];
        std::vector<Drawn> drawn[2];
        bool  mapSeen = false, mapShown = false, mapAnchorValid = false, mapOrigValid = false;
        float mapAx = 0.0f, mapAy = 0.0f, mapOrig[4] = {};
        WinInfo wins[64];
        int   winCount = 0, lastWin = 0;
        Group groups[64];
        int   groupCount = 0;
    };
    DrawState g_beforeBackdrop;
    int g_backdropNesting = 0;
}

void BeginBackdropRender() {
    if (g_backdropNesting++ > 0) return;
    DrawState& s = g_beforeBackdrop;
    s.depth = g_drawDepth;
    s.window = g_curWindow;
    s.fixedAnchor = g_fixedAnchor;
    s.fixedAx = g_fixedAx;
    s.fixedAy = g_fixedAy;
    s.groupAnchor = g_groupAnchor;
    s.groupSet = g_groupSet;
    s.groupAx = g_groupAx;
    s.groupAy = g_groupAy;
    s.cursorDrawing = g_cursorDrawing;
    s.piecesCur = g_piecesCur;
    for (int i = 0; i < 2; ++i) {
        s.pieces[i] = g_pieces[i];
        s.drawn[i] = g_drawn[i];
    }
    s.mapSeen = g_mapSeen;
    s.mapShown = g_mapShown.load(std::memory_order_relaxed);
    s.mapAnchorValid = g_mapAnchorValid;
    s.mapAx = g_mapAx;
    s.mapAy = g_mapAy;
    s.mapOrigValid = g_mapOrigValid;
    std::memcpy(s.mapOrig, g_mapOrig, sizeof(s.mapOrig));
    std::memcpy(s.wins, g_wins, sizeof(s.wins));
    s.winCount = g_winCount;
    s.lastWin = g_lastWin;
    std::memcpy(static_cast<void*>(s.groups), g_groups, sizeof(s.groups));
    s.groupCount = g_groupCount;
    // The render draws the world and the windows as a frame of its own: the world outside any window
    // (else its translucent tree tops went through the windows' enlarging - moved, cut off or left
    // out behind "start the chosen mission?" in the camp, YouTuber's report 2026-11-17), the windows
    // from depth 0 as in every frame.
    g_drawDepth = 0;
    g_curWindow = nullptr;
    g_fixedAnchor = false;
    g_groupAnchor = false;
    g_groupSet = false;
    g_cursorDrawing = false;
}

void EndBackdropRender() {
    if (g_backdropNesting <= 0 || --g_backdropNesting > 0) return;
    DrawState& s = g_beforeBackdrop;
    g_drawDepth = s.depth;
    g_curWindow = s.window;
    g_fixedAnchor = s.fixedAnchor;
    g_fixedAx = s.fixedAx;
    g_fixedAy = s.fixedAy;
    g_groupAnchor = s.groupAnchor;
    g_groupSet = s.groupSet;
    g_groupAx = s.groupAx;
    g_groupAy = s.groupAy;
    g_cursorDrawing = s.cursorDrawing;
    g_piecesCur = s.piecesCur;
    for (int i = 0; i < 2; ++i) {
        g_pieces[i].swap(s.pieces[i]);
        g_drawn[i].swap(s.drawn[i]);
    }
    g_mapSeen = s.mapSeen;
    g_mapShown.store(s.mapShown, std::memory_order_relaxed);
    g_mapAnchorValid = s.mapAnchorValid;
    g_mapAx = s.mapAx;
    g_mapAy = s.mapAy;
    g_mapOrigValid = s.mapOrigValid;
    std::memcpy(g_mapOrig, s.mapOrig, sizeof(g_mapOrig));
    std::memcpy(g_wins, s.wins, sizeof(g_wins));
    g_winCount = s.winCount;
    g_lastWin = s.lastWin;
    std::memcpy(static_cast<void*>(g_groups), s.groups, sizeof(g_groups));
    g_groupCount = s.groupCount;
}

int Factor() {
    return g_factor;
}

bool CursorDrawing() {
    return g_cursorDrawing;
}

bool MenuZoomCrop(int frameW, int frameH, RECT& crop) {
    if (!g_menuZoomEnabled || !g_zoomOn) return false;
    int w = 0, h = 0;
    WindowManager::GetInternalResolution(w, h);
    return frameW == w && frameH == h && CropFor(frameW, frameH, crop);
}

float MenuZoomMouseScale() {
    return g_zoomMouse;
}

bool MenuCropRect(int frameW, int frameH, RECT& crop) {
    return CropFor(frameW, frameH, crop);
}

void MenuZoomUnavailable() {
    g_zoomUnavailable = true;
}

bool DialogShown() {
    return g_dialogShown;
}

bool DialogLoopRunning() {
    return g_dialogLoopDepth > 0;
}

bool EngineAlive() {
    return InMission();
}

bool OnMapScrollEnd(float x, float y) {
    if (!g_mapShown.load(std::memory_order_relaxed)) return false;
    const float l = g_mapShownRect[0].load(std::memory_order_relaxed);
    const float t = g_mapShownRect[1].load(std::memory_order_relaxed);
    const float r = g_mapShownRect[2].load(std::memory_order_relaxed);
    const float b = g_mapShownRect[3].load(std::memory_order_relaxed);
    if (x < l || x >= r || y < t || y >= b) return false;
    const float band = (b - t) * 0.085f;    // the rolled-up ends of the parchment
    return y < t + band || y >= b - band;
}

bool OnMapEdge(float x, float y) {
    if (!g_mapShown.load(std::memory_order_relaxed)) return false;
    const float l = g_mapShownRect[0].load(std::memory_order_relaxed);
    const float t = g_mapShownRect[1].load(std::memory_order_relaxed);
    const float r = g_mapShownRect[2].load(std::memory_order_relaxed);
    const float b = g_mapShownRect[3].load(std::memory_order_relaxed);
    if (x < l || x >= r || y < t || y >= b) return false;
    const float cl = g_mapContentRect[0].load(std::memory_order_relaxed);
    const float ct = g_mapContentRect[1].load(std::memory_order_relaxed);
    const float cr = g_mapContentRect[2].load(std::memory_order_relaxed);
    const float cb = g_mapContentRect[3].load(std::memory_order_relaxed);
    return x < cl || x >= cr || y < ct || y >= cb;
}

void NoteEdgePress(bool on) {
    g_edgePress.store(on, std::memory_order_relaxed);
}

bool MapDragAvailable() {
    return o_MapDrag != nullptr;
}

bool MapDragCamera() {
    return g_mapDragCamera;
}

void SetMapDragCamera(bool on) {
    if (on == g_mapDragCamera || (on && !o_MapDrag)) return;
    g_mapDragCamera = on;
    Logger::Info("UiScale: dragging on the map moves %s (settings panel).", on ? "the camera" : "the window");
}

bool InterfaceScaleAvailable() {
    return g_uiInstalled;
}

bool InterfaceScaled() {
    return g_factor > 1;
}

void SetInterfaceScale(bool on) {
    if (!g_uiInstalled || on == (g_factor > 1)) return;
    g_factor = on ? 2 : 1;
    if (!on) g_pointerResync = true;
    Logger::Info("UiScale: mission interface %s (settings panel).", on ? "x2" : "at its size");
}

bool MapInCorner() {
    return g_mapCorner;
}

void SetMapInCorner(bool on) {
    g_mapCorner = on;
}

bool MenuZoomAvailable() {
    return g_menuZoomPossible && !g_zoomUnavailable;
}

bool MenuZoomOn() {
    return g_menuZoomEnabled;
}

void SetMenuZoom(bool on) {
    if (!MenuZoomAvailable() || on == g_menuZoomEnabled) return;
    if (!on) MenuZoomOff();
    g_menuZoomEnabled = on;
    Logger::Info("UiScale: menu zoom %s (settings panel).", on ? "on" : "off");
}

unsigned CursorSerial() {
    return g_cursorSerial;
}

bool Drawing() {
    // never inside a dialog's loop: the dialog is no part of the mission's interface (Hook_ShowDialog)
    return g_factor > 1 && g_drawDepth > 0 && g_dialogLoopDepth == 0;
}

void ScaleRect(RECT& rc) {
    float l = static_cast<float>(rc.left), t = static_cast<float>(rc.top);
    float r = static_cast<float>(rc.right), b = static_cast<float>(rc.bottom);
    Transform(l, t, r, b);
    if (!g_lastWasMap) {
        // anchors are whole pixels (0, W/2, W) and the factor is 2: the results are whole too
        rc.left = static_cast<LONG>(l);
        rc.top = static_cast<LONG>(t);
        rc.right = static_cast<LONG>(r);
        rc.bottom = static_cast<LONG>(b);
        return;
    }
    // the map at its own size (2.5x ...): rounded corner, rounded size - the same picture keeps one size
    // wherever the window is dragged (DDM::ScaledPicture16 keeps a scaled copy per size)
    rc.left = static_cast<LONG>(std::lround(l));
    rc.top = static_cast<LONG>(std::lround(t));
    rc.right = rc.left + static_cast<LONG>(std::lround(r - l));
    rc.bottom = rc.top + static_cast<LONG>(std::lround(b - t));
}

bool LastScaledWasMap() {
    return g_lastWasMap;
}

void SetMapScale(int percent) {
    const float f = static_cast<float>((std::max)(150, (std::min)(500, percent))) / 100.0f;
    if (f == g_mapFactor) return;
    g_mapFactor = f;
    Logger::Info("UiScale: opened map x%.2f (settings panel).", f);
}

void ScaleRectF(float* ltrb) {
    Transform(ltrb[0], ltrb[1], ltrb[2], ltrb[3]);
}

void NoteSource(const void* surface, unsigned serial, const RECT& s, bool keyed, uint16_t key, bool mirrorX, bool mirrorY) {
    auto& cur = g_pieces[g_piecesCur];
    if (g_lastPiece < 0 || g_lastPiece >= static_cast<int>(cur.size())) return;
    Piece& p = cur[static_cast<size_t>(g_lastPiece)];
    p.src = surface;
    p.srcSerial = serial;
    p.sl = static_cast<float>(s.left);
    p.st = static_cast<float>(s.top);
    p.sr = static_cast<float>(s.right);
    p.sb = static_cast<float>(s.bottom);
    p.key = key;
    p.keyed = keyed;
    p.mirrorX = mirrorX;
    p.mirrorY = mirrorY;
    g_lastPiece = -1;
}

int DescribePointer(float x, float y, char* out, int size) {
    if (!out || size < 64) return 0;
    int skipped = 0;
    const Piece* p = PieceUnder(x, y, &skipped);
    if (!p) {
        return std::snprintf(out, size, "world pieces=%u seeThrough=%d", static_cast<unsigned>(g_pieces[g_piecesCur ^ 1].size()), skipped);
    }
    const float ox = p->ax + (x - p->ax) / p->f, oy = p->ay + (y - p->ay) / p->f;
    return std::snprintf(out, size, "piece %.0f,%.0f,%.0f,%.0f anchor=%.0f,%.0f f=%.2f keyed=%d src=%p s=%.0f,%.0f,%.0f,%.0f -> %.1f,%.1f onWindow=%d seeThroughHere=%d skipped=%d",
                         p->l, p->t, p->r, p->b, p->ax, p->ay, p->f, p->keyed ? 1 : 0, p->src, p->sl, p->st, p->sr, p->sb, ox, oy,
                         OnMissionWindow(ox, oy) ? 1 : 0, SeeThrough(*p, x, y) ? 1 : 0, skipped);
}

int DescribeWindows(char* out, int size) {
    if (!out || size < 64) return 0;
    auto* game = *reinterpret_cast<uint8_t* const*>(0x006C0B3C);
    if (!game || !InMission() || !Readable(game + 0x68, 0x10)) return std::snprintf(out, size, "no mission");
    uint8_t* wm = game + 0x68;
    void** begin = *reinterpret_cast<void***>(wm + 8);
    void** end = *reinterpret_cast<void***>(wm + 0xC);
    const int count = begin && end > begin ? static_cast<int>(end - begin) : 0;
    int len = std::snprintf(out, size, "windows=%d", count);
    const auto& drawn = g_drawn[g_piecesCur ^ 1];
    for (int i = 0; i < count && len < size - 200; ++i) {
        auto* win = static_cast<uint8_t*>(begin[i]);
        if (!Readable(win, 0x84)) {
            len += std::snprintf(out + len, size - len, " [%d]=%p ?", i, win);
            continue;
        }
        const float* rc = reinterpret_cast<const float*>(win + 0x70);
        len += std::snprintf(out + len, size - len, " [%d]=%p vt=%08X rc=%.0f,%.0f,%.0f,%.0f ok=%d f=%d%d", i, win,
                             *reinterpret_cast<const uint32_t*>(win), rc[0], rc[1], rc[2], rc[3], win[0x80], win[0x5C], win[0x5D]);
        for (const Drawn& d : drawn) {
            if (d.win == win && d.box.any) {
                len += std::snprintf(out + len, size - len, " drew=%.0f,%.0f,%.0f,%.0f", d.box.l, d.box.t, d.box.r, d.box.b);
            }
        }
    }
    for (const Drawn& d : drawn) {
        if (!d.win && d.box.any && len < size - 80) {
            len += std::snprintf(out + len, size - len, " [none] drew=%.0f,%.0f,%.0f,%.0f", d.box.l, d.box.t, d.box.r, d.box.b);
        }
    }
    return len;
}

void Install() {
    // The selection frame follows the cursor between ticks too (any resolution, any scale).
    static const uint8_t kSelection[] = { 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x6A, 0xFF, 0x68, 0x20, 0xD3, 0x65, 0x00, 0x50 };
    if (Hook(0x004D0400, kSelection, sizeof(kSelection), reinterpret_cast<void*>(&Hook_DrawSelection),
             reinterpret_cast<void**>(&o_DrawSelection))) {
        Logger::Info("UiScale: the selection frame follows the cursor in the frames between ticks.");
    }
    // the dialog loop: sub esp,10h / push ebx / push ebp / push esi / mov esi,ecx / push edi
    static const uint8_t kDialogLoop[] = { 0x83, 0xEC, 0x10, 0x53, 0x55, 0x56, 0x8B, 0xF1, 0x57 };
    if (!Hook(0x0053C390, kDialogLoop, sizeof(kDialogLoop), reinterpret_cast<void*>(&Hook_DialogLoop),
              reinterpret_cast<void**>(&o_DialogLoop))) {
        Logger::Warn("UiScale: dialog loop not recognised - dialogs in a mission are zoomed with their backdrop.");
    }
    // showing a dialog: push ebx / mov ebx,[esp+0Ch] / push esi / push edi / mov edi,[esp+10h]
    static const uint8_t kShowDialog[] = { 0x53, 0x8B, 0x5C, 0x24, 0x0C, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x10 };
    if (!Hook(0x0053BB80, kShowDialog, sizeof(kShowDialog), reinterpret_cast<void*>(&Hook_ShowDialog),
              reinterpret_cast<void**>(&o_ShowDialog))) {
        Logger::Warn("UiScale: dialog start not recognised - a dialog opened from a mission window may be drawn 2x.");
    }
    const auto& cfg = SherwoodConfig::Get();
    // the opened map: dragging on its picture moves the camera, its edges drag the window
    static const uint8_t kMapDrag[] = { 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x6A, 0xFF, 0x68, 0x48, 0x87, 0x66, 0x00, 0x50 };
    static const uint8_t kMapSetPos[] = { 0x6A, 0xFF, 0x68, 0xC8, 0x84, 0x66, 0x00, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x50 };
    static const uint8_t kCenter[] = { 0x6A, 0xFF, 0x68, 0x40, 0xC4, 0x65, 0x00, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x50 };
    g_mapDragCamera = cfg.mapDragCamera;
    if (std::memcmp(reinterpret_cast<const void*>(0x0054A0F0), kMapSetPos, sizeof(kMapSetPos)) == 0 &&
        std::memcmp(reinterpret_cast<const void*>(0x004C7F60), kCenter, sizeof(kCenter)) == 0 &&
        Hook(0x0054C1E0, kMapDrag, sizeof(kMapDrag), reinterpret_cast<void*>(&Hook_MapDrag), reinterpret_cast<void**>(&o_MapDrag))) {
        Logger::Info("UiScale: map - dragging on its picture moves the camera %s, the window is dragged by its edges (left or right button) and follows the cursor in every frame.",
                     g_mapDragCamera ? "(on)" : "(off: MapDragCamera=0, the window moves as in the game)");
    } else {
        g_mapDragCamera = false;
        Logger::Warn("UiScale: map window code not recognised - the map is dragged as in the game.");
    }

    // The hooks go in whenever the feature is possible (it can be switched in the settings panel);
    // with the feature off they only pass the calls on.
    const bool ui = cfg.width >= 2048;
    const bool menu = cfg.height >= 960 && cfg.renderer == 1 && cfg.presenter == 0;
    if (!ui) {
        Logger::Info("UiScale: no interface scale (internal width %d < 2048).", cfg.width);
    }
    if (!menu) {
        Logger::Info("UiScale: no menu zoom (internal height %d, Renderer=%d, Presenter=%d).", cfg.height, cfg.renderer, cfg.presenter);
    }
    static const uint8_t kDrawOnly[] = { 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x6A, 0xFF, 0x68, 0x30, 0x58, 0x67, 0x00, 0x50 };
    auto finishMenuZoom = [&]() {
        g_menuZoomPossible = menu && o_Draw != nullptr;
        g_menuZoomEnabled = g_menuZoomPossible && cfg.menuZoom;
        if (g_menuZoomPossible) {
            Logger::Info("UiScale: menu zoom %s - menus and dialogs shown on the whole window (the middle 768 lines of the frame).",
                         g_menuZoomEnabled ? "on" : "off (MenuZoom=0)");
        }
    };
    static const uint8_t kPointerOnly[] = { 0x6A, 0xFF, 0x68, 0x68, 0x65, 0x67, 0x00, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x50 };
    if (!ui) {
        // the dialog loop's window manager drawing tells when a dialog is up; the dialogs'
        // pointers are kept under the cursor (Hook_PointerUpdate)
        if (!Hook(0x006189E0, kDrawOnly, sizeof(kDrawOnly), reinterpret_cast<void*>(&Hook_Draw), reinterpret_cast<void**>(&o_Draw)) ||
            !Hook(0x00628210, kPointerOnly, sizeof(kPointerOnly), reinterpret_cast<void*>(&Hook_PointerUpdate),
                  reinterpret_cast<void**>(&o_PointerUpdate))) {
            if (o_Draw) MH_DisableHook(reinterpret_cast<LPVOID>(0x006189E0));
            o_Draw = nullptr;
            Logger::Warn("UiScale: window manager code not recognised - no menu zoom.");
        }
        finishMenuZoom();
        return;
    }
    static const uint8_t kDraw[]    = { 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x6A, 0xFF, 0x68, 0x30, 0x58, 0x67, 0x00, 0x50 };
    static const uint8_t kPointer[] = { 0x6A, 0xFF, 0x68, 0x68, 0x65, 0x67, 0x00, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x50 };
    static const uint8_t kMessage[] = { 0x6A, 0xFF, 0x68, 0xD8, 0x03, 0x67, 0x00, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x50 };
    // window loop of WindowManager::Draw: mov eax,[esi+8]; mov ecx,[eax+edi*4]; mov eax,[ecx];
    // call [eax+14h] | mov eax,[esp+18h]; inc edi
    static const uint8_t kLoop[]    = { 0x8B, 0x46, 0x08, 0x8B, 0x0C, 0xB8, 0x8B, 0x01, 0xFF, 0x50, 0x14 };
    static const uint8_t kDone[]    = { 0x8B, 0x44, 0x24, 0x18, 0x47 };
    static const uint8_t kRect[]    = { 0x6A, 0xFF, 0x68, 0xD3, 0x29, 0x67, 0x00, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x50 };
    static const uint8_t kBlend[]   = { 0x6A, 0xFF, 0x68, 0x60, 0x29, 0x67, 0x00, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x50 };
    static const uint8_t kMoney[]   = { 0x6A, 0xFF, 0x68, 0x92, 0xC4, 0x65, 0x00, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x50 };
    static const uint8_t kClock[]   = { 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x6A, 0xFF, 0x68, 0x30, 0x27, 0x66, 0x00, 0x50 };
    static const uint8_t kCursor[]  = { 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x6A, 0xFF, 0x68, 0x20, 0x2F, 0x67, 0x00, 0x50 };
    const bool known = std::memcmp(reinterpret_cast<const void*>(0x006189E0), kDraw, sizeof(kDraw)) == 0 &&
                       std::memcmp(reinterpret_cast<const void*>(0x00628210), kPointer, sizeof(kPointer)) == 0 &&
                       std::memcmp(reinterpret_cast<const void*>(0x005C84A0), kMessage, sizeof(kMessage)) == 0 &&
                       std::memcmp(reinterpret_cast<const void*>(0x00618A34), kLoop, sizeof(kLoop)) == 0 &&
                       std::memcmp(reinterpret_cast<const void*>(0x00618A3F), kDone, sizeof(kDone)) == 0 &&
                       std::memcmp(reinterpret_cast<const void*>(0x005ECDC0), kRect, sizeof(kRect)) == 0 &&
                       std::memcmp(reinterpret_cast<const void*>(0x005EC5F0), kBlend, sizeof(kBlend)) == 0 &&
                       std::memcmp(reinterpret_cast<const void*>(0x004C8140), kMoney, sizeof(kMoney)) == 0 &&
                       std::memcmp(reinterpret_cast<const void*>(0x00511B90), kClock, sizeof(kClock)) == 0 &&
                       std::memcmp(reinterpret_cast<const void*>(0x005F1CA0), kCursor, sizeof(kCursor)) == 0;
    if (!known) {
        Logger::Warn("UiScale: the window code does not match this build - interface stays at its size.");
        if (!menu || !Hook(0x006189E0, kDrawOnly, sizeof(kDrawOnly), reinterpret_cast<void*>(&Hook_Draw), reinterpret_cast<void**>(&o_Draw)) ||
            !Hook(0x00628210, kPointerOnly, sizeof(kPointerOnly), reinterpret_cast<void*>(&Hook_PointerUpdate),
                  reinterpret_cast<void**>(&o_PointerUpdate))) {
            if (o_Draw) MH_DisableHook(reinterpret_cast<LPVOID>(0x006189E0));
            o_Draw = nullptr;
        }
        finishMenuZoom();
        return;
    }
    g_factor = 1;       // until every hook is in place
    g_mapCorner = cfg.mapPosition == 1;
    const bool ok = Hook(0x006189E0, kDraw, sizeof(kDraw), reinterpret_cast<void*>(&Hook_Draw), reinterpret_cast<void**>(&o_Draw)) &&
                    Hook(0x005C84A0, kMessage, sizeof(kMessage), reinterpret_cast<void*>(&Hook_Message), reinterpret_cast<void**>(&o_Message)) &&
                    Hook(0x00628210, kPointer, sizeof(kPointer), reinterpret_cast<void*>(&Hook_PointerUpdate), reinterpret_cast<void**>(&o_PointerUpdate)) &&
                    Hook(0x00618A34, kLoop, sizeof(kLoop), reinterpret_cast<void*>(&Hook_WindowLoop), &o_WindowLoop) &&
                    Hook(0x00618A3F, kDone, sizeof(kDone), reinterpret_cast<void*>(&Hook_WindowDone), &o_WindowDone) &&
                    Hook(0x005ECDC0, kRect, sizeof(kRect), reinterpret_cast<void*>(&Hook_DrawRect), reinterpret_cast<void**>(&o_DrawRect)) &&
                    Hook(0x005EC5F0, kBlend, sizeof(kBlend), reinterpret_cast<void*>(&Hook_BlendBlit), reinterpret_cast<void**>(&o_BlendBlit)) &&
                    Hook(0x004C8140, kMoney, sizeof(kMoney), reinterpret_cast<void*>(&Hook_Money), reinterpret_cast<void**>(&o_Money)) &&
                    Hook(0x00511B90, kClock, sizeof(kClock), reinterpret_cast<void*>(&Hook_Clock), reinterpret_cast<void**>(&o_Clock)) &&
                    Hook(0x005F1CA0, kCursor, sizeof(kCursor), reinterpret_cast<void*>(&Hook_Cursor), reinterpret_cast<void**>(&o_Cursor));
    if (!ok) {
        // Half a chain is worse than none: the menu zoom with no pointer hook puts the dialogs' clicks away from the
        // cursor (audit 2026-11-22). The hooks that went in are taken out again.
        const std::pair<uintptr_t, void**> chain[] = {
            { 0x006189E0, reinterpret_cast<void**>(&o_Draw) },        { 0x005C84A0, reinterpret_cast<void**>(&o_Message) },
            { 0x00628210, reinterpret_cast<void**>(&o_PointerUpdate) }, { 0x00618A34, &o_WindowLoop },
            { 0x00618A3F, &o_WindowDone },                              { 0x005ECDC0, reinterpret_cast<void**>(&o_DrawRect) },
            { 0x005EC5F0, reinterpret_cast<void**>(&o_BlendBlit) },   { 0x004C8140, reinterpret_cast<void**>(&o_Money) },
            { 0x00511B90, reinterpret_cast<void**>(&o_Clock) },       { 0x005F1CA0, reinterpret_cast<void**>(&o_Cursor) } };
        for (const auto& [addr, orig] : chain) {
            if (!*orig) continue;
            MH_DisableHook(reinterpret_cast<LPVOID>(addr));
            *orig = nullptr;
        }
    }
    finishMenuZoom();
    {
        static const uint8_t kLine[] = { 0x6A, 0xFF, 0x68, 0x78, 0x29, 0x67, 0x00, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x50 };
        Hook(0x005ECBD0, kLine, sizeof(kLine), reinterpret_cast<void*>(&Hook_DrawLine), reinterpret_cast<void**>(&o_DrawLine));
        // the sword swing's trail and its recognition under the smooth zoom (see TrailInFrame)
        static const uint8_t kTrail[] = { 0x6A, 0xFF, 0x68, 0xC0, 0x81, 0x66, 0x00, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00 };
        static const uint8_t kSwing[] = { 0x6A, 0xFF, 0x68, 0xEA, 0x80, 0x66, 0x00, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00 };
        const bool trail = Hook(0x00548800, kTrail, sizeof(kTrail), reinterpret_cast<void*>(&Hook_TrailDraw),
                                reinterpret_cast<void**>(&o_TrailDraw));
        const bool swing = Hook(0x00547600, kSwing, sizeof(kSwing), reinterpret_cast<void*>(&Hook_SwingRecognise),
                                reinterpret_cast<void**>(&o_SwingRecognise));
        Logger::Info("UiScale: sword swing under the smooth zoom - trail %d, recognition %d.", trail ? 1 : 0, swing ? 1 : 0);
        // the same point is not appended to a swing again (see TrailAppendStub)
        static const uint8_t kAppend[] = { 0x8D, 0x5E, 0x5C, 0x8D, 0x96, 0x84, 0x00, 0x00, 0x00, 0x52, 0x8B, 0xCB };
        bool noRepeats = false;
        if (std::memcmp(reinterpret_cast<const void*>(kAddrTrailAppend), kAppend, sizeof(kAppend)) == 0) {
            uint8_t jmp[9] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90 };
            const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&TrailAppendStub) - (kAddrTrailAppend + 5));
            std::memcpy(&jmp[1], &rel, 4);
            noRepeats = Memory::PatchBytes(kAddrTrailAppend, jmp, sizeof(jmp));
        }
        Logger::Info("UiScale: a swing's trail gets no repeated points (one per tick, as without the mod's frames between ticks): %s.",
                     noRepeats ? "on" : "NOT installed (bytes differ)");
    }
    if (!ok) {
        g_factor = 1;
        Logger::Warn("UiScale: hooks could not be installed - interface stays at its size.");
        return;
    }
    g_uiInstalled = true;
    g_factor = cfg.uiScale >= 2 ? 2 : 1;
    g_mapFactor = static_cast<float>((std::max)(150, (std::min)(500, cfg.mapScale))) / 100.0f;
    g_mapFactorUsed = g_mapFactor;
    Logger::Info("UiScale: mission interface %s (each window grows from its screen corner, middle windows about their centre; mouse mapped back); "
                 "map %s, opened map x%.2f (MapScale), its picture: %s (MinimapHD=%d).",
                 g_factor > 1 ? "x2" : "at its size (UIScale=0, can be switched in the settings panel)",
                 g_mapCorner ? "in the top-right corner" : "where the game opens it", g_mapFactor,
                 cfg.minimapHd >= 3 ? "made from the level's HD picture, tinted like parchment" :
                 cfg.minimapHd == 2 ? "made from the level's HD picture" :
                 cfg.minimapHd == 1 ? "the game's, upscaled" : "the game's own", cfg.minimapHd);
}

} // namespace UiScale
