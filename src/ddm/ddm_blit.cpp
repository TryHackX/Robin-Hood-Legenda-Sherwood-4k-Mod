#include "ddm_blit.h"
#include "ddm_hd_patch.h"
#include "../diag.h"

#include <emmintrin.h>
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <new>
#include <vector>

namespace DDM {

void Fill16(const Buf& dst, const RECT& r, uint16_t color) {
    const int w = r.right - r.left;
    const int h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return;

    const __m128i v = _mm_set1_epi16(static_cast<short>(color));
    uint8_t* row = dst.bits + r.top * dst.pitch + r.left * 2;
    for (int y = 0; y < h; ++y) {
        auto* p = reinterpret_cast<uint16_t*>(row);
        int x = 0;
        for (; x + 8 <= w; x += 8) {
            _mm_storeu_si128(reinterpret_cast<__m128i*>(p + x), v);
        }
        for (; x < w; ++x) {
            p[x] = color;
        }
        row += dst.pitch;
    }
}

namespace {
    // Row move for the same surface (the map scroll shifts the whole view by a few pixels, so
    // source and destination overlap by almost the full row). Every block is loaded completely
    // before it is stored, which is safe for any overlap distance: forwards when the data moves
    // towards lower addresses, backwards otherwise.
    inline void MoveRow(uint8_t* d, const uint8_t* s, size_t n) {
        if (d == s || n == 0) return;
        if (d < s) {
            size_t i = 0;
            for (; i + 32 <= n; i += 32) {
                const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + i));
                const __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + i + 16));
                _mm_storeu_si128(reinterpret_cast<__m128i*>(d + i), a);
                _mm_storeu_si128(reinterpret_cast<__m128i*>(d + i + 16), b);
            }
            for (; i < n; ++i) d[i] = s[i];
        } else {
            size_t i = n;
            for (; i >= 32; i -= 32) {
                const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + i - 16));
                const __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + i - 32));
                _mm_storeu_si128(reinterpret_cast<__m128i*>(d + i - 16), a);
                _mm_storeu_si128(reinterpret_cast<__m128i*>(d + i - 32), b);
            }
            while (i > 0) {
                --i;
                d[i] = s[i];
            }
        }
    }
}

void Copy16(const Buf& dst, int dx, int dy, const Buf& src, const RECT& sr) {
    const int w = sr.right - sr.left;
    const int h = sr.bottom - sr.top;
    if (w <= 0 || h <= 0) return;

    const size_t rowBytes = static_cast<size_t>(w) * 2;
    const uint8_t* s = src.bits + sr.top * src.pitch + sr.left * 2;
    uint8_t* d = dst.bits + dy * dst.pitch + dx * 2;

    if (dst.bits != src.bits) {
        for (int y = 0; y < h; ++y) {
            std::memcpy(d, s, rowBytes);
            s += src.pitch;
            d += dst.pitch;
        }
        return;
    }

    if (dy > sr.top) {
        // same surface, moving down: go bottom-up so rows are read before they are overwritten
        s += static_cast<ptrdiff_t>(h - 1) * src.pitch;
        d += static_cast<ptrdiff_t>(h - 1) * dst.pitch;
        for (int y = 0; y < h; ++y) {
            MoveRow(d, s, rowBytes);
            s -= src.pitch;
            d -= dst.pitch;
        }
        return;
    }
    for (int y = 0; y < h; ++y) {
        MoveRow(d, s, rowBytes);
        s += src.pitch;
        d += dst.pitch;
    }
}

namespace {
    inline void KeyRow(uint16_t* d, const uint16_t* s, int w, uint16_t key, const __m128i& k) {
        int x = 0;
        for (; x + 8 <= w; x += 8) {
            const __m128i sv = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + x));
            const __m128i dv = _mm_loadu_si128(reinterpret_cast<const __m128i*>(d + x));
            const __m128i m  = _mm_cmpeq_epi16(sv, k);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(d + x),
                             _mm_or_si128(_mm_and_si128(m, dv), _mm_andnot_si128(m, sv)));
        }
        for (; x < w; ++x) {
            const uint16_t p = s[x];
            if (p != key) d[x] = p;
        }
    }
}

void CopyKey16(const Buf& dst, int dx, int dy, const Buf& src, const RECT& sr, uint16_t key) {
    const int w = sr.right - sr.left;
    const int h = sr.bottom - sr.top;
    if (w <= 0 || h <= 0) return;

    const __m128i k = _mm_set1_epi16(static_cast<short>(key));

    if (dst.bits == src.bits) {
        // Rare (keyed blit within one surface): go through a temporary copy of the source.
        std::vector<uint16_t> tmp(static_cast<size_t>(w) * h);
        for (int y = 0; y < h; ++y) {
            std::memcpy(&tmp[static_cast<size_t>(y) * w], src.bits + (sr.top + y) * src.pitch + sr.left * 2,
                        static_cast<size_t>(w) * 2);
        }
        for (int y = 0; y < h; ++y) {
            KeyRow(reinterpret_cast<uint16_t*>(dst.bits + (dy + y) * dst.pitch + dx * 2),
                   &tmp[static_cast<size_t>(y) * w], w, key, k);
        }
        return;
    }

    const uint8_t* s = src.bits + sr.top * src.pitch + sr.left * 2;
    uint8_t* d = dst.bits + dy * dst.pitch + dx * 2;
    for (int y = 0; y < h; ++y) {
        KeyRow(reinterpret_cast<uint16_t*>(d), reinterpret_cast<const uint16_t*>(s), w, key, k);
        s += src.pitch;
        d += dst.pitch;
    }
}

