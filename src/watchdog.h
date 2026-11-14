#pragma once
#include <windows.h>

// Keeps the game from hanging silently:
//  * hang reports - when the game window is in front and the main thread has neither pumped a
//    message nor presented a frame for a few seconds, its call stack goes to the log;
//  * clean exit   - ExitProcess ends the process at once (the DLL detach phase of a 2002 game
//    plus its drivers is where exits get stuck), and if the process is still alive a few
//    seconds after the game window was destroyed it is ended as well, after logging where the
//    main thread was. No more windowless Game.exe left behind.
namespace Watchdog {
    void Init();                         // DllMain: installs the ExitProcess hook, starts the thread
    void Shutdown();

    // EXE entry point (outside the loader lock): ends windowless leftover Game.exe processes
    // of this installation, which keep the sprite bank locked. Returns false if another copy
    // is in the way and the user has been told - the caller should exit.
    bool EndLeftoverInstances();

    void Beat();                         // main thread: "I am alive" (message pumped / frame shown)
    void OnMainWindowCreated(HWND hwnd);
    void OnMainWindowDestroyed();
}
