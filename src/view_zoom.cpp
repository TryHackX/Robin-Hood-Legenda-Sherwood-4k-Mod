#include "view_zoom.h"
#include "config.h"
#include "logger.h"
#include "ui_scale.h"
#include "widescreen_patch.h"
#include "ddm/ddm_hd.h"
#include "ddm/ddm_present.h"

#include <windows.h>
#include <MinHook.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

// See view_zoom.h. Game.exe (docs\ZOOM_CAMERA.md, static RE 2026-11-14):
//   E = RHEngine* [0x006C0FA8]; +0x4728 viewport W,H; +0x4730 level W,H; +0x4738 camera (top-left,
//   world px); +0x4740 camera of the last render; +0x475C zoom; +0x4760 zoom of the last render;
//   +0x4898 u16 zoom index (table 0.5/1/2); +0x48C0 camera state (3/4 = zoom animation,
//   5 = full redraw); +0x48D0 world surface valid (0 -> the next update redraws everything);
//   +0x48C4 map surface (new for every level); +0x4790/+0x4791 zoom in/out in progress,
//   +0x4768 camera travel; [E+0x68] = the object the HUD messages go to.
//   0x004DBE40 / 0x004DBDB0 ZoomIn/OutRequest  thiscall(E, Event*), ret 4 - every zoom key/button
//   0x004C7CD0 CanZoomIn                       thiscall(E) -> al (0x004C7CF0 CanZoomOut: widescreen_patch)
//   0x004C7C90 CanStartZoom                    thiscall(E) -> al
//   0x004DEE70 ScreenToWorld                   thiscall(E, Vec2* out), ret 4: pointer / zoom + camera
//   0x004C7F60 CenterCameraOn                  thiscall(E, const Vec2* point), ret 4
//   0x004DB7F0 SetCameraPosition               thiscall(E, Vec2*), ret 4 (floors; kept in the level
//                                              by the hook of widescreen_patch.cpp)
//   HUD zoom buttons: 0x005490A0 message(this, 9, CanZoomIn << 16 | CanZoomOut) ret 8,
//   0x005460C0 post(target = [E+0x68], message*) ret 4, 0x00549110 message destructor (as Update
//   does at 0x004C84E6..0x004C8516 after a zoom).
namespace ViewZoom {
namespace {
    constexpr uintptr_t kAddrEnginePtr = 0x006C0FA8;
    constexpr int kViewport = 0x4728, kLevelSize = 0x4730, kCamera = 0x4738, kRenderCam = 0x4740,
                  kZoom = 0x475C, kRenderZoom = 0x4760, kZoomIndex = 0x4898, kCamState = 0x48C0,
                  kWorldValid = 0x48D0, kMapSurface = 0x48C4;

    using PFN_ZoomRequest   = void(__fastcall*)(uint8_t* engine, void* edx, void* evt);
    using PFN_EngineTest    = uint32_t(__fastcall*)(uint8_t* engine, void* edx);
    using PFN_ScreenToWorld = void(__fastcall*)(uint8_t* engine, void* edx, float* out);
    using PFN_CenterOn      = void(__fastcall*)(uint8_t* engine, void* edx, const float* point);
    using PFN_SetCamera     = void(__fastcall*)(uint8_t* engine, void* edx, float* pos);
    using PFN_MsgCtor       = void*(__fastcall*)(void* msg, void* edx, uint32_t type, uint32_t value);
    using PFN_MsgPost       = void(__fastcall*)(void* target, void* edx, void* msg);
    using PFN_MsgDtor       = void(__fastcall*)(void* msg, void* edx);

    PFN_ZoomRequest   o_ZoomIn = nullptr;
    PFN_ZoomRequest   o_ZoomOut = nullptr;
    PFN_EngineTest    o_CanZoomIn = nullptr;
    PFN_ScreenToWorld o_ScreenToWorld = nullptr;
    PFN_CenterOn      o_CenterOn = nullptr;
    const auto g_fnCanStartZoom = reinterpret_cast<PFN_EngineTest>(0x004C7C90);
    const auto g_fnSetCamera    = reinterpret_cast<PFN_SetCamera>(0x004DB7F0);
    const auto g_fnMsgCtor      = reinterpret_cast<PFN_MsgCtor>(0x005490A0);
    const auto g_fnMsgPost      = reinterpret_cast<PFN_MsgPost>(0x005460C0);
    const auto g_fnMsgDtor      = reinterpret_cast<PFN_MsgDtor>(0x00549110);

    bool g_installed = false;
    bool g_enabled = true;
    int  g_stepPercent = 115;
    int  g_maxPercent = 400;

