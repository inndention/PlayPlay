# PLAY PLAY (C++/Qt) Roadmap

Approved 2026-10-22. This file records the
phase status and the two reference apps the product must resemble and rival.

## Status
| Phase | Scope | Status |
|---|---|---|
| 0 | Build hygiene, SDK discovery, tests, scripts | done (commit 8ad37e2) |
| 1 | Channel engine: audio, A/V sync, encoder stages, TS/UDP/NDI sinks, gapless cuts | done (commit 3aa54ff) |
| 2 | Control plane: Supervisor routing, REST/WS, native app adapter, **scheduling core** (below) | done (see Phase 2 notes) |
| 3 | DekTec ASI **done**, DekTec SDI baseband **done**; SRT/QSV (custom FFmpeg), PSIP pending | in progress |
| 4 | Compositor: layer stack, transitions, live inputs, CG, audio mix, D3D11 path | done (see Phase 4 notes); HTML CG needs qt6-webengine |
| 5 | Native QML UI modelled on PlayPlayClean, wired to the Supervisor | done (see Phase 5 notes); segment editor, playlists tabs, watch folders in 5b |
| 6 | Web dashboard served by the Supervisor (same REST/WS; bearer token) | done (see Phase 6 notes) |
| 7 | Hardening (stall watchdog, output alarms), output-locked clock / genlock, logs + /metrics, Windows service, soak script, package + installer script, web ingest | done (see Phase 7 notes); Inno Setup not installed here, zip package verified |
| 9 | PlayPlay OBS parity: query/folder/series blocks, hour recipes + ad-break builder, locked anchors, weighted fill (format clock), autofill policy, ad contracts, auto-generated spots (recipes, TTS, cards, jobs, GEN markers, scatter), HTML reports (as-played, per-asset, period, adherence, contracts), karaoke captions + fact box CG, mux expansion (NDI sources, encoder config, TSDuck tables, auto-start streams), sessions/clone/failover/remote nodes, context menus | done (see docs/playplay-obs-parity.md) |
| 8 | Integrations: full NDI scan, ONVIF cameras + PTZ, HTML CG pack, AN-FS mesh + Star agents, PlayPlay OBS station API, library build-out (watch folders, removable, hashes, thumbnails, relink, collections), faster-whisper transcription + segment editing, ATSC mux (TSDuck statmux + PSIP from the schedule) | done (see docs/integrations.md) |

## Phase 2 notes (2026-10-22)
- **Supervisor is the automation host.** `PlayPlaySupervisor.exe` owns the SQLite database (`<data-dir>/playplay.db`:
  channels, hour_slots, media, as_run, settings), spawns one `PlayPlayChannelEngine.exe` per enabled channel with its
  config, executes the schedule (`playout/schedule/`), and serves REST on **:7700** and WebSocket on **:7701**
  (8080/8081 are taken on this workstation). The operator app launches a Supervisor if none is running and connects as a
  UI client over IPC; closing the app does not stop playout.
- **Executor model.** Every second: resolve the channel's local hour (its time zone), build runlists for this hour and
  the next (dated override > weekly template; hard/soft/fill timing; smart slots; gap fill; hour loop), push only when
  the hour, the schedule, or the engine's connection changed, or the engine idles with something due. Join in
  progress advances the in-point; the first item of the next hour is hard-anchored at :00 unless the hour is a merged
  continuation; an empty schedule plays the emergency loop. As-run START/END rows record planned vs actual, variance
  and origin (schedule/fill/emergency/manual).
- **Measured:** hard start executed within one frame of the planned instant; as-run variance −25 ms before the
  frame-ceiling fix, 0..+33 ms after.
