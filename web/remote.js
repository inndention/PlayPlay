/* PLAY PLAY web dashboard — Remote console: other Supervisors (nodes) driven from this UI, station
   state sessions (capture / clone / export / import / apply / push / pull) and the primary/backup
   failover pair (role, peer, heartbeat, replication, promote/demote). Talks to /api/v1/remote/*,
   /api/v1/sessions/*, /api/v1/failover/*. */

const R = {
  // cfg = what the form shows (server config + the operator's edits); cfgKeys = the keys the operator actually edited (only those are saved)
  nodes: [], discovery: null, discoveryPassword: "", overview: null, sessions: [], groups: [], self: {}, fo: null, hist: [], cfg: null, cfgDirty: false, cfgKeys: new Set(),
  nodeEdit: null, pushSel: {}, pull: null, applySid: null, applyGroups: new Set(), applyReplace: false, applyArm: false, applyReport: null, applyBusy: false,
  timer: 0, loading: false,
  pad: { d: null, edit: false, dirty: false, log: [], sel: null, cat: 0, addNode: "local", addCh: "", busy: {} },
};
window.R = R;
const esc = s => String(s ?? "").replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
// A value as a JS string literal for an inline handler inside a double-quoted attribute: JSON.stringify escapes quotes/backslashes for the JS parser,
// esc() escapes them for the HTML parser (which decodes &quot; BEFORE the handler is compiled, so esc() alone does not protect a '...' literal).
// Session / node / channel names and ids can come from a peer or an imported file: never splice them into a handler any other way.
const js = v => esc(JSON.stringify(String(v ?? "")));
const safeUrl = u => /^https?:\/\//i.test(String(u || "").trim()) ? String(u).trim() : "#";   // no javascript: / data: hrefs from a node list entry
const badge = (text, color) => `<span class="badge" style="color:${color};border-color:${color};background:color-mix(in srgb,${color} 16%,transparent)">${esc(text)}</span>`;
const roleColor = r => r === "primary" ? "var(--accent2)" : r === "backup" ? "var(--standby)" : "var(--t2)";
const ago = ms => { if (!ms) return "never"; const s = Math.max(0, Math.round((Date.now() - ms) / 1000)); return s < 60 ? s + " s ago" : s < 3600 ? Math.round(s / 60) + " min ago" : s < 86400 ? (s / 3600).toFixed(1) + " h ago" : Math.round(s / 86400) + " d ago"; };
const when = ms => ms ? new Date(ms).toLocaleString([], { dateStyle: "short", timeStyle: "short" }) : "—";
const tokenQ = () => pp.S.token ? "?token=" + encodeURIComponent(pp.S.token) : "";
const slugName = s => String(s || "session").replace(/[^a-z0-9]+/gi, "-").replace(/^-|-$/g, "").toLowerCase() || "session";
const ALL_GROUPS = ["channels", "outputs", "schedule", "media", "settings", "cg", "spots", "programming"];
const GROUP_LABEL = { channels: "Channels (profiles, identity, fill)", outputs: "Outputs (this node's hardware config!)", schedule: "Schedule (template + dated hours)", media: "Media records", settings: "General settings", cg: "CG (cg.*)", spots: "Spots (spots.*)", programming: "Programming (recipes, traffic, clock)" };

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------
function loadPad() {
  if (R.pad.dirty) return;
  pp.GET("/remote/pad").then(d => { R.pad.d = d; if (!R.pad.addCh && (d.channels || [])[0]) R.pad.addCh = d.channels[0].id; pp.render(); }).catch(() => {});
  pp.GET("/remote/pad/log").then(l => { R.pad.log = (l.log || []).slice(-6).reverse(); pp.render(); }).catch(() => {});
}
async function loadRemote(full) {
  if (R.loading) return; R.loading = true;
  loadPad();
  try {
    const [fo, ses, nodes, discovery] = await Promise.all([pp.GET("/failover/status").catch(() => null), pp.GET("/sessions").catch(() => null), pp.GET("/remote/nodes").catch(() => null), pp.GET("/remote/discovery").catch(() => null)]);
    // Refresh everything from the node EXCEPT the keys being edited: a stale role/peer in the form must never overwrite what the node did meanwhile.
    if (fo) { R.fo = fo; const edited = {}; for (const k of R.cfgKeys) edited[k] = (R.cfg || {})[k]; R.cfg = Object.assign({}, fo.config || {}, edited); }
    if (ses) { R.sessions = ses.sessions || []; R.groups = ses.groups || []; R.self = ses.node || {}; }
    if (nodes) R.nodes = nodes.nodes || [];
    if (discovery) R.discovery = discovery;
    pp.render();
    if (full !== false) {
      pp.GET("/failover/history").then(h => { R.hist = h.history || []; pp.render(); }).catch(() => {});
      if (R.nodes.length) pp.GET("/remote/overview").then(o => { R.overview = o; pp.render(); }).catch(() => {});
      else R.overview = { nodes: [] };
    }
  } finally { R.loading = false; }
}
export function onEnterRemote(v) {
  clearInterval(R.timer); R.timer = 0;
  if (v !== "remote") return;
  loadRemote(true);
  R.timer = setInterval(() => { if (pp.S.view !== "remote") { clearInterval(R.timer); R.timer = 0; return; } loadRemote(true); }, 5000);
}
let wsDebounce;
export function onWsRemote(m) {
  if (m.type !== "redundancy") return false;
  if (R.fo) { R.fo.role = m.role; R.fo.epoch = m.epoch; R.fo.peer = Object.assign({}, R.fo.peer || {}, m.peer || {}, { lastOkMs: m.lastHeartbeat }); }
  if (pp.S.view === "remote") { clearTimeout(wsDebounce); wsDebounce = setTimeout(() => loadRemote(false), 400); }
  return true;
}

