#include "audio_fix.h"
#include "config.h"
#include "window_manager.h"
#include "logger.h"
#include "diag.h"
#include "timing.h"
#include <MinHook.h>
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstring>

// NOTE: Fmod.dll is UPX-packed and imports WINMM.dll, so our proxy's DllMain runs BEFORE
// Fmod's unpacker. Hooks written at that point land in the still-empty UPX0 section and are
// wiped by the unpacker. AudioFix::Initialize() must therefore run from LateInit (the hooked
// Game.exe entry point), when every statically linked DLL has finished initialising.
namespace {
    using FSOUND_Init_t          = signed char (__stdcall*)(int mixrate, int maxsoftwarechannels, unsigned int flags);
    using FSOUND_SetBufferSize_t = signed char (__stdcall*)(int len_ms);
    using FSOUND_SetHWND_t       = signed char (__stdcall*)(void* hwnd);
    using FSOUND_PlaySound_t     = int   (__stdcall*)(int channel, void* sample);
    using FSOUND_PlaySoundEx_t   = int   (__stdcall*)(int channel, void* sample, void* dsp, signed char startPaused);
    using FSOUND_Stream_Play_t   = int   (__stdcall*)(int channel, void* stream);
    using FSOUND_Sample_Load_t   = void* (__stdcall*)(int index, const char* nameOrData, unsigned int mode, int memLength);

    FSOUND_Init_t          g_OrigFSOUND_Init          = nullptr;
    FSOUND_SetBufferSize_t g_OrigFSOUND_SetBufferSize = nullptr;
    FSOUND_SetHWND_t       g_OrigFSOUND_SetHWND       = nullptr;
    FSOUND_PlaySound_t     g_OrigPlaySound            = nullptr;
    FSOUND_PlaySoundEx_t   g_OrigPlaySoundEx          = nullptr;
    FSOUND_Stream_Play_t   g_OrigStreamPlay           = nullptr;
    FSOUND_Sample_Load_t   g_OrigSampleLoad           = nullptr;

    bool g_bufferSizeConfigured = false;

    signed char __stdcall Hook_FSOUND_SetBufferSize(int len_ms) {
        const int wanted = SherwoodConfig::Get().audioBufferMs;
        const int useMs = (wanted > 0) ? std::max(len_ms, wanted) : len_ms;
        g_bufferSizeConfigured = true;
        Logger::Info("AudioFix: FSOUND_SetBufferSize(requested=%d ms -> used=%d ms).", len_ms, useMs);
        return g_OrigFSOUND_SetBufferSize(useMs);
    }

    signed char __stdcall Hook_FSOUND_SetHWND(void* hwnd) {
        if (!hwnd) {
            hwnd = WindowManager::GetGameWindow();
        }
        Logger::Info("AudioFix: FSOUND_SetHWND(hwnd=0x%08X).", reinterpret_cast<uintptr_t>(hwnd));
        return g_OrigFSOUND_SetHWND(hwnd);
    }

    signed char __stdcall Hook_FSOUND_Init(int mixrate, int maxsoftwarechannels, unsigned int flags) {
        const int wanted = SherwoodConfig::Get().audioBufferMs;
        if (wanted > 0 && !g_bufferSizeConfigured && g_OrigFSOUND_SetBufferSize) {
            g_OrigFSOUND_SetBufferSize(wanted);
            g_bufferSizeConfigured = true;
        }
        const uint64_t t0 = Diag::Now();
        const signed char ok = g_OrigFSOUND_Init(mixrate, maxsoftwarechannels, flags);
        Logger::Info("AudioFix: FSOUND_Init(mixrate=%d, channels=%d, flags=0x%08X) -> %d in %.1f ms.",
                     mixrate, maxsoftwarechannels, flags, static_cast<int>(ok), Diag::TicksToMs(Diag::Now() - t0));
        return ok;
    }

    void CountStart(int result) {
        if (result == -1) {
            Diag::Add(Diag::C_SND_FAIL);
        }
        if (Diag::IsMainThread() && Timing::InExtraFrame()) {
            Diag::Add(Diag::C_SND_EXTRA);
        }
    }

