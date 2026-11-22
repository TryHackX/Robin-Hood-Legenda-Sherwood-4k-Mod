#include "widescreen_patch.h"
#include "config.h"
#include "ddm/ddm_hd.h"
#include "ui_scale.h"
#include "view_zoom.h"
#include "logger.h"
#include "pattern.h"
#include <MinHook.h>
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>

#include <atomic>
#include <cmath>
#include <intrin.h>

namespace {
    // RHEngine* global singleton in Game.exe
    constexpr uintptr_t kAddrRHEnginePtr = 0x006C0FA8;

    uintptr_t         g_addrZoomMidSetCamRet      = 0x004C8EC1;
    uintptr_t         g_addrApplyGraphicConfigRet = 0x0052CEAC;
    std::atomic<bool> g_userSelectedMenuRes{false};

    using GetGraphicConfig_t    = uint8_t* (__fastcall*)(void* thisPtr, void* edx);
    using ClampCameraPosition_t = uint8_t  (__fastcall*)(uint8_t* thisPtr, void* edx, float* pos);
    using ClampCameraAndZoom_t  = float*   (__cdecl*)(float* outRet, const float* inPos);
    using SetCameraPosition_t   = void     (__fastcall*)(uint8_t* thisPtr, void* edx, float* pos);
    using GetLocalizedString_t  = uint8_t* (__cdecl*)(uint8_t* outStr, uint32_t stringId);
    using WStringGrow_t         = uint8_t  (__thiscall*)(void* thisPtr, size_t newLen, int bTrim);

    GetGraphicConfig_t    g_OrigGetGraphicConfig    = nullptr;
    ClampCameraPosition_t g_OrigClampCameraPosition = nullptr;
    ClampCameraAndZoom_t  g_OrigClampCameraAndZoom  = nullptr;
    SetCameraPosition_t   g_OrigSetCameraPosition   = nullptr;
    GetLocalizedString_t  g_OrigGetLocalizedString  = nullptr;
    WStringGrow_t         g_fnWStringGrow           = reinterpret_cast<WStringGrow_t>(0x004239E0);

    wchar_t g_btn3Label[32] = L"3840 * 2160";

    // Full-height view: the rows the hedge along the bottom covers (80 at the interface's 1x, 160 at 2x).
    // The world is drawn down to the screen edge, but the camera may go so far that the level's bottom
    // edge lies just above the hedge (before 1.2.5 it lay at the screen edge, its last rows behind the
    // hedge - "the bottom of the screen is cut off", YouTuber's video 2026-11-18). The six camera limits
    // of the engine read this value (fsub [g_cameraStrip], see PatchFullHeightView).
    float g_cameraStrip = 0.0f;

    // Safely clamp camera coordinates to [0.0f, max(0.0f, mapSize - viewportSize / zoom)]
    // Prevents negative camera coordinates when playing in 1080p/1440p/4K on smaller maps (e.g. Sherwood Hideout).
    void EnforceSafeCameraBounds(uint8_t* engPtr, float* pos) {
        if (!engPtr || !pos) {
            return;
        }

        const float viewportW = *reinterpret_cast<const float*>(engPtr + 0x4728);
        const float viewportH = *reinterpret_cast<const float*>(engPtr + 0x472C);
        const float mapW      = *reinterpret_cast<const float*>(engPtr + 0x4730);
        const float mapH      = *reinterpret_cast<const float*>(engPtr + 0x4734);
        float zoom            = *reinterpret_cast<const float*>(engPtr + 0x475C);

        if (zoom <= 0.001f) {
            zoom = 1.0f;
        }

        if (mapW > 0.0f && viewportW > 0.0f) {
            const float effViewW = viewportW / zoom;
            const float maxCamX = std::max(0.0f, mapW - effViewW);
            pos[0] = std::clamp(pos[0], 0.0f, maxCamX);
        } else if (pos[0] < 0.0f) {
            pos[0] = 0.0f;
        }

        if (mapH > 0.0f && viewportH > 0.0f) {
            // Game HUD bottom bar occupies 80px in logical space (0x00677598 = 80.0f); with the
            // full-height view the hedge's own rows (g_cameraStrip): the level's bottom may come above it
            const float hudStrip = SherwoodConfig::Get().fullHeightView ? g_cameraStrip : 80.0f;
            const float effViewH = std::max(0.0f, viewportH - hudStrip) / zoom;
            const float maxCamY = std::max(0.0f, mapH - effViewH);
            pos[1] = std::clamp(pos[1], 0.0f, maxCamY);
        } else if (pos[1] < 0.0f) {
            pos[1] = 0.0f;
        }
    }