void Stretch16(const Buf& dst, const RECT& dr, const Buf& src, const RECT& sr, const RECT& clip,
               bool keyed, uint16_t key, bool mirrorX, bool mirrorY) {
    const int dw = dr.right - dr.left;
    const int dh = dr.bottom - dr.top;
    const int sw = sr.right - sr.left;
    const int sh = sr.bottom - sr.top;
    const int cw = clip.right - clip.left;
    const int ch = clip.bottom - clip.top;
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0 || cw <= 0 || ch <= 0) return;

    // Source column for every destination column, computed once.
    static thread_local std::vector<int> xmap;
    xmap.resize(static_cast<size_t>(cw));
    for (int i = 0; i < cw; ++i) {
        int sx = static_cast<int>((static_cast<long long>(clip.left - dr.left + i) * sw) / dw);
        if (sx >= sw) sx = sw - 1;
        xmap[static_cast<size_t>(i)] = sr.left + (mirrorX ? (sw - 1 - sx) : sx);
    }

    // Same-surface stretches read from a snapshot so the source is not modified mid-blit.
    std::vector<uint8_t> snapshot;
    const uint8_t* srcBits = src.bits;
    if (dst.bits == src.bits) {
        snapshot.assign(src.bits, src.bits + static_cast<size_t>(src.pitch) * src.h);
        srcBits = snapshot.data();
    }

    int prevSy = -1;
    uint16_t* prevRow = nullptr;
    for (int y = 0; y < ch; ++y) {
        int sy = static_cast<int>((static_cast<long long>(clip.top - dr.top + y) * sh) / dh);
        if (sy >= sh) sy = sh - 1;
        sy = sr.top + (mirrorY ? (sh - 1 - sy) : sy);

        auto* d = reinterpret_cast<uint16_t*>(dst.bits + (clip.top + y) * dst.pitch + clip.left * 2);
        if (!keyed && sy == prevSy && prevRow) {
            std::memcpy(d, prevRow, static_cast<size_t>(cw) * 2); // vertical upscale: repeat the row
        } else {
            const auto* s = reinterpret_cast<const uint16_t*>(srcBits + sy * src.pitch);
            const int* xm = xmap.data();
            if (keyed) {
                for (int x = 0; x < cw; ++x) {
                    const uint16_t p = s[xm[x]];
                    if (p != key) d[x] = p;
                }
            } else {
                for (int x = 0; x < cw; ++x) {
                    d[x] = s[xm[x]];
                }
            }
        }
        prevSy = sy;
        prevRow = d;
    }
}

// ------------------------------------------------------------------------------ smooth 2x sprites
namespace {
    // RGB565 with gaps between the channels, so that sums of up to 16 pixels stay apart:
    // G in bits 21..26, R in 11..15, B in 0..4.
    inline uint32_t Spread(uint32_t p) { return (p | (p << 16)) & 0x07E0F81Fu; }

    inline uint16_t DarkenShadow(uint32_t px, int32_t mul, const uint16_t* m) {
        const uint32_t b = static_cast<uint32_t>((static_cast<int32_t>((px & 0x1F) << 10) * mul) >> 16) & m[0];
        const uint32_t g = static_cast<uint32_t>((static_cast<int32_t>(((px >> 5) & 0x3F) << 9) * mul) >> 16) & m[1];
        const uint32_t r = static_cast<uint32_t>((static_cast<int32_t>((px >> 11) << 10) * mul) >> 16) & m[2];
        return static_cast<uint16_t>(((b >> 8) & 0xFF) | (g >> 2) | (r << 3));
    }

    // One source row, filtered horizontally for the destination columns of the clip rectangle.
    struct FilteredRow {
        int row = INT_MIN;                 // logical source row this holds
        std::vector<uint32_t> color;       // sum of opaque taps (weights 1 + 3)
        std::vector<uint8_t>  opaque;      // weight of the opaque taps, 0..4
        std::vector<uint8_t>  shadow;      // weight of the shadow taps, 0..4
    };
}

namespace {
    // interface pictures: beyond the surface's edge the edge pixel continues (tiles of the HUD
    // lie side by side; fading to transparent at each tile's edge left a seam line)
    thread_local bool t_edgeClamp = false;
}

void SetSmoothEdgeClamp(bool on) {
    t_edgeClamp = on;
}

