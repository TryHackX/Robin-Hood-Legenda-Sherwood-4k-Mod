#pragma once
#include <windows.h>
#include <cstdint>

// Shows a finished RGB565 frame in the game window: Direct3D 11 flip-model swap chain with a
// scaling shader, GDI StretchDIBits as a fallback. All calls are serialised internally.
namespace DDM::Present {
    void SetWindow(HWND hwnd);
    void Shutdown();

    // Presents `bits` (w x h, RGB565, `pitch` bytes per row) scaled to the window, aspect preserved.
    void Frame(const uint8_t* bits, int pitch, int w, int h);

    const char* BackendName();
}
