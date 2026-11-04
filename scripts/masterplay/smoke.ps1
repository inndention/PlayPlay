# MasterPlay REST smoke test (docs/plans/masterplay.md MP-11) against a TEST Supervisor, never the live one.
#
#   pwsh -NoProfile -File scripts\masterplay\smoke.ps1 -Port 7797 [-Channel <id>] [-Token <bearer>] [-Fixtures <dir>] [-NoApply]
#
# Walks the /api/v1/masterplay routes: config, parse, browse, dry-run import, an apply into today's hours (must answer
# 409: on air), an apply to a day two weeks out, history, undo, export (JSON and the .in download), as-run dry run,
# resolve, config/from-ini and links. The target channel is the first one the Supervisor lists unless -Channel names
# one; the apply only touches dated overrides of one far-future day and is undone at the end, so the copied test DB is
# left as found (the dated day is also deleted by the undo). -NoApply skips the two writes.
#
# Exit codes: 0 all steps passed, 1 a step failed, 2 the routes are not wired into that Supervisor (MP-12), 3 cannot
# reach the Supervisor. Refuses the live ports (7700).
param([int]$Port = 7797, [string]$Channel = '', [string]$Token = '', [string]$Fixtures = '', [switch]$NoApply, [string]$BindHost = '127.0.0.1')
$ErrorActionPreference = 'Stop'
if ($Port -in 7700, 7701, 8080, 8899, 443) { Write-Output "REFUSED: port $Port belongs to the live stack"; exit 3 }
if (-not $Fixtures) { $Fixtures = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'tests\masterplay\fixtures' }
$Fixtures = (Resolve-Path $Fixtures).Path
$base = "http://${BindHost}:$Port/api/v1"
$mp = "$base/masterplay"
$headers = @{ 'Content-Type' = 'application/json; charset=utf-8' }
if ($Token) { $headers['Authorization'] = "Bearer $Token" }

$script:pass = 0; $script:fail = 0
function Say($ok, $what, $detail = '') {
    if ($ok) { $script:pass++; Write-Output ("  ok   {0} {1}" -f $what, $detail) } else { $script:fail++; Write-Output ("  FAIL {0} {1}" -f $what, $detail) }
}
# One HTTP call: {status, body (parsed JSON or raw text), headers}. Never throws on a 4xx/5xx: the status is the result.
function Call($method, $url, $body = $null) {
    $args = @{ Method = $method; Uri = $url; Headers = $headers; TimeoutSec = 120; SkipHttpErrorCheck = $true }
    if ($null -ne $body) { $args['Body'] = [System.Text.Encoding]::UTF8.GetBytes((ConvertTo-Json $body -Depth 12 -Compress)) }
    $r = Invoke-WebRequest @args
    $text = $r.Content; if ($text -is [byte[]]) { $text = [System.Text.Encoding]::UTF8.GetString($text) }
    $json = $null
    try { if ($text -and $text.TrimStart().StartsWith('{')) { $json = $text | ConvertFrom-Json -AsHashtable } } catch {}
    return @{ status = [int]$r.StatusCode; body = $json; text = $text; headers = $r.Headers }
}

Write-Output "MasterPlay smoke against $mp (fixtures: $Fixtures)"
try { $st = Call GET "$base/status" } catch { Write-Output "cannot reach $base ($($_.Exception.Message))"; exit 3 }
if ($st.status -ne 200) { Write-Output "GET /status answered $($st.status) ($($st.text))"; exit 3 }

# 0. The routes exist at all (MP-12 wiring).
$info = Call GET $mp
if ($info.status -eq 404 -or -not $info.body -or $info.body.lane -ne 'masterplay') {
    Write-Output "GET /api/v1/masterplay answered $($info.status): the MasterPlay routes are not wired into this Supervisor (slice MP-12)."
    exit 2
}
Say $true 'GET /masterplay' "routes=$($info.body.routes.Count) pending=$($info.body.pending)"
$cat = Call GET "$mp/catalog"
Say ($cat.status -eq 200 -and $cat.body.instructions.Count -gt 20) 'GET /catalog' "instructions=$($cat.body.instructions.Count)"

# 1. The channel to write to: the first one listed (the copied test DB), never created here (that would spawn engines).
if (-not $Channel) {
    $ch = Call GET "$base/channels"
    $list = @(); if ($ch.body -is [hashtable] -and $ch.body.channels) { $list = @($ch.body.channels) } elseif ($ch.text.TrimStart().StartsWith('[')) { $list = @($ch.text | ConvertFrom-Json -AsHashtable) }
    if ($list.Count -gt 0) { $Channel = $list[0].id }
}
if (-not $Channel) { Write-Output 'no channel to target (-Channel <id>)'; exit 1 }
Write-Output "  target channel: $Channel"

