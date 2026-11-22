#pragma once
#include <windows.h>
#include <cstdint>

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
    // DDM, right after ScaleRect: the picture that rectangle shows (surface, its serial, the source rectangle,
    // the colour key if keyed). The pointer over the key there is not over the picture - unless the point it maps
    // to lies on one of the game's windows, it is the world's (the grass below the gate on the carriage map lay
    // under the see-through top of the enlarged hedge, 2026-11-22).
    void NoteSource(const void* surface, unsigned serial, const RECT& s, bool keyed, uint16_t key, bool mirrorX, bool mirrorY);
    // test harness: what the pointer at (x, y) is mapped to and why
    int DescribePointer(float x, float y, char* out, int size);
    // ... in short: 0 the world gets the real position, 1 a window of the game takes the pointer, 2 a point off the
    // windows the world gets (the dragged map, the moved map's old place), 4 the interface takes it (its visible
    // pictures and the hedge strip outside the game's windows); seeThrough = pictures skipped there as see-through
    int ClassifyPointer(float x, float y, int* seeThrough);
    // ... the last of them belonged to the opened map: drawn at the map's own size ([Display] MapScale,
    // 150..500 %, a tall map no larger than fits above the hedge), its pictures through DDM::ScaledPicture16
    bool LastScaledWasMap();
    void SetMapScale(int percent);      // settings panel, main thread

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
bool EngineAlive();           // the mission engine object exists (it also stays set in the main menu after a mission)

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
