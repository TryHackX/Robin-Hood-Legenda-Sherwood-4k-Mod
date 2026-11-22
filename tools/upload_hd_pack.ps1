# Uploads the HD graphics packs (release\hdpack\SherwoodHD-*.zip, made by tools\make_hd_packs.py)
# as the GitHub release "hd-pack-1" (not marked "Latest": the mod's own release stays the latest).
# Needs GitHub CLI logged in:  tools\gh\cli\bin\gh.exe auth login --web
#   powershell -File tools\upload_hd_pack.ps1                 upload now (all packs)
#   powershell -File tools\upload_hd_pack.ps1 -Only SherwoodHD-5-mapy-4k-a.zip,SherwoodHD-5-mapy-4k-b.zip
#                                                             only these (new packs), the release notes updated too
#   powershell -File tools\upload_hd_pack.ps1 -WaitForLogin   wait (up to 3 h) until the login is done
param([switch]$WaitForLogin, [string]$Tag = 'hd-pack-1', [string[]]$Only = @())

$ErrorActionPreference = 'Stop'
$root  = Split-Path $PSScriptRoot -Parent
$gh    = Join-Path $root 'tools\gh\cli\bin\gh.exe'
$repo  = 'TryHackX/Robin-Hood-Legenda-Sherwood-4k-Mod'
$notes = Join-Path $root "release\RELEASE_NOTES_$Tag.md"
$files = @(Get-ChildItem (Join-Path $root 'release\hdpack') -Filter 'SherwoodHD-*.zip' | Sort-Object Name)
# powershell -File passes "a,b" as one string
$Only = @($Only | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
if ($Only.Count -gt 0) {
    $files = @($files | Where-Object { $Only -contains $_.Name })
    if ($files.Count -ne $Only.Count) { throw "not all of $($Only -join ', ') are in release\hdpack" }
} elseif ($files.Count -lt 7) {
    throw "expected at least 7 packs in release\hdpack, found $($files.Count)"
}
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
} elseif ($Only.Count -gt 0) {
    & $gh release edit $Tag --repo $repo --notes-file $notes
    if ($LASTEXITCODE -ne 0) { throw 'could not update the release notes' }
}
foreach ($f in $files) {
    Write-Output ('{0:HH:mm:ss} uploading {1} ({2:N0} MB)...' -f (Get-Date), $f.Name, ($f.Length / 1MB))
    & $gh release upload $Tag $f.FullName --repo $repo --clobber
    if ($LASTEXITCODE -ne 0) { throw "upload failed: $($f.Name) - run the script again to continue" }
}
Write-Output ('{0:HH:mm:ss} done: https://github.com/{1}/releases/tag/{2}' -f (Get-Date), $repo, $Tag)
