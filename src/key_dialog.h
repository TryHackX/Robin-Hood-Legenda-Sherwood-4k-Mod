#pragma once

// The mod's keys in the game's own key screen (Options -> Keyboard, 1.3.6): below the game's 28 actions five rows of
// the mod (ModKeys), bound exactly like the game's - click a row, press a key; "Default 1 / 2" also put the mod's
// defaults back, "OK" keeps the changes (SherwoodFix.ini), "Cancel" drops them. See key_dialog.cpp.
#include <cstdint>

namespace KeyDialog {
    void Install();      // DllMain, after MinHook and WidescreenPatch (whose hook of the game's texts calls the one below)
    bool Open();         // the key screen is up (main thread): the mod's panel key does not open the panel there
    // the game's text routine (0x005E23D0) made text `id` in `str` (a VC6 std::wstring) for the code that returns to
    // `caller`: when that is the key list's row label (0x005CEE5D - the same ids also name keys there: the arrows,
    // left Shift), the mod's rows get their labels written over it - true when it did
    bool ApplyRowLabel(void* str, uint32_t id, const void* caller);
}