    uint8_t* __fastcall Hook_GetGraphicConfig(void* thisPtr, void* edx) {
        uint8_t* cfgPtr = g_OrigGetGraphicConfig(thisPtr, edx);
        if (cfgPtr) {
            const auto& cfg = SherwoodConfig::Get();
            if (cfg.replace1024x768 && cfg.width > 0 && cfg.height > 0) {
                float* widthPtr  = reinterpret_cast<float*>(cfgPtr + 0x54);
                float* heightPtr = reinterpret_cast<float*>(cfgPtr + 0x58);
                void* retAddr = _ReturnAddress();

                if (retAddr == reinterpret_cast<void*>(g_addrApplyGraphicConfigRet)) {
                    // Caller at 0x0052CEAC (RHMenuGraphics::Apply) is writing user-selected menu resolution
                    g_userSelectedMenuRes.store(true, std::memory_order_release);
                } else if (!g_userSelectedMenuRes.load(std::memory_order_acquire)) {
                    // Enforce SherwoodFix.ini resolution on startup even if profile previously saved another HD mode
                    *widthPtr  = static_cast<float>(cfg.width);
                    *heightPtr = static_cast<float>(cfg.height);
                } else if (*widthPtr <= 1024.0f) {
                    *widthPtr  = static_cast<float>(cfg.width);
                    *heightPtr = static_cast<float>(cfg.height);
                }
            }
        }
        return cfgPtr;
    }

    uint8_t* __cdecl Hook_GetLocalizedString(uint8_t* outStr, uint32_t stringId) {
        uint8_t* res = g_OrigGetLocalizedString(outStr, stringId);
        if (res && SherwoodConfig::Get().replace1024x768) {
            const wchar_t* customLabel = nullptr;
            if (stringId == 0x2B) {        // 43: originally "640 * 480"
                customLabel = L"1920 * 1080";
            } else if (stringId == 0x2C) { // 44: originally "800 * 600"
                customLabel = L"2560 * 1440";
            } else if (stringId == 0x2D) { // 45: originally "1024 * 768"
                customLabel = g_btn3Label;
            }

            if (customLabel) {
                size_t len = std::wcslen(customLabel);
                if (g_fnWStringGrow(res, len, 1) != 0) {
                    wchar_t* buf = *reinterpret_cast<wchar_t**>(res + 4);
                    if (buf) {
                        std::memcpy(buf, customLabel, len * sizeof(wchar_t));
                        buf[len] = L'\0';
                        *reinterpret_cast<uint32_t*>(res + 8) = static_cast<uint32_t>(len);
                    }
                }
            }
        }
        return res;
    }

    uint8_t __fastcall Hook_ClampCameraPosition(uint8_t* thisPtr, void* edx, float* pos) {
        uint8_t changed = g_OrigClampCameraPosition(thisPtr, edx, pos);
        EnforceSafeCameraBounds(thisPtr, pos);
        return changed;
    }

    float* __cdecl Hook_ClampCameraAndZoom(float* outRet, const float* inPos) {
        uint8_t* engPtr = *reinterpret_cast<uint8_t**>(kAddrRHEnginePtr);
        const float prevZoom = engPtr ? *reinterpret_cast<const float*>(engPtr + 0x475C) : 1.0f;

        float* res = g_OrigClampCameraAndZoom(outRet, inPos);

        engPtr = *reinterpret_cast<uint8_t**>(kAddrRHEnginePtr);
        if (engPtr) {
            const float newZoom = *reinterpret_cast<const float*>(engPtr + 0x475C);
            // If ClampCameraAndZoom reset zoom back to 1.0f, keep zoomIdx (+0x4898) and worldSurfaceValid (+0x48D0) synced
            if (std::fabs(prevZoom - 1.0f) > 0.01f && std::fabs(newZoom - 1.0f) < 0.01f) {
                *reinterpret_cast<uint16_t*>(engPtr + 0x4898) = 1;
                *reinterpret_cast<uint8_t*>(engPtr + 0x48D0) = 0;
            }
            if (res) {
                EnforceSafeCameraBounds(engPtr, res);
            }
        }
        return res;
    }

    void __fastcall Hook_SetCameraPosition(uint8_t* thisPtr, void* edx, float* pos) {
        // RHEngine::Update at 0x004C8EBC temporarily passes the world center point before subtracting viewport/(2*newZoom) at 0x004C8FAB
        if (_ReturnAddress() == reinterpret_cast<void*>(g_addrZoomMidSetCamRet)) {
            g_OrigSetCameraPosition(thisPtr, edx, pos);
            return;
        }
        EnforceSafeCameraBounds(thisPtr, pos);
        g_OrigSetCameraPosition(thisPtr, edx, pos);
        // Also ensure stored camera coordinates at +0x4738/+0x473C are never negative after rounding
        EnforceSafeCameraBounds(thisPtr, reinterpret_cast<float*>(thisPtr + 0x4738));
    }

    // ---- full-height world view (docs\HUD_STRIP.md) -------------------------------------------
    // The engine reserves the bottom 80 pixel rows for the HUD bar: the world view is
    // (width, height-80)/zoom everywhere. The bar's artwork is 800 px wide in total (left and
    // right corner piece), so on any wider screen the middle of those rows is black. With the
    // 80 replaced by 0 in the 42 places that compute the view, the map and the objects are
    // drawn down to the screen edge and the HUD corners simply lie on top of them.
    //   18 x fsub dword ptr [0x677598]  (80.0f)  -> fsub dword ptr [0x677488] (0.0f)
    //   24 x push 0xC2A00000            (-80.0f) -> push 0                    (Vec2(x, -80) + viewport)
    // All or nothing: render rectangles, scroll strips and camera clamps must agree.
    constexpr uintptr_t kFsubSites[] = {
        0x4C0414, 0x4C046F, 0x4C7D2C, 0x4C808C, 0x4C80B5, 0x4CED05, 0x4CED62, 0x4CEEFB, 0x4CEF21,
        0x4CEF3D, 0x4CEF61, 0x4CF715, 0x4CF723, 0x4D01CD, 0x4D01DD, 0x4D0229, 0x575A11, 0x575A58 };
    constexpr uintptr_t kPushSites[] = {
        0x4BEDFD, 0x4C16F2, 0x4C37C7, 0x4C748E, 0x4C87A1, 0x4C8994, 0x4C8DD9, 0x4C92E7, 0x4C94E7,
        0x4C9B67, 0x4C9C38, 0x4CEF8C, 0x4CF851, 0x4CF92C, 0x4CFA5B, 0x4CFD7E, 0x4CFDED, 0x4CFFF4,
        0x4D00CC, 0x4D0A8E, 0x4D910E, 0x4DA4AF, 0x4DA570, 0x4DA6A2 };

