#include "key_dialog.h"
#include "config.h"
#include "logger.h"
#include "mod_keys.h"
#include "widescreen_patch.h"

#include <windows.h>
#include <MinHook.h>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <intrin.h>

// The game's key screen (Options -> Keyboard), static RE 2026-11-23:
//   dialog, vtable 0x006798E8 (ctor ends at 0x0053FC9A, OnCreate 0x0053FDD0, dtor 0x0053FCC0); buttons +0x140 OK
//   (0x005405E0), +0x240 "Default 1" (0x00540540, keyset1.cfg), +0x340 "Default 2" (0x00540590), +0x440 "Choice"
//   (0x00540650: the profile's own set back), +0x540 Cancel; the key list +0x640; its working copy of the keys
//   (RHKeyConfig) +0x868; the profile +0x870 (its keys +0x34, the chosen set +0x3C).
//   RHKeyConfig: +0 u16* (29 scan codes in 0x3A bytes), +4 u16. Count 0x0051EBB0 (always 28), FindKey 0x0051EBC0
//   (the action with a key, 0xFFFF none), KeyAt 0x0051EC20 (&keys[i]), dtor 0x0051EBA0.
//   The key list: SetConfig 0x005CF1C0 (+0x20C = the working copy, then Refresh), Refresh 0x005CED90 (Count rows:
//   text 0x97 + i "label :" | 0x005CD010 the key's name), the row waiting for a key +0x214 (0xFFFF none); a key
//   pressed then: 0x005CE660 says which keys only clear the row (Esc, F10, F11, Home, Scroll Lock, Num Enter, Print
//   Screen, the Windows keys, Apps), else FindKey clears the action that had it and KeyAt(row) takes it.
//   Text 0x005E23D0 cdecl(std::wstring* out, id) - VC6 strings: +0 allocator byte, +4 wchar_t* (refcount byte
//   before it), +8 length, +0xC capacity; _Grow 0x004239E0 thiscall(n, trim).
// With the working copy of the open key screen only, these answer for five more rows: Count 28 + 5, KeyAt 28.. = the
// mod's keys being edited, FindKey looks at them too, the text of rows 28.. is the mod's label; OK writes them.
namespace KeyDialog {
namespace {
    constexpr int kGameKeys = 28;
    constexpr uint32_t kTextBase = 0x97;
    constexpr uint32_t kDialogVtable = 0x006798E8;

    uint8_t* g_cfg = nullptr;       // the open key screen's working copy (dialog + 0x868), main thread
    uint8_t* g_list = nullptr;      // its key list (dialog + 0x640)
    uint16_t g_pending[ModKeys::kCount] = {};
    uint16_t g_quickAtOpen[2] = {}; // the game's quick save / load keys when the screen opened
    bool     g_inRefresh = false;

    using PFN_Count     = uint16_t(__fastcall*)(void* cfg, void* edx);
    using PFN_FindKey   = uint16_t(__fastcall*)(void* cfg, void* edx, uint32_t dik);
    using PFN_KeyAt     = uint16_t*(__fastcall*)(void* cfg, void* edx, uint32_t index);
    using PFN_CfgDtor   = void(__fastcall*)(uint8_t* cfg, void* edx);
    using PFN_SetConfig = void(__fastcall*)(uint8_t* list, void* edx, uint8_t* cfg);
    using PFN_Refresh   = void(__fastcall*)(uint8_t* list, void* edx);
    using PFN_Forbidden = bool(__fastcall*)(uint8_t* list, void* edx, uint32_t dik);
    using PFN_GetText   = void*(__cdecl*)(void* out, uint32_t id);
    using PFN_Button    = uint32_t(__fastcall*)(uint8_t* dlg, void* edx, uint32_t a, uint32_t b, uint32_t c);
    using PFN_Grow      = bool(__fastcall*)(void* str, void* edx, uint32_t n, uint32_t trim);

    PFN_Count     o_Count = nullptr;
    PFN_FindKey   o_FindKey = nullptr;
    PFN_KeyAt     o_KeyAt = nullptr;
    PFN_CfgDtor   o_CfgDtor = nullptr;
    PFN_SetConfig o_SetConfig = nullptr;
    PFN_Refresh   o_Refresh = nullptr;
    PFN_Forbidden o_Forbidden = nullptr;
    PFN_GetText   o_GetText = nullptr;
    PFN_Button    o_Ok = nullptr;
    PFN_Button    o_Default1 = nullptr;
    PFN_Button    o_Default2 = nullptr;
    PFN_Button    o_Choice = nullptr;

