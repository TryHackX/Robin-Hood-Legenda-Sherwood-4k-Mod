#include "proxy_winmm.h"
#include <windows.h>
#include <mmsystem.h>
#include <cstring>

namespace {
    HMODULE g_hRealWinMM = nullptr;
    FARPROC g_pProcs[192] = { nullptr };
    const char* const g_szProcNames[] = {
        "CloseDriver",
        "DefDriverProc",
        "DriverCallback",
        "DrvGetModuleHandle",
        "GetDriverModuleHandle",
        "NotifyCallbackData",
        "OpenDriver",
        "PlaySound",
        "PlaySoundA",
        "PlaySoundW",
        "SendDriverMessage",
        "WOW32DriverCallback",
        "WOW32ResolveMultiMediaHandle",
        "WOWAppExit",
        "aux32Message",
        "auxGetDevCapsA",
        "auxGetDevCapsW",
        "auxGetNumDevs",
        "auxGetVolume",
        "auxOutMessage",
        "auxSetVolume",
        "joy32Message",
        "joyConfigChanged",
        "joyGetDevCapsA",
        "joyGetDevCapsW",
        "joyGetNumDevs",
        "joyGetPos",
        "joyGetPosEx",
        "joyGetThreshold",
        "joyReleaseCapture",
        "joySetCapture",
        "joySetThreshold",
        "mci32Message",
        "mciDriverNotify",
        "mciDriverYield",
        "mciExecute",
        "mciFreeCommandResource",
        "mciGetCreatorTask",
        "mciGetDeviceIDA",
        "mciGetDeviceIDFromElementIDA",
        "mciGetDeviceIDFromElementIDW",
        "mciGetDeviceIDW",
        "mciGetDriverData",
        "mciGetErrorStringA",
        "mciGetErrorStringW",
        "mciGetYieldProc",
        "mciLoadCommandResource",
        "mciSendCommandA",
        "mciSendCommandW",
        "mciSendStringA",
        "mciSendStringW",
        "mciSetDriverData",
        "mciSetYieldProc",
        "mid32Message",
        "midiConnect",
        "midiDisconnect",
        "midiInAddBuffer",
        "midiInClose",
        "midiInGetDevCapsA",
        "midiInGetDevCapsW",
        "midiInGetErrorTextA",
        "midiInGetErrorTextW",
        "midiInGetID",
        "midiInGetNumDevs",
        "midiInMessage",
        "midiInOpen",
        "midiInPrepareHeader",
        "midiInReset",
        "midiInStart",
        "midiInStop",
        "midiInUnprepareHeader",
        "midiOutCacheDrumPatches",
        "midiOutCachePatches",
        "midiOutClose",
        "midiOutGetDevCapsA",
        "midiOutGetDevCapsW",
        "midiOutGetErrorTextA",
        "midiOutGetErrorTextW",
        "midiOutGetID",
        "midiOutGetNumDevs",
        "midiOutGetVolume",
        "midiOutLongMsg",
        "midiOutMessage",
        "midiOutOpen",
        "midiOutPrepareHeader",
        "midiOutReset",
        "midiOutSetVolume",
        "midiOutShortMsg",
        "midiOutUnprepareHeader",
        "midiStreamClose",
        "midiStreamOpen",
        "midiStreamOut",
        "midiStreamPause",
        "midiStreamPosition",
        "midiStreamProperty",
        "midiStreamRestart",
        "midiStreamStop",
        "mixerClose",
        "mixerGetControlDetailsA",
        "mixerGetControlDetailsW",
        "mixerGetDevCapsA",
        "mixerGetDevCapsW",
        "mixerGetID",
        "mixerGetLineControlsA",
        "mixerGetLineControlsW",
        "mixerGetLineInfoA",
        "mixerGetLineInfoW",
        "mixerGetNumDevs",
        "mixerMessage",
        "mixerOpen",
        "mixerSetControlDetails",
        "mmDrvInstall",
        "mmGetCurrentTask",
        "mmTaskBlock",
        "mmTaskCreate",
        "mmTaskSignal",
        "mmTaskYield",
        "mmioAdvance",
        "mmioAscend",
        "mmioClose",
        "mmioCreateChunk",
        "mmioDescend",
        "mmioFlush",
        "mmioGetInfo",
        "mmioInstallIOProcA",
        "mmioInstallIOProcW",
        "mmioOpenA",
        "mmioOpenW",
        "mmioRead",
        "mmioRenameA",
        "mmioRenameW",
        "mmioSeek",
        "mmioSendMessage",
        "mmioSetBuffer",
        "mmioSetInfo",
        "mmioStringToFOURCCA",
        "mmioStringToFOURCCW",
        "mmioWrite",
        "mmsystemGetVersion",
        "mod32Message",
        "mxd32Message",
        "sndPlaySoundA",
        "sndPlaySoundW",
        "tid32Message",
        "timeBeginPeriod",
        "timeEndPeriod",
        "timeGetDevCaps",
        "timeGetSystemTime",
        "timeGetTime",
        "timeKillEvent",
        "timeSetEvent",
        "waveInAddBuffer",
        "waveInClose",
        "waveInGetDevCapsA",
        "waveInGetDevCapsW",
        "waveInGetErrorTextA",
        "waveInGetErrorTextW",
        "waveInGetID",
        "waveInGetNumDevs",
        "waveInGetPosition",
        "waveInMessage",
        "waveInOpen",
        "waveInPrepareHeader",
        "waveInReset",
        "waveInStart",
        "waveInStop",
        "waveInUnprepareHeader",
        "waveOutBreakLoop",
        "waveOutClose",
        "waveOutGetDevCapsA",
        "waveOutGetDevCapsW",
        "waveOutGetErrorTextA",
        "waveOutGetErrorTextW",
        "waveOutGetID",
        "waveOutGetNumDevs",
        "waveOutGetPitch",
        "waveOutGetPlaybackRate",
        "waveOutGetPosition",
        "waveOutGetVolume",
        "waveOutMessage",
        "waveOutOpen",
        "waveOutPause",
        "waveOutPrepareHeader",
        "waveOutReset",
        "waveOutRestart",
        "waveOutSetPitch",
        "waveOutSetPlaybackRate",
        "waveOutSetVolume",
        "waveOutUnprepareHeader",
        "waveOutWrite",
        "wid32Message",
        "wod32Message",
    };
}

