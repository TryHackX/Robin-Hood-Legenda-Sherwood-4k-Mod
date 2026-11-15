#include "ddm_hd_font.h"
#include "ddm_hd.h"
#include "../logger.h"

#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace DDM::HD::Font {
namespace {
    // An upscaled strip: twice the size of the game's, colour and coverage per pixel.
    struct HdStrip {
        int w = 0, h = 0;               // HD size (2x the game's strip)
        std::vector<uint16_t> colour;   // RGB565
        std::vector<uint8_t>  alpha;    // 0..255
    };
    // A strip surface of the game, once recognised: which HD strip belongs to it (or none).
    struct Seen {
        uint64_t key = 0;
        uint32_t probe = 0;             // a few of its pixels, to notice another picture at the same place
        const HdStrip* hd = nullptr;
    };
    std::unordered_map<uint64_t, std::unique_ptr<HdStrip>> g_hd;   // by key; null = no file
    std::unordered_map<const uint8_t*, Seen> g_seen;               // by the strip's pixel address
    std::string g_dumpDir;                                         // test harness
    std::atomic<bool> g_dumping{ false };
    unsigned g_drawn = 0, g_strips = 0, g_hdStrips = 0;

    std::string GameDir() {
        char exe[MAX_PATH]{};
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        std::string path(exe);
        path.erase(path.find_last_of("\\/") + 1);
        return path;
    }

    uint64_t Fnv(const Buf& b, uint64_t h) {
        for (int y = 0; y < b.h; ++y) {
            const uint8_t* p = b.bits + static_cast<size_t>(y) * b.pitch;
            for (int x = 0; x < b.w * 2; ++x) h = (h ^ p[x]) * 1099511628211ull;
        }
        return h;
    }

    uint32_t Probe(const Buf& b) {
        uint32_t h = 2166136261u;
        for (int i = 0; i < 64; ++i) {
            const int x = (i * 977) % b.w, y = (i * 13) % b.h;
            h = (h ^ reinterpret_cast<const uint16_t*>(b.bits + static_cast<size_t>(y) * b.pitch)[x]) * 16777619u;
        }
        return h;
    }