    bool Readable(const void* p, size_t n) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!p || !VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
            return false;
        }
        return reinterpret_cast<uintptr_t>(p) + n <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    }

    bool IsOurs(const void* cfg) {
        return g_cfg && cfg == g_cfg;
    }

    void PendingFromConfig() {
        for (int a = 0; a < ModKeys::kCount; ++a) g_pending[a] = static_cast<uint16_t>(ModKeys::Dik(a));
    }

    void PendingDefaults() {
        for (int a = 0; a < ModKeys::kCount; ++a) g_pending[a] = static_cast<uint16_t>(ModKeys::Default(a));
    }

    // ---- RHKeyConfig ----------------------------------------------------------------------------
    uint16_t __fastcall Hook_Count(void* cfg, void* edx) {
        const uint16_t n = o_Count(cfg, edx);
        return IsOurs(cfg) ? static_cast<uint16_t>(n + ModKeys::kCount) : n;
    }

    uint16_t* __fastcall Hook_KeyAt(void* cfg, void* edx, uint32_t index) {
        const uint32_t i = index & 0xFFFF;
        if (IsOurs(cfg) && i >= kGameKeys && i < static_cast<uint32_t>(kGameKeys + ModKeys::kCount)) return &g_pending[i - kGameKeys];
        return o_KeyAt(cfg, edx, index);
    }

    // the game's rows first (as the game searches); a key one of the mod's rows has -> that row, so the list clears it
    // when another row takes the key
    uint16_t __fastcall Hook_FindKey(void* cfg, void* edx, uint32_t dik) {
        const uint16_t r = o_FindKey(cfg, edx, dik);
        const uint16_t k = static_cast<uint16_t>(dik & 0xFFFF);
        if (r != 0xFFFF || !IsOurs(cfg) || k == 0) return r;
        for (int a = 0; a < ModKeys::kCount; ++a) {
            if (g_pending[a] == k) return static_cast<uint16_t>(kGameKeys + a);
        }
        return r;
    }

    void __fastcall Hook_CfgDtor(uint8_t* cfg, void* edx) {
        if (cfg && cfg == g_cfg) {
            g_cfg = nullptr;
            g_list = nullptr;
        }
        o_CfgDtor(cfg, edx);
    }

    // ---- the key list ---------------------------------------------------------------------------
    void __fastcall Hook_SetConfig(uint8_t* list, void* edx, uint8_t* cfg) {
        // only the key screen gives its list a working copy: the dialog lies 0x868 before it
        const uint8_t* dlg = cfg ? cfg - 0x868 : nullptr;
        if (dlg && Readable(dlg, 0x880) && *reinterpret_cast<const uint32_t*>(dlg) == kDialogVtable) {
            g_cfg = cfg;
            g_list = list;
            PendingFromConfig();
            g_quickAtOpen[0] = *o_KeyAt(cfg, nullptr, 26);
            g_quickAtOpen[1] = *o_KeyAt(cfg, nullptr, 27);
            Logger::Info("KeyDialog: the game's key screen is open - the mod's %d keys are listed below its own.", ModKeys::kCount);
        }
        o_SetConfig(list, edx, cfg);
    }

    void __fastcall Hook_Refresh(uint8_t* list, void* edx) {
        const bool ours = g_cfg && list == g_list;
        if (ours) g_inRefresh = true;
        o_Refresh(list, edx);
        g_inRefresh = false;
    }

    // the mod's rows also take F10, F11, Home, Scroll Lock and Num Enter (the game keeps those off its own actions:
    // pressed, they clear the row); Esc, the Windows keys, Apps and Print Screen still clear it
    bool __fastcall Hook_Forbidden(uint8_t* list, void* edx, uint32_t dik) {
        const bool forbidden = o_Forbidden(list, edx, dik);
        if (!forbidden || !g_cfg || list != g_list) return forbidden;
        const uint16_t row = *reinterpret_cast<const uint16_t*>(list + 0x214);
        if (row < kGameKeys || row >= kGameKeys + ModKeys::kCount) return forbidden;
        const uint16_t k = static_cast<uint16_t>(dik & 0xFFFF);
        return !(k == 0x44 || k == 0x57 || k == 0xC7 || k == 0x46 || k == 0x9C);
    }

    // the text routine's hook of its own, only when WidescreenPatch has none there (it calls ApplyRowLabel)
    void* __cdecl Hook_GetText(void* out, uint32_t id) {
        void* res = o_GetText(out, id);
        if (res) KeyDialog::ApplyRowLabel(res, id, _ReturnAddress());
        return res;
    }

    // ---- the buttons ----------------------------------------------------------------------------
    uint32_t __fastcall Hook_Ok(uint8_t* dlg, void* edx, uint32_t a, uint32_t b, uint32_t c) {
        if (dlg && IsOurs(dlg + 0x868)) {
            for (int act = 0; act < ModKeys::kCount; ++act) {
                int k = g_pending[act];
                if (act == ModKeys::kPanel && k == 0) {
                    Logger::Info("KeyDialog: the settings panel keeps its key (it cannot be left without one).");
                    continue;
                }
                if (k != ModKeys::Dik(act)) ModKeys::Set(act, k);
            }
            // a quick save / load key chosen here is the one the mod uses from now on (its own QuickSaveKey /
            // QuickLoadKey, set in the F10 panel, gives way to the newer choice)
            const auto& cfg = SherwoodConfig::Get();
            for (int load = 0; load < 2; ++load) {
                const uint16_t now = *o_KeyAt(dlg + 0x868, nullptr, load ? 27 : 26);
                const int override = load ? cfg.quickLoadKey : cfg.quickSaveKey;
                if (now != g_quickAtOpen[load] && override >= 0) ModKeys::SetQuickKey(load != 0, -1);
            }
        }
        return o_Ok(dlg, edx, a, b, c);
    }

    uint32_t __fastcall Hook_Default1(uint8_t* dlg, void* edx, uint32_t a, uint32_t b, uint32_t c) {
        if (dlg && IsOurs(dlg + 0x868)) PendingDefaults();          // before: the button refreshes the list
        return o_Default1(dlg, edx, a, b, c);
    }

    uint32_t __fastcall Hook_Default2(uint8_t* dlg, void* edx, uint32_t a, uint32_t b, uint32_t c) {
        if (dlg && IsOurs(dlg + 0x868)) PendingDefaults();
        return o_Default2(dlg, edx, a, b, c);
    }

    uint32_t __fastcall Hook_Choice(uint8_t* dlg, void* edx, uint32_t a, uint32_t b, uint32_t c) {
        if (dlg && IsOurs(dlg + 0x868)) PendingFromConfig();        // the player's own keys back
        return o_Choice(dlg, edx, a, b, c);
    }

    bool Hook(uintptr_t addr, const uint8_t* prolog, size_t n, void* detour, void** original) {
        return std::memcmp(reinterpret_cast<const void*>(addr), prolog, n) == 0 &&
               MH_CreateHook(reinterpret_cast<LPVOID>(addr), detour, original) == MH_OK;
    }
}

