#include "save_thumbs.h"
#include "config.h"
#include "logger.h"
#include "settings_menu.h"
#include "ui_scale.h"
#include "view_zoom.h"
#include "ddm/ddm_hd.h"
#include "ddm/ddm_hd_patch.h"

#include <MinHook.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

// See save_thumbs.h. Game.exe (static RE 2026-11-24):
//   0x004DA2F0 RHEngine::MakeSavePicture thiscall(engine) -> al: SBPicture at engine +0xE4, its vtable +0x14 takes the
//              frame of [engine+0xDC], then SBPicture::Resize 0x00612300 (Vec2(160, 120), 1) - for RGB565 the filtered
//              0x006128B0. Called by RHGame 0x0050F37A, 0x0050F4CE, 0x00512E43, 0x00513096 (the game's menu opens);
//              RHGame::Save 0x00511340 copies the picture (0x006134A0) and writes it to <save>_t (0x00611960:
//              u16 w, u16 h, u32 bytes per pixel, u32 size, bzip2).
//   SBPicture: vtable 0x0067A880, +4 kind (3 = RGB565), +0x14 pixels, +0x18 colour key, +0x1C bytes of pixels, +0x20 u16
//              width, +0x22 u16 height, +0x24 u16 row length, +0x30 compression in the file (2 = bzip2) [runtime].
//   0x00531BD0 the load / save screen: the entry clicked ([dialog+0x760]) -> its RHSaveGame -> picture file name
//              (0x0056E6B0) -> SBPicture load 0x00611660 -> a surface of it ([dialog+0x8C4]) drawn 1:1.
namespace SaveThumbs {
namespace {
    constexpr uintptr_t kMakeSavePicture = 0x004DA2F0;
    constexpr int kPicW = 160, kPicH = 120;

    using PFN_MakeSavePicture = char(__fastcall*)(uint8_t* engine, void* edx);
    PFN_MakeSavePicture o_MakeSavePicture = nullptr;
    bool g_installed = false;

    // test harness / log: the last picture made
    unsigned g_made = 0;
    float g_lastX = 0.0f, g_lastY = 0.0f, g_lastW = 0.0f, g_lastH = 0.0f;
    bool g_lastMagnified = false;

    struct Picture {
        uint16_t* px = nullptr;
        int w = 0, h = 0, stride = 0;
    };

