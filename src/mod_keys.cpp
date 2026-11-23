#include "mod_keys.h"
#include "config.h"
#include "logger.h"

#include <windows.h>
#include <cctype>
#include <cstdio>

// See mod_keys.h.
namespace ModKeys {
namespace {
    constexpr int kDefaults[kCount] = { 0x44, 0x42, 0x41, 0x40, 0x43 };   // F10, F8, F7, F6, F9
    constexpr const char* kIni[kCount] = { "SettingsKey", "HDKeyToggle", "HDKeyNext", "HDKeyPrev", "HDKeyBetween" };

    void SyncVirtualKeys(SherwoodConfig& cfg) {
        cfg.settingsKey  = SherwoodConfig::VkFromDik(cfg.modKeys[kPanel]);
        cfg.hdKeyToggle  = SherwoodConfig::VkFromDik(cfg.modKeys[kHdToggle]);
        cfg.hdKeyNext    = SherwoodConfig::VkFromDik(cfg.modKeys[kHdNext]);
        cfg.hdKeyPrev    = SherwoodConfig::VkFromDik(cfg.modKeys[kHdPrev]);
        cfg.hdKeyBetween = SherwoodConfig::VkFromDik(cfg.modKeys[kBetween]);
    }

    void IniText(int dik, char* out, int n) {
        const char* name = dik > 0 ? SherwoodConfig::KeyName(dik) : nullptr;
        if (dik <= 0) std::snprintf(out, n, "none");
        else if (name) std::snprintf(out, n, "%s", name);
        else std::snprintf(out, n, "0x%02X", dik);
    }
}

int Dik(int action) {
    return (action >= 0 && action < kCount) ? SherwoodConfig::Get().modKeys[action] : 0;
}

int Default(int action) {
    return (action >= 0 && action < kCount) ? kDefaults[action] : 0;
}

void Set(int action, int dik) {
    if (action < 0 || action >= kCount) return;
    if (dik < 0 || dik > 0xFF) dik = 0;
    if (action == kPanel && dik == 0) return;       // the panel always has a key
    auto& cfg = SherwoodConfig::Get();
    cfg.modKeys[action] = dik;
    SyncVirtualKeys(cfg);
    char text[24];
    IniText(dik, text, sizeof(text));
    WritePrivateProfileStringA("Display", kIni[action], text, cfg.iniPath.c_str());
    Logger::Info("ModKeys: %s = %s.", kIni[action], text);
}

void SetQuickKey(bool load, int value) {
    auto& cfg = SherwoodConfig::Get();
    (load ? cfg.quickLoadKey : cfg.quickSaveKey) = value;
    char text[24];
    if (value < 0) text[0] = 0;
    else IniText(value, text, sizeof(text));
    WritePrivateProfileStringA("Input", load ? "QuickLoadKey" : "QuickSaveKey", text, cfg.iniPath.c_str());
    Logger::Info("ModKeys: %s = %s.", load ? "QuickLoadKey" : "QuickSaveKey", value < 0 ? "(as in the game's key settings)" : text);
}

void ResetDefaults() {
    for (int a = 0; a < kCount; ++a) Set(a, kDefaults[a]);
    SetQuickKey(false, -1);
    SetQuickKey(true, -1);
}

const char* IniName(int action) {
    return (action >= 0 && action < kCount) ? kIni[action] : "";
}

const wchar_t* NativeLabel(int action) {
    // The game's texts are UTF-16 ("Przybliż" = U+017C); the list adds " :". Written as UTF-8 bytes and turned
    // into UTF-16 here: the compiler put L"ń" into the DLL as U+0139 U+201E (the UTF-8 bytes read as code
    // page 1250 - two wrong letters for each Polish one on the key screen, 2026-11-23).
    static const char* const kUtf8[kCount] = {
        "Mod: panel ustawie\xC5\x84",
        "Mod: t\xC5\x82o HD / orygina\xC5\x82",
        "Mod: nast\xC4\x99pny wariant t\xC5\x82" "a HD",
        "Mod: poprzedni wariant t\xC5\x82" "a HD",
        "Mod: klatki po\xC5\x9Brednie animacji",
    };
    static wchar_t labels[kCount][64];
    static bool built = false;
    if (!built) {
        for (int a = 0; a < kCount; ++a) {
            if (!MultiByteToWideChar(CP_UTF8, 0, kUtf8[a], -1, labels[a], 64)) labels[a][0] = 0;
        }
        built = true;
    }
    return (action >= 0 && action < kCount) ? labels[action] : L"";
}

const char* PanelLabel(int action) {
    switch (action) {
        case kPanel:    return "KLAWISZ: PANEL USTAWIEN";
        case kHdToggle: return "KLAWISZ: TLO HD / ORYGINAL";
        case kHdNext:   return "KLAWISZ: NASTEPNY WARIANT TLA";
        case kHdPrev:   return "KLAWISZ: POPRZEDNI WARIANT TLA";
        case kBetween:  return "KLAWISZ: KLATKI POSREDNIE";
        default:        return "";
    }
}

void Name(int dik, char* out, int n) {
    if (!out || n <= 0) return;
    const char* name = dik > 0 ? SherwoodConfig::KeyName(dik) : nullptr;
    if (dik <= 0) std::snprintf(out, n, "BRAK");
    else if (name) std::snprintf(out, n, "%s", name);
    else std::snprintf(out, n, "KOD 0X%02X", dik);
    for (char* p = out; *p; ++p) *p = static_cast<char>(std::toupper(static_cast<unsigned char>(*p)));
}

int ActionOf(int dik, int except) {
    if (dik <= 0) return -1;
    for (int a = 0; a < kCount; ++a) {
        if (a != except && Dik(a) == dik) return a;
    }
    return -1;
}

} // namespace ModKeys