bool InitWinMMProxy() {
    if (g_hRealWinMM) return true;
    char sysDir[MAX_PATH]{};
    GetSystemDirectoryA(sysDir, MAX_PATH);
    std::string path = std::string(sysDir) + "\\winmm.dll";
    g_hRealWinMM = LoadLibraryA(path.c_str());
    if (!g_hRealWinMM) return false;
    for (size_t i = 0; i < 192; ++i) {
        g_pProcs[i] = GetProcAddress(g_hRealWinMM, g_szProcNames[i]);
    }
    return true;
}

void ShutdownWinMMProxy() {
    if (g_hRealWinMM) {
        FreeLibrary(g_hRealWinMM);
        g_hRealWinMM = nullptr;
    }
}

HMODULE GetRealWinMMModule() {
    return g_hRealWinMM;
}

namespace {
    using PFN_waveOutWrite = UINT(WINAPI*)(HANDLE, WAVEHDR*, UINT);
    PFN_waveOutWrite g_realWaveOutWrite = nullptr;

    UINT WINAPI Silent_waveOutWrite(HANDLE hwo, WAVEHDR* hdr, UINT size) {
        if (hdr && size >= sizeof(WAVEHDR) && hdr->lpData && hdr->dwBufferLength) {
            std::memset(hdr->lpData, 0, hdr->dwBufferLength);   // FMOD mixes 16-bit samples: 0 = silence
        }
        return g_realWaveOutWrite(hwo, hdr, size);
    }
}

bool SilenceWaveOut() {
    if (g_realWaveOutWrite) return true;
    for (size_t i = 0; i < 192; ++i) {
        if (std::strcmp(g_szProcNames[i], "waveOutWrite") != 0) continue;
        if (!g_pProcs[i]) return false;
        g_realWaveOutWrite = reinterpret_cast<PFN_waveOutWrite>(g_pProcs[i]);
        g_pProcs[i] = reinterpret_cast<FARPROC>(&Silent_waveOutWrite);    // Proxy_waveOutWrite jumps through it
        return true;
    }
    return false;
}

