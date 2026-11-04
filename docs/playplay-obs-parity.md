# PlayPlay OBS parity inventory

Generated 2026-10-22 against PlayPlay OBS (the FastAPI station controller, `playplay-obs/server/*.py`, 395 KB
`api.py`, 60 web docks) and the C++ port at Phase 9. The owner's rule: **if it is a feature in PlayPlay OBS it
has to be represented here.** Every PlayPlay OBS route group / dock is listed with its status in the port.

| Status | Meaning |
|---|---|
| **Verified** | Built in the port and exercised against the running Supervisor / engine / hardware. |
| **Built** | Implemented in the port, not yet exercised on real data or hardware. |
| **Replace** | The port covers the need with a different mechanism (reason given). |
| **Port** | Still to be ported (phase noted). |
| **Retire** | Not carried forward (OBS-specific or experimental). |

## 1. Scheduling and playout core

| PlayPlay OBS | Port | Status | Where |
|---|---|---|---|
| Weekly grid `channel × day × hour`, dated overrides beat the grid | Template `D-H` + dated `YYYY-MM-DD-H` slots | **Verified** | `ScheduleStore`, `/api/v1/schedule/{ch}/week\|slot` |
| Wall-clock engine, 250 ms correction, late-start seeking | Executor per second, engine hard starts, join in progress | **Verified** | `ScheduleExecutor::composePlaylist` |
| **Query blocks** (`query_block` types/target_sec/prefer/categories/subcategories/series/nth_latest, late bound, no-repeat 30 min, length fit) | `SlotItem.queryBlock` resolved in `RunlistBuilder::expandQuery` via `QueryBlocks`; picks cached per hour; cross-hour exclusion; contract pacing bonus | **Verified** | `playout/schedule/queryblocks.*`, `/programming/query/preview` |
| **Folder blocks** (random weighted by freshness / uniform) | `SlotItem.folderBlock` + `order` random\|name\|date\|size\|added (round robin) | **Verified** | `QueryBlocks::resolveFolder` |
| **Series blocks** (`series` + `nth_latest`: latest / last week's in one hour) | `queryBlock.series[] + nthLatest`, positional, exclusions ignored | **Built** | `QueryBlocks::candidates` |
| **Hour recipes** (`GET/PUT /api/hour-recipes`, `POST /schedule/{ch}/hour-recipe` scope hour/day/week, keep_locks, empty_only) | `/api/v1/programming/recipes`, `/schedule/{ch}/hour-recipe` (+ `dated`, `hours[]`, `dows[]`, preview/apply) | **Verified** | `FormatClock::layRecipe` |
| "Programs with ad breaks every N minutes" | Ad-break recipe builder (`/programming/recipes/ad-break`) with program/break/autogen/ID parts | **Verified** | `FormatClock::adBreakRecipe` |
| **Locked blocks** (`locked`, `lock_at_sec`, `lock_label`; leads vs anchors; carry through fills/shuffle/paste; nearest boundary) | `SlotItem.locked/lockAtSec/lockLabel`; `RunlistBuilder::seatAnchors`; `FormatClock::carryLocks`; `/slot/{id}/lock` | **Verified** | |
| **Weighted fill / format clock** (`program_weights` buckets by airtime pct, rotation least-played→freshness→jitter, deficit round robin, anchors reserved, one program per hour, long-program segmenting + carry) | `/channels/{id}/weights`, `/schedule/{ch}/weighted-fill` | **Verified** | `FormatClock::fillWeighted` |
| **Autofill policy** (`order`, `no_repeat_min`, `enabled`, contract pacing) | `/channels/{id}/autofill` + executor synthesis of empty hours (recipe or weighted) | **Verified** | `ScheduleExecutor::effectiveOrSynthesized` |
| **Ad contracts** + pacing (behind-pace bonus, fulfilled back-off) | `traffic.contracts`, `/traffic/contracts`, `/channels/{id}/pacing`, `ScheduleStore::airedCount` | **Built** | `FormatClock::contractPacing` |
| Shuffle / clear hour, copy hour/day, paste | `/slot/{id}/shuffle`, `/schedule/{ch}/clear`, `/schedule/{ch}/copy` | **Verified** | |
| Resolved preview ("what would air") | `/slot/{id}/resolved` (editor state or stored) | **Verified** | `ScheduleExecutor::previewSlot` |
| Re-roll picks | `/channels/{id}/reroll` | **Verified** | |
| Segments (segment sets, play in→out of the mother file) | `MediaRecord.segments`, `SlotItem.segmentId`, transcript-based split | **Verified** (Phase 8) | |
| Date overrides route | `PUT /schedule/{ch}/slot/YYYY-MM-DD-H` | **Verified** | |
| AI fill / AI enrich (OpenRouter) | Deterministic recipes/format clock; LLM only for spot scripts | **Replace** | by design: no AI in the on-air path |
| Free mode / free playlist, cue-next/take/hold/resume/preview | Transport actions + TAKE from the library | **Verified** | |
| Touch scheduler `/sched` (phone) | Web dashboard is responsive; no dedicated tray UI | **Port** (Phase 10) | |
| MasterList popup editor | Hour editor + weekly grid + Programming page | **Replace** | |

## 2. Generated media (newsroom / newsspots)

| PlayPlay OBS | Port | Status |
|---|---|---|
| Recipes (weather ×5, headlines, story, community, events, obits, sports, roads, almanac, history, economics, tech) | `SpotGenerator` recipe table (settings-overridable) | **Built** |
| LLM script → Kokoro TTS → card → ffmpeg → ingest to `autogen rotation` with TTL | Template writer (+ optional OpenRouter), `spot_tts.py` (Kokoro → SAPI fallback), `spot_card.py` furniture + ffmpeg two-plane compose | **Verified** (TTS, compose) / **Built** (end to end) |
| Spot jobs: interval or clock times, days, grace, standing instructions, placements channel/hours/minute/days | `spots.jobs` + 30 s scheduler | **Built** |
| GEN markers: `auto_spot` locked items, enforce on save, freshness colours | `extra.autoSpot` + `queryBlock` markers, `enforceMarkers()` hooked into slot PUT, marker states fresh/due/stale/off | **Built** |
| Newsspots scatter (RSS → story spots → ≤N/hour banded random minutes, skip on-air hour) | `/spots/scatter/*` | **Built** |
| Revise a spot with a note | `POST /spots/spot/{mediaId}/revise` (note to the LLM writer, `revisionOf` on the new record); guided spot studio (docs/spot-studio.md) | **Verified** |
| Generated Media dock | Spots tab (web) + Spots page (QML) | **Built** |

## 3. Reports

| PlayPlay OBS | Port | Status |
|---|---|---|
| As-played log + CSV | `/reports/asplayed.html\|.csv`, JSON | **Built** |
| Per-asset report / asrun / asscheduled | `/reports/asset/{id}.html` (airings by day, upcoming placements incl. eligible blocks, dayparts, totals) | **Built** |
| Assets report (many assets, filters) | `/reports/assets.html?ids=&q=&type=&category=&client=&channel=&days=` | **Built** |
| Full / period report (daily/weekly/monthly, group by type/client/series/category, narrative) | `/reports/full.html` | **Built** |
| Adherence (tolerance 90 s) | `/reports/adherence.html` | **Built** |
| Contracts delivery | `/reports/contracts.html` | **Built** |
| Top assets / compare | `/reports/top.html`, `/reports/compare.html?ids=` | **Built** |
| Branded print-ready HTML shell | `station.brand` settings, same masthead/cards/tables layout | **Built** |
| AI narrative / "ask the report" | Template narrative only | **Replace** |

## 4. CG

| PlayPlay OBS | Port | Status |
|---|---|---|
| Templates with typed fields, in/out animations, presets with hotkeys | HTML CG pack (`web/cg`, 50 templates) + Qt template sources | **Verified** (Phase 8) |
| CG Text (update live layer without re-take) | `cg update` action → `LayerSource::update` | **Verified** |
| Karaoke caption overlay (whisper words) | `TemplateSource kind karaoke`, follow-program poller, `/channels/{id}/cg/karaoke`, `web/cg/karaoke.html` | **Built** |
| Fact box | `TemplateSource kind factbox`, presets, `/channels/{id}/cg/factbox`, `web/cg/factbox.html` | **Built** |
| CG rules (suppress during ads, keyword/category triggers) | `cgOnStart/cgOnEnd` per item; rules engine | **Port** (Phase 10) |
| Per-asset CG cues (in +10 s / out −10 s), First Aired notice | `cgOverlays` per item | **Built** / **Port** (First Aired) |
| News ticker (NWS alerts + operator alerts + RSS) | `ticker` template kind; feed-driven cycle | **Port** (Phase 10) |
| Vertical backdrop (9:16) / aspect classes | Compositor fit modes | **Replace** |
| AI CG designer | | **Retire** |

## 5. Mux, outputs, encoders

| PlayPlay OBS / broadcast_gui | Port | Status |
|---|---|---|
| Branch Output per channel (RTMP/SRT/record) | Per-channel outputs: ndi, udp, srt, rtmp, file, dektec_asi, dektec_sdi with `enabled/autoStart/retry`; start/stop/restart per output | **Built** |
| ATSC mux, statmux, PSIP/EPG, DekTec ASI | `MuxManager` (TSDuck `tsp`, `tsdektec`), NDI service sources via fixed-source channels, advanced encoder config, table overrides, dry run, capabilities | **Verified** (UDP) / **Built** (ASI port, NDI) |
| Encoder presets | ATSC MPEG-2/AC-3 HD/SD, ATSC H.264, stream H.264/HEVC, QSV (when present) | **Built** |

## 6. Redundancy and remote control

| PlayPlay OBS | Port | Status |
|---|---|---|
| Sessions: capture / recapture / clone / export / import / apply (groups, dry run, media resolution) / push / pull | `SessionsManager`, `/api/v1/sessions/*` | **Built** |
| Failover: primary/backup, heartbeat, grace, promote/demote, epoch split-brain, replication, media pulls | `FailoverPair`, `/api/v1/failover/*` | **Built** |
| Sync dock / remote nodes, asset presence, transfers | `/api/v1/remote/*` (status fan-out, proxy, push/pull sessions) + AN-FS / Star transfers (Phase 8) | **Built** |
| LAN password, HTTPS listener | REST bearer token, station password on `/api/` | **Built** / **Port** (TLS) |

## 7. Library, ingest, media pipeline

| PlayPlay OBS | Port | Status |
|---|---|---|
| Watch folders, ffprobe, fingerprint, thumbnails, classification, transcription | `IngestManager`, `TranscriptionManager` (faster-whisper) | **Verified** (Phase 8) |
| Ingest audit queue (fix type/title, dedupe, approve/reject) | Library status review/approved, duplicates, bulk edit | **Verified** (Phase 8) |
| FTS search titles + transcripts | `searchMedia` + transcriptText | **Built** |
| Taxonomy (categories / subcategories / channel scoping) | `extra.category/subcategory`, `allowedChannels`, facets endpoint | **Built** |
| Happy Radio music intelligence | | **Retire** (unless requested) |
| Legacy import | PlayPlayClean JSON import; others per module | **Port** |

## 8. Live, field, social, OBS-specific

| PlayPlay OBS | Port | Status |
|---|---|---|
| Multiview / Now Playing docks | On Air page, web channel cards + MJPEG previews | **Verified** |
| Field WebRTC (mediamtx WHIP/WHEP), Live dock, Facebook Live comments | | **Port** (Phase 10) |
| Live news pipeline (record, classify, replay) | | **Port** (Phase 10) |
| Automation macros / triggers (obs-websocket) | Device commands + CG hooks; no OBS | **Replace** |
| WebView input, Canvas layouts, Switcher/OBS mixer docks | HTML source in the compositor; no OBS | **Replace** / **Retire** |
| PTZ, Inputs (NDI/SDI discovery) | ONVIF + NDI discovery (Phase 8) | **Verified** |
| Social publishing (social.py + publisher.py) | Publisher: drafts, review ladder, schedule, Facebook/website send, corrections, retractions, metrics, kill switch (docs/publisher.md) | **Verified** (against a mock Graph/WordPress) |

## Context menus (operator surface)

Right-click menus now exist on: weekly grid cells (edit, fill with recipe template/dated, weighted fill, shuffle,
validate, copy hour/day, clear, delete), hour editor rows (move, duplicate, lock / lock at / label, hard/soft/fill
timing, edit block, open media, asset report, remove), library rows (add to hour, folder block, take, edit,
report, transcribe, set type, archive, remove), channel cards (transport, arm, load hour, snap, re-roll, schedule,
programming, report, restart engine), rundown rows (skip, re-roll, open media, report) and as-run rows (reports).
Both the web dashboard and the QML app.

## 9. Render nodes, Star nodes, spot studio, publisher, agents (2026-10-23)

| PlayPlay OBS / Star | Port | Status |
|---|---|---|
| mesh_render.py (opt-in peer dispatch, durable request ids), studio_render.py (spot-node Jobs API), star split render | RenderFarm, Render page (docs/render-nodes.md) | **Verified** (spot-node, local) / **Built** (peers, StarAgent segments) |
| star-node daemon (fleet, sync, swarm, browse, exec, watchdog), stations.py allowlist, /api/nodes, /api/fleet | StarNodes, Star page (docs/star-nodes.md) | **Built** (verified against a mock node) |
| Guided spots (spot_start/update/generate), review, publish into ad rotation | SpotStudio, Studio page (docs/spot-studio.md) | **Verified** |
| social.py / publisher.py / pub_* lane | Publisher, Publish page (docs/publisher.md) | **Verified** (mock Graph) |
| console.py operator commands, /api/op/*, /api/ai/*, StarAgent tasks | AgentSuite, Agents page (docs/agents.md) | **Verified** (LLM assistant needs a key) |
