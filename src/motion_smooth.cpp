#include "motion_smooth.h"
#include "config.h"
#include "diag.h"
#include "logger.h"

#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace MotionSmooth {
namespace {
    // Game.exe (see docs\OBJECT_RENDER.md)
    constexpr uintptr_t kAddrEnginePtr = 0x006C0FA8;            // RHEngine*
    constexpr uintptr_t kAddrPIUpdate  = 0x0055F290;            // RHPositionInterface::Update, thiscall(P), ret
    constexpr int kLists[] = { 0x49B8, 0x4998, 0x49D8 };        // vector<RHElement*>: ground FX, attached FX, sorted master list
    constexpr int kElemId      = 0x0C;                          // unique creation counter
    constexpr int kElemSprite  = 0x50;                          // RHSprite*
    constexpr int kPosIface    = 0x04;                          // P = S + 4
    constexpr int kPFlags      = 0x04;                          // validity bits (1 = world xyz valid)
    constexpr int kPWorld      = 0x0C;                          // float x, y, z ... origin, projected (0x1C bytes in all)
    constexpr int kPWorldBytes = 0x1C;
    constexpr int kPBBox       = 0xC0;                          // world bounding box (0x14 bytes)
    constexpr int kSCullRect   = 0x1A0;                         // screen-culling rect (0x14 bytes)
    constexpr int kRectBytes   = 0x14;

    constexpr float kMaxStep = 120.0f;   // more than this per tick is a teleport, not movement
    constexpr size_t kMaxElements = 4096;

    using PFN_PIUpdate = void(__thiscall*)(void* P);
    const auto g_fnPIUpdate = reinterpret_cast<PFN_PIUpdate>(kAddrPIUpdate);

    bool g_enabled = false;
    bool g_faulted = false;

    struct RawElem {
        uint8_t* el;
        uint8_t* sprite;
        uint32_t id;
        uint32_t flags;   // position validity bits; bit 0 = world xyz is valid
        float    pos[3];
    };
    RawElem g_raw[kMaxElements];

    struct Track {
        uint32_t id = 0;
        uint32_t tick = 0;
        float    cur[3] = {};
    };
    std::unordered_map<uint8_t*, Track> g_tracks;
    uint32_t g_tick = 0;

    struct Moving {
        uint8_t* sprite;
        float prev[3];
        float cur[3];
    };
    std::vector<Moving> g_moving;

    struct Saved {
        uint8_t* sprite;
        uint32_t flags;
        uint8_t  world[kPWorldBytes];
        uint8_t  bbox[kRectBytes];
        uint8_t  cull[kRectBytes];
    };
    std::vector<Saved> g_saved;

    void Fault(const char* where) {
        if (!g_faulted) {
            g_faulted = true;
            g_enabled = false;
            Logger::Error("MotionSmooth: access violation while %s - feature switched off for this session.", where);
        }
    }

