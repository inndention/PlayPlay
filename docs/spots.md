# Generated spots (Spots page)

Port of PlayPlay OBS's newsroom (`newsroom.py`) and news scatter (`newsspots.py`). The Supervisor turns
feeds, forecasts and free text into short branded spots, puts them in the library as perishable
`news` / `weather` media, and pins the freshest one into the weekly grid through locked **GEN markers**.

```
recipe + material ──► script (templates, optional LLM) ──► narration WAV (Kokoro → SAPI fallback)
        └─► furniture PNG (Pillow) + bed (photo or gradient) ──► ffmpeg (zoompan + overlay + ducked music)
                ──► <uploads>/spots/<recipe>_<yyyymmdd_hhmm>_<id>.mp4 ──► ingest ──► library record
                        ──► job.lastMediaId ──► GEN markers refreshed in the grid
```

Everything runs in the Supervisor process without blocking playout: fetches use Qt's network stack,
the three heavy steps (card, voice, ffmpeg) are child processes, and only one spot renders at a time.

## Pieces

| Piece | Where | Notes |
|---|---|---|
| Recipes | built into `SpotGenerator`, overridable per field in settings `spots.recipes` | weather, weather_am, weather_pm, weather_week, almanac, headlines, story, community, events, obituaries, sports, roads, history, economics, tech_review, plus the scene-rendered **local spots** cinema, traffic, sports_local, realty (below), **markets_close** and **weather_full** (their own sections); custom recipes allowed |
| Script writer | deterministic sentence templates; LLM optional (`spots.llm`) | headlines/community/... read titles + first sentence verbatim; weather from Open-Meteo; history from Wikipedia "on this day"; `story` reads the item verbatim; free text is read as-is |
| Card | `scripts/spot_card.py` (Pillow) | furniture plane only: kicker chip, headline (≤3 lines), 2–4 bullet lines, station strap (call letters / brand / series / date + clock); a readability scrim when a photo sits underneath. Without a photo it also renders a gradient bed |
| Photo plane | ffmpeg `zoompan` on the story image (an RSS enclosure / media:content that is an image: image extension, `type="image/*"`, `medium="image"` or a thumbnail; else the first `<img>`; audio / video enclosures are never used), else `<backgrounds>/<recipe>.jpg|png` or `default.*`, else the gradient bed | two-plane rule from the newsspots KB: the browser/Pillow never moves pixels, ffmpeg does |
| Voice | `scripts/spot_tts.py` | engine `auto`: `kokoro` pip package → `kokoro-onnx` + models under `--kokoro-dir` → PlayPlay OBS style `ttsvenv/Scripts/python.exe kokoro_synth.py` → **Kokoro through sherpa-onnx-node** (`scripts/kokoro_sherpa.js` with Node; the int8 kokoro-en-v0_19 model in `D:\StarMedia\models\kokoro-int8-en-v0_19` and the addon in `D:\StarMedia\motion-spots\node_modules\sherpa-onnx-node`, i.e. StarMedia's install; override with `STARMEDIA_KOKORO_DIR` / `SHERPA_ONNX_NODE_DIR`; voices af af_bella af_nicole af_sarah af_sky am_adam am_michael bf_emma bf_isabella bm_george bm_lewis, newer names map to the nearest: af_heart → af_bella) → **Windows SAPI** (always available: Kokoro `af_*`/`bf_*` ids map to Zira, `am_*`/`bm_*` to David, or give an installed SAPI voice name). Every clip is levelled to one speech loudness (voiced RMS −19 dB, a 2 ms peak limiter at −1.5 dBFS), so Kokoro and SAPI spots air at the same level (≈ −20 LUFS); Kokoro takes ~2 s per second of speech on this host |
| Music bed | `<data>/spots/music/*.mp3|wav|m4a|flac|ogg`, else a **composed MIDI-style bed** (`scripts/spot_music.py`, see "Generated music") | file names carrying `energy N` (1–10) are matched to the recipe's energy; mixed at −16 dB, side-chain ducked under the narration, faded out |
| Output | H.264 yuv420p 1920x1080 30 fps, AAC 48 kHz stereo, faststart; duration = narration + 0.8 s, min 10 s (scatter stories min `minSec`, default 30 s) |
| Library record | type `news` (`weather` for weather recipes), `extra.category = "autogen rotation"`, `extra.subcategory` = recipe subcategory, `series` = recipe series, `extra.perishable = true`, `availableEndMs = now + TTL`, `extra.tags = ["gen:<jobId>"]` (job spots) or `["newsScatter"]` (scatter stories), `allowedChannels` = the job's channel plus every placement channel (unset = all channels when the job channel is "(all channels)"; one-off / scatter spots: their channel), `extra.autogen = {jobId, recipe, generatedAt, headline, script, lines, sources[], writer, ttsEngine, voice, music, durationSec, ttlMin}` |
| Network limits | feeds / forecast JSON 6 MB, story photo 12 MB (aborted early when the headers say it is not an image), LLM reply 4 MB | a transfer timeout only catches stalled transfers, so the size is capped as it arrives |
| Expiry | sweep every 10 min | archives autogen media 1 h past `availableEndMs`; deletes files (and records) older than `spots.keepDays` (default 3) when they live under the spots output dir and no job still points at them |

## Jobs and GEN markers

A job = recipe + cadence + placements (settings `spots.jobs`). Cadence is `every: {kind: "interval", minutes}`
or `{kind: "clock", times: ["06:05", "17:05"]}` with optional `dows` (0 = Sunday). The 30 s job tick runs
due jobs one at a time; clock jobs get a 20-minute grace window so a box that was busy at 06:05 still makes
the 06:05 spot. The TTL of a generation is the recipe TTL floored at 1.25 × the cadence, so a window never
closes before its successor exists. When a generation succeeds the prior one's window closes immediately
(`expirePrior`); when it fails or legitimately skips ("nothing to report" for roads/events/...), the prior
spot's window is extended to the next cycle so the pinned hours never fall through to autofill.

Each placement `{channel, hours: "all" | [..], atMin, dows: []}` becomes ONE marker item per (channel, day,
hour) in the weekly template (and in any dated override of the current week):

```json
{ "title": "GEN 3: Weather", "type": "weather", "durationSec": 60, "locked": true, "lockAtSec": 300, "lockLabel": "GEN 3 WEATHER",
  "queryBlock": { "types": ["news", "weather"], "targetSec": 60, "prefer": "newest", "categories": ["autogen rotation"], "subcategories": ["Weather"], "tags": ["gen:<jobId>"] },
  "autoSpot": { "jobId": "…", "recipe": "weather", "genNum": 3 } }
```

The marker is a SOFT locked anchor, not a hard start: the RunlistBuilder seats it at the item boundary nearest
`atMin` and cuts nothing (PlayPlay OBS `lock_at_sec`: "an anchor, not a hard timecode"). A hard start would
truncate the airing program at the marker minute and drop every later soft item of the hour. The block is
pinned to its own job by the `gen:<jobId>` tag the job's spots carry, so a newer spot of the same subcategory
from another job (or a scatter story) never airs in its place. The RunlistBuilder resolves the query block to
the job's newest airable spot at build time, so the grid is a standing weekly pattern that always airs the
latest generation: the marker never changes when a spot is generated (it carries identity only, no media id).

Rules for writing markers:

- **Only for a job with an airable spot.** Markers are placed by the job's first successful generation and
  by saving its placements once a spot exists; the two seed jobs (disabled, no spot) place nothing, so
  hours that autofill keep autofilling. A job whose spot window has closed leaves the grid as it is.
- **Enforce markers** (also 20 s after start, and after every saved slot, coalesced into one pass ~300 ms
  later) re-asserts the markers of ENABLED jobs only, restores any an editor removed, and removes markers
  whose job is gone. A switched-off job keeps what it has.
- **Never the hour on air.** A change that would add, move or remove a marker in the hour currently on air
  (or in today's past hours) is held back and applied by the 30 s tick once that hour is over, so a live
  edit cannot replace the program that is playing.
- Bulk writes (a 168-slot placement) emit one `schedule` event per channel (`{channelId, slotId: "", bulk: true}`),
  not one per slot.
- Two seed jobs are created once, disabled: weather every 60 min at :05, headlines every 120 min at :35.

Marker state per job (badge on the Jobs tab): **fresh** (latest spot inside its window), **due** (newest spot
older than the cadence: the generator failed or has not run), **stale** (nothing airable behind the pin),
**off** (job disabled: its hours keep airing whatever was last generated).

## Headline News (the `headlines` recipe, render `stories`)

The headlines recipe renders a story-by-story news package from the newest posts of getstarnews.com
(`playout/supervisor/headlinenews.*`, recipe fields `render: "stories"`, `stories: 5`, `pageImages`, `site`,
`siteName`, `storyLayout: "box" | "full"`; all overridable in settings `spots.recipes.headlines`).

1. **Posts.** The feed (`https://www.getstarnews.com/feed/`) gives titles, dates and RSS categories, newest first,
   within `maxAgeHours` (24). Items that are not headlines are dropped for every RSS recipe: a bare clock time as the
   title ("10:38 p.m.") or a short caption with nothing behind it ("Welcome Race Fans!"). Duplicates go (same link
   or same headline).
2. **Story pages.** Each candidate's page gives its picture (`og:image`; site-wide pictures such as the logo or the
   default backdrop count as none), the complete description and the post text (first `<article>`, boilerplate
   like "Official sources: ..." skipped), the place (URL `/local/<place>/story/`, spelled as the site's tag when the
   tag keeps the separators) and the publish time. A title the site cut short ("... law...", imported social posts)
   is rebuilt from the post's first sentence. The newest stories with their own picture win; imageless ones fill
   up when there are not enough.
3. **Script.** Intro ("Here are the top stories at this hour from Get Star News."), one scene per story (headline +
   the first sentence that does not repeat it, cut at a clause break past 34 words), outro ("For more on these
   stories, visit Get Star News dot com."). Spoken forms: months ("Oct." -> "October"), "Ave." / "St." / "Rd.",
   compass points and quadrants ("231 W. First St.", "Avenue SW"), short ranges ("23-24" -> "23 to 24"); phone
   numbers stay. The sentence splitter (used by every recipe) keeps "Oct. 23", "J. Smith", "U.S." and "3 p.m. Saturday"
   inside their sentence. The LLM writer is not used for this render.
4. **Scenes.** `spot_card.py --scenes` paints every scene in one run: furniture (HEADLINE NEWS + category chips, the
   counter, the place, headline, summary, the station strap) and the still bed. `storyLayout: "box"` (default) frames
   the post's picture whole on the right (never cropped: getstarnews pictures are editorial art with their own
   text) over a blurred copy of itself; `"full"` fills the frame with the picture behind a left scrim (for sites
   that post plain photographs). Intro and outro use a blurred, dimmed backdrop of the first / last picture.
5. **Voice.** `spot_tts.py --batch` voices every scene with one engine load (SAPI in one PowerShell session) and
   trims each clip's leading / trailing silence, so scene lengths follow the speech.
6. **Compose.** Each scene lasts its narration + 0.35 s lead + 0.75 s tail (intro >= 4 s, outro >= 5 s) in whole
   frames; scenes are joined with 0.6 s crossfades (fade into the first story and the outro, soft wipes between
   stories), each voice starting as its picture arrives; the photo plane moves per scene (push in, pull out, drift
   right, drift left); the music bed (when present) ducks under the voice. About 100 s for five stories.
7. **Record.** As every spot (type `news`, perishable, GEN marker tag) plus `extra.autogen.render = "stories"`,
   `stories[]` and `timeline[]` (where each scene sits). The Spots page preview lists the scenes in air order with each
   story's picture and the exact words the voice reads.

## Local spots (render `scenes`: cinema, traffic, sports, featured home)

Four recipes read a structured page instead of RSS and write their own scenes (`playout/supervisor/localspots.*`, pure and unit
tested in `tests/test_localspots.cpp`). The scene pipeline after the script is Headline News's: photos -> furniture cards
(`spot_card.py --scenes`) -> one voice per scene -> ffmpeg crossfades. Scene fields beyond the news ones: `detail` (the line a
viewer must not miss, drawn gold under the title: showtimes, score, price), `eyebrow` / `big` / `sub` (intro and outro lines),
`counter` (empty for a lone scene). Every recipe field below is overridable in settings `spots.recipes.<key>`.

| Recipe | Source | What airs | Job (as created) |
|---|---|---|---|
| `cinema` | `hollywoodcinema.net` schema.org `Movie` / `ScreeningEvent` markup | intro + one scene per movie (poster, rating, runtime, first synopsis sentence, **showtimes still ahead today**, else the next day with shows) + outro (address, ticket prices, site) | clock 00:05 / 04:05 / ... / 20:05, placed hours 0, 4, ... 20 at :27 |
| `traffic` | VDOT 511 GeoJSON layers `incidents`, `construction`, `weatherClosures` (`https://511.vdot.virginia.gov/services/511/map/layers/map/<layer>`) within `radiusMi` (45) of the weather lat/lon | intro + one scene per alert (road closed > lane closures > delays > shoulder, nearest first; the 511 message verbatim, whole sentences, <= 44 words) + outro; **no alerts = a one-scene "ALL CLEAR" spot; a feed that cannot be read fails the run** (never "roads clear" on a dead feed) | every 60 min, placed every hour at :20 |
| `sports_local` | MaxPreps team schedule pages (`__NEXT_DATA__`) for Martinsville, Magna Vista, Bassett, Patrick County, Carlisle x 9 sports (`schools` / `sports` overridable) | intro + a scene per **final in the last `recentDays` (4)** (win / loss, winner's score first, between two local schools once) + a scene per **game in the next `upcomingHours` (48)** (tonight / tomorrow, home / away, conference) + outro; nothing to report = skipped | clock 02:05 / 06:05 / ... / 22:05, placed hours 2, 6, ... 22 at :27 |
| `realty` | the brokerage site's Showcase IDX listing JSON (`havenmore.com`) | one **Active** home per run: facts + its own words from the listing (calls to action, disclaimers, steering language dropped), **4-8 of its photos**, each shown as it is discussed, then contact + "listing courtesy of" | every 60 min, placed every hour at :52 |

Notes per recipe:

- **cinema.** The site lists one week (Wednesday to Sunday) and keeps days that are over, so the page is read as a week anchored on
  the day most movies list first; a day already past is dropped, and when the whole listed week is behind us the page is taken as
  next week's. Shows starting within `leadMin` (10) of generation are not offered. Posters are TMDB `w780`. `maxMovies` 6,
  `synopsisWords` 24. The footer's address and "All Tickets $5 except 9PM shows $8" are parsed, not hardcoded.
- **traffic.** Speech forms: `I-81` -> "Interstate 81", `Rt. 672` / `Rte 773` -> "Route ...", `US-19` -> "U S 19". Add NC coverage by
  adding a layer URL; `layers`, `base`, `radiusMi`, `maxScenes` are recipe fields.
- **sports_local.** A MaxPreps page for another school, state or season (it redirects unknown slugs) yields nothing. Finals older
  than `recentDays` and unscored games that started more than 3 h ago are not reported. The 46 page fetches run in parallel and
  are merged in request order, so the script does not depend on which page answered first.
- **realty.** Rotation state is settings `spots.realty = {featured: {mls: ms}, seen: {mls: ms}, lastPick, lastCheckMs}`; `GET
  /spots/status` reports it as `realty`. Every run reads the listings (so status changes are seen within the hour): a home that
  goes Pending / Under Contract stops airing; a **never-featured home airs on the next run** (newest first), then the one featured
  longest ago, never the one just shown. A home is marked featured when its spot exists, so a failed render does not skip it. A
  brand-new listing raises an info alert ("1 new listing(s) on havenmore.com will air next"). Photos are the listing's full-size
  URLs; each is used once; the description's order picks them (kitchen sentence -> mid-gallery, yard -> the end, MLS order being
  front, living, kitchen, bedrooms, baths, extras, yard); no vision model is used. Recipe fields: `minPhotos` 4, `maxPhotos` 8,
  `statuses` ["Active"], `contactPhone` (one station line for every home instead of the listing agent's number).
  Brokerage attribution ("Listing courtesy of Keller Williams Martinsville", MLS number) is always on the closing card.

- **Own library category (`mediaCategory`, default `autogen rotation`; the four local recipes use `autogen local`).** The hour recipes'
  generic "Autogen ~3m" blocks play the *newest* spot of `autogen rotation`, so spots of that category air in those blocks as well as
  at their own GEN marker: a measured Friday hour played the sports spot 3x, the featured home 3x and traffic 3x. A recipe with its own
  cadence therefore names another category and airs only through its marker (once per placement). The marker queries the same
  category, and applying a job's placements moves its current spot into the recipe's category, so changing the field needs no re-render.
  Check any hour with `POST /schedule/{ch}/slot/{slot}/resolved` (e.g. slot `5-14`: Friday 14:00).

## Market Close (`markets_close`)

A short market wrap (`playout/supervisor/marketspots.*`, tests in `tests/test_marketspots.cpp`): the S&P 500 (a headline number with a
7-session chart), a Big Tech table (Amazon, Meta, Google, Microsoft: price, change chip, sparkline) and a Crypto table (Bitcoin, Ethereum:
24-hour change), then a closing card. About 55 s. Data is Yahoo's public daily-bar chart endpoint, one request per symbol
(`symbols` in the recipe overrides the list: `{symbol, label, spoken, kind: index|stock|crypto}`).

- **Made once after the close, aired every 3 hours until the next open.** Job (see `scripts/local_spot_jobs.py`): clock 16:20 Mon-Fri; placements
  at hours 17, 20, 23 on Mon-Fri and 2, 5, 8 on Tue-Sat, at :42. A Friday wrap therefore airs through Saturday morning and nothing stale
  airs on Sunday or Monday morning. The spot's window is the 3.75 days between Friday's and Monday's runs.
- **Holidays make no spot.** With `requireToday` (default) the index's last session must be today's date (US Eastern); a weekend or market holiday skips
  the run, and the previous wrap stays airable. A manual run during the session says "is trading" instead of "closed" (`MARKETS NOW`).
- **Wording** reports moves and never recommends: "slipped / rose / gained / dropped / tumbled / jumped" by size of move, prices as dollars and cents
  (five-figure prices without cents), and the closing card says the data is delayed and "for information only, not investment advice".
- Cards: green up, red down (`tone`), `rows` tables and `spark` charts are generic `spot_card.py` features (any recipe may use them).

## Weather - Forecast & Almanac (`weather_full`)

The animated weather spot (`playout/supervisor/weatherspots.*`, `scripts/weather_cards.py`; tests `tests/test_weatherspots.cpp`). Six segments
(recipe `segments` picks any subset, `cardDays` 3-8), about 95 s:

| Segment | Content | Source |
|---|---|---|
| Right now | big temperature, condition, feels like / humidity / wind, high / low, rain chance (evening: tonight's low and tomorrow's high) | Open-Meteo current + daily |
| 7-Day Forecast | **daily cards**: weekday, date, animated icon, condition, high / low, rain %, today outlined | Open-Meteo daily |
| Sunset | sunset time, golden hour, sunrise, daylight, tomorrow's sunset and the minutes' difference (after sunset: "The sun set at...") | Open-Meteo daily |
| Moon Phase | phase-accurate moon, % illuminated, next full / new moon, a cycle bar | computed (mean synodic month from the 2000-01-06 new moon: phase good to hours, dates can be a day off) |
| Burn Safety | a rating gauge (Low / Moderate / High / Very High / Extreme) with its reasons, advice and, Feb 15 - Apr 30, Virginia's 4 PM burning law; after 6 PM it is tomorrow's | Open-Meteo wind / humidity / rain + NWS alerts |
| Lake Levels | Smith Mountain and Leesville (vs full pond, with a 24 h trend once a day of history exists) and Philpott | AEP "Most Recent Conditions" table, USGS 02071900 |

- **Burn rating** is a transparent rule set, not an official fire-danger index: gusts (>=10 / 15 / 25 mph = +1 / 2 / 3), afternoon minimum humidity
  (<=45 / 35 / 25 % = +1 / 2 / 3), under 0.10 in. of rain in 3 days (+2, under 0.25 in. +1), rain expected today (-2), high >= 85 F (+1);
  score <= 2 Low, 3-4 Moderate, 5-6 High, >= 7 Very High. An NWS Fire Weather Watch makes it at least High, a Red Flag Warning Extreme ("Do not burn
  outdoors"). Every read ends "Check for local burn bans before you light anything."
- **Lake history:** AEP publishes only the present level, so each run appends one reading to settings `spots.lakes` (8 days kept) and the trend is read
  from the sample nearest to 24 h ago. Philpott's change comes from the USGS series. Full-pond values come from AEP's own table (Philpott has none: its
  pool changes with the season).
- **Animations** (the first use of animated beds): `spot_card.py` hands a scene with an `anim` object to `weather_cards.py`, which writes a looping
  60-frame / 15 fps JPEG sequence (the *bed*: sky, panels, icons, particles) and a transparent PNG (the *furniture*: chrome and all text). ffmpeg loops the
  sequence (`-stream_loop -1`) for the length of the narration, so every motion is periodic: sun rays turn through one of twelve symmetric positions, clouds
  drift one tile period per loop in parallax lanes, rain falls whole laps, stars twinkle on integer cycles, the lake waves and the gauge needle sway.
  Animated icons: sun / moon, mostly clear, partly cloudy, cloudy, fog, drizzle, rain, heavy rain, storm (two-flash lightning), snow. The sky follows the
  current condition and day / night. `python -I scripts/weather_cards.py --demo <dir>` renders one frame pair of every scene type for a look.
- It keeps the `autogen rotation` category (like the other weather recipes), so the hour recipes' Autogen blocks rotate it as they did the old Weather spot.
  `scripts/local_spot_jobs.py` points the existing hourly "Weather" job at it (`--keep-weather` to leave that job alone). A render takes about five
  minutes on this host (Kokoro voices ~2 s per second of speech), well inside the hour.

Preview any of them without rendering: `POST /spots/preview-script {recipe}`.

## Area markets (Martinsville / Reidsville / Danville)

A **market** is the place a spot is made for. It is data (`playout/supervisor/areamarket.*`, unit tested in `tests/test_areamarket.cpp`): display name,
state, county, nearby towns, the weather point, news / sports / traffic source hints, intro and sign-off wording, and notes on how the voice should
sound. Built in: **Martinsville, VA** (Henry County), **Reidsville, NC** (Rockingham County), **Danville, VA** (Pittsylvania County).

**Which market a spot gets** (first that is set): the request (`market` on `POST /spots/generate` / `preview-script`; the Generate tab, a studio project) >
the job (`market` on the job) > the recipe (`spots.recipes.<key>.market`) > the station default (`spots.market.default`). `"none"` at any level means
"no market" and stops the search; `""` / `"default"` inherit. An unknown key is ignored. **With nothing set the recipes behave exactly as before** (the
generic Martinsville-flavoured defaults and the Weather place / coordinates below), so the live jobs are unchanged until someone picks a market.

**What a market changes** (the recipe is built built-in -> market patch -> your `spots.recipes` overrides, so an operator override still wins):

| Part | Effect |
|---|---|
| Weather | the market's lat / lon / place replace `spots.weather` for the spots it makes (forecast URL, "Here's your Danville weather", sign-off "Stay with us for Danville and Pittsylvania County weather updates."); the weather_full recipe drops the lake segment outside Martinsville (the lake data is Smith Mountain / Leesville / Philpott) and North Carolina drops Virginia's 4 PM burning law |
| News recipes (community, events, obituaries, story, roads, sports) | the market's feeds (Martinsville Bulletin / WSLS; Register & Bee; WXII / Fox 8 / WFMY) plus Get Star News, narrowed by a `localFilter` regex over title, text, category and link (the corridor-wide Star News feed carries `/local/<town>/` links), so a Danville spot only reads Danville / Pittsylvania County stories; "nothing to report" when none match. National recipes (economics, tech, history, market close) are never narrowed |
| Wording | each recipe's intro / outro / `introText` / `signOff` use the market's area name ("In Danville and Pittsylvania County community news.", "Here are the top Martinsville and Henry County stories at this hour from Get Star News."); `series` becomes "<Town> Weather", "<Town> Headline News"... |
| Local spots | traffic: centre, radius, county in the all-clear line, outro; sports_local: the market's MaxPreps schools (Reidsville: Reidsville, Rockingham County, Morehead, McMichael; Danville: George Washington, Dan River, Tunstall, Gretna, Chatham) and "Your <area> sports update"; cinema / realty: a market without its own source (a blank `url`) is skipped with a message, never filled from another town's page |
| Writer prompt (LLM) | a paragraph naming the market, its towns, the voice notes, the market's own sign-off and the places not to name |
| Voice | `voice.pronunciations` respell place names in the narration only ("Pittsylvania" -> "Pit sill vane ya", "Reidsville" -> "Reedsville"); on-screen text is untouched |
| Library record | `extra.market {key, name, label, state, county, area}`, `extra.autogen.market`, tag `market:<key>` |

**Not wired yet**: Reidsville **traffic** (NCDOT's DriveNC feed needs an API key, so the recipe fails with that message instead of airing an all-clear from
Virginia data), cinema and realty for Reidsville and Danville (no brokerage / theatre source set), lake levels outside Martinsville, a Reidsville NWS alert
point other than the lat / lon query.

**Operators** add or amend a market in settings `spots.markets` (`PUT /spots/config {markets: {...}}`, replaces the object): `{danville: {news: {feeds: [...]}, recipes:
{"cinema": {url: "https://..."}}}}` merges field by field over the built-in; a key with a `name` is a new market (`state`, `county`, `towns[]`, `lat`, `lon` and
`news.keywords` give it a filter). `recipes` keys may list several recipes (`"weather,weather_am"`); string values may use `{place} {PLACE} {area} {AREA} {county}
{state} {stateName} {region} {towns} {station}`.

REST: `GET /spots/markets` -> `{default, effectiveDefault, markets: [summaries]}`; `GET /spots/markets/{key}` -> the full market + `localizedRecipes`; `PUT /spots/config
{market: {default: "danville" | "none" | ""}}` sets the station default; jobs take `market`; `preview-script` answers `market` and `marketContext`. UI: the Spots page
(web: Config tab station default + market table, Jobs editor, Generate tab, recipe editor; native: Config, job editor, Generate), the Studio project form.

## Generated music (MIDI-style beds)

When `<data>/spots/music` holds no files (or `spots.music.mode` is `generated`), every spot gets its own composed bed. A `music` step runs
between the voice and the compose step: `scripts/spot_music.py` writes a **General MIDI file** (format 1, 480 PPQ: conductor, pad, bass, arpeggio,
lead and a drum track on channel 10) and renders it with a built-in numpy synthesizer to a 48 kHz stereo WAV (about 1 s per 30 s of music, levelled
to roughly −13 dBFS RMS with a soft limiter), then the normal mix puts it at −16 dB, side-chain ducked under the narration and faded out.

- **Settings:** `spots.music = {mode}`: `auto` (default: files when there are any, else composed), `generated` (always composed), `files` (the old
  behaviour, never composed), `off` (no music). `GET/PUT /spots/config` carries `music`; `env.musicScript` says whether the script is installed.
- **Mood** (recipe `mood` overrides; else by recipe): weather/weather_pm/weather_week/weather_full `calm`, weather_am `bright`, almanac `night`,
  headlines/story `news`, community/realty `warm`, events `upbeat`, obituaries `solemn`, sports/sports_local `sports`, roads/traffic `urgent`,
  cinema/history `cinematic`, economics/markets_close `steady`, tech_review `quirky`; other recipes by energy (≤3 calm, ≤5 warm, ≤7 news, else upbeat).
  Tempo, key, chord progression and instrumentation come from the mood and a seed (recipe + headline + day), so a spot is repeatable the same day and
  differs the next. The last bar always resolves to the tonic, and the length is the spot's (narration + margin, or the scene timeline).
- **Output:** the library record's `autogen.music` reads `MIDI: <bpm> bpm, <key> (<mood>)` and `autogen.musicMid` points at the spot's `.mid` copy in
  `<spots out>/music/` (open it in any MIDI tool or re-render it with another sound set). A composing failure logs a warning and the spot airs without music.
- **CLI:** `python scripts/spot_music.py --out bed.wav --mid bed.mid --seconds 30 --mood sports --energy 7 --seed 42` (moods: calm warm bright news urgent
  sports upbeat solemn cinematic steady quirky night) prints a JSON summary.
- **Deployment:** `stage_runtime` copies `spot_music.py` to `build\scripts`, which the live Supervisor reads; the Supervisor build with the music step is needed too.

## News scatter

Settings `spots.scatter = {enabled, channel, perHour, feeds[], voice, ttlMin, maxNewPerRun, everyMin, minSec}`.
`generate` reads the feeds, skips stories already seen (a 500-URL ring in `spots.scatter.seen`; a story that
fails three times is given up on) and renders up to `maxNewPerRun` of them as verbatim `story` spots over
their photo, tagged `extra.newsScatter = true` and `tags: ["newsScatter"]`; items older than the `story`
recipe's `maxAgeHours` are not considered. `apply` fills every remaining hour of today's dated slots
(creating the dated slot from the template copy when needed) with up to `perHour` soft-anchored locked query
blocks (`subcategories: ["News"]`, `tags: ["newsScatter"]`, `prefer: newest`, `noRepeatMin: 30`,
`extra.newsScatter`) at banded random minutes (one pick per band between :03 and :57), skips the hour on air
and replaces the previous scatter. With `enabled`, both run every `everyMin` (default 45) minutes; a freshly rendered story
triggers a re-scatter a few seconds later.

## Engine switches: in-house tools or the old programs

Each tool step can run an in-house program with the same arguments instead of the script / ffmpeg.exe it replaces
(DEP-13). Settings `spots.engines`:

| key | old program (default) | in-house tool | notes |
|---|---|---|---|
| `card` | `python -I spot_card.py` | `PlayPlaySpotRender card` | furniture + still beds; an animated weather scene answers 64, so the script paints that whole spot |
| `tts` | `python -I spot_tts.py` | `PlayPlaySpeech tts` | SAPI natively. Only the `kokoro` pip package in the Supervisor's Python is detected (`env.kokoroPackage`): with it, `auto` stays on the script. A Node / sherpa-onnx Kokoro (this station's) is invisible to the tool, so `auto` or `native` there voices SAPI where the script would voice Kokoro: keep `tts` on `python` on such a host, or use `native` only with `spots.tts.engine = sapi`, until DEP-11 |
| `music` | `python -I spot_music.py` | `PlayPlaySpotRender music` | same MIDI to the byte; the WAV sits at −12 dBFS (the script's ≈ −13.2, sometimes clipped) |
| `compose` | `ffmpeg.exe` | `PlayPlayMediaTool` (ffmpeg's own command line) | the graph runner (DEP-05) is not in yet: the tool answers 64 and ffmpeg runs |

- Values: `python` / `ffmpeg` (or unset) = the old program, **the shipped default**; `native` = the tool whenever its exe is
  found; `auto` = the tool only once `<tool> version` lists the command (probed once per exe, without waiting), and for
  `tts` only once a look in the Supervisor's Python found no `kokoro` pip package (the script's first engine, invisible to
  the tool). Any other value is refused by `PUT config` (kept as it was). `dir` = the folder with the tools (default:
  next to the Supervisor).
- **Fallback:** a native run that exits 64 ("unsupported here"), fails to start, crashes or fails in any other way is run
  again with the old program, with the same arguments and the same timeout; a step that timed out is not run twice. The
  fallback is logged once per hour per step (a warning unless it was 64 / not startable). Every step writes per-job files
  atomically, so the retry is safe.
- **What ran:** each generation's result and its library record (`extra.autogen.engines`) say per step `native`,
  `python` / `ffmpeg` or `python (fallback: <why>)`.
- **Status:** `GET /spots/config` has `engines` (the settings, unset steps shown as their old program, `dirResolved`) and
  `env.engines.<step> = {mode, uses: native|python|ffmpeg, tool, exe, found, reason, commands, ready}`, plus
  `env.kokoroPackage` (null until checked). A deleted tool shows `uses: python`, `reason: "<tool> not found"`, and the spot
  is made by the script without a warning.
- **Spot studio:** the local card engine is ready when card, tts and compose each have a program (the tool, or Python with
  its script / ffmpeg); the reason names what is missing.
- **Transcription** has the same switch: `transcribe.config.engine` (`python` default, `native`, `auto`; `toolsDir`) runs
  `PlayPlaySpeech asr` with transcribe.py's arguments; any non-zero exit, a crash (a missing DLL next to the tool included)
  or a start failure re-runs the job with transcribe.py (until DEP-12 the tool has no `asr` and always answers 64); only a
  run killed by `DELETE /transcribe/jobs/{id}` ends `cancelled`. `GET /transcribe/config` reports `engineResolved`; each job and the
  media's `extra.transcript` carry `engine`. `POST /transcribe/probe` no longer waits on the Supervisor's event loop (it
  used to block it for up to 20 s): the Python check runs asynchronously and the request is answered when it finishes.

## REST (`/api/v1/spots/...`)

| Method | Path | Body → response |
|---|---|---|
| GET | `recipes` | `{recipes: [...]}` |
| GET / PUT | `config` | `{weather, llm, scatter, tts, ffmpeg, engines, keepDays, brand, musicDir, backgroundsDir, recipes}` (`engines`: see "Engine switches"; PUT merges; `llm.apiKey` is write-only: config reports `hasKey`, and `GET /settings/spots.llm` answers with the key stripped) |
| GET | `jobs` | `{jobs: [...with nextRunMs, due, ttlMin, cadenceMin], markers: {jobId: state}}` |
| POST | `jobs` | job body → the saved job |
| PUT / DELETE | `jobs/{id}` | partial update (placements are re-applied) / remove job + its markers |
| POST | `jobs/{id}/run` | async; `{wait: true}` (default) replies when done with `{ok, mediaId, path, durationSec, headline}` or an error; `{wait: false}` → 202 `{genId}` (the operator app uses this: its REST client aborts after 8 s and a render takes minutes; completion is the `recent[]` entry with that `genId` in the `spots` status / WS event) |
| POST | `jobs/{id}/placements` | re-apply this job's markers |
| POST | `generate` | `{recipe, channel, data, voice, feeds[], targetSec, ttlMin, wait}` one-off, same reply as run |
| POST | `preview-script` | `{recipe, channel, data, feeds[]}` → `{headline, lines[], narration, words, estimatedSec, writer, sources[]}` without rendering; `{wait: false}` → 202 `{previewId}` |
| GET | `preview-script/{id}` | `{pending: true}` while the preview is queued / writing, then the preview (or `{error}`) with `pending: false` |
| DELETE | `generation/{id}` | cancel a queued or running generation |
| GET | `status` | `{current, queue[], recent[], markers{}, scatter{}, env{}, jobs}` |
| GET | `markers` | `{markers: [every GEN / scatter item across template + this week's overrides; fixedOffsetSec / lockAtSec = the anchor in seconds into the hour], states}` |
| POST | `markers/enforce` | runs the pass now: `{placed, updated, removed, jobs}` |
| GET | `spot/{mediaId}` | the media record + `autogen` (script, sources, engine) + `currentForJob` |
| POST | `scatter/generate` | async; replies after the feeds are read: `{considered, fresh, queued}` (renders continue in the background) |
| POST | `scatter/apply` | `{channel, perHour}` → `{placed, cleared, pool, slots}` |
| GET | `scatter` | scatter status |
| POST | `sweep` | run the expiry sweep now |

WS: every state change broadcasts `{type: "spots", ...status}` (QML: `uiEvent("spots", payload)`).

## Operating notes

- `spots.weather` defaults to Martinsville VA (36.69, −79.87, °F). Set `units: "metric"` for °C / km/h.
- Feed freshness: every RSS recipe has `maxAgeHours` (headlines 24, sports 36, roads 3, events 168, most others 72;
  custom recipes 48; `0` = no limit). Older items are dropped before the recipe filter, so a feed that stopped
  updating yields "nothing to report" (skipIfNone) instead of yesterday's crash read as current. Undated items are kept.
- Channel scope: a job on "(all channels)" renders with the first channel's brand / timezone but its spots are
  unscoped, so markers on any channel can resolve them; a job on one channel scopes its spots to that channel
  plus every channel one of its placements targets.
- A spot whose MP4 is still waiting in the shared ingest queue after 5 minutes is reported as failed, but when
  its record lands later the job adopts it (lastMediaId, prior window closed, markers) instead of losing it.
- The LLM writer (`spots.llm = {enabled, apiKey, model, baseUrl}`, OpenRouter-compatible chat completions)
  is optional; the templates are used when it is off and on any failure. The template writer never invents
  text: it reads titles and first sentences verbatim and cleans them for speech (URLs, emoji, "read more",
  bracketed directions, symbols → words; capped to ≈2.6 words/s of the target length on a sentence boundary).
- Kokoro: install the `kokoro` package into the Supervisor's Python, or point `spots.tts.kokoroDir` at a
  PlayPlay OBS `tools` folder (`ttsvenv` + `kokoro_synth.py`) or at a folder with `kokoro-v1.0.onnx` +
  `voices-v1.0.bin` (needs `kokoro-onnx`). Nothing to install for SAPI.
- Backgrounds: drop `weather.jpg`, `headlines.jpg`, ... or `default.jpg` into `<data>/spots/backgrounds`.
- Brand colours come from `spots.brand {accent, ink}`; the strap shows the channel's `callLetters`
  (else `brandName`) and `brandName`.
- Failed renders keep their work folder under `<data>/spots/work/<genId>` for inspection; successful ones
  are cleaned up.

## Data sources and terms

The spot recipes fetch material from third-party services. Their terms apply to whoever runs this software, not to this
repository, and **you are responsible for having the rights to anything you scrape, read aloud or put on air**.

| Source | Used by | Things to check |
|---|---|---|
| Open-Meteo (`api.open-meteo.com`) | weather spots, pillar panels, burn safety | The free tier is for non-commercial use only and the data is CC BY 4.0, so attribution is required. A commercial station needs an Open-Meteo subscription (or another provider). |
| NWS (`api.weather.gov`), USGS water services, Virginia 511 (VDOT), Wikipedia "on this day" | weather alerts, lake levels, traffic, history | Public or openly licensed feeds with their own usage and attribution policies; Wikipedia text is CC BY-SA. |
| MaxPreps team pages | `sports_local` | A commercial website: read its terms before scraping, and prefer a licensed feed for on-air use. |
| A local cinema's website, a TV station's site, a brokerage's listing pages | `cinema`, `traffic`, `realty` and similar recipes | The named businesses are only the defaults of the recipes (settings `spots.recipes.<key>`); change the source URLs to sites you may use, and get the owners' consent before airing their listings, posters or photographs. |
| RSS feeds of news outlets | `headlines`, `sports`, `events` and other feed recipes | Headlines and first sentences are read verbatim; the outlets' terms and copyright apply. |
| TMDB (movie posters) | `cinema` | Requires attribution and a non-commercial or licensed agreement; see TMDB's terms. |
