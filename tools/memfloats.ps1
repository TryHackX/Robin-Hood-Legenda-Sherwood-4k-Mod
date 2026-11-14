# Shows memory of the hidden test game as floats and hex words (tools\hctl.ps1 "mem").
#   powershell -File tools\memfloats.ps1 -Addr 0x116B4BC0 [-Len 64]
param([Parameter(Mandatory = $true)][string]$Addr, [int]$Len = 64)

$hctl = Join-Path $PSScriptRoot 'hctl.ps1'
$base = [Convert]::ToUInt32(($Addr -replace '^0x', ''), 16)
$reply = powershell -NoProfile -ExecutionPolicy Bypass -File $hctl -Cmd ("mem 0x{0:X8} {1}" -f $base, $Len) | Select-Object -Last 1
if ($reply -notmatch ' -> ok (.*)$') { Write-Output "no answer / error: $reply"; exit 1 }
$bytes = [byte[]](($Matches[1].Trim() -split '\s+') | Where-Object { $_ -match '^[0-9A-Fa-f]{2}$' } | ForEach-Object { [Convert]::ToByte($_, 16) })
for ($i = 0; $i + 4 -le $bytes.Length; $i += 4) {
    "{0:X8}: {1,14} | {2:X8}" -f ($base + $i), [BitConverter]::ToSingle($bytes, $i), [BitConverter]::ToUInt32($bytes, $i)
}