    // Raw walk over the engine's element lists. Kept free of C++ objects so it can use SEH:
    // everything here is read through pointers recovered by static analysis.
    int ReadElements() {
        int n = 0;
        __try {
            auto* engine = *reinterpret_cast<uint8_t**>(kAddrEnginePtr);
            if (!engine) {
                return 0;
            }
            for (int offset : kLists) {
                auto** it  = *reinterpret_cast<uint8_t***>(engine + offset + 4);
                auto** end = *reinterpret_cast<uint8_t***>(engine + offset + 8);
                if (!it || end < it || static_cast<size_t>(end - it) > kMaxElements) {
                    continue;
                }
                for (; it < end && n < static_cast<int>(kMaxElements); ++it) {
                    uint8_t* el = *it;
                    if (!el) continue;
                    uint8_t* sprite = *reinterpret_cast<uint8_t**>(el + kElemSprite);
                    if (!sprite) continue;
                    RawElem& r = g_raw[n++];
                    r.el = el;
                    r.sprite = sprite;
                    r.id = *reinterpret_cast<const uint32_t*>(el + kElemId);
                    r.flags = *reinterpret_cast<const uint32_t*>(sprite + kPosIface + kPFlags);
                    std::memcpy(r.pos, sprite + kPosIface + kPWorld, sizeof(r.pos));
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return -1;
        }
        return n;
    }

    // Writes the interpolated position of one sprite and lets the game recompute what it
    // derives from it. Returns false on an access violation.
    bool OverrideOne(Saved* save, uint8_t* sprite, const float* pos) {
        __try {
            uint8_t* p = sprite + kPosIface;
            save->sprite = sprite;
            save->flags = *reinterpret_cast<const uint32_t*>(p + kPFlags);
            std::memcpy(save->world, p + kPWorld, kPWorldBytes);
            std::memcpy(save->bbox, p + kPBBox, kRectBytes);
            std::memcpy(save->cull, sprite + kSCullRect, kRectBytes);

            std::memcpy(p + kPWorld, pos, 3 * sizeof(float));
            *reinterpret_cast<uint32_t*>(p + kPFlags) = 1; // only the world position is valid
            g_fnPIUpdate(p);                               // projected position, draw origin, bbox
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
        return true;
    }

    bool RestoreOne(const Saved* save) {
        __try {
            uint8_t* p = save->sprite + kPosIface;
            std::memcpy(p + kPWorld, save->world, kPWorldBytes);
            std::memcpy(p + kPBBox, save->bbox, kRectBytes);
            std::memcpy(save->sprite + kSCullRect, save->cull, kRectBytes);
            *reinterpret_cast<uint32_t*>(p + kPFlags) = save->flags;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
        return true;
    }
}

bool Initialize() {
    const auto& cfg = SherwoodConfig::Get();
    if (!cfg.smoothMotion) {
        Logger::Info("MotionSmooth: disabled (SmoothMotion=0).");
        return false;
    }
    // RHPositionInterface::Update prologue: mov eax,fs:[0] / push -1 / push 0x6699D8 / push eax
    static const uint8_t kProlog[] = { 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x6A, 0xFF, 0x68, 0xD8, 0x99, 0x66, 0x00, 0x50 };
    if (std::memcmp(reinterpret_cast<const void*>(kAddrPIUpdate), kProlog, sizeof(kProlog)) != 0) {
        Logger::Warn("MotionSmooth: position update function not recognised - object smoothing disabled.");
        return false;
    }
    g_moving.reserve(256);
    g_saved.reserve(256);
    g_enabled = true;
    Logger::Info("MotionSmooth: object position interpolation enabled (PI::Update @ 0x%08X).",
                 static_cast<uint32_t>(kAddrPIUpdate));
    return true;
}

bool IsEnabled() {
    return g_enabled;
}

void CaptureTick() {
    g_moving.clear();
    if (!g_enabled) {
        return;
    }
    const int n = ReadElements();
    if (n < 0) {
        Fault("reading the element lists");
        return;
    }
    ++g_tick;

    for (int i = 0; i < n; ++i) {
        const RawElem& r = g_raw[i];
        Track& t = g_tracks[r.el];
        if (t.tick == g_tick) {
            continue; // listed twice
        }
        if (!(r.flags & 1) || !std::isfinite(r.pos[0]) || !std::isfinite(r.pos[1]) || !std::isfinite(r.pos[2])) {
            t.tick = 0; // no valid world position this tick: start over when it has one
            continue;
        }
        const bool known = (t.id == r.id && t.tick + 1 == g_tick);
        if (known) {
            const float dx = r.pos[0] - t.cur[0];
            const float dy = r.pos[1] - t.cur[1];
            const float dz = r.pos[2] - t.cur[2];
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 > 0.0001f && d2 < kMaxStep * kMaxStep) {
                Moving m;
                m.sprite = r.sprite;
                std::memcpy(m.prev, t.cur, sizeof(m.prev));
                std::memcpy(m.cur, r.pos, sizeof(m.cur));
                g_moving.push_back(m);
            }
        }
        t.id = r.id;
        t.tick = g_tick;
        std::memcpy(t.cur, r.pos, sizeof(t.cur));
    }

    Diag::Add(Diag::C_MOTION);
    Diag::AddValue(Diag::C_MOTION, g_moving.size());

    // forget elements that left the lists (their addresses get reused)
    if ((g_tick & 0xFF) == 0) {
        for (auto it = g_tracks.begin(); it != g_tracks.end();) {
            if (it->second.tick + 2 < g_tick) {
                it = g_tracks.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void Apply(float alpha) {
    g_saved.clear();
    if (!g_enabled || g_moving.empty() || alpha >= 0.999f) {
        return;
    }
    if (alpha < 0.0f) alpha = 0.0f;
    for (const Moving& m : g_moving) {
        float pos[3];
        for (int k = 0; k < 3; ++k) {
            pos[k] = m.prev[k] + (m.cur[k] - m.prev[k]) * alpha;
        }
        Saved s{};
        if (!OverrideOne(&s, m.sprite, pos)) {
            if (s.sprite) {
                RestoreOne(&s); // undo whatever was written before the fault
            }
            Fault("overriding a position");
            break;
        }
        g_saved.push_back(s);
    }
}

void Restore() {
    for (auto it = g_saved.rbegin(); it != g_saved.rend(); ++it) {
        if (!RestoreOne(&*it)) {
            Fault("restoring a position");
        }
    }
    g_saved.clear();
}

} // namespace MotionSmooth
