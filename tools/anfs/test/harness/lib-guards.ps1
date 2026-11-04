# tools/anfs/test/harness/lib-guards.ps1 - the safety guards of the AN-FS compatibility harness (ANFS-F8,
# anfs-cpp.md section 7, layer 3 step 1). Dot-source it: . "$PSScriptRoot\lib-guards.ps1"
#
# Everything here protects the LIVE systems on this machine: the Node bridge (192.168.1.177:8080, example.invalid),
# the live Node peer on this PC (0.0.0.0:8899), its TLS front (443) and the PlayPlay Supervisor (7700/7701).
# The harness never contacts them and never kills a process it did not start.

Set-StrictMode -Version Latest

# Ports the harness refuses outright (the C++ test-mode guard refuses the first three as well, deviation C7).
$script:AnfsForbiddenPorts = @(8080, 8899, 443, 7700, 7701)
# Hosts the harness refuses outright; everything that is not loopback is refused too.
$script:AnfsForbiddenHosts = @('192.168.1.177', 'example.invalid')
# The ports the harness may use (tools/anfs/README.md "Reserved test ports").
$script:AnfsReservedPorts = @{
    cppServer = 18080; cppPeer = 18899; cppPeerRelay = 18898; simPeer = 18897; simBridge = 18081
    csServer = 18180; csPeer = 18199; tls = 18443
}

function Test-AnfsLoopbackHost {
    param([Parameter(Mandatory)][string]$HostName)
    $h = $HostName.Trim().ToLowerInvariant().Trim('[', ']')
    if ($h -eq 'localhost' -or $h -eq '::1') { return $true }
    return $h -match '^127\.\d{1,3}\.\d{1,3}\.\d{1,3}$'
}

# "" when the port is allowed, else the refusal text.
function Get-AnfsPortRefusal {
    param([Parameter(Mandatory)][int]$Port, [string]$Label = 'port')
    if ($Port -lt 1 -or $Port -gt 65535) { return "$Label $Port is out of range" }
    if ($script:AnfsForbiddenPorts -contains $Port) { return "$Label $Port belongs to the live stack (8080/8899/443/7700/7701) and is refused" }
    return ''
}

# "" when the URL is a loopback http(s) URL on an allowed port, else the refusal text.
function Get-AnfsUrlRefusal {
    param([Parameter(Mandatory)][string]$Url, [string]$Label = 'url')
    $u = $null
    if (-not [System.Uri]::TryCreate($Url, [System.UriKind]::Absolute, [ref]$u)) { return "$Label '$Url' is not an absolute URL" }
    if ($u.Scheme -ne 'http' -and $u.Scheme -ne 'https') { return "$Label '$Url' is not http(s)" }
    $h = $u.Host.ToLowerInvariant()
    if ($script:AnfsForbiddenHosts -contains $h) { return "$Label host $h is the live Node bridge and is refused" }
    if (-not (Test-AnfsLoopbackHost $h)) { return "$Label host $h is not loopback; the harness talks to 127.0.0.1 only" }
    $p = Get-AnfsPortRefusal -Port $u.Port -Label "$Label port"
    if ($p) { return $p }
    return ''
}

# The PID owning a listening TCP port on any local address, 0 when free. Uses Get-NetTCPConnection (no netstat parse).
function Get-AnfsPortOwnerPid {
    param([Parameter(Mandatory)][int]$Port)
    try {
        $c = Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction Stop | Select-Object -First 1
        if ($c) { return [int]$c.OwningProcess }
    } catch { }
    return 0
}

# Throws when any configured port or URL is refused, or when a port is held by a process the harness did not start
# (OwnedPids = PIDs this harness launched; those may own their ports on a re-check).
function Assert-AnfsSafeConfig {
    param(
        [hashtable]$Ports = @{},
        [hashtable]$Urls = @{},
        [int[]]$OwnedPids = @(),
        [switch]$SkipOwnerCheck
    )
    $problems = @()
    foreach ($k in $Ports.Keys) {
        $r = Get-AnfsPortRefusal -Port ([int]$Ports[$k]) -Label $k
        if ($r) { $problems += $r }
    }
    foreach ($k in $Urls.Keys) {
        $r = Get-AnfsUrlRefusal -Url ([string]$Urls[$k]) -Label $k
        if ($r) { $problems += $r }
    }
    if (-not $SkipOwnerCheck -and $problems.Count -eq 0) {
        foreach ($k in $Ports.Keys) {
            $owner = Get-AnfsPortOwnerPid -Port ([int]$Ports[$k])
            if ($owner -ne 0 -and $OwnedPids -notcontains $owner) {
                $name = try { (Get-Process -Id $owner -ErrorAction Stop).ProcessName } catch { '?' }
                $problems += "$k port $($Ports[$k]) is held by PID $owner ($name), which this harness did not start"
            }
        }
    }
    if ($problems.Count) { throw ("harness guard: " + ($problems -join '; ')) }
}

# Stops only processes this harness started: by the recorded PID, re-checked against the recorded start time and
# executable so a reused PID is never killed (memory note "re-check PIDs before killing").
function Stop-AnfsOwnedProcess {
    param([Parameter(Mandatory)][hashtable]$Record, [int]$GraceMs = 3000)
    $p = $null
    try { $p = Get-Process -Id $Record.Pid -ErrorAction Stop } catch { return 'gone' }
    $samePath = $true
    try {
        # Full-path, case-insensitive: the recorded path may carry forward slashes (a CMake-defined path).
        if ($p.Path -and $Record.Path) {
            $a = [System.IO.Path]::GetFullPath($p.Path).ToLowerInvariant()
            $b = [System.IO.Path]::GetFullPath($Record.Path).ToLowerInvariant()
            if ($a -ne $b) { $samePath = $false }
        }
    } catch { }
    $sameStart = $true
    try { if ($Record.StartTime -and ([math]::Abs(($p.StartTime - $Record.StartTime).TotalSeconds) -gt 2)) { $sameStart = $false } } catch { }
    if (-not $samePath -or -not $sameStart) { return "pid $($Record.Pid) reused by another process; left alone" }
    try { $p.CloseMainWindow() | Out-Null } catch { }
    if (-not $p.WaitForExit($GraceMs)) { Stop-Process -Id $Record.Pid -Force -ErrorAction SilentlyContinue; $p.WaitForExit(5000) | Out-Null }
    return 'stopped'
}
