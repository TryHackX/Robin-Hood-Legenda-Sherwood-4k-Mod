#include "ddm_hd.h"
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
#include <intrin.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
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
    // HideGroundAnimation).
    struct GroundAnim {
        int  mx, my, w, h;   // position in level pixels, frame size
        bool repaint;        // looks like the ground it is drawn on: water
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
    void TakeBase(const Buf& src, uint64_t key, const HdMap* hd) {
        g_baseMap = nullptr;
        g_base.clear();
        if (!hd) return;
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
        }
        return o_BlendStretch(self, edx, src, dst, alphaSrc, key, dstW, dstH, srcPitch, dstPitch, scaleX, scaleY);
    }

    // ---------------------------------------------------------------------------------------
    // Test mode only: the game's general blitter (0x005E6900, thiscall, 6 arguments - text,
    // selection rings, light effects ...) writes its arguments into a running draw trace, so
    // that it can be seen which of its many modes draws what.
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
                g_current = Resolve(g_variant, key, src.w, src.h);
                g_groundAnims.clear(); // another level (or another state of it)
                TakeBase(src, key, g_current);
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

// Water. The levels' water (ponds, streams) is animated by large sprites that repaint the
// ground they lie on, frame after frame: the same picture as the level background at that
// spot, with ripples - stored at low resolution and heavily block-compressed. Magnified over
// an HD background they turn the pond back into blocks, while the pond of the HD picture
// itself looks far better. So such a sprite is left out when the HD background is shown:
// still water from the HD picture. It is recognised by content, not by name: a large sprite
// whose pixels are close to the level picture beneath it. Flags, smoke, fires, mill wheels
// differ from what is under them and are drawn as always.
bool HideGroundAnimation(const Buf& src, const RECT& s, const RECT& d) {
    const auto& cfg = SherwoodConfig::Get();
    const HdMap* hd = g_current;
    if (cfg.hdWater == 0 || !hd || !hd->px || !g_originValid || !g_srcBits) return false;
    if (src.w * src.h < 4000) return false; // small sprites are never water surfaces

    // position of the whole frame (not only of the part on screen) in level pixels
    const int hx = d.left + g_originX - 2 * s.left;
    const int hy = d.top + g_originY - 2 * s.top;
    if ((hx | hy) & 1) return false;
    const int mx = hx / 2, my = hy / 2;
    int otherPlaces = 0; // same frame size seen elsewhere and not water: a moving object
    for (const GroundAnim& g : g_groundAnims) {
        if (g.w != src.w || g.h != src.h) continue;
        if (g.mx == mx && g.my == my) return g.repaint;
        if (!g.repaint) ++otherPlaces;
    }
    if (otherPlaces >= 3) return false; // water does not move; no need to compare at every position
    if (g_groundAnims.size() > 4096) g_groundAnims.clear();

    // First time this sprite is seen here: compare it with the level picture.
    uint64_t diffSum = 0;
    uint32_t opaque = 0, total = 0;
    if (SurfaceAlive(reinterpret_cast<uintptr_t>(g_identity), g_srcBits)) {
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
    GroundAnim g{ mx, my, src.w, src.h, false };
    const double meanDiff = opaque ? static_cast<double>(diffSum) / (3.0 * opaque) : 255.0; // per channel, 0..255
    const double coverage = total ? 100.0 * opaque / total : 0.0;
    g.repaint = opaque >= 3000 && meanDiff < kGroundRepaintMaxDiff;
    g_groundAnims.push_back(g);
    Logger::Info("HD: sprite %dx%d at level position %d,%d: %.0f%% opaque, differs from the ground beneath by %.1f of 255 -> %s.",
                 src.w, src.h, mx, my, coverage, meanDiff,
                 g.repaint ? "water surface, left out (still water from the HD picture)" : "drawn");
    return g.repaint;
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

void InstallHooks() {
    if (!SherwoodConfig::Get().hdBackgrounds) return;
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
    if (SherwoodConfig::Get().hdSprites) {
        // push ebp / mov ebp,esp / and esp,-8 / sub esp,0x44 / mov eax,[ebp+0x1C] / push ebx / push esi / xor esi,esi
        static const uint8_t kBlendStretch[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x83, 0xEC, 0x44, 0x8B, 0x45, 0x1C, 0x53, 0x56, 0x33, 0xF6 };
        const bool blend = HookRoutine(0x005E8B10, kBlendStretch, sizeof(kBlendStretch), reinterpret_cast<void*>(&Hook_BlendStretch),
                                       reinterpret_cast<void**>(&o_BlendStretch));
        Logger::Info("HD: stretched alpha blit @ 0x005E8B10 (selection ring, light effects) draws upscaled frames: %d.", blend ? 1 : 0);
    }
    if (TestHarness::Enabled() || (SherwoodConfig::Get().uiScale >= 2 && SherwoodConfig::Get().width >= 2048)) {
        // push -1 / push 0x6728F8 / mov eax, fs:[0] / push eax / mov fs:[0], esp / sub esp, 0x84
        static const uint8_t kGeneralBlit[] = { 0x6A, 0xFF, 0x68, 0xF8, 0x28, 0x67, 0x00, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00,
                                                0x50, 0x64, 0x89, 0x25, 0x00, 0x00, 0x00, 0x00, 0x81, 0xEC, 0x84, 0x00, 0x00, 0x00 };
        const bool general = HookRoutine(0x005E6900, kGeneralBlit, sizeof(kGeneralBlit), reinterpret_cast<void*>(&Hook_GeneralBlit),
                                         reinterpret_cast<void**>(&o_GeneralBlit));
        Logger::Info("General blitter @ 0x005E6900 hooked (draw traces, interface scale): %d.", general ? 1 : 0);
    }
    Sprites::InstallHooks();
}

} // namespace DDM::HD
