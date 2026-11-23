# Background test on a chosen save. The hidden game starts at the main menu, "Graj!" continues the profile
# (a Continue point opens with the pause menu: "Kontynuuj"), then the given harness steps run and their shots
# become PNGs in tools\out\. Afterwards the player's saves are put back from the latest backup (make one first:
# tools\save_backup.ps1) and the instance ends. Nothing appears on the screen.
#   -Save path     a save file (its _t file next to it), e.g. a YouTuber's Savegame_104
#   -Profile dir   another player's Savegame folder (Profiles + Profile_NNN): it replaces the player's for the run
#                  and -Save becomes that profile's QuickSave - load it in -Steps with the game's quick load key
#                  (F5 = "key 0x3F"). Without -Profile, -Save goes in as the player's Continue - but the game does
#                  not load a foreign Continue: it starts the player's mission from its Restart point instead.
#   -Name n        name of the log extract tools\out\harness\<n>.txt
#   -Steps "a; b"  harness commands, e.g. "key 0x3F; sleep 8000; shotout f_view; hold 0x2A; shotout f_shift"
#                  ("sleep MS" waits here)
#   -Ini file      settings (default tools\testcfg\user_copy.ini)
#   -NoPlay        the steps run at the main menu (no "Graj!"), e.g. to open "Wczytaj gre"
param([string]$Save = '', [string]$Profile = '', [string]$Name = 'save', [string]$Steps = '', [string]$Ini = '', [switch]$NoPlay)

$root = Split-Path $PSScriptRoot -Parent
if (-not $Ini) { $Ini = Join-Path $PSScriptRoot 'testcfg\user_copy.ini' }
$hctl = Join-Path $PSScriptRoot 'hctl.ps1'
$log = Join-Path $root 'SherwoodFix.log'
$saveRoot = Join-Path $root 'DATA\Savegame'
function Send-H([string]$c) { (powershell -NoProfile -ExecutionPolicy Bypass -File $hctl -Cmd $c | Select-Object -Last 1) -replace '^.*? -> ', '' }

if (Get-Process Game -ErrorAction SilentlyContinue) { Write-Output 'RESULT: Game.exe is running - not starting a test'; exit 2 }
if ($Save -and (-not (Test-Path $Save) -or -not (Test-Path "${Save}_t"))) { Write-Output "RESULT: no save $Save (and ${Save}_t)"; exit 2 }
if ((powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'save_backup.ps1') -Check) -notmatch 'unchanged') {
    Write-Output 'RESULT: the saves differ from the backup - make a new backup or restore first'; exit 2
}

$added = @()
if ($Profile) {
    foreach ($d in Get-ChildItem $Profile -Directory) {
        if (Test-Path (Join-Path $saveRoot $d.Name)) { Write-Output "RESULT: $($d.Name) exists among the player's saves"; exit 2 }
    }
    Copy-Item (Join-Path $Profile 'Profiles') (Join-Path $saveRoot 'Profiles') -Force
    foreach ($d in Get-ChildItem $Profile -Directory) {
        $dst = Join-Path $saveRoot $d.Name
        Copy-Item $d.FullName $dst -Recurse
        $added += $dst
        if ($Save) {
            Copy-Item $Save (Join-Path $dst 'QuickSave') -Force
            Copy-Item "${Save}_t" (Join-Path $dst 'QuickSave_t') -Force
        }
    }
} elseif ($Save) {
    Copy-Item $Save (Join-Path $saveRoot 'Profile_001\Continue') -Force
    Copy-Item "${Save}_t" (Join-Path $saveRoot 'Profile_001\Continue_t') -Force
}
$startLine = 0
$shots = @()
try {
    $start = powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'htest.ps1') -Ini $Ini -MenuOnly -NoSaveBackup
    if (($start | Select-Object -Last 1) -notmatch '^menu: ok') { Write-Output "RESULT: no menu: $($start | Select-Object -Last 1)"; exit 1 }
    if (Test-Path $log) { $startLine = (Get-Content $log).Count }
    if (-not $NoPlay) {
        [void](Send-H 'moveto 1514 693'); [void](Send-H 'click')                 # "Graj!"
        $ok = $false
        foreach ($i in 1..80) { Start-Sleep -Milliseconds 500; if ((Send-H 'stat') -match 'engine=1 map=[1-9]') { $ok = $true; break } }
        if (-not $ok) { Write-Output 'RESULT: the mission did not load'; exit 1 }
        Start-Sleep -Seconds 2
        Write-Output "loaded: $((Send-H 'stat') -replace ' desktopBlocked.*', '')"
    }
    foreach ($c in ($Steps -split ';')) {
        $c = $c.Trim()
        if (-not $c) { continue }
        if ($c -match '^sleep (\d+)$') { Start-Sleep -Milliseconds ([int]$matches[1]); continue }
        if ($c -match '^shot(out)? (\S+)$') { $shots += $matches[2] }
        Write-Output "$c -> $((Send-H $c) -replace ' desktopBlocked.*', '')"
    }
} finally {
    [void](Send-H 'quit')
    Start-Sleep -Seconds 4
    Get-Process Game -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -eq 0 } | Stop-Process -Force -ErrorAction SilentlyContinue
    foreach ($d in $added) { Remove-Item $d -Recurse -Force }
    powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'save_backup.ps1') -Restore -All | Out-Null
    Write-Output "saves: $(powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'save_backup.ps1') -Check)"
    foreach ($s in $shots) {
        $bmp = Join-Path $root "tools\out\harness\$s.bmp"
        if (-not (Test-Path $bmp)) { continue }
        python -c "from PIL import Image; Image.open(r'$bmp').save(r'$(Join-Path $root "tools\out\$s.png")')"
        Remove-Item $bmp
    }
    if (Test-Path $log) {
        $out = Join-Path $root "tools\out\harness\$Name.txt"
        Get-Content $log | Select-Object -Skip $startLine | Where-Object { $_ -match 'HD menu|ViewZoom|dialog|WARN|ERROR|HD:|Level|level' } |
            Set-Content -Path $out -Encoding ascii
        Write-Output "log lines: $((Get-Content $out).Count) -> $out"
    }
}
