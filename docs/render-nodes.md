# Render nodes (Render page)

Port of PlayPlay OBS `mesh_render.py` (opt-in peer dispatch with durable request ids), `studio_render.py`
(StarMedia spot-node Jobs API v1) and the Star split render (`star_render_split`). Code:
`playout/supervisor/renderfarm.*`, routes in `supervisorcore_suites.cpp`, pure rules in `suitelogic.*`.

```
POST /render/jobs ──► RenderFarm queue ──► worker
   spot       ──► spot-node  POST /v1/jobs (Idempotency-Key = requestId) ► poll ► GET /v1/jobs/{id}/result ► MP4
               └► PlayPlay peer POST /api/newsroom/spot {request_id} ► reconcile GET /api/newsroom/render-request/{id}
   transcode  ──► local ffmpeg (preset) ► MP4/WAV
   split      ──► N segments (-ss/-t) over workers [local | StarAgent node] ► concat ► MP4
                                         ► IngestManager.enqueue(origin "render") ► library record ► job.mediaId
```

## Nodes

Settings `render.nodes` `{nodes: [...]}`. A node: `{id, name, kind: spotnode|playplay|staragent|local, url, token
(write-only; reads give hasToken), enabled, render (opt-in), primary, maxBusy (default 1), notes, starNode}`.

- **spotnode** — StarMedia render service (port 8775). Health `GET /health` (`capable`, `queue {running, queued,
  max}`, engines). Ready = capable and queue not full. The local one is **discovered** automatically when
  `D:\StarMedia\mesh\token` exists and `127.0.0.1:8775/health` answers (`discovered: true`; token read from the file at
  call time, never stored).
- **playplay** — another PlayPlay station. A peer is used only when it is opted in (`render: true`, or `render=true`
  in its notes, the PlayPlay OBS convention), not `primary`, not paused, and its queue is ≤ `maxBusy`
  (`suitelogic::chooseRenderNode`). PlayPlay OBS peers must report `capabilities.newsroom_mesh_v2`; C++ peers answer
  `/api/v1/render/status`.
- **staragent** — a StarAgent daemon (port 8780, `starNode` = its StarClient node id). Runs split-render segments with
  `POST /exec` when the input exists on that node (`remoteInput`), and the segment comes back by Star fetch.
- **local** — this Supervisor; always present, `maxLocal` ffmpeg processes (default 2).

## Jobs

Settings `render.jobs` (newest 300 kept). `POST /render/jobs` bodies:

| kind | fields |
|---|---|
| `spot` | `type` motion_spot \| template_render, `brief`, `template`, `variables` / `scenes`, `lengthSec` (3–120), `aspect`, `mode` offline \| hybrid, `voice`, `music`, `captions`, `brand` (object or kit id), `assets`, `target` auto \| node id, `ingest` {type, title, category, client, allowedChannels, archived}, `requestId` |
| `transcode` | `input` (path or media id), `preset` h264_1080p30 \| h264_720p30 \| proxy_540p \| audio_wav \| loudnorm \| custom, `args` (custom only when `render.config.allowCustomArgs`), `ingest` |
| `split` | `input`, `segments` (1–50), `workers` ["local", node ids], `preset`, `copy`, `durationSec` (probed when absent), `remoteInput` {nodeId: path}, `ingest` |

Job: `{id, kind, state queued|running|done|failed|cancelled, step, pct, node, nodeName, requestId, remoteId,
input, output, mediaId, error, costUsd, segments[{index, startSec, durSec, worker, state, pct, output, error}],
createdMs, startedMs, finishedMs, log[]}`. WS event `render` carries `{jobs, counts}`.

Idempotency: a spot job's `requestId` is the spot-node Idempotency-Key, so a lost response never renders twice.
`render.requests` records station-side requests (`POST /api/newsroom/spot` with `request_id`): the same id with the
same body returns the stored result, a different body is 409.

## Routes (`/api/v1`)

`GET /render/status` · `GET|PUT /render/config` (`studioMode` off|prefer|only, `renderMode` offline|hybrid,
`waitSec`, `maxLocal`, `meshDispatch` auto|local|off, `allowCustomArgs`) · `GET|PUT /render/nodes` ·
`POST /render/nodes` · `PUT|DELETE /render/nodes/{id}` · `POST /render/probe` · `POST /render/nodes/{id}/probe` ·
`GET /render/templates` (spot-node `/v1/templates`, cached 10 min) · `GET /render/jobs?state=&limit=` ·
`GET|DELETE /render/jobs/{id}` · `POST /render/jobs` · `POST /render/jobs/{id}/cancel` · `POST /render/jobs/clear`.

Station-compatible (PlayPlay OBS shapes): `GET /api/newsroom/studio`, `POST /api/newsroom/spot`
(`{recipe, data, channel, voice, notes, request_id, mesh: auto|local, background}`),
`GET /api/newsroom/render-request/{id}`.

## Verified (2026-10-23, test Supervisor on 7740 with a copied DB)

- **Real, end to end:** the local spot-node on `dev` was discovered from the mesh token, probed (healthy, 37 templates
  listed), rendered a `motion_spot` and a guided-studio spot; the MP4s were downloaded and ingested (archived). A spot
  job interrupted by a Supervisor restart resumed polling and completed. Local `transcode` (proxy_540p) and a 3-segment
  local `split` (h264_720p30) both finished and landed in the library.
- **Implemented, not exercised:** dispatch to PlayPlay peers (`/api/newsroom/spot` with `request_id` + reconcile) and
  StarAgent segment workers (`/exec` + Star fetch): no peer was opted in and no remote StarAgent/star-node was reachable
  from this box (node-a / node-b / node-c ports refused or timed out).
