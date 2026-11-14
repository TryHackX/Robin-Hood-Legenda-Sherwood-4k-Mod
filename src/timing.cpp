#include "timing.h"
#include "config.h"
#include "logger.h"
#include "pattern.h"
#include "proxy_winmm.h"
#include "render_ddraw.h"
#include "window_manager.h"
#include "diag.h"
#include "audio_fix.h"
#include "motion_smooth.h"
#include "input_fix.h"
#include "test_harness.h"
#include "watchdog.h"
#include "ddm/ddm.h"
#include "ddm/ddm_hd.h"
#include "widescreen_patch.h"

#include <MinHook.h>
#include <emmintrin.h>
#include <intrin.h>
#include <algorithm>
#include <cmath>
#include <cstring>

#pragma intrinsic(_ReturnAddress)

// Game loop pacing and extra frames between the 25 Hz game ticks.
//
// RHGame::Run does everything once per 40 ms tick: input, logic, then RHGame::RenderFrame
// (0x005105D0), which draws the world, HUD and cursor and presents. The camera therefore moves
// in 25 steps per second and so does the cursor.
//
// While waiting for the next tick we call the game's own RenderFrame again ("extra frames"),
// one per display refresh:
//   * if the camera scrolls, every frame (the tick's own one included) moves it by the part of
//     the tick's scroll distance that matches the time since the previous frame, so the map
//     glides at a constant speed whatever the frame rate is;
//   * otherwise the frame is redrawn as is, which moves the software cursor to its current
//     position (the input thread updates it continuously).
// Game logic still runs once per tick on an exact 40 ms average schedule, so game speed is
// untouched. With VSync=1 the presenter blocks until the display refresh, which paces the loop.
namespace Timing {
    namespace {
        LARGE_INTEGER g_qpcFreq{};
        LARGE_INTEGER g_qpcStart{};
        LARGE_INTEGER g_lastFrameQpc{};
        DWORD g_baseTickCount = 0;

        double g_currentFPS = 0.0;
        double g_fpsAccumTime = 0.0;
        int g_fpsFrameCount = 0;

        using PFN_timeBeginPeriod = DWORD(WINAPI*)( UINT);
        using PFN_timeEndPeriod   = DWORD(WINAPI*)(UINT);
        using PFN_timeGetTime     = DWORD(WINAPI*)();
        using PFN_GetTickCount    = DWORD(WINAPI*)();
        using PFN_Sleep           = VOID(WINAPI*)(DWORD);
        using PFN_QPC             = BOOL(WINAPI*)(LARGE_INTEGER*);

        PFN_timeBeginPeriod o_timeBeginPeriod = nullptr;
        PFN_timeEndPeriod   o_timeEndPeriod   = nullptr;
        PFN_timeGetTime     o_timeGetTime     = nullptr;
        PFN_GetTickCount    o_GetTickCount    = nullptr;
        PFN_Sleep           o_Sleep           = nullptr;
        PFN_QPC             o_QPC             = nullptr;

        // Engine functions (see docs\RE_NOTES.md)
        using PFN_RenderFrame      = void(__thiscall*)(void* pGame, int showCursor, int doPresent); // ret 8
        using PFN_ClampScrollDelta = uint8_t(__fastcall*)(uint8_t* pEngine, void* edx);
        using PFN_CursorJitter     = void(__cdecl*)();

        PFN_RenderFrame      g_fnRenderFrame    = reinterpret_cast<PFN_RenderFrame>(0x005105D0);
        PFN_ClampScrollDelta o_ClampScrollDelta = nullptr;
        PFN_CursorJitter     o_CursorJitter     = nullptr;

        uintptr_t g_addrEngineClampScrollRet = 0x004C86DD;

        // Extra-frame state (main thread only)
        bool     g_extraFramesOk     = false;  // engine functions verified at start-up
        bool     g_inExtraFrame      = false;
        bool     g_scrollThisTick    = false;  // the tick's own frame scrolled the camera
        int      g_displayHz         = 60;
        float    g_velX              = 0.0f;   // camera speed of the current tick, pixels per 40 ms
        float    g_velY              = 0.0f;
        float    g_remX              = 0.0f;   // distance not yet applied (sub-pixel remainder)
        float    g_remY              = 0.0f;
        float    g_quantum           = 1.0f;
        uint64_t g_lastScrollQpc     = 0;      // time of the last applied scroll step
        uint64_t g_lastFrameStartQpc = 0;      // when the previous frame started
        uint64_t g_prevTickExitQpc   = 0;
        uint64_t g_tickIdealStartQpc = 0;      // when the current tick should have started
        double   g_avgMainWorkUs     = 0.0;
        double   g_avgExtraUs        = 0.0;

        inline uint64_t ReadRawQPC() {
            LARGE_INTEGER now{};
            if (o_QPC) {
                o_QPC(&now);
            } else {
                QueryPerformanceCounter(&now);
            }
            return static_cast<uint64_t>(now.QuadPart);
        }

        // camera is zooming / being moved by a script: leave it alone
        bool CameraBusy(const uint8_t* pEngine) {
            const uint8_t camState = pEngine[0x48C0];
            return camState == 3 || camState == 4 ||
                   pEngine[0x4790] != 0 || pEngine[0x4791] != 0 || pEngine[0x4768] != 0;
        }

        // The game's scroll speeds (pixels per tick) were tuned for 640-1024 pixel wide screens;
        // on a 2560 pixel wide view the same speed crawls. EdgeScrollSpeed=0 scales it with the
        // internal width, any other value is a fixed percentage.
        float ScrollSpeedScale() {
            const auto& cfg = SherwoodConfig::Get();
            if (cfg.edgeScrollSpeed > 0) {
                return static_cast<float>(cfg.edgeScrollSpeed) / 100.0f;
            }
            int w = 0, h = 0;
            WindowManager::GetInternalResolution(w, h);
            return std::clamp(static_cast<float>(w) / 1024.0f, 1.0f, 3.0f);
        }