extern "C" {
    __declspec(naked) void Proxy_CloseDriver() { __asm { jmp dword ptr [g_pProcs + 0] } }
    __declspec(naked) void Proxy_DefDriverProc() { __asm { jmp dword ptr [g_pProcs + 4] } }
    __declspec(naked) void Proxy_DriverCallback() { __asm { jmp dword ptr [g_pProcs + 8] } }
    __declspec(naked) void Proxy_DrvGetModuleHandle() { __asm { jmp dword ptr [g_pProcs + 12] } }
    __declspec(naked) void Proxy_GetDriverModuleHandle() { __asm { jmp dword ptr [g_pProcs + 16] } }
    __declspec(naked) void Proxy_NotifyCallbackData() { __asm { jmp dword ptr [g_pProcs + 20] } }
    __declspec(naked) void Proxy_OpenDriver() { __asm { jmp dword ptr [g_pProcs + 24] } }
    __declspec(naked) void Proxy_PlaySound() { __asm { jmp dword ptr [g_pProcs + 28] } }
    __declspec(naked) void Proxy_PlaySoundA() { __asm { jmp dword ptr [g_pProcs + 32] } }
    __declspec(naked) void Proxy_PlaySoundW() { __asm { jmp dword ptr [g_pProcs + 36] } }
    __declspec(naked) void Proxy_SendDriverMessage() { __asm { jmp dword ptr [g_pProcs + 40] } }
    __declspec(naked) void Proxy_WOW32DriverCallback() { __asm { jmp dword ptr [g_pProcs + 44] } }
    __declspec(naked) void Proxy_WOW32ResolveMultiMediaHandle() { __asm { jmp dword ptr [g_pProcs + 48] } }
    __declspec(naked) void Proxy_WOWAppExit() { __asm { jmp dword ptr [g_pProcs + 52] } }
    __declspec(naked) void Proxy_aux32Message() { __asm { jmp dword ptr [g_pProcs + 56] } }
    __declspec(naked) void Proxy_auxGetDevCapsA() { __asm { jmp dword ptr [g_pProcs + 60] } }
    __declspec(naked) void Proxy_auxGetDevCapsW() { __asm { jmp dword ptr [g_pProcs + 64] } }
    __declspec(naked) void Proxy_auxGetNumDevs() { __asm { jmp dword ptr [g_pProcs + 68] } }
    __declspec(naked) void Proxy_auxGetVolume() { __asm { jmp dword ptr [g_pProcs + 72] } }
    __declspec(naked) void Proxy_auxOutMessage() { __asm { jmp dword ptr [g_pProcs + 76] } }
    __declspec(naked) void Proxy_auxSetVolume() { __asm { jmp dword ptr [g_pProcs + 80] } }
    __declspec(naked) void Proxy_joy32Message() { __asm { jmp dword ptr [g_pProcs + 84] } }
    __declspec(naked) void Proxy_joyConfigChanged() { __asm { jmp dword ptr [g_pProcs + 88] } }
    __declspec(naked) void Proxy_joyGetDevCapsA() { __asm { jmp dword ptr [g_pProcs + 92] } }
    __declspec(naked) void Proxy_joyGetDevCapsW() { __asm { jmp dword ptr [g_pProcs + 96] } }
    __declspec(naked) void Proxy_joyGetNumDevs() { __asm { jmp dword ptr [g_pProcs + 100] } }
    __declspec(naked) void Proxy_joyGetPos() { __asm { jmp dword ptr [g_pProcs + 104] } }
    __declspec(naked) void Proxy_joyGetPosEx() { __asm { jmp dword ptr [g_pProcs + 108] } }
    __declspec(naked) void Proxy_joyGetThreshold() { __asm { jmp dword ptr [g_pProcs + 112] } }
    __declspec(naked) void Proxy_joyReleaseCapture() { __asm { jmp dword ptr [g_pProcs + 116] } }
    __declspec(naked) void Proxy_joySetCapture() { __asm { jmp dword ptr [g_pProcs + 120] } }
    __declspec(naked) void Proxy_joySetThreshold() { __asm { jmp dword ptr [g_pProcs + 124] } }
    __declspec(naked) void Proxy_mci32Message() { __asm { jmp dword ptr [g_pProcs + 128] } }
    __declspec(naked) void Proxy_mciDriverNotify() { __asm { jmp dword ptr [g_pProcs + 132] } }
    __declspec(naked) void Proxy_mciDriverYield() { __asm { jmp dword ptr [g_pProcs + 136] } }
    __declspec(naked) void Proxy_mciExecute() { __asm { jmp dword ptr [g_pProcs + 140] } }
    __declspec(naked) void Proxy_mciFreeCommandResource() { __asm { jmp dword ptr [g_pProcs + 144] } }
    __declspec(naked) void Proxy_mciGetCreatorTask() { __asm { jmp dword ptr [g_pProcs + 148] } }
    __declspec(naked) void Proxy_mciGetDeviceIDA() { __asm { jmp dword ptr [g_pProcs + 152] } }
    __declspec(naked) void Proxy_mciGetDeviceIDFromElementIDA() { __asm { jmp dword ptr [g_pProcs + 156] } }
    __declspec(naked) void Proxy_mciGetDeviceIDFromElementIDW() { __asm { jmp dword ptr [g_pProcs + 160] } }
    __declspec(naked) void Proxy_mciGetDeviceIDW() { __asm { jmp dword ptr [g_pProcs + 164] } }
    __declspec(naked) void Proxy_mciGetDriverData() { __asm { jmp dword ptr [g_pProcs + 168] } }
    __declspec(naked) void Proxy_mciGetErrorStringA() { __asm { jmp dword ptr [g_pProcs + 172] } }
    __declspec(naked) void Proxy_mciGetErrorStringW() { __asm { jmp dword ptr [g_pProcs + 176] } }
    __declspec(naked) void Proxy_mciGetYieldProc() { __asm { jmp dword ptr [g_pProcs + 180] } }
    __declspec(naked) void Proxy_mciLoadCommandResource() { __asm { jmp dword ptr [g_pProcs + 184] } }
    __declspec(naked) void Proxy_mciSendCommandA() { __asm { jmp dword ptr [g_pProcs + 188] } }
    __declspec(naked) void Proxy_mciSendCommandW() { __asm { jmp dword ptr [g_pProcs + 192] } }
    __declspec(naked) void Proxy_mciSendStringA() { __asm { jmp dword ptr [g_pProcs + 196] } }
    __declspec(naked) void Proxy_mciSendStringW() { __asm { jmp dword ptr [g_pProcs + 200] } }
    __declspec(naked) void Proxy_mciSetDriverData() { __asm { jmp dword ptr [g_pProcs + 204] } }
    __declspec(naked) void Proxy_mciSetYieldProc() { __asm { jmp dword ptr [g_pProcs + 208] } }
    __declspec(naked) void Proxy_mid32Message() { __asm { jmp dword ptr [g_pProcs + 212] } }
    __declspec(naked) void Proxy_midiConnect() { __asm { jmp dword ptr [g_pProcs + 216] } }
    __declspec(naked) void Proxy_midiDisconnect() { __asm { jmp dword ptr [g_pProcs + 220] } }
    __declspec(naked) void Proxy_midiInAddBuffer() { __asm { jmp dword ptr [g_pProcs + 224] } }
    __declspec(naked) void Proxy_midiInClose() { __asm { jmp dword ptr [g_pProcs + 228] } }
    __declspec(naked) void Proxy_midiInGetDevCapsA() { __asm { jmp dword ptr [g_pProcs + 232] } }
    __declspec(naked) void Proxy_midiInGetDevCapsW() { __asm { jmp dword ptr [g_pProcs + 236] } }
    __declspec(naked) void Proxy_midiInGetErrorTextA() { __asm { jmp dword ptr [g_pProcs + 240] } }
    __declspec(naked) void Proxy_midiInGetErrorTextW() { __asm { jmp dword ptr [g_pProcs + 244] } }
    __declspec(naked) void Proxy_midiInGetID() { __asm { jmp dword ptr [g_pProcs + 248] } }
    __declspec(naked) void Proxy_midiInGetNumDevs() { __asm { jmp dword ptr [g_pProcs + 252] } }
    __declspec(naked) void Proxy_midiInMessage() { __asm { jmp dword ptr [g_pProcs + 256] } }
    __declspec(naked) void Proxy_midiInOpen() { __asm { jmp dword ptr [g_pProcs + 260] } }
    __declspec(naked) void Proxy_midiInPrepareHeader() { __asm { jmp dword ptr [g_pProcs + 264] } }
    __declspec(naked) void Proxy_midiInReset() { __asm { jmp dword ptr [g_pProcs + 268] } }
    __declspec(naked) void Proxy_midiInStart() { __asm { jmp dword ptr [g_pProcs + 272] } }
    __declspec(naked) void Proxy_midiInStop() { __asm { jmp dword ptr [g_pProcs + 276] } }
    __declspec(naked) void Proxy_midiInUnprepareHeader() { __asm { jmp dword ptr [g_pProcs + 280] } }
    __declspec(naked) void Proxy_midiOutCacheDrumPatches() { __asm { jmp dword ptr [g_pProcs + 284] } }
    __declspec(naked) void Proxy_midiOutCachePatches() { __asm { jmp dword ptr [g_pProcs + 288] } }
    __declspec(naked) void Proxy_midiOutClose() { __asm { jmp dword ptr [g_pProcs + 292] } }
    __declspec(naked) void Proxy_midiOutGetDevCapsA() { __asm { jmp dword ptr [g_pProcs + 296] } }
    __declspec(naked) void Proxy_midiOutGetDevCapsW() { __asm { jmp dword ptr [g_pProcs + 300] } }
    __declspec(naked) void Proxy_midiOutGetErrorTextA() { __asm { jmp dword ptr [g_pProcs + 304] } }
    __declspec(naked) void Proxy_midiOutGetErrorTextW() { __asm { jmp dword ptr [g_pProcs + 308] } }
    __declspec(naked) void Proxy_midiOutGetID() { __asm { jmp dword ptr [g_pProcs + 312] } }
    __declspec(naked) void Proxy_midiOutGetNumDevs() { __asm { jmp dword ptr [g_pProcs + 316] } }
    __declspec(naked) void Proxy_midiOutGetVolume() { __asm { jmp dword ptr [g_pProcs + 320] } }
    __declspec(naked) void Proxy_midiOutLongMsg() { __asm { jmp dword ptr [g_pProcs + 324] } }
    __declspec(naked) void Proxy_midiOutMessage() { __asm { jmp dword ptr [g_pProcs + 328] } }
    __declspec(naked) void Proxy_midiOutOpen() { __asm { jmp dword ptr [g_pProcs + 332] } }
    __declspec(naked) void Proxy_midiOutPrepareHeader() { __asm { jmp dword ptr [g_pProcs + 336] } }
    __declspec(naked) void Proxy_midiOutReset() { __asm { jmp dword ptr [g_pProcs + 340] } }
    __declspec(naked) void Proxy_midiOutSetVolume() { __asm { jmp dword ptr [g_pProcs + 344] } }
    __declspec(naked) void Proxy_midiOutShortMsg() { __asm { jmp dword ptr [g_pProcs + 348] } }
    __declspec(naked) void Proxy_midiOutUnprepareHeader() { __asm { jmp dword ptr [g_pProcs + 352] } }
    __declspec(naked) void Proxy_midiStreamClose() { __asm { jmp dword ptr [g_pProcs + 356] } }
    __declspec(naked) void Proxy_midiStreamOpen() { __asm { jmp dword ptr [g_pProcs + 360] } }
    __declspec(naked) void Proxy_midiStreamOut() { __asm { jmp dword ptr [g_pProcs + 364] } }
    __declspec(naked) void Proxy_midiStreamPause() { __asm { jmp dword ptr [g_pProcs + 368] } }
    __declspec(naked) void Proxy_midiStreamPosition() { __asm { jmp dword ptr [g_pProcs + 372] } }
    __declspec(naked) void Proxy_midiStreamProperty() { __asm { jmp dword ptr [g_pProcs + 376] } }
    __declspec(naked) void Proxy_midiStreamRestart() { __asm { jmp dword ptr [g_pProcs + 380] } }
    __declspec(naked) void Proxy_midiStreamStop() { __asm { jmp dword ptr [g_pProcs + 384] } }
    __declspec(naked) void Proxy_mixerClose() { __asm { jmp dword ptr [g_pProcs + 388] } }
    __declspec(naked) void Proxy_mixerGetControlDetailsA() { __asm { jmp dword ptr [g_pProcs + 392] } }
    __declspec(naked) void Proxy_mixerGetControlDetailsW() { __asm { jmp dword ptr [g_pProcs + 396] } }
    __declspec(naked) void Proxy_mixerGetDevCapsA() { __asm { jmp dword ptr [g_pProcs + 400] } }
    __declspec(naked) void Proxy_mixerGetDevCapsW() { __asm { jmp dword ptr [g_pProcs + 404] } }
    __declspec(naked) void Proxy_mixerGetID() { __asm { jmp dword ptr [g_pProcs + 408] } }
    __declspec(naked) void Proxy_mixerGetLineControlsA() { __asm { jmp dword ptr [g_pProcs + 412] } }
    __declspec(naked) void Proxy_mixerGetLineControlsW() { __asm { jmp dword ptr [g_pProcs + 416] } }
    __declspec(naked) void Proxy_mixerGetLineInfoA() { __asm { jmp dword ptr [g_pProcs + 420] } }
    __declspec(naked) void Proxy_mixerGetLineInfoW() { __asm { jmp dword ptr [g_pProcs + 424] } }
    __declspec(naked) void Proxy_mixerGetNumDevs() { __asm { jmp dword ptr [g_pProcs + 428] } }
    __declspec(naked) void Proxy_mixerMessage() { __asm { jmp dword ptr [g_pProcs + 432] } }
    __declspec(naked) void Proxy_mixerOpen() { __asm { jmp dword ptr [g_pProcs + 436] } }
    __declspec(naked) void Proxy_mixerSetControlDetails() { __asm { jmp dword ptr [g_pProcs + 440] } }
    __declspec(naked) void Proxy_mmDrvInstall() { __asm { jmp dword ptr [g_pProcs + 444] } }
    __declspec(naked) void Proxy_mmGetCurrentTask() { __asm { jmp dword ptr [g_pProcs + 448] } }
    __declspec(naked) void Proxy_mmTaskBlock() { __asm { jmp dword ptr [g_pProcs + 452] } }
    __declspec(naked) void Proxy_mmTaskCreate() { __asm { jmp dword ptr [g_pProcs + 456] } }
    __declspec(naked) void Proxy_mmTaskSignal() { __asm { jmp dword ptr [g_pProcs + 460] } }
    __declspec(naked) void Proxy_mmTaskYield() { __asm { jmp dword ptr [g_pProcs + 464] } }
    __declspec(naked) void Proxy_mmioAdvance() { __asm { jmp dword ptr [g_pProcs + 468] } }
    __declspec(naked) void Proxy_mmioAscend() { __asm { jmp dword ptr [g_pProcs + 472] } }
    __declspec(naked) void Proxy_mmioClose() { __asm { jmp dword ptr [g_pProcs + 476] } }
    __declspec(naked) void Proxy_mmioCreateChunk() { __asm { jmp dword ptr [g_pProcs + 480] } }
    __declspec(naked) void Proxy_mmioDescend() { __asm { jmp dword ptr [g_pProcs + 484] } }
    __declspec(naked) void Proxy_mmioFlush() { __asm { jmp dword ptr [g_pProcs + 488] } }
    __declspec(naked) void Proxy_mmioGetInfo() { __asm { jmp dword ptr [g_pProcs + 492] } }
    __declspec(naked) void Proxy_mmioInstallIOProcA() { __asm { jmp dword ptr [g_pProcs + 496] } }
    __declspec(naked) void Proxy_mmioInstallIOProcW() { __asm { jmp dword ptr [g_pProcs + 500] } }
    __declspec(naked) void Proxy_mmioOpenA() { __asm { jmp dword ptr [g_pProcs + 504] } }
    __declspec(naked) void Proxy_mmioOpenW() { __asm { jmp dword ptr [g_pProcs + 508] } }
    __declspec(naked) void Proxy_mmioRead() { __asm { jmp dword ptr [g_pProcs + 512] } }
    __declspec(naked) void Proxy_mmioRenameA() { __asm { jmp dword ptr [g_pProcs + 516] } }
    __declspec(naked) void Proxy_mmioRenameW() { __asm { jmp dword ptr [g_pProcs + 520] } }
    __declspec(naked) void Proxy_mmioSeek() { __asm { jmp dword ptr [g_pProcs + 524] } }
    __declspec(naked) void Proxy_mmioSendMessage() { __asm { jmp dword ptr [g_pProcs + 528] } }
    __declspec(naked) void Proxy_mmioSetBuffer() { __asm { jmp dword ptr [g_pProcs + 532] } }
    __declspec(naked) void Proxy_mmioSetInfo() { __asm { jmp dword ptr [g_pProcs + 536] } }
    __declspec(naked) void Proxy_mmioStringToFOURCCA() { __asm { jmp dword ptr [g_pProcs + 540] } }
    __declspec(naked) void Proxy_mmioStringToFOURCCW() { __asm { jmp dword ptr [g_pProcs + 544] } }
    __declspec(naked) void Proxy_mmioWrite() { __asm { jmp dword ptr [g_pProcs + 548] } }
    __declspec(naked) void Proxy_mmsystemGetVersion() { __asm { jmp dword ptr [g_pProcs + 552] } }
    __declspec(naked) void Proxy_mod32Message() { __asm { jmp dword ptr [g_pProcs + 556] } }
    __declspec(naked) void Proxy_mxd32Message() { __asm { jmp dword ptr [g_pProcs + 560] } }
    __declspec(naked) void Proxy_sndPlaySoundA() { __asm { jmp dword ptr [g_pProcs + 564] } }
    __declspec(naked) void Proxy_sndPlaySoundW() { __asm { jmp dword ptr [g_pProcs + 568] } }
    __declspec(naked) void Proxy_tid32Message() { __asm { jmp dword ptr [g_pProcs + 572] } }
    __declspec(naked) void Proxy_timeBeginPeriod() { __asm { jmp dword ptr [g_pProcs + 576] } }
    __declspec(naked) void Proxy_timeEndPeriod() { __asm { jmp dword ptr [g_pProcs + 580] } }
    __declspec(naked) void Proxy_timeGetDevCaps() { __asm { jmp dword ptr [g_pProcs + 584] } }
    __declspec(naked) void Proxy_timeGetSystemTime() { __asm { jmp dword ptr [g_pProcs + 588] } }
    __declspec(naked) void Proxy_timeGetTime() { __asm { jmp dword ptr [g_pProcs + 592] } }
    __declspec(naked) void Proxy_timeKillEvent() { __asm { jmp dword ptr [g_pProcs + 596] } }
    __declspec(naked) void Proxy_timeSetEvent() { __asm { jmp dword ptr [g_pProcs + 600] } }
    __declspec(naked) void Proxy_waveInAddBuffer() { __asm { jmp dword ptr [g_pProcs + 604] } }
    __declspec(naked) void Proxy_waveInClose() { __asm { jmp dword ptr [g_pProcs + 608] } }
    __declspec(naked) void Proxy_waveInGetDevCapsA() { __asm { jmp dword ptr [g_pProcs + 612] } }
    __declspec(naked) void Proxy_waveInGetDevCapsW() { __asm { jmp dword ptr [g_pProcs + 616] } }
    __declspec(naked) void Proxy_waveInGetErrorTextA() { __asm { jmp dword ptr [g_pProcs + 620] } }
    __declspec(naked) void Proxy_waveInGetErrorTextW() { __asm { jmp dword ptr [g_pProcs + 624] } }
    __declspec(naked) void Proxy_waveInGetID() { __asm { jmp dword ptr [g_pProcs + 628] } }
    __declspec(naked) void Proxy_waveInGetNumDevs() { __asm { jmp dword ptr [g_pProcs + 632] } }
    __declspec(naked) void Proxy_waveInGetPosition() { __asm { jmp dword ptr [g_pProcs + 636] } }
    __declspec(naked) void Proxy_waveInMessage() { __asm { jmp dword ptr [g_pProcs + 640] } }
    __declspec(naked) void Proxy_waveInOpen() { __asm { jmp dword ptr [g_pProcs + 644] } }
    __declspec(naked) void Proxy_waveInPrepareHeader() { __asm { jmp dword ptr [g_pProcs + 648] } }
    __declspec(naked) void Proxy_waveInReset() { __asm { jmp dword ptr [g_pProcs + 652] } }
    __declspec(naked) void Proxy_waveInStart() { __asm { jmp dword ptr [g_pProcs + 656] } }
    __declspec(naked) void Proxy_waveInStop() { __asm { jmp dword ptr [g_pProcs + 660] } }
    __declspec(naked) void Proxy_waveInUnprepareHeader() { __asm { jmp dword ptr [g_pProcs + 664] } }
    __declspec(naked) void Proxy_waveOutBreakLoop() { __asm { jmp dword ptr [g_pProcs + 668] } }
    __declspec(naked) void Proxy_waveOutClose() { __asm { jmp dword ptr [g_pProcs + 672] } }
    __declspec(naked) void Proxy_waveOutGetDevCapsA() { __asm { jmp dword ptr [g_pProcs + 676] } }
    __declspec(naked) void Proxy_waveOutGetDevCapsW() { __asm { jmp dword ptr [g_pProcs + 680] } }
    __declspec(naked) void Proxy_waveOutGetErrorTextA() { __asm { jmp dword ptr [g_pProcs + 684] } }
    __declspec(naked) void Proxy_waveOutGetErrorTextW() { __asm { jmp dword ptr [g_pProcs + 688] } }
    __declspec(naked) void Proxy_waveOutGetID() { __asm { jmp dword ptr [g_pProcs + 692] } }
    __declspec(naked) void Proxy_waveOutGetNumDevs() { __asm { jmp dword ptr [g_pProcs + 696] } }
    __declspec(naked) void Proxy_waveOutGetPitch() { __asm { jmp dword ptr [g_pProcs + 700] } }
    __declspec(naked) void Proxy_waveOutGetPlaybackRate() { __asm { jmp dword ptr [g_pProcs + 704] } }
    __declspec(naked) void Proxy_waveOutGetPosition() { __asm { jmp dword ptr [g_pProcs + 708] } }
    __declspec(naked) void Proxy_waveOutGetVolume() { __asm { jmp dword ptr [g_pProcs + 712] } }
    __declspec(naked) void Proxy_waveOutMessage() { __asm { jmp dword ptr [g_pProcs + 716] } }
    __declspec(naked) void Proxy_waveOutOpen() { __asm { jmp dword ptr [g_pProcs + 720] } }
    __declspec(naked) void Proxy_waveOutPause() { __asm { jmp dword ptr [g_pProcs + 724] } }
    __declspec(naked) void Proxy_waveOutPrepareHeader() { __asm { jmp dword ptr [g_pProcs + 728] } }
    __declspec(naked) void Proxy_waveOutReset() { __asm { jmp dword ptr [g_pProcs + 732] } }
    __declspec(naked) void Proxy_waveOutRestart() { __asm { jmp dword ptr [g_pProcs + 736] } }
    __declspec(naked) void Proxy_waveOutSetPitch() { __asm { jmp dword ptr [g_pProcs + 740] } }
    __declspec(naked) void Proxy_waveOutSetPlaybackRate() { __asm { jmp dword ptr [g_pProcs + 744] } }
    __declspec(naked) void Proxy_waveOutSetVolume() { __asm { jmp dword ptr [g_pProcs + 748] } }
    __declspec(naked) void Proxy_waveOutUnprepareHeader() { __asm { jmp dword ptr [g_pProcs + 752] } }
    __declspec(naked) void Proxy_waveOutWrite() { __asm { jmp dword ptr [g_pProcs + 756] } }
    __declspec(naked) void Proxy_wid32Message() { __asm { jmp dword ptr [g_pProcs + 760] } }
    __declspec(naked) void Proxy_wod32Message() { __asm { jmp dword ptr [g_pProcs + 764] } }
}
