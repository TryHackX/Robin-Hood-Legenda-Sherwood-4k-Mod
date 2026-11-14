#pragma once
#include <cstddef>
#include <cstdint>

namespace MemoryOpt {
    void Initialize();
    void Shutdown();

    // Exposed SIMD helpers
    void* __cdecl FastMemcpy_SSE2(void* dst, const void* src, size_t count);
}
