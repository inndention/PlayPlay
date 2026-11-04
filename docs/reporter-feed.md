# Reporter feed — a reporter goes live from the getstarnews app into PlayPlay

Status 2026-10-26: built in `build-seg` (lane `reporterfeed`, option `PLAYPLAY_BUILD_REPORTERFEED`, ON only in
build-seg / build-lanes), not deployed to the live build. See "What is verified" at the end.

## How the app goes live

Source of the sibling web app (a separate project, not part of this repository):

- **The site** (PHP, `api_live.php` + `inc\live.php`) never carries video. It keeps a registry of who is live
  (`list / get / config / start / heartbeat / end / moderate`, Bearer token against `users.remember_token` or the PHP
  session) and, for the "Star News page" target, asks Facebook Graph (`v21.0`) for a live video with the page token and
  hands the phone the one-time `rtmps://` stream URL (`inc\live.php:756-787`; logs redact the key, `:655-668`).
  Viewers get Facebook's player through `permalink_url` / `embed_url`; the React app polls the list every 30 s.
- **The phone app** is a React + Capacitor shell (`app\`) whose live studio is native Android Kotlin, **LiveCast**
  (`app\android\livecast\src\main\java\com\livecast\app\`, also a copy at `getstarnews_new\LiveCast\`). It encodes with
  RootEncoder (`streaming\StreamEngine.kt`: `GenericStream`, Camera2 + microphone, 48 kHz audio, adaptive video bitrate)
  and **publishes RTMP / RTMPS** (`goLive(rtmpUrl)` → `stream.startStream(url)`). No WebRTC/WHIP, HLS upload or
  MediaRecorder chunking is used for this feature (the call-in feature uses LiveKit at `wss://wyat.getstarnews.com`,
  unrelated).
- **Three targets** (`streaming\StreamConfig.kt`, picked in `ui\live\LiveViewModel.kt:438`):
  1. *Phone → Facebook*: the phone calls Graph `POST /{me|page}/live_videos` itself and streams to `secure_stream_url`.
  2. *Phone → Star News page*: `api_live.php?action=start` returns the page's `rtmps://` URL once; the phone streams there.
  3. *Phone → PlayPlay station*: RTMP to the `publish_url` of a PlayPlay **field session**. The client
     (`playplay\PlayPlayApi.kt`, `PlayPlayController.kt`) speaks the OBS-era station API: `POST /login` (station
     password form → `pp_auth` cookie), `GET /api/field/sessions?all=0|1` (`{sessions, stream, channels}`),
     `POST /api/field/sessions {reporter, channel_id, lower_third1/2, bug_text, countdown_sec, transport:"rtmp"}`,
     `PATCH .../{id}`, `POST .../{id}/golive {skip_countdown}`, `/take-now`, `/cancel`, `/end`, `/heartbeat`.
     The app starts pushing RTMP **as soon as GO LIVE is tapped**, before the cut; the station runs the countdown
     (`status: idle → countdown → live → ended`) and the phone only mirrors `countdown_sec` on its screen while
     `status == "countdown"` (`LiveViewModel.kt:246-270`). The app polls the session list every 2.5 s and takes
     `publish_url` from it.
- **The old station side** (`server/live.py`, `webrtc.py` in the PlayPlay OBS tree) ran MediaMTX on
  1935 with a shared `field` credential; the publish URL was `rtmp://<lan>:1935/<slot>/live?user=field&pass=<key>`
  (RootEncoder needs the credentials on the stream name, not as userinfo), OBS pulled `rtmp://127.0.0.1:1935/<slot>/live`
  with an `ffmpeg_source`, and `go_live` showed a countdown CG then cut the scene.

**Consequence for PlayPlay**: the receiver must accept an **RTMP publish with a stream key**; that is what the app
does today with any of its three targets. A **WebRTC WHIP** publish is not needed for the app but is the other common
case, so the design keeps a `mediamtx` mode where a MediaMTX the operator runs takes RTMP or WHIP and the engine pulls
RTSP from it (URLs only; PlayPlay does not run MediaMTX; none is installed on this box).

## What was built

