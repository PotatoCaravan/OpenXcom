# scripts/_common.ps1
# Shared helpers for the OpenXcom Extended (AI Mods) verification scripts.
# Dot-source this file: . "$PSScriptRoot\_common.ps1"

function Get-RepoRoot {
    return (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
}

function Resolve-OxceExe {
    param(
        [string]$Configuration = 'Release',
        [string]$Platform = 'x64'
    )
    return (Join-Path (Get-RepoRoot) "bin\$Platform\$Configuration\OpenXcom.exe")
}

function Get-OxceDllDir {
    param([string]$Platform = 'x64')
    # The VS post-build event copies the SDL runtime DLLs to bin\<Platform>\ (one level ABOVE the
    # exe, which lands in bin\<Platform>\<Configuration>\). Putting this dir on PATH lets the exe
    # find them from anywhere.
    return (Join-Path (Get-RepoRoot) "bin\$Platform")
}

function Ensure-OxceBuilt {
    param(
        [string]$Configuration = 'Release',
        [string]$Platform = 'x64',
        [switch]$NoBuild
    )
    $exe = Resolve-OxceExe -Configuration $Configuration -Platform $Platform
    if (Test-Path $exe) { return $exe }
    if ($NoBuild) {
        throw "OpenXcom.exe not found at '$exe' and -NoBuild was set. Run scripts/build-check.ps1 first."
    }
    Write-Host "Executable missing; building $Configuration|$Platform first..." -ForegroundColor Yellow
    & (Join-Path $PSScriptRoot 'build-check.ps1') -Configuration $Configuration -Platform $Platform
    if ($LASTEXITCODE -ne 0) { throw "Build failed (exit $LASTEXITCODE); cannot run." }
    if (-not (Test-Path $exe)) { throw "Build reported success but '$exe' is still missing." }
    return $exe
}

function ConvertTo-QuotedArg {
    param([string]$Value)
    if ($Value -match '\s') { return '"' + $Value + '"' }
    return $Value
}

function Invoke-Oxce {
    # Runs OpenXcom.exe headless and returns @{ ExitCode; StdOut; StdErr }.
    # Handles two Windows gotchas: the SDL DLLs live one dir up (added to PATH), and Start-Process
    # -ArgumentList does not quote array elements that contain spaces (we build a quoted string).
    param(
        [Parameter(Mandatory)] [string]$Exe,
        [string[]]$OxceArgs = @(),
        [string]$Platform = 'x64'
    )
    if (-not (Test-Path $Exe)) {
        throw "OpenXcom.exe not found at '$Exe'. Build it with scripts/build-check.ps1."
    }
    $dllDir = Get-OxceDllDir -Platform $Platform
    if (Test-Path $dllDir) { $env:PATH = "$dllDir;$env:PATH" }

    $argString = ($OxceArgs | ForEach-Object { ConvertTo-QuotedArg $_ }) -join ' '
    $outFile = [System.IO.Path]::GetTempFileName()
    $errFile = [System.IO.Path]::GetTempFileName()
    # Reliable output channel: a GUI (/SUBSYSTEM:WINDOWS) build's stdout can't be captured by a
    # launcher, so the engine also writes its report to the file named by OXCE_VERIFY_OUT.
    $reportFile = [System.IO.Path]::GetTempFileName()
    $env:OXCE_VERIFY_OUT = $reportFile
    try {
        $spArgs = @{
            FilePath               = $Exe
            Wait                   = $true
            NoNewWindow            = $true
            PassThru               = $true
            RedirectStandardOutput = $outFile
            RedirectStandardError  = $errFile
        }
        if ($argString) { $spArgs.ArgumentList = $argString }
        $p = Start-Process @spArgs
        $report = (Get-Content $reportFile -Raw -ErrorAction SilentlyContinue)
        $stdout = (Get-Content $outFile -Raw -ErrorAction SilentlyContinue)
        return [pscustomobject]@{
            ExitCode = $p.ExitCode
            # Prefer the file report; fall back to captured stdout (console/CI builds).
            StdOut   = if ($report) { $report } else { $stdout }
            StdErr   = (Get-Content $errFile -Raw -ErrorAction SilentlyContinue)
        }
    }
    finally {
        Remove-Item Env:\OXCE_VERIFY_OUT -ErrorAction SilentlyContinue
        Remove-Item $outFile, $errFile, $reportFile -ErrorAction SilentlyContinue
    }
}
