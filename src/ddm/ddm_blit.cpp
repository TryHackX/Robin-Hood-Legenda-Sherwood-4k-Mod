#include "ddm_blit.h"

#include <emmintrin.h>
#include <algorithm>
#include <climits>
#include <cstring>
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
        auto* d = reinterpret_cast<uint16_t*>(dst.bits + static_cast<size_t>(y) * dst.pitch) + clip.left;
        const uint32_t* ca = a2.color.data();
        const uint32_t* cb = b.color.data();
        const uint8_t* oa = a2.opaque.data();
        const uint8_t* ob = b.opaque.data();
        const uint8_t* sa = a2.shadow.data();
        const uint8_t* sb = b.shadow.data();
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
            acc = ((acc + 0x01004008u) >> 4) & 0x07E0F81Fu;
            d[n] = static_cast<uint16_t>(acc | (acc >> 16));
        }
    }
}

} // namespace DDM
