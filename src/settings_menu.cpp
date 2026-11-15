#include "settings_menu.h"
#include "config.h"
#include "logger.h"
#include "ui_scale.h"
#include "view_zoom.h"
#include "window_manager.h"
#include "ddm/ddm_hd.h"

#include <MinHook.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <vector>

// See settings_menu.h.
namespace SettingsMenu {
namespace {
    std::atomic<bool> g_open{ false };
    std::atomic<int>  g_leftClicks{ 0 };     // input thread -> main thread
    std::atomic<int>  g_rightClicks{ 0 };
    std::atomic<int>  g_testCmd{ 0 };        // test harness -> main thread (one at a time)

    // main thread
    int g_sel = 0;
    struct Row { int l, t, r, b; };
    Row g_rows[32];
    int g_rowCount = 0;

    constexpr uintptr_t kAddrEnginePtr = 0x006C0FA8;
    bool InMission() {
        return *reinterpret_cast<void* const*>(kAddrEnginePtr) != nullptr;
    }

    // ---- the options --------------------------------------------------------------------
    enum Item {
        kHdBackground, kHdSprites, kBetween, kSmoothSprites, kWater,
        kUiScale, kMap, kMapDrag, kSmoothZoom, kZoomStep, kZoomMax, kMenuZoom, kHdMenu,
        kVSync, kFilter, kScroll, kFps, kMouse,
        kItemCount
    };

    const char* Label(int id) {
        switch (id) {
            case kHdBackground:  return "TLO HD - WARIANT (F6 F7 F8)";
            case kHdSprites:     return "POSTACIE I OBIEKTY HD";
            case kBetween:       return "KLATKI POSREDNIE ANIMACJI (F9)";
            case kSmoothSprites: return "WYGLADZANIE OBIEKTOW BEZ HD";
            case kWater:         return "WODA NA TLE HD";
            case kUiScale:       return "INTERFEJS W MISJI";
            case kMap:           return "MINIMAPA";
            case kMapDrag:       return "MINIMAPA - PRZECIAGANIE LEWYM";
            case kSmoothZoom:    return "PLYNNY ZOOM (LUPA)";
            case kZoomStep:      return "KROK ZOOMU";
            case kZoomMax:       return "NAJWIEKSZE PRZYBLIZENIE";
            case kMenuZoom:      return "MENU I OKNA GRY";
            case kHdMenu:        return "TLA MENU W HD (AI)";
            case kVSync:         return "SYNCHRONIZACJA PIONOWA (VSYNC)";
            case kFilter:        return "FILTR SKALOWANIA OBRAZU";
            case kScroll:        return "PLYNNE PRZEWIJANIE MAPY";
            case kFps:           return "LICZNIK FPS";
            case kMouse:         return "PREDKOSC KURSORA";
            default:             return "";
        }
    }

    // why an option cannot be changed now (nullptr: it can)
    const char* Unavailable(int id) {
        const auto& cfg = SherwoodConfig::Get();
        switch (id) {
            case kHdBackground:
                if (!cfg.hdBackgrounds) return "WYLACZONE W INI (HDBACKGROUNDS=0)";
                return InMission() ? nullptr : "ZMIANA TYLKO W MISJI";
            case kBetween:
                return InMission() ? nullptr : "ZMIANA TYLKO W MISJI";
            case kUiScale:
                return UiScale::InterfaceScaleAvailable() ? nullptr : "OD SZEROKOSCI OBRAZU 2048";
            case kMap:
                if (!UiScale::InterfaceScaleAvailable()) return "OD SZEROKOSCI OBRAZU 2048";
                return UiScale::InterfaceScaled() ? nullptr : "TYLKO Z INTERFEJSEM 2X";
            case kMapDrag:
                return UiScale::MapDragAvailable() ? nullptr : "NIEZNANA WERSJA GRY";
            case kSmoothZoom:
                return ViewZoom::Installed() ? nullptr : "TYLKO W TRYBIE HD (SZEROKOSC OD 2048)";
            case kZoomStep:
            case kZoomMax:
                if (!ViewZoom::Installed()) return "TYLKO W TRYBIE HD (SZEROKOSC OD 2048)";
                return ViewZoom::Enabled() ? nullptr : "TYLKO Z PLYNNYM ZOOMEM";
            case kMenuZoom:
                return UiScale::MenuZoomAvailable() ? nullptr : "TYLKO Z DIRECT3D 11";
            case kHdMenu:
                return UiScale::MenuZoomOn() ? nullptr : "TYLKO Z MENU NA CALY EKRAN";
            default:
                return nullptr;
        }
    }

