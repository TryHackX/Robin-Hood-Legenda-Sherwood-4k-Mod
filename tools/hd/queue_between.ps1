# The in-between frame work that needs the graphics card, as one queue (2026-11-22, user: "na razie gram, wiec
# kolejkuj"). Everything waits while the game's window is open (the generator also pauses itself then).
#  1. the level's own animations (DATA\Animations: birds, ducks, hens, flags, smoke ...) -> store _between_anim:
#     first pass, last -> first loops, motion, quality (between.t / between.q)
#  2. a second try of the characters' pairs the first store rejected (big moves: falls, quick strikes, bending
#     down): three settings on a random sample of rejected pairs, the one with most usable frames on all of them
#     -> store _between_more, motion, quality
# Log: hd\work\queue_between.log
#   -Survey n     sample size of the settings test (default 1500)
#   -SkipAnim / -SkipRetry
param([int]$Survey = 1500, [switch]$SkipAnim, [switch]$SkipRetry)

$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Set-Location $root
$log = Join-Path $root 'hd\work\queue_between.log'
function Say([string]$t) { $l = '[{0}] {1}' -f (Get-Date -Format 'HH:mm:ss'), $t; Add-Content -Path $log -Value $l; Write-Output $l }
function Py([string[]]$a) { & python -I @a *>> $log; return $LASTEXITCODE }
function WaitGame {
    $said = $false
    while (Get-Process Game -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 }) {
        if (-not $said) { Say 'the game is open - waiting'; $said = $true }
        Start-Sleep -Seconds 30
    }
}

Say 'queue start'
if (-not $SkipAnim) {
    WaitGame
    Say 'level animations: first pass'
    [void](Py @('tools\hd\make_inbetweens.py', '--files', 'Animations/', '--store', '_between_anim', '--sharp', '--frames-from', '_common', 'dat2'))
    Say 'level animations: last -> first pairs of loops'
    [void](Py @('tools\hd\make_inbetweens.py', '--files', 'Animations/', '--store', '_between_anim', '--sharp', '--frames-from', '_common', 'dat2', '--loops'))
    Say 'level animations: motion'
    [void](Py @('tools\hd\between_motion.py', '--store', '_between_anim'))
    Say 'level animations: quality'
    [void](Py @('tools\hd\between_quality.py', '--store', '_between_anim'))
}
if (-not $SkipRetry) {
    $settings = @(
        @{ name = 'flow scales x2'; args = @('--rife-scales', '32,16,8,4,2') },
        @{ name = 'magnify 2'; args = @('--magnify', '2') },
        @{ name = 'magnify 2 + flow scales x2'; args = @('--magnify', '2', '--rife-scales', '32,16,8,4,2') }
    )
    $best = $null
    $bestOk = -1
    foreach ($s in $settings) {
        WaitGame
        Say "rejected pairs, test on $Survey of them: $($s.name)"
        $out = & python -I tools\hd\make_inbetweens.py --files Characters/ --rejected-from _between --store _between_survey --sharp --survey $Survey @($s.args) 2>&1 | ForEach-Object { "$_" }
        $out | Add-Content -Path $log
        $done = $out | Select-String 'done: (\d+) pairs.* (\d+) usable' | Select-Object -Last 1
        if ($done) {
            $ok = [int]$done.Matches[0].Groups[2].Value
            Say ("  {0}: {1} usable of {2}" -f $s.name, $ok, $done.Matches[0].Groups[1].Value)
            if ($ok -gt $bestOk) { $bestOk = $ok; $best = $s }
        }
    }
    Remove-Item (Join-Path $root 'hd\sprites\_between_survey') -Recurse -Force -ErrorAction SilentlyContinue
    if ($best -and $bestOk -gt 0) {
        WaitGame
        Say "rejected pairs, all of them: $($best.name)"
        [void](Py (@('tools\hd\make_inbetweens.py', '--files', 'Characters/', '--rejected-from', '_between', '--store', '_between_more', '--sharp') + $best.args))
        Say 'second try: motion'
        [void](Py @('tools\hd\between_motion.py', '--store', '_between_more'))
        Say 'second try: quality'
        [void](Py @('tools\hd\between_quality.py', '--store', '_between_more'))
    } else {
        Say 'no setting gave usable frames - nothing more'
    }
}
Say 'queue done'