namespace {
// The filter itself: for every destination row of `clip` it hands the two filtered source rows
// and their vertical weights to `emitRow(y, ca, cb, oa, ob, sa, sb, wya, wyb)` - per column n
// of the clip: colour sums ca/cb (spread RGB565), opaque weights oa/ob and shadow weights sa/sb
// (0..4 each); wya + wyb = 4. The caller turns them into pixels.
template <typename EmitRow>
void SmoothCore(const RECT& dr, const Buf& src, const RECT& sr, const RECT& clip, uint32_t key, uint32_t marker,
                bool mirrorX, bool mirrorY, EmitRow&& emitRow) {
    const int sw = sr.right - sr.left;
    const int sh = sr.bottom - sr.top;
    const int cw = clip.right - clip.left;

    static thread_local FilteredRow rows[2];
    static thread_local std::vector<uint32_t> lineColor; // one source row with a transparent pixel on either side
    static thread_local std::vector<uint8_t>  lineKind;  // 0 transparent, 1 opaque, 2 shadow
    for (FilteredRow& r : rows) {
        r.row = INT_MIN;
        r.color.resize(static_cast<size_t>(cw));
        r.opaque.resize(static_cast<size_t>(cw));
        r.shadow.resize(static_cast<size_t>(cw));
    }
    lineColor.resize(static_cast<size_t>(sw) + 2);
    lineKind.resize(static_cast<size_t>(sw) + 2);

    const auto filterRow = [&](FilteredRow& out, int ly) {
        out.row = ly;
        int sy = sr.top + (mirrorY ? (sh - 1 - ly) : ly);
        if (t_edgeClamp) sy = (std::max)(0, (std::min)(src.h - 1, sy));
        if (sy < 0 || sy >= src.h) {
            std::fill(out.opaque.begin(), out.opaque.end(), static_cast<uint8_t>(0));
            std::fill(out.shadow.begin(), out.shadow.end(), static_cast<uint8_t>(0));
            std::fill(out.color.begin(), out.color.end(), 0u);
            return;
        }
        const auto* s = reinterpret_cast<const uint16_t*>(src.bits + static_cast<size_t>(sy) * src.pitch);
        for (int lx = -1; lx <= sw; ++lx) {
            int sx = sr.left + (mirrorX ? (sw - 1 - lx) : lx);
            if (t_edgeClamp) sx = (std::max)(0, (std::min)(src.w - 1, sx));
            uint32_t c = 0;
            uint8_t kind = 0;
            if (sx >= 0 && sx < src.w) {
                const uint32_t p = s[sx];
                if (p == marker) {
                    kind = 2;
                } else if (p != key) {
                    kind = 1;
                    c = Spread(p);
                }
            }
            lineColor[static_cast<size_t>(lx + 1)] = c;
            lineKind[static_cast<size_t>(lx + 1)] = kind;
        }
        const uint32_t* lc = lineColor.data() + 1;
        const uint8_t* lk = lineKind.data() + 1;
        for (int n = 0; n < cw; ++n) {
            const int i = clip.left + n - dr.left;
            const int k = i >> 1;
            // destination column i lies a quarter of a source pixel left (even) or right (odd) of pixel k
            const int xa = (i & 1) ? k : k - 1, xb = xa + 1;
            const uint32_t wa = (i & 1) ? 3u : 1u, wb = 4u - wa;
            out.color[static_cast<size_t>(n)]  = wa * lc[xa] + wb * lc[xb];
            out.opaque[static_cast<size_t>(n)] = static_cast<uint8_t>(wa * (lk[xa] == 1) + wb * (lk[xb] == 1));
            out.shadow[static_cast<size_t>(n)] = static_cast<uint8_t>(wa * (lk[xa] == 2) + wb * (lk[xb] == 2));
        }
    };

    const auto rowFor = [&](int ly) -> const FilteredRow& {
        if (rows[0].row == ly) return rows[0];
        if (rows[1].row == ly) return rows[1];
        // replace the row that is further away: rows are asked for in ascending order
        FilteredRow& victim = (rows[0].row < rows[1].row) ? rows[0] : rows[1];
        filterRow(victim, ly);
        return victim;
    };

    for (int y = clip.top; y < clip.bottom; ++y) {
        const int j = y - dr.top;
        const int k = j >> 1;
        const int ya = (j & 1) ? k : k - 1, yb = ya + 1;
        const uint32_t wya = (j & 1) ? 3u : 1u, wyb = 4u - wya;
        const FilteredRow& a = rowFor(ya);
        const FilteredRow& b = rowFor(yb);
        // rowFor may have replaced `a` when it filled `b`: both rows must be present
        const FilteredRow& a2 = (a.row == ya) ? a : rowFor(ya);
        emitRow(y, a2.color.data(), b.color.data(), a2.opaque.data(), b.opaque.data(), a2.shadow.data(), b.shadow.data(),
                wya, wyb);
    }
}

inline uint16_t Pack(uint32_t acc) {
    acc = ((acc + 0x01004008u) >> 4) & 0x07E0F81Fu;
    return static_cast<uint16_t>(acc | (acc >> 16));
}
} // namespace

void SmoothSprite2x16(const Buf& dst, const RECT& dr, const Buf& src, const RECT& sr, const RECT& clip,
                      uint32_t key, uint32_t marker, uint32_t factor, const uint16_t* maskBGR,
                      bool mirrorX, bool mirrorY) {
    const int sw = sr.right - sr.left;
    const int sh = sr.bottom - sr.top;
    const int cw = clip.right - clip.left;
    if (sw <= 0 || sh <= 0 || cw <= 0 || clip.bottom <= clip.top) return;
    static const uint16_t kMask565[3] = { 0x1F00, 0x1F80, 0x1F00 };
    const uint16_t* mask = maskBGR ? maskBGR : kMask565;
    const int32_t mul = static_cast<int16_t>(static_cast<uint16_t>(factor << 6));

    SmoothCore(dr, src, sr, clip, key, marker, mirrorX, mirrorY,
               [&](int y, const uint32_t* ca, const uint32_t* cb, const uint8_t* oa, const uint8_t* ob,
                   const uint8_t* sa, const uint8_t* sb, uint32_t wya, uint32_t wyb) {
        auto* d = reinterpret_cast<uint16_t*>(dst.bits + static_cast<size_t>(y) * dst.pitch) + clip.left;
        for (int n = 0; n < cw; ++n) {
            const uint32_t opaque = wya * oa[n] + wyb * ob[n];      // 0..16
            const uint32_t shadow = wya * sa[n] + wyb * sb[n];
            if (opaque + shadow == 0) continue;
            uint32_t acc = wya * ca[n] + wyb * cb[n];
            if (opaque < 16) {
                const uint32_t under = d[n];
                acc += (16 - opaque - shadow) * Spread(under);
                if (shadow) acc += shadow * Spread(DarkenShadow(under, mul, mask));
            }
            d[n] = Pack(acc);
        }
    });
}

