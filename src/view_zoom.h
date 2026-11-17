#pragma once
#include <cstdint>

// Smooth zoom ("lupa", [Display] SmoothZoom=1, 2026-11-14 night, user wish).
//
// The engine draws characters at three zoom levels only (0.5 / 1 / 2: separate code branches,
// docs\OBJECT_RENDER.md), and its zoom-out to 1.0 left a black band on levels narrower than the
// screen. Here the player's zoom is any value between "the whole level fits the screen" and
// ZoomMax: the engine renders at 1.0 (below 2.0) or 2.0 (HD pictures, from 2.0 up) and the
// presenter magnifies the WORLD by the rest - the world is copied right before the mission's
// windows are drawn; a frame pixel that differs from that copy belongs to the interface and is
// shown at its place, everything else comes from the magnified world. The engine's level is
// switched without its zoom animation (zoom, index, camera about the same view centre, world
// surface invalid = full redraw). Mouse positions over the world are mapped through the
// magnification (ScreenToWorld 0x004DEE70); the shown part of the frame moves towards the
// level's edge when the camera cannot (scrolling beyond the camera's limit, centring on a point
// near the edge), so every part of the level can be seen.
namespace ViewZoom {
    void Install();                     // DllMain, after MinHook is initialised
    bool Installed();                   // hooks in place
    bool HandlesZoom();                 // installed and switched on: the zoom keys and buttons are ours

    // settings (panel F10 / INI)
    bool Enabled();
    void SetEnabled(bool on);
    int  StepPercent();                 // one zoom step: zoom x (this / 100)
    void SetStepPercent(int percent);
    int  MaxPercent();                  // largest zoom (200 = the engine's 2.0)
    void SetMaxPercent(int percent);

    // main thread, once per game tick (before the game's update): level start, engine level
    void OnGameTick(uint8_t* engine);
    // UiScale, main thread: the mission's world is drawn, its windows are not yet
    void OnWorldDrawn();
    // UiScale, main thread, at the same moment (every mission frame, also a dialog's backdrop): the
    // frame beside / below a level smaller than the view black. At the engine's zoom 1.0 on a level
    // smaller than the screen (the camp of Sherwood: 1920x1088) the engine draws only the level and
    // leaves the rest as it was - pieces of earlier frames at zoom 2.0 (the stream, magnified) stayed
    // there and showed in dialog backdrops (YouTuber's report 2026-11-17) and without the smooth zoom.
    void ClearOutsideLevel();

    // presenter, main thread: the magnification for the frame being presented (once per frame)
    struct View {
        float m = 1.0f;                 // magnification of the world (> 1)
        float cropX = 0.0f, cropY = 0.0f;   // frame position shown at the output's top-left corner
        float worldH = 0.0f;            // frame rows that hold the world (below them: HUD strip)
        const uint16_t* snap = nullptr; // the world before the windows were drawn, RGB565, w x h
        int   w = 0, h = 0;
    };
    bool TakeView(View& out);

    // the frame position whose world a screen position shows (false: no magnification)
    bool ScreenToFrame(float& x, float& y);
    // the last frame with the world was magnified (main thread)
    bool Magnifying();
    // the last frame with the world, if it was magnified: its world copy and the magnification
    // (ddm_hd_menu.cpp: the backdrop of a dialog shows the world the same way)
    bool LastWorld(View& out);
    // UiScale::Hook_DrawRect: the minimap's camera rectangle -> the part the player sees
    bool MapViewRect(float rect[4]);

    // timing.cpp: the engine clamped a scroll step - world pixels the camera could not move
    void OnScrollClamped(float dx, float dy);
    // widescreen_patch.cpp: the hook of CanZoomOut 0x004C7CF0 asks this while HandlesZoom()
    bool CanZoomOut();

    // input thread: mouse wheel notches (+ = closer)
    void OnWheel(int notches);
    // input thread: the wheel turned (the game's zoom actions that follow zoom about the cursor)
    void NoteWheel();

    // test harness
    int  Describe(char* out, int size);
    void TestSetZoom(float z);          // jump straight to an effective zoom
    void TestGameZoom(int dir);         // the engine's own zoom animation past the hooks (as a script)
}
