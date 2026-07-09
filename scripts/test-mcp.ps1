# scripts/test-mcp.ps1
# [AI-MODS] Autonomous end-to-end test for the OpenXcom AI MCP server.
#
# Runs the MCP server's offline self-check, then launches the engine in headless --restserver mode
# and drives the MCP server (over stdio, as a real MCP client would) through a full
# publish -> wait_for_alien_decision -> submit_alien_action round trip, verifying the action lands
# back in the engine. Needs Python on PATH and no game data.
#
# Exit code: 0 = all passed, 1 = a failure.
[CmdletBinding()]
param(
    [int]$Port = 8765,
    [string]$Configuration = 'Release',
    [string]$Platform = 'x64',
    [switch]$NoBuild
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\_common.ps1"

$repo = Get-RepoRoot
$mcp = Join-Path $repo 'mcp-server\openxcom_ai_mcp.py'
$itest = Join-Path $repo 'mcp-server\test_integration.py'

# 0. Offline self-check (no engine needed).
Write-Host "=== MCP self-check ===" -ForegroundColor Cyan
python $mcp --self-check
if ($LASTEXITCODE -ne 0) { Write-Host "MCP self-check failed." -ForegroundColor Red; exit 1 }

# 1. Launch the engine in headless mock-harness mode.
$exe = Ensure-OxceBuilt -Configuration $Configuration -Platform $Platform -NoBuild:$NoBuild
$dllDir = Get-OxceDllDir -Platform $Platform
if (Test-Path $dllDir) { $env:PATH = "$dllDir;$env:PATH" }
$base = "http://127.0.0.1:$Port"

Write-Host "`n=== launching engine: --restserver --restai-port $Port ===" -ForegroundColor Cyan
$proc = Start-Process -FilePath $exe -PassThru -WindowStyle Hidden -ArgumentList @(
    '--restserver', '--restai-port', "$Port", '--restserver-seconds', '40')

$code = 1
try {
    # Wait for /health.
    $ready = $false
    for ($i = 0; $i -lt 100; $i++) {
        try { if ((Invoke-WebRequest "$base/health" -TimeoutSec 2 -UseBasicParsing).StatusCode -eq 200) { $ready = $true; break } }
        catch { Start-Sleep -Milliseconds 150 }
    }
    if (-not $ready) { Write-Host "Engine did not become reachable." -ForegroundColor Red; throw "engine unreachable" }

    # 2. Drive the MCP server end-to-end against the running engine.
    Write-Host "`n=== MCP integration test ===" -ForegroundColor Cyan
    python $itest --host 127.0.0.1 --port $Port
    $code = $LASTEXITCODE
}
finally {
    try { Invoke-WebRequest "$base/shutdown" -Method Post -TimeoutSec 2 -UseBasicParsing | Out-Null } catch {}
    if (-not $proc.WaitForExit(8000)) { try { $proc.Kill() } catch {} }
}

if ($code -eq 0) { Write-Host "`nMCP END-TO-END PASSED" -ForegroundColor Green; exit 0 }
else { Write-Host "`nMCP END-TO-END FAILED" -ForegroundColor Red; exit 1 }
