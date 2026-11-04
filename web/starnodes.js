/* PLAY PLAY web dashboard — Star page (docs/star-nodes.md): the Star node registry (enrol / edit / probe / remove),
   the allowlisted remote action runner, config, the action audit and the station view (/api/stations, outside
   /api/v1). All state lives in SN (window.SNS); every input writes into SN as it is typed. */

const SN = {
  loaded: false, loading: false, nodes: [], config: {}, cfg: null, cfgDirty: false, allow: { actions: [], fleetCommands: [], stationPaths: [] }, audit: [],
  enrol: { name: "", execUrl: "", host: "", token: "", stateUrl: "", playplayUrl: "", playplayPass: "", roles: "", notes: "", force: false }, enrolOpen: false, enrolBusy: false,
  edit: null, act: { node: "", action: "fleet.status", args: {} }, result: null, actBusy: false,
  stations: [], stationsErr: "", caps: {}, capsBusy: {},
};
window.SNS = SN;
const esc = s => String(s ?? "").replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
const js = v => esc(JSON.stringify(String(v ?? "")));
const bdg = (text, color) => `<span class="badge" style="color:${color};border-color:${color};background:color-mix(in srgb,${color} 16%,transparent)">${esc(text)}</span>`;
const okb = (ok, label) => ok === undefined || ok === null ? bdg(label + " ?", "var(--t3)") : bdg(label, ok ? "var(--ready)" : "var(--danger)");
const when = ms => ms ? new Date(ms).toLocaleString([], { month: "short", day: "numeric", hour: "numeric", minute: "2-digit", second: "2-digit" }) : "—";
const opts = (list, cur) => list.map(o => { const v = Array.isArray(o) ? o[0] : o, l = Array.isArray(o) ? o[1] : o; return `<option value="${esc(v)}" ${String(v) === String(cur) ? "selected" : ""}>${esc(l)}</option>`; }).join("");
const pretty = v => { if (v === undefined) return ""; if (typeof v === "string") { try { return JSON.stringify(JSON.parse(v), null, 2); } catch { return v; } } return JSON.stringify(v, null, 2); };
const SERVICES = ["playplay-server", "mediamtx", "obs-ch1", "obs-ch2", "obs-ch3", "obs-ch4"];
const DEFAULT_ACTIONS = ["fleet.status", "fleet.analytics", "fleet.query", "fleet.asPlayed", "sync.jobs", "sync.run", "swarm.index", "browse", "state", "service.restart", "station.status", "station.capabilities", "station.call", "exec"];
const DEFAULT_FLEET = ["on_air", "system_health", "as_played_today", "run_report", "adherence", "schedule_hour", "upcoming", "channel_list"];
// Station-compatible routes live outside /api/v1: same bearer token as app.js's api() helper.
async function rawGet(path) {
  const r = await fetch(path, { headers: pp.S.token ? { Authorization: "Bearer " + pp.S.token } : {} });
  const t = await r.text(); let d; try { d = JSON.parse(t); } catch { d = t; }
  if (!r.ok) throw new Error((d && d.error) || ("GET " + path + " → " + r.status));
  return d;
}
// The args each allowlisted action takes (docs/star-nodes.md).
const ARGS = {
  "fleet.query": [["stationId", "text", "station id on the node"], ["command", "fleet", ""], ["args", "json", "{}"]],
  "fleet.asPlayed": [["date", "date", ""]],
  "sync.run": [["id", "text", "sync job id"]],
  "browse": [["path", "text", "D:\\media"]],
  "service.restart": [["service", "service", ""]],
  "station.call": [["method", "method", ""], ["path", "text", "/api/schedule/hour?..."], ["body", "json", "{}"]],
  "exec": [["command", "area", "Get-Process obs64"], ["shell", "shell", ""]],
};

