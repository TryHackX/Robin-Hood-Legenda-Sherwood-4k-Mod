#include "ddm_hd_patch.h"
#include "ddm.h"
#include "ddm_hd.h"
#include "ddm_hd_font.h"
#include "ddm_hd_menu.h"
#include "../config.h"
#include "../diag.h"
#include "../logger.h"
#include "../ui_scale.h"
#include "../window_manager.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

// See ddm_hd_patch.h.
namespace DDM::HD::Patch {
namespace {
    constexpr size_t kMaxShadows = 5;   // 21 MB each at 1440p (the screen's two storages, the dialog's surfaces)

    struct Shadow {
        const uint8_t* bits = nullptr;   // the surface's pixel storage (the flip chain rotates storages)
        int w = 0, h = 0;
        RECT crop{};                     // the part kept (surface pixels)
        int cw = 0, ch = 0;
        std::vector<uint32_t> ref;       // cw x ch: 0, or 0x10000 | the frame pixel the HD block belongs to
        std::unique_ptr<uint32_t[]> hd;  // 2cw x 2ch BGRA - not cleared: only blocks with a ref are ever shown
        RECT dirty{};
        bool anyDirty = false;
        uintptr_t identity = 0;
        unsigned lastUse = 0;
    };
    std::vector<std::unique_ptr<Shadow>> g_shadows;
    unsigned  g_use = 0;
    uintptr_t g_nextIdentity = 1;
    bool      g_cursor = false;

    // time spent here (QPC ticks), for the presenter's hitch report
    long long g_spent = 0;
    struct Timed {
        LARGE_INTEGER t0{};
        Timed() { QueryPerformanceCounter(&t0); }
        ~Timed() {
            LARGE_INTEGER t1{};
            QueryPerformanceCounter(&t1);
            g_spent += t1.QuadPart - t0.QuadPart;
        }
    };
    // surfaces a dialog is being composed in (got a whole copy of the screen)
    std::vector<const uint8_t*> g_comp;

    bool Composing(const uint8_t* bits) {
        return std::find(g_comp.begin(), g_comp.end(), bits) != g_comp.end();
    }

    bool BaseOn() {
        return SherwoodConfig::Get().hdMenu && UiScale::MenuZoomOn() && Diag::IsMainThread();
    }

    // patches into this surface now?
    bool ActiveFor(const uint8_t* bits) {
        return BaseOn() && (UiScale::DialogLoopRunning() || Composing(bits));
    }

    // upscaled pictures (buttons, the briefing's parchment, portraits ...)
    struct Picture {
        int w2 = 0, h2 = 0;
        std::vector<uint32_t> bgra;      // colour, alpha = how much of it covers (0..255)
        std::vector<uint8_t>  shade;     // how much of the pixel is the darkening shadow (0..255)
    };
    struct PicSeen {
        uint32_t probe = 0;
        uint64_t key = 0;
        const Picture* pic = nullptr;
    };
    std::unordered_map<uint64_t, std::unique_ptr<Picture>> g_pics;    // by content key; null: no file
    std::unordered_map<uint64_t, PicSeen> g_picSeen;                  // by place (pixels + rectangle)

    unsigned g_glyphs = 0, g_glyphsNoHd = 0, g_pictures = 0, g_picturesNoHd = 0, g_copies = 0, g_created = 0, g_resets = 0;
    unsigned g_picLoaded = 0, g_picCollected = 0;

    std::string GameDir() {
        char exe[MAX_PATH]{};
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        std::string path(exe);
        path.erase(path.find_last_of("\\/") + 1);
        return path;
    }

    bool FrameSized(const Buf& b) {
        int fw = 0, fh = 0;
        WindowManager::GetInternalResolution(fw, fh);
        return b.bits && b.w == fw && b.h == fh;
    }

    inline uint32_t To8888(uint32_t p) {
        uint32_t r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
        r = (r << 3) | (r >> 2);
        g = (g << 2) | (g >> 4);
        b = (b << 3) | (b >> 2);
        return 0xFF000000u | (r << 16) | (g << 8) | b;
    }

    // c over bg with a (0..255)
    inline uint32_t Mix(uint32_t bg, uint32_t c, uint32_t a) {
        if (a >= 255) return 0xFF000000u | c;
        if (a == 0) return bg;
        const uint32_t ia = 255 - a;
        const uint32_t r = (((c >> 16) & 0xFF) * a + ((bg >> 16) & 0xFF) * ia + 127) / 255;
        const uint32_t g = (((c >> 8) & 0xFF) * a + ((bg >> 8) & 0xFF) * ia + 127) / 255;
        const uint32_t b = ((c & 0xFF) * a + (bg & 0xFF) * ia + 127) / 255;
        return 0xFF000000u | (r << 16) | (g << 8) | b;
    }

    // bg darkened like the game's shadow pixels (dst * factor / 256), s = how much of the pixel is shadow
    inline uint32_t Shade(uint32_t bg, uint32_t s, uint32_t factor) {
        if (s == 0) return bg;
        const uint32_t m = 256 - ((256 - (std::min)(factor, 256u)) * s + 127) / 255;   // of 256
        const uint32_t r = (((bg >> 16) & 0xFF) * m) >> 8, g = (((bg >> 8) & 0xFF) * m) >> 8, b = ((bg & 0xFF) * m) >> 8;
        return 0xFF000000u | (r << 16) | (g << 8) | b;
    }

    void AddDirty(Shadow& s, int l, int t, int r, int b) {
        if (r <= l || b <= t) return;
        if (!s.anyDirty) {
            s.dirty = RECT{ l, t, r, b };
            s.anyDirty = true;
        } else {
            s.dirty.left = (std::min)(s.dirty.left, static_cast<LONG>(l));
            s.dirty.top = (std::min)(s.dirty.top, static_cast<LONG>(t));
            s.dirty.right = (std::max)(s.dirty.right, static_cast<LONG>(r));
            s.dirty.bottom = (std::max)(s.dirty.bottom, static_cast<LONG>(b));
        }
    }

