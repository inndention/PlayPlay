# PLAY PLAY C++ port — feature parity checklist

Generated 2026-10-22 against the tree at Phase 5 (own compositor, Supervisor-driven UI). One line per feature from the
operator inventory, with a status:

| Status | Meaning |
|---|---|
| **Verified** | Built in the C++ port and exercised end to end (tests, soak run, hardware, or screenshots in this repo). |
| **Built** | Implemented in the port but not yet verified against real signal, hardware or a long run. |
| **Port** | Exists in the Electron/PlayPlayPy reference and is planned for the port as-is (phase noted). |
| **Replace** | Planned, but the reference implementation is not reused: the port does it differently (reason noted). |
| **Retire** | Not carried forward (reason noted). |

Phase numbers refer to `docs/ROADMAP.md`. "Engine" = `PlayPlayChannelEngine.exe`, "Supervisor" = `PlayPlaySupervisor.exe`,
"App" = the Qt/QML operator app.

## Acceptance list (the easy-to-miss items, first)

| Item | Status | Where / how it is checked |
|---|---|---|
| Correct resume offsets (join in progress after late start or restart) | **Verified** | `ScheduleExecutor::composePlaylist` advances `inSec` by the lateness; `test_schedule` + Phase 2 e2e (+19 ms start). Engine restart re-pushes with offset. |
| Hard-time events (fixed-time items, :00 cut) | **Verified** | `hardStartUtcMs` with frame ceiling (never early); `test_pipeline`, Phase 2 e2e. |
| Segment collections (virtual clips per source) | **Port** (Phase 5b) | `MediaRecord.segments` stored; `SlotItem.segmentId/parentMediaId` modelled; editor UI pending. |
| Ad separation rules (client/competitive) | **Port** (Phase 8 traffic) | `FillPolicy` has weights and cycling; separation/competitive rules not yet. |
| Per-channel graphics state | **Verified** | Each engine owns its Compositor; `state.layers` per channel; CG commands routed by channelId. |
| File availability at remote sites | **Built** | AN-FS search shows which nodes hold a file (`swarmNodes`); the station API lets the Star library reconcile; validator rule pending. |
| Truthful as-run logs (actual times from the engine clock) | **Verified** | Engine logs START/END/SKIP/FAILED/TRUNCATED/CG with frame + TC; Supervisor maps to planned and stores variance. |

## 1. Core playback and operator controls

| Feature | Status | Notes |
|---|---|---|
| Video playback | **Verified** | DecoderPipeline (FFmpeg 7.1, d3d11va), gapless cuts, A/V alignment 0.9 ms. |
| Still-image playback with duration | **Built** | `image`/`sequence` sources in the compositor; schedule item `duration` applies. Not run as a program item yet. |
| Play / pause / resume / stop | **Verified** | Engine transport commands; PAUSED holds the last frame with silence. |
| Next / previous | **Built** | `skip` verified; `previous` not implemented as a transport action (operator uses `goto`). |
| Seek | **Verified** | `seek` reopens at the new in-point on the loader thread. |
| Volume / mute | **Built** | `masterGainDb` config command; per-layer gain/mute. No operator fader yet. |
| Cue / preload next before take | **Verified** | `prerollSec` preparation on a detached loader; take at the exact frame. |
| Preview independent of program | **Replace** | No second player: preview is the compositor's JPEG tap + NDI monitor. A true preview bus (PVW) is planned as a second compositor instance (Phase 7). |
| Multiple DDR players | **Replace** | One program bus + N layers per channel instead of N DDRs; extra channels are extra engines. |
| Clip in/out points, virtual clips | **Verified** | `timeIn/timeOut` per item; no new files. |
| Cut / mix / dip transitions | **Verified** | Compositor program bus; `test_compositor`. |
| Wipe support | **Verified** | `wipe_*`, `push_*`, `squeeze` implemented; keep. |
| Scheduled playback | **Verified** | ScheduleExecutor pushes hour runlists; hard starts at :00. |
| Continuous playlist loop | **Built** | `isLooping` hour flag and item `loop`; hour-loop verified in `test_schedule`. |
| Folder loop with ordering (name/date/size/random) | **Built** (Phase 9) | Folder blocks: `SlotItem.folderBlock {folder, order random\|name\|date\|size\|added, weight}` resolved at build time (`QueryBlocks::resolveFolder`). |
| Loop until hour, then resume schedule | **Built** | `HourSlot.loopOverride` modelled and honoured by the builder; not exercised live. |
| Manual intervention + return to automation | **Verified** | ARM/disarm, LOAD HOUR, SNAP, TAKE (manual item resumes the list after). |
| Current/next/elapsed/remaining/on-air status | **Verified** | Engine telemetry at 1 Hz; shown in the control column. |

