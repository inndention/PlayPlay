# Star nodes (Star page)

Port of the PlayPlay OBS star-node daemon surface (`star-node/src/*.js`: fleet, sync, swarm, watchdog, exec),
`stations.py` (stations the HUB may drive through an allowlist) and the `/api/nodes` registry. Code:
`playout/supervisor/starnodes.*`, routes in `supervisorcore_suites.cpp`, allowlists in `suitelogic.*`.

## Registry and enrolment

Settings `starnodes.nodes` `{nodes: [...]}`. A node: `{id, name, host, execUrl (star-node exec API, port 8898),
token (write-only: x-agent-auth), stateUrl (watchdog API, port 8125; loopback only), playplayUrl (the node's
PlayPlay, e.g. http://host:7860 or http://host:7700), playplayPass (write-only, sent as X-PP-Pass), roles[],
enabled, render, allowExec (default false), notes, station {kind: playplay|cpp|masterplay, channel}}`.

`POST /starnodes/enrol {name, execUrl | host, token, stateUrl?, playplayUrl?, playplayPass?, roles?, notes?, force?}`
verifies the node before it is stored: `GET {execUrl}/api/fleet/status` with `x-agent-auth`. 401 = token refused
(not enrolled); unreachable = refused unless `force` (then stored as offline). Health is polled every
`starnodes.config.pollSec` (60 s): exec API, watchdog state (when `stateUrl` is set) and the node's PlayPlay
(`/api/status` or `/api/v1/status`).

## Remote actions (allowlisted)

`POST /starnodes/{id}/action {action, args}` — nothing outside this table runs:

| action | does |
|---|---|
| `fleet.status` / `fleet.analytics` | `GET /api/fleet/status` / `analytics` on the node |
| `fleet.query` | `POST /api/fleet/query {stationId, command, args}`; `command` ∈ on_air, system_health, as_played_today, run_report, adherence, schedule_hour, upcoming, channel_list (checked here *and* on the node) |
| `fleet.asPlayed` | `GET /api/fleet/as-played?date=` |
| `sync.jobs` / `sync.run` | `GET /api/sync/jobs` / `POST /api/sync/jobs/{id}/run` |
| `swarm.index` | `GET /api/swarm/index` |
| `browse` | `GET /api/browse?path=` |
| `state` / `service.restart` | watchdog `GET /api/state` / `POST /api/service/{id}/restart` (service ∈ playplay-server, mediamtx, obs-ch1..obs-ch4) |
| `station.status` / `station.capabilities` | the node's PlayPlay status; OPTIONS probes (405 = route exists, 404 = predates it) |
| `station.call` | one PlayPlay API call `{method, path, body}` matching the stations allowlist (schedule hours, shuffle/autofill, hour recipes, inputs, media reads, ingest, newsroom spots, events); never `/api/obs`, settings, nodes, failover, sessions, and never DELETE |
| `exec` | `POST /api/exec {command, shell}` — only when the node has `allowExec` **and** `starnodes.config.allowExec` is on (both default off); audited |

Every action is written to `starnodes.audit` (newest 300).

The `station.call` allowlist also covers the C++ station's own read surface (outputs, readiness, system stats, Facebook Live status, remote pad, spots / mux status,
channels, flight plan, overlays) and two calls that act: `POST /api/v1/channels/{id}/transport` and `POST /api/v1/remote/pad/{id}/press`; `fleet.query` accepts the read
commands `outputs`, `facebook_status`, `system_stats`, `readiness`, `spots_status`, `library_stats`. See "The whole system through the station agent" in `docs/AGENTS.md`.

## Routes (`/api/v1`)

`GET /starnodes` · `GET|PUT /starnodes/config` · `POST /starnodes/enrol` · `PUT|DELETE /starnodes/{id}` ·
`POST /starnodes/probe` · `POST /starnodes/{id}/probe` · `POST /starnodes/{id}/action` · `GET /starnodes/audit`.

Station-compatible (PlayPlay OBS shapes): `GET /api/stations`, `GET /api/stations/{sid}/capabilities`,
`POST /api/stations/{sid}/call` (`{method, path, body}` → `{status, body, ms}`), `GET|POST /api/nodes`,
`PATCH|DELETE /api/nodes/{id}`, `POST /api/nodes/{id}/refresh`, `GET /api/nodes/{id}/inventory`, `GET /api/fleet`.
`/api/nodes` rows are the same registry (`url` = playplayUrl, `auth_key` = playplayPass, `render=true` in notes opts
a node into render dispatch).

## Verified (2026-10-23)

No real star-node exec API (8898) was reachable from this box, so enrolment and actions were exercised against a
loopback mock with the same routes and `x-agent-auth` check: wrong token → 403, unreachable → 502 unless `force`,
verified enrol (token never echoed); `fleet.status`, `fleet.query on_air`, `browse` (Windows paths intact) reach the
node; `fleet.query exec`, unknown actions, `exec` with allowExec off, `station.call` DELETE / POST /api/settings are
refused before any request leaves (the mock's log confirms) and every attempt is in the audit. `station.call GET
/api/channels` ran against a real C++ station (the test Supervisor itself). Watchdog `state` needs a loopback
`stateUrl` (star-node binds 8125 to 127.0.0.1 only).