async function loadStar() {
  if (SN.loading) return; SN.loading = true;
  try {
    const [n, a] = await Promise.all([pp.GET("/starnodes").catch(() => ({})), pp.GET("/starnodes/audit").catch(() => ({}))]);
    SN.nodes = n.nodes || []; SN.config = n.config || {}; if (!SN.cfgDirty) SN.cfg = JSON.parse(JSON.stringify(SN.config));
    SN.allow = Object.assign({ actions: [], fleetCommands: [], stationPaths: [] }, n.allow || {}); SN.audit = a.audit || []; SN.loaded = true;
    if (!SN.act.node && SN.nodes.length) SN.act.node = SN.nodes[0].id;
  } finally { SN.loading = false; }
  pp.render(); loadStations();
}
function loadStations() {
  rawGet("/api/stations").then(d => { SN.stations = Array.isArray(d) ? d : (d.stations || d.items || []); SN.stationsErr = ""; pp.render(); })
    .catch(e => { SN.stationsErr = e.message || String(e); pp.render(); });
}
const loadAudit = () => pp.GET("/starnodes/audit").then(a => { SN.audit = a.audit || []; pp.render(); }).catch(() => {});
let reloadT = 0; const reloadSoon = () => { clearTimeout(reloadT); reloadT = setTimeout(loadStar, 400); };

export function onEnterStar(v) { if (v === "star") loadStar(); }
export function onWsStar(m) {
  if (m.type !== "starnodes") return false;
  if (Array.isArray(m.nodes)) SN.nodes = m.nodes; else if (m.node && m.node.id) { const i = SN.nodes.findIndex(x => x.id === m.node.id); if (i >= 0) SN.nodes[i] = m.node; else SN.nodes.push(m.node); }
  else if (pp.S.view === "star") reloadSoon();
  return true;
}