    Shadow* Find(const uint8_t* bits) {
        for (auto& s : g_shadows) {
            if (s->bits == bits) {
                s->lastUse = ++g_use;
                return s.get();
            }
        }
        return nullptr;
    }

    Shadow* Create(const Buf& b, const Shadow* keep) {
        RECT crop{};
        if (!UiScale::MenuCropRect(b.w, b.h, crop)) return nullptr;
        if (g_shadows.size() >= kMaxShadows) {
            size_t victim = g_shadows.size();
            for (size_t i = 0; i < g_shadows.size(); ++i) {
                if (g_shadows[i].get() == keep) continue;
                if (victim == g_shadows.size() || g_shadows[i]->lastUse < g_shadows[victim]->lastUse) victim = i;
            }
            if (victim == g_shadows.size()) return nullptr;
            g_shadows.erase(g_shadows.begin() + static_cast<ptrdiff_t>(victim));
        }
        auto s = std::make_unique<Shadow>();
        s->bits = b.bits;
        s->w = b.w;
        s->h = b.h;
        s->crop = crop;
        s->cw = crop.right - crop.left;
        s->ch = crop.bottom - crop.top;
        if (s->cw <= 0 || s->ch <= 0) return nullptr;
        // (the HD part is not cleared - 17 MB at 1440p, a hitch when a dialog opens - and only what was
        // drawn is marked: the presenter sends a new shadow's refs whole and of its HD only that part)
        s->ref.assign(static_cast<size_t>(s->cw) * s->ch, 0u);
        s->hd.reset(new (std::nothrow) uint32_t[static_cast<size_t>(s->cw) * s->ch * 4]);
        if (!s->hd) return nullptr;
        s->identity = g_nextIdentity++;
        s->lastUse = ++g_use;
        ++g_created;
        g_shadows.push_back(std::move(s));
        return g_shadows.back().get();
    }

    uint64_t Fnv(const Buf& b, const RECT& r) {
        uint64_t h = 1469598103934665603ull;
        for (int y = r.top; y < r.bottom; ++y) {
            const uint8_t* p = b.bits + static_cast<size_t>(y) * b.pitch + static_cast<size_t>(r.left) * 2;
            for (int x = 0; x < (r.right - r.left) * 2; ++x) h = (h ^ p[x]) * 1099511628211ull;
        }
        h ^= (static_cast<uint64_t>(r.right - r.left) << 40) ^ (static_cast<uint64_t>(r.bottom - r.top) << 20);
        return h * 1099511628211ull;
    }

    uint32_t Probe(const Buf& b, const RECT& r) {
        const int w = r.right - r.left, h = r.bottom - r.top;
        uint32_t v = 2166136261u;
        for (int i = 0; i < 48; ++i) {
            const int x = r.left + (i * 977) % w, y = r.top + (i * 131) % h;
            v = (v ^ reinterpret_cast<const uint16_t*>(b.bits + static_cast<size_t>(y) * b.pitch)[x]) * 16777619u;
        }
        return v;
    }

    const Picture* LoadPicture(uint64_t key, int w, int h) {
        auto it = g_pics.find(key);
        if (it != g_pics.end()) return it->second.get();
        char name[64];
        std::snprintf(name, sizeof(name), "%016llx.hdp", static_cast<unsigned long long>(key));
        const std::string path = GameDir() + "hd\\menu_parts\\" + name;
        std::unique_ptr<Picture> p;
        HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            struct { char magic[4]; uint32_t version, w, h; } hdr{};
            DWORD got = 0;
            if (ReadFile(f, &hdr, sizeof(hdr), &got, nullptr) && got == sizeof(hdr) && std::memcmp(hdr.magic, "HDMP", 4) == 0 &&
                hdr.version == 1 && static_cast<int>(hdr.w) == 2 * w && static_cast<int>(hdr.h) == 2 * h) {
                p = std::make_unique<Picture>();
                p->w2 = static_cast<int>(hdr.w);
                p->h2 = static_cast<int>(hdr.h);
                const size_t n = static_cast<size_t>(p->w2) * p->h2;
                p->bgra.resize(n);
                p->shade.resize(n);
                DWORD a = 0, b = 0;
                if (!ReadFile(f, p->bgra.data(), static_cast<DWORD>(n * 4), &a, nullptr) || a != n * 4 ||
                    !ReadFile(f, p->shade.data(), static_cast<DWORD>(n), &b, nullptr) || b != n) {
                    p.reset();
                }
            }
            CloseHandle(f);
            if (p) {
                ++g_picLoaded;
                Logger::Info("HD menu parts: %s (%dx%d) for a picture of %dx%d.", name, p->w2, p->h2, w, h);
            } else {
                Logger::Warn("HD menu parts: %s could not be read.", name);
            }
        }
        return (g_pics[key] = std::move(p)).get();
    }

