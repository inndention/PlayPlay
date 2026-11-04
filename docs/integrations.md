# Integrations: inputs, cameras, HTML CG, mesh, Star agents, mux, transcription

Added 2026-10-22 on top of Phase 7. Everything is served by the Supervisor on the same REST (`:7700`) and
WebSocket (`:7701`) surface; the operator app (QML) and the web dashboard are both clients of it.

## Live inputs: full NDI scan
- Each channel engine runs the NDI finder every 10 s (`NdiSource::discoverFull`): it waits until the source
  list has been stable for 400 ms (one wait returns on the first change and yields a partial list), and
  reports `{name, url, host, source, ip, port}` per source in its telemetry (`liveInputs`, `liveInputsAtMs`).
- Scan settings `ndi.discovery` = `{groups, extraIps}` (comma lists) reach the finder through the engine
  config (`ndi`) and `PUT /api/v1/inputs/ndi-config` pushes them live. `POST /api/v1/inputs/scan` asks every
  engine to rescan (transport action `rescan_inputs`) and runs an ONVIF probe.
- `GET /api/v1/inputs` merges every engine's list (`seenBy` = channels), plus the cameras.

## Cameras: ONVIF discovery and PTZ (`playout/supervisor/onvifmanager.*`)
- WS-Discovery probe on every IPv4 interface (239.255.255.250:3702, repeated after 600 ms); replies give
  XAddrs and the name/hardware/location scopes. Saved cameras (`onvif.devices` in settings, with
  credentials) merge with found ones by host.
- Per device: `GetCapabilities` → Media/PTZ addresses, `GetProfiles` (prefers a PTZ profile),
  `GetStreamUri` (RTSP, credentials embedded so FFmpeg can open it), `GetPresets`. SOAP 1.2 with a
  WS-UsernameToken digest; HTTP Digest is answered too.
- PTZ: `POST /api/v1/cameras/{id}/ptz` `{action: move|stop|preset|setPreset|removePreset|home|setHome|absolute|relative, x, y, z, preset, name}`.
  `POST /api/v1/cameras/{id}/register` adds the camera as a `live` library item (stream source) for the schedule.
- Scheduled device commands (hour slot `deviceCommands[{commandId, offsetSec}]`) fire from the Supervisor:
  `ptz:<camera id or name>:preset:<token>`, `ptz:<cam>:home`, `ptz:<cam>:move:x,y,z,secs`, `cg:{json}`,
  `transport:<action>`.
- Consoles: app **Inputs** page (NDI list + scan settings, camera list, PTZ pad, presets, stream, register),
  web **Inputs** tab.

## HTML CG template pack (`web/cg`)
- The user's `cg_templates` pack (48 standalone HTML templates driven by URL parameters, the animation
  library and the control-centre scripts) lives in `web/cg`; `templates.json` is generated from the
  pack's `TEMPLATES` table plus parameter sniffing (regenerate with the script in the session notes).
- `GET /api/v1/cg/templates`; `POST /api/v1/cg/html/{channel}` `{template, params, animation, role, onAir}`
  builds the template URL (same query vocabulary as `js/cg-client.js`), stores it per channel and
  broadcasts `cgHtml`. `web/cg/output.html?channel=main` is a browser-source page (OBS / vMix / NDI
  browser) that follows that state. `onAir: "native"` also puts the nearest Qt-rendered template on air
  (lower thirds, bugs, tickers, countdowns, emergency); `onAir: "html"` sends an `html` layer (needs the
  WebEngine build).

## AN-FS mesh (`playout/supervisor/meshclient.*`)
- Bridge API (no default: the feature is off until `mesh.anfsUrl` is set, e.g. `http://192.0.2.10:8080`; settings `mesh.anfsUrl/anfsToken/anfsCookie`): `/api/nodes`,
  `/api/browse?node=&path=`, `/api/search?q=`, `/api/download?node=&path=` (Range → resumable), chunked
  `/api/upload/init|chunk|complete` (needs the mesh login).
- `GET /api/v1/mesh/nodes|browse|search`, `POST /api/v1/mesh/fetch` (into `<uploads>/mesh/<node>/`, then
  probed into the library), `POST /api/v1/mesh/push`, jobs under `/api/v1/transfers`.

## Star agents (`playout/supervisor/starclient.*`)
- StarAgent daemons (`D:\StarMedia\StarAgent`, port 8780, bearer token, loopback + tailnet) expose a file
  system (`/fs/list|stat|search|hash|download` with Range, chunked `/fs/upload/*`), `/status`,
  `/nowplaying`, `/exec`. The local daemon is discovered from `D:\StarMedia\StarAgent\agent-token`;
  other nodes come from `mesh/nodes.json` (tokens are pasted in the Mesh console, settings `star.agents`).