// ------------------------------------------------------------------ interface pictures, cached
// The interface is drawn again in every frame, and at the 2x interface scale every one of its
// pictures went through the filter above: about 0.6 million pixels a frame for the HUD alone,
// some 5 ms - half the time of a whole frame (Perf/Blit lines 2026-11-15: "stretch 2.5 Mpx
// 20 ms per tick"). The pictures hardly ever change, so the filtered result is kept: per
// picture its fully opaque pixels as final colours, its soft edge as colour sums with their
// coverage (blended with what is under them when drawn - the same arithmetic, the same result).
// An entry is used only while the source pixels the filter reads (its 1-pixel border included)
// are exactly what they were; a picture that keeps changing is filtered directly.
namespace {
    struct SmoothRun {
        int y = 0, x0 = 0, x1 = 0;      // row and columns [x0, x1) of the magnified picture
        bool full = false;              // true: final colours; false: colour sums + coverage
        size_t off = 0;                 // into color (full) or acc / cover (soft edge)
    };
    struct SmoothEntry {
        const uint8_t* bits = nullptr;
        int pitch = 0, srcW = 0, srcH = 0;
        RECT sr{};
        uint32_t key = 0;
        bool clamp = false;
        RECT area{};                    // the source pixels the filter reads
        std::vector<uint16_t> seen;     // ... as they were when the entry was made
        std::vector<SmoothRun> runs;
        std::vector<uint16_t> color;
        std::vector<uint32_t> acc;
        std::vector<uint8_t>  cover;    // 1..15 (sixteenths)
        uint64_t lastUse = 0;
        size_t   bytes = 0;
        unsigned changes = 0;           // consecutive uses that found the picture changed
        unsigned direct = 0;            // uses left that filter directly (a picture that keeps changing)
        bool scaled = false;            // ScaledPicture16: any size (outW x outH) instead of 2x
        int  outW = 0, outH = 0;
    };
    std::vector<SmoothEntry*> g_smooth;
    size_t   g_smoothBytes = 0;
    uint64_t g_smoothUse = 0;
    constexpr size_t kSmoothMaxBytes = 48u << 20;
    constexpr size_t kSmoothMaxEntries = 1024;

    size_t EntryBytes(const SmoothEntry& e) {
        return sizeof(SmoothEntry) + e.seen.capacity() * 2 + e.runs.capacity() * sizeof(SmoothRun) +
               e.color.capacity() * 2 + e.acc.capacity() * 4 + e.cover.capacity();
    }

    bool SameSource(const SmoothEntry& e, const Buf& src) {
        const int aw = e.area.right - e.area.left;
        const uint16_t* seen = e.seen.data();
        for (int y = e.area.top; y < e.area.bottom; ++y, seen += aw) {
            const auto* row = reinterpret_cast<const uint16_t*>(src.bits + static_cast<size_t>(y) * src.pitch) + e.area.left;
            if (std::memcmp(row, seen, static_cast<size_t>(aw) * 2) != 0) return false;
        }
        return true;
    }

    // The HD version of the whole picture (AI-upscaled, the menus' files - ddm_hd_patch.h InterfacePicture)
    // as the entry's runs instead of the filter: its colour where it covers fully, colour sums with
    // their coverage on its soft edge, a darkening where only its shadow lies (the game darkens there).
    // Only for pictures drawn with the green colour key the files were made with, or drawn without a
    // key and covering every pixel.
    bool BuildFromHd(SmoothEntry& e, const Buf& src) {
        if (e.key != kNoColor && (e.key & 0xFFFFu) != 0x07C0u) return false;
        const int sw = e.sr.right - e.sr.left, sh = e.sr.bottom - e.sr.top;
        int w2 = 0, h2 = 0;
        const uint32_t* bgra = nullptr;
        const uint8_t* shade = nullptr;
        if (!HD::Patch::InterfacePicture(src, e.sr, w2, h2, bgra, shade) || w2 != 2 * sw || h2 != 2 * sh) return false;
        const size_t n = static_cast<size_t>(w2) * h2;
        if (e.key == kNoColor) {
            for (size_t i = 0; i < n; ++i) {
                if ((bgra[i] >> 24) != 255u) return false;
            }
        }
        auto to565 = [](uint32_t c) {
            return static_cast<uint16_t>(((c >> 8) & 0xF800u) | ((c >> 5) & 0x07E0u) | ((c >> 3) & 0x001Fu));
        };
        // coverage in sixteenths: the colour's (alpha) or else half darkness under its shadow
        auto coverAt = [&](size_t i, bool& colour) -> uint32_t {
            const uint32_t a = bgra[i] >> 24;
            colour = a > 0;
            if (colour) return (a * 16u + 127u) / 255u;
            return (static_cast<uint32_t>(shade[i]) * 8u + 127u) / 255u;
        };
        e.runs.clear();
        e.color.clear();
        e.acc.clear();
        e.cover.clear();
        for (int y = 0; y < h2; ++y) {
            const size_t row = static_cast<size_t>(y) * w2;
            int x = 0;
            while (x < w2) {
                bool colour = false;
                const uint32_t first = coverAt(row + x, colour);
                if (first == 0) { ++x; continue; }
                SmoothRun run;
                run.y = y;
                run.x0 = x;
                run.full = first >= 16;
                run.off = run.full ? e.color.size() : e.acc.size();
                for (; x < w2; ++x) {
                    const uint32_t c = coverAt(row + x, colour);
                    if (c == 0 || (c >= 16) != run.full) break;
                    if (run.full) {
                        e.color.push_back(to565(bgra[row + x]));
                    } else {
                        e.acc.push_back(colour ? c * Spread(to565(bgra[row + x])) : 0u);   // shadow: black
                        e.cover.push_back(static_cast<uint8_t>(c));
                    }
                }
                run.x1 = x;
                e.runs.push_back(run);
            }
        }
        return true;
    }

