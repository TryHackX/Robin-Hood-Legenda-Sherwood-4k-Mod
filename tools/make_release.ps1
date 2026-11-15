# Packs the release archive: release\SherwoodEngineFix-v<Version>.zip with the built winmm.dll, the
# default settings (release\SherwoodFix.ini) and README.md. Build first (tools\deploy.ps1).
#   powershell -File tools\make_release.ps1 -Version 1.0.0
param([string]$Version = '1.0.0')

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$dll  = Join-Path $root 'src\build\Release\winmm.dll'
$pkg  = Join-Path $root 'release\package'
$zip  = Join-Path $root "release\SherwoodEngineFix-v$Version.zip"

if (-not (Test-Path $dll)) { throw "no build: $dll" }
if (Test-Path $pkg) { Remove-Item -Recurse -Force $pkg }
New-Item -ItemType Directory -Force $pkg | Out-Null
Copy-Item $dll $pkg
Copy-Item (Join-Path $root 'release\SherwoodFix.ini') $pkg
Copy-Item (Join-Path $root 'README.md') $pkg
# quality of the in-between frames of the HD pack (tools\hd\between_quality.py): small, belongs to the mod -
# between.q for the current store, between.v1.q for the first pack's store (the DLL takes the one that matches);
# between.t = the animation steps' lengths for the current store (tools\hd\between_motion.py)
$qdir = Join-Path $pkg 'hd\sprites\_between'
foreach ($name in 'between.q', 'between.v1.q', 'between.t') {
    $q = Join-Path $root "hd\sprites\_between\$name"
    if (-not (Test-Path $q)) { continue }
    New-Item -ItemType Directory -Force $qdir | Out-Null
    Copy-Item $q $qdir
}
# run-time frames (icons above heads, markers, light at night ...; tools\hd\upscale_icons.py): small, newer than the pack's
$icons = @(Get-ChildItem (Join-Path $root 'hd\sprites\_icons\icons.*') -ErrorAction SilentlyContinue)
if ($icons.Count -eq 2) {
    $idir = Join-Path $pkg 'hd\sprites\_icons'
    New-Item -ItemType Directory -Force $idir | Out-Null
    $icons | Copy-Item -Destination $idir
}
# HD fonts (tools\hd\upscale_fonts.py): small, belong to the mod
$fonts = @(Get-ChildItem (Join-Path $root 'hd\fonts\*.hdfont') -ErrorAction SilentlyContinue)
if ($fonts.Count -gt 0) {
    $fdir = Join-Path $pkg 'hd\fonts'
    New-Item -ItemType Directory -Force $fdir | Out-Null
    $fonts | Copy-Item -Destination $fdir
}
# HD pictures of menus and dialogs (buttons, the briefing's parchment, seals, portraits; tools\hd\upscale_menu_parts.py)
$parts = @(Get-ChildItem (Join-Path $root 'hd\menu_parts\*.hdp') -ErrorAction SilentlyContinue)
if ($parts.Count -gt 0) {
    $pdir = Join-Path $pkg 'hd\menu_parts'
    New-Item -ItemType Directory -Force $pdir | Out-Null
    $parts | Copy-Item -Destination $pdir
}
# aliases of the HD menu backgrounds of the HD pack (menus opened in a mission find them by these)
foreach ($d in @(Get-ChildItem (Join-Path $root 'hd\menu') -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -ne 'remake' })) {
    $al = @(Get-ChildItem (Join-Path $d.FullName '*.alias') -ErrorAction SilentlyContinue)
    if ($al.Count -eq 0) { continue }
    $adir = Join-Path $pkg ("hd\menu\" + $d.Name)
    New-Item -ItemType Directory -Force $adir | Out-Null
    $al | Copy-Item -Destination $adir
    # menu backgrounds made after HD pack 1 (1.1.8: the credits' forest, ~4 MB packed per variant)
    foreach ($key in @('2ef7a848bc09b885')) {
        $m = Join-Path $d.FullName "$key.mhd"
        if (Test-Path $m) { Copy-Item $m $adir }
    }
}
if (Test-Path $zip) { Remove-Item -Force $zip }
# entries named with forward slashes, as the ZIP format wants (Compress-Archive and ZipFile of Windows
# PowerShell 5.1 write "hd\sprites\...", which some unpackers take for one file name)
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$fs = [IO.File]::Open($zip, [IO.FileMode]::Create)
$za = New-Object IO.Compression.ZipArchive($fs, [IO.Compression.ZipArchiveMode]::Create)
foreach ($f in Get-ChildItem $pkg -Recurse -File) {
    $name = $f.FullName.Substring($pkg.Length + 1).Replace('\', '/')
    [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($za, $f.FullName, $name, [IO.Compression.CompressionLevel]::Optimal)
}
$za.Dispose()
$fs.Dispose()
Remove-Item -Recurse -Force $pkg

$hash = (Get-FileHash $zip -Algorithm SHA256).Hash
Write-Output ("{0}  {1:N0} bytes  SHA256 {2}" -f $zip, (Get-Item $zip).Length, $hash)