    // a picture without an HD version: kept for tools\hd\upscale_menu_parts.py (like the fonts) -
    // only where the HD work folder exists
    void Collect(uint64_t key, const Buf& src, const RECT& r, uint32_t colourKey, uint32_t marker) {
        static const std::string work = GameDir() + "hd\\work\\";
        static const bool collect = GetFileAttributesA(work.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (!collect) return;
        const std::string dir = work + "menu_parts\\";
        char name[64];
        std::snprintf(name, sizeof(name), "part_%016llx.raw", static_cast<unsigned long long>(key));
        const std::string path = dir + name;
        if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) return;
        // The credits draw their scrolling text as one picture 400 wide that grows every frame: 5160
        // files, 5.5 GB on 2026-11-15/16. A picture of the same width but another height than one
        // collected in the last two seconds is such an animation (two seals side by side have the
        // same size: both are kept), and a session collects at most 300 pictures.
        static int s_collected = 0;
        static LONG s_lastW = 0, s_lastH = 0;
        static DWORD s_lastAt = 0;
        const LONG pw = r.right - r.left, ph = r.bottom - r.top;
        const DWORD now = GetTickCount();
        const bool animation = pw == s_lastW && ph != s_lastH && now - s_lastAt < 2000;
        s_lastW = pw;
        s_lastH = ph;
        s_lastAt = now;
        if (animation || s_collected >= 300) return;
        ++s_collected;
        CreateDirectoryA(dir.c_str(), nullptr);
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) return;
        const uint32_t hdr[5] = { 0x504D4253u /* "SBMP" */, static_cast<uint32_t>(r.right - r.left),
                                  static_cast<uint32_t>(r.bottom - r.top), colourKey, marker };
        std::fwrite(hdr, sizeof(hdr), 1, f);
        for (int y = r.top; y < r.bottom; ++y) {
            std::fwrite(src.bits + static_cast<size_t>(y) * src.pitch + static_cast<size_t>(r.left) * 2, 2,
                        static_cast<size_t>(r.right - r.left), f);
        }
        std::fclose(f);
        ++g_picCollected;
        Logger::Info("HD menu parts: picture %ldx%ld collected as %s.", r.right - r.left, r.bottom - r.top, name);
    }

    // the HD version of rect `r` of `src` (recognised by content, once per place and content)
    const Picture* FindPicture(const Buf& src, const RECT& r, uint32_t colourKey, uint32_t marker) {
        const int w = r.right - r.left, h = r.bottom - r.top;
        if (w < 4 || h < 4 || static_cast<int64_t>(w) * h > 800 * 800) return nullptr;
        const uint64_t place = (reinterpret_cast<uintptr_t>(src.bits) * 0x9E3779B97F4A7C15ull) ^
                               (static_cast<uint64_t>(r.left) << 48) ^ (static_cast<uint64_t>(r.top) << 32) ^
                               (static_cast<uint64_t>(w) << 16) ^ static_cast<uint64_t>(h);
        PicSeen& seen = g_picSeen[place];
        const uint32_t probe = Probe(src, r);
        if (seen.key == 0 || seen.probe != probe) {
            seen.key = Fnv(src, r);
            seen.probe = probe;
            seen.pic = LoadPicture(seen.key, w, h);
            if (!seen.pic) Collect(seen.key, src, r, colourKey, marker);
        }
        return seen.pic;
    }

    // the pixels of the destination rectangle before the game draws (only what the shadow covers matters)
    void KeepBefore(Op& op) {
        op.before.resize(static_cast<size_t>(op.w) * op.h);
        for (int y = 0; y < op.h; ++y) {
            std::memcpy(op.before.data() + static_cast<size_t>(y) * op.w,
                        op.dst.bits + static_cast<size_t>(op.y + y) * op.dst.pitch + static_cast<size_t>(op.x) * 2,
                        static_cast<size_t>(op.w) * 2);
        }
    }

    // the destination rectangle inside the surface and touching the menu zoom's part
    bool Place(Op& op) {
        if (op.x < 0 || op.y < 0 || op.w <= 0 || op.h <= 0) return false;
        if (op.x + op.w > op.dst.w) op.w = op.dst.w - op.x;
        if (op.y + op.h > op.dst.h) op.h = op.dst.h - op.y;
        if (op.w <= 0 || op.h <= 0) return false;
        RECT crop{};
        if (!UiScale::MenuCropRect(op.dst.w, op.dst.h, crop)) return false;
        return op.x < crop.right && op.y < crop.bottom && op.x + op.w > crop.left && op.y + op.h > crop.top;
    }

    // what the drawn thing contributes to HD sub-pixel (i, j) of its 1x pixel (px, py)
    struct Sub {
        uint32_t colour = 0;    // BGRA (alpha ignored)
        uint32_t a = 0;         // coverage 0..255
        uint32_t shade = 0;     // shadow 0..255
    };
    inline Sub SubOf(const Op& op, int px, int py, int i, int j) {
        Sub s;
        if (op.kind == 1) {
            const int hx = op.hx + 2 * px + i, hy = op.hy + 2 * py + j;
            if (hx >= 0 && hy >= 0 && hx < op.hdW && hy < op.hdH) {
                const size_t k = static_cast<size_t>(hy) * op.hdW + hx;
                s.colour = To8888(op.hdColour[k]);
                s.a = op.hdAlpha[k];
            }
        } else if (op.kind == 2) {
            const auto* p = static_cast<const Picture*>(op.picture);
            const size_t k = static_cast<size_t>(2 * py + j) * p->w2 + static_cast<size_t>(2 * px + i);
            s.colour = p->bgra[k];
            s.a = p->bgra[k] >> 24;
            s.shade = p->shade[k];
        }
        return s;
    }
}

bool Active() {
    return BaseOn() && UiScale::DialogLoopRunning();
}

// ---- text drawn by GDI ---------------------------------------------------------------------------
namespace {
    using PFN_TextOutA = BOOL(WINAPI*)(HDC, int, int, LPCSTR, int);
    using PFN_TextOutW = BOOL(WINAPI*)(HDC, int, int, LPCWSTR, int);
    PFN_TextOutA o_TextOutA = nullptr;
    PFN_TextOutW o_TextOutW = nullptr;
    unsigned g_gdiTexts = 0, g_gdiTextsHd = 0;

    // the game's fonts twice as large and smoothed (by the LOGFONT they were made from)
    struct BigFont {
        LOGFONTA lf{};
        HFONT font = nullptr;
    };
    std::vector<BigFont> g_bigFonts;

    HFONT Bigger(HDC dc) {
        LOGFONTA lf{};
        const HGDIOBJ cur = GetCurrentObject(dc, OBJ_FONT);
        if (!cur || GetObjectA(cur, sizeof(lf), &lf) == 0) return nullptr;
        for (const BigFont& b : g_bigFonts) {
            if (std::memcmp(&b.lf, &lf, sizeof(lf)) == 0) return b.font;
        }
        LOGFONTA big = lf;
        big.lfHeight *= 2;
        big.lfWidth *= 2;
        big.lfQuality = ANTIALIASED_QUALITY;
        const HFONT f = CreateFontIndirectA(&big);
        if (!f) return nullptr;
        if (g_bigFonts.size() >= 16) {
            DeleteObject(g_bigFonts.front().font);
            g_bigFonts.erase(g_bigFonts.begin());
        }
        g_bigFonts.push_back(BigFont{ lf, f });
        return f;
    }

