# Requirements status: scheduling, playback engine, media, graphics and control, reliability

Checked 2026-10-23 against the code (three read-only audits, then the changes listed under "Done in this pass"). Verdicts
come from reading the code and, where noted, from running it; `docs/port-parity-checklist.md` is older and in places
optimistic. **B** = built, **P** = partial, **M** = missing.

## Done in this pass

| Item | Where |
|---|---|
| **Hard-start interruption policy.** "Exact start" and "finish the program" cannot both win, so each hard-start item says which does: `conflictPolicy` = `truncate` (default, the clip is cut at the start), `finish_previous` (the clip finishes, the item starts late by at most `maxLateSec`, blank = any; a late start is not sent to the engine as an exact start), `drop_previous` (the clip is not played). | `RunlistBuilder::build`, web item properties |
| **Backtiming.** `backtime` on a hard-start item: the events before it are timed backward from it, so they end exactly on the start; the gap goes in front (filler when there is some, else the first item holds for its own exact start). | `RunlistBuilder::build`, web item properties |
| **Dropped events are recorded** (`Runlist.dropped`, with their full length), so overrun is measurable even when the builder leaves events out. | `runlistbuilder.*` |
| **Flight plan** (`playout/schedule/flightplan.*`, `GET /api/v1/flightplan/{ch}`, web page *Flight plan* with Operate, Plan and Fleet): now / next / later with live countdowns; clock anchors; **time debt** per interval ("3 m 12 s over before 1:06:40 PM (3 events will not air)"); the **safe edit boundary** (the on-air event plus `protectSec`, default 90 s); a **readiness strip** per event; **Why?** per event. Works with no engine connected (the hours are laid out from the stored schedule). | `flightplan.*`, `supervisorcore_flightplan.cpp`, `web/flight.js` |
| **Correction drawer.** For an overrun: drop optional events, trim events that declare a minimum length, swap an optional clip for a shorter one; the cheapest combination that covers the debt is pre-selected. For a gap: eligible clips that fit (approved, in window, allowed on the channel, least recently played first). Mandatory events (ads, station IDs, live, locked, or `mandatory`) and the protected window are never touched. | `FlightPlan::corrections`, `applyOps` |
| **Ghost preview and schedule diff.** `POST .../preview` applies the ops to copies and reports "1 removed, 1 moved, 3 added, 1 m 42 s gap left" with details and the new time debt; nothing is saved. | `FlightPlan::diff` |
| **Apply, audited.** `POST .../apply` saves the edited hours as dated overrides (a synthesized autofill hour is frozen from its build first; a touched query block is fixed to its current picks) and records who / what / why (`GET .../audit`). | `supervisorcore_flightplan.cpp` |
| **Pre-air readiness.** Library record, approval (rejected = fail, review = warning), library "missing" flag, file on disk and its size against the library record (a different size = copy in progress or replaced), availability window, channel scope, duration. `GET /api/v1/readiness`; a watcher checks the next two hours of every armed channel every 5 minutes and raises **one** alert per new problem (about hours ahead instead of the engine's 3 s preroll). | `FlightPlan::readiness`, watcher |
| **Disk alarm.** Free space where the database and the library live: warning under 20 GB, critical under 5 GB (settings `alerts.disk`). | watcher |
| **Accepted vs confirmed commands.** `POST /channels/{id}/transport` returns 503 when the engine is not connected (it used to say ok), 422 for a take that cannot be resolved, and `{accepted:true, confirmed:false, commandId}` otherwise; the confirmation is the engine's state change / as-run row. | `supervisorcore.cpp` |

Per-item keys are top-level in the slot JSON (unknown top-level keys are kept): `conflictPolicy`, `maxLateSec`, `backtime`,
`optional`, `mandatory`, `minDurationSec`, `canTrim`.

## Playback engine

| Item | | Notes |
|---|---|---|
| play, pause, stop, seek, skip/take next, goto | B | `engine.cpp executeCommand`; a take cuts in as soon as it is loaded |
| cue | P | no "load, park on frame 1, wait for take": cueing is the implicit preroll of the next item |
| preview separate from program | **M** | the preview tap is a JPEG of program; no preview bus |
| preload next item, gapless cut | B | `startPrepare` on a loader thread `prerollSec` ahead; `takePrepared` |
| no black or silence when the next item is late | P | the engine falls to the colour-bars slate at a missed cut |
| frame-rate conversion, scaling | B | drop/repeat (no blending); letterbox with bicubic scaling |
| interlaced / progressive | P | SDI outputs weave interlaced rasters; no deinterlace of interlaced sources |
| sample rate / channel mapping | P | resampled to 48 kHz; channel layout is FFmpeg's default for N channels, no explicit mapping |
| per-clip gain, mute, fades | P | layer gain / mute / ducking and master gain exist; the playlist item has no gain, mute or fade fields |
| in/out points, trims, loops | B | |
| still images as program items | P | likely ends after one frame (it is an `ImageSource` for layers only) |
| live inputs, timed live segments | B | live extension limits are not implemented |
| fallback at end of playlist | P | emergency loop at schedule level; the engine airs bars, not a freeze of the last frame |
| missing / corrupt media | B | FAILED as-run row, alert, next item prepared |
| stall and output-loss detection | P | decoder stall and sink loss are watched; a wedged clock thread is not |

## Playlist and scheduling

| Item | | Notes |
|---|---|---|
| drag-and-drop | P | files dropped from the desktop; reorder is by buttons |
| insert / delete / reorder / duplicate | B | replace-with-another-media is only available through the correction drawer |
| duration, remaining, estimated start, over/under | **B now** | flight plan (the start shown is the plan, not re-projected from the actual position) |
| follow / hard / soft | B | plus `fill_until` and locked soft anchors |
| hard start vs the clip playing | **B now** | per-item policy, see above (the engine itself still cuts at an exact start) |
| backtiming | **B now** | |
| manual holds and takes | B | a hold is not sticky: the next push leaves it |
| daily / weekly recurrence, date exceptions | P | weekly template + hand-entered dated overrides; no recurrence rules, end dates, holiday calendar, alternate weeks |
| breaks, promos, IDs, filler rotation | B | |
| short schedules, overruns, live programs | P | hour-overrun dialog, gap fill, loop, emergency loop; no live overrun limits |
| protect the on-air item | **B now** (flight plan); P in the editors | the editors do not lock the on-air row |
| save / restore the active position | P | position is derived from the wall clock; takes and ad-hoc pushes are not restored |
| behaviour after restart | P | one mode (join the current schedule); no resume / hold choice |
| midnight, DST, clock steps | P | hours are computed in the channel time zone; the second 01:xx of a fall-back day is untested; an OS clock step moves hard starts against the plan |
| validate before air | **P → B for readiness** | the validator is still manual; the readiness watcher is automatic |

## Media management

| Item | | Notes |
|---|---|---|
| watch folders | B | polls every 3 s; a file settles for about two polls |
| metadata | B (shallow) | first video / audio stream only; no sample rate, field order, pixel format |
| thumbnails; proxies | B; P | exact-frame and storyboard previews exist; no proxy is generated at ingest |
| black / silence / decode QC | **M** | |
| flag missing media before air | **B now** | readiness watcher (the library sweep still only sets a flag) |
| verify copies; partial-copy protection | P | `.part` and a settle window; mesh and failover pulls do not check size or hash; the settle window is fixed |
| asset versions | **M** | a file replaced in place keeps its stale duration and thumbnail |
| expiry dates | B | |
| shared storage and cache; multi-site availability | P | no pre-air prefetch, cache eviction, or per-site availability rule |

## Graphics, commercials, external control

| Item | | Notes |
|---|---|---|
| bugs, lower thirds, scheduled overlays, clear-all | B | crawls: templates yes, live data binding no |
| graphics preview | P | template previews and a program MJPEG; no preview bus |
| OBS control | **M** | only in the legacy app |
| remote API with documented commands | P | ~250 routes, bearer token, WebSocket; no OpenAPI, no route listing |
| GPI / GPO | **M** | |
| break start / end events | **M** | |
| SCTE-35 | P | splice_insert cue out / in per mux service, automatic on ad breaks or by hand (docs/mux.md "SCTE-35"); not on the engine's own UDP / SRT outputs, no time_signal / segmentation descriptors |
| as-run of spots, interrupted or failed | P | a spot cut short by a take / replace ends as plain END, and counts as delivered |
| accepted vs confirmed | **P now** | REST honest about delivery; no `commandId` carried through the engine and no WS `commandResult` yet |

## Reliability and operator controls

| Item | | Notes |
|---|---|---|
| engine independent of the UI; UI reconnects | B | |
| watchdog / controlled restart | P | a wedged clock thread keeps heartbeating; two respawn paths can race |
| **Supervisor crash recovery** | **M** | engines never reconnect to a restarted Supervisor; orphans keep outputs |
| emergency filler, manual override | B | |
| disk alarm | **B now**; input and storage alarms P | live-input loss is a flag, not an alert; alerts go to the UI and logs (SMS only for the mux) |
| on-air / cued / missing / failed indicators | B | |
| as-run log | B | rows are lost while the Supervisor link is down |
| config backup / restore | P | sessions export / import; no scheduled backup or database snapshot |
| single active controller per output | P | between nodes yes (epoch); within a node any client may command |
| standby and switchover | P | warm backup, auto-promote; no runbook; the backup needs an external router |
| install / upgrade / rollback | P | installer reuses its AppId; no pre-upgrade backup or rollback procedure |

## Next, in order of value for a 24/7 station

1. **Supervisor-crash recovery:** engines reconnect with a 2 s retry and re-announce themselves; pid files so a restarted Supervisor adopts or kills strays.
2. **Hung-engine detection:** heartbeat must carry an advancing frame number; one respawn path.
3. **Fallback on air:** hold the last frame, then the station's emergency slate or filler, bars last.
4. **As-run truth for spots:** END of an item under about 98 % of its length is TRUNCATED / SKIP; delivery counts only complete spots; the engine spools as-run rows while the Supervisor is away.
5. **Break events** (and a GPO hook), then SCTE-35 through the mux.
6. **Operator editor changes in the native app:** the interruption-policy and optional / mandatory controls (so far only in the web editor), a locked on-air row, drag reorder.
7. **Media QC and versions:** black / silence / decode checks at ingest, re-probe on a changed size or mtime, `processing` until a copy is verified.
