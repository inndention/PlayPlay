# OBS Scenes: the libobs live-input layer

PlayPlay can run OBS Studio's core (libobs) **headless inside a channel engine** as one live-input layer: a scene graph
of colour, image, media, text and test items with OBS filters, switched with OBS transitions (cut, fade, swipe, slide,
wipe, stinger). The layer is pulled into the compositor exactly like an NDI camera (newest frame wins, the clock thread
never waits), so scheduled playout, outputs, CG and timing stay PlayPlay's. The engineering write-up with the build
recipe, measurements and limits is [libobs-spike.md](libobs-spike.md); this page is the operator's guide to the
Supervisor routes and the two UIs.

Engines need `PLAYPLAY_WITH_LIBOBS=ON` (and the libobs install, default `D:\SDKs\obs\install`). The Supervisor routes
below exist in **every** Supervisor build; against an engine without the option a Show is accepted, the engine refuses
the layer ("unknown source type" in its log) and the status says *not on the engine*.

**Budget to keep in mind.** One libobs layer per engine process. A cut reaches the picture 2 frames after the
command; the picture is 1.3 to 1.9 frames old (two free-running clocks). The layer costs about half a core (libobs
itself 15 %, the rest is the full-raster blend) and about 225 MB that stays for the life of the engine process.

## Where it is in the UIs

| | Web dashboard | Native app |
|---|---|---|
| Page | **OBS Scenes** tab (`web/libobs.js`) | **Inputs** view, right-hand panel *OBS scenes (libobs)* (`Play_Play_Native/qml/ui/LibObsPanel.qml`) |
| Scene sets | list, select, New, Save, Delete | drop-down, New, Save, Delete |
| Editor | form built from the catalog (scenes → items → filters, transition) or raw JSON | JSON with "+ item" inserts from the catalog; Validate |
| On air | Show on the active channel, Switch (to, via, ms), Hide | same |
| Status | 2 s poll: program scene, frame age p50/p99/max, libobs fps, render ms, lagged frames, switches | same, one line |
| Dashboard strip | `libobsStrip()` (below); the On Air page's *OBS scenes* card | – |

Both UIs act on the **active channel** of the dashboard / app.

## Scene sets

A *scene set* is one saved spec: the scenes libobs holds at once, which one is program when shown, and the default
transition between them. It is stored in the Supervisor settings key `libobs.scenes` as

```json
{ "scenes": [ { "id": "studio-a", "name": "Studio A", "layer": 50, "updatedMs": 1761500000000,
                "spec": { "type": "libobs", "audio": true,
                          "scenes": [ { "name": "A", "items": [ ... ] }, { "name": "B", "items": [ ... ] } ],
                          "program": "A",
                          "transition": { "id": "fade_transition", "durationMs": 500 } } } ] }
```

