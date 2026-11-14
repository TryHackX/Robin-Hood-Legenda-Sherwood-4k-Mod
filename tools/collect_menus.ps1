# Walks through the main menu screens of the hidden test game (tools\htest.ps1 -MenuOnly must be
# running) so that the mod writes every composed menu picture to hd\work\menu_frames\ (the input for
# tools\hd\upscale_menu.py). Screenshots of each screen go to tools\out\menus\.
# Coordinates are the game's frame pixels (2560x1440, menu zoom on).
param([int]$WaitMs = 1800)

$hctl = Join-Path $PSScriptRoot 'hctl.ps1'
$out = Join-Path $PSScriptRoot 'out\menus'
New-Item -ItemType Directory -Force $out | Out-Null
function Send-Game([string]$c, [int]$wait = 0, [string]$shot = '') {
    $a = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $hctl, '-Cmd', $c)
    if ($wait -gt 0) { $a += @('-Wait', $wait) }
    if ($shot) { $a += @('-ShotOut', "menus\$shot", '-MaxWidth', '960') }
    (powershell @a | Select-Object -Last 1)
}
function Visit([string]$name, [string]$open, [string]$back) {
    Send-Game "moveto $open; click" $WaitMs $name | Out-Null
    Write-Output ("{0,-14} {1}" -f $name, (Send-Game 'menuhd'))
    if ($back) { Send-Game "moveto $back; click" $WaitMs | Out-Null }
}

$x = 1514
Write-Output ("{0,-14} {1}" -f 'main', (Send-Game 'menuhd' 600 'main'))
Visit 'options'   "$x 815" ''
Visit 'graphics'  "$x 816" "$x 939"      # Grafika -> Anuluj
Visit 'sounds'    "$x 856" "$x 939"      # Dzwieki -> Anuluj/Wstecz (bottom button)
Visit 'keyboard'  "$x 898" "$x 939"      # Klawiatura -> Anuluj
Send-Game "moveto $x 940; click" $WaitMs | Out-Null   # options: Wstecz
Visit 'load'      "$x 734" "$x 939"      # Wczytaj -> Anuluj
Visit 'player'    "$x 774" ''            # Wybierz gracza (back by Esc below)
Send-Game 'key 0x01' $WaitMs | Out-Null
Visit 'films'     "$x 856" ''
Send-Game 'key 0x01' $WaitMs | Out-Null
Visit 'credits'   "$x 898" ''
Send-Game 'key 0x01' $WaitMs | Out-Null
Write-Output ("{0,-14} {1}" -f 'end', (Send-Game 'menuhd' 600 'end'))
Get-ChildItem (Join-Path (Split-Path $PSScriptRoot -Parent) 'hd\work\menu_frames') -Filter *.mnf -ErrorAction SilentlyContinue |
    Select-Object Name, Length | Format-Table -AutoSize | Out-String
