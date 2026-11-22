#include "memory_opt.h"
#include "config.h"
#include "logger.h"
#include "pattern.h"
#include <MinHook.h>
#include <windows.h>
#include <emmintrin.h>
#include <cstdint>
#include <cstring>

namespace {
    using Memcpy_t = void* (__cdecl*)(void* dst, const void* src, size_t count);
    Memcpy_t g_OrigCrtMemcpy = nullptr;

    constexpr uintptr_t kAddrFogClearLoop     = 0x0058FF83;
    constexpr uintptr_t kAddrFogClearEpilogue = 0x0058FF90;

    // Verify IMAGE_FILE_LARGE_ADDRESS_AWARE (4 GB virtual address space on 64-bit Windows 11)
    void VerifyLargeAddressAware() {
        HMODULE hExe = GetModuleHandleA(nullptr);
        bool laaHeaderSet = false;
        if (hExe) {
            auto* dos = reinterpret_cast<PIMAGE_DOS_HEADER>(hExe);
            if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
                auto* nt = reinterpret_cast<PIMAGE_NT_HEADERS>(
                    reinterpret_cast<uint8_t*>(hExe) + dos->e_lfanew);
                if (nt->Signature == IMAGE_NT_SIGNATURE) {
                    laaHeaderSet = (nt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) != 0;
                }
            }
        }

        MEMORYSTATUSEX memStatus{};
        memStatus.dwLength = sizeof(memStatus);
        GlobalMemoryStatusEx(&memStatus);
        const uint32_t totalVirtualMB = static_cast<uint32_t>(memStatus.ullTotalVirtual / (1024ULL * 1024ULL));

        // Only reported. Until 2026-11-22 a missing flag made every start copy Game.exe to Game.exe.bak and try to
        // write the flag into the running exe - which Windows refuses (audit): the GOG exe stays as it is, the mod
        // keeps its large pictures out of the 2 GB address space (strips, textures on the graphics card).
        Logger::Info("MemoryOpt: LARGE_ADDRESS_AWARE=%s | Virtual Address Space=%u MB.",
                     laaHeaderSet ? "ENABLED" : "DISABLED", totalVirtualMB);
    }

    // SSE2-accelerated Fog of War rectangle clear (fills rows with 0xFFFFFFFF)
    extern "C" void __stdcall ClearFogRows_SSE2(
        uint8_t* dst, uint32_t dwordCountPerRow, int32_t rowDeltaBytes, uint32_t rowCount)
    {
        if (!dst || dwordCountPerRow == 0 || rowCount == 0) {
            return;
        }

        const __m128i allOnes = _mm_set1_epi32(-1);
        const uint32_t rowBytes = dwordCountPerRow * 4;
        const int32_t fullStride = static_cast<int32_t>(rowBytes) + rowDeltaBytes;

        for (uint32_t r = 0; r < rowCount; ++r) {
            uint8_t* p = dst;
            uint32_t remBytes = rowBytes;

            // 64-byte unrolled SSE2 stores (16 dwords per iteration)
            while (remBytes >= 64) {
                _mm_storeu_si128(reinterpret_cast<__m128i*>(p + 0),  allOnes);
                _mm_storeu_si128(reinterpret_cast<__m128i*>(p + 16), allOnes);
                _mm_storeu_si128(reinterpret_cast<__m128i*>(p + 32), allOnes);
                _mm_storeu_si128(reinterpret_cast<__m128i*>(p + 48), allOnes);
                p += 64;
                remBytes -= 64;
            }
            while (remBytes >= 16) {
                _mm_storeu_si128(reinterpret_cast<__m128i*>(p), allOnes);
                p += 16;
                remBytes -= 16;
            }
            while (remBytes >= 4) {
                *reinterpret_cast<uint32_t*>(p) = 0xFFFFFFFFu;
                p += 4;
                remBytes -= 4;
            }

            dst += fullStride;
        }
    }

    __declspec(naked) void FogClearSSE2Detour() {
        __asm {
            // At 0x0058FF83:
            // edi = dstPtr, esi = dwordCountPerRow, ebx = rowDeltaBytes, edx = rowCount
            push edx
            push ebx
            push esi
            push edi
            call ClearFogRows_SSE2
            mov eax, kAddrFogClearEpilogue
            jmp eax
        }
    }
}