// ---------------------------------------------------------------------------------------------
export function viewStar() {
  if (!SN.loaded) loadStar();
  const online = SN.nodes.filter(n => nodeOk(n)).length;
  const head = `<div class="panel"><h3>Star nodes <span class="right">${bdg(online + "/" + SN.nodes.length + " online", online ? "var(--ready)" : "var(--t3)")}${SN.config.allowExec ? bdg("exec allowed", "var(--danger)") : bdg("exec off", "var(--t3)")}
      <button class="ghost small" onclick="pp.starProbe()">Probe all</button><button class="small" onclick="SNS.enrolOpen=!SNS.enrolOpen;pp.render()">+ enrol node</button><button class="ghost small" onclick="pp.starReload()">&#8635;</button></span></h3>
    ${SN.enrolOpen ? viewEnrol() : ""}${viewNodes()}${SN.edit ? viewEdit() : ""}</div>`;
  return `<div class="grid" style="gap:12px">${head}<div class="grid cols2">${viewAction()}<div class="grid" style="gap:12px;align-content:start">${viewConfig()}${viewStations()}</div></div>${viewAudit()}</div>`;
}
function nodeOk(n) { const h = n.health || {}; return n.online ?? h.ok ?? (h.exec && h.exec.ok); }
function healthChips(n) {
  const h = n.health || {}; const parts = [];
  for (const k of ["exec", "watchdog", "playplay"]) { const v = h[k]; if (v === undefined) continue; const ok = typeof v === "object" && v ? (v.ok ?? v.online) : !!v; parts.push(`<span title="${esc(typeof v === "object" && v ? (v.error || v.reason || (v.ms !== undefined ? v.ms + " ms" : "")) : "")}">${okb(ok, k)}</span>`); }
  if (!parts.length) parts.push(okb(nodeOk(n), n.online === false ? "offline" : "online"));
  const err = h.error || h.reason || ""; const seen = h.checkedMs || h.atMs || n.lastSeenMs;
  return parts.join(" ") + (err ? `<div style="color:var(--danger);font-size:10px" title="${esc(err)}">${esc(String(err).slice(0, 70))}</div>` : "") + (seen ? `<div style="color:var(--t3);font-size:10px">${when(seen)}</div>` : "");
}
function viewNodes() {
  const rows = SN.nodes.map(n => `<tr><td><b>${esc(n.name || n.id)}</b><div class="mono" style="color:var(--t3);font-size:10px">${esc(n.id)}</div></td>
    <td class="mono" style="font-size:11px">${esc(n.execUrl || n.host || "")}${n.hasToken ? " &#128273;" : ""}${n.stateUrl ? `<div style="color:var(--t3)">state ${esc(n.stateUrl)}</div>` : ""}${n.playplayUrl ? `<div style="color:var(--t3)">pp ${esc(n.playplayUrl)}${n.hasPlayplayPass ? " &#128273;" : ""}</div>` : ""}</td>
    <td>${(n.roles || []).map(r => bdg(r, "var(--accent2)")).join(" ")}${n.station && n.station.kind ? " " + bdg(n.station.kind + (n.station.channel ? " " + n.station.channel : ""), "var(--accent)") : ""}</td>
    <td>${n.enabled !== false ? bdg("on", "var(--ready)") : bdg("off", "var(--t3)")} ${n.render ? bdg("render", "var(--accent)") : ""} ${n.allowExec ? bdg("exec", "var(--danger)") : ""}</td>
    <td style="white-space:normal">${healthChips(n)}</td>
    <td class="row" style="flex-wrap:nowrap"><button class="ghost small" onclick="pp.starProbe(${js(n.id)})">probe</button><button class="ghost small" onclick="SNS.act.node=${js(n.id)};pp.render()">act</button><button class="ghost small" onclick="pp.starEdit(${js(n.id)})">&#9998;</button><button class="ghost small" onclick="pp.starRemove(${js(n.id)})">&#10005;</button></td></tr>`).join("");
  return `<div class="scroll" style="max-height:40vh"><table><thead><tr><th>Node</th><th>Endpoints</th><th>Roles</th><th>Flags</th><th>Health</th><th></th></tr></thead><tbody>${rows || `<tr><td colspan="6" class="empty">No Star nodes enrolled. Enrolment verifies the node's exec API (port 8898) with its x-agent-auth token before it is stored.</td></tr>`}</tbody></table></div>`;
}
function nodeForm(o, path, isEdit) {
  const b = k => `${path}.${k}`;
  return `<div class="grid cols4" style="gap:8px">
    <label class="f">Name <input value="${esc(o.name)}" oninput="${b("name")}=this.value"></label>
    <label class="f">Exec URL <input value="${esc(o.execUrl)}" placeholder="http://host:8898" oninput="${b("execUrl")}=this.value"></label>
    <label class="f">or host <input value="${esc(o.host)}" placeholder="hostname / IP" oninput="${b("host")}=this.value"></label>
    <label class="f">Token (x-agent-auth) <input type="password" autocomplete="new-password" value="${esc(o.token)}" placeholder="${isEdit && o.hasToken ? "•••••• (blank keeps it)" : "required"}" oninput="${b("token")}=this.value"></label>
    <label class="f">State URL (watchdog) <input value="${esc(o.stateUrl)}" placeholder="http://127.0.0.1:8125" oninput="${b("stateUrl")}=this.value"></label>
    <label class="f">PlayPlay URL <input value="${esc(o.playplayUrl)}" placeholder="http://host:7700" oninput="${b("playplayUrl")}=this.value"></label>
    <label class="f">PlayPlay pass <input type="password" autocomplete="new-password" value="${esc(o.playplayPass)}" placeholder="${isEdit && o.hasPlayplayPass ? "•••••• (blank keeps it)" : "optional"}" oninput="${b("playplayPass")}=this.value"></label>
    <label class="f">Roles (comma) <input value="${esc(o.roles)}" placeholder="render, ingest" oninput="${b("roles")}=this.value"></label>
    <label class="f" style="grid-column:span 2">Notes <input value="${esc(o.notes)}" oninput="${b("notes")}=this.value"></label>
    ${isEdit ? `<label class="f">Station kind <select onchange="${b("stationKind")}=this.value">${opts([["", "(none)"], "playplay", "cpp", "masterplay"], o.stationKind)}</select></label><label class="f">Station channel <input value="${esc(o.stationChannel)}" oninput="${b("stationChannel")}=this.value"></label>` : ""}</div>`;
}
function viewEnrol() {
  const e = SN.enrol;
  return `<div class="body" style="border-bottom:1px solid var(--b0)"><div class="fh">ENROL A STAR NODE</div>${nodeForm(e, "SNS.enrol", false)}
    <div class="row" style="margin-top:8px"><label><input type="checkbox" ${e.force ? "checked" : ""} onchange="SNS.enrol.force=this.checked"> force (store as offline when unreachable)</label><span class="fill"></span>
      <button class="success" ${SN.enrolBusy ? "disabled" : ""} onclick="pp.starEnrol()">${SN.enrolBusy ? "Verifying…" : "VERIFY + ENROL"}</button><button class="ghost" onclick="SNS.enrolOpen=false;pp.render()">cancel</button></div></div>`;
}
function viewEdit() {
  const e = SN.edit;
  return `<div class="body" style="border-top:1px solid var(--b0)"><div class="fh">EDIT NODE ${esc(e.id)}</div>${nodeForm(e, "SNS.edit", true)}
    <div class="row" style="margin-top:8px"><label><input type="checkbox" ${e.enabled ? "checked" : ""} onchange="SNS.edit.enabled=this.checked"> enabled</label><label><input type="checkbox" ${e.render ? "checked" : ""} onchange="SNS.edit.render=this.checked"> render</label>
      <label style="color:var(--danger)"><input type="checkbox" ${e.allowExec ? "checked" : ""} onchange="SNS.edit.allowExec=this.checked"> allow exec on this node</label><span class="fill"></span>
      <button class="success" onclick="pp.starSave()">SAVE</button><button class="ghost" onclick="SNS.edit=null;pp.render()">cancel</button></div></div>`;
}

