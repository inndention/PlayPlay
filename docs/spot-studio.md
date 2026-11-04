# Spot studio (guided spots, review, ad rotation)

The guided half of spot creation; the automated half (recipes, jobs, GEN markers, scatter) is `docs/spots.md`.
Ports the StarMedia guided spot flow (`spot_start` → `spot_update` → `spot_generate` → `spot_status`) and PlayPlay
OBS's "revise a spot with a note". Code: `playout/supervisor/spotstudio.*`, routes in `supervisorcore_suites.cpp`,
timing rules in `suitelogic.*`.

```
brief ──► project: concepts (1-3) + draft script (scenes: beat, visual, vo, onScreen, exact durations) + questions
  ──► PUT answers / scene edits / end card / instruction (timing rebalanced to the exact length; compose goes stale)
  ──► generate: engine local  = SpotGenerator (recipe "studio_spot": narration ► Kokoro/SAPI ► card ► ffmpeg)
                engine studio = RenderFarm spot job ► spot-node (template_render with the scenes, or motion_spot)
  ──► media lands ARCHIVED (never airs unreviewed) ──► review: submit ► approve | changes | reject
  ──► publish: media un-archived, type ad|promo|psa, client, flight dates, channels, ad contract (traffic.contracts)
                ► the ad-break builder / format clock rotate it; optional social draft (Publisher)
```

## Project

Settings `spotstudio.projects` (newest 200). `{id, title, kind ad|promo|psa|announcement, client, brief, lengthSec
6|15|30|60, aspect, template, engine local|studio, voice, music, tone, brand {name, cta, url, colors[]},
concepts[{title, hook, angle}], concept, scenes[{n, beat, visual, vo, onScreen, durationSec}], endCard {headline,
cta, url, vo}, questions[{id, q, options}], stale, status draft|rendering|rendered|in_review|changes_requested|
approved|rejected|published|failed, render {engine, jobId|genId, state, pct, error, mediaId, path, durationSec},
review {notes, history[]}, published {mediaId, contractId, ms}, history[], createdMs, updatedMs}`.

Scene counts by length: 6 s → 2, 15 s → 3, 30 s → 5, 60 s → 8 (end card included). Durations always sum to the
length (`suitelogic::balanceScenes`). With an OpenRouter key, an `instruction` rewrites the script; without it
the instruction is kept as a review note and the deterministic writer is used.

## Routes (`/api/v1`)

`GET /spotstudio/options` (templates from spot-node, voices, music beds, engines ready?) ·
`GET|POST /spotstudio/projects` · `GET|PUT|DELETE /spotstudio/projects/{id}` ·
`POST /spotstudio/projects/{id}/generate {step: script|compose|all, engine?}` ·
`POST /spotstudio/projects/{id}/review {action: submit|approve|reject|changes, notes}` ·
`POST /spotstudio/projects/{id}/publish {type, client, title, channels[], startDate, endDate, contractId |
contract {spotsPerDay, spotsTotal, dayparts, priority}, social {queue, message}}` ·
`POST /spots/spot/{mediaId}/revise {notes, voice?}` (regenerates an auto spot with the editor's note; the new
media records `extra.revisionOf`).

## Verified (2026-10-23)

End to end on the test Supervisor: brief → 3-scene 15 s draft with questions → answers clear the questions and
rebuild the end card → scene edit → review refused before a render → local render (SpotGenerator, Kokoro/SAPI, 14.84 s
MP4 moved to `uploads/studio`, archived, `status: review`) → submit → changes → submit → approve → publish (type `ad`,
client, channel, flight dates, new contract `ct-…` 6/day in `traffic.contracts`, social draft queued). A second project
rendered through the `studio` engine on the spot-node. "Revise with a note" regenerated a `history` spot whose record
carries `revisionOf` and `autogen.notes`; a non-auto spot is refused. Without an OpenRouter key (`OPENROUTER_API_KEY`, see
docs/agents.md) the writer instruction is kept as a note instead of rewriting the script; with one, the writer uses the
shared model setting (default `anthropic/claude-sonnet-5.5`).

## Market

A project carries `market` (`""` = the station default, `"none"` = generic, or a market key: Martinsville, Reidsville, Danville; docs/spots.md "Area markets"). It is
passed to the local render (place-name respellings in the voice, the library record's `extra.market`), added to the LLM rewrite prompt and to the studio engine's
brief. The script itself stays yours: the market never changes facts in a brief.
