<!-- Generated 2026-10-22 from a read-only inventory of the original PlayPlay app (the Electron reference app).
     Items marked [NOT WIRED] / [ORPHAN] exist in the reference code but do nothing; do not replicate them blindly. -->

# PlayPlayClean: scheduling and playout-automation inventory

Everything below is under the original app's source tree (`reference/electron-src/` holds a copy). Line numbers are approximate.

## 1. Schedule model

### Core types (`src\lib\types.ts`)
- **WeeklySchedule** (L426): `slots: Record<string, HourSlot>`, `version`, `lastModified`, `startDate?`.
- **Slot id formats**: `"D-H"` template slot (day 0-6, Sunday=0, hour 0-23); `"YYYY-MM-DD-H"` date override (wins over template); multi-site `site:<siteId>:<day>-<hour>`.
- **HourSlot** (L359): `id, dayOfWeek?, date?, hour, items: PlaylistItem[]`; flags `isLooping, randomizeAds, keepProgramOrder`; `blockType?: general|show|news|sports|paid_program|maintenance`; `showBlock? {id,title,seriesName?,locked?,autofillTemplateId?,notes?}`; `showHour? {enabled, watchFolderPath?}`; `mergedEvent? {id, rootSlotId, slotIds[], title, durationHours, isContinuation?}`; `loopOverride? {enabled, loopUntilHour, startedAtHour?}`; `deviceCommands? [{id, commandId, offsetSec (may be negative), label?}]`.
- **PlaylistItem** (L287): `id, mediaId, media`; timing `startTime?, endTime?` (ms), `isFixedTime?, fixedTime?, timeIn?, timeOut?` (s); `smartConfig? {type: latest|previous|specific_date|random, seriesName?, targetDate?, clientId?}`, `isResolved?`; CG `cgOnStart?[], cgOnEnd?[], cgOverlays? [{id,filePath,startOffset,duration}]`; `obsCommands?[], actions?: AutomationAction[]`; segments `parentMediaId?, segmentId?, segmentLabel?`; `caspar? {loop?, transitionName?, transitionDuration?, transitionEasing?, transitionDirection?, scaleMode?}`; block semantics `timingMode?: hard_start|soft_start|fill_until`, `blockType?: media|category|latest_of_type|live|graphics|macro|fill`, `categoryId?, liveSourceId?, resolvedMedia?, resolvedAt?, isLocked?, priority? (-10..10), repeatRule?, transitionType?: cut|fade|mix|wipe, status?: scheduled|playing|completed|skipped|failed, scheduleNote?`.
- **MediaItem** (L51): `type: video|image|live|station_id|bumper|program|paid_program|ad|promo|gap`; `duration, inPoint/outPoint, programSeries, episodeDate, client`; priority fields `priority, priorityScore (0-100), priorityFrozen, priorityBurnRateMultiplier`; availability `availableStart/End, paused, pauseReplacementType, archived`; `channelExclusiveTo, applicableSiteIds, regionTags, clientGroupId`; `adMetadata {campaignId?, client?, isRotator?, weight?}`; `bumperSubtype: generic|breaking|program, bumperProgramId`; `segmentCollections[] {id,name,segments[{id,label,startTime,endTime,color?}],isActive}`; `liveSource {kind: gstreamer|ndi|capture|url|obs_scene, ...}`; `playCount, lastPlayedAt, stats`.
- **FreePlaylist** (L434), **ScheduleCommand** (L1598: show_webpage|switch_scene|play_stream|cg_display|execute_script|social_post|send_to_remote|audio_ducking|tally_control|custom with inPoint/outPoint 0-3600 s), **CommandContainer** (L1561: equipment onvif|obs|atem|tc1, conditions onStart/onEnd/onTime/onCondition, handlers onSuccess/onFailure), **SlotTimingAnalysis**, **HourGridCell** (fill, ad count/coverage, hasHardStart/hasOverrun/gapSeconds/hasConflict).

### Persistence
- `src\lib\storage.ts`: localStorage `playplay_weekly` (ch-1) and `playplay_weekly__<channelId>`; `createEmptyWeeklySchedule()` = 168 template slots (defaults isLooping false, randomizeAds true, keepProgramOrder true). Hour/day clipboard copy/paste.
- `electron\mediaDb.js:184`: SQLite `schedule_slots(id, dayOfWeek, hour, items JSON, isLooping, randomizeAds, keepProgramOrder, updatedAt)`, `schedule_meta`, `free_playlists`. **Data-loss risk:** no columns for date, blockType, showBlock, showHour, mergedEvent, loopOverride, deviceCommands.
- `electron\playlistMigrator.js`: path remap exact -> re-root -> filename -> stem -> 64 KB SHA-256 -> Levenshtein; reads/writes M3U and JSON.

