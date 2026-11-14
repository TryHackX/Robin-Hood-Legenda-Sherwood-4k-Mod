# Automated smoke test: start the game, skip the intro videos, press "Graj!" in the main menu
# (continues the current profile's campaign) and watch the first seconds of the mission.
# Reports whether the game survived and what the log says. Needs the foreground: it only runs
# when the user has been idle (see gamectl.ps1 -Focus).
#   powershell -ExecutionPolicy Bypass -File tools\autotest_newgame.ps1 [-PlaySeconds 25] [-Tag name] [-KeepRunning]
#   -MoveScale: mouse counts per internal pixel. 1 at 2560x1440; 2 at 1280x720 on a 1440p monitor
#               (MouseSensitivity=0 halves the cursor speed there).
param([int]$PlaySeconds = 25, [string]$Tag = 'autotest', [switch]$KeepRunning, [double]$MoveScale = 1.0)

$root = Split-Path -Parent $PSScriptRoot
$ctl  = Join-Path $PSScriptRoot 'gamectl.ps1'
$log  = Join-Path $root 'SherwoodFix.log'
function Ctl { powershell -NoProfile -ExecutionPolicy Bypass -File $ctl @args }
function Alive { [bool](Get-Process -Name Game -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 }) }

Ctl -Start | Out-Null
Start-Sleep -Seconds 6
if (-not (Alive)) { Write-Output 'RESULT: the game did not start.'; exit 1 }
$f = Ctl -Focus
Write-Output $f
if ($f -match 'not taking the foreground') { Ctl -Kill | Out-Null; exit 4 }

# intro videos: Esc skips one each
foreach ($i in 1..5) { Ctl -Key 0x01 -Wait 1700 | Out-Null }
Start-Sleep -Seconds 2
Ctl -Shot "${Tag}_menu" | Out-Null

# main menu: the cursor starts in the middle of the 1024x768 menu; "Graj!" is 224 px right, 40 px up
Ctl -MoveX ([int](224 * $MoveScale)) -MoveY ([int](-40 * $MoveScale)) -Wait 500 | Out-Null
Ctl -Click -Wait 1500 | Out-Null
Ctl -Click -Wait 500 | Out-Null

$sw = [Diagnostics.Stopwatch]::StartNew()
$shots = 0
while ($sw.Elapsed.TotalSeconds -lt $PlaySeconds) {
    Start-Sleep -Milliseconds 300
    if (-not (Alive)) { break }
    # a screenshot roughly every second; the last ones show what was on screen before a crash
    if ($shots -lt 40) { $shots++; Ctl -Shot ("{0}_play{1:D2}" -f $Tag, $shots) | Out-Null }
}
$alive = Alive
Write-Output ("RESULT: game alive after {0:F0} s: {1}" -f $sw.Elapsed.TotalSeconds, $alive)
Get-Content $log | Select-String -Pattern 'FATAL|Map \d|MotionSmooth: access|Watchdog: (no frame|the process|the game window is in front)|ExitProcess' |
    Select-Object -Last 6 | ForEach-Object { $_.Line }
Get-Content $log | Select-String -Pattern 'Perf\(5s\)|Input\(5s\)' | Select-Object -Last 2 | ForEach-Object { $_.Line }
if ($alive -and -not $KeepRunning) { Ctl -Close }
