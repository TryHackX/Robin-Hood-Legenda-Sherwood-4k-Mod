# Packs the remade menu backgrounds (hd\menu\remake\*.mhd + *.alias, made by tools\hd\menu_remake.py import) as an
# optional download: release\SherwoodEngineFix-menu-remake-<Name>.zip - unpacked into the game folder it adds
# hd\menu\remake\; the mod shows those backgrounds with HDMenuRemake=1 (F10: "MENU W HD" -> "TAK, TLA Z REMAKE GDY SA").
#   powershell -File tools\make_remake_pack.ps1 -Name 1
param([string]$Name = '1')

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$src  = Join-Path $root 'hd\menu\remake'
$zip  = Join-Path $root "release\SherwoodEngineFix-menu-remake-$Name.zip"

$files = @(Get-ChildItem $src -File | Where-Object { $_.Extension -in '.mhd', '.alias' })
if (-not ($files | Where-Object { $_.Extension -eq '.mhd' })) { throw "no remade backgrounds in $src (tools\hd\menu_remake.py import)" }
if (Test-Path $zip) { Remove-Item -Force $zip }
# entries named with forward slashes (see make_release.ps1)
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$fs = [IO.File]::Open($zip, [IO.FileMode]::Create)
$za = New-Object IO.Compression.ZipArchive($fs, [IO.Compression.ZipArchiveMode]::Create)
foreach ($f in $files) {
    [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($za, $f.FullName, "hd/menu/remake/$($f.Name)",
                                                                  [IO.Compression.CompressionLevel]::Optimal)
}
$za.Dispose()
$fs.Dispose()
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash
Write-Output ("{0}  {1} files  {2:N0} bytes  SHA256 {3}" -f $zip, $files.Count, (Get-Item $zip).Length, $hash)
