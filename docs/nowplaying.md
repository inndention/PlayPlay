# Now Playing (web)

The dashboard page `#nowplaying` is the desktop On Air panel (`Play_Play_Native/qml/views/OnAirView.qml`) in a browser, with Facebook Live one box away.

| Area | Where | Notes |
|---|---|---|
| PGM / PVW monitors | `web/nowplaying.js` | JPEG snapshots, 1 Hz; PGM has a red tally, PVW green |
| Input wall (8 keys) | `web/nowplaying.js` | Click = preview, double-click = take, right-click / MAP = map; CUT / AUTO bar |
| Control deck | `web/nowplaying.js` | On air, countdown, next, timecode, drift, LUFS; play / pause / next / stop / hold / snap / arm / load hour / restart |
| Graphics | `web/nowplaying.js` | Layers on air with HIDE, CG slots, quick templates, Text 1-3 |
| Rundown | `web/nowplaying.js` | On-air row with progress, next row, next hour |
| Status line | `web/nowplaying.js` | fps, bitrate, outputs, CPU / memory / network from `/system/stats` |
| Facebook Live | `web/facebooklive.js` | Shared with the Outputs tab; write-only key with show / hide |
| OBS scenes | `web/libobs.js` (`libobsStrip(ch, true)`) | Status and program scene, one button per scene to switch, Hide; while off, one Show button per saved scene set |
| Reporter feed | `web/reporter.js` (`reporterStrip(ch)`) | Live sessions with feed light, countdown, GO / Take now / Cancel / END, a one-line New feed form, the one-time publish URL and key |

The page was later extended by the "Now Playing overhaul" work (shows, arrangeable layout, program recording); this note covers the base page, bundle route and shared pieces. The nav entry is the first tab; the older channel cards stay reachable as the "Channels" tab.

## OBS and Reporter cards, skipped inputs, header

* **OBS scenes** (panel id `obs`, 6 columns) embeds `libobsStrip(channel, true)`. `onEnterNowPlaying` starts its 2 s status poll
  (`libobsStripEnter`) and leaving the page stops it. While a libobs layer is on air the card shows one button per scene (the
  program scene is marked and disabled), **Hide**, and the scene set's transition does the switch; while off it lists the saved
  scene sets (`GET /libobs/scenes`) as **Show** buttons, disabled when the channel's engine is not connected. Building scene
  sets stays on the OBS Scenes tab (the card links there). A layout saved before this panel existed gets it appended at the end.
* **Reporter feed** (panel id `reporter`, 6 columns) embeds `reporterStrip(channel)`: one row per live session with a dot that is
  green while the phone publishes, the countdown (WS `reporter` events, plus a quiet `GET /reporter/sessions` every 10 s),
  **GO / Take now / Cancel / END**, a one-line **New feed** form (name, Enter) and the one-time publish URL and key. A reporter
  feed is a library live entry, so the MAP picker lists it as "Reporter: <name>"; mapping it to a key shows its picture on the
  wall before GO. Details: [reporter-feed.md](reporter-feed.md).
* **Inputs the Supervisor will not monitor.** `POST /channels/{id}/monitor` answers `{monitored[], skipped[{key, why}]}`. The page
  keeps the list (`NP.skipped`), shows a "no source" badge with the reason as the tooltip, and stops requesting that tile's
  snapshot (it used to 404 once a second). SDI inputs, which show as a card by design, keep their kind label instead.
* **Header.** 23 tabs do not fit a window. `web/tabsbar.js` lets the tab bar scroll inside its own cell (the brand, clock and zoom
  buttons stay put), turns the mouse wheel into sideways scrolling, fades the edge that has more tabs and keeps the active tab in view.

## Shared state with the desktop

The input map is the Supervisor setting `ui.inputMap` (`{"slots":[8 x null | {kind,title,path,mediaId,enginePath,sub}], "transition":{type,durationSec}}`), written with `PUT /api/v1/settings/ui.inputMap`. The desktop `ui/InputMap.qml` and the web page read and write the same value, so mapping an input in one UI shows in the other. The mapping picker lists what the desktop lists: library live entries (`/media?type=live`), NDI sources on the network and ONVIF cameras (`/inputs`). The on-air slot is found by comparing the engine's current path with each slot's `enginePath` (case and trailing slash ignored), as the desktop does.

A take is `POST /api/v1/channels/{id}/transport {"action":"take","path":...,"title":...,"duration":1800,"mediaId"?,"transition"?}`. CUT sends no transition; AUTO sends the saved transition. Keys on this page (no field focused): `1`-`8` preview, `Alt+1`-`8` hot cut, `Ctrl+Enter` CUT, `Ctrl+Shift+Enter` AUTO, `Esc` clear.