// ---------------------------------------------------------------------------
// View
// ---------------------------------------------------------------------------
function failoverCard() {
  const fo = R.fo || {}; const c = R.cfg || fo.config || {}; const peer = fo.peer || {}; const repl = fo.replication || {}; const role = fo.role || "standalone";
  const peerDot = peer.reachable ? `<span class="dot on" style="background:var(--ready)"></span>` : `<span class="dot" style="background:${peer.url ? "var(--danger)" : "var(--t3)"}"></span>`;
  const field = (k, label, w, ph) => `<label class="f">${label}<input value="${esc(c[k] ?? "")}" placeholder="${esc(ph || "")}" style="width:${w}px" onchange="pp.foField('${k}',this.value)"></label>`;
  const check = (k, label) => `<label style="display:flex;gap:6px;align-items:center;font-size:12px"><input type="checkbox" ${c[k] ? "checked" : ""} onchange="pp.foField('${k}',this.checked)">${label}</label>`;
  const hist = (R.hist || []).slice(0, 12).map(h => `<div style="display:flex;gap:8px;font-size:11px"><span class="mono" style="color:var(--t3);white-space:nowrap">${esc(when(h.timeMs))}</span>${badge(h.event, h.level === "critical" ? "var(--danger)" : h.level === "warning" || h.level === "error" ? "var(--warn)" : "var(--t2)")}<span style="color:var(--t1)">${esc(h.detail)}</span></div>`).join("") || `<div class="muted" style="font-size:11px">no events yet</div>`;
  const unresolved = repl.unresolvedMedia || [];
  return `<div class="panel"><h3>Failover pair <span class="right">${badge(role, roleColor(role))} <span class="mono" style="font-size:10px">${esc(fo.nodeName || "")} · ${esc((fo.nodeId || "").slice(0, 8))} · epoch ${esc(fo.epoch || 0)}</span></span></h3>
    <div class="body" style="display:grid;gap:10px">
      <div class="row" style="gap:12px">${peerDot}<span>peer <span class="mono">${esc(peer.url || "— not paired —")}</span></span>
        ${peer.url ? `${peer.role ? badge(peer.role, roleColor(peer.role)) : ""}<span class="muted" style="font-size:11px">${esc(peer.reachable ? "heartbeat " + ago(peer.lastOkMs) + (peer.state && peer.state.armedChannels ? " · armed: " + (peer.state.armedChannels.join(", ") || "none") : "") : (peer.error || "waiting for first heartbeat"))}${peer.silentSec > 0 && !peer.reachable ? " · silent " + Math.round(peer.silentSec) + " s / grace " + (c.graceSec || 30) + " s" : ""}</span>` : ""}
        <span class="fill"></span>
        ${role === "backup" ? `<button class="success small" onclick="pp.foPromote()">⚡ Promote to primary</button>` : role === "primary" ? `<button class="danger small" onclick="pp.foDemote()">⬇ Demote to backup</button>` : ""}
      </div>
      <div class="row" style="gap:10px;align-items:flex-end">
        <label class="f">Role<select onchange="pp.foField('role',this.value)">${["standalone", "primary", "backup"].map(r => `<option ${c.role === r ? "selected" : ""}>${r}</option>`).join("")}</select></label>
        ${field("peerUrl", "Peer URL", 220, "http://192.168.1.50:7700")}${field("peerToken", "Peer token (its rest.token)", 160, "")}
        ${field("intervalSec", "Heartbeat s", 60)}${field("graceSec", "Grace s", 60)}${field("replicateIntervalSec", "Replicate every s", 70)}
        <div style="display:grid;gap:3px">${check("auto", "auto-promote")}${check("replicate", "replicate state")}${check("pullMedia", "pull missing media")}</div>
        <button class="primary small" ${R.cfgDirty ? "" : "disabled"} onclick="pp.foSave()">Save</button>
      </div>
      ${role === "backup" ? `<div class="muted" style="font-size:11px">Replication: ${repl.enabled ? "on" : "off"} · last ${ago(repl.lastOkMs)}${repl.lastGroups && repl.lastGroups.length ? " (" + repl.lastGroups.join(", ") + ")" : ""} · ${unresolved.length} unresolved media${repl.pulling ? " · pulling " + repl.pulling : ""}${repl.pulled ? " · pulled " + repl.pulled : ""}${unresolved.length ? `<div class="scroll" style="max-height:80px;margin-top:4px">${unresolved.slice(0, 20).map(u => `<div class="mono" style="font-size:10px;color:var(--t2)">${esc(u.title || u.id)} <span style="color:var(--t3)">${esc(u.path || "")}</span></div>`).join("")}</div>` : ""}</div>` : ""}
      <div><div class="fh" style="margin-bottom:4px">HISTORY</div><div class="scroll" style="max-height:150px;display:grid;gap:2px">${hist}</div></div>
      <div class="muted" style="font-size:11px">The backup heartbeats the primary every <b>${esc(c.intervalSec || 5)} s</b>; after <b>${esc(c.graceSec || 30)} s</b> of silence it ${c.auto ? "arms every channel and takes air at the wall clock" : "raises a critical alert (auto-promote is off)"}. Two primaries resolve toward the newer promotion epoch. A backup never arms channels and keeps its own <i>outputs</i>; everything else is cloned from the primary.</div>
    </div></div>`;
}

