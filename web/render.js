/* PLAY PLAY web dashboard — Render page (docs/render-nodes.md): render nodes (spot-node, PlayPlay peers, StarAgent
   daemons, local), the render config, new jobs (spot / transcode / split) and the live job list. All state lives in
   RN (window.RNS); every input writes into RN as it is typed, so a WS repaint never wipes what the operator types. */

const RN = {
  loaded: false, loading: false, status: {}, nodes: [], cfg: null, cfgDirty: false, templates: [], jobs: [], counts: {}, open: {},
  nodeEdit: null, jobState: "", busy: false,
  job: { kind: "spot", type: "motion_spot", template: "", brief: "", lengthSec: 30, aspect: "16:9", mode: "offline", voice: "", target: "auto",
    ingestType: "promo", ingestTitle: "", ingestOn: true, input: "", preset: "h264_1080p30", segments: 4, workers: ["local"], copy: false },
};
window.RNS = RN;
const esc = s => String(s ?? "").replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
const js = v => esc(JSON.stringify(String(v ?? "")));          // a value as a JS string literal inside a double-quoted handler attribute
const bdg = (text, color) => `<span class="badge" style="color:${color};border-color:${color};background:color-mix(in srgb,${color} 16%,transparent)">${esc(text)}</span>`;
const STATE = { queued: "var(--warn)", running: "var(--accent)", done: "var(--ready)", failed: "var(--danger)", cancelled: "var(--t3)" };
const when = ms => ms ? new Date(ms).toLocaleString([], { month: "short", day: "numeric", hour: "numeric", minute: "2-digit" }) : "—";
const secs = (a, b) => a ? Math.round(((b || Date.now()) - a) / 1000) + " s" : "—";
const opts = (list, cur) => list.map(o => { const v = Array.isArray(o) ? o[0] : o, l = Array.isArray(o) ? o[1] : o; return `<option value="${esc(v)}" ${String(v) === String(cur) ? "selected" : ""}>${esc(l)}</option>`; }).join("");
const PRESETS = ["h264_1080p30", "h264_720p30", "proxy_540p", "audio_wav", "loudnorm", "custom"];
const KINDS = ["spotnode", "playplay", "staragent", "local"];
const INGEST_TYPES = ["promo", "ad", "psa", "program", "bumper", "filler"];
const openMedia = id => { pp.nav("library"); setTimeout(() => { if (pp.libOpen) pp.libOpen(id); }, 350); };
const health = n => { const h = n.health || {}; return { ok: h.ok ?? n.ok, ready: h.ready ?? n.ready, reason: h.reason || n.reason || h.error || "", q: h.queue || n.queue || {}, ms: h.latencyMs ?? h.ms ?? n.latencyMs }; };

async function loadRender() {
  if (RN.loading) return; RN.loading = true;
  try {
    const [st, nd, cf, jb] = await Promise.all([pp.GET("/render/status").catch(() => ({})), pp.GET("/render/nodes").catch(() => ({})), pp.GET("/render/config").catch(() => null),
      pp.GET("/render/jobs?limit=100" + (RN.jobState ? "&state=" + encodeURIComponent(RN.jobState) : "")).catch(() => ({}))]);
    RN.status = st || {}; RN.nodes = nd.nodes || (Array.isArray(nd) ? nd : []); if (cf && !RN.cfgDirty) RN.cfg = JSON.parse(JSON.stringify(cf.config || cf));
    RN.jobs = jb.jobs || []; RN.counts = jb.counts || st.counts || RN.counts; RN.loaded = true;
  } finally { RN.loading = false; }
  pp.render();
  if (!RN.templates.length) pp.GET("/render/templates").then(t => { RN.templates = t.templates || []; pp.render(); }).catch(() => {});
}
const loadNodes = () => pp.GET("/render/nodes").then(n => { RN.nodes = n.nodes || []; pp.render(); }).catch(() => {});
const loadJobs = () => pp.GET("/render/jobs?limit=100" + (RN.jobState ? "&state=" + encodeURIComponent(RN.jobState) : "")).then(j => { RN.jobs = j.jobs || []; RN.counts = j.counts || RN.counts; pp.render(); }).catch(() => {});