    // scratch picture for the text twice as large (32 bits, top-down)
    HDC      g_tdc = nullptr;
    HBITMAP  g_tbm = nullptr;
    uint32_t* g_tbits = nullptr;
    int      g_tw = 0, g_th = 0;
    std::vector<uint16_t> g_tcol;
    std::vector<uint8_t>  g_tcov;

    bool Scratch(int w, int h) {
        if (w <= 0 || h <= 0 || w > 4096 || h > 1024) return false;
        if (g_tbm && w <= g_tw && h <= g_th) return true;
        if (!g_tdc) g_tdc = CreateCompatibleDC(nullptr);
        if (!g_tdc) return false;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = (std::max)(w, g_tw);
        bi.bmiHeader.biHeight = -(std::max)(h, g_th);
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        const HBITMAP bm = CreateDIBSection(g_tdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bm) return false;
        SelectObject(g_tdc, bm);
        if (g_tbm) DeleteObject(g_tbm);
        g_tbm = bm;
        g_tbits = static_cast<uint32_t*>(bits);
        g_tw = bi.bmiHeader.biWidth;
        g_th = -bi.bmiHeader.biHeight;
        return true;
    }

    inline uint16_t From8888(COLORREF c) {
        const uint32_t r = GetRValue(c), g = GetGValue(c), b = GetBValue(c);
        return static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }

    template <typename Ch, typename Orig, typename Extent, typename Out>
    BOOL TextHook(HDC dc, int x, int y, const Ch* s, int n, Orig orig, Extent extent, Out textOut) {
        Buf b{};
        if (!s || n <= 0 || g_cursor || !BaseOn() || !SurfaceOfDC(dc, b)) return orig(dc, x, y, s, n);
        ++g_gdiTexts;
        // where it lands (MM_TEXT: logical = surface pixels)
        SIZE sz{};
        TEXTMETRICA tm{};
        if (!extent(dc, s, n, &sz) || !GetTextMetricsA(dc, &tm)) return orig(dc, x, y, s, n);
        const UINT align = GetTextAlign(dc);
        int left = x, top = y;
        if ((align & TA_CENTER) == TA_CENTER) left = x - sz.cx / 2;
        else if (align & TA_RIGHT) left = x - sz.cx;
        if ((align & TA_BASELINE) == TA_BASELINE) top = y - tm.tmAscent;
        else if (align & TA_BOTTOM) top = y - sz.cy;
        // room for parts outside the advance box (the tail of "j" runs under the letter before it)
        const int mx = 3 + tm.tmOverhang + tm.tmAveCharWidth / 2, my = 3 + tm.tmDescent / 2;
        RECT r{ left - mx, top - my, left + sz.cx + mx, top + sz.cy + my };
        RECT clip{};
        const int cr = GetClipBox(dc, &clip);
        if (cr == NULLREGION) return orig(dc, x, y, s, n);
        if (cr != ERROR && !IntersectRect(&r, &r, &clip)) return orig(dc, x, y, s, n);
        if (TraceActive()) {
            char text[220];
            int k = std::snprintf(text, sizeof(text), "TEXT surface %dx%d at (%d,%d) box (%ld,%ld,%ld,%ld) align=%u bk=%d '", b.w, b.h, x, y,
                                  r.left, r.top, r.right, r.bottom, align, GetBkMode(dc));
            for (int i = 0; i < n && k < 200; ++i) text[k++] = static_cast<char>(s[i] < 128 ? s[i] : '?');
            if (k < 218) { text[k++] = '\''; text[k] = 0; } else text[219] = 0;
            TraceMark(text);
        }
        static Op op;
        op.Clear();
        op.dst = b;
        op.x = r.left;
        op.y = r.top;
        op.w = r.right - r.left;
        op.h = r.bottom - r.top;
        const bool patch = FrameSized(b) && ActiveFor(b.bits) && Place(op);
        if (patch) KeepBefore(op);
        const BOOL ok = orig(dc, x, y, s, n);
        if (!patch || !ok) return ok;
        GdiFlush();
        // the same text twice as large, white on black: its coverage
        const int w2 = op.w * 2, h2 = op.h * 2;
        const HFONT big = Bigger(dc);
        if (!big || !Scratch(w2, h2)) return ok;
        for (int yy = 0; yy < h2; ++yy) std::memset(g_tbits + static_cast<size_t>(yy) * g_tw, 0, static_cast<size_t>(w2) * 4);
        const HGDIOBJ oldFont = SelectObject(g_tdc, big);
        SetTextColor(g_tdc, RGB(255, 255, 255));
        SetBkMode(g_tdc, TRANSPARENT);
        SetTextAlign(g_tdc, align & ~TA_UPDATECP);
        textOut(g_tdc, 2 * (x - op.x), 2 * (y - op.y), s, n);
        GdiFlush();
        SelectObject(g_tdc, oldFont);
        g_tcol.assign(static_cast<size_t>(w2) * h2, From8888(GetTextColor(dc)));
        g_tcov.resize(static_cast<size_t>(w2) * h2);
        // the game's small text is drawn without smoothing and looks bolder than smoothed letters of the same
        // font: coverage through a gamma of 0.75 gives them back that weight
        static uint8_t s_gamma[256];
        static bool s_gammaReady = false;
        if (!s_gammaReady) {
            for (int v = 0; v < 256; ++v) s_gamma[v] = static_cast<uint8_t>(std::pow(v / 255.0, 0.75) * 255.0 + 0.5);
            s_gammaReady = true;
        }
        for (int yy = 0; yy < h2; ++yy) {
            const uint32_t* row = g_tbits + static_cast<size_t>(yy) * g_tw;
            uint8_t* cov = g_tcov.data() + static_cast<size_t>(yy) * w2;
            for (int xx = 0; xx < w2; ++xx) cov[xx] = s_gamma[(row[xx] >> 8) & 0xFF];
        }
        if (GetBkMode(dc) == OPAQUE) {
            // the game filled the text's cell with its background colour: that is part of the drawing
            const uint16_t bk = From8888(GetBkColor(dc)), fg = From8888(GetTextColor(dc));
            const int bx0 = 2 * (left - op.x), by0 = 2 * (top - op.y);
            for (int yy = (std::max)(0, by0); yy < (std::min)(h2, by0 + 2 * static_cast<int>(sz.cy)); ++yy) {
                for (int xx = (std::max)(0, bx0); xx < (std::min)(w2, bx0 + 2 * static_cast<int>(sz.cx)); ++xx) {
                    const size_t i = static_cast<size_t>(yy) * w2 + xx;
                    const uint32_t a = g_tcov[i];
                    const uint32_t m = (To8888(bk) & 0xFFFFFF);
                    const uint32_t t = (To8888(fg) & 0xFFFFFF);
                    const uint32_t mixed = Mix(0xFF000000u | m, t, a);
                    g_tcol[i] = static_cast<uint16_t>((((mixed >> 19) & 0x1F) << 11) | (((mixed >> 10) & 0x3F) << 5) | ((mixed >> 3) & 0x1F));
                    g_tcov[i] = 255;
                }
            }
        }
        op.kind = 1;
        op.glyph = true;
        op.hdColour = g_tcol.data();
        op.hdAlpha = g_tcov.data();
        op.hdW = w2;
        op.hdH = h2;
        op.hx = 0;
        op.hy = 0;
        End(op);
        ++g_gdiTextsHd;
        return ok;
    }

