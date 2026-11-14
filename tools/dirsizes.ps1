# Sizes of the entries of some folders (MB) - to decide what goes into a release.
#   powershell -File tools\dirsizes.ps1 tools src docs
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Dirs)

$root = Split-Path $PSScriptRoot -Parent
foreach ($d in $Dirs) {
    Get-ChildItem (Join-Path $root $d) -Force | ForEach-Object {
        $mb = if ($_.PSIsContainer) {
            ((Get-ChildItem $_.FullName -Recurse -File -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum) / 1MB
        } else { $_.Length / 1MB }
        "{0,-50} {1,10:N2} MB" -f "$d\$($_.Name)", $mb
    }
}
