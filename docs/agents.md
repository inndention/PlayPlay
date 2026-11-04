# Agents (StarAgent suite, Agents page)

Port of PlayPlay OBS's operator console surface (`console.py` command table, `/api/op/*`, `/api/ai/*`) joined with
the StarAgent daemons already known to `StarClient` and the Star nodes. One task runner for all of them, with a
persistent result log. Code: `playout/supervisor/agentsuite.*`, operator commands in `supervisorcore_suites.cpp`.

## Agents

`GET /agents` lists:

| kind | agent | runs |
|---|---|---|
| `operator` | `station` (this Supervisor) | the command table (`GET /agents/commands`): read commands (channel_list, on_air, system_health, schedule_hour, upcoming, library_stats, find_media, as_played_today, expiring_soon, spots_status, render_status, publish_queue, star_nodes) and action commands (skip_current, arm, disarm, snap, run_spot_job, enforce_markers, publish_approve) |
| `staragent` | each StarAgent daemon (`star.agents`, port 8780) | health, status, nowplaying, list, search, and `exec` only when `agents.config.allowExec` is on |
| `starnode` | each enrolled Star node | the Star node action allowlist (`docs/star-nodes.md`) |
| `llm` | `assistant` (when an OpenRouter key is set in `spots.llm` or `agents.config`) | a prompt answered with a tool loop over the **read** operator commands (action commands only with `agents.config.allowActions`), at most 6 tool rounds |

## The whole system through the station agent (2026-10-25)

Besides the original read / action commands, the command table now covers every area of this station (34 commands; `GET /agents/commands`):

| command | risk | does |
|---|---|---|
| `capabilities {group?}` | read | every REST route grouped (`channels`, `live`, `spots`, `overlays`, `mux` ...) with how an agent may use it: `api_get`, `api_call` or `blocked`; without `group` it answers the group counts and this command table, so an LLM or a Star agent can discover the whole surface |
| `api_get {path}` | read | `GET` any route (with or without `/api/v1`) through the station's own REST; credentials are hidden in the reply; replies over 120 KB come back as `{truncated, bytes, preview}` |
| `api_call {method, path, body}` | action | `POST` / `PUT` on operator areas only (channels' transport / outputs / cg ..., schedule, spots, live, overlays, outputs, mux, library, inputs, flightplan, programming, ingest, publish, render, media, remote-pad presses) |
| `outputs {channel?}`, `output_control {channel, name, op}` | read / action | streaming outputs with state, bitrate, errors; start / stop / restart one |
| `facebook_status`, `facebook_go_live`, `facebook_end` | read / action / action | Facebook Live (docs/mux.md); the stream key is never returned |
| `pad_list`, `pad_press {id}` | read / action | the remote pad (docs/remote-pad.md) |
| `spot_generate {recipe}` | action | one spot now (the composed MIDI music bed is added automatically) |
| `system_stats`, `readiness`, `flight_plan {channel}` | read | CPU / memory / network / fps, schedule readiness, time debt |

**Guard rails** (`suites::agentApiAllowed`, `suites::redactSecrets` in `suitelogic.cpp`, unit-tested): never reachable by an agent are settings, the remote node list,
session exports, `/agents`, previews and `/api/op`; no `DELETE`; no `PUT /channels/{id}` (a channel definition change restarts its engine); no writes to sessions, failover,
Star nodes, remote nodes, alerts, AI config or render nodes; remote-pad layout edits. In every reply string values under keys that look like credentials (api keys, tokens,
passwords, secrets, passphrases, stream keys, authorization, cookies) become `"(hidden)"`, rtmp(s) URLs lose their key segment and `?token=`-style query values are masked.
Action commands still need `agents.config.allowActions` for the LLM assistant (and are `risk: action` in tasks).

**Star hub:** `fleet.query` accepts the new read commands `outputs`, `facebook_status`, `system_stats`, `readiness`, `spots_status`, `library_stats` (not the actions); the hub's
`station.call` allowlist (docs/star-nodes.md) gained the C++ station's read surface (`/api/v1/outputs`, `readiness`, `system/stats`, `live/facebook`, `remote/pad`, `spots/status`,
`mux/status`, `channels`, `channels/{id}/outputs`, `flightplan/{id}`, `overlays/{id}`) plus `POST /api/v1/channels/{id}/transport` and `POST /api/v1/remote/pad/{id}/press`.
Going live or editing settings on another station is not allowlisted for the hub: it goes through that station's own pad or dashboard.

