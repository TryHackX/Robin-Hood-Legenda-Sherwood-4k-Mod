#include "ddm_hd_menu.h"
#include "ddm.h"
#include "ddm_hd.h"
#include "../config.h"
#include "../logger.h"
#include "../test_harness.h"
#include "../timing.h"
#include "../ui_scale.h"
#include "../view_zoom.h"
#include "../window_manager.h"

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// See ddm_hd_menu.h.
namespace DDM::HD::Menu {
namespace {
    // the composed picture last seen (the menus use one such surface)
    struct Source {
        unsigned serial = 0, gen = 0, genWrite = 0;
        uint64_t key = 0;
        uint64_t inner = 0;       // InnerKeyOf, when needed (0: not computed)
        bool valid = false;
        int8_t onBase = 0;        // (A): 0 not checked, 1 still mostly the last recognised screen, -1 not
    };
    Source g_src;

    // upscaled pictures of the current variant, by key (nullptr: there is none)
    struct Picture {
        int cropW = 0, cropH = 0;        // the shown part they were made for
        int l = 0, t = 0, w = 0, h = 0;  // the part of it that has a picture (1x, crop pixels)
        std::vector<uint32_t> px;        // 2w x 2h BGRA
    };
    std::unordered_map<uint64_t, std::shared_ptr<Picture>> g_pics;
    std::string g_variant;

    // canvases over the shown part of the frame
    RECT g_crop{};
    int  g_cw = 0, g_ch = 0;
    std::vector<uint32_t> g_orig;        // 0 or 0x10000 | rgb565
    std::vector<uint32_t> g_hd;          // BGRA, 2x
    RECT g_dirty{};
    bool g_anyDirty = false;
    bool g_anyLayer = false;
    unsigned g_generation = 1;
    // The HD canvas holds this picture over its whole box (filled once per picture; copies of the same
    // picture only change which pixels show it - the original-pixel canvas). Changes of the HD canvas
    // are uploaded on their own (16 MB at 2x - not for every frame of the credits).
    const Picture* g_canvasPic = nullptr;
    RECT g_hdDirty{};
    bool g_anyHdDirty = false;

    // collector
    std::unordered_set<uint64_t> g_written;
    bool g_writeFailed = false;

    // the backdrop of a dialog in a mission (see GetBackdrop)
    std::vector<uint32_t> g_backdrop;        // BGRA, alpha = level (255) or dialog picture (0)
    int      g_backdropW = 0, g_backdropH = 0;
    unsigned g_backdropGen = 0;
    Source   g_backdropSrc;
    bool     g_backdropValid = false;
    bool     g_backdropNone = false;     // the last backdrop was no darkened level at all (a menu picture)
    // the level on the screen when the dialog started (the backdrop is made from it)
    std::vector<uint16_t> g_world;
    int  g_worldW = 0, g_worldH = 0;
    bool g_worldValid = false;
    // the smooth zoom magnified that level's world (view_zoom.h): its copy without the interface and
    // the magnification - the backdrop is shown the same way (else the world jumped back to the
    // engine's zoom behind the in-game menu)
    std::vector<uint16_t> g_lupaWorld;
    ViewZoom::View g_lupa;
    bool g_lupaValid = false;
    std::vector<uint32_t> g_backdropShow;   // BGRA: what is shown where the backdrop shows (empty = g_backdrop)

    uint32_t To8888(uint16_t p) {
        uint32_t r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
        r = (r << 3) | (r >> 2);
        g = (g << 2) | (g >> 4);
        b = (b << 3) | (b >> 2);
        return (r << 16) | (g << 8) | b;
    }

