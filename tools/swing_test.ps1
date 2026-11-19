# Sword swing test in the BACKGROUND TEST MODE: start the hidden game at the main menu, press "Graj!" (the
# player's Continue point loads - Lincoln, pause menu), "Kontynuuj", start a fight (the scenario of
# tools\coop_run.ps1: camera right, Robin by his portrait, the tower, a guard in the courtyard) and draw
# strokes. The log lines "Swing points" / "Swing check" / "Swing:" (src\ui_scale.cpp) are copied to
# tools\out\harness\swing_<Name>.txt. Ends the instance and puts the player's saves back.
#   -Name n     output name
#   -Ini file   settings (default tools\testcfg\user_copy.ini)
#   -Wheel n    mouse wheel notches over the fighter before the strokes (+ in, - out; the smooth zoom)
param([string]$Name = 's1', [string]$Ini = '', [int]$Wheel = 0)

$root = Split-Path $PSScriptRoot -Parent
if (-not $Ini) { $Ini = Join-Path $PSScriptRoot 'testcfg\user_copy.ini' }
$hctl = Join-Path $PSScriptRoot 'hctl.ps1'
$log = Join-Path $root 'SherwoodFix.log'
function Send-H([string]$c) { (powershell -NoProfile -ExecutionPolicy Bypass -File $hctl -Cmd $c | Select-Object -Last 1) -replace '^.*? -> ', '' }

if (Get-Process Game -ErrorAction SilentlyContinue) { Write-Output 'RESULT: Game.exe is running - not starting a test'; exit 2 }
if (-not (Test-Path (Join-Path $root 'tools\out\last_save_backup.txt'))) { Write-Output 'RESULT: run tools\save_backup.ps1 first'; exit 2 }
if ((powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'save_backup.ps1') -Check) -notmatch 'unchanged') {
    Write-Output 'RESULT: the saves differ from the backup - make a new backup or restore first'; exit 2
}

# one stroke: start point, then the whole movement split into harness moves of at most 120 px (the harness steps 12 px every 8 ms)
function Stroke([int]$x, [int]$y, [int]$dx, [int]$dy, [string]$shot = '') {
    [void](Send-H "moveto $x $y")
    [void](Send-H 'down')
    $n = [int]([math]::Ceiling([math]::Max([math]::Abs($dx), [math]::Abs($dy)) / 120.0))
    $moves = @()
    for ($i = 1; $i -le $n; $i++) {
        $mx = [int]($dx * $i / $n) - [int]($dx * ($i - 1) / $n); $my = [int]($dy * $i / $n) - [int]($dy * ($i - 1) / $n)
        $moves += "move $mx $my"
    }
    # one call; with a shot the last move is "moveshot": the output frame while the stroke still goes on
    # (the trail fades within a fraction of a second)
    if ($shot) { $moves[-1] = "moveshot $($moves[-1].Substring(5)) $shot" }
    [void](Send-H ($moves -join '; '))
    [void](Send-H 'up')
    Start-Sleep -Milliseconds 1500
}

$startLine = 0
if (Test-Path $log) { $startLine = (Get-Content $log).Count }
try {
    $start = powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'htest.ps1') -Ini $Ini -MenuOnly
    if (($start | Select-Object -Last 1) -notmatch '^menu: ok') { Write-Output "RESULT: no menu: $($start | Select-Object -Last 1)"; exit 1 }
    if (Test-Path $log) { $startLine = [math]::Min($startLine, (Get-Content $log).Count) }
    [void](Send-H 'moveto 1514 693'); [void](Send-H 'click')                     # "Graj!"
    $ok = $false
    foreach ($i in 1..80) { Start-Sleep -Milliseconds 500; if ((Send-H 'stat') -match 'engine=1 map=[1-9]') { $ok = $true; break } }
    if (-not $ok) { Write-Output 'RESULT: the mission did not load'; exit 1 }
    Start-Sleep -Seconds 2
    [void](Send-H 'moveto 1516 734'); [void](Send-H 'click')                     # "Kontynuuj"
    Start-Sleep -Seconds 2
    [void](Send-H 'key 0xCD 2500')                                               # camera to the right (Robin's tower)
    Start-Sleep -Milliseconds 800
    [void](Send-H 'moveto 336 1344'); [void](Send-H 'click')                     # Robin (portrait)
    Start-Sleep -Milliseconds 800
    [void](Send-H 'moveto 2080 240'); [void](Send-H 'click')                     # the tower top
    Start-Sleep -Milliseconds 2500
    [void](Send-H 'moveto 1840 784'); [void](Send-H 'click')                     # a guard: jump down, fight
    Start-Sleep -Milliseconds 5000
    [void](powershell -NoProfile -ExecutionPolicy Bypass -File $hctl -Cmd 'stat' -Shot "swing_${Name}_fight")

    if ($Wheel) {
        [void](Send-H 'moveto 1904 728')
        foreach ($i in 1..[math]::Abs($Wheel)) { [void](Send-H "wheel $([math]::Sign($Wheel))"); Start-Sleep -Milliseconds 400 }   # one step per notch
        Start-Sleep -Milliseconds 2500
        Write-Output "zoom: $(Send-H 'stat')"
    }
    Stroke 1500 760 600 0 "swing_${Name}_trail"     # right
    Stroke 2150 760 -600 0      # left
    Stroke 1850 1000 0 -450     # up
    Stroke 1850 520 0 450       # down
    Stroke 1500 640 600 150     # right, a little down
    Stroke 1500 900 600 -40     # right, nearly flat
    Stroke 1600 760 900 0       # long right
    Write-Output "strokes done: $(Send-H 'stat')"
} finally {
    [void](Send-H 'quit')
    Start-Sleep -Seconds 4
    Get-Process Game -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -eq 0 } | Stop-Process -Force -ErrorAction SilentlyContinue
    powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'save_backup.ps1') -Restore -All | Out-Null
    Write-Output "saves: $(powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'save_backup.ps1') -Check)"
    $out = Join-Path $root "tools\out\harness\swing_$Name.txt"
    if (Test-Path $log) {
        Get-Content $log | Select-Object -Skip $startLine | Where-Object { $_ -match 'Swing|ViewZoom|Perf\(5s\)' } | Set-Content -Path $out -Encoding ascii
        Write-Output "swing lines: $((Get-Content $out).Count) -> $out"
    }
}