        uint64_t FramePeriodQpc() {
            const auto& cfg = SherwoodConfig::Get();
            const uint64_t freq = static_cast<uint64_t>(g_qpcFreq.QuadPart);
            if (!cfg.vsync && cfg.framesPerTick > 0) {
                return (freq * 40ULL / 1000ULL) / static_cast<uint64_t>(std::clamp(cfg.framesPerTick, 1, 10));
            }
            // +1: Windows reports 143/119/59 Hz for the fractional 144/120/60 Hz modes
            return freq / static_cast<uint64_t>(std::max(g_displayHz + 1, 30));
        }

        // Part of the tick's scroll distance that belongs to the time since the previous step.
        void TakeScrollStep(uint64_t nowQpc, float& qX, float& qY) {
            const uint64_t tickLen = static_cast<uint64_t>(g_qpcFreq.QuadPart) * 40ULL / 1000ULL;
            uint64_t dt;
            if (g_lastScrollQpc == 0 || nowQpc <= g_lastScrollQpc || (nowQpc - g_lastScrollQpc) > tickLen * 2) {
                // scrolling starts (again): begin with one frame's worth instead of a jump
                dt = std::min(FramePeriodQpc(), tickLen);
                g_remX = 0.0f;
                g_remY = 0.0f;
            } else {
                dt = std::min(nowQpc - g_lastScrollQpc, tickLen);
            }
            const float frac = static_cast<float>(static_cast<double>(dt) / static_cast<double>(tickLen));
            const float rawX = g_velX * frac + g_remX;
            const float rawY = g_velY * frac + g_remY;
            qX = std::trunc(rawX / g_quantum) * g_quantum;
            qY = std::trunc(rawY / g_quantum) * g_quantum;
            g_remX = rawX - qX;
            g_remY = rawY - qY;
            g_lastScrollQpc = nowQpc;
        }

        // ---- things the game does once per RenderFrame and that must stay once per tick ----------

        // 0x005B7F60 advances a sprite's animation; the scenery layer calls it from the renderer
        // (0x005C6763, 0x005C67C5), so chimney smoke, flags etc. would run as many times faster
        // as there are frames per tick. No game logic runs during an extra frame, so any call
        // made then comes from the renderer and is skipped. 2 is what the function itself
        // returns when animations are frozen.
        using PFN_AnimAdvance = int(__fastcall*)(void* sprite, void* edx, int arg);
        PFN_AnimAdvance o_AnimAdvance = nullptr;

        int __fastcall Hook_AnimAdvance(void* sprite, void* edx, int arg) {
            if (g_inExtraFrame) {
                return 2;
            }
            return o_AnimAdvance(sprite, edx, arg);
        }

        // 0x0055EE00 (HUD, called from RenderFrame) walks the character panels and runs
        // 0x005D9180 on each: the blink of the three action icons, counted in drawn frames
        // (5 frames per on/off phase). At 144 frames per second the icons flickered six times
        // too fast. The function draws nothing itself, so extra frames skip it altogether.
        using PFN_HudTimers = void(__fastcall*)(void* hud, void* edx);
        PFN_HudTimers o_HudTimers = nullptr;

        void __fastcall Hook_HudTimers(void* hud, void* edx) {
            if (g_inExtraFrame) {
                return;
            }
            o_HudTimers(hud, edx);
        }

        // Other state that RenderFrame advances once per drawn frame; saved before an extra
        // frame and put back after it, so it keeps moving once per tick:
        //   [pGame+0x3574]  frames left for the on-screen message (0x0051068F: dec)
        //   0x006C12A0 / 0x006C133C / 0x006C11E8 / 0x006C1212 / 0x006C11EE
        //                   cursor shape transition: frame, delay, target, direction, started
        //                   (cursor draw 0x005F22D9..0x005F239A)
        //   0x006C121C / 0x006C12AD  pulse of the highlighted cursor (cursor type 2 with [0x6C12B6]
        //                   set): +-10 per drawn frame between 0 and 100, direction flag
        //                   (0x005F211E..0x005F2153) - 6x too fast with extra frames
        struct PerRenderState {
            uint32_t messageFrames;
            uint32_t cursorFrame;
            uint32_t cursorDelay;
            uint32_t cursorTarget;
            uint8_t  cursorGrowing;
            uint8_t  cursorStarted;
            uint16_t cursorPulse;
            uint8_t  cursorPulseUp;
        };

        void SavePerRenderState(const void* pGame, PerRenderState& s) {
            s.messageFrames = *reinterpret_cast<const uint32_t*>(reinterpret_cast<uintptr_t>(pGame) + 0x3574);
            s.cursorFrame   = *reinterpret_cast<const uint32_t*>(0x006C12A0);
            s.cursorDelay   = *reinterpret_cast<const uint32_t*>(0x006C133C);
            s.cursorTarget  = *reinterpret_cast<const uint32_t*>(0x006C11E8);
            s.cursorGrowing = *reinterpret_cast<const uint8_t*>(0x006C1212);
            s.cursorStarted = *reinterpret_cast<const uint8_t*>(0x006C11EE);
            s.cursorPulse   = *reinterpret_cast<const uint16_t*>(0x006C121C);
            s.cursorPulseUp = *reinterpret_cast<const uint8_t*>(0x006C12AD);
        }

        void RestorePerRenderState(void* pGame, const PerRenderState& s) {
            *reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(pGame) + 0x3574) = s.messageFrames;
            *reinterpret_cast<uint32_t*>(0x006C12A0) = s.cursorFrame;
            *reinterpret_cast<uint32_t*>(0x006C133C) = s.cursorDelay;
            *reinterpret_cast<uint32_t*>(0x006C11E8) = s.cursorTarget;
            *reinterpret_cast<uint8_t*>(0x006C1212)  = s.cursorGrowing;
            *reinterpret_cast<uint8_t*>(0x006C11EE)  = s.cursorStarted;
            *reinterpret_cast<uint16_t*>(0x006C121C) = s.cursorPulse;
            *reinterpret_cast<uint8_t*>(0x006C12AD)  = s.cursorPulseUp;
        }