export function onEnterRender(v) { if (v === "render") loadRender(); }
export function onWsRender(m) {
  if (m.type !== "render") return false;
  if (Array.isArray(m.jobs)) RN.jobs = RN.jobState ? m.jobs.filter(j => j.state === RN.jobState) : m.jobs;
  if (m.counts) RN.counts = m.counts;
  return true;
}

// ---------------------------------------------------------------------------------------------
export function viewRender() {
  if (!RN.loaded) loadRender();
  const c = RN.counts || {}, cfg = RN.cfg || {};
  const ready = RN.nodes.filter(n => n.enabled !== false && health(n).ready).length;
  const warn = (RN.status.warnings || []).map(w => `<div class="alert warning">${esc(w)}</div>`).join("");
  const head = `<div class="panel"><h3>Render <span class="right">${bdg(ready + "/" + RN.nodes.length + " nodes ready", ready ? "var(--ready)" : "var(--danger)")}
      ${bdg("studio " + (cfg.studioMode || "?"), "var(--accent2)")}${bdg((cfg.renderMode || "?") + " render", "var(--t2)")}${bdg("mesh " + (cfg.meshDispatch || "?"), "var(--t2)")}
      ${bdg((c.running || 0) + " running", "var(--accent)")}${bdg((c.queued || 0) + " queued", (c.queued ? "var(--warn)" : "var(--t3)"))}${bdg((c.done || 0) + " done", "var(--ready)")}${c.failed ? bdg(c.failed + " failed", "var(--danger)") : ""}
      <button class="ghost small" onclick="pp.renderReload()">&#8635;</button></span></h3>${warn ? `<div class="body" style="padding:6px 12px">${warn}</div>` : ""}</div>`;
  return `<div class="grid" style="gap:12px">${head}${viewNodes()}<div class="grid cols2">${viewNewJob()}${viewConfig()}</div>${viewJobs()}</div>`;
}