namespace MemoryOpt {

void* __cdecl FastMemcpy_SSE2(void* dst, const void* src, size_t count) {
    if (count < 128 || !dst || !src) {
        return g_OrigCrtMemcpy ? g_OrigCrtMemcpy(dst, src, count) : std::memcpy(dst, src, count);
    }

    auto* d = static_cast<uint8_t*>(dst);
    const auto* s = static_cast<const uint8_t*>(src);

    // Check for overlapping regions (in case game calls memcpy on overlapping buffers)
    const uintptr_t dAddr = reinterpret_cast<uintptr_t>(d);
    const uintptr_t sAddr = reinterpret_cast<uintptr_t>(s);
    if ((dAddr > sAddr && dAddr - sAddr < count) ||
        (sAddr > dAddr && sAddr - dAddr < count)) {
        return g_OrigCrtMemcpy ? g_OrigCrtMemcpy(dst, src, count) : std::memmove(dst, src, count);
    }

    size_t rem = count;

    // For very large surface transfers (>= 256 KB, e.g. full HD/1440p/4K frame copies),
    // align destination to 16 bytes and use non-temporal streaming stores to avoid cache pollution.
    if (rem >= 262144) {
        while ((reinterpret_cast<uintptr_t>(d) & 15) != 0 && rem > 0) {
            *d++ = *s++;
            --rem;
        }
        while (rem >= 64) {
            __m128i v0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + 0));
            __m128i v1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + 16));
            __m128i v2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + 32));
            __m128i v3 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + 48));
            _mm_stream_si128(reinterpret_cast<__m128i*>(d + 0),  v0);
            _mm_stream_si128(reinterpret_cast<__m128i*>(d + 16), v1);
            _mm_stream_si128(reinterpret_cast<__m128i*>(d + 32), v2);
            _mm_stream_si128(reinterpret_cast<__m128i*>(d + 48), v3);
            s += 64;
            d += 64;
            rem -= 64;
        }
        _mm_sfence();
    } else {
        // Standard 64-byte unrolled L1/L2 cached SSE2 copy
        while (rem >= 64) {
            __m128i v0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + 0));
            __m128i v1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + 16));
            __m128i v2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + 32));
            __m128i v3 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + 48));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(d + 0),  v0);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(d + 16), v1);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(d + 32), v2);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(d + 48), v3);
            s += 64;
            d += 64;
            rem -= 64;
        }
    }

    while (rem >= 16) {
        _mm_storeu_si128(reinterpret_cast<__m128i*>(d),
                         _mm_loadu_si128(reinterpret_cast<const __m128i*>(s)));
        s += 16;
        d += 16;
        rem -= 16;
    }
    while (rem >= 4) {
        *reinterpret_cast<uint32_t*>(d) = *reinterpret_cast<const uint32_t*>(s);
        s += 4;
        d += 4;
        rem -= 4;
    }
    while (rem > 0) {
        *d++ = *s++;
        --rem;
    }

    return dst;
}

void Initialize() {
    VerifyLargeAddressAware();

    const auto& cfg = SherwoodConfig::Get();
    HMODULE hGame = GetModuleHandleA(nullptr);

    if (cfg.useSSE2Blit) {
        // Hook internal MSVC6 CRT memcpy at 0x00642580
        uintptr_t addrMemcpy = Memory::FindPattern(
            hGame, "55 8B EC 57 56 8B 75 0C 8B 4D 10 8B 7D 08 8B C1 8B D1 03 C6 3B FE 76 08");
        if (!addrMemcpy) {
            addrMemcpy = 0x00642580;
        }
        if (MH_CreateHook(reinterpret_cast<LPVOID>(addrMemcpy),
                          reinterpret_cast<LPVOID>(&FastMemcpy_SSE2),
                          reinterpret_cast<LPVOID*>(&g_OrigCrtMemcpy)) == MH_OK) {
            MH_EnableHook(reinterpret_cast<LPVOID>(addrMemcpy));
            Logger::Info("MemoryOpt: Hooked internal CRT memcpy at 0x%08X with SIMD SSE2 routine.",
                         static_cast<uint32_t>(addrMemcpy));
        }
    }

    if (cfg.optimizeFogOfWar) {
        // Patch RHSight::ClearFogRect inner loop (0x0058FF83) to use SSE2 128-bit vector stores
        // 0x0058FF83: 33 C0 F7 D0 8B CE F3 AB 03 FB 4A 75 F7
        if (*reinterpret_cast<uint8_t*>(kAddrFogClearLoop) == 0x33 &&
            *reinterpret_cast<uint8_t*>(kAddrFogClearLoop + 1) == 0xC0 &&
            *reinterpret_cast<uint8_t*>(kAddrFogClearLoop + 6) == 0xF3 &&
            *reinterpret_cast<uint8_t*>(kAddrFogClearLoop + 7) == 0xAB) {
            uint8_t patch[13];
            patch[0] = 0xE9; // JMP rel32
            int32_t rel = static_cast<int32_t>(
                reinterpret_cast<uintptr_t>(&FogClearSSE2Detour) - (kAddrFogClearLoop + 5));
            std::memcpy(&patch[1], &rel, 4);
            std::memset(&patch[5], 0x90, 8); // NOP padding up to 0x0058FF90
            Memory::PatchBytes(kAddrFogClearLoop, patch, sizeof(patch));
            Logger::Info("MemoryOpt: Patched RHSight::ClearFogRect at 0x%08X with SSE2 Fog clear.",
                         static_cast<uint32_t>(kAddrFogClearLoop));
        }
    }
}

void Shutdown() {
    // Hooks cleaned up globally by MH_Uninitialize
}

} // namespace MemoryOpt
