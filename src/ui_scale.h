#pragma once
#include <windows.h>

// Interface scale ([Display] UIScale=2, internal width >= 2048).
//
// At 2560x1440 the game composes its picture natively: the world is drawn at the 2.0 zoom (with
// the HD pictures), but the game's windows - HUD corners, portraits, buttons, the map, dialogs,
// tooltips, on-screen messages - keep their size in screen pixels and become small.
// With this option every picture of the mission's windows is drawn twice as large, away from the
// screen corner it belongs to (a window in the middle - the map, a dialog - about its own centre),
// and the mouse position the windows see is mapped back the same way; the world keeps the full
// resolution and the real mouse position.
namespace UiScale {
    void Install();      // DllMain, after MinHook is initialised

    int  Factor();       // 2 when active, 1 otherwise
    bool Drawing();      // the game's windows are being drawn right now (main thread)

    // While Drawing(): a rectangle in window coordinates -> screen pixels.
    void ScaleRect(RECT& r);
    void ScaleRectF(float* ltrb);

    // The game is drawing its mouse cursor (enlarged about its hot spot); the serial changes with
    // every cursor drawing.
    bool     CursorDrawing();
    unsigned CursorSerial();

    // Menu zoom ([Display] MenuZoom=1, Direct3D 11 presenter, internal height >= 960). The main
    // menu and the game's dialogs (briefing, in-game menu, load/save, options) are laid out for a
    // 1024x768 screen in the middle of the frame and look tiny at 2560x1440. While the game's
    // dialog loop runs, the presenter shows the middle part of the frame, 768 lines high, on the
    // whole window; the game's cursor is kept inside that part and its speed on the screen stays
    // the same.
    bool  MenuZoomCrop(int frameW, int frameH, RECT& crop);  // presenter, main thread: the part to show
    bool  MenuCropRect(int frameW, int frameH, RECT& crop);  // that part's geometry (whether zoomed now or not)
    float MenuZoomMouseScale();                              // input thread: 1, or less while zoomed
    void  MenuZoomUnavailable();                             // presenter: no Direct3D 11 - feature off

    // Timing: a dialog's backdrop render (RHGame::RenderFrame(0, 0) from 0x0053B6B0) begins / ended.
    // A window opens such a dialog from its own Draw, so the render runs inside the mission's
    // window drawing: that state is set aside meanwhile and comes back after (main thread).
    void BeginBackdropRender();
    void EndBackdropRender();

    // A dialog of the game is up (its dialog loop drew last) - main thread. Needs the
    // WindowManager::Draw hook (interface scale or menu zoom possible).
    bool DialogShown();
    bool DialogLoopRunning();     // main thread: the game's dialog loop runs (from its first instruction)

    // Input thread: the screen point lies on a rolled-up end of the opened (enlarged) map - a
    // left click there closes the map like a right click.
    bool OnMapScrollEnd(float x, float y);

    // The opened map ([Display] MapDragCamera=1): dragging on its picture of the level moves the
    // camera; the window is dragged by its edges with either button and follows the cursor in
    // every frame. Input thread: the screen point lies on the map's edges (outside the picture) -
    // a right press there is given to the game as a left one (NoteEdgePress while it lasts).
    bool OnMapEdge(float x, float y);
    void NoteEdgePress(bool on);
    bool MapDragAvailable();             // the map window's code was recognised
    bool MapDragCamera();
    void SetMapDragCamera(bool on);      // settings panel, main thread

    // Settings panel: switches at run time (main thread, between frames).
    bool InterfaceScaleAvailable();     // hooks in place (internal width >= 2048)
    bool InterfaceScaled();
    void SetInterfaceScale(bool on);
    bool MapInCorner();
    void SetMapInCorner(bool on);
    bool MenuZoomAvailable();           // Direct3D 11, internal height >= 960
    bool MenuZoomOn();
    void SetMenuZoom(bool on);

    // test harness: the mission's windows (object, vtable, rectangle, what each drew last frame)
    int DescribeWindows(char* out, int size);
}