    void Build(SmoothEntry& e, const Buf& src) {
        const int aw = e.area.right - e.area.left, ah = e.area.bottom - e.area.top;
        e.seen.resize(static_cast<size_t>(aw) * ah);
        for (int y = 0; y < ah; ++y) {
            std::memcpy(e.seen.data() + static_cast<size_t>(y) * aw,
                        reinterpret_cast<const uint16_t*>(src.bits + static_cast<size_t>(e.area.top + y) * src.pitch) + e.area.left,
                        static_cast<size_t>(aw) * 2);
        }
        if (BuildFromHd(e, src)) return;
        e.runs.clear();
        e.color.clear();
        e.acc.clear();
        e.cover.clear();
        const int dw = 2 * (e.sr.right - e.sr.left), dh = 2 * (e.sr.bottom - e.sr.top);
        const RECT whole{ 0, 0, dw, dh };
        const bool clampBefore = t_edgeClamp;
        t_edgeClamp = e.clamp;
        SmoothCore(whole, src, e.sr, whole, e.key, kNoColor, false, false,
                   [&](int y, const uint32_t* ca, const uint32_t* cb, const uint8_t* oa, const uint8_t* ob,
                       const uint8_t*, const uint8_t*, uint32_t wya, uint32_t wyb) {
            int n = 0;
            while (n < dw) {
                const uint32_t first = wya * oa[n] + wyb * ob[n];
                if (first == 0) { ++n; continue; }
                SmoothRun run;
                run.y = y;
                run.x0 = n;
                run.full = first == 16;
                run.off = run.full ? e.color.size() : e.acc.size();
                for (; n < dw; ++n) {
                    const uint32_t o = wya * oa[n] + wyb * ob[n];
                    if (o == 0 || (o == 16) != run.full) break;
                    const uint32_t acc = wya * ca[n] + wyb * cb[n];
                    if (run.full) {
                        e.color.push_back(Pack(acc));
                    } else {
                        e.acc.push_back(acc);
                        e.cover.push_back(static_cast<uint8_t>(o));
                    }
                }
                run.x1 = n;
                e.runs.push_back(run);
            }
        });
        t_edgeClamp = clampBefore;
    }

    void DrawEntry(const SmoothEntry& e, const Buf& dst, const RECT& dr, const RECT& clip) {
        const int cx0 = clip.left - dr.left, cx1 = clip.right - dr.left;
        const int cy0 = clip.top - dr.top, cy1 = clip.bottom - dr.top;
        for (const SmoothRun& r : e.runs) {
            if (r.y < cy0 || r.y >= cy1) continue;
            const int x0 = (std::max)(r.x0, cx0), x1 = (std::min)(r.x1, cx1);
            if (x1 <= x0) continue;
            auto* d = reinterpret_cast<uint16_t*>(dst.bits + static_cast<size_t>(dr.top + r.y) * dst.pitch) + dr.left;
            if (r.full) {
                std::memcpy(d + x0, e.color.data() + r.off + (x0 - r.x0), static_cast<size_t>(x1 - x0) * 2);
                continue;
            }
            const uint32_t* acc = e.acc.data() + r.off - r.x0;
            const uint8_t* cover = e.cover.data() + r.off - r.x0;
            for (int x = x0; x < x1; ++x) {
                d[x] = Pack(acc[x] + (16u - cover[x]) * Spread(d[x]));
            }
        }
    }

    void TrimCache() {
        if (g_smoothBytes <= kSmoothMaxBytes && g_smooth.size() <= kSmoothMaxEntries) return;
        std::sort(g_smooth.begin(), g_smooth.end(), [](const SmoothEntry* a, const SmoothEntry* b) { return a->lastUse > b->lastUse; });
        while (!g_smooth.empty() && (g_smoothBytes > kSmoothMaxBytes * 3 / 4 || g_smooth.size() > kSmoothMaxEntries * 3 / 4)) {
            g_smoothBytes -= g_smooth.back()->bytes;
            delete g_smooth.back();
            g_smooth.pop_back();
        }
    }
}

