# Remote control for automated in-game tests (synthetic input goes through SendInput, which
# DirectInput sees like real devices). Input is only sent while Game.exe owns the foreground window.
#
#   -Start                 launch Game.exe without UAC (RunAsInvoker), prints the PID
#   -Start -Headless       launch it in background test mode: invisible, silent, driven by tools\hctl.ps1
#                          (no focus, no synthetic system input - usable while someone works on the PC)
#   -Start -Quiet          an ordinary visible run, but without sound (for checks at night)
#   -Start -Foreground     let the game put its own window in front, as in a start by the player
#                          (only when the user has been idle for -MinIdle seconds)
#   -Shot <name>           screenshot of the primary monitor -> tools\out\<name>.png (1280x720)
#   -MoveX <dx> -MoveY <dy> relative mouse move (split into small steps)
#   -Click                 left click        -RClick   right click
#   -Key <scancode>        key press, e.g. 0x01 Esc, 0x1C Enter, 0x39 Space; arrows 0x48/0x50/0x4B/0x4D with -Ext
#   -HoldMs <n>            how long -Key is held (default 60)      -Ext   extended key (arrows)
#   -Wait <ms>             sleep afterwards
#   -Focus                 bring the game window to the front; refused unless the user has been idle
#                          for -MinIdle seconds (default 120). Remembers the window that had the focus.
#   -Unfocus               give the focus back to that window (an "Alt+Tab away")
#   -ClickToFront          activate the game the way a player does: one left click, sent ONLY if the
#                          window under the mouse pointer is the game's and the user has been idle
#   -AltTab [-AltHoldMs n] press a real Alt+Tab (Alt stays down n ms after Tab, default 500)
#   -Close                 close the game the way Alt+F4 does and report whether the process ended
#   -Kill                  terminate Game.exe
#   -Status                is the game running / foreground
# Several actions may be combined; they run in the order Start, Move, Click, RClick, Key, Wait, Shot, Close, Kill.
param(
    [switch]$Start, [string]$Shot, [int]$MoveX = 0, [int]$MoveY = 0, [switch]$Click, [switch]$RClick,
    [int]$Key = -1, [int]$HoldMs = 60, [switch]$Ext, [int]$Wait = 0, [switch]$Kill, [switch]$Status, [switch]$Close,
    [switch]$Focus, [switch]$Unfocus, [int]$MinIdle = 120, [switch]$AltTab, [int]$AltHoldMs = 500, [switch]$Cancel,
    [switch]$IgnoreLeftover, [switch]$Headless, [switch]$Quiet, [switch]$Foreground, [switch]$ClickToFront, [string]$Ini = ''
)