`playout/supervisor/reporterfeed.{h,cpp}` (registered through `laneext`, added to PlayPlaySupervisor by
`cmake/lane-reporterfeed.cmake`), `web/reporter.js` (tab "Reporter"), `Play_Play_Native/qml/ui/ReporterFeedPanel.qml`
(On Air page, right column, above the graphics on air), `tests/test_reporterfeed.cpp`.

### Media path (mode `ffmpeg`, default)

```
phone (LiveCast / any RTMP app) --rtmp://<host>:1936+slot/reporter/<key>--> ffmpeg.exe -listen 1  (one per session)
                                                                               | -c copy -f mpegts
                                                                               v
                                                     udp://127.0.0.1:50040+slot  <-- the engine's `stream` live source
                                                     udp://127.0.0.1:50140+slot  <-- a second copy of the stream, for the input monitors
```

- A session takes the first free **slot** 0..portCount-1: RTMP port `rtmpPortBase+slot` (default 1936..1943; 1935 is
  never used by default) and engine UDP `udpPortBase+slot` (50040..). Each slot is one ffmpeg.exe (`ffmpeg.exe` next to
  the Supervisor, else on PATH; `config.ffmpegPath` overrides). The listener is started when the session is created, so the
  phone can push before the cut, and is restarted (0.5 s, or 5 s after an immediate exit such as a port conflict) whenever
  the publisher leaves while the session is active. The **stream key is the RTMP stream name**. ffmpeg's listener only *warns*
  about a publish with another name and would ingest it anyway (verified with ffmpeg 9), so the Supervisor reads the
  listener's output and ends the listener the moment it sees "Unexpected stream": that publisher is dropped, the
  session shows `lastError: a publisher with a wrong stream key was refused`, and the listener is back 300 ms later.
  The warning line names the real key, so the listener output is never logged.
- The feed is a **library entry** `live://reporter/<id>` (type live, title `Reporter: <name>`, `liveSource
  {type:"stream", url:"udp://127.0.0.1:<port>", realtime, lowLatency}`), so the On Air input map (library live entries)
  can map it to a bus key and the existing take resolver (`resolveTake`) finds it. `GET /api/v1/inputs` lists active
  sessions under `reporter` (`{name:"Reporter: x", type:"stream", url, path, sessionId, status, feedUp, channel}`).
- **Two copies of the stream.** A UDP port takes one reader. When the feed was mapped to a bus key, the wall's monitor and the take
  (GO) both opened `udp://127.0.0.1:<udpPortBase+slot>`, and the take failed with `open failed: Error number -10048` (address in use).
  The ingest therefore writes the stream twice from the one input: the monitors' copy on `udpPortBase+100+slot` (50140..), then
  the engine's own port last. The library entry's `liveSource` carries `monitorUrl`; the Supervisor's
  `POST /channels/{id}/monitor` swaps it in for the monitor lease only, and a take keeps using `url`. Mode `mediamtx` needs no
  copy (RTSP serves many readers). `GET /api/v1/inputs` shows `monitorUrl` too.
- **Children die with the Supervisor.** Each ingest ffmpeg is put into a kill-on-close job object (as `MuxManager::adopt`), so a
  crashed or killed Supervisor no longer leaves listeners holding ports 1936.. for the next one (two orphans were left on
  2026-11-02). Sessions are in memory only, so at start the service also deletes the `live://reporter/*` library entries a
  previous run left behind.