void SmoothPicture2x16(const Buf& dst, const RECT& dr, const Buf& src, const RECT& sr, const RECT& clip, uint32_t key) {
    const int sw = sr.right - sr.left, sh = sr.bottom - sr.top;
    if (sw <= 0 || sh <= 0 || clip.right <= clip.left || clip.bottom <= clip.top) return;
    // Interface pictures are small. A big one (a screen-sized copy enlarged by mistake - the dialog
    // opened from a mission window, code review 2026-11-17) would make an entry of 30-45 MB per
    // build and push every HUD picture out of the cache: filtered directly, only the clip.
    if (static_cast<int64_t>(sw) * sh > 512 * 512) {
        SmoothSprite2x16(dst, dr, src, sr, clip, key, kNoColor, 256, nullptr, false, false);
        return;
    }
    const bool clamp = t_edgeClamp;
    SmoothEntry* e = nullptr;
    for (SmoothEntry* c : g_smooth) {
        if (!c->scaled && c->bits == src.bits && c->pitch == src.pitch && c->srcW == src.w && c->srcH == src.h && c->key == key &&
            c->clamp == clamp && EqualRect(&c->sr, &sr)) {
            e = c;
            break;
        }
    }
    if (!e) {
        e = new SmoothEntry();
        e->bits = src.bits;
        e->pitch = src.pitch;
        e->srcW = src.w;
        e->srcH = src.h;
        e->sr = sr;
        e->key = key;
        e->clamp = clamp;
        e->area = RECT{ (std::max)(0L, sr.left - 1), (std::max)(0L, sr.top - 1),
                        (std::min)(static_cast<LONG>(src.w), sr.right + 1), (std::min)(static_cast<LONG>(src.h), sr.bottom + 1) };
        g_smooth.push_back(e);
        e->changes = 0;
    } else if (e->direct > 0) {
        // a picture that keeps changing: filtered directly for a while
        --e->direct;
        e->lastUse = ++g_smoothUse;
        SmoothSprite2x16(dst, dr, src, sr, clip, key, kNoColor, 256, nullptr, false, false);
        return;
    } else if (SameSource(*e, src)) {
        e->changes = 0;
        e->lastUse = ++g_smoothUse;
        DrawEntry(*e, dst, dr, clip);
        return;
    } else if (++e->changes >= 4) {
        e->changes = 0;
        e->direct = 120;
        e->lastUse = ++g_smoothUse;
        SmoothSprite2x16(dst, dr, src, sr, clip, key, kNoColor, 256, nullptr, false, false);
        return;
    }
    g_smoothBytes -= e->bytes;
    try {
        Build(*e, src);
    } catch (const std::bad_alloc&) {
        // no memory for the cache (a 32-bit process): every entry goes, this picture is filtered directly
        for (SmoothEntry* c : g_smooth) delete c;
        g_smooth.clear();
        g_smoothBytes = 0;
        SmoothSprite2x16(dst, dr, src, sr, clip, key, kNoColor, 256, nullptr, false, false);
        return;
    }
    e->bytes = EntryBytes(*e);
    g_smoothBytes += e->bytes;
    e->lastUse = ++g_smoothUse;
    DrawEntry(*e, dst, dr, clip);
    TrimCache();
}

// ------------------------------------------------------------------ the opened map at its own size
// The map window drawn at [Display] MapScale (1.5x .. 4x, 2026-11-18, user: "different sizes of the minimap"):
// its pictures - the scroll with the level and the people's markers - are scaled to any size once and kept like
// the 2x pictures above (an entry per picture and size, the same runs). The source is the picture's HD version
// (HD::Patch::MapPicture: the minimap made from the level's HD background, 4x, or the game's picture upscaled,
// 2x) or the game's own pixels. Resampled with a tent filter as wide as the reduction (at least one source
// pixel: bilinear when enlarging), colour premultiplied by coverage - the transparent green never bleeds in.
namespace {
    struct Taps {
        std::vector<int> first, count;
        std::vector<size_t> off;
        std::vector<float> w;
    };

    void MakeTaps(int in, int out, Taps& t) {
        const float scale = static_cast<float>(in) / static_cast<float>(out);
        const float support = (std::max)(1.0f, scale);
        t.first.assign(out, 0);
        t.count.assign(out, 0);
        t.off.assign(out, 0);
        t.w.clear();
        for (int o = 0; o < out; ++o) {
            const float c = (static_cast<float>(o) + 0.5f) * scale - 0.5f;
            const int i0 = (std::max)(0, static_cast<int>(std::floor(c - support)) + 1);
            const int i1 = (std::min)(in - 1, static_cast<int>(std::ceil(c + support)) - 1);
            const size_t off = t.w.size();
            float sum = 0.0f;
            for (int i = i0; i <= i1; ++i) {
                const float wgt = (std::max)(0.0f, 1.0f - std::fabs(static_cast<float>(i) - c) / support);
                t.w.push_back(wgt);
                sum += wgt;
            }
            if (sum <= 0.0f) {      // outside every tap (cannot happen with support >= 1): the nearest pixel
                t.w.resize(off);
                t.w.push_back(1.0f);
                t.first[o] = (std::max)(0, (std::min)(in - 1, static_cast<int>(std::lround(c))));
                t.count[o] = 1;
                t.off[o] = off;
                continue;
            }
            for (size_t k = off; k < t.w.size(); ++k) t.w[k] /= sum;
            t.first[o] = i0;
            t.count[o] = i1 - i0 + 1;
            t.off[o] = off;
        }
    }

