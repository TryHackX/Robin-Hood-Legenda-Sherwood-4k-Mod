#pragma once
#include <string>

struct SherwoodConfig {
    // [Display]
    int width = 2560;     // the release defaults (release\SherwoodFix.ini): HD mode
    int height = 1440;
    bool replace1024x768 = true;
    bool windowedBorderless = true;
    bool vsync = true;
    int maxFPS = 120;
    int shaderFilter = 1; // 0 = nearest, 1 = sharp bilinear, 2 = bilinear
    bool fixAspectRatio = true;
    int renderer = 1;     // 1 = own in-memory DirectDraw + Direct3D 11, 0 = GOG wrapper (ddraw.dll)
    int presenter = 0;    // 0 = Direct3D 11 (GDI if it fails), 1 = force GDI
    bool allowTearing = false; // with VSync=0: present immediately even if that tears
    bool fullHeightView = true;  // draw the world in the bottom 80 rows too (HUD corners on top of it)
    bool hdBackgrounds = true;   // at zoom 2.0 take the background from hd\maps\<variant>\*.hdmap when present
    std::string hdVariant;       // folder under hd\maps shown at start ("" = first one, "off" = original pixels)
    int hdKeyToggle = 0x77;      // virtual key: original <-> HD background (F8); 0 = none
    int hdKeyNext = 0x76;        // next HD variant (F7)
    int hdKeyPrev = 0x75;        // previous HD variant (F6)
    bool hdView = true;          // internal width >= 2048: start levels at the 2.0 zoom (framing of 1280x720)
    bool smoothSprites = true;   // 2.0 zoom: magnify sprites with filtering instead of 2x2 blocks
    int hdWater = 2;             // on HD backgrounds: 2 = the HD water moved by waves, 3 = stronger waves, 1 = still water
                                 // from the HD picture, 0 = the game's low-res ground animations as they are
                                 // (chimney smoke: 1..3 = over the HD picture)
    bool hdSprites = true;       // 2.0 zoom: draw upscaled sprite frames from hd\sprites\<variant>\ where they exist
    bool hdInBetween = true;     // generated in-between frames of sprite animations (hd\sprites\_between)
    int  hdInBetweenQuality = 17; // ... only those whose two moved source frames differ by at most this (of 255; 32 = all)
    int uiScale = 2;             // internal width >= 2048: 2 = the game's windows (HUD, map, dialogs) drawn x2 from their screen corners
    int mapPosition = 1;         // with uiScale: where the opened map lies - 0 where the game puts it (top, middle), 1 top-right corner
    bool mapDragCamera = true;   // dragging on the opened map's picture moves the camera (the window is moved by its edges)
    bool smoothZoom = true;      // HD view: any zoom from "the level fills the screen" to zoomMax (view_zoom.h)
    int zoomStep = 115;          // one zoom step, percent
    int zoomMax = 400;           // largest zoom, percent (200 = the engine's 2.0)
    bool menuZoom = true;        // Direct3D 11, internal height >= 960: menus and dialogs (laid out for 1024x768) shown on the whole window
    int settingsKey = 0x79;      // virtual key of the mod's settings panel (F10); 0 = no panel
    bool hdMenu = true;          // with menuZoom: upscaled menu pictures (hd\menu\<variant>\) where they exist
    bool hdMenuRemake = true;    // ... and remade ones (hd\menu\remake\, tools\hd\import_menu_remake.py) before those
    int hdKeyBetween = 0x78;     // virtual key that switches them on and off (F9); 0 = none

    // [Performance]
    bool highResolutionTimer = true;
    bool smoothCameraScroll = true;
    bool smoothCursor = true;  // redraw the frame between ticks so the software cursor moves smoothly
    bool smoothMotion = true;  // interpolate object positions between ticks (needs extra frames)
    int framesPerTick = 0;     // frames per 40 ms game tick; 0 = display refresh rate / 25
    bool optimizeFogOfWar = true;
    bool threadOptimization = false; // pin threads to fixed cores (measured to cause erratic slowdowns; off)
    bool useSSE2Blit = true;
    bool forceSystemMemorySurfaces = true;

    // [Input]
    bool rawMouseInput = true;
    bool lockCursorToWindow = true;
    int edgeScrollSpeed = 0; // map scroll speed in percent; 0 = scale with the internal width (width / 1024)
    int mouseReportRateHz = 125; // max mouse movement reports/s handed to the game (0 = unlimited/original)
    int mouseSensitivity = 0;    // cursor speed in percent; 0 = internal width / window width (same speed on screen)

    // [Audio]
    int audioBufferMs = 0; // minimum FMOD mixing buffer in ms (0 = leave the game's own setting)
    int hearingRangePercent = 0; // range of positional sounds in percent; 0 = scale with the internal width

    // [Debug]
    bool enableLogging = true;
    bool showFPSOverlay = false;
    bool perfLog = true;   // Perf(5s)/Spike lines in SherwoodFix.log
    bool profiler = false; // 1 kHz sampling profiler of the main thread (Prof.* lines)
    bool hangWatchdog = true; // log the main thread's stack when the game hangs; end a process that will not exit

    std::string iniPath;      // the file these settings came from (a few of them are written back)
    static SherwoodConfig& Get();
    void Load(const std::string& iniPath);
};
