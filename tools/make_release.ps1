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
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path (Join-Path $pkg '*') -DestinationPath $zip
Remove-Item -Recurse -Force $pkg

$hash = (Get-FileHash $zip -Algorithm SHA256).Hash
Write-Output ("{0}  {1:N0} bytes  SHA256 {2}" -f $zip, (Get-Item $zip).Length, $hash)