    // GUI container hit test 0x0061A6E0, thiscall(win, state*), ret 4. The two full-width HUD
    // containers (RHGame+0x1970 and +0x1A10, rect (0,H-80)-(W,H)) count as "hit" themselves and
    // thereby swallow every click in the bottom rows. With their rect flagged invalid for the
    // duration of the test only their children (portraits, buttons) are hit; a click between
    // them goes to the world like anywhere else.
    using HudHitTest_t = int(__fastcall*)(uint8_t* win, void* edx, void* state);
    HudHitTest_t g_OrigHudHitTest = nullptr;

    // True if [p, p+n) is committed, readable memory (no exception is raised to find out: the
    // crash logger would take a first-chance exception for a crash).
    bool Readable(const void* p, size_t n) {
        auto a = reinterpret_cast<uintptr_t>(p);
        const uintptr_t end = a + n;
        while (a < end) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(reinterpret_cast<LPCVOID>(a), &mbi, sizeof(mbi)) != sizeof(mbi) || mbi.State != MEM_COMMIT ||
                (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
                !(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
                return false;
            }
            a = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        }
        return true;
    }

    // RHGame* [0x6C0B3C] is NOT cleared when a mission ends: back in the main menu it points to
    // the freed game object. Reading its HUD containers through it crashed the game in the
    // graphics options (2026-11-14, ACCESS_VIOLATION READ game+0x1970). It is used only while it
    // is readable and still carries the vtable of RHGame (0x00678CA0, set by its constructor at
    // 0x0050BAB8). The answer is kept for the pointer value until the game ticks again.
    constexpr uint32_t kRHGameVtable = 0x00678CA0;
    uint8_t* g_checkedGame = nullptr;
    bool     g_checkedGameOk = false;
    DWORD    g_checkedAt = 0;

    uint8_t* LiveGame() {
        auto* game = *reinterpret_cast<uint8_t**>(0x006C0B3C);
        if (!game) return nullptr;
        const DWORD now = GetTickCount();
        if (game != g_checkedGame || now - g_checkedAt > 100) {
            g_checkedGame = game;
            g_checkedAt = now;
            g_checkedGameOk = Readable(game, 0x1A10 + sizeof(void*)) && *reinterpret_cast<const uint32_t*>(game) == kRHGameVtable;
        }
        return g_checkedGameOk ? game : nullptr;
    }

    int __fastcall Hook_HudHitTest(uint8_t* win, void* edx, void* state) {
        uint8_t* game = win ? LiveGame() : nullptr;
        bool strip = false;
        if (game) {
            for (const int off : { 0x1970, 0x1A10 }) {
                if (win == game + off || win == *reinterpret_cast<uint8_t**>(game + off)) strip = true;
            }
        }
        if (!strip) {
            return g_OrigHudHitTest(win, edx, state);
        }
        const uint8_t rectValid = win[0x80];
        win[0x80] = 0;
        const int result = g_OrigHudHitTest(win, edx, state);
        win[0x80] = rectValid;
        return result;
    }

    // RHEngine::CanZoomOut 0x004C7CF0, thiscall(E), result in al (docs\ZOOM_CAMERA.md §3):
    //     zoom > 0.5 && mapW >= vpW*2/zoom && mapH >= (vpH-80)*2/zoom
    // The game has three zoom levels (0.5, 1.0, 2.0). The HD view starts every level at 2.0, the
    // closest one. At 2560x1440 the 1.0 view (2560x1440 map pixels) is wider or taller than most
    // levels (Nottingham 2304x3520, Derby, Sherwood, the crossroads), so the game refused to zoom
    // out at all - and as 2.0 is the closest level, the zoom buttons and keys did nothing (user
    // report 2026-11-14). In the HD view the step 2.0 -> 1.0 is always allowed; where the level is
    // smaller than the view the camera stays at the level's edge and a black band remains (as on
    // any screen larger than the level). 1.0 -> 0.5 keeps the game's own check.
    using CanZoomOut_t = uint8_t(__fastcall*)(uint8_t* engine, void* edx);
    CanZoomOut_t g_OrigCanZoomOut = nullptr;

    uint8_t __fastcall Hook_CanZoomOut(uint8_t* engine, void* edx) {
        if (ViewZoom::HandlesZoom()) {
            return ViewZoom::CanZoomOut() ? 1 : 0;      // smooth zoom: down to "the level fills the screen"
        }
        const uint8_t original = g_OrigCanZoomOut(engine, edx);
        if (original || !engine || !SherwoodConfig::Get().hdView) {
            return original;
        }
        const float vpW  = *reinterpret_cast<const float*>(engine + 0x4728);
        const float zoom = *reinterpret_cast<const float*>(engine + 0x475C);
        return (vpW >= 2048.0f && zoom >= 1.99f) ? 1 : 0;
    }

