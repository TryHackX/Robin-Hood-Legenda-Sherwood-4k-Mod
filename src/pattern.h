#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include <windows.h>

namespace Memory {
    // Scan module memory for an IDA-style pattern (e.g. "68 80 02 00 00 E8 ? ? ? ?")
    uintptr_t FindPattern(HMODULE hModule, const char* pattern);

    // Safely write bytes to code/rdata sections with VirtualProtect + FlushInstructionCache
    bool PatchBytes(uintptr_t address, const void* data, size_t size);

    template <typename T>
    bool Write(uintptr_t address, const T& value) {
        return PatchBytes(address, &value, sizeof(T));
    }
}