### Editors
- `UnifiedScheduler.tsx` (live grid): week nav, carry-over template, AI schedule, validate. Hour menu: Edit Hour, Set/Clear Loop Mode, Device Commands, show hour, Merge into multi-hour event (1-4 h), Unmerge, watch folder, Shake ads (weighted) + undo, Copy/Paste Hour, Analytics, Clear Hour; toggles loop/randomize/keep-order; export to free playlist; send to remote; copy to next hour/day. Day menu: Copy Day, Paste Day exact, Paste Day keeping target day's ads, Clear Day. Drag-and-drop media.
- `HourSlotEditor.tsx`: Loop Hour / Randomize Ads / Keep Program Order, hour timeline, per-item CG on start/end, OBS commands, Facebook actions; embeds `SegmentEditor` and `AutoFillPanel`.
- `showHourMerge.ts`: new show-hour content drops existing program rows to priority -8, new rows 0; max 20 watch files.
- [ORPHAN] `WeeklyScheduler.tsx`, `RundownManager.tsx` (Rundown/RundownSegment types with backtime/fronttime/cueTime), `HourlyRundownPanel.tsx`, `TabbedPlaylistEditor.tsx`.

## 2. Timing rules

| Rule | Implementation |
|---|---|
| Wall-clock schedule calc | `PlayoutController.tsx:4213 calculateSchedule()`: sequential from `scheduleStartTime`; fixed-time items insert a synthetic gap; single-hour mode cuts at top of next hour (rewrites timeOut, drops later items); merged hours run continuously. |
| Hard/soft/fill analysis | `src\lib\scheduleTiming.ts analyzeSlotTiming()`: hard-start anchored (late = error); soft flows, warns into next anchor; fill_until stretches to next anchor or 3600 s; overrun past 3600 s = error. Native C++ port in `nativePlayoutLogic.ts`. |
| Pre-air validation | `scheduleValidator.ts` codes: missing_media, missing_source, expired, not_yet_available, channel_exclusive, site_unavailable, gap, overrun, timing_conflict, overfill/underfill (3600 +-30 s; underfill skipped when looping), missing_legal_id. |
| Join in progress ("boss clock") | `casparBossClock.ts computeBossClockPlayoutPosition(items, hourStartMs, now)` -> {itemIndex, itemId, seekTime, isWithinHour}; `buildCasparRunlistForBossClock` slices from the on-air item, first item's timeIn = seek. Used by LOAD HOUR/LIVE, Snap schedule, auto-push, startup catch-up. |
| Snap-to on late start (main) | `controlService.js:146`: within 2000 ms ON_TIME; late but before end -> seek delta (LATE_START_x); PAST_END; EARLY_START. |
| Top-of-hour transition | `PlayoutController.tsx:9833`: only while ARMED; polls 5 s; when expected slot != loaded slot (and not merged): stop engine, 200 ms, load expected live. |
| Auto-push at :00 | `useHourAutoPush.ts`: polls 5 s, fires minute 0 with 30 s grace, once per hour per date, every channel; boss-clock runlist; `casparPlayMultiSync` for >1 channel. Startup catch-up in `useStartupCatchup.ts`. |
| Hour Loop Mode | `HourSlot.loopOverride`; `isWithinHourLoopWindow(start, until, now)` cyclic across midnight; TOH handler holds; `handleMediaEnded` restarts first item. |
| Hour device commands | `PlayoutController.tsx:10089`: 1 s tick while armed; fires at topOfHour + offsetSec (negative = pre-roll on next hour); dedupe `bucket:commandId:offset`; must run within 60 s. |
| Auto-advance / end | `handleMediaEnded`: next item; else loop first if loop mode; else stop. Logs as-played, CG on start/end, play count. |
| Manual timing | Anchor TOH, Align clock to playhead, Free Play ON/OFF + Queue Start, Reload on schedule change (5 s poll while armed, replace from current item onward). |
| Always play latest episode | `resolveSmartSlot` (:4171): type latest or `alwaysPlayLatestEpisode` with seriesName -> newest episodeDate. |
| Automation policy | `automationStateMachine.ts DEFAULT_POLICY`: drift ignore 50 ms, soft seek 500, hard seek 2000, reload 10000, else skip; transition cut 500, preroll 5000, min event 1000; fillMode slate; maxConsecutiveErrors 3; live extension up to 300 s. |
| TOH countdown | `StudioClock.tsx:56` mm:ss, warning <5 min, critical <1 min; TTS hourly announce and 3-min no-content warning from :57. |
| STRICT/RELAX | [NOT WIRED] stored and logged only. |
| Gap-fill settings | [NOT WIRED] `fillGapsWithIds, fillGapsWithBumpers, maxFillDuration (60), shuffleAdsInHour, autoAdvance, loopAtEndOfSchedule` are edited and persisted but not read by playout. Same for `randomizeAds`, `keepProgramOrder`, `isLooping` beyond editing/validation. |
| Manual gap fill | `AutoFillPanel.tsx`: ID at top/bottom of hour, bumpers before programs (series-matched), ads in breaks (targetAdMinutes 12, maxAdBreakLength 180 s, 30 s ads). Ranking `rankMediaForSmartFill`: freshness <=80 + priority*4 + series boost (40/15) - playCount*8 + random. |