`spec` is exactly what the engine's `LibObsSource` reads ([libobs-spike.md §3.1](libobs-spike.md#31-spec)). Items:
`id` (a libobs source kind), `name` (what Switch and item updates address), `settings` (that kind's own keys),
`fit` (`stretch` | `inner` | `outer` | `none`), `pos {x,y}`, `size {width,height}`, `visible`, `filters[] {id, settings}`.
Colours may be written `#rrggbb` / `#aarrggbb`. Every save is validated against the catalog; a bad spec is refused with
the first problem, never sent to an engine.

## REST

All under `/api/v1`, same bearer token as the rest of the Supervisor.

| Route | Body / answer |
|---|---|
| `GET /libobs/catalog` | `{items[], filters[], transitions[], fit[], layerDefault, onePerProcess}`; each kind `{id, name, module, params[{key,type,default,hint,values}], note}`. The editors render from this. |
| `GET /libobs/scenes` | `{scenes:[...]}` |
| `PUT /libobs/scenes` | `{scenes:[...]}` replaces the list (validated whole; 422 names the first problem) |
| `POST /libobs/scenes` | one record `{id?, name, layer?, spec}` upserted (`id` derived from the name when missing); answers `{scene, replaced}` |
| `DELETE /libobs/scenes/{id}` | |
| `POST /libobs/validate` | `{spec, layer?}` → `{ok, spec (normalised), command}` or `{ok:false, error}`: a dry run of Show |
| `POST /channels/{ch}/libobs/show` | `{sceneId}` or `{spec}`, optional `layer` (1..99, default the record's or 50), `in` (`cut`/`fade`), `inSec`, `audioMute`, `opacity`, `placement`. Sends the engine's layer **show** command with the spec as the source. Answers *accepted*, not confirmed: the engine opens libobs on its own thread. |
| `POST /channels/{ch}/libobs/switch` | `{to, transition?, durationMs?}`; `transition` a kind id or `{id, durationMs, settings}` (a stinger: `{id:"obs_stinger_transition", settings:{path, transition_point, tp_type}}`). Without it the scene set's transition is used. |
| `POST /channels/{ch}/libobs/hide` | removes the layer (so the next Show can open: one libobs source per process). `{keep:true, fadeSec}` only hides it. |
| `GET /channels/{ch}/libobs/status` | `{onAir, layer, layerState, engineState, obs{program, transition, scenes[], ageP50Ms, ageP99Ms, ageMaxMs, obsFps, obsFrameTimeMs, obsLaggedFrames, switches, switchDispatchMs, version, modules[]}, shown{...}, note}` from the engine's 1 Hz telemetry plus what the Supervisor last sent. |

The channel routes go through the Supervisor's own layer-command path (`POST /channels/{ch}/cg`), so the as-run log
shows the layer as `CG_SHOW` / `CG_HIDE` with role `libobs`, and a WebSocket event `libobs {channelId, event:
show|switch|hide|remove, ...}` follows each; `libobsScenes {scenes}` follows a store change.

Status codes: 404 unknown saved scene, 422 bad spec / bad transition / bad layer, 503 the engine is not reachable (or
the Supervisor has no loopback REST port), 200 accepted.

### Examples

```bash
curl -s http://127.0.0.1:7700/api/v1/libobs/catalog | jq '.transitions[].id'
```

```bash
curl -s -X POST http://127.0.0.1:7700/api/v1/channels/main/libobs/show -H 'Content-Type: application/json' -d '{"sceneId":"studio-a"}'
```

```bash
curl -s -X POST http://127.0.0.1:7700/api/v1/channels/main/libobs/switch -H 'Content-Type: application/json' -d '{"to":"B","transition":"fade_transition","durationMs":300}'
```

```bash
curl -s -X POST http://127.0.0.1:7700/api/v1/channels/main/libobs/hide
```

## Embedding the strip (Now Playing dashboard)

`web/libobs.js` exports a compact one-line strip for another page to embed:

```js
import { libobsStrip, libobsStripEnter, libobsStripLeave } from "./libobs.js";

// in the view builder (a string, rebuilt on every WS tick like every other view)
html += libobsStrip(channelId);            // "OBS · ON layer 50 · program A" + "→ B" (next scene) + Hide; "Scenes…" when off

// when the embedding view is entered / left
libobsStripEnter(channelId);               // starts a 2 s status poll and refreshes the strip in place (data-libobs-strip)
libobsStripLeave();
```

The strip's buttons are `pp.loStripSwitch(ch, to)` / `pp.loStripHide(ch)` (already on `window.pp` through
`ppExtSuites`); `pp.nav("libobs")` opens the full page. Nothing else needs wiring: the status lives in the page's own
cache (`LO.status[channel]`) and `onWsLibObs` refreshes it on the `libobs` WS event.

With a second argument, `libobsStrip(channelId, true)` and `libobsStripEnter(channelId, true)` give the card the On Air (Now
Playing) page embeds: one button per scene of the program on air (the live one is marked and disabled), **Hide**, and, while
nothing is on air, one **Show** button per saved scene set (`pp.loShowSaved(id, channel)`; disabled when the channel's engine is not
connected). `libobsStripEnter(ch, true)` also reads `GET /libobs/scenes` once, so the card works without ever opening the OBS
Scenes tab. Scene and channel names go into the handlers as JSON strings (`jq()`), so a scene called `O'Brien` is safe.

## What is not covered

* **One libobs layer per engine**, no NDI or browser items inside libobs (DistroAV and CEF are not built; use PlayPlay's
  own NDI / HTML sources beside the layer). See [libobs-spike.md §6](libobs-spike.md#6-what-does-not-and-why).
* The native panel edits the spec as JSON; the form editor is web-only.
* Item-level live updates (`{item, settings}` / `{item, visible}`) are engine commands you can send through
  `POST /channels/{ch}/cg` with `{action:"update", layer, item:"cap", settings:{text:"BREAKING"}}`; neither UI has a
  control for them yet.