    // The backdrop the game made (`comp`): the level darkened, with pictures of the dialog in it.
    // How the game darkens is learned from the pairs (level pixel -> backdrop pixel): for every
    // colour the result it gives most often (majority vote). A backdrop pixel that is not that
    // result belongs to the dialog; there the darkened level is put in (for the backdrop shown at
    // its size) and alpha says "dialog".
    void BuildBackdrop(const uint16_t* comp, int w, int h) {
        LARGE_INTEGER q0{}, q1{}, qf{};
        QueryPerformanceCounter(&q0);
        g_backdropShow.clear();
        g_backdrop.resize(static_cast<size_t>(w) * h);
        const size_t n = static_cast<size_t>(w) * h;
        static std::vector<uint32_t> to8888;        // RGB565 -> 0x00RRGGBB, once
        if (to8888.empty()) {
            to8888.resize(65536);
            for (uint32_t c = 0; c < 65536; ++c) to8888[c] = To8888(static_cast<uint16_t>(c));
        }
        const bool haveWorld = g_worldValid && g_worldW == w && g_worldH == h;
        RECT crop{};
        if (!haveWorld || !UiScale::MenuCropRect(w, h, crop)) {
            for (size_t i = 0; i < n; ++i) g_backdrop[i] = 0xFF000000u | to8888[comp[i]];
            g_backdropNone = false;
            return;
        }
        // Only the part the menu zoom shows is looked at (2026-11-15: the whole frame took ~30 ms when
        // the in-game menu opened): the presenter tests the backdrop only there, and outside it the game
        // never draws a dialog - the old darkened level stays, shown as it is.
        static std::vector<uint16_t> cand;
        static std::vector<int32_t> votes;
        cand.assign(65536, 0);
        votes.assign(65536, 0);
        auto vote = [&](size_t i) {
            const uint16_t a = g_world[i], b = comp[i];
            int32_t& v = votes[a];
            if (v == 0) { cand[a] = b; v = 1; }
            else if (cand[a] == b) ++v;
            else --v;
        };
        for (int y = crop.top; y < crop.bottom; ++y) {
            const size_t row = static_cast<size_t>(y) * w;
            for (int x = crop.left; x < crop.right; ++x) vote(row + x);
        }
        // every 4th pixel of the rest: the colours seen only there get their darkened colour too (the
        // level magnified by the smooth zoom below takes its colours from the whole frame)
        for (int y = 0; y < h; y += 2) {
            const size_t row = static_cast<size_t>(y) * w;
            const bool rowIn = y >= crop.top && y < crop.bottom;
            for (int x = 0; x < w; x += 2) {
                if (rowIn && x >= crop.left && x < crop.right) continue;
                vote(row + x);
            }
        }
        // The vote guesses: a level colour seen mostly under the dialog's pictures got a colour of the
        // picture as its "darkened" colour, and there the picture counted as level - the level showed
        // through it in dots shaped like what lay below (the statistics page after a mission,
        // 2026-11-17). The game darkens with a known per-pixel map (0x0053B330, the colour table
        // made for it, bit-identical) or not at all: whichever of the two fits the backdrop clearly
        // replaces the guesses.
        bool exact = false;     // the game's own colour table: every level colour's darkened colour is known
        {
            const uint16_t* coloured = HD::LastBackdropColouring();
            size_t nMap = 0, nSame = 0, nAll = 0;
            for (int y = crop.top; y < crop.bottom; y += 2) {
                const size_t row = static_cast<size_t>(y) * w;
                for (int x = crop.left; x < crop.right; x += 2) {
                    const uint16_t a = g_world[row + x], b = comp[row + x];
                    ++nAll;
                    nSame += a == b ? 1u : 0u;
                    nMap += coloured && coloured[a] == b ? 1u : 0u;
                }
            }
            if (nAll > 0) {
                if (coloured && nMap >= nSame && nMap * 10 >= nAll * 3) {
                    for (uint32_t c = 0; c < 65536; ++c) cand[c] = coloured[c];
                    exact = true;
                } else if (nSame * 10 >= nAll * 3) {
                    for (uint32_t c = 0; c < 65536; ++c) cand[c] = static_cast<uint16_t>(c);
                }
            }
        }
        // The options opened from the in-game menu cover the shown part with a picture of their own:
        // counted over the whole frame they were 87 % level - the level showed through where dark pixels
        // of the picture matched it by chance (a dotted ring by the title, 2026-11-15). Black pixels prove
        // nothing (the vote maps every dark level colour to the black of such a picture) - only the
        // others count.
        // A strip of the level NOT darkened beside the darkened backdrop (YouTuber's report 2026-11-21,
        // a question in Nottingham, 1.2.7: a full-height strip on the right showed the level undarkened
        // and larger). Two ways the backdrop can hold such pixels, both repaired below when the game's
        // own colour table is known (`exact`) and the backdrop is the darkened level (`!none`):
        //  - in the shown part: the game's backdrop is the level itself, undarkened (c == world, while
        //    the darkened colour differs). It counted as "a picture of the dialog", and the presenter
        //    showed the frame there - enlarged with the dialog. Now it is level: the backdrop keeps the
        //    frame's colour (so the presenter still recognises it) and its shown version (t5) the
        //    darkened one;
        //  - outside the shown part (where the game draws no dialog): anything that is not the darkened
        //    level of the dialog's start (e.g. an older picture) is shown as that darkened level.
        static std::vector<uint32_t> undarkened, foreign;
        undarkened.clear();
        foreign.clear();
        size_t shownLevel = 0, shown = 0;
        for (int y = 0; y < h; ++y) {
            const size_t row = static_cast<size_t>(y) * w;
            uint32_t* out = g_backdrop.data() + row;
            const uint16_t* c = comp + row;
            const bool rowIn = y >= crop.top && y < crop.bottom;
            for (int x = 0; x < w; ++x) {
                const uint16_t dark = cand[g_world[row + x]];
                if (!rowIn || x < crop.left || x >= crop.right) {
                    out[x] = 0xFF000000u | to8888[c[x]];
                    if (exact && c[x] != dark) foreign.push_back(static_cast<uint32_t>(row + x));
                    continue;
                }
                bool level = c[x] == dark;
                if (!level && exact && c[x] == g_world[row + x]) {
                    undarkened.push_back(static_cast<uint32_t>(row + x));
                    level = true;
                }
                out[x] = level ? 0xFF000000u | to8888[c[x]] : to8888[dark];   // alpha 0: a picture of the dialog lies here
                if (c[x] != 0) {
                    ++shown;
                    shownLevel += level ? 1u : 0u;
                }
            }
        }
        if (shown == 0) shown = 1;
        // a backdrop that is not the darkened level at all (a menu picture): all enlarged - or its
        // HD menu picture, when there is one (OnScreenCopy)
        const bool none = shownLevel * 10 < shown * 4;
        g_backdropNone = none;
        g_backdropShow.clear();
        if (none) {
            for (size_t i = 0; i < n; ++i) g_backdrop[i] &= 0x00FFFFFFu;
        } else if ((!undarkened.empty() || !foreign.empty()) && undarkened.size() + foreign.size() < n / 3) {
            // (more than a third of the frame would mean the level copy is another picture than the backdrop's)
            auto box = [&](const std::vector<uint32_t>& px, char* text, size_t size) {
                int x0 = w, y0 = h, x1 = -1, y1 = -1;
                for (const uint32_t i : px) {
                    const int x = static_cast<int>(i % static_cast<uint32_t>(w)), y = static_cast<int>(i / static_cast<uint32_t>(w));
                    x0 = (std::min)(x0, x); y0 = (std::min)(y0, y); x1 = (std::max)(x1, x); y1 = (std::max)(y1, y);
                }
                if (px.empty()) std::snprintf(text, size, "none");
                else std::snprintf(text, size, "%zu px in %d,%d-%d,%d", px.size(), x0, y0, x1 + 1, y1 + 1);
            };
            char a[96], b[96];
            box(undarkened, a, sizeof(a));
            box(foreign, b, sizeof(b));
            for (const uint32_t i : foreign) g_backdrop[i] = 0xFF000000u | to8888[cand[g_world[i]]];
            if (!undarkened.empty()) {
                g_backdropShow = g_backdrop;
                for (const uint32_t i : undarkened) g_backdropShow[i] = 0xFF000000u | to8888[cand[g_world[i]]];
            }
            Logger::Info("HD menu: dialog backdrop - level the game left undarkened in the shown part: %s; outside it, not "
                         "the darkened level: %s (shown part %ld,%ld-%ld,%ld) - shown darkened.",
                         a, b, crop.left, crop.top, crop.right, crop.bottom);
        }
        QueryPerformanceCounter(&q1);
        QueryPerformanceFrequency(&qf);
        Logger::Info("HD menu: dialog backdrop - %.0f %% of the shown part darkened level%s (%.1f ms).",
                     100.0 * static_cast<double>(shownLevel) / static_cast<double>(shown),
                     none ? " - too little: all enlarged with the dialog" : " (shown at its size), the rest enlarged with the dialog",
                     static_cast<double>(q1.QuadPart - q0.QuadPart) * 1000.0 / static_cast<double>(qf.QuadPart));
        const size_t level = none ? 0 : n;      // (the magnified world below: only over a darkened level)

        // The level was shown magnified (smooth zoom): where the backdrop shows the world (not the
        // interface: the frame equals the world copy there), it shows the magnified world, darkened
        // the way the game darkened it (the colour map just learned), bilinear.
        if (!g_lupaValid || g_lupa.w != w || g_lupa.h != h || level < n / 5 || g_lupa.m <= 1.0f) return;
        // Integer arithmetic: darkened colour of every RGB565 value once, source columns/rows and
        // weights (0..256) once per column/row. (With floats per pixel and channel this took ~150 ms
        // at 2560x1440 - the hitch when the in-game menu opened, user report 2026-11-15.)
        const ULONGLONG t0 = GetTickCount64();
        if (g_backdropShow.size() != g_backdrop.size()) g_backdropShow = g_backdrop;
        const float m = g_lupa.m;
        // not the hedge band along the bottom: its black base and the black frame below a level
        // smaller than the view were the same pixel in both copies - the magnified world showed
        // through the hedge ("the bushes break up", YouTuber's report 2026-11-17)
        int worldRows = (std::min)(h, static_cast<int>(g_lupa.worldH));
        if (const int band = DDM::LastHudBandTop(); band > 0) worldRows = (std::min)(worldRows, band);
        static std::vector<uint32_t> dark;          // RGB565 -> darkened 0x00RRGGBB
        dark.resize(65536);
        for (uint32_t c = 0; c < 65536; ++c) dark[c] = to8888[cand[c]];
        static std::vector<int> colA, colB, colW;
        colA.resize(static_cast<size_t>(w));
        colB.resize(static_cast<size_t>(w));
        colW.resize(static_cast<size_t>(w));
        for (int x = 0; x < w; ++x) {
            const float fx = g_lupa.cropX + (static_cast<float>(x) + 0.5f) / m - 0.5f;
            const int x0 = (std::max)(0, (std::min)(w - 1, static_cast<int>(fx)));
            colA[static_cast<size_t>(x)] = x0;
            colB[static_cast<size_t>(x)] = (std::min)(w - 1, x0 + 1);
            colW[static_cast<size_t>(x)] = static_cast<int>((std::max)(0.0f, (std::min)(1.0f, fx - static_cast<float>(x0))) * 256.0f + 0.5f);
        }
        // a + (b - a) * w / 256 for the red-blue pair and green of 0x00RRGGBB at once
        auto mix = [](uint32_t a, uint32_t b, uint32_t w) {
            const uint32_t iw = 256 - w;
            const uint32_t rb = (((a & 0x00FF00FFu) * iw + (b & 0x00FF00FFu) * w) >> 8) & 0x00FF00FFu;
            const uint32_t g = (((a & 0x0000FF00u) * iw + (b & 0x0000FF00u) * w) >> 8) & 0x0000FF00u;
            return rb | g;
        };
        // rows in bands on a few threads (each writes only its own rows)
        auto band = [&](int from, int to) {
            for (int y = from; y < to; ++y) {
                const float fy = g_lupa.cropY + (static_cast<float>(y) + 0.5f) / m - 0.5f;
                const int y0 = (std::max)(0, (std::min)(h - 1, static_cast<int>(fy)));
                const int y1 = (std::min)(h - 1, y0 + 1);
                const uint32_t wy = static_cast<uint32_t>((std::max)(0.0f, (std::min)(1.0f, fy - static_cast<float>(y0))) * 256.0f + 0.5f);
                const uint16_t* r0 = g_lupaWorld.data() + static_cast<size_t>(y0) * w;
                const uint16_t* r1 = g_lupaWorld.data() + static_cast<size_t>(y1) * w;
                const size_t row = static_cast<size_t>(y) * w;
                for (int x = 0; x < w; ++x) {
                    const size_t i = row + x;
                    if (!(g_backdrop[i] & 0xFF000000u) || g_world[i] != g_lupaWorld[i]) continue;   // dialog picture / interface
                    const int xa = colA[static_cast<size_t>(x)], xb = colB[static_cast<size_t>(x)];
                    const uint32_t wx = static_cast<uint32_t>(colW[static_cast<size_t>(x)]);
                    const uint32_t top = mix(dark[r0[xa]], dark[r0[xb]], wx);
                    const uint32_t bottom = mix(dark[r1[xa]], dark[r1[xb]], wx);
                    g_backdropShow[i] = 0xFF000000u | mix(top, bottom, wy);
                }
            }
        };
        const int parts = static_cast<int>((std::max)(1u, (std::min)(4u, std::thread::hardware_concurrency())));
        std::vector<std::thread> helpers;
        try {
            for (int p = 1; p < parts; ++p) helpers.emplace_back(band, worldRows * p / parts, worldRows * (p + 1) / parts);
        } catch (...) {
            // no threads: the rest on this one
        }
        const int done = static_cast<int>(helpers.size()) + 1;
        band(0, worldRows * 1 / parts);
        if (done < parts) band(worldRows * done / parts, worldRows);
        for (std::thread& t : helpers) t.join();
        Logger::Info("HD menu: dialog backdrop shows the level magnified x%.2f, as the smooth zoom did (%u ms).", m,
                     static_cast<unsigned>(GetTickCount64() - t0));
    }

