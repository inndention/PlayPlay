# Programming: query blocks, hour recipes, format clock, autofill, contracts

Added 2026-10-22 (Phase 9). This is the PlayPlay OBS way of filling hours without dragging files: the operator says
what an hour is **made of** and the Supervisor binds it to media when the hour is built. Everything lives in
`playout/schedule/queryblocks.*`, `playout/schedule/formatclock.*`, `RunlistBuilder::expandQuery`,
`ScheduleExecutor::effectiveOrSynthesized` and the routes in `playout/supervisor/supervisorcore_programming.cpp`.
UI: web **Programming** tab + hour editor (Add block, Fill hour, Resolve, right-click menus); QML **Programming** page
+ the same controls in the Schedule view.

## Late-bound items

A `SlotItem` may carry one of:

```json
"queryBlock":  { "types": ["program"], "targetSec": 600, "prefer": "least_played",
                 "categories": [], "subcategories": [], "series": [], "tags": [], "clients": [],
                 "nthLatest": 0, "noRepeatMin": 30, "minSec": 0, "maxSec": 0, "fitToTarget": true }
"folderBlock": { "folder": "D:\\media\\ads", "targetSec": 0, "weight": "freshness", "order": "random", "recursive": true }
```

plus the lock fields `isLocked`, `lockAtSec` (soft anchor, seconds into the hour; a negative value means "no anchor" and is
what both editors send to unlock), `lockLabel`. An explicit `isLocked: false` wins over a remembered `lockAtSec`; an anchor
sent without any lock flag locks. PlayPlay OBS snake_case (`query_block`, `folder_block`, `lock_at_sec`, `lock_label`) is
accepted on input.

Resolution (`QueryBlocks`):
- eligibility: path set, duration > 0.5 s, availability window, not archived/missing, `allowedChannels`;
- filters: type, `extra.category`, `extra.subcategory`, `series`, `extra.tags`, `client` (all case-insensitive, empty = any);
- `prefer`: `least_played` (fewest plays → longest since aired, never aired first → newest; the three keys compare in that
  order, they are not summed), `newest`, `oldest`, `most_played`, `random` (seeded per hour+item), `longest`, `shortest`;
- items aired inside `noRepeatMin` rank behind fresh ones but stay eligible (a tiny pool must still resolve);
- ad-contract pacing bonus (see below) sorts ahead of `prefer`;
- `targetSec > 0`: items are taken until the target is covered (10 % tolerance, ≥ 15 s). If nothing fits and nothing
  has been placed, the best candidate is taken anyway, flagged `overrun`, and `RunlistBuilder::expandQuery` cuts it to the
  block target (`outPoint = targetSec`, runlist note "cut to the block target") so the rest of the hour keeps its shape.
  `fitToTarget: false` opts out: the whole item airs, flagged only;
- series blocks (`series` set) pick positionally (`nthLatest` 0 = latest, 1 = previous) by episode date and ignore
  exclusions, so "latest GNB" and "last week's GNB" can share an hour;
- folder blocks match a path prefix, or a bare name against the file's folder / category / subcategory / tag / series.
  `order random` is weighted by freshness (`1/(1+plays)`, ×0.15 if aired in the last 30 min); `name|date|size|added`
  play round robin (the item after the most recently aired one; a folder nothing in has aired starts at its first item).

The builder expands each block into concrete runlist items (origin `query`), never repeating a media id within the
hour or against the neighbouring hour, and caches the picks per `"<hourStartMs>#<itemId>"` (plus a `"...@spec"` entry
holding a hash of the block spec) so an hour keeps its picks while it airs. A cached pick is reused only while the block
spec is unchanged and every pick is still playable, still matches the block and is not held by another item of the hour
(an explicit item added later, or a block edited in place, re-resolves the block). A block whose picks are all cached never
reads the media library; the executor reads the library at most once per rebuild.

`POST /api/v1/channels/{id}/reroll` gives the blocks that have **not started airing** different picks: their previous picks
rank last when the block resolves again and every seed moves on (a per-channel re-roll epoch), so deterministic orderings
(`least_played`, `newest`...) move to the next best candidate; if nothing else is left the old picks come back rather than a
hole. Blocks that are on air or already aired keep their picks (the item the engine is playing must stay the item the
runlist says is on air), and the next hour's synthesized autofill layout is laid again.

Soft anchors (`isLocked` + `lockAtSec`) are seated at the item boundary nearest their offset; nothing is cut (a hard
start would). Use `timingMode: hard_start` when the cut matters.

## Hour recipes

```
GET  /api/v1/programming/recipes              {recipes[], default, types[], prefers[]}
PUT  /api/v1/programming/recipes              {recipes:[{id, name, idAtTop, parts:[{label, types[], minutes, prefer, categories[], subcategories[], series[]}]}]}
POST /api/v1/programming/recipes/ad-break     {programMin, adMin, autogenMin, programTypes[], adTypes[], prefer, adPrefer, idEveryBreak, idAtTop, programCategories[], programSeries[], name, save} → recipe + preview
POST /api/v1/schedule/{ch}/hour-recipe        {recipeId|recipe|parts, scope: hour|day|week|hours, slotId|date|dow|hour, hours[], dows[], weekStart, dated, emptyOnly, keepLocks, apply}
```