## 2. Timing and automation engine

| Feature | Status | Notes |
|---|---|---|
| One authoritative clock per channel | **Verified** | FrameClock thread per engine; Supervisor TimeAuthority supplies only the NTP offset. |
| Determine what should play from schedule + time | **Verified** | `RunlistBuilder` + `ScheduleExecutor` (per-second tick). |
| Hard starts and in-progress handling | **Verified** | See acceptance list. |
| Join at correct offset after late start/restart | **Verified** | See acceptance list. |
| Frame/timecode conversion, configurable frame rate | **Verified** | `FrameRate` rational, 1601/1602 cadence, drop-frame TC; `test_framerate`. |
| Preroll and transition timing | **Verified** | `prerollSec`; transitions frame-accurate from the take frame. |
| Expected vs observed position comparison | **Built** | Executor stores `driftSec`; shown in the UI. |
| Drift tolerances and correction policy (seek/reload/skip/fill) | **Port** (Phase 7) | Only `snap` (seek) and SNAP-on-late-start exist; policy table not ported. |
| Gap filling: slate, loop, random, black | **Verified** | FillPolicy (IDs/bumpers/filler), emergency loop, slate when nothing available. |
| Live-event extension limits and return | **Port** (Phase 7) | Live items play as sources; extension/overrun limits not implemented. |
| Cue ack/retry/expiry/duplicate protection | **Replace** | Engine commands are a queue on the clock thread with generation counters (no lost or duplicated takes); the reference cue dispatcher is not ported. |
| Explicit states | **Verified** | IDLE / PREROLL / ON_AIR / PAUSED / HOLD / ERROR (+ executor emergency flag). "filling/drifted" are derived in the UI. |
| Background control independent of the UI | **Verified** | Supervisor + engines run headless; the app is a client. |

## 3. Playlists, rundowns, scheduling

| Feature | Status | Notes |
|---|---|---|
| Drag-and-drop playlist creation / reorder | **Built** | Hour editor adds from the library (+) and reorders with arrows; drag-reorder pending polish. |
| Multiple saved playlists, tabs, lock | **Port** (Phase 5b) | Only hour recipes exist today. |
| Weekly templates and dated schedules | **Verified** | Template `D-H` and dated `YYYY-MM-DD-H` slots; dated wins. |
| Hour editing and whole-week grid | **Verified** | ScheduleView (screenshots). |
| Copy hour/day/template to another date/week | **Verified** (Phase 9) | `POST /schedule/{ch}/copy {from:{scope,...}, to:{dow\|date\|weekStart, dated}}`; grid context menu. |
| Import/export hour as playlist | **Port** (Phase 5b) | `/schedule/{ch}/import` accepts PlayPlayClean JSON; M3U/export pending. |
| Calculated start/end and totals | **Verified** | Runlist planned times; grid fill %, As-Scheduled report. |
| Under/over/conflict/empty warnings | **Verified** | `ScheduleValidator` codes; Validate buttons in the UI. |
| Fixed-time items in an hour | **Verified** | `hard_start` + `fixedOffsetSec`. |
| Keep program order, randomize ads | **Built** | Flags modelled and applied by the builder; not exercised live. |
| Autofill | **Verified** (Phase 9) | Autofill policy per channel (`/channels/{id}/autofill`): empty hours synthesized from a recipe or the format clock at build time; gap fill + contract pacing. |
| Smart slots (latest/previous/date/random/client rotation) | **Built** | `smartConfig` resolved in the builder; client rotation = `random` by client until traffic lands. |
| Series / episode-date matching | **Built** | `programSeries`, `episodeDate` on media; used by smart slots. |
| Rundown management for live production | **Port** (Phase 7) | Orphan in the reference; keep as a later feature. |
| Scheduled commands within an hour | **Built** | `deviceCommands` modelled; CG hooks execute; OBS/device targets pending. |
| Schedule versioning and safe live updates | **Verified** | Store revision counter; executor re-pushes from the current item onward only. |