    BOOL WINAPI Hook_TextOutA(HDC dc, int x, int y, LPCSTR s, int n) {
        return TextHook(dc, x, y, s, n, o_TextOutA, &GetTextExtentPoint32A, &TextOutA);
    }

    BOOL WINAPI Hook_TextOutW(HDC dc, int x, int y, LPCWSTR s, int n) {
        return TextHook(dc, x, y, s, n, o_TextOutW, &GetTextExtentPoint32W, &TextOutW);
    }

    bool PatchSlot(uintptr_t slot, void* replacement, void** original) {
        auto** p = reinterpret_cast<void**>(slot);
        DWORD oldProt = 0;
        if (!*p || !VirtualProtect(p, sizeof(void*), PAGE_READWRITE, &oldProt)) return false;
        if (original) *original = *p;
        *p = replacement;
        VirtualProtect(p, sizeof(void*), oldProt, &oldProt);
        return true;
    }
}

bool InstallGdiText() {
    // Game.exe import slots (GOG build): GDI32!TextOutA / TextOutW
    constexpr uintptr_t kIatTextOutA = 0x00677068, kIatTextOutW = 0x0067707C;
    const HMODULE gdi = GetModuleHandleA("gdi32.dll");
    if (!gdi) return false;
    const void* realA = reinterpret_cast<const void*>(GetProcAddress(gdi, "TextOutA"));
    const void* realW = reinterpret_cast<const void*>(GetProcAddress(gdi, "TextOutW"));
    // the slots must hold what the import table says (another hook or a different build: left alone)
    auto slotOk = [](uintptr_t slot, const void* real) {
        const void* cur = *reinterpret_cast<void* const*>(slot);
        return cur && (cur == real || reinterpret_cast<uintptr_t>(cur) > 0x70000000u);
    };
    if (!slotOk(kIatTextOutA, realA) || !slotOk(kIatTextOutW, realW)) return false;
    return PatchSlot(kIatTextOutA, reinterpret_cast<void*>(&Hook_TextOutA), reinterpret_cast<void**>(&o_TextOutA)) &&
           PatchSlot(kIatTextOutW, reinterpret_cast<void*>(&Hook_TextOutW), reinterpret_cast<void**>(&o_TextOutW));
}

void SetCursorDrawing(bool on) {
    g_cursor = on;
}

void Reset() {
    if (!g_shadows.empty()) ++g_resets;
    g_shadows.clear();
    g_picSeen.clear();
    g_comp.clear();
}

