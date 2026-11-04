# Publisher (Publish page)

Port of PlayPlay OBS `social.py` (Facebook queue from finished spots) and `publisher.py` (publications as immutable
records: corrections are new rows, retraction is a marker) plus the StarMedia publishing lane (`pub_draft`,
`pub_status`, `pub_stop`). Code: `playout/supervisor/publisher.*`, routes in `supervisorcore_suites.cpp`, rules in
`suitelogic.*`.

## Items

Settings `publisher.items` (newest 500). One item: `{id, kind article|spot|video|slate|text|link, source
manual|spot_job|spot_studio|agent_draft, title, body, caption, linkUrl, mediaId, channels[facebook, website],
status, revision, scheduleAtMs, ai {text, voice, images, video, model, agent}, sourceLinks[], tags[], tier
absolute|default, why, perChannel {facebook: {state, remoteId, permalink, error, attempts, postedMs}}, corrects,
metrics, history[{ms, actor, action, note, revision}], createdMs, updatedMs}`.

Status ladder: `draft → pending_approval | held → approved → scheduled → publishing → published | failed`;
side exits `skipped`, `retracted`, `corrected`. Rules (`suitelogic::classifyDraft`):

- **absolute tier** (obituaries, crime, injury or death, minors, allegations) is always `held` for a person;
  never auto-approved, whatever the mode;
- an `agent_draft` needs at least one `https://` source link on a public host, and its `ai.text` is forced on;
- a video item can be approved only when its library media is `status: approved` (unreviewed video never leaves);
- the AI label (`publisher.config.aiLabel`, default "Made with AI assistance.") is appended to every outgoing copy
  whose `ai` says AI made something;
- `maxPerDay` caps automatic queueing from spots.

Modes (`publisher.config.mode`): `off` (nothing queued automatically), `review` (finished spots of the listed
`recipes` are queued `pending_approval`), `auto` (queued approved unless absolute tier). A **kill switch**
(`POST /publish/stop`) stops all sending at once (scope all | facebook | site) and lowers the mode to off; releasing it
is a separate explicit call.

Sending (worker every 30 s, or `POST /publish/items/{id}/send`): Facebook Graph `POST {graphBase}/{page}/feed`
(text/link) or `{graphVideoBase}/{page}/videos` (multipart MP4); website = WordPress REST `POST {url}/wp/v2/posts`
as a **draft** unless `website.autoPublish`. Failures retry up to 3 times. Metrics (`POST /publish/items/{id}/metrics`)
read reactions/comments/shares back from Graph.

## Routes (`/api/v1`)

`GET /publish/status` · `GET|PUT /publish/config` (tokens write-only) · `POST /publish/verify` ·
`GET /publish/queue?status=&kind=&source=&channel=` · `GET /publish/items/{id}` · `POST /publish/draft`
(create, or revise with `id`) · `POST /publish/items/{id}/{approve|hold|skip|schedule|send|retry|correct|retract|metrics}`
· `DELETE /publish/items/{id}` (unpublished only) · `POST /publish/stop {reason, scope}` · `POST /publish/release
{scope}` · `GET /publish/audit`.

Station-compatible (PlayPlay OBS shapes): `GET|POST /api/social/config`, `POST /api/social/verify`,
`GET|POST /api/social/queue`, `POST /api/social/queue/{id}/approve|skip|retry`, `GET /api/social/pulse`,
`GET /api/publish/config`, `GET /api/publish/publications`, `POST /api/publish/schedule`,
`POST /api/publish/{id}/send|correct|retract|metrics`.

## Verified (2026-10-23)

No Facebook Page or website credentials exist on this box, so `graphBase`, `graphVideoBase` and the WordPress URL were
pointed at a loopback mock; everything else is the real code path: review gate (409 before approval), text + link
posts to `/feed` with the AI label appended, a 2 MB multipart MP4 to `/videos`, WordPress posted as a **draft** with
Basic auth, metrics read back, correction as a new linked item, retraction, absolute-tier and source-less agent drafts
held, archived/unreviewed media refused, kill switch → 423 then release. Note: `GET /api/social/queue` answers
`{posts: [...]}` (the Supervisor's REST server only returns objects), not a bare array.