    // counters (test harness)
    unsigned g_copies = 0, g_hdCopies = 0, g_keys = 0, g_dumps = 0, g_loaded = 0;

    std::string GameDir() {
        char exe[MAX_PATH]{};
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        std::string path(exe);
        path.erase(path.find_last_of("\\/") + 1);
        return path;
    }

    uint64_t Fnv1a64(const uint8_t* p, size_t n, uint64_t h) {
        for (size_t i = 0; i < n; ++i) {
            h ^= p[i];
            h *= 0x100000001B3ull;
        }
        return h;
    }

    // the key: dimensions of the shown part, then its pixels row by row
    uint64_t KeyOf(const Buf& src, const RECT& crop) {
        const uint16_t dims[2] = { static_cast<uint16_t>(crop.right - crop.left), static_cast<uint16_t>(crop.bottom - crop.top) };
        uint64_t h = Fnv1a64(reinterpret_cast<const uint8_t*>(dims), sizeof(dims), 0xCBF29CE484222325ull);
        for (int y = crop.top; y < crop.bottom; ++y) {
            const uint8_t* row = src.bits + static_cast<size_t>(y) * src.pitch + static_cast<size_t>(crop.left) * 2;
            h = Fnv1a64(row, static_cast<size_t>(crop.right - crop.left) * 2, h);
        }
        return h ? h : 1;
    }