function nodesPanel() {
  const rows = (R.overview && R.overview.nodes) || R.nodes.map(n => Object.assign({}, n, { reachable: undefined }));
  const sesOpts = R.sessions.map(s => `<option value="${esc(s.id)}">${esc(s.name)}</option>`).join("");
  const row = n => {
    const sel = R.pushSel[n.id] || (R.sessions[0] && R.sessions[0].id) || "";
    // channel ids / names come from the node's own /status: every dynamic value reaches the handler through js()
    const px = (ch, act, okMsg) => `pp.remProxy(${js(n.id)},'POST',${js("/channels/" + encodeURIComponent(ch.id) + "/" + act)},${js(okMsg)})`;
    const chans = (n.channels || []).map(ch => `<div class="row" style="gap:6px;font-size:11px"><b>${esc(ch.name || ch.id)}</b>${badge(ch.state || "offline", ch.state === "PLAYING" ? "var(--onair)" : ch.state ? "var(--t2)" : "var(--t3)")}${ch.armed ? badge("armed", "var(--ready)") : badge("disarmed", "var(--t3)")}<span style="color:var(--t1);max-width:220px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap">${esc(ch.title || "")}</span>
      <button class="ghost small" onclick="${px(ch, ch.armed ? "disarm" : "arm", (ch.armed ? "Disarmed " : "Armed ") + ch.id + " on " + n.name)}">${ch.armed ? "disarm" : "arm"}</button>
      <button class="ghost small" onclick="${px(ch, "load-hour", "Hour pushed on " + n.name)}">load hour</button>
      <button class="ghost small" onclick="${px(ch, "snap", "Snapped " + ch.id + " on " + n.name)}">snap</button></div>`).join("") || `<span class="muted" style="font-size:11px">${n.reachable === false ? "" : "no channels"}</span>`;
    return `<tr><td><b>${esc(n.name)}</b><div class="mono" style="font-size:10px;color:var(--t3)">${esc(n.nodeName || "")} ${esc(n.id)}</div></td>
      <td><a href="${esc(safeUrl(n.url))}" target="_blank" rel="noopener" class="mono" style="color:var(--accent);font-size:11px">${esc(n.url)}</a></td>
      <td>${n.reachable === undefined ? badge("…", "var(--t3)") : n.reachable ? badge("online " + (n.latencyMs >= 0 ? n.latencyMs + " ms" : ""), "var(--ready)") : badge("offline", "var(--danger)")}${n.error && !n.reachable ? `<div style="color:var(--t3);font-size:10px">${esc(n.error)}</div>` : ""}</td>
      <td>${n.role ? badge(n.role, roleColor(n.role)) : ""}${n.health && n.health.status ? `<div style="font-size:10px;color:var(--t3)">health ${esc(n.health.status)} ${esc(n.health.score)}</div>` : ""}</td>
      <td style="display:grid;gap:3px">${chans}</td>
      <td><div class="row" style="gap:4px"><button class="small" onclick="window.open(${js(safeUrl(n.url))},'_blank','noopener')">Open dashboard</button>
        ${sesOpts ? `<select class="small" style="max-width:140px" onchange="pp.remPushSel(${js(n.id)},this.value)">${R.sessions.map(s => `<option value="${esc(s.id)}" ${s.id === sel ? "selected" : ""}>${esc(s.name)}</option>`).join("")}</select><button class="small" onclick="pp.remPush(${js(n.id)})">Push session</button>` : ""}
        <button class="small" onclick="pp.remPullList(${js(n.id)})">Pull…</button><button class="ghost small" onclick="pp.remNodeEdit(${js(n.id)})">edit</button><button class="ghost small" onclick="pp.remNodeDel(${js(n.id)},${js(n.name)})">&#10005;</button></div>
        ${R.pull && R.pull.nodeId === n.id ? `<div class="card" style="margin-top:6px;padding:6px"><div class="row" style="font-size:11px"><b>Sessions on ${esc(n.name)}</b><span class="fill"></span><button class="ghost small" onclick="pp.remPullClose()">close</button></div>${(R.pull.sessions || []).map(s => `<div class="row" style="gap:6px;font-size:11px"><span>${esc(s.name)}</span><span class="mono" style="color:var(--t3)">v${esc(s.version || 1)} · ${esc(when(s.updatedAt))}</span><span class="fill"></span><button class="small" onclick="pp.remPull(${js(n.id)},${js(s.id)})">← pull</button></div>`).join("") || `<div class="muted" style="font-size:11px">${esc(R.pull.error || "no sessions there")}</div>`}</div>` : ""}</td></tr>`;
  };
  const e = R.nodeEdit;
  const form = e ? `<div class="row" style="padding:8px 12px;border-top:1px solid var(--b0);gap:8px;align-items:flex-end">
      <label class="f">Name<input value="${esc(e.name || "")}" style="width:140px" onchange="R.nodeEdit.name=this.value"></label>
      <label class="f">IP address<input value="${esc(e.url || "")}" placeholder="192.168.1.50" style="width:240px" onchange="R.nodeEdit.url=this.value"></label>
      <label class="f">Password<input type="password" value="${esc(e.password || "")}" placeholder="${e.hasPassword ? "unchanged" : "station password"}" autocomplete="new-password" style="width:200px" onchange="R.nodeEdit.password=this.value"></label>
      <button class="primary small" onclick="pp.remNodeSave()">${e.id ? "Save node" : "Add node"}</button><button class="ghost small" onclick="R.nodeEdit=null;pp.render()">cancel</button></div>`
    : `<div class="row" style="padding:8px 12px;border-top:1px solid var(--b0)"><button class="small" onclick="pp.remNodeEdit('')">+ node</button><span class="fill"></span><span class="muted" style="font-size:11px">this node: ${esc(R.self.name || "")} <span class="mono">${esc((R.self.id || "").slice(0, 8))}</span> · calls go through POST /api/v1/remote/nodes/{id}/proxy (channels, schedule, media, sessions, failover, reports, spots, mux)</span></div>`;
  const d = R.discovery;
  const auto = d ? `<div class="row" style="padding:8px 12px;border-top:1px solid var(--b0);gap:8px;align-items:flex-end"><label class="f">Shared LAN password<input type="password" autocomplete="new-password" value="${esc(R.discoveryPassword)}" placeholder="${d.hasPassword ? "saved" : "enter on each node"}" onchange="R.discoveryPassword=this.value"></label><button class="small ${d.enabled ? "ghost" : "primary"}" onclick="pp.remDiscovery(${!d.enabled})">${d.enabled ? "Disable auto-connect" : "Enable auto-connect"}</button>${d.enabled ? '<button class="small" onclick="pp.remDiscovery(true)">Update password</button>' : ""}<span class="muted">${d.enabled ? (d.listening ? "Searching the LAN" : "Discovery unavailable: " + esc(d.error || "socket error")) : "Enable on each PLAY PLAY node with the same password"}</span></div>` : "";
  return `<div class="panel"><h3>Nodes <span class="right">${rows.length} configured${R.overview && R.overview.timeMs ? " · polled " + ago(R.overview.timeMs) : ""}<button class="ghost small" onclick="pp.remLoad()">refresh</button></span></h3>
    <div class="scroll" style="max-height:48vh"><table><thead><tr><th>Node</th><th>URL</th><th>Reach</th><th>Role</th><th>Channels</th><th>Actions</th></tr></thead><tbody>${rows.map(row).join("") || `<tr><td colspan="6" class="empty">No remote nodes. Add another Supervisor using its IP address and password to drive it from here, push sessions to it, or pair it as a failover peer.</td></tr>`}</tbody></table></div>${form}${auto}</div>`;
}

