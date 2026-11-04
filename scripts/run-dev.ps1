<#
.SYNOPSIS
  Launch the PLAY PLAY stack for development: Supervisor first, then the
  operator app. Both inherit the Qt/FFmpeg bin dir on PATH so no deploy step
  is needed for a dev run.

.EXAMPLE
  .\scripts\run-dev.ps1                 # supervisor + app
  .\scripts\run-dev.ps1 -SupervisorOnly # just the supervisor (for REST testing)
  .\scripts\run-dev.ps1 -Channel main   # also spawn one engine via REST
#>
[CmdletBinding()]
param(
    [string]$BuildDir = '',
    [switch]$SupervisorOnly,
    [string]$Channel = '',
    [string]$QtRoot = $(if ($env:PLAYPLAY_QT_ROOT) { $env:PLAYPLAY_QT_ROOT } elseif ($env:CONDA_PREFIX) { Join-Path $env:CONDA_PREFIX 'Library' } else { Join-Path $env:USERPROFILE 'Miniconda3\Library' }),
    [int]$RestPort = 7700
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if (-not $BuildDir) { $BuildDir = Join-Path $root 'build' }

$supervisor = Join-Path $BuildDir 'PlayPlaySupervisor.exe'
$engine     = Join-Path $BuildDir 'PlayPlayChannelEngine.exe'
$app        = Join-Path $BuildDir 'PlayPlay.exe'
foreach ($exe in @($supervisor, $engine, $app)) {
    if (-not (Test-Path $exe)) { throw "Missing $exe - run scripts\build.ps1 first" }
}

$env:PATH = "$QtRoot\bin;$BuildDir;$env:PATH"
$env:QML2_IMPORT_PATH = "$QtRoot\lib\qt6\qml"
$env:QT_PLUGIN_PATH = "$QtRoot\lib\qt6\plugins"   # Library\plugins is Qt 5
$env:QT_FORCE_STDERR_LOGGING = '1'   # Supervisor/engine qDebug goes to the console, not the debugger
$dataDir = Join-Path $root 'channels'
New-Item -ItemType Directory -Force $dataDir | Out-Null

Write-Host "Starting Supervisor (REST :$RestPort, WS :7701)" -ForegroundColor Cyan
$sup = Start-Process -FilePath $supervisor -ArgumentList @('--engine-path', $engine, '--data-dir', $dataDir) `
    -WorkingDirectory $BuildDir -PassThru -NoNewWindow

# Wait for REST to answer.
$deadline = (Get-Date).AddSeconds(10)
$ready = $false
while ((Get-Date) -lt $deadline) {
    try {
        $r = Invoke-RestMethod -Uri "http://127.0.0.1:$RestPort/api/v1/status" -TimeoutSec 1
        $ready = $true; break
    } catch { Start-Sleep -Milliseconds 250 }
}
if (-not $ready) { Write-Warning 'Supervisor REST did not answer within 10 s' } else {
    Write-Host "Supervisor ready: $($r | ConvertTo-Json -Compress)" -ForegroundColor Green
}

if ($Channel) {
    Write-Host "Spawning channel '$Channel' via REST" -ForegroundColor Cyan
    $body = @{ id = $Channel; name = $Channel } | ConvertTo-Json
    Invoke-RestMethod -Method Post -Uri "http://127.0.0.1:$RestPort/api/v1/channels" -ContentType 'application/json' -Body $body | ConvertTo-Json -Compress
}

if ($SupervisorOnly) {
    Write-Host "Supervisor PID $($sup.Id). Press Ctrl+C to stop." -ForegroundColor Yellow
    Wait-Process -Id $sup.Id
    exit
}

Write-Host 'Starting operator app' -ForegroundColor Cyan
Start-Process -FilePath $app -WorkingDirectory $BuildDir -Wait
Write-Host 'App closed; stopping Supervisor' -ForegroundColor Yellow
Stop-Process -Id $sup.Id -Force -ErrorAction SilentlyContinue