## 4. Media library and ingest

| Feature | Status | Notes |
|---|---|---|
| File import, bulk import, folder scan | **Verified** | `/media/scan`, drag-and-drop in the app, streaming upload from the web dashboard; ffprobe on the Supervisor. |
| Watch folders / drive monitoring / patterns | **Verified** | `IngestManager` watch folders (stable-size pickup, per-folder type), `ingest.watch`; UI in Tools and the web Library. |
| USB / removable media ingest | **Built** | Volumes with a `PLAYPLAY_INGEST` folder are copied into the upload area and probed on insertion. |
| Probing: duration, A/V properties, thumbnails | **Verified** | ffprobe + ffmpeg thumbnails (`/thumbs/{id}.jpg`), audio channels, container, embedded title. |
| Search, filters, favourites, smart collections, sorting | **Verified** | `/api/v1/library` facets (type, status, client, series, origin, missing, archived), tags, collections, sorts; search includes transcripts. |
| Categories (program, paid, ad, promo, ID, bumper, live, image, gap) | **Verified** | Shared vocabulary in `SlotItem.type` / `MediaRecord.type`. |
| Bumper subtypes tied to a program | **Port** (Phase 5b) | Series-matched bumpers in fill ranking only. |
| Metadata (title, client, series, dates, notes, priority) | **Verified** | Full editor in the app (MediaEditor) and the web Library drawer; bulk edits. |
| Archive while preserving references | **Verified** | `archived` flag; items keep `mediaId`. |
| Stable IDs, hashes, duplicate detection | **Verified** | Quick hash (head+tail+size) or full md5; `/media/duplicates` by hash and by size+filename. |
| Missing-file detection, relink | **Verified** | 5-minute sweep sets `missing`; probed files relink by hash or size+filename; manual relink. |
| Proxy generation / association | **Retire** | The engine decodes any source natively; no proxies needed for playout. Low-res previews come from the compositor tap. |
| Persistent DB and path resolution | **Verified** | SQLite in the Supervisor data dir. |
| Play counts, last played, airtime, history | **Built** | `playCount`, `lastPlayedMs` updated from as-run; airtime via as-run summary. |

## 5. Segment and clip editing

| Feature | Status | Notes |
|---|---|---|
| Scrubber and in/out marking | **Built** | Numeric in/out with a segment bar; transcript word selection marks in/out; frame-accurate scrubbing with a preview decoder still pending. |
| Named segment collections, active collection | **Verified** | `extra.segmentCollections` + `activeCollection`; the active list is what the schedule resolves. |
| Add/label/duplicate/reorder/delete segments | **Verified** | MediaEditor (app) and the Library drawer (web). |
| Auto split with overlap | **Verified** | Interval + overlap, and split at transcript pauses. |
| Keyboard shortcuts | **Port** | Not yet. |
| Segments to scheduled clips | **Built** | `SlotItem.segmentId/parentMediaId` resolve to in/out. |
| External VKM editor | **Retire** | Replaced by the in-app editor. |
| Preserve the original file | **Verified** | Virtual clips only. |

## 6. Character graphics and overlays