    void ResetCanvases(const RECT& crop) {
        g_crop = crop;
        g_cw = crop.right - crop.left;
        g_ch = crop.bottom - crop.top;
        g_orig.assign(static_cast<size_t>(g_cw) * g_ch, 0u);
        g_hd.assign(static_cast<size_t>(g_cw) * g_ch * 4, 0u);
        g_dirty = RECT{ 0, 0, g_cw, g_ch };
        g_anyDirty = true;
        g_anyLayer = false;
        g_canvasPic = nullptr;
        g_anyHdDirty = false;
        ++g_generation;
    }

    std::string g_variantKey;     // the variant and whether remade pictures come first
    // what the canvases were made from: the source pixels of the shown part, and its key
    std::vector<uint16_t> g_srcCrop;
    uint64_t g_curKey = 0, g_curInner = 0;
    // the canvases were emptied (OnScreenFill): the next copy fills all of them, not only the pixels
    // that changed since the copy before
    bool g_fullRefill = false;

    // Pictures under something the game drew over them (2026-11-16, user: the question whether to quit
    // showed the old 1x main menu, the credits' forest had no HD / remade version). The pixels a
    // picture was recognised in ("base"): a copy whose own key is unknown shows the picture wherever
    // it still has those pixels.
    using Pixels = std::shared_ptr<const std::vector<uint16_t>>;
    // (A) the last screen recognised in a copy to the screen - a dialog over a menu screen: the game
    //     copies the screen, draws the parchment into it and copies that to the screen
    std::shared_ptr<Picture> g_basePic;
    Pixels g_baseCrop;
    Pixels g_curBase;                    // the base the canvases were filled with (null: none)
    // (B) surfaces that got a whole copy of a surface with a picture - the credits: the fixed forest is
    //     copied into the surface the scrolling text is mixed into, every frame
    struct CopyBase {
        const uint8_t* dst = nullptr;
        std::shared_ptr<Picture> pic;
        Pixels crop;
        unsigned use = 0;
        DWORD time = 0;                  // GetTickCount of the copy: used only by the copy to the screen right after it
    };
    CopyBase g_copyBase[2];
    unsigned g_copyUse = 0;

    // the crop pixels of a surface of the screen's size
    Pixels CropPixels(const Buf& src, const RECT& crop) {
        auto v = std::make_shared<std::vector<uint16_t>>(static_cast<size_t>(crop.right - crop.left) * (crop.bottom - crop.top));
        const int w = crop.right - crop.left;
        for (int y = crop.top; y < crop.bottom; ++y) {
            std::memcpy(v->data() + static_cast<size_t>(y - crop.top) * w,
                        src.bits + static_cast<size_t>(y) * src.pitch + static_cast<size_t>(crop.left) * 2, static_cast<size_t>(w) * 2);
        }
        return v;
    }

    // true: the variant changed (the canvases were emptied)
    bool CheckVariant() {
        const char* v = DDM::HD::MenuVariantName();
        const std::string want = std::string(v ? v : "") + (SherwoodConfig::Get().hdMenuRemake ? "|remake" : "");
        if (g_variantKey == want) return false;
        {
            g_variantKey = want;
            g_variant = v ? v : "";
            g_pics.clear();
            g_canvasPic = nullptr;              // the pictures are read again (other shared objects)
            g_basePic.reset();
            for (CopyBase& c : g_copyBase) c = CopyBase{};
            if (!g_orig.empty()) {
                std::fill(g_orig.begin(), g_orig.end(), 0u);
                g_dirty = RECT{ 0, 0, g_cw, g_ch };
                g_anyDirty = true;
                g_anyLayer = false;
            }
        }
        return true;
    }

    // hd\menu\<variant>\<key>.mhd: "MNHD", u32 1, u16 cropW, cropH, l, t, w, h, then 2w x 2h BGRA
    // A remade picture (hd\menu\remake\, tools\hd\import_menu_remake.py) comes before the variant's
    // upscaled one when HDMenuRemake=1. <key>.alias (16 hex digits) names the picture of another key:
    // the same menu picture with something else around it (see InnerKeyOf).
    std::shared_ptr<Picture> ReadMhd(const std::string& path);
    std::shared_ptr<Picture> LoadFrom(const std::string& folder, uint64_t key) {
        char name[64];
        std::snprintf(name, sizeof(name), "%016llx", static_cast<unsigned long long>(key));
        const std::string base = GameDir() + "hd\\menu\\" + folder + "\\" + name;
        if (auto p = ReadMhd(base + ".mhd")) return p;
        FILE* f = nullptr;
        if (fopen_s(&f, (base + ".alias").c_str(), "rb") == 0 && f) {
            char text[17]{};
            const size_t n = std::fread(text, 1, 16, f);
            std::fclose(f);
            if (n == 16) return ReadMhd(GameDir() + "hd\\menu\\" + folder + "\\" + text + ".mhd");
        }
        return nullptr;
    }

    std::shared_ptr<Picture> Load(uint64_t key) {
        const auto it = g_pics.find(key);
        if (it != g_pics.end()) return it->second;
        std::shared_ptr<Picture> pic;
        if (g_variant != "off" && !g_variant.empty()) {
            if (SherwoodConfig::Get().hdMenuRemake) pic = LoadFrom("remake", key);
            if (!pic) pic = LoadFrom(g_variant, key);
        }
        if (g_pics.size() > 64) {                   // a handful of menu screens; never grows without bound
            // first the pictures without an HD version (the credits made one per frame - the main menu's
            // remade picture, 16 MB, was read again after them)
            for (auto e = g_pics.begin(); e != g_pics.end();) e = e->second ? std::next(e) : g_pics.erase(e);
            if (g_pics.size() > 64) g_pics.clear();
        }
        g_pics[key] = pic;
        return pic;
    }

