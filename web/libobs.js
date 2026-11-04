/* PLAY PLAY web dashboard — OBS Scenes: the libobs live-input layer (docs/libobs.md). Saved scene specs (settings
   libobs.scenes), a scene editor driven by GET /api/v1/libobs/catalog (item kinds, filter kinds, transition kinds and their
   parameters), Show on channel / Switch / Hide through /api/v1/channels/{id}/libobs/* and the live status (engine telemetry:
   program scene, frame age, libobs fps). Also exports the compact "OBS" strip the Now Playing dashboard embeds
   (libobsStrip / libobsStripEnter / libobsStripLeave; see docs/libobs.md "Embedding the strip").

   Conventions as the other pages: the view is a string rebuilt by app.js on every WS tick, handlers hang off window.pp
   (ppExtLibObs), state lives in LO. */

const LO = {
  ready: false, catalog: null, scenes: [], status: {}, pollTimer: null, stripTimer: null, stripChannel: "", stripFull: false, scenesLoaded: false,
  sel: "", draft: null, dirty: false, json: false, jsonText: "", err: "", busy: false,
  sw: { to: "", transition: "", durationMs: 500 },
  exp: {},                                              // "sceneIdx" -> expanded in the editor
};
window.LO = LO;

const E = s => String(s ?? "").replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
const num = (v, d = 0) => { const n = parseFloat(v); return Number.isFinite(n) ? n : d; };
const clone = o => JSON.parse(JSON.stringify(o));
const jq = v => E(JSON.stringify(String(v ?? "")));
const cssq = s => (typeof CSS !== "undefined" && CSS.escape) ? CSS.escape(String(s)) : String(s).replace(/["\\]/g, "\\$&");     // for [data-x="..."] selectors
const pp = () => window.pp;
const toast = (m, k) => { try { pp().toast(m, k); } catch {} };
const kindOf = (list, id) => (LO.catalog && LO.catalog[list] || []).find(k => k.id === id) || null;
const fmtMs = v => (v === undefined || v === null ? "–" : (Math.round(v * 10) / 10) + " ms");

// ---------------------------------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------------------------------
async function loadAll() {
  try {
    if (!LO.catalog) LO.catalog = await pp().GET("/libobs/catalog");
    const s = await pp().GET("/libobs/scenes"); LO.scenes = s.scenes || [];
    if (!LO.sel && LO.scenes.length) LO.sel = LO.scenes[0].id;
    LO.ready = true;
  } catch (e) { LO.err = String(e.message || e); }
  pp().render();
}
async function loadStatus(ch) {
  if (!ch) return;
  try { LO.status[ch] = await pp().GET("/channels/" + encodeURIComponent(ch) + "/libobs/status"); } catch { LO.status[ch] = { channelId: ch, engineState: "UNREACHABLE" }; }
}
function startPoll() { stopPoll(); const tick = async () => { await loadStatus(pp().S.active); if (pp().S.view === "libobs") pp().render(); }; tick(); LO.pollTimer = setInterval(tick, 2000); }
function stopPoll() { if (LO.pollTimer) { clearInterval(LO.pollTimer); LO.pollTimer = null; } }

export function onEnterLibObs(v) { if (v === "libobs") { loadAll(); startPoll(); } else stopPoll(); }
export function onWsLibObs(m) {
  if (m.type === "libobsScenes") { LO.scenes = m.scenes || []; return true; }
  if (m.type === "libobs") { loadStatus(m.channelId); return true; }
  return false;
}

// ---------------------------------------------------------------------------------------------------
// Editor model: a saved record {id, name, layer, spec:{scenes:[{name, items:[{id,name,settings,fit,pos,size,visible,filters:[]}]}], program, transition:{id,durationMs,settings}}}
// ---------------------------------------------------------------------------------------------------
function newRecord() {
  return { id: "", name: "New scene set", layer: (LO.catalog && LO.catalog.layerDefault) || 50,
    spec: { type: "libobs", audio: true, scenes: [{ name: "A", items: [{ id: "color_source_v3", name: "bg", settings: { color: "#2040c0" } }] }], program: "A", transition: { id: "fade_transition", durationMs: 500 } } };
}
function draftFrom(rec) { LO.draft = rec ? clone(rec) : newRecord(); LO.dirty = false; LO.err = ""; LO.jsonText = JSON.stringify(LO.draft.spec, null, 2); }
function current() { return LO.scenes.find(s => s.id === LO.sel) || null; }
function ensureDraft() { if (!LO.draft) draftFrom(current()); return LO.draft; }
function touch() { LO.dirty = true; pp().render(); }
function defaultsFor(kind) { const o = {}; (kind && kind.params || []).forEach(p => { if (p.default !== undefined && p.type !== "path" && p.type !== "paths" && p.type !== "url") o[p.key] = p.default; }); return o; }

function paramInput(p, value, onchange) {
  const v = value === undefined ? (p.default !== undefined ? p.default : "") : value;
  if (p.type === "bool") return `<label class="row" style="gap:4px"><input type="checkbox" ${v ? "checked" : ""} onchange="${onchange}(this.checked)"> ${E(p.key)}</label>`;
  if (p.type === "enum") return `<label>${E(p.key)} <select onchange="${onchange}(this.value)">${(p.values || []).map(x => `<option value="${E(x)}" ${String(x) === String(v) ? "selected" : ""}>${E(x)}</option>`).join("")}</select></label>`;
  if (p.type === "color") return `<label>${E(p.key)} <input type="text" class="mono" style="width:110px" value="${E(v)}" placeholder="#rrggbb" onchange="${onchange}(this.value)"></label>`;
  if (p.type === "int" || p.type === "float") return `<label>${E(p.key)} <input type="number" step="${p.type === "int" ? 1 : "any"}" style="width:90px" value="${E(v)}" onchange="${onchange}(this.value)"></label>`;
  if (p.type === "font" || p.type === "paths") return `<label>${E(p.key)} <input type="text" class="mono" style="width:220px" value="${E(typeof v === "object" ? JSON.stringify(v) : v)}" title="${E(p.hint || "JSON")}" onchange="${onchange}(this.value)"></label>`;
  return `<label>${E(p.key)} <input type="text" style="width:${p.type === "path" || p.type === "url" ? 320 : 160}px" value="${E(v)}" placeholder="${E(p.hint || "")}" onchange="${onchange}(this.value)"></label>`;
}
function paramsForm(kind, settings, prefix) {
  if (!kind || !kind.params || !kind.params.length) return `<span style="color:var(--t3);font-size:11px">${E(kind && kind.note || "no parameters")}</span>`;
  return `<div class="row" style="gap:10px;font-size:11px">${kind.params.map(p => paramInput(p, settings[p.key], prefix + "('" + E(p.key) + "','" + p.type + "',")).join("")}</div>${kind.note ? `<div style="color:var(--t3);font-size:10px;margin-top:2px">${E(kind.note)}</div>` : ""}`;
}
function itemHtml(si, ii, it) {
  const kind = kindOf("items", it.id); const name = it.name || it.id;
  const fits = (LO.catalog && LO.catalog.fit) || ["stretch", "inner", "outer", "none"];
  const filt = (it.filters || []).map((f, fi) => `<div class="row" style="gap:8px;padding:4px 0 2px 16px;border-top:1px dashed var(--b0)"><span class="badge" style="border-color:var(--t3);color:var(--t3)">filter</span><b>${E(f.id)}</b>
      ${paramsForm(kindOf("filters", f.id), f.settings || {}, `pp.loSetFilter(${si},${ii},${fi}`)}${["chroma_key_filter_v2", "color_key_filter_v2"].includes(f.id) ? `<label>Screen color <input type="color" value="${/^#[0-9a-f]{6}$/i.test((f.settings || {}).key_color || "") ? E(f.settings.key_color) : "#00ff00"}" onchange="pp.loKeyColor(${si},${ii},${fi},this.value)"></label><button class="small" ${typeof window.EyeDropper === "function" ? "" : "disabled"} onclick="pp.loPickKeyColor(${si},${ii},${fi})" title="Sample a screen color">Eyedropper</button><button class="small" onclick="pp.loApplyKey(${si},${ii},${fi})">Apply live</button><span style="color:var(--t3);font-size:10px">Apply live to the shown scene; Save retains these settings.</span>` : ""}<button class="ghost small" onclick="pp.loDelFilter(${si},${ii},${fi})" title="Remove filter">✕</button></div>`).join("");
  return `<div style="padding:6px 8px;border:1px solid var(--b0);border-radius:6px;margin:4px 0;background:var(--bg1)">
    <div class="row" style="gap:8px"><span class="badge" style="border-color:var(--accent);color:var(--accent)">${E(kind ? kind.name : it.id)}</span>
      <input type="text" value="${E(name)}" style="width:120px" title="item name (what switch / item updates address)" onchange="pp.loSetItem(${si},${ii},'name',this.value)">
      <label style="font-size:11px">fit <select onchange="pp.loSetItem(${si},${ii},'fit',this.value)">${fits.map(f => `<option ${(it.fit || "stretch") === f ? "selected" : ""}>${f}</option>`).join("")}</select></label>
      <label style="font-size:11px">pos <input type="text" class="mono" style="width:90px" value="${E(it.pos ? it.pos.x + "," + it.pos.y : "")}" placeholder="x,y" onchange="pp.loSetItem(${si},${ii},'pos',this.value)"></label>
      <label style="font-size:11px">size <input type="text" class="mono" style="width:90px" value="${E(it.size ? it.size.width + "x" + it.size.height : "")}" placeholder="WxH" onchange="pp.loSetItem(${si},${ii},'size',this.value)"></label>
      <label class="row" style="gap:4px;font-size:11px"><input type="checkbox" ${it.visible === false ? "" : "checked"} onchange="pp.loSetItem(${si},${ii},'visible',this.checked)"> visible</label>
      <span class="fill"></span>
      <select onchange="pp.loAddFilter(${si},${ii},this.value);this.value=''"><option value="">+ filter…</option>${(LO.catalog.filters || []).map(f => `<option value="${E(f.id)}">${E(f.name)}</option>`).join("")}</select>
      <button class="ghost small" onclick="pp.loMoveItem(${si},${ii},-1)" title="Lower (drawn first)">↑</button><button class="ghost small" onclick="pp.loMoveItem(${si},${ii},1)" title="Raise (drawn last)">↓</button>
      <button class="ghost small" onclick="pp.loDelItem(${si},${ii})" title="Remove item">✕</button></div>
    <div style="margin-top:4px">${paramsForm(kind, it.settings || {}, `pp.loSetSetting(${si},${ii}`)}</div>${filt}</div>`;
}
function sceneHtml(si, sc) {
  const d = LO.draft; const open = LO.exp[si] !== false;
  return `<div class="panel" style="margin:6px 0"><h3 style="cursor:pointer" onclick="pp.loToggle(${si})">${open ? "▾" : "▸"} SCENE <input type="text" value="${E(sc.name)}" style="width:140px;text-transform:none;letter-spacing:0" onclick="event.stopPropagation()" onchange="pp.loSetScene(${si},'name',this.value)">
      ${d.spec.program === sc.name ? `<span class="badge solid" style="background:var(--ready)">program at show</span>` : `<button class="ghost small" onclick="event.stopPropagation();pp.loSetProgram('${E(sc.name)}')">make program</button>`}
      <span class="right">${(sc.items || []).length} item(s) <select onclick="event.stopPropagation()" onchange="pp.loAddItem(${si},this.value);this.value=''"><option value="">+ item…</option>${(LO.catalog.items || []).map(k => `<option value="${E(k.id)}">${E(k.name)}</option>`).join("")}</select>
      <button class="ghost small" onclick="event.stopPropagation();pp.loDelScene(${si})" title="Remove scene">✕</button></span></h3>
    ${open ? `<div class="body" style="padding:6px 10px">${(sc.items || []).map((it, ii) => itemHtml(si, ii, it)).join("") || `<span style="color:var(--t3)">empty scene (black)</span>`}</div>` : ""}</div>`;
}
function transitionForm() {
  const d = LO.draft; const t = d.spec.transition || { id: "cut_transition" }; const kind = kindOf("transitions", t.id);
  const settings = Object.assign({}, t.settings || {}); if (t.durationMs !== undefined) settings.durationMs = t.durationMs;
  return `<div class="row" style="gap:10px"><label>transition <select onchange="pp.loSetTransition(this.value)">${(LO.catalog.transitions || []).map(k => `<option value="${E(k.id)}" ${k.id === t.id ? "selected" : ""}>${E(k.name)}</option>`).join("")}</select></label>
    ${paramsForm(kind, settings, "pp.loSetTransitionParam(")}</div>`;
}
function editorHtml() {
  const d = ensureDraft(); const rec = current();
  const head = `<div class="row" style="gap:10px"><label>name <input type="text" value="${E(d.name)}" style="width:180px" onchange="pp.loSetRec('name',this.value)"></label>
    <label>id <input type="text" class="mono" value="${E(d.id)}" style="width:140px" placeholder="from name" onchange="pp.loSetRec('id',this.value)"></label>
    <label>layer <input type="number" min="1" max="99" value="${E(d.layer || 50)}" style="width:60px" onchange="pp.loSetRec('layer',this.value)"></label>
    <label class="row" style="gap:4px"><input type="checkbox" ${d.spec.audio === false ? "" : "checked"} onchange="pp.loSetSpec('audio',this.checked)"> audio</label>
    <span class="fill"></span><button class="ghost small ${LO.json ? "active" : ""}" onclick="pp.loJson()">JSON</button>
    <button class="small" onclick="pp.loValidate()">Validate</button><button class="primary small" ${LO.dirty ? "" : "disabled"} onclick="pp.loSave()">Save</button>
    ${rec ? `<button class="ghost small" onclick="pp.loDelete('${E(rec.id)}')">Delete</button>` : ""}</div>`;
  const body = LO.json
    ? `<textarea class="mono" style="width:100%;height:360px;margin-top:8px" onchange="pp.loJsonText(this.value)">${E(LO.jsonText)}</textarea><div style="color:var(--t3);font-size:10px">spec only (scenes / program / transition); the form view re-reads it</div>`
    : `<div style="margin-top:6px">${(d.spec.scenes || []).map((sc, si) => sceneHtml(si, sc)).join("")}<button class="small" onclick="pp.loAddScene()">+ scene</button></div><div style="margin-top:8px">${transitionForm()}</div>`;
  return `<div class="panel"><h3>SCENE EDITOR ${LO.dirty ? `<span class="badge" style="border-color:var(--standby);color:var(--standby)">unsaved</span>` : ""}<span class="right">${E(LO.err)}</span></h3><div class="body">${head}${body}</div></div>`;
}

// ---------------------------------------------------------------------------------------------------
// Status + control
// ---------------------------------------------------------------------------------------------------
function statusHtml(ch, st, compact) {
  if (!ch) return `<span style="color:var(--t3)">pick a channel</span>`;
  st = st || LO.status[ch] || {};
  const obs = st.obs || {}; const on = !!st.onAir;
  const dot = `<span class="dot${on ? "" : " off"}"></span>`;
  const prog = obs.program || (st.shown && st.shown.program) || "";
  const line1 = on ? `${dot} <b>ON</b> layer ${st.layer} · program <b>${E(prog)}</b>${obs.transition ? " · " + E(obs.transition) : ""}` : `${dot} off${st.note ? ` <span style="color:var(--t3)">· ${E(st.note)}</span>` : ""}${/^(OFFLINE|UNREACHABLE|ERROR|STARTING)$/.test(st.engineState || "") ? ` <span style="color:var(--t3)">· engine ${E(st.engineState)}</span>` : ""}`;
  if (compact) return line1;
  const line2 = on ? `<div class="mono" style="font-size:11px;color:var(--t2)">age p50 ${fmtMs(obs.ageP50Ms)} · p99 ${fmtMs(obs.ageP99Ms)} · max ${fmtMs(obs.ageMaxMs)} · obs ${obs.obsFps ? (Math.round(obs.obsFps * 100) / 100) + " fps" : "–"} · render ${fmtMs(obs.obsFrameTimeMs)} · lagged ${obs.obsLaggedFrames ?? "–"} · switches ${obs.switches ?? 0}${obs.switchDispatchMs !== undefined ? " (last " + fmtMs(obs.switchDispatchMs) + ")" : ""} · libobs ${E(obs.version || "")}</div>` : "";
  return `<div>${line1}</div>${line2}`;
}
function sceneNamesOnAir(ch) {
  const st = LO.status[ch] || {}; const obs = st.obs || {};
  if (Array.isArray(obs.scenes) && obs.scenes.length) return obs.scenes;
  const rec = st.shown && st.shown.sceneId ? LO.scenes.find(s => s.id === st.shown.sceneId) : null;
  return rec ? (rec.spec.scenes || []).map(s => s.name) : (LO.draft ? (LO.draft.spec.scenes || []).map(s => s.name) : []);
}
function controlHtml(ch) {
  const st = LO.status[ch] || {}; const names = sceneNamesOnAir(ch); const d = LO.draft;
  if (!LO.sw.to || !names.includes(LO.sw.to)) LO.sw.to = names[0] || "";
  const trs = (LO.catalog && LO.catalog.transitions) || [];
  return `<div class="panel"><h3>ON AIR · ${E(ch || "no channel")}<span class="right">${LO.busy ? "…" : ""}</span></h3><div class="body">
    ${statusHtml(ch)}
    <div class="row" style="margin-top:8px;gap:8px">
      <button class="success small" ${ch && d ? "" : "disabled"} onclick="pp.loShow()" title="${LO.dirty ? "shows the unsaved draft" : "shows the selected saved scene set"}">Show ${LO.dirty ? "draft" : E(d ? d.name : "")} on ${E(ch)}</button>
      <span class="fill"></span>
      <label>to <select onchange="LO.sw.to=this.value">${names.map(n => `<option ${n === LO.sw.to ? "selected" : ""}>${E(n)}</option>`).join("")}</select></label>
      <label>via <select onchange="LO.sw.transition=this.value"><option value="">(scene set's)</option>${trs.map(t => `<option value="${E(t.id)}" ${t.id === LO.sw.transition ? "selected" : ""}>${E(t.name)}</option>`).join("")}</select></label>
      <label>ms <input type="number" style="width:70px" value="${LO.sw.durationMs}" onchange="LO.sw.durationMs=parseInt(this.value)||0"></label>
      <button class="primary small" ${st.onAir ? "" : "disabled"} onclick="pp.loSwitch()">Switch</button>
      <button class="danger small" ${st.onAir || (st.shown) ? "" : "disabled"} onclick="pp.loHide()">Hide</button></div>
    <div style="color:var(--t3);font-size:10px;margin-top:6px">one libobs layer per engine; Hide removes the layer so the next Show can open. A cut lands 2 frames after the command; the picture is 1–2 frames old (docs/libobs.md).</div>
  </div></div>`;
}

export function viewLibObs() {
  const S = pp().S; const ch = S.active;
  if (!LO.ready) return `<div class="panel"><h3>OBS SCENES</h3><div class="body">${LO.err ? E(LO.err) : "loading…"}</div></div>`;
  const list = LO.scenes.map(s => `<tr class="${s.id === LO.sel ? "onair" : ""}" style="cursor:pointer" onclick="pp.loSelect('${E(s.id)}')"><td><b>${E(s.name)}</b><div class="mono" style="color:var(--t3);font-size:10px">${E(s.id)}</div></td>
      <td>${(s.spec.scenes || []).map(x => E(x.name)).join(", ")}</td><td class="mono">${E((s.spec.transition || {}).id || "cut")}</td><td>${s.layer || 50}</td>
      <td><button class="success small" ${ch ? "" : "disabled"} onclick="event.stopPropagation();pp.loShowSaved('${E(s.id)}')">Show</button></td></tr>`).join("");
  return `<div class="grid" style="gap:12px">
    ${controlHtml(ch)}
    <div class="grid cols2" style="gap:12px;align-items:start">
      <div class="panel"><h3>SAVED SCENE SETS (${LO.scenes.length})<span class="right"><button class="small" onclick="pp.loNew()">+ New</button><button class="ghost small" onclick="pp.loReload()">&#8635;</button></span></h3>
        <div class="body" style="padding:0">${LO.scenes.length ? `<table><tr><th>name</th><th>scenes</th><th>transition</th><th>layer</th><th></th></tr>${list}</table>` : `<div style="padding:12px;color:var(--t3)">none yet: New, build a scene set, Save</div>`}</div></div>
      ${editorHtml()}
    </div></div>`;
}

// ---------------------------------------------------------------------------------------------------
// The compact strip (Now Playing dashboard). HTML only; the caller re-renders on its own tick, so the strip reads cached
// status and keeps its own 2 s poll while entered.
// ---------------------------------------------------------------------------------------------------
function libobsCard(ch, st, names, cur) {
  const chan = (pp().S.channels || []).find(c => c.profile && c.profile.id === ch), up = !!(chan && chan.process && chan.process.connected);
  const scenes = st.onAir && names.length ? `<div class="np-obs-scenes">${names.map(n => `<button class="small${n === cur ? " danger" : ""}" ${n === cur ? "disabled" : ""} onclick="pp.loStripSwitch(${jq(ch)},${jq(n)})" title="${n === cur ? "on program now" : "switch with the scene set's transition"}">${E(n)}</button>`).join("")}</div>` : "";
  const sets = st.onAir ? "" : LO.scenes.length
    ? `<div class="np-obs-sets">${LO.scenes.slice(0, 8).map(s => `<button class="small" ${up ? "" : "disabled"} onclick="pp.loShowSaved(${jq(s.id)},${jq(ch)})" title="show this scene set on ${E(ch)} (layer ${s.layer || 50})">&#9654; ${E(s.name)}</button>`).join("")}${LO.scenes.length > 8 ? `<button class="ghost small" onclick="pp.nav('libobs')">+${LO.scenes.length - 8} more</button>` : ""}</div>`
    : `<div class="np-obs-sets" style="color:var(--t3);font-size:11px">${LO.scenesLoaded ? "No saved scene sets yet: open Scene sets, build one and Save." : "Loading scene sets…"}</div>`;
  return `<div class="np-obs-card" data-libobs-strip="${E(ch)}"><div class="row" style="gap:8px;font-size:12px"><span class="badge" style="border-color:var(--accent);color:var(--accent)">OBS</span>${statusHtml(ch, st, true)}<span class="fill"></span>${st.onAir || st.shown ? `<button class="danger small" onclick="pp.loStripHide(${jq(ch)})">Hide</button>` : ""}</div>${scenes}${sets}</div>`;
}
/** Compact "OBS" strip for a channel: status line + Switch (next scene) / Hide. With `full` it is the Now Playing card instead: one
 *  button per scene of the program on air (the live one is marked), Hide, and, while nothing is on air, one Show button per saved
 *  scene set. Returns HTML; call libobsStripEnter(ch, full) once when the embedding view opens and libobsStripLeave() when it closes. */
export function libobsStrip(channelId, full) {
  const ch = channelId || pp().S.active; const st = LO.status[ch] || {};
  const names = sceneNamesOnAir(ch); const cur = (st.obs && st.obs.program) || ""; const next = names.length ? names[(Math.max(0, names.indexOf(cur)) + 1) % names.length] : "";
  if (full) return libobsCard(ch, st, names, cur);
  return `<div class="row" data-libobs-strip="${E(ch)}" style="gap:8px;font-size:12px"><span class="badge" style="border-color:var(--accent);color:var(--accent)">OBS</span>${statusHtml(ch, st, true)}<span class="fill"></span>
    ${st.onAir && next ? `<button class="small" onclick="pp.loStripSwitch(${jq(ch)},${jq(next)})" title="Switch with the scene set's transition">→ ${E(next)}</button>` : ""}
    ${st.onAir || st.shown ? `<button class="danger small" onclick="pp.loStripHide(${jq(ch)})">Hide</button>` : `<button class="ghost small" onclick="pp.nav('libobs')">Scenes…</button>`}</div>`;
}
export function libobsStripEnter(channelId, full) {
  libobsStripLeave(); LO.stripChannel = channelId || pp().S.active; LO.stripFull = !!full;
  if (!LO.scenesLoaded) pp().GET("/libobs/scenes").then(s => { LO.scenes = s.scenes || []; LO.scenesLoaded = true; }).catch(() => {});
  const tick = async () => { await loadStatus(LO.stripChannel); const el = document.querySelector(`[data-libobs-strip="${cssq(LO.stripChannel)}"]`); if (el) el.outerHTML = libobsStrip(LO.stripChannel, LO.stripFull); };
  tick(); LO.stripTimer = setInterval(tick, 2000);
}
export function libobsStripLeave() { if (LO.stripTimer) { clearInterval(LO.stripTimer); LO.stripTimer = null; } LO.stripChannel = ""; LO.stripFull = false; }

// ---------------------------------------------------------------------------------------------------
// Handlers (window.pp.*)
// ---------------------------------------------------------------------------------------------------
async function post(path, body) { LO.busy = true; pp().render(); try { return await pp().POST(path, body); } finally { LO.busy = false; } }
function parseSetting(type, v) {
  if (type === "bool") return !!v; if (type === "int") return Math.round(num(v)); if (type === "float") return num(v);
  if (type === "font" || type === "paths") { try { return JSON.parse(v); } catch { return v; } }
  if (type === "enum" && /^-?\d+$/.test(String(v))) return parseInt(v, 10);
  return v;
}
export const ppExtLibObs = {
  async loApplyKey(si, ii, fi) { const scene = LO.draft?.spec?.scenes?.[si], item = scene?.items?.[ii], filter = item?.filters?.[fi], ch = pp().S.active; if (!ch || !filter) return; try { await pp().POST('/channels/'+encodeURIComponent(ch)+'/libobs/filter', {layer:LO.draft.layer || 50, scene:scene.name, item:item.name, filter:clone(filter)}); toast('Live key update sent; Save retains the settings', 'ok'); } catch(e) { toast(e.message || 'Live key update failed', 'err'); } },
  loReload() { loadAll(); },
  loSelect(id) { if (LO.dirty && !confirm("Discard the unsaved draft?")) return; LO.sel = id; draftFrom(current()); pp().render(); },
  loNew() { if (LO.dirty && !confirm("Discard the unsaved draft?")) return; LO.sel = ""; draftFrom(null); LO.dirty = true; pp().render(); },
  loToggle(si) { LO.exp[si] = LO.exp[si] === false; pp().render(); },
  loJson() { if (LO.json) { try { LO.draft.spec = JSON.parse(LO.jsonText); LO.err = ""; } catch (e) { LO.err = "JSON: " + e.message; return pp().render(); } } else LO.jsonText = JSON.stringify(LO.draft.spec, null, 2); LO.json = !LO.json; pp().render(); },
  loJsonText(t) { LO.jsonText = t; LO.dirty = true; },
  loSetRec(k, v) { LO.draft[k] = k === "layer" ? Math.max(1, Math.min(99, parseInt(v) || 50)) : v; touch(); },
  loSetSpec(k, v) { LO.draft.spec[k] = v; touch(); },
  loSetScene(si, k, v) { const sc = LO.draft.spec.scenes[si]; if (k === "name" && LO.draft.spec.program === sc.name) LO.draft.spec.program = v; sc[k] = v; touch(); },
  loSetProgram(name) { LO.draft.spec.program = name; touch(); },
  loAddScene() { const n = LO.draft.spec.scenes.length; LO.draft.spec.scenes.push({ name: String.fromCharCode(65 + (n % 26)) + (n >= 26 ? Math.floor(n / 26) : ""), items: [] }); touch(); },
  loDelScene(si) { const sc = LO.draft.spec.scenes.splice(si, 1)[0]; if (LO.draft.spec.program === sc.name) LO.draft.spec.program = (LO.draft.spec.scenes[0] || {}).name || ""; touch(); },
  loAddItem(si, kindId) { if (!kindId) return; const k = kindOf("items", kindId); const sc = LO.draft.spec.scenes[si]; sc.items = sc.items || []; sc.items.push({ id: kindId, name: (k ? k.name.toLowerCase().replace(/[^a-z0-9]+/g, "_") : kindId) + "_" + (sc.items.length + 1), settings: defaultsFor(k) }); touch(); },
  loDelItem(si, ii) { LO.draft.spec.scenes[si].items.splice(ii, 1); touch(); },
  loMoveItem(si, ii, d) { const a = LO.draft.spec.scenes[si].items; const j = ii + d; if (j < 0 || j >= a.length) return; [a[ii], a[j]] = [a[j], a[ii]]; touch(); },
  loSetItem(si, ii, k, v) {
    const it = LO.draft.spec.scenes[si].items[ii];
    if (k === "pos") { const m = String(v).split(/[ ,x]+/); if (m.length >= 2 && m[0] !== "") it.pos = { x: num(m[0]), y: num(m[1]) }; else delete it.pos; }
    else if (k === "size") { const m = String(v).split(/[ ,x]+/); if (m.length >= 2 && m[0] !== "") it.size = { width: num(m[0]), height: num(m[1]) }; else delete it.size; }
    else if (k === "visible") { if (v) delete it.visible; else it.visible = false; }
    else it[k] = v;
    touch();
  },
  loSetSetting(si, ii, key, type, v) { const it = LO.draft.spec.scenes[si].items[ii]; it.settings = it.settings || {}; it.settings[key] = parseSetting(type, v); LO.dirty = true; },
  loAddFilter(si, ii, fid) { if (!fid) return; const it = LO.draft.spec.scenes[si].items[ii]; it.filters = it.filters || []; it.filters.push({ id: fid, settings: defaultsFor(kindOf("filters", fid)) }); touch(); },
  loDelFilter(si, ii, fi) { LO.draft.spec.scenes[si].items[ii].filters.splice(fi, 1); touch(); },
  loSetFilter(si, ii, fi, key, type, v) { const f = LO.draft.spec.scenes[si].items[ii].filters[fi]; f.settings = f.settings || {}; f.settings[key] = parseSetting(type, v); LO.dirty = true; },
  loKeyColor(si, ii, fi, color) { if (!/^#[0-9a-f]{6}$/i.test(String(color))) return; const f = LO.draft?.spec?.scenes?.[si]?.items?.[ii]?.filters?.[fi]; if (!f || !["chroma_key_filter_v2", "color_key_filter_v2"].includes(f.id)) return; f.settings = f.settings || {}; f.settings.key_color_type = "custom"; f.settings.key_color = color; touch(); },
  async loPickKeyColor(si, ii, fi) { if (typeof window.EyeDropper !== "function") { toast("Eyedropper is unavailable; use the color picker", "err"); return; } const draft = LO.draft, filter = draft?.spec?.scenes?.[si]?.items?.[ii]?.filters?.[fi]; try { const result = await new window.EyeDropper().open(); if (LO.draft === draft && draft?.spec?.scenes?.[si]?.items?.[ii]?.filters?.[fi] === filter) pp().loKeyColor(si, ii, fi, result.sRGBHex); } catch (e) { if (e.name !== "AbortError") toast("Could not sample the screen color", "err"); } },
  loSetTransition(id) { const k = kindOf("transitions", id); const t = { id }; const d = defaultsFor(k); if (d.durationMs !== undefined) { t.durationMs = d.durationMs; delete d.durationMs; } if (Object.keys(d).length) t.settings = d; LO.draft.spec.transition = t; touch(); },
  loSetTransitionParam(key, type, v) { const t = LO.draft.spec.transition; if (key === "durationMs") t.durationMs = Math.round(num(v)); else { t.settings = t.settings || {}; t.settings[key] = parseSetting(type, v); } LO.dirty = true; },
  async loValidate() { try { const r = await pp().POST("/libobs/validate", { spec: LO.json ? JSON.parse(LO.jsonText) : LO.draft.spec, layer: LO.draft.layer }); LO.err = r.ok ? "" : r.error; toast(r.ok ? "Valid: " + r.spec.scenes.length + " scene(s), program " + r.spec.program : r.error, r.ok ? "ok" : "err"); } catch (e) { LO.err = String(e.message || e); } pp().render(); },
  async loSave() {
    if (LO.json) { try { LO.draft.spec = JSON.parse(LO.jsonText); } catch (e) { LO.err = "JSON: " + e.message; return pp().render(); } }
    try { const r = await post("/libobs/scenes", LO.draft); LO.sel = r.scene.id; LO.dirty = false; LO.err = ""; toast("Saved " + r.scene.name, "ok"); await loadAll(); draftFrom(current()); } catch (e) { LO.err = String(e.message || e); } pp().render();
  },
  async loDelete(id) { if (!confirm("Delete scene set " + id + "?")) return; try { await pp().api("DELETE", "/libobs/scenes/" + encodeURIComponent(id)); LO.sel = ""; LO.draft = null; await loadAll(); draftFrom(current()); } catch {} pp().render(); },
  async loShow() {
    const ch = pp().S.active; if (!ch || !LO.draft) return;
    const body = LO.dirty || !LO.sel ? { spec: LO.json ? JSON.parse(LO.jsonText) : LO.draft.spec, layer: LO.draft.layer } : { sceneId: LO.sel };
    try { const r = await post("/channels/" + encodeURIComponent(ch) + "/libobs/show", body); toast("Show accepted on " + ch + " (layer " + r.layer + ")", "ok"); } catch {} setTimeout(() => loadStatus(ch).then(() => pp().render()), 1200); pp().render();
  },
  async loShowSaved(id, chArg) { const ch = chArg || pp().S.active; if (!ch) return; try { const r = await post("/channels/" + encodeURIComponent(ch) + "/libobs/show", { sceneId: id }); toast("Show accepted on " + ch + " (layer " + r.layer + ")", "ok"); } catch {} setTimeout(() => loadStatus(ch).then(() => pp().render()), 1200); pp().render(); },
  async loSwitch() { const ch = pp().S.active; if (!ch || !LO.sw.to) return; const b = { to: LO.sw.to }; if (LO.sw.transition) { b.transition = LO.sw.transition; b.durationMs = LO.sw.durationMs; } try { await post("/channels/" + encodeURIComponent(ch) + "/libobs/switch", b); toast("Switch to " + LO.sw.to, "ok"); } catch {} setTimeout(() => loadStatus(ch).then(() => pp().render()), 400); pp().render(); },
  async loHide() { const ch = pp().S.active; if (!ch) return; try { await post("/channels/" + encodeURIComponent(ch) + "/libobs/hide", {}); toast("libobs layer removed from " + ch, "ok"); } catch {} setTimeout(() => loadStatus(ch).then(() => pp().render()), 400); pp().render(); },
  async loStripSwitch(ch, to) { try { await pp().POST("/channels/" + encodeURIComponent(ch) + "/libobs/switch", { to }); toast("Switch to " + to, "ok"); } catch {} setTimeout(() => loadStatus(ch).then(() => pp().render()), 400); },
  async loStripHide(ch) { try { await pp().POST("/channels/" + encodeURIComponent(ch) + "/libobs/hide", {}); toast("libobs layer removed", "ok"); } catch {} setTimeout(() => loadStatus(ch).then(() => pp().render()), 400); },
};