# 2. Config: a file-system source over the fixture folder; read back whole.
$cfg = @{ sources = @(@{ id = 'smoke'; label = 'Smoke fixtures'; read = 'fs'; playlistDir = $Fixtures; logPath = (Join-Path $Fixtures 'LogFile.log'); osdDir = $Fixtures; tz = 'UTC' });
          pathMap = @(@{ from = 'D:\Media\Missing'; to = $Fixtures }); defaults = @{ missing = 'keepPath' }; asrun = @{ tz = 'UTC' } }
$saved = Call GET "$mp/config"
$r = Call PUT "$mp/config" $cfg
Say ($r.status -eq 200 -and $r.body.sources.Count -eq 1) 'PUT /config' "sources=$($r.body.sources.Count)"
$r = Call PUT "$mp/config" @{ sources = @(@{ id = 'x'; read = 'ftp' }) }
Say ($r.status -eq 400) 'PUT /config (bad read) -> 400' "got $($r.status)"
$r = Call GET "$mp/config"
Say ($r.status -eq 200 -and $r.body.sources[0].id -eq 'smoke') 'GET /config' ''

# 3. Parse a fixture through the source, and pasted text.
$r = Call POST "$mp/parse" @{ source = 'smoke'; path = 'import_times.in' }
Say ($r.status -eq 200 -and $r.body.events.Count -ge 9 -and $r.body.roundTrip -eq $true) 'POST /parse (source path)' "events=$($r.body.events.Count) roundTrip=$($r.body.roundTrip)"
$r = Call POST "$mp/parse" @{ text = "0000-00-00T00:00:00`tName=Pasted`tPlayMode=1`r`nD:\Media\Prog\Movie.mpg`r`n"; name = 'pasted.in' }
Say ($r.status -eq 200 -and $r.body.events.Count -eq 1) 'POST /parse (text)' "events=$($r.body.events.Count)"
$r = Call POST "$mp/parse" @{ source = 'smoke'; path = 'nope.in' }
Say ($r.status -eq 404) 'POST /parse (missing file) -> 404' "got $($r.status)"

# 4. Browse the source.
$r = Call GET "$mp/browse?source=smoke"
$ins = @($r.body.entries | Where-Object { $_.name -like '*.in' })
Say ($r.status -eq 200 -and $ins.Count -ge 10) 'GET /browse?source=smoke' ".in files=$($ins.Count)"

# 5. Import: dry run by default. A pasted every-day repeat event fills every hour, so a dated import of today always has
#    on-air hours (409 on apply) while a day two weeks out is safe to write and undo.
$text = "0000-00-00T00:00:00`tName=Smoke`tEventDuration=01:00:00`tPlayMode=1`r`n!Repeat`r`nD:\Media\Prog\Movie.mpg`t[00:30:00]`r`nD:\Media\Prog\Talk.mpg`t[00:30:00]`r`n"
$future = (Get-Date).ToUniversalTime().AddDays(14).ToString('yyyy-MM-dd')
$today = (Get-Date).ToString('yyyy-MM-dd')
$body = @{ text = $text; name = 'smoke.in'; channel = $Channel; mode = 'dates'; from = $future; to = $future; options = @{ missing = 'keepPath'; timeZone = 'UTC' } }
$r = Call POST "$mp/import" $body
$slots = 0; if ($r.body -and $r.body.plan -and $r.body.plan.slots) { $slots = @($r.body.plan.slots.Keys).Count }
Say ($r.status -eq 200 -and $r.body.dryRun -eq $true -and $slots -ge 1) 'POST /import (dry run)' "slots=$slots status=$($r.status) $($r.body.error)"
$r = Call POST "$mp/import" @{ text = $text; channel = 'no-such-channel-smoke'; mode = 'dates'; from = $future; to = $future }
Say ($r.status -eq 400) 'POST /import (unknown channel) -> 400' "got $($r.status)"
$importId = ''
if (-not $NoApply) {
    $onair = @{ text = $text; name = 'smoke.in'; channel = $Channel; mode = 'dates'; from = $today; to = $today; dryRun = $false; options = @{ missing = 'keepPath' } }
    $r = Call POST "$mp/import" $onair
    Say ($r.status -eq 409) 'POST /import (today, apply) -> 409 on air' "got $($r.status) $($r.body.error)"
    $apply = $body.Clone(); $apply['dryRun'] = $false; $apply['by'] = 'smoke.ps1'
    $r = Call POST "$mp/import" $apply
    $importId = "$($r.body.importId)"
    Say ($r.status -eq 200 -and $importId -and $r.body.written.Count -ge 1) 'POST /import (apply, +14 days)' "importId=$importId written=$($r.body.written.Count)"
    $r = Call GET "$mp/imports?limit=5"
    Say ($r.status -eq 200 -and @($r.body.imports | Where-Object { $_.id -eq $importId }).Count -eq 1) 'GET /imports' "count=$($r.body.imports.Count)"
    $r = Call GET "$mp/imports/$importId"
    Say ($r.status -eq 200 -and $r.body.backup) 'GET /imports/{id}' "backup slots=$($r.body.backup.slots)"
}

