# Redundancy: sessions, remote nodes, primary/backup failover

PLAY PLAY's redundancy layer lives in the Supervisor (`playout/supervisor/sessionsmanager.*`,
`failoverpair.*`, `supervisorcore_redundancy.cpp`) and is operated from the **Remote** tab of the
web dashboard (`web/remote.js`) or the operator app (`Play_Play_Native/qml/views/RemoteView.qml`).
It ports PlayPlay OBS's `sessions.py` + `failover.py`:

* **Sessions** — portable, versioned bundles of station state you can capture, clone, export to a
  file, import, apply back onto a station, and push/pull between nodes.
* **Remote nodes** — other Supervisors driven from this UI through a whitelisted proxy.
* **Failover pair** — two Supervisors paired as primary + warm backup with heartbeats, automatic
  promotion, split-brain resolution and continuous state replication (media files optional).

Everything is settings-backed and reconfigurable at runtime; nothing requires a restart.

---

## 1. Sessions

### Bundle format (`playplay-session/1`)

```json
{
  "format": "playplay-session/1",
  "id": "20261022153012-7f3a",           // registry id (compact UTC timestamp + 4 hex)
  "name": "Friday lineup", "notes": "", "version": 1, "origin": "capture",
  "createdAt": 1761147012000, "updatedAt": 1761147012000,
  "node": { "id": "<uuid>", "name": "PLAYOUT-A", "version": "dev" },
  "groups": {
    "channels":    [ ChannelProfile.toJson()… ],           // outputs INCLUDED (applied only with group "outputs")
    "schedule":    [ HourSlot.toJson()… ],                 // every channel: template "D-H" + dated "YYYY-MM-DD-H"
    "media":       [ MediaRecord.toJson() minus transcriptText… ],
    "settings":    { "key": "value" },                     // non-local keys not claimed by the groups below
    "cg":          { "cg.*": "…" },
    "spots":       { "spots.*": "…" },
    "programming": { "recipes.*|traffic.*|clock.*|programming.*|hour_recipes": "…" }
  },
  "mediaManifest": { "<mediaId>": { "hash": "…", "path": "D:\\media\\x.mp4", "size": 123, "title": "…" } },
  "summary": { "channels": 2, "schedule": 336, "media": 1240, "settings": 12, "cg": 3, "spots": 4, "programming": 6, "mediaRefs": 1240, "groups": [...] }
}
```

**Never bundled (local-only settings)** — exact keys `rest.token`, `rest.bind`, `rest.port`, `ws.port`,
`station.password`, `station.port`, `failover.config`, `failover.epoch`, `remote.nodes`, `node.id`,
`node.name`, `mesh.anfsToken`, `mesh.anfsCookie`, `alerts.twilio`; prefixes `failover.`, `redundancy.`,
`node.`, `rest.`, `mesh.anfs`, `alerts.twilio`, `local.`. They are also skipped on apply even if a
foreign bundle carries them.

**Credential settings** the station itself needs — `spots.llm` (LLM API key), `star.agents` (agent tokens),
`onvif.devices` (camera passwords) — are left out of every captured / exported / pushed / `?data=1` session,
and stripped from imported or received ones and from older session files on read, so a `.ppsession.json`
never carries them. Applying a session therefore never touches them (absent keys are not written). The only
carrier is the replication export: `GET /sessions/export-live?secrets=1` includes them, and the Supervisor
honours that **only when its REST is token-protected** (`rest.token` set; the response says `secrets:true|false`),
so a promoted backup can still generate spots, drive cameras and reach its Star agents.

**Session ids** are file names, so only the shape `newId()` makes is accepted: `^[0-9]{14}-[0-9a-f]{4}$`
(UTC stamp, `-`, 4 hex). Anything else never reaches the file system: `POST /sessions/receive` answers 400
`invalid session id`, every `{id}` route (read, export, clone, apply, delete, push, pull) treats it as unknown
(404) or 400, and an `index.json` row with another id is dropped.

Sessions are files: `<dataDir>/sessions/<id>.json` + `index.json` (rebuilt from the files if missing).

### Apply semantics (`applyBundle`)

