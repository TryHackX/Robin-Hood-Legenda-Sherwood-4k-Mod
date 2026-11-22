#include "ddm_hd.h"
#include "ddm_hd_font.h"
#include "ddm_hd_patch.h"
#include "ddm_hd_sprites.h"
#include "../config.h"
#include "../diag.h"
#include "../logger.h"
#include "../render_ddraw.h"
#include "../test_harness.h"
#include "../timing.h"
#include "../ui_scale.h"
#include "../window_manager.h"
#include "ddm.h"

#include <MinHook.h>
#include <emmintrin.h>
#include <intrin.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace DDM::HD {
namespace {
    constexpr uintptr_t kAddrEnginePtr  = 0x006C0FA8; // RHEngine*; map size floats at +0x4730 / +0x4734
    constexpr int       kOffCameraState = 0x48C0;     // 1 idle, 2 scroll, 3/4 zoom transition, 5 full redraw
    constexpr uintptr_t kAddrTintSpan2x = 0x0062469C; // see Hook_TintSpan2x
    constexpr uintptr_t kAddrSpanMask   = 0x006C11C8; // 4 words: channel masks of the span routines

    constexpr int kMaxMapped = 3; // HD pictures kept mapped at a time (60+ MB of address space each)

    // A sprite counts as a repaint of the ground (water) when its opaque pixels differ from the
    // level picture beneath by less than this on average (per channel, of 255). Measured values
    // are in the log ("HD: sprite ... differs from the ground beneath by ...").
    // Measured on Nottingham (day): the ten water animations 8.0 .. 13.7, every other large
    // sprite (trees, buildings, carts, mill) 45 .. 82.
    constexpr double kGroundRepaintMaxDiff = 25.0;
    // Water is irregular (the water animations measured: 39 .. 67 % of the rectangle opaque).
    // A repaint that covers (nearly) its whole rectangle is an overlay instead: the smoke above
    // the chimneys of Lincoln's houses is a full rectangle (98 .. 100 %) that repaints rock and
    // roof with thin smoke - as close to the ground as water (12.8 .. 18.2 in its first frames).
    // It was left out as water (user report 2026-11-15: no smoke in HD).
    constexpr double kGroundOverlayMinCoverage = 90.0;
    // An overlay is blended over the HD picture only where it differs from the level picture
    // beneath (the frame's own copy of the ground is low-resolution and block-compressed: drawn
    // as it is, it showed as a blurry patch around the smoke). How much of the (3x3 smoothed)
    // frame covers the HD picture follows the size of that difference (0..255, the largest
    // channel): nothing up to the first value - compression noise (median 9.5) - all of it from
    // the second; the smoke itself is 30 .. 75 (90th .. 99th percentile). (Adding the difference
    // to the HD picture instead kept the rock's detail under the smoke: it looked too thin.)
    constexpr int kOverlayFadeFrom = 8, kOverlayFadeTo = 36;

    enum class Load { Missing, Mapped, Unloaded };

    struct HdMap {
        int      variant = 0;
        uint64_t key = 0;
        int      w = 0, h = 0;              // size of the HD picture (2x the level)
        const uint16_t* px = nullptr;       // RGB565 rows, top to bottom (Mapped only)
        const void* view = nullptr;
        HANDLE   mapping = nullptr;
        HANDLE   file = INVALID_HANDLE_VALUE;
        Load     state = Load::Missing;
        uint32_t lastUse = 0;
    };
    std::deque<HdMap> g_maps;               // deque: entries never move (g_current points into it)
    uint32_t g_useCounter = 0;

    // ---- variants (sub-folders of hd\maps), main thread only
    std::vector<std::string> g_variants;
    bool g_scanned = false;
    int  g_variant = -1;                    // index into g_variants; -1 = original pixels
    int  g_lastOn = 0;                      // the variant "toggle" returns to
    char g_variantName[64] = "off";         // copy for other threads (stat of the test harness)

    enum : int { kReqNone = 0, kReqNext, kReqPrev, kReqToggle, kReqName };
    std::atomic<int> g_request{ kReqNone };
    char g_requestName[64]{};               // written before g_request = kReqName

    // ---- result of the last look-up (the game blits from the same surface thousands of times)
    const void*  g_identity = nullptr;
    unsigned     g_lockGen = 0, g_writeGen = 0;
    int          g_lookupVariant = -2;
    uint64_t     g_lookupKey = 0;
    DWORD        g_lastVerifyMs = 0;
    const HdMap* g_current = nullptr;
    // the level's map surface the current HD picture belongs to (for the tint spans)
    const uint8_t* g_srcBits = nullptr;
    int g_srcPitch = 0, g_srcW = 0, g_srcH = 0;

    // The level picture the HD picture was made from: a copy taken when the HD picture was found.
    // The game changes parts of its level picture while playing - a building is shown from the
    // inside while a character is in it (2026-11-14: the HD background kept showing the outside
    // walls and roof until the next full redraw, and the whole level lost its HD picture while
    // inside). Every background blit compares the level picture with this copy: where it
    // differs, the game's pixels are shown, everywhere else the HD picture.
    std::vector<uint16_t> g_base;
    const HdMap* g_baseMap = nullptr;
    uint64_t     g_baseKey = 0;

    bool g_redrawPending = false;

    // Where the frame sits in the HD picture: destination pixel (0, 0) of the engine's world
    // surface (and of the frame composed from it) shows HD pixel (g_originX, g_originY).
    // Known from every background blit; equals twice the camera position.
    int  g_originX = 0, g_originY = 0;
    bool g_originValid = false;

    // Sprites of this level that were compared with the level picture beneath them (see
    // DrawGroundAnimation).
    struct WaterRun { int y, x0, x1; size_t off; };     // pixels of the water in one HD row
    struct WaterAnim {                  // animated HD water (HDWater=2/3), made by the water thread
        // input, written before the job is queued
        const void* map = nullptr;      // the HD picture it was made from (a variant switch makes it stale)
        int  W = 0, H = 0, ox = 0, oy = 0, strength = 2;
        std::vector<uint16_t> hd;       // the HD pixels of the sprite's rectangle
        std::vector<uint8_t>  mask;     // where the game's sprite is opaque (HD pixels)
        // output, valid once state == 2
        std::vector<WaterRun> runs;
        size_t pixels = 0;              // water pixels per frame
        std::vector<uint16_t> frames;   // kWaterFrames x pixels, RGB565
        std::atomic<int> state{ 0 };    // 0 queued, 1 being made, 2 ready, 3 failed / cancelled / dropped
        std::atomic<bool> cancel{ false };
        LONGLONG readyAt = 0;           // QueryPerformanceCounter when first drawn (fade-in)
        uint64_t lastUse = 0;
    };
    struct GroundFrames {               // the last two frames, 2x magnified, for a cross-fade
        int W = 0, H = 0;                       // size of the magnified frame
        std::vector<int16_t> cur, prev;         // 4 planes of W x H values (see Magnify2x)
        std::vector<int16_t> curSpan, prevSpan; // per HD row: first and end column with something to do
        uint64_t curHash = 0;
        LONGLONG tCur = 0;              // QueryPerformanceCounter when `cur` appeared
        double   intervalMs = 0.0;      // how long a frame stays (mean of the last changes)
        bool     hasPrev = false;
    };
    struct GroundAnim {
        int  mx = 0, my = 0, w = 0, h = 0;  // position in level pixels, frame size
        bool repaint = false;               // looks like the ground it is drawn on
        bool overlay = false;               // ... and covers its whole rectangle: smoke
        std::shared_ptr<GroundFrames> frames;
        std::shared_ptr<WaterAnim> water;
    };
    std::vector<GroundAnim> g_groundAnims;

    std::string GameDir() {
        char exe[MAX_PATH]{};
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        std::string path(exe);
        path.erase(path.find_last_of("\\/") + 1);
        return path;
    }

