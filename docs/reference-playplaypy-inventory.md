<!-- Generated 2026-10-22 from a read-only inventory of the earlier Python/PySide "PlayPlayPy" app
     (the original PlayPlay app's `pp` folder, deployed on a remote node).
     [STUB] / [NOT WIRED] / [BUG] mark things that exist in code but do not work; do not replicate them. -->

# PlayPlayPy: capability inventory

**Correction first:** this code has no OBS playout engine. `launch_ui.py:317-327` picks only `internal` (FFmpeg/Jody) or `caspar` (default). OBS is a remote-control panel for an external OBS Studio plus browser-source URLs served by the internal engine's relay on port 8080.

## 0. Entry points and process model
- `launch_ui.py`: loads `.env`, kills orphan ffmpeg/casparcg, flags `--engine --internal --caspar --caspar-auto-start --caspar-dir --api --output --remote-controller --remote-bind --remote-port --no-web-panel`; engine from `PLAYPLAY_ENGINE_MODE` else `PLAYPLAY_CASPAR=0` -> internal. After engine start: `start_toh_watcher()` and `load_and_snap_all_channels()`. `RemoteController` on port 9095. `StatePoller` -> `MainWindow.update_state`. Font Segoe UI 11; software rendering forced.
- No HTTP between UI and engine: `ApiClient` calls the engine object in-process.

## 1. Schedule model
### 1.1 Database (`pp/local_db.py`, SQLite WAL, `PLAYPLAY_DB` default `<root>/playplay_local.db`)
- No playlist, series/episode, segment or settings tables; segments are JSON on `media`.
- `schedule_slots(slot_id PK, items JSON)`; id `CH{n}-{weekday 0=Mon}-{hour}`; legacy `{day}-{hour}` for CH1. **Recurring weekly template, not dated** (week offset only relabels).
- `media`: identity (`id, raw_filename, display_title, context_summary, title`), classification (`content_type` program|ad|station_id|bumper|promo|filler, `category, source_origin, confidence, schedule_relevance`), suggestions (`suggested_channel/block/time_window`), technical (`duration, duration_sec, file_path, src, file_size_bytes, fingerprint, technical_status`), lifecycle (`status, playout_status, rights_status, human_review`), audit (`created_at, updated_at, played_count, error_message, retry_count`), metadata (`program_series, client, episode_date, transcript, notes`), assets (`thumbnail, thumbnail_chain, collection_id, tags`), `expiration_date, qc_status, allowed_channels`, `clip_segments` [{id,label,time_in,time_out,source}], `transcript_segments` [{start,end,text}], probe (`media_kind, file_extension, container, video_codec, audio_codec, width, height, frame_rate, audio_channels, sample_rate`), client (`client_id, client_match_confidence, social_summary, web_summary`). FTS5 `media_fts`.
- Hypersearch keys: type, category, status, kind, ext/format, container, codec, channel, source, tag, min/max duration, missing; scope title|transcript|all.
- `scheduling_rules(rule_id, channel, window_start, window_end, categories, types, max_age_hours, min_confidence, suggested_block, priority 50, requires_review, enabled)`.
- `channel_profiles(channel_id, channel_name, call_letters, brand_name, caspar_channel, primary_layer 10, bug_layer 20, lower_third_layer 30, legal_id_layer 80, emergency_layer 90, bug_asset, legal_id_asset, emergency_slate, output_url, connection_mode local|remote, remote_url, remote_channel_id, remote_auth_key, remote_sync_media, remote_sync_schedule, remote_auto_reconnect, remote_relay, remote_node_id, timezone, enabled, notes)`. `timezone` never used.
- `as_played(id, channel_id, channel_label, media_id, title, content_type, scheduled_duration_sec, seek_sec, started_at, source engine|jody|caspar)`: **start events only, no end/actual duration**.
- `clients(id, name, aliases, contact, notes)`; `ad_contracts(client_id, media_id, campaign_name, contract_start, contract_end, contracted_spots, price_per_spot, campaign_total, channels, status)`; `client_exposure_report` -> under_played|over_played|on_target, billed_total.
- `app_settings` keys: arm_locked, emergency_playlist, remote_node_id, remote_role, remote_control, remote_host_url, openrouter_api_key, openrouter_agent_model.
- Others: input_sources, remote_sessions, file_transfer_jobs/logs, watch_folders(path, destinations, enabled), remote_nodes, remote_media_inventory, schedule_sync_alerts.

### 1.2 Slot item dict
Keys: `id, mediaId, title, display_title, duration, type, file_path, thumbnail, fingerprint`, `timeIn/timeOut` (segments), `loop` (autofill), `source_id` + `type='live_source'`, `cg_template, cg_role, cg_params` + `type='cg'`, `media_key`. **Absent:** hard/fixed starts, padding, per-item priority, smart slots, series rotation. `planned_at` is display-only.

### 1.3 Runlist (`pp/engine/runlist.py`)
`RunlistItem(id, title, file_path, duration_sec, time_in, time_out, wall_start_ms, wall_end_ms, type, loop, seekable)`; `seek_offset_at()` clamps to out-0.1. `build_runlist_from_schedule(slot_items, hour_anchor_ms)` lays items back to back from the hour, skips cg, resolves live sources, drops pathless. `extract_cg_events_from_schedule`. `find_current_item` returns the item containing now, else the **last**.

### 1.4 Preflight (`schedule_validator.py`)
`validate_slot` -> {ok, slotId, channelId, totalDuration, errors, warnings, issues[]}; codes missing_cg_template, missing_path, missing_media, wrong_channel, channel_exclusive, expired, bad_expiration, qc_failed, rights_blocked, item_overruns_hour (warn), overrun, gap, missing_legal_id. [BUG] total uses `duration` while per-item uses in/out.

### 1.5 Weekly grid (`weekly_schedule.py`)
7 day columns (Mon-Sun) x 24 hour rows; drag MIME `application/x-playplay-media`; `ScheduleCellWidget` 108x62 (compact 58x24) with fill colour by percent, item count, fill badge, preview text, 3 px fill bar, now-playing outline `#1ccf8d`, sync markers `!N` `#ff4d4d` / `⇄N` `#ffb020`. Cell menu: Open Editor, Copy Hour, Paste Hour, Send To (Move/Copy), Clear Hour. Day menu: Copy, Paste, Send Day To, Clear. Controls: channel tabs, Prev/Today/Compact/Next. Now-playing refresh 30 s.

### 1.6 Hour editor (`hour_slot_editor.py`)
1200x700: header stats, preflight label, `TimelinePreview` bar (program #3b82f6, ad #ef4444, station_id #10b981, bumper #f59e0b, promo #ec4899, video #6366f1, image #14b8a6, gap #374151, cg #e67e22), splitter MediaLibrary | EditorList (drag reorder). Buttons: Auto Fill, AI Fill Gap, Add CG Event, Remove Selected, Cancel, Save. Live source asks duration (1-14400 s); multi-segment media opens `SegmentPickerDialog`. Save validates and confirms. `CGEventDialog`: template, layer role, show-for (default 8 s), params JSON.

### 1.7 Segment editor (`segment_editor_dialog.py`)
960x720: QMediaPlayer preview, play, position, log zoom, Fit, Auto-Segment (scene thr 0.3 | silence -30 dB/0.5 s | length 60 s), Timed Transcript, `SegmentTimelineWidget` (frame strip, roll vs independent edges, hover frame preview, hold-to-zoom 20 s, Split Here / Split After Cue, spacebar plays to next boundary, transcript cue strip), `SegmentDetailPanel` (Label, In, Out, Split at Midpoint, Delete). Saves to `media.clip_segments`. `SegmentPickerDialog`: Select All/None, Use Whole File, Add Selected.

### 1.8 ContentItem (`content_item.py`)
`ContentStatus`: detected, intake, probing, context, classify, identity, sched_match, normalizing, ready, queued, playing, played, archived; exits quarantined, needs_review, rejected, failed, expired, duplicate. `VALID_TRANSITIONS` [NOT WIRED]. `compute_fingerprint` = SHA-256 of first 64 KB + size.

## 2. Playout timing and automation
### 2.1 Shared transport semantics
- Join in progress: `play()` uses `find_current_item` then `seek_offset_at(now)`.
- Auto-advance on EOF: strict (armed, strict, not free play) snaps to wall-clock item; else next item (seeked to wall-clock when armed); end -> IDLE; Caspar free play loops to 0.
- Top of hour: 5 s polling thread calls `load_and_snap_all_channels()`; hour N runlist only, overruns cut at next hour. **No catch-up/carry-over.**
- Gap fill: none at playout time (edit-time only, or supervisor/emergency).
- Arm/strict/free-play are engine-wide although the UI shows them per channel.
- [STUB] set_item_fill, load_seg_set, move_item_to_toh (appends), `RunlistItem.loop` unhonoured, `startTime` always 0. No pre-roll/LOADBG.

### 2.2 Internal engine
`PlayoutEngine(max_channels=4)`, `StreamRelayServer` :8080, output `http://127.0.0.1:8080/feed/ch-{i}`, `USE_JODY` default true. `Channel`: FFmpeg subprocess per item (`-ss`, 1080p29.97, x264/nvenc/qsv CRF 18, GOP 60, AAC 192k, MPEG-TS; DeckLink uyvy422); pause = kill + resume with seek; 0.25 s monitor. [BUG] `time_out` never passed (no `-t`). `JodyChannel`: asyncio player, profiles srt_1080p60, rtmp_1080p60, mpegts_udp, decklink, null; also ignores time_out. `ChannelSupervisor` (internal only): tick 2 s, stall after 8 s, 3 recoveries then fallback, `_inject_filler` [BUG key mismatch]. `get_supervisor_health()` unused.

### 2.3 CasparCG engine
Host/port env; two AMCP sockets; max 4 channels; layer roles primary 10, bug 20, lower_third 30, legal_id 80, emergency 90 (env + profile overrides). PLAY `{ch}-{layer} "{clip}" SEEK f LENGTH f` (fps env default 60); STOP then CLEAR channel; PAUSE/PLAY; deadline watchdog remaining+1 s; EOF from INFO every 0.5 s; MIXER FILL letterbox from media width/height; clip name resolution; live override `go_live`/`return_to_automation`.
- **ARM-LOCKED guard** (2 s tick): forces armed/strict/not-free; reconnects or auto-starts Caspar; memory warn 8192 MB / restart 16384 MB with 600 s cooldown; re-asserts stream consumers every 60 s; hour change -> `_handle_top_of_hour`; per channel: missing file -> emergency loop; not playing/empty/error/title mismatch -> `_recover_channel` (5 s cooldown) reload+snap, else emergency loop. Status {ok, alert, lastAction, lastError, lastRecoveryAt, emergencyActive}.
- Emergency loop: `app_settings.emergency_playlist` else default order station_id, bumper, filler, promo; repeated to 86400 s / 500 items.
- Identity takes: emergency slate (CLEAR + layer 90), legal ID (80), channel bug (20); asset lookup profile -> per-channel env -> global env.
- Manual inserts need Free Play: cue_to_channel_after_current, play_to_channel_now, insert_items. Live inputs: play/stop_input_source, go_live/return_from_live.
- Server management: start/restart/stop/status, `configure_server` patches casparcg.config (video mode, media path, channels <= 16, consumer type), consumer stacks decklink|screen|system-audio|ndi|stream|file, ADD/REMOVE STREAM, validate_config, discover_media (CLS).
- `get_state()` keys: isPlaying, isArmed, armLocked, currentTime, currentMedia, playlist, channels[] (channelId, label, channelName, callLetters, brandName, casparChannel, layers, isPlaying, isArmed, isStrict, freePlay, title, currentTime, totalTime, runlistLen, outputUrl, connectionMode, remoteOnline, remoteLatencyMs, healthError, nextTitle, liveOverride, liveSourceLabel, rundown, emergencyActive), activeChannelId, engineMode, casparConnected/Version/ServerRunning/ServerPid/Memory*, casparConfiguredChannels, casparLayers, emergencyActive, scheduleGuard, lastCasparCommand.

### 2.4 Remote channel
Proxies a channel on another instance: `/api/channel/runlist` (with media sync), `/api/caspar/action` or relay enqueue, state from `/api/status` (0.2 s cache) or relay.

### 2.5 OBS
`ObsClient` (obs-websocket v5 via obsws_python): get_version, scene list/set, scene item list/enable, input list/mute, stream/record/virtualcam get+toggle. `OBSProcess` finds/launches obs64.exe (portable mode). OBS panel: browser-source URLs `http://{lan}:8080/output[N]` hard-coded CH1-3, Copy/Push, studio control (launch, host/port 4455/password, connect, toggles, lists; poll 3 s). [NOT WIRED] Refresh, connection persistence; no scene automation tied to schedule. Stream relay (internal only): /health, /api/state, /preview/{ch} mpegts.js player, /output[N] raw TS or 307 to preview, POST /feed/{ch}.

### 2.6 Input sources
`build_input_clip`: stream -> URL, route -> route://ch[-layer], dshow/mf -> dshow://video=..:audio=.., ndi -> ndi://name, decklink -> DECKLINK n. `discover_dshow_devices`.

### 2.7 Output checks
URL/consumer validators only (no black/silence detection): validate_stream_url/args, validate_decklink_consumer, validate_consumer_stack, build_stream_url, recommended_stream_args, discover_decklink_devices; protocols rtmp, srt, udp, rtp, custom.

## 3. Ingest (`pp/ingest/*`)
Inputs: Browse, Import Folder, drag-drop, watch folders (`FolderWatcher`: size stable 1500 ms then settle 5000 ms; not recursive), remote `/api/files/ingest`. `IngestWorker._process`: ffprobe + 320x180 thumbnail at 5 s + 5-frame 160x90 chain; fingerprint + duplicate check; skeleton row; Whisper on first 300 s (base.en, VAD); OpenRouter AI (type, title, display_title, context_summary, category, program_series, client, client_confidence, station, episode_date, confidence, notes); identity + client match; scheduler match (`DEFAULT_RULES`); auto tags; best thumbnail; copy to media_folder; `save_media` with status ready|needs_review and notes sections [Whisper]/[AI]/[Heuristic]. [STUB] collection mapping, normalisation, quarantine, ContentSummarizer. Toggles `PLAYPLAY_INGEST_AI`, `PLAYPLAY_INGEST_WHISPER`. Transcript backfill and batch analyser (writes display_title, context_summary, content_type, category, confidence, program_series, episode_date, client, clip_segments, tags). Auto-segment scene/silence/length. Schedule assistant `suggest_fill`. [BUG] AI fill and autofill query `status='active'` but ingest writes `ready`.

## 4. CG
Flat `cg_templates/*.html`, played as HTML producer `PLAY ch-layer "[HTML] file:///...?k=v"` (no AMCP CG ADD/UPDATE). `show_template`/`clear_template` per role; `_cg_state[(channel, role)]` in memory; scheduled CG events fire/auto-clear at 1 s. CG panel: Connect, Start Server, Rescan, Discover Media, Consumers; template list; channel/template/params (schema form for `star_news_lowerthird`: label, headline, subhead, details, style x9, animationIn x5, animationOut x4, holdMs); per-role toggles; Update On-Air (re-PLAY). Quick lower-third on control panel.

## 5. Remote / multi-site (`pp/remote/*`)
`RemoteController` :9095; `SessionManager` key -> bearer token, scopes read|command|transfer|admin, TTL 12 h. GET /health, /api/status, /api/sessions, /api/nodes, /api/relay/poll|state, /api/transfers, /api/media, /api/media/filters, /api/schedule/weekly, /api/tracking, /api/alerts, /api/files/download; static /panel/. POST /api/session/open|heartbeat|revoke, /api/node/announce (unauthenticated), /api/caspar/action {action, params}, /api/files/upload/init|{id}/complete|ingest|copy, /api/relay/enqueue|state, /api/channel/runlist, /api/schedule/weekly; PUT resumable upload chunks. Actions: play, pause, stop, next, snap, live, loadHour, arm, armLocked, strict, freePlay, connect, emergencySlate, clearEmergency, legalId, channelBug, clearChannelBug, cgShow, cgClear, runlist, scheduleWeekly. `FileBridge` (.part, offsets, fsync, SHA-256, atomic rename), `RemoteBridgeClient.ensure_media` (dedup by hash, 4 MB chunks), `NodeAnnouncer` every 10 s (nodeId, name, hostname, url, port, key, channels, relay, schedule), `RelayPoller` 0.7 s. Host imports node schedule into `node-{id8}-{ch}`, pushes week back, reconciles missing media with `schedule_sync_alerts`. Roles standalone|host|node; [STUB] controller mode.

## 6. UI
### 6.1 Main window (`main_window.py`)
1800x960 "PLAY PLAY — TV Playout System": TopBar 45 px, SubBar 42 px nav, VerticalNav 68 px + QStackedWidget, StatusBar 25 px (engine/channel/ARM-LOCKED/alerts, Python/Engine/Remote badges). No docks; splitters. Nav: On Air (ScheduleTicker 32 px + splitter [340,440,940] ControlPanel | MediaLibrary | TimelineStrip+WeeklySchedule), Schedule, Library, Ingest, Inspector, Reports, OBS URLs, CG, Live, Remote, Output. Tools menu: Reload Schedule, Snap to Top of Hour, Insert Ad Break [STUB], OpenRouter Agent, Open Multiview [STUB], OBS URLs; Config, Logs, Help. VerticalNav: ▶, CH1-CH3 (fixed 3), ＋, URLS, ▷, 📅, ▥, ▤, ≋, ↗ push, ↙ pull.

### 6.2 Panels
- ControlPanel (330-420 px): Clock, UTC, TO TOP OF HOUR countdown; + CH, CasparCG badge, MV badge; stacked ChannelPlayerCards (126 px: label, ON AIR/ARMED/IDLE, REMOTE latency, arm, now playing, elapsed, remaining, gradient progress, Take/Play/Pause/Stop/Next/Slate); collapsible Audio Levels (2 VU meters [STUB simulated], mute, monitor); schedule tools TOH, ARM LOCK, Align, Free, Break, Reload; emergency row SLATE, LOOP, BUG, ID; quick lower-third; LOAD HOUR, PUSH › CHn, copy URL; Hour Rundown table IN/OUT/Title/Dur with next-row red flash in last 10 s; rundown menu Play Now, Send to Next, Send to Top of Hour, Use as Fill [STUB], Load Segment Set [STUB]; card menu Configure Output, Test Remote Bridge, Sync Current Hour + Media, Delete Channel.
- ScheduleTicker: NOW, NEXT, OUT time, Hour Remaining. TimelineStrip 82 px hour map with playhead.
- MediaLibrary: hypersearch (180 ms), scope, Index; tabs Library / Feeds-Streams / Collections [NOT WIRED]; filters Type, Status, Media, Category, Format, Codec, Channel, Duration, availability, Sort (Newest, Title, Longest, Shortest, Largest, Most Played); 72 px rows (thumbnail, segment badge, type badge, status dot, title, tech line, play stats); context menu Cue Next, Play Now ▸, Cue After Current ▸, Rename, Open Folder, Copy Path, Copy Asset ID, Edit Segments, Generate Transcript, Retire Date, Archive/Restore, Delete; bulk Change Type, Archive, Restore, Delete.
- IngestPanel: Browse Files, Watch Folders, Import Folder, Media Folder, Clear Done, Backfill Transcripts; drop zone; job rows (stage, progress, filmstrip, tags, confidence).
- ContentInspectorPanel: tabs Review Queue / Ready / All Recent (5 s); ContentCard with Approve/Hold/Reject/Audit; AuditTrailPanel.
- ReportsPanel: As-Played Log / Client Exposure / Clients & Contracts; period 24h/7d/30d/All; AirtimeChart by type; table Started, Title, Type, Sched Duration, Channel, Source; Export CSV; exposure report with contracts under/over.
- ClientsPanel, LiveSwitchingPanel (per channel GO LIVE / Return), InputSourcesPanel (stream, route, dshow, mf, ndi, decklink; Take to Air with role), RemoteSessionsPanel (sessions, bridge URL/key, topology role, nodes), OutputRouterPanel (per channel dot/label/mode/remote/URL, Copy, Test, Configure, Open Multiview), OBSPanel, MultiviewWindow (status tiles 2x2 + master clock; Grid/Compact/Focus; transport toolbar; F fullscreen; second screen).
- Dialogs: ConfigDialog (Caspar host/port/auto-connect/dir/video mode/channels/consumer/device/windowed/auto-start, engine mode [BUG precedence], API/Output URLs, media & failsafe), ChannelConfigDialog (local/remote, bridge, identity, layer spins, assets, presets, output URL; [NOT WIRED] loopback preview), ConsumerConfigDialog (stack add/edit/remove, validate, start/stop live, restart server; DeckLink/NDI/Stream/Screen/File editors), SendToDialog, WatchFoldersDialog, AgentDialog (OpenRouter tools), LogConsole.

### 6.3 State model and polling
Dataclasses RundownItem, ChannelState, ScheduleCell, MediaItem, PlayoutState. StatePoller: 100 ms playing, 500 ms idle, 2 s offline; media 30 s; week 60 s; forced refreshes.

### 6.4 Style tokens (`styles.py`)
Base bg `#0b0c12`, fg `#e7e8f2`, font Segoe UI/Inter/Arial 12 px. Frames: TopBar `#15161d`, SubBar `#0f1018`, Panel `#10111a` (border `#242633`, r10), PanelSoft `#141521`, BlackPanel `#05060a`, StatusBar `#0a0b11`. Labels: Title 15/800 `#f4f6ff`, Muted `#8f93aa`, Cyan `#00d7ff`, Amber `#ffb923`, Green `#7fffd4`, Red `#ff6f7a`, Clock 30/900, CountdownBig 26/900 `#00d7ff`. Buttons base `#1a1c2a`/`#2b2e42`/`#d9dcef` r7, hover border `#00c8ff`; Active `#00bff3`; Green `#11382d`/`#7fffd4`/`#1ccf8d`; Amber `#3d2900`/`#ffcc44`/`#ffb923`; Red `#3d1018`/`#ff8f98`/`#ff4555`; ArmBtn `#252038`/`#b8aaff`, ArmBtnOn `#3a2a60`/`#d8c8ff`/`#9b80ff`; ArmLockBtnOn `#4a0712`/white/`#ff3040`; SchedBtn `#0e2a1e`/`#50ffb0`; LiveBtn `#280c10`/`#ff8090`/`#cc1f33`; StrictBtn checked `#3a2800`/`#ffe080`; PlayBtn `#0a2a18`/`#7fffd4`; StopBtn `#200a0a`/`#ff6060` 14/900; LoadBtn `#101828`/`#80b8ff`; PushBtn `#001c2c`/`#00d7ff`; ChannelPill checked `#002840`/`#00d7ff`. Lists `#08090f`, selected `#0d2030` with left 3 px `#00d7ff`. Inputs `#0b0c13` border `#2a2d40` focus `#00c8ff`. Accents green `#1ccf8d`, amber `#f0b429`, red `#ff4455`, cyan `#00d7ff`.

## 7. API and settings
`ApiClient` in-process facade (`cmd_*`, `cg_*`, `obs_*`, `remote_*`, `get_state`). [BUG] `cmd_schedule_update_slot` hot-reloads only legacy slot ids. `config.py`: `%LOCALAPPDATA%/PlayPlay/playplay_settings.json` with media_folder, fallback_path, auto_copy_on_ingest, search_media_folder, use_fallback_when_missing (+ caspar_* keys written, not read). `resolve_media_path`: URL -> path -> alternate drive letters -> media-folder basename -> fallback. Env vars: PLAYPLAY_ENGINE_MODE, PLAYPLAY_CASPAR*, PLAYPLAY_EMERGENCY_LOOP_SECONDS/_MAX_ITEMS, PLAYPLAY_{CH}_LEGAL_ID|EMERGENCY_SLATE|BUG_ASSET, PLAYPLAY_USE_JODY, PLAYPLAY_OBS_DIR, PLAYPLAY_REMOTE_*, PLAYPLAY_WEB_PANEL, PLAYPLAY_FILE_BRIDGE_ROOTS, PLAYPLAY_INGEST_*, PLAYPLAY_TRANSCRIPT_SECONDS, WHISPER_*, OPENROUTER_*, PLAYPLAY_DB, PLAYPLAY_ENGINE_DATA_DIR, PLAYPLAY_API, PLAYPLAY_OUTPUT. Event log: append-only `playplay_events.jsonl`.

## 8. Docs
`PYTHON_CASPARCG_MASTER_CONTROL.md` matches the code (layer contract, DeckLink fill/key, identity env, operator controls, preflight). `NATIVE_INGEST.md` matches. `PLATFORM_PLAN.md` describes the Electron/Postgres platform, not `pp/`.