    constexpr int kMouseSteps[] = { 0, 50, 60, 70, 80, 90, 100, 110, 120, 130, 140, 150, 175, 200 };
    constexpr int kMouseStepCount = static_cast<int>(sizeof(kMouseSteps) / sizeof(kMouseSteps[0]));

    void ValueText(int id, char* out, int n) {
        const auto& cfg = SherwoodConfig::Get();
        auto yesNo = [&](bool v) { std::snprintf(out, n, "%s", v ? "TAK" : "NIE"); };
        switch (id) {
            case kHdBackground: {
                const char* name = DDM::HD::CurrentVariantName();
                if (!name || !*name || !std::strcmp(name, "off")) {
                    std::snprintf(out, n, "WYLACZONE (ORYGINAL)");
                } else {
                    std::snprintf(out, n, "%s", name);
                }
                break;
            }
            case kHdSprites:     yesNo(cfg.hdSprites); break;
            case kBetween:       yesNo(DDM::HD::BetweenOn()); break;
            case kSmoothSprites: yesNo(cfg.smoothSprites); break;
            case kWater:
                std::snprintf(out, n, "%s", cfg.hdWater == 1 ? "NIERUCHOMA Z OBRAZU HD" :
                                            cfg.hdWater == 2 ? "HD Z MIGOTANIEM Z GRY" : "ANIMACJA Z GRY (NISKA ROZDZ.)");
                break;
            case kUiScale:       std::snprintf(out, n, "%s", UiScale::InterfaceScaled() ? "2X WIEKSZY" : "ORYGINALNY"); break;
            case kMap:           std::snprintf(out, n, "%s", UiScale::MapInCorner() ? "PRAWY GORNY ROG" : "JAK W GRZE (U GORY)"); break;
            case kMapDrag:       std::snprintf(out, n, "%s", UiScale::MapDragCamera() ? "PRZESUWA KAMERE" : "PRZESUWA OKNO (JAK W GRZE)"); break;
            case kSmoothZoom:    std::snprintf(out, n, "%s", ViewZoom::Enabled() ? "TAK" : "NIE (TYLKO 1X I 2X)"); break;
            case kZoomStep:      std::snprintf(out, n, "%d%%", ViewZoom::StepPercent()); break;
            case kZoomMax:       std::snprintf(out, n, "%.1fX", ViewZoom::MaxPercent() / 100.0); break;
            case kMenuZoom:      std::snprintf(out, n, "%s", UiScale::MenuZoomOn() ? "NA CALY EKRAN" : "ORYGINALNE (MALE)"); break;
            case kHdMenu:        yesNo(cfg.hdMenu); break;
            case kVSync:         yesNo(cfg.vsync); break;
            case kFilter:
                std::snprintf(out, n, "%s", cfg.shaderFilter == 0 ? "OSTRE PIKSELE" : (cfg.shaderFilter == 1 ? "OSTRY BILINEARNY" : "MIEKKI"));
                break;
            case kScroll:        yesNo(cfg.smoothCameraScroll); break;
            case kFps:           yesNo(cfg.showFPSOverlay); break;
            case kMouse:
                if (cfg.mouseSensitivity <= 0) std::snprintf(out, n, "AUTO");
                else std::snprintf(out, n, "%d%%", cfg.mouseSensitivity);
                break;
            default: out[0] = 0; break;
        }
        for (char* p = out; *p; ++p) *p = static_cast<char>(std::toupper(static_cast<unsigned char>(*p)));
    }

