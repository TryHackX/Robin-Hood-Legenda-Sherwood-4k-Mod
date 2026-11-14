# Background test scenario: start the game hidden (tools\gamectl.ps1 -Start -Headless), skip the
# intro videos, and - unless -MenuOnly - press "Graj!" and wait until the mission runs.
# Nothing appears on the screen and no sound is played. Afterwards drive it with tools\hctl.ps1,
# and end it with:  tools\hctl.ps1 -Cmd quit
#   -Ini file     settings for this run (default: the player's SherwoodFix.ini)
#   -MenuOnly     stop at the main menu
param([string]$Ini = '', [switch]$MenuOnly)

$ctl  = Join-Path $PSScriptRoot 'gamectl.ps1'
$hctl = Join-Path $PSScriptRoot 'hctl.ps1'
function Send-H([string]$c) { (powershell -NoProfile -ExecutionPolicy Bypass -File $hctl -Cmd $c | Select-Object -Last 1) -replace '^.*? -> ', '' }

# The hidden instance must never touch the player's pointer. The game centres the Windows pointer
# on every pass of the mission loop; winmm.dll cuts that off in test mode and reports the number
# of swallowed calls as "desktopBlocked=" in the answer to "stat". No such field = an old DLL
# without the isolation: end the instance at once. As a second check the pointer itself is
# watched: the player is producing input, yet it sits on the exact screen centre = pinned.
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class PtrGuard {
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [StructLayout(LayoutKind.Sequential)] public struct LASTINPUTINFO { public uint cbSize; public uint dwTime; }
  [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
  [DllImport("user32.dll")] public static extern bool GetLastInputInfo(ref LASTINPUTINFO i);
  [DllImport("user32.dll")] public static extern int GetSystemMetrics(int n);
  [DllImport("kernel32.dll")] public static extern uint GetTickCount();
}
'@
function Test-PointerPinned {
    $cx = [int]([PtrGuard]::GetSystemMetrics(0) / 2); $cy = [int]([PtrGuard]::GetSystemMetrics(1) / 2)
    $atCentre = 0; $active = 0; $n = 12
    foreach ($i in 1..$n) {
        $p = New-Object PtrGuard+POINT; [void][PtrGuard]::GetCursorPos([ref]$p)
        if ($p.X -eq $cx -and $p.Y -eq $cy) { $atCentre++ }
        $li = New-Object PtrGuard+LASTINPUTINFO; $li.cbSize = 8; [void][PtrGuard]::GetLastInputInfo([ref]$li)
        if (([PtrGuard]::GetTickCount() - $li.dwTime) -lt 400) { $active++ }
        Start-Sleep -Milliseconds 80
    }
    return ($atCentre -eq $n -and $active -ge 3)
}
function Stop-TestInstance([string]$why) {
    Get-Process Game -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -eq 0 } | Stop-Process -Force -ErrorAction SilentlyContinue
    Write-Output "RESULT: test instance ended - $why"
    exit 1
}

$startArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $ctl, '-Start', '-Headless')
if ($Ini) { $startArgs += @('-Ini', $Ini) }
Write-Output (powershell @startArgs)

# wait for the first frames (intro video = 640x480)
$st = ''
foreach ($i in 1..40) { Start-Sleep -Milliseconds 500; $st = Send-H 'stat'; if ($st -match '^ok') { break } }
if ($st -notmatch '^ok') { Write-Output "RESULT: no answer from the game: $st"; exit 1 }
if ($st -notmatch 'desktopBlocked=') { Stop-TestInstance 'this winmm.dll does not isolate the pointer in test mode (no desktopBlocked= in stat)' }

# one Esc per video, only while a video is on (a spare Esc would open the in-game menu later)
foreach ($i in 1..10) {
    $st = Send-H 'stat'
    if ($st -notmatch 'internal=640x480') { break }
    [void](Send-H 'key 0x01')
    Start-Sleep -Milliseconds 1300
}
Start-Sleep -Milliseconds 1500
$st = Send-H 'stat'
if ($st -notmatch 'internal=(\d+)x(\d+)') { Write-Output "RESULT: unexpected state: $st"; exit 1 }
$w = [int]$matches[1]; $h = [int]$matches[2]
Write-Output "menu: $st"
if ($MenuOnly) { exit 0 }

# main menu: 1024x480 art centred on the screen; "Graj!" is 234 px right and 27 px above the centre
[void](Send-H ("moveto {0} {1}" -f ([int]($w / 2) + 234), ([int]($h / 2) - 27)))
[void](Send-H 'click')
foreach ($i in 1..80) {
    Start-Sleep -Milliseconds 500
    $st = Send-H 'stat'
    if ($st -match 'engine=1 map=[1-9]') { break }
}
if (Test-PointerPinned) { Stop-TestInstance 'the Windows pointer is pinned to the screen centre while the mission runs' }
Start-Sleep -Seconds 2
Write-Output "mission: $(Send-H 'stat')"