**Verified (isolated test Supervisor, copied DB):** command table 34, capabilities groups and per-route verdicts (settings routes all `blocked`), reads and a pad press worked, every
blocked route and the DELETE / channel PUT / settings POST refused with the reason, engine-less actions answered 409 from the target, the same commands as agent tasks
(`risk` read / action recorded), and the fake Facebook stream key set in settings appeared nowhere in any reply (searched for the whole key and a fragment). 15 unit tests in `test_suites`.
Not exercised: the LLM assistant (the OpenRouter account has no credit), a real Star hub / StarAgent daemon calling the new commands, and actions against a live engine.

## Tasks

`POST /agents/tasks {agent, command, args, prompt?}` queues a task and returns it; tasks run one per agent at a time.
Task: `{id, agent, agentKind, command, args, prompt, risk, state queued|running|done|failed|cancelled, result,
error, steps[] (llm tool calls), createdMs, startedMs, finishedMs}`. Settings `agents.tasks` keeps the newest 300.
WS event `agents` carries the changed task.

## Routes (`/api/v1`)

`GET /agents` · `GET /agents/commands` · `GET|PUT /agents/config` (`allowExec`, `allowActions`, `model`, `apiKey`
write-only) · `GET /agents/tasks?agent=&state=&limit=` · `POST /agents/tasks` · `GET|DELETE /agents/tasks/{id}` ·
`POST /agents/tasks/clear`.

Station-compatible (PlayPlay OBS shapes): `GET /api/op/status`, `GET /api/op/commands`, `POST /api/op/exec
{command, args}` (runs synchronously, answers the result), `GET /api/ai/status`, `POST /api/ai/test`.

## AI key and models (shared by every OpenRouter caller)

`playout/supervisor/aimodels.*` is the one place the agent assistant, the spot studio writer and the spot generator LLM
writer get their key, base URL and model.

- **Key:** the `OPENROUTER_API_KEY` environment variable. A key typed in the Agents or Spots settings is saved once as
  `ai.config.apiKey` and overrides it. Routes report `hasKey` and `keySource` (`env` | `settings` | `none`), never the key;
  `GET /settings/ai.config` answers without it.
- **Model:** default `anthropic/claude-sonnet-5.5`. Legacy families (gpt-4o, gpt-4*, gpt-3.5, claude-3/2/instant,
  gemini-1.x/2.0/pro, palm) are never used: at start-up any saved setting naming one is moved to the default, and a
  request for one resolves to the default.
- **Catalog:** `GET /api/v1/ai/models[?all=1][&refresh=1]` fetches `https://openrouter.ai/api/v1/models`, keeps text-in /
  text-out models, drops the legacy families, and returns them newest first as `{id, name, created, context,
  promptPerMtok, completionPerMtok, vision}`. Without `all=1` it hides models older than 12 months. The list is cached in
  memory and in settings `ai.catalog` and refreshed every 6 h; if OpenRouter can't be reached it serves the cached copy,
  else a built-in current list. `GET|PUT /api/v1/ai/config` reads / sets `{model, apiKey (write-only), clearKey, baseUrl}`.
- **UI:** the Agents page (assistant model) and the Spots config (writer model) use a searchable picker showing price per
  million tokens and context (`qml/ui/ModelPicker.qml`, `web/aimodels.js`); key fields say "from environment
  (OPENROUTER_API_KEY)" when the variable is set.

## Verified (2026-10-23)

Operator commands (on_air, channel_list, system_health, upcoming, find_media, skip_current refused with no engine),
the real StarAgent daemon on this PC (health, status, list; online after the start-up probe), a Star node agent
(fleet.status), `/api/op/status|commands|exec` and `/api/ai/status`. With the environment key (keySource `env`): the catalog
returned 317 current models (446 with `all=1`), none legacy; saved `gpt-4o-mini` / `gpt-4o` settings were migrated to the
default at start-up. A call with the default model reached OpenRouter and was refused with 402 "insufficient credits"
(the key is valid; the account has no credit), so the end-to-end call was made with a free catalog model
(`recursive/flint-v1.0:free`): `/api/ai/test` answered and the assistant used the `channel_list` tool and answered
correctly.
