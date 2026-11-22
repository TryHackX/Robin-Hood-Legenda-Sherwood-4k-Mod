#include "config.h"
#include "logger.h"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
    void WriteDefaultIniIfMissing(const std::string& path) {
        DWORD attr = GetFileAttributesA(path.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES) return;

        // The release's own SherwoodFix.ini, built into the DLL (winmm.rc, resource 101): a player who unpacked
        // only winmm.dll gets the same settings as from the archive - before 2026-11-16 a short file of its own
        // with Width=1920 was written here, which left the HD mode off.
        HMODULE self = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(&WriteDefaultIniIfMissing), &self)) {
            if (HRSRC res = FindResourceA(self, MAKEINTRESOURCEA(101), MAKEINTRESOURCEA(10) /* RT_RCDATA */)) {
                const DWORD size = SizeofResource(self, res);
                HGLOBAL h = LoadResource(self, res);
                const void* data = h ? LockResource(h) : nullptr;
                if (data && size > 0) {
                    FILE* rf = nullptr;
                    if (fopen_s(&rf, path.c_str(), "wb") == 0 && rf) {
                        std::fwrite(data, 1, size, rf);
                        std::fclose(rf);
                        return;
                    }
                }
            }
        }

        // The embedded INI could not be loaded or written: nothing is written, the built-in defaults apply.
        Logger::Warn("Config: no default INI could be written (%s).", path.c_str());
    }

    bool ReadBool(const char* section, const char* key, bool defVal, const char* path) {
        return GetPrivateProfileIntA(section, key, defVal ? 1 : 0, path) != 0;
    }

    int ReadInt(const char* section, const char* key, int defVal, const char* path) {
        return static_cast<int>(GetPrivateProfileIntA(section, key, defVal, path));
    }

    // a key for QuickSaveKey / QuickLoadKey: "" or "game" = -1, "none" / "brak" / "0" = 0, a name (F5, Insert,
    // A, 7, Num0 ...) or a scan code (0x3F) = that DirectInput code
    int ReadKey(const char* section, const char* key, const char* path) {
        char text[64]{};
        GetPrivateProfileStringA(section, key, "", text, sizeof(text), path);
        char* s = text;
        while (*s == ' ' || *s == '\t') ++s;
        for (char* e = s + std::strlen(s); e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == ';'); --e) e[-1] = 0;
        if (!*s || !_stricmp(s, "game") || !_stricmp(s, "gra")) return -1;
        if (!_stricmp(s, "none") || !_stricmp(s, "brak") || !std::strcmp(s, "0")) return 0;
        const int code = SherwoodConfig::KeyFromName(s);
        return code > 0 ? code : -1;
    }
}

int SherwoodConfig::KeyFromName(const char* s) {
    if (!s || !*s) return 0;
    if ((s[0] == '0') && (s[1] == 'x' || s[1] == 'X')) {
        const long v = std::strtol(s + 2, nullptr, 16);
        return (v > 0 && v < 0x100) ? static_cast<int>(v) : 0;
    }
    for (int i = 1; i < 0x100; ++i) {
        const char* name = KeyName(i);
        if (name && !_stricmp(name, s)) return i;
    }
    return 0;
}

