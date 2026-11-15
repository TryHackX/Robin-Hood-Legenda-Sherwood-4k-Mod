#include "config.h"
#include <windows.h>
#include <algorithm>
#include <cstdio>

namespace {
    void WriteDefaultIniIfMissing(const std::string& path) {
        DWORD attr = GetFileAttributesA(path.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES) return;

        FILE* f = nullptr;
        fopen_s(&f, path.c_str(), "w");
        if (!f) return;

        fprintf(f,
            "; ============================================================\n"
            "; SherwoodEngineFix - Configuration File\n"
            "; Robin Hood: The Legend of Sherwood (2002) Windows 11 Patch\n"
            "; ============================================================\n\n"
            "[Display]\n"
            "; Custom resolution replacing the in-game 1024x768 mode\n"
            "Width=1920\n"
            "Height=1080\n"
            "Replace1024x768=1\n"
            "WindowedBorderless=1\n"
            "VSync=0\n"
            "MaxFPS=120\n"
            "ShaderFilter=1\n"
            "FixAspectRatio=1\n\n"
            "[Performance]\n"
            "HighResolutionTimer=1\n"
            "SmoothCameraScroll=1\n"
            "OptimizeFogOfWar=1\n"
            "ThreadOptimization=1\n"
            "UseSSE2Blit=1\n"
            "ForceSystemMemorySurfaces=1\n\n"
            "[Input]\n"
            "RawMouseInput=0\n"
            "LockCursorToWindow=1\n"
            "EdgeScrollSpeed=100\n"
            "; Max mouse movement reports per second handed to the game (0 = unlimited)\n"
            "MouseReportRateHz=125\n\n"
            "[Debug]\n"
            "EnableLogging=1\n"
            "ShowFPSOverlay=0\n"
            "PerfLog=1\n"
            "Profiler=0\n"
        );
        fclose(f);
    }

    bool ReadBool(const char* section, const char* key, bool defVal, const char* path) {
        return GetPrivateProfileIntA(section, key, defVal ? 1 : 0, path) != 0;
    }

    int ReadInt(const char* section, const char* key, int defVal, const char* path) {
        return static_cast<int>(GetPrivateProfileIntA(section, key, defVal, path));
    }
}

SherwoodConfig& SherwoodConfig::Get() {
    static SherwoodConfig instance;
    return instance;
}