bool Open() {
    return g_cfg != nullptr;
}

bool ApplyRowLabel(void* str, uint32_t id, const void* caller) {
    if (!g_inRefresh || !str || id < kTextBase || reinterpret_cast<uintptr_t>(caller) != 0x005CEE5D) return false;
    const uint32_t row = id - kTextBase;
    if (row < kGameKeys || row >= static_cast<uint32_t>(kGameKeys + ModKeys::kCount)) return false;
    // over the text the game made (as WidescreenPatch writes its resolution labels): _Grow, the letters, the length
    const wchar_t* text = ModKeys::NativeLabel(static_cast<int>(row - kGameKeys));
    const uint32_t n = static_cast<uint32_t>(std::wcslen(text));
    if (!n || !reinterpret_cast<PFN_Grow>(0x004239E0)(str, nullptr, n, 1)) return false;
    auto* p = *reinterpret_cast<wchar_t**>(static_cast<uint8_t*>(str) + 4);
    if (!p) return false;
    std::memcpy(p, text, n * sizeof(wchar_t));
    p[n] = 0;
    *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(str) + 8) = n;
    return true;
}

void Install() {
    static const uint8_t kCount[]     = { 0x66, 0xB8, 0x1C, 0x00, 0xC3 };
    static const uint8_t kFindKey[]   = { 0x8B, 0x09, 0x66, 0x8B, 0x54, 0x24, 0x04, 0x56, 0xBE, 0xFF, 0xFF, 0x00, 0x00 };
    static const uint8_t kKeyAt[]     = { 0x8B, 0x44, 0x24, 0x04, 0x8B, 0x09, 0x25, 0xFF, 0xFF, 0x00, 0x00, 0x8D, 0x04, 0x41 };
    static const uint8_t kCfgDtor[]   = { 0x8B, 0x01, 0x50, 0xE8 };
    static const uint8_t kSetConfig[] = { 0x8B, 0x44, 0x24, 0x04, 0x8B, 0x91, 0x0C, 0x02, 0x00, 0x00, 0x3B, 0xC2 };
    static const uint8_t kRefresh[]   = { 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x6A, 0xFF, 0x68, 0x00, 0x0E, 0x67, 0x00 };
    static const uint8_t kForbidden[] = { 0x66, 0x8B, 0x44, 0x24, 0x04, 0x66, 0x3D, 0x46, 0x00 };
    static const uint8_t kGetText[]   = { 0x6A, 0xFF, 0x68, 0x78, 0x25, 0x67, 0x00, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00 };
    static const uint8_t kButton[]    = { 0x56, 0x8B, 0xF1, 0x8A, 0x86, 0x50, 0x08, 0x00, 0x00, 0x84, 0xC0 };
    static const uint8_t kChoice[]    = { 0x56, 0x8B, 0xF1, 0x8B, 0x86, 0x70, 0x08, 0x00, 0x00, 0x8D, 0x8E, 0x68, 0x08, 0x00, 0x00 };
    struct Entry { uintptr_t addr; const uint8_t* prolog; size_t n; void* detour; void** original; };
    const Entry entries[] = {
        { 0x0051EBB0, kCount, sizeof(kCount), reinterpret_cast<void*>(&Hook_Count), reinterpret_cast<void**>(&o_Count) },
        { 0x0051EBC0, kFindKey, sizeof(kFindKey), reinterpret_cast<void*>(&Hook_FindKey), reinterpret_cast<void**>(&o_FindKey) },
        { 0x0051EC20, kKeyAt, sizeof(kKeyAt), reinterpret_cast<void*>(&Hook_KeyAt), reinterpret_cast<void**>(&o_KeyAt) },
        { 0x0051EBA0, kCfgDtor, sizeof(kCfgDtor), reinterpret_cast<void*>(&Hook_CfgDtor), reinterpret_cast<void**>(&o_CfgDtor) },
        { 0x005CF1C0, kSetConfig, sizeof(kSetConfig), reinterpret_cast<void*>(&Hook_SetConfig), reinterpret_cast<void**>(&o_SetConfig) },
        { 0x005CED90, kRefresh, sizeof(kRefresh), reinterpret_cast<void*>(&Hook_Refresh), reinterpret_cast<void**>(&o_Refresh) },
        { 0x005CE660, kForbidden, sizeof(kForbidden), reinterpret_cast<void*>(&Hook_Forbidden), reinterpret_cast<void**>(&o_Forbidden) },
        { 0x005405E0, kButton, sizeof(kButton), reinterpret_cast<void*>(&Hook_Ok), reinterpret_cast<void**>(&o_Ok) },
        { 0x00540540, kButton, sizeof(kButton), reinterpret_cast<void*>(&Hook_Default1), reinterpret_cast<void**>(&o_Default1) },
        { 0x00540590, kButton, sizeof(kButton), reinterpret_cast<void*>(&Hook_Default2), reinterpret_cast<void**>(&o_Default2) },
        { 0x00540650, kChoice, sizeof(kChoice), reinterpret_cast<void*>(&Hook_Choice), reinterpret_cast<void**>(&o_Choice) },
    };
    // all or nothing: half of these would let the list write past the game's keys
    bool ok = true;
    for (const Entry& e : entries) ok = ok && Hook(e.addr, e.prolog, e.n, e.detour, e.original);
    if (ok) {
        for (const Entry& e : entries) ok = ok && MH_EnableHook(reinterpret_cast<LPVOID>(e.addr)) == MH_OK;
    }
    if (!ok) {
        for (const Entry& e : entries) {
            if (!*e.original) continue;
            MH_DisableHook(reinterpret_cast<LPVOID>(e.addr));
            MH_RemoveHook(reinterpret_cast<LPVOID>(e.addr));
            *e.original = nullptr;
        }
        Logger::Warn("KeyDialog: the game's key screen does not match this build - the mod's keys only in the F10 panel.");
        return;
    }
    // the rows' labels: WidescreenPatch's hook of the game's texts writes them (its resolution labels live there);
    // without that hook, one of this module's own
    bool labels = WidescreenPatch::TextHookInstalled();
    if (!labels && Hook(0x005E23D0, kGetText, sizeof(kGetText), reinterpret_cast<void*>(&Hook_GetText),
                        reinterpret_cast<void**>(&o_GetText))) {
        labels = MH_EnableHook(reinterpret_cast<LPVOID>(0x005E23D0)) == MH_OK;
    }
    Logger::Info("KeyDialog: the mod's keys in the game's key screen (Options -> Keyboard): on%s.",
                 labels ? "" : " (labels NOT available)");
}

} // namespace KeyDialog