    void PatchCanZoomOut() {
        static const uint8_t kProlog[] = { 0xD9, 0x81, 0x5C, 0x47, 0x00, 0x00, 0xDC, 0x1D, 0xB8, 0x74, 0x67, 0x00 };
        const bool ok = std::memcmp(reinterpret_cast<const void*>(0x004C7CF0), kProlog, sizeof(kProlog)) == 0 &&
                        MH_CreateHook(reinterpret_cast<LPVOID>(0x004C7CF0), reinterpret_cast<LPVOID>(&Hook_CanZoomOut),
                                      reinterpret_cast<LPVOID*>(&g_OrigCanZoomOut)) == MH_OK &&
                        MH_EnableHook(reinterpret_cast<LPVOID>(0x004C7CF0)) == MH_OK;
        Logger::Info("WidescreenPatch: HD view may zoom out from 2.0 to 1.0 on every level: %d.", ok ? 1 : 0);
    }

    // ---- coloured see-through areas over the level: cut to the level picture (2026-11-22) ----
    // A polygon filler (0x00590xxx/0x00592xxx/0x00593xxx, object in ecx; vision cones, the fields under left Shift)
    // draws them as vertical spans with one of ten span routines (chosen at 0x0058C6F0: [obj+0x20C] 1:1,
    // [obj+0x210] twice as large, [obj+0x214] half as large; 565 / 555), thiscall (depth threshold, x, y0, y1,
    // value, value per row), ret 0x18. Each reads the level picture - the map surface, locked: [obj+0x44] bits,
    // [obj+0x48] pitch, size at [obj+0x30] / [obj+0x32] - at (x + [obj+0x90], y + [obj+0x94]): the place on the
    // screen plus the camera, never checked against the level's size. The game's own views stayed inside a level;
    // the HD view's 1.0 on small levels and the full-height view show a black band beyond it, and a span over the
    // band read past the map surface: the user's game ended there twice (0x0058C88B: 2026-11-21 Nottingham, 8 KB
    // past its end; 2026-11-22 a crossroads map 1408x960, 131 rows past it - more than the surfaces' margins).
    // Each span is cut to the rows and columns the level picture (and the target) has; outside them there is no
    // level to colour anyway.
    using Span_t = void(__fastcall*)(uint8_t* obj, void* edx, int depth, int x, int y0, int y1, int value, int perRow);
    enum SpanKind { kSpan1x, kSpan2x, kSpanHalf };
    std::atomic<unsigned> g_spansCut{ 0 };

    // false: nothing of the span lies over the level picture (left out). y0..y1 inclusive; the routines draw
    // nothing when y1 <= y0.
    bool ClipSpan(const uint8_t* o, SpanKind kind, int x, int& y0, int& y1, int& value, int perRow) {
        const int dstW = *reinterpret_cast<const uint16_t*>(o + 0x0C), dstH = *reinterpret_cast<const uint16_t*>(o + 0x0E);
        const int srcW = *reinterpret_cast<const uint16_t*>(o + 0x30);
        int srcH = *reinterpret_cast<const uint16_t*>(o + 0x32);
        const int offX = *reinterpret_cast<const int*>(o + 0x90), offY = *reinterpret_cast<const int*>(o + 0x94);
        if (dstW <= 0 || dstH <= 0 || srcW <= 0 || srcH <= 0) return true;      // sizes not known: as the game does it
        // test harness only ("dbg 1024"): the level counts 400 rows shorter - the cut spans can be seen
        if (DDM::HD::DebugFlags() & 1024u) srcH = (std::max)(1, srcH - 400);
        if (kind == kSpanHalf) {
            // target (x >> 1, y >> 1), level (x + 1 + offX, y + 1 + offY); rows y0, y0 + 2, ... ((y1 - y0 + 2) >> 1)
            const int lx = x + 1 + offX;
            if (x < 0 || (x >> 1) >= dstW || lx < 0 || lx >= srcW) return false;
            const int lo = (std::max)(0, -1 - offY);
            if (y0 < lo) {
                const int k = (lo - y0 + 1) / 2;
                y0 += 2 * k;
                value += 2 * k * perRow;
            }
            if (y1 <= y0) return false;
            const int room = srcH - 2 - offY - y0;                  // level rows left below the first one
            const int fit = (std::min)(room / 2 + 1, dstH - (y0 >> 1));
            if (room < 0 || fit <= 0) return false;
            if (((y1 - y0 + 2) >> 1) > fit) y1 = y0 + 2 * fit - 1;
            return y1 > y0;
        }
        // 1:1 / twice as large: level (x + offX, y + offY); the target (x, y) / (2x, 2y) 2x2
        const int lx = x + offX;
        const int tw = kind == kSpan2x ? dstW / 2 : dstW, th = kind == kSpan2x ? dstH / 2 : dstH;
        if (x < 0 || x >= tw || lx < 0 || lx >= srcW) return false;
        const int lo = (std::max)(0, -offY), hi = (std::min)(th - 1, srcH - 1 - offY);
        if (y0 < lo) {
            value += (lo - y0) * perRow;
            y0 = lo;
        }
        if (y1 > hi) y1 = hi;
        return y1 > y0;
    }

