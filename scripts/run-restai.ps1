# scripts/run-restai.ps1
# [AI-MODS] Autonomous smoke test for the rest-ai-server branch.
#
# Launches OpenXcom.exe in headless --restserver mode and exercises the REST endpoints from a
# SEPARATE process, proving the embedded server is reachable over a real socket (not just the
# in-process loopback that the --selftest already covers). Needs no game data, so it is CI-safe.
#
# For a LIVE battle test instead, launch the engine interactively:
#     $env:PATH = "bin\x64;$env:PATH"; bin\x64\Release\OpenXcom.exe --restai
# start a New Battle, and once it is the alien turn drive it with scripts/mock-brain.py.
#
# Exit code: 0 = all checks passed, 1 = a check failed.
[CmdletBinding()]
param(
    [int]$Port = 8765,
    [string]$Configuration = 'Release',
    [string]$Platform = 'x64',
    [switch]$NoBuild
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\_common.ps1"

$exe = Ensure-OxceBuilt -Configuration $Configuration -Platform $Platform -NoBuild:$NoBuild
$dllDir = Get-OxceDllDir -Platform $Platform
if (Test-Path $dllDir) { $env:PATH = "$dllDir;$env:PATH" }

$base = "http://127.0.0.1:$Port"
$script:fail = 0
function Check([bool]$cond, [string]$msg) {
    if ($cond) { Write-Host "PASS $msg" -ForegroundColor Green }
    else { Write-Host "FAIL $msg" -ForegroundColor Red; $script:fail++ }
}

Write-Host "Launching headless REST server: OpenXcom.exe --restserver --restai-port $Port"
$proc = Start-Process -FilePath $exe -PassThru -WindowStyle Hidden -ArgumentList @(
    '--restserver', '--restai-port', "$Port", '--restserver-seconds', '30')

try {
    # 1. Wait for the server to bind and answer /health.
    $ready = $false
    for ($i = 0; $i -lt 100; $i++) {
        try {
            $r = Invoke-WebRequest -Uri "$base/health" -TimeoutSec 2 -UseBasicParsing
            if ($r.StatusCode -eq 200) { $ready = $true; break }
        } catch { Start-Sleep -Milliseconds 150 }
    }
    Check $ready "GET /health reachable from a separate process (port $Port)"

    if ($ready) {
        # 2. No decision pending -> 204 No Content.
        $code = 0
        try {
            $r = Invoke-WebRequest -Uri "$base/pending-decision" -TimeoutSec 2 -UseBasicParsing
            $code = $r.StatusCode
        } catch { $code = $_.Exception.Response.StatusCode.value__ }
        Check ($code -eq 204) "GET /pending-decision is 204 when idle (got $code)"

        # 3. An action POST is accepted.
        try {
            $r = Invoke-WebRequest -Uri "$base/action" -Method Post -ContentType 'application/x-yaml' `
                -Body "action:`n  type: NONE`n" -TimeoutSec 2 -UseBasicParsing
            Check ($r.StatusCode -eq 200) "POST /action accepted (200)"
        } catch { Check $false "POST /action accepted (threw: $($_.Exception.Message))" }
    }
}
finally {
    # 4. Ask the server to exit cleanly, then make sure the process is gone.
    try { Invoke-WebRequest -Uri "$base/shutdown" -Method Post -TimeoutSec 2 -UseBasicParsing | Out-Null } catch {}
    if (-not $proc.WaitForExit(8000)) {
        Write-Host "Server did not exit within 8s; killing." -ForegroundColor Yellow
        try { $proc.Kill() } catch {}
    }
}
Check ($proc.HasExited) "server process exited cleanly after POST /shutdown"

if ($script:fail -eq 0) {
    Write-Host "REST SMOKE PASSED" -ForegroundColor Green
    exit 0
} else {
    Write-Host "REST SMOKE FAILED ($script:fail check(s))" -ForegroundColor Red
    exit 1
}
