# Pillar panels: information beside narrow pictures

A 9:16 phone video, a 4:3 archive clip or a square social post leaves black bars beside the picture on a 16:9 channel. When the
item on air is narrower than `maxAspect`, the Supervisor puts `web/cg/pillar_panels.html` on **layer 11** (role `pillar`, under
every other graphic). The page draws only in the two bars: a slow animated starfield with glass cards for

* **weather** (now, high / low, three days; Open-Meteo, fetched every `weather.refreshMin` minutes),
* **this day in history** (Wikipedia "on this day", or the station's own list; rotates every `historySec`),
* **coming up** (the next programmes from the executor's runlists, break types skipped, repeated segments listed once),
* **sponsor banners** (two slots rotating through `sponsors`, every `sponsorSec`).

The middle of the page (1080 × aspect of 1920 pixels) is transparent, so the picture is untouched; bugs, straps and tickers stay on
top. Narrow bars (4:3 gives 240 px a side) switch to a compact layout with larger type and fewer cards.

Code: `playout/supervisor/pillarpanels.{h,cpp}` (settings, decision, data, fetching), `OverlayManager::syncPillar` (the layer's
life), `web/cg/pillar_panels.html` (the page), Overlays page → **Pillar panels** tab (`web/overlays.js`).

## When the panels appear

On every item change the OverlayManager asks `PillarPanels::specFor(channel, item)`:

1. off when `enabled` is false, the channel is not in `channels` (empty = all), or the type is in `excludeTypes` / not in `types`;
2. the picture's aspect: media `extra.displayAspect` ("9:16", "4:3" or a number) wins, else `width / height` from the library
   record (× `extra.sar`; `extra.rotation` 90 / 270 swaps them);
3. live inputs have no known size: only with `extra.displayAspect` on their library entry (or `live: true` and an aspect);
4. panels when aspect < `maxAspect` (default 1.5; capped below 16:9). `extra.pillarPanels` true / false on a media record forces
   them on / off for that item.

The same layer is updated in place while consecutive items are narrow (new up next, new aspect) and hidden when a full-frame item
starts or the channel goes off air. An engine restart drops it; the next telemetry puts it back. A failed deploy (no browser: the
page has no native twin) raises one alert and retries after 60 s.

Rules can hide the panels by naming the role: `then.suppress: ["pillar"]`. `"*"` does not include `pillar` (nor `legal_id` /
`emergency`): the panels fill black bars, they are not on-screen graphics in the usual sense.

## Data

The page fetches nothing. `PillarPanels::data(ch)` builds `{station, accent, tz, panels, weather, history, upNext, sponsors,
rotate, stars}` and the OverlayManager hands it over as the `data` param (JSON); later changes go out as an `update` (the page's
`window.update`), so rotation positions survive. Weather and history are cached in the Supervisor; a failed fetch keeps the last
good copy and reports `error` in the status. Nothing is fetched while the feature is off.

History entries are kept to one sentence (25-170 characters), spread across the years, and any entry with a word from
`history.avoid` is skipped: by default deaths, attacks and disasters (`kill*`, `death*`, `bomb*`, `crash*`, ...; a trailing `*`
is a prefix, other entries are whole words, so "Dieppe" is not caught by "die").

## Settings `pillar.config`

```json
{ "enabled": false, "channels": [], "maxAspect": 1.5, "types": [], "excludeTypes": [], "live": false,
  "template": "pillar_panels", "station": "", "accent": "#ffc94a",
  "panels": { "weather": true, "sponsors": true, "history": true, "upNext": true },
  "weather": { "place": "Martinsville", "lat": 36.6915, "lon": -79.8725, "refreshMin": 15 },
  "sponsors": [ { "name": "Uptown Hardware", "tagline": "Since 1952", "image": "/brand/uptown.png", "color": "#ffc94a" } ],
  "sponsorSec": 9,
  "history": { "source": "wikipedia", "items": [ { "year": 1958, "text": "..." } ], "count": 8, "avoid": ["kill*", "..."] },
  "historySec": 11,
  "upNext": { "count": 3, "skipTypes": ["ad", "promo", "psa", "bumper", "station_id", "filler", "gap", "cg", "image"] },
  "stars": { "speed": 1, "density": 1, "fps": 24 } }
```

Sponsor `image`: a path under the web root (`/brand/x.png`) or any URL; without an image the banner shows the name.
`station` blank = the channel's call letters, else its name. `stars.fps` caps the starfield's repaint rate (the html renderer
streams what the page paints; 24 is plenty for a slow drift).

## REST

| method path | body -> response |
|---|---|
| GET `/api/v1/pillar` | `{config, weather, history, channels:{ch:{on, aspect, why, title, atMs}}, previews}` |
| GET / PUT `/api/v1/pillar/config` | `{config}` (PUT replaces the top-level keys it names; weather / history changes refetch) |
| GET `/api/v1/pillar/{ch}/data` | `{data, item}`: what the page gets and the item it was decided for |
| POST `/api/v1/pillar/{ch}/preview` | `{on, aspect?:"9:16", sec?:60}`: panels on that channel regardless of the picture, for `sec` (5-900 s) |
| POST `/api/v1/pillar/refresh` | fetch weather and history now |

The page alone: `/cg/pillar_panels.html?demo=1&aspect=0.5625` shows sample data in any browser.

## Verified (2026-10-23)

Test Supervisor (copied DB, own ports, null outputs): a 1080x1920 library clip played with "play item" put the panels on layer 11
(`picture is 0.56:1`) with live Open-Meteo weather, Wikipedia history, real up-next titles and two sponsors, and took them down when
the next 16:9 item started; the preview route forced them over a 16:9 spot. Unit tests: `tests/test_cgrules.cpp`.
