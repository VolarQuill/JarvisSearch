# =====================================================================
#  JARVIS SEARCH - installer
# ---------------------------------------------------------------------
#      powershell -ExecutionPolicy Bypass -File .\install.ps1
#
#  Copies the program to %LOCALAPPDATA%\JarvisSearch, registers it so
#  that it starts with Windows, and puts a shortcut in the Start menu.
#
#  The user's own config.txt is never silently destroyed.  If the
#  installed copy differs from the shipped one - which normally means it
#  was edited - it is left exactly as it is and a snapshot is written
#  next to it as "config.txt.bak".
#
#  Options:
#      -NoAutostart   do not touch the Run entry
#      -NoShortcut    do not write the Start menu shortcut
#      -NoStart       do not start the bar when the install is done
#      -Force         replace an edited config.txt with the shipped copy
#                     (the snapshot is still written first)
#      -InstallDir    where to put the program, %LOCALAPPDATA%\JarvisSearch
# =====================================================================
[CmdletBinding()]
param(
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA 'JarvisSearch'),
    [switch]$NoAutostart,
    [switch]$NoShortcut,
    [switch]$NoStart,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe  = Join-Path $root 'JarvisSearch.exe'

if (-not (Test-Path -LiteralPath $exe)) {
    Write-Error "JarvisSearch.exe not found next to install.ps1 ($root).  Build it first with build.sh."
    exit 1
}

$summary = @()
function Add-Summary ([string]$text) { $script:summary += $text }

function Test-SameContent {
    param([string]$PathA, [string]$PathB)
    $hashA = (Get-FileHash -LiteralPath $PathA -Algorithm SHA256).Hash
    $hashB = (Get-FileHash -LiteralPath $PathB -Algorithm SHA256).Hash
    return ($hashA -eq $hashB)
}

function Install-DataFile {
    <#
        .SYNOPSIS
            Installs one shipped data file (config.txt).

        .DESCRIPTION
            The user's copy is never silently destroyed:
              * not present                   -> copied in
              * identical to the shipped copy -> left alone
              * different (i.e. possibly edited) -> left alone, and a
                snapshot of it is written to "<name>.bak"
            With -Force a differing file is replaced, but the snapshot is
            still written first, so the user's edits stay recoverable.
    #>
    param(
        [Parameter(Mandatory = $true)][string] $Source,
        [Parameter(Mandatory = $true)][string] $Destination,
        [Parameter(Mandatory = $true)][string] $Label
    )

    if (-not (Test-Path -LiteralPath $Source)) {
        Write-Warn ('no shipped ' + $Label + ' found, skipping')
        Add-Summary ('skipped ' + $Label + ' (not present in the project folder)')
        return
    }

    $backup = $Destination + '.bak'

    if (-not (Test-Path -LiteralPath $Destination)) {
        Copy-Item -LiteralPath $Source -Destination $Destination -Force
        Write-Host ('installed ' + $Label)
        Add-Summary ('installed ' + $Label)
        return
    }

    if (Test-SameContent -PathA $Source -PathB $Destination) {
        Write-Host ($Label + ' is already up to date')
        Add-Summary ($Label + ' unchanged (already up to date)')
        return
    }

    #  The installed copy differs from the shipped copy, so it may have
    #  been edited by the user.  Snapshot it before doing anything else.
    Copy-Item -LiteralPath $Destination -Destination $backup -Force

    if ($Force) {
        Copy-Item -LiteralPath $Source -Destination $Destination -Force
        Write-Host ($Label + ' replaced with the shipped copy (previous contents saved as ' + (Split-Path -Leaf $backup) + ')')
        Add-Summary ($Label + ' replaced with the shipped copy')
    }
    else {
        Write-Warn ($Label + ' differs from the shipped copy and was left untouched (snapshot: ' + (Split-Path -Leaf $backup) + ')')
        Add-Summary ($Label + ' left as it is (snapshot: ' + (Split-Path -Leaf $backup) + ')')
    }
}

# ---------------------------------------------------------------------
#  the program itself
# ---------------------------------------------------------------------
Write-Host "installing to $InstallDir"
New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null

#  The bar is running while it is replaced, so the old copy is stopped
#  first.  A file that is in use cannot be overwritten.
Get-Process -Name 'JarvisSearch' -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 400

Copy-Item -LiteralPath $exe -Destination (Join-Path $InstallDir 'JarvisSearch.exe') -Force
Write-Host 'installed JarvisSearch.exe'
Add-Summary 'installed JarvisSearch.exe'

Install-DataFile -Source (Join-Path $root 'config.txt') `
                 -Destination (Join-Path $InstallDir 'config.txt') `
                 -Label 'config.txt'

if (Test-Path -LiteralPath (Join-Path $root 'README.md')) {
    Copy-Item -LiteralPath (Join-Path $root 'README.md') -Destination (Join-Path $InstallDir 'README.md') -Force
    Add-Summary 'installed README.md'
}

# ---------------------------------------------------------------------
#  start with Windows
# ---------------------------------------------------------------------
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$target = Join-Path $InstallDir 'JarvisSearch.exe'
if (-not $NoAutostart) {
    New-Item -Path $runKey -Force | Out-Null
    Set-ItemProperty -Path $runKey -Name 'JarvisSearch' -Value ('"{0}"' -f $target)
    Write-Host "autostart registered: $runKey -> JarvisSearch"
    Add-Summary 'registered to start with Windows'
}
else {
    Remove-ItemProperty -Path $runKey -Name 'JarvisSearch' -ErrorAction SilentlyContinue
    Write-Host 'autostart not registered'
    Add-Summary 'autostart not registered'
}

# ---------------------------------------------------------------------
#  start menu shortcut
# ---------------------------------------------------------------------
if (-not $NoShortcut) {
    $shell = New-Object -ComObject WScript.Shell
    $lnk = $shell.CreateShortcut((Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\JARVIS Search.lnk'))
    $lnk.TargetPath = $target
    $lnk.WorkingDirectory = $InstallDir
    $lnk.Description = 'JARVIS Search'
    $lnk.Save()
    Write-Host 'start menu shortcut written'
    Add-Summary 'start menu shortcut written'
}

# ---------------------------------------------------------------------
#  run it now
# ---------------------------------------------------------------------
if (-not $NoStart) {
    Write-Host 'starting the bar'
    Start-Process -FilePath $target
    Add-Summary 'the bar is running'
}

Write-Host ''
foreach ($line in $summary) { Write-Host ('  - ' + $line) }
Write-Host ''
Write-Host 'done.  press Ctrl+Alt+Space to focus the bar.'
