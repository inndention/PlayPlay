# Overlay manager (Supervisor)

`OverlayManager` (playout/supervisor/overlaymanager.*, routes in supervisorcore_overlays.cpp) tracks what graphics are on air per
channel, deploys the web/cg template pack through the engine's `html` layer source (headless Chromium, see overlay-renderer.md) or
through the engine's native template kinds, and drives defaults, presets, rules and cues. All routes are under `/api/v1` and
bearer-gated like the rest. Handlers return objects: lists are wrapped (`{presets:[...]}`, `{rules:[...]}`, `{defaults:[...]}`);
PUT bodies accept the wrapped object or a bare JSON array. Errors are `{error, status}`. Channel ids `renderer`, `presets`, `rules`
and `cgslots` are reserved by the paths below.

## Layer ids (engine layer id = z order)

| role band | overlay ids (slot 0..n) | compositor role id (never handed out) |
|---|---|---|
| pillar | 11 (pillar panels, docs/pillar-panels.md) | — |
| secondary | 12 13 14 16 17 18 19 | 15 |
| bug | 21..29 | 20 |
| lower_third | 33..39 | 30 (31 karaoke, 32 fact box are caption layers) |
| ticker | 41..49 | 40 |
| legal_id | 81..89 | 80 |
| emergency | 91..99 | 90 |

Unknown roles use the secondary band. Deploy takes the first free slot (several overlays coexist); `slot` or `layer` picks one
(replacing what is there); `exclusive:true` hides the others of the band first. `GET /overlays` returns the table as `bands`.

## Routes