$root = Split-Path -Parent $PSScriptRoot
Add-Type -AssemblyName System.Drawing, System.Windows.Forms
Add-Type -Namespace Native -Name U32 -MemberDefinition @'
[DllImport("user32.dll")] public static extern void mouse_event(uint flags, int dx, int dy, uint data, System.UIntPtr extra);
[DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, System.UIntPtr extra);
[DllImport("user32.dll")] public static extern System.IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(System.IntPtr hWnd, out uint pid);
[DllImport("user32.dll")] public static extern bool PostMessageA(System.IntPtr hWnd, uint msg, System.UIntPtr wParam, System.IntPtr lParam);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(System.IntPtr hWnd);
[StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
[DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
[DllImport("user32.dll")] public static extern System.IntPtr WindowFromPoint(POINT p);
[DllImport("user32.dll")] public static extern System.IntPtr GetAncestor(System.IntPtr hWnd, uint flags);
[DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
[DllImport("user32.dll")] public static extern bool GetWindowRect(System.IntPtr hWnd, out RECT r);
[StructLayout(LayoutKind.Sequential)] public struct LASTINPUTINFO { public uint cbSize; public uint dwTime; }
[DllImport("user32.dll")] public static extern bool GetLastInputInfo(ref LASTINPUTINFO p);
[DllImport("kernel32.dll")] public static extern uint GetTickCount();
'@

function Test-GameForeground {
    $h = [Native.U32]::GetForegroundWindow()
    $procId = 0
    [void][Native.U32]::GetWindowThreadProcessId($h, [ref]$procId)
    $g = Get-Process -Name Game -ErrorAction SilentlyContinue
    return ($g -and ($g.Id -contains [int]$procId))
}

function Assert-Foreground {
    if (-not (Test-GameForeground)) {
        Write-Output 'ABORT: Game.exe is not the foreground window - no input sent.'
        exit 3
    }
}

if ($Start) {
    # a Game.exe without a window that has been around for a while is a leftover, not a running game
    $live = @(Get-Process -Name Game -ErrorAction SilentlyContinue | Where-Object {
        $_.MainWindowHandle -ne 0 -or ((Get-Date) - $_.StartTime).TotalSeconds -lt 30 })
    # a background test instance has no window on purpose; it announces itself with a named event
    $testAlive = $false
    $ev = $null
    if ([Threading.EventWaitHandle]::TryOpenExisting('Local\SherwoodFixTestInstance', [ref]$ev)) { $testAlive = $true; $ev.Close() }
    if ($live.Count -gt 0 -or $testAlive) {
        Write-Output ("Game.exe already running{0}." -f $(if ($testAlive) { ' (background test instance)' } else { '' }))
    } else {
        $env:__COMPAT_LAYER = 'RunAsInvoker DWM8And16BitMitigation 8And16BitAggregateBlts HighDpiAware'
        # a scripted start never pushes the game in front of the user by itself, unless -Foreground
        # is given and nobody has touched the PC for a while
        $mayFront = $false
        if ($Foreground) {
            $li0 = New-Object Native.U32+LASTINPUTINFO
            $li0.cbSize = 8
            [void][Native.U32]::GetLastInputInfo([ref]$li0)
            $idle0 = ([Native.U32]::GetTickCount() - $li0.dwTime) / 1000.0
            # input injected by this script is not the user (same rule as -Focus)
            $mine0 = Join-Path $root 'tools\out\last_injected_tick.txt'
            if (Test-Path $mine0) {
                $mineTick = [uint32](Get-Content $mine0 | Select-Object -First 1)
                if ($li0.dwTime -le ($mineTick + 200)) { $idle0 = [Math]::Max($idle0, [double]$MinIdle) }
            }
            $mayFront = $idle0 -ge $MinIdle
            if (-not $mayFront) { Write-Output ("-Foreground ignored: user idle only {0:F0} s" -f $idle0) }
        }
        if (-not $mayFront) { $env:SHERWOODFIX_NO_FOREGROUND = '1' }
        # -Headless: background test mode - invisible window, no sound, input from tools\hctl.ps1
        if ($Headless) { $env:SHERWOODFIX_TEST = '1' }
        if ($Quiet) { $env:SHERWOODFIX_QUIET = '1' }
        # -Ini file: settings for this run only (the player's SherwoodFix.ini stays untouched)
        if ($Ini) { $env:SHERWOODFIX_INI = (Resolve-Path $Ini).Path }
        # -IgnoreLeftover: start even if an (elevated) leftover Game.exe cannot be ended from here;
        # menus work, a mission will crash because the sprite bank stays locked.
        if ($IgnoreLeftover) { $env:SHERWOODFIX_IGNORE_LEFTOVER = '1' }
        $p = Start-Process -FilePath (Join-Path $root 'Game.exe') -WorkingDirectory $root -PassThru
        Remove-Item Env:\__COMPAT_LAYER -ErrorAction SilentlyContinue
        Remove-Item Env:\SHERWOODFIX_IGNORE_LEFTOVER -ErrorAction SilentlyContinue
        Remove-Item Env:\SHERWOODFIX_NO_FOREGROUND -ErrorAction SilentlyContinue
        Remove-Item Env:\SHERWOODFIX_TEST -ErrorAction SilentlyContinue
        Remove-Item Env:\SHERWOODFIX_QUIET -ErrorAction SilentlyContinue
        Remove-Item Env:\SHERWOODFIX_INI -ErrorAction SilentlyContinue
        Write-Output "started PID=$($p.Id)"
        if ($mayFront) {
            # the mod presses Alt (and may minimise/restore) while it takes the foreground, a few seconds
            # after the start: that is our input, not the user's
            Start-Sleep -Seconds 6
            New-Item -ItemType Directory -Force (Join-Path $root 'tools\out') | Out-Null
            Set-Content -Path (Join-Path $root 'tools\out\last_injected_tick.txt') -Value ([Native.U32]::GetTickCount()) -Encoding ascii
        }
    }
}

$prevFile = Join-Path $root 'tools\out\prev_foreground.txt'
$mineFile = Join-Path $root 'tools\out\last_injected_tick.txt'
function Save-InjectTime {
    New-Item -ItemType Directory -Force (Split-Path $mineFile) | Out-Null
    Set-Content -Path $mineFile -Value ([Native.U32]::GetTickCount()) -Encoding ascii
}

if ($Focus) {
    # Only when nobody is at the keyboard: taking the foreground away from a working user is rude.
    $li = New-Object Native.U32+LASTINPUTINFO
    $li.cbSize = 8
    [void][Native.U32]::GetLastInputInfo([ref]$li)
    $idle = ([Native.U32]::GetTickCount() - $li.dwTime) / 1000.0
    # input injected by this script also counts as "last input"; if nothing newer than our own
    # last injection exists, the user has still not touched anything
    if (Test-Path $mineFile) {
        $mine = [uint32](Get-Content $mineFile | Select-Object -First 1)
        if ($li.dwTime -le ($mine + 200)) { $idle = [Math]::Max($idle, [double]$MinIdle) }
    }
    $t = @(Get-Process -Name Game -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 }) | Select-Object -First 1
    if (-not $t) {
        Write-Output 'focus: no Game.exe with a window.'
    } elseif ($idle -lt $MinIdle) {
        Write-Output ("focus: user input {0:F0} s ago (< {1} s) - not taking the foreground." -f $idle, $MinIdle)
        exit 4
    } else {
        $prev = [Native.U32]::GetForegroundWindow()
        if ($prev -ne $t.MainWindowHandle) {
            New-Item -ItemType Directory -Force (Split-Path $prevFile) | Out-Null
            Set-Content -Path $prevFile -Value $prev.ToInt64() -Encoding ascii
        }
        # a process may set the foreground window while a key press of its own is in flight
        [Native.U32]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero)
        $ok = [Native.U32]::SetForegroundWindow($t.MainWindowHandle)
        [Native.U32]::keybd_event(0x12, 0, 0x0002, [UIntPtr]::Zero)
        Save-InjectTime
        Start-Sleep -Milliseconds 400
        Write-Output ("focus: SetForegroundWindow={0}, game foreground now: {1} (user idle {2:F0} s)" -f $ok, (Test-GameForeground), $idle)
    }
}

if ($ClickToFront) {
    $li = New-Object Native.U32+LASTINPUTINFO
    $li.cbSize = 8
    [void][Native.U32]::GetLastInputInfo([ref]$li)
    $idle = ([Native.U32]::GetTickCount() - $li.dwTime) / 1000.0
    if (Test-Path $mineFile) {
        $mine = [uint32](Get-Content $mineFile | Select-Object -First 1)
        if ($li.dwTime -le ($mine + 200)) { $idle = [Math]::Max($idle, [double]$MinIdle) }
    }
    $g = Get-Process -Name Game -ErrorAction SilentlyContinue
    $pt = New-Object Native.U32+POINT
    [void][Native.U32]::GetCursorPos([ref]$pt)
    $under = [Native.U32]::GetAncestor([Native.U32]::WindowFromPoint($pt), 2)   # GA_ROOT
    $underPid = 0
    [void][Native.U32]::GetWindowThreadProcessId($under, [ref]$underPid)
    $gw = @($g | Where-Object { $_.MainWindowHandle -ne 0 }) | Select-Object -First 1
    if ($idle -ge $MinIdle -and $gw -and -not ($g.Id -contains [int]$underPid)) {
        # the pointer is somewhere else (another monitor?): put it on the middle of the game window
        $rc = New-Object Native.U32+RECT
        [void][Native.U32]::GetWindowRect($gw.MainWindowHandle, [ref]$rc)
        [void][Native.U32]::SetCursorPos([int](($rc.L + $rc.R) / 2), [int](($rc.T + $rc.B) / 2))
        Save-InjectTime
        Start-Sleep -Milliseconds 150
        [void][Native.U32]::GetCursorPos([ref]$pt)
        $under = [Native.U32]::GetAncestor([Native.U32]::WindowFromPoint($pt), 2)
        [void][Native.U32]::GetWindowThreadProcessId($under, [ref]$underPid)
    }
    if ($idle -lt $MinIdle) {
        Write-Output ("click-to-front: user input {0:F0} s ago - nothing sent." -f $idle)
    } elseif (-not ($g -and ($g.Id -contains [int]$underPid))) {
        Write-Output 'click-to-front: the window under the pointer is not the game - nothing sent.'
    } else {
        [Native.U32]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 60
        [Native.U32]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)
        Save-InjectTime
        Start-Sleep -Milliseconds 500
        Write-Output ("click-to-front: clicked on the game window, game foreground now: {0}" -f (Test-GameForeground))
    }
}

