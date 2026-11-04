<#
.SYNOPSIS
  Build a self-contained PLAY PLAY package: executables, Qt + FFmpeg + NDI runtime, QML, web dashboard,
  default config, and the service install scripts. Produces dist\PlayPlay-<version>\ and a .zip; if Inno Setup
  (ISCC.exe) is installed, also compiles installer\playplay.iss into a setup .exe.

.EXAMPLE
  .\scripts\package.ps1
  .\scripts\package.ps1 -Version 2.0.1 -SkipBuild
#>
param(
    [string]$Version = '3.0.0',
    [switch]$SkipBuild,
    [string]$BuildDir = '',
    [string]$QtRoot = $(if ($env:PLAYPLAY_QT_ROOT) { $env:PLAYPLAY_QT_ROOT } elseif ($env:CONDA_PREFIX) { Join-Path $env:CONDA_PREFIX 'Library' } else { Join-Path $env:USERPROFILE 'Miniconda3\Library' }),
    [string]$FfmpegRoot = 'C:\SDKs\ffmpeg-9.0-win64-gpl-shared',
    [string]$LibObsRoot = 'D:\SDKs\obs\install',
    [string]$NdiRoot = 'C:\Program Files\NDI\NDI 6 SDK'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$build = if ($BuildDir) { [IO.Path]::GetFullPath($BuildDir) } else { Join-Path $root 'build' }
$dist = Join-Path $root "dist\PlayPlay-$Version"
if ($BuildDir -and -not $SkipBuild) { throw 'BuildDir requires -SkipBuild; build that directory before packaging.' }
if (-not $SkipBuild) { & (Join-Path $root 'scripts\build.ps1') -NoTests }
if (Test-Path $dist) { throw "Package directory already exists: $dist. Choose a new version or move the existing package." }
New-Item -ItemType Directory -Force $dist | Out-Null

# Executables + the runtime DLLs the build already stages next to them
foreach ($f in @('PlayPlay.exe', 'PlayPlaySupervisor.exe', 'PlayPlayChannelEngine.exe', 'qt.conf')) { Copy-Item (Join-Path $build $f) $dist }
Get-ChildItem $build -Filter '*.dll' | Copy-Item -Destination $dist
foreach ($d in @('platforms', 'sqldrivers', 'imageformats', 'web')) { if (Test-Path (Join-Path $build $d)) { Copy-Item -Recurse (Join-Path $build $d) (Join-Path $dist $d) } }
if (Test-Path (Join-Path $dist 'web')) { Remove-Item -LiteralPath (Join-Path $dist 'web') -Recurse -Force }
Copy-Item -LiteralPath (Join-Path $root 'web') -Destination (Join-Path $dist 'web') -Recurse
foreach ($f in @('PlayPlayMediaTool.exe','PlayPlayMux.exe','PlayPlayStudio.exe','PlayPlayRecompose.exe','PlayPlaySpeech.exe','PlayPlaySpotRender.exe','PlayPlayWatchdog.exe','anfs.exe')) {
    if (Test-Path (Join-Path $build $f)) { Copy-Item (Join-Path $build $f) $dist }
}
if (Test-Path (Join-Path $LibObsRoot 'bin\64bit\obs.dll')) {
    $obsDest = Join-Path $dist 'libobs'
    New-Item -ItemType Directory -Force (Join-Path $obsDest 'bin\64bit') | Out-Null
    Get-ChildItem (Join-Path $LibObsRoot 'bin\64bit') -Filter '*.dll' | Copy-Item -Destination (Join-Path $obsDest 'bin\64bit')
    Copy-Item (Join-Path $LibObsRoot 'core') $obsDest -Recurse
    Copy-Item (Join-Path $LibObsRoot 'data') $obsDest -Recurse
}
# Stage helper executables before the dependency walk, so their imports are checked too.
foreach ($tool in @('ffmpeg.exe', 'ffprobe.exe')) {
    $source = Join-Path $FfmpegRoot ("bin\" + $tool)
    if (Test-Path $source) { Copy-Item $source $dist }
}

# Qt runtime for the operator app (QML modules, plugins, translations)
$windeploy = Join-Path $QtRoot 'bin\windeployqt6.exe'
$qtpaths = Join-Path $QtRoot 'bin\qtpaths6.exe'
if (Test-Path $windeploy) {
    # conda's windeployqt cannot locate qtpaths on its own; pass it explicitly.
    & $windeploy --qtpaths $qtpaths --qmldir (Join-Path $root 'Play_Play_Native\qml') --no-translations --no-opengl-sw (Join-Path $dist 'PlayPlay.exe') | Out-Null
    & $windeploy --qtpaths $qtpaths --no-translations (Join-Path $dist 'PlayPlaySupervisor.exe') | Out-Null
    & $windeploy --qtpaths $qtpaths --no-translations (Join-Path $dist 'PlayPlayChannelEngine.exe') | Out-Null
} else { Write-Warning "windeployqt6 not found at $windeploy; Qt plugins/QML not deployed" }
# The style is picked at runtime (QQuickStyle::setStyle("Fusion")), which the qmldir scan cannot see:
# ship the complete Controls and Templates module trees so any style resolves. QtQuick\Shapes: the PlayPlay.Buttons icons
# (qrc:/qml/PlayPlay/Buttons/Icon.qml) import it from qrc, which the --qmldir scan of the source tree may miss.
$qmlSrc = Join-Path $QtRoot 'lib\qt6\qml'
foreach ($m in @('QtQuick\Controls', 'QtQuick\Templates', 'QtQuick\Layouts', 'QtQuick\Window', 'QtQuick\Shapes', 'QtQml', 'QtQml\Models', 'QtQml\WorkerScript')) {
    $src = Join-Path $qmlSrc $m; $dst = Join-Path $dist ("qml\" + $m)
    if (Test-Path $src) { New-Item -ItemType Directory -Force (Split-Path -Parent $dst) | Out-Null; Copy-Item -Recurse -Force $src (Split-Path -Parent $dst) }
}
# qt.conf in the package must point at the package itself, not the conda prefix
@"
[Paths]
Prefix=.
Plugins=.
QmlImports=qml
"@ | Set-Content (Join-Path $dist 'qt.conf')

# Only the SQLite driver is used; drop the others windeployqt pulled in (they drag unrelated client libraries).
Get-ChildItem (Join-Path $dist 'sqldrivers') -Filter *.dll -ErrorAction SilentlyContinue | Where-Object { $_.Name -notlike 'qsqlite*' } | Remove-Item -Force

# Resolve every DLL the executables import (FFmpeg pulls in codec libraries such as x264, dav1d,
# opus, vpx, zlib...) by walking the import tables with dumpbin and copying from the conda prefix.
$dumpbin = Get-ChildItem 'C:\BuildTools\VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe' -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName
if (-not $dumpbin) { $dumpbin = (Get-Command dumpbin -ErrorAction SilentlyContinue).Source }
if ($dumpbin) {
    $srcBins = @((Join-Path $FfmpegRoot 'bin'), (Join-Path $QtRoot 'bin'))   # FFmpeg 9 first, then the conda prefix (Qt and its deps)
    $have = @{}; Get-ChildItem $dist -Filter *.dll | ForEach-Object { $have[$_.Name.ToLower()] = $true }
    $queue = New-Object System.Collections.Queue
    Get-ChildItem $dist -Include *.exe, *.dll -Recurse | ForEach-Object { $queue.Enqueue($_.FullName) }
    $seen = @{}
    while ($queue.Count -gt 0) {
        $f = $queue.Dequeue(); if ($seen[$f]) { continue }; $seen[$f] = $true
        $deps = (& $dumpbin /nologo /dependents $f 2>$null) | Where-Object { $_ -match '^\s+\S+\.dll\s*$' } | ForEach-Object { $_.Trim() }
        foreach ($d in $deps) {
            $dl = $d.ToLower()
            if ($have[$dl]) { continue }
            if ($dl -like 'api-ms-*' -or $dl -like 'ext-ms-*' -or (Test-Path (Join-Path $env:SystemRoot "System32\$d"))) { continue }
            $cand = $srcBins | ForEach-Object { Join-Path $_ $d } | Where-Object { Test-Path $_ } | Select-Object -First 1
            if ($cand) { Copy-Item $cand $dist; $have[$dl] = $true; $queue.Enqueue((Join-Path $dist $d)); Write-Host "  + $d" }
            else { Write-Warning "dependency not found: $d (needed by $(Split-Path -Leaf $f))" }
        }
    }
} else { Write-Warning 'dumpbin not found; transitive DLL dependencies were not resolved (run from a VS developer shell)' }

# NDI runtime (redistributable) and ffprobe for ingest
$ndiDll = Join-Path $NdiRoot 'Bin\x64\Processing.NDI.Lib.x64.dll'
if (Test-Path $ndiDll) { Copy-Item $ndiDll $dist }
$ffprobe = Join-Path $FfmpegRoot 'bin\ffprobe.exe'
if (Test-Path $ffprobe) { Copy-Item $ffprobe $dist }
$ffmpeg = Join-Path $FfmpegRoot 'bin\ffmpeg.exe'
if (Test-Path $ffmpeg) { Copy-Item $ffmpeg $dist }   # thumbnails + mux encoders
New-Item -ItemType Directory -Force (Join-Path $dist 'scripts') | Out-Null
Copy-Item (Join-Path $root 'scripts\transcribe.py') (Join-Path $dist 'scripts')   # faster-whisper worker

# Service helpers + docs
@'
@echo off
rem Install the PLAY PLAY Supervisor as a Windows service (run as Administrator).
"%~dp0PlayPlaySupervisor.exe" --install-service --data-dir "%ProgramData%\PlayPlay\supervisor"
'@ | Set-Content (Join-Path $dist 'install-service.cmd')
@'
@echo off
"%~dp0PlayPlaySupervisor.exe" --uninstall-service
'@ | Set-Content (Join-Path $dist 'uninstall-service.cmd')
Copy-Item (Join-Path $root 'docs\ROADMAP.md') (Join-Path $dist 'ROADMAP.md')
Copy-Item (Join-Path $root 'docs\port-parity-checklist.md') (Join-Path $dist 'PARITY.md')
@"
PLAY PLAY $Version
==================
PlayPlay.exe               operator app (launches a local Supervisor if none is running)
PlayPlaySupervisor.exe     automation host: schedule DB, engines, REST :7700, WebSocket :7701, web dashboard at http://<host>:7700/
PlayPlayChannelEngine.exe  one per channel, started by the Supervisor
install-service.cmd        run as Administrator to install the Supervisor as a Windows service
Data: %ProgramData%\PlayPlay\supervisor (service) or %AppData%\PlayPlay\PLAY PLAY\supervisor (app-launched)
API token: PUT /api/v1/settings/rest.token {"value":"..."}; the app reads PLAYPLAY_API_TOKEN.
HTML graphics require an installed Chrome or Microsoft Edge browser. Optional features may require hardware drivers, Python and faster-whisper, or external services.
"@ | Set-Content (Join-Path $dist 'README.txt')

$zip = "$dist.zip"
if (Test-Path $zip) { throw "Package archive already exists: $zip" }
Compress-Archive -Path "$dist\*" -DestinationPath $zip
$size = [math]::Round((Get-Item $zip).Length / 1MB, 1)
Write-Host "Package: $zip ($size MB), folder: $dist" -ForegroundColor Green

# Inno Setup (optional)
$iscc = @("$env:ProgramFiles(x86)\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
if ($iscc) {
    & $iscc "/DAppVersion=$Version" "/DSourceDir=$dist" "/DOutputDir=$(Join-Path $root 'dist')" (Join-Path $root 'installer\playplay.iss')
} else { Write-Host 'Inno Setup not installed; skipped setup.exe. See README for external prerequisites.' -ForegroundColor Yellow }