function argField(action, [k, type, ph]) {
  const v = SN.act.args[k] ?? ""; const b = `SNS.act.args[${js(k)}]`;
  if (type === "fleet") { const list = SN.allow.fleetCommands.length ? SN.allow.fleetCommands : DEFAULT_FLEET; return `<label class="f">${esc(k)} <select onchange="${b}=this.value">${opts([["", "(pick)"]].concat(list), v)}</select></label>`; }
  if (type === "service") return `<label class="f">${esc(k)} <select onchange="${b}=this.value">${opts([["", "(pick)"]].concat(SERVICES), v)}</select></label>`;
  if (type === "method") return `<label class="f">${esc(k)} <select onchange="${b}=this.value">${opts(["GET", "POST", "PUT", "PATCH"], v || "GET")}</select></label>`;
  if (type === "shell") return `<label class="f">${esc(k)} <select onchange="${b}=this.value">${opts([["", "default"], "powershell", "cmd", "bash"], v)}</select></label>`;
  if (type === "json" || type === "area") return `<label class="f" style="grid-column:1/-1">${esc(k)}${type === "json" ? " (JSON)" : ""} <textarea rows="${type === "json" ? 3 : 2}" class="mono" placeholder="${esc(ph)}" oninput="${b}=this.value">${esc(v)}</textarea></label>`;
  return `<label class="f">${esc(k)} <input type="${type === "date" ? "date" : "text"}" value="${esc(v)}" placeholder="${esc(ph)}" oninput="${b}=this.value"></label>`;
}
function viewAction() {
  const a = SN.act; const actions = SN.allow.actions.length ? SN.allow.actions : DEFAULT_ACTIONS; const fields = ARGS[a.action] || [];
  const node = SN.nodes.find(n => n.id === a.node);
  const warn = a.action === "exec" ? `<div class="alert error" style="grid-column:1/-1">&#9888; exec runs a shell command on the node. It runs only when this node has "allow exec" <b>and</b> the Star config allows exec; every run is audited.${node && !node.allowExec ? " This node does not allow exec." : ""}${!SN.config.allowExec ? " Exec is off in the config." : ""}</div>`
    : a.action === "service.restart" ? `<div class="alert warning" style="grid-column:1/-1">Restarts a service through the node's watchdog; anything it plays stops while it restarts.</div>`
    : a.action === "station.call" ? `<div style="grid-column:1/-1;color:var(--t3);font-size:11px">Allowed station paths: ${esc((SN.allow.stationPaths || []).join(" · ") || "(see docs/star-nodes.md)")}. Never DELETE, /api/obs, settings, nodes, failover or sessions.</div>` : "";
  const r = SN.result;
  return `<div class="panel"><h3>Remote action</h3><div class="body grid cols2" style="gap:8px">
    <label class="f">Node <select onchange="SNS.act.node=this.value">${opts([["", "(pick a node)"]].concat(SN.nodes.map(n => [n.id, n.name || n.id])), a.node)}</select></label>
    <label class="f">Action <select onchange="SNS.act.action=this.value;SNS.act.args={};pp.render()">${opts(actions, a.action)}</select></label>
    ${warn}${fields.map(f => argField(a.action, f)).join("")}
    <div class="row" style="grid-column:1/-1"><button class="${a.action === "exec" ? "danger" : "primary"}" ${SN.actBusy ? "disabled" : ""} onclick="pp.starRun()">${SN.actBusy ? "Running…" : "Run " + esc(a.action)}</button>${r ? `<span style="color:var(--t3);font-size:11px">${esc(r.label)} · ${r.ok ? "ok" : "failed"}${r.ms !== undefined ? " · " + r.ms + " ms" : ""}</span>` : ""}</div>
    ${r ? `<pre class="mono" style="grid-column:1/-1;margin:0;max-height:40vh;overflow:auto;background:var(--bg1);border:1px solid var(--b0);border-radius:5px;padding:8px;font-size:11px;white-space:pre-wrap;color:${r.ok ? "var(--t0)" : "var(--danger)"}">${esc(r.text)}</pre>` : ""}</div></div>`;
}
function viewConfig() {
  const c = SN.cfg || {};
  return `<div class="panel"><h3>Star config ${SN.cfgDirty ? bdg("unsaved", "var(--warn)") : ""}</h3><div class="body grid cols2" style="gap:8px">
    <label class="f">Exec <label style="text-transform:none;color:var(--danger)"><input type="checkbox" ${c.allowExec ? "checked" : ""} onchange="SNS.cfg.allowExec=this.checked;SNS.cfgDirty=true;pp.render()"> allow exec on nodes that allow it</label></label>
    <label class="f">Poll (s) <input type="number" min="10" value="${esc(c.pollSec ?? 60)}" oninput="SNS.cfg.pollSec=parseInt(this.value)||60;SNS.cfgDirty=true"></label>
    <div class="row" style="grid-column:1/-1"><button class="success small" ${SN.cfgDirty ? "" : "disabled"} onclick="pp.starCfgSave()">SAVE</button><button class="ghost small" ${SN.cfgDirty ? "" : "disabled"} onclick="SNS.cfgDirty=false;SNS.cfg=JSON.parse(JSON.stringify(SNS.config));pp.render()">revert</button></div></div></div>`;
}
function viewStations() {
  const rows = SN.stations.map(s => { const sid = s.id || s.sid || s.name; const cap = SN.caps[sid];
    return `<tr><td><b>${esc(s.name || sid)}</b><div class="mono" style="color:var(--t3);font-size:10px">${esc(sid)}${s.url ? " · " + esc(s.url) : ""}</div></td><td>${okb(s.online ?? s.ok, s.online === false ? "offline" : "online")}</td><td>${esc(s.kind || s.type || "")}</td>
      <td><button class="ghost small" ${SN.capsBusy[sid] ? "disabled" : ""} onclick="pp.starCaps(${js(sid)})">capabilities</button></td></tr>${cap ? `<tr><td colspan="4"><pre class="mono" style="margin:0;max-height:24vh;overflow:auto;font-size:10px;white-space:pre-wrap">${esc(cap)}</pre></td></tr>` : ""}`; }).join("");
  return `<div class="panel"><h3>Stations <span class="right"><button class="ghost small" onclick="pp.starStations()">&#8635;</button></span></h3>
    ${SN.stationsErr ? `<div class="body" style="color:var(--danger)">${esc(SN.stationsErr)}</div>` : ""}<div class="scroll" style="max-height:34vh"><table><thead><tr><th>Station</th><th>State</th><th>Kind</th><th></th></tr></thead><tbody>${rows || `<tr><td colspan="4" class="empty">No stations.</td></tr>`}</tbody></table></div></div>`;
}
function viewAudit() {
  const rows = SN.audit.slice(0, 200).map(a => `<tr><td class="mono" style="color:var(--t3)">${when(a.ms || a.atMs || a.timeMs)}</td><td>${esc(a.node || a.nodeId || "")}</td><td class="mono">${esc(a.action || "")}</td><td>${a.ok === false ? bdg("failed", "var(--danger)") : a.ok ? bdg("ok", "var(--ready)") : bdg(a.status ?? "?", "var(--t3)")}</td>
    <td>${esc(a.actor || a.user || "")}</td><td style="max-width:520px" title="${esc(a.error || pretty(a.args) || "")}">${esc(a.error || (a.args ? JSON.stringify(a.args) : "") || a.note || "")}</td></tr>`).join("");
  return `<div class="panel"><h3>Action audit <span class="right"><button class="ghost small" onclick="pp.starAudit()">&#8635;</button></span></h3><div class="scroll" style="max-height:34vh"><table><thead><tr><th>When</th><th>Node</th><th>Action</th><th>Result</th><th>Actor</th><th>Detail</th></tr></thead><tbody>${rows || `<tr><td colspan="6" class="empty">No actions yet.</td></tr>`}</tbody></table></div></div>`;
}