    // main thread
    const void* g_level = nullptr;          // the map surface the state below belongs to
    float g_zTarget = 2.0f;                 // the zoom asked for
    float g_zShown = 2.0f;                  // the zoom shown now (follows the target smoothly)
    float g_offX = 0.0f, g_offY = 0.0f;     // view centre minus the frame's centre, world pixels
    bool  g_haveCentre = false;             // CenterCameraOn: this point to the view's centre
    float g_centreX = 0.0f, g_centreY = 0.0f;
    bool  g_hudDirty = false;
    float g_pushX = 0.0f, g_pushY = 0.0f;   // scrolling the camera could not do (world pixels)
    LARGE_INTEGER g_freq{}, g_last{};
    std::atomic<int> g_wheel{ 0 };          // input thread -> main thread
    std::atomic<float> g_testZoom{ 0.0f };  // test harness -> main thread
    std::atomic<int>   g_testGameZoom{ 0 }; // test harness: the engine's own zoom animation (+1 in, -1 out)
    std::atomic<DWORD> g_wheelTime{ 0 };    // input thread: when the mouse wheel turned last
    // zoom with the wheel: the world point under the cursor stays under it while the zoom changes
    bool  g_anchor = false;
    float g_anchorPX = 0.0f, g_anchorPY = 0.0f;     // screen point
    float g_anchorWX = 0.0f, g_anchorWY = 0.0f;     // world point

    // what the last frame with the world showed (presenter, mouse mapping)
    struct Shown {
        bool  on = false;
        float m = 1.0f, cropX = 0.0f, cropY = 0.0f;
        float camX = 0.0f, camY = 0.0f, zoom = 1.0f;    // the frame's camera and engine zoom
        float frameW = 0.0f, worldH = 0.0f;
    };
    Shown g_shown;
    bool  g_viewPending = false;            // OnWorldDrawn made a view for the frame being drawn
    int   g_worldW = 0, g_worldH = 0;      // the world texture (DDM::Present::UploadWorld)
    std::vector<uint16_t> g_readBack;      // LastWorld: the world read back from it
    // the world of a dialog's backdrop render (OnBackdropWorldDrawn), the render's number (0 = none)
    std::vector<uint16_t> g_backdropWorld;
    int      g_backdropWorldW = 0, g_backdropWorldH = 0;
    unsigned g_backdropWorldRender = 0;
    Shown    g_backdropView;                // the magnification computed for that render
    unsigned g_switches = 0;
    float g_lastEngineZoom = 0.0f;         // the engine's zoom as this module left it
    bool  g_gameZooming = false;           // the game runs a zoom animation of its own

    uint8_t* Engine() { return *reinterpret_cast<uint8_t* const*>(kAddrEnginePtr); }
    float F(const uint8_t* e, int off) { return *reinterpret_cast<const float*>(e + off); }
    float Strip() { return SherwoodConfig::Get().fullHeightView ? 0.0f : 80.0f; }

    // the smallest zoom: the whole level fills the screen at the engine's 1.0 (no black band)
    float ZMin(const uint8_t* e) {
        const float vw = F(e, kViewport), vh = F(e, kViewport + 4) - Strip();
        const float lw = F(e, kLevelSize), lh = F(e, kLevelSize + 4);
        float z = 1.0f;
        if (lw > 1.0f) z = (std::max)(z, vw / lw);
        if (lh > 1.0f) z = (std::max)(z, vh / lh);
        return z;
    }
    float ZMax() { return (std::max)(2.0f, static_cast<float>(g_maxPercent) / 100.0f); }

    // the engine's level for a zoom: 2.0 (HD pictures) from 2.0 up, 1.0 below
    float EngineFor(float z, float zMin) { return (z >= 1.995f || zMin >= 1.995f) ? 2.0f : 1.0f; }

    // the bottom rows of the shown picture the hedge covers (full-height view; 0 otherwise)
    float HedgeBand() { return WidescreenPatch::CameraStrip(); }

    // the shown part's centre row: within the level, but low enough that the level's last rows can come
    // above the hedge (`bandFrame` = the hedge's rows in frame pixels), never below the frame's last row
    float ClampShownY(float cy, float contentB, float halfH, float bandFrame, float frameH) {
        const float cyMax = (std::min)(contentB + bandFrame, frameH) - halfH;
        if (cyMax < halfH) return (std::min)(contentB + bandFrame, frameH) * 0.5f;
        return (std::max)(halfH, (std::min)(cyMax, cy));
    }

    bool Busy(uint8_t* e) {
        const uint8_t st = e[kCamState];
        return st == 3 || st == 4 || e[0x4790] || e[0x4791] || e[0x4768] || !(g_fnCanStartZoom(e, nullptr) & 0xFF);
    }

    bool CanZoomInNow(const uint8_t* e) { return e && g_zTarget < ZMax() - 0.001f; }
    bool CanZoomOutNow(const uint8_t* e) { return e && g_zTarget > ZMin(e) + 0.001f; }

    // the HUD's zoom buttons: enabled / greyed as the zoom allows (the game tells it after its
    // own zoom animations only)
    void RefreshHud(uint8_t* e) {
        void* target = *reinterpret_cast<void**>(e + 0x68);
        if (!target) return;
        const uint32_t value = (CanZoomInNow(e) ? 1u << 16 : 0u) | (CanZoomOutNow(e) ? 1u : 0u);
        alignas(8) uint8_t msg[64] = {};
        g_fnMsgCtor(msg, nullptr, 9, value);
        g_fnMsgPost(target, nullptr, msg);
        g_fnMsgDtor(msg, nullptr);
    }