    void WriteIni(const char* section, const char* key, int value) {
        char text[16];
        std::snprintf(text, sizeof(text), "%d", value);
        WritePrivateProfileStringA(section, key, text, SherwoodConfig::Get().iniPath.c_str());
    }

    void Step(int id, int dir) {
        if (Unavailable(id)) return;
        auto& cfg = SherwoodConfig::Get();
        switch (id) {
            case kHdBackground:
                DDM::HD::RequestStep(dir);              // at the next game tick; remembered there
                break;
            case kHdSprites:
                cfg.hdSprites = !cfg.hdSprites;
                WriteIni("Display", "HDSprites", cfg.hdSprites ? 1 : 0);
                break;
            case kBetween:
                DDM::HD::RequestBetween(DDM::HD::BetweenOn() ? 0 : 1);   // at the next tick; remembered there
                break;
            case kSmoothSprites:
                cfg.smoothSprites = !cfg.smoothSprites;
                WriteIni("Display", "SmoothSprites", cfg.smoothSprites ? 1 : 0);
                break;
            case kWater: {
                // still HD water -> HD water with the game's shimmer -> the game's own frames
                static const int kOrder[3] = { 1, 2, 0 };
                int at = 0;
                while (at < 3 && kOrder[at] != cfg.hdWater) ++at;
                cfg.hdWater = kOrder[((at < 3 ? at : 0) + (dir < 0 ? 2 : 1)) % 3];
                WriteIni("Display", "HDWater", cfg.hdWater);
                break;
            }
            case kUiScale: {
                const bool on = !UiScale::InterfaceScaled();
                UiScale::SetInterfaceScale(on);
                cfg.uiScale = on ? 2 : 0;
                WriteIni("Display", "UIScale", cfg.uiScale);
                break;
            }
            case kMap: {
                const bool corner = !UiScale::MapInCorner();
                UiScale::SetMapInCorner(corner);
                cfg.mapPosition = corner ? 1 : 0;
                WriteIni("Display", "MapPosition", cfg.mapPosition);
                break;
            }
            case kMapDrag: {
                const bool on = !UiScale::MapDragCamera();
                UiScale::SetMapDragCamera(on);
                cfg.mapDragCamera = on;
                WriteIni("Display", "MapDragCamera", on ? 1 : 0);
                break;
            }
            case kSmoothZoom: {
                const bool on = !ViewZoom::Enabled();
                ViewZoom::SetEnabled(on);
                cfg.smoothZoom = on;
                WriteIni("Display", "SmoothZoom", on ? 1 : 0);
                break;
            }
            case kZoomStep: {
                static const int kSteps[] = { 110, 115, 125, 150, 200 };
                int i = 0;
                for (int k = 0; k < 5; ++k) if (kSteps[k] == ViewZoom::StepPercent()) i = k;
                i = ((i + dir) % 5 + 5) % 5;
                ViewZoom::SetStepPercent(kSteps[i]);
                cfg.zoomStep = kSteps[i];
                WriteIni("Display", "ZoomStep", cfg.zoomStep);
                break;
            }
            case kZoomMax: {
                static const int kMax[] = { 300, 400, 600, 800 };
                int i = 0;
                for (int k = 0; k < 4; ++k) if (kMax[k] == ViewZoom::MaxPercent()) i = k;
                i = ((i + dir) % 4 + 4) % 4;
                ViewZoom::SetMaxPercent(kMax[i]);
                cfg.zoomMax = kMax[i];
                WriteIni("Display", "ZoomMax", cfg.zoomMax);
                break;
            }
            case kMenuZoom: {
                const bool on = !UiScale::MenuZoomOn();
                UiScale::SetMenuZoom(on);
                cfg.menuZoom = on;
                WriteIni("Display", "MenuZoom", on ? 1 : 0);
                break;
            }
            case kHdMenu:
                cfg.hdMenu = !cfg.hdMenu;
                WriteIni("Display", "HDMenu", cfg.hdMenu ? 1 : 0);
                break;
            case kVSync:
                cfg.vsync = !cfg.vsync;
                WriteIni("Display", "VSync", cfg.vsync ? 1 : 0);
                break;
            case kFilter:
                cfg.shaderFilter = ((cfg.shaderFilter + dir) % 3 + 3) % 3;
                WriteIni("Display", "ShaderFilter", cfg.shaderFilter);
                break;
            case kScroll:
                cfg.smoothCameraScroll = !cfg.smoothCameraScroll;
                WriteIni("Performance", "SmoothCameraScroll", cfg.smoothCameraScroll ? 1 : 0);
                break;
            case kFps:
                cfg.showFPSOverlay = !cfg.showFPSOverlay;
                WriteIni("Debug", "ShowFPSOverlay", cfg.showFPSOverlay ? 1 : 0);
                break;
            case kMouse: {
                int index = 0;
                for (int i = 0; i < kMouseStepCount; ++i) {
                    if (kMouseSteps[i] == cfg.mouseSensitivity) index = i;
                }
                index = ((index + dir) % kMouseStepCount + kMouseStepCount) % kMouseStepCount;
                cfg.mouseSensitivity = kMouseSteps[index];
                WriteIni("Input", "MouseSensitivity", cfg.mouseSensitivity);
                break;
            }
            default: break;
        }
        char value[64];
        ValueText(id, value, sizeof(value));
        Logger::Info("Settings: %s -> %s.", Label(id), value);
    }