    uint8_t* Engine() {
        __try {
            return *reinterpret_cast<uint8_t**>(kAddrEnginePtr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    bool IsLevelBackground(const Buf& src) {
        __try {
            const auto* engine = *reinterpret_cast<const uint8_t* const*>(kAddrEnginePtr);
            if (!engine) return false;
            const float mapW = *reinterpret_cast<const float*>(engine + 0x4730);
            const float mapH = *reinterpret_cast<const float*>(engine + 0x4734);
            return src.w >= 640 && src.w == static_cast<int>(mapW) && src.h == static_cast<int>(mapH);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // the copy of the level picture the current HD picture belongs to (see g_base)
    unsigned g_levelPicturesSerial = 0;     // LevelPicturesNow: changes with the pictures it gives

    // (also without an HD picture for it: another variant may have one - see the lookup in TryBackgroundBlit)
    void TakeBase(const Buf& src, uint64_t key, const HdMap* hd) {
        ++g_levelPicturesSerial;
        g_baseMap = nullptr;
        g_baseKey = 0;
        g_base.clear();
        try {
            g_base.resize(static_cast<size_t>(src.w) * src.h);
        } catch (...) {
            g_base.clear();
            return;
        }
        for (int y = 0; y < src.h; ++y) {
            std::memcpy(g_base.data() + static_cast<size_t>(y) * src.w, src.bits + static_cast<size_t>(y) * src.pitch,
                        static_cast<size_t>(src.w) * 2);
        }
        g_baseMap = hd;
        g_baseKey = key;
    }

    // share of the level picture (sampled) that differs from the copy; -1 = no comparable copy
    double ChangedShare(const Buf& src) {
        if (g_base.size() != static_cast<size_t>(src.w) * src.h) return -1.0;
        size_t n = 0, diff = 0;
        for (int y = 0; y < src.h; y += 8) {
            const auto* cur = reinterpret_cast<const uint16_t*>(src.bits + static_cast<size_t>(y) * src.pitch);
            const uint16_t* base = g_base.data() + static_cast<size_t>(y) * src.w;
            for (int x = 0; x < src.w; x += 8) {
                ++n;
                diff += cur[x] != base[x];
            }
        }
        return n ? static_cast<double>(diff) / static_cast<double>(n) : -1.0;
    }

    // Same walk as tools\hd\upscale_maps.py: FNV-1a over every 7th pixel in row order.
    uint64_t HashPixels(const Buf& b) {
        uint64_t hash = 0xCBF29CE484222325ULL;
        const size_t total = static_cast<size_t>(b.w) * static_cast<size_t>(b.h);
        for (size_t i = 0; i < total; i += 7) {
            const size_t y = i / static_cast<size_t>(b.w), x = i % static_cast<size_t>(b.w);
            const uint16_t p = *reinterpret_cast<const uint16_t*>(b.bits + y * static_cast<size_t>(b.pitch) + x * 2);
            hash = (hash ^ p) * 0x100000001B3ULL;
        }
        return hash;
    }

    void ScanVariants() {
        if (g_scanned) return;
        g_scanned = true;
        const std::string base = GameDir() + "hd\\maps\\";
        WIN32_FIND_DATAA fd{};
        HANDLE find = FindFirstFileA((base + "*").c_str(), &fd);
        if (find != INVALID_HANDLE_VALUE) {
            do {
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.') {
                    g_variants.emplace_back(fd.cFileName);
                }
            } while (FindNextFileA(find, &fd));
            FindClose(find);
        }
        std::sort(g_variants.begin(), g_variants.end());

        const auto& cfg = SherwoodConfig::Get();
        g_variant = -1;
        if (cfg.hdBackgrounds && !g_variants.empty()) {
            g_variant = 0;
            if (_stricmp(cfg.hdVariant.c_str(), "off") == 0) {
                g_variant = -1;
            } else {
                for (size_t i = 0; i < g_variants.size(); ++i) {
                    if (_stricmp(g_variants[i].c_str(), cfg.hdVariant.c_str()) == 0) g_variant = static_cast<int>(i);
                }
            }
        }
        if (g_variant >= 0) g_lastOn = g_variant;
        std::snprintf(g_variantName, sizeof(g_variantName), "%s", g_variant >= 0 ? g_variants[static_cast<size_t>(g_variant)].c_str() : "off");
        Sprites::SetVariant(g_variantName);

        std::string list;
        for (const auto& v : g_variants) list += (list.empty() ? "" : ", ") + v;
        Logger::Info("HD: %zu background variant(s) in hd\\maps: %s; showing: %s.", g_variants.size(),
                     list.empty() ? "(none)" : list.c_str(), g_variantName);
    }

    void Unmap(HdMap& m) {
        if (m.view) UnmapViewOfFile(m.view);
        if (m.mapping) CloseHandle(m.mapping);
        if (m.file != INVALID_HANDLE_VALUE) CloseHandle(m.file);
        m.view = nullptr;
        m.mapping = nullptr;
        m.file = INVALID_HANDLE_VALUE;
        m.px = nullptr;
        if (m.state == Load::Mapped) m.state = Load::Unloaded;
    }

    bool Map(HdMap& m, int baseW, int baseH, bool firstTime) {
        char name[160];
        std::snprintf(name, sizeof(name), "hd\\maps\\%s\\%016llx.hdmap", g_variants[static_cast<size_t>(m.variant)].c_str(),
                      static_cast<unsigned long long>(m.key));
        const std::string path = GameDir() + name;
        m.file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (m.file == INVALID_HANDLE_VALUE) {
            m.state = Load::Missing;
            if (firstTime) {
                Logger::Info("HD: variant '%s' has no picture for this level (%dx%d, key %016llx) - original pixels.",
                             g_variants[static_cast<size_t>(m.variant)].c_str(), baseW, baseH, static_cast<unsigned long long>(m.key));
            }
            return false;
        }
        m.mapping = CreateFileMappingA(m.file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        m.view = m.mapping ? MapViewOfFile(m.mapping, FILE_MAP_READ, 0, 0, 0) : nullptr;
        LARGE_INTEGER size{};
        GetFileSizeEx(m.file, &size);
        struct Header { char magic[4]; uint32_t version, width, height, reserved; };
        const auto* hdr = static_cast<const Header*>(m.view);
        if (m.view && size.QuadPart >= static_cast<LONGLONG>(sizeof(Header)) && std::memcmp(hdr->magic, "HDMP", 4) == 0 &&
            hdr->version == 1 && hdr->width == static_cast<uint32_t>(baseW) * 2 && hdr->height == static_cast<uint32_t>(baseH) * 2 &&
            size.QuadPart >= static_cast<LONGLONG>(sizeof(Header)) + static_cast<LONGLONG>(hdr->width) * hdr->height * 2) {
            m.w = static_cast<int>(hdr->width);
            m.h = static_cast<int>(hdr->height);
            m.px = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(m.view) + sizeof(Header));
            m.state = Load::Mapped;
            if (firstTime) {
                Logger::Info("HD: upscaled background %s in use (%dx%d for a %dx%d level).", name, m.w, m.h, baseW, baseH);
            }
            return true;
        }
        Logger::Warn("HD: %s does not fit this level (%dx%d), is damaged, or could not be mapped (error %lu) - ignored.",
                     name, baseW, baseH, GetLastError());
        Unmap(m);
        m.state = Load::Missing;
        return false;
    }

    const HdMap* Resolve(int variant, uint64_t key, int baseW, int baseH) {
        HdMap* entry = nullptr;
        for (HdMap& m : g_maps) {
            if (m.variant == variant && m.key == key) entry = &m;
        }
        const bool firstTime = entry == nullptr;
        if (!entry) {
            g_maps.emplace_back();
            entry = &g_maps.back();
            entry->variant = variant;
            entry->key = key;
        }
        if (entry->state == Load::Unloaded || firstTime) {
            Map(*entry, baseW, baseH, firstTime);
        }
        entry->lastUse = ++g_useCounter;
        // keep the address space use bounded (32-bit process)
        for (;;) {
            int mapped = 0;
            HdMap* oldest = nullptr;
            for (HdMap& m : g_maps) {
                if (m.state != Load::Mapped) continue;
                ++mapped;
                if (&m != entry && (!oldest || m.lastUse < oldest->lastUse)) oldest = &m;
            }
            if (mapped <= kMaxMapped || !oldest) break;
            Unmap(*oldest);
        }
        return entry;
    }

    void Forget() {
        g_identity = nullptr;
        g_current = nullptr;
        g_lookupVariant = -2;
        g_srcBits = nullptr;
        g_originValid = false;
        g_groundAnims.clear();
    }

    void SetVariant(int index) {
        if (index >= static_cast<int>(g_variants.size())) index = -1;
        if (index < -1) index = static_cast<int>(g_variants.size()) - 1;
        if (index == g_variant) return;
        g_variant = index;
        if (index >= 0) g_lastOn = index;
        const char* name = index >= 0 ? g_variants[static_cast<size_t>(index)].c_str() : "off";
        std::snprintf(g_variantName, sizeof(g_variantName), "%s", name);
        Sprites::SetVariant(name);
        Forget();
        g_redrawPending = true;

        char text[96];
        if (index >= 0) {
            std::snprintf(text, sizeof(text), "GRAFIKA HD: %s (%d Z %zu)", name, index + 1, g_variants.size());
        } else {
            std::snprintf(text, sizeof(text), "GRAFIKA HD: WYLACZONA (ORYGINAL)");
        }
        RenderDDraw::ShowNotice(text, 3.0);
        Logger::Info("HD: background variant switched to '%s'.", name);
        // remembered for the next start
        WritePrivateProfileStringA("Display", "HDVariant", name, SherwoodConfig::Get().iniPath.c_str());
    }

    // The engine redraws its whole world surface (RenderBackground) when the camera state is 5;
    // it sets that itself after loading and after resolution changes. Not during a zoom
    // transition (3/4): wait for it to finish.
    void RedrawWhenPossible() {
        if (!g_redrawPending) return;
        uint8_t* engine = Engine();
        if (!engine) {
            g_redrawPending = false;
            return;
        }
        __try {
            uint8_t& state = engine[kOffCameraState];
            if (state == 1 || state == 2) {
                state = 5;
                g_redrawPending = false;
            } else if (state == 5) {
                g_redrawPending = false;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_redrawPending = false;
        }
    }

    std::atomic<int> g_betweenRequest{ -1 }; // -1 none, 0 off, 1 on, 2 the other state

    void PollHotKeys() {
        const auto& cfg = SherwoodConfig::Get();
        static bool s_down[4] = { false, false, false, false };
        const int keys[4] = { cfg.hdKeyToggle, cfg.hdKeyNext, cfg.hdKeyPrev, cfg.hdKeyBetween };
        const HWND hwnd = WindowManager::GetGameWindow();
        const bool focused = hwnd && GetForegroundWindow() == hwnd;
        for (int i = 0; i < 4; ++i) {
            const bool down = focused && keys[i] > 0 && (GetAsyncKeyState(keys[i]) & 0x8000) != 0;
            if (down && !s_down[i]) {
                if (i == 3) g_betweenRequest.store(2);
                else g_request.store(i == 0 ? kReqToggle : (i == 1 ? kReqNext : kReqPrev));
            }
            s_down[i] = down;
        }
    }

    // In-between frames: the state at start comes from the settings; a switch is shown on the
    // screen and remembered for the next start.
    void UpdateBetween() {
        static bool s_started = false;
        if (!s_started) {
            s_started = true;
            if (SherwoodConfig::Get().hdInBetween && Sprites::BetweenAvailable()) Sprites::SetBetween(true);
        }
        const int req = g_betweenRequest.exchange(-1);
        if (req < 0) return;
        if (!Sprites::BetweenAvailable()) {
            RenderDDraw::ShowNotice("KLATKI POSREDNIE: BRAK DANYCH W HD - SPRITES - BETWEEN", 3.0);
            return;
        }
        const bool on = req == 2 ? !Sprites::BetweenOn() : req == 1;
        Sprites::SetBetween(on);
        RenderDDraw::ShowNotice(on ? "KLATKI POSREDNIE: WLACZONE" : "KLATKI POSREDNIE: WYLACZONE", 3.0);
        Logger::Info("HD: in-between frames switched %s.", on ? "on" : "off");
        WritePrivateProfileStringA("Display", "HDInBetween", on ? "1" : "0", SherwoodConfig::Get().iniPath.c_str());
    }

    // ---------------------------------------------------------------------------------------
    // Water. The game draws its water (and any other tinted area) as polygons, every frame, by
    // reading the pixels straight from the map surface and blending them with a colour
    // (0x005EEC00, only caller 0x00461D98). At 2x zoom the span routine 0x0062469C writes each
    // blended source pixel as a 2x2 block - blocky low-resolution water on top of the HD
    // background. Same blend here, but with the four HD pixels that belong to the source pixel.
    using PFN_TintSpan = void(__cdecl*)(const uint16_t* src, uint16_t* dst, int dstPitch, int count, uint32_t color, uint32_t invAlpha);
    PFN_TintSpan o_TintSpan2x = nullptr;

    std::atomic<unsigned> g_debugFlags{ 0 }; // test harness: bit 0/1/2 = leave out a drawing routine

    void __cdecl Hook_TintSpan2x(const uint16_t* src, uint16_t* dst, int dstPitch, int count, uint32_t color, uint32_t invAlpha) {
        if (g_debugFlags.load(std::memory_order_relaxed) & 4u) return;
        const HdMap* hd = g_current;
        const auto* p = reinterpret_cast<const uint8_t*>(src);
        if (!hd || !hd->px || !g_srcBits || count <= 0 || g_srcPitch <= 0 ||
            p < g_srcBits || p >= g_srcBits + static_cast<size_t>(g_srcPitch) * static_cast<size_t>(g_srcH)) {
            o_TintSpan2x(src, dst, dstPitch, count, color, invAlpha);
            return;
        }
        const size_t off = static_cast<size_t>(p - g_srcBits);
        const int y = static_cast<int>(off / static_cast<size_t>(g_srcPitch));
        const int x = static_cast<int>(off % static_cast<size_t>(g_srcPitch)) / 2;
        if (x + count > g_srcW || y >= g_srcH) {
            o_TintSpan2x(src, dst, dstPitch, count, color, invAlpha);
            return;
        }
        // The original (MMX): per channel  out = (c << k) * (invAlpha << 6) >> 16  +  (tint << 5),
        // masked and packed back to 5-6-5; tint is the colour already multiplied by alpha.
        const auto* mask = reinterpret_cast<const uint16_t*>(kAddrSpanMask);
        const uint32_t mB = mask[0], mG = mask[1], mR = mask[2];
        const int32_t  mul = static_cast<int16_t>(static_cast<uint16_t>(invAlpha << 6));
        const uint32_t addB = (color & 0xFF) << 5, addG = ((color >> 8) & 0xFF) << 5, addR = ((color >> 16) & 0xFF) << 5;
        const uint16_t* h0 = hd->px + static_cast<size_t>(2 * y) * static_cast<size_t>(hd->w) + static_cast<size_t>(2 * x);
        const uint16_t* h1 = h0 + hd->w;
        uint16_t* d0 = dst;
        auto* d1 = reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(dst) + dstPitch);
        const int n = count * 2;
        for (int row = 0; row < 2; ++row) {
            const uint16_t* h = row ? h1 : h0;
            uint16_t* d = row ? d1 : d0;
            for (int i = 0; i < n; ++i) {
                const uint32_t px = h[i];
                uint32_t b = static_cast<uint32_t>((static_cast<int32_t>((px & 0x1F) << 10) * mul) >> 16) + addB;
                uint32_t g = static_cast<uint32_t>((static_cast<int32_t>(((px >> 5) & 0x3F) << 9) * mul) >> 16) + addG;
                uint32_t r = static_cast<uint32_t>((static_cast<int32_t>((px >> 11) << 10) * mul) >> 16) + addR;
                if (b > 0xFFFF) b = 0xFFFF;
                if (g > 0xFFFF) g = 0xFFFF;
                if (r > 0xFFFF) r = 0xFFFF;
                b &= mB; g &= mG; r &= mR;
                d[i] = static_cast<uint16_t>(((b >> 8) & 0xFF) | (g >> 2) | (r << 3));
            }
        }
    }

    // ---------------------------------------------------------------------------------------
    // Sprites with a ground shadow, 2x zoom. The game's blended blit (0x005EC5F0) magnifies
    // such a sprite itself, with 0x006220EC or - with colour key 0x07C0 - 0x0062261C:
    //     (src, dst, dstWidth, dstHeight, srcPitch, dstPitch, factor, marker), cdecl
    // A source pixel equal to `marker` darkens the destination (dst * factor / 256), any other
    // is copied; each source pixel fills a 2x2 block. For the shadow the original reads ONE
    // destination pixel and writes its darkened value to all four - on an HD background every
    // shadow turned the ground beneath it back into 2x2 blocks. Here each of the four
    // destination pixels is darkened by itself; everything else is unchanged.
    using PFN_Sprite2x = void(__cdecl*)(const uint16_t* src, uint16_t* dst, uint32_t dstW, uint32_t dstH,
                                        int srcPitch, int dstPitch, uint32_t factor, uint32_t marker);
    PFN_Sprite2x o_Sprite2xKeyed = nullptr;
    PFN_Sprite2x o_Sprite2xPlain = nullptr;

    inline uint16_t Darken565(uint32_t px, int32_t mul, uint32_t mB, uint32_t mG, uint32_t mR) {
        const uint32_t b = static_cast<uint32_t>((static_cast<int32_t>((px & 0x1F) << 10) * mul) >> 16) & mB;
        const uint32_t g = static_cast<uint32_t>((static_cast<int32_t>(((px >> 5) & 0x3F) << 9) * mul) >> 16) & mG;
        const uint32_t r = static_cast<uint32_t>((static_cast<int32_t>((px >> 11) << 10) * mul) >> 16) & mR;
        return static_cast<uint16_t>(((b >> 8) & 0xFF) | (g >> 2) | (r << 3));
    }

    // Filtered version (SmoothSprites=1): needs to know the two surfaces, which the routine is
    // not told - they are the ones the game locked just before calling it.
    template <bool Keyed>
    bool Sprite2xSmooth(const uint16_t* src, uint16_t* dst, uint32_t cols, uint32_t rows,
                        int srcPitch, int dstPitch, uint32_t factor, uint32_t marker) {
        Buf sb{}, db{};
        if (!FindLockedBuf(src, sb) || !FindLockedBuf(dst, db) || sb.bits == db.bits ||
            sb.pitch != srcPitch || db.pitch != dstPitch) {
            return false;
        }
        const size_t sOff = static_cast<size_t>(reinterpret_cast<const uint8_t*>(src) - sb.bits);
        const size_t dOff = static_cast<size_t>(reinterpret_cast<const uint8_t*>(dst) - db.bits);
        RECT sr{}, dr{};
        sr.left = static_cast<LONG>((sOff % static_cast<size_t>(sb.pitch)) / 2);
        sr.top = static_cast<LONG>(sOff / static_cast<size_t>(sb.pitch));
        sr.right = sr.left + static_cast<LONG>(cols);
        sr.bottom = sr.top + static_cast<LONG>(rows);
        dr.left = static_cast<LONG>((dOff % static_cast<size_t>(db.pitch)) / 2);
        dr.top = static_cast<LONG>(dOff / static_cast<size_t>(db.pitch));
        dr.right = dr.left + static_cast<LONG>(cols) * 2;
        dr.bottom = dr.top + static_cast<LONG>(rows) * 2;
        if (sr.right > sb.w || sr.bottom > sb.h || dr.right > db.w || dr.bottom > db.h) {
            return false;
        }
        SmoothSprite2x16(db, dr, sb, sr, dr, Keyed ? 0x07C0u : kNoColor, marker, factor,
                         reinterpret_cast<const uint16_t*>(kAddrSpanMask), false, false);
        return true;
    }

    template <bool Keyed>
    void Sprite2x(const uint16_t* src, uint16_t* dst, uint32_t dstW, uint32_t dstH,
                  int srcPitch, int dstPitch, uint32_t factor, uint32_t marker) {
        const uint32_t cols = dstW >> 1, rows = dstH >> 1;
        if (cols == 0 || rows == 0) return;
        if (Sprites::TryShadowBlit(src, dst, cols, rows, srcPitch, dstPitch, factor, marker, Keyed)) {
            return; // the upscaled frame was drawn
        }
        if (Sprites::MissActive()) { // test harness: what is still magnified
            Buf sb{};
            if (FindLockedBuf(src, sb)) Sprites::NoteFallback(sb.bits, sb.w, sb.h, 1);
            else Sprites::NoteFallback(nullptr, static_cast<int>(cols), static_cast<int>(rows), 1);
        }
        if (SherwoodConfig::Get().smoothSprites && g_variant >= 0 &&  // "off" = the game's own look
            Sprite2xSmooth<Keyed>(src, dst, cols, rows, srcPitch, dstPitch, factor, marker)) {
            return;
        }
        const auto* mask = reinterpret_cast<const uint16_t*>(kAddrSpanMask);
        const uint32_t mB = mask[0], mG = mask[1], mR = mask[2];
        const int32_t mul = static_cast<int16_t>(static_cast<uint16_t>(factor << 6));
        for (uint32_t y = 0; y < rows; ++y) {
            uint16_t* d0 = dst;
            auto* d1 = reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(dst) + dstPitch);
            for (uint32_t x = 0; x < cols; ++x, d0 += 2, d1 += 2) {
                const uint32_t p = src[x];
                if (Keyed && p == 0x07C0) continue;
                if (p == marker) {
                    d0[0] = Darken565(d0[0], mul, mB, mG, mR);
                    d0[1] = Darken565(d0[1], mul, mB, mG, mR);
                    d1[0] = Darken565(d1[0], mul, mB, mG, mR);
                    d1[1] = Darken565(d1[1], mul, mB, mG, mR);
                } else {
                    const auto c = static_cast<uint16_t>(p);
                    d0[0] = c; d0[1] = c; d1[0] = c; d1[1] = c;
                }
            }
            src = reinterpret_cast<const uint16_t*>(reinterpret_cast<const uint8_t*>(src) + srcPitch);
            dst = reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(dst) + 2 * dstPitch);
        }
    }

    void __cdecl Hook_Sprite2xKeyed(const uint16_t* src, uint16_t* dst, uint32_t dstW, uint32_t dstH,
                                    int srcPitch, int dstPitch, uint32_t factor, uint32_t marker) {
        if (g_debugFlags.load(std::memory_order_relaxed) & 1u) return;
        Sprite2x<true>(src, dst, dstW, dstH, srcPitch, dstPitch, factor, marker);
    }

    void __cdecl Hook_Sprite2xPlain(const uint16_t* src, uint16_t* dst, uint32_t dstW, uint32_t dstH,
                                    int srcPitch, int dstPitch, uint32_t factor, uint32_t marker) {
        if (g_debugFlags.load(std::memory_order_relaxed) & 2u) return;
        Sprite2x<false>(src, dst, dstW, dstH, srcPitch, dstPitch, factor, marker);
    }

    // ---------------------------------------------------------------------------------------
    // Stretched alpha blit of the game's general blitter (its mode 0x28201: 16 bits, stretched,
    // colour key, alpha from the green channel of a second picture; 0x005E8B10, thiscall):
    //     (src, dst, alphaSrc, key, dstWidth, dstHeight, srcPitch, dstPitch, scaleX, scaleY)
    // This draws the ring under the selected character and light effects. The original blends
    // one destination pixel per source pixel and repeats the result (2x2 blocks at the 2x zoom).
    using PFN_BlendStretch = uint32_t(__fastcall*)(void* self, void* edx, const uint16_t* src, uint16_t* dst, const uint16_t* alphaSrc,
                                                   uint32_t key, uint32_t dstW, uint32_t dstH, uint32_t srcPitch, uint32_t dstPitch,
                                                   float scaleX, float scaleY);
    PFN_BlendStretch o_BlendStretch = nullptr;

    // The 2x case per destination pixel. The game blends ONE destination pixel per source pixel,
    // repeats the result over the 2x2 block and copies whole rows down - so whatever lies under
    // the picture's transparent parts turns into 2x2 blocks as well: under every letter of a
    // tooltip (the interface's text is drawn this way at the 2x interface scale). With the smooth
    // zoom those blocks differed from the world copy, were taken for interface and showed the
    // world at its own size between magnified world pixels - "dotted" boxes under the text
    // (user report 2026-11-15). Same arithmetic as the game (0x005E8B10), for every pixel; where
    // the alpha picture is 0 the pixel stays as it is (the game's formula gives the same).
    void BlendStretch2x(const uint16_t* src, uint16_t* dst, const uint16_t* alphaSrc, uint32_t key,
                        uint32_t cols, uint32_t rows, uint32_t srcPitch, uint32_t dstPitch) {
        for (uint32_t y = 0; y < rows * 2; ++y) {
            const size_t so = static_cast<size_t>(y >> 1) * srcPitch;
            const auto* s = reinterpret_cast<const uint16_t*>(reinterpret_cast<const uint8_t*>(src) + so);
            const auto* a = reinterpret_cast<const uint16_t*>(reinterpret_cast<const uint8_t*>(alphaSrc) + so);
            auto* out = reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(dst) + static_cast<size_t>(y) * dstPitch);
            for (uint32_t x = 0; x < cols * 2; ++x) {
                const uint32_t sp = s[x >> 1];
                if (sp == key) continue;
                uint32_t al = (static_cast<uint32_t>(a[x >> 1]) >> 2) & 0x1F0;
                if (al == 0) continue;
                if (al > 0xFF) al = 0xFF;
                const uint32_t ia = 0x100 - al, d = out[x];
                const uint32_t r = (((((sp >> 8) & 0xF8) * al) >> 8) & 0xF8) + (((((d >> 8) & 0xF8) * ia) >> 8) & 0xF8);
                const uint32_t g = (((((sp >> 3) & 0xFC) * al) >> 8) & 0xFC) + (((((d >> 3) & 0xFC) * ia) >> 8) & 0xFC);
                const uint32_t b = ((((sp & 0x1F) << 3) * al) >> 11) + ((((d & 0x1F) << 3) * ia) >> 11);
                out[x] = static_cast<uint16_t>((r << 8) + (g << 3) + b);
            }
        }
    }

    uint32_t __fastcall Hook_BlendStretch(void* self, void* edx, const uint16_t* src, uint16_t* dst, const uint16_t* alphaSrc,
                                          uint32_t key, uint32_t dstW, uint32_t dstH, uint32_t srcPitch, uint32_t dstPitch,
                                          float scaleX, float scaleY) {
        if (scaleX == 2.0f && scaleY == 2.0f && dstW >= 2 && dstH >= 2) {
            if (alphaSrc == src &&
                Sprites::TryBlendBlit(src, dst, dstW / 2, dstH / 2, static_cast<int>(srcPitch), static_cast<int>(dstPitch), key & 0xFFFF)) {
                return 1; // the upscaled frame was drawn
            }
            if (Sprites::MissActive()) { // test harness: what is still magnified
                Buf sb{};
                if (FindLockedBuf(src, sb)) Sprites::NoteFallback(sb.bits, sb.w, sb.h, 3);
                else Sprites::NoteFallback(nullptr, static_cast<int>(dstW / 2), static_cast<int>(dstH / 2), 3);
            }
            if (src && dst && alphaSrc && !((srcPitch | dstPitch) & 1)) {
                if (TraceActive()) {
                    char text[120];
                    std::snprintf(text, sizeof(text), "BLEND2X %ux%u alpha=%s key=%04X", dstW, dstH,
                                  alphaSrc == src ? "own" : "other", key & 0xFFFF);
                    TraceMark(text);
                }
                BlendStretch2x(src, dst, alphaSrc, key & 0xFFFF, dstW / 2, dstH / 2, srcPitch, dstPitch);
                return 1;
            }
        }
        return o_BlendStretch(self, edx, src, dst, alphaSrc, key, dstW, dstH, srcPitch, dstPitch, scaleX, scaleY);
    }

    // ---------------------------------------------------------------------------------------
    // Text (and other pictures with an alpha picture) magnified 2x: the general blitter's mode
    // 0x28401 at the factor 2 (0x005E84E0) calls this MMX routine, cdecl:
    //     (src, dst, alpha, dstW, dstH, srcPitch, dstPitch, alphaPitch)
    // Per source pixel that is not 0x07C0: weight w = 2 * (blue of the alpha picture) of 64,
    // out = (src * w + dst * (64 - w)) per channel (5-6-5 spread to bytes, the game's channel
    // masks at 0x006C11C8), computed with the destination pixel at the block's top-left and
    // written as the whole 2x2 block. The interface's text is drawn this way at the 2x
    // interface scale: under every letter's cell the picture (alpha 0 there) turned the world
    // into 2x2 blocks. With the smooth zoom those block pixels differed from the world copy,
    // were taken for interface and showed the world at its own size between magnified world
    // pixels - "dotted" boxes under tooltips (user report 2026-11-15). Here the same arithmetic
    // per destination pixel; where w = 0 the pixel stays as it is (the formula gives the same).
    using PFN_AlphaText2x = void(__cdecl*)(const uint16_t* src, uint16_t* dst, const uint16_t* alpha, uint32_t dstW,
                                           uint32_t dstH, int srcPitch, int dstPitch, int alphaPitch);
    PFN_AlphaText2x o_AlphaText2x = nullptr;

    void __cdecl Hook_AlphaText2x(const uint16_t* src, uint16_t* dst, const uint16_t* alpha, uint32_t dstW, uint32_t dstH,
                                  int srcPitch, int dstPitch, int alphaPitch) {
        const uint32_t cols = dstW >> 1, rows = dstH >> 1;
        if (!cols || !rows || !src || !dst || !alpha) return;
        // the glyph of an HD font, when the strip has one (ddm_hd_font.h)
        if (Font::AlphaGlyph2x(src, alpha, dst, static_cast<int>(dstW), static_cast<int>(dstH), dstPitch)) return;
        const auto* mask = reinterpret_cast<const uint16_t*>(kAddrSpanMask);
        const uint32_t mB = mask[0], mG = mask[1], mR = mask[2];
        for (uint32_t y = 0; y < rows * 2; ++y) {
            const auto* s = reinterpret_cast<const uint16_t*>(reinterpret_cast<const uint8_t*>(src) +
                                                              static_cast<ptrdiff_t>(y >> 1) * srcPitch);
            const auto* a = reinterpret_cast<const uint16_t*>(reinterpret_cast<const uint8_t*>(alpha) +
                                                              static_cast<ptrdiff_t>(y >> 1) * alphaPitch);
            auto* out = reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(dst) + static_cast<ptrdiff_t>(y) * dstPitch);
            for (uint32_t x = 0; x < cols * 2; ++x) {
                const uint32_t sp = s[x >> 1];
                if (sp == 0x07C0) continue;
                const uint32_t w = (a[x >> 1] & 0x1Fu) * 2;
                if (w == 0) continue;
                const uint32_t iw = 64 - w, d = out[x];
                const uint32_t b = (((((sp & 0x1F) << 2) * w + ((d & 0x1F) << 2) * iw) & mB) >> 8) & 0xFF;
                const uint32_t g = (((((sp >> 5) & 0x3F) << 1) * w + (((d >> 5) & 0x3F) << 1) * iw) & mG) >> 2;
                const uint32_t r = ((((sp >> 11) << 2) * w + ((d >> 11) << 2) * iw) & mR) << 3;
                out[x] = static_cast<uint16_t>(r | g | b);
            }
        }
    }

