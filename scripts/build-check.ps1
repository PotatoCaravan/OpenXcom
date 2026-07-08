<#
.SYNOPSIS
    Compile gate for OpenXcom Extended (AI Mods fork).

.DESCRIPTION
    Builds src\OpenXcom.2010.sln with MSBuild (located via vswhere) and returns MSBuild's
    exit code. This is the FIRST thing to run after any C++ change: if it does not print
    "BUILD OK" and return 0, the change is not done.

    Dependencies are vendored under deps\, so no package install is needed. Uses the plain
    Release/Debug toolset (never Release_XP, which needs the VS2017 XP toolset we don't have).

.PARAMETER Configuration
    Release (default) or Debug. Debug also runs the legacy NDEBUG asserts at startup.

.PARAMETER Platform
    x64 (default) or Win32.

.PARAMETER CMake
    Build via CMake instead of MSBuild (configures + builds under build\).

.PARAMETER Clean
    Clean before building.

.EXAMPLE
    ./scripts/build-check.ps1
.EXAMPLE
    ./scripts/build-check.ps1 -Configuration Debug -Platform Win32
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    [ValidateSet('x64', 'Win32')]
    [string]$Platform = 'x64',
    [switch]$CMake,
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$sln = Join-Path $repoRoot 'src\OpenXcom.2010.sln'

function Find-MSBuild {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) {
        throw "vswhere.exe not found ($vswhere). Install Visual Studio or the C++ Build Tools."
    }
    $msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild `
        -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
    if (-not $msbuild) {
        throw "MSBuild not found via vswhere. Install the 'Desktop development with C++' workload."
    }
    return $msbuild
}

if ($CMake) {
    $build = Join-Path $repoRoot 'build'
    if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }
    New-Item -ItemType Directory -Force -Path $build | Out-Null
    Write-Host '== CMake configure ==' -ForegroundColor Cyan
    & cmake -S $repoRoot -B $build -DCMAKE_BUILD_TYPE=$Configuration
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    Write-Host '== CMake build ==' -ForegroundColor Cyan
    & cmake --build $build --config $Configuration --parallel
    exit $LASTEXITCODE
}

$msbuild = Find-MSBuild
Write-Host "MSBuild : $msbuild" -ForegroundColor DarkGray
Write-Host "Building: $Configuration|$Platform" -ForegroundColor Cyan

$targets = if ($Clean) { 'Clean;Build' } else { 'Build' }
& $msbuild $sln /nologo /m /t:$targets /p:Configuration=$Configuration /p:Platform=$Platform /v:minimal
$code = $LASTEXITCODE

if ($code -eq 0) {
    $exe = Join-Path $repoRoot "bin\$Platform\$Configuration\OpenXcom.exe"
    Write-Host "BUILD OK -> $exe" -ForegroundColor Green
}
else {
    Write-Host "BUILD FAILED (exit $code)" -ForegroundColor Red
}
exit $code
