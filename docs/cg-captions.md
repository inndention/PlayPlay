# Caption overlays: karaoke captions and the fact box

Two template kinds rendered by the channel engine's compositor (`TemplateSource`, QPainter) with
HTML twins for the browser-source renderer (`web/cg/karaoke.html`, `web/cg/factbox.html`), plus
Supervisor routes and panels in both UIs (CG view → "Captions & Fact box").

## Karaoke captions (`kind: "karaoke"`, layer 31)

Shows the caption line that is being spoken, word by word: spoken words in `pastColor`, the current
word in `highlightColor` with a left-to-right fill proportional to progress inside the word, upcoming
words in `color`, a rounded translucent box behind, title-safe placement (5 % margins), a short fade
on every line change. The layer's canvas is the full raster, so `position` is honoured whatever the
layer's placement says, but only the caption box is rendered and handed to the compositor
(`LayerSource::contentRect()`; about a quarter of a raster at most, usually far less), and between
two lines the source reports `isBlank()` so the compositor skips the layer instead of blending a
transparent picture.

Re-rendering happens only when the visible state changes: the line, the fade step, and, in
`mode: "word"`, the current word and (with `fill`) one of 12 fill steps inside it. `mode: "line"` or
`fill: false` re-render far less. While a word is being swept the picture does change about every
frame (a 0.3 s word at 30 fps), which is the cost of a smooth fill; measured at 1080p30 on the CPU
path a caption frame costs about 4 ms (caption box only) where the full-raster picture cost about
16 ms. Lines are broken by the `maxChars` budget **and** by the measured pixel width of the
title-safe box, so a large `fontSize` wraps instead of running off the screen (a single word wider
than the box is clipped to it).

Colours are Qt `#AARRGGBB` (alpha first, what QML emits and what `QColor` parses) everywhere in the
engine spec; the HTML twins convert 8-digit values from that order too, so one spec string looks the
same in both renderers. `#rrggbb` is unambiguous; do not write CSS-order `#rrggbbaa`.

### Spec

```json
{ "type": "template", "kind": "karaoke",
  "segments": [{ "start": 30.0, "end": 34.0, "text": "Welcome back", "words": [{ "word": "Welcome", "start": 30.0, "end": 30.4 }] }],
  "words": [{ "word": "..", "start": 0, "end": 0 }],
  "mode": "word",                 "position": "bottom",         "align": "center",
  "maxLines": 2,                  "maxChars": 48,               "fontSize": 46,
  "color": "#ebebf5",             "highlightColor": "#ffd60a",  "pastColor": "#ffffff",
  "nextColor": "#96ebebf5",       "bgColor": "#be05050a",       "fill": true,
  "inPointSec": 0, "startAtProgramSec": 0, "syncOffsetSec": 0, "timeBase": "program",
  "leadSec": 0.25, "tailSec": 1.2, "lineGapSec": 1.5, "fadeSec": 0.2, "breakOnSegments": true,
  "textShadow": true, "padding": 22, "radius": 10 }
```

* `segments` (the transcript store format) or a flat `words` array. Segments without `words` are
  spread evenly over their duration by character count.
* Lines break at segment boundaries (`breakOnSegments`), at silences longer than `lineGapSec`, at the
  `maxChars` budget and at the title-safe box width. `maxLines` lines are shown: the live one plus a
  dimmed preview of the next.
* A line goes live `leadSec` before its first word, but never before the previous line has finished:
  in contiguous speech the pre-roll does not cut the last word being spoken. It stays up `tailSec`
  after its last word; in a longer pause the layer is blank.
* `mode: "word"` colours per word; `mode: "line"` highlights the whole live line (no per-word state).
* `position`: `bottom` | `lower_third` (above a ticker) | `top` | `center`; `y` (1080p px) overrides.
* Sizes (`fontSize`, `padding`, `radius`, `y`) are 1080p values scaled by `raster width / 1920`.

### Sync: program time, in point, offsets

Transcript word times are **file-relative** seconds. The engine reports the item position **after the
item's in point**. `LayerSource::wantsProgramTime()` (new, default false) lets a source ask the
compositor for the program item position instead of "seconds since the layer was shown";
`TemplateSource` returns true for karaoke unless `timeBase: "layer"`.

