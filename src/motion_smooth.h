#pragma once

// Smooth movement of characters, animals, carts and projectiles between the 25 Hz game ticks.
//
// The game moves every object once per tick. The mod already draws several frames per tick
// (see timing.cpp); before each of them the objects that moved are placed between where they
// were at the previous tick and where they are now, the frame is drawn, and the real positions
// are put back. Game logic never sees the interpolated values. The price is that moving
// objects are shown up to one tick (40 ms) behind their logical position.
//
// Layout and functions used are documented in docs\OBJECT_RENDER.md.
namespace MotionSmooth {
    bool Initialize();

    // Tick's own frame, right after the game logic: remember previous and current positions.
    void CaptureTick();

    // Before a frame: alpha 0 = positions of the previous tick, 1 = current positions.
    void Apply(float alpha);

    // After the frame: restore exactly what Apply changed.
    void Restore();

    bool IsEnabled();
}