    std::shared_ptr<Picture> ReadMhd(const std::string& path) {
        std::shared_ptr<Picture> pic;
        {
            FILE* f = nullptr;
            if (fopen_s(&f, path.c_str(), "rb") == 0 && f) {
                struct { char magic[4]; uint32_t version; uint16_t cropW, cropH, l, t, w, h; } hdr{};
                if (std::fread(&hdr, sizeof(hdr), 1, f) == 1 && std::memcmp(hdr.magic, "MNHD", 4) == 0 && hdr.version == 1 &&
                    hdr.w > 0 && hdr.h > 0 && hdr.l + hdr.w <= hdr.cropW && hdr.t + hdr.h <= hdr.cropH) {
                    auto p = std::make_shared<Picture>();
                    p->cropW = hdr.cropW;
                    p->cropH = hdr.cropH;
                    p->l = hdr.l;
                    p->t = hdr.t;
                    p->w = hdr.w;
                    p->h = hdr.h;
                    p->px.resize(static_cast<size_t>(hdr.w) * hdr.h * 4);
                    if (std::fread(p->px.data(), 4, p->px.size(), f) == p->px.size()) {
                        pic = p;
                        ++g_loaded;
                    }
                }
                std::fclose(f);
                if (!pic) Logger::Warn("HD menu: %s could not be read - ignored.", path.c_str());
                else Logger::Info("HD menu: %s (%ux%u at %u,%u).", path.c_str(), hdr.w, hdr.h, hdr.l, hdr.t);
            }
        }
        return pic;
    }

    // The menu picture without what lies around it: the game's 1024x768 layout is centred in the
    // frame and its pictures fill the rows 142..624 of it (crop pixels 171..1195 x 142..624 at
    // 1365x768); 2 pixels in from that. Menus opened in a mission (options, load / save) have the
    // darkened level around their picture instead of black - their whole key never matches the main
    // menu's. tools\hd\upscale_menu.py writes <inner key>.alias next to each picture.
    constexpr int kInnerL = 173, kInnerT = 144, kInnerR = 1193, kInnerB = 622;
    uint64_t InnerKeyOf(const Buf& src, const RECT& crop) {
        if (crop.right - crop.left != 1365 || crop.bottom - crop.top != 768) return 0;
        uint64_t h = 0xCBF29CE484222325ull ^ 0x494E4E52ull;   // "INNR"
        for (int y = crop.top + kInnerT; y < crop.top + kInnerB; ++y) {
            const uint8_t* row = src.bits + static_cast<size_t>(y) * src.pitch + static_cast<size_t>(crop.left + kInnerL) * 2;
            h = Fnv1a64(row, static_cast<size_t>(kInnerR - kInnerL) * 2, h);
        }
        return h ? h : 1;
    }

    // hd\work\menu_frames\<key>.mnf: "MNUF", u32 1, u16 cropW, cropH, frameW, frameH, then the
    // shown part, RGB565 row by row
    void WriteForTool(uint64_t key, const Buf& src, const RECT& crop) {
        if (g_writeFailed || g_written.count(key) || g_written.size() > 64) return;
        // A picture that is different in every copy is an animation, not a menu picture: the credits
        // scroll their text over the forest - every frame was written as a picture of its own (2 MB each,
        // ~40 a second, 1 GB in hd\work\menu_frames and the credits at 38 frames a second; 2026-11-15).
        // The third new picture within a second stops the files until the copies calm down again.
        static DWORD lastNew = 0;
        static int burst = 0;
        const DWORD now = GetTickCount();
        burst = (now - lastNew < 1000) ? burst + 1 : 0;
        lastNew = now;
        if (burst >= 2) return;
        g_written.insert(key);
        const std::string dir = GameDir() + "hd\\work\\menu_frames";
        CreateDirectoryA((GameDir() + "hd\\work").c_str(), nullptr);
        CreateDirectoryA(dir.c_str(), nullptr);
        char name[64];
        std::snprintf(name, sizeof(name), "\\%016llx.mnf", static_cast<unsigned long long>(key));
        const std::string path = dir + name;
        if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) return;   // from an earlier run
        FILE* f = nullptr;
        if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) {
            g_writeFailed = true;
            return;
        }
        struct { char magic[4]; uint32_t version; uint16_t cropW, cropH, frameW, frameH; } hdr{
            { 'M', 'N', 'U', 'F' }, 1, static_cast<uint16_t>(crop.right - crop.left), static_cast<uint16_t>(crop.bottom - crop.top),
            static_cast<uint16_t>(src.w), static_cast<uint16_t>(src.h) };
        std::fwrite(&hdr, sizeof(hdr), 1, f);
        for (int y = crop.top; y < crop.bottom; ++y) {
            std::fwrite(src.bits + static_cast<size_t>(y) * src.pitch + static_cast<size_t>(crop.left) * 2, 2,
                        static_cast<size_t>(crop.right - crop.left), f);
        }
        std::fclose(f);
        ++g_dumps;
        Logger::Info("HD menu: menu picture %016llx without an HD version written to hd\\work\\menu_frames "
                     "(tools\\hd\\upscale_menu.py makes it).", static_cast<unsigned long long>(key));
    }

    void AddDirty(int l, int t, int r, int b) {
        if (!g_anyDirty) {
            g_dirty = RECT{ l, t, r, b };
            g_anyDirty = true;
        } else {
            g_dirty.left = (std::min)(g_dirty.left, static_cast<LONG>(l));
            g_dirty.top = (std::min)(g_dirty.top, static_cast<LONG>(t));
            g_dirty.right = (std::max)(g_dirty.right, static_cast<LONG>(r));
            g_dirty.bottom = (std::max)(g_dirty.bottom, static_cast<LONG>(b));
        }
    }

    void AddHdDirty(int l, int t, int r, int b) {
        if (!g_anyHdDirty) {
            g_hdDirty = RECT{ l, t, r, b };
            g_anyHdDirty = true;
        } else {
            g_hdDirty.left = (std::min)(g_hdDirty.left, static_cast<LONG>(l));
            g_hdDirty.top = (std::min)(g_hdDirty.top, static_cast<LONG>(t));
            g_hdDirty.right = (std::max)(g_hdDirty.right, static_cast<LONG>(r));
            g_hdDirty.bottom = (std::max)(g_hdDirty.bottom, static_cast<LONG>(b));
        }
    }

    // canvases over crop pixels [l, r) x [t, b) from the source pixels kept in g_srcCrop. `base` (crop
    // pixels of the whole shown part, may be null): the picture shows only where the source still has
    // the pixel the picture was recognised in - not where the game drew something over it.
    void FillCanvases(const std::shared_ptr<Picture>& pic, int l, int t, int r, int b, const uint16_t* base = nullptr) {
        const bool usable = pic && pic->cropW == g_cw && pic->cropH == g_ch;
        if (usable) ++g_hdCopies;
        if (usable && g_canvasPic != pic.get()) {
            // a picture the HD canvas does not hold yet: its whole box, once
            for (int py = 0; py < pic->h; ++py) {
                const uint32_t* hp = pic->px.data() + static_cast<size_t>(py * 2) * (pic->w * 2);
                uint32_t* hrow = g_hd.data() + static_cast<size_t>((pic->t + py) * 2) * (g_cw * 2) + static_cast<size_t>(pic->l * 2);
                std::memcpy(hrow, hp, static_cast<size_t>(pic->w) * 2 * 4);
                std::memcpy(hrow + g_cw * 2, hp + pic->w * 2, static_cast<size_t>(pic->w) * 2 * 4);
            }
            g_canvasPic = pic.get();
            AddHdDirty(pic->l, pic->t, pic->l + pic->w, pic->t + pic->h);
        }
        // the original-pixel canvas: only the box of what changed goes to the presenter
        int cl = g_cw, ct = g_ch, cr = -1, cb = -1;
        for (int cy = t; cy < b; ++cy) {
            const uint16_t* sp = g_srcCrop.data() + static_cast<size_t>(cy) * g_cw;
            const uint16_t* bp = base ? base + static_cast<size_t>(cy) * g_cw : nullptr;
            uint32_t* orow = g_orig.data() + static_cast<size_t>(cy) * g_cw;
            const int py = usable ? cy - pic->t : -1;
            const bool rowIn = usable && py >= 0 && py < pic->h;
            for (int cx = l; cx < r; ++cx) {
                uint32_t v = 0;
                if (rowIn) {
                    const int px = cx - pic->l;
                    if (px >= 0 && px < pic->w && (!bp || bp[cx] == sp[cx])) v = 0x10000u | sp[cx];
                }
                if (orow[cx] != v) {
                    orow[cx] = v;
                    cl = (std::min)(cl, cx);
                    cr = (std::max)(cr, cx + 1);
                    ct = (std::min)(ct, cy);
                    cb = (std::max)(cb, cy + 1);
                }
            }
        }
        if (usable) g_anyLayer = true;
        if (cr > cl) AddDirty(cl, ct, cr, cb);
    }
}

