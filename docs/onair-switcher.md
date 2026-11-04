# On Air: automation + switcher

The native app's On Air page (`Play_Play_Native/qml/views/OnAirView.qml`) is one screen:

| Area | What it does | Code |
|---|---|---|
| Monitor wall | PGM (controlled channel, red tally) · PVW (selected input, green tally) · a bank with every channel's program output and the eight mapped inputs | `OnAirView.qml`, `ui/MonitorTile`, `ui/ProgramImage`, `ui/InputCard` |
| Input bus | Keys 1-8, right-click > Map to... (searchable list of library live entries, NDI sources, ONVIF cameras). Click = preview, double-click = take to PGM | `ui/BusButton`, `ui/InputPicker`, `ui/InputMap` |
| Control deck | On-air title, big countdown, next, drift, engine telemetry line; PLAY/PAUSE, STOP, NEXT, HOLD, SNAP, ARMED, LOAD HOUR, TOH, ENGINE | `OnAirView.qml`, `ui/DeckButton` |
| Rundown | Hour rundown now + next hour; on-air row shows remaining time and a progress line, next row is marked | `ui/HourRundown` |
| Graphics on air | Engine compositor layers (annotated with the overlay manager's template and text), per-layer hide, CLEAR ALL | `ui/CurrentCg` |
| Graphics deck | Quick strip, template picker, Text 1/2/3, approximate preview, SHOW / UPDATE / HIDE | `ui/GraphicsDeck`, `ui/CgPreview` |
| Status bar (every page) | fps and output bitrate/drops per channel, CPU, memory, network | `ui/SystemStatusBar`, `GET /api/v1/system/stats` |

The persistent left control column is hidden on this page (it carries its own transport and rundown) and stays on every other page.

## Switcher

* Mapping is the Supervisor setting `ui.inputMap` (`{"slots":[8 x null | {kind,title,path,mediaId,enginePath,sub}]}`), written with `PUT /api/v1/settings/ui.inputMap`.
* Take = `POST /api/v1/channels/{id}/transport {"action":"take","path":"live://ndi/<name>","title":...,"duration":1800}`; library entries also send `mediaId`. `resolveTake` turns it into `ndi://`, `sdi://` or the stream URL. A take cuts at once and the schedule queues behind it; NEXT (skip) returns to the schedule.
* Keys (On Air page only): `1`-`8` select on preview, `Alt+1`-`Alt+8` hot-cut, `Ctrl+Enter` cuts the preview, `Ctrl+Shift+Enter` takes it with the AUTO transition, `Esc` clears it. F1-F8 stay page navigation.
  `Space` (every page) plays / pauses the active channel. It keeps working after a button click and does nothing while a text input
  (text field, text area, editable combo) is being edited or a popup / dialog / menu has the focus (UXB-0).
* **CUT / AUTO**: CUT is always a hard cut. AUTO sends the take with `transition: {type, durationSec}` (MIX, DIP, WIPE, PUSH, SQZ; 0.25 / 0.5 / 1 / 2 s), which the engine runs on its A/B program bus. The choice is saved in `ui.inputMap.transition`.
* **Warm take**: the input on preview is received at full quality by the controlled channel's engine (see input monitors). A take of that input hands the open receiver to the program bus and cuts on the next frame. Any other live take opens a fresh receiver and now waits for its first picture (up to 4 s, `source.firstFrameTimeoutMs`) before cutting, instead of cutting to black while NDI connects.

## Confidence pictures

Program previews (engine `PreviewTap`, every channel tile, `/preview/<ch>.jpg|.mjpeg`):

* Area-averaged downscale (swscale `SWS_AREA`), so text and tickers stay readable; aspect ratio kept.
* Two profiles: **base** 480x270 at ~10 fps, always on; **boost** 640x360 at ~15 fps while a UI asks for it. The On Air page boosts the controlled channel every 15 s (`POST /channels/{id}/cg {"action":"preview","boostSec":40}`); it lapses on its own. Profiles can be changed the same way: `{"action":"preview","base":{width,height,fps,quality},"boost":{...}}`. Engine status: `preview` in the channel state.
* MJPEG viewers on a slow link skip pictures instead of queueing them.

Input monitors (engine `InputMonitor`, live pictures of the eight inputs without putting them on air):

* `POST /api/v1/channels/{id}/monitor {"inputs":[{key:"in1", path, mediaId, title, quality:"low"|"high"}], "leaseSec":45}`. The Supervisor resolves each input like a take and sends `{"action":"monitor"}` to the engine. The native On Air page leases the mapped inputs of the controlled channel every 15 s and on every change, and releases them when you leave the page or switch channel; when no UI renews it the engine closes every receiver after the lease.
* low = NDI at the sender's lowest bandwidth (its proxy stream), no audio, 320x180 at ~5 fps. high (the PVW input) = full bandwidth with audio, 480x270 at ~10 fps, kept warm for the take. Changing quality opens the new receiver first, so the tile never blanks.
* Pictures are filed as `<ch>:in<N>` (`image://preview/main:in3`, `/preview/main:in3.mjpeg`). Engine status: `monitors: [{key, type, quality, state: opening|open|failed|card, connected, warm, frames, stale, error}]`; tiles show LIVE / WARM, "connecting…" and errors.
* Not monitored (shown as a source card): SDI inputs (a second receiver on a DekTec port would fight the take; the card's ports also carry outputs), line-ins. Network streams (RTSP cameras) are monitored by decoding the stream in the engine.
* A tile shows, best first: the program picture of a channel that has the input on air, the monitor picture, the source card.

## CG text 1/2/3

Any template accepts `params.text1`, `text2`, `text3` (deploy, update, presets, `POST /api/v1/cg/html/{ch}`). `playout/supervisor/cgtext.h` maps textN onto the template's N-th text field: `textFields` in `web/cg/templates.json` if a template declares it, otherwise the first three text/textarea params that are not logos, icons, images or positions. A real field sent explicitly is never overridden, and `textN` stays in the params. The manifest (`GET /api/v1/cg/templates`) carries the resolved `textFields` of every template, which the web dashboard (Overlays and Integrations pages) and the native Graphics deck use for their labels.

## System stats

`GET /api/v1/system/stats` (sampled every second, `playout/supervisor/systemstats.{h,cpp}`):

```
{ atMs, cores,
  cpu:    { systemPct, supervisorPct, enginesPct, cores },
  memory: { totalMB, usedMB, usedPct, commitUsedMB, supervisorMB, enginesMB },
  net:    { inBps, outBps, linkMbps, interfaces:[{name,inBps,outBps,linkMbps}] },   // physical adapters that are up, bytes/s
  supervisor: { pid, cpu, memMB, privateMB, uptimeSec },
  channels: [{ id, state, stale, nominalFps, fps, frame, width, height, drops, lateTicks, tickMs, tickMsMax,
               bitrateKbps, outputsUp, outputCount, outputs:[{name,kind,state,connected,bitrateKbps,dropped}], pid, cpu, memMB }],
  totals: { bitrateKbps, drops } }
```
`fps` is measured from the engine's frame counter over the last ~3-8 s of telemetry; `bitrateKbps` is the sum of the output sinks' measured bitrates (the encoder stage's configured target is not counted); `stale` = no telemetry for 3.5 s. The web dashboard shows the same numbers in a footer (`web/sysbar.js`).

## Output mixes (different graphics and sound per destination)

A channel has "main" plus up to 7 extra output mixes. Every mix shows the same program bus (same video, same takes and
transitions); each graphic chooses the mixes it appears in, and each mix has its own audio faders. Typical use: the ATSC feed
carries the EAS crawl and the station bug, the web stream carries a "watch on the web" bug instead, a clean feed carries none.

* **Define**: Audio mixer > `+ Mix` (or `PUT /api/v1/channels/{id}/mixes {"mixes":[{"id":"web","name":"Web"}]}`). Saved in the
  channel profile (`extra.mixes`), applied to the running engine at once, and sent with the engine's start config. The list is
  authoritative: a mix left out is removed and its outputs fall back to main. Ids: letters, digits, `-`, `_`.
* **Route an output**: Outputs > Mix. The output entry gets `"mix":"web"`. Outputs of the same mix and encoder profile share one
  encoder; another mix gets its own.
* **Graphics**: Graphics deck > ON: ALL MIXES / MAIN / WEB... A graphic with no list is in every mix. Engine field `mixes` on a
  layer (`show`, `update`); the deck sends it as an `update` after the overlay deploy, and the engine keeps an assignment for a
  layer that is still opening for 5 s.
* **Monitoring**: each extra mix has a confidence tile on the On Air wall (`MIX · WEB`, `/preview/<ch>:mix-<id>.mjpeg`, 384x216
  at ~6 fps). Mixes whose graphics are identical share one rendered picture, so an extra mix costs nothing until it differs.
* **Engine commands** (`POST /channels/{id}/cg`): `{"action":"mix","mix":"web",name,strips,master,solo,follow,reset}` creates or
  updates one, `{"action":"mix","mix":"web","remove":true}` drops it.

## Per-source audio and meters

Every source (program, each audio layer such as line-ins, slate) is pulled once per frame and metered before its fader; each mix
then sums the sources with its own faders. In `meters` (sent with the preview frames):

* `sources: {"program": {peaks[], rms[], audible}, "L61": {...}}`: the per-source level before any fader.
* `strips[].pre`: the same, on main's strips; `strips[].peaks` is post-fader.
* `mixes: [{id, name, channelPeaksDbfs, momentaryLufs, shortTermLufs, masterGainDb, solo, duckGainDb, strips:[{id, gainDb,
  mute, explicit, inMix, peaks}]}]`.

The mixer strip shows both: the thin meter is the source level, the wide one what the strip puts into the selected mix. Mix
tabs (MAIN, WEB...) choose which mix the faders act on. Main's faders are the channel-wide ones; an extra mix follows main for
every strip it has not been given, then keeps its own value (`explicit`). Mutes and the ducker are per mix (a ducking layer only
ducks the mixes it is in); "audio follows video" off mutes the taken item in every mix. Commands:
`{"action":"audio","mix":"web","strips":{"program":{"gainDb":-6},"L61":{"mute":true}},"master":{"gainDb":0},"solo":""}`.

## Performance (frame path)

Measured on this workstation (Xeon E5-2697A v4, Arc A380, 1080p29.97) with `bench_pipeline` (`PLAYPLAY_BENCH=1`), clock-thread time
per tick for program + a static lower third + an animated full-raster page:

| path | before | now |
|---|---|---|
| GPU compositing | 16.2 ms (synchronous readback, every layer re-uploaded) | 7.5 ms (upload cache, readback 1-2 frames later) |
| CPU compositing | 4.6 ms, 11 ms CPU | 4.1 ms, 4.4 ms CPU (only the visible box of each overlay is drawn) |
| encoder RGB -> YUV | 10.7 ms | 2.0 ms (threaded) |
| decoder YUV -> BGRA | single thread | threaded (3.2 vs 7.5 ms for NV12 from the hardware decoder) |

* On this machine the D3D11 readback is the bottleneck: the Arc driver only finishes a frame's work when the CPU maps the result
  (the GPU itself needs ~4 ms), and this board cannot enable Resizable BAR. So the GPU path is not faster here for outputs that
  need CPU frames (NDI, x264, the mux, previews). **`gpuMode: "auto"`** (the default whenever `gpu` is on) times both paths on the
  live content for 60 frames each, composites with the cheaper one and re-checks every ~1800 composited frames; `"on"` forces the
  GPU. Status: `compositor.composite {path, phase, gpuMsPerFrame, cpuMsPerFrame}`.
* With the GPU on, outputs and audio are delayed by the readback pipeline (auto: 1 frame, on: 2), so the clock thread never waits on
  the GPU. Turning the GPU on later (config update) keeps the depth as it is.
* Overlay crop: large overlay pictures are scanned for their visible box once per picture buffer; only that box is drawn (CPU) or
  uploaded (GPU). `PLAYPLAY_NO_OVERLAY_CROP=1` turns it off (benchmark). GPU switches for diagnosis: `PLAYPLAY_GPU_NOCACHE`,
  `PLAYPLAY_GPU_READBACK=buffer`, `PLAYPLAY_GPU_NO_DRIVER_THREADS`.
* Decoder stats carry `transferMs` (GPU -> CPU download) and `convertMs`.

The native previews are double-buffered (`ui/ProgramImage.qml`): a single asynchronously loaded Image is empty while its next frame
loads, which showed as black flashes once previews ran at 10-15 pictures a second.

## Test hooks

`PlayPlay.exe --test-select <n>` / `--test-take <n>` run the bus-key functions for input n three seconds after start (for screenshots).

## Audio mixer (collapsible drawer under the input bus)

Strips of the controlled channel come from the engine (`metersJson().strips`, sent with the preview frames): `program` (the on-air item and operator takes) and one per audio layer. Each strip has a meter, a fader (-60..+12 dB, double-click = 0 dB), M (mute) and S (solo); MASTER has a fader and meter.

* **Line in**: `+ Line in` lists the sound card's inputs and microphones and, as loopback, any playback device (`GET /api/v1/audio/devices`, `playout/core/audiodevices.*`). A line-in is an audio layer of role `audio`, ids 61-68, source `{type:"audio_in", device, label, loopback}` (`compositor/audioinsource.*`, WASAPI shared mode, FIFO 1/4 s). CLEAR ALL of graphics leaves role `audio` alone. The list is saved per channel in the setting `ui.mixer` and re-deployed when the mixer opens on an engine that lost them.
* **Engine commands** (all through `POST /channels/{id}/cg`): `{"action":"audio","program":{"gainDb","mute"},"master":{"gainDb"},"solo":"program"|"L<layer>"|""}`; line-in gain / mute are `{"action":"update","layer":61,"audioGainDb":-6,"audioMute":true}`.
* **Solo** silences every other strip on air (the header shows SOLO ON until cleared).
* **AFV (audio follows video)**: ON (default) = a taken input brings its audio. OFF = the take carries `audioMute:true`: that item's own audio is muted (strip shows "take muted") while line-ins stay; the next item has its audio again. Saved in `ui.mixer`.
* Needs the new engine and Supervisor builds (engine restart on the live stack).

## CG per input

Right-click an input (bus key or monitor tile) > **CG on this input**: tick CG1, CG2… (checked = the input has it, "all inputs" CGs
say so), or "No CG on this input". The CG goes up when that input is taken to air and stays while it is on air (timing per CG).
Bus keys show what an input brings in their corner: `CG1,2 · PTZ1`. The CGs themselves are stored in the Graphics deck: build a
graphic with the template picker and Text 1/2/3, then `→ CG` (or right-click CG1 > Store). CG SLOTS strip: click = show / hide by
hand, right-click = store, load into the deck, on inputs (all / each input / none), start (0-10 s after the take), stay (while on air
or 5-30 s), clear, remove; `+` adds CG4, CG5… Backend and rules: docs/overlays.md "CG slots".

## PTZ drawer

Camera control for ONVIF cameras. A camera's picture can arrive as NDI (|HX), SDI, RTSP or SRT while its control is ONVIF, so the
input does not always say which camera it is:

* **PTZ1, PTZ2 …** (chips at the top; `+` adds one) are bound to ONVIF cameras: pick a chip and choose its camera in the list below
  it, or right-click the chip > Bind to camera. Saved as the setting `ui.ptzMap` (`{slots:[{id:"ptz1",name:"PTZ1",cameraId}]}`).
* Every input is set to **AUTO**, **PTZn** or **None** (right-click the input > PTZ camera). Saved on the input map slot as
  `ptz` (`"auto"` when missing); mapping the key to another source resets it to AUTO.
* **AUTO** finds the camera from the input: a library camera (`live://camera/<id>`), the camera's own stream URI, or the camera's
  address inside the NDI name, URL or title (`CAMERA (NDIHX2-192.168.1.64)` → the ONVIF camera at 192.168.1.64; 192.168.1.6 does not match
  192.168.1.64). SDI inputs and NDI names without an address need PTZn.
* The drawer's AUTO chip follows the input on preview, then the input on air, and says what it resolved (`AUTO · input 2 (PVW) →
  detected by address 192.168.1.64 · CAMERA`); the PTZ chip of that camera gets a green border. A PTZn chip controls its camera directly.
  With nothing to follow and exactly one PTZ camera in the house, that camera is used.
* Hold a pad key to move, release to stop; zoom, speed, presets (scrollable), store preset, home. Same `POST /api/v1/cameras/{id}/ptz`
  calls as the Inputs page.

Test hook: `PlayPlay.exe --test-menu <n>` opens input n's right-click menu 6 s after start, `--test-menu cg<n>` the Graphics deck's CG n
menu (for screenshots).
