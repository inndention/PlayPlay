param(
    [string]$BuildDir = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build'),
    [string]$QtRoot = $(if ($env:PLAYPLAY_QT_ROOT) { $env:PLAYPLAY_QT_ROOT } elseif ($env:CONDA_PREFIX) { Join-Path $env:CONDA_PREFIX 'Library' } else { Join-Path $env:USERPROFILE 'Miniconda3\Library' }),
    [string]$FfmpegRoot = 'C:\SDKs\ffmpeg-9.0-win64-gpl-shared'
)
$ErrorActionPreference = 'Stop'
$dumpbin = (Get-Command dumpbin -ErrorAction SilentlyContinue).Source   # on PATH inside a Visual Studio developer shell
if (-not $dumpbin) { $dumpbin = Get-ChildItem 'C:\BuildTools\VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe' -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName }
if (-not $dumpbin) { throw 'dumpbin not found (run from a Visual Studio developer shell)' }
$sources = @((Join-Path $FfmpegRoot 'bin'), (Join-Path $QtRoot 'bin'))
$queue = [System.Collections.Queue]::new()
Get-ChildItem -LiteralPath $BuildDir -File | Where-Object { $_.Name -match '^PlayPlay(?:Supervisor|ChannelEngine|Recompose|Studio|MediaTool|Mux|Speech|SpotRender|Watchdog|ButtonGallery)?\.exe$' -or $_.Extension -eq '.dll' } | ForEach-Object { $queue.Enqueue($_.FullName) }
$seen = @{}
$missing = @{}
while ($queue.Count) {
    $file = $queue.Dequeue()
    if ($seen[$file]) { continue }
    $seen[$file] = $true
    $deps = (& $dumpbin /nologo /dependents $file 2>$null) | Where-Object { $_ -match '^\s+\S+\.dll\s*$' } | ForEach-Object { $_.Trim() }
    foreach ($dep in $deps) {
        $target = Join-Path $BuildDir $dep
        if (Test-Path -LiteralPath $target) { $queue.Enqueue($target); continue }
        if ($dep -like 'api-ms-*' -or $dep -like 'ext-ms-*' -or (Test-Path -LiteralPath (Join-Path $env:SystemRoot "System32\$dep"))) { continue }
        $source = $sources | ForEach-Object { Join-Path $_ $dep } | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
        if ($source) { Copy-Item -LiteralPath $source -Destination $target; $queue.Enqueue($target); Write-Host "Staged $dep" }
        else { $missing[$dep] = $true }
    }
}
if ($missing.Count) { Write-Warning ('Dependencies not found: ' + (($missing.Keys | Sort-Object) -join ', ')) }