Order: **media → channels → schedule → settings groups**. A dry run computes the same report
without writing. All writes go through `ScheduleStore`.

* **media** — for each record: file at the bundle path exists → kept; else a local record with the
  same content `hash` (and an existing file) → path rewritten to the local file; a hash match under a
  *different* local id → the bundle id is **remapped** to the local id (record not duplicated; a foreign
  apply only — replication uses `mirror`, which keeps every record under the primary's id because the
  contracts / recipes / spots settings reference those ids; settings values that name a remapped id are
  rewritten with it); else a
  local record with the same id and an existing file → local path kept (warning); else the record is
  imported with `extra.missing = true` and listed in `unresolvedMedia` so the schedule stays intact.
  Local `transcriptText`, transcript summary and thumbnail survive when the bundle lacks them.
* **channels** — `outputs` stay local unless `"outputs"` is in `groups`; the `armed` flag stays local
  (`keepArmed`, override with `armFromBundle:true`); a channel new to this node starts disarmed **and
  with empty outputs** (the sender's outputs address the sender's hardware/targets; a warning says so).
  A channel id containing `/`, `\`, `:` or `..` is refused (it names the engine's data directory).
  Identity asset paths that do not exist here produce warnings.
* **schedule** — every slot saved via `saveSlot`; item `mediaId`s are rewritten through the remap table,
  `path` refreshed from the local record, unresolved refs flagged `extra.unresolvedMedia = true`.
  With `replace:true`, local slots of the bundle's channels that the bundle does not carry are deleted.
* **settings / cg / spots / programming** — `setSetting` per key, local-only keys skipped with a warning.

Report: `{dryRun, groups, applied:{group:n}, remapped:{src:local}, unresolvedMedia:[{id,title,hash,path,size}],
scheduleRewrittenRefs, scheduleFlaggedItems, scheduleRemovedSlots, warnings:[], errors:[], ok,
createdChannels:[ids new to this node], restartChannels:[ids whose frame format changed]}`. An error report
(unknown group) has `ok:false` and no `applied`: nothing was written.
After a real apply that wrote something — `/sessions/{id}/apply`, `/sessions/apply-bundle` **and** a
replication cycle that carried `channels` — the Supervisor reloads every executor (they cache each channel's
profile), spawns an engine for each channel the apply created (disarmed, no outputs), and restarts the
engine of a running channel whose frame format changed (the `PUT /channels/{id}` rule). A dry run or an
error report touches nothing.

### Node identity

`node.id` (uuid, generated once) and `node.name` (default: machine host name) in settings.

### REST (`/api/v1`, bearer token like everything else)

| Method | Path | Body / query → response |
|---|---|---|
| GET | `/sessions` | → `{sessions:[index rows], node, groups, dir}` |
| POST | `/sessions/capture` | `{name, notes}` → index row |
| GET | `/sessions/{id}` | `?data=1` for the full bundle → row or wire |
| PUT | `/sessions/{id}` | `{name?, notes?}` → row |
| DELETE | `/sessions/{id}` | → `{deleted}` |
| POST | `/sessions/{id}/recapture` | → row (version+1) |
| POST | `/sessions/{id}/clone` | `{name}` → new row |
| GET | `/sessions/{id}/export` | → full wire JSON |
| GET | `/sessions/export/{id}` | raw download, `Content-Disposition: attachment; filename=session-<slug>.ppsession.json` |
| POST | `/sessions/import` | `{name, bundle}` (or `data`, or the bundle itself) → row |
| POST | `/sessions/{id}/apply` | `{groups:[…], dryRun, replace, armFromBundle}` → report |
| POST | `/sessions/apply-bundle` | `{bundle, groups, dryRun, replace}` → report (no stored session) |
| POST | `/sessions/receive` | wire JSON (from a peer push) → `{action: created|updated|skipped|conflict, id, version, reason?}` |
| GET | `/sessions/export-live` | `?groups=a,b&since=<marks json>&secrets=1` → bundle of the changed groups + `marks`, `exported`, `secrets` (see credential settings above) |
| GET | `/sessions/marks` | → current marks |

Groups accepted by apply: `channels, outputs, schedule, media, settings, cg, spots, programming`
(unknown names are an error, never "apply everything").

### Marks (change detection for replication)

`{"channels":"<n>:<md5-16>", "schedule":"<n>:<max updated_at>", "media":"<n>:<md5-16>",
"settings|cg|spots|programming":"<n>:<md5-16 of key=value>", "revision": <store revision>}`.
`export-live` omits a group whose mark equals the one in `since`.

---

## 2. Remote nodes

Settings `remote.nodes` = `{nodes:[{id, name, url, token}]}` — `token` is the other node's `rest.token`.

| Method | Path | Notes |
|---|---|---|
| GET / PUT | `/remote/nodes` | PUT `{nodes:[…]}` assigns ids, normalises URLs |
| GET | `/remote/nodes/{id}/status` | async: `<url>/api/v1/status` + `/failover/status` → `{reachable, latencyMs, role, epoch, channels:[{id,name,armed,state,title}], health, uptimeMs, error}` |
| GET | `/remote/overview` | async fan-out of the above for all nodes, replies when all answered or after 5 s (`timeout` rows marked unreachable) |
| POST | `/remote/nodes/{id}/proxy` | `{method, path, body}` → forwarded to `<url>/api/v1<path>` with the node's token; whitelisted prefixes `/channels /schedule /status /media /sessions /failover /asrun /reports /spots /mux /library`, anything else → 403; reply carries the upstream status + JSON |
| POST | `/remote/nodes/{id}/push-session/{sid}` | POSTs our session's wire JSON to the node's `/sessions/receive` |
| POST | `/remote/nodes/{id}/pull-session/{remoteSid}` | GETs `<url>/api/v1/sessions/{remoteSid}?data=1` and `receiveBundle`s it (`remoteSid` must be a valid session id, else 400) |

A **401/403 from the node** (it rejected the token stored for it) is answered by proxy / push / pull as
**502** with `{error: "<proxy|push|pull>: the node rejected our token (HTTP 401: …) — correct its token under Remote > Nodes"}`.
It is never forwarded as this Supervisor's own 401, which the dashboard would take for a local login failure
(it opens the local token dialog, which cannot fix a peer's token).

The dashboard's Nodes table offers Open dashboard, per-channel arm/disarm, load hour, snap (via the
proxy), Push session (picker) and Pull… (lists the node's sessions through the proxy).

---

## 3. Failover pair

Settings key `failover.config`:

```json
{ "role": "standalone|primary|backup", "peerUrl": "http://192.168.1.50:7700", "peerToken": "<peer rest.token>",
  "intervalSec": 5, "graceSec": 30, "auto": true, "replicate": true, "replicateIntervalSec": 60, "pullMedia": false }