```
program time base (default):  t = programPos + inPointSec + syncOffsetSec
layer time base:              t = layerPos + startAtProgramSec + inPointSec + syncOffsetSec
```

* `inPointSec` – the on-air item's in point. The Supervisor takes the engine's own value when the
  engine reports one (`inPoint` in the telemetry state and in the as-run START event: it knows a
  join-in-progress offset and follows seeks), otherwise the runlist item's `inPoint`. The item is
  found by the telemetry `itemId` (the runlist id of the engine item) in the executor's runlist
  lookup; the whole-hour `Runlist` object itself has no `assetId` / `inPoint` / `cgOnStart`, those
  live in its `items[]`. **Without the engine's value a late join (engine restart, load-hour-now
  mid-item) leaves the captions off by the join offset**, because the runlist's `inPoint` does not
  include it.
* `startAtProgramSec` – for a layer attached to an item with a `startOffset` and `timeBase: "layer"`
  (also accepts `startOffset`).
* `syncOffsetSec` – manual trim (positive = captions earlier relative to the audio).

### Supervisor routes

| Method | Path | Body / result |
| --- | --- | --- |
| `POST` | `/api/v1/channels/{id}/cg/karaoke` | `{mediaId?, layer?: 31, role?: "lower_third", mode?: "word|line", style?: {...}, follow?: bool, action?: "show|hide", position?, color?, highlightColor?, pastColor?, bgColor?, fontSize?, maxLines?, maxChars?, syncOffsetSec?, transition?}` → `{channelId, mediaId, layer, role, mode, words, segments, inPointSec, follow, shown, hasTranscript}`; 404 when the media has no transcript text (no file, or a transcript with no speech, e.g. music; 200 with `warning` when `follow` is on); 409 when nothing is on air and no `mediaId`. |
| `GET` | `/api/v1/channels/{id}/cg/karaoke` | `{channelId, follow, layer, role, mode, style, shown, shownAssetId, onAirAssetId, onAirTitle, state, hasTranscript, itemAsks}` (`hasTranscript` = the transcript has text, same rule as the 404 above) |

The Supervisor loads the transcript (`TranscriptionManager::transcript(mediaId)`), compacts it (word
start/end, no probabilities; the segment text is kept only for segments without word times), takes the
in point as described above, and sends `{action:"show", layer, role, source:{type:"template",
kind:"karaoke", segments, inPointSec, mediaId, ...style}, in/out: fade 0.3}` through the normal CG
path (`onUiCommand` → `MSG_CG_COMMAND` → `PlayoutEngine::executeCg`).

The layer list in the engine telemetry (`layers[]`, which is re-sent every second to every client and
returned by `GET /channels/{id}`) carries `segmentsCount` / `wordsCount` / `linesCount` / `itemsCount`
instead of the transcript or row arrays (`Compositor::layersJson`); the full spec stays in the layer.

Style and layer/role/mode are remembered per channel together with `follow` in the setting key
**`cg.karaokeFollow.<channel>`** (`{follow, layer, role, mode, style}`), so a show without style
re-uses the last one.

### Follow mode and schedule items

Two triggers evaluate the on-air item (key = telemetry `itemId` + `assetId`, so a second segment of
the same media with another in point is a new item): a 1 s poller over `m_telemetry` (created in
`registerIntegrationRoutes`, no changes to the telemetry handler), and the executor's as-run `START`
event, which arrives up to a second before telemetry shows the new item and fires again after an
operator seek (same item, new in point). Telemetry that still names the previous item for ~1 s after a
START is ignored. The engine cuts program-time layers on the frame of every take
(`Compositor::programTake` / `programClear`), so the old item's words never replay over the new
item; the Supervisor re-issues them for the new one at once. On a change:

* **follow on** → show captions for the new asset; when it has no transcript text (no file, or music /
  silence with `"segments": []`) the previous item's layer is taken off the air.
* **pause** keeps the captions: PAUSED counts as on air, and the compositor freezes program-time layers
  at the position where the hold began, so they stay with the held picture and resume with it. Only
  leaving ON_AIR / PAUSED (stop, hold, goto, engine gone) hides them, and coming back on air
  re-issues them even for the same item.