const splitList = s => String(s || "").split(",").map(x => x.trim()).filter(Boolean);
function nodeBody(o, isEdit) {
  const b = { name: o.name.trim(), stateUrl: o.stateUrl.trim(), playplayUrl: o.playplayUrl.trim(), roles: splitList(o.roles), notes: o.notes };
  if (o.execUrl.trim()) b.execUrl = o.execUrl.trim(); if (o.host.trim()) b.host = o.host.trim();
  if (o.token) b.token = o.token; if (o.playplayPass) b.playplayPass = o.playplayPass;          // blank = keep the stored secret
  if (isEdit) { b.enabled = !!o.enabled; b.render = !!o.render; b.allowExec = !!o.allowExec; b.station = o.stationKind ? { kind: o.stationKind, channel: o.stationChannel || "" } : {}; }
  return b;
}
// ---------------------------------------------------------------------------------------------
export const ppExtStar = {
  starReload() { loadStar(); },
  starStations() { loadStations(); },
  starAudit() { loadAudit(); },
  starProbe(id) { pp.toast(id ? "Probing " + id + "…" : "Probing all Star nodes…", "ok"); pp.POST(id ? "/starnodes/" + encodeURIComponent(id) + "/probe" : "/starnodes/probe").then(() => loadStar()).catch(() => loadStar()); },
  starEnrol() {
    const e = SN.enrol; if (!e.execUrl.trim() && !e.host.trim()) { pp.toast("Exec URL or host is required", "err"); return; } if (!e.token) { pp.toast("The node's token is required", "err"); return; }
    const body = nodeBody(e, false); body.force = !!e.force; SN.enrolBusy = true; pp.render();
    pp.POST("/starnodes/enrol", body).then(r => { pp.toast("Enrolled " + ((r && (r.name || (r.node && r.node.name))) || e.name || "node"), "ok"); SN.enrol = { name: "", execUrl: "", host: "", token: "", stateUrl: "", playplayUrl: "", playplayPass: "", roles: "", notes: "", force: false }; SN.enrolOpen = false; loadStar(); })
      .catch(() => {}).finally(() => { SN.enrolBusy = false; pp.render(); });
  },
  starEdit(id) { const n = SN.nodes.find(x => x.id === id); if (!n) return;
    SN.edit = { id: n.id, name: n.name || "", execUrl: n.execUrl || "", host: n.host || "", token: "", hasToken: !!n.hasToken, stateUrl: n.stateUrl || "", playplayUrl: n.playplayUrl || "", playplayPass: "", hasPlayplayPass: !!n.hasPlayplayPass,
      roles: (n.roles || []).join(", "), notes: n.notes || "", enabled: n.enabled !== false, render: !!n.render, allowExec: !!n.allowExec, stationKind: (n.station && n.station.kind) || "", stationChannel: (n.station && n.station.channel) || "" };
    pp.render(); },
  starSave() { const e = SN.edit; if (!e) return; const go = () => pp.PUT("/starnodes/" + encodeURIComponent(e.id), nodeBody(e, true)).then(() => { SN.edit = null; pp.toast("Node saved", "ok"); loadStar(); }).catch(() => {});
    const was = (SN.nodes.find(n => n.id === e.id) || {}).allowExec; if (e.allowExec && !was) pp.confirmDo("Allow shell exec on " + (e.name || e.id) + "? Commands run there when the Star config also allows exec.", go); else go(); },
  starRemove(id) { const n = SN.nodes.find(x => x.id === id) || {}; pp.confirmDo("Remove Star node \"" + (n.name || id) + "\" from the registry? Its stored token is deleted.", () => pp.api("DELETE", "/starnodes/" + encodeURIComponent(id)).then(() => loadStar()).catch(() => {})); },
  starCfgSave() { const c = SN.cfg; const go = () => pp.PUT("/starnodes/config", { allowExec: !!c.allowExec, pollSec: c.pollSec }).then(r => { SN.cfgDirty = false; if (r && typeof r === "object") { SN.config = r.config || r; SN.cfg = JSON.parse(JSON.stringify(SN.config)); } pp.toast("Star config saved", "ok"); pp.render(); }).catch(() => {});
    if (c.allowExec && !SN.config.allowExec) pp.confirmDo("Turn exec on? Nodes that allow exec will run shell commands sent from here.", go); else go(); },
  starRun() {
    const a = SN.act; if (!a.node) { pp.toast("Pick a node", "err"); return; }
    const args = {}; for (const [k, type] of ARGS[a.action] || []) { let v = a.args[k]; if (v === undefined || v === "") { if (type === "method") v = "GET"; else continue; }
      if (type === "json") { try { v = JSON.parse(v); } catch { pp.toast(k + " is not valid JSON", "err"); return; } } args[k] = v; }
    if (a.action === "exec" && !args.command) { pp.toast("A command is required", "err"); return; }
    if (a.action === "service.restart" && !args.service) { pp.toast("Pick a service", "err"); return; }
    const node = SN.nodes.find(n => n.id === a.node) || {}; const label = a.action + " on " + (node.name || a.node);
    const run = () => { SN.actBusy = true; SN.result = null; pp.render(); const t0 = Date.now();
      pp.POST("/starnodes/" + encodeURIComponent(a.node) + "/action", { action: a.action, args }).then(r => { SN.result = { ok: !(r && r.ok === false), label, ms: (r && r.ms) ?? (Date.now() - t0), text: pretty(r) }; })
        .catch(e => { SN.result = { ok: false, label, text: e.message || String(e) }; }).finally(() => { SN.actBusy = false; pp.render(); loadAudit(); }); };
    if (a.action === "exec") pp.confirmDo("Run this shell command on " + (node.name || a.node) + "?\n\n" + args.command, run);
    else if (a.action === "service.restart") pp.confirmDo("Restart " + args.service + " on " + (node.name || a.node) + "?", run);
    else if (a.action === "station.call" && args.method && args.method !== "GET") pp.confirmDo(args.method + " " + (args.path || "") + " on the station behind " + (node.name || a.node) + "?", run);
    else run();
  },
  starCaps(sid) { SN.capsBusy[sid] = true; pp.render(); rawGet("/api/stations/" + encodeURIComponent(sid) + "/capabilities").then(d => { SN.caps[sid] = pretty(d); }).catch(e => { SN.caps[sid] = "error: " + (e.message || e); pp.toast("Capabilities: " + (e.message || e), "err"); }).finally(() => { SN.capsBusy[sid] = false; pp.render(); }); },
};