    // Measurement only: how long does starting / loading a sound block the calling thread,
    // how often does it fail, and does the render path (extra frames) start sounds?
    int __stdcall Hook_FSOUND_PlaySound(int channel, void* sample) {
        int r;
        {
            Diag::Scope s(Diag::C_SND_PLAY);
            r = g_OrigPlaySound(channel, sample);
        }
        CountStart(r);
        return r;
    }

    int __stdcall Hook_FSOUND_PlaySoundEx(int channel, void* sample, void* dsp, signed char startPaused) {
        int r;
        {
            Diag::Scope s(Diag::C_SND_PLAY);
            r = g_OrigPlaySoundEx(channel, sample, dsp, startPaused);
        }
        CountStart(r);
        return r;
    }

    // ---- hearing range (game code, RHsound.cpp) ----------------------------------------------
    // 0x005B2F30 (thiscall, one float = camera zoom factor, ret 4) fills four distances at
    // this+0xB8..0xC4: how far from the centre of the view a positional sound can be heard.
    // At zoom 1 they are 800/240 and 640/240 pixels - tuned for 640-1024 pixel wide screens.
    // On a 2560 pixel wide view half of the screen lies outside that range, so fights away from
    // the centre are silent. Scale the distances with the internal width.
    using PFN_SetHearing = void(__fastcall*)(uint8_t* self, void* edx, float factor);
    PFN_SetHearing g_OrigSetHearing = nullptr;
    uint8_t* g_hearingObj = nullptr;
    float    g_hearingBase[4] = {};
    float    g_hearingApplied = 0.0f;
    constexpr int kHearingOffsets[4] = { 0xB8, 0xBC, 0xC0, 0xC4 };

    float HearingScale() {
        const auto& cfg = SherwoodConfig::Get();
        if (cfg.hearingRangePercent > 0) {
            return static_cast<float>(cfg.hearingRangePercent) / 100.0f;
        }
        int w = 0, h = 0;
        WindowManager::GetInternalResolution(w, h);
        return std::clamp(static_cast<float>(w) / 1024.0f, 1.0f, 3.0f);
    }

    void ApplyHearing() {
        if (!g_hearingObj) {
            return;
        }
        const float scale = HearingScale();
        for (int i = 0; i < 4; ++i) {
            *reinterpret_cast<float*>(g_hearingObj + kHearingOffsets[i]) = g_hearingBase[i] * scale;
        }
        if (scale != g_hearingApplied) {
            g_hearingApplied = scale;
            Logger::Info("AudioFix: hearing range x%.2f (%.0f/%.0f px -> %.0f/%.0f px).", scale,
                         g_hearingBase[0], g_hearingBase[1], g_hearingBase[0] * scale, g_hearingBase[1] * scale);
        }
    }

