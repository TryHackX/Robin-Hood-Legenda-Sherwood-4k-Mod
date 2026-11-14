#pragma once
#include <windows.h>
#include <string>

bool InitWinMMProxy();
void ShutdownWinMMProxy();
HMODULE GetRealWinMMModule();
