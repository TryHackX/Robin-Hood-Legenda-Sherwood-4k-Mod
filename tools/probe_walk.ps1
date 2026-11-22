# Where can a character walk? With a character selected in the hidden test instance, the game's cursor over the world
# is "walk there" or a cross ("cannot go there"). This moves the cursor over a grid of screen points and asks the mod
# which cursor picture the game drew there (harness command "cursor"), one hctl call for all of them.
#   powershell -File tools\probe_walk.ps1 -X0 1100 -X1 2500 -Y0 1000 -Y1 1400 -Step 40 -Out tools\out\walk.csv
# The CSV: sx, sy, cursor (WxH key). Map screen points to level pixels with the camera from "stat" (zoom 2.0:
# level = cam + screen / 2).
param([int]$X0 = 1100, [int]$X1 = 2500, [int]$Y0 = 1000, [int]$Y1 = 1400, [int]$Step = 40, [string]$Out = '')

$hctl = Join-Path $PSScriptRoot 'hctl.ps1'
$points = @()
for ($y = $Y0; $y -le $Y1; $y += $Step) {
    for ($x = $X0; $x -le $X1; $x += $Step) { $points += , @($x, $y) }
}
$cmds = @()
foreach ($p in $points) { $cmds += "moveto $($p[0]) $($p[1])"; $cmds += "cursor" }
# about 60 points per call (the answers must come within the timeout)
$rows = @('sx,sy,cursor')
for ($i = 0; $i -lt $cmds.Count; $i += 120) {
    $chunk = $cmds[$i..([Math]::Min($i + 119, $cmds.Count - 1))] -join '; '
    $answers = powershell -NoProfile -ExecutionPolicy Bypass -File $hctl -Cmd $chunk -TimeoutMs 60000
    $last = ''
    foreach ($a in $answers) {
        if ($a -match '^moveto (\d+) (\d+) -> ') { $last = "$($matches[1]),$($matches[2])" }
        elseif ($a -match '^cursor -> ok cursor=(\d+x\d+) key=([0-9a-f]+)') { $rows += "$last,$($matches[1])_$($matches[2])" }
    }
}
if ($Out) { Set-Content -Path $Out -Value $rows -Encoding ascii; Write-Output "$($rows.Count - 1) points -> $Out" }
else { $rows }