| method path | body -> response |
|---|---|
| GET `/overlays` | `{renderer, channels:{ch:{layers[],defaults[],connected}}, presets[], rules[], templates, bands}` |
| GET `/overlays/{ch}` | `{channelId, connected, engineState, emergency, layers[], defaults[], item{active,runlistId,title,elapsedSec,totalSec,cues[],rulesFired[]}, suppressedRoles[], bands}` |
| POST `/overlays/{ch}/deploy` | `{template, params, animation, role?, slot?, layer?, mode:auto|html|native, holdSec?, label?, hotkey?, exclusive?, filePath?}` -> layer record. 404 unknown template/channel, 409 no usable mode (message says why), 503 engine not connected |
| POST `/overlays/{ch}/{layer}/update` | `{params, replace?}` -> record. html: `update{fields}` (re-navigates when no element matches); native: rebuilt template source |
| POST `/overlays/{ch}/{layer}/retake` | -> record (html: page re-navigated, in animation replays) |
| POST `/overlays/{ch}/{layer}/hide` | `{transition?}` -> record (`state:"hiding"`). html: page `animateOut`, wait animOutDuration (default 400, max 3000 ms), engine hide, remove |
| DELETE `/overlays/{ch}/{layer}` | -> `{deleted}` (immediate engine remove) |
| POST `/overlays/{ch}/clear` | `{roles?:[...], includeDefaults?:false}` -> `{cleared[], keptDefaults[]}` |
| GET/PUT `/overlays/presets` | `{presets:[{id,name,hotkey,template,params,animation,role,mode,holdSec}]}` |
| POST `/overlays/{ch}/presets/{id}/fire` | `{params?, toggle?}` -> record (re-fire reuses the preset's slot) |
| POST `/overlays/{ch}/presets/{id}/hide` | -> `{hidden[]}` |
| GET/PUT `/overlays/{ch}/defaults` | `{defaults:[{id,template,params,animation,role,mode,enabled}]}`; GET adds `state`,`layer` |
| POST `/overlays/{ch}/defaults/apply` | redeploy every enabled default now |
| GET/PUT `/overlays/rules` | `{rules:[{id,name,enabled,channels[],negate,when:{types,categories,subcategories,keywords,series,inputs,mode:all|any},then:{suppress:[roles|"*"],show:{template,params,animation,role,startOffset,duration,endOffset} or [ {...}, ... ]}}]}` |
| GET `/overlays/rules/test?channel=&mediaId=[&type=&path=&input=in3&live=1]` | `{media, rules:[{id,name,match,reasons,then}], suppressRoles[]}` |
| GET `/overlays/{ch}/cues` | items of the current and next hour carrying `cgOverlays` / media `cgCues`, plus the active item's cue states |
| GET/PUT `/overlays/cgslots` | `{slots:[{id,name,enabled,template,params,animation,role,mode,holdSec,startOffset,duration,inputs[],channels[],mixes[]}]}`; GET adds `byInput {in1:[ids]}` and `inputs` (bus size). See "CG slots" |
| POST `/overlays/cgslots/assign` | `{input:"in3" \| 3, cg:["cg1","cg2"]}`: input 3 gets exactly these CGs -> same as GET. 400 bad input, 404 unknown CG |
| POST `/overlays/{ch}/cgslots/{id}/fire` | `{toggle?, params?, holdSec?}` -> layer record (`presetId:"cg:<id>"`, `cg`); toggle on a CG that is up hides it (`toggledOff`). 409 empty CG |
| POST `/overlays/{ch}/cgslots/{id}/hide` | -> `{hidden[]}` (hand-fired and input-fired layers of that CG) |
| GET `/overlays/renderer` | `{available,path,version,preferred,env,candidates[], engines:{ch:{html:{state,frames,fps,restarts,lastError,layers[]}}}, settings}` |
| PUT `/overlays/renderer` | `{browserPath, htmlNthFrame?, htmlFirstFrameMs?}`: stored; applies to the next engine start (nothing restarted) |
| POST `/overlays/renderer/test` | `{channel, visible?:false, timeoutMs?}` -> `{ok, error?, status, browser}` after the first frame is attached and 1.5 s of fps; the card is invisible unless `visible` |

Layer record: `{ch, channelId, id, layer, slot, role, band, template, name, mode:html|native, requestedMode, params, animation, shownAtMs,
holdUntilMs, holdSec, source:operator|preset|default|cue|rule|adopted|test, label, hotkey, state, url?, presetId?, defaultId?, cueId?,
ruleId?, itemId?, error?, note?, fellBackFrom?, restored?, engine?:{state,status}}`. States: `pending` (sent, engine has not attached it),
`on_air`, `hiding`, `suppressed`, `hidden` (a default the operator took down), `lost`, `failed`.

## Behaviour

* **Mode.** auto = html when a browser is found and the template file exists under the web root, else the native twin (categories
  lower-thirds, banners, social, bugs, score-bugs, weather, sports-extended, tickers, countdown, emergency, plus fact box, ad slate,
  countdown timer, candidate/podcast straps), else 409. An html layer that fails (engine error "CG layer N: ...") or never attaches is
  redeployed natively once (auto only) and an alert is pushed; html then waits 30 s before auto tries it again on that channel.
* **html layers** are 1920x1080 pages scaled to the channel raster. A layer with an in animation is attached at opacity 0, the page
  `replay`s, then opacity 1, so the template's animation is visible on air.
* **Rapid deploy/hide.** Hide/delete on a pending layer is held until the engine attaches it (as-run `CG_SHOW` or telemetry); delete
  leaves a 20 s tombstone and removes late attaches.
* **Hold** is manager-driven (engine `holdSec` = hold + 3 s as a dead-man switch).
* **Reconcile** every telemetry (1 Hz): a layer the engine no longer reports becomes `lost` (alert); defaults are redeployed (6 tries);
  non-default lost layers expire after 120 s; unknown engine layers in the overlay bands are adopted (`source:"adopted"`).
* **Engine connect/restart:** defaults are redeployed, operator/preset layers restored (remaining hold, dropped when < 3 s), cue/rule/test
  layers dropped (the scheduler refires cues). Leaving emergency redeploys any missing default.
* **Cues:** `SlotItem.cgOverlays` and media `extra.cgCues`: `{id, template|templateId|filePath, params, animation, role, mode, startOffset,
  duration, endOffset, requireAired}`. Item cues default in 0 / until the end; media cues default in 10 s / out 10 s before the end. Hide time =
  min(start+duration, end-endOffset); a cue whose window is under 0.5 s is skipped; one fire per runlist id; a late join fires when the
  window is still open. Time base = engine position + join offset (from the executor's join-in-progress push, or the engine `inPoint` when
  reported). Karaoke entries are left to the caption follower. Cue layers are hidden when the item ends.
* **Tokens** in params: `{{title}} {{series}} {{client}} {{category}} {{first_aired}}` (MM-dd-yyyy of the first earlier START in the as-run
  table, empty until it has aired before; `requireAired:true` skips the cue then).
* **Rules** run when the on-air item changes (type from the runlist item, category/series/keywords from the media record; keywords match
  title, tags, notes, transcript). `suppress` hides engine layers by role label (managed or not) and restores them when the item changes;
  `"*"` excludes `legal_id`, `emergency` and `pillar` unless named; layers 31/32 are never role-suppressed. `show` runs as a cue; an
  array of shows puts several graphics up (a name strap and a LIVE bug for one input, say).
  * `types` matches the runlist item's type **or** the library record's type (a news clip scheduled as `program` is still `news`),
    and `live` matches any live source: item / media type live, or an NDI / SDI / RTSP / SRT / UDP / RTMP path.
  * `inputs` matches the switcher input on air: `in1`..`in8` (or `1`..`8`) are the On Air keys (`ui.inputMap` slots, matched by engine
    path or library id); anything else matches part of the engine path (`ndi://CAMERA (NDIHX2-192.168.1.64)`) or of the input title.
  * `when.mode`: `all` (default, every listed criterion) or `any` (one is enough). `channels` always scopes the rule.
  * `negate: true` fires when the criteria do **not** match (a rule with no criteria then never fires).
  * Station rules, as the Rules tab's one-click presets write them:
    `{"name":"No graphics during ads","when":{"types":["ad","promo","psa"]},"then":{"suppress":["*"]}}`;
    `{"name":"Graphics only on news and live","negate":true,"when":{"mode":"any","types":["news","live"],"categories":["news"]},"then":{"suppress":["*"]}}`
    (graphics come back by themselves on the next news or live item);
    `{"name":"Name strap on input 1","when":{"inputs":["in1"]},"then":{"show":{"template":"lower_third","params":{"text1":"Jane Doe","text2":"Anchor"},"startOffset":2,"duration":10}}}`.
  * The rule test takes `input=in3` (a switcher input) or `type=` / `path=` instead of a media id; the dashboard's test box accepts
    `in3` and `type:ad`.
* **CG slots** (CG1, CG2 ...; setting `overlays.cgSlots`, at most 32; empty CG1..CG3 until something is stored). A slot is a stored
  graphic (template + params, Text 1/2/3 included) plus where it goes:
  * `inputs`: `["*"]` = every switcher input, `["in2","in5"]` = those On Air keys, `[]` = fired by hand only. `"3"`, `"Input 3"`,
    `"all"` / `"any"` are normalised on save. "All inputs" means the bus inputs (a take, or a scheduled live item that is mapped on
    the bus), not every program item.
  * An assigned slot runs as a rule (`id "cg:<id>"`, evaluated after the station rules, shown in the rule test) with
    `show {template, params, startOffset (default 1 s after the take), duration (0 = while the input is on air), label: name, mixes}`.
  * A take to another input that has the same CG keeps the layer up (cue state `fired`, reason "kept on air from the previous
    input"), so an all-inputs bug does not animate out and in on every cut. The CG goes when the next item does not have it.
  * Fired by hand (`.../cgslots/{id}/fire`) it is an operator-style layer (`presetId "cg:<id>"`, kept across an engine restart); the
    input rule then does not put up a second copy. `holdSec` is the hand-fire auto-hide; `mixes` limits it to output mixes.
  * Assignment from an input: `POST /overlays/cgslots/assign`. Taking an input out of an all-inputs CG turns `["*"]` into the
    explicit list of the other bus inputs (bus size = `ui.inputMap` slots, at least 8).
  * UI: native On Air (right-click an input > CG on this input; Graphics deck CG SLOTS strip: click = show / hide, right-click =
    store the deck's graphic, load, inputs, start, stay, clear; `→ CG` stores the deck in a slot) and the web Overlays page
    (CG slots tab; Deploy > Store in CG…). WS `overlayConfig {kind:"cgSlots"}` on every change.
* **Pillar panels** (layer 11): see docs/pillar-panels.md.
* Not done yet: option to hide graphics during emergency, multi-channel deploy route, per-rule show timing UI.

## Settings keys / WS events

`cg.browserPath`, `cg.htmlNthFrame` (env PLAYPLAY_HTML_NTH_FRAME), `cg.htmlFirstFrameMs`, `overlays.presets`, `overlays.defaults.<ch>`,
`overlays.rules`, `overlays.cgSlots`. WS/IPC (`broadcastUi`): `overlays {channelId, layers[]}` on every change (100 ms coalesced), `overlayConfig {kind, channelId?}`,
the legacy `cgHtml` event for operator/preset deploys, and `alert`.

## Failure modes

No browser: auto goes native (409 when there is no twin), one warning per channel per hour. Engine down: 503. Html open fails or times out
(first frame + 6 s): `failed`, alert, native fallback. Engine restart: see above. Template file missing: html impossible, native used.
