# Reports

Branded, print-ready HTML reports (plus CSV and JSON) over the as-run log, the media library, the effective
schedule and the traffic contracts. Port of PlayPlay OBS `server/reports.py` onto the Supervisor's `ScheduleStore`.

Code: `playout/supervisor/reports.h/.cpp` (`ReportBuilder`, pure functions over the store),
`playout/supervisor/supervisorcore_reports.cpp` (routes), `web/reports.js` (dashboard card set under the as-played
table), `Play_Play_Native/qml/views/ReportsView.qml` (Reports panel + row context menu).

## URLs

| Pretty URL (HTML / CSV / JSON; `?token=` when the station has an API token) | JSON API (bearer token applies) | What |
|---|---|---|
| `/reports/index.html` (also `/reports/`) | `/api/v1/reports` | Landing page with one form per report / API catalogue |
| `/reports/asplayed.html` | `/api/v1/reports/asplayed` | As-played log for a channel + day or range |
| `/reports/asset/{id}.html` | `/api/v1/reports/asset/{id}` | Per-asset report |
| `/reports/assets.html` | `/api/v1/reports/assets` | Many assets, one compact section each |
| `/reports/full.html` | `/api/v1/reports/full` | Period / full report |
| `/reports/adherence.html` | `/api/v1/reports/adherence` | As-scheduled vs as-played for one channel-day |
| `/reports/contracts.html` | `/api/v1/reports/contracts` | Ad contract delivery |
| `/reports/top.html` | `/api/v1/reports/top` | Most-aired assets |
| `/reports/compare.html` | `/api/v1/reports/compare` | Side-by-side airings per day |

Formats: `/reports/<name>.html`, `/reports/<name>.csv`, `/reports/<name>.json`, or `?format=html|csv|json` on any
of them. CSV is UTF-8 with BOM, CRLF, RFC-4180 quoting, served as an attachment (`<name>_<from>.csv`); a text cell
starting with `=`, `+`, `-`, `@`, TAB or CR gets a leading apostrophe (plain numbers such as a negative variance are
left alone) so a media title cannot run as a spreadsheet formula. The JSON
routes add `htmlUrl` / `csvUrl` pointing at the pretty URL with the same parameters (never the `token`). Dates are
`YYYY-MM-DD` in the **channel's time zone** (`ChannelProfile.timezone`; "all channels" uses the first channel's zone).
Every window is capped at 366 days. A report runs synchronously on the Supervisor's event-loop thread (next to the
executor tick, telemetry and engine IPC), so every as-run scan is bounded: at most 400 000 rows **and** 1.5 s of
wall-clock per report (`truncated: true` + a banner when either limit, or a 100 000-row day chunk, is hit; the oldest
days are scanned first, so a truncated window is missing its newest days). Narrow the range, pick one channel or add
filters for a complete picture. An all-channels scan walks the channels one by one (the as-run index is
`(channel_id, actual_start)`; a bare time range is a full table scan per chunk), so as-run rows of a channel that has
been deleted from the channels table only appear when that channel id is asked for explicitly. The assets report
emits at most 50 000 airing rows in total (`rows` per asset still applies), and media records are looked up by id on
demand instead of loading the library per request.

Query strings: spaces must be `%20` (the Supervisor's query decoder keeps a literal `+`). The dashboard and the
operator app build URLs with `encodeURIComponent`; the forms on `/reports/index.html` submit through a small script
that does the same and drops empty fields (so `from=&to=` never overrides a chosen `date`).

## Parameters

| Report | Parameters (defaults) |
|---|---|
| asplayed | `channel` (first channel; `all` = every channel), `date` (today) **or** `from`,`to`, `period=daily|weekly|monthly|quarterly|custom`, `type`, `origin`, `q` (title/asset-id contains), `events` (event type or `all`), `limit` (5000, max 20000) |
| asset | `id` (required), `days` (30), `channel` |
| assets | `ids` (comma list) **or** library filter `q`,`type`,`category`,`subcategory`,`client`,`series`,`channel`,`archived=1`; `days` (30), `limit` (200 assets, max 800), `rows` (200 airings per asset) |
| full | `period=daily|weekly|monthly|quarterly|annually|custom` (daily; `custom` with `from`,`to`), `date` (anchor, today), `group=all|type|client|series|category|origin` (all), `channel` |
| adherence | `channel` (first channel), `date` (today), `toleranceSec` (90) |
| contracts | `id` (one contract; blank = all), `date` (as-of, today) |
| top | `days` (30), `n` (25, max 200), `type`, `channel` |
| compare | `ids` (2-8, comma list), `days` (30), `channel` |

Weeks start on Sunday (station day-of-week). `period=weekly&date=2026-10-22` is the week containing that date.

## Data model notes

* A **START** as-run row is one airing; its `actualEndMs`/`actualDurationMs` are filled by `completeAsRun` when
  the END arrives (so `complete: true` on a START row means it ran to its end). SKIP / FAILED / TRUNCATED rows are
  separate events. Planned times come from the ScheduleExecutor (`plannedStartMs`); `varianceMs` = actual - planned.
* Status badges: `on time`, `late`, `early`, `fill`, `manual`, `unscheduled` (START without a planned time),
  `skipped`, `failed`, `truncated`. "Late > 5 s" in the as-played cards counts `varianceMs > 5000`.
* **Adherence** rebuilds the day's planned timeline from `effectiveSlot(channel, date, hour)` (hour start +
  cumulative durations; hard starts at `fixedOffsetSec` / `fixedUtcMs`), then matches START rows in that hour by
  slot item id / runlist id first, then by `mediaId` (closest start). Hour starts are wall-clock
  (`RunlistBuilder::hourStartUtcMs`, like the executor), so 23 h and 25 h DST days line up. Delta uses the
  executor's `plannedStartMs` when the row has one. Runlist ids are `<slotId>#<itemId>#<n>`.
  **Query / folder blocks** (`SlotItem::query()` / `folder()`) are one planned item each: the rows they air carry the
  ids `<blockId>#<k>` (`RunlistBuilder::expandQuery`), so every such row in the hour is consumed by the block, its
  status and delta come from the first pick, `picks` / `pickCount` list them and the row shows a "block · N aired"
  tag (`matchedBy: "block"`). Unmatched planned items are `missed` once their planned start + tolerance has passed;
  before that they are `pending` (not missed, not in `planned` or the score; `counts.pending`). Unmatched START rows
  are `unscheduled`. Score = on-time / planned (matched or missed items).
