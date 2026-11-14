#include "ddm_hd_menu.h"
#include "ddm_hd.h"
#include "../config.h"
#include "../logger.h"
#include "../ui_scale.h"
#include "../window_manager.h"

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
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
        bool valid = false;
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

    // collector
    std::unordered_set<uint64_t> g_written;
    bool g_writeFailed = false;

    // the backdrop of a dialog in a mission (see GetBackdrop)
    std::vector<uint32_t> g_backdrop;        // BGRA, alpha = level (255) or dialog picture (0)
    int      g_backdropW = 0, g_backdropH = 0;
    unsigned g_backdropGen = 0;
    Source   g_backdropSrc;
    bool     g_backdropValid = false;
    // the level on the screen when the dialog started (the backdrop is made from it)
    std::vector<uint16_t> g_world;
    int  g_worldW = 0, g_worldH = 0;
    bool g_worldValid = false;

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
        g_backdrop.resize(static_cast<size_t>(w) * h);
        const size_t n = static_cast<size_t>(w) * h;
        const bool haveWorld = g_worldValid && g_worldW == w && g_worldH == h;
        if (!haveWorld) {
            for (size_t i = 0; i < n; ++i) g_backdrop[i] = 0xFF000000u | To8888(comp[i]);
            return;
        }
        static std::vector<uint16_t> cand;
        static std::vector<int32_t> votes;
        cand.assign(65536, 0);
        votes.assign(65536, 0);
        for (size_t i = 0; i < n; ++i) {
            const uint16_t a = g_world[i], b = comp[i];
            int32_t& v = votes[a];
            if (v == 0) { cand[a] = b; v = 1; }
            else if (cand[a] == b) ++v;
            else --v;
        }
        size_t level = 0;
        for (size_t i = 0; i < n; ++i) {
            const uint16_t dark = cand[g_world[i]];
            if (comp[i] == dark) {
                g_backdrop[i] = 0xFF000000u | To8888(comp[i]);
                ++level;
            } else {
                g_backdrop[i] = To8888(dark);       // alpha 0: a picture of the dialog lies here
            }
        }
        // a backdrop that is not the darkened level at all (a menu picture): all enlarged
        if (level < n / 5) {
            for (size_t i = 0; i < n; ++i) g_backdrop[i] &= 0x00FFFFFFu;
        }
        Logger::Info("HD menu: dialog backdrop - %.0f %% darkened level (shown at its size), the rest enlarged with the dialog.",
                     100.0 * static_cast<double>(level) / static_cast<double>(n));
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
        ++g_generation;
    }

    void CheckVariant() {
        const char* v = DDM::HD::MenuVariantName();
        if (g_variant != v) {
            g_variant = v;
            g_pics.clear();
            if (!g_orig.empty()) {
                std::fill(g_orig.begin(), g_orig.end(), 0u);
                g_dirty = RECT{ 0, 0, g_cw, g_ch };
                g_anyDirty = true;
                g_anyLayer = false;
            }
        }
    }

    // hd\menu\<variant>\<key>.mhd: "MNHD", u32 1, u16 cropW, cropH, l, t, w, h, then 2w x 2h BGRA
    std::shared_ptr<Picture> Load(uint64_t key) {
        const auto it = g_pics.find(key);
        if (it != g_pics.end()) return it->second;
        std::shared_ptr<Picture> pic;
        if (g_variant != "off" && !g_variant.empty()) {
            char name[64];
            std::snprintf(name, sizeof(name), "%016llx.mhd", static_cast<unsigned long long>(key));
            const std::string path = GameDir() + "hd\\menu\\" + g_variant + "\\" + name;
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
            }
        }
        if (g_pics.size() > 64) g_pics.clear();     // a handful of menu screens; never grows without bound
        g_pics[key] = pic;
        return pic;
    }

    // hd\work\menu_frames\<key>.mnf: "MNUF", u32 1, u16 cropW, cropH, frameW, frameH, then the
    // shown part, RGB565 row by row
    void WriteForTool(uint64_t key, const Buf& src, const RECT& crop) {
        if (g_writeFailed || g_written.count(key) || g_written.size() > 500) return;
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
}