- **REST surface:** `/api/v1/status`, `/channels[...]` (CRUD, start/stop/restart/arm/disarm/load-hour/snap/reload,
  transport, playlist, outputs, state, runlist, planned, cg), `/schedule/{ch}/week|template|slot/{id}[/validate]|import`,
  `/media[/{id}|/scan]`, `/asrun[/summary]`, `/settings/{key}`, `/preview/{ch}.jpg|.mjpeg`. WebSocket pushes
  `snapshot`, `telemetry`, `stateChange`, `runlist`, `asrun`, `alert`, `schedule`.
- **Not in Phase 2:** the QML pages still read the app's own MediaDb/ScheduleDb for display (Phase 5 rewires them to
  the Supervisor), CG commands are accepted but the compositor (Phase 4) executes them, device commands at hour offsets
  are stored but not dispatched yet.

## Integrations (2026-10-22, after Phase 7)
See `docs/integrations.md` for the REST surface and the design of each piece: inputs/NDI scan, ONVIF/PTZ,
the HTML CG pack under `web/cg`, the AN-FS mesh and StarAgent clients, the PlayPlay OBS-compatible station
API, the ingest pipeline and library, transcription with faster-whisper and transcript-driven segments,
and the ATSC multiplexer ported from the Starfile broadcast_gui.

## References
- **UI target: the original PlayPlay app** (Electron/React). Screenshots in `docs/ui-reference/`, feature and token inventory in
  `docs/reference-feature-inventory.md`. The source is kept read-only in `reference/electron-src/`.
- **Feature/scheduling target: PlayPlayPy** (the earlier Python/PySide app, deployed on a remote node). Inventory in `docs/reference-playplaypy-inventory.md`. It plays through CasparCG or an
  internal FFmpeg engine; OBS is a controlled peripheral (obs-websocket) plus browser-source URLs.

## Phase 3 notes (2026-10-22): DekTec ASI on air
- `DekTecAsiSink` (playout/engine/sinks/dektecasisink.*) drives a DTA-2154 port through DTAPI: attach by port, force
  the port to output + ASI if needed, 188-byte TS mode, TS rate = mux rate, FIFO prefill (400 ms) then SEND, writer thread
  with FIFO back-pressure, latched underflow/overflow counters and FIFO load in telemetry. `TsMuxer` wraps libavformat's
  mpegts muxer with a byte callback (CBR with null stuffing), so the same packets can also feed UDP/SRT/file sinks.
- Engine output config: `{"type":"dektec_asi","port":2,"muxrate":19392658,"profile":{"preset":"mpeg2_ac3",
  "videoBitrateKbps":15000,"maxrateKbps":15000},"options":{"serviceId":1,"videoPid":49,"audioPid":52,"pmtPid":48}}`.
  Set it per channel with `PUT /api/v1/channels/{id}/outputs` or in the channel profile.
- Measured on the DTA-2154 (port 2): TS rate 19,392,657 bps, FIFO steady at 5.7 % (≈390 ms), 0 underflows, 0 overflows,
  MPEG-2 15 Mbps + AC-3 384 kbps encoding live at 1080p29.97 alongside NDI. No loopback cable is fitted, so the stream
  was verified from the card's own counters; `tsp -I dektec -c 0` on port 1 will show it once port 2 is cabled to port 1.
- The 2154's factory I/O config has port 2 as the output and ports 1/3/4 as inputs; the sink can flip a port to output
  (`configurePort`), which persists in the card.

## Phase 3 notes: DekTec SDI baseband
- `DekTecSdiSink` (playout/engine/sinks/dektecsdisink.*) uses the DTAPI Matrix API: one output row on the chosen port,
  8-bit UYVY 4:2:2 video, PCM audio channels in ADD mode (24-bit in 32, AES channel status set), video standard chosen
  from the channel raster and rate (1080p29.97/30/25/24/23.98, 1080i59.94/60/50, 720p59.94/60/50, 525i/625i) or
  forced with `videoStd`. The card's frame callback pops program frames from a 4-deep queue and pulls exactly the
  card's per-frame sample count from an audio FIFO (its own 1601/1602 cadence); two frames are held before the first
  picture so scheduling jitter never repeats a frame. Underruns, overruns, skipped callbacks and queue depth are in
  telemetry. The port is switched to output + HD-SDI (persistent in the card) when needed.
