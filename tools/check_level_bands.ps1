# Background test: loads the saved games of the load list one after another and looks for a black band beside the
# level (a level narrower / shorter than the screen shown at the engine's zoom 1.0 - the first frame and the first
# briefing of a level before 1.2.3). For every entry: the level's size (stat), a shot right after the load and
# one 2 s later, and how much of the right edge strip and of the strip above the HUD is pure black.
#   powershell -File tools\check_level_bands.ps1 [-First 0] [-Count 30] [-Scroll 0]
# -Scroll N: the list is scrolled N lines down (mouse wheel) before an entry is picked - older saves.
# Needs a hidden test instance in a mission (tools\htest.ps1). Loads saves: back up DATA\Savegame first
# (tools\save_backup.ps1) - the game itself does not write when loading, but the test may leave a mission.
param([int]$First = 0, [int]$Count = 30, [int]$Scroll = 0)

$root = Split-Path -Parent $PSScriptRoot
$hctl = Join-Path $PSScriptRoot 'hctl.ps1'
$outDir = Join-Path $root 'tools\out\level_bands'
New-Item -ItemType Directory -Force $outDir | Out-Null

function Cmd([string[]]$c, [int]$wait = 0) {
    & $hctl -Cmd $c -Wait $wait -TimeoutMs 30000 2>&1 | Out-String
}

$py = @'
import sys
from PIL import Image
import numpy as np
for p in sys.argv[1:]:
    a = np.asarray(Image.open(p).convert('RGB')).astype(np.int32)
    h, w, _ = a.shape
    right = a[100:1250, w - 60:w - 4].max(-1)       # beside the right edge (below the corner icons)
    bottom = a[1180:1260, 300:w - 300].max(-1)       # just above the hedge band
    print('%s right_black=%.2f bottom_black=%.2f' % (p.split('\\')[-1], (right < 8).mean(), (bottom < 8).mean()))
'@
$pyFile = Join-Path $outDir 'band_metric.py'
Set-Content -Path $pyFile -Value $py -Encoding ascii

$results = @()
for ($i = $First; $i -lt $First + $Count; $i++) {
    $y = [int][math]::Round(497 + 14.93 * $i)
    $null = Cmd @('key 0x01') 900
    $null = Cmd @('moveto 1515 774', 'click') 1200
    if ($Scroll -gt 0) { $null = Cmd (@('moveto 1100 700') + (1..$Scroll | ForEach-Object { 'wheel -1' })) 300 }
    $null = Cmd @("moveto 1020 $y", 'click') 400
    $a = Cmd @('moveto 1515 857', 'click', "shotout band_${i}_a") 2500
    $b = Cmd @('stat', "shotout band_${i}_b") 0
    $map = if ($b -match 'map=(\d+x\d+)') { $matches[1] } else { '?' }
    $zoom = if ($b -match 'zoom=([\d\.]+)') { $matches[1] } else { '?' }
    $bmpA = Join-Path $root "tools\out\harness\band_${i}_a.bmp"
    $bmpB = Join-Path $root "tools\out\harness\band_${i}_b.bmp"
    $m = & python -I $pyFile $bmpA $bmpB 2>&1 | Out-String
    $line = "entry $i (scroll $Scroll, y=$y): map=$map zoom=$zoom | " + (($m -split "`r?`n" | Where-Object { $_ }) -join ' | ')
    Write-Output $line
    $results += $line
}
$results | Set-Content -Path (Join-Path $outDir ("result_scroll$Scroll" + '.txt')) -Encoding utf8