void OnScreenCopy(const Buf& src, unsigned serial, unsigned gen, unsigned genWrite, const RECT& s, int dx, int dy) {
    const auto& cfg = SherwoodConfig::Get();
    if (!UiScale::MenuZoomOn()) return;
    const bool mission = *reinterpret_cast<void* const*>(0x006C0FA8) != nullptr;
    if (mission) {
        // In a mission: the mission's own frames are copied this way too - only a dialog's
        // backdrop matters (the frozen picture of the level, different every time: no HD picture)
        g_backdropValid = false;
        if (!UiScale::DialogLoopRunning()) return;
        if (g_backdropSrc.serial != serial || g_backdropSrc.gen != gen || g_backdropSrc.genWrite != genWrite ||
            g_backdropW != src.w || g_backdropH != src.h) {
            g_backdropSrc.serial = serial;
            g_backdropSrc.gen = gen;
            g_backdropSrc.genWrite = genWrite;
            g_backdropW = src.w;
            g_backdropH = src.h;
            static std::vector<uint16_t> comp;
            comp.resize(static_cast<size_t>(src.w) * src.h);
            for (int y = 0; y < src.h; ++y) {
                std::memcpy(comp.data() + static_cast<size_t>(y) * src.w, src.bits + static_cast<size_t>(y) * src.pitch,
                            static_cast<size_t>(src.w) * 2);
            }
            BuildBackdrop(comp.data(), src.w, src.h);
            ++g_backdropGen;
        }
        if (!g_backdropNone) {
            g_backdropValid = true;
            return;
        }
        // a menu picture over everything (the options or load / save opened from the in-game
        // menu): its HD picture as in the main menu
    }
    g_backdropValid = false;
    if (!cfg.hdMenu) return;
    RECT crop{};
    if (!UiScale::MenuCropRect(src.w, src.h, crop)) return;
    ++g_copies;
    const bool variantChanged = CheckVariant();

    const size_t cropPx = static_cast<size_t>(crop.right - crop.left) * (crop.bottom - crop.top);
    // (B) first: the source got a whole copy of a surface with a picture just now (the credits' forest,
    // every frame) - the picture where the source still has those pixels; no keys over the whole
    // frame for every frame of the scrolling text
    std::shared_ptr<Picture> pic;
    Pixels base;
    bool fromCopy = false;
    if (!mission) {
        const DWORD now = GetTickCount();
        for (const CopyBase& c : g_copyBase) {
            if (c.dst == src.bits && c.pic && c.crop && c.crop->size() == cropPx && now - c.time <= 50) {
                pic = c.pic;
                base = c.crop;
                fromCopy = true;
                break;
            }
        }
    }

    // which picture: computed once per state of the source surface
    if (!pic && (!g_src.valid || g_src.serial != serial || g_src.gen != gen || g_src.genWrite != genWrite)) {
        g_src.serial = serial;
        g_src.gen = gen;
        g_src.genWrite = genWrite;
        g_src.key = KeyOf(src, crop);
        g_src.inner = 0;
        g_src.onBase = 0;
        g_src.valid = true;
        ++g_keys;
    }
    if (!pic) {
        pic = Load(g_src.key);
        if (!pic) {
            // the menu picture alone (in a mission: the level around it; no work file - every visit differs)
            if (!g_src.inner) g_src.inner = InnerKeyOf(src, crop);
            if (g_src.inner) pic = Load(g_src.inner);
        }
        if (pic) {
            // (A) also "in a mission" (2026-11-22, user: the questions "quit the game?" in the main menu and
            // "overwrite?" on the save screen showed the old 1x picture): the engine's pointer stays set after a
            // mission, so the main menu counted as a mission too; and in a mission this point is reached only
            // with a menu picture over everything (load / save / options), where a question is drawn the same way
            if (pic != g_basePic || g_src.onBase == 0) {
                // recognised: the pixels it was recognised in, for what may be drawn over it later (A)
                g_basePic = pic;
                g_baseCrop = CropPixels(src, crop);
                g_src.onBase = 1;
            }
        } else if (g_basePic && g_baseCrop && g_baseCrop->size() == cropPx) {
            // (A) the last recognised screen with something drawn over it (a dialog): when most of the
            // picture's pixels are still there. Black proves nothing (most menus have black around their
            // picture): only the other pixels count.
            if (g_src.onBase == 0) {
                const Picture& bp = *g_basePic;
                const int cw = crop.right - crop.left;
                size_t in = 0, same = 0;
                for (int y = bp.t; y < bp.t + bp.h; y += 2) {
                    const auto* row = reinterpret_cast<const uint16_t*>(src.bits + static_cast<size_t>(crop.top + y) * src.pitch) + crop.left;
                    const uint16_t* was = g_baseCrop->data() + static_cast<size_t>(y) * cw;
                    for (int x = bp.l; x < bp.l + bp.w; x += 2) {
                        if (was[x] == 0) continue;
                        ++in;
                        same += row[x] == was[x] ? 1u : 0u;
                    }
                }
                g_src.onBase = (in > 0 && same * 2 >= in) ? 1 : -1;
                Logger::Info("HD menu: a screen without a picture of its own - %.0f %% still the last menu picture%s.",
                             in ? 100.0 * static_cast<double>(same) / static_cast<double>(in) : 0.0,
                             g_src.onBase > 0 ? " (shown in HD around what was drawn over it)" : "");
            }
            if (g_src.onBase > 0) {
                pic = g_basePic;
                base = g_baseCrop;
            }
        }
        if (!pic) WriteForTool(g_src.key, src, crop);
    }

    // the canvases follow the copy (crop pixels)
    if (g_orig.empty() || crop.left != g_crop.left || crop.top != g_crop.top || crop.right != g_crop.right || crop.bottom != g_crop.bottom) {
        if (!pic) return;               // nothing to show yet: no canvases needed
        ResetCanvases(crop);
    }
    const int l = (std::max)(static_cast<int>(crop.left), dx);
    const int t = (std::max)(static_cast<int>(crop.top), dy);
    const int r = (std::min)(static_cast<int>(crop.right), dx + static_cast<int>(s.right - s.left));
    const int b = (std::min)(static_cast<int>(crop.bottom), dy + static_cast<int>(s.bottom - s.top));
    if (r <= l || b <= t) return;
    g_curKey = fromCopy ? 0 : g_src.key;        // (B): no key of this source was computed
    g_curInner = fromCopy ? 0 : g_src.inner;
    if (g_srcCrop.size() != static_cast<size_t>(g_cw) * g_ch) g_srcCrop.assign(static_cast<size_t>(g_cw) * g_ch, 0);
    // The same picture with the same base as the last copy: only the pixels that changed since then
    // (the credits: the column of text) - the rest of the canvases is right already.
    // (Not after the canvases were emptied: the black around a menu's picture equals the zeros the source
    // copy was emptied to - only the picture's own pixels were filled again, and after loading a saved
    // game the remade background of the load / save screen showed only in the middle, 2026-11-17.)
    static const Picture* lastPic = nullptr;
    static const void* lastBase = nullptr;
    const bool refillAll = variantChanged || g_fullRefill;
    g_fullRefill = false;
    const bool same = !refillAll && pic.get() == lastPic && base.get() == lastBase && pic;
    lastPic = pic.get();
    lastBase = base.get();
    int bl = r, bt = b, br = -1, bb = -1;
    for (int y = t; y < b; ++y) {
        const int sy = static_cast<int>(s.top) + (y - dy);
        const auto* sp = reinterpret_cast<const uint16_t*>(src.bits + static_cast<size_t>(sy) * src.pitch) + (static_cast<int>(s.left) - dx);
        uint16_t* row = g_srcCrop.data() + static_cast<size_t>(y - crop.top) * g_cw + (l - crop.left);
        const uint16_t* in = sp + l;
        const int n = r - l;
        if (same) {
            int a = 0;
            while (a < n && row[a] == in[a]) ++a;
            if (a < n) {
                int z = n - 1;
                while (z > a && row[z] == in[z]) --z;
                bl = (std::min)(bl, l + a);
                br = (std::max)(br, l + z + 1);
                bt = (std::min)(bt, y);
                bb = (std::max)(bb, y + 1);
            }
        }
        std::memcpy(row, in, static_cast<size_t>(n) * 2);
    }
    g_curBase = base;
    const uint16_t* bp = base ? base->data() : nullptr;
    if (refillAll) FillCanvases(pic, 0, 0, g_cw, g_ch, bp);         // the emptied canvases again, all of them
    else if (!same) FillCanvases(pic, l - crop.left, t - crop.top, r - crop.left, b - crop.top, bp);
    else if (br > bl) FillCanvases(pic, bl - crop.left, bt - crop.top, br - crop.left, bb - crop.top, bp);
}

