# Builds winmm.dll (Release, Win32) and copies it into the game folder.
# Refuses to deploy while Game.exe is running (the DLL is locked and the log would be lost).
#   powershell -File tools\deploy.ps1            build + deploy
#   powershell -File tools\deploy.ps1 -NoBuild   deploy the last build only
#   powershell -File tools\deploy.ps1 -ReplaceInUse   also deploy when a windowless leftover Game.exe holds the DLL
param([switch]$NoBuild, [switch]$ReplaceInUse)

$ErrorActionPreference = 'Stop'
$root  = Split-Path -Parent $PSScriptRoot
$cmake = 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$built = Join-Path $root 'src\build\Release\winmm.dll'
$live  = Join-Path $root 'winmm.dll'
$log   = Join-Path $root 'SherwoodFix.log'

if (-not $NoBuild) {
    # cmake prints harmless warnings on stderr; under 'Stop' PowerShell 5.1 would abort on them
    $ErrorActionPreference = 'Continue'
    $out = & $cmake --build (Join-Path $root 'src\build') --config Release 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    $out | Where-Object { $_ -match 'error|warning C|FAILED|LNK' } | Select-Object -First 40
    if ($code -ne 0) { Write-Output 'BUILD FAILED'; exit 1 }
}

$running = @(Get-Process -Name Game -ErrorAction SilentlyContinue)
$swap = $false
if ($running.Count -gt 0) {
    # A Game.exe without any window is a leftover that failed to exit; it only keeps the old DLL
    # mapped. Windows allows renaming a mapped DLL, so with -ReplaceInUse the old file is moved
    # aside and the new one takes its name. A game that has a window is never touched.
    $withWindow = @($running | Where-Object { $_.MainWindowHandle -ne 0 })
    if (-not $ReplaceInUse -or $withWindow.Count -gt 0) {
        Write-Output 'Game.exe is running - not deploying.'
        exit 2
    }
    $swap = $true
}

# keep the log of the previous run before the next start overwrites it
if (Test-Path $log) {
    $stamp = (Get-Item $log).LastWriteTime.ToString('yyyy-MM-dd_HHmmss')
    $keep  = Join-Path $root "backups\SherwoodFix_$stamp.log"
    if (-not (Test-Path $keep)) { Copy-Item $log $keep }
}

if ((Get-FileHash $built).Hash -ne (Get-FileHash $live).Hash) {
    if ($swap) {
        $aside = Join-Path $root ("backups\winmm_inuse_{0}.dll" -f (Get-Date -Format 'yyyy-MM-dd_HHmmss'))
        Move-Item $live $aside
        Copy-Item $built $live
        Write-Output ("deployed winmm.dll ({0} bytes); the copy still mapped by the leftover Game.exe is now {1}" -f (Get-Item $live).Length, $aside)
        exit 0
    }
    Copy-Item $built $live -Force
    Write-Output ("deployed winmm.dll ({0} bytes)" -f (Get-Item $live).Length)
} else {
    Write-Output 'winmm.dll already up to date.'
}