## 3. Traffic and ads
- `trafficEngine.ts`: ContentClass program|promo|commercial|psa|id|bumper|filler|hardware. TrafficItem adds clientId, contractId, targetPlays/PerDay/PerHour, allowed/blockedDayparts, category, baseWeight 1-100, priorityBoost, minSeparation, notBefore/notAfter, isActive, isPaused. Dayparts: early_morning 6-9, morning 9-12, midday 12-15, afternoon 15-17, early_fringe 17-20, primetime 20-23, late_night 23-1, overnight 1-6.
  - `calculateWeight()`: eligibility gates (active, flight, daypart, fits, separation, hourly max); delivery multipliers <50% x2.0, <80% x1.5, <100% x1.2, >=100% x0.3; daily target <50% x1.5, >=100% x0.5; boost x(1+boost/100); recency <1 h x0.7, <4 h x0.9; cap 5x base. Competitive separation is a TODO stub.
  - `selectFillContent()` weighted random without repeats; `buildBreak(BreakConfig{totalDuration, bumperIn?, bumperOut?, requireId?, idPosition, minAds?, maxAds?})` -> BreakContent.
  - PlayLog {id, mediaId, contentClass, clientId, timestamp, duration, scheduled, position scheduled|fill|makeup, daypart, hour, completed}; PlayStats per daypart/hour.
- `shakeHourAds.ts`: replace every ad with weighted random ad, weight max(e, priority+10) x adMetadata.weight; single-level undo.
- Quick break (`PlayoutController.tsx:7666`): 360-480 s break, weight max(1, priority+10)/(1+playCount*0.15), inserted after active item.
- Types AdClient{rules: SchedulingRule{allowedHours, allowedDays, maxPlaysPerHour, maxPlaysPerDay, minIntervalMinutes, priority}}, Campaign{targetPlays, currentPlays, daypartRestrictions?, separationMinutes?, competitiveExclusions?}.
- Screens: CampaignManagementPage, TrafficBillingDashboard (Spot daypart AM Drive|Midday|PM Drive|Evening|Late Night|Overnight|ROS; status scheduled|aired|missed|preempted|make-good), ClientManager/TrafficReport. [ORPHAN] CampaignManager.tsx, OverlayAdsScheduler.tsx. Automation engine ad pool selectors are never called from the operator flow.