    // ---------------------------------------------------------------------------------------
    // Menus and dialogs with the menu zoom (ddm_hd_patch.h) - the game's 1:1 routines:
    //   0x00622EB4 text with an alpha picture, cdecl (src, dst, alpha, w, h, ...) - the 1:1 brother
    //              of 0x00622FD4 (the general blitter's mode 0x20401 via 0x005E8410)
    //   0x00622550 / 0x00622028 pictures with a shadow marker, keyed (0x07C0) / plain, cdecl
    //              (src, dst, w, h, srcSkip, dstSkip, factor, marker) - the 1:1 brothers of
    //              0x0062261C / 0x006220EC (BlendBlit 0x005EC5F0: buttons, the cursor)
    //   0x005F1700 the pointer draws the cursor (thiscall): its pictures are not patches
    // Each is left to the game; what it drew is drawn again in HD into the surface's shadow.
    using PFN_AlphaText1x = void(__cdecl*)(const uint16_t* src, uint16_t* dst, const uint16_t* alpha, uint32_t w, uint32_t h,
                                           int a6, int a7, int a8);
    PFN_AlphaText1x o_AlphaText1x = nullptr;

    void __cdecl Hook_AlphaText1x(const uint16_t* src, uint16_t* dst, const uint16_t* alpha, uint32_t w, uint32_t h,
                                  int a6, int a7, int a8) {
        static Patch::Op op;    // main thread only
        bool patch = false;
        if (w && h && w < 4096 && h < 4096 && Diag::IsMainThread()) {
            op.Clear();
            patch = Patch::BeginAlphaGlyph(src, dst, alpha, static_cast<int>(w), static_cast<int>(h), op);
        }
        o_AlphaText1x(src, dst, alpha, w, h, a6, a7, a8);
        if (patch) Patch::End(op);
    }

    using PFN_Shadow1x = void(__cdecl*)(const uint16_t* src, uint16_t* dst, uint32_t w, uint32_t h, int srcSkip, int dstSkip,
                                        uint32_t factor, uint32_t marker);
    PFN_Shadow1x o_Shadow1xKeyed = nullptr;
    PFN_Shadow1x o_Shadow1xPlain = nullptr;

    template <bool Keyed>
    void Shadow1x(const uint16_t* src, uint16_t* dst, uint32_t w, uint32_t h, int srcSkip, int dstSkip, uint32_t factor, uint32_t marker) {
        static Patch::Op op;    // main thread only
        bool patch = false;
        if (w && h && w < 4096 && h < 4096 && Diag::IsMainThread()) {
            op.Clear();
            patch = Patch::BeginShadowPicture(src, dst, static_cast<int>(w), static_cast<int>(h), factor, marker, Keyed, op);
        }
        (Keyed ? o_Shadow1xKeyed : o_Shadow1xPlain)(src, dst, w, h, srcSkip, dstSkip, factor, marker);
        if (patch) Patch::End(op);
    }
    void __cdecl Hook_Shadow1xKeyed(const uint16_t* src, uint16_t* dst, uint32_t w, uint32_t h, int srcSkip, int dstSkip,
                                    uint32_t factor, uint32_t marker) {
        Shadow1x<true>(src, dst, w, h, srcSkip, dstSkip, factor, marker);
    }
    void __cdecl Hook_Shadow1xPlain(const uint16_t* src, uint16_t* dst, uint32_t w, uint32_t h, int srcSkip, int dstSkip,
                                    uint32_t factor, uint32_t marker) {
        Shadow1x<false>(src, dst, w, h, srcSkip, dstSkip, factor, marker);
    }

    // ---------------------------------------------------------------------------------------
    // The in-game menu's backdrop (2026-11-15, "lekki lag zwiecha" when Esc opens the menu): the game
    // colours the whole picture of the level behind a dialog - 0x0053B330, cdecl (surface id, hue,
    // factor), returns al: per pixel RGB565 -> floats (red * 2/32, green /64, blue /32, all * factor),
    // 0x0053CBC0 (RGB -> HSV), 0x0053CA30 (HSV -> RGB with the hue replaced), three _ftol (x32, x64,
    // x32). At 2560x1440 that is 3.7 million times two calls and three FPU mode switches: 290 ms of
    // the ~400 ms the menu took to appear. The result depends on the pixel alone: a table of all 65536
    // colours is made per (hue, factor) by running the same steps - the game's own helpers, the x87
    // arithmetic as in the original, on the same FPU settings: bit-identical - and the surface is
    // mapped through it (~10 ms).
    using PFN_Colorize = uint8_t(__cdecl*)(int id, uint32_t hue, float factor);
    PFN_Colorize o_Colorize = nullptr;
    using PFN_RgbToHsv = void(__cdecl*)(float r, float g, float b, float* h, float* s, float* v);
    using PFN_HsvToRgb = void(__cdecl*)(float h, float s, float v, float* r, float* g, float* b);
    using PFN_SurfLock = uint8_t(__fastcall*)(void* mgr, void* edx, int id, void* info, int flags);
    using PFN_SurfUnlock = void(__fastcall*)(void* mgr, void* edx, void* info);
    constexpr uintptr_t kAddrSurfaceManager = 0x006C11D0;

