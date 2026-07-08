<#
.SYNOPSIS
    Headless mod/ruleset/script validation (OpenXcom.exe --validate).

.DESCRIPTION
    Loads the selected master mod and its active mods with strict validation and reports whether
    they load cleanly - without opening a window and without touching your real saves/options.cfg
    (a throwaway temp folder is used for -user/-cfg). This is the third rung of the verification
    ladder and the key check after any .rul / Y-Script change.

    Copyrighted X-COM game data is read from an EXTERNAL folder via -data, so nothing copyrighted
    ever enters the repo. By default that folder is auto-detected two directories above the repo
    (the installed engine), or taken from scripts/gamedata-root.local.txt (see setup-gamedata.ps1),
    or supplied with -DataRoot.

.PARAMETER Master
    Master mod id to validate: xcom1 (UFO, default) or xcom2 (TFTD), or any installed master.
.PARAMETER DataRoot
    Data folder containing standard\ common\ UFO\ TFTD\. Defaults to the repo's own bin\, which
    holds the version-matched rulesets plus the game-data junctions created by setup-gamedata.ps1.
    Override to validate against a different install.
.PARAMETER Configuration / Platform
    Build flavour to run (Release/x64 by default).
.PARAMETER NoBuild
    Fail instead of building if the exe is missing.
.PARAMETER KeepTemp
    Keep the throwaway user/config folder (and its openxcom.log) for inspection.

.OUTPUTS
    Exit code: 0 = clean, 1 = validation failure, 2 = no game data / environment.

.EXAMPLE
    ./scripts/validate-mods.ps1 -Master xcom1
#>
[CmdletBinding()]
param(
    [string]$Master = 'xcom1',
    [string]$DataRoot,
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    [ValidateSet('x64', 'Win32')]
    [string]$Platform = 'x64',
    [switch]$NoBuild,
    [switch]$KeepTemp
)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\_common.ps1"

# --- Resolve the data folder (contains standard\ common\ UFO\ TFTD\) ---
if (-not $DataRoot) {
    $DataRoot = Join-Path (Get-RepoRoot) 'bin'
}
if (-not (Test-Path $DataRoot)) {
    Write-Host "Data folder not found: $DataRoot" -ForegroundColor Red
    exit 2
}
# Warn early if the game-data junctions/assets are missing (setup-gamedata.ps1 not run yet).
if (-not (Test-Path (Join-Path $DataRoot 'UFO\GEODATA'))) {
    Write-Host "Note: '$DataRoot\UFO' has no game assets yet - run scripts/setup-gamedata.ps1 first." -ForegroundColor Yellow
}

$exe = Ensure-OxceBuilt -Configuration $Configuration -Platform $Platform -NoBuild:$NoBuild

# Isolated user/config dir so we never touch the player's real saves or options.cfg.
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("oxce-validate-" + [Guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force -Path $temp | Out-Null

Write-Host "Validating master '$Master'" -ForegroundColor Cyan
Write-Host "  data : $DataRoot" -ForegroundColor DarkGray
Write-Host "  temp : $temp" -ForegroundColor DarkGray

try {
    $r = Invoke-Oxce -Exe $exe -Platform $Platform -OxceArgs @(
        '-data', $DataRoot,
        '-user', $temp,
        '-cfg', $temp,
        '-master', $Master,
        '-validate'
    )
    if ($r.StdOut) { Write-Host $r.StdOut.TrimEnd() }

    if ($r.ExitCode -ne 0) {
        $log = Join-Path $temp 'openxcom.log'
        if (Test-Path $log) {
            Write-Host "--- last 40 log lines ($log) ---" -ForegroundColor DarkYellow
            Get-Content $log -Tail 40 | ForEach-Object { Write-Host $_ }
        }
        elseif ($r.StdErr) {
            Write-Host $r.StdErr.TrimEnd() -ForegroundColor DarkYellow
        }
    }

    switch ($r.ExitCode) {
        0 { Write-Host "VALIDATION PASSED (master=$Master)" -ForegroundColor Green }
        2 { Write-Host "VALIDATION SKIPPED: no X-COM game data under '$DataRoot'" -ForegroundColor Yellow }
        default { Write-Host "VALIDATION FAILED (exit $($r.ExitCode))" -ForegroundColor Red }
    }
    exit $r.ExitCode
}
finally {
    if ($KeepTemp) {
        Write-Host "Kept temp dir: $temp" -ForegroundColor DarkGray
    }
    else {
        Remove-Item $temp -Recurse -Force -ErrorAction SilentlyContinue
    }
}