## 4. Automation, states, events
- Renderer `automationEngine.ts` (1 Hz despite 20 Hz comment): derive -> reconcile -> RendererCommand{load|seek|play|pause|stop|fade, priority normal|urgent|correction}. ScheduleEvent priority override 4 > live_lock 3 > fixed 2 > normal 1 > fill 0; insertOverride, lockEvent, setFillPool, setSlate; fillMode slate|loop|random|black. Commands ignored unless ARMED and not Caspar-primary.
- `automationStateMachine.ts`: EngineMode standby|running; PlaybackState on_air|filling|drifted|prerolling|transitioning|gap|standby; CorrectionAction none|soft_seek|hard_seek|reload|skip|extend|fill; TransitionDecision play|skip|fill|extend|wait; DerivedState {activeEventId, expectedMediaId, expectedPosition, reportedMediaId, actualPosition, drift, correctionAction, nextEventId, timeToNext, isPrerolling, fillActive, gapDetected, rendererSettling}.
- Main-process control plane (`controlService.js`, `stateEngine.js`, `cueDispatcher.js`, `continuityClock.js`, `timeAuthority.js`): three 25 ms loops; engines internal|mpv. PlannedEvent{eventId,startTime,duration,endTime,assetId,engineId,priority,metadata}. SystemState EXPECTED_PLAYING|LATE_START|OVERRUN|DESYNC|ENGINE_UNRESPONSIVE|FAILOVER_ACTIVE|IDLE|ERROR; reasons NO_EVENT_SCHEDULED, ENGINE_NOT_REPORTING, ENGINE_STALE (>5 s), WAITING_FOR_EVENT, EVENT_LATE_START (>5 s), WRONG_ASSET, SYNC_OK, SHOULD_BE_PLAYING, EVENT_OVERRUN; 10 s overdue grace; actions START_EVENT, SWITCH_ASSET, STOP_OVERRUN. Cue{cueId, targetEngine, earliestTime, expiryTime (+5 s), status PENDING|SENT|ACKNOWLEDGED|EXPIRED|FAILED|CANCELLED}, 3 retries 1 s x n. ContinuityClock flags GREEN <=5 ms, BLUE <=20, YELLOW <=100, ORANGE <=500, RED.
- Per-item actions: facebook_post|facebook_live_start|facebook_live_stop (8 s timeout, never block playout).
- AutomationBoard: social post log, AI production cues, AI filler slates (traffic|closing|sports|obituary|history|headline|tipline|facts|psa|weather). Not a rule editor.
- `obsScheduler.ts`: one-shot OBS commands at runAt.

## 5. As-run and reports
- `asRunLogger.js` (SQLite asrun.db WAL): `asrun_log(event_id, planned_start, actual_start, engine_id, asset_id, status ON_TIME|LATE_START|OVERRUN|DESYNC|FAILED|CANCELLED, reason, metadata, timestamp)`; `scheduled_log(event_id UNIQUE, planned_start, duration, engine_id, asset_id, asset_title, client_id, client_name, metadata, timestamp)`; queryLog, queryScheduledLog, generateDiscrepancyReport (|delta| > 1 s), getUnifiedReport (LEFT JOIN).
- `playoutLog.ts`: event types play_start|play_end|play_error|fade_start|fade_complete|gap_detected|fill_inserted|override_applied|schedule_loaded|engine_start|engine_stop; entry {t, channelId, type, mediaId, mediaTitle, source scheduled|fill|makeup|manual|override, durationMs, completed, scheduledStart, actualStart, actualEnd, hourSlot, itemId, playlistIndex, detail}; daily JSONL.
- Reports: `/reports` Summary / As-Played (Actual, Planned, Event, Asset, Status, Reason; CSV) / Discrepancy; hourly CSV `YYYY-MM-DD_HNN_AsPlayed.csv` (Timestamp, ISO_Time, MediaID, Title, Class, Duration, Scheduled, Position, Daypart, Completed); detailed builder html|json|csv with variance; REST `/api/v1/reports/as-played|scheduled|snapshot`.