    // the level the object was seen in (the engine's map surface, E+0x48C4): another level may have
    // another sound object - the old one is only written to again after the game set its range anew
    void* g_hearingLevel = nullptr;
    void* CurrentLevel() {
        const auto* engine = *reinterpret_cast<const uint8_t* const*>(0x006C0FA8);
        if (!engine) return nullptr;
        __try {
            return *reinterpret_cast<void* const*>(engine + 0x48C4);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    void __fastcall Hook_SetHearing(uint8_t* self, void* edx, float factor) {
        g_OrigSetHearing(self, edx, factor);
        g_hearingObj = self;
        g_hearingLevel = CurrentLevel();
        for (int i = 0; i < 4; ++i) {
            g_hearingBase[i] = *reinterpret_cast<const float*>(self + kHearingOffsets[i]);
        }
        ApplyHearing();
    }

    int __stdcall Hook_FSOUND_Stream_Play(int channel, void* stream) {
        Diag::Scope s(Diag::C_SND_PLAY);
        return g_OrigStreamPlay(channel, stream);
    }

    void* __stdcall Hook_FSOUND_Sample_Load(int index, const char* nameOrData, unsigned int mode, int memLength) {
        Diag::Scope s(Diag::C_SND_LOAD);
        return g_OrigSampleLoad(index, nameOrData, mode, memLength);
    }
}

namespace AudioFix {

void Initialize() {
    HMODULE hFmod = GetModuleHandleA("fmod.dll");
    if (!hFmod) {
        Logger::Warn("AudioFix: fmod.dll not loaded; skipping FMOD hooks.");
        return;
    }

    int hooked = 0;
    auto hook = [&](const char* name, LPVOID detour, LPVOID* orig) {
        auto* target = reinterpret_cast<uint8_t*>(GetProcAddress(hFmod, name));
        if (!target) {
            return;
        }
        // Still packed (all zero)? Then hooking would be lost and would corrupt FMOD on unhook.
        if (target[0] == 0 && target[1] == 0 && target[2] == 0 && target[3] == 0) {
            Logger::Warn("AudioFix: %s is not unpacked yet - hook skipped.", name);
            return;
        }
        if (MH_CreateHook(target, detour, orig) == MH_OK && MH_EnableHook(target) == MH_OK) {
            ++hooked;
        }
    };

    hook("_FSOUND_SetBufferSize@4", reinterpret_cast<LPVOID>(&Hook_FSOUND_SetBufferSize), reinterpret_cast<LPVOID*>(&g_OrigFSOUND_SetBufferSize));
    hook("_FSOUND_SetHWND@4",       reinterpret_cast<LPVOID>(&Hook_FSOUND_SetHWND),       reinterpret_cast<LPVOID*>(&g_OrigFSOUND_SetHWND));
    hook("_FSOUND_Init@12",         reinterpret_cast<LPVOID>(&Hook_FSOUND_Init),          reinterpret_cast<LPVOID*>(&g_OrigFSOUND_Init));
    hook("_FSOUND_PlaySound@8",     reinterpret_cast<LPVOID>(&Hook_FSOUND_PlaySound),     reinterpret_cast<LPVOID*>(&g_OrigPlaySound));
    hook("_FSOUND_PlaySoundEx@16",  reinterpret_cast<LPVOID>(&Hook_FSOUND_PlaySoundEx),   reinterpret_cast<LPVOID*>(&g_OrigPlaySoundEx));
    hook("_FSOUND_Stream_Play@8",   reinterpret_cast<LPVOID>(&Hook_FSOUND_Stream_Play),   reinterpret_cast<LPVOID*>(&g_OrigStreamPlay));
    hook("_FSOUND_Sample_Load@16",  reinterpret_cast<LPVOID>(&Hook_FSOUND_Sample_Load),   reinterpret_cast<LPVOID*>(&g_OrigSampleLoad));

    Logger::Info("AudioFix: %d/7 fmod.dll hooks installed (late init, after UPX unpack).", hooked);

    // hearing range: fld [esp+4] / fcomp [0x67749C] / mov eax,[esp+4]
    static const uint8_t kHearingProlog[] = { 0xD9, 0x44, 0x24, 0x04, 0xD8, 0x1D, 0x9C, 0x74, 0x67, 0x00, 0x8B, 0x44, 0x24, 0x04 };
    auto* hearingAddr = reinterpret_cast<uint8_t*>(0x005B2F30);
    if (std::memcmp(hearingAddr, kHearingProlog, sizeof(kHearingProlog)) == 0 &&
        MH_CreateHook(hearingAddr, reinterpret_cast<LPVOID>(&Hook_SetHearing),
                      reinterpret_cast<LPVOID*>(&g_OrigSetHearing)) == MH_OK &&
        MH_EnableHook(hearingAddr) == MH_OK) {
        Logger::Info("AudioFix: hearing range scaling installed @ 0x005B2F30.");
    } else {
        Logger::Warn("AudioFix: hearing range function not recognised - positional sound range left as is.");
    }
}

void OnGameTick() {
    // the internal resolution may have changed since the game last set the range - written only
    // while the object belongs to the level running now (code review 2026-11-17)
    if (g_hearingObj && HearingScale() != g_hearingApplied && g_hearingLevel && CurrentLevel() == g_hearingLevel) {
        ApplyHearing();
    }
}

void Shutdown() {
    // Hooks cleaned up globally by MH_Uninitialize
}

} // namespace AudioFix