* **item asks for captions** → same, for that item only. The on-air runlist item asks when
  * `cgOnStart` contains the string `"karaoke"` (or `"karaoke:{json style}"`, or a JSON string /
    object whose `kind` or `source.kind` is `karaoke`), or
  * `cgOverlays` contains `{"templateId": "karaoke", "layer": 31, "style": {...}}` or
    `{"command": {"source": {"kind": "karaoke"}}}`.

  The engine ignores the bare `"karaoke"` hook (`identityCg` knows no such identity graphic) — the
  engine cannot read the Supervisor's transcript store, so the Supervisor expands it. Put this in a
  schedule item:

  ```json
  "cgOnStart": ["karaoke"]
  "cgOnStart": ["karaoke:{\"position\":\"top\",\"mode\":\"line\"}"]
  "cgOverlays": [{ "id": "cap", "templateId": "karaoke", "layer": 31, "startOffset": 0, "style": { "position": "lower_third" } }]
  ```

  A full command object also works wherever the engine accepts one, e.g. from a device command
  `cg:{...}` or `POST /channels/{id}/cg`, as long as the transcript words are inline:
  `{"action":"show","layer":31,"role":"lower_third","source":{"type":"template","kind":"karaoke","segments":[...],"inPointSec":0}}`.
* **manual one-shot** (no follow, item does not ask) → the layer is hidden when the asset changes
  (its words would be wrong for the next item; the engine itself cuts it at the take) or when the
  program leaves ON_AIR / PAUSED.

WS events: `cgKaraoke` `{channelId, shown, follow, mediaId?, words?, reason?}` on every show/hide.

## Fact box (`kind: "factbox"`, layer 32)

A panel with an accent header (title + optional icon glyph), bullet rows **or** label/value rows,
an optional picture under the header, a footer line, a soft drop shadow and an optional progressive
row reveal driven by layer time (`reveal: "lines"`, one more row every `revealIntervalSec` after
`revealDelaySec`). `nativeSize()` is the panel size including the shadow margin, so the compositor
places it with the layer's anchor / x / y; the panel keeps its full height while rows reveal.

```json
{ "type": "template", "kind": "factbox", "title": "By the numbers", "icon": "★",
  "lines": ["Population 13,400", "Founded 1793"],
  "items": [{ "label": "Population", "value": "13,400" }],
  "width": 520, "fontSize": 28, "accentColor": "#00d4ff", "bgColor": "#e80a0c14", "color": "#ffffff",
  "titleColor": "#05050a", "labelColor": "#aab4c8", "image": "D:/gfx/map.png", "footer": "Source: county records",
  "reveal": "lines", "revealIntervalSec": 1.2, "revealDelaySec": 0.4, "radius": 10, "shadow": true }
```

`lines` may also be a newline-separated string (blank lines dropped, each line trimmed); the reveal
count and the drawn rows are derived by the same helper, so every path (REST route, `POST /channels/{id}/cg`,
a device command `cg:{...}`, a stored spec) shows the same rows. `items` wins over `lines` when both
are present. `image` is decoded and scaled once, when the layer is opened (`update` re-decodes only
when `image` or `width` is part of it), never per render, so a reveal step does not touch the disk
on the clock thread.

### Supervisor routes

| Method | Path | Body / result |
| --- | --- | --- |
| `POST` | `/api/v1/channels/{id}/cg/factbox` | the spec above + `{layer?: 32, role?: "factbox", action?: "show|update|hide", position?: "right|left|top-right|top-left|bottom-left|bottom-right|center", placement?, transition?, in?, out?, holdSec?}`. `position` becomes an anchor with 5 % margins on the channel raster unless `placement` is given. `show` defaults to `slide_up` 0.4 s in / `fade` 0.4 s out. → `{channelId, layer, shown, command}` / `{updated, source}` |
| `GET` | `/api/v1/cg/factbox/presets` | `{presets: [{id, name, spec}]}` (setting key **`cg.factboxPresets`**) |
| `PUT` | `/api/v1/cg/factbox/presets` | `{presets: [...]}` replaces the list; `{name, spec, id?}` upserts one; `{deleteId}` removes one → `{presets, updatedMs}`; WS `factboxPresets` |

