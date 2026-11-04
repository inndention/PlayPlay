param(
    [string]$QtRoot = $(if ($env:PLAYPLAY_QT_ROOT) { $env:PLAYPLAY_QT_ROOT } elseif ($env:CONDA_PREFIX) { Join-Path $env:CONDA_PREFIX 'Library' } else { Join-Path $env:USERPROFILE 'Miniconda3\Library' })
)
$ErrorActionPreference = 'Stop'
$buildDir = Join-Path (Split-Path -Parent $PSScriptRoot) 'build'
$dataDir = Join-Path $env:LOCALAPPDATA 'PlayPlay-Extras-Bench'
$ipcName = 'PlayPlayExtrasBench'
$restPort = 17897
$wsPort = 17898
$healthPort = 17899
$baseUrl = "http://127.0.0.1:$restPort"
$env:PATH = "$qtRoot\bin;$buildDir;$env:PATH"
$env:QML2_IMPORT_PATH = "$qtRoot\lib\qt6\qml"
$env:QT_PLUGIN_PATH = "$qtRoot\lib\qt6\plugins"

foreach ($path in @((Join-Path $buildDir 'PlayPlay.exe'), (Join-Path $buildDir 'PlayPlaySupervisor.exe'), (Join-Path $qtRoot 'bin\Qt6Core.dll'))) {
    if (-not (Test-Path -LiteralPath $path)) { throw "Required file not found: $path" }
}

$status = $null
try { $status = Invoke-RestMethod -Uri "$baseUrl/api/v1/status" -TimeoutSec 2 } catch {}
if ($status -and $status.ipcName -ne $ipcName) { throw "Port $restPort is already used by another Supervisor." }
if (-not $status) {
    $ports = Get-NetTCPConnection -LocalPort $restPort,$wsPort,$healthPort -State Listen -ErrorAction SilentlyContinue
    if ($ports) { throw "A PLAY PLAY extras port ($restPort, $wsPort, or $healthPort) is already in use." }
    $newDatabase = -not (Test-Path -LiteralPath (Join-Path $dataDir 'playplay.db'))
    New-Item -ItemType Directory -Force -Path $dataDir | Out-Null
    $supervisor = Start-Process -FilePath (Join-Path $buildDir 'PlayPlaySupervisor.exe') -WorkingDirectory $buildDir -WindowStyle Hidden -PassThru -ArgumentList @(
        '--data-dir', ('"' + $dataDir + '"'), '--rest-port', $restPort, '--ws-port', $wsPort,
        '--health-port', $healthPort, '--ipc-name', $ipcName, '--no-autostart', '--web-root', ('"' + (Join-Path $buildDir 'web') + '"')
    )
    for ($i = 0; $i -lt 40 -and -not $status; $i++) {
        Start-Sleep -Milliseconds 250
        if ($supervisor.HasExited) { throw 'The extras Supervisor exited before it was ready. Check the bench log directory.' }
        try { $status = Invoke-RestMethod -Uri "$baseUrl/api/v1/status" -TimeoutSec 1 } catch {}
    }
    if (-not $status -or $status.ipcName -ne $ipcName) { throw 'The extras Supervisor did not become ready.' }
    if ($newDatabase) {
        # A fresh database creates a main channel with a station NDI name; keep the bench output disabled.
        Invoke-RestMethod -Method Put -Uri "$baseUrl/api/v1/channels/main" -ContentType 'application/json' -Body '{"enabled":false,"outputs":[]}' | Out-Null
    }
}
Start-Process -FilePath (Join-Path $buildDir 'PlayPlay.exe') -WorkingDirectory $buildDir -ArgumentList @('--supervisor-port', $restPort, '--supervisor-ipc', $ipcName)