    template <int I>
    struct SpanHook {
        static Span_t orig;
        static SpanKind kind;
        static void __fastcall Hook(uint8_t* obj, void* edx, int depth, int x, int y0, int y1, int value, int perRow) {
            static bool s_seen = false;
            if (!s_seen) {
                s_seen = true;
                Logger::Info("WidescreenPatch: span routine %d first used (level picture %dx%d, target %dx%d, camera %d,%d).", I,
                             *reinterpret_cast<const uint16_t*>(obj + 0x30), *reinterpret_cast<const uint16_t*>(obj + 0x32),
                             *reinterpret_cast<const uint16_t*>(obj + 0x0C), *reinterpret_cast<const uint16_t*>(obj + 0x0E),
                             *reinterpret_cast<const int*>(obj + 0x90), *reinterpret_cast<const int*>(obj + 0x94));
            }
            const int x0 = x, oy0 = y0, oy1 = y1;
            const bool keep = ClipSpan(obj, kind, x, y0, y1, value, perRow);
            if (oy1 > oy0 && (!keep || y0 != oy0 || y1 != oy1)) {
                if (g_spansCut.fetch_add(1, std::memory_order_relaxed) < 3) {
                    Logger::Info("WidescreenPatch: a see-through area reached past the level picture (%dx%d, camera %d,%d): "
                                 "span x=%d rows %d..%d cut to %s.",
                                 *reinterpret_cast<const uint16_t*>(obj + 0x30), *reinterpret_cast<const uint16_t*>(obj + 0x32),
                                 *reinterpret_cast<const int*>(obj + 0x90), *reinterpret_cast<const int*>(obj + 0x94),
                                 x0, oy0, oy1, keep ? "the rows over the level" : "nothing");
                }
            }
            if (keep) orig(obj, edx, depth, x, y0, y1, value, perRow);
        }
    };
    template <int I> Span_t SpanHook<I>::orig = nullptr;
    template <int I> SpanKind SpanHook<I>::kind = kSpan1x;

    template <int I>
    int InstallSpan(uintptr_t addr, SpanKind kind, const uint8_t (&prolog)[9]) {
        SpanHook<I>::kind = kind;
        return std::memcmp(reinterpret_cast<const void*>(addr), prolog, sizeof(prolog)) == 0 &&
               MH_CreateHook(reinterpret_cast<LPVOID>(addr), reinterpret_cast<LPVOID>(&SpanHook<I>::Hook),
                             reinterpret_cast<LPVOID*>(&SpanHook<I>::orig)) == MH_OK &&
               MH_EnableHook(reinterpret_cast<LPVOID>(addr)) == MH_OK ? 1 : 0;
    }

    void PatchSpanClip() {
        static const uint8_t kA[9] = { 0x8B, 0x44, 0x24, 0x10, 0x83, 0xEC, 0x18, 0x53, 0x55 };   // mov eax,[esp+10h]; sub esp,18h; push ebx; push ebp
        static const uint8_t kB[9] = { 0x83, 0xEC, 0x18, 0x53, 0x55, 0x8B, 0x6C, 0x24, 0x28 };   // sub esp,18h; push ebx; push ebp; mov ebp,[esp+28h]
        static const uint8_t kC[9] = { 0x8B, 0x44, 0x24, 0x0C, 0x8B, 0x54, 0x24, 0x10, 0x83 };   // mov eax,[esp+0Ch]; mov edx,[esp+10h]; sub ...
        static const uint8_t kD[9] = { 0x83, 0xEC, 0x18, 0x8B, 0x44, 0x24, 0x24, 0x53, 0x55 };   // sub esp,18h; mov eax,[esp+24h]; push ebx; push ebp
        int n = 0;
        n += InstallSpan<0>(0x0058C7A0, kSpan1x, kA);
        n += InstallSpan<1>(0x0058C960, kSpan1x, kB);
        n += InstallSpan<2>(0x0058CB40, kSpan1x, kA);
        n += InstallSpan<3>(0x0058CD00, kSpan1x, kB);
        n += InstallSpan<4>(0x0058CEE0, kSpan2x, kC);
        n += InstallSpan<5>(0x0058D0A0, kSpan2x, kD);
        n += InstallSpan<6>(0x0058D290, kSpan2x, kC);
        n += InstallSpan<7>(0x0058D450, kSpan2x, kD);
        n += InstallSpan<8>(0x0058D630, kSpanHalf, kA);
        n += InstallSpan<9>(0x0058D7E0, kSpanHalf, kA);
        Logger::Info("WidescreenPatch: see-through areas over the level (vision cones, Shift fields) cut to the level picture: %d of 10 span routines.", n);
    }