void OnSurfaceCopy(const Buf& dst, const Buf& src, unsigned serial, unsigned gen, unsigned genWrite) {
    if (!SherwoodConfig::Get().hdMenu || !UiScale::MenuZoomOn()) return;
    if (*reinterpret_cast<void* const*>(0x006C0FA8) != nullptr) return;     // menus outside missions only
    RECT crop{};
    if (!UiScale::MenuCropRect(src.w, src.h, crop)) return;
    CheckVariant();
    // the source's picture, once per state of the source (the credits copy the same forest every frame)
    static Source s;
    static std::shared_ptr<Picture> pic;
    static Pixels pixels;
    if (!s.valid || s.serial != serial || s.gen != gen || s.genWrite != genWrite) {
        s.serial = serial;
        s.gen = gen;
        s.genWrite = genWrite;
        s.valid = true;
        pic = Load(KeyOf(src, crop));
        if (!pic) {
            const uint64_t inner = InnerKeyOf(src, crop);
            if (inner) pic = Load(inner);
        }
        pixels = pic ? CropPixels(src, crop) : nullptr;
    }
    CopyBase* slot = nullptr;
    for (CopyBase& c : g_copyBase) if (c.dst == dst.bits) slot = &c;
    if (!pic) {
        if (slot) *slot = CopyBase{};           // overwritten with something else
        return;
    }
    if (!slot) slot = g_copyBase[0].use <= g_copyBase[1].use ? &g_copyBase[0] : &g_copyBase[1];
    slot->dst = dst.bits;
    slot->pic = pic;
    slot->crop = pixels;
    slot->use = ++g_copyUse;
    slot->time = GetTickCount();
}

