# scripts/run-restai.ps1
# [AI-MODS] Autonomous cross-process API test for the rest-ai-server branch.
#
# Launches OpenXcom.exe in headless --restserver mode (which also exposes mock-harness endpoints:
# /publish, /last-action, /sample-request) and exercises the whole REST surface from a SEPARATE
# process, including a full publish -> pending-decision -> action -> last-action round trip. This
# proves the server + exchange + wire format work over a real socket, not just in-process. Needs no
# game data, so it is CI-safe.
#
# For a LIVE battle test instead, launch interactively:
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

# HTTP helper that returns @{ Status; Body } and never throws on 4xx/5xx.
# NOTE: $Body is intentionally untyped -- a [string]-typed param coerces a $null default to "",
# which would attach an (empty) body to GET requests and fail with "Cannot send a content-body
# with this verb-type".
function Req([string]$Method, [string]$Path, $Body = $null) {
    $params = @{ Uri = "$base$Path"; Method = $Method; TimeoutSec = 3; UseBasicParsing = $true }
    if ($null -ne $Body) { $params.Body = $Body; $params.ContentType = 'application/x-yaml' }
    try {
        $r = Invoke-WebRequest @params
        # For non-text content types (e.g. application/x-yaml) Invoke-WebRequest returns .Content as
        # a byte[]; decode it so body matching works instead of getting "114 101 113 ..." numbers.
        $content = $r.Content
        if ($content -is [byte[]]) { $content = [System.Text.Encoding]::UTF8.GetString($content) }
        return [pscustomobject]@{ Status = [int]$r.StatusCode; Body = [string]$content }
    } catch {
        $resp = $_.Exception.Response
        if ($resp) {
            $body = ''
            try { $body = (New-Object System.IO.StreamReader($resp.GetResponseStream())).ReadToEnd() } catch {}
            return [pscustomobject]@{ Status = [int]$resp.StatusCode.value__; Body = $body }
        }
        return [pscustomobject]@{ Status = 0; Body = $_.Exception.Message }
    }
}

Write-Host "Launching headless REST server: OpenXcom.exe --restserver --restai-port $Port"
$proc = Start-Process -FilePath $exe -PassThru -WindowStyle Hidden -ArgumentList @(
    '--restserver', '--restai-port', "$Port", '--restserver-seconds', '30')

try {
    # 1. Wait for the server to bind and answer /health.
    $ready = $false
    for ($i = 0; $i -lt 100; $i++) {
        if ((Req GET '/health').Status -eq 200) { $ready = $true; break }
        Start-Sleep -Milliseconds 150
    }
    Check $ready "GET /health reachable from a separate process (port $Port)"

    if ($ready) {
        # 2. /health body.
        $h = Req GET '/health'
        Check ($h.Body -match 'ok') "GET /health body reports ok"

        # 3. Idle -> 204.
        Check ((Req GET '/pending-decision').Status -eq 204) "GET /pending-decision is 204 when idle"

        # 4. Sample request payload is served and looks right.
        $s = Req GET '/sample-request'
        Check (($s.Status -eq 200) -and ($s.Body -match 'request:') -and ($s.Body -match 'STR_SECTOID')) `
            "GET /sample-request returns a representative payload"

        # 5. Full exchange round trip across the process boundary.
        $marker = "probe-" + [guid]::NewGuid().ToString('N').Substring(0, 8)
        $pub = Req POST '/publish' "request:`n  probe: $marker`n"
        Check ($pub.Status -eq 200) "POST /publish accepted"
        $pd = Req GET '/pending-decision'
        Check (($pd.Status -eq 200) -and ($pd.Body -match $marker)) `
            "GET /pending-decision returns the just-published request"
        $act = Req POST '/action' "action:`n  type: SNAPSHOT`n  target: {x: 7, y: 8, z: 1}`n"
        Check ($act.Status -eq 200) "POST /action accepted"
        $la = Req GET '/last-action'
        Check (($la.Status -eq 200) -and ($la.Body -match 'SNAPSHOT')) `
            "GET /last-action echoes the submitted action"

        # 6. Robustness: an unknown path 404s; a garbage action body is still accepted (parsing/
        #    validation happens later, on the game side, and degrades to idle).
        Check ((Req GET '/no-such-endpoint').Status -eq 404) "unknown path returns 404"
        Check ((Req POST '/action' "this is not: valid: yaml [[[").Status -eq 200) `
            "POST /action tolerates a malformed body (200)"
        $big = 'x' * 20000
        Check ((Req POST '/action' "action:`n  type: NONE`n  note: $big`n").Status -eq 200) `
            "POST /action accepts a large body"
    }
}
finally {
    # 7. Ask the server to exit cleanly, then make sure the process is gone.
    Req POST '/shutdown' | Out-Null
    if (-not $proc.WaitForExit(8000)) {
        Write-Host "Server did not exit within 8s; killing." -ForegroundColor Yellow
        try { $proc.Kill() } catch {}
    }
}
Check ($proc.HasExited) "server process exited cleanly after POST /shutdown"

if ($script:fail -eq 0) {
    Write-Host "REST API SMOKE PASSED (all checks)" -ForegroundColor Green
    exit 0
} else {
    Write-Host "REST API SMOKE FAILED ($script:fail check(s))" -ForegroundColor Red
    exit 1
}
