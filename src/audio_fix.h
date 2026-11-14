#pragma once

namespace AudioFix {
    void Initialize();
    void Shutdown();
    // Called once per game tick (main thread): keeps the hearing range in step with the resolution.
    void OnGameTick();
}
