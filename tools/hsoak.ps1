# Background soak test: start the game hidden (tools\htest.ps1), then for -Minutes keep the camera
# moving over the level, switch HD background variants, try zooming out and in, and watch that
# the game keeps answering. Ends the instance and prints a verdict with the numbers from
# SherwoodFix.log (tick rate, work per tick, warnings, errors, crash).
#   -Ini file      settings for the run (default tools\testcfg\native1440.ini)
#   -Minutes n     how long to keep going (default 4)
#   -KeepRunning   leave the instance running at the end
#   -Ui            also use the interface: open and close the map (top-right button), the in-game
#                  menu (Esc) and the mod's settings panel (F10, nothing changed), click the portrait
param([string]$Ini = '', [double]$Minutes = 4, [switch]$KeepRunning, [switch]$Ui)

$root = Split-Path $PSScriptRoot -Parent
if (-not $Ini) { $Ini = Join-Path $PSScriptRoot 'testcfg\native1440.ini' }
$hctl = Join-Path $PSScriptRoot 'hctl.ps1'
$log  = Join-Path $root 'SherwoodFix.log'
function Send-H([string]$c) { (powershell -NoProfile -ExecutionPolicy Bypass -File $hctl -Cmd $c | Select-Object -Last 1) -replace '^.*? -> ', '' }

$start = powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'htest.ps1') -Ini $Ini
$start | Select-Object -Last 1 | ForEach-Object { Write-Output $_ }
if (($start | Select-Object -Last 1) -notmatch '^mission: ok') { Write-Output 'RESULT: FAIL - the mission did not start'; exit 1 }
# briefing: OK on every page (a briefing has up to several pages; while it is up the menu zoom
# keeps the cursor in the middle of the screen, so the corner cannot be reached)
for ($page = 0; $page -lt 8; $page++) {
    [void](Send-H 'moveto 1280 888')
    [void](Send-H 'click')
    Start-Sleep -Milliseconds 1500
    if ((Send-H 'moveto 2455 128') -match '^ok') { break }
}
Start-Sleep -Seconds 3

$keys = @('0xC8', '0xD0', '0xCB', '0xCD')   # up, down, left, right
$rand = New-Object System.Random 12345
$deadline = (Get-Date).AddMinutes($Minutes)
$steps = 0; $noAnswer = 0; $lastStat = ''
while ((Get-Date) -lt $deadline) {
    $steps++
    $k = $keys[$rand.Next(0, 4)]
    $hold = $rand.Next(300, 2200)
    $st = Send-H "key $k $hold"
    if ($st -notmatch '^ok') { $noAnswer++ }
    switch ($steps % 12) {
        3  { [void](Send-H 'hd next') }
        6  { [void](Send-H 'zoomout') }
        7  { [void](Send-H 'zoomin') }
        9  { [void](Send-H 'hd toggle') }
        10 { [void](Send-H 'hd toggle') }
    }
    if ($Ui) {
        switch ($steps % 16) {
            1  { [void](Send-H 'moveto 2455 128'); [void](Send-H 'click') }                 # map open
            2  { [void](Send-H 'moveto 2390 420'); [void](Send-H 'down')                    # drag on the map: camera
                 [void](Send-H 'move 30 20'); [void](Send-H 'move 30 20'); [void](Send-H 'up') }
            3  { [void](Send-H 'moveto 2105 400'); [void](Send-H 'down R')                  # map window by its edge
                 [void](Send-H 'move -40 20'); [void](Send-H 'move 40 -20'); [void](Send-H 'up R') }
            4  { [void](Send-H 'moveto 2455 128'); [void](Send-H 'click') }                 # map closed
            5  { [void](Send-H 'moveto 560 1290'); [void](Send-H 'click') }                 # portrait
            8  { [void](Send-H 'key 0x01'); Start-Sleep -Milliseconds 1500
                 [void](Send-H 'moveto 1514 734'); [void](Send-H 'click') }                 # Esc menu, Continue
            12 { [void](Send-H 'settings open'); Start-Sleep -Milliseconds 800
                 [void](Send-H 'settings down'); [void](Send-H 'settings close') }       # settings panel
        }
    }
    if ($steps % 5 -eq 0) {
        $lastStat = Send-H 'stat'
        if ($lastStat -notmatch '^ok') { $noAnswer++ }
        if (-not (Get-Process Game -ErrorAction SilentlyContinue)) { Write-Output 'RESULT: FAIL - the game process is gone'; break }
        if ($noAnswer -ge 4) { Write-Output 'RESULT: FAIL - the game stopped answering'; break }
    }
}
Write-Output "steps: $steps, unanswered commands: $noAnswer"
Write-Output "last: $lastStat"

$lines = Get-Content $log
$perf = $lines | Select-String -Pattern 'Perf\(5s\): Tick=([\d.]+)Hz Work=([\d.]+)ms \(max (\d+)\) Present=([\d.]+)Hz' | Select-Object -Skip 3
$ticks = @(); $works = @(); $maxes = @(); $pres = @()
foreach ($p in $perf) { $ticks += [double]$p.Matches[0].Groups[1].Value; $works += [double]$p.Matches[0].Groups[2].Value; $maxes += [int]$p.Matches[0].Groups[3].Value; $pres += [double]$p.Matches[0].Groups[4].Value }
if ($ticks.Count -gt 0) {
    Write-Output ("perf over {0} five-second windows: tick rate min {1:N1} / avg {2:N1} Hz; work per tick avg {3:N1} ms, worst single tick {4} ms; frames presented avg {5:N1} / min {6:N1} per second" -f `
        $ticks.Count, ($ticks | Measure-Object -Minimum).Minimum, ($ticks | Measure-Object -Average).Average, ($works | Measure-Object -Average).Average,
        ($maxes | Measure-Object -Maximum).Maximum, ($pres | Measure-Object -Average).Average, ($pres | Measure-Object -Minimum).Minimum)
}
$bad = $lines | Select-String -Pattern '\[ERROR\]|EXCEPTION_|ACCESS_VIOLATION|Unhandled|no beat for|changed its level picture' |
       Where-Object { $_.Line -notmatch 'hang reports after|CrashVectoredHandler registered' }
# expected in test mode: the hidden window never gets the input devices
$warn = $lines | Select-String -Pattern '\[WARN\]' |
        Where-Object { $_.Line -notmatch 'TEST MODE|could not be replaced|Settings file overridden|DirectInput acquire failed on activation' }
Write-Output ("errors/crash lines: {0}; other warnings: {1}" -f @($bad).Count, @($warn).Count)
@($bad) | Select-Object -First 8 | ForEach-Object { Write-Output ("  " + $_.Line) }
@($warn) | Select-Object -First 8 | ForEach-Object { Write-Output ("  " + $_.Line) }

if (-not $KeepRunning) {
    [void](Send-H 'quit')
    Start-Sleep -Seconds 4
    if (Get-Process Game -ErrorAction SilentlyContinue) { Write-Output 'note: the game did not exit after quit' }
}
if (@($bad).Count -eq 0 -and $noAnswer -lt 4) { Write-Output 'RESULT: PASS' } else { Write-Output 'RESULT: CHECK the lines above' }
