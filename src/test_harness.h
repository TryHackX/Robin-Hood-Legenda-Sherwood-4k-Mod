#pragma once
#include <windows.h>
#include <cstdint>
#include <string>

// Background test mode - the game runs invisibly and is driven by commands, so that changes can
// be checked without taking the screen, the mouse or the speakers away from whoever is using
// the PC. Switched on by the environment variable SHERWOODFIX_TEST=1 (tools\gamectl.ps1 -Start
// -Headless); a normally started game never enters it.
//
// What changes in this mode:
//   * the game window is created hidden and never activated (no taskbar button either);
//   * the game is told it is active although it is not in front (watchdog.cpp), and
//     deactivation messages are swallowed;
//   * FMOD plays through its WINMM output into a waveOut device this DLL emulates (proxy_winmm.h
//     SilenceWaveOut: nothing reaches Windows, no sound setting changes), and the intro videos get no sound system;
//   * mouse and keyboard records come from the command file instead of DirectInput (the devices
//     cannot be acquired without the foreground) and enter the game exactly where real records
//     do: SBInputThreadProc -> ProcessInputEvent (0x005F0C20);
//   * frames can be dumped to BMP files: the game's own frame or the scaled output.
// A real start of the game ends a running test instance (watchdog.cpp, EndLeftoverInstances).
//
// Commands: one per line in <game dir>\tools\out\harness\cmd.txt, "<seq> <command> [args]";
// the answer "<seq> ok|err ..." is appended to ack.txt. See tools\hctl.ps1.
//   move DX DY | moveto X Y | click [L|R] | down [L|R] | up [L|R] | wheel N | key DIK [HOLD_MS]
//   zoomin | zoomout | shot NAME | shotout NAME | mem ADDR LEN | stat | quit
namespace TestHarness {
    bool Enabled();
    void Init(); // DllMain, game process only

    struct InputRecord {
        uint32_t type; // 0 = mouse, 1 = keyboard (first argument of ProcessInputEvent)
        uint32_t ofs;  // mouse: 0 X, 4 Y, 8 wheel, 0x0C.. buttons; keyboard: DIK scan code
        uint32_t data; // movement in internal pixels, or 0x80 = pressed / 0 = released
    };
    // Input thread, inside the mouse branch: the next injected record that is due.
    bool NextInput(InputRecord& out);

    // Main thread, once per game tick: runs engine calls requested by commands (zoomin / zoomout).
    void OnGameTick();

    // Presenter (main thread): has a frame dump been requested? `scaled` = the image as it goes
    // to the screen (after the scaling shader) instead of the game's own frame.
    bool TakeShotRequest(std::string& path, bool& scaled);
    void ShotDone(bool ok, int width, int height);
}
