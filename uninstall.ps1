# =====================================================================
#  JARVIS SEARCH - uninstaller
# ---------------------------------------------------------------------
#      powershell -ExecutionPolicy Bypass -File .\uninstall.ps1
#
#  Removes the autostart entry, the start menu shortcut and the
#  installed folder.  The folder is kept when -KeepFiles is given, so
#  that an edited config.txt is not thrown away.
#
#  Options:
#      -KeepFiles   leave %LOCALAPPDATA%\JarvisSearch in place
#      -InstallDir  the folder to remove
# =====================================================================
[CmdletBinding()]
param(
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA 'JarvisSearch'),
    [switch]$KeepFiles
)

$ErrorActionPreference = 'Stop'

Write-Host 'stopping the bar'
Get-Process -Name 'JarvisSearch' -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue

$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
Remove-ItemProperty -Path $runKey -Name 'JarvisSearch' -ErrorAction SilentlyContinue
Write-Host 'autostart entry removed'

$lnk = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\JARVIS Search.lnk'
Remove-Item $lnk -ErrorAction SilentlyContinue
Write-Host 'start menu shortcut removed'

if (-not $KeepFiles -and (Test-Path -LiteralPath $InstallDir)) {
    Remove-Item -LiteralPath $InstallDir -Recurse -Force
    Write-Host "removed $InstallDir"
}
elseif (Test-Path -LiteralPath $InstallDir) {
    Write-Host "kept $InstallDir"
}

Write-Host 'done.'