function sessionsPanel() {
  const row = s => { const sm = s.summary || {}; const node = s.node || {};
    const chips = [["ch", sm.channels], ["hours", sm.schedule], ["media", sm.media], ["settings", (sm.settings || 0) + (sm.cg || 0) + (sm.spots || 0) + (sm.programming || 0)]].filter(([, n]) => n).map(([l, n]) => badge(n + " " + l, "var(--t2)")).join(" ");
    return `<tr class="${R.applySid === s.id ? "onair" : ""}"><td><b style="cursor:pointer" title="rename" onclick="pp.sesRename(${js(s.id)},${js(s.name)})">${esc(s.name)}</b><div style="font-size:10px;color:var(--t3)">${esc(s.notes || "")} <span class="mono">v${esc(s.version || 1)} · ${esc(s.id)}</span></div></td>
      <td class="mono" style="font-size:11px">${esc(when(s.createdAt))}${s.updatedAt && s.updatedAt !== s.createdAt ? `<div style="color:var(--t3);font-size:10px">upd ${esc(when(s.updatedAt))}</div>` : ""}</td>
      <td>${esc(node.name || "")}<div style="font-size:10px;color:var(--t3)">${esc(s.origin || "")}</div></td><td>${chips}</td>
      <td><div class="row" style="gap:4px"><button class="success small" onclick="pp.sesApplyOpen(${js(s.id)})">Apply…</button><button class="small" onclick="pp.sesClone(${js(s.id)},${js(s.name)})">Clone</button><button class="small" onclick="pp.sesRecapture(${js(s.id)})">Recapture</button>
        <button class="small" title="download session-${esc(slugName(s.name))}.ppsession.json" onclick="window.open(${js("/api/v1/sessions/export/" + encodeURIComponent(s.id) + tokenQ())},'_blank')">Export</button><button class="ghost small" onclick="pp.sesDelete(${js(s.id)},${js(s.name)})">&#10005;</button></div></td></tr>`; };
  let apply = "";
  if (R.applySid) {
    const s = R.sessions.find(x => x.id === R.applySid) || { name: R.applySid };
    const rep = R.applyReport;
    const repText = rep ? JSON.stringify({ dryRun: rep.dryRun, applied: rep.applied, remapped: rep.remapped, unresolvedMedia: rep.unresolvedMedia, scheduleRewrittenRefs: rep.scheduleRewrittenRefs, scheduleFlaggedItems: rep.scheduleFlaggedItems, scheduleRemovedSlots: rep.scheduleRemovedSlots, warnings: rep.warnings, errors: rep.errors }, null, 2) : (R.applyBusy ? "running dry run…" : "");
    apply = `<div class="body" style="border-top:1px solid var(--b0);display:grid;gap:8px">
      <div class="row"><b>Apply "${esc(s.name)}" to THIS station</b><span style="color:var(--warn);font-size:11px">replaces the selected groups here; media, as-run history and local settings (token, bind, node identity, failover, remote nodes) are untouched</span><span class="fill"></span><button class="ghost small" onclick="pp.sesApplyClose()">close</button></div>
      <div style="display:grid;grid-template-columns:repeat(4,1fr);gap:4px">${ALL_GROUPS.map(g => `<label style="display:flex;gap:6px;align-items:center;font-size:12px;${g === "outputs" ? "color:var(--warn)" : ""}"><input type="checkbox" ${R.applyGroups.has(g) ? "checked" : ""} onchange="pp.sesApplyGroup('${g}',this.checked)">${esc(GROUP_LABEL[g])}</label>`).join("")}</div>
      <div class="row" style="gap:14px;font-size:12px"><label style="display:flex;gap:6px;align-items:center"><input type="checkbox" ${R.applyReplace ? "checked" : ""} onchange="pp.sesApplyReplace(this.checked)">replace: delete hours this session does not carry</label><label style="display:flex;gap:6px;align-items:center"><input type="checkbox" ${R.applyArm ? "checked" : ""} onchange="R.applyArm=this.checked">take the armed flag from the session (default: keep local)</label>
        <span class="fill"></span><button class="small" onclick="pp.sesDry()" ${R.applyBusy ? "disabled" : ""}>Dry run</button><button class="success small" onclick="pp.sesApplyNow()" ${R.applyBusy || !rep ? "disabled" : ""}>Apply now</button></div>
      <pre class="mono scroll" style="margin:0;max-height:240px;font-size:11px;white-space:pre-wrap;background:var(--bg1);padding:8px;border-radius:6px">${esc(repText)}</pre></div>`;
  }
  return `<div class="panel"><h3>Sessions <span class="right">${R.sessions.length} stored<button class="small" onclick="pp.sesImport()">⬆ Import file</button><button class="primary small" onclick="pp.sesCapture()">＋ Capture current state</button></span></h3>
    <div class="scroll" style="max-height:40vh"><table><thead><tr><th>Session</th><th>Created</th><th>Node</th><th>Contents</th><th>Actions</th></tr></thead><tbody>${R.sessions.map(row).join("") || `<tr><td colspan="5" class="empty">No sessions yet. Capture bundles channels, every hour of the schedule, media records (without transcripts), CG, spots and programming settings into a portable file you can clone, export, push to another node or restore here.</td></tr>`}</tbody></table></div>${apply}</div>`;
}