* **Asset -> upcoming placements** scans the next 7 days of effective slots on every channel the asset is allowed
  on: `mediaId == id` -> `direct`; a query block (types / categories / subcategories / series / tags / clients,
  length bounds) or folder block the asset can air from, judged by the very matcher the RunlistBuilder resolves
  blocks with (`QueryBlocks::matches` / `matchesFolder`: availability window, `allowedChannels`, playout status) ->
  `eligible via block`. Placement times are wall-clock hours (DST safe).
* **Assets report** marks `never aired` (no airing in the window and `playCount == 0`) and lists **orphans**:
  as-run asset ids that no longer resolve to a media record.
* **Contracts** read settings key `traffic.contracts`, parsed with the executor's own `AdContract` (the shape
  `PUT /api/v1/traffic/contracts` stores and the web / QML editors write):
  `{contracts:[{id, client, title, mediaIds[], spotsTotal, spotsPerDay, startDate, endDate, channels[], dayparts:[{from,to}], status: active|paused|done, priority}]}`
  (`from`/`to` as `"HH:MM"` or hour numbers; `channels` empty = every channel; missing key = no contracts; the
  pre-AdContract spellings `channel` and `campaign` are still read). The report JSON keeps the normalised contract
  fields and adds the delivery numbers; **`contractStatus`** is the stored status and **`status`** the delivery
  status. Window and target follow `FormatClock::contractReport`: an open `endDate` means the as-of day (the whole
  total is due now); `spotsTotal` is paced only when there is a `startDate`; otherwise target = `spotsPerDay x days`
  (a missing start looks back 30 days for counting, flagged `openStart`); no `spotsTotal` and no `spotsPerDay` =
  **no target**. Expected-to-date = target x elapsed / total days, where the **day in progress counts only as far
  as the wall clock has got into it** (`elapsedExact`; a past as-of date counts whole days); pacing % = aired /
  expected; projected = aired / elapsedExact x total; status `complete | on pace (>= 95 %, or under one spot due so
  far) | behind | under-delivered | not started | no target`, and a paused / done contract shows its own status
  and is not counted as behind. Delivery counts only spots aired on the contract's channels and inside its
  dayparts; the executor's autofill pacing (`airedCount`) counts every channel and daypart, so the two `aired`
  numbers can differ for a channel- or daypart-restricted contract. The as-run scan walks the merged contract
  windows (disjoint spans, only the channels the contracts run on, at most 366 days in total, newest first) instead
  of one range from the earliest start to the latest end.
* **Full report** groups by media attributes (client, series, `extra.category`) joined from the library; rows whose
  asset is gone fall into "unattributed" / "uncategorised". The narrative paragraph is a plain template over
  the totals (no AI).
* Dayparts (6-way): Overnight 00-06, Early 06-09, Daytime 09-16, Early fringe 16-19, Prime 19-23, Late night 23-24.

## Branding

Settings key `station.brand`: `{name, tagline, logoUrl, footer, accent}`. Defaults: `name` = first channel's
`brandName` or "PLAY PLAY", tagline "Broadcast playout automation", accent `#d80018`, no logo. The masthead
bolds the last word of the name in the accent colour. All CSS is inline (print-ready, `@media print` rules,
`break-inside: avoid`); the only external asset is the optional logo.

Set it with `PUT /api/v1/settings/station.brand {"value": {"name": "Star News", "accent": "#d80018", "logoUrl": "https://.../logo.png"}}`.

## UI

* Web dashboard -> Reports tab: the "Reports" card set under the live as-played table (period picker, channel,
  group, as-played / adherence / contracts / top buttons, asset search with "Asset report" links, compare box for
  up to 4 assets, "Assets report for filter"). Links open in a new tab and carry `?token=` when a token is set.
  Media ids reach the inline handlers through `data-id` attributes, never inside a JS string.
* Operator app -> Reports view: the "Reports" panel (period / date / group / channel, buttons that open the system
  browser via `view.repUrl("/reports/...")` — `api.rawUrl` plus the token —, asset search (debounced 300 ms) with a
  7-day summary line) and a right-click menu on every as-run row (Asset report, this hour in the as-played report,
  add to compare, open in library).

## Security note

`/reports/*` is **behind the same bearer token as `/api/`** (settings `rest.token`; with no token configured nothing
is gated). Its `.html`, `.csv` and `.json` / `?format=json` forms return the same as-run history, client names,
contract delivery and local file paths as the gated API, so they check the token themselves: browsers cannot send an
`Authorization` header on navigation, hence `?token=<token>` on the report URL (401 otherwise). Report pages opened
with a token keep it on their own links and forms; `htmlUrl` / `csvUrl` in the JSON never echo it. Responses carry no
CORS header (report pages are navigation targets, not XHR targets) and `Referrer-Policy: no-referrer`. Thumbnails
(`/thumbs/`) stay open like the static dashboard.