void OnScreenCopy(const Buf& src, unsigned serial, unsigned gen, unsigned genWrite, const RECT& s, int dx, int dy) {
    const auto& cfg = SherwoodConfig::Get();
    if (!UiScale::MenuZoomOn()) return;
    if (*reinterpret_cast<void* const*>(0x006C0FA8) != nullptr) {
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
        g_backdropValid = true;
        return;
    }
    g_backdropValid = false;
    if (!cfg.hdMenu) return;
    RECT crop{};
    if (!UiScale::MenuCropRect(src.w, src.h, crop)) return;
    ++g_copies;
    CheckVariant();

    // which picture: computed once per state of the source surface
    if (!g_src.valid || g_src.serial != serial || g_src.gen != gen || g_src.genWrite != genWrite) {
        g_src.serial = serial;
        g_src.gen = gen;
        g_src.genWrite = genWrite;
        g_src.key = KeyOf(src, crop);
        g_src.valid = true;
        ++g_keys;
    }
    const std::shared_ptr<Picture> pic = Load(g_src.key);
    if (!pic) WriteForTool(g_src.key, src, crop);

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
    const bool usable = pic && pic->cropW == g_cw && pic->cropH == g_ch;
    if (usable) ++g_hdCopies;
    for (int y = t; y < b; ++y) {
        const int sy = static_cast<int>(s.top) + (y - dy);
        const auto* sp = reinterpret_cast<const uint16_t*>(src.bits + static_cast<size_t>(sy) * src.pitch);
        const int cy = y - crop.top;
        uint32_t* orow = g_orig.data() + static_cast<size_t>(cy) * g_cw;
        for (int x = l; x < r; ++x) {
            const int cx = x - crop.left;
            const int px = cx - (usable ? pic->l : 0), py = cy - (usable ? pic->t : 0);
            if (usable && px >= 0 && py >= 0 && px < pic->w && py < pic->h) {
                const int sx = static_cast<int>(s.left) + (x - dx);
                orow[cx] = 0x10000u | sp[sx];
                const uint32_t* hp = pic->px.data() + static_cast<size_t>(py * 2) * (pic->w * 2) + static_cast<size_t>(px * 2);
                uint32_t* hrow = g_hd.data() + static_cast<size_t>(cy * 2) * (g_cw * 2) + static_cast<size_t>(cx * 2);
                hrow[0] = hp[0];
                hrow[1] = hp[1];
                hrow[g_cw * 2] = hp[pic->w * 2];
                hrow[g_cw * 2 + 1] = hp[pic->w * 2 + 1];
                g_anyLayer = true;
            } else {
                orow[cx] = 0;
            }
        }
    }
    AddDirty(l - crop.left, t - crop.top, r - crop.left, b - crop.top);
}

bool GetLayer(const RECT& crop, Layer& out) {
    if (!SherwoodConfig::Get().hdMenu || g_orig.empty() || !g_anyLayer) return false;
    if (crop.left != g_crop.left || crop.top != g_crop.top || crop.right != g_crop.right || crop.bottom != g_crop.bottom) return false;
    out.orig = g_orig.data();
    out.hd = g_hd.data();
    out.w = g_cw;
    out.h = g_ch;
    out.dirty = g_anyDirty ? g_dirty : RECT{ 0, 0, 0, 0 };
    out.generation = g_generation;
    g_anyDirty = false;
    return true;
}

bool GetBackdrop(Backdrop& out) {
    // only while a dialog of a mission is up (its loop runs) and its backdrop was copied
    if (!g_backdropValid || !UiScale::DialogLoopRunning() || *reinterpret_cast<void* const*>(0x006C0FA8) == nullptr ||
        g_backdrop.empty()) {
        return false;
    }
    out.bgra = g_backdrop.data();
    out.w = g_backdropW;
    out.h = g_backdropH;
    out.generation = g_backdropGen;
    return true;
}

void NoteDialogStart() {
    g_worldValid = false;
    if (!UiScale::MenuZoomOn() || *reinterpret_cast<void* const*>(0x006C0FA8) == nullptr) return;
    g_worldValid = DDM::CopyFront(g_world, g_worldW, g_worldH);
}

int Describe(char* out, int size) {
    return std::snprintf(out, size, "variant=%s copies=%u hdCopies=%u keys=%u loaded=%u written=%u layer=%d key=%016llx",
                         g_variant.c_str(), g_copies, g_hdCopies, g_keys, g_loaded, g_dumps, g_anyLayer ? 1 : 0,
                         static_cast<unsigned long long>(g_src.key));
}

} // namespace DDM::HD::Menu