    const HdStrip* Load(uint64_t key, int w, int h) {
        auto it = g_hd.find(key);
        if (it != g_hd.end()) return it->second.get();
        char name[64];
        std::snprintf(name, sizeof(name), "%016llx.hdfont", static_cast<unsigned long long>(key));
        const std::string path = GameDir() + "hd\\fonts\\" + name;
        std::unique_ptr<HdStrip> s;
        HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            struct { char magic[4]; uint32_t version, w, h; } hdr{};
            DWORD got = 0;
            if (ReadFile(f, &hdr, sizeof(hdr), &got, nullptr) && got == sizeof(hdr) && std::memcmp(hdr.magic, "HDFT", 4) == 0 &&
                hdr.version == 1 && static_cast<int>(hdr.w) == 2 * w && static_cast<int>(hdr.h) == 2 * h) {
                s = std::make_unique<HdStrip>();
                s->w = static_cast<int>(hdr.w);
                s->h = static_cast<int>(hdr.h);
                const size_t n = static_cast<size_t>(s->w) * s->h;
                s->colour.resize(n);
                s->alpha.resize(n);
                DWORD a = 0, b = 0;
                if (!ReadFile(f, s->colour.data(), static_cast<DWORD>(n * 2), &a, nullptr) || a != n * 2 ||
                    !ReadFile(f, s->alpha.data(), static_cast<DWORD>(n), &b, nullptr) || b != n) {
                    s.reset();
                }
            }
            CloseHandle(f);
            if (s) {
                ++g_hdStrips;
                Logger::Info("HD font: %s (%dx%d) for a strip of %dx%d.", name, s->w, s->h, w, h);
            } else {
                Logger::Warn("HD font: %s could not be read.", name);
            }
        }
        return (g_hd[key] = std::move(s)).get();
    }

    void Dump(uint64_t key, const Buf& colour, const Buf* alpha) {
        char name[96];
        std::snprintf(name, sizeof(name), "font_%016llx.raw", static_cast<unsigned long long>(key));
        const std::string path = g_dumpDir + name;
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) return;
        const uint32_t hdr[4] = { 0x53464253u /* "SBFS" */, static_cast<uint32_t>(colour.w), static_cast<uint32_t>(colour.h),
                                  alpha ? 1u : 0u };
        std::fwrite(hdr, sizeof(hdr), 1, f);
        for (int y = 0; y < colour.h; ++y) std::fwrite(colour.bits + static_cast<size_t>(y) * colour.pitch, 2, colour.w, f);
        if (alpha) {
            for (int y = 0; y < alpha->h; ++y) std::fwrite(alpha->bits + static_cast<size_t>(y) * alpha->pitch, 2, alpha->w, f);
        }
        std::fclose(f);
        Logger::Info("HD font: strip %dx%d%s dumped as %s.", colour.w, colour.h, alpha ? " (with coverage)" : "", name);
    }

    // The strip `colour` (and its coverage strip) - recognised once per surface.
    const HdStrip* Strip(const Buf& colour, const Buf* alpha) {
        if (colour.w < 64 || colour.h < 6 || colour.h > 96 || colour.w < 8 * colour.h) return nullptr;   // not a font strip
        if (alpha && (alpha->w != colour.w || alpha->h != colour.h)) return nullptr;
        Seen& seen = g_seen[colour.bits];
        const uint32_t probe = Probe(colour) ^ (alpha ? Probe(*alpha) * 31u : 0u);
        if (seen.key == 0 || seen.probe != probe) {
            uint64_t key = Fnv(colour, 1469598103934665603ull);
            if (alpha) key = Fnv(*alpha, key ^ 0x5A);
            key = (key & ~0xFFFull) | (static_cast<uint64_t>(colour.h) & 0xFFF);   // keep it readable: low bits = height
            seen.key = key;
            seen.probe = probe;
            seen.hd = Load(key, colour.w, colour.h);
            ++g_strips;
            if (g_dumping.load()) {
                Dump(key, colour, alpha);
            } else if (!seen.hd) {
                // a font without an HD version: kept for tools\hd\upscale_fonts.py (like the icons) -
                // only where the HD work folder exists (the machine the HD pictures are made on)
                static const std::string work = GameDir() + "hd\\work\\";
                static const bool collect = GetFileAttributesA(work.c_str()) != INVALID_FILE_ATTRIBUTES;
                if (collect) {
                    char name[64];
                    std::snprintf(name, sizeof(name), "fonts\\font_%016llx.raw", static_cast<unsigned long long>(key));
                    if (GetFileAttributesA((work + name).c_str()) == INVALID_FILE_ATTRIBUTES) {
                        const std::string before = g_dumpDir;
                        g_dumpDir = work + "fonts\\";
                        CreateDirectoryA(g_dumpDir.c_str(), nullptr);
                        Dump(key, colour, alpha);
                        g_dumpDir = before;
                    }
                }
            }
        }
        return seen.hd;
    }

    // c over d with coverage a8 (0..255). The channels spread apart (green in the upper half) so that
    // the products of a 5-bit weight cannot run into each other (with an 8-bit weight red ran into
    // blue: the letters got a blue outline).
    inline uint16_t Blend565(uint32_t c, uint32_t d, uint32_t a8) {
        const uint32_t a = (a8 + 4) >> 3;                          // 0..32
        const uint32_t cs = (c | (c << 16)) & 0x07E0F81Fu, ds = (d | (d << 16)) & 0x07E0F81Fu;
        const uint32_t r = ((cs * a + ds * (32 - a)) >> 5) & 0x07E0F81Fu;
        return static_cast<uint16_t>(r | (r >> 16));
    }

    // HD glyph cell (2*gx, 2*gy, w, h) onto `dst` at (0, 0) with pitch `dstPitch` (bytes), clipped by the caller
    void DrawHd(const HdStrip& hd, int hx, int hy, int w, int h, uint16_t* dst, int dstPitch, int x0, int y0, int x1, int y1) {
        for (int y = y0; y < y1; ++y) {
            if (hy + y < 0 || hy + y >= hd.h) continue;
            const uint16_t* c = hd.colour.data() + static_cast<size_t>(hy + y) * hd.w + hx;
            const uint8_t* a = hd.alpha.data() + static_cast<size_t>(hy + y) * hd.w + hx;
            auto* out = reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(dst) + static_cast<size_t>(y) * dstPitch);
            for (int x = x0; x < x1; ++x) {
                if (hx + x < 0 || hx + x >= hd.w) continue;
                const uint32_t al = a[x];
                if (al < 4) continue;
                out[x] = al >= 252 ? c[x] : Blend565(c[x], out[x], al);
            }
        }
        (void)w;
        (void)h;
    }
}

