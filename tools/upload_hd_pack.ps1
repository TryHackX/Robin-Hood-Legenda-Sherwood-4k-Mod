# Uploads the HD graphics packs (release\hdpack\SherwoodHD-*.zip, made by tools\make_hd_packs.py)
# as the GitHub release "hd-pack-1" (not marked "Latest": the mod's own release stays the latest).
# Needs GitHub CLI logged in:  tools\gh\cli\bin\gh.exe auth login --web
#   powershell -File tools\upload_hd_pack.ps1                 upload now
#   powershell -File tools\upload_hd_pack.ps1 -WaitForLogin   wait (up to 3 h) until the login is done
param([switch]$WaitForLogin, [string]$Tag = 'hd-pack-1')

$ErrorActionPreference = 'Stop'
$root  = Split-Path $PSScriptRoot -Parent
$gh    = Join-Path $root 'tools\gh\cli\bin\gh.exe'
$repo  = 'TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod'
$notes = Join-Path $root "release\RELEASE_NOTES_$Tag.md"
$files = @(Get-ChildItem (Join-Path $root 'release\hdpack') -Filter 'SherwoodHD-*.zip' | Sort-Object Name)
if ($files.Count -ne 7) { throw "expected 7 packs in release\hdpack, found $($files.Count)" }
foreach ($f in $files) { if ($f.Length -ge 2GB) { throw "$($f.Name) is not under 2 GB" } }

$deadline = (Get-Date).AddHours(3)
while ($true) {
    $ErrorActionPreference = 'Continue'
    & $gh auth status --hostname github.com *> $null
    $ok = $LASTEXITCODE -eq 0
    $ErrorActionPreference = 'Stop'
    if ($ok) { break }
    if (-not $WaitForLogin -or (Get-Date) -gt $deadline) { throw 'GitHub CLI is not logged in (gh auth login --web)' }
    Write-Output ('{0:HH:mm:ss} waiting for the GitHub CLI login (other terminal tab)...' -f (Get-Date))
    Start-Sleep -Seconds 20
}
Write-Output 'GitHub CLI logged in.'

$ErrorActionPreference = 'Continue'
$env:GCM_INTERACTIVE = 'never'
git -C $root rev-parse -q --verify "refs/tags/$Tag" *> $null
if ($LASTEXITCODE -ne 0) {
    git -C $root tag -a $Tag -m 'HD graphics pack 1'
    git -C $root push origin $Tag
}
& $gh release view $Tag --repo $repo *> $null
if ($LASTEXITCODE -ne 0) {
    & $gh release create $Tag --repo $repo --title 'HD graphics pack 1' --notes-file $notes --latest=false
    if ($LASTEXITCODE -ne 0) { throw 'could not create the release' }
}
foreach ($f in $files) {
    Write-Output ('{0:HH:mm:ss} uploading {1} ({2:N0} MB)...' -f (Get-Date), $f.Name, ($f.Length / 1MB))
    & $gh release upload $Tag $f.FullName --repo $repo --clobber
    if ($LASTEXITCODE -ne 0) { throw "upload failed: $($f.Name) - run the script again to continue" }
}
Write-Output ('{0:HH:mm:ss} done: https://github.com/{1}/releases/tag/{2}' -f (Get-Date), $repo, $Tag)