    void Open(bool open) {
        if (open == g_open.load()) return;
        g_open.store(open);
        g_leftClicks.store(0);
        g_rightClicks.store(0);
        Logger::Info("Settings: panel %s.", open ? "opened" : "closed");
    }

    void Move(int dir) {
        g_sel = ((g_sel + dir) % kItemCount + kItemCount) % kItemCount;
    }

    // a click at the game's cursor (frame pixels): the row under it changes its value
    void Click(int dir) {
        const float cx = *reinterpret_cast<const float*>(0x006C1300);
        const float cy = *reinterpret_cast<const float*>(0x006C1304);
        for (int i = 0; i < g_rowCount && i < kItemCount; ++i) {
            const Row& r = g_rows[i];
            if (cx >= r.l && cx < r.r && cy >= r.t && cy < r.b) {
                g_sel = i;
                Step(i, dir);
                return;
            }
        }
    }

    // ---- keys: edges and auto-repeat for the arrows (main thread) ------------------------------
    struct KeyState { bool down = false; DWORD since = 0; DWORD lastRepeat = 0; };
    KeyState g_keys[256];

    bool Pressed(int vk, bool focused, bool repeat) {
        if (vk <= 0 || vk > 255) return false;
        KeyState& k = g_keys[vk];
        const bool down = focused && (GetAsyncKeyState(vk) & 0x8000) != 0;
        const DWORD now = GetTickCount();
        bool fire = false;
        if (down && !k.down) {
            fire = true;
            k.since = now;
            k.lastRepeat = now;
        } else if (down && repeat && now - k.since > 350 && now - k.lastRepeat >= 90) {
            fire = true;
            k.lastRepeat = now;
        }
        k.down = down;
        return fire;
    }

    // ---- keyboard filter: the game gets no key presses while the panel is open ---------------
    struct DiRecord { uint32_t ofs, data, timeStamp, sequence; };
    using PFN_ProcessInputEvent = uint8_t(__cdecl*)(uint32_t type, const DiRecord* rec);
    PFN_ProcessInputEvent o_ProcessInputEvent = nullptr;

    uint8_t __cdecl Hook_ProcessInputEvent(uint32_t type, const DiRecord* rec) {
        if ((type & 0xFF) == 1 && rec && (rec->data & 0x80) && g_open.load(std::memory_order_relaxed)) {
            return 0;   // a key going down: the panel's (released keys still reach the game)
        }
        return o_ProcessInputEvent(type, rec);
    }

