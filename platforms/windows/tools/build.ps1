<#
.SYNOPSIS
    Configures and builds LiveWall for Windows.

.DESCRIPTION
    The whole dependency list is the Windows SDK, so this needs Visual Studio
    (or the Build Tools) with the "Desktop development with C++" workload and
    nothing else. No vcpkg, no NuGet restore, no submodules.

.EXAMPLE
    ./tools/build.ps1
    ./tools/build.ps1 -Configuration Debug
    ./tools/build.ps1 -Preset vs -Test
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',

    [ValidateSet('default', 'vs', 'ci')]
    [string]$Preset = 'default',

    [switch]$Test,

    # Skips the configure step when only sources changed. Configuring is a few
    # seconds, so this is for tight edit-build loops rather than for CI.
    [switch]$NoConfigure
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root

try {
    if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
        throw "cmake was not found on PATH. Install Visual Studio's C++ workload, or run this from a Developer PowerShell."
    }

    if (-not $NoConfigure) {
        Write-Host "==> Configuring ($Preset)" -ForegroundColor Cyan
        cmake --preset $Preset
        if ($LASTEXITCODE -ne 0) { throw "configure failed" }
    }

    Write-Host "==> Building ($Configuration)" -ForegroundColor Cyan
    cmake --build --preset $Preset --config $Configuration
    if ($LASTEXITCODE -ne 0) { throw "build failed" }

    if ($Test) {
        Write-Host "==> Testing" -ForegroundColor Cyan
        ctest --preset $Preset --build-config $Configuration --output-on-failure
        if ($LASTEXITCODE -ne 0) { throw "tests failed" }
    }

    $exe = Join-Path $root "out/build/$Preset/LiveWall.exe"
    if (Test-Path $exe) {
        $size = (Get-Item $exe).Length
        Write-Host ""
        Write-Host ("==> Done: {0} ({1:N0} KB)" -f $exe, ($size / 1KB)) -ForegroundColor Green
        Write-Host "    Run with:  $exe"
        Write-Host "    Verbose:   `$env:LIVEWALL_VERBOSE=1; $exe"
        Write-Host "    Probe:     $exe --probe"
    }
}
finally {
    Pop-Location
}