void OnCopy(const Buf& dst, int dx, int dy, const Buf& src, const RECT& s, bool dstIsScreen, bool srcIsScreen) {
    const Timed timed;
    if (!FrameSized(dst)) return;
    if (!FrameSized(src)) {
        // from a small surface - the background under the cursor put back, or a picture: where the
        // pixel is now what a patch was made for, the patch stays (the cursor moved on); any other
        // patch there is gone for good (else it could match again by chance later: black dots)
        if (g_shadows.empty()) return;
        Shadow* d = Find(dst.bits);
        if (!d) return;
        const RECT& c = d->crop;
        const int l = (std::max)(static_cast<int>(c.left), dx), t = (std::max)(static_cast<int>(c.top), dy);
        const int r = (std::min)(static_cast<int>(c.right), dx + static_cast<int>(s.right - s.left));
        const int b = (std::min)(static_cast<int>(c.bottom), dy + static_cast<int>(s.bottom - s.top));
        bool changed = false;
        for (int y = t; y < b; ++y) {
            uint32_t* rr = d->ref.data() + static_cast<size_t>(y - c.top) * d->cw;
            const auto* px = reinterpret_cast<const uint16_t*>(dst.bits + static_cast<size_t>(y) * dst.pitch);
            for (int x = l; x < r; ++x) {
                uint32_t& ref = rr[x - c.left];
                if (ref && ref != (0x10000u | px[x])) {
                    ref = 0;
                    changed = true;
                }
            }
        }
        if (changed) AddDirty(*d, l - c.left, t - c.top, r - c.left, b - c.top);
        return;
    }
    const bool whole = dx == 0 && dy == 0 && s.left == 0 && s.top == 0 && s.right == src.w && s.bottom == src.h;
    if (whole && BaseOn()) {
        if (srcIsScreen && !dstIsScreen) {
            // a dialog is being composed in `dst`
            if (!Composing(dst.bits)) {
                if (g_comp.size() >= 4) g_comp.erase(g_comp.begin());
                g_comp.push_back(dst.bits);
            }
        } else if (dstIsScreen && !Composing(src.bits) && !UiScale::DialogLoopRunning()) {
            g_comp.clear();     // a mission frame: no dialog being set up
        }
    }
    if (g_shadows.empty() && !ActiveFor(dst.bits)) return;
    Shadow* d = Find(dst.bits);
    Shadow* from = src.bits == dst.bits ? d : Find(src.bits);
    if (!d) {
        if (!from) return;
        d = Create(dst, from);
        if (!d) return;
    }
    ++g_copies;
    const RECT& c = d->crop;
    const int sw = s.right - s.left, sh = s.bottom - s.top;
    const int l = (std::max)(static_cast<int>(c.left), dx), t = (std::max)(static_cast<int>(c.top), dy);
    const int r = (std::min)(static_cast<int>(c.right), dx + sw), b = (std::min)(static_cast<int>(c.bottom), dy + sh);
    if (r <= l || b <= t) return;
    const int ox = static_cast<int>(s.left) - dx, oy = static_cast<int>(s.top) - dy;    // source = destination + o
    const int dw2 = d->cw * 2;
    // rows in the order that keeps an overlapping copy within one storage right
    const bool up = from == d && oy < 0;
    for (int k = 0; k < b - t; ++k) {
        const int y = up ? b - 1 - k : t + k;
        uint32_t* rr = d->ref.data() + static_cast<size_t>(y - c.top) * d->cw;
        const int sy = y + oy;
        int xl = r, xr = r;     // part of the row whose source lies in the source shadow
        if (from && sy >= from->crop.top && sy < from->crop.bottom) {
            xl = (std::max)(l, static_cast<int>(from->crop.left) - ox);
            xr = (std::min)(r, static_cast<int>(from->crop.right) - ox);
            if (xr < xl) xl = xr = r;
        }
        if (xr > xl) {
            const int n = xr - xl;
            const uint32_t* fr = from->ref.data() + static_cast<size_t>(sy - from->crop.top) * from->cw + (xl + ox - from->crop.left);
            std::memmove(rr + (xl - c.left), fr, static_cast<size_t>(n) * 4);
            const int fw2 = from->cw * 2;
            for (int j = 0; j < 2; ++j) {
                uint32_t* hr = d->hd.get() + static_cast<size_t>((y - c.top) * 2 + j) * dw2 + static_cast<size_t>((xl - c.left) * 2);
                const uint32_t* fh = from->hd.get() + static_cast<size_t>((sy - from->crop.top) * 2 + j) * fw2 +
                                     static_cast<size_t>((xl + ox - from->crop.left) * 2);
                std::memmove(hr, fh, static_cast<size_t>(n) * 8);
            }
        }
        // (after the copy: within one storage the source may lie there)
        for (int x = l; x < xl; ++x) rr[x - c.left] = 0;
        for (int x = (std::max)(xr, l); x < r; ++x) rr[x - c.left] = 0;
    }
    AddDirty(*d, l - c.left, t - c.top, r - c.left, b - c.top);
}

void OnOverwrite(const Buf& dst, const RECT& rc) {
    const Timed timed;
    if (g_shadows.empty()) return;
    Shadow* d = Find(dst.bits);
    if (!d) return;
    const RECT& c = d->crop;
    const int l = (std::max)(c.left, rc.left), t = (std::max)(c.top, rc.top);
    const int r = (std::min)(c.right, rc.right), b = (std::min)(c.bottom, rc.bottom);
    if (r <= l || b <= t) return;
    for (int y = t; y < b; ++y) {
        uint32_t* rr = d->ref.data() + static_cast<size_t>(y - c.top) * d->cw;
        std::fill(rr + (l - c.left), rr + (r - c.left), 0u);
    }
    AddDirty(*d, l - c.left, t - c.top, r - c.left, b - c.top);
}

bool BeginAlphaGlyph(const uint16_t* src, uint16_t* dst, const uint16_t* alpha, int w, int h, Op& op) {
    const Timed timed;
    if (g_cursor || !BaseOn() || !src || !dst || !alpha) return false;
    Buf db{}, sb{}, ab{};
    if (!FindLockedBuf(dst, db) || !FrameSized(db) || !ActiveFor(db.bits) || !FindLockedBuf(src, sb) || !FindLockedBuf(alpha, ab)) return false;
    const size_t off = static_cast<size_t>(reinterpret_cast<const uint8_t*>(dst) - db.bits);
    op.dst = db;
    op.x = static_cast<int>((off % static_cast<size_t>(db.pitch)) / 2);
    op.y = static_cast<int>(off / static_cast<size_t>(db.pitch));
    op.w = w;
    op.h = h;
    if (!Place(op)) return false;
    Font::HdGlyph g;
    if (Font::FindAlphaGlyph(src, alpha, g)) {
        op.kind = 1;
        op.hdColour = g.colour;
        op.hdAlpha = g.alpha;
        op.hdW = g.w;
        op.hdH = g.h;
        op.hx = g.hx;
        op.hy = g.hy;
    } else {
        if (!Find(db.bits)) return false;
        op.kind = 3;
    }
    op.glyph = true;
    op.src = src;
    op.srcPitch = sb.pitch;
    op.alpha = alpha;
    op.alphaPitch = ab.pitch;
    op.key = 0x07C0;
    KeepBefore(op);
    return true;
}

bool BeginKeyed(const Buf& dst, int dx, int dy, const Buf& src, const RECT& s, uint16_t key, Op& op) {
    const Timed timed;
    if (g_cursor || !FrameSized(dst) || src.bits == dst.bits || !ActiveFor(dst.bits)) return false;
    op.dst = dst;
    op.x = dx;
    op.y = dy;
    op.w = s.right - s.left;
    op.h = s.bottom - s.top;
    if (!Place(op) || op.w != s.right - s.left || op.h != s.bottom - s.top) return false;   // cut off: left alone
    Font::HdGlyph g;
    if (Font::FindKeyedGlyph(src, s, g)) {
        op.kind = 1;
        op.glyph = true;
        op.hdColour = g.colour;
        op.hdAlpha = g.alpha;
        op.hdW = g.w;
        op.hdH = g.h;
        op.hx = g.hx;
        op.hy = g.hy;
    } else if (const Picture* p = FindPicture(src, s, key, 0xFFFFFFFFu)) {
        op.kind = 2;
        op.picture = p;
    } else {
        if (!Find(dst.bits)) return false;
        op.kind = 3;
    }
    op.src = reinterpret_cast<const uint16_t*>(src.bits + static_cast<size_t>(s.top) * src.pitch) + s.left;
    op.srcPitch = src.pitch;
    op.key = key;
    KeepBefore(op);
    return true;
}