## Pictures at 1 Hz

Nothing streams MJPEG. Each second the page re-requests `/preview/<ch>.jpg` (program) and `/preview/<ch>:in<N>.jpg` (each mapped input and the PVW) with a cache-busting query. The Supervisor already files the engine's input monitors under `<ch>:in<N>`, so **no new snapshot route was needed**: `/preview/main:in3.jpg` is the per-input snapshot.

The input pictures exist only while a client leases the monitors. The page posts `POST /api/v1/channels/{id}/monitor {"inputs":[...],"leaseSec":45}` every 15 s (the PVW input at `quality:"high"`, the rest `"low"`), boosts the program picture with `POST /channels/{id}/cg {"action":"preview","boostSec":40}`, and releases the lease (`inputs:[]`) when you leave the page or switch channel. SDI inputs and line-ins are not monitored and show a source card.

## The bundle: `GET /api/v1/nowplaying/{id}`

`playout/supervisor/nowplayingroutes.cpp` is a lane extension (`laneext.h`, order 110), added to PlayPlaySupervisor from `cmake/lane-nowplaying.cmake`. It answers one JSON per call, built from the Supervisor's own routes over loopback in parallel (`/channels/{id}`, `/state`, `/runlist`, `/overlays/{id}`, `/overlays/cgslots`, `/system/stats`, `/live/facebook`) plus `ui.inputMap` from the store. Concurrent requests for a channel share one assembly and a bundle younger than 400 ms is served again (`"cached":true`), so many dashboards cost about seven loopback calls a second. One failing part is left out and named in `errors`; the bundle still answers 200. Unknown channel: 404; invalid id: 400. `GET /api/v1/nowplaying` returns the lane info.

```
{ ok, atMs, channelId, errors[],
  channel:  { id, name, callLetters, width, height, frameRate, hidden, engineConnected, engineState, armed, lastPushReason, found },
  state:    { state, itemId, title, path, nextItemId, nextTitle, nextReady, position, duration, remaining, timecode, ... },
  onAir:    { itemId, title, path, position, duration, remaining, timecode, live, type?, plannedStartMs?, plannedEndMs?, mediaId? },
  next:     { itemId, title, ready, type?, plannedStartMs?, duration? },
  runlist:  { current:{items[]}, next:{items[]} },
  inputs:   { count:8, onAirIndex, mapLoaded, transition:{type,durationSec},
              slots:[{index,key:"in1",mapped,kind,title,path,mediaId,enginePath,sub,onAir,snapshotUrl,streamUrl,monitor?}] },
  cg:       { layers[], engineLayers[], item, slots[], emergency },
  audio, outputs[], stats (system), channelStats (this channel's fps/bitrate/drops), previewUrl, facebook }
```

If a Supervisor lacks the lane (option off), the page falls back to assembling the same shape in the browser from the ordinary routes (`assembleClient()` in `nowplaying.js`), a request set about seven times heavier.

The lane builds by default only in `build-seg`, `build-lanes` and `build-np`; elsewhere `-DPLAYPLAY_BUILD_NOWPLAYING=ON` is needed, so the live `build\` is unchanged until reviewed.

## Facebook Live module

`web/facebooklive.js` exports the state (`FB`), the card (`viewFacebook(chans, {big, presets})`), `loadFacebook()` and the `pp.fb*` handlers (`ppExtFacebook`). `web/mux.js` imports it: the Outputs tab renders the same card (compact), `X.fb` is `FB`, and `ppExtMux` spreads `ppExtFacebook`. Routes: `GET/PUT /api/v1/live/facebook`, `POST .../start` and `.../stop`. The key is write-only: the server answers only `hasKey` and `keyHint`.

## Tests

* `tests/test_nowplaying.cpp` (target `test_nowplaying`, from `cmake/lane-nowplaying.cmake`): registration, route table and station copy, a full bundle over the wire against stub upstream routes (shared map, monitors, on-air slot, snapshot URLs), cache and coalescing, a failed part reported in `errors`, 404 / 400.
* `tests/web/nowplaying_render.test.mjs`: `node tests/web/nowplaying_render.test.mjs`. Renders the page against stubs; checks tally, eight keys, JPEG-only pictures, deck, graphics, rundown, status line, Facebook box, the select / CUT / AUTO / map / CG / Facebook handlers, lease release on leave, the client fallback and the Outputs tab card.