    // v * c * k (* 2), x87 as the original: the product stays in the FPU until the store
    float X87Scale(int v, float c, float k, int twice) {
        float scaled;
        __asm {
            fild v
            fmul c
            fmul k
            cmp twice, 0
            je single
            fadd st(0), st(0)
        single:
            fstp scaled
        }
        return scaled;
    }

    // _ftol(v * c) with the game's own _ftol (0x00642B7C: truncation)
    int X87Ftol(float v, float c) {
        int whole;
        static const uintptr_t ftol = 0x00642B7C;
        __asm {
            fld v
            fmul c
            call dword ptr [ftol]
            mov whole, eax
        }
        return whole;
    }

    const uint16_t* ColorizeTable(uint32_t hue, float factor) {
        struct Table { uint32_t hue = 0xFFFFFFFFu; float factor = -1.0f; unsigned use = 0; std::vector<uint16_t> map; };
        static Table tables[2];
        static unsigned use = 0;
        for (Table& t : tables) {
            if (t.hue == hue && t.factor == factor && t.map.size() == 65536) {
                t.use = ++use;
                return t.map.data();
            }
        }
        Table& t = tables[0].use <= tables[1].use ? tables[0] : tables[1];
        const auto rgbToHsv = reinterpret_cast<PFN_RgbToHsv>(0x0053CBC0);
        const auto hsvToRgb = reinterpret_cast<PFN_HsvToRgb>(0x0053CA30);
        int hv = static_cast<int>(hue & 0xFFFF);
        float hueF;
        __asm {
            fild hv
            fstp hueF
        }
        t.map.resize(65536);
        for (uint32_t p = 0; p < 65536; ++p) {
            const float r = X87Scale(static_cast<int>(p >> 11), 0.03125f, factor, 1);
            const float g = X87Scale(static_cast<int>((p >> 5) & 0x3F), 0.015625f, factor, 0);
            const float b = X87Scale(static_cast<int>(p & 0x1F), 0.03125f, factor, 0);
            float h = 0, s = 0, v = 0, ro = 0, go = 0, bo = 0;
            rgbToHsv(r, g, b, &h, &s, &v);
            hsvToRgb(hueF, s, v, &ro, &go, &bo);
            const int G = X87Ftol(go, 64.0f) & 0x3F;
            const int R = X87Ftol(ro, 32.0f);
            const int B = X87Ftol(bo, 32.0f) & 0x1F;
            t.map[p] = static_cast<uint16_t>((R << 11) | (G << 5) | B);
        }
        t.hue = hue;
        t.factor = factor;
        t.use = ++use;
        return t.map.data();
    }

    // the last colouring (the HD menu's dialog backdrop compares the level with it - LastBackdropColouring)
    uint32_t g_colourHue = 0;
    float    g_colourFactor = 0.0f;
    bool     g_colourKnown = false;

    uint8_t __cdecl Hook_Colorize(int id, uint32_t hue, float factor) {
        auto* mgr = *reinterpret_cast<uint8_t* const*>(kAddrSurfaceManager);
        if (!mgr || *reinterpret_cast<const uint16_t*>(mgr + 0x10C) != 16 || !Diag::IsMainThread()) {
            g_colourKnown = false;
            return o_Colorize(id, hue, factor);     // 15-bit colour: the game's own way
        }
        g_colourHue = hue & 0xFFFF;
        g_colourFactor = factor;
        g_colourKnown = true;
        LARGE_INTEGER q0{}, q1{}, qf{};
        QueryPerformanceCounter(&q0);
        alignas(8) uint8_t info[64] = {};           // the game's lock record (24 bytes used)
        const auto lock = reinterpret_cast<PFN_SurfLock>(0x005E5A00);
        const auto unlock = reinterpret_cast<PFN_SurfUnlock>(0x005E5EA0);
        static bool verified = false;
        if (!verified && TestHarness::Enabled()) {
            // test mode, once: the table against the game's own routine on the same picture
            verified = true;
            if (!lock(mgr, nullptr, id, info, 0)) return 0;
            const int w = *reinterpret_cast<const uint16_t*>(info + 4), h = *reinterpret_cast<const uint16_t*>(info + 6);
            const int pitch = *reinterpret_cast<const int*>(info + 8);
            const uint8_t* bits = *reinterpret_cast<uint8_t* const*>(info + 0x10);
            std::vector<uint16_t> expect(static_cast<size_t>(w) * h);
            const uint16_t* map = ColorizeTable(hue & 0xFFFF, factor);
            for (int y = 0; y < h; ++y) {
                const auto* row = reinterpret_cast<const uint16_t*>(bits + static_cast<size_t>(y) * pitch);
                for (int x = 0; x < w; ++x) expect[static_cast<size_t>(y) * w + x] = map[row[x]];
            }
            unlock(mgr, nullptr, info);
            const uint8_t r = o_Colorize(id, hue, factor);
            std::memset(info, 0, sizeof(info));
            if (lock(mgr, nullptr, id, info, 0)) {
                const uint8_t* after = *reinterpret_cast<uint8_t* const*>(info + 0x10);
                size_t diff = 0;
                for (int y = 0; y < h; ++y) {
                    const auto* row = reinterpret_cast<const uint16_t*>(after + static_cast<size_t>(y) * pitch);
                    for (int x = 0; x < w; ++x) diff += row[x] != expect[static_cast<size_t>(y) * w + x] ? 1u : 0u;
                }
                unlock(mgr, nullptr, info);
                Logger::Info("Dialogs: backdrop colouring checked against the game's routine: %zu of %d pixels differ.", diff, w * h);
            }
            return r;
        }
        if (!lock(mgr, nullptr, id, info, 0)) return 0;
        const int w = *reinterpret_cast<const uint16_t*>(info + 4), h = *reinterpret_cast<const uint16_t*>(info + 6);
        const int pitch = *reinterpret_cast<const int*>(info + 8);
        uint8_t* bits = *reinterpret_cast<uint8_t* const*>(info + 0x10);
        if (bits && w > 0 && h > 0 && pitch >= w * 2) {
            const uint16_t* map = ColorizeTable(hue & 0xFFFF, factor);
            for (int y = 0; y < h; ++y) {
                auto* row = reinterpret_cast<uint16_t*>(bits + static_cast<size_t>(y) * pitch);
                for (int x = 0; x < w; ++x) row[x] = map[row[x]];
            }
        }
        unlock(mgr, nullptr, info);
        QueryPerformanceCounter(&q1);
        QueryPerformanceFrequency(&qf);
        Logger::Info("HD menu: the dialog's backdrop coloured (hue %u, x%.2f) %dx%d in %.1f ms.", hue & 0xFFFF, factor, w, h,
                     static_cast<double>(q1.QuadPart - q0.QuadPart) * 1000.0 / static_cast<double>(qf.QuadPart));
        return 1;
    }

    using PFN_DrawPointer = void(__fastcall*)(void* self, void* edx);
    PFN_DrawPointer o_DrawPointer = nullptr;

    void __fastcall Hook_DrawPointer(void* self, void* edx) {
        Patch::SetCursorDrawing(true);
        o_DrawPointer(self, edx);
        Patch::SetCursorDrawing(false);
    }

    // ---------------------------------------------------------------------------------------
    // The game's general blitter (0x005E6900, thiscall, 6 arguments - text, selection rings, light
    // effects ...): with the interface scale x2 it draws the windows' text and pictures twice as large
    // and clipped to the frame (below); in test mode it also writes its arguments into a running draw
    // trace, so that it can be seen which of its many modes draws what.
    using PFN_GeneralBlit = uint32_t(__fastcall*)(void* self, void* edx, int srcId, const float* srcRect, int dstId,
                                                  const float* dstRect, int srcId2, uint32_t flags);
    PFN_GeneralBlit o_GeneralBlit = nullptr;

