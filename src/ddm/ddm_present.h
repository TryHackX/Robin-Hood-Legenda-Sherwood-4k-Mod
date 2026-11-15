#pragma once
#include <windows.h>
#include <cstdint>
#include <vector>

// Shows a finished RGB565 frame in the game window: Direct3D 11 flip-model swap chain with a
// scaling shader, GDI StretchDIBits as a fallback. All calls are serialised internally.
namespace DDM::Present {
    void SetWindow(HWND hwnd);
    void Shutdown();

    // Presents `bits` (w x h, RGB565, `pitch` bytes per row) scaled to the window, aspect preserved.
    void Frame(const uint8_t* bits, int pitch, int w, int h);

    // Smooth zoom (view_zoom.h), main thread: the world as drawn before the windows, straight into
    // the texture the next frame magnifies (one copy); false = no Direct3D 11 / failed.
    bool UploadWorld(const uint8_t* bits, int pitch, int w, int h);
    // ... and back from it (a dialog's backdrop shows the world the same way), RGB565.
    bool ReadWorld(std::vector<uint16_t>& out, int& w, int& h);

    const char* BackendName();
}
