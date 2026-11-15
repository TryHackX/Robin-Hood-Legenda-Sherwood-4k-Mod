<#
 stack_sample.ps1 - what the game is busy with during a hitch: sends one harness command to the
 background test instance (tools\htest.ps1) and right after that takes read-only snapshots of all
 threads of Game.exe (the code of tools\threaddump.ps1), one every -EveryMs for -Ms. Then counts,
 per thread, the code addresses (module!export+off / Game.exe address) seen in the snapshots: the
 ones present in most snapshots of the main thread are where the time goes.

   powershell -ExecutionPolicy Bypass -File tools\stack_sample.ps1 -Cmd "click" [-Ms 1500] [-EveryMs 60]

 Output: tools\out\stack_sample_<time>\sNN.txt (snapshots) and summary.txt.
#>
param([string]$Cmd = '', [int]$Ms = 1500, [int]$EveryMs = 60, [int]$StackBytes = 16384)

$root = Split-Path -Parent $PSScriptRoot
$p = Get-Process -Name Game -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p) { Write-Output 'No Game.exe running.'; exit 1 }

# the snapshot code of threaddump.ps1, compiled once
$text = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'threaddump.ps1'))
$a = $text.IndexOf("`$src = @'"); $b = $text.IndexOf("'@", $a + 10)
if ($a -lt 0 -or $b -lt 0) { Write-Output 'threaddump.ps1: snapshot code not found'; exit 1 }
$src = $text.Substring($a + 10, $b - $a - 10).Trim("`r", "`n")
Add-Type -TypeDefinition $src -Language CSharp

$outDir = Join-Path $root ('tools\out\stack_sample_' + (Get-Date -Format 'yyyyMMdd_HHmmss'))
New-Item -ItemType Directory -Force $outDir | Out-Null

if ($Cmd) {
    # the harness command, without waiting for its answer (tools\hctl.ps1 Send-One)
    $dir = Join-Path $root 'tools\out\harness'
    $seqFile = Join-Path $dir 'seq.txt'
    $seq = 1
    if (Test-Path $seqFile) { $seq = [int](Get-Content $seqFile | Select-Object -First 1) + 1 }
    Set-Content $seqFile $seq -Encoding ascii
    $fs = [IO.File]::Open((Join-Path $dir 'cmd.txt'), 'Append', 'Write', 'ReadWrite')
    $bytes = [Text.Encoding]::ASCII.GetBytes("$seq $Cmd`n")
    $fs.Write($bytes, 0, $bytes.Length)
    $fs.Close()
}

$sw = [Diagnostics.Stopwatch]::StartNew()
$k = 0
while ($sw.ElapsedMilliseconds -lt $Ms) {
    $t0 = $sw.ElapsedMilliseconds
    $snap = [ThreadDump]::Run($p.Id, $StackBytes, 0, '')
    [IO.File]::WriteAllText((Join-Path $outDir ('s{0:D2}.txt' -f $k)), "@$t0 ms`r`n" + $snap)
    $k++
    $left = $EveryMs - ($sw.ElapsedMilliseconds - $t0)
    if ($left -gt 0) { Start-Sleep -Milliseconds $left }
}
Write-Output "$k snapshots in $outDir"