// ---------------------------------------------------------------------------
// Remote pad: mappable, themed buttons that call this station or any remote node (docs/remote-pad.md)
// ---------------------------------------------------------------------------
const roleKey = r => ["neutral", "go", "stop", "warn", "info", "accent"].includes(r) ? r : "neutral";
function padPanel() {
  const P = R.pad, d = P.d; if (!d) return `<div class="panel"><h3>Remote pad</h3><div class="empty" style="padding:12px">loading…</div></div>`;
  const th = (d.themes || []).find(t => t.id === d.theme) || (d.themes || [])[0] || {};
  const nodeName = id => ((d.nodes || []).find(n => n.id === id) || {}).name || id;
  const groups = [...new Set((d.buttons || []).map(b => b.group || "Main"))];
  const vars = ["bg", "panel", "border", "text", "muted", "neutral", "go", "stop", "warn", "info", "accent"].map(k => `--pd-${k}:${esc(th[k] || "#888")}`).join(";");
  const btn = b => { const busy = P.busy[b.id]; const col = `var(--pd-${roleKey(b.role)})`;
    return `<button class="padbtn ${P.sel === b.id && P.edit ? "sel" : ""}" style="background:${col}" ${busy ? "disabled" : ""} onclick="pp.padClick(${js(b.id)})" title="${esc(b.method + " " + b.path)} → ${esc(nodeName(b.node))}${b.channel ? " / " + esc(b.channel) : ""}${b.hotkey ? " [" + esc(b.hotkey) + "]" : ""}"><b>${esc(b.label)}</b><small>${esc(nodeName(b.node))}${b.channel ? " · " + esc(b.channel) : ""}${b.hotkey ? " · " + esc(b.hotkey) : ""}</small></button>`; };
  const sel = (d.buttons || []).find(b => b.id === P.sel);
  const opt = (list, cur) => list.map(([v, t]) => `<option value="${esc(v)}" ${v === cur ? "selected" : ""}>${esc(t)}</option>`).join("");
  const editor = !P.edit ? "" : `<div style="margin-top:10px;padding-top:10px;border-top:1px solid var(--pd-border)">
    <div class="row" style="gap:8px;align-items:end;flex-wrap:wrap">
      <label class="f">Add <select id="padCat" onchange="pp.padSet('cat',this.value)">${(d.catalog || []).map((c, i) => `<option value="${i}" ${String(i) === String(P.cat) ? "selected" : ""}>${esc(c.group)}: ${esc(c.label)}</option>`).join("")}<option value="custom" ${P.cat === "custom" ? "selected" : ""}>Custom call…</option></select></label>
      <label class="f">On <select onchange="pp.padSet('addNode',this.value)">${opt((d.nodes || []).map(n => [n.id, n.name]), P.addNode)}</select></label>
      <label class="f">Channel <select onchange="pp.padSet('addCh',this.value)">${opt((d.channels || []).map(c => [c.id, c.name || c.id]), P.addCh)}</select></label>
      <button class="small" onclick="pp.padAdd()">+ Add button</button><span class="fill"></span>
      <label class="f">Theme <select onchange="pp.padTheme(this.value)">${opt((d.themes || []).map(t => [t.id, t.name]), d.theme)}</select></label>
      <button class="primary small" ${P.dirty ? "" : "disabled"} onclick="pp.padSave()">Save pad</button><button class="ghost small" onclick="pp.padCancel()">Cancel</button></div>
    ${sel ? `<div class="row" style="gap:8px;align-items:end;flex-wrap:wrap;margin-top:8px"><b style="font-size:11px">Edit "${esc(sel.label)}"</b>
      <label class="f">Label <input value="${esc(sel.label)}" style="width:130px" oninput="pp.padField('label',this.value)"></label>
      <label class="f">Group <input value="${esc(sel.group || "")}" style="width:90px" oninput="pp.padField('group',this.value)"></label>
      <label class="f">Colour <select onchange="pp.padField('role',this.value,true)">${opt(["neutral", "go", "stop", "warn", "info", "accent"].map(r => [r, r]), sel.role)}</select></label>
      <label class="f">Node <select onchange="pp.padField('node',this.value,true)">${opt((d.nodes || []).map(n => [n.id, n.name]), sel.node)}</select></label>
      <label class="f">Channel <input value="${esc(sel.channel || "")}" style="width:80px" oninput="pp.padField('channel',this.value)"></label>
      <label class="f">Key <input value="${esc(sel.hotkey || "")}" style="width:50px" placeholder="e.g. F5" oninput="pp.padField('hotkey',this.value)"></label>
      <label class="f">Method <select onchange="pp.padField('method',this.value,true)">${opt(["GET", "POST", "PUT", "DELETE"].map(m => [m, m]), sel.method)}</select></label>
      <label class="f">Path <input value="${esc(sel.path)}" style="width:230px" oninput="pp.padField('path',this.value)"></label>
      <label class="f">Body (JSON) <input value="${esc(JSON.stringify(sel.body || {}))}" style="width:200px" oninput="pp.padField('body',this.value)"></label>
      <label style="display:flex;gap:4px;align-items:center;font-size:11px"><input type="checkbox" ${sel.confirm ? "checked" : ""} onchange="pp.padField('confirm',this.checked,true)"> confirm</label>
      <button class="danger small" onclick="pp.padDelete()">Delete</button></div>` : `<div style="color:var(--t3);font-size:11px;margin-top:6px">Click a button to edit it. Paths reach channels, schedule, spots, streaming, overlays, outputs and status on the chosen node.</div>`}</div>`;
  const log = (P.log || []).map(l => `<span style="margin-right:12px;color:${l.ok ? "var(--pd-muted)" : "var(--danger)"}">${l.ok ? "✓" : "✗"} ${esc(l.label)} · ${esc(l.ms)} ms${l.ok ? "" : " · " + esc(l.error || l.http)}</span>`).join("");
  return `<style>.pad{background:var(--pd-bg);color:var(--pd-text);border:1px solid var(--pd-border);border-radius:8px;padding:12px}.pad .grp{font-size:10px;letter-spacing:.08em;text-transform:uppercase;color:var(--pd-muted);margin:8px 0 4px}.pad .keys{display:flex;flex-wrap:wrap;gap:8px}
    .padbtn{min-width:104px;min-height:56px;border:1px solid var(--pd-border);border-radius:6px;color:#fff;display:flex;flex-direction:column;align-items:center;justify-content:center;gap:2px;padding:6px 10px;cursor:pointer;text-shadow:0 1px 2px rgba(0,0,0,.35)}.padbtn small{font-size:9px;opacity:.8}.padbtn:active{transform:translateY(1px)}.padbtn.sel{outline:2px solid var(--pd-accent)}.padbtn[disabled]{opacity:.55}
    .pad .f,.pad label{color:var(--pd-muted)}</style>
    <div class="panel"><h3>Remote pad <span style="font-weight:400;color:var(--t3);font-size:11px">buttons that drive this station or a remote node</span></h3>
    <div class="pad" style="${vars}"><div style="display:flex;align-items:center;gap:8px"><b>${esc(th.name || "")}</b><span class="fill" style="flex:1"></span><button class="small" onclick="pp.padEdit()">${P.edit ? "Done" : "Edit pad"}</button></div>
      ${groups.map(g => `<div class="grp">${esc(g)}</div><div class="keys">${(d.buttons || []).filter(b => (b.group || "Main") === g).map(btn).join("")}</div>`).join("") || `<div class="empty" style="padding:12px;color:var(--pd-muted)">No buttons yet. Press <b>Edit pad</b> and add some (skip, arm, go live, or any call), pick a colour and a hotkey.</div>`}
      ${editor}<div style="margin-top:8px;font-size:10px">${log}</div></div></div>`;
}