A recipe is a repeating pattern of time ("10 min programs, 5 min ads, 3 min autogen rotation, 1 min IDs, repeat").
Each part becomes a query block; the cycle repeats until the hour is full (a last short block closes a gap ≥ 60 s).
`keepLocks` re-seats the hour's locked items (leads first, anchors at their offsets). The default recipe matches
PlayPlay OBS's `DEFAULT_HOUR_PARTS`. The ad-break builder produces "programs with a break every N minutes". The last gap of
the hour (shorter than the next part) is closed with the recipe's longest non-program part that still fits, never with a
program cut after a few minutes; a programs-only recipe closes with its own part.

## Weighted fill (format clock)

```
GET/PUT /api/v1/channels/{id}/weights         {buckets:[{label, types[], categories[], subcategories[], series, pct, maxPerHour}]}
POST    /api/v1/schedule/{ch}/weighted-fill   {scope..., buckets?, keepLocks, oneProgramPerHour, programCapSec, seed, apply}
```

Shares are **airtime** of the hour. Per bucket a rotation queue (least played → freshness → seeded jitter); each pick
goes to the bucket most behind its share (renormalised over the buckets still alive); anchors reserve their airtime
and the next anchor's offset bounds the room (a locked hard-start item without `lockAtSec` is an anchor at its `fixedOffsetSec`,
like a soft anchor at its `lockAtSec`); one program per hour, and programs the rule rejects keep their place in the
rotation (they are not rotated to the back, so every program gets its turn); a program longer than `programCapSec` (40 min)
airs its leading segment and the remainder is carried into the next adjacent hour as a continuation (`inPoint`).
Produces concrete media (not late bound). Hours the pool could not fill are never blanked.

## Autofill policy

```
GET/PUT /api/v1/channels/{id}/autofill        {enabled, mode: recipe|weighted, recipeId, order[], noRepeatMin, adPacing}
```

Stored in `ChannelProfile.extra.autofill`. With `enabled`, an hour with no items is synthesised when the executor
builds it: from the recipe (late bound) or the format clock (seeded by the hour, so stable while it airs). Nothing
is written to the schedule; the runlist notes say `autofill (...)`.

The executor synthesises an hour again on every rebuild (hour change, any store write), so the result must not move:
item ids are a function of the hour (`af-<hours since epoch>-q<n>` for recipe blocks, `-w<n>` for weighted items), which
keeps runlist ids, pick-cache keys and the push hash stable, and a weighted layout (concrete media sorted by play counts,
which change as items air) is frozen per hour until the policy, buckets, a re-roll or an unavailable file change it. A
program longer than the 40 minute cap that the weighted fill splits leaves its remainder as a continuation (`inPoint`) that
leads the next hour synthesized after it (it is carried from the previous hour's frozen layout, so it survives rebuilds;
a Supervisor restart in the middle of the second hour starts that hour fresh).

Fixed-source channels: `ChannelProfile.extra.fixedSource = {type: ndi|udp|srt|rtsp|sdi|file, name|url|port, title}`
makes the executor air one looping live item (used by the mux for NDI service sources). The item is the same item in every
hour: slot id `fixed`, item id `fixed-<hash of the spec>`, so the runlist id never changes. Nothing is queued for :00 (no
second receiver, no hard cut), the hour change is not a new playlist, and only a changed source spec replaces the item.

## Ad contracts and pacing

```
GET/PUT /api/v1/traffic/contracts             {contracts:[{id, client, title, mediaIds[], spotsTotal, spotsPerDay, startDate, endDate, channels[], dayparts[], status, priority}]}
GET     /api/v1/traffic/contracts/{id}        delivery report (aired, target, pacingPct, remaining, daysLeft, neededPerDay, projectedShortfall)
GET     /api/v1/channels/{id}/pacing          {bonus:{mediaId: score}}
```

`FormatClock::contractPacing`: `+25 × (linear target − aired)` while behind, `−500` once fulfilled; `spotsPerDay`
adds `+20 × remaining today` / `−300` when met; `priority × 5`. `aired` and `today` count every creative of the contract
together (`mediaIds` is a list; `contractReport` sums the same way), and each creative gets the contract's bonus. The bonus is applied when query/folder blocks resolve
(ads float to the top of their tier) and refreshed on every rebuild from the as-run table (`ScheduleStore::airedCount`).

## Hour tools

```
POST /api/v1/schedule/{ch}/slot/{slot}/resolved   body = editor state (or empty) → the runlist the executor would build
POST /api/v1/schedule/{ch}/slot/{slot}/lock       {itemId, locked, lockAtSec, lockLabel}
POST /api/v1/schedule/{ch}/slot/{slot}/shuffle    shuffle unlocked interstitials (programs keep their place)
POST /api/v1/schedule/{ch}/clear                  {scope..., keepLocks}
POST /api/v1/schedule/{ch}/copy                   {from:{scope, slotId|date|dow|weekStart}, to:{dow|date|weekStart, hour?, dated}}
POST /api/v1/programming/query/preview            {channel, block|folder, n} → ranked candidates + picks
GET  /api/v1/programming/facets                   distinct types / categories / subcategories / series / clients / tags / folders
```

## Context menus and clipboard

Right-click works on grid cells, editor rows, library rows, channel cards, rundown rows and as-run rows in both UIs.
The editor has an in-app clipboard: Cut/Copy/Paste items (Ctrl+X/C/V, Delete), Copy/Paste hour (template or dated),
Copy/Paste day, copy an hour to every day or to every hour of a day, Properties (every item field). Text size:
`A−`/`A+` in the header (Ctrl+= / Ctrl+- / Ctrl+0), persisted per machine (QML `Theme.scale`) or per browser
(`document.documentElement.style.zoom`).