| Feature | Status | Notes |
|---|---|---|
| Lower thirds, bugs, tickers, clocks | **Verified** | Native templates rendered by the compositor; on air in the Phase 4 run. |
| Weather, custom graphics | **Port** (Phase 6) | Weather needs a data source; custom = HTML CG. |
| WYSIWYG designer with live preview | **Port** (Phase 6) | Template builder exists (form + program monitor); canvas designer pending. |
| Template library, import, parameters | **Built** | Templates are JSON specs; library/import pending. |
| Text, images, fonts, colours, gradients, shadows, position | **Built** | Templates: fonts/colours/padding/radius; gradients/shadows pending. |
| Entrance/exit animation, loops | **Verified** | fade/slide/wipe in/out; tickers loop. |
| Multiple layers, predictable stacking | **Verified** | z = layer id; roles 10/15/20/30/40/80/90. |
| Per-channel config and visibility | **Verified** | Identity per channel profile; state per engine. |
| Manual take/show/hide, timed auto-hide | **Verified** | `holdSec`; CG page. |
| Trigger at item start/end | **Verified** | `cgOnStart`/`cgOnEnd` (objects or identity names). |
| Scheduled overlay ads | **Port** (Phase 8) | `cgOverlays` modelled; execution pending. |
| QR-code overlays | **Port** (Phase 6) | Old `QRCodeOverlay.qml` exists; move into a template kind. |
| Schedule / TV-guide / multi-site tickers | **Port** (Phase 6) | Ticker kind exists; data feeds pending. |
| News slates, slate maker | **Port** (Phase 6/8) | |
| Clean browser output for external compositing | **Replace** | Not needed: the engine composites internally; NDI/SDI carry the clean feed. An NDI "key + fill" output is a possible Phase 7 add. |
| CasparCG / NodeCG / OBS graphics paths | **Retire** (Caspar, NodeCG) / **Port** (OBS as a controlled peripheral, Phase 7) | Decision 2026-10-22: no CasparCG. |

## 7. Channels, clean output, monitoring

| Feature | Status | Notes |
|---|---|---|
| Independent channels | **Verified** | One engine process per channel. |
| Channel tabs / breakout windows | **Built** | Rail pills + control column; breakout windows pending. |
| Per-channel output destinations | **Verified** | Output page → `PUT /channels/{id}/outputs` live. |
| Clean program output | **Verified** | NDI / SDI / TS outputs carry program only. |
| Fullscreen output and display selection | **Replace** | Program never goes through a desktop window; a confidence display window (NDI receive) is planned (Phase 7). |
| State sync on output window reconnect | **Verified** | UI reconnect gets snapshot + telemetry + runlists. |
| Multiview / preview grids | **Port** (Phase 7) | Old `MultiviewPage.qml` to be rewired to preview taps. |
| Confidence / talent monitor | **Port** (Phase 7) | |
| Studio clocks and countdowns | **Verified** | StudioClock, TOH countdown, item countdown. |
| Multiview streaming for remote | **Built** | `/preview/{id}.mjpeg` per channel; composite multiview pending. |
| Simulcast config and preview | **Port** (Phase 7) | Multiple outputs per channel already; simulcast = same profile fan-out (exists). |
| Video router / routing matrix | **Port** (Phase 7) | Output router page from PlayPlayPy. |
| Channel-count limits | **Replace** | No fixed cap: bounded by CPU/GPU; the Supervisor reports engine health. |

## 8. Live inputs and production control

| Feature | Status | Notes |
|---|---|---|
| NDI sources | **Verified** | `NdiSource` receive + discovery; aired as a scheduled live program item (engine-to-engine NDI). |
| Capture devices (DirectShow) | **Built** | `FileSource` realtime mode with `inputFormat: dshow`. |
| Network URLs | **Built** | udp/srt (needs custom FFmpeg)/rtsp/rtmp/hls/http via FFmpeg. |
| OBS scenes as sources | **Port** (Phase 7) | Via NDI from OBS. |
| Custom GStreamer pipelines | **Retire** | FFmpeg covers the cases; no GStreamer dependency. |
| Scheduled live takeovers with limits, auto-return | **Verified** | `type: live` items with `liveSource`, duration or fill-until, return to the next item/hard start. |
| Discovery, status, source-loss handling | **Built** | Discovery + connected/no-signal status; loss falls back to last frame/black. |
| OBS WebSocket (v5) management, scenes, sources, streaming | **Port** (Phase 7) | Old integration code exists in the app tree. |
| Saved OBS commands on items/schedules | **Port** (Phase 7) | `deviceCommands` model ready. |
| PTZ, equipment mappings, command panels | **Port** (Phase 7) | |
| ONVIF / ATEM / TC1 entries | **Retire** unless a device is on site | Review per device. |