```
Runtime: `failover.epoch` (unix ms of the last promotion). History ring of 50 events in memory.

### Heartbeat

Every `intervalSec` the node GETs `<peerUrl>/api/v1/failover/heartbeat` (bearer `peerToken`, 4 s timeout).
The probe answers `{nodeId, nodeName, role, epoch, timeMs, armedChannels:[…], onAir:{ch:{state,title,connected,armed}}}`.

* **Backup**: silence > `graceSec` → with `auto` → `promote("peer silent N s")`: role primary,
  epoch = now, every channel armed (`ScheduleExecutor::setArmed` + `profile.armed` persisted), critical
  alert; without `auto` → one critical alert. A backup only counts silence after it has seen the peer
  **once** since boot (a mis-typed URL must not arm a standby); until then it warns "never seen".
* **Primary**: peer also primary with a **newer** epoch (tie → higher nodeId wins) → `demote`: all
  channels disarmed **and playout stopped** (engines go to slate: disarming alone only stops the executor
  pushing, the engines would keep airing the hour(s) already pushed next to the new primary), role backup,
  replication marks reset for a full re-sync.
* `401/403` from the peer = peer alive, our token wrong → no silence counting, error alert once.
* Both backup → critical alert ("nobody drives playout"), no automatic action.
* A node booting as backup disarms every channel before the first heartbeat.
* Setting role to backup through the API disarms immediately and stops playout (like demote). Setting
  **primary on a backup is a takeover and goes through `promote()`**: epoch = now, every channel armed, critical
  alert (otherwise the old primary would yield to the newer epoch while this node aired nothing). standalone → primary
  leaves arming as is (and seeds the epoch if it was 0); standalone idles the timer.
* `PUT /failover/config` is a partial update. A client changing `role` should send `expectedRole` (the role it
  last saw): if the node holds another role now (auto-promote, a newer epoch) the answer is **409** and nothing
  changes. Both UIs save only the keys the operator edited (never the whole cached form, so a stale role
  cannot revert a promotion) and ask for confirmation before a role change.

### Replication (backup only)

Every `replicateIntervalSec` the backup GETs `export-live?groups=channels,schedule,media,settings,cg,spots,programming&since=<peer marks>&secrets=1`
and applies the result with `replace` (schedule), `keepOutputs`, `keepArmed`, `mirror` (media keep the primary's ids);
afterwards it disarms all channels again (belt and braces). When the `channels` group was applied, the
Supervisor reloads the executors (they cache each profile; without that a later promote would save the
boot-time copy over the replicated one and air on stale fill / identity / timezone) and spawns engines for
channels the primary gained since boot. `promote()` also re-reads each executor's profile before arming it.
The marks returned by the primary are sent back as `since` next time,
so unchanged groups cost nothing. A promotion during a request in flight discards the reply.

Unresolved media (files the backup lacks) are listed in `GET /failover/status → replication.unresolvedMedia`.
The list is rebuilt by a cycle that carried the `media` group, merged by a schedule-only cycle, and left alone
by cycles that carried neither (cg, settings, spots, programming, channels), so it no longer empties itself
on an unrelated change while the records are still missing.
With `pullMedia` they are downloaded from `<peerUrl>/api/v1/download/<id>` into
`<dataDir>/media/replica/<id12>_<basename>` (3 in flight, failed ids retried after 10 min) and the
record is relinked (`extra.replicaOf = peerUrl`, `missing` cleared).

### REST

| Method | Path | Body → response |
|---|---|---|
| GET | `/failover/status` | `{role, nodeId, nodeName, epoch, config, active, peer:{url,reachable,lastOkMs,silentSec,error,missed,role,epoch,nodeId,nodeName,state}, replication:{enabled,lastOkMs,lastGroups,peerMarks,lastReport,unresolvedMedia,pulling,pulled}, armedChannels, historyCount}` |
| PUT (or POST) | `/failover/config` | partial `{role, peerUrl, peerToken, intervalSec, graceSec, auto, replicate, replicateIntervalSec, pullMedia, expectedRole?}` → status (409 when `expectedRole` is not the node's current role; backup→primary = promote) |
| GET | `/failover/heartbeat` | the probe peers call |
| POST | `/failover/promote` | `{reason}` → `{role, epoch, reason}` |
| POST | `/failover/demote` | `{reason}` → `{role, reason}` |
| GET | `/failover/history` | `{history:[{timeMs, event, detail, level, role}]}` |

### WebSocket

Event `redundancy`: `{type:"redundancy", role, epoch, peer:{url, reachable, role, epoch, error}, lastHeartbeat,
lastReplicatedMs, sessions}` on every heartbeat transition, replication, config change, role change
and sessions registry change. Alerts come through the normal `alert` event with `channelId:"failover"`.

`FailoverPair::standby()` is true while the role is backup — generation and analysis workers can read
it through the Supervisor to hold off heavy work on a warm standby.

---

## 4. Pairing two Supervisors (loopback test or two machines)

1. On both nodes set `rest.token` (settings) and note each other's URL.
2. Primary: Remote tab → Failover card → role **primary**, peer URL = backup, peer token = backup's `rest.token`, Save.
3. Backup: role **backup**, peer URL = primary, peer token = primary's `rest.token`, Save. Its channels disarm;
   within `replicateIntervalSec` it is a clone (outputs stay its own — configure them once).
4. Watch the heartbeat age / history on either side. Stop the primary: after `graceSec` the backup
   promotes (critical alert) and takes air at the wall clock. Restart the old primary: it sees the newer
   epoch and demotes itself.
5. Second Supervisor on one machine: start it with another `dataDir`, REST and WS ports and IPC name,
   point both at `http://127.0.0.1:<port>`.