- Engine output config: `{"type":"dektec_sdi","port":2,"videoStd":"auto","audioChannels":2}`.
- Measured on the DTA-2154 port 2 at 1080p29.97: end-to-end delay 5 frames, 0 underruns / 0 overruns / 0 skipped
  frames over the run after the start-up prime, running alongside NDI.
- Known limit: the card's clock and the engine's frame clock are free-running; over long runs a frame will
  occasionally repeat or drop. Locking the engine timeline to the card (DT_MXCLOCK_SW_FIFO / genlock) is scheduled
  with the hardening phase.

## Compositor (Phase 4, expanded) — no CasparCG
Decision 2026-10-22: PLAY PLAY depends on no external playout or graphics server. The channel engine's mixer
grows into a full compositor:
- **Layer stack per channel** with the PlayPlayPy roles as defaults: 10 program (decoder A/B for gapless cuts),
  15 secondary video (PiP / squeezeback / live input), 20 channel bug, 30 lower third, 40 ticker, 80 legal ID,
  90 emergency slate; arbitrary extra layers for CG. Each layer: source, position/scale/crop, opacity, z, audio
  gain, mute, key (alpha from source or fill+key pair).
- **Sources:** file decoder (existing), live inputs (NDI receive, DekTec SDI/ASI input on the DTA-2154, DirectShow
  capture, SRT/UDP/RTMP pull), still/image sequence, colour/bars/black generator, HTML CG (QtWebEngine offscreen
  rendered to BGRA), Qt-rendered CG templates (existing CGElement model), clock/countdown.
- **Transitions** at cut points and on layer show/hide: cut, mix/fade, dip-to-black, wipe (L/R/U/D), push,
  with frame-accurate start and duration; squeezeback for breaks.
- **Audio mixer:** per-layer gain with ducking (program under voice/CG audio), silence/limiter, program
  loudness meter (EBU R128 momentary/short-term), tone and 1 kHz test generator.
- **GPU path:** Direct3D 11 (Qt RHI) compositing with NV12/BGRA textures uploaded once per frame; software
  QPainter path kept as fallback and for tests. Decoded hardware frames (d3d11va) stay on the GPU until the
  final read-back for encode/NDI/SDI.
- **Control:** every layer and transition is addressable from the schedule (cgOnStart/cgOnEnd/cgOverlays,
  device commands), from the operator UI (CG console preview/program, take, update) and from REST/WebSocket.
- **Verification:** reference-frame tests for blending/keying, transition timing within one frame, audio
  ducking curves, 24 h soak with layers active, GPU vs CPU path bit-for-bit within tolerance.

## Phase 4 notes (2026-10-22): the compositor is on air
Code: `playout/engine/compositor/` (`compositor.*`, `source.h`, `basicsources.*`, `livesources.*`, `sourcefactory.cpp`,
`gpucompositor.*`, `loudness.*`); `PlayoutEngine` now renders every frame through `Compositor::render()`.
- **Program bus** holds A (on air) and B (incoming). `takePrepared` hands the preloaded `DecoderPipeline` to the
  compositor as a `FileSource`; the item's `transition` `{type, durationSec, dipColor}` runs frame-accurately from
  the take frame: `cut | mix | dip | wipe_left/right/up/down | push_left/right/up/down | squeeze`. The outgoing source
  is retired on a reaper thread once the transition ends (never closed on the clock thread). With only the program on
  the bus the frame passes through zero-copy (tick ~0.1 ms).
- **Layers** are a z-ordered map keyed by id; role defaults: program 10, secondary 15, bug 20, lower_third 30,
  ticker 40, legal_id 80, emergency 90. Each has placement (rect or anchor+margins at natural size), opacity, in/out
  transitions (`cut | fade | slide_* | wipe_*`), `holdSec` auto-hide, audio gain/mute/duck.