## 9. Playback engines and hardware adapters

| Feature | Status | Notes |
|---|---|---|
| Internal player replacement | **Verified** | Native engine (FFmpeg + own compositor). |
| MPV playback path | **Retire** | Superseded by the native engine. |
| FFmpeg / FFprobe integration | **Verified** | Libraries in the engine; ffprobe CLI for ingest. |
| NDI discovery / receive / send / tally | **Built** | Send verified (NDI Studio Monitor); receive built; tally pending. |
| DeckLink | **Port** (Phase 7, if hardware is present) | |
| DekTec enumeration, TS playback, SDI output | **Verified** | ASI and SDI on the DTA-2154. |
| Engine start/stop/status/logs | **Verified** | Supervisor spawns/respawns; Remote page. |
| Runtime discovery and unavailable-integration reporting | **Built** | Compile-time `HAS_NDI`/`HAS_DEKTEC`; runtime errors surface as alerts. |
| External vs native engines | **Verified** | Decision: all native; external only for OBS as a peripheral. |

## 10. Encoding, transport stream, mux, ASI

| Feature | Status | Notes |
|---|---|---|
| Per-channel encoding config | **Verified** | Profiles per output (mpeg2_ac3, h264_aac). |
| OBS Branch Output | **Retire** | |
| FFmpeg encoding fallback | **Verified** | It is the primary path. |
| Multi-program TS mux | **Port** (Phase 7) | One program per TS today; PlayMux stays the external MPTS mux until then. |
| PID / TSID / service / virtual channel config | **Built** | `serviceName`, mux options; PIDs via profile options; UI partial. |
| PSIP generation and injection | **Port** (Phase 7) | `lib/psipGenerator.ts` to port. |
| TSDuck control / analysis | **Replace** | TSDuck used as a verification tool, not a runtime dependency. |
| Bitrate regulation, mux status | **Verified** | CBR TS with null padding; FIFO load telemetry on ASI. |
| Multiple destinations, branch outputs | **Verified** | N sinks per profile. |
| DekTec ASI port selection, duplication | **Verified** (port) / **Built** (duplication = two ASI sinks) | |
| Compliance diagnostics | **Built** | TSDuck clean analysis in Phase 1/3; keep as acceptance test. |

## 11. Advertising, traffic, campaigns, billing — **Port** (Phase 8)

All items: client records, campaigns, dayparts, caps, separation, competitive exclusion, weighted rotation, pools,
states, reports, proof-of-play. `FillPolicy` and the as-run store are the hooks; `services/trafficengine.cpp` from the
old tree is the starting point.

## 12. As-run, analytics, troubleshooting

| Feature | Status | Notes |
|---|---|---|
| Scheduled vs actual start/end | **Verified** | As-run rows with variance. |
| Actual duration, completed/partial | **Verified** | END/TRUNCATED/SKIP events. |
| Channel/asset/instance/schedule ids in logs | **Verified** | `runlistItemId`, `slotId`, `assetId`. |
| Manual overrides, skips, errors, recovery | **Verified** | MANUAL/SKIP/FAILED/HOLD/STOP rows. |
| Durable history and export | **Verified** | SQLite; CSV copy in the app, CSV download in the web dashboard. |
| Play counts, airtime, campaign stats | **Built** | Counts/airtime; campaign stats with Phase 8. |
| Central error console, severity, acknowledgement | **Built** | Alerts in the app and dashboard with levels; output alarms and stall alerts from the engine; acknowledgement pending. |
| Persistent app/engine logs | **Verified** | JSON-line daily files per process with pruning (`--log-dir`); Prometheus `/metrics`. |
| Engine health monitoring | **Verified** | Heartbeats, restarts, tick/late telemetry. |
| Multi-site issues | **Port** (Phase 9) | |

## 13. Multi-site operation and content distribution — **Built** (AN-FS mesh + Star agents, see docs/integrations.md)