    void TraceGeneralBlit(void* self, int srcId, const float* srcRect, int dstId, const float* dstRect, int srcId2, uint32_t flags) {
        __try {
            char text[240];
            const uint32_t base = *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(self) + 0x70);
            int n = std::snprintf(text, sizeof(text), "GBLT flags=%08X|%08X src=%d src2=%d dst=%d", flags, base, srcId, srcId2, dstId);
            if (srcRect && n > 0 && n < 200) {
                n += std::snprintf(text + n, sizeof(text) - n, " s=(%.1f,%.1f,%.1f,%.1f)", srcRect[0], srcRect[1], srcRect[2], srcRect[3]);
            }
            if (dstRect && n > 0 && n < 200) {
                std::snprintf(text + n, sizeof(text) - n, " d=(%.1f,%.1f,%.1f,%.1f)", dstRect[0], dstRect[1], dstRect[2], dstRect[3]);
            }
            TraceMark(text);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    uint32_t __fastcall Hook_GeneralBlit(void* self, void* edx, int srcId, const float* srcRect, int dstId,
                                         const float* dstRect, int srcId2, uint32_t flags) {
        if (TraceActive()) TraceGeneralBlit(self, srcId, srcRect, dstId, dstRect, srcId2, flags);
        if (dstId == 0 && dstRect && UiScale::Drawing()) {
            // interface scale: text and other window pictures into the frame, twice as large -
            // the blitter itself stretches when the rectangles differ in size. It does NOT clip
            // (it writes past the frame: crash 2026-11-14 with menu text at the bottom edge), so
            // the rectangles are cut to the frame here, the source by the same share.
            float r[4] = { dstRect[0], dstRect[1], dstRect[2], dstRect[3] };
            UiScale::ScaleRectF(r);
            if (r[2] <= r[0] || r[3] <= r[1]) return 1;     // left out (a dialog's backdrop render)
            int fw = 0, fh = 0;
            WindowManager::GetInternalResolution(fw, fh);
            const bool inside = r[0] >= 0.0f && r[1] >= 0.0f && r[2] <= static_cast<float>(fw) && r[3] <= static_cast<float>(fh);
            if (inside) {
                return o_GeneralBlit(self, edx, srcId, srcRect, dstId, r, srcId2, flags);
            }
            if (!srcRect || r[2] <= r[0] || r[3] <= r[1]) {
                return 1; // partly outside and no source rectangle to cut: left out
            }
            float s[4] = { srcRect[0], srcRect[1], srcRect[2], srcRect[3] };
            const float limit[4] = { 0.0f, 0.0f, static_cast<float>(fw), static_cast<float>(fh) };
            for (int axis = 0; axis < 2; ++axis) {
                const int lo = axis, hi = axis + 2;
                const float len = r[hi] - r[lo], slen = s[hi] - s[lo];
                if (r[lo] < limit[lo]) { s[lo] += (limit[lo] - r[lo]) / len * slen; r[lo] = limit[lo]; }
                if (r[hi] > limit[hi]) { s[hi] -= (r[hi] - limit[hi]) / len * slen; r[hi] = limit[hi]; }
            }
            if (r[2] - r[0] < 1.0f || r[3] - r[1] < 1.0f || s[2] <= s[0] || s[3] <= s[1]) {
                return 1;
            }
            return o_GeneralBlit(self, edx, srcId, s, dstId, r, srcId2, flags);
        }
        return o_GeneralBlit(self, edx, srcId, srcRect, dstId, dstRect, srcId2, flags);
    }

    bool HookRoutine(uintptr_t addr, const uint8_t* prolog, size_t prologLen, void* detour, void** original) {
        return std::memcmp(reinterpret_cast<const void*>(addr), prolog, prologLen) == 0 &&
               MH_CreateHook(reinterpret_cast<LPVOID>(addr), detour, original) == MH_OK &&
               MH_EnableHook(reinterpret_cast<LPVOID>(addr)) == MH_OK;
    }
}

bool VariantOn() {
    return g_variant >= 0;
}

void SetDebugFlags(unsigned flags) {
    g_debugFlags.store(flags);
}

unsigned DebugFlags() {
    return g_debugFlags.load(std::memory_order_relaxed);
}

bool TryBackgroundBlit(const Buf& dst, const RECT& d, const RECT& clip,
                       const Buf& src, const RECT& s, const void* identity,
                       unsigned lockGeneration, unsigned writeGeneration) {
    if (!SherwoodConfig::Get().hdBackgrounds) return false;
    ScanVariants();
    if (g_variant < 0) return false;

    // Look the surface up again when it is another one, was drawn into, or the variant changed.
    // A mere Lock happens every frame (the game reads its map for the water): then the content
    // is only verified now and then.
    bool lookup = identity != g_identity || g_lookupVariant != g_variant || writeGeneration != g_writeGen;
    const DWORD now = GetTickCount();
    if (!lookup && lockGeneration != g_lockGen && now - g_lastVerifyMs >= 500) {
        lookup = true;
    }
    // Another surface magnified 2x that is no level picture: drawn as the game draws it, and the
    // state of the level's HD picture stays - forgetting it here cost the next background blit a
    // new hash and base copy and reset the water loops (code review 2026-11-17).
    if (identity != g_identity && !IsLevelBackground(src)) return false;
    if (lookup) {
        const bool sameSurface = identity == g_identity && g_lookupVariant == g_variant;
        g_lockGen = lockGeneration;
        g_writeGen = writeGeneration;
        g_lastVerifyMs = now;
        if (!IsLevelBackground(src)) {
            g_identity = identity;
            g_lookupVariant = g_variant;
            g_current = nullptr;
            g_srcBits = nullptr;
        } else {
            const uint64_t key = HashPixels(src);
            bool keep = false;
            if (sameSurface && g_current && key != g_lookupKey) {
                // a part of the same level (a building's inside) or another picture altogether?
                const double changed = g_baseMap == g_current ? ChangedShare(src) : -1.0;
                if (changed >= 0.0 && changed < 0.5) {
                    keep = true;
                    Logger::Info("HD: the game %s its level picture (%.1f %% differs from the picture the HD one was made from) - %s.",
                                 key == g_baseKey ? "restored" : "changed part of", changed * 100.0,
                                 key == g_baseKey ? "HD everywhere" : "the changed part is shown from the game's picture, the rest in HD");
                } else {
                    Logger::Warn("HD: the game changed its level picture while playing (key %016llx -> %016llx).",
                                 static_cast<unsigned long long>(g_lookupKey), static_cast<unsigned long long>(key));
                }
            }
            if (!keep && (!sameSurface || key != g_lookupKey || !g_current)) {
                // Another variant for the same level, whose picture the game has changed in part since (the carriage
                // mission draws its carriage into it): the changed picture has no file of its own. The new variant's
                // HD picture by the key of the level's own picture, whose copy stays (2026-11-22: after F6/F7 the
                // carriage mission was shown in the original pixels to its end).
                double share = -1.0;
                if (g_lookupVariant != g_variant && g_baseKey != 0 && key != g_baseKey && (share = ChangedShare(src)) >= 0.0 &&
                    share < 0.5) {
                    g_current = Resolve(g_variant, g_baseKey, src.w, src.h);
                    g_groundAnims.clear();
                    ++g_levelPicturesSerial;
                    g_baseMap = g_current;
                    Logger::Info("HD: variant switched on a level picture the game has changed in part (%.1f %%) - its HD picture "
                                 "by the level's own key %016llx.", share * 100.0, static_cast<unsigned long long>(g_baseKey));
                } else {
                    g_current = Resolve(g_variant, key, src.w, src.h);
                    g_groundAnims.clear(); // another level (or another state of it)
                    TakeBase(src, key, g_current);
                }
            }
            g_identity = identity;
            g_lookupVariant = g_variant;
            g_lookupKey = key;
            g_srcBits = src.bits;
            g_srcPitch = src.pitch;
            g_srcW = src.w;
            g_srcH = src.h;
        }
    }
    const HdMap* hd = g_current;
    if (!hd || !hd->px) {
        g_originValid = false;
        return false;
    }

    // dst pixel (x, y) shows HD pixel (2*s.left + x - d.left, 2*s.top + y - d.top); outside the
    // picture (view larger than the level) it is black, as with the low-resolution source.
    const int offX = 2 * s.left - d.left;
    const int offY = 2 * s.top - d.top;
    g_originX = offX;
    g_originY = offY;
    g_originValid = true;
    // the level picture compared with the one the HD picture was made from (see g_base)
    const bool compare = g_baseMap == hd && src.w == g_srcW && src.h == g_srcH && hd->w == 2 * src.w && hd->h == 2 * src.h &&
                         g_base.size() == static_cast<size_t>(src.w) * src.h;
    for (int y = clip.top; y < clip.bottom; ++y) {
        auto* out = reinterpret_cast<uint16_t*>(dst.bits + static_cast<size_t>(y) * dst.pitch) + clip.left;
        const int hy = y + offY;
        int x0 = clip.left, x1 = clip.right;
        if (hy < 0 || hy >= hd->h) {
            std::memset(out, 0, static_cast<size_t>(x1 - x0) * 2);
            continue;
        }
        const uint16_t* row = hd->px + static_cast<size_t>(hy) * hd->w;
        int hx0 = x0 + offX, hx1 = x1 + offX;
        if (hx0 < 0) {
            const int n = (std::min)(-hx0, x1 - x0);
            std::memset(out, 0, static_cast<size_t>(n) * 2);
            out += n; x0 += n; hx0 += n;
        }
        if (hx1 > hd->w) {
            const int n = (std::min)(hx1 - hd->w, x1 - x0);
            std::memset(out + (x1 - x0 - n), 0, static_cast<size_t>(n) * 2);
            x1 -= n;
        }
        if (x1 <= x0) continue;
        if (!compare) {
            std::memcpy(out, row + hx0, static_cast<size_t>(x1 - x0) * 2);
            continue;
        }
        // level row of this HD row: the game's pixel where its picture differs from the copy
        const int my = hy >> 1;
        const auto* cur = reinterpret_cast<const uint16_t*>(src.bits + static_cast<size_t>(my) * src.pitch);
        const uint16_t* base = g_base.data() + static_cast<size_t>(my) * src.w;
        int hx = hx0;
        while (hx < hx0 + (x1 - x0)) {
            // a run of HD pixels over unchanged level pixels
            int run = hx;
            while (run < hx0 + (x1 - x0) && cur[run >> 1] == base[run >> 1]) ++run;
            if (run > hx) {
                std::memcpy(out + (hx - hx0), row + hx, static_cast<size_t>(run - hx) * 2);
                hx = run;
            }
            // changed level pixels: the game's picture, each pixel twice as large
            while (hx < hx0 + (x1 - x0) && cur[hx >> 1] != base[hx >> 1]) {
                out[hx - hx0] = cur[hx >> 1];
                ++hx;
            }
        }
    }
    return true;
}

namespace {
    // A picture of 4 values per level pixel, magnified 2x with the smooth filter's weights:
    //   overlay: [alpha 0..256, alpha * R5 * 16 / 256, ... G6, ... B5]
    //   else:    [0, R5, G6, B5 differences x 16]
    // The result goes to 4 planes of W x H = 2w x 2h values:
    //   overlay: A = alpha 0..256, then R, G, B = alpha * colour (R5 / G6 / B5): out = (HD * (256 - A) + C) / 256
    //   else:    0, then R, G, B = differences x 256:                            out = HD + D / 256
    // `span` gets per HD row the first and the end column where anything is to be done.
    void Magnify2x(const int16_t* pic, int w, int h, bool overlay, GroundFrames& gf, std::vector<int16_t>& out,
                   std::vector<int16_t>& span) {
        const int W = 2 * w, H = 2 * h;
        gf.W = W;
        gf.H = H;
        const size_t plane = static_cast<size_t>(W) * H;
        out.assign(plane * 4, 0);
        span.assign(static_cast<size_t>(H) * 2, 0);
        const int first = overlay ? 0 : 1;
        for (int j = 0; j < H; ++j) {
            const int k = j >> 1;
            const int ya = (std::max)(0, (std::min)(h - 1, (j & 1) ? k : k - 1));
            const int yb = (std::max)(0, (std::min)(h - 1, ((j & 1) ? k : k - 1) + 1));
            const int wya = (j & 1) ? 3 : 1, wyb = 4 - wya;
            const int16_t* ra = pic + static_cast<size_t>(ya) * w * 4;
            const int16_t* rb = pic + static_cast<size_t>(yb) * w * 4;
            const size_t row = static_cast<size_t>(j) * W;
            int x0 = W, x1 = 0;
            for (int i = 0; i < W; ++i) {
                const int kx = i >> 1;
                const int xa = (std::max)(0, (std::min)(w - 1, (i & 1) ? kx : kx - 1));
                const int xb = (std::max)(0, (std::min)(w - 1, ((i & 1) ? kx : kx - 1) + 1));
                const int wxa = (i & 1) ? 3 : 1, wxb = 4 - wxa;
                const int16_t* p00 = ra + xa * 4; const int16_t* p01 = ra + xb * 4;
                const int16_t* p10 = rb + xa * 4; const int16_t* p11 = rb + xb * 4;
                bool any = false;
                for (int n = 0; n < 4; ++n) {
                    int v = wya * (wxa * p00[n] + wxb * p01[n]) + wyb * (wxa * p10[n] + wxb * p11[n]);   // x 16
                    if (overlay && n == 0) v = (v + 8) >> 4;     // alpha 0..256
                    out[plane * n + row + i] = static_cast<int16_t>(v);
                    if (n >= first && v != 0) any = true;
                }
                if (any) { x0 = (std::min)(x0, i); x1 = i + 1; }
            }
            span[static_cast<size_t>(j) * 2] = static_cast<int16_t>(x0 < x1 ? x0 : 0);
            span[static_cast<size_t>(j) * 2 + 1] = static_cast<int16_t>(x0 < x1 ? x1 : 0);
        }
    }

