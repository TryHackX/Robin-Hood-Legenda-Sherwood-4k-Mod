# The hidden test instance plays on the user's profile. Before a test that may leave a mission:
#   tools\save_backup.ps1            copy DATA\Savegame to backups\savegame_before_test_<time>
#                                    (path kept in tools\out\last_save_backup.txt)
# After the test:
#   tools\save_backup.ps1 -Check     list what changed against that copy
#   tools\save_backup.ps1 -Restore   put back changed or new files except Restart* (the game writes
#                                    those at every mission start) - the user's Continue point and
#                                    Profiles come back as they were
param([switch]$Check, [switch]$Restore)

$root = Split-Path -Parent $PSScriptRoot
$saves = Join-Path $root 'DATA\Savegame'
$note = Join-Path $root 'tools\out\last_save_backup.txt'

if (-not $Check -and -not $Restore) {
    $dst = Join-Path $root ('backups\savegame_before_test_' + (Get-Date -Format 'yyyy-MM-dd_HHmmss'))
    New-Item -ItemType Directory -Force $dst | Out-Null
    Copy-Item -Path (Join-Path $saves '*') -Destination $dst -Recurse -Force
    Set-Content -Path $note -Value ('backups\' + (Split-Path -Leaf $dst)) -Encoding ascii
    Write-Output "backup: $dst"
    exit 0
}

$rel = (Get-Content $note -ErrorAction Stop | Select-Object -First 1).Trim()
$bak = Join-Path $root $rel
if (-not (Test-Path $bak)) { Write-Output "no backup at $bak"; exit 1 }
$base = (Resolve-Path $saves).Path
$changed = @()
Get-ChildItem $saves -Recurse -File | ForEach-Object {
    $r = $_.FullName.Substring($base.Length).TrimStart('\')
    $o = Join-Path $bak $r
    if (-not (Test-Path $o)) { $changed += [pscustomobject]@{ Kind = 'NEW'; Path = $r } }
    elseif ((Get-FileHash $_.FullName).Hash -ne (Get-FileHash $o).Hash) { $changed += [pscustomobject]@{ Kind = 'CHG'; Path = $r } }
}
if (-not $changed) { Write-Output "saves unchanged against $rel"; exit 0 }
foreach ($c in $changed) {
    $leaf = Split-Path -Leaf $c.Path
    if ($Restore -and $leaf -notlike 'Restart*') {
        if ($c.Kind -eq 'CHG') {
            Copy-Item -Path (Join-Path $bak $c.Path) -Destination (Join-Path $saves $c.Path) -Force
            Write-Output "restored $($c.Path)"
        } else {
            Write-Output "new file kept (not in the copy): $($c.Path)"
        }
    } else {
        Write-Output "$($c.Kind) $($c.Path)"
    }
}