    bool PictureAt(uint8_t* pic, Picture& out) {
        uint32_t kind = 0, bytes = 0;
        __try {
            kind = *reinterpret_cast<const uint32_t*>(pic + 4);
            out.px = *reinterpret_cast<uint16_t* const*>(pic + 0x14);
            bytes = *reinterpret_cast<const uint32_t*>(pic + 0x1C);
            out.w = *reinterpret_cast<const uint16_t*>(pic + 0x20);
            out.h = *reinterpret_cast<const uint16_t*>(pic + 0x22);
            out.stride = *reinterpret_cast<const uint16_t*>(pic + 0x24);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
        // kind 3 = RGB565 (the save picture: 160x120, 38400 bytes of pixels)
        return kind == 3 && out.px && out.w > 0 && out.h > 0 && out.stride >= out.w &&
               bytes == static_cast<uint32_t>(out.stride) * static_cast<uint32_t>(out.h) * 2u;
    }

    // every picture pixel the mean of the source pixels it covers (RGB565 channels)
    void Shrink(const uint16_t* src, int stride, double rx, double ry, double rw, double rh, const Picture& out) {
        for (int oy = 0; oy < out.h; ++oy) {
            const int y0 = static_cast<int>(ry + rh * oy / out.h);
            const int y1 = (std::max)(y0 + 1, static_cast<int>(ry + rh * (oy + 1) / out.h));
            uint16_t* row = out.px + static_cast<size_t>(oy) * out.stride;
            for (int ox = 0; ox < out.w; ++ox) {
                const int x0 = static_cast<int>(rx + rw * ox / out.w);
                const int x1 = (std::max)(x0 + 1, static_cast<int>(rx + rw * (ox + 1) / out.w));
                uint32_t r = 0, g = 0, b = 0, n = 0;
                for (int y = y0; y < y1; ++y) {
                    const uint16_t* s = src + static_cast<size_t>(y) * stride;
                    for (int x = x0; x < x1; ++x) {
                        const uint32_t p = s[x];
                        r += p >> 11;
                        g += (p >> 5) & 63;
                        b += p & 31;
                        ++n;
                    }
                }
                row[ox] = static_cast<uint16_t>((((r + n / 2) / n) << 11) | (((g + n / 2) / n) << 5) | ((b + n / 2) / n));
            }
        }
    }

    bool ShrinkGuarded(const uint16_t* src, int stride, double rx, double ry, double rw, double rh, const Picture& out) {
        __try {
            Shrink(src, stride, rx, ry, rw, rh, out);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    void HexOf(const uint8_t* p, int n, char* out) {
        out[0] = 0;
        __try {
            for (int i = 0; i < n; ++i) std::snprintf(out + 3 * i, 4, "%02X ", p[i]);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    // the game's picture of the save made again from the world the player saw
    void Redraw(uint8_t* engine) {
        Picture pic;
        if (!PictureAt(engine + 0xE4, pic) || pic.w != kPicW || pic.h != kPicH) {
            static bool once = false;
            if (!once) {
                once = true;
                char hex[3 * 0x40 + 1];
                HexOf(engine + 0xE4, 0x40, hex);
                Logger::Warn("Saves: the game's picture of the save is not the RGB565 160x120 one expected (%dx%d) - left as it is. "
                             "Its first bytes: %s", pic.w, pic.h, hex);
            }
            return;
        }
        float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
        if (!ViewZoom::SeenWorld(x, y, w, h)) return;
        const uint16_t* src = nullptr;
        int sw = 0, sh = 0, stride = 0;
        ViewZoom::View lupa;
        std::vector<uint16_t> uncovered;    // the frame without the game's cursor
        const bool magnified = ViewZoom::Magnifying() && ViewZoom::LastWorld(lupa) && lupa.snap && lupa.w > 0 && lupa.h > 0;
        if (magnified) {
            src = lupa.snap;    // the world before the windows were drawn
            sw = lupa.w;
            sh = lupa.h;
            stride = lupa.w;
        } else {
            // the frame presented last (Flip rotated it to the front; the back holds the one before)
            const uint8_t* bits = nullptr;
            int pitch = 0;
            if (!DDM::FrontBits(bits, pitch, sw, sh) || !bits || pitch < sw * 2) return;
            src = reinterpret_cast<const uint16_t*>(bits);
            stride = pitch / 2;
            // the game's cursor (the frame still holds it as drawn): what it covered instead
            DDM::HD::Patch::Cursor cur;
            if (DDM::HD::Patch::GetCursor(bits, pitch, cur) && cur.under && cur.x >= 0 && cur.y >= 0 && cur.x + cur.w <= sw &&
                cur.y + cur.h <= sh) {
                uncovered.assign(src, src + static_cast<size_t>(stride) * sh);
                for (int r = 0; r < cur.h; ++r) {
                    std::memcpy(uncovered.data() + static_cast<size_t>(cur.y + r) * stride + cur.x, cur.under + static_cast<size_t>(r) * cur.w,
                                static_cast<size_t>(cur.w) * 2);
                }
                src = uncovered.data();
            }
            // the frame holds the windows too: below the interface's top row (money, shields; in the camp the team's
            // portraits, 48 of its pixels high) and above the hedge's curled corners (165 of its pixels high)
            const float f = static_cast<float>((std::max)(1, UiScale::Factor()));
            const float top = 50.0f * f;
            const float bottom = SherwoodConfig::Get().fullHeightView ? static_cast<float>(sh) - 165.0f * f : static_cast<float>(sh);
            if (y < top) {
                h -= top - y;
                y = top;
            }
            if (y + h > bottom) h = bottom - y;
        }
        // inside the source, then its middle 4:3
        double rx = (std::max)(0.0, static_cast<double>(x)), ry = (std::max)(0.0, static_cast<double>(y));
        double rw = (std::min)(static_cast<double>(sw) - rx, static_cast<double>(w) - (rx - x));
        double rh = (std::min)(static_cast<double>(sh) - ry, static_cast<double>(h) - (ry - y));
        if (rw < kPicW || rh < kPicH) return;
        if (rw * 3.0 > rh * 4.0) {
            const double cw = rh * 4.0 / 3.0;
            rx += (rw - cw) * 0.5;
            rw = cw;
        } else {
            const double ch = rw * 3.0 / 4.0;
            ry += (rh - ch) * 0.5;
            rh = ch;
        }
        if (!ShrinkGuarded(src, stride, rx, ry, rw, rh, pic)) return;
        ++g_made;
        g_lastX = static_cast<float>(rx);
        g_lastY = static_cast<float>(ry);
        g_lastW = static_cast<float>(rw);
        g_lastH = static_cast<float>(rh);
        g_lastMagnified = magnified;
        static bool logged = false;
        if (!logged) {
            logged = true;
            Logger::Info("Saves: the save's picture from the world the player sees (%.0fx%.0f at %.0f,%.0f of the %s, 4:3) instead "
                         "of the whole frame squeezed into 160x120.", rw, rh, rx, ry, magnified ? "magnified world" : "frame");
        }
    }

    uint8_t* g_lastEngine = nullptr;     // test harness (DumpPicture): the object the last picture belongs to

    char __fastcall Hook_MakeSavePicture(uint8_t* engine, void* edx) {
        const char made = o_MakeSavePicture(engine, edx);
        if (made && engine) {
            g_lastEngine = engine;
            Redraw(engine);
        }
        return made;
    }

    // ---- the date under the picture on the load / save screens
    struct Label {
        char  text[24] = "";
        DWORD noted = 0;            // when the game opened the picture file (0 = none)
        bool  bound = false;        // the picture of that file was drawn: its surface
        unsigned serial = 0;
        RECT  rect{};
        DWORD drawn = 0;            // when it was drawn last
    };
    std::mutex g_labelLock;
    Label g_label;
}

void Install() {
    static const uint8_t kProlog[] = { 0x6A, 0xFF, 0x68, 0x30, 0xE1, 0x65, 0x00, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00 };
    if (std::memcmp(reinterpret_cast<const void*>(kMakeSavePicture), kProlog, sizeof(kProlog)) == 0 &&
        MH_CreateHook(reinterpret_cast<LPVOID>(kMakeSavePicture), reinterpret_cast<LPVOID>(&Hook_MakeSavePicture),
                      reinterpret_cast<LPVOID*>(&o_MakeSavePicture)) == MH_OK &&
        MH_EnableHook(reinterpret_cast<LPVOID>(kMakeSavePicture)) == MH_OK) {
        g_installed = true;
        Logger::Info("Saves: the saves' pictures are made from the world the player sees; the load and save screens show "
                     "the date of the save clicked.");
    } else {
        o_MakeSavePicture = nullptr;
        Logger::Warn("Saves: the routine of the save's picture (0x%08X) not recognised - pictures as the game makes them.",
                     static_cast<unsigned>(kMakeSavePicture));
    }
}

void NoteFileOpened(const char* name, DWORD access) {
    if (!name || !(access & GENERIC_READ) || (access & GENERIC_WRITE)) return;
    const size_t n = std::strlen(name);
    if (n < 4 || _stricmp(name + n - 2, "_t") != 0) return;
    // a picture of the game's saves (Data\Savegame\Profile_NNN\<save>_t)
    std::string lower(name);
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower.find("savegame") == std::string::npos) return;
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    const std::string save(name, n - 2);
    if (!GetFileAttributesExA(save.c_str(), GetFileExInfoStandard, &fa) && !GetFileAttributesExA(name, GetFileExInfoStandard, &fa)) {
        return;
    }
    SYSTEMTIME utc{}, local{};
    if (!FileTimeToSystemTime(&fa.ftLastWriteTime, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) return;
    std::lock_guard<std::mutex> lk(g_labelLock);
    std::snprintf(g_label.text, sizeof(g_label.text), "%02u.%02u.%04u %02u:%02u", local.wDay, local.wMonth, local.wYear,
                  local.wHour, local.wMinute);
    g_label.noted = GetTickCount() | 1u;
    g_label.bound = false;
    g_label.serial = 0;
    g_label.drawn = 0;
}

void NoteScreenBlit(unsigned serial, int srcW, int srcH, const RECT& d) {
    if (srcW != kPicW || srcH != kPicH) return;
    std::lock_guard<std::mutex> lk(g_labelLock);
    const DWORD now = GetTickCount();
    if (!g_label.bound) {
        // the first picture of a save's size drawn after the game read a save's picture file: that picture
        if (!g_label.noted || now - g_label.noted > 3000) return;
        g_label.bound = true;
        g_label.serial = serial;
    }
    if (serial != g_label.serial) return;
    g_label.rect = d;
    g_label.drawn = now;
}

void Draw(uint8_t* dst, LONG dstPitch, bool dst8888, int w, int h, int originX, int originY) {
    char text[sizeof(Label::text)];
    RECT r{};
    {
        std::lock_guard<std::mutex> lk(g_labelLock);
        // shown while the screen draws the picture (it does so every frame)
        if (!g_label.bound || !g_label.drawn || GetTickCount() - g_label.drawn > 400) return;
        std::memcpy(text, g_label.text, sizeof(text));
        r = g_label.rect;
    }
    const int pw = r.right - r.left;
    if (pw <= 0) return;
    const int scale = (std::max)(1, pw / 80);   // letters 2x for a picture drawn 160 wide
    SettingsMenu::DrawLabel(dst, dstPitch, dst8888, w, h, (r.left + r.right) / 2 - originX, r.bottom - originY + 2 * scale, text,
                            scale, 0xFE4A);
}

bool DumpPicture(const char* path) {
    uint8_t* const engine = g_lastEngine;
    Picture pic;
    if (!engine || !PictureAt(engine + 0xE4, pic)) return false;
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    const uint32_t hdr[2] = { static_cast<uint32_t>(pic.w), static_cast<uint32_t>(pic.h) };
    std::fwrite(hdr, sizeof(hdr), 1, f);
    for (int y = 0; y < pic.h; ++y) std::fwrite(pic.px + static_cast<size_t>(y) * pic.stride, 2, static_cast<size_t>(pic.w), f);
    std::fclose(f);
    return true;
}

int Describe(char* out, int size) {
    std::lock_guard<std::mutex> lk(g_labelLock);
    return std::snprintf(out, size, "installed=%d made=%u last=%.0f,%.0f %.0fx%.0f %s label='%s' bound=%d serial=%u rect=%ld,%ld,%ld,%ld drawn=%lums",
                         g_installed ? 1 : 0, g_made, g_lastX, g_lastY, g_lastW, g_lastH, g_lastMagnified ? "magnified" : "frame",
                         g_label.text, g_label.bound ? 1 : 0, g_label.serial, g_label.rect.left, g_label.rect.top,
                         g_label.rect.right, g_label.rect.bottom,
                         g_label.drawn ? static_cast<unsigned long>(GetTickCount() - g_label.drawn) : 0ul);
}

} // namespace SaveThumbs
