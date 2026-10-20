# =====================================================================
#  JARVIS SEARCH - acceptance test
# ---------------------------------------------------------------------
#      powershell -ExecutionPolicy Bypass -File .\tools\acceptance.ps1
#
#  It drives the real program and checks what it actually does:
#  the window and where it sits, the global hotkey, the query field,
#  the arrow keys, launching an application and opening a web search.
#
#  The keyboard is simulated by posting the window messages that the
#  program would receive, so the test does not depend on which window
#  happens to be in front and it cannot be confused by another program
#  stealing the focus half way through.
#
#  The bar only holds the keyboard focus while it is summoned, and the
#  summoning is the last thing the test does, so the keys of the person
#  at the keyboard cannot end up in the query.
#
#  Options:
#      -SkipLaunch   do not really start anything, only check the
#                     log lines that the program writes
#      -SkipWeb      do not open a browser
#      -KeepRunning  leave the bar running when the test is done
#      -ShowLog      print every line of the log
# =====================================================================
[CmdletBinding()]
param(
    [switch]$SkipLaunch,
    [switch]$SkipWeb,
    [switch]$KeepRunning,
    [switch]$ShowLog
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$exe  = Join-Path $root 'JarvisSearch.exe'
if (-not (Test-Path $exe)) {
    Write-Error "JarvisSearch.exe not found in $root.  Build it first with build.sh."
    exit 1
}

#  Nothing is really started unless the whole test is asked for, so
#  the test never opens anything on the screen of the person at the
#  keyboard.
$full = (-not $SkipLaunch) -and (-not $SkipWeb)

$work = Join-Path $env:TEMP 'jarvissearch-acceptance'
New-Item -ItemType Directory -Force -Path $work | Out-Null
$log = Join-Path $work 'run.log'
$png = Join-Path $work 'frame.png'
foreach ($f in @($log, $png)) { if (Test-Path $f) { Remove-Item $f -Force } }

Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;

public class JS {
    public delegate bool EnumProc(IntPtr h, IntPtr l);

    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern int GetWindowThreadProcessId(IntPtr h, out int pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetClassNameW(IntPtr h, StringBuilder s, int m);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindowW(string cls, IntPtr name);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);
    [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW")]
    public static extern IntPtr GetWindowLongPtr(IntPtr h, int i);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern uint RegisterWindowMessageW(string name);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();

    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }

    public const uint WM_KEYDOWN = 0x0100;
    public const uint WM_CHAR   = 0x0102;
    public const uint WM_KILLFOCUS = 0x0008;
    public const uint WM_HOTKEY = 0x0312;
    public const uint GW_HWNDNEXT = 2;
    public const int  WS_EX_LAYERED  = 0x00080000;
    public const int  WS_EX_TOOLWINDOW = 0x00000080;
    public const int  WS_EX_TOPMOST = 0x00000008;
    public const int  WS_EX_TRANSPARENT = 0x00000020;
    public const int  GWL_EXSTYLE = -20;
    public const int  VK_RETURN = 0x0D;
    public const int  VK_TAB = 0x09;
    public const int  VK_ESCAPE = 0x1B;
    public const int  VK_UP = 0x26;
    public const int  VK_DOWN = 0x28;

    public static IntPtr FindBarWindow(int pid) {
        IntPtr found = IntPtr.Zero;
        EnumWindows(delegate(IntPtr h, IntPtr l) {
            int p; GetWindowThreadProcessId(h, out p);
            if (p == pid) {
                StringBuilder cn = new StringBuilder(256);
                GetClassNameW(h, cn, 256);
                if (cn.ToString() == "JarvisSearchBar") { found = h; return false; }
            }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
"@

$script:pass = 0
$script:fail = 0
$script:notes = @()

function Ok   ([string]$name, [string]$detail = '') {
    $script:pass++
    Write-Host ("  PASS  {0}{1}" -f $name, $(if ($detail) { "  --  $detail" } else { '' }))
}
function Bad  ([string]$name, [string]$detail = '') {
    $script:fail++
    Write-Host ("  FAIL  {0}{1}" -f $name, $(if ($detail) { "  --  $detail" } else { '' }))
    $script:notes += $name
}
function Note ([string]$text) { Write-Host ("        " + $text) }
function Head ([string]$text) { Write-Host ''; Write-Host ("== " + $text) }

function Read-Log {
    if (-not (Test-Path $log)) { return @() }
    return @(Get-Content $log)
}

function Ex-Style ([IntPtr]$h) { return [int][JS]::GetWindowLongPtr($h, [JS]::GWL_EXSTYLE) }

function Height ([IntPtr]$h) {
    $r = New-Object JS+RECT
    [void][JS]::GetWindowRect($h, [ref]$r)
    return ($r.Bottom - $r.Top)
}

#  The bar is on the desktop when the desktop window is somewhere
#  below it in the z order.  A few invisible helpers sit in the band
#  as well, so the search walks down instead of looking at the
#  neighbour only.
function In-Desktop-Band ([IntPtr]$h, [IntPtr]$desktop) {
    if ($desktop -eq [IntPtr]::Zero) { return $false }
    $w = $h
    for ($i = 0; $i -lt 64 -and $w -ne [IntPtr]::Zero; $i++) {
        if ($w -eq $desktop) { return $true }
        $w = [JS]::GetWindow($w, [JS]::GW_HWNDNEXT)
    }
    return $false
}

#  The message the bar listens on to be summoned.  It is the same one
#  a second copy of the program uses, so the test drives the real
#  path instead of poking the window directly.
function Summon ([IntPtr]$h) {
    $msg = [JS]::RegisterWindowMessageW('JarvisSearchShowBar')
    [void][JS]::PostMessageW($h, $msg, [IntPtr]::Zero, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 600
}

# ---------------------------------------------------------------------
#  1.  the self test: one frame rendered to a file
# ---------------------------------------------------------------------
Head 'render'
$selftest = Start-Process -FilePath $exe `
    -ArgumentList '--selftest', '--dump', $png `
    -Wait -PassThru -RedirectStandardOutput (Join-Path $work 'selftest.out') `
    -RedirectStandardError  (Join-Path $work 'selftest.err')

if ($selftest.ExitCode -eq 0 -and (Test-Path $png) -and ((Get-Item $png).Length -gt 2000)) {
    Ok 'the self test renders a frame' ("{0:N0} bytes" -f (Get-Item $png).Length)
} else {
    Bad 'the self test renders a frame' ("exit code {0}" -f $selftest.ExitCode)
}
if ($ShowLog -and (Test-Path (Join-Path $work 'selftest.out'))) {
    Get-Content (Join-Path $work 'selftest.out') | ForEach-Object { Note $_ }
}

# ---------------------------------------------------------------------
#  2.  start the bar for real
# ---------------------------------------------------------------------
Head 'the window'

Get-Process -Name 'JarvisSearch' -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 800

$barArgs = @('--log', $log)
if (-not $full) { $barArgs += '--dry-launch' }
$proc = Start-Process -FilePath $exe -ArgumentList $barArgs -PassThru
Start-Sleep -Seconds 4

$hwnd = [JS]::FindBarWindow($proc.Id)
if ($hwnd -eq [IntPtr]::Zero) {
    Bad 'the bar creates its window'
    Write-Host ''
    Write-Host 'the window never appeared, the log says:'
    Read-Log | ForEach-Object { Write-Host "  $_" }
    try { $proc.Kill() } catch {}
    exit 1
}
Ok 'the bar creates its window' ("hwnd {0}, pid {1}" -f $hwnd, $proc.Id)

if ([JS]::IsWindowVisible($hwnd)) { Ok 'the bar is visible' } else { Bad 'the bar is visible' }

$ex = Ex-Style $hwnd
if ($ex -band [JS]::WS_EX_LAYERED) {
    Bad 'the bar is not a layered window' 'a layered window floats above every application'
} else {
    Ok 'the bar is not a layered window'
}
if ($ex -band [JS]::WS_EX_TOPMOST) {
    Bad 'the bar does not stay on top while it rests'
} else {
    Ok 'the bar does not stay on top while it rests'
}
if ($ex -band [JS]::WS_EX_TOOLWINDOW)  { Ok 'the bar is a tool window' }
else                                   { Bad 'the bar is a tool window' }
if ($ex -band [JS]::WS_EX_TRANSPARENT) { Ok 'the resting bar lets the clicks through' }
else                                   { Bad 'the resting bar lets the clicks through' }

#  The bar has to sit on the desktop, so the shell's desktop window
#  has to be below it.
$progman = [JS]::FindWindowW('Progman', [IntPtr]::Zero)
if (In-Desktop-Band $hwnd $progman) {
    Ok 'the bar rests on the desktop' 'the shell desktop window is below it'
} else {
    Bad 'the bar rests on the desktop' ("the desktop window is {0}" -f $progman)
}

$r = New-Object JS+RECT
[void][JS]::GetWindowRect($hwnd, [ref]$r)
$w = $r.Right - $r.Left
$h = $r.Bottom - $r.Top
if ($w -gt 200 -and $h -gt 20) { Ok 'the bar has a sensible size' ("{0}x{1} pixels" -f $w, $h) }
else { Bad 'the bar has a sensible size' ("{0}x{1} pixels" -f $w, $h) }

#  The application list is built on a worker thread, so the test waits
#  for it before it looks at the results.
$scanned = $false
for ($i = 0; $i -lt 40; $i++) {
    if ((Read-Log) -match '^scan done') { $scanned = $true; break }
    Start-Sleep -Milliseconds 500
}
if ($scanned) { Ok 'the application list is built' }
else { Bad 'the application list is built' 'the scan did not finish in twenty seconds' }

# ---------------------------------------------------------------------
#  3.  the global hotkey
# ---------------------------------------------------------------------
Head 'the global hotkey'
$logLines = Read-Log
if ($logLines -match 'hotkey not registered') {
    Bad 'the hotkey is free' 'another program already owns Ctrl+Alt+Space'
} else {
    Ok 'the hotkey is free' 'Ctrl+Alt+Space'
}

# ---------------------------------------------------------------------
#  4.  the arrow keys
# ---------------------------------------------------------------------
Head 'the arrow keys'
[void][JS]::SendMessageW($hwnd, [JS]::WM_KEYDOWN, [IntPtr][JS]::VK_DOWN, [IntPtr]0)
Start-Sleep -Milliseconds 300
$logLines = Read-Log
$nav = @($logLines | Where-Object { $_ -match '^nav key=' })
if ($nav.Count -eq 0) {
    Bad 'the down arrow moves the selection down' 'nothing was written to the log'
} else {
    $entry = $nav[-1]
    Note $entry
    if ($entry -match 'sel=1') { Ok 'the down arrow moves the selection down' }
    else { Bad 'the down arrow moves the selection down' $entry }
}
[void][JS]::SendMessageW($hwnd, [JS]::WM_KEYDOWN, [IntPtr][JS]::VK_UP, [IntPtr]0)
Start-Sleep -Milliseconds 300
$logLines = Read-Log
$nav = @($logLines | Where-Object { $_ -match '^nav key=' })
if ($nav.Count -gt 0 -and $nav[-1] -match 'sel=0') { Ok 'the up arrow moves the selection back up' }
else { Bad 'the up arrow moves the selection back up' $(if ($nav.Count -gt 0) { $nav[-1] } else { 'nothing in the log' }) }

# ---------------------------------------------------------------------
#  5.  typing a query
# ---------------------------------------------------------------------
Head 'typing'
foreach ($ch in 'notepad'.ToCharArray()) {
    [void][JS]::SendMessageW($hwnd, [JS]::WM_CHAR, [IntPtr][int][char]$ch, [IntPtr]1)
    Start-Sleep -Milliseconds 40
}
Start-Sleep -Milliseconds 400
$logLines = Read-Log
$chars = @($logLines | Where-Object { $_ -match '^char=' })
if ($chars.Count -eq 7) { Ok 'every character reaches the query field' ('7 characters') }
else { Bad 'every character reaches the query field' ("{0} characters" -f $chars.Count) }

$lastQuery = ''
foreach ($line in $logLines) {
    if ($line -match '^char=\d+ query=(.*)$') { $lastQuery = $Matches[1] }
}
if ($lastQuery -eq 'notepa') { Ok 'the query is built up in order' ('"' + $lastQuery + '" before the last character') }
else { Bad 'the query is built up in order' ('"' + $lastQuery + '"') }

# ---------------------------------------------------------------------
#  6.  launching the application that was found
# ---------------------------------------------------------------------
Head 'launching an application'
#  A notepad that is already open would be brought to the front
#  instead of starting a new one, so it is closed first.
Get-Process -Name 'Notepad' -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 2
$before = @(Get-Process -Name 'Notepad' -ErrorAction SilentlyContinue | ForEach-Object { $_.Id })
[void][JS]::SendMessageW($hwnd, [JS]::WM_KEYDOWN, [IntPtr][JS]::VK_RETURN, [IntPtr]0)
Start-Sleep -Seconds 3

$logLines = Read-Log
$launch = @($logLines | Where-Object { $_ -match '^launch app=' })
if ($launch.Count -eq 0) {
    Bad 'the application is launched' 'nothing was written to the log'
} else {
    $entry = $launch[-1]
    Note $entry
    if ($entry -match 'ok=1') { Ok 'the application is launched' }
    else { Bad 'the application is launched' 'the program reported a failure' }
    if ($entry -match 'app=Notepad') { Ok 'the right application was chosen' }
    else { Bad 'the right application was chosen' }
}

if ($full) {
    #  A store application is started by the shell, so the process can
    #  take a moment to appear.
    $fresh = @()
    for ($i = 0; $i -lt 20; $i++) {
        Start-Sleep -Milliseconds 500
        $now = @(Get-Process -Name 'Notepad' -ErrorAction SilentlyContinue | ForEach-Object { $_.Id })
        $fresh = @($now | Where-Object { $before -notcontains $_ })
        if ($fresh.Count -gt 0) { break }
    }
    if ($fresh.Count -gt 0) {
        Ok 'the application really starts' ("pid {0}" -f $fresh[0])
        Get-Process -Id $fresh -ErrorAction SilentlyContinue | Stop-Process -Force
    } else {
        Bad 'the application really starts' 'no new process appeared'
    }
}

# ---------------------------------------------------------------------
#  7.  a web search
# ---------------------------------------------------------------------
if (-not $SkipWeb) {
    Head 'opening a web search'
    foreach ($ch in 'jarvis hud'.ToCharArray()) {
        [void][JS]::SendMessageW($hwnd, [JS]::WM_CHAR, [IntPtr][int][char]$ch, [IntPtr]1)
        Start-Sleep -Milliseconds 40
    }
    Start-Sleep -Milliseconds 300
    [void][JS]::SendMessageW($hwnd, [JS]::WM_KEYDOWN, [IntPtr][JS]::VK_TAB, [IntPtr]0)
    Start-Sleep -Milliseconds 300
    [void][JS]::SendMessageW($hwnd, [JS]::WM_KEYDOWN, [IntPtr][JS]::VK_RETURN, [IntPtr]0)
    Start-Sleep -Seconds 3

    $logLines = Read-Log
    $web = @($logLines | Where-Object { $_ -match '^open web provider=' })
    if ($web.Count -eq 0) {
        Bad 'the web search opens' 'nothing was written to the log'
    } else {
        $entry = $web[-1]
        Note $entry
        if ($entry -match 'url=https://www\.google\.com/search\?q=jarvis\+hud|q=jarvis%20hud') {
            Ok 'the web search opens' 'the query is in the url'
        } else {
            Bad 'the web search opens' 'the url does not hold the query'
        }
    }
}

# ---------------------------------------------------------------------
#  8.  the escape key clears the query
# ---------------------------------------------------------------------
Head 'clearing the query'
[void][JS]::SendMessageW($hwnd, [JS]::WM_CHAR, [IntPtr][int][char]'x', [IntPtr]1)
Start-Sleep -Milliseconds 300
[void][JS]::SendMessageW($hwnd, [JS]::WM_KEYDOWN, [IntPtr][JS]::VK_ESCAPE, [IntPtr]0)
Start-Sleep -Milliseconds 400
#  The escape is handled as a key, not as a character, so the proof is
#  that the next character lands in an empty query.
[void][JS]::SendMessageW($hwnd, [JS]::WM_CHAR, [IntPtr][int][char]'y', [IntPtr]1)
Start-Sleep -Milliseconds 400
$logLines = Read-Log
$lastQuery = ''
foreach ($line in $logLines) {
    if ($line -match '^char=\d+ query=(.*)$') { $lastQuery = $Matches[1] }
}
if ($lastQuery -eq '') { Ok 'escape clears the query' }
else { Bad 'escape clears the query' ('"' + $lastQuery + '" is left over') }

# ---------------------------------------------------------------------
#  9.  summoning the bar, and putting it away again
# ---------------------------------------------------------------------
Head 'summoning the bar'
Summon $hwnd
$ex = Ex-Style $hwnd
if ($ex -band [JS]::WS_EX_TOPMOST) { Ok 'the summoned bar is on top' }
else { Bad 'the summoned bar is on top' }
if ($ex -band [JS]::WS_EX_TRANSPARENT) { Bad 'the summoned bar can be clicked' }
else { Ok 'the summoned bar can be clicked' }

$h = Height $hwnd
if ($h -gt 120) { Ok 'the summoned bar opens its panel' ("{0} pixels tall" -f $h) }
else { Bad 'the summoned bar opens its panel' ("{0} pixels tall" -f $h) }

Head 'going back to the desktop'
#  The bar goes away when it loses the focus, which is what happens
#  when the person at the keyboard clicks somewhere else.
[void][JS]::SendMessageW($hwnd, [JS]::WM_KILLFOCUS, [IntPtr]::Zero, [IntPtr]::Zero)
Start-Sleep -Milliseconds 800

$ex = Ex-Style $hwnd
if ($ex -band [JS]::WS_EX_TOPMOST) { Bad 'the bar goes back to the desktop' 'it is still on top' }
else { Ok 'the bar goes back to the desktop' }
if ($ex -band [JS]::WS_EX_TRANSPARENT) { Ok 'the resting bar lets the clicks through again' }
else { Bad 'the resting bar lets the clicks through again' }

$below = [JS]::GetWindow($hwnd, [JS]::GW_HWNDNEXT)
if (In-Desktop-Band $hwnd $progman) {
    Ok 'the bar is back in the desktop band'
} else {
    Bad 'the bar is back in the desktop band' ("the window below it is {0}" -f $below)
}

$h = Height $hwnd
if ($h -lt 120) { Ok 'the panel is closed when the bar rests' ("{0} pixels tall" -f $h) }
else { Bad 'the panel is closed when the bar rests' ("{0} pixels tall" -f $h) }

# ---------------------------------------------------------------------
#  10.  the hotkey brings the bar up and puts it away again
# ---------------------------------------------------------------------
Head 'the hotkey'
#  The message the system sends when the hotkey is pressed.  Posting it
#  drives exactly the same code the real key press does.
[void][JS]::PostMessageW($hwnd, [JS]::WM_HOTKEY, [IntPtr]0xB01, [IntPtr]::Zero)
Start-Sleep -Milliseconds 800
$h = Height $hwnd
if ($h -gt 120) { Ok 'the hotkey brings the bar up' ("{0} pixels tall" -f $h) }
else { Bad 'the hotkey brings the bar up' ("{0} pixels tall" -f $h) }

[void][JS]::PostMessageW($hwnd, [JS]::WM_HOTKEY, [IntPtr]0xB01, [IntPtr]::Zero)
Start-Sleep -Milliseconds 800
$h = Height $hwnd
if ($h -lt 120) { Ok 'the hotkey puts the bar away again' ("{0} pixels tall" -f $h) }
else { Bad 'the hotkey puts the bar away again' ("{0} pixels tall" -f $h) }

# ---------------------------------------------------------------------
#  11.  only one bar at a time
# ---------------------------------------------------------------------
Head 'one bar only'
$second = Start-Process -FilePath $exe -PassThru
Start-Sleep -Seconds 3
$secondExited = $second.HasExited
$bars = 0
$cb = [JS+EnumProc]{
    param($h, $l)
    $p = 0
    [void][JS]::GetWindowThreadProcessId($h, [ref]$p)
    $cn = New-Object System.Text.StringBuilder 256
    [void][JS]::GetClassNameW($h, $cn, 256)
    if ($cn.ToString() -eq 'JarvisSearchBar') { $script:barCount++ }
    return $true
}
$script:barCount = 0
[void][JS]::EnumWindows($cb, [IntPtr]::Zero)
if ($script:barCount -eq 1) { Ok 'there is only one bar on the screen' }
else { Bad 'there is only one bar on the screen' ("{0} bars" -f $script:barCount) }
if ($secondExited) { Ok 'a second copy steps aside' } else { Note 'the second copy is still running'; try { $second.Kill() } catch {} }

# ---------------------------------------------------------------------
#  done
# ---------------------------------------------------------------------
if (-not $KeepRunning) {
    try { $proc.Kill() } catch {}
} else {
    Note 'the bar is left running'
}

Write-Host ''
Write-Host ('{0} passed, {1} failed' -f $script:pass, $script:fail)
if ($script:fail -gt 0) {
    Write-Host ('failed: ' + ($script:notes -join ', '))
    exit 1
}
Write-Host 'the bar is working.'
exit 0