const char* SherwoodConfig::KeyName(int code) {
    // DirectInput scan codes (dinput.h) of the keys a quick save may sit on
    static const struct { int code; const char* name; } kKeys[] = {
        { 0x3B, "F1" }, { 0x3C, "F2" }, { 0x3D, "F3" }, { 0x3E, "F4" }, { 0x3F, "F5" }, { 0x40, "F6" },
        { 0x41, "F7" }, { 0x42, "F8" }, { 0x43, "F9" }, { 0x44, "F10" }, { 0x57, "F11" }, { 0x58, "F12" },
        { 0x02, "1" }, { 0x03, "2" }, { 0x04, "3" }, { 0x05, "4" }, { 0x06, "5" }, { 0x07, "6" }, { 0x08, "7" },
        { 0x09, "8" }, { 0x0A, "9" }, { 0x0B, "0" },
        { 0x10, "Q" }, { 0x11, "W" }, { 0x12, "E" }, { 0x13, "R" }, { 0x14, "T" }, { 0x15, "Y" }, { 0x16, "U" },
        { 0x17, "I" }, { 0x18, "O" }, { 0x19, "P" }, { 0x1E, "A" }, { 0x1F, "S" }, { 0x20, "D" }, { 0x21, "F" },
        { 0x22, "G" }, { 0x23, "H" }, { 0x24, "J" }, { 0x25, "K" }, { 0x26, "L" }, { 0x2C, "Z" }, { 0x2D, "X" },
        { 0x2E, "C" }, { 0x2F, "V" }, { 0x30, "B" }, { 0x31, "N" }, { 0x32, "M" },
        { 0x0F, "Tab" }, { 0x0E, "Backspace" }, { 0x1C, "Enter" }, { 0x39, "Space" },
        { 0xD2, "Insert" }, { 0xD3, "Delete" }, { 0xC7, "Home" }, { 0xCF, "End" }, { 0xC9, "PageUp" },
        { 0xD1, "PageDown" }, { 0xC5, "Pause" }, { 0x46, "ScrollLock" },
        { 0x52, "Num0" }, { 0x4F, "Num1" }, { 0x50, "Num2" }, { 0x51, "Num3" }, { 0x4B, "Num4" }, { 0x4C, "Num5" },
        { 0x4D, "Num6" }, { 0x47, "Num7" }, { 0x48, "Num8" }, { 0x49, "Num9" }, { 0x37, "Num*" }, { 0xB5, "Num/" },
        { 0x9C, "NumEnter" }, { 0x53, "Num." },
    };
    for (const auto& k : kKeys) {
        if (k.code == code) return k.name;
    }
    return nullptr;
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
    windowWidth        = (std::max)(0, ReadInt("Display", "WindowWidth", windowWidth, p));
    windowHeight       = (std::max)(0, ReadInt("Display", "WindowHeight", windowHeight, p));
    windowX            = ReadInt("Display", "WindowX", windowX, p);
    windowY            = ReadInt("Display", "WindowY", windowY, p);
    vsync              = ReadBool("Display", "VSync", vsync, p);
    maxFPS             = ReadInt("Display", "MaxFPS", maxFPS, p);
    shaderFilter       = ReadInt("Display", "ShaderFilter", shaderFilter, p);
    fixAspectRatio     = ReadBool("Display", "FixAspectRatio", fixAspectRatio, p);
    renderer           = ReadInt("Display", "Renderer", renderer, p);
    presenter          = ReadInt("Display", "Presenter", presenter, p);
    gpu                = ReadInt("Display", "GPU", gpu, p) == 1 ? 1 : 0;
    allowTearing       = ReadBool("Display", "AllowTearing", allowTearing, p);
    fullHeightView     = ReadBool("Display", "FullHeightView", fullHeightView, p);
    hdBackgrounds      = ReadBool("Display", "HDBackgrounds", hdBackgrounds, p);
    hdLevelLayer       = ReadBool("Display", "HDLevelLayer", hdLevelLayer, p);
    hdMaps4k           = (std::min)((std::max)(ReadInt("Display", "HDMaps4k", hdMaps4k, p), 0), 2);
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
    mapScale           = ReadInt("Display", "MapScale", mapScale, p);
    if (mapScale <= 0) mapScale = 200;
    mapScale           = (std::max)(150, (std::min)(500, mapScale));
    minimapHd          = (std::max)(0, (std::min)(3, ReadInt("Display", "MinimapHD", minimapHd, p)));
    minimapCamp        = ReadInt("Display", "MinimapCamp", minimapCamp, p) != 0 ? 1 : 0;
    smoothZoom         = ReadBool("Display", "SmoothZoom", smoothZoom, p);
    zoomStep           = ReadInt("Display", "ZoomStep", zoomStep, p);
    zoomMax            = ReadInt("Display", "ZoomMax", zoomMax, p);
    menuZoom           = ReadBool("Display", "MenuZoom", menuZoom, p);
    settingsKey        = ReadInt("Display", "SettingsKey", settingsKey, p);
    hdMenu             = ReadBool("Display", "HDMenu", hdMenu, p);
    hdMenuRemake       = ReadBool("Display", "HDMenuRemake", hdMenuRemake, p);

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
    windowsKey         = ReadBool("Input", "WindowsKey", windowsKey, p);
    lockCursorToWindow = ReadBool("Input", "LockCursorToWindow", lockCursorToWindow, p);
    edgeScrollSpeed    = ReadInt("Input", "EdgeScrollSpeed", edgeScrollSpeed, p);
    mouseReportRateHz  = ReadInt("Input", "MouseReportRateHz", mouseReportRateHz, p);
    mouseSensitivity   = ReadInt("Input", "MouseSensitivity", mouseSensitivity, p);
    quickSaveKey       = ReadKey("Input", "QuickSaveKey", p);
    quickLoadKey       = ReadKey("Input", "QuickLoadKey", p);

    // [Audio]
    audioBufferMs = ReadInt("Audio", "BufferMs", audioBufferMs, p);
    hearingRangePercent = ReadInt("Audio", "HearingRangePercent", hearingRangePercent, p);

    // [Debug]
    enableLogging  = ReadBool("Debug", "EnableLogging", enableLogging, p);
    showFPSOverlay = ReadBool("Debug", "ShowFPSOverlay", showFPSOverlay, p);
    perfLog        = ReadBool("Debug", "PerfLog", perfLog, p);
    profiler       = ReadBool("Debug", "Profiler", profiler, p);
    hangWatchdog   = ReadBool("Debug", "HangWatchdog", hangWatchdog, p);}