# 6. Export the (just written, or whatever is there) dated day: JSON and the raw download.
$r = Call POST "$mp/export" @{ channel = $Channel; mode = 'dates'; from = $future; to = $future; dateStyle = 'iso' }
Say ($r.status -eq 200 -and $r.body.ContainsKey('text')) 'POST /export' "events=$($r.body.stats.events) bytes=$("$($r.body.text)".Length)"
$r = Call GET "$mp/export.in?channel=$Channel&mode=dates&from=$future&to=$future"
$cd = "$($r.headers['Content-Disposition'])"
Say ($r.status -eq 200 -and $cd -like 'attachment*') 'GET /export.in (download)' "$cd"

# 7. Undo the apply: the dated day goes back to what the backup holds (nothing), a second undo answers 409.
if ($importId) {
    $r = Call POST "$mp/import/$importId/undo" @{ by = 'smoke.ps1' }
    Say ($r.status -eq 200) 'POST /import/{id}/undo' "restored=$($r.body.restored.Count) deleted=$($r.body.deleted.Count)"
    $r = Call POST "$mp/import/$importId/undo" @{}
    Say ($r.status -eq 409) 'undo twice -> 409' "got $($r.status)"
    $r = Call POST "$mp/import/nope/undo" @{}
    Say ($r.status -eq 404) 'undo unknown -> 404' "got $($r.status)"
}

# 8. As-run: dry run of the fixture LogFile.log (nothing inserted), state untouched.
$r = Call POST "$mp/asrun/import" @{ source = 'smoke'; statusPath = (Join-Path $Fixtures 'Status.log') }
Say ($r.status -eq 200 -and $r.body.dryRun -eq $true -and $r.body.rows) 'POST /asrun/import (dry run)' "played=$($r.body.rows.played) inserted=$($r.body.inserted)"
$r = Call GET "$mp/asrun/state"
Say ($r.status -eq 200 -and -not $r.body.state.ContainsKey('smoke')) 'GET /asrun/state (dry run left no state)' ''

# 9. Resolve, from-ini, links.
$r = Call POST "$mp/resolve" @{ paths = @('D:\Media\Prog\Movie.mpg', 'D:\Media\Missing\sample.in') }
Say ($r.status -eq 200 -and $r.body.results.Count -eq 2) 'POST /resolve' "results=$($r.body.results.Count)"
$r = Call POST "$mp/config/from-ini" @{ source = 'smoke'; path = 'mplay_sample.ini' }
Say ($r.status -eq 200 -and $r.body.source) 'POST /config/from-ini' "playlistDir=$($r.body.source.playlistDir)"
$r = Call POST "$mp/links" @{ channel = $Channel; subPath = 'sub_news.in'; slots = @('1-9'); source = 'smoke'; enabled = $false }
$linkId = "$($r.body.link.id)"
Say ($r.status -eq 200 -and $linkId) 'POST /links' "id=$linkId"
$r = Call GET "$mp/links"
Say ($r.status -eq 200 -and @($r.body.links | Where-Object { $_.id -eq $linkId }).Count -eq 1) 'GET /links' "count=$($r.body.links.Count)"
if ($linkId) { $r = Call DELETE "$mp/links/$linkId"; Say ($r.status -eq 200 -and $r.body.deleted) 'DELETE /links/{id}' '' }

# 10. Put the config back the way it was (the test DB is a copy, but keep the run repeatable).
if ($saved.status -eq 200 -and $saved.body) {
    $restore = @{}; foreach ($k in 'sources', 'pathMap', 'defaults', 'resolver', 'asrun') { if ($saved.body.ContainsKey($k)) { $restore[$k] = $saved.body[$k] } }
    $r = Call PUT "$mp/config" $restore
    Say ($r.status -eq 200) 'PUT /config (restored)' ''
}

Write-Output ("smoke: {0} passed, {1} failed" -f $script:pass, $script:fail)
if ($script:fail -gt 0) { exit 1 }
exit 0