bool BeginShadowPicture(const uint16_t* src, uint16_t* dst, int w, int h, uint32_t factor, uint32_t marker, bool keyed, Op& op) {
    const Timed timed;
    if (g_cursor || !BaseOn() || !src || !dst) return false;
    Buf db{}, sb{};
    if (!FindLockedBuf(dst, db) || !FrameSized(db) || !ActiveFor(db.bits) || !FindLockedBuf(src, sb) || sb.bits == db.bits) return false;
    const size_t off = static_cast<size_t>(reinterpret_cast<const uint8_t*>(dst) - db.bits);
    op.dst = db;
    op.x = static_cast<int>((off % static_cast<size_t>(db.pitch)) / 2);
    op.y = static_cast<int>(off / static_cast<size_t>(db.pitch));
    op.w = w;
    op.h = h;
    if (!Place(op) || op.w != w || op.h != h) return false;
    const size_t soff = static_cast<size_t>(reinterpret_cast<const uint8_t*>(src) - sb.bits);
    const int sx = static_cast<int>((soff % static_cast<size_t>(sb.pitch)) / 2), sy = static_cast<int>(soff / static_cast<size_t>(sb.pitch));
    if (sx + w > sb.w || sy + h > sb.h) return false;
    const RECT r{ sx, sy, sx + w, sy + h };
    if (const Picture* p = FindPicture(sb, r, keyed ? 0x07C0u : 0xFFFFFFFFu, marker & 0xFFFF)) {
        op.kind = 2;
        op.picture = p;
    } else {
        if (!Find(db.bits)) return false;
        op.kind = 3;
    }
    op.src = src;
    op.srcPitch = sb.pitch;
    op.key = keyed ? 0x07C0u : 0xFFFFFFFFu;
    op.marker = marker & 0xFFFF;
    op.factor = factor;
    KeepBefore(op);
    return true;
}

void End(Op& op) {
    const Timed timed;
    if (op.kind == 0) return;
    Shadow* sh = Find(op.dst.bits);
    if (!sh) {
        if (op.kind == 3) return;
        sh = Create(op.dst, nullptr);
        if (!sh) return;
    }
    const RECT& c = sh->crop;
    const int l = (std::max)(static_cast<int>(c.left), op.x), t = (std::max)(static_cast<int>(c.top), op.y);
    const int r = (std::min)(static_cast<int>(c.right), op.x + op.w), b = (std::min)(static_cast<int>(c.bottom), op.y + op.h);
    if (r <= l || b <= t) return;
    const int w2 = sh->cw * 2;
    // the pixel under (qx, qy) of the rectangle before the draw; outside it the surface (unchanged)
    auto before = [&](int qx, int qy) -> uint32_t {
        if (qx >= 0 && qy >= 0 && qx < op.w && qy < op.h) return op.before[static_cast<size_t>(qy) * op.w + qx];
        const int sx = (std::min)((std::max)(op.x + qx, 0), op.dst.w - 1), sy = (std::min)((std::max)(op.y + qy, 0), op.dst.h - 1);
        return reinterpret_cast<const uint16_t*>(op.dst.bits + static_cast<size_t>(sy) * op.dst.pitch)[sx];
    };
    unsigned changed = 0;
    for (int y = t; y < b; ++y) {
        const int py = y - op.y, cy = y - c.top;
        const auto* drow = reinterpret_cast<const uint16_t*>(op.dst.bits + static_cast<size_t>(y) * op.dst.pitch);
        // (text drawn by GDI has no source pixels: what it changed is what it drew)
        const auto* srow = op.src ? reinterpret_cast<const uint16_t*>(reinterpret_cast<const uint8_t*>(op.src) +
                                                                      static_cast<size_t>(py) * op.srcPitch)
                                  : nullptr;
        const auto* arow = op.alpha ? reinterpret_cast<const uint16_t*>(reinterpret_cast<const uint8_t*>(op.alpha) +
                                                                         static_cast<size_t>(py) * op.alphaPitch)
                                    : nullptr;
        for (int x = l; x < r; ++x) {
            const int px = x - op.x, cx = x - c.left;
            const uint32_t B = op.before[static_cast<size_t>(py) * op.w + px], A = drow[x];
            bool drawn;
            if (!srow) {
                drawn = A != B;
            } else {
                const uint32_t sp = srow[px];
                drawn = op.key == 0xFFFFFFFFu || sp != op.key;
                if (drawn && arow) drawn = (arow[px] & 0x1Fu) != 0;
            }
            uint32_t& ref = sh->ref[static_cast<size_t>(cy) * sh->cw + cx];
            const bool valid = ref == (0x10000u | B);
            if (op.kind == 3) {             // no HD version: whatever the game drew is not in the shadow
                if (drawn || !valid) ref = 0;
                continue;
            }
            Sub sub[4];
            bool any = drawn;
            for (int k = 0; k < 4; ++k) {
                sub[k] = SubOf(op, px, py, k & 1, k >> 1);
                if (sub[k].a || sub[k].shade) any = true;
            }
            uint32_t* h0 = sh->hd.get() + static_cast<size_t>(cy * 2) * w2 + static_cast<size_t>(cx * 2);
            if (!any) {                     // the drawn thing does not reach this pixel at all
                if (!valid) ref = 0;
                continue;
            }
            // the HD version of what was there - not needed where the drawn thing covers all four sub-pixels
            // (the inside of a picture: the briefing's parchment took ~20 ms with the background enlarged)
            const bool opaque = sub[0].a >= 255 && sub[1].a >= 255 && sub[2].a >= 255 && sub[3].a >= 255 &&
                                !(sub[0].shade | sub[1].shade | sub[2].shade | sub[3].shade);
            uint32_t bg[4] = { 0, 0, 0, 0 };
            int stride = 0;
            bool hdBg = valid;
            if (opaque) {
                if (!valid) hdBg = Menu::HdUnder(c, cx, cy, static_cast<uint16_t>(B), stride) != nullptr;
            } else if (valid) {
                bg[0] = h0[0]; bg[1] = h0[1]; bg[2] = h0[w2]; bg[3] = h0[w2 + 1];
            } else if (const uint32_t* m = Menu::HdUnder(c, cx, cy, static_cast<uint16_t>(B), stride)) {
                bg[0] = m[0]; bg[1] = m[1]; bg[2] = m[stride]; bg[3] = m[stride + 1];
                hdBg = true;
            } else {
                // the old pixels enlarged (bilinear: 9:3:3:1 of the pixel and its neighbours towards the sub-pixel)
                const uint32_t self = To8888(B);
                for (int k = 0; k < 4; ++k) {
                    const int ex = (k & 1) ? 1 : -1, ey = (k >> 1) ? 1 : -1;
                    const uint32_t hn = To8888(before(px + ex, py)), vn = To8888(before(px, py + ey)), dn = To8888(before(px + ex, py + ey));
                    uint32_t out = 0xFF000000u;
                    for (int sft = 0; sft < 24; sft += 8) {
                        const uint32_t v = (((self >> sft) & 0xFF) * 9 + ((hn >> sft) & 0xFF) * 3 + ((vn >> sft) & 0xFF) * 3 +
                                            ((dn >> sft) & 0xFF) + 8) >> 4;
                        out |= v << sft;
                    }
                    bg[k] = out;
                }
            }
            if (!drawn && !(op.glyph || hdBg)) {
                // a picture's soft HD edge over a background without an HD version: left to the frame
                if (!valid) ref = 0;
                continue;
            }
            if (opaque) {
                h0[0] = 0xFF000000u | sub[0].colour;
                h0[1] = 0xFF000000u | sub[1].colour;
                h0[w2] = 0xFF000000u | sub[2].colour;
                h0[w2 + 1] = 0xFF000000u | sub[3].colour;
            } else {
                for (int k = 0; k < 4; ++k) {
                    const uint32_t under = Shade(bg[k], sub[k].shade, op.factor);
                    const uint32_t out = Mix(under, sub[k].colour, sub[k].a);
                    if (k == 0) h0[0] = out;
                    else if (k == 1) h0[1] = out;
                    else if (k == 2) h0[w2] = out;
                    else h0[w2 + 1] = out;
                }
            }
            ref = 0x10000u | (drawn ? A : B);
            ++changed;
        }
    }
    AddDirty(*sh, l - c.left, t - c.top, r - c.left, b - c.top);
    if (op.glyph) {
        if (op.kind == 1) ++g_glyphs; else ++g_glyphsNoHd;
    } else {
        if (op.kind == 2) ++g_pictures; else ++g_picturesNoHd;
    }
    (void)changed;
}

