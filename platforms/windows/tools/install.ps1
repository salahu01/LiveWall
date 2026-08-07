<#
.SYNOPSIS
    Builds LiveWall and installs it to %LOCALAPPDATA%\Programs\LiveWall.

.DESCRIPTION
    Why this exists rather than just running out of the build directory:

    "Start with Windows" stores the *path* of the executable in the registry's
    Run key. Point it at a build directory and it keeps working until the
    directory is cleaned, moved or rebuilt somewhere else — at which point
    Windows silently starts nothing at login. An installed copy is stable; a
    build directory is not.

    (The app repairs a stale registration on its next launch, but only if it is
    launched, which after a failed login-start it will not be.)

    Installs per user rather than to Program Files, because the app writes only
    to %LOCALAPPDATA% and HKCU and asking for elevation would gain nothing.

.EXAMPLE
    ./tools/install.ps1
    ./tools/install.ps1 -Prefix "C:\Tools"
#>
[CmdletBinding()]
param(
    [string]$Prefix = (Join-Path $env:LOCALAPPDATA 'Programs'),
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$target = Join-Path $Prefix 'LiveWall'
$source = Join-Path $root 'out/build/default/LiveWall.exe'

if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot 'build.ps1') -Configuration $Configuration
}

if (-not (Test-Path $source)) {
    throw "no executable at $source — run tools/build.ps1 first"
}

# Replacing a running copy leaves the old process attached to a deleted file,
# which is the state that makes the tray icon stop answering clicks.
$running = Get-Process -Name 'LiveWall' -ErrorAction SilentlyContinue
if ($running) {
    Write-Host "==> Quitting the running copy" -ForegroundColor Cyan
    $running | ForEach-Object { $_.CloseMainWindow() | Out-Null }
    Start-Sleep -Milliseconds 500
    Get-Process -Name 'LiveWall' -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 200
}

Write-Host "==> Installing to $target" -ForegroundColor Cyan
New-Item -ItemType Directory -Force -Path $target | Out-Null
Copy-Item -Path $source -Destination (Join-Path $target 'LiveWall.exe') -Force

Write-Host "==> Launching" -ForegroundColor Cyan
Start-Process -FilePath (Join-Path $target 'LiveWall.exe')

Write-Host ""
Write-Host "Installed: $target\LiveWall.exe" -ForegroundColor Green
Write-Host "Enable 'Start with Windows' from the tray menu — it will now survive a rebuild,"
Write-Host "because this copy is not the one build.ps1 overwrites."