    // planar-interleaved floats, `ch` per pixel
    void Resample(const std::vector<float>& src, int sw, int sh, int ch, std::vector<float>& dst, int dw, int dh) {
        Taps tx, ty;
        MakeTaps(sw, dw, tx);
        MakeTaps(sh, dh, ty);
        std::vector<float> tmp(static_cast<size_t>(dw) * sh * ch, 0.0f);
        for (int y = 0; y < sh; ++y) {
            const float* row = src.data() + static_cast<size_t>(y) * sw * ch;
            float* out = tmp.data() + static_cast<size_t>(y) * dw * ch;
            for (int x = 0; x < dw; ++x) {
                const float* wgt = tx.w.data() + tx.off[x];
                float* o = out + static_cast<size_t>(x) * ch;
                for (int k = 0; k < tx.count[x]; ++k) {
                    const float* p = row + static_cast<size_t>(tx.first[x] + k) * ch;
                    for (int c = 0; c < ch; ++c) o[c] += wgt[k] * p[c];
                }
            }
        }
        dst.assign(static_cast<size_t>(dw) * dh * ch, 0.0f);
        const size_t rowLen = static_cast<size_t>(dw) * ch;
        for (int y = 0; y < dh; ++y) {
            float* out = dst.data() + static_cast<size_t>(y) * rowLen;
            const float* wgt = ty.w.data() + ty.off[y];
            for (int k = 0; k < ty.count[y]; ++k) {
                const float* row = tmp.data() + static_cast<size_t>(ty.first[y] + k) * rowLen;
                const float wk = wgt[k];
                for (size_t i = 0; i < rowLen; ++i) out[i] += wk * row[i];
            }
        }
    }

    void BuildScaled(SmoothEntry& e, const Buf& src) {
        const int aw = e.area.right - e.area.left, ah = e.area.bottom - e.area.top;
        e.seen.resize(static_cast<size_t>(aw) * ah);
        for (int y = 0; y < ah; ++y) {
            std::memcpy(e.seen.data() + static_cast<size_t>(y) * aw,
                        reinterpret_cast<const uint16_t*>(src.bits + static_cast<size_t>(e.area.top + y) * src.pitch) + e.area.left,
                        static_cast<size_t>(aw) * 2);
        }
        const int sw = e.sr.right - e.sr.left, sh = e.sr.bottom - e.sr.top;
        constexpr int kCh = 5;      // red, green, blue (premultiplied, 0..255), coverage, shadow (0..1)
        std::vector<float> in;
        int iw = 0, ih = 0;
        int wk = 0, hk = 0;
        const uint32_t* bgra = nullptr;
        const uint8_t* shade = nullptr;
        const bool keyOk = e.key == kNoColor || (e.key & 0xFFFFu) == 0x07C0u;   // the files' own colour key
        if (keyOk && HD::Patch::MapPicture(src, e.sr, wk, hk, bgra, shade) && wk >= sw && hk >= sh) {
            const size_t n = static_cast<size_t>(wk) * hk;
            bool opaque = true;
            in.resize(n * kCh);
            for (size_t i = 0; i < n; ++i) {
                const uint32_t c = bgra[i];
                const float a = static_cast<float>(c >> 24) / 255.0f;
                if ((c >> 24) != 255u) opaque = false;
                in[i * kCh + 0] = static_cast<float>((c >> 16) & 0xFFu) * a;
                in[i * kCh + 1] = static_cast<float>((c >> 8) & 0xFFu) * a;
                in[i * kCh + 2] = static_cast<float>(c & 0xFFu) * a;
                in[i * kCh + 3] = a;
                in[i * kCh + 4] = static_cast<float>(shade[i]) / 255.0f;
            }
            if (e.key == kNoColor && !opaque) {
                in.clear();     // drawn without a key: only a picture that covers every pixel (as BuildFromHd)
            } else {
                iw = wk;
                ih = hk;
            }
        }
        if (in.empty()) {
            iw = sw;
            ih = sh;
            in.assign(static_cast<size_t>(iw) * ih * kCh, 0.0f);
            for (int y = 0; y < sh; ++y) {
                const auto* row = reinterpret_cast<const uint16_t*>(src.bits + static_cast<size_t>(e.sr.top + y) * src.pitch) + e.sr.left;
                for (int x = 0; x < sw; ++x) {
                    const uint32_t p = row[x];
                    if (e.key != kNoColor && p == (e.key & 0xFFFFu)) continue;
                    float* o = in.data() + (static_cast<size_t>(y) * sw + x) * kCh;
                    const uint32_t r5 = p >> 11, g6 = (p >> 5) & 63u, b5 = p & 31u;
                    o[0] = static_cast<float>((r5 << 3) | (r5 >> 2));
                    o[1] = static_cast<float>((g6 << 2) | (g6 >> 4));
                    o[2] = static_cast<float>((b5 << 3) | (b5 >> 2));
                    o[3] = 1.0f;
                }
            }
        }
        std::vector<float> out;
        Resample(in, iw, ih, kCh, out, e.outW, e.outH);
        auto to565 = [](float r, float g, float b) {
            const auto c8 = [](float v) { return static_cast<uint32_t>((std::max)(0.0f, (std::min)(255.0f, v + 0.5f))); };
            return static_cast<uint16_t>(((c8(r) & 0xF8u) << 8) | ((c8(g) & 0xFCu) << 3) | (c8(b) >> 3));
        };
        e.runs.clear();
        e.color.clear();
        e.acc.clear();
        e.cover.clear();
        for (int y = 0; y < e.outH; ++y) {
            const float* row = out.data() + static_cast<size_t>(y) * e.outW * kCh;
            // coverage in sixteenths: the colour's, or else half darkness under its shadow (as BuildFromHd)
            auto coverAt = [&](int x, bool& colour, uint16_t& c565) -> uint32_t {
                const float* p = row + static_cast<size_t>(x) * kCh;
                const uint32_t cov = static_cast<uint32_t>((std::min)(1.0f, p[3]) * 16.0f + 0.5f);
                colour = cov > 0;
                if (colour) {
                    const float inv = 1.0f / (std::max)(p[3], 1e-6f);
                    c565 = to565(p[0] * inv, p[1] * inv, p[2] * inv);
                    return cov;
                }
                return static_cast<uint32_t>((std::min)(1.0f, p[4]) * 8.0f + 0.5f);
            };
            int x = 0;
            while (x < e.outW) {
                bool colour = false;
                uint16_t c565 = 0;
                const uint32_t first = coverAt(x, colour, c565);
                if (first == 0) { ++x; continue; }
                SmoothRun run;
                run.y = y;
                run.x0 = x;
                run.full = colour && first >= 16;
                run.off = run.full ? e.color.size() : e.acc.size();
                for (; x < e.outW; ++x) {
                    const uint32_t c = coverAt(x, colour, c565);
                    if (c == 0 || (colour && c >= 16) != run.full) break;
                    if (run.full) {
                        e.color.push_back(c565);
                    } else {
                        e.acc.push_back(colour ? c * Spread(c565) : 0u);   // shadow: black
                        e.cover.push_back(static_cast<uint8_t>((std::min)(c, 15u)));
                    }
                }
                run.x1 = x;
                e.runs.push_back(run);
            }
        }
    }
}

