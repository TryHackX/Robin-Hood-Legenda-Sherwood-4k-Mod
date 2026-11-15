# Screenshots of the main menu screens as the player sees them (HD output, buttons and texts in place):
# the layout references for remade menu backgrounds (tools\hd\export_menu_remake.py).
# The hidden test game must be running in the main menu (tools\htest.ps1 -MenuOnly).
# Coordinates are the game's frame pixels (2560x1440, menu zoom on), as in tools\collect_menus.ps1.
# Only screens are opened and left with their back / cancel buttons: no setting is changed.
param([int]$WaitMs = 2000, [string]$OutName = 'menus_layout')

$hctl = Join-Path $PSScriptRoot 'hctl.ps1'
$out = Join-Path $PSScriptRoot "out\$OutName"
New-Item -ItemType Directory -Force $out | Out-Null
function Send-Game([string]$c, [int]$wait = 0, [string]$shot = '') {
    $a = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $hctl, '-Cmd', $c)
    if ($wait -gt 0) { $a += @('-Wait', $wait) }
    if ($shot) { $a += @('-ShotOut', "$OutName\$shot", '-MaxWidth', '2560') }
    (powershell @a | Select-Object -Last 1)
}
$x = 1514
$park = 'moveto 2400 1380'      # the cursor out of the picture
Send-Game $park $WaitMs 'main' | Out-Null
Send-Game "moveto $x 815; click; $park" $WaitMs 'options' | Out-Null          # Opcje
Send-Game "moveto $x 816; click; $park" $WaitMs 'graphics' | Out-Null         # Grafika
Send-Game "moveto $x 939; click" $WaitMs | Out-Null                           # Anuluj
Send-Game "moveto $x 940; click" $WaitMs | Out-Null                           # Wstecz
Send-Game "moveto $x 734; click; $park" $WaitMs 'load' | Out-Null             # Wczytaj
Send-Game "moveto $x 939; click" $WaitMs | Out-Null                           # Anuluj
Send-Game "moveto $x 856; click; $park" $WaitMs 'films' | Out-Null            # Wyswietl filmy
Send-Game 'key 0x01' $WaitMs | Out-Null
Send-Game "$park; menuhd" $WaitMs 'main_again' | Out-Null
Get-ChildItem $out -File | Select-Object Name, Length | Format-Table -AutoSize | Out-String