    // ---- drawing: 5x7 letters, scaled ------------------------------------------------------
    const uint8_t* Glyph(char c) {
        static const uint8_t kSpace[7] = { 0, 0, 0, 0, 0, 0, 0 };
        static const uint8_t kDigits[10][7] = {
            {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
            {0x0E,0x11,0x01,0x06,0x08,0x10,0x1F}, {0x1F,0x02,0x04,0x06,0x01,0x11,0x0E},
            {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
            {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
            {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
        };
        static const uint8_t kLetters[26][7] = {
            {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
            {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C},
            {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
            {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
            {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, {0x07,0x02,0x02,0x02,0x02,0x12,0x0C},
            {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
            {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, {0x11,0x19,0x15,0x13,0x11,0x11,0x11},
            {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
            {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
            {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
            {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, {0x11,0x11,0x11,0x11,0x11,0x0A,0x04},
            {0x11,0x11,0x11,0x15,0x15,0x1B,0x11}, {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
            {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},
        };
        static const uint8_t kDot[7]     = {0x00,0x00,0x00,0x00,0x00,0x06,0x06};
        static const uint8_t kColon[7]   = {0x00,0x06,0x06,0x00,0x06,0x06,0x00};
        static const uint8_t kDash[7]    = {0x00,0x00,0x00,0x1F,0x00,0x00,0x00};
        static const uint8_t kLParen[7]  = {0x02,0x04,0x08,0x08,0x08,0x04,0x02};
        static const uint8_t kRParen[7]  = {0x08,0x04,0x02,0x02,0x02,0x04,0x08};
        static const uint8_t kPercent[7] = {0x18,0x19,0x02,0x04,0x08,0x13,0x03};
        static const uint8_t kLess[7]    = {0x02,0x04,0x08,0x10,0x08,0x04,0x02};
        static const uint8_t kGreater[7] = {0x08,0x04,0x02,0x01,0x02,0x04,0x08};
        static const uint8_t kSlash[7]   = {0x01,0x01,0x02,0x04,0x08,0x10,0x10};
        static const uint8_t kComma[7]   = {0x00,0x00,0x00,0x00,0x06,0x04,0x08};
        static const uint8_t kEquals[7]  = {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00};
        static const uint8_t kBang[7]    = {0x04,0x04,0x04,0x04,0x04,0x00,0x04};
        static const uint8_t kPlus[7]    = {0x00,0x04,0x04,0x1F,0x04,0x04,0x00};
        static const uint8_t kUnder[7]   = {0x00,0x00,0x00,0x00,0x00,0x00,0x1F};
        if (c >= '0' && c <= '9') return kDigits[c - '0'];
        if (c >= 'A' && c <= 'Z') return kLetters[c - 'A'];
        if (c >= 'a' && c <= 'z') return kLetters[c - 'a'];
        switch (c) {
            case '.': return kDot;
            case ':': return kColon;
            case '-': return kDash;
            case '(': return kLParen;
            case ')': return kRParen;
            case '%': return kPercent;
            case '<': return kLess;
            case '>': return kGreater;
            case '/': return kSlash;
            case ',': return kComma;
            case '=': return kEquals;
            case '!': return kBang;
            case '+': return kPlus;
            case '_': return kUnder;
            default:  return kSpace;
        }
    }

    // RGB565 colours
    constexpr uint16_t kTitle    = 0xFEEC;
    constexpr uint16_t kText     = 0xDEFA;
    constexpr uint16_t kValue    = 0xFE4A;
    constexpr uint16_t kGrey     = 0x8410;
    constexpr uint16_t kHint     = 0xB5B6;
    constexpr uint16_t kBorder   = 0xCD08;
    constexpr uint16_t kSelected = 0x29E3;

    uint32_t To8888(uint16_t c) {
        uint32_t r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
        r = (r << 3) | (r >> 2);
        g = (g << 2) | (g >> 4);
        b = (b << 3) | (b >> 2);
        return 0xFF000000u | (r << 16) | (g << 8) | b;
    }

    // The panel is composed in ordinary memory (the texture the presenter fills is write-combined:
    // reading it back is very slow) and then copied into the texture row by row.
    struct Canvas {
        uint16_t* px;
        int w, h;

        void Fill(int l, int t, int r, int b, uint16_t c) const {
            for (int y = (std::max)(t, 0); y < (std::min)(b, h); ++y) {
                uint16_t* row = px + static_cast<size_t>(y) * w;
                for (int x = (std::max)(l, 0); x < (std::min)(r, w); ++x) row[x] = c;
            }
        }
        void Text(int x, int y, const char* s, int scale, uint16_t c) const {
            for (int i = 0; s[i]; ++i) {
                const uint8_t* g = Glyph(s[i]);
                const int cx = x + i * 6 * scale;
                for (int gy = 0; gy < 7; ++gy) {
                    for (int gx = 0; gx < 5; ++gx) {
                        if (!(g[gy] & (0x10 >> gx))) continue;
                        Fill(cx + gx * scale, y + gy * scale, cx + (gx + 1) * scale, y + (gy + 1) * scale, c);
                    }
                }
            }
        }
    };

    int TextWidth(const char* s, int scale) {
        return static_cast<int>(std::strlen(s)) * 6 * scale;
    }

    std::vector<uint16_t> g_panel;      // main thread
    std::vector<uint32_t> g_row8888;

    void CopyOut(uint8_t* dst, LONG dstPitch, bool wide, int x0, int y0, int pw, int ph);

    // `src`: the frame pixels under the texture (RGB565), `dst`: the mapped texture
    void DrawPanel(uint8_t* dst, LONG dstPitch, bool wide, int w, int h, int originX, int originY,
                   const uint8_t* src, int srcPitch) {
        if (!g_open.load() || !dst || !src || w < 320 || h < 240) return;
        const int s = h >= 1080 ? 4 : (h >= 600 ? 2 : 1);
        const int ch = 6 * s;                   // letter advance
        const int rowH = 12 * s;
        const int cols = 62;
        const int pad = 3 * ch;
        const bool running = InMission() && !UiScale::DialogShown();
        const int lines = kItemCount + 6 + (running ? 1 : 0);
        const int pw = (std::min)(cols * ch + 2 * pad, w);
        const int ph = (std::min)(lines * rowH + 2 * pad, h);
        const int x0 = (w - pw) / 2, y0 = (h - ph) / 2;

        // the picture behind, at an eighth of its brightness
        g_panel.resize(static_cast<size_t>(pw) * ph);
        for (int y = 0; y < ph; ++y) {
            const auto* sp = reinterpret_cast<const uint16_t*>(src + static_cast<size_t>(y0 + y) * srcPitch) + x0;
            uint16_t* row = g_panel.data() + static_cast<size_t>(y) * pw;
            for (int x = 0; x < pw; ++x) row[x] = static_cast<uint16_t>((sp[x] & 0xC718u) >> 3);
        }
        const Canvas cv{ g_panel.data(), pw, ph };
        const int bt = (std::max)(2, s / 2 + 1);
        cv.Fill(0, 0, pw, bt, kBorder);
        cv.Fill(0, ph - bt, pw, ph, kBorder);
        cv.Fill(0, 0, bt, ph, kBorder);
        cv.Fill(pw - bt, 0, pw, ph, kBorder);

        int y = pad;
        const char* title = "USTAWIENIA SHERWOODFIX";
        cv.Text((pw - TextWidth(title, s)) / 2, y, title, s, kTitle);
        y += rowH * 2;

        const int textX = pad;
        const int right = pw - pad;
        g_rowCount = 0;
        for (int i = 0; i < kItemCount; ++i) {
            const bool sel = i == g_sel;
            const int rt = y - (rowH - 7 * s) / 2;
            if (sel) cv.Fill(bt + s, rt, pw - bt - s, rt + rowH, kSelected);
            const char* why = Unavailable(i);
            cv.Text(textX, y, Label(i), s, why ? kGrey : kText);
            char value[80];
            if (why) {
                std::snprintf(value, sizeof(value), "%s", why);
            } else {
                char v[64];
                ValueText(i, v, sizeof(v));
                std::snprintf(value, sizeof(value), sel ? "< %s >" : "%s", v);
            }
            cv.Text(right - TextWidth(value, s), y, value, s, why ? kGrey : (sel ? kTitle : kValue));
            g_rows[g_rowCount++] = Row{ originX + x0, originY + y0 + rt, originX + x0 + pw, originY + y0 + rt + rowH };
            y += rowH;
        }
        y += rowH;
        const char* hint1 = "KLIK LUB STRZALKI: ZMIANA   F10 / ESC: ZAMKNIJ";
        const char* hint2 = "ZMIANY DZIALAJA OD RAZU I SA ZAPISYWANE W SHERWOODFIX.INI";
        cv.Text((pw - TextWidth(hint1, s)) / 2, y, hint1, s, kHint);
        y += rowH;
        cv.Text((pw - TextWidth(hint2, s)) / 2, y, hint2, s, kHint);
        if (running) {
            y += rowH;
            const char* hint3 = "GRA SIE NIE ZATRZYMUJE - PAUZA: NAJPIERW ESC, POTEM F10";
            cv.Text((pw - TextWidth(hint3, s)) / 2, y, hint3, s, kGrey);
        }

        CopyOut(dst, dstPitch, wide, x0, y0, pw, ph);
    }

    // g_panel (pw x ph) into the texture at (x0, y0): writes only
    void CopyOut(uint8_t* dst, LONG dstPitch, bool wide, int x0, int y0, int pw, int ph) {
        if (wide) g_row8888.resize(static_cast<size_t>(pw));
        for (int yy = 0; yy < ph; ++yy) {
            const uint16_t* row = g_panel.data() + static_cast<size_t>(yy) * pw;
            uint8_t* d = dst + static_cast<size_t>(y0 + yy) * dstPitch;
            if (wide) {
                for (int x = 0; x < pw; ++x) g_row8888[static_cast<size_t>(x)] = To8888(row[x]);
                std::memcpy(d + static_cast<size_t>(x0) * 4, g_row8888.data(), static_cast<size_t>(pw) * 4);
            } else {
                std::memcpy(d + static_cast<size_t>(x0) * 2, row, static_cast<size_t>(pw) * 2);
            }
        }
    }

    // In the menus and the game's dialogs: where the mod's settings are (the player looked for
    // them in the game's own graphics options).
    void DrawHint(uint8_t* dst, LONG dstPitch, bool wide, int w, int h, const uint8_t* src, int srcPitch) {
        const char* text = "F10 - USTAWIENIA MODA";
        const int s = h >= 1080 ? 3 : (h >= 600 ? 2 : 1);
        const int pad = 3 * s;
        const int bw = TextWidth(text, s) + 2 * pad - s, bh = 7 * s + 2 * pad;
        const int x0 = w - bw - 4 * s, y0 = h - bh - 4 * s;
        if (!dst || !src || x0 < 0 || y0 < 0) return;
        g_panel.resize(static_cast<size_t>(bw) * bh);
        for (int y = 0; y < bh; ++y) {
            const auto* sp = reinterpret_cast<const uint16_t*>(src + static_cast<size_t>(y0 + y) * srcPitch) + x0;
            uint16_t* row = g_panel.data() + static_cast<size_t>(y) * bw;
            for (int x = 0; x < bw; ++x) row[x] = static_cast<uint16_t>((sp[x] & 0xE79Cu) >> 2);
        }
        const Canvas cv{ g_panel.data(), bw, bh };
        cv.Text(pad, pad, text, s, kHint);
        CopyOut(dst, dstPitch, wide, x0, y0, bw, bh);
    }
}

void Install() {
    // ProcessInputEvent 0x005F0C20: mov al,[0x006C1204] / sub esp,14h / cmp al,1
    static const uint8_t kProlog[] = { 0xA0, 0x04, 0x12, 0x6C, 0x00, 0x83, 0xEC, 0x14, 0x3C, 0x01 };
    const int key = SherwoodConfig::Get().settingsKey;
    if (key <= 0) {
        Logger::Info("Settings: panel has no key (SettingsKey=0).");
        return;
    }
    if (std::memcmp(reinterpret_cast<const void*>(0x005F0C20), kProlog, sizeof(kProlog)) == 0 &&
        MH_CreateHook(reinterpret_cast<LPVOID>(0x005F0C20), reinterpret_cast<LPVOID>(&Hook_ProcessInputEvent),
                      reinterpret_cast<LPVOID*>(&o_ProcessInputEvent)) == MH_OK &&
        MH_EnableHook(reinterpret_cast<LPVOID>(0x005F0C20)) == MH_OK) {
        Logger::Info("Settings: panel on key 0x%02X; the game's key presses are held back while it is open.", key);
    } else {
        o_ProcessInputEvent = nullptr;
        Logger::Warn("Settings: input event code not recognised - the game also sees keys pressed in the panel.");
    }
}

bool IsOpen() {
    return g_open.load(std::memory_order_relaxed);
}

void Poll() {
    const auto& cfg = SherwoodConfig::Get();
    const HWND hwnd = WindowManager::GetGameWindow();
    const bool focused = hwnd && GetForegroundWindow() == hwnd;

    switch (g_testCmd.exchange(0)) {
        case 1: Open(true); break;
        case 2: Open(false); break;
        case 3: Move(-1); break;
        case 4: Move(+1); break;
        case 5: Step(g_sel, -1); break;
        case 6: Step(g_sel, +1); break;
        case 7: Click(+1); break;
        default: break;
    }

    if (Pressed(cfg.settingsKey, focused, false)) Open(!g_open.load());
    const bool open = g_open.load();
    const bool esc = Pressed(VK_ESCAPE, focused, false);
    const bool up = Pressed(VK_UP, focused, true);
    const bool down = Pressed(VK_DOWN, focused, true);
    const bool left = Pressed(VK_LEFT, focused, true);
    const bool right = Pressed(VK_RIGHT, focused, true);
    const bool enter = Pressed(VK_RETURN, focused, false);
    if (!open) {
        g_leftClicks.store(0);
        g_rightClicks.store(0);
        return;
    }
    if (esc) {
        Open(false);
        return;
    }
    if (up) Move(-1);
    if (down) Move(+1);
    if (left) Step(g_sel, -1);
    if (right || enter) Step(g_sel, +1);
    if (g_leftClicks.exchange(0) > 0) Click(+1);
    if (g_rightClicks.exchange(0) > 0) Click(-1);
}

void Draw(uint8_t* dst, LONG dstPitch, bool dst8888, int w, int h, int originX, int originY,
          const uint8_t* src, int srcPitch) {
    if (g_open.load()) {
        DrawPanel(dst, dstPitch, dst8888, w, h, originX, originY, src, srcPitch);
    } else if (UiScale::DialogShown() && SherwoodConfig::Get().settingsKey == 0x79) {
        DrawHint(dst, dstPitch, dst8888, w, h, src, srcPitch);
    }
}

bool OnMouseButton(uint32_t ofs, uint32_t data) {
    if (!g_open.load(std::memory_order_relaxed) || ofs < 0x0C || ofs > 0x0F) return false;
    if (!(data & 0x80)) return false;           // releases reach the game (a press before opening)
    if (ofs == 0x0C) g_leftClicks.fetch_add(1);
    else if (ofs == 0x0D) g_rightClicks.fetch_add(1);
    return true;
}

void TestCommand(const char* what, char* out, int size) {
    static const char* const kCmds[] = { "", "open", "close", "up", "down", "left", "right", "click" };
    for (int i = 1; i < 8; ++i) {
        if (!std::strcmp(what, kCmds[i])) {
            g_testCmd.store(i);
            const DWORD t0 = GetTickCount();
            while (g_testCmd.load() != 0 && GetTickCount() - t0 < 2000) Sleep(5);
            break;
        }
    }
    int len = std::snprintf(out, size, "open=%d sel=%d", g_open.load() ? 1 : 0, g_sel);
    for (int i = 0; i < kItemCount && len < size - 80; ++i) {
        char v[64];
        const char* why = Unavailable(i);
        if (why) std::snprintf(v, sizeof(v), "(%s)", why);
        else ValueText(i, v, sizeof(v));
        len += std::snprintf(out + len, size - len, " | %s", v);
    }
}

} // namespace SettingsMenu