Design: each site runs a Supervisor; a "mesh" service registers peers (mDNS + static list), exchanges
hash-indexed media catalogues, and transfers files over HTTP(S) with ranges (resume), bandwidth caps and schedules.
File availability per site becomes a schedule validator rule. Starfile: **Retire** unless still in use.

## 14. Remote guests and studio communications — **Port** (Phase 10)

WebRTC guest I/O, queue, PTT and producer dashboard. The old `webrtcsessionmanager`/`guestqueueservice` code is
the starting point; guests enter the compositor as `secondary` layers.

## 15. News, social, AI — **Port** (Phase 10) / **Retire** (experimental)

News feed + slates, weather/tower cams, Kokoro TTS (local), transcription, social posting (Facebook story/live),
show archive: **Port**. AI caller/personality, voice commands: **Retire** unless requested again.

## 16. C-port safeguards and acceptance checks

| Check | Status | Notes |
|---|---|---|
| Playout survives GUI close/stall/reconnect | **Verified** | Supervisor/engines headless (Windows service mode available); UI reconnects with full state. |
| 64-bit timestamps, rational rates, no float accumulation | **Verified** | `FrameRate::frameToNs` overflow-safe; sample boundaries exact. |
| Wall-clock scheduling vs monotonic playback | **Verified** | Executor (wall) vs FrameClock (monotonic) with NTP offset only. |
| Midnight, DST, clock corrections | **Built** | Hour boundaries computed in the channel TZ; DST transitions covered by `test_schedule`; clock-step handling in Phase 7 soak. |
| No disk/network/log/UI in real-time callbacks | **Verified** | Loader/reaper threads; SDI/NDI callbacks only copy. |
| Frame ownership, bounded queues, backpressure | **Verified** | Pooled AVFrames, bounded FIFOs with drain, sink drop counters. |
| A/V sync, channel mapping, sample rate, aspect, interlace, colour | **Verified** / **Built** | Sync and 48 kHz verified; interlaced SDI weave built; colour management basic (BT.709 assumed). |
| Missing media / decoder failure / lost input / output failure | **Verified** / **Built** | FAILED items skipped with alert; decoder stall watchdog holds then skips; lost input holds last frame; sink failures, drops and underruns alert. |
| No double-firing of spots/scenes/social on retry | **Built** | Engine command generation counters; social/OBS pending. |
| Data migration (media, schedules, segments, clients, templates, settings, history) | **Port** (Phase 8) | Schedule JSON import exists; others with their phases. |
| Atomic config/DB updates, power-loss recovery | **Verified** | SQLite WAL; profiles saved transactionally. |
| Unicode and UNC paths | **Built** | QString/UTF-8 throughout; UNC untested. |
| Protect remote control and credentials | **Port** (Phase 6) | REST token + bind address planned. |
| Package DLLs, codecs, fonts, templates, runtimes | **Verified** (zip) / **Built** (Inno script) | `scripts/package.ps1` walks import tables; packaged app verified on a bare PATH; setup.exe needs Inno Setup. |
| Tests: restart mid-program, edit during playback, live overrun, disconnected hardware, overnight | **Built** | `scripts/soak.ps1` (TSDuck CC/PCR, polls, verdict); 10-minute run passed, overnight is a parameter; hardware-pull still manual. |
| Verify clean output and audio, not just the operator display | **Verified** | TSDuck on ASI, NDI Studio Monitor, SDI monitor, recorded TS frame checks. |

## 17. PlayPlay OBS parity (Phase 9, 2026-10-22)

See `docs/playplay-obs-parity.md` for the full inventory. Summary: query / folder / series blocks, hour recipes and
the ad-break builder, locked anchors, weighted fill (format clock), autofill policy, ad contracts with pacing,
auto-generated spots with GEN markers and scatter, branded HTML reports (as-played, per-asset, period, adherence,
contracts, top, compare), karaoke captions and fact box CG, mux expansion (NDI sources, encoder configuration,
TSDuck tables, auto-start streams), sessions / clone / failover / remote nodes, and right-click context menus in
both UIs.