    // The magnified picture onto the HD picture already in `dst` (rect `d`, visible part `clip`),
    // cross-faded from `prev` (t = 0) to `cur` (t = 256): overlays blended, differences added.
    // Overlays: SSE2, 8 pixels at a time (two smoke columns cost 0.5 ms a frame in plain code).
    void ApplyGround2x(const Buf& dst, const RECT& d, const RECT& clip, const GroundFrames& gf, int t, bool overlay) {
        const int W = gf.W, H = gf.H;
        const size_t plane = static_cast<size_t>(W) * H;
        if (W <= 0 || gf.cur.size() != plane * 4) return;
        const bool fade = t < 256 && gf.hasPrev && gf.prev.size() == gf.cur.size() && gf.prevSpan.size() == gf.curSpan.size();
        const __m128i tv = _mm_set1_epi16(static_cast<short>((t < 0 ? 0 : t) << 7));
        const __m128i k256 = _mm_set1_epi16(256), k128 = _mm_set1_epi16(128);
        const __m128i m6 = _mm_set1_epi16(63), m5 = _mm_set1_epi16(31);
        for (int y = clip.top; y < clip.bottom; ++y) {
            const int j = y - d.top;
            if (j < 0 || j >= H) continue;
            int x0 = gf.curSpan[static_cast<size_t>(j) * 2], x1 = gf.curSpan[static_cast<size_t>(j) * 2 + 1];
            if (fade) {
                const int p0 = gf.prevSpan[static_cast<size_t>(j) * 2], p1 = gf.prevSpan[static_cast<size_t>(j) * 2 + 1];
                if (p1 > p0) {
                    if (x1 > x0) { x0 = (std::min)(x0, p0); x1 = (std::max)(x1, p1); }
                    else { x0 = p0; x1 = p1; }
                }
            }
            x0 = (std::max)(x0, static_cast<int>(clip.left - d.left));
            x1 = (std::min)(x1, static_cast<int>(clip.right - d.left));
            if (x1 <= x0) continue;
            const size_t row = static_cast<size_t>(j) * W;
            const int16_t* c[4] = { &gf.cur[row], &gf.cur[plane + row], &gf.cur[2 * plane + row], &gf.cur[3 * plane + row] };
            const int16_t* p[4] = { nullptr, nullptr, nullptr, nullptr };
            if (fade) for (int n = 0; n < 4; ++n) p[n] = &gf.prev[plane * n + row];
            auto* out = reinterpret_cast<uint16_t*>(dst.bits + static_cast<size_t>(y) * dst.pitch) + d.left;
            int i = x0;
            if (overlay) {
                for (; i + 8 <= x1; i += 8) {
                    __m128i v[4];
                    for (int n = 0; n < 4; ++n) {
                        v[n] = _mm_loadu_si128(reinterpret_cast<const __m128i*>(c[n] + i));
                        if (fade) {
                            const __m128i pv = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p[n] + i));
                            // prev + (cur - prev) * t / 256 (mulhi by t*128, then x2)
                            v[n] = _mm_add_epi16(pv, _mm_slli_epi16(_mm_mulhi_epi16(_mm_sub_epi16(v[n], pv), tv), 1));
                        }
                    }
                    const __m128i hp = _mm_loadu_si128(reinterpret_cast<const __m128i*>(out + i));
                    const __m128i inv = _mm_sub_epi16(k256, v[0]);
                    const __m128i r = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(_mm_mullo_epi16(_mm_srli_epi16(hp, 11), inv), v[1]), k128), 8);
                    const __m128i g = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(
                                          _mm_mullo_epi16(_mm_and_si128(_mm_srli_epi16(hp, 5), m6), inv), v[2]), k128), 8);
                    const __m128i b = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(_mm_mullo_epi16(_mm_and_si128(hp, m5), inv), v[3]), k128), 8);
                    const __m128i px = _mm_or_si128(_mm_or_si128(_mm_slli_epi16(_mm_min_epi16(r, m5), 11),
                                                                 _mm_slli_epi16(_mm_min_epi16(g, m6), 5)), _mm_min_epi16(b, m5));
                    _mm_storeu_si128(reinterpret_cast<__m128i*>(out + i), px);
                }
            }
            static const int kMax[3] = { 31, 63, 31 };
            for (; i < x1; ++i) {
                int v[4];
                for (int n = 0; n < 4; ++n) v[n] = fade ? p[n][i] + (((c[n][i] - p[n][i]) * t) >> 8) : c[n][i];
                const uint32_t px = out[i];
                int col[3] = { static_cast<int>(px >> 11), static_cast<int>((px >> 5) & 63), static_cast<int>(px & 31) };
                for (int n = 0; n < 3; ++n) {
                    col[n] = overlay ? (col[n] * (256 - v[0]) + v[n + 1] + 128) >> 8
                                     : col[n] + (v[n + 1] + (v[n + 1] >= 0 ? 128 : -128)) / 256;
                    col[n] = (std::max)(0, (std::min)(kMax[n], col[n]));
                }
                out[i] = static_cast<uint16_t>((col[0] << 11) | (col[1] << 5) | col[2]);
            }
        }
    }

    uint64_t HashFrame(const Buf& src) {
        uint64_t hsh = 1469598103934665603ull;
        for (int y = 0; y < src.h; ++y) {
            const auto* p = reinterpret_cast<const uint32_t*>(src.bits + static_cast<size_t>(y) * src.pitch);
            for (int x = 0; x < src.w / 2; ++x) hsh = (hsh ^ p[x]) * 1099511628211ull;
        }
        return hsh;
    }

    bool MakeGroundDifference(GroundAnim& anim, const Buf& src, int mx, int my, int16_t* diff);

    // ---------------------------------------------------------------- animated HD water
    // The game's water animation holds no waves - only compression shimmer (2026-11-15,
    // tools\hd\water_motion_study.py). Here the still water of the HD picture is moved: a few
    // slow wave trains, periodic in time (a seamless loop), displace the SMOOTH part of the
    // picture by up to a few pixels - reflections of trees and sky wobble - while its finest
    // texture stays in place (no detail lost by resampling: 99 % kept, against 52..77 % when the
    // whole picture is resampled); the light tilts a little with the waves (light from the top
    // left) and a faint glint where they face it. The vertical wavelength is halved (isometric
    // view). The water is where the game's sprite is opaque; the effect fades in over kWaterEdge
    // HD pixels from its edge. Prototype with the same arithmetic: tools\hd\water_anim_proto.py
    // (presets "waves" = HDWater=2, "waves_strong" = HDWater=3).
    // The loop (kWaterFrames frames) is made once per water by a thread of its own (about half a
    // second for a pond; until then and while fading in, the still HD water shows); drawing only
    // cross-fades two ready frames over the water's pixels.
    constexpr int    kWaterFrames = 24;
    constexpr double kWaterLoopSeconds = 4.0;
    constexpr int    kWaterEdge = 7;
    constexpr float  kWaterSplitSigma = 1.2f;
    constexpr double kWaterFadeInSeconds = 0.6;
    constexpr size_t kWaterBudgetBytes = 40u << 20;

    struct Wave { float lambda, angle, amp, cycles; };
    struct WaterLook { Wave waves[4]; float shade, glint; };
    const WaterLook kWaterLooks[2] = {
        { { { 64, 20, 0.85f, 1 }, { 41, 110, 0.60f, 2 }, { 27, 200, 0.42f, 2 }, { 17, 290, 0.26f, 3 } }, 0.07f, 0.10f },
        { { { 56, 25, 1.20f, 1 }, { 36, 115, 0.85f, 2 }, { 23, 205, 0.55f, 3 }, { 15, 295, 0.35f, 3 } }, 0.09f, 0.16f },
    };

    SRWLOCK            g_waterLock = SRWLOCK_INIT;
    CONDITION_VARIABLE g_waterWake = CONDITION_VARIABLE_INIT;
    std::deque<std::shared_ptr<WaterAnim>> g_waterQueue;    // under g_waterLock
    bool g_waterThread = false;                             // main thread
    std::vector<std::shared_ptr<WaterAnim>> g_waters;       // main thread: everything made, for the budget
    uint64_t g_waterUse = 0;

    inline float CubicW(float f, int tap) {                 // Catmull-Rom weights, taps -1 .. 2
        const float f2 = f * f, f3 = f2 * f;
        switch (tap) {
            case 0:  return -0.5f * f3 + f2 - 0.5f * f;
            case 1:  return 1.5f * f3 - 2.5f * f2 + 1.0f;
            case 2:  return -1.5f * f3 + 2.0f * f2 + 0.5f * f;
            default: return 0.5f * f3 - 0.5f * f2;
        }
    }

    bool BuildWater(WaterAnim& wa) {
        const int W = wa.W, H = wa.H;
        const size_t n = static_cast<size_t>(W) * H;
        if (W <= 0 || H <= 0 || wa.hd.size() != n || wa.mask.size() != n) return false;
        wa.runs.clear();
        size_t off = 0;
        for (int y = 0; y < H; ++y) {
            int x = 0;
            while (x < W) {
                while (x < W && !wa.mask[static_cast<size_t>(y) * W + x]) ++x;
                if (x >= W) break;
                const int x0 = x;
                while (x < W && wa.mask[static_cast<size_t>(y) * W + x]) ++x;
                wa.runs.push_back({ y, x0, x, off });
                off += static_cast<size_t>(x - x0);
            }
        }
        wa.pixels = off;
        if (!off) return false;

        // the picture on the 0..255 scale (as RGB565 expands), its smooth part (Gaussian) and the rest
        std::vector<float> img(n * 3), tmp(n * 3), low(n * 3);
        for (size_t i = 0; i < n; ++i) {
            const uint32_t p = wa.hd[i];
            const uint32_t r = p >> 11, g = (p >> 5) & 63, b = p & 31;
            img[i * 3 + 0] = static_cast<float>((r << 3) | (r >> 2));
            img[i * 3 + 1] = static_cast<float>((g << 2) | (g >> 4));
            img[i * 3 + 2] = static_cast<float>((b << 3) | (b >> 2));
        }
        const int R = static_cast<int>(std::ceil(3.0f * kWaterSplitSigma));
        std::vector<float> k(static_cast<size_t>(2 * R + 1));
        float ks = 0.0f;
        for (int i = -R; i <= R; ++i) {
            k[static_cast<size_t>(i + R)] = std::exp(-0.5f * (i / kWaterSplitSigma) * (i / kWaterSplitSigma));
            ks += k[static_cast<size_t>(i + R)];
        }
        for (float& v : k) v /= ks;
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                float a[3] = { 0, 0, 0 };
                for (int i = -R; i <= R; ++i) {
                    const int xx = (std::max)(0, (std::min)(W - 1, x + i));
                    const float* p = &img[(static_cast<size_t>(y) * W + xx) * 3];
                    for (int c = 0; c < 3; ++c) a[c] += p[c] * k[static_cast<size_t>(i + R)];
                }
                for (int c = 0; c < 3; ++c) tmp[(static_cast<size_t>(y) * W + x) * 3 + c] = a[c];
            }
        }
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                float a[3] = { 0, 0, 0 };
                for (int i = -R; i <= R; ++i) {
                    const int yy = (std::max)(0, (std::min)(H - 1, y + i));
                    const float* p = &tmp[(static_cast<size_t>(yy) * W + x) * 3];
                    for (int c = 0; c < 3; ++c) a[c] += p[c] * k[static_cast<size_t>(i + R)];
                }
                for (int c = 0; c < 3; ++c) low[(static_cast<size_t>(y) * W + x) * 3 + c] = a[c];
            }
        }
        tmp.clear();
        tmp.shrink_to_fit();

        // how far from the edge of the water (chessboard distance, up to kWaterEdge) -> weight
        std::vector<uint8_t> cur(wa.mask), next(n), dist(n, 0);
        for (int d = 1; d <= kWaterEdge; ++d) {
            bool any = false;
            for (size_t i = 0; i < n; ++i) if (cur[i]) { dist[i] = static_cast<uint8_t>(d); any = true; }
            if (!any) break;
            for (int y = 0; y < H; ++y) {
                for (int x = 0; x < W; ++x) {
                    bool all = true;
                    for (int dy = -1; dy <= 1 && all; ++dy) {
                        for (int dx = -1; dx <= 1 && all; ++dx) {
                            const int xx = x + dx, yy = y + dy;
                            all = xx >= 0 && yy >= 0 && xx < W && yy < H && cur[static_cast<size_t>(yy) * W + xx];
                        }
                    }
                    next[static_cast<size_t>(y) * W + x] = all ? 1 : 0;
                }
            }
            cur.swap(next);
        }
        const WaterLook& look = kWaterLooks[wa.strength >= 3 ? 1 : 0];
        float ux[4], uy[4], kx[4], ky[4], amp[4], cyc[4], ph0[4], tot = 0.0f;
        const float pi2 = 6.2831853f, lightX = -0.6f, lightY = -0.8f;
        for (int i = 0; i < 4; ++i) {
            const Wave& wv = look.waves[i];
            ux[i] = std::cos(wv.angle * 0.017453293f);
            uy[i] = std::sin(wv.angle * 0.017453293f);
            kx[i] = ux[i] / wv.lambda;
            ky[i] = 2.0f * uy[i] / wv.lambda;
            amp[i] = wv.amp;
            cyc[i] = wv.cycles;
            ph0[i] = wv.lambda * 0.37f;
            tot += wv.amp;
        }

        wa.frames.assign(static_cast<size_t>(kWaterFrames) * off, 0);
        for (int f = 0; f < kWaterFrames; ++f) {
            if (wa.cancel.load(std::memory_order_relaxed)) return false;
            uint16_t* out = &wa.frames[static_cast<size_t>(f) * off];
            const float time = static_cast<float>(f) / kWaterFrames;
            for (const WaterRun& r : wa.runs) {
                for (int x = r.x0; x < r.x1; ++x) {
                    const size_t i = static_cast<size_t>(r.y) * W + x;
                    const float t = static_cast<float>(dist[i]) / kWaterEdge;
                    const float w = t * t * (3.0f - 2.0f * t);
                    float dx = 0.0f, dy = 0.0f, slope = 0.0f;
                    if (w > 0.0f) {
                        const float gx = static_cast<float>(wa.ox + x), gy = static_cast<float>(wa.oy + r.y);
                        for (int q = 0; q < 4; ++q) {
                            const float c = std::cos(pi2 * (kx[q] * gx + ky[q] * gy - cyc[q] * time) + ph0[q]);
                            dx += amp[q] * c * ux[q];
                            dy += amp[q] * c * uy[q] * 0.5f;
                            slope += amp[q] * c * (ux[q] * lightX + uy[q] * lightY);
                        }
                    }
                    const float s = slope / tot;
                    // the smooth part at the displaced place (Catmull-Rom), the fine texture in place
                    const float sx = (std::max)(0.0f, (std::min)(W - 1.001f, x + w * dx));
                    const float sy = (std::max)(0.0f, (std::min)(H - 1.001f, r.y + w * dy));
                    const int x0 = static_cast<int>(sx), y0 = static_cast<int>(sy);
                    const float fx = sx - x0, fy = sy - y0;
                    float acc[3] = { 0, 0, 0 };
                    for (int j = 0; j < 4; ++j) {
                        const int yy = (std::max)(0, (std::min)(H - 1, y0 + j - 1));
                        const float wy = CubicW(fy, j);
                        for (int ii = 0; ii < 4; ++ii) {
                            const int xx = (std::max)(0, (std::min)(W - 1, x0 + ii - 1));
                            const float wgt = wy * CubicW(fx, ii);
                            const float* p = &low[(static_cast<size_t>(yy) * W + xx) * 3];
                            acc[0] += p[0] * wgt; acc[1] += p[1] * wgt; acc[2] += p[2] * wgt;
                        }
                    }
                    const float light = 1.0f + w * look.shade * s;
                    float g = 0.0f;
                    if (look.glint > 0.0f && s > 0.55f) {
                        const float e = (s - 0.55f) / 0.45f;
                        g = e * e * look.glint * w;
                    }
                    uint32_t ch[3];
                    for (int c = 0; c < 3; ++c) {
                        float v = (acc[c] + (img[i * 3 + c] - low[i * 3 + c])) * light;
                        v += g * (255.0f - v);
                        v = (std::max)(0.0f, (std::min)(255.0f, v));
                        ch[c] = static_cast<uint32_t>(v * (c == 1 ? 63.0f : 31.0f) / 255.0f + 0.5f);
                    }
                    out[r.off + static_cast<size_t>(x - r.x0)] = static_cast<uint16_t>((ch[0] << 11) | (ch[1] << 5) | ch[2]);
                }
            }
        }
        return true;
    }

    DWORD WINAPI WaterThread(LPVOID) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        for (;;) {
            std::shared_ptr<WaterAnim> job;
            AcquireSRWLockExclusive(&g_waterLock);
            while (g_waterQueue.empty()) SleepConditionVariableSRW(&g_waterWake, &g_waterLock, INFINITE, 0);
            job = g_waterQueue.front();
            g_waterQueue.pop_front();
            ReleaseSRWLockExclusive(&g_waterLock);
            if (job->cancel.load()) { job->state.store(3, std::memory_order_release); continue; }
            job->state.store(1, std::memory_order_relaxed);
            bool ok = false;
            const ULONGLONG t0 = GetTickCount64();
            try {
                ok = BuildWater(*job);
            } catch (...) {                 // out of memory: still water
                ok = false;
            }
            if (ok) {
                Logger::Info("HD water: %d-frame loop for %dx%d at %d,%d (%u water pixels, %u KB) made in %u ms.", kWaterFrames,
                             job->W, job->H, job->ox / 2, job->oy / 2, static_cast<unsigned>(job->pixels),
                             static_cast<unsigned>(job->frames.size() * 2 / 1024), static_cast<unsigned>(GetTickCount64() - t0));
            }
            if (!ok) {
                job->frames.clear();
                job->frames.shrink_to_fit();
            }
            job->hd.clear();                // the input is not needed any more
            job->hd.shrink_to_fit();
            job->mask.clear();
            job->mask.shrink_to_fit();
            job->state.store(ok ? 2 : (job->cancel.load() ? 3 : 4), std::memory_order_release);
        }
    }

    // main thread: a new water animation for a sprite drawn at level position (mx, my)
    std::shared_ptr<WaterAnim> QueueWater(const HdMap& hd, const Buf& src, int mx, int my, int strength) {
        auto wa = std::make_shared<WaterAnim>();
        wa->map = &hd;
        wa->W = 2 * src.w;
        wa->H = 2 * src.h;
        wa->ox = 2 * mx;
        wa->oy = 2 * my;
        wa->strength = strength;
        const size_t n = static_cast<size_t>(wa->W) * wa->H;
        try {
            wa->hd.assign(n, 0);
            wa->mask.assign(n, 0);
        } catch (...) {
            wa->state.store(4);
            return wa;
        }
        for (int y = 0; y < wa->H; ++y) {
            const int hy = wa->oy + y;
            const auto* sp = reinterpret_cast<const uint16_t*>(src.bits + static_cast<size_t>(y >> 1) * src.pitch);
            for (int x = 0; x < wa->W; ++x) {
                const int hx = wa->ox + x;
                if (hx < 0 || hy < 0 || hx >= hd.w || hy >= hd.h) continue;
                const size_t i = static_cast<size_t>(y) * wa->W + x;
                wa->hd[i] = hd.px[static_cast<size_t>(hy) * hd.w + hx];
                wa->mask[i] = sp[x >> 1] != 0x07C0 ? 1 : 0;
            }
        }
        if (!g_waterThread) {
            if (HANDLE th = CreateThread(nullptr, 0, WaterThread, nullptr, 0, nullptr)) {
                CloseHandle(th);
                g_waterThread = true;
            } else {
                wa->state.store(4);
                return wa;
            }
        }
        AcquireSRWLockExclusive(&g_waterLock);
        g_waterQueue.push_back(wa);
        ReleaseSRWLockExclusive(&g_waterLock);
        WakeConditionVariable(&g_waterWake);
        g_waters.push_back(wa);
        return wa;
    }

    // main thread: keep the ready loops within the budget (the least recently drawn go first)
    void TrimWater() {
        size_t bytes = 0;
        g_waters.erase(std::remove_if(g_waters.begin(), g_waters.end(), [](const std::shared_ptr<WaterAnim>& w) {
            return w.use_count() == 1 && w->state.load() >= 2;  // nobody draws it any more
        }), g_waters.end());
        for (const auto& w : g_waters) if (w->state.load() == 2) bytes += w->frames.capacity() * 2;
        if (bytes <= kWaterBudgetBytes) return;
        std::vector<std::shared_ptr<WaterAnim>> ready;
        for (const auto& w : g_waters) if (w->state.load() == 2) ready.push_back(w);
        std::sort(ready.begin(), ready.end(), [](const auto& a, const auto& b) { return a->lastUse < b->lastUse; });
        for (const auto& w : ready) {
            if (bytes <= kWaterBudgetBytes * 3 / 4) break;
            bytes -= w->frames.capacity() * 2;
            w->frames.clear();
            w->frames.shrink_to_fit();
            w->state.store(3);              // made again when it is drawn again
        }
    }

    // main thread: the loop at this moment over the water's pixels (rect `whole` = the sprite)
    void DrawWater(const Buf& dst, const RECT& whole, const RECT& clip, WaterAnim& wa, LONGLONG now, LONGLONG freq) {
        const double sec = static_cast<double>(now) / static_cast<double>(freq);
        const double pos = std::fmod(sec, kWaterLoopSeconds) / kWaterLoopSeconds * kWaterFrames;
        const int f0 = static_cast<int>(pos) % kWaterFrames, f1 = (f0 + 1) % kWaterFrames;
        const int t = static_cast<int>((pos - std::floor(pos)) * 256.0);
        if (!wa.readyAt) wa.readyAt = now;
        const double since = static_cast<double>(now - wa.readyAt) / static_cast<double>(freq);
        const int ramp = since >= kWaterFadeInSeconds ? 256 : static_cast<int>(since / kWaterFadeInSeconds * 256.0);
        const uint16_t* fa = &wa.frames[static_cast<size_t>(f0) * wa.pixels];
        const uint16_t* fb = &wa.frames[static_cast<size_t>(f1) * wa.pixels];
        const __m128i tv = _mm_set1_epi16(static_cast<short>(t)), rv = _mm_set1_epi16(static_cast<short>(ramp));
        const __m128i m6 = _mm_set1_epi16(63), m5 = _mm_set1_epi16(31);
        // a + (b - a) * w / 256 per channel (w 0..256; the difference times w fits 16 bits)
        auto lerp565 = [&](__m128i a, __m128i b, __m128i w) {
            const __m128i ar = _mm_srli_epi16(a, 11), ag = _mm_and_si128(_mm_srli_epi16(a, 5), m6), ab = _mm_and_si128(a, m5);
            const __m128i br = _mm_srli_epi16(b, 11), bg = _mm_and_si128(_mm_srli_epi16(b, 5), m6), bb = _mm_and_si128(b, m5);
            const __m128i r = _mm_add_epi16(ar, _mm_srai_epi16(_mm_mullo_epi16(_mm_sub_epi16(br, ar), w), 8));
            const __m128i g = _mm_add_epi16(ag, _mm_srai_epi16(_mm_mullo_epi16(_mm_sub_epi16(bg, ag), w), 8));
            const __m128i bl = _mm_add_epi16(ab, _mm_srai_epi16(_mm_mullo_epi16(_mm_sub_epi16(bb, ab), w), 8));
            return _mm_or_si128(_mm_or_si128(_mm_slli_epi16(r, 11), _mm_slli_epi16(g, 5)), bl);
        };
        auto lerp1 = [](uint32_t a, uint32_t b, int w) {
            const int r = static_cast<int>(a >> 11) + (((static_cast<int>(b >> 11) - static_cast<int>(a >> 11)) * w) >> 8);
            const int g = static_cast<int>((a >> 5) & 63) + (((static_cast<int>((b >> 5) & 63) - static_cast<int>((a >> 5) & 63)) * w) >> 8);
            const int bl = static_cast<int>(a & 31) + (((static_cast<int>(b & 31) - static_cast<int>(a & 31)) * w) >> 8);
            return static_cast<uint16_t>((r << 11) | (g << 5) | bl);
        };
        for (const WaterRun& r : wa.runs) {
            const int y = whole.top + r.y;
            if (y < clip.top || y >= clip.bottom) continue;
            const int x0 = (std::max)(static_cast<int>(whole.left) + r.x0, static_cast<int>(clip.left));
            const int x1 = (std::min)(static_cast<int>(whole.left) + r.x1, static_cast<int>(clip.right));
            if (x1 <= x0) continue;
            auto* out = reinterpret_cast<uint16_t*>(dst.bits + static_cast<size_t>(y) * dst.pitch);
            const size_t base = r.off + static_cast<size_t>(x0 - (whole.left + r.x0));
            const uint16_t* pa = fa + base - x0;
            const uint16_t* pb = fb + base - x0;
            int x = x0;
            for (; x + 8 <= x1; x += 8) {
                __m128i v = lerp565(_mm_loadu_si128(reinterpret_cast<const __m128i*>(pa + x)),
                                    _mm_loadu_si128(reinterpret_cast<const __m128i*>(pb + x)), tv);
                if (ramp < 256) v = lerp565(_mm_loadu_si128(reinterpret_cast<const __m128i*>(out + x)), v, rv);
                _mm_storeu_si128(reinterpret_cast<__m128i*>(out + x), v);
            }
            for (; x < x1; ++x) {
                uint16_t v = lerp1(pa[x], pb[x], t);
                if (ramp < 256) v = lerp1(out[x], v, ramp);
                out[x] = v;
            }
        }
        wa.lastUse = ++g_waterUse;
    }
}

