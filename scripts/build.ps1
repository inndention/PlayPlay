<#
.SYNOPSIS
  Configure, build, test, and stage the PLAY PLAY playout stack.

.EXAMPLE
  .\scripts\build.ps1                 # Release build into .\build
  .\scripts\build.ps1 -Config Debug   # Debug build into .\build-debug
  .\scripts\build.ps1 -Clean          # wipe the build dir first
  .\scripts\build.ps1 -NoTests        # skip ctest
  .\scripts\build.ps1 -Deploy         # also run windeployqt6 (QML + plugins)
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')]
    [string]$Config = 'Release',
    [switch]$Clean,
    [switch]$NoTests,
    [switch]$Deploy,
    [string]$QtRoot = $(if ($env:PLAYPLAY_QT_ROOT) { $env:PLAYPLAY_QT_ROOT } elseif ($env:CONDA_PREFIX) { Join-Path $env:CONDA_PREFIX 'Library' } else { Join-Path $env:USERPROFILE 'Miniconda3\Library' }),
    [string]$VsDevShell = ''   # Launch-VsDevShell.ps1; found with vswhere when empty
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$buildDir = if ($Config -eq 'Release') { Join-Path $root 'build' } else { Join-Path $root "build-$($Config.ToLower())" }

# --- MSVC environment ------------------------------------------------------
if (-not $env:VCToolsInstallDir) {
    if (-not $vsDevShell) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        $vsPath = if (Test-Path $vswhere) { & $vswhere -latest -products * -property installationPath } else { $null }
        $vsDevShell = if ($vsPath) { Join-Path $vsPath 'Common7\Tools\Launch-VsDevShell.ps1' } else { 'C:\BuildTools\Common7\Tools\Launch-VsDevShell.ps1' }
    }
    if (-not (Test-Path $vsDevShell)) { throw "Visual Studio dev shell not found at $vsDevShell" }
    & $vsDevShell -Arch amd64 -SkipAutomaticLocation 2>$null | Out-Null
}
if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) { throw 'ninja not found on PATH' }
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) { throw 'cmake not found on PATH' }

# --- Configure --------------------------------------------------------------
if ($Clean -and (Test-Path $buildDir)) {
    Write-Host "Cleaning $buildDir" -ForegroundColor Yellow
    Remove-Item -Recurse -Force $buildDir
}
Write-Host "Configuring ($Config) -> $buildDir" -ForegroundColor Cyan
cmake -S $root -B $buildDir -G Ninja "-DCMAKE_BUILD_TYPE=$Config" "-DPLAYPLAY_QT_ROOT=$QtRoot"
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }

# --- Build ------------------------------------------------------------------
Write-Host 'Building' -ForegroundColor Cyan
cmake --build $buildDir --parallel
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }

# --- Test -------------------------------------------------------------------
if (-not $NoTests) {
    Write-Host 'Running tests' -ForegroundColor Cyan
    $env:PATH = "$QtRoot\bin;$env:PATH"
    ctest --test-dir $buildDir --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed' }
}

# --- Deploy Qt runtime (optional) --------------------------------------------
if ($Deploy) {
    $windeploy = Join-Path $QtRoot 'bin\windeployqt6.exe'
    if (-not (Test-Path $windeploy)) { throw "windeployqt6 not found at $windeploy" }
    Write-Host 'Deploying Qt runtime' -ForegroundColor Cyan
    & $windeploy --release --qmldir (Join-Path $root 'Play_Play_Native\qml') `
        --no-translations --no-opengl-sw (Join-Path $buildDir 'PlayPlay.exe')
    & $windeploy --release --no-translations (Join-Path $buildDir 'PlayPlaySupervisor.exe')
    & $windeploy --release --no-translations (Join-Path $buildDir 'PlayPlayChannelEngine.exe')
}

Write-Host "Done. Executables in $buildDir" -ForegroundColor Green