    // the world point at the centre of what the player sees now
    void ViewCentre(const uint8_t* e, float& x, float& y) {
        const float vw = F(e, kViewport), vh = F(e, kViewport + 4) - Strip();
        if (g_shown.on) {
            x = g_shown.camX + (g_shown.cropX + g_shown.frameW / (2.0f * g_shown.m)) / g_shown.zoom;
            y = g_shown.camY + (g_shown.cropY + g_shown.worldH / (2.0f * g_shown.m)) / g_shown.zoom;
            return;
        }
        const float z = F(e, kZoom) > 0.01f ? F(e, kZoom) : 1.0f;
        x = F(e, kCamera) + vw / (2.0f * z);
        y = F(e, kCamera + 4) + vh / (2.0f * z);
    }

    // The engine to another zoom level at once (no zoom animation): zoom and index, the camera
    // about the same view centre, the world surface invalid (the next update redraws it all).
    bool SwitchEngine(uint8_t* e, float zoom, float cx, float cy) {
        if (Busy(e)) return false;
        const float vw = F(e, kViewport), vh = F(e, kViewport + 4) - Strip();
        const float before = F(e, kZoom);
        *reinterpret_cast<float*>(e + kZoom) = zoom;
        *reinterpret_cast<uint16_t*>(e + kZoomIndex) = static_cast<uint16_t>(zoom >= 1.5f ? 2 : 1);
        float pos[2] = { cx - vw / (2.0f * zoom), cy - vh / (2.0f * zoom) };
        g_fnSetCamera(e, nullptr, pos);
        e[kWorldValid] = 0;
        // what the camera could not do (level edge) the shown part does
        const auto* cam = reinterpret_cast<const float*>(e + kCamera);
        g_offX = cx - (cam[0] + vw / (2.0f * zoom));
        g_offY = cy - (cam[1] + vh / (2.0f * zoom));
        g_hudDirty = true;
        ++g_switches;
        g_lastEngineZoom = zoom;
        Logger::Info("ViewZoom: engine zoom %.1f -> %.1f (view %.2f) about %.0f,%.0f.", before, zoom, g_zShown, cx, cy);
        return true;
    }

    // the view centre that keeps the anchor's world point under its screen point at zoom z
    void AnchorCentre(float z, float W, float H, float& vx, float& vy) {
        vx = g_anchorWX - (g_anchorPX - W * 0.5f) / z;
        vy = g_anchorWY - (g_anchorPY - H * 0.5f) / z;
    }

    void Step(int dir, bool atCursor) {
        uint8_t* e = Engine();
        if (!e) return;
        g_anchor = false;
        const float px = *reinterpret_cast<const float*>(0x006C1300);
        const float py = *reinterpret_cast<const float*>(0x006C1304);
        if (atCursor && g_shown.zoom > 0.01f && g_shown.m > 0.01f && py < g_shown.worldH) {
            g_anchor = true;
            g_anchorPX = px;
            g_anchorPY = py;
            g_anchorWX = g_shown.camX + (g_shown.cropX + px / g_shown.m) / g_shown.zoom;
            g_anchorWY = g_shown.camY + (g_shown.cropY + py / g_shown.m) / g_shown.zoom;
        }
        const float zMin = ZMin(e), zMax = ZMax();
        const float s = static_cast<float>(g_stepPercent) / 100.0f;
        const float z = g_zTarget;
        float n = dir > 0 ? z * s : z / s;
        // the engine's own levels are stops on the way: 1.0 where the level is large enough,
        // 2.0 = the HD pictures pixel for pixel
        for (const float stop : { 1.0f, 2.0f }) {
            if (dir > 0 && z < stop - 0.001f && n > stop + 0.001f) n = stop;
            if (dir < 0 && z > stop + 0.001f && n < stop - 0.001f) n = stop;
        }
        g_zTarget = (std::min)(zMax, (std::max)(zMin, n));
        g_hudDirty = true;
    }

    // ---- hooks
    // the game maps the mouse wheel to its zoom actions too: a step right after a wheel turn
    // zooms about the cursor, keys and buttons about the screen's centre
    bool FromWheel() {
        return GetTickCount() - g_wheelTime.load(std::memory_order_relaxed) < 300;
    }

    void __fastcall Hook_ZoomIn(uint8_t* e, void* edx, void* evt) {
        if (!HandlesZoom()) {
            o_ZoomIn(e, edx, evt);
            return;
        }
        Step(+1, FromWheel());
    }

    void __fastcall Hook_ZoomOut(uint8_t* e, void* edx, void* evt) {
        if (!HandlesZoom()) {
            o_ZoomOut(e, edx, evt);
            return;
        }
        Step(-1, FromWheel());
    }