- **Sources** (`LayerSource::create(spec)`): `file|capture|udp|stream` (DecoderPipeline; `realtime` for devices),
  `color|black|bars|slate|tone`, `image|sequence`, `template` (QPainter: lower_third, bug, ticker, clock, countdown,
  text, box), `ndi` (NDI 6 receive, BGRA + float audio, newest-frame sync, bounded audio FIFO), `dektec_sdi_in`
  (DTAPI Matrix input row with de-embedded PCM), `html` (only with `-DPLAYPLAY_WITH_WEBENGINE=ON`; conda-forge
  `qt6-webengine` would move qt6-main to 6.10+, not installed).
- **Audio**: every source pulled once per frame, gain + ducker (program attenuated by `depthDb` while a `duck`
  layer is audible; attack/release time constants), summed, hard-clipped with a clip counter, metered with a
  BS.1770 K-weighted LoudnessMeter (momentary/short-term/gated integrated/sample peak). Calibration verified:
  stereo -23 dBFS 1 kHz reads -23.0 LUFS.
- **GPU path**: raw Direct3D 11 (no QRhi: private API in this Qt), one draw per DrawOp with scissor clip,
  premultiplied source-over blend, readback to a pooled frame; CPU QPainter path renders the same draw list and the
  test compares them (max diff 1/255 on the Arc A380). Enabled per channel with `"gpu": true`; falls back to CPU on
  device loss. Steady-state 720p four-draw render 5.4 ms, 1080p with four layers ~8-12 ms per tick (readback bound;
  keeping frames on the GPU through to the encoder is the next optimisation).
- **Control**: `POST /api/v1/channels/{id}/cg` → `MSG_CG_COMMAND` → `PlayoutEngine::handleCgCommand`:
  `{action: show|hide|update|clear|remove|program_clear, layer|role, source|template, placement, opacity, in, out,
  holdSec, transition, audioGain|audioGainDb, audioMute, duck}`. Runlist items carry `transition`, `cgOnStart[]`,
  `cgOnEnd[]` (same command objects). Source opening happens off the clock thread; the layer attaches on the next
  tick. As-run logs `CG_SHOW` / `CG_HIDE`; channel state carries `compositor` (program/incoming status, gpu, audio
  LUFS/peak/duck/clips) and `layers`.
- **Verified**: `tests/test_compositor.cpp` (15 cases); standalone 30 s run with mix/push/dip takes and six
  runlist-driven layers (bug, lower third, ticker, clock, looping PiP file, clear) to H.264 TS + NDI with the GPU
  path on, no missed cuts; REST-driven CG on a Supervisor-spawned engine.
- **Open**: HTML CG (WebEngine), genlock of live inputs, GPU-resident frames to the encoder, pre-creating GPU
  textures per layer size (first use of a new size costs a few ms).

## Phase 5 notes (2026-10-22): operator UI rewired to the Supervisor
Code: `Play_Play_Native/qml/main.qml` (shell), `qml/ui/*` (controls and panels), `qml/views/*` (pages),
`playout/supervisorapi.*` (REST bridge for QML + preview image provider), `playout/supervisorclient.*` (IPC: telemetry,
runlists, previews with meters, alerts, as-run feed). Old pages remain in the tree but are no longer loaded.
- **Layout = PlayPlayClean**: top menu (On Air, Schedule, Reports, Timeline, Remote | Output, CG Templates, Tools |
  Config, Logs, Help), icon rail with channel pills, persistent control column (studio clock + TOH countdown, channel
  cards with live preview, selected/current/next/countdown, ARM/SCHED/stop/play/next, ENGINE/LOAD HOUR/PUSH, audio VU,
  hour rundown), schedule ticker across the content area. Tokens from the Clean `:root` live in `Theme.qml`.