void ScaledPicture16(const Buf& dst, const RECT& dr, const Buf& src, const RECT& sr, const RECT& clip, uint32_t key) {
    const int sw = sr.right - sr.left, sh = sr.bottom - sr.top;
    const int dw = dr.right - dr.left, dh = dr.bottom - dr.top;
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0 || clip.right <= clip.left || clip.bottom <= clip.top) return;
    const bool keyed = key != kNoColor;
    if (static_cast<int64_t>(sw) * sh > 512 * 512 || static_cast<int64_t>(dw) * dh > 2048 * 2048 || !Diag::IsMainThread()) {
        Stretch16(dst, dr, src, sr, clip, keyed, static_cast<uint16_t>(key & 0xFFFFu), false, false);
        return;
    }
    SmoothEntry* e = nullptr;
    for (SmoothEntry* c : g_smooth) {
        if (c->scaled && c->outW == dw && c->outH == dh && c->bits == src.bits && c->pitch == src.pitch && c->srcW == src.w &&
            c->srcH == src.h && c->key == key && EqualRect(&c->sr, &sr)) {
            e = c;
            break;
        }
    }
    if (!e) {
        e = new SmoothEntry();
        e->bits = src.bits;
        e->pitch = src.pitch;
        e->srcW = src.w;
        e->srcH = src.h;
        e->sr = sr;
        e->key = key;
        e->scaled = true;
        e->outW = dw;
        e->outH = dh;
        e->area = RECT{ (std::max)(0L, sr.left), (std::max)(0L, sr.top),
                        (std::min)(static_cast<LONG>(src.w), sr.right), (std::min)(static_cast<LONG>(src.h), sr.bottom) };
        g_smooth.push_back(e);
    } else if (e->direct > 0) {
        --e->direct;
        e->lastUse = ++g_smoothUse;
        Stretch16(dst, dr, src, sr, clip, keyed, static_cast<uint16_t>(key & 0xFFFFu), false, false);
        return;
    } else if (SameSource(*e, src)) {
        e->changes = 0;
        e->lastUse = ++g_smoothUse;
        DrawEntry(*e, dst, dr, clip);
        return;
    } else if (++e->changes >= 4) {
        e->changes = 0;
        e->direct = 120;    // a picture that keeps changing: stretched directly for a while
        e->lastUse = ++g_smoothUse;
        Stretch16(dst, dr, src, sr, clip, keyed, static_cast<uint16_t>(key & 0xFFFFu), false, false);
        return;
    }
    g_smoothBytes -= e->bytes;
    try {
        BuildScaled(*e, src);
    } catch (const std::bad_alloc&) {
        for (SmoothEntry* c : g_smooth) delete c;
        g_smooth.clear();
        g_smoothBytes = 0;
        Stretch16(dst, dr, src, sr, clip, keyed, static_cast<uint16_t>(key & 0xFFFFu), false, false);
        return;
    }
    e->bytes = EntryBytes(*e);
    g_smoothBytes += e->bytes;
    e->lastUse = ++g_smoothUse;
    DrawEntry(*e, dst, dr, clip);
    TrimCache();
}

void DropScaledPictures() {
    for (auto it = g_smooth.begin(); it != g_smooth.end();) {
        if ((*it)->scaled) {
            g_smoothBytes -= (*it)->bytes;
            delete *it;
            it = g_smooth.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace DDM