## HTML renderer twins (`web/cg`)

Both pages follow the pack conventions (1920×1080 transparent body, query parameters, `window.update(data)`,
`window.play()`, `window.stop()`) and are listed in `templates.json` under the category `captions`, so they
appear in the HTML template pickers and can be deployed to `/cg/output.html?channel=...`.

* `karaoke.html?channel=main[&token=..]` polls `/api/v1/channels/<ch>` (program position, asset, item id,
  state), fetches `/api/v1/media/<asset>/transcript` and the item's in point (the engine's `inPoint` when
  reported, else the runlist item with that item id), and interpolates between polls with
  `performance.now()`. A new item clears the old words at once; PAUSED freezes the clock but keeps the
  caption; any other state hides it. `media=`, `words=`, `segments=`, `src=` give the words directly;
  `demo=1` runs a sample. Styling parameters mirror the engine spec
  (`position, align, mode, fontSize, maxChars, maxLines, color, highlight, past, bg, inPoint, sync`;
  8-digit colours are `#AARRGGBB` like the engine's). Lines break by `maxChars` and by the measured
  width of the 1728 px title-safe box.
  `window.update({segments|words, programPosSec, inPointSec, syncOffsetSec, mode, ...})`,
  `window.seek(sec)`, `window.play()` / `window.stop()` (stop hides and stays hidden until play).
* `factbox.html?title=..&lines=a|b|c&items=label=value|..&position=right&accent=00d4ff&reveal=lines&interval=1.2`
  with `image`, `footer`, `width`, `fontSize`, `bg`, `color`. `window.update(data)` accepts arrays for
  `lines`/`items`; `play()` restarts the reveal, `stop()` hides.

## Operator UI

* **PlayPlay (QML)** `views/CGView.qml` → panel "Captions & Fact box": media id (blank = asset on air),
  Follow program toggle, position / mode / font / sync trim / colours, SHOW / HIDE, transcript and
  on-air badges (polled every 5 s and on the `cgKaraoke` event); fact box title, lines (one per line),
  position, accent, width, reveal toggle + interval, footer, SHOW / UPDATE / HIDE, presets (load /
  delete / save as). Mode and Position are adopted from the server only when the server's value
  changes (first load, channel switch, another client, a SHOW), so a pick that has not been SHOWn
  yet is not reset by the 5 s refresh or a `cgKaraoke` event. The box colour field is `#aarrggbb`.
* **Web dashboard** `web/integrations.js` → `viewCgCaptions()` appended to the CG view; handlers
  `pp.karaokeShow()`, `pp.karaokeHide()`, `pp.karaokeFollow(on)`, `pp.factboxShow(action)`,
  `pp.factboxPresetSave()`, `pp.factboxPresetLoad()`, `pp.factboxPresetDelete()`. Lines written as
  `label: value` on every row are sent as `items`.

## Tests

`tests/test_cg_captions.cpp` (Qt Test, offscreen) covers the line layout and visibility window
(including the line switch in contiguous speech and the pixel-width breaking), the time-base
arithmetic, the rendered karaoke raster (opaque pixels in the title-safe band, highlight colour
present, transparent past the last word, top placement), that `video()` delivers only the caption box
and reports blank between lines, the state key per mode (line mode / no fill re-render less), the
`#AARRGGBB` box colour, the fact box panel (size, accent header, items variant, string `lines`, the
picture decoded once), the progressive reveal (counted by text pixels), and the compositor: a karaoke
layer follows the program position, is cut by a take, is frozen by a hold (pause) and keeps its
transcript out of the telemetry. PNGs land in `$PLAYPLAY_TEST_OUT` (default `<temp>/playplay_cg`).

## Engine hooks the Supervisor can use

Both are optional; the Supervisor falls back to the runlist item without them.

* `PlayoutEngine::stateJson()` / `State`: `inPoint` = `m_current.inPointSec` (the in point the decoder
  really uses: runlist in point + join-in-progress offset, + a seek target).
* The `START` as-run event of `takePrepared()`: the same `inPoint` in its extra fields.