    void PatchFullHeightView() {
        static const uint8_t kFsub80[] = { 0xD8, 0x25, 0x98, 0x75, 0x67, 0x00 };
        static const uint8_t kFsub0[]  = { 0xD8, 0x25, 0x88, 0x74, 0x67, 0x00 };
        static const uint8_t kPush80[] = { 0x68, 0x00, 0x00, 0xA0, 0xC2 };
        static const uint8_t kPush0[]  = { 0x68, 0x00, 0x00, 0x00, 0x00 };
        static const uint8_t kHitProlog[] = { 0x83, 0xEC, 0x08, 0x56, 0x8B, 0xF1, 0x8A, 0x46, 0x5C, 0x84, 0xC0 };

        bool match = *reinterpret_cast<const float*>(0x00677488) == 0.0f &&
                     std::memcmp(reinterpret_cast<const void*>(0x0061A6E0), kHitProlog, sizeof(kHitProlog)) == 0;
        for (uintptr_t a : kFsubSites) match = match && std::memcmp(reinterpret_cast<const void*>(a), kFsub80, sizeof(kFsub80)) == 0;
        for (uintptr_t a : kPushSites) match = match && std::memcmp(reinterpret_cast<const void*>(a), kPush80, sizeof(kPush80)) == 0;
        if (!match) {
            Logger::Warn("WidescreenPatch: full-height view NOT applied - the code does not match this build.");
            return;
        }
        bool ok = true;
        for (uintptr_t a : kFsubSites) ok = Memory::PatchBytes(a, kFsub0, sizeof(kFsub0)) && ok;
        for (uintptr_t a : kPushSites) ok = Memory::PatchBytes(a, kPush0, sizeof(kPush0)) && ok;
        // the camera's limits (not the drawn view): the hedge's rows, so the level's last rows can be
        // scrolled above the hedge - CenterCameraOn 0x4C7F60, ClampScrollDelta 0x4CEC60, the camera
        // clamp 0x5758D0 (docs\HUD_STRIP.md, class "camera clamp")
        static const uintptr_t kCameraSites[] = { 0x4C808C, 0x4C80B5, 0x4CED05, 0x4CED62, 0x575A11, 0x575A58 };
        uint8_t fsubStrip[6] = { 0xD8, 0x25 };
        const uint32_t stripAddr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_cameraStrip));
        std::memcpy(fsubStrip + 2, &stripAddr, 4);
        for (uintptr_t a : kCameraSites) ok = Memory::PatchBytes(a, fsubStrip, sizeof(fsubStrip)) && ok;
        const bool hit = MH_CreateHook(reinterpret_cast<LPVOID>(0x0061A6E0), reinterpret_cast<LPVOID>(&Hook_HudHitTest),
                                       reinterpret_cast<LPVOID*>(&g_OrigHudHitTest)) == MH_OK &&
                         MH_EnableHook(reinterpret_cast<LPVOID>(0x0061A6E0)) == MH_OK;
        Logger::Info("WidescreenPatch: full-height world view (HUD strip of 80 px removed from 42 view computations): %s; clicks between HUD widgets reach the world: %d.",
                     ok ? "applied" : "PARTLY applied (write failed)", hit ? 1 : 0);
    }

    void PatchMenuResolutionSlot(
        HMODULE hGame,
        const char* setterPattern, uintptr_t fallbackSetterAddr,
        const char* checkPattern,  uintptr_t fallbackCheckAddr,
        int width, int height)
    {
        const float targetW = static_cast<float>(width);
        const float targetH = static_cast<float>(height);
        const uint32_t targetWInt = static_cast<uint32_t>(width);

        uintptr_t addrSetter = Memory::FindPattern(hGame, setterPattern);
        if (!addrSetter) {
            addrSetter = fallbackSetterAddr;
        }
        if (*reinterpret_cast<uint8_t*>(addrSetter) == 0x68 &&
            *reinterpret_cast<uint8_t*>(addrSetter + 5) == 0x68) {
            Memory::Write<float>(addrSetter + 1, targetH);
            Memory::Write<float>(addrSetter + 6, targetW);
        }

        uintptr_t addrCheck = Memory::FindPattern(hGame, checkPattern);
        if (!addrCheck) {
            addrCheck = fallbackCheckAddr;
        }
        if (*reinterpret_cast<uint8_t*>(addrCheck) == 0x3D) {
            Memory::Write<uint32_t>(addrCheck + 1, targetWInt);
        }

        Logger::Info("WidescreenPatch: Patched menu slot (setter=0x%08X, check=0x%08X) -> %dx%d.",
                     static_cast<uint32_t>(addrSetter), static_cast<uint32_t>(addrCheck), width, height);
    }
}

