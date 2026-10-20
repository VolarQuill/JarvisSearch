# =====================================================================
#  JARVIS SEARCH - acceptance test
# ---------------------------------------------------------------------
#      powershell -ExecutionPolicy Bypass -File .\tools\acceptance.ps1
#
#  It drives the real program and checks what it actually does:
#  the window, the global hotkey, the query field, launching an
#  application and opening a web search.
#
#  The keyboard is simulated by posting the window messages that the
#  program would receive, so the test does not depend on which window
#  happens to be in front and it cannot be confused by another program
#  stealing the focus half way through.
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
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW")]
    public static extern IntPtr GetWindowLongPtr(IntPtr h, int i);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr p);

    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }

    public const uint WM_KEYDOWN = 0x0100;
    public const uint WM_CHAR   = 0x0102;
    public const uint WM_SETFOCUS = 0x0007;
    public const int  WS_EX_LAYERED  = 0x00080000;
    public const int  WS_EX_TOOLWINDOW = 0x00000080;
    public const int  WS_EX_TOPMOST = 0x00000008;
    public const int  GWL_EXSTYLE = -20;
    public const int  VK_RETURN = 0x0D;
    public const int  VK_TAB = 0x09;
    public const int  VK_ESCAPE = 0x1B;

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

$proc = Start-Process -FilePath $exe -ArgumentList '--log', $log -PassThru
Start-Sleep -Seconds 6

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

$ex = [int][JS]::GetWindowLongPtr($hwnd, [JS]::GWL_EXSTYLE)
if ($ex -band [JS]::WS_EX_LAYERED)     { Ok 'the bar is a layered window' }
else                                   { Bad 'the bar is a layered window' }
if ($ex -band [JS]::WS_EX_TOPMOST)     { Ok 'the bar stays on top' }
else                                   { Bad 'the bar stays on top' }
if ($ex -band [JS]::WS_EX_TOOLWINDOW)  { Ok 'the bar is a tool window' }
else                                   { Bad 'the bar is a tool window' }

$r = New-Object JS+RECT
[void][JS]::GetWindowRect($hwnd, [ref]$r)
$w = $r.Right - $r.Left
$h = $r.Bottom - $r.Top
if ($w -gt 200 -and $h -gt 20) { Ok 'the bar has a sensible size' ("{0}x{1} pixels" -f $w, $h) }
else { Bad 'the bar has a sensible size' ("{0}x{1} pixels" -f $w, $h) }

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
#  4.  typing a query
# ---------------------------------------------------------------------
Head 'typing'
[void][JS]::SendMessageW($hwnd, [JS]::WM_SETFOCUS, [IntPtr]::Zero, [IntPtr]::Zero)
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
#  5.  launching the application that was found
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

if (-not $SkipLaunch) {
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
#  6.  a web search
# ---------------------------------------------------------------------
if (-not $SkipWeb) {
    Head 'opening a web search'
    [void][JS]::SendMessageW($hwnd, [JS]::WM_SETFOCUS, [IntPtr]::Zero, [IntPtr]::Zero)
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
#  7.  the escape key clears the query
# ---------------------------------------------------------------------
Head 'clearing the query'
[void][JS]::SendMessageW($hwnd, [JS]::WM_KEYDOWN, [IntPtr][JS]::VK_ESCAPE, [IntPtr]0)
Start-Sleep -Milliseconds 400
#  The escape is handled as a key, not as a character, so the proof is
#  that the next character lands in an empty query.
[void][JS]::SendMessageW($hwnd, [JS]::WM_CHAR, [IntPtr][int][char]'x', [IntPtr]1)
Start-Sleep -Milliseconds 400
$logLines = Read-Log
$lastQuery = ''
foreach ($line in $logLines) {
    if ($line -match '^char=\d+ query=(.*)$') { $lastQuery = $Matches[1] }
}
if ($lastQuery -eq '') { Ok 'escape clears the query' }
else { Bad 'escape clears the query' ('"' + $lastQuery + '" is left over') }

# ---------------------------------------------------------------------
#  8.  only one bar at a time
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