- **Data paths**: IPC for everything live (telemetry 1 Hz, runlists on push and on connect, preview JPEG + meters at
  ~5 Hz, alerts, as-run rows); REST (`api.get/post/put/del` from QML) for media, schedule, channels, outputs, settings,
  as-run queries. The client polls the channel snapshot every 4 s for process/executor state.
- **Pages**: On Air (run-of-show header, rundown now+next, program monitor, R128 loudness, layers, engine stats);
  Schedule (media library with drag-and-drop import from Explorer, sort/filters, reveal in Explorer | weekly grid with
  fill % and dated overrides | hour editor with timing/in-out/transition/CG hooks, validate, save as template or date,
  **hour recipes**); Reports (As Played with variance + summary + CSV, **As Scheduled** per day); Timeline (tracks for
  video/CG/audio/switching with playhead); Remote (endpoints, supervisor status, engine processes); Output (per-output
  editor for ASI/SDI/NDI/UDP/SRT/RTMP/file with codec profile, live sink stats); CG Templates (template builder for
  native graphics, live inputs as PiP incl. **discovered NDI sources**, layer list); Tools (folder scan, media table
  maintenance, engine restart/reload); Config (automation, gap fill, playback, engine; channels CRUD; identity assets;
  media; notes); Logs (alerts + live as-run feed); Help.