- `GET /api/v1/star/agents`, `POST /api/v1/star/probe`, `GET /api/v1/star/{node}/list|search|status|nowplaying`,
  `POST /api/v1/star/fetch|push` (resumable download, 4 MiB chunked upload with resume).

## PlayPlay OBS station API (what the Star tooling expects)
- The Supervisor answers the station shape used by the Star `pp_*` tools and the AN-FS bridge's
  "send to PlayPlay": `GET /api/channels`, `/api/state`, `/api/media[?limit]`, `/api/media/{id}`,
  `POST /api/media/ingest {path|paths, ctype, review, allowed_channels, expire|valid_until, valid_from,
  category, subcategory, client, series, display_title}`, `/api/ingest/batch[/{id}]`, `/api/ingest/taxonomy`,
  `/api/watch-folders`, `/api/events/recent`, `/api/v2/playout/current` (the StarAgent now-playing probe).
- Auth: `X-PP-Pass: <station.password>` or the bearer token; loopback is open when no token is set.
  `station.port` (settings) opens a second listener (7860 to match the registry) at the next start.

## Media library build-out (`playout/supervisor/ingestmanager.*`)
- One probe queue for scans, drops, uploads, watch folders, removable drives, mesh/Star fetches and the
  station API. Each job: ffprobe → quick hash (md5 of first+last 4 MiB + size; `ingest.config.fullHash`
  for a full md5 like AN-FS) → thumbnail (ffmpeg, 320 px at 10 %) → record with `status`
  (approved|review) and `playoutStatus`.
- Watch folders (`ingest.watch`): a file is imported once its size is stable over two polls and it can be
  opened; removable drives with a `PLAYPLAY_INGEST` folder are copied into the upload area and probed.
- Missing-file sweep every 5 min (`missing` flag); a probed file whose hash or size+filename matches a
  missing record relinks it; `POST /api/v1/media/{id}/relink` does it by hand.
- Library: `GET /api/v1/library?q&type&status&client&series&tag&missing&archived&sort&limit` with facets,
  `POST /api/v1/media/bulk`, `/media/delete`, `/media/duplicates`, collections, export/import,
  `/thumbs/{id}.jpg`. Search also matches transcript text.
- Folder paths may be typed with backslashes or quotes (the earlier 400 "folder not found" came from
  testing the raw string).

## Transcription and segments (`playout/supervisor/transcriptionmanager.*`, `scripts/transcribe.py`)
- faster-whisper (CTranslate2) in a Python subprocess; levels **fast** (base), **balanced** (small),
  **accurate** (medium), **best** (large-v3) set model, beam and VAD. Word timestamps; transcript JSON at
  `<data>/transcripts/<id>.json`; the record keeps a summary and the text for search. Python path:
  `transcribe.config.python` (defaults to `C:\Program Files\Python311\python.exe`, which has
  faster-whisper 1.2.1 here; CPU int8 without a CUDA driver). Measured: a 61 s spot at `fast` in 54 s on
  the CPU.
- Segments are virtual clips (`MediaRecord.segments`, named collections in `extra.segmentCollections`):
  manual, auto by fixed interval with overlap, auto by transcript pauses, and from a transcript word
  range (`POST /api/v1/media/{id}/segments/from-words`). The hour editor places a segment through
  `SlotItem.segmentId`.

## ATSC multiplexer (`playout/supervisor/muxmanager.*`, from the original Python `broadcast_gui` tool)
- Services of the 19.392658 Mb/s transport: a PlayPlay channel (engine gets a UDP TS output on
  239.255.25.N with the service's PIDs), an external input encoded by FFmpeg (SRT/RTMP/RTSP/UDP/file/test,
  NDI and SDI go through a channel), or an already-muxed UDP stream.
- TSDuck master: null input regulated to the transport rate, PAT + PSIP (MGT/TVCT/STT, EIT/ETT 0..7
  from the PlayPlay schedule: show block title, else the first program; rebuilt every 10 min, STT every
  5 s) compiled with `tstabcomp`, one `merge` per service, bitrate monitors (JSON lines → metrics),
  continuity/PCR checks, output DekTec ASI (`tsdektec` sets the port), UDP or file.
- Statmux: bounded weighted water-filling of the video budget; channel-count presets (2..12 services).
  Alerts to the Supervisor bus and optional Twilio SMS (`alerts.twilio`). Child processes live in a
  kill-on-close job so a dead Supervisor never leaves encoders multicasting.
- Verified with a file output: 19.39 Mb/s transport, PSIP compiled, continuity and PCR OK.