void SherwoodConfig::Load(const std::string& iniPath) {
    WriteDefaultIniIfMissing(iniPath);
    this->iniPath = iniPath;
    const char* p = iniPath.c_str();

    // [Display]
    width              = ReadInt("Display", "Width", width, p);
    height             = ReadInt("Display", "Height", height, p);
    if (width <= 0 || height <= 0) {
        width  = GetSystemMetrics(SM_CXSCREEN);
        height = GetSystemMetrics(SM_CYSCREEN);
        if (width <= 0 || height <= 0) {
            width  = 1920;
            height = 1080;
        }
    }
    replace1024x768    = ReadBool("Display", "Replace1024x768", replace1024x768, p);
    windowedBorderless = ReadBool("Display", "WindowedBorderless", windowedBorderless, p);
    vsync              = ReadBool("Display", "VSync", vsync, p);
    maxFPS             = ReadInt("Display", "MaxFPS", maxFPS, p);
    shaderFilter       = ReadInt("Display", "ShaderFilter", shaderFilter, p);
    fixAspectRatio     = ReadBool("Display", "FixAspectRatio", fixAspectRatio, p);
    renderer           = ReadInt("Display", "Renderer", renderer, p);
    presenter          = ReadInt("Display", "Presenter", presenter, p);
    allowTearing       = ReadBool("Display", "AllowTearing", allowTearing, p);
    fullHeightView     = ReadBool("Display", "FullHeightView", fullHeightView, p);
    hdBackgrounds      = ReadBool("Display", "HDBackgrounds", hdBackgrounds, p);
    {
        char name[64]{};
        GetPrivateProfileStringA("Display", "HDVariant", "", name, sizeof(name), p);
        hdVariant = name;
    }
    hdKeyToggle        = ReadInt("Display", "HDKeyToggle", hdKeyToggle, p);
    hdKeyNext          = ReadInt("Display", "HDKeyNext", hdKeyNext, p);
    hdKeyPrev          = ReadInt("Display", "HDKeyPrev", hdKeyPrev, p);
    hdView             = ReadBool("Display", "HDView", hdView, p);
    smoothSprites      = ReadBool("Display", "SmoothSprites", smoothSprites, p);
    hdWater            = ReadInt("Display", "HDWater", hdWater, p);
    hdSprites          = ReadBool("Display", "HDSprites", hdSprites, p);
    hdInBetween        = ReadBool("Display", "HDInBetween", hdInBetween, p);
    hdInBetweenQuality = (std::max)(0, (std::min)(255, ReadInt("Display", "HDInBetweenQuality", hdInBetweenQuality, p)));
    hdKeyBetween       = ReadInt("Display", "HDKeyBetween", hdKeyBetween, p);
    uiScale            = ReadInt("Display", "UIScale", uiScale, p);
    mapPosition        = ReadInt("Display", "MapPosition", mapPosition, p);
    mapDragCamera      = ReadBool("Display", "MapDragCamera", mapDragCamera, p);
    smoothZoom         = ReadBool("Display", "SmoothZoom", smoothZoom, p);
    zoomStep           = ReadInt("Display", "ZoomStep", zoomStep, p);
    zoomMax            = ReadInt("Display", "ZoomMax", zoomMax, p);
    menuZoom           = ReadBool("Display", "MenuZoom", menuZoom, p);
    settingsKey        = ReadInt("Display", "SettingsKey", settingsKey, p);
    hdMenu             = ReadBool("Display", "HDMenu", hdMenu, p);

    // [Performance]
    highResolutionTimer       = ReadBool("Performance", "HighResolutionTimer", highResolutionTimer, p);
    smoothCameraScroll        = ReadBool("Performance", "SmoothCameraScroll", smoothCameraScroll, p);
    smoothCursor              = ReadBool("Performance", "SmoothCursor", smoothCursor, p);
    smoothMotion              = ReadBool("Performance", "SmoothMotion", smoothMotion, p);
    framesPerTick             = ReadInt("Performance", "FramesPerTick", framesPerTick, p);
    optimizeFogOfWar          = ReadBool("Performance", "OptimizeFogOfWar", optimizeFogOfWar, p);
    threadOptimization        = ReadBool("Performance", "ThreadOptimization", threadOptimization, p);
    useSSE2Blit               = ReadBool("Performance", "UseSSE2Blit", useSSE2Blit, p);
    forceSystemMemorySurfaces = ReadBool("Performance", "ForceSystemMemorySurfaces", forceSystemMemorySurfaces, p);

    // [Input]
    rawMouseInput      = ReadBool("Input", "RawMouseInput", rawMouseInput, p);
    lockCursorToWindow = ReadBool("Input", "LockCursorToWindow", lockCursorToWindow, p);
    edgeScrollSpeed    = ReadInt("Input", "EdgeScrollSpeed", edgeScrollSpeed, p);
    mouseReportRateHz  = ReadInt("Input", "MouseReportRateHz", mouseReportRateHz, p);
    mouseSensitivity   = ReadInt("Input", "MouseSensitivity", mouseSensitivity, p);

    // [Audio]
    audioBufferMs = ReadInt("Audio", "BufferMs", audioBufferMs, p);
    hearingRangePercent = ReadInt("Audio", "HearingRangePercent", hearingRangePercent, p);

    // [Debug]
    enableLogging  = ReadBool("Debug", "EnableLogging", enableLogging, p);
    showFPSOverlay = ReadBool("Debug", "ShowFPSOverlay", showFPSOverlay, p);
    perfLog        = ReadBool("Debug", "PerfLog", perfLog, p);
    profiler       = ReadBool("Debug", "Profiler", profiler, p);
    hangWatchdog   = ReadBool("Debug", "HangWatchdog", hangWatchdog, p);}