    uint32_t __fastcall Hook_CanZoomIn(uint8_t* e, void* edx) {
        if (!HandlesZoom()) return o_CanZoomIn(e, edx);
        return CanZoomInNow(e) ? 1u : 0u;
    }

    void __fastcall Hook_ScreenToWorld(uint8_t* e, void* edx, float* out) {
        o_ScreenToWorld(e, edx, out);
        if (!out || !e || !g_enabled || !g_shown.on) return;
        const float zoom = F(e, kZoom);
        if (zoom < 0.01f) return;
        // the screen position the engine used, back from its answer
        const float px = (out[0] - F(e, kCamera)) * zoom;
        const float py = (out[1] - F(e, kCamera + 4)) * zoom;
        if (py >= g_shown.worldH) return;       // the HUD strip shows no world
        out[0] = g_shown.camX + (g_shown.cropX + px / g_shown.m) / g_shown.zoom;
        out[1] = g_shown.camY + (g_shown.cropY + py / g_shown.m) / g_shown.zoom;
    }

    void __fastcall Hook_CenterOn(uint8_t* e, void* edx, const float* point) {
        if (point && g_enabled) {
            g_centreX = point[0];
            g_centreY = point[1];
            g_haveCentre = true;
        }
        o_CenterOn(e, edx, point);
    }