bool LevelOrigin(int& x, int& y) {
    x = g_originX;
    y = g_originY;
    return g_originValid && g_current != nullptr;
}

bool LevelPicturesNow(LevelPictures& out) {
    const HdMap* hd = g_current;
    if (!SherwoodConfig::Get().hdBackgrounds || g_variant < 0 || !hd || !hd->px || g_baseMap != hd || g_srcW <= 0 || g_srcH <= 0 ||
        g_base.size() != static_cast<size_t>(g_srcW) * g_srcH || hd->w != 2 * g_srcW || hd->h != 2 * g_srcH) {
        return false;
    }
    out.hd = hd->px;
    out.hdW = hd->w;
    out.hdH = hd->h;
    out.level = g_base.data();
    out.levelW = g_srcW;
    out.levelH = g_srcH;
    out.serial = g_levelPicturesSerial * 64u + static_cast<unsigned>(g_variant & 63);
    out.key = hd->key;
    out.variant = g_variant < static_cast<int>(g_variants.size()) ? g_variants[static_cast<size_t>(g_variant)].c_str() : "";
    return true;
}

std::string GameFolder() {
    return GameDir();
}

const std::string& WorkFolder() {
    // with the tools that use what is collected (tools\hd): earlier versions created hd\work for every player
    static const std::string work = [] {
        auto isDir = [](const std::string& p) {
            const DWORD a = GetFileAttributesA(p.c_str());
            return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
        };
        const std::string dir = GameDir() + "hd\\work\\";
        return (isDir(dir) && isDir(GameDir() + "tools\\hd")) ? dir : std::string();
    }();
    return work;
}

// Ground animations: large sprites that repaint the ground they lie on, frame after frame - the
// level picture of that spot plus ripples (water) or smoke, low-resolution and heavily block-
// compressed. Magnified over an HD background such a frame shows as a blurry, blocky patch.
// Recognised by content, not by name: a large sprite whose pixels are close to the level
// picture beneath it (flags, fires, mill wheels differ from what is under them: drawn as always).
//  - Overlays (the sprite covers its whole rectangle - chimney smoke): the difference to the level
//    picture beneath, smoothed and with the compression noise faded out, is blended over the HD
//    picture: smoke over the HD rock, no patch.
//  - Water (irregular): HDWater=1 (the player's choice of 2026-11-14) still water from the HD
//    picture; HDWater=2 / 3 (the player's wish of 2026-11-15: "rób wodę") the HD water moved by
//    waves (see "animated HD water" above); HDWater=0 the game's frames as they are.
// Returns true when the sprite was handled here (drawn or left out).
bool DrawGroundAnimation(const Buf& dst, const RECT& d, const RECT& clip, const Buf& src, const RECT& s) {
    const auto& cfg = SherwoodConfig::Get();
    const HdMap* hd = g_current;
    if (cfg.hdWater == 0 || !hd || !hd->px || !g_originValid || !g_srcBits) return false;
    if (src.w * src.h < 4000) return false; // small sprites never repaint the ground

    // position of the whole frame (not only of the part on screen) in level pixels
    const int hx = d.left + g_originX - 2 * s.left;
    const int hy = d.top + g_originY - 2 * s.top;
    if ((hx | hy) & 1) return false;
    const int mx = hx / 2, my = hy / 2;
    GroundAnim* anim = nullptr;
    int otherPlaces = 0; // same frame size seen elsewhere and not a repaint: a moving object
    for (GroundAnim& g : g_groundAnims) {
        if (g.w != src.w || g.h != src.h) continue;
        if (g.mx == mx && g.my == my) { anim = &g; break; }
        if (!g.repaint) ++otherPlaces;
    }
    const bool levelAlive = SurfaceAlive(reinterpret_cast<uintptr_t>(g_identity), g_srcBits);
    if (!anim) {
        if (otherPlaces >= 3) return false; // the ground does not move; no need to compare at every position
        if (g_groundAnims.size() > 4096) g_groundAnims.clear();
        // First time this sprite is seen here: compare it with the level picture.
        uint64_t diffSum = 0;
        uint32_t opaque = 0, total = 0;
        if (levelAlive) {
            const int x0 = (std::max)(0, -mx), y0 = (std::max)(0, -my);
            const int x1 = (std::min)(src.w, g_srcW - mx), y1 = (std::min)(src.h, g_srcH - my);
            for (int y = y0; y < y1; ++y) {
                const auto* sp = reinterpret_cast<const uint16_t*>(src.bits + static_cast<size_t>(y) * src.pitch);
                const auto* mp = reinterpret_cast<const uint16_t*>(g_srcBits + static_cast<size_t>(my + y) * g_srcPitch) + mx;
                for (int x = x0; x < x1; ++x) {
                    ++total;
                    const uint32_t a = sp[x];
                    if (a == 0x07C0) continue;
                    const uint32_t b = mp[x];
                    ++opaque;
                    diffSum += static_cast<uint32_t>(std::abs(static_cast<int>(a >> 11) - static_cast<int>(b >> 11))) * 8 +
                               static_cast<uint32_t>(std::abs(static_cast<int>((a >> 5) & 63) - static_cast<int>((b >> 5) & 63))) * 4 +
                               static_cast<uint32_t>(std::abs(static_cast<int>(a & 31) - static_cast<int>(b & 31))) * 8;
                }
            }
        }
        GroundAnim g;
        g.mx = mx; g.my = my; g.w = src.w; g.h = src.h;
        const double meanDiff = opaque ? static_cast<double>(diffSum) / (3.0 * opaque) : 255.0; // per channel, 0..255
        const double coverage = total ? 100.0 * opaque / total : 0.0;
        g.repaint = opaque >= 3000 && meanDiff < kGroundRepaintMaxDiff;
        g.overlay = g.repaint && coverage >= kGroundOverlayMinCoverage;
        g_groundAnims.push_back(g);
        anim = &g_groundAnims.back();
        Logger::Info("HD: sprite %dx%d at level position %d,%d: %.0f%% opaque, differs from the ground beneath by %.1f of 255 -> %s.",
                     src.w, src.h, mx, my, coverage, meanDiff,
                     !g.repaint ? "drawn" :
                     g.overlay ? "overlay (smoke): its difference to the ground over the HD picture" :
                     cfg.hdWater >= 2 ? "water: the HD water moved by waves" :
                                        "water surface, left out (still water from the HD picture)");
    }
    if (!anim->repaint) return false;
    // test harness (dbg 32): overlays are taken for water - Lincoln, the level of the test runs,
    // has no water animation, but chimney smoke at a known place
    const bool asWater = !anim->overlay || (DebugFlags() & 32u) != 0;
    if (asWater) {
        if (cfg.hdWater < 2) return true;                   // still water from the HD picture
        const int strength = cfg.hdWater >= 3 ? 3 : 2;
        WaterAnim* wa = anim->water.get();
        const int st = wa ? wa->state.load(std::memory_order_acquire) : -1;
        if (!wa || wa->map != hd || wa->strength != strength || st == 3) {
            if (wa) wa->cancel.store(true);
            anim->water = QueueWater(*hd, src, mx, my, strength);
            TrimWater();
            return true;                                    // still water until the loop is made
        }
        if (st != 2) return true;
        LARGE_INTEGER now{}, freq{};
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&freq);
        const RECT whole{ d.left - 2 * s.left, d.top - 2 * s.top, d.left - 2 * s.left + 2 * src.w, d.top - 2 * s.top + 2 * src.h };
        DrawWater(dst, whole, clip, *wa, now.QuadPart, freq.QuadPart);
        return true;
    }
    if (!levelAlive) return false;                          // nothing to compare with: as the game draws it

    // The difference picture is made when the animation shows another frame; in between, the
    // last two are cross-faded over the time a frame stays (the game steps its smoke a few times
    // a second - with 144 pictures a second it jumped; one frame of delay is not visible).
    const int w = src.w, h = src.h;
    if (!anim->frames) anim->frames = std::make_shared<GroundFrames>();
    GroundFrames& gf = *anim->frames;
    const size_t cells = static_cast<size_t>(w) * h * 4;       // the level-pixel picture
    const size_t big = cells * 4;                               // ... magnified 2x
    LARGE_INTEGER now{}, freq{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    const uint64_t frameHash = HashFrame(src);
    if (gf.cur.size() != big || frameHash != gf.curHash) {
        if (gf.cur.size() == big) {
            const double ms = static_cast<double>(now.QuadPart - gf.tCur) * 1000.0 / static_cast<double>(freq.QuadPart);
            if (ms >= 20.0 && ms <= 1000.0) gf.intervalMs = gf.intervalMs > 0.0 ? gf.intervalMs * 0.75 + ms * 0.25 : ms;
            gf.prev.swap(gf.cur);
            gf.prevSpan.swap(gf.curSpan);
            gf.hasPrev = true;
        }
        gf.curHash = frameHash;
        gf.tCur = now.QuadPart;
        static std::vector<int16_t> lo;                         // main thread (frame drawing)
        lo.assign(cells, 0);
        if (!MakeGroundDifference(*anim, src, mx, my, lo.data())) {
            gf.W = 2 * w;                                       // water: still until a mean exists
            gf.H = 2 * h;
            gf.cur.assign(big, 0);
            gf.curSpan.assign(static_cast<size_t>(h) * 4, 0);
            return true;
        }
        Magnify2x(lo.data(), w, h, anim->overlay, gf, gf.cur, gf.curSpan);
    }
    int t = 256;
    if (gf.hasPrev && gf.intervalMs > 0.0) {
        const double ms = static_cast<double>(now.QuadPart - gf.tCur) * 1000.0 / static_cast<double>(freq.QuadPart);
        t = static_cast<int>((std::min)(1.0, ms / gf.intervalMs) * 256.0);
    }
    // the magnified picture covers the whole frame; `d` may be a part of it (`clip` lies inside `d`)
    const RECT whole{ d.left - 2 * s.left, d.top - 2 * s.top, d.left - 2 * s.left + 2 * w, d.top - 2 * s.top + 2 * h };
    ApplyGround2x(dst, whole, clip, gf, t, anim->overlay);
    return true;
}

