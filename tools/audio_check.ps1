# Is a process heard? Lists its audio sessions on every active playback device and their loudest peak over a few
# seconds (the meter the Windows volume mixer shows). For the hidden test instance the answer must be "no session" or
# a peak of 0 (2026-11-22: the player heard the hidden game).
#   powershell -File tools\audio_check.ps1                  the newest Game.exe
#   powershell -File tools\audio_check.ps1 -ProcessId 1234 -Seconds 5
#   powershell -File tools\audio_check.ps1 -ProcessId -1    the system sounds session (PID 0) - to see the check works
# Read-only: it changes no volume and no mute.
param([int]$ProcessId = 0, [double]$Seconds = 3)

if ($ProcessId -lt 0) { $ProcessId = 0 }
elseif ($ProcessId -eq 0) {
    $p = Get-Process Game -ErrorAction SilentlyContinue | Sort-Object StartTime -Descending | Select-Object -First 1
    if (-not $p) { Write-Output "no Game.exe running"; exit 1 }
    $ProcessId = $p.Id
}

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

[ComImport, Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IMMDeviceEnumerator {
    int EnumAudioEndpoints(int dataFlow, int stateMask, out IMMDeviceCollection devices);
    int GetDefaultAudioEndpoint(int dataFlow, int role, out IMMDevice device);
}
[ComImport, Guid("0BD7A1BE-7A1A-44DB-8397-CC5392387B5E"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IMMDeviceCollection {
    int GetCount(out int count);
    int Item(int index, out IMMDevice device);
}
[ComImport, Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IMMDevice {
    int Activate(ref Guid iid, int clsCtx, IntPtr activationParams, [MarshalAs(UnmanagedType.IUnknown)] out object iface);
    int OpenPropertyStore(int access, out IntPtr store);
    int GetId([MarshalAs(UnmanagedType.LPWStr)] out string id);
}
[ComImport, Guid("77AA99A0-1BD6-484F-8BC7-2C654C9A9B6F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IAudioSessionManager2 {
    int GetAudioSessionControl(IntPtr guid, int flags, out IntPtr control);
    int GetSimpleAudioVolume(IntPtr guid, int flags, out IntPtr volume);
    int GetSessionEnumerator(out IAudioSessionEnumerator sessions);
}
[ComImport, Guid("E2F5BB11-0570-40CA-ACDD-3AA01277DEE8"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IAudioSessionEnumerator {
    int GetCount(out int count);
    int GetSession(int index, [MarshalAs(UnmanagedType.IUnknown)] out object session);
}
[ComImport, Guid("bfb7ff88-7239-4fc9-8fa2-07c950be9c6d"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IAudioSessionControl2 {
    int GetState(out int state);
    int GetDisplayName(out IntPtr name);
    int SetDisplayName(IntPtr name, IntPtr ctx);
    int GetIconPath(out IntPtr path);
    int SetIconPath(IntPtr path, IntPtr ctx);
    int GetGroupingParam(out Guid param);
    int SetGroupingParam(ref Guid param, IntPtr ctx);
    int RegisterAudioSessionNotification(IntPtr events);
    int UnregisterAudioSessionNotification(IntPtr events);
    int GetSessionIdentifier(out IntPtr id);
    int GetSessionInstanceIdentifier(out IntPtr id);
    int GetProcessId(out uint pid);
}
[ComImport, Guid("C02216F6-8C67-4B5B-9D00-D008E73E0064"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IAudioMeterInformation {
    int GetPeakValue(out float peak);
}
[ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")]
class MMDeviceEnumeratorCo { }

public static class AudioCheck {
    public static List<string> Sessions(uint pid, double seconds) {
        var result = new List<string>();
        var en = (IMMDeviceEnumerator)new MMDeviceEnumeratorCo();
        IMMDeviceCollection devices;
        en.EnumAudioEndpoints(0 /* eRender */, 1 /* ACTIVE */, out devices);
        int n; devices.GetCount(out n);
        var meters = new List<KeyValuePair<string, IAudioMeterInformation>>();
        for (int i = 0; i < n; ++i) {
            IMMDevice dev; devices.Item(i, out dev);
            string id; dev.GetId(out id);
            Guid iid = typeof(IAudioSessionManager2).GUID;
            object o; if (dev.Activate(ref iid, 23 /* CLSCTX_ALL */, IntPtr.Zero, out o) != 0) continue;
            var mgr = (IAudioSessionManager2)o;
            IAudioSessionEnumerator sessions; if (mgr.GetSessionEnumerator(out sessions) != 0) continue;
            int count; sessions.GetCount(out count);
            for (int k = 0; k < count; ++k) {
                object s; sessions.GetSession(k, out s);
                var c2 = s as IAudioSessionControl2;
                if (c2 == null) continue;
                uint p; c2.GetProcessId(out p);
                if (p != pid) continue;
                int state; c2.GetState(out state);
                meters.Add(new KeyValuePair<string, IAudioMeterInformation>("device " + i + " session state " + state, s as IAudioMeterInformation));
            }
        }
        var peaks = new float[meters.Count];
        var until = DateTime.Now.AddSeconds(seconds);
        while (DateTime.Now < until) {
            for (int m = 0; m < meters.Count; ++m) {
                float v = 0; if (meters[m].Value != null) meters[m].Value.GetPeakValue(out v);
                if (v > peaks[m]) peaks[m] = v;
            }
            System.Threading.Thread.Sleep(50);
        }
        for (int m = 0; m < meters.Count; ++m) result.Add(meters[m].Key + ": loudest peak " + peaks[m].ToString("0.0000"));
        return result;
    }
}
'@

$lines = [AudioCheck]::Sessions([uint32]$ProcessId, $Seconds)
if ($lines.Count -eq 0) {
    Write-Output "PID ${ProcessId}: no audio session on any playback device - nothing is heard."
} else {
    foreach ($l in $lines) { Write-Output "PID ${ProcessId}: $l" }
}
