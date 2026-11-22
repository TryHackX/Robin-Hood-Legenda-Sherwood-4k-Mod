#pragma once
#include <windows.h>
#include <string>

bool InitWinMMProxy();
void ShutdownWinMMProxy();
HMODULE GetRealWinMMModule();
// Test mode (test_harness.cpp): waveOut devices are emulated (FMOD's WINMM output) - buffers taken and "played" in
// real time, nothing sent to Windows: no sound, no audio session, no Windows sound setting changed. False: not possible.
bool SilenceWaveOut();