export function viewRemote() {
  return `<style>#rem .dot{display:inline-block;width:8px;height:8px;border-radius:50%} #rem a.btn{text-decoration:none} #rem table td{vertical-align:top}</style>
    <div id="rem" class="grid" style="gap:12px">${padPanel()}${failoverCard()}${nodesPanel()}${sessionsPanel()}</div>`;
}

// ---------------------------------------------------------------------------
// Handlers (window.pp.*)
// ---------------------------------------------------------------------------
async function proxy(nodeId, method, path, body) { return pp.POST("/remote/nodes/" + encodeURIComponent(nodeId) + "/proxy", { method, path, body: body || {} }); }
function dlFile(name, text) { const a = document.createElement("a"); a.href = URL.createObjectURL(new Blob([text], { type: "application/json" })); a.download = name; a.click(); setTimeout(() => URL.revokeObjectURL(a.href), 2000); }

export const ppExtRemote = {
  remLoad() { loadRemote(true); },
  // --- remote pad
  padEdit() { R.pad.edit = !R.pad.edit; if (!R.pad.edit && R.pad.dirty) { R.pad.dirty = false; loadPad(); } R.pad.sel = null; pp.render(); },
  padSet(k, v) { R.pad[k] = v; pp.render(); },
  padTheme(id) { R.pad.d.theme = id; R.pad.dirty = true; pp.render(); },
  padAdd() {
    const d = R.pad.d, c = R.pad.cat === "custom" ? { label: "Custom", group: "Custom", method: "GET", path: "/status", body: {}, role: "neutral", confirm: false } : d.catalog[parseInt(R.pad.cat) || 0];
    const b = Object.assign({ id: "b" + Math.random().toString(36).slice(2, 8), node: R.pad.addNode, channel: c.needsChannel ? R.pad.addCh : "", hotkey: "" }, JSON.parse(JSON.stringify(c))); delete b.needsChannel;
    d.buttons = (d.buttons || []).concat([b]); R.pad.sel = b.id; R.pad.dirty = true; pp.render();
  },
  padField(k, v, rerender) {
    const b = (R.pad.d.buttons || []).find(x => x.id === R.pad.sel); if (!b) return;
    if (k === "body") { try { b.body = JSON.parse(v || "{}"); } catch { return; } } else b[k] = v;
    R.pad.dirty = true; if (rerender) pp.render();
  },
  padDelete() { R.pad.d.buttons = (R.pad.d.buttons || []).filter(b => b.id !== R.pad.sel); R.pad.sel = null; R.pad.dirty = true; pp.render(); },
  padSave() { pp.PUT("/remote/pad", { theme: R.pad.d.theme, buttons: R.pad.d.buttons }).then(() => { R.pad.dirty = false; R.pad.sel = null; pp.toast("Pad saved", "ok"); loadPad(); }).catch(e => pp.toast("pad: " + e, "err")); },
  padCancel() { R.pad.dirty = false; R.pad.edit = false; R.pad.sel = null; loadPad(); pp.render(); },
  padClick(id) {
    if (R.pad.edit) { R.pad.sel = id; pp.render(); return; }
    const b = (R.pad.d.buttons || []).find(x => x.id === id); if (!b) return;
    const go = () => { R.pad.busy[id] = true; pp.render(); pp.POST("/remote/pad/" + encodeURIComponent(id) + "/press").then(() => pp.toast(b.label + ": sent", "ok")).catch(e => pp.toast(b.label + ": " + e, "err")).finally(() => { delete R.pad.busy[id]; loadPad(); }); };
    if (b.confirm) pp.confirmDo(b.label + (b.channel ? " on " + b.channel : "") + "?", go); else go();
  },
  // --- failover
  foField(k, v) { R.cfg = R.cfg || {}; if (["intervalSec", "graceSec", "replicateIntervalSec"].includes(k)) v = parseInt(v) || 0; R.cfg[k] = v; R.cfgKeys.add(k); R.cfgDirty = true; pp.render(); },
  // Saves ONLY the keys the operator edited (never the whole cached form): a stale role/peer in the form must not revert what the node did
  // since (auto-promote, a newer epoch). A role change needs a confirm, and carries the role it was chosen against (`expectedRole`): the node
  // answers 409 if its role is no longer that one.
  foSave() {
    if (!R.cfgKeys.size) return;
    const patch = {}; for (const k of R.cfgKeys) patch[k] = (R.cfg || {})[k];
    const cur = (R.fo && R.fo.role) || "standalone";
    if (patch.role === cur) delete patch.role;      // already the node's role: nothing to change, nothing to send
    const send = () => pp.PUT("/failover/config", patch).then(st => { R.fo = st; R.cfg = Object.assign({}, st.config || {}); R.cfgKeys.clear(); R.cfgDirty = false; pp.toast("Failover config saved — role " + st.role, "ok"); loadRemote(true); })
      .catch(() => { R.cfgKeys.delete("role"); R.cfgDirty = R.cfgKeys.size > 0; loadRemote(true); });   // e.g. 409: the node's role changed — show the real one again
    if (patch.role === undefined) { send(); return; }
    patch.expectedRole = cur;
    pp.confirmDo("Change this node's role from " + cur.toUpperCase() + " to " + patch.role.toUpperCase() + "? " +
      (patch.role === "primary" && cur === "backup" ? "It takes air NOW: every channel arms and the epoch is bumped, so the current primary will demote itself (same as Promote). "
        : patch.role === "backup" ? "Every channel disarms and playout stops (slate) on this node. " : "") + "Other unsaved fields are saved too.", send);
  },
  foPromote() { pp.confirmDo("PROMOTE this node to PRIMARY? Every channel arms and joins the schedule at the wall clock. If the other node is still on air you will have two primaries until it sees the newer epoch and demotes.", () => pp.POST("/failover/promote", { reason: "operator (web)" }).then(r => { pp.toast("Promoted: " + r.role, "ok"); loadRemote(true); })); },
  foDemote() { pp.confirmDo("DEMOTE this node to BACKUP? Every channel disarms and playout stops (slate) on this node, and it becomes a warm standby of its peer.", () => pp.POST("/failover/demote", { reason: "operator (web)" }).then(r => { pp.toast("Demoted: " + r.role, "ok"); loadRemote(true); })); },
  // --- nodes
  remDiscovery(enabled) { const body = { enabled }; if (R.discoveryPassword) body.password = R.discoveryPassword; pp.PUT("/remote/discovery", body).then(d => { R.discovery = d; R.discoveryPassword = ""; pp.toast(enabled ? "LAN auto-connect enabled" : "LAN auto-connect disabled", "ok"); loadRemote(true); }).catch(() => {}); },
  remNodeEdit(id) { R.nodeEdit = id ? Object.assign({}, R.nodes.find(n => n.id === id) || {}) : { name: "", url: "", password: "" }; pp.render(); },
  remNodeSave() { const e = R.nodeEdit; if (!e || !e.url) { pp.toast("IP address required", "err"); return; } const list = R.nodes.filter(n => n.id !== e.id); list.push(e); pp.PUT("/remote/nodes", { nodes: list }).then(r => { R.nodes = r.nodes || []; R.nodeEdit = null; pp.toast("Node saved", "ok"); loadRemote(true); }); },
  remNodeDel(id, name) { pp.confirmDo("Remove node \"" + name + "\" from this list? (nothing changes on that node)", () => pp.PUT("/remote/nodes", { nodes: R.nodes.filter(n => n.id !== id) }).then(r => { R.nodes = r.nodes || []; loadRemote(true); })); },
  remProxy(nodeId, method, path, okMsg) { proxy(nodeId, method, path).then(() => { pp.toast(okMsg || "Done", "ok"); setTimeout(() => loadRemote(true), 800); }); },
  remPushSel(nodeId, sid) { R.pushSel[nodeId] = sid; },
  remPush(nodeId) { const sid = R.pushSel[nodeId] || (R.sessions[0] && R.sessions[0].id); if (!sid) { pp.toast("No session to push", "err"); return; } const n = R.nodes.find(x => x.id === nodeId) || {};
    pp.POST("/remote/nodes/" + encodeURIComponent(nodeId) + "/push-session/" + encodeURIComponent(sid)).then(r => pp.toast("Push to " + (n.name || nodeId) + ": " + (r.action || "ok") + (r.reason ? " — " + r.reason : ""), r.action === "conflict" ? "err" : "ok")); },
  remPullList(nodeId) { R.pull = { nodeId, sessions: [], error: "loading…" }; pp.render(); proxy(nodeId, "GET", "/sessions").then(r => { R.pull = { nodeId, sessions: r.sessions || [], error: "" }; pp.render(); }).catch(e => { R.pull = { nodeId, sessions: [], error: String(e.message || e) }; pp.render(); }); },
  remPull(nodeId, sid) { pp.POST("/remote/nodes/" + encodeURIComponent(nodeId) + "/pull-session/" + encodeURIComponent(sid)).then(r => { pp.toast("Pulled: " + (r.action || "ok") + (r.reason ? " — " + r.reason : ""), r.action === "conflict" ? "err" : "ok"); loadRemote(false); }); },
  remPullClose() { R.pull = null; pp.render(); },
  // --- sessions
  sesCapture() { const name = prompt("Name this station snapshot:", "Station " + new Date().toISOString().slice(0, 16).replace("T", " ")); if (name === null) return; const notes = prompt("Notes (optional):", "") || "";
    pp.POST("/sessions/capture", { name: name || "Session", notes }).then(s => { pp.toast("Captured \"" + s.name + "\"", "ok"); loadRemote(false); }); },
  sesImport() { const inp = document.createElement("input"); inp.type = "file"; inp.accept = ".json,application/json"; inp.onchange = async () => { const f = inp.files[0]; if (!f) return; let parsed; try { parsed = JSON.parse(await f.text()); } catch { pp.toast("Not valid JSON", "err"); return; }
      pp.POST("/sessions/import", { name: (parsed.name || f.name.replace(/\.(ppsession\.)?json$/i, "")) + " (imported)", bundle: parsed }).then(s => { pp.toast("Imported \"" + s.name + "\" — review, then Apply", "ok"); loadRemote(false); }); }; inp.click(); },
  sesRename(id, cur) { const name = prompt("Rename session:", cur); if (name === null || !name.trim()) return; pp.PUT("/sessions/" + id, { name: name.trim() }).then(() => loadRemote(false)); },
  sesClone(id, cur) { const name = prompt("Clone name:", cur + " (copy)"); if (name === null) return; pp.POST("/sessions/" + id + "/clone", { name }).then(s => { pp.toast("Cloned as \"" + s.name + "\"", "ok"); loadRemote(false); }); },
  sesRecapture(id) { pp.confirmDo("Re-capture the current station state into this session (bumps its version)?", () => pp.POST("/sessions/" + id + "/recapture").then(() => { pp.toast("Re-captured", "ok"); loadRemote(false); })); },
  sesDelete(id, name) { pp.confirmDo("Delete session \"" + name + "\"? Only the snapshot file goes; the live station is untouched.", () => pp.api("DELETE", "/sessions/" + id).then(() => { if (R.applySid === id) R.applySid = null; loadRemote(false); })); },
  sesExportDl(id, name) { pp.GET("/sessions/" + id + "/export").then(w => dlFile("session-" + (name || id).replace(/[^a-z0-9]+/gi, "-").toLowerCase() + ".ppsession.json", JSON.stringify(w, null, 2))); },
  sesApplyOpen(id) { R.applySid = id; R.applyGroups = new Set(["channels", "schedule", "media", "settings", "cg", "spots", "programming"]); R.applyReplace = false; R.applyArm = false; R.applyReport = null; pp.render(); ppExtRemote.sesDry(); },
  sesApplyGroup(g, on) { if (on) R.applyGroups.add(g); else R.applyGroups.delete(g); R.applyReport = null; pp.render(); },
  sesApplyReplace(on) { R.applyReplace = on; R.applyReport = null; pp.render(); },
  sesApplyClose() { R.applySid = null; R.applyReport = null; pp.render(); },
  sesDry() { if (!R.applySid) return; R.applyBusy = true; pp.render();
    pp.POST("/sessions/" + R.applySid + "/apply", { groups: [...R.applyGroups], dryRun: true, replace: R.applyReplace, armFromBundle: R.applyArm }).then(r => { R.applyReport = r; }).catch(() => {}).finally(() => { R.applyBusy = false; pp.render(); }); },
  sesApplyNow() { if (!R.applySid) return; const groups = [...R.applyGroups]; if (!groups.length) { pp.toast("Pick at least one group", "err"); return; }
    const warn = (groups.includes("outputs") ? "OUTPUTS (this node's hardware config) will be overwritten. " : "") + (R.applyReplace ? "Hours not in the session are DELETED. " : "");
    pp.confirmDo("Apply " + groups.join(", ") + " from this session onto THIS station now? " + warn + "The dry run above shows what changes.", () => { R.applyBusy = true; pp.render();
      pp.POST("/sessions/" + R.applySid + "/apply", { groups, dryRun: false, replace: R.applyReplace, armFromBundle: R.applyArm }).then(r => { R.applyReport = r; const n = Object.values(r.applied || {}).reduce((a, b) => a + b, 0); pp.toast("Applied " + n + " record(s), " + (r.unresolvedMedia || []).length + " unresolved media" + ((r.errors || []).length ? ", " + r.errors.length + " error(s)" : ""), (r.errors || []).length ? "err" : "ok"); }).catch(() => {}).finally(() => { R.applyBusy = false; pp.render(); loadRemote(false); }); }); },
};

document.addEventListener("keydown", e => {
  if (!window.pp || pp.S.view !== "remote" || R.pad.edit || !R.pad.d) return;
  const t = e.target; if (t && (/^(INPUT|TEXTAREA|SELECT)$/.test(t.tagName) || t.isContentEditable)) return;
  if (e.ctrlKey || e.metaKey || e.altKey) return;
  const k = String(e.key || "").toLowerCase(), b = (R.pad.d.buttons || []).find(x => x.hotkey && x.hotkey.toLowerCase() === k);
  if (b) { e.preventDefault(); pp.padClick(b.id); }
});