bool AlphaGlyph2x(const uint16_t* src, const uint16_t* alpha, uint16_t* dst, int dstW, int dstH, int dstPitch) {
    Buf sb{}, ab{};
    if (!FindLockedBuf(src, sb) || !FindLockedBuf(alpha, ab) || sb.bits == ab.bits) return false;
    const HdStrip* hd = Strip(sb, &ab);
    if (!hd) return false;
    const size_t off = static_cast<size_t>(reinterpret_cast<const uint8_t*>(src) - sb.bits);
    const int gx = static_cast<int>((off % static_cast<size_t>(sb.pitch)) / 2), gy = static_cast<int>(off / static_cast<size_t>(sb.pitch));
    const int w = dstW & ~1, h = dstH & ~1;
    DrawHd(*hd, 2 * gx, 2 * gy, w, h, dst, dstPitch, 0, 0, w, h);
    ++g_drawn;
    return true;
}

bool KeyedGlyph2x(const Buf& dst, const RECT& d, const RECT& clip, const Buf& strip, const RECT& s, uint16_t key) {
    (void)key;
    const HdStrip* hd = Strip(strip, nullptr);
    if (!hd) return false;
    auto* base = reinterpret_cast<uint16_t*>(dst.bits + static_cast<size_t>(d.top) * dst.pitch) + d.left;
    DrawHd(*hd, 2 * s.left, 2 * s.top, d.right - d.left, d.bottom - d.top, base, dst.pitch,
           clip.left - d.left, clip.top - d.top, clip.right - d.left, clip.bottom - d.top);
    ++g_drawn;
    return true;
}

bool FindAlphaGlyph(const uint16_t* src, const uint16_t* alpha, HdGlyph& out) {
    Buf sb{}, ab{};
    if (!FindLockedBuf(src, sb) || !FindLockedBuf(alpha, ab) || sb.bits == ab.bits) return false;
    const HdStrip* hd = Strip(sb, &ab);
    if (!hd) return false;
    const size_t off = static_cast<size_t>(reinterpret_cast<const uint8_t*>(src) - sb.bits);
    out.colour = hd->colour.data();
    out.alpha = hd->alpha.data();
    out.w = hd->w;
    out.h = hd->h;
    out.hx = 2 * static_cast<int>((off % static_cast<size_t>(sb.pitch)) / 2);
    out.hy = 2 * static_cast<int>(off / static_cast<size_t>(sb.pitch));
    ++g_drawn;
    return true;
}

bool FindKeyedGlyph(const Buf& strip, const RECT& s, HdGlyph& out) {
    const HdStrip* hd = Strip(strip, nullptr);
    if (!hd) return false;
    out.colour = hd->colour.data();
    out.alpha = hd->alpha.data();
    out.w = hd->w;
    out.h = hd->h;
    out.hx = 2 * static_cast<int>(s.left);
    out.hy = 2 * static_cast<int>(s.top);
    ++g_drawn;
    return true;
}

void SetDump(const char* dir) {
    g_dumpDir = dir ? dir : "";
    if (!g_dumpDir.empty() && g_dumpDir.back() != '\\') g_dumpDir += '\\';
    if (!g_dumpDir.empty()) CreateDirectoryA(g_dumpDir.c_str(), nullptr);
    g_seen.clear();                     // every strip is looked at (and written) again
    g_dumping.store(!g_dumpDir.empty());
}

int Describe(char* out, int size) {
    return std::snprintf(out, size, "strips=%u hdStrips=%u glyphs=%u dumping=%d", g_strips, g_hdStrips, g_drawn,
                         g_dumping.load() ? 1 : 0);
}

} // namespace DDM::HD::Font
