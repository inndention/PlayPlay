# Remote pad

Mappable, themed buttons that drive this station or any remote node (a node is another Supervisor listed under Remote → Nodes,
`remote.nodes`). The pad sits at the top of the Remote page in the web dashboard (`web/remote.js`, `padPanel`) and in the native app
(`Play_Play_Native/qml/ui/RemotePad.qml`, embedded in `RemoteView.qml`). Code: the "remote pad" block in
`playout/supervisor/supervisorcore_redundancy.cpp`; it reuses the remote-node HTTP helper (`FailoverPair::fetchJson`) and the node list.

## Buttons

Settings `remote.pad = {theme, buttons[]}`; a button is
`{id, label, group, node ("local" or a node id), channel, method, path, body, role, confirm, hotkey}`.

- **Call:** `method` + `path` (+ JSON `body`) on the node. `{channel}` in the path is filled from the button's `channel`. Paths are limited to operator
  prefixes: `/channels /schedule /status /spots /live /outputs /overlays /mux /library /inputs /flight`; `..`, `//` and anything else (settings, sessions,
  failover, remote) are dropped when the pad is saved. Local presses call this Supervisor's own REST (with its token); remote presses use the node's URL and token.
- **Colour roles:** `neutral go stop warn info accent`, painted from the pad theme. **Themes** (`studio broadcast night daylight neon`) are served by the
  Supervisor (`themes[]` in `GET /remote/pad`) so the web and native pads look the same.
- **confirm:** web asks "Skip on main?"; native needs a second tap within 3 s (the button shows CONFIRM?).
- **hotkey:** web: a single key or `F1`..`F12` while the Remote page is open and no field has focus; native: `F9`-`F12` or `Ctrl/Alt/Shift+<key>` (F1-F8 and
  Ctrl+C/V/X/L/0/-/Right are page shortcuts).
- **catalog:** `GET /remote/pad` returns ready-made actions (Skip, Play, Pause, Stop, Arm, Disarm, Load hour, Snap to schedule, Restart engine, Go live / End live
  Facebook, Status) to add with one click, plus "Custom call…".

## Routes (`/api/v1`)

| Route | Answer |
|---|---|
| `GET /remote/pad` | `{theme, buttons[], themes[], catalog[], nodes[{id,name}] (first is local), channels[{id,name}]}` |
| `PUT /remote/pad {theme, buttons[]}` | validates and stores (ids made unique, labels ≤ 28 chars, ≤ 96 buttons, bad paths/methods dropped), returns the stored pad |
| `POST /remote/pad/{id}/press` | `{ok, http, ms, response}`; 404 unknown button or node, 409 button needs a channel, 502 target unreachable / token refused, target errors passed through |
| `GET /remote/pad/log` | the last 60 presses `{tMs, button, label, node, method, path, http, ok, ms, error?}` (in memory) |

## Verified (2026-10-25, isolated test Supervisor, the node list pointing at itself)

Default pad empty with 5 themes and 12 catalog actions; a save dropped `/settings/...` and `/status/../settings` and kept the rest; local and remote presses
returned 200 with latency; a `{channel}` button without channel → 409; Facebook start without a channel → 409 from the target; unknown node → 404. Native
app (attached to the test Supervisor with `--supervisor-port/--supervisor-ipc`) renders the pad on Remote and the Facebook Live panel on Output; the web views
render under Node with escaped labels. Not exercised: a press of a real transport action on a live engine, and a second physical node.
