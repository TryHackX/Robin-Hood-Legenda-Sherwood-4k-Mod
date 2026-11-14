#include "pattern.h"
#include <cstdlib>
#include <cstring>

namespace Memory {
    namespace {
        struct PatternByte {
            uint8_t value;
            bool wildcard;
        };

        std::vector<PatternByte> ParsePattern(const char* pattern) {
            std::vector<PatternByte> bytes;
            const char* cur = pattern;
            while (*cur) {
                if (*cur == ' ') {
                    ++cur;
                    continue;
                }
                if (*cur == '?') {
                    ++cur;
                    if (*cur == '?') ++cur;
                    bytes.push_back({ 0, true });
                } else {
                    char* end = nullptr;
                    uint8_t val = static_cast<uint8_t>(strtoul(cur, &end, 16));
                    bytes.push_back({ val, false });
                    cur = end;
                }
            }
            return bytes;
        }

        bool GetModuleBounds(HMODULE hModule, uintptr_t& outBase, size_t& outSize) {
            if (!hModule) hModule = GetModuleHandleA(nullptr);
            auto* dos = reinterpret_cast<PIMAGE_DOS_HEADER>(hModule);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
            auto* nt = reinterpret_cast<PIMAGE_NT_HEADERS>(reinterpret_cast<uintptr_t>(hModule) + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
            outBase = reinterpret_cast<uintptr_t>(hModule);
            outSize = nt->OptionalHeader.SizeOfImage;
            return true;
        }
    }

    uintptr_t FindPattern(HMODULE hModule, const char* pattern) {
        uintptr_t base = 0;
        size_t size = 0;
        if (!GetModuleBounds(hModule, base, size)) return 0;

        auto pat = ParsePattern(pattern);
        if (pat.empty() || size < pat.size()) return 0;

        const auto* scanBytes = reinterpret_cast<const uint8_t*>(base);
        const size_t patLen = pat.size();
        const size_t limit = size - patLen;

        for (size_t i = 0; i <= limit; ++i) {
            bool found = true;
            for (size_t j = 0; j < patLen; ++j) {
                if (!pat[j].wildcard && scanBytes[i + j] != pat[j].value) {
                    found = false;
                    break;
                }
            }
            if (found) {
                return base + i;
            }
        }
        return 0;
    }

    std::vector<uintptr_t> FindAllPatterns(HMODULE hModule, const char* pattern) {
        std::vector<uintptr_t> results;
        uintptr_t base = 0;
        size_t size = 0;
        if (!GetModuleBounds(hModule, base, size)) return results;

        auto pat = ParsePattern(pattern);
        if (pat.empty() || size < pat.size()) return results;

        const auto* scanBytes = reinterpret_cast<const uint8_t*>(base);
        const size_t patLen = pat.size();
        const size_t limit = size - patLen;

        for (size_t i = 0; i <= limit; ++i) {
            bool found = true;
            for (size_t j = 0; j < patLen; ++j) {
                if (!pat[j].wildcard && scanBytes[i + j] != pat[j].value) {
                    found = false;
                    break;
                }
            }
            if (found) {
                results.push_back(base + i);
            }
        }
        return results;
    }

    bool PatchBytes(uintptr_t address, const void* data, size_t size) {
        DWORD oldProtect = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(address), size, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            return false;
        }
        std::memcpy(reinterpret_cast<void*>(address), data, size);
        VirtualProtect(reinterpret_cast<void*>(address), size, oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<const void*>(address), size);
        return true;
    }
}