- **Engine additions for the UI**: per-channel sample peaks + loudness in `meters` (rides on preview frames), NDI
  discovery every 10 s published as `liveInputs` in channel state, identity CG hooks (`cgOnStart: ["bug",
  "lower_third", "legal_id", "clock", "ticker"]` resolve to the channel's identity graphics; objects pass through),
  emergency-loop items visible in the runlist.
- **Verification**: `--screenshot <png> --page <id>` on the app grabs the window headlessly; a local screenshot script
  seeds media + an hour and captures all ten pages (0 QML errors). Screenshots reviewed for every page.
- **Open**: segment editor (scrubber, collections), saved playlists/tabs, watch folders + removable media, thumbnails,
  multiview page, breakout windows, drag-reorder in the hour editor, web dashboard (Phase 6) reuses the same REST.
- **Phase 5b (requested 2026-10-22)**: multi-node media library with file sharing/transfer between Supervisors
  (hash-indexed catalogue exchange, HTTP range transfers with resume, bandwidth caps, per-site availability in the
  validator) — specified in `docs/port-parity-checklist.md` §13 as Phase 9.

## Phase 6 notes (2026-10-22): web dashboard
Code: `web/index.html`, `web/app.js`, `web/style.css` (vanilla ES modules, no build step); staged to `build/web` and
served by the Supervisor from `<exe dir>/web` (or `--web-root`). Open `http://<supervisor>:7700/`.
- **Views**: On Air (one card per channel: MJPEG preview, state, current/next/remaining, loudness bar, ARM/LOAD HOUR/
  SNAP, stop/play-pause/skip behind confirm dialogs, hour rundown), Schedule (weekly grid with fill %, click an hour
  for its items, validate, load hour; editing stays in the operator app), Reports (as-played day table, summary, CSV
  download), CG (lower third / bug / ticker / clock / emergency slate quick actions, program to slate, layer list with
  show/hide/remove, program monitor), Outputs (per-sink statistics), Logs (alerts + live as-run feed).
- **Data**: REST `/api/v1` for everything on demand; WebSocket `:7701` for `snapshot`, `telemetry`, `runlist`,
  `asrun`, `alert`, `schedule`, `mediaScanDone` pushes (low-rate subscription); `/preview/{id}.mjpeg` for pictures.
  Falls back to 5 s polling when the socket is down and reconnects automatically.
- **Auth**: settings key `rest.token` (PUT /settings/rest.token) turns on a bearer-token gate for `/api/*`,
  `/preview/*` and the WebSocket (`?token=`); static files stay open so the login prompt can load. The dashboard
  stores the token in localStorage; the native app reads `PLAYPLAY_API_TOKEN` or QSettings `supervisor/token`.
  Verified: 401 without, 200 with header or query token; dashboard prompts and then connects live.
- **Verified in the built-in browser**: all six views render with no console errors against a live channel; a
  lower-third show from the page reached the engine (as-run `CG_SHOW`, layer visible on the dashboard monitor).
- **Open**: HTTPS/TLS termination (use a reverse proxy for now), per-user roles, mobile layout polish, multiview
  composite, schedule editing on the web.

## Phase 7 notes (2026-10-22): hardening, genlock, soak, packaging, web ingest
- **Output-locked clock (genlock slave).** `FrameClock::setRateTrimPpm` applies a slow rate trim; once a second the
  engine reads the DekTec SDI sink's queue depth (the phase error between engine frames and card callbacks) and runs
  a PI loop (`hardeningTick`) that trims the clock (+-300 ppm) so the queue sits at the 2-frame target: the whole
  chain then follows the card. `clockSource: internal` disables it. SDI sink options `genlock: true` and
  `genlockRefPort: N` set the card's GENREF/GENLOCKED I/O configs and report `genlock: locked|no_lock` in the sink
  stats (needs a reference signal to verify; not available on this bench). Telemetry: `clockTrimPpm`, `clockLocked`.
- **Decoder stall watchdog.** ON_AIR with no new decoded frame for 1 s raises an alert (last frame held); after
  `stallSkipSec` (3 s) the item is logged FAILED and skipped. **Output alarms**: per sink, disconnects, drops,
  underruns and an empty ASI FIFO raise alerts (rate-limited to one per 30 s per sink); a sink that fails to start
  now alerts instead of only logging.
- **Logs.** `installFileLogging` (playout/core/logsink.*) writes JSON lines to `<log-dir>/<process>-YYYY-MM-DD.log`
  with daily rotation and 14-day pruning. Supervisor `--log-dir` (default `<data-dir>/logs`, `none` to disable) and
  it passes `--log-dir` to every engine (`engine-<id>-...`).
- **/metrics.** Prometheus text on the REST port: supervisor uptime/clients/NTP offset, per channel on-air/armed/
  emergency, tick, late ticks, missed cuts, decoder queue, clock trim/lock, stall seconds, loudness, schedule drift,
  per output connected/dropped/bytes/underruns/FIFO, per encoder frames/drops/encode ms.
- **Windows service.** `PlayPlaySupervisor --install-service [options]` registers the Supervisor (auto start,
  restart on failure x3, same options plus `--service`); `--uninstall-service` removes it. The service path runs the
  same core on the SCM thread; the operator app attaches over IPC. Not exercised here (needs an elevated shell).
- **Soak.** `scripts/soak.ps1 -Minutes N [-Asi] [-MediaFolder ...]`: fresh data dir, UDP H.264 TS (+ ASI), hour
  schedule from the media folder looping, TSDuck continuity + PCR + bitrate monitor, 30 s polls, summary with
  verdict in `soak-report.txt`. Overnight: `-Minutes 480 -Asi`. Runs on this bench: 10 min (engine only: 1 late tick at start, 0 missed cuts, 37 starts), 5 min and 3 min with the UDP TS: 0 missed cuts, 0 decoder underruns, 0 critical alerts after seeding; TSDuck direct check 981 PCR OK / 0 jitter / 0 continuity errors. Output-locked clock on the DTA-2154: settles at about -3 ppm within a minute, 0 underruns/overruns over 3 min.
- **Package.** `scripts/package.ps1 [-Version] [-SkipBuild]` builds `dist/PlayPlay-<v>/` + zip: executables, Qt
  runtime via windeployqt (`--qtpaths` needed with conda's Qt), the full Controls/Templates QML trees (the style is
  chosen at runtime), plugins (SQLite only), web dashboard, NDI runtime, ffprobe, and every transitive DLL found by
  walking import tables with dumpbin (conda FFmpeg pulls ~50 codec/support libraries). Verified: the packaged app
  runs from the folder with PATH reduced to System32 (`--screenshot` smoke test). `installer/playplay.iss` is the
  Inno Setup script (service task, firewall rule, shortcuts); ISCC is not installed on this machine, so the setup
  .exe is untested.
- **Web ingest (requested 2026-10-22).** `PUT /api/v1/media/upload?name=&type=` streams the body straight to
  `<media-root|data-dir/media>/uploads/<name>` (never buffered in memory; `.part` until complete; name de-duplicated)
  and queues an ffprobe; `GET /api/v1/download/{mediaId}` streams a library file as an attachment; `GET
  /api/v1/media/upload-info` reports the folder, free space and pending probes. Dashboard **Ingest** view: drop zone
  or file picker with per-file progress, import type, folder scan, media table with type/archive/remove/download.
  Verified: 218 kB clip uploaded -> probed (promo, 10 s, 1280x720) -> downloaded with an identical SHA-256.
- **Live inputs and streams as program (requested 2026-10-22).** Schedule items of `type: live` carry a
  `liveSource` spec ({type: ndi|sdi_in|stream, name|port|url}); the builder forwards it (or looks it up from a
  library entry of type live), the engine opens the LayerSource off the clock thread and takes it on the program bus
  with transitions and CG hooks like any file; `fill_until` or a duration ends it. Library entries for live sources
  are registered on the Tools page (Register) or `POST /media {type: live, liveSource}`; discovered NDI names are
  offered as one-click fills. App hour editor: "+ Live"; web: library + live form beside the grid. Verified:
  clip -> NDI live (15 s, bug, mix) -> clip in a standalone engine, and engine B taking another engine's NDI output
  as program (picture received, not its own slate).
- **Schedule rules.** Hour flags (loop, randomize ads, keep program order, block type) in both editors; channel
  rules (ID/bumper/filler fill, ID at top of hour, shuffle ads, latest episode, strict wall-clock, max fill,
  emergency type) in a Rules panel beside the grid in the app and on the web (PUT /channels/{id} {fill, strict}).
- **Web schedule editing.** The dashboard's Schedule view is now an editor: media library and live-input form on
  the left, grid, rules and an hour editor (timing, at, in/out, transition, move/remove, flags, validate, save as
  template or for the date, delete). Verified round trip: live item added and saved from the browser.
- **Open**: reference genlock verification with house sync, service install test on an elevated shell, Inno
  compile, N+1 warm standby engine, HTTPS.

## UI direction (Phase 5)
Reproduce the PlayPlayClean operator surface in QML, with PlayPlayPy's panels where Clean lacks them:
- **Chrome:** top tab bar `On Air | Schedule | Reports | Timeline | Remote || Output | CG Templates | Tools || Config | Logs | Help`;
  left icon rail with channel pills (ON/OFF, merge colour, + add), Playout / Weekly Schedule / NLE Timeline /
  Ingest / Inspector / Live / Remote / Output; status bar (engine, channel, ARM-LOCK, alerts).
- **Persistent left column** (380-520 px): studio clock + TO TOP OF HOUR countdown; channel cards (ON AIR/STANDBY,
  thumbnail, current/next, next-in, progress); context bar Selected/Current/Next/Countdown; transport
  `ARM | SCHED | STRICT/RELAX | Stop | Play/Pause | Next | LIVE | MULTIVIEW | LOAD HOUR | PUSH`; identity row
  `SLATE | LOOP | BUG | ID`; quick lower-third; Hour Rundown (IN/OUT/Title/Dur, next row flashes red in last 10 s).
- **Centre/right by tab:** On Air = schedule ticker (NOW/NEXT/OUT/hour remaining) + run-of-show (Load current hour,
  START/DURATION/END, Snap, Free Play, Queue Start, Anchor TOH, Align, Reload) + playlist; Schedule = Media Library |
  Weekly 7x24 grid (fill %, now-playing outline, sync markers, hour/day context menus); Timeline = NLE tracks
  Video / CG / Aux Audio / Switching + library; Reports = As-Played / Discrepancy / Client Exposure.
- **Theme tokens** (`Play_Play_Native/qml/Theme.qml`): PlayPlayClean `:root` palette (bg #05050a, surface #0f0f18,
  elevated #141420, border #2a2a40, text #f0f0f8/#a0a0b8, accent #00d4ff, secondary #a855f7, on-air #ff2d55,
  standby #ff9500, ready #30d158, cued #5e5ce6, warning #ffd60a, error #ff453a; timeline video #5856d6, audio
  #30d158, graphics #ff9f0a, ad #ff375f, bumper #ac8e68), fonts Sora / Inter / JetBrains Mono (shipped), radii
  3/5/8/12/16, 13 px base at <=1400 px. PlayPlayPy's button role colours (ArmLock red, Sched green, Live red,
  Push cyan) map onto these status tokens.

## Scheduling core (added to Phase 2, native C++ in the app + engine)
Match the union of both apps and fix what neither finishes:
- **Model:** dated week = 168 template slots (`D-H`) plus date overrides (`YYYY-MM-DD-H`), per channel; HourSlot
  {items, isLooping, randomizeAds, keepProgramOrder, blockType, showBlock, mergedEvent (1-4 h), loopOverride
  window, deviceCommands at +/- offsets}; items with timingMode hard_start|soft_start|fill_until, inPoint/outPoint,
  segments from clip_segments, smartConfig (latest episode / previous / specific date / random), priority,
  transition, status, cgOnStart/cgOnEnd/cgOverlays, live sources. SQLite schema must persist **every** field
  (PlayPlayClean loses them).
- **Timing (engine-owned):** wall-clock runlist per hour with hard-start anchoring; join in progress and startup
  catch-up (seek into the on-air item); auto-push at :00 with carry-over for merged hours; overrun cut at the next
  hard start; fill_until stretch; loop windows; auto-advance; out-points honoured (both apps drop them).
- **Gap fill at playout:** filler pool (station_id, bumper, filler, promo) with the PlayPlayClean weighting
  (freshness, priority, series boost, play count) and PlayPlayPy's emergency loop when nothing is scheduled or
  the file is missing. Wire the settings both apps left dead (fillGapsWithIds/Bumpers, maxFillDuration,
  shuffleAdsInHour, loopAtEndOfSchedule, randomizeAds, keepProgramOrder).
- **Traffic:** dayparts, calculateWeight (delivery/daily/boost/recency multipliers, separation, hourly max),
  buildBreak, shake ads with undo, quick break; clients, contracts, exposure report.
- **Guard (ARM-LOCK):** per-channel arm/strict/free-play (not engine-wide); 2 s guard that recovers not-playing,
  wrong-title, missing-file and engine-dead cases; memory/health alarms.
- **Validation:** the union of both validators' codes (missing_media/source/path, expired, not_yet_available,
  channel_exclusive, site_unavailable, qc_failed, rights_blocked, gap, overrun, item_overruns_hour,
  timing_conflict, over/underfill, missing_legal_id, missing_cg_template).
- **As-run:** START and END with planned vs actual, seek offset, source, timecode; scheduled_log; discrepancy
  and client-exposure reports; CSV/JSON export.
- **Identity layers:** channel profile with call letters/brand, bug/legal ID/emergency slate assets, CG layer
  roles primary/bug/lower_third/legal_id/emergency mapped to mixer layers 10/20/30/80/90.
- **Ingest (Phase 5/7):** watch folders with settle time, ffprobe, fingerprint, thumbnails + filmstrip,
  duplicate check, review queue; Whisper/AI classification optional behind a flag.
- **Remote (later):** host/node roles, resumable file bridge, schedule sync alerts; OBS remote-control panel
  via obs-websocket v5, browser-source/preview URLs from the Supervisor, NDI as the OBS feed.
