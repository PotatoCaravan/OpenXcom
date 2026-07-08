<#
.SYNOPSIS
    Runs the engine's headless self-tests (OpenXcom.exe --selftest) and reports the result.

.DESCRIPTION
    The self-tests are fast, pure-C++ checks that need no game data and open no window. This is
    the second rung of the verification ladder (after build-check.ps1): it proves the engine
    binary starts and its core routines behave. Exit code 0 = all passed.

.PARAMETER Configuration
    Release (default) or Debug.
.PARAMETER Platform
    x64 (default) or Win32.
.PARAMETER NoBuild
    Fail instead of building if the exe is missing.

.EXAMPLE
    ./scripts/run-selftest.ps1
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    [ValidateSet('x64', 'Win32')]
    [string]$Platform = 'x64',
    [switch]$NoBuild
)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\_common.ps1"

$exe = Ensure-OxceBuilt -Configuration $Configuration -Platform $Platform -NoBuild:$NoBuild
Write-Host "Running self-tests: $exe" -ForegroundColor Cyan

$r = Invoke-Oxce -Exe $exe -OxceArgs @('--selftest') -Platform $Platform
if ($r.StdOut) { Write-Host $r.StdOut.TrimEnd() }
if ($r.ExitCode -ne 0 -and $r.StdErr) { Write-Host $r.StdErr.TrimEnd() -ForegroundColor DarkYellow }

if ($r.ExitCode -eq 0) {
    Write-Host "SELF-TESTS PASSED" -ForegroundColor Green
}
else {
    Write-Host "SELF-TESTS FAILED (exit $($r.ExitCode))" -ForegroundColor Red
}
exit $r.ExitCode
