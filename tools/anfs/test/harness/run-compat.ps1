<#
.SYNOPSIS
  AN-FS compatibility harness (ANFS-F8 skeleton; anfs-cpp.md section 7, layer 3).

.DESCRIPTION
  Starts the C++ anfs.exe (and later the C# reference, the sim peer and the sim bridge) on the reserved test ports,
  replays scenarios/*.json against them, diffs the answers and stops everything by recorded PID.

  This slice implements the frame: the guards (lib-guards.ps1), the scratch data folders with test:true configs,
  start/stop of the C++ server on 18080 by PID with a status wait, the report, -DryRun and -SmokeOnly. The scenario
  replay, the C# oracle, the sim peer matrix and -Soak are stubs that later slices fill in (scenarios/README.md).

  Never touches: 8080, 8899, 443, 7700, 7701, 192.168.1.177, example.invalid, any process it did not start.

.PARAMETER AnfsExe        anfs.exe to test (default: <repo>\build-anfs\anfs.exe)
.PARAMETER SimExe         anfs_sim.exe (default: beside AnfsExe); only checked for existence in this slice
.PARAMETER CsExe          the C# reference anfs.exe (optional; when missing the C# side is skipped)
.PARAMETER TokenFile      a file OUTSIDE the repo holding the agent token (never stored or printed)
.PARAMETER CppServerPort  default 18080
.PARAMETER CppPeerPort    default 18899
.PARAMETER CppRelayPort   default 18898
.PARAMETER SimPeerPort    default 18897
.PARAMETER SimBridgePort  default 18081
.PARAMETER CsServerPort   default 18180
.PARAMETER CsPeerPort     default 18199
.PARAMETER BridgeUrl      the bridge URL the C++ peer would use (default http://127.0.0.1:<CppServerPort>)
.PARAMETER DryRun         run the guards, create the scratch folders and configs, write the report; start nothing
.PARAMETER SmokeOnly      guards + start the C++ server, wait for status running, one GET, stop by PID, report
.PARAMETER Soak           the layer-4 soak (not implemented in this slice; reported as skipped)
.PARAMETER KeepScratch    keep $env:TEMP\anfs-compat-<pid> after the run

.OUTPUTS
  Exit 0 ok, 2 guard refusal or usage, 1 failure. report.md and report.json in the scratch folder.
#>
[CmdletBinding()]
param(
    [string]$AnfsExe = '',
    [string]$SimExe = '',
    [string]$CsExe = '',
    [string]$TokenFile = '',
    [int]$CppServerPort = 18080,
    [int]$CppPeerPort = 18899,
    [int]$CppRelayPort = 18898,
    [int]$SimPeerPort = 18897,
    [int]$SimBridgePort = 18081,
    [int]$CsServerPort = 18180,
    [int]$CsPeerPort = 18199,
    [string]$BridgeUrl = '',
    [switch]$DryRun,
    [switch]$SmokeOnly,
    [switch]$Soak,
    [switch]$KeepScratch,
    [int]$StartTimeoutSec = 20
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\lib-guards.ps1"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
if (-not $AnfsExe) { $AnfsExe = Join-Path $repoRoot 'build-anfs\anfs.exe' }
if (-not $SimExe) { $SimExe = Join-Path (Split-Path $AnfsExe -Parent) 'anfs_sim.exe' }
if (-not $BridgeUrl) { $BridgeUrl = "http://127.0.0.1:$CppServerPort" }

$scratch = Join-Path $env:TEMP ("anfs-compat-" + $PID)
$report = [ordered]@{
    startedAt = (Get-Date).ToString('o'); mode = $(if ($DryRun) { 'dry-run' } elseif ($SmokeOnly) { 'smoke' } else { 'full' })
    scratch = $scratch; anfsExe = $AnfsExe; guards = 'pending'; steps = @(); processes = @(); result = 'pending'
}
$started = @()     # records {Name, Pid, Path, StartTime, Port, DataDir}
$exitCode = 0

function Add-Step([string]$name, [string]$status, [string]$detail = '') {
    $script:report.steps += [ordered]@{ name = $name; status = $status; detail = $detail }
    Write-Host ("[{0}] {1}{2}" -f $status, $name, $(if ($detail) { " - $detail" } else { '' }))
}

function Write-Report {
    try {
        New-Item -ItemType Directory -Force $scratch | Out-Null
        $script:report.finishedAt = (Get-Date).ToString('o')
        ($script:report | ConvertTo-Json -Depth 6) | Set-Content -Encoding UTF8 (Join-Path $scratch 'report.json')
        $md = @("# AN-FS compat run ($($script:report.mode))", '', "- started: $($script:report.startedAt)", "- result: $($script:report.result)", "- guards: $($script:report.guards)", '', '| step | status | detail |', '|---|---|---|')
        foreach ($s in $script:report.steps) { $md += "| $($s.name) | $($s.status) | $($s.detail) |" }
        $md | Set-Content -Encoding UTF8 (Join-Path $scratch 'report.md')
        Write-Host "report: $(Join-Path $scratch 'report.md')"
    } catch { Write-Warning "report not written: $_" }
}

# ---- 1. guards ------------------------------------------------------------------------------------------------------
try {
    $ports = @{ CppServerPort = $CppServerPort; CppPeerPort = $CppPeerPort; CppRelayPort = $CppRelayPort
                SimPeerPort = $SimPeerPort; SimBridgePort = $SimBridgePort; CsServerPort = $CsServerPort; CsPeerPort = $CsPeerPort }
    $urls = @{ BridgeUrl = $BridgeUrl }
    # A dry run checks the values only; a real run also refuses ports held by processes it did not start.
    Assert-AnfsSafeConfig -Ports $ports -Urls $urls -SkipOwnerCheck:$DryRun
    if ($TokenFile) {
        $tokAbs = [System.IO.Path]::GetFullPath($TokenFile)
        if ($tokAbs.ToLowerInvariant().StartsWith($repoRoot.ToLowerInvariant())) { throw "harness guard: TokenFile must live outside the repository" }
        if (-not (Test-Path $tokAbs)) { throw "harness guard: TokenFile not found" }
    }
    $report.guards = 'passed'
    Add-Step 'guards' 'ok' "ports $($ports.Values -join ',') and $BridgeUrl allowed"
} catch {
    $report.guards = "refused: $_"
    $report.result = 'refused'
    Add-Step 'guards' 'refused' "$_"
    Write-Report
    # Nothing was started and the report is on the console: no scratch folder is left behind by a refusal.
    if (-not $KeepScratch) { Remove-Item -Recurse -Force $scratch -ErrorAction SilentlyContinue }
    exit 2
}

# ---- 2. scratch folders and configs ------------------------------------------------------------------------------
try {
    $dirs = @{}
    foreach ($n in 'cpp-server', 'cpp-peer', 'cpp-peer-relay', 'cs-server', 'cs-peer', 'sim', 'fixtures') {
        $dirs[$n] = Join-Path $scratch $n
        New-Item -ItemType Directory -Force $dirs[$n] | Out-Null
    }
    # test:true configs (the C++ test-mode guard then refuses 8080/8899/443 itself as well). The token, when given,
    # goes into the config files only (scratch folder, deleted after the run unless -KeepScratch).
    $token = ''
    if ($TokenFile) { $token = (Get-Content $TokenFile | Where-Object { $_.Trim() } | Select-Object -First 1).Trim() }
    $cfg = [ordered]@{ mode = 'server'; port = $CppServerPort; bindAll = $false; bindAddress = '127.0.0.1'; test = $true
                       statusPort = 0; nodeId = 'compat-cpp-server'; name = 'compat-cpp-server'; rev = 1 }
    if ($token) { $cfg.agentToken = $token }
    ($cfg | ConvertTo-Json) | Set-Content -Encoding UTF8 (Join-Path $dirs['cpp-server'] 'anfs.json')
    $peerCfg = [ordered]@{ mode = 'peer'; port = $CppPeerPort; bindAll = $false; bindAddress = '127.0.0.1'; test = $true
                           bridgeUrl = $BridgeUrl; nodeId = 'compat-cpp-peer'; name = 'compat-cpp-peer'; rev = 1 }
    if ($token) { $peerCfg.agentToken = $token }
    ($peerCfg | ConvertTo-Json) | Set-Content -Encoding UTF8 (Join-Path $dirs['cpp-peer'] 'anfs.json')
    $relayCfg = [ordered]@{ mode = 'peer'; port = $CppRelayPort; bindAll = $false; bindAddress = '127.0.0.1'; test = $true
                            bridgeUrl = $BridgeUrl; relayOnly = $true; nodeId = 'compat-cpp-relay'; name = 'compat-cpp-relay'; rev = 1 }
    if ($token) { $relayCfg.agentToken = $token }
    ($relayCfg | ConvertTo-Json) | Set-Content -Encoding UTF8 (Join-Path $dirs['cpp-peer-relay'] 'anfs.json')
    $token = $null
    Add-Step 'scratch' 'ok' $scratch
} catch {
    Add-Step 'scratch' 'failed' "$_"
    $report.result = 'failed'
    Write-Report
    exit 1
}

if (-not (Test-Path $AnfsExe)) {
    Add-Step 'anfs.exe' $(if ($DryRun) { 'missing' } else { 'failed' }) $AnfsExe
    if (-not $DryRun) { $report.result = 'failed'; Write-Report; exit 1 }
} else { Add-Step 'anfs.exe' 'ok' $AnfsExe }
Add-Step 'anfs_sim.exe' $(if (Test-Path $SimExe) { 'ok' } else { 'missing' }) $SimExe
Add-Step 'C# oracle' $(if ($CsExe -and (Test-Path $CsExe)) { 'ok' } else { 'skipped' }) $(if ($CsExe) { $CsExe } else { 'not given' })

if ($DryRun) {
    Add-Step 'start' 'skipped' 'dry run'
    Add-Step 'scenarios' 'skipped' 'dry run'
    $report.result = 'dry-run'
    Write-Report
    if (-not $KeepScratch) { Remove-Item -Recurse -Force $scratch -ErrorAction SilentlyContinue }
    exit 0
}

# ---- 3. start the C++ server by PID -----------------------------------------------------------------------------
function Start-Owned([string]$name, [string]$exe, [string[]]$argv, [int]$port, [string]$dataDir) {
    $out = Join-Path $scratch "$name.out.log"
    $err = Join-Path $scratch "$name.err.log"
    $p = Start-Process -FilePath $exe -ArgumentList $argv -PassThru -NoNewWindow -RedirectStandardOutput $out -RedirectStandardError $err
    $rec = @{ Name = $name; Pid = $p.Id; Path = $exe; StartTime = $(try { $p.StartTime } catch { $null }); Port = $port; DataDir = $dataDir }
    $script:started += $rec
    $script:report.processes += [ordered]@{ name = $name; pid = $p.Id; port = $port }
    return $rec
}

function Wait-AnfsRunning([hashtable]$rec, [int]$timeoutSec) {
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    while ((Get-Date) -lt $deadline) {
        $p = Get-Process -Id $rec.Pid -ErrorAction SilentlyContinue
        if (-not $p) { return "exited early (see $($rec.Name).err.log)" }
        $st = & $AnfsExe status --data $rec.DataDir 2>$null
        if ($LASTEXITCODE -eq 0 -and $st) {
            try {
                $j = $st | ConvertFrom-Json
                if ($j.running -and $j.port -eq $rec.Port -and $j.appliedRev -ge 1) { return '' }
            } catch { }
        }
        Start-Sleep -Milliseconds 250
    }
    return "not running after $timeoutSec s"
}

function Stop-AllOwned {
    foreach ($rec in [array]$script:started | Sort-Object { $_.Name } -Descending) {
        $r = Stop-AnfsOwnedProcess -Record $rec
        if ($r -eq 'stopped' -or $r -eq 'gone') {
            Add-Step "stop $($rec.Name) (pid $($rec.Pid))" 'ok' $r
        } else {
            # A process of ours that could not be stopped is never silent: the run fails and the scratch stays.
            Add-Step "stop $($rec.Name) (pid $($rec.Pid))" 'failed' $r
            $script:exitCode = 1
            $script:report.result = 'failed'
        }
    }
    $script:started = @()
}

try {
    $srv = Start-Owned 'cpp-server' $AnfsExe @('run', '--data', $dirs['cpp-server']) $CppServerPort $dirs['cpp-server']
    Add-Step 'start cpp-server' 'ok' "pid $($srv.Pid) port $CppServerPort"
    $w = Wait-AnfsRunning $srv $StartTimeoutSec
    if ($w) { throw "cpp-server: $w" }
    # The port is now owned by our own PID: the owner re-check must accept it.
    Assert-AnfsSafeConfig -Ports @{ CppServerPort = $CppServerPort } -OwnedPids @($srv.Pid)
    $owner = Get-AnfsPortOwnerPid -Port $CppServerPort
    Add-Step 'cpp-server running' 'ok' "status running, port $CppServerPort owned by pid $owner"
    try {
        $resp = Invoke-WebRequest -Uri "http://127.0.0.1:$CppServerPort/api/health" -UseBasicParsing -TimeoutSec 5 -SkipHttpErrorCheck
        Add-Step 'GET /api/health' 'ok' "HTTP $($resp.StatusCode)"
    } catch { Add-Step 'GET /api/health' 'failed' "$_"; throw }

    if ($SmokeOnly) {
        Add-Step 'scenarios' 'skipped' 'smoke only'
    } else {
        # ---- 4./5. scenario replay and the peer matrix: later slices (scenarios/README.md) ----
        $scen = Get-ChildItem (Join-Path $PSScriptRoot 'scenarios') -Filter '*.json' -ErrorAction SilentlyContinue
        Add-Step 'scenarios' 'skipped' ("replay not implemented in this slice; {0} scenario file(s) found" -f @($scen).Count)
        Add-Step 'peer matrix' 'skipped' 'anfs_sim peer / bridge wiring comes with W1 peer-link'
    }
    if ($Soak) { Add-Step 'soak' 'skipped' 'layer 4 not implemented in this slice' }
    $report.result = 'ok'
} catch {
    Add-Step 'run' 'failed' "$_"
    $report.result = 'failed'
    $exitCode = 1
} finally {
    Stop-AllOwned
    Write-Report
    if (-not $KeepScratch -and $exitCode -eq 0) { Remove-Item -Recurse -Force $scratch -ErrorAction SilentlyContinue }
}
exit $exitCode
