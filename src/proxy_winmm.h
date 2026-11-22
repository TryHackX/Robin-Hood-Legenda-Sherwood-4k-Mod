#pragma once
#include <windows.h>
#include <string>

bool InitWinMMProxy();
void ShutdownWinMMProxy();
HMODULE GetRealWinMMModule();
// Test mode (test_harness.cpp): every buffer handed to waveOutWrite (FMOD's WINMM output) is made silence first -
// the device plays, nothing is heard, nothing of the Windows sound settings changes. False: not possible.
bool SilenceWaveOut();