namespace WidescreenPatch {

void Initialize() {
    const auto& cfg = SherwoodConfig::Get();
    HMODULE hGame = GetModuleHandleA(nullptr);

    if (cfg.replace1024x768 && cfg.width > 0 && cfg.height > 0) {
        // Button 1 (originally 640x480) -> 1920x1080 (Full HD)
        PatchMenuResolutionSlot(
            hGame,
            "68 00 00 F0 43 68 00 00 20 44 8D 4C 24", 0x0052CFA9,
            "3D 80 02 00 00 74 0F",                   0x0052D0A1,
            1920, 1080);

        // Button 2 (originally 800x600) -> 2560x1440 (WQHD 1440p)
        PatchMenuResolutionSlot(
            hGame,
            "68 00 00 16 44 68 00 00 48 44 8D 4C 24", 0x0052CFF2,
            "3D 20 03 00 00 74 1D",                   0x0052D0B8,
            2560, 1440);

        // Button 3 (originally 1024x768) -> 3840x2160 (4K UHD) or custom INI resolution
        int btn3W = 3840;
        int btn3H = 2160;
        if ((cfg.width != 1920 || cfg.height != 1080) &&
            (cfg.width != 2560 || cfg.height != 1440)) {
            btn3W = cfg.width;
            btn3H = cfg.height;
        }
        std::swprintf(g_btn3Label, sizeof(g_btn3Label) / sizeof(wchar_t), L"%d * %d", btn3W, btn3H);

        PatchMenuResolutionSlot(
            hGame,
            "68 00 00 40 44 68 00 00 80 44 8D 4C 24", 0x0052D038,
            "3D 00 04 00 00 74 3A",                   0x0052D0BF,
            btn3W, btn3H);

        // Hook GetLocalizedString (0x005E23D0) so Options -> Graphics displays "1920 * 1080", "2560 * 1440", "3840 * 2160"
        uintptr_t addrGetStr = Memory::FindPattern(hGame, "6A FF 68 78 25 67 00 64 A1 00 00 00 00 50 64 89 25 00 00 00 00 83 EC 14");
        if (!addrGetStr) {
            addrGetStr = 0x005E23D0;
        }
        if (MH_CreateHook(reinterpret_cast<LPVOID>(addrGetStr),
                          reinterpret_cast<LPVOID>(&Hook_GetLocalizedString),
                          reinterpret_cast<LPVOID*>(&g_OrigGetLocalizedString)) == MH_OK) {
            MH_EnableHook(reinterpret_cast<LPVOID>(addrGetStr));
            Logger::Info("WidescreenPatch: Hooked GetLocalizedString @ 0x%08X for HD/1440p/4K menu labels.",
                         static_cast<uint32_t>(addrGetStr));
        }

        // Resolve RHMenuGraphics::Apply call site to GetGraphicConfig (ret=0x0052CEAC)
        uintptr_t addrApplyCfgCall = Memory::FindPattern(
            hGame, "B9 48 0E 6C 00 E8 ? ? ? ? 8A 8E BC 0D 00 00 83 C0 50");
        if (addrApplyCfgCall) {
            g_addrApplyGraphicConfigRet = addrApplyCfgCall + 10;
        }

        // Hook GetGraphicConfig (0x0055DBB0) to auto-upgrade legacy saved profiles
        uintptr_t addrGetCfg = Memory::FindPattern(hGame, "8B 41 28 C3 90 90 90 90");
        if (!addrGetCfg) {
            addrGetCfg = 0x0055DBB0;
        }
        if (MH_CreateHook(reinterpret_cast<LPVOID>(addrGetCfg),
                          reinterpret_cast<LPVOID>(&Hook_GetGraphicConfig),
                          reinterpret_cast<LPVOID*>(&g_OrigGetGraphicConfig)) == MH_OK) {
            MH_EnableHook(reinterpret_cast<LPVOID>(addrGetCfg));
        }
    }

    if (cfg.fullHeightView) {
        PatchFullHeightView();
    }
    if (cfg.hdView) {
        PatchCanZoomOut();
    }
    // always: any view larger than a level (the HD view, the full height, the smooth zoom, a large resolution)
    PatchSpanClip();

    // 4. Hook camera clamping & positioning functions to prevent negative-coordinate crashes
    // on maps smaller than the viewport (e.g. Sherwood Hideout in 1080p/1440p/4K).
    uintptr_t addrClampCam = Memory::FindPattern(hGame, "6A FF 68 50 BD 65 00 64 A1 00 00 00 00");
    if (!addrClampCam) {
        addrClampCam = 0x004C0340;
    }
    if (MH_CreateHook(reinterpret_cast<LPVOID>(addrClampCam),
                      reinterpret_cast<LPVOID>(&Hook_ClampCameraPosition),
                      reinterpret_cast<LPVOID*>(&g_OrigClampCameraPosition)) == MH_OK) {
        MH_EnableHook(reinterpret_cast<LPVOID>(addrClampCam));
        Logger::Info("WidescreenPatch: Hooked RHEngine::ClampCameraPosition at 0x%08X.",
                     static_cast<uint32_t>(addrClampCam));
    }

    uintptr_t addrClampZoom = Memory::FindPattern(hGame, "6A FF 68 1F B3 66 00 64 A1 00 00 00 00");
    if (!addrClampZoom) {
        addrClampZoom = 0x005758D0;
    }
    if (MH_CreateHook(reinterpret_cast<LPVOID>(addrClampZoom),
                      reinterpret_cast<LPVOID>(&Hook_ClampCameraAndZoom),
                      reinterpret_cast<LPVOID*>(&g_OrigClampCameraAndZoom)) == MH_OK) {
        MH_EnableHook(reinterpret_cast<LPVOID>(addrClampZoom));
        Logger::Info("WidescreenPatch: Hooked ClampCameraAndZoom at 0x%08X.",
                     static_cast<uint32_t>(addrClampZoom));
    }

    uintptr_t addrZoomMid = Memory::FindPattern(
        hGame, "50 8B CE C6 84 24 94 01 00 00 1C E8");
    if (addrZoomMid) {
        g_addrZoomMidSetCamRet = addrZoomMid + 16;
    }

    uintptr_t addrSetCam = Memory::FindPattern(hGame, "56 57 8B 7C 24 0C 83 EC 08 8B F1 D9 07");
    if (!addrSetCam) {
        addrSetCam = 0x004DB7F0;
    }
    if (MH_CreateHook(reinterpret_cast<LPVOID>(addrSetCam),
                      reinterpret_cast<LPVOID>(&Hook_SetCameraPosition),
                      reinterpret_cast<LPVOID*>(&g_OrigSetCameraPosition)) == MH_OK) {
        MH_EnableHook(reinterpret_cast<LPVOID>(addrSetCam));
        Logger::Info("WidescreenPatch: Hooked RHEngine::SetCameraPosition at 0x%08X.",
                     static_cast<uint32_t>(addrSetCam));
    }
}

void Shutdown() {
    // Hooks cleaned up globally by MH_Uninitialize
}

namespace {
    // RHEngine::ZoomInRequest(Event*) 0x004DBE40: push esi / mov esi,ecx / call CanZoomIn /
    // test al,al / jz. With the smooth zoom installed the entry is hooked; a call still reaches
    // the original while the smooth zoom is switched off.
    bool ZoomInRequestKnown() {
        static const uint8_t kZoomInProlog[] = { 0x56, 0x8B, 0xF1, 0xE8 };
        static const bool s_known = ViewZoom::Installed() ||
                                    (std::memcmp(reinterpret_cast<const void*>(0x004DBE40), kZoomInProlog, sizeof(kZoomInProlog)) == 0 &&
                                     std::memcmp(reinterpret_cast<const void*>(0x004DBE40 + 8), "\x84\xC0\x74", 3) == 0);
        return s_known;
    }
}

// "HD view". With an internal resolution of 2560x1440 the game's 1.0 zoom shows four times the
// area the game was made for - more than most levels hold (black band at the side) and with
// tiny figures. Its 2.0 zoom has the framing of a 1280x720 screen while HUD, texts and cursor
// stay native, and the background can come from the HD pictures (ddm_hd). So: start every
// level at 2.0, and never stay at a zoom whose view is larger than the level. Zooming out with
// the game's own keys still works where the level is large enough.
float CameraStrip() {
    return g_cameraStrip;
}

void OnGameTick(unsigned char* engine) {
    const auto& cfg = SherwoodConfig::Get();
    // the hedge's rows at the interface's scale now (F10 may switch it)
    g_cameraStrip = cfg.fullHeightView ? 80.0f * static_cast<float>(UiScale::Factor()) : 0.0f;
    if (!engine || !cfg.hdView || ViewZoom::HandlesZoom()) {
        return;     // with the smooth zoom, view_zoom.cpp starts the levels at 2.0
    }
    constexpr uintptr_t kAddrZoomInRequest = 0x004DBE40;
    if (!ZoomInRequestKnown()) {
        return;
    }
    __try {
        const float vpW  = *reinterpret_cast<const float*>(engine + 0x4728);
        const float vpH  = *reinterpret_cast<const float*>(engine + 0x472C);
        const float mapW = *reinterpret_cast<const float*>(engine + 0x4730);
        const float mapH = *reinterpret_cast<const float*>(engine + 0x4734);
        const float zoom = *reinterpret_cast<const float*>(engine + 0x475C);
        if (vpW < 2048.0f || mapW <= 0.0f || mapH <= 0.0f || zoom <= 0.01f) {
            return;
        }
        static const void* s_level = nullptr;
        static float s_mapW = 0.0f, s_mapH = 0.0f;
        static int   s_startTicks = 0; // ticks left in which a freshly loaded level is brought to 2.0
        static bool  s_logged = false;
        const void* level = *reinterpret_cast<void* const*>(engine + 0x48C4); // map surface: new for every level
        if (level != s_level || mapW != s_mapW || mapH != s_mapH) {
            s_level = level;
            s_mapW = mapW;
            s_mapH = mapH;
            s_startTicks = 75; // 3 s
            s_logged = false;
        }
        if (zoom >= 1.99f) {
            // keep watching the first 3 s of the level: a zoom-out the game had deferred while the
            // zoom-in ran would otherwise follow right after it (seen 2026-11-14 once zooming out
            // was allowed on every level)
            if (s_startTicks > 0) --s_startTicks;
            return;
        }
        const float strip = cfg.fullHeightView ? 0.0f : 80.0f;
        const bool viewTooLarge = (vpW / zoom > mapW) || ((vpH - strip) / zoom > mapH);
        const bool levelStart = s_startTicks > 0;
        if (s_startTicks > 0) {
            --s_startTicks;
        }
        // Only when a level starts: afterwards the player may zoom out (Hook_CanZoomOut), also
        // where the 1.0 view is larger than the level.
        if (!levelStart) {
            return;
        }
        if (engine[0x48C0] != 1) {
            return; // scrolling, a zoom transition or a full redraw is in progress
        }
        static DWORD s_lastRequest = 0;
        const DWORD now = GetTickCount();
        if (now - s_lastRequest < 300) {
            return;
        }
        s_lastRequest = now;
        uint32_t evt[8]{};
        evt[2] = 1; // zoom about the screen centre, not the cursor
        reinterpret_cast<void(__thiscall*)(void*, void*)>(kAddrZoomInRequest)(engine, evt);
        if (!s_logged) {
            s_logged = true;
            Logger::Info("HD view: level %.0fx%.0f at zoom %.1f on a %.0fx%.0f screen -> zooming in to 2.0%s.",
                         mapW, mapH, zoom, vpW, vpH, viewTooLarge ? " (the view was larger than the level)" : "");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

} // namespace WidescreenPatch