if ($Unfocus) {
    # give the foreground back to the window that had it before -Focus (what Alt+Tab away does)
    if (Test-Path $prevFile) {
        $h = [IntPtr][int64](Get-Content $prevFile | Select-Object -First 1)
        [Native.U32]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero)
        $ok = [Native.U32]::SetForegroundWindow($h)
        [Native.U32]::keybd_event(0x12, 0, 0x0002, [UIntPtr]::Zero)
        Save-InjectTime
        Start-Sleep -Milliseconds 400
        Write-Output ("unfocus: SetForegroundWindow={0}, game foreground now: {1}" -f $ok, (Test-GameForeground))
    } else {
        Write-Output 'unfocus: no saved foreground window.'
    }
}

if ($AltTab) {
    # A real Alt+Tab: Windows switches to the previously used window, and the game's DirectInput
    # keyboard sees Alt and Tab held down. Alt stays down for -AltHoldMs after Tab is released,
    # like a hand that is slow to let go. Only with the game in front or the user away.
    $li = New-Object Native.U32+LASTINPUTINFO
    $li.cbSize = 8
    [void][Native.U32]::GetLastInputInfo([ref]$li)
    $userRecent = $true
    if (Test-Path $mineFile) {
        $mine = [uint32](Get-Content $mineFile | Select-Object -First 1)
        if ($li.dwTime -le ($mine + 200)) { $userRecent = $false }
    }
    if ((([Native.U32]::GetTickCount() - $li.dwTime) / 1000.0) -ge $MinIdle) { $userRecent = $false }
    if ($userRecent -and -not (Test-GameForeground)) {
        Write-Output 'alttab: the user is active and the game is not in front - nothing sent.'
        exit 4
    }
    [Native.U32]::keybd_event(0x12, 0x38, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 70
    [Native.U32]::keybd_event(0x09, 0x0F, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 70
    [Native.U32]::keybd_event(0x09, 0x0F, 0x0002, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds $AltHoldMs
    if ($Cancel) {
        # Esc while Alt is still down closes the task switcher without switching
        [Native.U32]::keybd_event(0x1B, 0x01, 0, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 70
        [Native.U32]::keybd_event(0x1B, 0x01, 0x0002, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 120
    }
    [Native.U32]::keybd_event(0x12, 0x38, 0x0002, [UIntPtr]::Zero)
    Save-InjectTime
    Start-Sleep -Milliseconds 300
    Write-Output ("alttab: sent (Alt held {0} ms, cancelled={1}), game foreground now: {2}" -f $AltHoldMs, [bool]$Cancel, (Test-GameForeground))
}

if ($MoveX -ne 0 -or $MoveY -ne 0) {
    Assert-Foreground
    $dx = $MoveX; $dy = $MoveY
    $steps = [Math]::Max(1, [Math]::Ceiling([Math]::Max([Math]::Abs($dx), [Math]::Abs($dy)) / 40.0))
    $doneX = 0; $doneY = 0
    for ($i = 1; $i -le $steps; $i++) {
        $tx = [int][Math]::Round($dx * $i / $steps); $ty = [int][Math]::Round($dy * $i / $steps)
        [Native.U32]::mouse_event(0x0001, $tx - $doneX, $ty - $doneY, 0, [UIntPtr]::Zero)
        $doneX = $tx; $doneY = $ty
        Start-Sleep -Milliseconds 12
    }
}

if ($Click) {
    Assert-Foreground
    [Native.U32]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 80
    [Native.U32]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 80
}

if ($RClick) {
    Assert-Foreground
    [Native.U32]::mouse_event(0x0008, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 80
    [Native.U32]::mouse_event(0x0010, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 80
}

if ($Key -ge 0) {
    Assert-Foreground
    $f = 0x0008; if ($Ext) { $f = $f -bor 0x0001 }
    [Native.U32]::keybd_event(0, [byte]$Key, [uint32]$f, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds $HoldMs
    [Native.U32]::keybd_event(0, [byte]$Key, [uint32]($f -bor 0x0002), [UIntPtr]::Zero)
}

if ($MoveX -ne 0 -or $MoveY -ne 0 -or $Click -or $RClick -or $Key -ge 0) { Save-InjectTime }

if ($Wait -gt 0) { Start-Sleep -Milliseconds $Wait }

if ($Shot) {
    $outDir = Join-Path $root 'tools\out'
    New-Item -ItemType Directory -Force $outDir | Out-Null
    $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
    $small = New-Object System.Drawing.Bitmap 1280, 720
    $g2 = [System.Drawing.Graphics]::FromImage($small)
    $g2.InterpolationMode = 'HighQualityBilinear'
    $g2.DrawImage($bmp, 0, 0, 1280, 720)
    $path = Join-Path $outDir "$Shot.png"
    $small.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $g2.Dispose(); $bmp.Dispose(); $small.Dispose()
    Write-Output "shot: $path (game foreground: $(Test-GameForeground))"
}

if ($Close) {
    # what Alt+F4 does: WM_SYSCOMMAND / SC_CLOSE to the game window (needs no focus)
    $targets = @(Get-Process -Name Game -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 })
    if ($targets.Count -eq 0) {
        Write-Output 'close: no Game.exe with a window.'
    }
    foreach ($t in $targets) {
        $scClose = New-Object UIntPtr ([uint32]0xF060)
        $sent = [Native.U32]::PostMessageA($t.MainWindowHandle, [uint32]0x0112, $scClose, [IntPtr]::Zero)
        $sw = [Diagnostics.Stopwatch]::StartNew()
        $gone = $t.WaitForExit(12000)
        Write-Output ("close: PID={0} posted={1} exited={2} after {3:F1} s" -f $t.Id, $sent, $gone, $sw.Elapsed.TotalSeconds)
    }
}

if ($Kill) {
    Get-Process -Name Game -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 800
    Write-Output ("killed; still running: " + [bool](Get-Process -Name Game -ErrorAction SilentlyContinue))
}

if ($Status) {
    $g = @(Get-Process -Name Game -ErrorAction SilentlyContinue)
    Write-Output ("running: " + [bool]$g.Count + " | foreground: " + (Test-GameForeground))
    foreach ($p in $g) {
        Write-Output ("  PID={0} window={1} started={2:HH:mm:ss} responding={3}" -f $p.Id, ($p.MainWindowHandle -ne 0), $p.StartTime, $p.Responding)
    }
}