- **Go live**: `POST .../go` sets `countdown` and `remainingSec`; a 1 s timer ticks every counting session and
  broadcasts WS `{type:"reporter", event:"countdown", id, remainingSec, session}`; at zero the Supervisor posts
  `{action:"take", path:"live://reporter/<id>", mediaId}` to its own `POST /api/v1/channels/<ch>/transport` (loopback,
  with the Supervisor's bearer token) — the same route the On Air page uses, so the schedule queues behind the take and
  `END` (`{action:"skip"}`) returns to it. While the transport request is pending the session reports `taking`. A refused take (engine not connected, 503/422) returns the session to `idle` with `lastError`. A successful response only confirms that the command was accepted; use the channel program monitor to confirm the picture. Engine as-run failures return the session to idle with the failure reason. In ffmpeg mode, if no reporter is publishing when the countdown ends, the take is refused with an error.
- The stream key is 24 hex chars from the system CSPRNG; the session keeps only its SHA-256 (`keyHint` = first 8 hex) and
  the listener holds the plain key in memory until the session ends. It is returned **only in the create reply** (both
  the v1 and the legacy create). Nothing logs it (the ingest log line shows the port only).

### REST

| Route | Body / reply |
| --- | --- |
| `POST /api/v1/reporter/sessions` | `{reporter, title?, channel?, countdownSec?}` → session fields + `publishUrl`, `streamKey`, `enginePath`, `keyShownOnce:true` (and `whipUrl` in mode mediamtx) |
| `GET /api/v1/reporter/sessions[?all=1]` | `{sessions:[...], host, mode}` (active only unless all=1) |
| `GET / DELETE /api/v1/reporter/sessions/{id}` | the session / `{deleted}` (DELETE ends it first) |
| `POST .../{id}/go` | `{countdownSec?}` (omit = the session's; 0 = take at once) → session |
| `POST .../{id}/take-now`, `/cancel`, `/end` | → session; 409 when the state does not allow it, 404 unknown |
| `GET / PUT /api/v1/reporter/config` | `{host, rtmpPortBase, portCount, udpPortBase, app, mode, mediamtxPublishBase, mediamtxWhipBase, mediamtxPullBase, defaultCountdownSec, ffmpegPath}` (setting `reporter.config`; ports/mode apply to sessions created from then on) |
| `GET/POST /api/field/sessions`, `PATCH .../{id}`, `POST .../{id}/golive|take-now|cancel|end|heartbeat` | the LiveCast app's OBS-era contract (snake_case: `channel_id`, `countdown_sec`, `transport:"rtmp"`, `publish_url` on create only) |

Session JSON: `{id, reporter, title, channel, status, countdownSec, remainingSec, slot, keyHint, feedUp, ingestRestarts,
lastError, createdMs, goMs, liveMs, endedMs}`. WS events (`type:"reporter"`): `session`, `countdown` (each second),
`live`, `cancel`, `ended`, `feedUp`, `feedDown`.

### Mode `mediamtx` (designed, not run here)

`PUT /api/v1/reporter/config {mode:"mediamtx", mediamtxPublishBase:"rtmp://<lan>:1935", mediamtxWhipBase:"http://<lan>:8889",
mediamtxPullBase:"rtsp://127.0.0.1:8554", app:"reporter"}`. The create reply then gives `publishUrl =
<publishBase>/reporter/<key>` (RTMP) and `whipUrl = <whipBase>/reporter/<key>/whip` (WebRTC WHIP), and the engine
pulls `<pullBase>/reporter/<id>`. MediaMTX must be configured so the path `reporter/<key>` is published under the
name `reporter/<id>` (a `runOnReady`/path alias), or `mediamtxPullBase` pointed at a path that MediaMTX exposes for the key; no
listener process is started. Nothing on this box runs MediaMTX; this mode is config only.

## Operator steps

1. Dashboard → **Reporter** tab (or On Air → the "Reporter feed" panel, NEW). Reporter name, title (shown as the lower
   third text in the legacy contract), channel, countdown seconds → **Create session**.
2. Copy the **publish URL** (and/or server + stream key) from the yellow "shown once" box and send it to the reporter.
   It is not shown again; a lost key means a new session.
3. The reporter, in the getstarnews app:
   - *Generic RTMP app* (Larix, Streamlabs, OBS mobile): server `rtmp://<host>:<port>/reporter`, stream key `<key>` — or
     the whole publish URL where the app takes one.
   - *LiveCast "Phone → PlayPlay station" target*: Station tab → host `<supervisor ip>:<rest port>` → create the session
     from the app. **Bench client update:** the local LiveCast source falls back to bearer-token authentication when cookie login fails and keeps the one-time `publish_url` in memory across polling and edits. Use the Supervisor REST bearer token as the station password. A new session is needed if the app process loses that URL. This source change still needs an installed-app and phone-to-station test.
4. Watch the session card: the dot turns green ("phone publishing") when ffmpeg logs the input. The feed is now on the On
   Air input map as **Reporter: <name>** (map it to a bus key to preview it).
5. **GO** starts the countdown (big digits on the card and the panel; the phone mirrors it in the legacy contract). At zero
   the feed is taken to program on the channel. **Take now** skips the countdown; **Cancel** stops it.
6. **END** returns the channel to its schedule (transport skip), stops the listener and removes the library entry. The
   session stays listed under "show ended" until deleted.

Firewall: the first session on a port makes Windows ask about ffmpeg.exe inbound TCP (1936..). Ports and the advertised
host are in `GET/PUT /api/v1/reporter/config`; the host defaults to the first non-loopback IPv4.

### On the web On Air page

The *Reporter feed* card (`reporterStrip()` in `web/reporter.js`, embedded by `web/nowplaying.js`) has the same controls in compact
form: type the reporter's name, press Enter or **New feed**, copy the one-time URL and key, watch the dot turn green, **GO**.
Mapping the feed to a bus key (MAP, Library, "Reporter: <name>") shows a live monitor picture before the cut. The module keeps its
state in `window.RF`: `web/remote.js` already publishes its own state as `window.R`, and the inline handlers read the global (the
Reporter page's name field had been writing into the Remote page's state until 2026-11-02).

## Files

- `playout/supervisor/reporterfeed.{h,cpp}` — service, state machine, ingest processes, routes (new)
- `cmake/lane-reporterfeed.cmake` — Supervisor sources + `test_reporterfeed` (new)
- `playout/supervisor/supervisorcore_integrations.cpp` — 6 guarded lines (`#ifdef PLAYPLAY_HAVE_REPORTERFEED`) in `GET /api/v1/inputs`
- `web/reporter.js` (new); `web/index.html` (one tab button), `web/app.js` (import, view, onEnter, onWs, pp handlers)
- `Play_Play_Native/qml/ui/ReporterFeedPanel.qml` (new); `resources.qrc` (one file), `Play_Play_Native/qml/views/OnAirView.qml` (one panel in the right column)
- `tests/test_reporterfeed.cpp` (new), `docs/reporter-feed.md`

## What is verified

2026-10-26: isolated `build-seg` with `PLAYPLAY_DEV_TREE=ON` and libobs, Now Playing, and reporter feed enabled: PlayPlay, Supervisor, and ChannelEngine built successfully; `test_reporterfeed`, `test_nowplaying`, `test_libobsroutes`, and `test_libobssource` passed. The web Now Playing render test also passed. These are component checks; an actual phone publish, countdown, and program take have not yet been verified.


Bench verification, 2026-10-26: an isolated Supervisor received a synthetic H.264/AAC RTMP publisher, reported `feedUp`, and listed the reporter under inputs. With the channel engine deliberately offline, countdown take returned to idle with the transport error. The web Now Playing, Reporter, and OBS Scenes pages loaded against this server. The test did not confirm an on-air picture or exercise an installed phone.


Additional bench verification, 2026-10-26: a synthetic RTMP stream with two-second keyframes reached the isolated channel engine, which reported ON_AIR and emitted an as-run START. A long keyframe interval caused a decoder failure and the reporter session returned to idle with that error; no publisher likewise produced a refusal. The OBS scene source was shown and switched from A to B on the isolated engine. The Facebook output sent five seconds of media to a local RTMP listener, which decoded it successfully. No Facebook service or physical phone was used.

Bench verification, 2026-11-02 (the running stack, binaries from `build\`; Now Playing page in the in-app browser): a session created
from the card (name, Enter); an ffmpeg RTMP publisher (test pattern, tone, H.264/AAC) turned the dot green and `feedUp` true; the
feed was in the MAP picker and mapped to key 8, where the monitor was `open` (picture checked on `/preview/ch2:in8.jpg`); GO
counted 5, 4, ... and the session went `live` with the monitor still open; the channel's program picture was the publisher's test
pattern (`/preview/ch2.jpg`) and key 8 showed ON AIR; END (real click on Confirm) returned the channel to its schedule and removed
the session. Before the second copy existed the same sequence failed at GO with -10048. Killing the Supervisor took its ingest
listener with it. `test_reporterfeed`: 17 passed. Not verified: a real phone (the getstarnews app / LiveCast) publishing, mode `mediamtx`.
