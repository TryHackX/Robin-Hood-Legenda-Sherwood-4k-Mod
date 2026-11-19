#pragma once
#include <cstddef>
#include <cstdint>

// Research probe for a co-op mode (2026-11-19): can the simulation of Game.exe be reproduced from
// the players' commands alone (lockstep), and where do the commands enter it?
//
// TEST MODE ONLY. Install() returns at once unless SHERWOODFIX_TEST=1, so a normally started game
// gets none of these hooks; the two hook points below return at once without a running session.
// Findings and addresses: docs\RE_NOTES.md, section "Co-op".
//
// Harness command "coop ..." (tools\hctl.ps1 -Cmd "coop stat"):
//   coop stat                    tick, session, rand() calls per context, clock reads in the logic
//   coop rec NAME                record: from the next engine tick on (arm it in the main menu = the
//                                mission's first tick) every input event that reaches the engine is
//                                logged with its tick -> tools\out\harness\coop_NAME_events.txt, plus
//                                a state hash per tick -> coop_NAME_rec_hash.txt (+ _rec_dump.txt)
//   coop play NAME [TAG]         replay that log from the next engine tick on (live input to the engine
//                                is dropped) -> coop_NAME_TAG_hash.txt (TAG default "play")
//   coop stop                    end the session (files closed)
//   coop trace on NAME | off     call trace of the command path -> coop_trace_NAME.txt
//   coop isolate MASK            rand() of these contexts gets its own generator (bit per context:
//                                1 other, 2 input, 4 logic, 8 render, 16 extra frame, 32 sound update;
//                                64 = every call made by the sound module's code, 0x005A6000..0x005B5800,
//                                whatever the context) - 0 = off
//   coop randdump NAME           rand() callers per context -> coop_rand_NAME.txt
//   coop seed N                  the seed set at the session start (default 0x5EED1234)
//   coop pinsrand on|off         srand() inside a session gets a seed derived from the session (default on)
//   coop dump                    full element dump at the next tick (into the session's dump file)
//   coop dumpevery N             element dump every N ticks (default 50; a replay also dumps every tick
//                                whose state differs from the recording's)
//   coop camera X Y              replay with the camera held at X,Y (one camera argument: back to recorded)
//   coop listeners | coop ai     the translator's listeners / the AI objects and their vtables
// Every session also writes coop_NAME_TAG_rand.txt (each main-thread rand() call: tick, context, caller,
// value; upper-case context = isolated) and coop_NAME_TAG_orders.txt (each RHOrder made: tick, callers).
namespace CoopProbe {
    void Install();
    void Command(const char* args, char* out, size_t outSize);

    // Hook points for game functions another module already hooks (inert without a session):
    // ScreenToWorld 0x004DEE70 (view_zoom.cpp) - records the world position of a logged event, and
    // gives a replayed event the logged one.
    void OnScreenToWorld(float* out);
    // Sword swing recognition 0x00547600 (ui_scale.cpp) - records the result, or replays it.
    uint32_t OnSwingRecognised(uint32_t result);
    // End of every RHGame::Run loop pass (timing.cpp, PaceGameLoopTick): notes why no engine tick ran.
    void OnLoopEnd(void* pGame);
}