        // The game's set-cursor function (0x005F14C0, cdecl(id, flag)). RenderFrame picks the cursor
        // for what lies under the mouse in the world (0x004DDF0A). In the game's own ticks the mouse
        // event of the HUD comes first and keeps the arrow over HUD buttons; between ticks there is
        // no such event, so in extra frames the world cursor won and the two shapes alternated 144
        // times a second (user report 2026-11-14: cursor flickering very fast in the top-right
        // corner). The original never draws between ticks: in extra frames the cursor keeps the
        // shape the tick gave it (its position still follows the mouse).
        using PFN_SetCursor = void(__cdecl*)(uint32_t id, uint32_t flag);
        PFN_SetCursor o_SetCursor = nullptr;

        void __cdecl Hook_SetCursor(uint32_t id, uint32_t flag) {
            if (DDM::TraceActive()) {
                char text[96];
                std::snprintf(text, sizeof(text), "SETCURSOR id=%04X by %08X now=%04X extra=%d%s", id & 0xFFFF,
                              static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress())),
                              *reinterpret_cast<const uint16_t*>(0x006C1216), g_inExtraFrame ? 1 : 0,
                              g_inExtraFrame ? " (kept)" : "");
                DDM::TraceMark(text);
            }
            if (g_inExtraFrame) {
                return;
            }
            o_SetCursor(id, flag);
        }

        // 0x00491B5A (player-character draw): `dec word ptr [esi+0x580]`, the length of the
        // afterimage effect counted in drawn frames.
        __declspec(naked) void GhostCounterGuard() {
            __asm {
                cmp byte ptr [g_inExtraFrame], 0
                jne skip
                dec word ptr [esi + 0x580]
            skip:
                ret
            }
        }

        // ---- smooth object movement (motion_smooth.cpp) ------------------------------------------
        using PFN_RenderFrameHook = void(__fastcall*)(void* pGame, void* edx, int showCursor, int doPresent);
        PFN_RenderFrameHook o_RenderFrame = nullptr;
        constexpr uintptr_t kAddrTickRenderRet = 0x0050FF0A; // RHGame::Run, after its RenderFrame call
        bool g_extraActive  = false; // the previous tick was drawn with extra frames
        bool g_tickSmoothed = false; // this tick's own frame showed interpolated positions
        bool g_tickExtras   = false; // this tick is expected to be drawn with extra frames
        float g_renderPhase = -1.0f; // see Timing::RenderPhase

        float MotionAlpha(uint64_t nowQpc) {
            const uint64_t tickLen = static_cast<uint64_t>(g_qpcFreq.QuadPart) * 40ULL / 1000ULL;
            if (g_tickIdealStartQpc == 0 || nowQpc <= g_tickIdealStartQpc) {
                return 0.0f;
            }
            const double a = static_cast<double>(nowQpc - g_tickIdealStartQpc) / static_cast<double>(tickLen);
            return (a >= 1.0) ? 1.0f : static_cast<float>(a);
        }

        void __fastcall Hook_RenderFrame(void* pGame, void* edx, int showCursor, int doPresent) {
            const bool fromTick = !g_inExtraFrame &&
                                  reinterpret_cast<uintptr_t>(_ReturnAddress()) == kAddrTickRenderRet;
            bool overridden = false;
            if (MotionSmooth::IsEnabled() && (g_inExtraFrame || fromTick)) {
                if (fromTick) {
                    MotionSmooth::CaptureTick();
                    // Smoothing only makes sense while extra frames are being drawn; without
                    // them the objects are shown where they really are.
                    g_tickSmoothed = g_extraActive;
                }
                if (g_tickSmoothed) {
                    MotionSmooth::Apply(MotionAlpha(ReadRawQPC()));
                    overridden = true;
                }
            }
            if (fromTick) {
                g_tickExtras = g_extraActive;
                DDM::HD::OnTickFrame(); // sprite animation steps are counted in game ticks
            }
            g_renderPhase = ((g_inExtraFrame || fromTick) && g_tickExtras) ? MotionAlpha(ReadRawQPC()) : -1.0f;
            if (DDM::TraceActive()) {
                DDM::TraceMark(g_inExtraFrame ? "--- RenderFrame (extra frame)" : (fromTick ? "--- RenderFrame (tick)" : "--- RenderFrame (other caller)"));
            }
            o_RenderFrame(pGame, edx, showCursor, doPresent);
            if (DDM::TraceActive()) {
                DDM::TraceMark("--- RenderFrame end");
            }
            if (overridden) {
                MotionSmooth::Restore();
            }
        }

        // RHGame::RenderFrame calls 0x005F1A90 once per frame; it nudges the cursor by a random
        // +-1 pixel and queues a matching mouse event (keeps hover logic alive, makes the pointer
        // tremble). Extra frames must not multiply that.
        void __cdecl Hook_CursorJitter() {
            if (g_inExtraFrame) {
                return;
            }
            o_CursorJitter();
        }

        // RHEngine::ClampScrollDelta is called from RHEngine::Update when the camera scrolls.
        // In the tick's own frame we note the tick's speed and replace it by the step that is due
        // right now; the extra frames apply the rest.
        uint8_t __fastcall Hook_ClampScrollDelta(uint8_t* pEngine, void* edx) {
            const auto& cfg = SherwoodConfig::Get();
            if (pEngine && !g_inExtraFrame &&
                reinterpret_cast<uintptr_t>(_ReturnAddress()) == g_addrEngineClampScrollRet) {
                auto* inVel = reinterpret_cast<float*>(pEngine + 0x48B8);
                g_scrollThisTick = false;
                if (!CameraBusy(pEngine) && (inVel[0] != 0.0f || inVel[1] != 0.0f)) {
                    const float scale = ScrollSpeedScale();
                    if (cfg.smoothCameraScroll && g_extraFramesOk) {
                        const float zoom = *reinterpret_cast<const float*>(pEngine + 0x475C);
                        g_quantum = (zoom > 0.01f && zoom < 0.99f) ? (1.0f / zoom) : 1.0f;
                        g_velX = inVel[0] * scale;
                        g_velY = inVel[1] * scale;
                        g_scrollThisTick = true;
                        float qX = 0.0f, qY = 0.0f;
                        TakeScrollStep(ReadRawQPC(), qX, qY);
                        inVel[0] = qX;
                        inVel[1] = qY;
                    } else {
                        inVel[0] *= scale;
                        inVel[1] *= scale;
                    }
                }
            }
            return o_ClampScrollDelta(pEngine, edx);
        }

        // Precise hybrid wait for a given number of microseconds using Sleep(1) + _mm_pause()
        void HybridWaitMicroseconds(int64_t waitUs) {
            if (waitUs <= 0) return;

            const uint64_t startTicks = ReadRawQPC();
            const uint64_t targetTicks = startTicks + (static_cast<uint64_t>(waitUs) * g_qpcFreq.QuadPart) / 1000000ULL;

            while (true) {
                uint64_t now = ReadRawQPC();
                if (now >= targetTicks) break;

                uint64_t remTicks = targetTicks - now;
                uint64_t remUs = (remTicks * 1000000ULL) / g_qpcFreq.QuadPart;

                if (remUs > 2000) {
                    if (o_Sleep) {
                        o_Sleep(1);
                    } else {
                        Sleep(1);
                    }
                } else {
                    _mm_pause();
                }
            }
        }

        DWORD WINAPI Hook_GetTickCount() {
            uint64_t deltaTicks = ReadRawQPC() - static_cast<uint64_t>(g_qpcStart.QuadPart);
            uint64_t elapsedMs = (deltaTicks * 1000ULL) / static_cast<uint64_t>(g_qpcFreq.QuadPart);
            return g_baseTickCount + static_cast<DWORD>(elapsedMs);
        }

        DWORD WINAPI Hook_timeGetTime() {
            return Hook_GetTickCount();
        }

        DWORD WINAPI Hook_timeEndPeriod(UINT uPeriod) {
            if (SherwoodConfig::Get().highResolutionTimer && uPeriod == 1) {
                // Keep 1ms kernel timer resolution active throughout game execution
                return 0; // TIMERR_NOERROR
            }
            return o_timeEndPeriod ? o_timeEndPeriod(uPeriod) : 0;
        }

        VOID WINAPI Hook_Sleep(DWORD dwMilliseconds) {
            if (dwMilliseconds == 0) {
                SwitchToThread();
                return;
            }
            if (o_Sleep) {
                o_Sleep(dwMilliseconds);
            }
        }

        void WaitUntilQpc(uint64_t targetTicks) {
            while (true) {
                uint64_t now = ReadRawQPC();
                if (now >= targetTicks) break;

                uint64_t remTicks = targetTicks - now;
                uint64_t remUs = (remTicks * 1000000ULL) / static_cast<uint64_t>(g_qpcFreq.QuadPart);
                if (remUs > 1500) {
                    if (o_Sleep) {
                        o_Sleep(1);
                    } else {
                        Sleep(1);
                    }
                } else {
                    _mm_pause();
                }
            }
        }

        // Called from the hooked game loop frame limiter @ 0x00510114 (end of every tick's work).
        void __stdcall PaceGameLoopTick(void* pGame, DWORD /*frameStartTick*/, DWORD targetMs) {
            const auto& cfg = SherwoodConfig::Get();
            const uint64_t entryQpc = ReadRawQPC();
            const uint64_t qpcFreq  = static_cast<uint64_t>(g_qpcFreq.QuadPart);
            const uint64_t tickLen  = (qpcFreq * static_cast<uint64_t>(targetMs)) / 1000ULL;

            Watchdog::Beat();
            InputFix::Poll();
            TestHarness::OnGameTick();

            double mainWorkUs = 0.0;
            if (g_prevTickExitQpc != 0 && entryQpc > g_prevTickExitQpc) {
                mainWorkUs = static_cast<double>(entryQpc - g_prevTickExitQpc) * 1000000.0 / static_cast<double>(qpcFreq);
                if (mainWorkUs < 200000.0) {
                    g_avgMainWorkUs = (g_avgMainWorkUs <= 0.0)
                        ? mainWorkUs
                        : (g_avgMainWorkUs * 0.8 + mainWorkUs * 0.2);
                }
            }
            Diag::OnTickWorkEnd(mainWorkUs);

            // When should the next tick start? Ticks follow an ideal schedule (one every targetMs)
            // so that starting a little early or late for the display does not change game speed.
            // After a long stall (loading, pause, background) the schedule restarts from now.
            if (g_tickIdealStartQpc == 0 || g_prevTickExitQpc == 0 || entryQpc > g_tickIdealStartQpc + 3 * tickLen) {
                g_tickIdealStartQpc = (g_prevTickExitQpc != 0 && entryQpc - g_prevTickExitQpc < tickLen)
                    ? g_prevTickExitQpc
                    : entryQpc - std::min<uint64_t>(tickLen, entryQpc);
            }
            const uint64_t nextStartQpc = g_tickIdealStartQpc + tickLen;

            auto* pEngine = pGame ? *reinterpret_cast<uint8_t**>(reinterpret_cast<uintptr_t>(pGame) + 0x18) : nullptr;
            const bool scrolling = g_scrollThisTick;
            g_scrollThisTick = false;

            DDM::HD::OnGameTick();                 // HD background variants: hot keys, switches
            WidescreenPatch::OnGameTick(pEngine);  // HD view: levels start at the 2.0 zoom

            // Some maps are smaller than a high-resolution view; the engine then shows the map in
            // the top-left corner and black around it. Note it once per map / zoom change.
            if (pEngine) {
                static float s_mapW = 0.0f, s_mapH = 0.0f, s_zoom = 0.0f;
                const float mapW = *reinterpret_cast<const float*>(pEngine + 0x4730);
                const float mapH = *reinterpret_cast<const float*>(pEngine + 0x4734);
                const float zoom = *reinterpret_cast<const float*>(pEngine + 0x475C);
                if (mapW != s_mapW || mapH != s_mapH || zoom != s_zoom) {
                    s_mapW = mapW; s_mapH = mapH; s_zoom = zoom;
                    const float vpW = *reinterpret_cast<const float*>(pEngine + 0x4728);
                    const float vpH = *reinterpret_cast<const float*>(pEngine + 0x472C);
                    if (mapW > 0.0f && mapH > 0.0f && zoom > 0.01f) {
                        const float viewW = vpW / zoom;
                        // the bottom 80 pixels belong to the HUD unless FullHeightView removed the strip
                        const float viewH = (vpH - (cfg.fullHeightView ? 0.0f : 80.0f)) / zoom;
                        Logger::Info("Map %.0fx%.0f px, view %.0fx%.0f px at zoom %.1f%s%s.",
                                     mapW, mapH, viewW, viewH, zoom,
                                     (viewW > mapW) ? " - map NARROWER than the view (black band at the side)" : "",
                                     (viewH > mapH) ? " - map LOWER than the view (black band below)" : "");
                    }
                }
            }

            const uint64_t periodQpc = FramePeriodQpc();
            const bool vsyncPaced = cfg.vsync && DDM::IsActive(); // Present blocks until the refresh
            bool ranExtra = false;

            if (targetMs == 0x28 && g_extraFramesOk && pEngine &&
                (cfg.smoothCameraScroll || cfg.smoothCursor) &&
                *reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(pGame) + 0x63) == 0 &&
                pEngine[0xD1] == 0 && periodQpc * 2 <= tickLen) {
                const uint64_t guardQpc = (qpcFreq * 300ULL) / 1000000ULL;

                for (int i = 0; i < 16; ++i) {
                    uint64_t now = ReadRawQPC();
                    if (vsyncPaced) {
                        // One frame per refresh. The refresh closest to the tick's start time is
                        // left to the tick's own frame.
                        if (now + periodQpc / 2 >= nextStartQpc) break;
                        // If Present did not block (window covered or minimised) do not spin.
                        const uint64_t earliest = g_lastFrameStartQpc + (periodQpc * 3) / 4;
                        if (now < earliest) {
                            WaitUntilQpc(earliest);
                            now = ReadRawQPC();
                            if (now + periodQpc / 2 >= nextStartQpc) break;
                        }
                    } else {
                        const double estUs = (g_avgExtraUs > 0.0) ? g_avgExtraUs : std::max(g_avgMainWorkUs * 0.7, 2000.0);
                        const uint64_t estQpc = static_cast<uint64_t>(estUs * static_cast<double>(qpcFreq) / 1000000.0);
                        const uint64_t next = g_lastFrameStartQpc + periodQpc;
                        if (next + estQpc + guardQpc > nextStartQpc) break; // would run into the tick
                        if (next > now) {
                            WaitUntilQpc(next);
                            now = ReadRawQPC();
                        }
                    }
                    if (CameraBusy(pEngine)) {
                        break;
                    }

                    float qX = 0.0f, qY = 0.0f;
                    if (scrolling && cfg.smoothCameraScroll) {
                        TakeScrollStep(now, qX, qY);
                    }
                    const bool moveCamera = (qX != 0.0f || qY != 0.0f);
                    g_lastFrameStartQpc = now;
                    if (!moveCamera && !cfg.smoothCursor) {
                        continue; // nothing to show in this refresh
                    }

                    const uint16_t savedStepX = *reinterpret_cast<uint16_t*>(pEngine + 0x4782);
                    const uint16_t savedStepY = *reinterpret_cast<uint16_t*>(pEngine + 0x4784);
                    if (moveCamera) {
                        // Same state the scroll input code leaves behind: velocity for this call,
                        // "direction held" flags so RHEngine::Update does not replace it from the
                        // inertia table, camera state 2 = scrolling. Update clears all of it again.
                        *reinterpret_cast<float*>(pEngine + 0x48B8) = qX;
                        *reinterpret_cast<float*>(pEngine + 0x48BC) = qY;
                        pEngine[0x6D] = 1;
                        pEngine[0x6E] = 1;
                        pEngine[0x48C0] = 2;
                    }
                    PerRenderState perRender{};
                    SavePerRenderState(pGame, perRender);
                    g_inExtraFrame = true;
                    g_fnRenderFrame(pGame, 1, 1);
                    g_inExtraFrame = false;
                    RestorePerRenderState(pGame, perRender);
                    *reinterpret_cast<uint16_t*>(pEngine + 0x4782) = savedStepX;
                    *reinterpret_cast<uint16_t*>(pEngine + 0x4784) = savedStepY;
                    ranExtra = true;

                    const uint64_t dt = ReadRawQPC() - now;
                    const double dtUs = static_cast<double>(dt) * 1000000.0 / static_cast<double>(qpcFreq);
                    g_avgExtraUs = (g_avgExtraUs <= 0.0) ? dtUs : (g_avgExtraUs * 0.8 + dtUs * 0.2);
                    Diag::Add(Diag::C_SUBFRAME, dt);
                }
            }

            // Start the next tick: exactly on schedule, or - when the display paces us - with the
            // refresh that is closest to it.
            uint64_t nowQpc = ReadRawQPC();
            if (nowQpc < nextStartQpc) {
                const bool closeEnough = vsyncPaced && ranExtra && (nextStartQpc - nowQpc) <= periodQpc / 2 + 1;
                if (!closeEnough) {
                    WaitUntilQpc(nextStartQpc);
                    nowQpc = ReadRawQPC();
                }
            }

            g_extraActive = ranExtra;
            g_prevTickExitQpc = nowQpc;
            g_lastFrameStartQpc = nowQpc;
            // keep the ideal schedule unless we fell behind by more than a tick
            g_tickIdealStartQpc = (nowQpc > nextStartQpc + tickLen) ? nowQpc : nextStartQpc;
            AudioFix::OnGameTick();
            Diag::OnTickWorkBegin();

            if (g_lastFrameQpc.QuadPart != 0) {
                double dt = static_cast<double>(nowQpc - g_lastFrameQpc.QuadPart) / static_cast<double>(g_qpcFreq.QuadPart);
                g_fpsAccumTime += dt;
                g_fpsFrameCount++;
                if (g_fpsAccumTime >= 0.5) {
                    g_currentFPS = static_cast<double>(g_fpsFrameCount) / g_fpsAccumTime;
                    g_fpsAccumTime = 0.0;
                    g_fpsFrameCount = 0;
                }
            }
            g_lastFrameQpc.QuadPart = static_cast<LONGLONG>(nowQpc);
        }

        uintptr_t g_gameLimiterResumeAddr = 0x00510126;

        // Naked hook replacing 0x00510114..0x00510125 in RHGame::Run
        // At 0x00510114: ebp = RHGame*, esi = frameStartTick
        __declspec(naked) void GameLoopLimiterDetour() {
            __asm {
                pushfd
                pushad
                push 0x28           // targetMs = 40 ms
                push esi            // frameStartTick
                push ebp            // RHGame*
                call PaceGameLoopTick
                popad
                popfd
                jmp dword ptr [g_gameLimiterResumeAddr]
            }
        }

        void PatchEngineTimersAndCameraCurve() {
            auto& cfg = SherwoodConfig::Get();
            HMODULE hExe = GetModuleHandleA(nullptr);

            // 1. Hook RHEngine::ClampScrollDelta (0x004CEC60) and resolve its call site in
            //    RHEngine::Update (return address 0x004C86DD):
            uintptr_t addrClampCall = Memory::FindPattern(hExe, "8B CE E8 ? ? ? ? 8D 86 B8 48 00 00");
            if (addrClampCall) {
                g_addrEngineClampScrollRet = addrClampCall + 7;
            }
            uintptr_t addrClampScroll = Memory::FindPattern(hExe, "83 EC 0C 53 56 8B F1 8D 4C 24 0C E8");
            if (!addrClampScroll) {
                addrClampScroll = 0x004CEC60;
            }
            if (MH_CreateHook(reinterpret_cast<LPVOID>(addrClampScroll),
                              reinterpret_cast<LPVOID>(&Hook_ClampScrollDelta),
                              reinterpret_cast<LPVOID*>(&o_ClampScrollDelta)) == MH_OK) {
                MH_EnableHook(reinterpret_cast<LPVOID>(addrClampScroll));
                Logger::Info("Hooked RHEngine::ClampScrollDelta @ 0x%08X (EngineUpdate ret=0x%08X).",
                             static_cast<uint32_t>(addrClampScroll), static_cast<uint32_t>(g_addrEngineClampScrollRet));
            }

            // 1b. Per-frame cursor jitter (0x005F1A90) must not run in extra frames.
            static const uint8_t kJitterProlog[] = { 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x6A, 0xFF, 0x68, 0x00, 0x2E, 0x67, 0x00 };
            static const uint8_t kRenderProlog[] = { 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x6A, 0xFF, 0x68, 0x40, 0x25, 0x66, 0x00 };
            auto* jitterAddr = reinterpret_cast<uint8_t*>(0x005F1A90);
            const bool renderOk = std::memcmp(reinterpret_cast<const void*>(0x005105D0), kRenderProlog, sizeof(kRenderProlog)) == 0;
            if (renderOk && o_ClampScrollDelta && std::memcmp(jitterAddr, kJitterProlog, sizeof(kJitterProlog)) == 0 &&
                MH_CreateHook(jitterAddr, reinterpret_cast<LPVOID>(&Hook_CursorJitter),
                              reinterpret_cast<LPVOID*>(&o_CursorJitter)) == MH_OK &&
                MH_EnableHook(jitterAddr) == MH_OK) {
                g_extraFramesOk = true;
                Logger::Info("Extra frames: RHGame::RenderFrame @ 0x005105D0 verified, cursor jitter @ 0x005F1A90 guarded.");

                // per-frame animation step (0x005B7F60: mov eax,[0x6C0A88] / push esi / mov esi,ecx)
                static const uint8_t kAnimProlog[] = { 0xA1, 0x88, 0x0A, 0x6C, 0x00, 0x56, 0x8B, 0xF1 };
                auto* animAddr = reinterpret_cast<uint8_t*>(0x005B7F60);
                const bool animOk = std::memcmp(animAddr, kAnimProlog, sizeof(kAnimProlog)) == 0 &&
                    MH_CreateHook(animAddr, reinterpret_cast<LPVOID>(&Hook_AnimAdvance),
                                  reinterpret_cast<LPVOID*>(&o_AnimAdvance)) == MH_OK &&
                    MH_EnableHook(animAddr) == MH_OK;

                // afterimage counter (0x00491B5A: 66 FF 8E 80 05 00 00) -> call GhostCounterGuard ; nop ; nop
                static const uint8_t kGhostDec[] = { 0x66, 0xFF, 0x8E, 0x80, 0x05, 0x00, 0x00 };
                constexpr uintptr_t kGhostAddr = 0x00491B5A;
                bool ghostOk = false;
                if (std::memcmp(reinterpret_cast<const void*>(kGhostAddr), kGhostDec, sizeof(kGhostDec)) == 0) {
                    uint8_t patch[7];
                    patch[0] = 0xE8;
                    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&GhostCounterGuard) - (kGhostAddr + 5));
                    std::memcpy(&patch[1], &rel, 4);
                    patch[5] = 0x90;
                    patch[6] = 0x90;
                    ghostOk = Memory::PatchBytes(kGhostAddr, patch, sizeof(patch));
                }
                // HUD blink timers (0x0055EE00: push esi / push edi / mov edi,ecx / mov esi,[edi+4] / mov eax,[edi+8])
                static const uint8_t kHudProlog[] = { 0x56, 0x57, 0x8B, 0xF9, 0x8B, 0x77, 0x04, 0x8B, 0x47, 0x08 };
                auto* hudAddr = reinterpret_cast<uint8_t*>(0x0055EE00);
                const bool hudOk = std::memcmp(hudAddr, kHudProlog, sizeof(kHudProlog)) == 0 &&
                    MH_CreateHook(hudAddr, reinterpret_cast<LPVOID>(&Hook_HudTimers),
                                  reinterpret_cast<LPVOID*>(&o_HudTimers)) == MH_OK &&
                    MH_EnableHook(hudAddr) == MH_OK;

                // set-cursor (0x005F14C0: mov eax,fs:[0] / push -1 / push 0x672D28 / push eax)
                static const uint8_t kSetCursorProlog[] = { 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x6A, 0xFF, 0x68, 0x28, 0x2D, 0x67, 0x00, 0x50 };
                auto* cursorAddr = reinterpret_cast<uint8_t*>(0x005F14C0);
                const bool cursorOk = std::memcmp(cursorAddr, kSetCursorProlog, sizeof(kSetCursorProlog)) == 0 &&
                    MH_CreateHook(cursorAddr, reinterpret_cast<LPVOID>(&Hook_SetCursor),
                                  reinterpret_cast<LPVOID*>(&o_SetCursor)) == MH_OK &&
                    MH_EnableHook(cursorAddr) == MH_OK;

                Logger::Info("Extra frames: per-frame animation step guarded=%d, afterimage counter guarded=%d, HUD blink timers guarded=%d, "
                             "cursor shape kept between ticks=%d.", animOk ? 1 : 0, ghostOk ? 1 : 0, hudOk ? 1 : 0, cursorOk ? 1 : 0);

                // object smoothing wraps every RenderFrame call
                if (MotionSmooth::Initialize()) {
                    auto* renderAddr = reinterpret_cast<uint8_t*>(0x005105D0);
                    if (MH_CreateHook(renderAddr, reinterpret_cast<LPVOID>(&Hook_RenderFrame),
                                      reinterpret_cast<LPVOID*>(&o_RenderFrame)) != MH_OK ||
                        MH_EnableHook(renderAddr) != MH_OK) {
                        Logger::Warn("MotionSmooth: could not hook RenderFrame - object smoothing inactive.");
                    }
                }
            } else {
                // without both verified we never call into the game between ticks
                Logger::Warn("Extra frames disabled: RenderFrame/cursor-jitter prologues do not match this build.");
            }

            // 2. Hook the main game loop busy-wait limiter at 0x00510114:
            // Pattern: FF D7 2B C6 83 F8 28 73 09 FF D7 2B C6 83 F8 28 72 F7
            uintptr_t gameLoopLim = Memory::FindPattern(hExe, "FF D7 2B C6 83 F8 28 73 09 FF D7 2B C6 83 F8 28 72 F7");
            if (gameLoopLim) {
                g_gameLimiterResumeAddr = gameLoopLim + 18;
                uint8_t patch[18];
                std::memset(patch, 0x90, sizeof(patch));
                patch[0] = 0xE9; // jmp rel32
                int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&GameLoopLimiterDetour) - (gameLoopLim + 5));
                std::memcpy(&patch[1], &rel, sizeof(rel));
                if (Memory::PatchBytes(gameLoopLim, patch, sizeof(patch))) {
                    Logger::Info("Installed game loop pacer @ 0x%08X (extra frames: scroll=%d cursor=%d, VSync=%d, display %d Hz).",
                                 gameLoopLim, cfg.smoothCameraScroll ? 1 : 0, cfg.smoothCursor ? 1 : 0,
                                 cfg.vsync ? 1 : 0, g_displayHz);
                }
            } else {
                Logger::Warn("Main game loop limiter pattern (0x510114) not found.");
            }

            // 3. Unlock Menu / UI frame rate limiter at 0x0052FCF7 (default 0x14 = 20ms = 50 FPS):
            // Pattern: FF 15 ? ? ? ? 8B 7C 24 68 2B C7 83 F8 14 73 14
            uintptr_t menuLim = Memory::FindPattern(hExe, "FF 15 ? ? ? ? 8B 7C 24 68 2B C7 83 F8 14 73 14");
            if (menuLim) {
                int maxFPS = (cfg.maxFPS > 0) ? cfg.maxFPS : 120;
                uint8_t targetMenuMs = static_cast<uint8_t>(std::clamp(1000 / maxFPS, 1, 20));
                // Patch both `cmp eax, 0x14` instructions at +14 and +27
                Memory::Write<uint8_t>(menuLim + 14, targetMenuMs);
                Memory::Write<uint8_t>(menuLim + 27, targetMenuMs);
                Logger::Info("Unlocked Menu/UI loop frame limiter @ 0x%08X: 20ms (50 FPS) -> %ums (%d FPS).",
                             menuLim, targetMenuMs, 1000 / targetMenuMs);
            }

            // 4. Camera scroll speed table in the RHEngine constructor (0x004BEFA7) rounds every
            // speed to an even number of pixels; let odd values through so the per-frame steps
            // are finer (`je` -> `jmp`).
            // Pattern: 25 01 00 00 80 79 05 48 83 C8 FE 40 74 0E D9 44 24 14
            if (cfg.smoothCameraScroll) {
                uintptr_t curveAddr = Memory::FindPattern(hExe, "25 01 00 00 80 79 05 48 83 C8 FE 40 74 0E D9 44 24 14");
                if (curveAddr) {
                    uint8_t jmpShort = 0xEB; // JMP rel8 instead of JE rel8
                    if (Memory::Write(curveAddr + 12, jmpShort)) {
                        Logger::Info("Patched RHEngine camera scroll velocity table @ 0x%08X (removed even-integer quantization).",
                                     curveAddr + 12);
                    }
                }
            }
        }
    }

    bool Init() {
        QueryPerformanceFrequency(&g_qpcFreq);
        QueryPerformanceCounter(&g_qpcStart);
        g_baseTickCount = GetTickCount();

        DEVMODEA dm{};
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency >= 30) {
            g_displayHz = static_cast<int>(dm.dmDisplayFrequency);
        }

        // 1. Enforce 1ms kernel timer resolution via real winmm.dll
        HMODULE hRealWinMM = GetRealWinMMModule();
        if (hRealWinMM) {
            o_timeBeginPeriod = reinterpret_cast<PFN_timeBeginPeriod>(GetProcAddress(hRealWinMM, "timeBeginPeriod"));
            o_timeEndPeriod   = reinterpret_cast<PFN_timeEndPeriod>(GetProcAddress(hRealWinMM, "timeEndPeriod"));
            o_timeGetTime     = reinterpret_cast<PFN_timeGetTime>(GetProcAddress(hRealWinMM, "timeGetTime"));

            if (o_timeBeginPeriod && SherwoodConfig::Get().highResolutionTimer) {
                o_timeBeginPeriod(1);
                Logger::Info("Timing: Windows kernel timer resolution set to 1 ms via timeBeginPeriod(1).");
            }
        }

        // 2. Hook GetTickCount, timeGetTime, timeEndPeriod, and Sleep
        HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
        if (hKernel32) {
            FARPROC pGTC = GetProcAddress(hKernel32, "GetTickCount");
            if (pGTC && MH_CreateHook(reinterpret_cast<LPVOID>(pGTC),
                                      reinterpret_cast<LPVOID>(&Hook_GetTickCount),
                                      reinterpret_cast<LPVOID*>(&o_GetTickCount)) == MH_OK) {
                MH_EnableHook(reinterpret_cast<LPVOID>(pGTC));
            }

            FARPROC pSleep = GetProcAddress(hKernel32, "Sleep");
            if (pSleep && MH_CreateHook(reinterpret_cast<LPVOID>(pSleep),
                                        reinterpret_cast<LPVOID>(&Hook_Sleep),
                                        reinterpret_cast<LPVOID*>(&o_Sleep)) == MH_OK) {
                MH_EnableHook(reinterpret_cast<LPVOID>(pSleep));
            }

            o_QPC = reinterpret_cast<PFN_QPC>(GetProcAddress(hKernel32, "QueryPerformanceCounter"));
        }

        if (hRealWinMM) {
            if (o_timeGetTime && MH_CreateHook(reinterpret_cast<LPVOID>(o_timeGetTime),
                                               reinterpret_cast<LPVOID>(&Hook_timeGetTime),
                                               reinterpret_cast<LPVOID*>(&o_timeGetTime)) == MH_OK) {
                MH_EnableHook(reinterpret_cast<LPVOID>(o_timeGetTime));
            }
            if (o_timeEndPeriod && MH_CreateHook(reinterpret_cast<LPVOID>(o_timeEndPeriod),
                                                 reinterpret_cast<LPVOID>(&Hook_timeEndPeriod),
                                                 reinterpret_cast<LPVOID*>(&o_timeEndPeriod)) == MH_OK) {
                MH_EnableHook(reinterpret_cast<LPVOID>(o_timeEndPeriod));
            }
        }

        // 3. Patch engine loops & camera curve
        PatchEngineTimersAndCameraCurve();

        Logger::Info("Timing: High-precision QPC timer hooks and game loop pacing initialized.");
        return true;
    }

    void Shutdown() {
        if (o_timeEndPeriod && SherwoodConfig::Get().highResolutionTimer) {
            o_timeEndPeriod(1);
        }
    }

    bool InExtraFrame() {
        return g_inExtraFrame;
    }

    float RenderPhase() {
        return g_renderPhase;
    }

    double GetElapsedMilliseconds() {
        uint64_t ticks = ReadRawQPC() - static_cast<uint64_t>(g_qpcStart.QuadPart);
        return (static_cast<double>(ticks) * 1000.0) / static_cast<double>(g_qpcFreq.QuadPart);
    }

    uint64_t GetElapsedMicroseconds() {
        uint64_t ticks = ReadRawQPC() - static_cast<uint64_t>(g_qpcStart.QuadPart);
        return (ticks * 1000000ULL) / static_cast<uint64_t>(g_qpcFreq.QuadPart);
    }

    double GetCurrentFPS() {
        return g_currentFPS;
    }

    void FrameLimitStep(double targetFPS) {
        if (targetFPS <= 0.0) return;

        const uint64_t frameDurationUs = static_cast<uint64_t>(1000000.0 / targetFPS);
        uint64_t nowTicks = ReadRawQPC();

        if (g_lastFrameQpc.QuadPart != 0) {
            uint64_t elapsedUs = ((nowTicks - static_cast<uint64_t>(g_lastFrameQpc.QuadPart)) * 1000000ULL) /
                                 static_cast<uint64_t>(g_qpcFreq.QuadPart);
            if (elapsedUs < frameDurationUs) {
                HybridWaitMicroseconds(static_cast<int64_t>(frameDurationUs - elapsedUs));
            }
        }
        g_lastFrameQpc.QuadPart = static_cast<LONGLONG>(ReadRawQPC());
    }
}
