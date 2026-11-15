# Remote control for a game running in BACKGROUND TEST MODE (tools\gamectl.ps1 -Start -Headless).
# The game is invisible and silent there, so none of this touches the screen, mouse or keyboard.
#
#   -Cmd "stat; key 0x01; moveto 640 360; click"      send commands in order, print each answer
#   -Shot name      dump the game's own frame      -> tools\out\<name>.png
#   -ShotOut name   dump the scaled output frame   -> tools\out\<name>.png
#   -Crop "x,y,w,h" cut this rectangle out of the dump (pixels of the dumped image)
#   -MaxWidth n     shrink the saved PNG to at most n pixels wide (0 = keep; default 1600)
#   -Wait ms        sleep after the commands (before the shot)
# Commands (src\test_harness.h): move DX DY | moveto X Y | click [L|R] | down [L|R] | up [L|R] |
#   key DIK [HOLD_MS] | shot NAME | shotout NAME | mem ADDR LEN | stat | quit
# DIK scan codes: 0x01 Esc, 0x1C Enter, 0x39 Space, 0x0F Tab, 0x3B.. F1.., arrows 0xC8/0xD0/0xCB/0xCD.
param(
    [string[]]$Cmd = @(), [int]$TimeoutMs = 15000, [string]$Shot = '', [string]$ShotOut = '',
    [string]$Crop = '', [int]$MaxWidth = 1600, [int]$Wait = 0
)

$root = Split-Path -Parent $PSScriptRoot
$dir  = Join-Path $root 'tools\out\harness'
New-Item -ItemType Directory -Force $dir | Out-Null
$cmdFile = Join-Path $dir 'cmd.txt'
$ackFile = Join-Path $dir 'ack.txt'
$seqFile = Join-Path $dir 'seq.txt'

function Send-One([string]$c) {
    $seq = 1
    if (Test-Path $seqFile) { $seq = [int](Get-Content $seqFile | Select-Object -First 1) + 1 }
    Set-Content $seqFile $seq -Encoding ascii
    $fs = [IO.File]::Open($cmdFile, 'Append', 'Write', 'ReadWrite')
    $bytes = [Text.Encoding]::ASCII.GetBytes("$seq $c`n")
    $fs.Write($bytes, 0, $bytes.Length)
    $fs.Close()
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $TimeoutMs) {
        if (Test-Path $ackFile) {
            $fsr = [IO.File]::Open($ackFile, 'Open', 'Read', 'ReadWrite')
            $sr = New-Object IO.StreamReader($fsr)
            $text = $sr.ReadToEnd()
            $sr.Close()
            $hit = $text -split "`n" | Where-Object { $_ -match "^$seq " } | Select-Object -Last 1
            if ($hit) { return $hit.Trim().Substring("$seq ".Length) }
        }
        Start-Sleep -Milliseconds 30
    }
    return "err no answer in $TimeoutMs ms (is a test instance running?)"
}

function Save-Shot([string]$command, [string]$name) {
    # the game writes the BMP into its harness folder by the bare name; the PNG may go into a subfolder
    $ans = Send-One "$command $(Split-Path -Leaf $name)"
    if ($ans -notmatch '^ok (.+\.bmp) (\d+)x(\d+)') { Write-Output "${command}: $ans"; return }
    $bmpPath = $matches[1]
    Add-Type -AssemblyName System.Drawing
    $src = [System.Drawing.Bitmap]::FromFile($bmpPath)
    $rect = New-Object System.Drawing.Rectangle 0, 0, $src.Width, $src.Height
    if ($Crop) {
        $p = $Crop -split '[,; ]+' | ForEach-Object { [int]$_ }
        $rect = New-Object System.Drawing.Rectangle $p[0], $p[1], $p[2], $p[3]
        $rect.Intersect((New-Object System.Drawing.Rectangle 0, 0, $src.Width, $src.Height))
    }
    $w = $rect.Width; $h = $rect.Height
    if ($MaxWidth -gt 0 -and $w -gt $MaxWidth) { $h = [int]($h * $MaxWidth / $w); $w = $MaxWidth }
    $dst = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($dst)
    $g.InterpolationMode = if ($w -eq $rect.Width) { 'NearestNeighbor' } else { 'HighQualityBicubic' }
    $g.PixelOffsetMode = 'Half'
    $g.DrawImage($src, (New-Object System.Drawing.Rectangle 0, 0, $w, $h), $rect, [System.Drawing.GraphicsUnit]::Pixel)
    $out = Join-Path $root "tools\out\$name.png"
    New-Item -ItemType Directory -Force (Split-Path -Parent $out) | Out-Null
    $dst.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $dst.Dispose(); $src.Dispose()
    Remove-Item $bmpPath -ErrorAction SilentlyContinue
    Write-Output ("{0}: {1} ({2}x{3} from {4}x{5})" -f $command, $out, $w, $h, $matches[2], $matches[3])
}

# several commands in one string are separated by ';' (an array does not survive "powershell -File")
$all = @($Cmd | ForEach-Object { $_ -split ';' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
foreach ($c in $all) { Write-Output ("{0} -> {1}" -f $c, (Send-One $c)) }
if ($Wait -gt 0) { Start-Sleep -Milliseconds $Wait }
if ($Shot)    { Save-Shot 'shot' $Shot }
if ($ShotOut) { Save-Shot 'shotout' $ShotOut }