bool GetPatch(const uint8_t* frameBits, const RECT& crop, Menu::Layer& out, uintptr_t& identity) {
    if (g_shadows.empty()) return false;
    Shadow* s = nullptr;
    for (auto& p : g_shadows) {
        if (p->bits == frameBits) s = p.get();
    }
    if (!s || s->crop.left != crop.left || s->crop.top != crop.top || s->crop.right != crop.right || s->crop.bottom != crop.bottom) {
        return false;
    }
    out.orig = s->ref.data();
    out.hd = s->hd.get();
    out.w = s->cw;
    out.h = s->ch;
    out.dirty = s->anyDirty ? s->dirty : RECT{ 0, 0, 0, 0 };
    out.generation = static_cast<unsigned>(s->identity);
    s->anyDirty = false;
    identity = s->identity;
    return true;
}

double TakeTimeUs() {
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    const double us = static_cast<double>(g_spent) * 1e6 / static_cast<double>(f.QuadPart);
    g_spent = 0;
    return us;
}

namespace {
    unsigned g_uiAsked = 0, g_uiFound = 0;
    bool     g_uiNoted = false;
}

bool InterfacePicture(const Buf& src, const RECT& r, int& w2, int& h2, const uint32_t*& bgra, const uint8_t*& shade) {
    if (!SherwoodConfig::Get().hdMenu || !Diag::IsMainThread()) return false;
    const int w = r.right - r.left, h = r.bottom - r.top;
    // the files are made for pictures with both sides >= 4 (as the menus'); a picture that changes
    // (health, numbers) gets a new key at every change - the lookups stay few: the caller asks only
    // when it builds its cached 2x version, and the answer per key is kept (LoadPicture)
    if (w < 4 || h < 4 || static_cast<int64_t>(w) * h > 640 * 480 || r.left < 0 || r.top < 0 || r.right > src.w ||
        r.bottom > src.h || g_pics.size() > 50000) {
        return false;
    }
    ++g_uiAsked;
    const Picture* p = LoadPicture(Fnv(src, r), w, h);
    if (!p) return false;
    ++g_uiFound;
    if (!g_uiNoted) {
        g_uiNoted = true;
        Logger::Info("HD menu parts: the mission interface draws its pictures from them too (first: %dx%d).", w, h);
    }
    w2 = p->w2;
    h2 = p->h2;
    bgra = p->bgra.data();
    shade = p->shade.data();
    return true;
}

int Describe(char* out, int size) {
    return std::snprintf(out, size,
                         "shadows=%u created=%u resets=%u copies=%u glyphs=%u glyphsNoHd=%u pictures=%u picturesNoHd=%u "
                         "picLoaded=%u picCollected=%u ui=%u/%u active=%d",
                         static_cast<unsigned>(g_shadows.size()), g_created, g_resets, g_copies, g_glyphs, g_glyphsNoHd,
                         g_pictures, g_picturesNoHd, g_picLoaded, g_picCollected, g_uiFound, g_uiAsked, Active() ? 1 : 0);
}

} // namespace DDM::HD::Patch
