#pragma once

namespace WidescreenPatch {
    void Initialize();
    void Shutdown();

    // Main thread, once per game tick with the RHEngine pointer: "HD view" (see the .cpp).
    void OnGameTick(unsigned char* engine);
}
