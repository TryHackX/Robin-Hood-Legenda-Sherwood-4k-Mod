# Checks in a hidden game that the mod reads the characters' store in parts (as the HD packs bring
# it): hd\sprites\_common\sprites.dat is set aside, the two parts from the packs take its place,
# a hidden mission runs, then the original comes back (also when something fails).
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$dir  = Join-Path $root 'hd\sprites\_common'
$dat  = Join-Path $dir 'sprites.dat'
$kept = Join-Path $dir 'sprites.dat.original'
$hctl = Join-Path $PSScriptRoot 'hctl.ps1'
# not "H": that is PowerShell's alias of Get-History, and an alias wins over a function
function Send-Cmd([string]$c) { powershell -NoProfile -ExecutionPolicy Bypass -File $hctl -Cmd $c }

if (Get-Process Game -ErrorAction SilentlyContinue) { throw 'the game runs - not now' }
if (Test-Path $kept) { throw "$kept exists already - restore it first" }
Add-Type -AssemblyName System.IO.Compression.FileSystem
Rename-Item $dat $kept
try {
    foreach ($p in @(@('SherwoodHD-1-postacie-a.zip', 'hd/sprites/_common/sprites.dat'), @('SherwoodHD-1-postacie-b.zip', 'hd/sprites/_common/sprites.dat.2'))) {
        $zip = [IO.Compression.ZipFile]::OpenRead((Join-Path $root "release\hdpack\$($p[0])"))
        try {
            $entry = $zip.GetEntry($p[1])
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, (Join-Path $root ($p[1] -replace '/', '\')), $true)
        } finally { $zip.Dispose() }
    }
    Get-ChildItem $dir | ForEach-Object { '{0,-24} {1,14:N0}' -f $_.Name, $_.Length }
    powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'htest.ps1') -Ini (Join-Path $PSScriptRoot 'testcfg\user_copy.ini') | Select-Object -Last 1
    for ($i = 0; $i -lt 8; $i++) {
        Send-Cmd 'moveto 1278 887; click' | Out-Null
        Start-Sleep -Milliseconds 1500
        if ((Send-Cmd 'moveto 2455 128') -match '-> ok') { break }
    }
    Start-Sleep 6
    Send-Cmd 'moveto 1280 700; stat'
    powershell -NoProfile -ExecutionPolicy Bypass -File $hctl -Cmd 'stat' -ShotOut split_store -MaxWidth 1280 | Select-Object -Last 1
    Select-String -Path (Join-Path $root 'SherwoodFix.log') -Pattern 'in 2 parts|store ''_common''|could not be read' | ForEach-Object { $_.Line }
} finally {
    # the original back first, whatever happened above
    $ErrorActionPreference = 'Continue'
    try { Send-Cmd 'quit' | Out-Null } catch {}
    for ($i = 0; $i -lt 20 -and (Get-Process Game -ErrorAction SilentlyContinue); $i++) { Start-Sleep -Milliseconds 500 }
    if (Get-Process Game -ErrorAction SilentlyContinue) {
        "the hidden game did not exit - restore by hand: delete sprites.dat and sprites.dat.2, rename $kept to sprites.dat"
    } else {
        Remove-Item (Join-Path $dir 'sprites.dat') -ErrorAction SilentlyContinue
        Remove-Item (Join-Path $dir 'sprites.dat.2') -ErrorAction SilentlyContinue
        Rename-Item $kept $dat
        'restored: ' + (Get-Item $dat).Length
    }
}
