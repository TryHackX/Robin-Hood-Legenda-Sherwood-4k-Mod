#pragma once

namespace WidescreenPatch {
    void Initialize();
    void Shutdown();

    // Main thread, once per game tick with the RHEngine pointer: "HD view" (see the .cpp).
    void OnGameTick(unsigned char* engine);

    // Full-height view: the bottom rows the hedge covers, in frame pixels (80 x interface scale; 0 off) -
    // the camera may bring the level's bottom edge above them.
    float CameraStrip();
}