function viewNodes() {
  const rows = RN.nodes.map(n => { const h = health(n); const q = h.q; const local = n.kind === "local";
    return `<tr><td>${bdg(n.kind || "?", n.kind === "spotnode" ? "var(--accent2)" : n.kind === "playplay" ? "var(--accent)" : n.kind === "staragent" ? "var(--standby)" : "var(--t2)")}</td>
      <td><b>${esc(n.name || n.id)}</b>${n.discovered ? " " + bdg("discovered", "var(--cued)") : ""}<div style="color:var(--t3);font-size:10px">${esc(n.id)}${n.notes ? " · " + esc(n.notes) : ""}</div></td>
      <td class="mono" style="font-size:11px">${esc(n.url || "")}${n.hasToken ? ` <span title="token stored">&#128273;</span>` : ""}</td>
      <td>${n.enabled !== false ? bdg("on", "var(--ready)") : bdg("off", "var(--t3)")}</td><td>${n.render ? bdg("opt-in", "var(--accent)") : "<span style='color:var(--t3)'>—</span>"}</td><td>${n.primary ? bdg("primary", "var(--accent2)") : ""}</td>
      <td>${h.ok === undefined ? bdg("?", "var(--t3)") : h.ok ? bdg("ok", "var(--ready)") : bdg("down", "var(--danger)")} ${h.ready ? bdg("ready", "var(--ready)") : h.ok ? bdg("busy", "var(--warn)") : ""}${h.reason ? `<div style="color:var(--t3);font-size:10px" title="${esc(h.reason)}">${esc(String(h.reason).slice(0, 60))}</div>` : ""}</td>
      <td class="mono">${q.running ?? 0}/${q.queued ?? 0}/${q.max ?? n.maxBusy ?? "—"}</td><td class="mono">${h.ms !== undefined && h.ms !== null ? Math.round(h.ms) + " ms" : "—"}</td>
      <td class="row" style="flex-wrap:nowrap"><button class="ghost small" onclick="pp.renderProbe(${js(n.id)})">probe</button>${local || n.discovered ? "" : `<button class="ghost small" onclick="pp.renderNodeEdit(${js(n.id)})">&#9998;</button><button class="ghost small" onclick="pp.renderNodeDel(${js(n.id)})">&#10005;</button>`}</td></tr>`; }).join("");
  return `<div class="panel"><h3>Render nodes <span class="right"><button class="ghost small" onclick="pp.renderProbe()">Probe all</button><button class="small" onclick="pp.renderNodeEdit('')">+ node</button></span></h3>
    <div class="scroll" style="max-height:40vh"><table><thead><tr><th>Kind</th><th>Name</th><th>URL</th><th>Enabled</th><th>Render</th><th>Primary</th><th>Health</th><th title="running / queued / max">Queue</th><th>Latency</th><th></th></tr></thead>
    <tbody>${rows || `<tr><td colspan="10" class="empty">No render nodes. The local Supervisor is always available; a local spot-node is discovered when it answers on 127.0.0.1:8775.</td></tr>`}</tbody></table></div>${RN.nodeEdit ? viewNodeEditor() : ""}</div>`;
}
function viewNodeEditor() {
  const e = RN.nodeEdit; const b = k => `RNS.nodeEdit.${k}`;
  return `<div class="body" style="border-top:1px solid var(--b0)"><div class="fh">${e.id ? "EDIT NODE " + esc(e.id) : "ADD NODE"}</div><div class="grid cols4" style="gap:8px">
    <label class="f">Kind <select onchange="${b("kind")}=this.value;pp.render()">${opts(KINDS.filter(k => k !== "local"), e.kind)}</select></label>
    <label class="f">Name <input value="${esc(e.name)}" oninput="${b("name")}=this.value"></label>
    <label class="f">URL <input value="${esc(e.url)}" placeholder="${e.kind === "spotnode" ? "http://host:8775" : e.kind === "staragent" ? "http://host:8780" : "http://host:7700"}" oninput="${b("url")}=this.value"></label>
    <label class="f">Token <input type="password" autocomplete="new-password" value="${esc(e.token)}" placeholder="${e.hasToken ? "•••••• (blank keeps the stored one)" : "none"}" oninput="${b("token")}=this.value"></label>
    <label class="f">Max busy <input type="number" min="0" value="${esc(e.maxBusy)}" oninput="${b("maxBusy")}=parseInt(this.value)||0"></label>
    ${e.kind === "staragent" ? `<label class="f">Star node id <input value="${esc(e.starNode)}" placeholder="StarClient node id" oninput="${b("starNode")}=this.value"></label>` : ""}
    <label class="f" style="grid-column:span 2">Notes <input value="${esc(e.notes)}" placeholder="render=true in notes also opts a PlayPlay peer in" oninput="${b("notes")}=this.value"></label>
    <div class="row" style="grid-column:1/-1"><label><input type="checkbox" ${e.enabled ? "checked" : ""} onchange="${b("enabled")}=this.checked"> enabled</label><label><input type="checkbox" ${e.render ? "checked" : ""} onchange="${b("render")}=this.checked"> render (opt in to dispatch)</label><label><input type="checkbox" ${e.primary ? "checked" : ""} onchange="${b("primary")}=this.checked"> primary (never dispatched to)</label>
      <span class="fill"></span><button class="success" onclick="pp.renderNodeSave()">SAVE</button><button class="ghost" onclick="RNS.nodeEdit=null;pp.render()">cancel</button></div></div></div>`;
}

function viewConfig() {
  const c = RN.cfg; if (!c) return `<div class="panel"><h3>Render config</h3><div class="empty">loading…</div></div>`;
  const set = (k, v) => `RNS.cfg.${k}=${v};RNS.cfgDirty=true`;
  return `<div class="panel"><h3>Render config ${RN.cfgDirty ? bdg("unsaved", "var(--warn)") : ""}</h3><div class="body grid cols2" style="gap:8px">
    <label class="f">Studio mode <select onchange="${set("studioMode", "this.value")}">${opts([["off", "off (local only)"], ["prefer", "prefer the spot-node"], ["only", "only the spot-node"]], c.studioMode)}</select></label>
    <label class="f">Render mode <select onchange="${set("renderMode", "this.value")}">${opts(["offline", "hybrid"], c.renderMode)}</select></label>
    <label class="f">Wait (s) <input type="number" min="0" value="${esc(c.waitSec ?? "")}" oninput="${set("waitSec", "parseFloat(this.value)||0")}"></label>
    <label class="f">Max local ffmpeg <input type="number" min="1" value="${esc(c.maxLocal ?? 2)}" oninput="${set("maxLocal", "parseInt(this.value)||1")}"></label>
    <label class="f">Mesh dispatch <select onchange="${set("meshDispatch", "this.value")}">${opts(["auto", "local", "off"], c.meshDispatch)}</select></label>
    <label class="f">Custom args <label style="text-transform:none"><input type="checkbox" ${c.allowCustomArgs ? "checked" : ""} onchange="${set("allowCustomArgs", "this.checked")}"> allow the custom transcode preset</label></label>
    <div class="row" style="grid-column:1/-1"><button class="success small" ${RN.cfgDirty ? "" : "disabled"} onclick="pp.renderCfgSave()">SAVE</button><button class="ghost small" ${RN.cfgDirty ? "" : "disabled"} onclick="RNS.cfgDirty=false;pp.renderReload()">revert</button></div></div></div>`;
}

function viewNewJob() {
  const j = RN.job; const b = k => `RNS.job.${k}`;
  const tpl = RN.templates.find(t => t.id === j.template) || null;
  const ingest = `<label class="f">Ingest type <select onchange="${b("ingestType")}=this.value">${opts(INGEST_TYPES, j.ingestType)}</select></label><label class="f">Ingest title <input value="${esc(j.ingestTitle)}" placeholder="library title" oninput="${b("ingestTitle")}=this.value"></label>`;
  let body = "";
  if (j.kind === "spot") {
    const aspects = tpl && (tpl.aspects || []).length ? tpl.aspects : ["16:9", "9:16", "1:1", "4:5"];
    const lengths = tpl && (tpl.lengths || []).length ? tpl.lengths : null;
    const targets = [["auto", "auto (least busy eligible)"]].concat(RN.nodes.filter(n => n.kind !== "staragent").map(n => [n.id, (n.name || n.id) + " (" + n.kind + ")"]));
    body = `<label class="f">Type <select onchange="${b("type")}=this.value;pp.render()">${opts([["motion_spot", "motion spot (from a brief)"], ["template_render", "template render"]], j.type)}</select></label>
      ${j.type === "template_render" ? `<label class="f">Template <select onchange="${b("template")}=this.value;pp.render()"><option value="">(pick a template)</option>${RN.templates.map(t => `<option value="${esc(t.id)}" ${t.id === j.template ? "selected" : ""}>${esc(t.title || t.id)}${t.category ? " · " + esc(t.category) : ""}</option>`).join("")}</select></label>` : `<div></div>`}
      ${tpl ? `<div style="grid-column:1/-1;color:var(--t3);font-size:11px">${esc(tpl.description || "")} · aspects ${esc((tpl.aspects || []).join(", ") || "any")} · lengths ${esc((tpl.lengths || []).join(", ") || "any")}</div>` : ""}
      <label class="f" style="grid-column:1/-1">Brief <textarea rows="3" oninput="${b("brief")}=this.value" placeholder="What the spot says, who it is for, the call to action">${esc(j.brief)}</textarea></label>
      <label class="f">Length (s) ${lengths ? `<select onchange="${b("lengthSec")}=parseInt(this.value)||30">${opts(lengths, j.lengthSec)}</select>` : `<input type="number" min="3" max="120" value="${esc(j.lengthSec)}" oninput="${b("lengthSec")}=parseInt(this.value)||30">`}</label>
      <label class="f">Aspect <select onchange="${b("aspect")}=this.value">${opts(aspects, j.aspect)}</select></label>
      <label class="f">Mode <select onchange="${b("mode")}=this.value">${opts(["offline", "hybrid"], j.mode)}</select></label>
      <label class="f">Voice <input value="${esc(j.voice)}" placeholder="default" oninput="${b("voice")}=this.value"></label>
      <label class="f">Target <select onchange="${b("target")}=this.value">${opts(targets, j.target)}</select></label><div></div>${ingest}`;
  } else if (j.kind === "transcode") {
    body = `<label class="f" style="grid-column:1/-1">Input (path or media id) <input value="${esc(j.input)}" placeholder="D:\\media\\clip.mov or a media id" oninput="${b("input")}=this.value"></label>
      <label class="f">Preset <select onchange="${b("preset")}=this.value">${opts(PRESETS.filter(p => p !== "custom" || (RN.cfg && RN.cfg.allowCustomArgs)), j.preset)}</select></label>
      <label class="f">Library <label style="text-transform:none"><input type="checkbox" ${j.ingestOn ? "checked" : ""} onchange="${b("ingestOn")}=this.checked;pp.render()"> ingest the output</label></label>${j.ingestOn ? ingest : ""}`;
  } else {
    const workers = [["local", "local"]].concat(RN.nodes.filter(n => n.kind === "staragent").map(n => [n.id, n.name || n.id]));
    body = `<label class="f" style="grid-column:1/-1">Input (path or media id) <input value="${esc(j.input)}" oninput="${b("input")}=this.value"></label>
      <label class="f">Segments <input type="number" min="1" max="50" value="${esc(j.segments)}" oninput="${b("segments")}=Math.max(1,Math.min(50,parseInt(this.value)||1))"></label>
      <label class="f">Preset <select onchange="${b("preset")}=this.value">${opts(PRESETS.filter(p => p !== "custom" && p !== "audio_wav"), j.preset)}</select></label>
      <label class="f" style="grid-column:1/-1">Workers <span class="row">${workers.map(([id, l]) => `<label style="font-size:12px"><input type="checkbox" ${j.workers.includes(id) ? "checked" : ""} onchange="const s=new Set(RNS.job.workers);this.checked?s.add(${js(id)}):s.delete(${js(id)});RNS.job.workers=[...s]"> ${esc(l)}</label>`).join("")}</span></label>
      <label class="f">Copy <label style="text-transform:none"><input type="checkbox" ${j.copy ? "checked" : ""} onchange="${b("copy")}=this.checked"> stream copy (no re-encode)</label></label>
      <label class="f">Library <label style="text-transform:none"><input type="checkbox" ${j.ingestOn ? "checked" : ""} onchange="${b("ingestOn")}=this.checked;pp.render()"> ingest the output</label></label>${j.ingestOn ? ingest : ""}`;
  }
  return `<div class="panel"><h3>New render job <span class="right">${["spot", "transcode", "split"].map(k => `<button class="ghost small${j.kind === k ? " active" : ""}" onclick="RNS.job.kind='${k}';pp.render()">${k}</button>`).join("")}</span></h3>
    <div class="body grid cols2" style="gap:8px">${body}<div class="row" style="grid-column:1/-1"><button class="primary" ${RN.busy ? "disabled" : ""} onclick="pp.renderSubmit()">${RN.busy ? "Queueing…" : "Queue " + j.kind + " job"}</button></div></div></div>`;
}

function viewJobs() {
  const rows = RN.jobs.map(j => { const active = j.state === "queued" || j.state === "running"; const segs = j.segments || [];
    const main = `<tr><td>${bdg(j.state || "?", STATE[j.state] || "var(--t3)")}</td><td>${bdg(j.kind || "?", "var(--t2)")}</td>
      <td><b>${esc(j.title || (j.ingest && j.ingest.title) || j.brief || j.input || j.id)}</b><div style="color:var(--t3);font-size:10px" class="mono">${esc(j.id)}${j.requestId ? " · req " + esc(j.requestId) : ""}${j.costUsd ? " · $" + Number(j.costUsd).toFixed(2) : ""}</div></td>
      <td>${esc(j.step || "")}</td><td><div class="meter" style="width:120px"><div class="bar" style="width:${Math.max(0, Math.min(100, j.pct || 0))}%;background:${STATE[j.state] || "var(--accent)"}"></div></div><span class="mono" style="font-size:10px;color:var(--t3)">${Math.round(j.pct || 0)}%</span></td>
      <td>${esc(j.nodeName || j.node || "")}</td><td class="mono" style="color:var(--t3)">${when(j.createdMs)}<div style="font-size:10px">${secs(j.startedMs, j.finishedMs)}</div></td>
      <td>${j.mediaId ? `<a href="#" onclick="pp.renderOpenMedia(${js(j.mediaId)});return false">${esc(j.mediaId)}</a>` : ""}${j.error ? `<div style="color:var(--danger);font-size:10px;white-space:normal" title="${esc(j.error)}">&#9888; ${esc(String(j.error).slice(0, 120))}</div>` : ""}</td>
      <td class="row" style="flex-wrap:nowrap">${segs.length ? `<button class="ghost small" onclick="RNS.open[${js(j.id)}]=!RNS.open[${js(j.id)}];pp.render()">${RN.open[j.id] ? "&#9662;" : "&#9656;"} ${segs.length} seg</button>` : ""}
        ${active ? `<button class="ghost small" onclick="pp.renderCancel(${js(j.id)})">cancel</button>` : `<button class="ghost small" onclick="pp.renderJobDel(${js(j.id)})">&#10005;</button>`}</td></tr>`;
    const seg = RN.open[j.id] ? segs.map(s => `<tr><td></td><td class="mono" style="color:var(--t3)">#${s.index}</td><td class="mono" style="font-size:11px">${(+s.startSec || 0).toFixed(1)} s + ${(+s.durSec || 0).toFixed(1)} s</td><td>${bdg(s.state || "?", STATE[s.state] || "var(--t3)")}</td>
      <td><div class="meter" style="width:120px"><div class="bar" style="width:${Math.max(0, Math.min(100, s.pct || 0))}%;background:var(--accent)"></div></div></td><td>${esc(s.worker || "")}</td><td colspan="3" style="font-size:10px;color:${s.error ? "var(--danger)" : "var(--t3)"}" title="${esc(s.error || s.output || "")}">${esc(s.error || s.output || "")}</td></tr>`).join("") : "";
    return main + seg; }).join("");
  return `<div class="panel"><h3>Jobs <span class="right"><select class="small" onchange="RNS.jobState=this.value;pp.renderReloadJobs()">${opts([["", "all states"], "queued", "running", "done", "failed", "cancelled"], RN.jobState)}</select><button class="ghost small" onclick="pp.renderClear()">Clear finished</button></span></h3>
    <div class="scroll" style="max-height:52vh"><table><thead><tr><th>State</th><th>Kind</th><th>Job</th><th>Step</th><th>Progress</th><th>Node</th><th>Created</th><th>Result</th><th></th></tr></thead>
    <tbody>${rows || `<tr><td colspan="9" class="empty">No render jobs.</td></tr>`}</tbody></table></div></div>`;
}

// ---------------------------------------------------------------------------------------------
export const ppExtRender = {
  renderReload() { loadRender(); },
  renderReloadJobs() { loadJobs(); },
  renderOpenMedia(id) { openMedia(id); },
  renderProbe(id) {
    pp.toast(id ? "Probing " + id + "…" : "Probing all nodes…", "ok");
    pp.POST(id ? "/render/nodes/" + encodeURIComponent(id) + "/probe" : "/render/probe").then(() => loadNodes()).catch(() => loadNodes());
  },
  renderNodeEdit(id) {
    const n = RN.nodes.find(x => x.id === id);
    RN.nodeEdit = n ? { id: n.id, kind: n.kind || "spotnode", name: n.name || "", url: n.url || "", token: "", hasToken: !!n.hasToken, enabled: n.enabled !== false, render: !!n.render, primary: !!n.primary, maxBusy: n.maxBusy ?? 1, notes: n.notes || "", starNode: n.starNode || "" }
      : { id: "", kind: "spotnode", name: "", url: "", token: "", hasToken: false, enabled: true, render: false, primary: false, maxBusy: 1, notes: "", starNode: "" };
    pp.render();
  },
  renderNodeSave() {
    const e = RN.nodeEdit; if (!e) return; if (!e.url.trim()) { pp.toast("URL is required", "err"); return; }
    const body = { kind: e.kind, name: e.name.trim() || e.url.trim(), url: e.url.trim(), enabled: e.enabled, render: e.render, primary: e.primary, maxBusy: e.maxBusy, notes: e.notes };
    if (e.kind === "staragent") body.starNode = e.starNode;
    if (e.token) body.token = e.token;                     // blank keeps the stored token
    (e.id ? pp.PUT("/render/nodes/" + encodeURIComponent(e.id), body) : pp.POST("/render/nodes", body)).then(() => { RN.nodeEdit = null; pp.toast("Node saved", "ok"); loadNodes(); }).catch(() => {});
  },
  renderNodeDel(id) { const n = RN.nodes.find(x => x.id === id) || {}; pp.confirmDo("Remove render node \"" + (n.name || id) + "\"? Its stored token is deleted.", () => pp.api("DELETE", "/render/nodes/" + encodeURIComponent(id)).then(() => loadNodes()).catch(() => {})); },
  renderCfgSave() { const c = RN.cfg; pp.PUT("/render/config", { studioMode: c.studioMode, renderMode: c.renderMode, waitSec: c.waitSec, maxLocal: c.maxLocal, meshDispatch: c.meshDispatch, allowCustomArgs: !!c.allowCustomArgs })
    .then(r => { RN.cfgDirty = false; RN.cfg = JSON.parse(JSON.stringify((r && (r.config || r)) || c)); pp.toast("Render config saved", "ok"); pp.render(); }).catch(() => {}); },
  renderSubmit() {
    const j = RN.job; let body;
    const ingest = { type: j.ingestType, ...(j.ingestTitle.trim() ? { title: j.ingestTitle.trim() } : {}) };
    if (j.kind === "spot") {
      if (!j.brief.trim() && j.type === "motion_spot") { pp.toast("A brief is required", "err"); return; }
      if (j.type === "template_render" && !j.template) { pp.toast("Pick a template", "err"); return; }
      body = { kind: "spot", type: j.type, brief: j.brief, lengthSec: j.lengthSec, aspect: j.aspect, mode: j.mode, target: j.target || "auto", ingest };
      if (j.type === "template_render") body.template = j.template; if (j.voice.trim()) body.voice = j.voice.trim();
    } else {
      if (!j.input.trim()) { pp.toast("Input is required", "err"); return; }
      body = { kind: j.kind, input: j.input.trim(), preset: j.preset };
      if (j.kind === "split") { if (!j.workers.length) { pp.toast("Pick at least one worker", "err"); return; } body.segments = j.segments; body.workers = j.workers; body.copy = !!j.copy; }
      if (j.ingestOn) body.ingest = ingest;
    }
    RN.busy = true; pp.render();
    pp.POST("/render/jobs", body).then(r => { pp.toast("Queued " + ((r && (r.id || (r.job && r.job.id))) || j.kind + " job"), "ok"); if (j.kind === "spot") { j.brief = ""; j.ingestTitle = ""; } loadJobs(); })
      .catch(() => {}).finally(() => { RN.busy = false; pp.render(); });
  },
  renderCancel(id) { pp.POST("/render/jobs/" + encodeURIComponent(id) + "/cancel").then(() => loadJobs()).catch(() => {}); },
  renderJobDel(id) { pp.confirmDo("Delete render job " + id + " from the list? The rendered file and library record are kept.", () => pp.api("DELETE", "/render/jobs/" + encodeURIComponent(id)).then(() => loadJobs()).catch(() => {})); },
  renderClear() { pp.confirmDo("Clear every finished (done, failed, cancelled) render job from the list?", () => pp.POST("/render/jobs/clear").then(() => loadJobs()).catch(() => {})); },
};