void OnScreenFill(const Buf& dst, const RECT& r) {
    // The game blanks the whole screen (loading a saved game: one colour fill of the back buffer,
    // then black frames until the level is there). The menu picture is gone - its HD / remade
    // version stayed wherever a black pixel happened to equal the old picture: a black, ragged
    // rectangle in the middle of the remade background (YouTuber's report 2026-11-17).
    if (g_orig.empty() || r.left > 0 || r.top > 0 || r.right < dst.w || r.bottom < dst.h) return;
    std::fill(g_orig.begin(), g_orig.end(), 0u);
    g_dirty = RECT{ 0, 0, g_cw, g_ch };
    g_anyDirty = true;
    g_anyLayer = false;
    g_curKey = 0;
    g_curInner = 0;
    g_canvasPic = nullptr;
    g_curBase.reset();
    g_srcCrop.clear();
    g_src = Source{};
    g_fullRefill = true;
}

bool GetLayer(const RECT& crop, Layer& out) {
    // the variant or the remade pictures switched (F10): the canvases again from the last picture
    if (CheckVariant() && !g_srcCrop.empty() && g_srcCrop.size() == g_orig.size()) {
        std::shared_ptr<Picture> pic = Load(g_curKey);
        if (!pic && g_curInner) pic = Load(g_curInner);
        FillCanvases(pic, 0, 0, g_cw, g_ch, g_curBase && pic ? g_curBase->data() : nullptr);
    }
    if (!SherwoodConfig::Get().hdMenu || g_orig.empty() || !g_anyLayer) return false;
    if (crop.left != g_crop.left || crop.top != g_crop.top || crop.right != g_crop.right || crop.bottom != g_crop.bottom) return false;
    out.orig = g_orig.data();
    out.hd = g_hd.data();
    out.w = g_cw;
    out.h = g_ch;
    out.dirty = g_anyDirty ? g_dirty : RECT{ 0, 0, 0, 0 };
    out.hdDirty = g_anyHdDirty ? g_hdDirty : RECT{ 0, 0, 0, 0 };
    out.generation = g_generation;
    g_anyDirty = false;
    g_anyHdDirty = false;
    return true;
}

const uint32_t* HdUnder(const RECT& crop, int cx, int cy, uint16_t px, int& stride) {
    if (!g_anyLayer || g_orig.empty() || crop.left != g_crop.left || crop.top != g_crop.top || crop.right != g_crop.right ||
        crop.bottom != g_crop.bottom || cx < 0 || cy < 0 || cx >= g_cw || cy >= g_ch) {
        return nullptr;
    }
    if (g_orig[static_cast<size_t>(cy) * g_cw + cx] != (0x10000u | px)) return nullptr;
    stride = g_cw * 2;
    return g_hd.data() + static_cast<size_t>(cy * 2) * stride + static_cast<size_t>(cx * 2);
}

bool GetBackdrop(Backdrop& out) {
    // only while a dialog of a mission is up (its loop runs) and its backdrop was copied
    if (!g_backdropValid || !UiScale::DialogLoopRunning() || *reinterpret_cast<void* const*>(0x006C0FA8) == nullptr ||
        g_backdrop.empty()) {
        return false;
    }
    out.bgra = g_backdrop.data();
    out.show = g_backdropShow.size() == g_backdrop.size() ? g_backdropShow.data() : nullptr;
    out.w = g_backdropW;
    out.h = g_backdropH;
    out.generation = g_backdropGen;
    return true;
}

void NoteDialogStart(bool fromBack) {
    g_worldValid = false;
    g_lupaValid = false;
    if (!UiScale::MenuZoomOn() || *reinterpret_cast<void* const*>(0x006C0FA8) == nullptr) return;
    const ULONGLONG t0 = GetTickCount64();
    // The level must be the same picture as the world copy below. A dialog opened from a mission
    // window (its click handler runs while the tick's frame is drawn) rendered the mission once for
    // its backdrop: that render is in the back buffer, of the same tick as the world copy - the
    // front buffer holds an older, perhaps in-between frame, and every pixel of the level differed
    // from the world copy: the backdrop showed the level at its size beside the magnified part (the
    // camp of Sherwood, 2026-11-17).
    fromBack = fromBack || Timing::LastRenderWasBackdrop();
    g_worldValid = fromBack ? DDM::CopyBack(g_world, g_worldW, g_worldH) : DDM::CopyFront(g_world, g_worldW, g_worldH);
    ViewZoom::View v;
    // the world of that very render when the level comes from it (the copy of the last frame the player
    // saw had the figures a little elsewhere - and in an extra frame they stood between two ticks)
    const bool sameRender = fromBack && Timing::LastRenderWasBackdrop() &&
                            ViewZoom::BackdropWorld(Timing::BackdropRenderCount(), v);
    if (g_worldValid && (sameRender || ViewZoom::LastWorld(v)) && v.w == g_worldW && v.h == g_worldH) {
        g_lupaWorld.assign(v.snap, v.snap + static_cast<size_t>(v.w) * v.h);
        g_lupa = v;
        g_lupa.snap = nullptr;
        g_lupaValid = true;
    }
    Logger::Info("HD menu: dialog start - level copied%s%s (%u ms).", fromBack ? " from the frame being drawn" : "",
                 g_lupaValid ? (sameRender ? " with the magnified world of the same render" : " with the magnified world") : "",
                 static_cast<unsigned>(GetTickCount64() - t0));
    if (TestHarness::Enabled() && g_worldValid) {
        // test mode: both pictures as the backdrop compares them (tools\out\harness\dialog_level.raw / _world.raw)
        const std::string dir = GameDir() + "tools\\out\\harness\\";
        auto dump = [&](const char* name, const std::vector<uint16_t>& px) {
            FILE* f = std::fopen((dir + name).c_str(), "wb");
            if (!f) return;
            const uint32_t hdr[2] = { static_cast<uint32_t>(g_worldW), static_cast<uint32_t>(g_worldH) };
            std::fwrite(hdr, sizeof(hdr), 1, f);
            std::fwrite(px.data(), 2, px.size(), f);
            std::fclose(f);
        };
        dump("dialog_level.raw", g_world);
        if (g_lupaValid) dump("dialog_world.raw", g_lupaWorld);
    }
}

int Describe(char* out, int size) {
    return std::snprintf(out, size, "variant=%s copies=%u hdCopies=%u keys=%u loaded=%u written=%u layer=%d key=%016llx",
                         g_variant.c_str(), g_copies, g_hdCopies, g_keys, g_loaded, g_dumps, g_anyLayer ? 1 : 0,
                         static_cast<unsigned long long>(g_src.key));
}

} // namespace DDM::HD::Menu
