#pragma once

// The mod's version (CMake's project version, compiled in since 1.3.6).
#ifndef SHERWOODFIX_VERSION
#define SHERWOODFIX_VERSION "dev"
#endif

// The mod's own keys (user 2026-11-23: "the mod's keys bindable in the game's own controls menu and in F10, the version
// and the bound key next to the F10 hint, a reset to the defaults"): the settings panel, the HD background's original /
// HD and its next / previous variant, the in-between frames. Kept as DirectInput scan codes (SherwoodConfig::modKeys -
// the game's own key screen works with those); the mod polls them as virtual keys (SherwoodConfig::settingsKey ...).
// Changed in the F10 panel (settings_menu.cpp) and in the game's Options -> Keyboard (key_dialog.cpp).
namespace ModKeys {
    enum Action { kPanel, kHdToggle, kHdNext, kHdPrev, kBetween, kCount };

    int  Dik(int action);                    // its scan code, 0 = none
    int  Default(int action);
    void Set(int action, int dik);           // the config, its virtual key and SherwoodFix.ini; logged
    void ResetDefaults();                    // the mod's keys and the quick save / load keys (as in the game's settings)
    void SetQuickKey(bool load, int value);  // QuickSaveKey / QuickLoadKey: -1 as in the game's settings, 0 none, else a code
    const char*    IniName(int action);
    const wchar_t* NativeLabel(int action);  // a row of the game's key screen (its font has the Polish letters)
    const char*    PanelLabel(int action);   // a row of the F10 panel (5x7 letters: A-Z, digits, few signs)
    // a key's name for the panel and the notices: "F10", "INSERT", "KOD 0X2B", "BRAK" for 0 (upper case)
    void Name(int dik, char* out, int n);
    int  ActionOf(int dik, int except = -1); // the mod action with this key (not `except`), -1 = none
}