## 6. Multi-channel
- PlayoutChannel {id, name, scheduleId, previewEnabled, previewPosition, outputConfig{mpvIpcPath, obsSessionId, ndiOutputName}, isPlaying, currentItem, nextItem, playMode scheduled|playlist_loop|folder_loop, nonTimeConfig{mode, loopUntilHour, folderPath, sortOrder, playlistId}, casparChannel, jodyChannel, mergeGroupId}; MultiChannelState{layoutMode 1x1|2x1|2x2, maxChannels 1-32, mergeGroups}.
- ChannelOutputConfig {id ch-N, number, name, enabled, casparChannel, casparEndpoint{host,port 5250}, jodyChannel, jodyOutputs[], screen, ndi, srt (srt://0.0.0.0:9000+N), rtmp, hls, decklink}; edited in ChannelsPanel (Settings).
- PlayoutController swaps ChannelSnapshot{playlist, activeItemId, isPlaying, scheduleStartTime, loadedSlotId, currentHourSlotId, isArmed, isStrictMode, playStartTime, initialSeekTime} per channel; each channel has its own schedule key.
- `channelEngineManager.js`: one Node process per channel, ports 8101-8200; REST /api/state, /api/playlist {playlist, autoplay}, /api/start|stop|skip|seek, /health; restores paused after restart.

## 7. CG
- CGElement{type lower_third|ticker|bug|clock|weather|custom, title, subtitle, tickerText, imageSrc, colors, effects, position{x,y,width,height %, anchor x9}, animationIn/Out, animationDuration, loop, autoHideAfter}; CGPreset.
- CGTemplate{category lower_third|ticker|bug|fullscreen|social|weather|banner, filePath, parameters[{name,type text|color|number|select|url,label,defaultValue,options}]}; `cgTemplates.ts`: ANIMATION_TYPES, ANIMATION_PRESETS, 31 CG_TEMPLATES, THEME_PACKS.
- `cgUnified.ts`: UnifiedCGLayerState{type element|template, templateId, templateUrl, values, isVisible, transition cut|fade|wipe_left|wipe_right|push_up|push_down, transitionDuration, layer, channel preview|program, core{text1-3, style1-3, animationIn/Loop/Out, anchorPreset (13), margin, width, height}}; preview/program snapshot; `buildTemplateQuery`.
- CGLivePanel: Preview/Program layer lists, commitPreview, takeToProgram, updateProgram, clearPreview, clearProgram.
- Scheduling CG: per-item cgOnStart/cgOnEnd; per-item cgOverlays (startOffset/duration) to Caspar layers 20-23, manual cue layer 24; ScheduleCommand cg_display; CGScheduledEvent {templateId, values, layer, channel, startTime, endTime?, durationSeconds?, status scheduled|armed|played|skipped|cancelled} saved to cg-scheduled-events.json. [NOT WIRED] nothing executes CGScheduledEvents.

## 8. Main operator UI (`PlayoutController.tsx`, routes `/` and `/playout` use the coreOnly variant)
- ViewMode: playout|scheduler|rundowns|timeline|traffic|analytics|settings|master|multigrid|library|tvguide|broadcast|mux|multichannel|campaigns|social|talent|billing|filesync|starfile|utilities|supervisor|browsers.
- Top menu bar: On Air, Schedule, Reports, Timeline, Remote | Output, CG Templates, Tools | Config, Logs, Help.
- Left rail: channel switcher (PLAY logo, channel pills with merge colour and ON/OFF, [+] add, DETACHED badge, OBS URLs); nav icons Playout, Weekly Schedule, NLE Timeline (core), plus As-Played & Reports, Remote Sessions (core); bottom Detach output, Compact density.
- Persistent left column (380-520 px): StudioClock (time + TOH countdown); channel stack rows (CH n/name, ON AIR/STANDBY, thumbnail, current/next, next-in mm:ss, type/weight/play counter, mix strip AD/PGM/GAP/FL, timing ticker); context bar Selected/Current/Next/Countdown; transport row ARM, SCHED (snap), STRICT/RELAX, Stop, Play/Pause, Next, LIVE, Caspar toggle, MULTIVIEW, LOAD HOUR, PUSH; interface rail (insert ad break, fullscreen, open output, copy URL); PlaylistEditor bound to calculatedPlaylist.
- Right area: ScheduleTicker across top; playout view = run-of-show section (Load current hour, START/DURATION/END, Snap schedule, Free Play, Queue Start, Anchor TOH, Align clock, Load Schedule, Reload, TOH) + PlaylistEditor; scheduler view = MediaLibrary | resizer | UnifiedScheduler with channel tab bar and Auto-push pill; timeline view = PrecisionTimeline tracks Video 1, CG Gfx Overlay, Aux Audio, Switching Events + MediaLibrary.
- Theme tokens (`globals.css :root`): surfaces #05050a / #0a0a12 / #0f0f18 / #141420 / #1a1a28 / #22223a, borders #2a2a40 / #1f1f35 / #3f3f60; text #f0f0f8 / #a0a0b8 / #707088 / #505068; accent #00d4ff (hover #33dfff, secondary #a855f7); status on-air #ff2d55, standby #ff9500, ready #30d158, cued #5e5ce6, warning #ffd60a, error #ff453a; timeline colours video #5856d6, audio #30d158, graphics #ff9f0a, effects #bf5af2, live #ff2d55, preroll #64d2ff, ad #ff375f, bumper #ac8e68; fonts Sora (display), Inter (sans), JetBrains Mono; radius 3/5/8/12/16/24; spacing 4-64 px; root font 13 px at <=1400 px.