// The difference picture of one frame (see DrawGroundAnimation); false: nothing to show yet.
namespace {
bool MakeGroundDifference(GroundAnim& anim, const Buf& src, int mx, int my, int16_t* diff) {
    const int w = src.w, h = src.h;
    if (anim.overlay) {
        // per level pixel: the frame (x16) and its difference to the level picture (x16)
        static std::vector<int16_t> raw;
        raw.assign(static_cast<size_t>(w) * h * 6, 0);
        static std::vector<uint8_t> has;
        has.assign(static_cast<size_t>(w) * h, 0);
        for (int y = 0; y < h; ++y) {
            if (my + y < 0 || my + y >= g_srcH) continue;
            const auto* sp = reinterpret_cast<const uint16_t*>(src.bits + static_cast<size_t>(y) * src.pitch);
            const auto* mp = reinterpret_cast<const uint16_t*>(g_srcBits + static_cast<size_t>(my + y) * g_srcPitch);
            for (int x = 0; x < w; ++x) {
                if (mx + x < 0 || mx + x >= g_srcW) continue;
                const int a = sp[x];
                if (a == 0x07C0) continue;
                const int b = mp[mx + x];
                int16_t* r = &raw[(static_cast<size_t>(y) * w + x) * 6];
                r[0] = static_cast<int16_t>((a >> 11) * 16);
                r[1] = static_cast<int16_t>(((a >> 5) & 63) * 16);
                r[2] = static_cast<int16_t>((a & 31) * 16);
                r[3] = static_cast<int16_t>(((a >> 11) - (b >> 11)) * 16);
                r[4] = static_cast<int16_t>((((a >> 5) & 63) - ((b >> 5) & 63)) * 16);
                r[5] = static_cast<int16_t>(((a & 31) - (b & 31)) * 16);
                has[static_cast<size_t>(y) * w + x] = 1;
            }
        }
        // 3x3 means of the opaque neighbours (the frame is block-compressed); how much the frame
        // differs from the ground decides how much of it covers the HD picture
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                if (!has[static_cast<size_t>(y) * w + x]) continue;
                int sum[6] = { 0, 0, 0, 0, 0, 0 }, n = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    const int yy = y + dy;
                    if (yy < 0 || yy >= h) continue;
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int xx = x + dx;
                        if (xx < 0 || xx >= w || !has[static_cast<size_t>(yy) * w + xx]) continue;
                        const int16_t* r = &raw[(static_cast<size_t>(yy) * w + xx) * 6];
                        for (int c = 0; c < 6; ++c) sum[c] += r[c];
                        ++n;
                    }
                }
                // size of the difference on the 0..255 scale (x16): 5-bit channels x 255/31, 6-bit x 255/63
                const int size = (std::max)({ std::abs(sum[3] / n) * 255 / 31, std::abs(sum[4] / n) * 255 / 63,
                                              std::abs(sum[5] / n) * 255 / 31 });
                const int from = kOverlayFadeFrom * 16, to = kOverlayFadeTo * 16;
                if (size <= from) continue;
                const int alpha = size >= to ? 256 : (size - from) * 256 / (to - from);
                int16_t* o = diff + (static_cast<size_t>(y) * w + x) * 4;
                o[0] = static_cast<int16_t>(alpha);
                for (int c = 0; c < 3; ++c) o[c + 1] = static_cast<int16_t>(sum[c] / n * alpha / 256);
            }
        }
        return true;
    }
    return false;
}
} // namespace

const uint16_t* LastBackdropColouring() {
    return g_colourKnown ? ColorizeTable(g_colourHue, g_colourFactor) : nullptr;
}

void OnGameTick() {
    if (!SherwoodConfig::Get().hdBackgrounds) return;
    ScanVariants();
    PollHotKeys();
    Sprites::OnGameTick();
    UpdateBetween();

    // once per start, in the first mission: which variant is on and which keys switch it
    static bool s_hintShown = false;
    if (!s_hintShown && !g_variants.empty()) {
        s_hintShown = true;
        char text[128];
        if (Sprites::BetweenAvailable() && SherwoodConfig::Get().hdKeyBetween == 0x78) {
            std::snprintf(text, sizeof(text), "GRAFIKA HD: %s   F8 - ORYGINAL   F7 F6 - WARIANT   F9 - KLATKI POSREDNIE (%s)",
                          g_variantName, Sprites::BetweenOn() ? "WL" : "WYL");
        } else {
            std::snprintf(text, sizeof(text), "GRAFIKA HD: %s   F8 - ORYGINAL   F7 F6 - WARIANT", g_variantName);
        }
        RenderDDraw::ShowNotice(text, 7.0);
    }

    const int req = g_request.exchange(kReqNone);
    if (req != kReqNone) {
        if (g_variants.empty()) {
            RenderDDraw::ShowNotice("GRAFIKA HD: BRAK WARIANTOW W FOLDERZE HD - MAPS", 3.0);
        } else if (req == kReqNext) {
            SetVariant(g_variant + 1);
        } else if (req == kReqPrev) {
            SetVariant(g_variant - 1);
        } else if (req == kReqToggle) {
            SetVariant(g_variant >= 0 ? -1 : g_lastOn);
        } else if (req == kReqName) {
            int index = -1;
            bool known = _stricmp(g_requestName, "off") == 0;
            for (size_t i = 0; i < g_variants.size(); ++i) {
                if (_stricmp(g_variants[i].c_str(), g_requestName) == 0) {
                    index = static_cast<int>(i);
                    known = true;
                }
            }
            if (known) SetVariant(index);
        }
    }
    RedrawWhenPossible();
}

void RequestStep(int step) {
    g_request.store(step >= 0 ? kReqNext : kReqPrev);
}

void RequestToggle() {
    g_request.store(kReqToggle);
}

void RequestByName(const char* name) {
    std::snprintf(g_requestName, sizeof(g_requestName), "%s", name ? name : "");
    g_request.store(kReqName);
}

const char* CurrentVariantName() {
    return g_variantName;
}

const char* MenuVariantName() {
    if (!SherwoodConfig::Get().hdBackgrounds) return "off";
    ScanVariants();     // before the first mission the variant of the settings is not chosen yet
    return g_variantName;
}

void OnTickFrame() {
    Sprites::OnTickFrame();
}

void RequestBetween(int mode) {
    g_betweenRequest.store(mode < 0 || mode > 2 ? 2 : mode);
}

bool BetweenOn() {
    return Sprites::BetweenOn();
}

namespace {
void InstallBackgroundHooks();
void InstallInterfaceHooks(bool hdView);
}

void InstallHooks() {
    // Hooks of features the settings panel (F10) can switch on later go in whenever the frame is large enough
    // (each checks its setting at run time); before 2026-11-22 several went in only when the feature was on at the
    // start, and switching it on in the panel did nothing (audit). The HD background's own hooks need HD backgrounds.
    const auto& cfg = SherwoodConfig::Get();
    const bool hdView = cfg.width >= 2048;
    if (cfg.hdBackgrounds) InstallBackgroundHooks();
    InstallInterfaceHooks(hdView);
    if (cfg.hdBackgrounds) Sprites::InstallHooks();
}

namespace {
void InstallBackgroundHooks() {
    // push ebp / mov ebp,esp / pushal / mov ecx,[ebp+0x14] / or ecx,ecx
    static const uint8_t kProlog[] = { 0x55, 0x89, 0xE5, 0x60, 0x8B, 0x4D, 0x14, 0x09, 0xC9 };
    if (std::memcmp(reinterpret_cast<const void*>(kAddrTintSpan2x), kProlog, sizeof(kProlog)) == 0 &&
        MH_CreateHook(reinterpret_cast<LPVOID>(kAddrTintSpan2x), reinterpret_cast<LPVOID>(&Hook_TintSpan2x),
                      reinterpret_cast<LPVOID*>(&o_TintSpan2x)) == MH_OK &&
        MH_EnableHook(reinterpret_cast<LPVOID>(kAddrTintSpan2x)) == MH_OK) {
        Logger::Info("HD: water tint spans @ 0x%08X read the HD background at 2x zoom.", static_cast<uint32_t>(kAddrTintSpan2x));
    } else {
        o_TintSpan2x = nullptr;
        Logger::Warn("HD: tint span routine not recognised - tinted areas stay low-resolution on HD backgrounds.");
    }

    // push ebp / mov ebp,esp / pushal / mov ecx,[ebp+0x10] / mov edx,[ebp+0x14] / shr ecx,1 / shr edx,1
    static const uint8_t kSpriteProlog[] = { 0x55, 0x89, 0xE5, 0x60, 0x8B, 0x4D, 0x10, 0x8B, 0x55, 0x14, 0xD1, 0xE9, 0xD1, 0xEA };
    const bool keyed = HookRoutine(0x0062261C, kSpriteProlog, sizeof(kSpriteProlog), reinterpret_cast<void*>(&Hook_Sprite2xKeyed),
                                   reinterpret_cast<void**>(&o_Sprite2xKeyed));
    const bool plain = HookRoutine(0x006220EC, kSpriteProlog, sizeof(kSpriteProlog), reinterpret_cast<void*>(&Hook_Sprite2xPlain),
                                   reinterpret_cast<void**>(&o_Sprite2xPlain));
    Logger::Info("HD: 2x sprite routines replaced (shadows darken every pixel of an HD background): keyed=%d plain=%d.",
                 keyed ? 1 : 0, plain ? 1 : 0);
    {
        // (HD sprites only: TryBlendBlit finds no record of a decoded frame while they are off)
        // push ebp / mov ebp,esp / and esp,-8 / sub esp,0x44 / mov eax,[ebp+0x1C] / push ebx / push esi / xor esi,esi
        static const uint8_t kBlendStretch[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x83, 0xEC, 0x44, 0x8B, 0x45, 0x1C, 0x53, 0x56, 0x33, 0xF6 };
        const bool blend = HookRoutine(0x005E8B10, kBlendStretch, sizeof(kBlendStretch), reinterpret_cast<void*>(&Hook_BlendStretch),
                                       reinterpret_cast<void**>(&o_BlendStretch));
        Logger::Info("HD: stretched alpha blit @ 0x005E8B10 (selection ring, light effects) draws upscaled frames: %d.", blend ? 1 : 0);
    }
}

void InstallInterfaceHooks(bool hdView) {
    const auto& cfg = SherwoodConfig::Get();
    if (TestHarness::Enabled() || hdView) {
        // (the interface scale x2: the general blitter passes everything through while UiScale::Drawing() is false)
        // push -1 / push 0x6728F8 / mov eax, fs:[0] / push eax / mov fs:[0], esp / sub esp, 0x84
        static const uint8_t kGeneralBlit[] = { 0x6A, 0xFF, 0x68, 0xF8, 0x28, 0x67, 0x00, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00,
                                                0x50, 0x64, 0x89, 0x25, 0x00, 0x00, 0x00, 0x00, 0x81, 0xEC, 0x84, 0x00, 0x00, 0x00 };
        const bool general = HookRoutine(0x005E6900, kGeneralBlit, sizeof(kGeneralBlit), reinterpret_cast<void*>(&Hook_GeneralBlit),
                                         reinterpret_cast<void**>(&o_GeneralBlit));
        Logger::Info("General blitter @ 0x005E6900 hooked (draw traces, interface scale): %d.", general ? 1 : 0);
        // push ebp / mov ebp,esp / pushal / mov edx,[ebp+0x18] / mov ecx,[ebp+0x14] / shr edx,1
        static const uint8_t kAlphaText2x[] = { 0x55, 0x89, 0xE5, 0x60, 0x8B, 0x55, 0x18, 0x8B, 0x4D, 0x14, 0xD1, 0xEA };
        const bool text = HookRoutine(0x00622FD4, kAlphaText2x, sizeof(kAlphaText2x), reinterpret_cast<void*>(&Hook_AlphaText2x),
                                      reinterpret_cast<void**>(&o_AlphaText2x));
        Logger::Info("HD: 2x text with an alpha picture @ 0x00622FD4 blended per pixel: %d.", text ? 1 : 0);
    }
    if (cfg.menuZoom && hdView) {       // (each looks at HDMenu when called: Patch BaseOn)
        // push ebp / mov ebp,esp / pushal / mov edx,[ebp+0x18] / mov ecx,[ebp+0x14] / or ecx,ecx
        static const uint8_t kAlphaText1x[] = { 0x55, 0x89, 0xE5, 0x60, 0x8B, 0x55, 0x18, 0x8B, 0x4D, 0x14, 0x09, 0xC9 };
        // push ebp / mov ebp,esp / pushal / mov ecx,[ebp+0x10] / mov edx,[ebp+0x14] / or ecx,ecx
        static const uint8_t kShadow1x[] = { 0x55, 0x89, 0xE5, 0x60, 0x8B, 0x4D, 0x10, 0x8B, 0x55, 0x14, 0x09, 0xC9 };
        // push -1 / push 0x672DE0 / mov eax, fs:[0]
        static const uint8_t kPointer[] = { 0x6A, 0xFF, 0x68, 0xE0, 0x2D, 0x67, 0x00, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00 };
        const bool t1 = HookRoutine(0x00622EB4, kAlphaText1x, sizeof(kAlphaText1x), reinterpret_cast<void*>(&Hook_AlphaText1x),
                                    reinterpret_cast<void**>(&o_AlphaText1x));
        const bool k1 = HookRoutine(0x00622550, kShadow1x, sizeof(kShadow1x), reinterpret_cast<void*>(&Hook_Shadow1xKeyed),
                                    reinterpret_cast<void**>(&o_Shadow1xKeyed));
        const bool p1 = HookRoutine(0x00622028, kShadow1x, sizeof(kShadow1x), reinterpret_cast<void*>(&Hook_Shadow1xPlain),
                                    reinterpret_cast<void**>(&o_Shadow1xPlain));
        const bool cur = HookRoutine(0x005F1700, kPointer, sizeof(kPointer), reinterpret_cast<void*>(&Hook_DrawPointer),
                                     reinterpret_cast<void**>(&o_DrawPointer));
        Logger::Info("HD menus: 1:1 text / pictures / cursor hooked for HD letters and pictures: %d %d %d %d.", t1 ? 1 : 0, k1 ? 1 : 0,
                     p1 ? 1 : 0, cur ? 1 : 0);
        Logger::Info("HD menus: GDI text (lists of saved games, keys) drawn again twice as large: %d.", Patch::InstallGdiText() ? 1 : 0);
    }
    {
        // push -1 / push 0x666C38 / mov eax, fs:[0]
        static const uint8_t kColorize[] = { 0x6A, 0xFF, 0x68, 0x38, 0x6C, 0x66, 0x00, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00 };
        // the helpers it calls, as expected (fld [esp+8] / fcomp [esp+0xC]; push ecx / fld [esp+0xC]) and _ftol
        static const uint8_t kRgbToHsv[] = { 0xD9, 0x44, 0x24, 0x08, 0xD8, 0x5C, 0x24, 0x0C };
        static const uint8_t kHsvToRgb[] = { 0x51, 0xD9, 0x44, 0x24, 0x0C };
        static const uint8_t kFtol[] = { 0x55, 0x8B, 0xEC, 0x83, 0xC4, 0xF4 };
        const bool helpers = std::memcmp(reinterpret_cast<const void*>(0x0053CBC0), kRgbToHsv, sizeof(kRgbToHsv)) == 0 &&
                             std::memcmp(reinterpret_cast<const void*>(0x0053CA30), kHsvToRgb, sizeof(kHsvToRgb)) == 0 &&
                             std::memcmp(reinterpret_cast<const void*>(0x00642B7C), kFtol, sizeof(kFtol)) == 0;
        const bool colorize = helpers && HookRoutine(0x0053B330, kColorize, sizeof(kColorize), reinterpret_cast<void*>(&Hook_Colorize),
                                                     reinterpret_cast<void**>(&o_Colorize));
        Logger::Info("Dialogs: the backdrop colouring @ 0x0053B330 through a colour table: %d.", colorize ? 1 : 0);
    }
}
} // namespace

} // namespace DDM::HD