    bool Hook(uintptr_t addr, const uint8_t* prolog, size_t n, void* detour, void** original) {
        return std::memcmp(reinterpret_cast<const void*>(addr), prolog, n) == 0 &&
               MH_CreateHook(reinterpret_cast<LPVOID>(addr), detour, original) == MH_OK &&
               MH_EnableHook(reinterpret_cast<LPVOID>(addr)) == MH_OK;
    }
}

bool Installed() { return g_installed; }
bool HandlesZoom() { return g_installed && g_enabled; }
bool Enabled() { return g_enabled; }

void SetEnabled(bool on) {
    if (!g_installed || on == g_enabled) return;
    g_enabled = on;
    g_shown.on = false;
    g_anchor = false;
    if (uint8_t* e = Engine()) {
        g_zTarget = g_zShown = F(e, kZoom) >= 1.5f ? 2.0f : 1.0f;
        g_level = nullptr;              // the next tick takes the level over again
        g_hudDirty = true;
    }
    Logger::Info("ViewZoom: smooth zoom %s (settings panel).", on ? "on" : "off");
}

int  StepPercent() { return g_stepPercent; }
void SetStepPercent(int p) { g_stepPercent = (std::max)(105, (std::min)(200, p)); }
int  MaxPercent() { return g_maxPercent; }

void SetMaxPercent(int p) {
    g_maxPercent = (std::max)(200, (std::min)(800, p));
    g_zTarget = (std::min)(g_zTarget, ZMax());
    g_hudDirty = true;
}

void OnGameTick(uint8_t* e) {
    if (!g_installed || !e) return;
    const void* level = *reinterpret_cast<void* const*>(e + kMapSurface);
    const bool newLevel = level != g_level;
    if (!g_enabled) {
        g_level = level;
        return;
    }
    const float zMin = ZMin(e), zMax = ZMax();
    if (newLevel) {
        // like the HD view: a level starts at 2.0 (sharp, HD pictures 1:1)
        g_level = level;
        const float start = SherwoodConfig::Get().hdView ? 2.0f : (F(e, kZoom) >= 1.5f ? 2.0f : 1.0f);
        g_zTarget = g_zShown = (std::min)(zMax, (std::max)(zMin, start));
        g_offX = g_offY = 0.0f;
        g_pushX = g_pushY = 0.0f;
        g_haveCentre = false;
        g_anchor = false;
        g_shown.on = false;
        g_hudDirty = true;
        // wheel turns from before the level (the question that started the mission, the loading) are not
        // for it: one such notch took a level 64 ms after its start from 2.0 to 1.74, the first briefing
        // was drawn at the engine's 1.0 with a black band beside the level (YouTuber's video, 2026-11-18)
        if (const int dropped = g_wheel.exchange(0)) {
            Logger::Info("ViewZoom: %d mouse wheel step(s) from before the level ignored.", dropped);
        }
        Logger::Info("ViewZoom: level %.0fx%.0f - zoom %.2f (from %.2f to %.2f).", F(e, kLevelSize), F(e, kLevelSize + 4),
                     g_zTarget, zMin, zMax);
    }
    // The game changed the engine's zoom itself - a script showing something with its zoom
    // animation, a loaded game: the player's zoom follows instead of magnifying the engine's new
    // picture back to the old zoom (user report: "the resolution turns native" during such events)
    const float ez = F(e, kZoom);
    if (e[kCamState] == 3 || e[kCamState] == 4) {
        g_gameZooming = true;
    } else if (newLevel) {
        g_lastEngineZoom = ez;
        g_gameZooming = false;
    } else if (g_gameZooming || std::fabs(ez - g_lastEngineZoom) > 0.01f) {
        if (std::fabs(ez - g_lastEngineZoom) > 0.01f) {
            g_zTarget = g_zShown = (std::min)(zMax, (std::max)(zMin, ez));
            g_offX = g_offY = 0.0f;
            g_anchor = false;
            g_hudDirty = true;
            Logger::Info("ViewZoom: the game set the zoom %.1f itself - followed.", ez);
        }
        g_lastEngineZoom = ez;
        g_gameZooming = false;
    }
    for (int n = g_wheel.exchange(0); n != 0; n += n > 0 ? -1 : 1) {
        Step(n > 0 ? +1 : -1, true);
    }
    const float test = g_testZoom.exchange(0.0f);
    if (test > 0.0f) {
        g_zTarget = g_zShown = (std::min)(zMax, (std::max)(zMin, test));
        g_hudDirty = true;
    }
    if (const int gz = g_testGameZoom.exchange(0)) {
        // test harness: the engine's own zoom animation, past the hooks - as a script starts it
        alignas(8) uint8_t evt[32] = {};
        if (gz > 0) o_ZoomIn(e, nullptr, evt);
        else o_ZoomOut(e, nullptr, evt);
    }
    g_zTarget = (std::min)(zMax, (std::max)(zMin, g_zTarget));
    const float want = EngineFor(g_zShown, zMin);
    if (std::fabs(F(e, kZoom) - want) > 0.01f) {
        float cx, cy;
        ViewCentre(e, cx, cy);
        SwitchEngine(e, want, cx, cy);
    }
    if (g_anchor && !Busy(e)) {
        // zoom about the cursor: the camera follows the view centre that keeps the point under it
        // (the shown part alone cannot near the zoom levels of the engine)
        const float W = F(e, kViewport), H = F(e, kViewport + 4) - Strip();
        const float z = F(e, kZoom);
        float vx, vy;
        AnchorCentre(g_zShown, W, H, vx, vy);
        const auto* cam = reinterpret_cast<const float*>(e + kCamera);
        float pos[2] = { vx - W / (2.0f * z), vy - H / (2.0f * z) };
        if (std::fabs(pos[0] - cam[0]) >= 1.0f || std::fabs(pos[1] - cam[1]) >= 1.0f) {
            g_fnSetCamera(e, nullptr, pos);
            // The engine's resting state blits its world surface as it is and only notes the new
            // camera: the background stayed where the old camera had it while the characters,
            // roofs and hints were drawn at the new one (user report 2026-11-14: things "come
            // apart" when zooming, Alt+Tab - a full redraw - fixed it). As scrolling, the engine
            // moves its world surface by camera - camera of the last render and draws the strips.
            if (e[kCamState] == 1) e[kCamState] = 2;
        }
        g_offX = vx - (cam[0] + W / (2.0f * z));
        g_offY = vy - (cam[1] + H / (2.0f * z));
        if (g_zShown == g_zTarget) g_anchor = false;
    }
    if (g_hudDirty) {
        g_hudDirty = false;
        RefreshHud(e);
    }
}

void PrepareLevel() {
    uint8_t* e = Engine();
    if (!g_installed || !g_enabled || !e) return;
    if (*reinterpret_cast<void* const*>(e + kMapSurface) == g_level) return;   // the level runs: nothing to do
    // A new level's first frame and its first briefing are drawn before the end of its first tick,
    // where OnGameTick runs: the engine was still at its own zoom (1.0 on most levels) - on a level
    // smaller than the screen the first frame and the briefing's backdrop showed it small with a
    // black band beside it (YouTuber's report 2026-11-17). The level's start comes before that
    // render, so it is drawn at the zoom the player then sees.
    OnGameTick(e);
}

void ClearOutsideLevel() {
    uint8_t* e = Engine();
    if (!e) return;
    const float zr = F(e, kRenderZoom) > 0.01f ? F(e, kRenderZoom) : F(e, kZoom);
    const float camX = F(e, kRenderCam), camY = F(e, kRenderCam + 4);
    const float W = F(e, kViewport), H = F(e, kViewport + 4) - Strip();
    const float lw = F(e, kLevelSize), lh = F(e, kLevelSize + 4);
    if (zr < 0.01f || W < 1.0f || H < 1.0f || lw < 1.0f || lh < 1.0f || W > 16384.0f || H > 16384.0f) return;
    // the level's right and bottom edges in frame pixels (the camera never lies left of / above it)
    const float r = (lw - camX) * zr, b = (lh - camY) * zr;
    const int right = static_cast<int>(std::ceil(r)), bottom = static_cast<int>(std::ceil(b));
    const int w = static_cast<int>(W), h = static_cast<int>(H);
    if (right > 0 && right < w) DDM::FillBack(RECT{ right, 0, w, h }, 0);
    if (bottom > 0 && bottom < h) DDM::FillBack(RECT{ 0, bottom, (std::min)(right, w), h }, 0);
}

void OnWorldDrawn() {
    g_viewPending = false;
    uint8_t* e = Engine();
    if (!g_installed || !g_enabled || !e || *reinterpret_cast<void* const*>(e + kMapSurface) != g_level) {
        g_shown.on = false;
        return;
    }
    // the shown zoom follows the target (about 60 ms)
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const double dt = g_last.QuadPart ? static_cast<double>(now.QuadPart - g_last.QuadPart) / static_cast<double>(g_freq.QuadPart) : 0.0;
    g_last = now;
    const float k = static_cast<float>(1.0 - std::exp(-(std::min)(dt, 0.25) / 0.06));
    g_zShown += (g_zTarget - g_zShown) * k;
    if (std::fabs(g_zTarget - g_zShown) < 0.002f * g_zTarget) g_zShown = g_zTarget;

    if (e[kCamState] == 3 || e[kCamState] == 4) {   // the game's own zoom animation: shown as it draws it
        g_shown.on = false;
        return;
    }
    // RHEngine::Update draws nothing in some frames (0x004C83FC: E+0xD1 set and E+0x4A94 & 0x1F):
    // the back buffer then still holds an older frame with its interface - the world of the last
    // drawn frame stays in the texture
    const bool worldDrawn = !(e[0xD1] == 1 && (*reinterpret_cast<const uint32_t*>(e + 0x4A94) & 0x1F));
    const float zr = F(e, kRenderZoom) > 0.01f ? F(e, kRenderZoom) : F(e, kZoom);
    const float camX = F(e, kRenderCam), camY = F(e, kRenderCam + 4);
    const float W = F(e, kViewport), H = F(e, kViewport + 4) - Strip();
    const float lw = F(e, kLevelSize), lh = F(e, kLevelSize + 4);
    if (zr < 0.01f || W < 1.0f || H < 1.0f || lw < 1.0f || lh < 1.0f) {
        g_shown.on = false;
        return;
    }
    // what of the frame holds the level (the camera never lies left of / above it)
    const float contentR = (std::min)(W, (lw - camX) * zr);
    const float contentB = (std::min)(H, (lh - camY) * zr);
    const float band = HedgeBand();
    float m = g_zShown / zr;
    if (contentR > 1.0f) m = (std::max)(m, W / contentR);       // never a black band (behind the hedge: no matter)
    if (contentB > 1.0f) m = (std::max)(m, (H - band) / contentB);
    if (m < 1.0005f) {
        g_shown = Shown{ false, 1.0f, 0.0f, 0.0f, camX, camY, zr, W, H };
        return;
    }
    if (g_anchor) {                 // zoom about the cursor (between ticks: the shown part follows)
        float vx, vy;
        AnchorCentre(g_zShown, W, H, vx, vy);
        g_offX = vx - (camX + W / (2.0f * zr));
        g_offY = vy - (camY + H / (2.0f * zr));
    }
    if (g_haveCentre) {             // the game centred the camera on a point: that point to the centre
        g_haveCentre = false;
        g_anchor = false;
        g_offX = g_centreX - (camX + W / (2.0f * zr));
        g_offY = g_centreY - (camY + H / (2.0f * zr));
    }
    g_offX += g_pushX;
    g_offY += g_pushY;
    g_pushX = g_pushY = 0.0f;
    const float halfW = W / (2.0f * m), halfH = H / (2.0f * m);
    float cx = W * 0.5f + g_offX * zr, cy = H * 0.5f + g_offY * zr;
    cx = (contentR - halfW < halfW) ? contentR * 0.5f : (std::max)(halfW, (std::min)(contentR - halfW, cx));
    cy = ClampShownY(cy, contentB, halfH, band / m, H);
    g_offX = (cx - W * 0.5f) / zr;
    g_offY = (cy - H * 0.5f) / zr;
    g_shown = Shown{ true, m, cx - halfW, cy - halfH, camX, camY, zr, W, H };
    // the world as it is now, before the windows are drawn over it - straight into the texture
    // the presenter magnifies (one copy per frame)
    const uint8_t* bits = nullptr;
    int pitch = 0, bw = 0, bh = 0;
    if (!worldDrawn && g_worldW >= static_cast<int>(W)) {
        g_viewPending = true;               // the texture still holds the last drawn world
    } else if (DDM::BackBits(bits, pitch, bw, bh) && bw >= static_cast<int>(W) && bh >= static_cast<int>(H) &&
               DDM::Present::UploadWorld(bits, pitch, bw, bh)) {
        g_worldW = bw;
        g_worldH = bh;
        g_viewPending = true;
    } else {
        g_shown.on = false;
    }
}

bool TakeView(View& out) {
    if (!g_viewPending) return false;
    g_viewPending = false;
    if (!g_shown.on || g_worldW <= 0) return false;
    out.m = g_shown.m;
    out.cropX = g_shown.cropX;
    out.cropY = g_shown.cropY;
    out.worldH = g_shown.worldH;
    out.snap = nullptr;                 // already in the presenter's texture
    out.w = g_worldW;
    out.h = g_worldH;
    return true;
}

void OnBackdropWorldDrawn(unsigned render) {
    g_backdropWorldRender = 0;
    uint8_t* e = Engine();
    if (!g_installed || !g_enabled || !e || *reinterpret_cast<void* const*>(e + kMapSurface) != g_level) return;
    // The magnification this render needs - computed for it: the last frame's may be of another zoom (a
    // level started at 2.0 and switched to the engine's 1.0 in the same tick drew its first briefing
    // before any frame of the new zoom - the briefing showed the level small with a black band beside it)
    Shown view{};
    {
        const float zr = F(e, kRenderZoom) > 0.01f ? F(e, kRenderZoom) : F(e, kZoom);
        const float camX = F(e, kRenderCam), camY = F(e, kRenderCam + 4);
        const float W = F(e, kViewport), H = F(e, kViewport + 4) - Strip();
        const float lw = F(e, kLevelSize), lh = F(e, kLevelSize + 4);
        if (zr < 0.01f || W < 1.0f || H < 1.0f || lw < 1.0f || lh < 1.0f || e[kCamState] == 3 || e[kCamState] == 4) return;
        const float contentR = (std::min)(W, (lw - camX) * zr);
        const float contentB = (std::min)(H, (lh - camY) * zr);
        const float band = HedgeBand();
        float m = g_zShown / zr;
        if (contentR > 1.0f) m = (std::max)(m, W / contentR);
        if (contentB > 1.0f) m = (std::max)(m, (H - band) / contentB);
        if (m < 1.0005f) return;                // shown as drawn: the level copied from the frame is right
        const float halfW = W / (2.0f * m), halfH = H / (2.0f * m);
        float cx = W * 0.5f + g_offX * zr, cy = H * 0.5f + g_offY * zr;
        cx = (contentR - halfW < halfW) ? contentR * 0.5f : (std::max)(halfW, (std::min)(contentR - halfW, cx));
        cy = ClampShownY(cy, contentB, halfH, band / m, H);
        view = Shown{ true, m, cx - halfW, cy - halfH, camX, camY, zr, W, H };
    }
    const uint8_t* bits = nullptr;
    int pitch = 0, bw = 0, bh = 0;
    if (!DDM::BackBits(bits, pitch, bw, bh) || bw <= 0 || bh <= 0) return;
    g_backdropWorld.resize(static_cast<size_t>(bw) * bh);
    for (int y = 0; y < bh; ++y) {
        std::memcpy(g_backdropWorld.data() + static_cast<size_t>(y) * bw, bits + static_cast<size_t>(y) * pitch,
                    static_cast<size_t>(bw) * 2);
    }
    g_backdropWorldW = bw;
    g_backdropWorldH = bh;
    g_backdropView = view;
    g_backdropWorldRender = render;
}

bool BackdropWorld(unsigned render, View& out) {
    if (!g_enabled || render == 0 || g_backdropWorldRender != render || !g_backdropView.on) return false;
    out.m = g_backdropView.m;
    out.cropX = g_backdropView.cropX;
    out.cropY = g_backdropView.cropY;
    out.worldH = g_backdropView.worldH;
    out.snap = g_backdropWorld.data();
    out.w = g_backdropWorldW;
    out.h = g_backdropWorldH;
    return true;
}

bool LastWorld(View& out) {
    if (!g_enabled || !g_shown.on || g_worldW <= 0) return false;
    int w = 0, h = 0;
    if (!DDM::Present::ReadWorld(g_readBack, w, h)) return false;
    out.m = g_shown.m;
    out.cropX = g_shown.cropX;
    out.cropY = g_shown.cropY;
    out.worldH = g_shown.worldH;
    out.snap = g_readBack.data();
    out.w = w;
    out.h = h;
    return true;
}

bool Magnifying() {
    return g_enabled && g_shown.on && g_shown.m > 1.0005f;
}

bool ScreenToFrame(float& x, float& y) {
    if (!g_enabled || !g_shown.on || y >= g_shown.worldH) return false;
    x = g_shown.cropX + x / g_shown.m;
    y = g_shown.cropY + y / g_shown.m;
    return true;
}

bool MapViewRect(float r[4]) {
    if (!g_enabled || !g_shown.on || g_shown.frameW < 1.0f || g_shown.worldH < 1.0f) return false;
    const float w = r[2] - r[0], h = r[3] - r[1];
    const float l = r[0] + w * g_shown.cropX / g_shown.frameW;
    const float t = r[1] + h * g_shown.cropY / g_shown.worldH;
    // whole pixels, as the game's own rectangle (the drawing routine draws nothing otherwise)
    r[0] = std::floor(l + 0.5f);
    r[1] = std::floor(t + 0.5f);
    r[2] = (std::max)(r[0] + 2.0f, std::floor(l + w / g_shown.m + 0.5f));
    r[3] = (std::max)(r[1] + 2.0f, std::floor(t + h / g_shown.m + 0.5f));
    return true;
}

void OnScrollClamped(float dx, float dy) {
    if (!g_enabled || !g_shown.on) return;
    g_pushX += dx;
    g_pushY += dy;
}

bool CanZoomOut() {
    return CanZoomOutNow(Engine());
}

void OnWheel(int notches) {
    if (HandlesZoom()) g_wheel.fetch_add(notches);
}

void NoteWheel() {
    g_wheelTime.store(GetTickCount(), std::memory_order_relaxed);
}

int Describe(char* out, int size) {
    uint8_t* e = Engine();
    return std::snprintf(out, size, "installed=%d on=%d target=%.3f shown=%.3f engine=%.1f m=%.3f crop=%.0f,%.0f off=%.1f,%.1f min=%.2f max=%.2f switches=%u lupa=%d",
                         g_installed ? 1 : 0, g_enabled ? 1 : 0, g_zTarget, g_zShown, e ? F(e, kZoom) : 0.0f,
                         g_shown.m, g_shown.cropX, g_shown.cropY, g_offX, g_offY, e ? ZMin(e) : 0.0f, ZMax(), g_switches,
                         g_shown.on ? 1 : 0);
}

void TestSetZoom(float z) {
    g_testZoom.store(z);
}

void TestGameZoom(int dir) {
    g_testGameZoom.store(dir > 0 ? 1 : -1);
}

void Install() {
    const auto& cfg = SherwoodConfig::Get();
    g_enabled = cfg.smoothZoom;
    g_stepPercent = (std::max)(105, (std::min)(200, cfg.zoomStep));
    g_maxPercent = (std::max)(200, (std::min)(800, cfg.zoomMax));
    QueryPerformanceFrequency(&g_freq);
    if (cfg.width < 2048 || !cfg.hdView || cfg.renderer != 1 || cfg.presenter != 0) {
        Logger::Info("ViewZoom: no smooth zoom (needs the HD view: Width >= 2048, HDView=1, Renderer=1, Presenter=0).");
        return;
    }
    // push esi / mov esi,ecx / call Can... (relative: MinHook moves it) / test al,al / je
    static const uint8_t kZoomIn[]   = { 0x56, 0x8B, 0xF1, 0xE8, 0x88, 0xBE, 0xFE, 0xFF, 0x84, 0xC0, 0x74 };
    static const uint8_t kZoomOut[]  = { 0x56, 0x8B, 0xF1, 0xE8, 0x38, 0xBF, 0xFE, 0xFF, 0x84, 0xC0, 0x74 };
    static const uint8_t kCanIn[]    = { 0xD9, 0x81, 0x5C, 0x47, 0x00, 0x00, 0xD8, 0x1D, 0xF4, 0x74, 0x67, 0x00 };
    static const uint8_t kToWorld[]  = { 0xD9, 0x05, 0x9C, 0x74, 0x67, 0x00, 0xD8, 0xB1, 0x5C, 0x47, 0x00, 0x00 };
    static const uint8_t kCenter[]   = { 0x6A, 0xFF, 0x68, 0x40, 0xC4, 0x65, 0x00, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x50 };
    static const uint8_t kMsgCtor[]  = { 0x8B, 0x44, 0x24, 0x08, 0x56, 0x50, 0x8B, 0xF1, 0x6A, 0x00 };
    static const uint8_t kMsgDtor[]  = { 0xC7, 0x01, 0x64, 0x9B, 0x67, 0x00 };
    const bool helpers = std::memcmp(reinterpret_cast<const void*>(0x005490A0), kMsgCtor, sizeof(kMsgCtor)) == 0 &&
                         std::memcmp(reinterpret_cast<const void*>(0x00549110), kMsgDtor, sizeof(kMsgDtor)) == 0;
    const bool ok = helpers &&
                    Hook(0x004DBE40, kZoomIn, sizeof(kZoomIn), reinterpret_cast<void*>(&Hook_ZoomIn), reinterpret_cast<void**>(&o_ZoomIn)) &&
                    Hook(0x004DBDB0, kZoomOut, sizeof(kZoomOut), reinterpret_cast<void*>(&Hook_ZoomOut), reinterpret_cast<void**>(&o_ZoomOut)) &&
                    Hook(0x004C7CD0, kCanIn, sizeof(kCanIn), reinterpret_cast<void*>(&Hook_CanZoomIn), reinterpret_cast<void**>(&o_CanZoomIn)) &&
                    Hook(0x004DEE70, kToWorld, sizeof(kToWorld), reinterpret_cast<void*>(&Hook_ScreenToWorld), reinterpret_cast<void**>(&o_ScreenToWorld)) &&
                    Hook(0x004C7F60, kCenter, sizeof(kCenter), reinterpret_cast<void*>(&Hook_CenterOn), reinterpret_cast<void**>(&o_CenterOn));
    if (!ok) {
        // half-installed hooks pass everything through (g_installed stays false)
        Logger::Warn("ViewZoom: the zoom code does not match this build - smooth zoom NOT installed.");
        return;
    }
    g_installed = true;
    Logger::Info("ViewZoom: smooth zoom installed (%s): step %d %%, up to %d %%; the world is magnified in the presenter, the interface stays.",
                 g_enabled ? "on" : "off: SmoothZoom=0", g_stepPercent, g_maxPercent);
}

} // namespace ViewZoom
