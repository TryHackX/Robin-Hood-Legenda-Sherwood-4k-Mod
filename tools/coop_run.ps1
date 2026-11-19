# Co-op probe run in the BACKGROUND TEST MODE (src\coop_probe.h): start the hidden game at the main
# menu, arm a probe session, press "Graj!" (the player's Continue point loads - Lincoln, pause menu),
# press "Kontynuuj", and either play a fixed scenario (record) or let the log replay itself (play).
# Ends the instance and puts the player's saves back.
#   -Name n       session name (files tools\out\harness\coop_<n>_*)
#   -Mode rec|play
#   -Tag t        name of a replay's hash files (default "play")
#   -Ticks n      engine ticks to run before the session stops (default 2400 = 96 s of game time)
#   -Isolate m    rand() isolation mask for the session (coop isolate; 64 = the sound module's own generator)
#   -Trace        also write the call trace (coop_trace_<n>_<mode/tag>.txt)
#   -Camera "x y" replay with the camera held there (world pixels) instead of where the recording had it
#   -DumpEvery n  element dump every n ticks (default 50; a replay also dumps every tick whose state
#                 hash differs from the recording's)
#   -Ini file     settings (default tools\testcfg\user_copy.ini)
# The scenario uses the screen positions of the player's Continue point of 2026-11-19 (Robin on the
# right tower of Lincoln castle, guards in the courtyard): camera right, select Robin by his portrait,
# walk on the tower, click a guard (jump down + fight), two sword strokes.
param(
    [string]$Name = 'c1', [ValidateSet('rec', 'play')][string]$Mode = 'rec', [string]$Tag = 'play',
    [int]$Ticks = 2400, [int]$Isolate = 64, [switch]$Trace, [string]$Camera = '', [int]$DumpEvery = 50, [string]$Ini = ''
)

$root = Split-Path $PSScriptRoot -Parent
if (-not $Ini) { $Ini = Join-Path $PSScriptRoot 'testcfg\user_copy.ini' }
$hctl = Join-Path $PSScriptRoot 'hctl.ps1'
function Send-H([string]$c) { (powershell -NoProfile -ExecutionPolicy Bypass -File $hctl -Cmd $c | Select-Object -Last 1) -replace '^.*? -> ', '' }
function Tick() { $s = Send-H 'coop stat'; if ($s -match 'tick=(\d+)') { return [int]$matches[1] } return -1 }

if (Get-Process Game -ErrorAction SilentlyContinue) { Write-Output 'RESULT: Game.exe is running - not starting a test'; exit 2 }
# the copy of the player's saves made before the first run (tools\save_backup.ps1) is what every run restores
if (-not (Test-Path (Join-Path $root 'tools\out\last_save_backup.txt'))) { Write-Output 'RESULT: run tools\save_backup.ps1 first'; exit 2 }
if ((powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'save_backup.ps1') -Check) -notmatch 'unchanged') {
    Write-Output 'RESULT: the saves differ from the backup - restore them first (save_backup.ps1 -Restore -All)'; exit 2
}

try {
    $start = powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'htest.ps1') -Ini $Ini -MenuOnly
    if (($start | Select-Object -Last 1) -notmatch '^menu: ok') { Write-Output "RESULT: no menu: $($start | Select-Object -Last 1)"; exit 1 }

    $label = if ($Mode -eq 'rec') { 'rec' } else { $Tag }
    [void](Send-H "coop isolate $Isolate")
    [void](Send-H "coop dumpevery $DumpEvery")
    if ($Camera) { Write-Output (Send-H "coop camera $Camera") }
    if ($Trace) { [void](Send-H "coop trace on ${Name}_$label") }
    if ($Mode -eq 'rec') { Write-Output (Send-H "coop rec $Name") } else { Write-Output (Send-H "coop play $Name $Tag") }

    # "Graj!": 234 px right of and 27 px above the screen centre (2560x1440 internal)
    [void](Send-H 'moveto 1514 693'); [void](Send-H 'click')
    $ok = $false
    foreach ($i in 1..80) { Start-Sleep -Milliseconds 500; if ((Send-H 'stat') -match 'engine=1 map=[1-9]') { $ok = $true; break } }
    if (-not $ok) { Write-Output 'RESULT: the mission did not load'; exit 1 }
    Start-Sleep -Seconds 2
    # the Continue point opens with the pause menu: "Kontynuuj"
    [void](Send-H 'moveto 1516 734'); [void](Send-H 'click')
    Start-Sleep -Seconds 2
    Write-Output "session: $(Send-H 'coop stat')"
    Write-Output "listeners: $(Send-H 'coop listeners')"

    if ($Mode -eq 'rec') {
        [void](Send-H 'key 0xCD 2500')                               # camera to the right (Robin's tower)
        Start-Sleep -Milliseconds 800
        [void](Send-H 'moveto 336 1344'); [void](Send-H 'click')     # select Robin (portrait)
        Start-Sleep -Milliseconds 800
        [void](Send-H 'moveto 2080 240'); [void](Send-H 'click')     # walk on the tower top
        Start-Sleep -Milliseconds 2500
        [void](Send-H 'moveto 1840 784'); [void](Send-H 'click')     # a guard in the courtyard: jump down, fight
        Start-Sleep -Milliseconds 5000
        foreach ($c in 'moveto 1700 760', 'down', 'move 120 0', 'move 120 0', 'move 120 0', 'up') { [void](Send-H $c) }   # stroke right
        Start-Sleep -Milliseconds 1500
        foreach ($c in 'moveto 1850 900', 'down', 'move 0 -120', 'move 0 -120', 'move 0 -120', 'up') { [void](Send-H $c) } # stroke up
        Write-Output "scenario done at tick $(Tick)"
    }

    $last = -1; $stuck = 0
    while ($true) {
        Start-Sleep -Seconds 2
        $t = Tick
        if ($t -ge $Ticks) { break }
        if ($t -eq $last) { $stuck++; if ($stuck -ge 10) { Write-Output "RESULT: no engine ticks for 20 s at tick $t"; break } } else { $stuck = 0 }
        $last = $t
    }
    [void](Send-H "coop randdump ${Name}_$label")
    Write-Output "end: $(Send-H 'coop stat')"
    powershell -NoProfile -ExecutionPolicy Bypass -File $hctl -Cmd 'stat' -Shot "coop_${Name}_${label}_end" | Select-Object -Last 1
    Write-Output (Send-H 'coop stop')
} finally {
    [void](Send-H 'quit')
    Start-Sleep -Seconds 4
    Get-Process Game -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -eq 0 } | Stop-Process -Force -ErrorAction SilentlyContinue
    # the player's saves as they were (the mission start rewrote Restart)
    powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'save_backup.ps1') -Restore -All | Out-Null
    $left = powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'save_backup.ps1') -Check
    Write-Output "saves: $left"
}
