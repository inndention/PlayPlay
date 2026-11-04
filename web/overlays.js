/* PLAY PLAY web dashboard — Overlays console: the HTML CG pack (web/cg) on air through the Supervisor's overlay
   manager. Live layer board (update / re-take / hide / delete, right-click menus everywhere), template gallery with
   scaled live previews, a deploy form generated from the manifest, hotkey presets, always-on defaults, suppress/show
   rules, per-item cues and the renderer status. Talks to /api/v1/overlays/* and follows the "overlays" WS event.

   Previews: app.js rebuilds #view with innerHTML on every WS tick and a re-inserted <iframe> reloads, so the live
   previews cannot live inside the view. They live in a persistent dock (#ovDock, appended to <body>, pointer-events
   off) and each ".ovSlot" placeholder in the view tells syncDock() where its iframe goes.

   Menus: every row, card and list in this view is tagged data-ctx="ov…" and its menu is built by a registerCtx() builder
   at the bottom of the file (web/ctx.js appends Copy id / Copy as JSON / Properties… and the edit menu on text fields). */

import { ctxMenu, registerCtx, showProps, copyText, copyJson, readClipboard } from "./ctx.js";

const OV = {
  ready: false, loading: false, down: "", tab: "deploy",
  renderer: {}, channels: {}, presets: [], rules: [], man: { templates: [], animations: [], presets: [] },
  // deploy form (also the editor for presets / defaults: `editing` says which one it writes back to)
  cat: "", q: "", sel: "", params: {}, anim: { in: "fade", out: "fade", duration: 600, outDuration: 400, easing: "ease-out" },
  role: "", slot: 0, hold: 0, mode: "auto", label: "", editing: null, stageUrl: "", nonce: 0, live: true,
  open: {},                                   // layer key "ch:layer" -> draft params of its inline update row
  clearDefs: false,                           // the board's Clear also takes the always-on defaults down
  ruleEdit: null, ruleTest: { channel: "", mediaId: "", res: null, busy: false },
  pillar: { st: null, draft: null, busy: false, err: "", aspect: "0.5625", loadedAt: 0 },
  cg: { slots: [], inputs: 8, names: [], loaded: false, err: "" },     // CG slots tab (GET/PUT /api/v1/overlays/cgslots)   // pillar panels tab (GET/PUT /api/v1/pillar*)
  cues: { items: [], total: 0, slot: "", at: 0, loading: false, err: "", sv: null, svErr: "" },
  rtest: null, rtestBusy: false, rpath: null, busy: {}, clearDefs: false, sc: {}, caret: null,
  dock: null, pvs: new Map(), maxLive: 8, poll: null, tick: null, hooked: false, mediaAsked: false,
};
window.OV = OV;
try { OV.live = localStorage.getItem("pp.ov.live") !== "0"; } catch {}

const ROLES = ["lower_third", "bug", "ticker", "secondary", "legal_id", "emergency", "pillar"];
const MODES = ["auto", "html", "native"];
const MEDIA_TYPES = ["program", "ad", "promo", "station_id", "bumper", "filler", "psa", "news", "weather", "image", "live"];
const DEF_ANIMS = ["none", "fade", "slide-left", "slide-right", "slide-up", "slide-down", "wipe-left", "wipe-right", "bounce", "zoom", "flip", "typewriter"];
const EASINGS = ["ease-out", "ease-in", "ease-in-out", "linear", "cubic-bezier(0.34,1.56,0.64,1)"];
const TOKENS = ["{{title}}", "{{series}}", "{{client}}", "{{category}}", "{{first_aired}}"];
const RESERVED_KEYS = ["ctrl+=", "ctrl+-", "ctrl+0", "ctrl+c", "ctrl+v", "ctrl+x", "ctrl+a", "ctrl+r", "ctrl+w", "ctrl+t", "ctrl+n", "ctrl+f", "ctrl+p", "ctrl+s", "f5", "f11", "f12", "space", "enter"];

// ---------------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------------
const E = s => String(s ?? "").replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
const jsq = v => E(JSON.stringify(String(v ?? "")));          // a value as a JS string literal inside an inline handler attribute
const pad2 = n => (n < 10 ? "0" : "") + n;
const iso = d => d.getFullYear() + "-" + pad2(d.getMonth() + 1) + "-" + pad2(d.getDate());
const clone = o => (o === undefined ? undefined : JSON.parse(JSON.stringify(o)));
const num = (v, d = 0) => { const n = parseFloat(v); return Number.isFinite(n) ? n : d; };
const sval = v => (v === undefined || v === null ? "" : typeof v === "object" ? JSON.stringify(v) : String(v));
const trunc = (s, n) => { s = String(s ?? ""); return s.length > n ? s.slice(0, n - 1) + "…" : s; };
const uid = p => p + Date.now().toString(36) + Math.floor(Math.random() * 1296).toString(36);
const enc = encodeURIComponent;
const bdg = (text, color) => `<span class="badge" style="color:${color};border-color:${color};background:color-mix(in srgb,${color} 16%,transparent)">${E(text)}</span>`;
const fmtAge = ms => { let s = Math.max(0, Math.floor(ms / 1000)); const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60); s %= 60; return (h ? h + ":" + pad2(m) : m) + ":" + pad2(s); };
const listOf = (v, key) => (Array.isArray(v) ? v : v && Array.isArray(v[key]) ? v[key] : v && Array.isArray(v.items) ? v.items : v && Array.isArray(v._array) ? v._array : []);
const toast = (m, k) => pp.toast(m, k);
const hex6 = v => { v = String(v || "").replace("#", ""); if (/^[0-9a-f]{3}$/i.test(v)) v = v.split("").map(c => c + c).join(""); return /^[0-9a-f]{6}$/i.test(v) ? "#" + v.toLowerCase() : "#00d4ff"; };
// Layer states of the overlay manager: pending -> on_air -> hiding -> (removed); suppressed (a rule took it down), hidden (a default the operator hid), lost, failed.
const stateColor = s => ({ on_air: "var(--ready)", shown: "var(--ready)", hidden: "var(--t3)", suppressed: "var(--accent2)", lost: "var(--danger)", failed: "var(--danger)", error: "var(--danger)", pending: "var(--warn)", hiding: "var(--standby)" }[s] || "var(--t2)");
const isUp = s => s === "on_air" || s === "shown" || s === "pending";   // a state a Hide still means something for
const engColor = s => (s === "running" || s === "ready" || s === "ok" ? "var(--ready)" : s === "error" || s === "failed" || s === "missing" ? "var(--danger)" : s ? "var(--warn)" : "var(--t3)");

const chanList = () => (pp.S.channels || []).filter(c => c.profile && !(c.profile.extra && c.profile.extra.hidden));
const chName = id => { const c = (pp.S.channels || []).find(x => x.profile && x.profile.id === id); return c ? c.profile.name || id : id; };
const cur = () => pp.S.active || ((chanList()[0] || {}).profile || {}).id || "";
const layersOf = ch => (((OV.channels[ch] || {}).layers) || []).slice().sort((a, b) => a.layer - b.layer);
const defaultsOf = ch => ((OV.channels[ch] || {}).defaults) || [];
const tplOf = id => OV.man.templates.find(t => t.id === id) || null;
const tplName = id => (tplOf(id) || {}).name || id || "";
const normAnim = a => { a = a || {}; return { in: a.in || "fade", out: a.out || "fade", duration: num(a.duration ?? a.inDuration, 600), outDuration: num(a.outDuration ?? a.duration, 400), easing: a.easing || "ease-out" }; };
const pval = (p, vals) => (vals[p.name] !== undefined ? sval(vals[p.name]) : sval(p.default));
// Non-empty params of a template (manifest defaults filled in, then anything extra such as pasted keys). Always strings: the native fallback reads them as strings.
// Text 1/2/3: any template takes text1..text3; the manifest's `textFields` names the template's own fields they fill (the Supervisor applies the same
// mapping on deploy / update, cgtext.h). Done here too so the live page preview shows the words. A field given explicitly is never overridden.
const textFieldsOf = t => (t && t.textFields) || [];
const textLabel = (t, f) => ((t.params || []).find(p => p.name === f) || {}).label || f;
const paramsOf = (t, vals) => {
  vals = vals || {}; const o = {};
  for (const p of t.params || []) { const v = pval(p, vals); if (v !== "") o[p.name] = v; }
  for (const [k, v] of Object.entries(vals)) if (!(t.params || []).some(p => p.name === k) && sval(v) !== "") o[k] = sval(v);
  textFieldsOf(t).forEach((f, i) => { const v = sval(vals["text" + (i + 1)]); if (v !== "" && sval(vals[f]) === "") o[f] = v; });
  return o;
};
const textRows = (t, vals, idp, set) => {
  const tf = textFieldsOf(t); if (!tf.length) return "";
  return `<div class="fh" style="font-size:10px;letter-spacing:1px;color:var(--t3);font-weight:700;margin:6px 0 4px">TEXT 1 · 2 · 3 <span style="font-weight:400;letter-spacing:0">(fill this template's own text fields)</span></div>
    <div class="ovForm">${tf.map((f, i) => `<label class="f">Text ${i + 1} <span class="mu sm">&rarr; ${E(textLabel(t, f))}</span><input id="${E(idp)}${i + 1}" value="${E(sval((vals || {})["text" + (i + 1)]))}" placeholder="${E(sval(((t.params || []).find(p => p.name === f) || {}).default))}" oninput="${set("text" + (i + 1), "this.value")}"></label>`).join("")}</div>`;
};
const summary = params => Object.entries(params || {}).filter(([k, v]) => sval(v) !== "" && !/^(color|colou?r\d?|logo|image|url)$/i.test(k)).slice(0, 3).map(([, v]) => trunc(sval(v), 26)).join(" · ");

// Same query vocabulary as the Supervisor's cgUrlFor (supervisorcore_integrations.cpp) and js/cg-animations.js.
function cgUrl(t, vals, anim) {
  const q = []; for (const [k, v] of Object.entries(paramsOf(t, vals))) q.push(enc(k) + "=" + enc(v));
  const a = normAnim(anim);
  if (a.in && a.in !== "none") q.push("animIn=" + enc(a.in), "animInDuration=" + a.duration);
  if (a.out && a.out !== "none") q.push("animOut=" + enc(a.out), "animOutDuration=" + a.outDuration);
  if (a.easing && a.easing !== "ease-out") q.push("animEasing=" + enc(a.easing));
  return "/cg/" + enc(t.id) + ".html" + (q.length ? "?" + q.join("&") : "");
}
const thumbUrl = t => cgUrl(t, {}, { in: "none", out: "none" });
const absUrl = rel => (typeof location !== "undefined" ? location.origin : "") + rel;

// Hotkeys: "1", "F2", "ctrl+3", "alt+x". Shift is not part of the key: Shift + hotkey HIDES the preset.
const MODS = { control: "ctrl", ctrl: "ctrl", cmd: "ctrl", meta: "ctrl", command: "ctrl", alt: "alt", option: "alt" };
function normHotkey(s) {                                    // "" for none, null for invalid
  s = String(s || "").trim(); if (!s) return "";
  let ctrl = false, alt = false, key = "";
  for (const p of s.split("+").map(x => x.trim().toLowerCase()).filter(Boolean)) { if (MODS[p] === "ctrl") ctrl = true; else if (MODS[p] === "alt") alt = true; else if (p !== "shift") key = p; }
  if (!/^(f([1-9]|1[0-2])|[a-z0-9]|up|down|left|right|home|end|pageup|pagedown|insert|[`\-=\[\];',./\\])$/.test(key)) return null;
  return (ctrl ? "ctrl+" : "") + (alt ? "alt+" : "") + key;
}
const hkDisp = h => String(h || "").split("+").map(p => (p.length > 1 ? p[0].toUpperCase() + p.slice(1) : p.toUpperCase())).join("+");
function comboOf(e) {
  const c = e.code || ""; let key = "";
  if (/^Digit\d$/.test(c)) key = c.slice(5); else if (/^Numpad\d$/.test(c)) key = c.slice(6); else if (/^Key[A-Z]$/.test(c)) key = c.slice(3).toLowerCase();
  else { const k = e.key || ""; if (!k || ["Control", "Shift", "Alt", "Meta", "Dead", "Unidentified"].includes(k)) return ""; key = k === " " ? "space" : k.length === 1 ? k.toLowerCase() : k.toLowerCase().replace("arrow", ""); }
  return (e.ctrlKey || e.metaKey ? "ctrl+" : "") + (e.altKey ? "alt+" : "") + key;
}

// Clipboard: copying puts the thing on the in-app clipboard (pp.S.clip, kinds "overlay" | "preset" | "default" | "rule") AND on the OS
// clipboard as JSON; pasting takes the in-app one when its kind fits, else parses the OS clipboard (ctx.js asks in a prompt when the
// browser blocks clipboard reads). A paste item is disabled, not hidden, when the in-app clipboard holds something incompatible.
const SPEC_KINDS = ["overlay", "preset", "default"];
const setClipK = (kind, data, label) => { pp.S.clip = { kind, data: clone(data), label: label || kind }; return copyJson(data, label || kind); };
const clipFits = kinds => !pp.S.clip || kinds.includes(pp.S.clip.kind);
async function readClip(kinds, check) {
  const c = pp.S.clip; if (c && kinds.includes(c.kind)) return clone(c.data);
  const txt = await readClipboard(); if (!txt || !String(txt).trim()) return null;
  try { let o = JSON.parse(txt); if (Array.isArray(o)) o = o[0]; if (o && typeof o === "object" && check(o)) return o; } catch {}
  toast("The clipboard does not hold " + (kinds[0] === "rule" ? "a rule" : "an overlay") + " JSON", "err"); return null;
}
const readSpecClip = () => readClip(SPEC_KINDS, o => !!(o.template || o.templateId || o.params));
const readRuleClip = () => readClip(["rule"], o => !!(o.when || o.then));
const asSpec = o => (o.params && typeof o.params === "object" && !Array.isArray(o.params)) || o.template || o.templateId ? o : { params: o };

// ---------------------------------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------------------------------
async function quiet(method, path, body) {                    // background reads: no toast, the caller decides what an error means
  const r = await fetch("/api/v1" + path, { method, headers: { "Content-Type": "application/json", ...(pp.S.token ? { Authorization: "Bearer " + pp.S.token } : {}) }, body: body !== undefined ? JSON.stringify(body) : undefined });
  const t = await r.text(); let d; try { d = JSON.parse(t); } catch { d = t; }
  if (!r.ok) { const e = new Error((d && d.error) || "HTTP " + r.status); e.status = r.status; throw e; }
  return d;
}
function setManifest(m) {
  const man = Array.isArray(m) ? { templates: m } : m || {};
  OV.man = { templates: man.templates || [], animations: man.animations && man.animations.length ? man.animations : DEF_ANIMS, presets: man.presets || [] };
}
function applyOverview(d) {
  OV.renderer = d.renderer || OV.renderer || {};
  if (d.channels) { const next = {}; for (const [ch, c] of Object.entries(d.channels)) next[ch] = { layers: listOf(c && c.layers, "layers"), defaults: listOf(c && c.defaults, "defaults") }; OV.channels = next; }
  if (d.presets) OV.presets = listOf(d.presets, "presets");
  if (d.rules) OV.rules = listOf(d.rules, "rules");
  if (d.templates) setManifest(d.templates);
}
async function loadAll() {
  if (OV.loading) return; OV.loading = true;
  try { applyOverview(await quiet("GET", "/overlays")); OV.down = ""; }
  catch (e) {
    OV.down = e.status === 404 ? "The overlay manager is not available on this Supervisor (GET /api/v1/overlays answered 404). Update and restart the Supervisor to enable overlays." : "Overlay manager: " + (e.message || e);
    if (!OV.man.templates.length) { try { setManifest(await quiet("GET", "/cg/templates")); } catch {} }
  } finally { OV.loading = false; OV.ready = true; if (pp.S.view === "overlays") pp.render(); }
}
function refreshSoon() { clearTimeout(refreshSoon.t); refreshSoon.t = setTimeout(loadAll, 500); }
function upsertLayer(ch, rec) {
  const r = rec && typeof rec.layer === "object" && rec.layer ? rec.layer : rec; if (!r || r.layer === undefined) return;
  const c = (OV.channels[ch] = OV.channels[ch] || { layers: [], defaults: [] }); c.layers = c.layers || [];
  const i = c.layers.findIndex(x => x.layer === r.layer); if (i >= 0) c.layers[i] = r; else c.layers.push(r);
}
async function loadCues() {                                  // the current hour of the active channel: items with cgOverlays, media with cgCues
  const ch = cur(); if (!ch || OV.cues.loading) return; OV.cues.loading = true; OV.cues.err = "";
  try {
    const now = new Date(), sun = new Date(now.getFullYear(), now.getMonth(), now.getDate() - now.getDay());
    const w = await quiet("GET", "/schedule/" + enc(ch) + "/week?start=" + iso(sun)); const slots = w.slots || {}, h = now.getHours();
    const dated = slots[iso(now) + "-" + h], tpl = slots[now.getDay() + "-" + h]; const slot = dated && (dated.items || []).length ? dated : tpl || {};
    if (!(pp.S.media || []).length && !OV.mediaAsked && window.loadMedia) { OV.mediaAsked = true; window.loadMedia(); }
    const items = slot.items || [];
    OV.cues.items = items.map((it, i) => {
      const m = (pp.S.media || []).find(x => x.id === it.mediaId) || null;
      const own = Array.isArray(it.cgOverlays) ? it.cgOverlays : [], med = m && Array.isArray(m.cgCues) ? m.cgCues : [];
      return { i, it, m, cues: own.map(c => ({ c, src: "item", inDef: 0, outDef: 0 })).concat(med.map(c => ({ c, src: "media", inDef: 10, outDef: 10 }))) };
    }).filter(x => x.cues.length);
    OV.cues.total = items.length; OV.cues.slot = slot.id || (dated ? iso(now) + "-" + h : now.getDay() + "-" + h); OV.cues.at = Date.now();
    try { OV.cues.sv = await quiet("GET", "/overlays/" + enc(ch) + "/cues"); OV.cues.svErr = ""; } catch (e) { OV.cues.sv = null; OV.cues.svErr = e.status === 404 ? "" : e.message || String(e); }   // what the Supervisor will fire / has fired for the item on air
  } catch (e) { OV.cues.err = e.message || String(e); } finally { OV.cues.loading = false; if (pp.S.view === "overlays") pp.render(); }
}
// A list PUT goes out as {key: list, items: list, _array: list}: the Supervisor's routes read JSON objects and wrap a bare array body as "_array".
const putList = (path, key, list) => pp.api("PUT", path, { [key]: list, items: list, _array: list });
const respList = (r, key, fallback) => { const v = r && (Array.isArray(r) ? r : r[key] !== undefined ? r[key] : r.items !== undefined ? r.items : r._array); return Array.isArray(v) ? v : fallback; };
async function savePresets(list) { OV.presets = respList(await putList("/overlays/presets", "presets", list), "presets", list); pp.render(); return true; }
async function saveRules(list) { OV.rules = respList(await putList("/overlays/rules", "rules", list), "rules", list); pp.render(); return true; }
async function saveDefaults(ch, list) { const r = await putList("/overlays/" + enc(ch) + "/defaults", "defaults", list); const c = (OV.channels[ch] = OV.channels[ch] || { layers: [], defaults: [] }); c.defaults = respList(r, "defaults", list); pp.render(); return true; }

// ---------------------------------------------------------------------------------------------------
// Form state <-> spec
// ---------------------------------------------------------------------------------------------------
function specFromForm() {
  const t = tplOf(OV.sel); if (!t) return null;
  const s = { template: t.id, params: paramsOf(t, OV.params), animation: normAnim(OV.anim), role: OV.role || t.role || "", mode: OV.mode, holdSec: Math.max(0, num(OV.hold, 0)) };
  if (OV.slot > 0) s.slot = OV.slot; if (OV.label.trim()) s.label = OV.label.trim();
  return s;
}
function deployBody(spec) { const b = { template: spec.template, params: spec.params || {}, animation: spec.animation || normAnim(OV.anim), mode: spec.mode || "auto" }; if (spec.role) b.role = spec.role; if (spec.slot > 0) b.slot = spec.slot; if (spec.holdSec > 0) b.holdSec = spec.holdSec; if (spec.label) b.label = spec.label; return b; }
function loadSpec(spec) {
  OV.sel = spec.template || spec.templateId || OV.sel; OV.params = {}; for (const [k, v] of Object.entries(spec.params || {})) OV.params[k] = sval(v);
  OV.anim = normAnim(spec.animation); OV.role = spec.role || ""; OV.slot = num(spec.slot, 0); OV.hold = num(spec.holdSec, 0); OV.mode = spec.mode || "auto"; OV.label = spec.label || "";
  stageNow();
}
function defaultSpec(t) { return { template: t.id, params: paramsOf(t, {}), animation: normAnim(OV.anim), role: t.role || "", mode: OV.mode, holdSec: 0 }; }
function stageNow() { const t = tplOf(OV.sel); OV.stageUrl = t ? cgUrl(t, OV.params, OV.anim) : ""; }
let prevT; const previewSoon = () => { clearTimeout(prevT); prevT = setTimeout(() => { stageNow(); if (pp.S.view === "overlays") pp.render(); }, 450); };
const freeSlot = (ch, role) => { const used = new Set(layersOf(ch).filter(l => l.role === role).map(l => num(l.slot, 0))); let s = 0; while (used.has(s)) s++; return s; };

// ---------------------------------------------------------------------------------------------------
// Actions on the Supervisor
// ---------------------------------------------------------------------------------------------------
async function deployTo(chs, spec) {
  if (!spec || !spec.template) { toast("Pick a template first"); return; }
  const body = deployBody(spec); let ok = 0; const notes = [];
  OV.busy.deploy = true; pp.render();
  for (const ch of chs) {
    try { const r = await pp.api("POST", "/overlays/" + enc(ch) + "/deploy", body); upsertLayer(ch, r); ok++; notes.push(chName(ch) + " " + (r && r.layer !== undefined ? "layer " + (typeof r.layer === "object" ? r.layer.layer : r.layer) : "") + (r && r.mode ? " (" + r.mode + ")" : "")); } catch {}
  }
  OV.busy.deploy = false;
  if (ok) toast("Deployed " + tplName(spec.template) + " → " + notes.join(", "), "ok");
  pp.render(); refreshSoon();
}
const allCh = () => chanList().map(c => c.profile.id);
async function layerCall(ch, layer, op, body, msg) {
  try { const r = await pp.api("POST", "/overlays/" + enc(ch) + "/" + layer + "/" + op, body || {}); upsertLayer(ch, r); if (msg) toast(msg, "ok"); } catch {}
  pp.render(); refreshSoon();
}
async function presetFire(p, chs) { for (const ch of chs) { try { const r = await pp.api("POST", "/overlays/" + enc(ch) + "/presets/" + enc(p.id) + "/fire", {}); upsertLayer(ch, r); } catch {} } toast("Fired " + p.name + " → " + chs.map(chName).join(", "), "ok"); pp.render(); refreshSoon(); }
async function presetHide(p, chs) { for (const ch of chs) { try { await pp.api("POST", "/overlays/" + enc(ch) + "/presets/" + enc(p.id) + "/hide", {}); } catch {} } toast("Hid " + p.name, "ok"); pp.render(); refreshSoon(); }
function cueSpec(c, item, m) {                               // a cue as a deploy spec; tokens are filled from the item for the test
  const tid = c.template || c.templateId || ""; const ctx = { title: (item && item.title) || (m && m.title) || "", series: (m && (m.programSeries || m.series)) || "", client: (m && m.client) || "", category: (m && (m.category || (m.extra || {}).category)) || "", first_aired: "" };
  const fill = v => String(v).replace(/\{\{(\w+)\}\}/g, (_, k) => (ctx[k] !== undefined ? ctx[k] : ""));
  const params = {}; for (const [k, v] of Object.entries(c.params || {})) params[k] = fill(sval(v));
  return { template: tid, params, animation: normAnim(c.animation), role: c.role || (tplOf(tid) || {}).role || "", mode: c.mode || "auto", holdSec: num(c.duration, 0) > 0 ? num(c.duration, 0) : 8 };
}
const specOfLayer = l => ({ template: l.template, params: clone(l.params || {}), animation: normAnim(l.animation), role: l.role || "", mode: l.mode === "html" || l.mode === "native" ? l.mode : "auto", holdSec: 0, ...(l.slot > 0 ? { slot: l.slot } : {}), ...(l.label ? { label: l.label } : {}) });
const findLayer = (ch, layer) => layersOf(ch).find(l => l.layer === layer);

// ---------------------------------------------------------------------------------------------------
// Dock: persistent live previews (see the header comment)
// ---------------------------------------------------------------------------------------------------
const zoomNow = () => parseFloat(document.documentElement.style.zoom || "1") || 1;
function ensureDock() {
  if (OV.dock && OV.dock.isConnected) return OV.dock;
  const d = document.createElement("div"); d.id = "ovDock"; d.style.cssText = "position:fixed;left:0;top:0;width:0;height:0;z-index:2;pointer-events:none"; document.body.appendChild(d); OV.dock = d; return d;
}
function removePv(key) { const pv = OV.pvs.get(key); if (!pv) return; try { pv.wrap.remove(); } catch {} OV.pvs.delete(key); }
function dockHide() { for (const k of [...OV.pvs.keys()]) removePv(k); if (OV.dock) OV.dock.style.display = "none"; }
function createPv(key) {
  const wrap = document.createElement("div"); wrap.style.cssText = "position:absolute;overflow:hidden;pointer-events:none;opacity:0;transition:opacity .15s";
  const f = document.createElement("iframe"); f.tabIndex = -1; f.setAttribute("aria-hidden", "true"); f.setAttribute("scrolling", "no"); f.setAttribute("title", "preview");
  f.style.cssText = "position:absolute;left:0;top:0;width:1920px;height:1080px;border:0;background:transparent;transform-origin:0 0;pointer-events:none";
  const pv = { key, wrap, f, url: "", loaded: false, seen: Date.now(), nonce: -1 };
  f.addEventListener("load", () => { pv.loaded = true; wrap.style.opacity = "1"; syncDock(); });
  wrap.appendChild(f); ensureDock().appendChild(wrap); OV.pvs.set(key, pv); return pv;
}
function syncDock() {
  if (typeof document === "undefined") return;
  const view = document.getElementById("view"); const on = pp.S.view === "overlays" && view;
  if (!on) { dockHide(); return; }
  const dock = ensureDock(); dock.style.display = ""; const z = zoomNow(); dock.style.zoom = String(1 / z);   // effective zoom 1: dock px are viewport px
  const top = document.getElementById("top"), hb = top ? top.getBoundingClientRect().bottom : 0, now = Date.now();
  const vp = { l: 0, t: hb, r: window.innerWidth, b: window.innerHeight };
  const clipCache = new Map();
  const clipOf = el => {                                      // intersect with every scrolling ancestor and the viewport below the sticky header
    let c = { ...vp };
    for (let p = el.parentElement; p && p !== view.parentElement; p = p.parentElement) {
      let r = clipCache.get(p);
      if (r === undefined) { const cs = getComputedStyle(p); r = /(auto|scroll|hidden|clip)/.test(cs.overflowX + cs.overflowY) ? p.getBoundingClientRect() : null; clipCache.set(p, r); }
      if (r) c = { l: Math.max(c.l, r.left), t: Math.max(c.t, r.top), r: Math.min(c.r, r.right), b: Math.min(c.b, r.bottom) };
    }
    return c;
  };
  const vis = [];
  for (const slot of view.querySelectorAll(".ovSlot")) {
    const prio = +slot.dataset.prio || 0; if (!prio && !OV.live) { slot.classList.remove("live"); continue; }
    const r = slot.getBoundingClientRect(), c = clipOf(slot);
    if (r.width > 8 && c.r - c.l > 2 && c.b - c.t > 2 && slot.dataset.url) vis.push({ slot, r, c, prio });
    else slot.classList.remove("live");
  }
  vis.sort((a, b) => b.prio - a.prio);                        // stable: the stage first, then document order
  const use = vis.slice(0, OV.maxLive + 1), keep = new Set();
  for (const u of use) {
    const key = u.slot.dataset.pv; keep.add(key); const pv = OV.pvs.get(key) || createPv(key); pv.seen = now;
    const url = u.slot.dataset.url, nonce = +u.slot.dataset.nonce || 0;
    if (pv.url !== url) { pv.url = url; pv.loaded = false; pv.wrap.style.opacity = "0"; pv.f.src = url; }
    else if (pv.nonce !== nonce && pv.nonce !== -1) { pv.loaded = false; pv.wrap.style.opacity = "0"; try { pv.f.contentWindow.location.reload(); } catch { pv.f.src = url; } }
    pv.nonce = nonce;
    const w = u.r.width, h = u.r.height; Object.assign(pv.wrap.style, { left: u.r.left + "px", top: u.r.top + "px", width: w + "px", height: h + "px", clipPath: `inset(${Math.max(0, u.c.t - u.r.top)}px ${Math.max(0, u.r.right - u.c.r)}px ${Math.max(0, u.r.bottom - u.c.b)}px ${Math.max(0, u.c.l - u.r.left)}px)` });
    pv.f.style.transform = "scale(" + (w / 1920) + ")"; u.slot.classList.toggle("live", pv.loaded);
  }
  for (const [key, pv] of [...OV.pvs.entries()]) if (!keep.has(key) && now - pv.seen > 2500) removePv(key);   // a short grace keeps a fast scroll from reloading every card
}
let syncRaf = 0; const syncSoon = () => { if (syncRaf || typeof requestAnimationFrame === "undefined") return; syncRaf = requestAnimationFrame(() => { syncRaf = 0; try { syncDock(); } catch {} }); };

// After app.js has replaced #view: put scroll offsets and the caret back, then position the previews (a microtask runs before the next paint).
function postRender() {
  try {
    const v = document.getElementById("view"); if (!v || pp.S.view !== "overlays") return;
    v.querySelectorAll("[data-sc]").forEach(el => { const y = OV.sc[el.dataset.sc]; if (y) el.scrollTop = y; });
    const a = document.activeElement; if (a && a.id && OV.caret && OV.caret.id === a.id) { try { a.setSelectionRange(OV.caret.s, OV.caret.e); } catch {} }
    syncDock();
  } catch {}
}
function hookOnce() {
  if (OV.hooked || typeof document === "undefined") return; OV.hooked = true;
  const keep = e => { const t = e.target; if (t && t.id && typeof t.selectionStart === "number" && t.closest && t.closest("#view")) { try { OV.caret = { id: t.id, s: t.selectionStart, e: t.selectionEnd }; } catch {} } };
  for (const ev of ["input", "keyup", "mouseup", "focusin"]) document.addEventListener(ev, keep, true);
  document.addEventListener("scroll", syncSoon, true); window.addEventListener("resize", syncSoon);
  document.addEventListener("keydown", e => {
    if (pp.S.view !== "overlays" || e.repeat) return;
    const t = e.target; if (t && (/^(INPUT|TEXTAREA|SELECT)$/.test(t.tagName) || t.isContentEditable)) return;
    if (document.querySelector("dialog[open]")) return;
    const combo = comboOf(e); if (!combo) return;
    const p = OV.presets.find(x => x.hotkey && normHotkey(x.hotkey) === combo); if (!p) return;
    e.preventDefault(); (e.shiftKey ? presetHide : presetFire)(p, [cur()]);
  });
}

// ---------------------------------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------------------------------
export function onEnterOverlays(v) {
  clearInterval(OV.poll); clearInterval(OV.tick); OV.poll = OV.tick = null;
  if (v !== "overlays") { dockHide(); return; }
  hookOnce(); loadAll(); loadCues();
  OV.poll = setInterval(() => { if (pp.S.view !== "overlays") return; loadAll(); if (OV.tab === "cues" && Date.now() - OV.cues.at > 20000) loadCues(); }, 6000);
  OV.tick = setInterval(() => {                               // time on air / hold countdowns in place, no re-render
    if (pp.S.view !== "overlays") return; const now = Date.now();
    document.querySelectorAll("#view [data-age]").forEach(el => { el.textContent = fmtAge(now - +el.dataset.age); });
    document.querySelectorAll("#view [data-until]").forEach(el => { const left = +el.dataset.until - now; el.textContent = left > 0 ? "hides in " + fmtAge(left) : "hiding"; });
    syncDock();
  }, 1000);
}
export function onWsOverlays(m) {
  if (m.type === "overlays") { const ch = m.channelId; if (ch) { const c = (OV.channels[ch] = OV.channels[ch] || { layers: [], defaults: [] }); if (m.layers) c.layers = listOf(m.layers, "layers"); if (m.defaults) c.defaults = listOf(m.defaults, "defaults"); } return true; }
  if (m.type === "overlayConfig") { if (m.kind === "cgSlots" && OV.cg.loaded) loadCg(); refreshSoon(); return true; }   // {kind: presets|rules|defaults|renderer|cgSlots, channelId?}
  if (m.type === "overlayPresets") { OV.presets = listOf(m.presets || m, "presets"); return true; }
  if (m.type === "overlayRules") { OV.rules = listOf(m.rules || m, "rules"); return true; }
  return false;
}

// ---------------------------------------------------------------------------------------------------
// Views
// ---------------------------------------------------------------------------------------------------
const CSS = `
.ov{display:grid;gap:12px;min-width:0} .ov .mu{color:var(--t3)} .ov .sm{font-size:10px} .ov td.wr{white-space:normal;max-width:none;overflow:visible}
.ovTabs{display:flex;gap:4px;flex-wrap:wrap;padding:6px 12px;border-bottom:1px solid var(--b0);align-items:center}
.ovSlot{position:relative;width:100%;aspect-ratio:16/9;background:repeating-conic-gradient(#1a1a28 0 25%,#0f0f18 0 50%) 0 0/20px 20px;border:1px solid var(--b0);border-radius:6px;overflow:hidden}
.ovSlot .ico{position:absolute;inset:0;display:flex;align-items:center;justify-content:center;font-size:26px;color:var(--t2);pointer-events:none} .ovSlot.live .ico{display:none}
.ovDeploy{display:grid;grid-template-columns:minmax(300px,1.05fr) minmax(300px,1fr) minmax(320px,1.1fr);gap:12px;padding:12px;align-items:start}
@media(max-width:1280px){.ovDeploy{grid-template-columns:1fr 1fr}.ovDeploy>.pv{grid-column:1/-1}} @media(max-width:800px){.ovDeploy{grid-template-columns:1fr}}
.ovGal{display:grid;grid-template-columns:repeat(auto-fill,minmax(148px,1fr));gap:8px;max-height:62vh;overflow:auto;padding:2px}
.ovCard{background:var(--bg3);border:1px solid var(--b0);border-radius:7px;padding:5px;cursor:pointer;min-width:0} .ovCard:hover{background:var(--bg4)} .ovCard.sel{border-color:var(--accent);background:var(--bg4)}
.ovCard .n{font-weight:700;font-size:12px;margin-top:4px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis} .ovCard .m{font-size:10px;color:var(--t3);white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.ovForm{display:grid;grid-template-columns:1fr 1fr;gap:8px} .ovForm .wide{grid-column:1/-1} .ovForm input,.ovForm select,.ovForm textarea{width:100%;min-width:0}
.ovUpd{display:grid;grid-template-columns:repeat(auto-fill,minmax(200px,1fr));gap:8px;padding:8px 4px}
.ovChip{display:inline-block;padding:2px 8px;border:1px solid var(--b1);border-radius:10px;font-size:11px;cursor:pointer;color:var(--t1);background:var(--bg3);margin:0 3px 3px 0} .ovChip.on{border-color:var(--accent);color:var(--accent);background:rgba(0,212,255,.12)}
.ovCgIn{display:inline-flex;align-items:center;gap:2px;margin-right:6px;font-size:11px;font-family:var(--mono);cursor:pointer}
.ovCgOn{font-size:9px;font-weight:700;letter-spacing:1px;color:var(--ok,#3c6);border:1px solid currentColor;border-radius:3px;padding:0 4px}
.ovKbd{font-family:var(--mono);font-size:10px;border:1px solid var(--b2);border-bottom-width:2px;border-radius:4px;padding:0 5px;color:var(--t1)}
.ov pre{margin:0;font-size:11px;white-space:pre-wrap;word-break:break-all;max-height:260px;overflow:auto}
#toast{z-index:300}`;

const cardSlot = t => `<div class="ovSlot" data-pv="${E("t:" + t.id)}" data-url="${E(thumbUrl(t))}" data-prio="0" data-nonce="0"><span class="ico">${E(t.icon || "▭")}</span></div>`;
const chPicker = () => `<span class="row" style="gap:4px;flex-wrap:nowrap">${chanList().map(c => `<button class="ghost small${c.profile.id === cur() ? " active" : ""}" onclick="pp.setActive(${jsq(c.profile.id)})">${E(c.profile.name || c.profile.id)}</button>`).join("")}</span>`;

function rendBadge() {
  const r = OV.renderer || {}, eng = ((r.engines || {})[cur()] || {}).html || null;
  const a = r.available === undefined ? null : !!r.available;
  return (a === null ? bdg("renderer ?", "var(--t3)") : a ? bdg("HTML " + (r.version && r.version !== "unknown" ? r.version : "ready"), "var(--ready)") : bdg("no browser", "var(--danger)"))
    + (eng ? bdg("engine " + (eng.state || "?") + (eng.fps ? " " + Number(eng.fps).toFixed(0) + " fps" : ""), engColor(eng.state)) : "");
}

function fieldHtml(p, val, id, set, tplId) {
  const lab = E(p.label || p.name) + (p.required ? " *" : ""), t = p.type || "text", v = sval(val), I = E(id);
  let inp;
  if (t === "select") { const opts = (p.options || []).map(String); if (v !== "" && !opts.includes(v)) opts.unshift(v); inp = `<select id="${I}" onchange="${set("this.value")}">${opts.map(o => `<option value="${E(o)}"${o === v ? " selected" : ""}>${E(o)}</option>`).join("")}</select>`; }
  else if (t === "color") inp = `<span class="row" style="flex-wrap:nowrap;gap:4px"><input type="color" value="${hex6(v)}" style="width:42px;padding:1px" onchange="${set("this.value.replace('#','')")}"><input id="${I}" class="mono" value="${E(v)}" oninput="${set("this.value.replace('#','')")}"></span>`;
  else if (t === "boolean") inp = `<span class="row"><input id="${I}" type="checkbox" style="width:auto"${v === "1" || v === "true" ? " checked" : ""} onchange="${set("this.checked?'1':'0'")}"> ${E(p.hint || "on")}</span>`;
  else if (t === "number") inp = `<input id="${I}" type="number" step="any" value="${E(v)}" oninput="${set("this.value")}">`;
  else if (t === "textarea") inp = `<textarea id="${I}" rows="3" oninput="${set("this.value")}">${E(v)}</textarea>`;
  else if (t === "datetime") inp = `<input id="${I}" type="datetime-local" value="${/^\d{4}-\d\d-\d\dT\d\d:\d\d/.test(v) ? E(v.slice(0, 16)) : ""}" onchange="${set("this.value")}">`;
  else inp = `<input id="${I}" value="${E(v)}" placeholder="${E(t === "image" ? "image URL or path" : sval(p.default))}" oninput="${set("this.value")}">`;
  return `<label class="f${t === "textarea" ? " wide" : ""}" data-ctx="ovField" data-ctx-tpl="${E(tplId || "")}" data-ctx-param="${E(p.name)}">${lab}${inp}</label>`;
}

function boardHtml(ch) {
  const L = layersOf(ch); const now = Date.now(); const rows = [];
  for (const k of Object.keys(OV.open)) { const [c, l] = k.split(":"); if (c === ch && !L.some(x => String(x.layer) === l)) delete OV.open[k]; }
  for (const l of L) {
    const key = ch + ":" + l.layer, t = tplOf(l.template), st = l.state || "on_air", lay = Number(l.layer);
    const hold = l.holdUntilMs > now && st === "on_air" ? `<span class="badge mono" data-until="${Number(l.holdUntilMs)}" style="color:var(--t1);border-color:var(--b2)">hides in ${fmtAge(l.holdUntilMs - now)}</span>` : "";
    rows.push(`<tr class="${st === "lost" || st === "failed" ? "past" : ""}" data-ctx="ovLayer" data-ctx-id="${E(key)}" data-ctx-title="Layer ${lay}" data-ctx-json="${E(JSON.stringify(l))}"><td class="mono">${lay}</td>
      <td><b>${E(l.role || "")}</b>${l.slot ? ` <span class="mu sm">#${E(l.slot)}</span>` : ""}</td>
      <td><b>${E(l.name || (t && t.name) || l.template)}</b><div class="mu sm mono">${E(l.template || "")}${l.label ? " · " + E(l.label) : ""}${l.origin || (l.source && typeof l.source === "string") ? " · " + E(l.origin || l.source) : ""}</div></td>
      <td title="${E(JSON.stringify(l.params || {}))}">${E(summary(l.params))}</td>
      <td class="mono" ${l.shownAtMs ? `data-age="${Number(l.shownAtMs)}"` : ""}>${l.shownAtMs ? fmtAge(now - l.shownAtMs) : "—"}</td>
      <td>${bdg(l.mode || "?", l.mode === "html" ? "var(--accent)" : l.mode === "native" ? "var(--accent2)" : "var(--t3)")} ${bdg(st, stateColor(st))} ${hold}${l.fellBackFrom ? " " + bdg("fell back from " + l.fellBackFrom, "var(--warn)") : ""}${l.error || l.lastError ? ` <span style="color:var(--danger)" title="${E(l.error || l.lastError)}">!</span>` : ""}</td>
      <td style="white-space:nowrap"><button class="small${OV.open[key] ? " info" : ""}" onclick="pp.ovUpd(${jsq(ch)},${lay})">Update</button> <button class="small" onclick="pp.ovRetake(${jsq(ch)},${lay})">Re-take</button> <button class="small" ${isUp(st) ? "" : "disabled"} onclick="pp.ovHide(${jsq(ch)},${lay})">Hide</button> <button class="ghost small" onclick="pp.ovDel(${jsq(ch)},${lay})" title="Delete the layer">&#10005;</button></td></tr>`);
    if (OV.open[key]) {
      const draft = OV.open[key]; const ps = t && (t.params || []).length ? t.params : Object.keys(l.params || {}).map(n => ({ name: n, label: n, type: "text" }));
      const txt = textFieldsOf(t).map((f, i) => ({ name: "text" + (i + 1), label: "Text " + (i + 1) + " → " + textLabel(t, f), type: "text" }));
      rows.push(`<tr><td colspan="7" class="wr" style="background:var(--bg1)"><div class="ovUpd">${txt.concat(ps).map(p => fieldHtml(p, draft[p.name], "ovu_" + lay + "_" + p.name, v => `pp.ovDraft(${jsq(key)},${jsq(p.name)},${v})`, l.template)).join("")}</div>
        <div class="row" style="padding:0 4px 8px"><button class="success small" onclick="pp.ovApply(${jsq(ch)},${lay})">Apply update</button><button class="ghost small" onclick="pp.ovUpd(${jsq(ch)},${lay})">cancel</button><span class="mu sm">sends only the text fields to the engine layer; HTML layers update in place without a re-take</span></div></td></tr>`);
    }
  }
  const tl = pp.S.telemetry[ch] || {};
  return `<div class="panel"><h3>Overlays on air — ${E(chName(ch))} <span class="right">${tl.title ? `<span class="sm" style="color:var(--t2)">program: ${E(trunc(tl.title, 40))}</span>` : ""}${rendBadge()}
      <label class="sm" style="display:inline-flex;gap:3px;align-items:center"><input type="checkbox" ${OV.clearDefs ? "checked" : ""} onchange="pp.ovClearDefs(this.checked)" style="width:auto"> incl. defaults</label>
      <button class="danger small" oncontextmenu="pp.ovClearMenu(event)" onclick="pp.ovClear()" title="Hide every overlay on this channel (right-click: by role, all channels)">Clear</button><button class="ghost small" onclick="pp.ovRefresh()" title="Reload">&#8635;</button></span></h3>
    ${OV.down ? `<div class="alert warning" style="margin:8px 12px">${E(OV.down)}</div>` : ""}
    <div class="scroll" style="max-height:34vh" data-ctx="ovBoard"><table><thead><tr><th>Layer</th><th>Role</th><th>Template</th><th>Params</th><th>On air</th><th>State</th><th></th></tr></thead><tbody>${rows.join("") || `<tr><td colspan="7" class="empty">${OV.ready ? "No overlays on " + E(chName(ch)) + ". Deploy one below, fire a preset, or enable an always-on default." : "Loading…"}</td></tr>`}</tbody></table></div></div>`;
}

function galleryHtml() {
  const T = OV.man.templates.filter(t => t.listed !== false), cats = [...new Set(T.map(t => t.category))], q = OV.q.trim().toLowerCase();
  const list = T.filter(t => q ? (t.name + " " + t.id + " " + (t.description || "") + " " + t.category).toLowerCase().includes(q) : !OV.cat || t.category === OV.cat);
  return `<div><div class="row" style="margin-bottom:6px;gap:6px"><input id="ovQ" placeholder="search ${T.length} templates" value="${E(OV.q)}" oninput="pp.ovQ(this.value)" style="flex:1;min-width:0"><label class="sm" style="display:inline-flex;gap:3px;align-items:center" title="Live iframe previews on the cards (up to ${OV.maxLive} at once)"><input type="checkbox" style="width:auto" ${OV.live ? "checked" : ""} onchange="pp.ovLive(this.checked)"> live</label></div>
    <div style="margin-bottom:6px"><span class="ovChip${!OV.cat ? " on" : ""}" onclick="pp.ovCat('')">all ${T.length}</span>${cats.map(c => `<span class="ovChip${OV.cat === c ? " on" : ""}" onclick="pp.ovCat(${jsq(c)})">${E(c)} ${T.filter(t => t.category === c).length}</span>`).join("")}</div>
    <div class="ovGal" data-sc="gal" onscroll="OV.sc.gal=this.scrollTop" data-ctx="ovGallery">${list.map(t => `<div class="ovCard${t.id === OV.sel ? " sel" : ""}" onclick="pp.ovPick(${jsq(t.id)})" data-ctx="ovTpl" data-ctx-id="${E(t.id)}" data-ctx-title="${E(t.name)}" title="${E(t.description || "")}">${cardSlot(t)}<div class="n">${E(t.name)}</div><div class="m">${E(t.category)} · ${E(t.role || "")}</div></div>`).join("") || `<div class="empty" style="grid-column:1/-1">${OV.man.templates.length ? "No template matches" : "No template pack loaded"}</div>`}</div></div>`;
}

function formHtml(ch) {
  const t = tplOf(OV.sel); if (!t) return `<div class="empty">Pick a template from the gallery</div>`;
  const a = normAnim(OV.anim), anims = OV.man.animations, ed = OV.editing;
  const opt = (arr, v) => arr.map(o => `<option value="${E(o)}"${o === v ? " selected" : ""}>${E(o)}</option>`).join("");
  const edName = ed ? (ed.kind === "preset" ? (OV.presets.find(x => x.id === ed.id) || {}).name : tplName((defaultsOf(ch).find(x => x.id === ed.id) || {}).template)) || "" : "";
  return `<div>${ed ? `<div class="alert warning" style="margin:0 0 8px">Editing ${E(ed.kind)} <b>${E(edName)}</b> — the form below is its content.</div>` : ""}
    <div style="font-weight:700;margin-bottom:6px">${E(t.name)} <span class="mu" style="font-weight:400">${E(t.description || "")}</span></div>
    ${textRows(t, OV.params, "ovt", (n, v) => `pp.ovParam(${jsq(n)},${v})`)}
    <div class="ovForm">${(t.params || []).map(p => fieldHtml(p, OV.params[p.name] !== undefined ? OV.params[p.name] : p.default, "ovp_" + p.name, v => `pp.ovParam(${jsq(p.name)},${v})`, t.id)).join("") || `<div class="mu wide">This template has no parameters.</div>`}</div>
    <div class="fh" style="font-size:10px;letter-spacing:1px;color:var(--t3);font-weight:700;margin:10px 0 4px">ANIMATION</div>
    <div class="row" style="margin-bottom:6px">${OV.man.presets.map(p => `<button class="ghost small" onclick="pp.ovAnimPreset(${jsq(p.id)})" title="${E(p.in)} / ${E(p.out)} ${E(p.duration)} ms">${E(p.icon || "")} ${E(p.name)}</button>`).join("")}</div>
    <div class="ovForm"><label class="f">In<select onchange="pp.ovAnim('in',this.value)">${opt(anims, a.in)}</select></label><label class="f">Out<select onchange="pp.ovAnim('out',this.value)">${opt(anims, a.out)}</select></label>
      <label class="f">In ms<input type="number" id="ovAIn" value="${a.duration}" oninput="pp.ovAnim('duration',this.value)"></label><label class="f">Out ms<input type="number" id="ovAOut" value="${a.outDuration}" oninput="pp.ovAnim('outDuration',this.value)"></label>
      <label class="f wide">Easing<input id="ovAEase" list="ovEase" value="${E(a.easing)}" oninput="pp.ovAnim('easing',this.value)"><datalist id="ovEase">${EASINGS.map(e => `<option value="${E(e)}">`).join("")}</datalist></label></div>
    <div class="fh" style="font-size:10px;letter-spacing:1px;color:var(--t3);font-weight:700;margin:10px 0 4px">PLACEMENT</div>
    <div class="ovForm"><label class="f">Role<select onchange="pp.ovSet('role',this.value)"><option value="">template default (${E(t.role || "—")})</option>${opt(ROLES, OV.role)}</select></label>
      <label class="f">Slot (0 = main, 1+ coexist)<input type="number" min="0" step="1" id="ovSlot" value="${E(OV.slot)}" oninput="pp.ovSet('slot',this.value)"></label>
      <label class="f">Hold (s, 0 = until hidden)<input type="number" min="0" step="1" id="ovHold" value="${E(OV.hold)}" oninput="pp.ovSet('hold',this.value)"></label>
      <label class="f">Mode<select onchange="pp.ovSet('mode',this.value)">${opt(MODES, OV.mode)}</select></label>
      <label class="f wide">Label (shown on the board)<input id="ovLabel" value="${E(OV.label)}" oninput="pp.ovSet('label',this.value)"></label></div>
    <div class="row" style="margin-top:10px"><button class="success" ${OV.busy.deploy ? "disabled" : ""} onclick="pp.ovDeploy(false)">${OV.busy.deploy ? "DEPLOYING…" : "DEPLOY to " + E(chName(ch))}</button>
      <button class="primary" ${OV.busy.deploy ? "disabled" : ""} onclick="pp.ovDeploy(true)" title="The same graphic on every visible channel">Deploy to all channels (${chanList().length})</button>
      <button class="ghost" onclick="pp.ovReset()" title="Back to the template defaults">reset</button></div>
    <div class="row" style="margin-top:6px">${ed ? `<button class="small info" onclick="pp.ovSaveEdit()">Save changes</button><button class="small" onclick="pp.ovSavePreset()">Save as new preset…</button><button class="ghost small" onclick="pp.ovCancelEdit()">cancel edit</button>`
      : `<button class="small" onclick="pp.ovSavePreset()">Save as preset…</button><button class="small" onclick="pp.ovCgStorePick()" title="CG1, CG2…: fire by hand or show when a switcher input goes on air">Store in CG…</button><button class="small" onclick="pp.ovAddDefault()" title="Always on, redeployed after an engine restart">Add as default…</button>`}</div></div>`;
}

function stageHtml() {
  const t = tplOf(OV.sel);
  return `<div class="pv" data-ctx="ovStage"><div class="ovSlot" data-pv="stage" data-url="${E(OV.stageUrl)}" data-prio="9" data-nonce="${OV.nonce}">${OV.stageUrl ? "" : `<span class="ico" style="font-size:12px">pick a template</span>`}</div>
    <div class="row" style="margin-top:6px"><button class="small" onclick="pp.ovReplay()">&#8635; replay</button>${t ? `<button class="ghost small" onclick="pp.ovCopyUrl()">copy URL</button><a class="btn small" target="_blank" href="${E(OV.stageUrl)}">open &#8599;</a>` : ""}<span class="fill"></span><span class="mu sm">1920×1080 scaled · checkerboard = transparent</span></div>
    <div class="mono mu sm" style="word-break:break-all;margin-top:4px">${E(OV.stageUrl)}</div>
    <div class="mu sm" style="margin-top:6px">Tokens usable in text params: ${TOKENS.map(k => `<code>${E(k)}</code>`).join(" ")} (filled from the on-air item when a cue or rule fires).</div></div>`;
}

function presetsTab(ch) {
  const dup = {}; for (const p of OV.presets) { const h = normHotkey(p.hotkey); if (h) dup[h] = (dup[h] || 0) + 1; }
  const rows = OV.presets.map(p => { const h = normHotkey(p.hotkey);
    return `<tr data-ctx="ovPreset" data-ctx-id="${E(p.id)}" data-ctx-title="${E(p.name)}" data-ctx-json="${E(JSON.stringify(p))}"><td><input id="ovhk_${E(p.id)}" class="mono" style="width:84px" value="${E(hkDisp(h || p.hotkey))}" placeholder="key" onchange="pp.ovPresetKey(${jsq(p.id)},this.value)">${h && dup[h] > 1 ? ' <span style="color:var(--danger)" title="two presets share this key">!</span>' : ""}</td>
      <td><b>${E(p.name)}</b></td><td>${E(tplName(p.template))}<div class="mu sm mono">${E(summary(p.params))}</div></td><td>${E(p.role || "")}</td><td>${E(p.mode || "auto")}</td><td class="mono">${p.holdSec ? E(p.holdSec) + " s" : "—"}</td>
      <td style="white-space:nowrap"><button class="success small" onclick="pp.ovPresetFire(${jsq(p.id)},false)">Fire</button> <button class="small" onclick="pp.ovPresetHide(${jsq(p.id)})">Hide</button> <button class="small" onclick="pp.ovPresetEdit(${jsq(p.id)})">Edit</button> <button class="ghost small" onclick="pp.ovPresetDel(${jsq(p.id)})">&#10005;</button></td></tr>`; }).join("");
  return `<div class="scroll" style="max-height:44vh" data-ctx="ovPresets"><table><thead><tr><th>Hotkey</th><th>Name</th><th>Template</th><th>Role</th><th>Mode</th><th>Hold</th><th></th></tr></thead><tbody>${rows || `<tr><td colspan="7" class="empty">No presets yet. Build a graphic in Deploy and press “Save as preset…”.</td></tr>`}</tbody></table></div>
    <div class="body mu sm">A hotkey fires the preset on <b>${E(chName(ch))}</b> while this tab is open and no field has focus (Shift + hotkey hides it). Keys: <span class="ovKbd">1</span> <span class="ovKbd">F2</span> <span class="ovKbd">ctrl+3</span> <span class="ovKbd">alt+x</span>. Right-click a row for more.</div>`;
}
function defaultsTab(ch) {
  const rows = defaultsOf(ch).map(d => `<tr data-ctx="ovDefault" data-ctx-id="${E(d.id)}" data-ctx-title="${E(tplName(d.template))}" data-ctx-json="${E(JSON.stringify(d))}"><td><input type="checkbox" style="width:auto" ${d.enabled !== false ? "checked" : ""} onchange="pp.ovDefToggle(${jsq(d.id)},this.checked)"></td><td><b>${E(tplName(d.template))}</b><div class="mu sm mono">${E(summary(d.params))}</div></td><td>${E(d.role || "")}</td><td>${E(d.mode || "auto")}</td>
      <td style="white-space:nowrap"><button class="small" onclick="pp.ovDefNow(${jsq(d.id)})">Deploy now</button> <button class="small" onclick="pp.ovDefEdit(${jsq(d.id)})">Edit</button> <button class="ghost small" onclick="pp.ovDefDel(${jsq(d.id)})">&#10005;</button></td></tr>`).join("");
  return `<div class="scroll" style="max-height:44vh" data-ctx="ovDefaults"><table><thead><tr><th>On</th><th>Graphic</th><th>Role</th><th>Mode</th><th></th></tr></thead><tbody>${rows || `<tr><td colspan="5" class="empty">No always-on graphics on ${E(chName(ch))}. Build one in Deploy and press “Add as default…”, or right-click a layer.</td></tr>`}</tbody></table></div>
    <div class="body mu sm">Defaults (a station bug, a clock) are redeployed by the Supervisor whenever the engine connects or restarts and after the channel leaves emergency. Clear keeps them unless “incl. defaults” is ticked.</div>`;
}

const ruleWhen = r => {
  const w = r.when || {}; const parts = [["types", w.types], ["cat", w.categories], ["kw", w.keywords], ["series", w.series], ["input", w.inputs]].filter(([, v]) => (v || []).length).map(([k, v]) => `${k}: ${v.join("/")}`);
  const txt = parts.join(w.mode === "any" ? " OR " : " · ") || "every item";
  return r.negate ? "NOT (" + txt + ")" : txt;
};
const showsOf = t => (Array.isArray(t.show) ? t.show : t.show ? [t.show] : []).filter(x => x && x.template);
const ruleThen = r => { const t = r.then || {}, o = []; if ((t.suppress || []).length) o.push("suppress " + t.suppress.join(", ")); const sh = showsOf(t); if (sh.length) o.push("show " + sh.map(x => tplName(x.template) + (x.params && (x.params.text1 || x.params.name) ? " “" + (x.params.text1 || x.params.name) + "”" : "")).join(", ")); return o.join(" + ") || "—"; };
// One-click starting points for the common station rules (docs/overlays.md "Rules").
const RULE_PRESETS = {
  noAds: () => ({ name: "No graphics during ads", when: { types: ["ad", "promo", "psa"], mode: "all" }, then: { suppress: ["*"] } }),
  newsLive: () => ({ name: "Graphics only on news and live", negate: true, when: { types: ["news", "live"], categories: ["news"], mode: "any" }, then: { suppress: ["*"] } }),
  programNewsWeather: () => ({ name: "Graphics on live, program, news and weather", negate: true, when: { types: ["live", "program", "news", "weather"], categories: ["live", "program", "news", "weather"], mode: "any" }, then: { suppress: ["*"] } }),
  breaks: () => ({ name: "No graphics on ads, promos and IDs", when: { types: ["ad", "ads", "promo", "promos", "id", "ids", "station_id"], categories: ["ad", "ads", "promo", "promos", "id", "ids", "station_id"], mode: "any" }, then: { suppress: ["*", "legal_id"] } }),
  input: () => ({ name: "Name strap on input 1", when: { inputs: ["in1"], mode: "all" }, then: { show: { template: (OV.man.templates.find(t => t.id === "lower_third") || OV.man.templates.find(t => t.role === "lower_third") || {}).id || "", params: { text1: "Anchor name", text2: "Title" }, startOffset: 2, duration: 10 } } }),
};
function rulesTab() {
  const E_ = OV.ruleEdit, rt = OV.ruleTest;
  const rows = OV.rules.map(r => `<tr data-ctx="ovRule" data-ctx-id="${E(r.id)}" data-ctx-title="${E(r.name)}" data-ctx-json="${E(JSON.stringify(r))}"><td><input type="checkbox" style="width:auto" ${r.enabled !== false ? "checked" : ""} onchange="pp.ovRuleEnable(${jsq(r.id)},this.checked)"></td><td><b>${E(r.name)}</b></td><td class="mu sm">${(r.channels || []).length ? E(r.channels.join(", ")) : "all channels"}</td><td>${E(ruleWhen(r))}</td><td>${E(ruleThen(r))}</td>
      <td style="white-space:nowrap"><button class="small" onclick="pp.ovRuleEdit(${jsq(r.id)})">Edit</button> <button class="small" onclick="pp.ovRuleTestFor(${jsq(r.id)})">Test</button> <button class="ghost small" onclick="pp.ovRuleDel(${jsq(r.id)})">&#10005;</button></td></tr>`).join("");
  const chip = (path, v, on) => `<span class="ovChip${on ? " on" : ""}" onclick="pp.ovRuleTog(${jsq(path)},${jsq(v)})">${E(v)}</span>`;
  let ed = "";
  if (E_) {
    const r = E_.r, w = r.when || {}, th = r.then || {}, sh = th.show || {}; const list = v => (v || []).join(", ");
    ed = `<div class="body" style="border-top:1px solid var(--b0);background:var(--bg3)"><div class="row" style="justify-content:space-between;margin-bottom:6px"><b>${E_.isNew ? "New rule" : "Edit rule"}</b><span class="fill"></span><button class="success small" onclick="pp.ovRuleSave()">Save rule</button><button class="ghost small" onclick="pp.ovRuleCancel()">cancel</button></div>
      <div class="ovForm" style="grid-template-columns:1fr 1fr 1fr 1fr"><label class="f" style="grid-column:span 2">Name<input id="ovrName" value="${E(r.name || "")}" oninput="pp.ovRuleSet('name',this.value)"></label>
        <label class="f">Enabled<select onchange="pp.ovRuleSet('enabled',this.value==='1')"><option value="1"${r.enabled !== false ? " selected" : ""}>yes</option><option value="0"${r.enabled === false ? " selected" : ""}>no</option></select></label><span></span>
        <div class="wide"><div class="mu sm" style="margin-bottom:3px">CHANNELS (none ticked = every channel)</div>${chanList().map(c => chip("channels", c.profile.id, (r.channels || []).includes(c.profile.id))).join("")}</div>
        <div class="wide"><div class="mu sm" style="margin-bottom:3px">WHEN THE ITEM IS OF TYPE (none = any)</div>${MEDIA_TYPES.map(t => chip("when.types", t, (w.types || []).includes(t))).join("")}</div>
        <label class="f" style="grid-column:span 2">Categories / subcategories (comma)<input id="ovrCat" value="${E(list(w.categories))}" oninput="pp.ovRuleList('when.categories',this.value)"></label><label class="f" style="grid-column:span 2">Series (comma)<input id="ovrSer" value="${E(list(w.series))}" oninput="pp.ovRuleList('when.series',this.value)"></label>
        <label class="f wide">Keywords in the transcript or title (comma, any match)<input id="ovrKw" value="${E(list(w.keywords))}" oninput="pp.ovRuleList('when.keywords',this.value)"></label>
        <label class="f" style="grid-column:span 2">Inputs (comma: in1..in8 = switcher keys, or part of an NDI name / stream URL / input title)<input id="ovrIn" value="${E(list(w.inputs))}" placeholder="in1, CAMERA (NDIHX2" oninput="pp.ovRuleList('when.inputs',this.value)"></label>
        <label class="f">Combine<select onchange="pp.ovRuleSet('when.mode',this.value)"><option value="all"${w.mode !== "any" ? " selected" : ""}>all of the above</option><option value="any"${w.mode === "any" ? " selected" : ""}>any one of them</option></select></label>
        <label class="f">Fires when the item<select onchange="pp.ovRuleSet('negate',this.value==='1')"><option value="0"${!r.negate ? " selected" : ""}>matches</option><option value="1"${r.negate ? " selected" : ""}>does NOT match</option></select></label>
        <div class="wide"><div class="mu sm" style="margin-bottom:3px">THEN SUPPRESS THESE ROLES while the item plays (restored when the next item starts; * = every role but legal_id, emergency and pillar)</div>${["*"].concat(ROLES).map(x => chip("then.suppress", x, (th.suppress || []).includes(x))).join("")}</div>
        <label class="f" style="grid-column:span 2">…and SHOW template (optional)<select onchange="pp.ovRuleShow('template',this.value)"><option value="">none</option>${OV.man.templates.map(t => `<option value="${E(t.id)}"${t.id === sh.template ? " selected" : ""}>${E(t.category)} / ${E(t.name)}</option>`).join("")}</select></label>
        <label class="f">Role<select onchange="pp.ovRuleShow('role',this.value)"><option value="">template default</option>${ROLES.map(x => `<option${x === sh.role ? " selected" : ""}>${x}</option>`).join("")}</select></label>
        <label class="f">Animation<select onchange="pp.ovRuleShow('anim',this.value)"><option value="">current form</option>${OV.man.presets.map(p => `<option value="${E(p.id)}">${E(p.name)}</option>`).join("")}</select></label>
        <label class="f wide">Params JSON (tokens like {{title}} allowed; text1 / text2 / text3 fill the template's text fields)<textarea id="ovrParams" rows="3" class="mono" oninput="OV.ruleEdit.paramsText=this.value">${E(E_.paramsText)}</textarea></label>
        ${(E_.extraShows || []).length ? `<div class="wide mu sm">This rule also shows ${E((E_.extraShows || []).map(x => tplName(x.template)).join(", "))} (kept as they are; edit them as JSON with Copy / Paste).</div>` : ""}</div></div>`;
  }
  const res = rt.res; let resHtml = "";
  if (rt.busy) resHtml = `<div class="mu">testing…</div>`;
  else if (res) { const arr = Array.isArray(res) ? res : res.rules || res.matches || res.fired || res.results || null;
    resHtml = (Array.isArray(arr) ? `<table><thead><tr><th>Rule</th><th>Fires</th><th>Why</th></tr></thead><tbody>${arr.map(x => `<tr><td>${E(x.name || x.id || x.rule || "")}</td><td>${bdg(String(x.fires ?? x.fired ?? x.matched ?? x.match ?? "?"), x.fires || x.fired || x.matched || x.match ? "var(--ready)" : "var(--t3)")}</td><td class="wr">${E(x.reason || x.why || x.explain || x.note || "")}</td></tr>`).join("")}</tbody></table>` : "") + `<details style="margin-top:4px"><summary class="mu sm" style="cursor:pointer">raw answer</summary><pre class="mono">${E(JSON.stringify(res, null, 2))}</pre></details>`; }
  return `<div class="scroll" style="max-height:30vh" data-ctx="ovRules"><table><thead><tr><th>On</th><th>Rule</th><th>Channels</th><th>When</th><th>Then</th><th></th></tr></thead><tbody>${rows || `<tr><td colspan="6" class="empty">No rules. Example: when type = ad, suppress lower_third and bug.</td></tr>`}</tbody></table></div>
    <div class="row" style="padding:8px 12px;border-top:1px solid var(--b0)"><button class="small primary" onclick="pp.ovRuleNew()">+ rule</button>
      <button class="small" title="Suppress every graphic while an ad, promo or PSA plays; they come back on the next item" onclick="pp.ovRulePreset('noAds')">+ No graphics on ads</button>
      <button class="small" title="Suppress every graphic unless the item is news or live" onclick="pp.ovRulePreset('newsLive')">+ Only on news &amp; live</button>
      <button class="small" title="Suppress graphics outside live, program, news and weather" onclick="pp.ovRulePreset('programNewsWeather')">+ Live, program, news &amp; weather</button>
      <button class="small" title="Suppress graphics on ads, promos and IDs even when scheduled as program" onclick="pp.ovRulePreset('breaks')">+ No ads, promos &amp; IDs</button>
      <button class="small" title="A name strap whenever a switcher input is on air" onclick="pp.ovRulePreset('input')">+ Name strap per input</button><span class="fill"></span>
      <label class="f" style="flex-direction:row;align-items:center;gap:6px">Test on <select class="small" onchange="OV.ruleTest.channel=this.value">${chanList().map(c => `<option value="${E(c.profile.id)}"${(rt.channel || cur()) === c.profile.id ? " selected" : ""}>${E(c.profile.name || c.profile.id)}</option>`).join("")}</select>
      media id or input <input id="ovrMid" class="mono" style="width:190px" value="${E(rt.mediaId)}" placeholder="${E(((pp.S.telemetry[cur()] || {}).assetId) || "media id, in3 or type:ad")}" oninput="OV.ruleTest.mediaId=this.value"></label><button class="small" onclick="pp.ovRuleTestFor('')">Explain</button></div>
    ${resHtml ? `<div class="body" style="padding-top:0">${resHtml}</div>` : ""}${ed}`;
}

function cuesTab(ch) {
  const C = OV.cues; const rows = [];
  C.items.forEach((x, xi) => x.cues.forEach((q, qi) => {
    const c = q.c, tid = c.template || c.templateId || "", known = !!tplOf(tid);
    const inS = num(c.startOffset, q.inDef), outS = num(c.endOffset, q.outDef), dur = num(c.duration, 0);
    rows.push(`<tr data-ctx="ovCue" data-ctx-id="${xi}:${qi}" data-ctx-title="${E(tid || "cue")}" data-ctx-json="${E(JSON.stringify({ source: q.src, cue: c, item: { id: x.it.id, title: x.it.title, mediaId: x.it.mediaId } }))}"><td class="mono">${x.i + 1}</td><td><b>${E(trunc((x.it.title || (x.m && x.m.title) || x.it.path || ""), 38))}</b><div class="mu sm">${E(x.it.type || "")}</div></td><td>${bdg(q.src === "media" ? "media cue" : "item cue", q.src === "media" ? "var(--accent2)" : "var(--accent)")}</td>
      <td>${E(known ? tplName(tid) : tid || "?")}${known ? "" : ` <span class="mu sm">(no template: caption or custom cue)</span>`}<div class="mu sm mono">${E(summary(c.params))}</div></td><td>${E(c.role || (tplOf(tid) || {}).role || "")}</td>
      <td class="mono">in +${E(inS)} s · ${dur > 0 ? "for " + E(dur) + " s" : "until " + (outS ? "end −" + E(outS) + " s" : "end")}</td><td><button class="small success" ${known ? "" : "disabled"} onclick="pp.ovCueFire(${xi},${qi})">Test-fire on ${E(chName(ch))}</button></td></tr>`);
  }));
  return `<div class="row" style="padding:8px 12px;border-bottom:1px solid var(--b0)"><span class="mu sm">${C.slot ? "hour " + E(C.slot) + " · " : ""}${E(C.items.length)} of ${E(C.total)} item(s) carry cues${C.at ? " · read " + fmtAge(Date.now() - C.at) + " ago" : ""}</span><span class="fill"></span><button class="small" onclick="pp.ovCuesReload()">${C.loading ? "reading…" : "Reload"}</button></div>
    ${C.err ? `<div class="alert error" style="margin:8px 12px">${E(C.err)}</div>` : ""}
    <div class="scroll" style="max-height:40vh" data-ctx="ovCues"><table><thead><tr><th>#</th><th>Item</th><th>Source</th><th>Graphic</th><th>Role</th><th>Timing</th><th></th></tr></thead><tbody>${rows.join("") || `<tr><td colspan="7" class="empty">No item in this hour carries <code>cgOverlays</code> and no media in it has <code>cgCues</code>.</td></tr>`}</tbody></table></div>
    <div class="body mu sm">The Supervisor fires item cues (<code>cgOverlays</code>: start offset, duration, end offset) and media cues (<code>cgCues</code>: default in 10 s after start, out 10 s before the end, skipped when the item is too short) from engine telemetry. Test-fire puts the graphic on air now for its duration (8 s when unbounded) with the item's title in the tokens.</div>`;
}

function rendererTab(ch) {
  const r = OV.renderer || {}, engines = r.engines || {}, rt = OV.rtest; const path = OV.rpath !== null ? OV.rpath : r.preferred || "";
  const eng = Object.entries(engines).map(([c, e]) => { const h = (e && e.html) || {}; return `<tr><td><b>${E(chName(c))}</b></td><td>${bdg(h.state || "—", engColor(h.state))}</td><td class="mono">${E(h.frames ?? "—")}</td><td class="mono">${h.fps !== undefined ? E(Number(h.fps).toFixed(1)) : "—"}</td><td class="mono">${E(h.lastFrameAgeMs !== undefined ? h.lastFrameAgeMs + " ms" : "—")}</td><td class="mono">${E(h.restarts ?? 0)}</td><td class="wr" style="color:${h.lastError ? "var(--danger)" : "var(--t3)"}">${E(h.lastError || "—")}</td></tr>`; }).join("");
  return `<div class="body ovForm" data-ctx="ovRenderer" style="grid-template-columns:1fr 1fr;align-items:start"><div>
      <div class="row">${r.available === undefined ? bdg("unknown", "var(--t3)") : r.available ? bdg("browser found", "var(--ready)") : bdg("no browser", "var(--danger)")}<span class="mono" style="word-break:break-all">${E(r.path || "—")}</span></div>
      <div class="mu sm" style="margin:4px 0">version ${E(r.version || "—")}${r.env ? " · PLAYPLAY_BROWSER=" + E(r.env) : ""}</div>
      <div class="mu sm" style="margin:8px 0 3px">SEARCH ORDER</div>${(r.candidates || []).map(c => `<div class="mono sm" style="color:${c.exists ? "var(--ready)" : "var(--t3)"}">${c.exists ? "✓" : "·"} ${E(c.path)}</div>`).join("") || `<div class="mu sm">—</div>`}
      <label class="f" style="margin-top:10px">Preferred browser (settings cg.browserPath)<input id="ovRPath" class="mono" value="${E(path)}" placeholder="C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe" oninput="OV.rpath=this.value"></label>
      <div class="row" style="margin-top:6px"><button class="small primary" onclick="pp.ovRSave()">Save path</button><button class="small" ${OV.rtestBusy ? "disabled" : ""} onclick="pp.ovRTest()">${OV.rtestBusy ? "Testing…" : "Test on " + E(chName(ch))}</button><button class="ghost small" onclick="pp.ovRefresh()">Re-scan</button></div>
      <div class="mu sm" style="margin-top:6px">A saved path applies to the next engine start (nothing is restarted). The test puts a test card on the channel and reports whether the engine produced frames.</div>
      ${rt ? `<div style="margin-top:8px">${bdg(rt.ok === false || rt.error ? "failed" : "ok", rt.ok === false || rt.error ? "var(--danger)" : "var(--ready)")} ${E(rt.message || rt.error || (rt.frames !== undefined ? rt.frames + " frame(s)" : ""))}<pre class="mono" style="margin-top:4px">${E(JSON.stringify(rt, null, 2))}</pre></div>` : ""}</div>
    <div><div class="mu sm" style="margin-bottom:3px">HTML ENGINE PER CHANNEL</div><table><thead><tr><th>Channel</th><th>State</th><th>Frames</th><th>fps</th><th>Last frame</th><th>Restarts</th><th>Last error</th></tr></thead><tbody>${eng || `<tr><td colspan="7" class="empty">No engine has reported an HTML renderer yet.</td></tr>`}</tbody></table></div></div>`;
}

// ---------------------------------------------------------------------------------------------------
// Pillar panels (docs/pillar-panels.md): side-bar information beside narrow pictures
// ---------------------------------------------------------------------------------------------------
async function loadPillar() {
  const P = OV.pillar; P.busy = true;
  try { P.st = await quiet("GET", "/pillar"); P.err = ""; if (!P.draft) P.draft = clone((P.st && P.st.config) || {}); P.loadedAt = Date.now(); }
  catch (e) { P.err = e.status === 404 ? "This Supervisor has no pillar panels yet (GET /api/v1/pillar answered 404): update and restart it." : e.message || String(e); }
  finally { P.busy = false; if (pp.S.view === "overlays") pp.render(); }
}
function pillarTab(ch) {
  const P = OV.pillar;
  if (!P.st && !P.err) { if (!P.busy) loadPillar(); return `<div class="body mu">reading…</div>`; }
  if (P.err && !P.st) return `<div class="alert error" style="margin:8px 12px">${E(P.err)}</div>`;
  const d = P.draft || {}, st = P.st || {}, pn = d.panels || {}, w = d.weather || {}, h = d.history || {}, u = d.upNext || {}, stars = d.stars || {};
  const tick = (path, on, label) => `<label class="row" style="gap:6px;align-items:center"><input type="checkbox" style="width:auto" ${on ? "checked" : ""} onchange="pp.ovPSet('${path}',this.checked)">${E(label)}</label>`;
  const inp = (path, v, label, extra = "") => `<label class="f">${E(label)}<input value="${E(v ?? "")}" ${extra} oninput="pp.ovPSet('${path}',this.value)"></label>`;
  const sp = (d.sponsors || []).map((x, i) => `<tr><td><input value="${E(x.name || "")}" placeholder="Sponsor name" oninput="pp.ovPSp(${i},'name',this.value)"></td><td><input value="${E(x.tagline || "")}" placeholder="Tagline" oninput="pp.ovPSp(${i},'tagline',this.value)"></td>
      <td><input class="mono" value="${E(x.image || "")}" placeholder="/brand/logo.png or https://…" oninput="pp.ovPSp(${i},'image',this.value)"></td><td><input type="color" style="width:44px;padding:0" value="${E(hex6(x.color || "#ffc94a"))}" onchange="pp.ovPSp(${i},'color',this.value)"></td>
      <td><button class="ghost small" onclick="pp.ovPSpDel(${i})">&#10005;</button></td></tr>`).join("");
  const wx = st.weather || {}, hs = st.history || {}, decisions = Object.entries(st.channels || {});
  const dec = decisions.map(([c, x]) => `<tr><td><b>${E(chName(c))}</b></td><td>${bdg(x.on ? "panels on" : "off", x.on ? "var(--ready)" : "var(--t3)")}</td><td class="mono">${x.aspect ? E(Number(x.aspect).toFixed(3)) : "—"}</td><td class="wr">${E(x.why || "")}</td><td class="wr mu sm">${E(trunc(x.title || "", 40))}</td></tr>`).join("");
  const pv = (st.previews || {})[ch];
  const pageUrl = "/cg/pillar_panels.html?demo=1&aspect=" + enc(P.aspect || "0.5625");
  return `<div class="body ovForm" style="grid-template-columns:1.2fr 1fr;align-items:start" data-ctx="ovPillar"><div>
      <div class="row" style="gap:12px;margin-bottom:8px">${tick("enabled", d.enabled, "Pillar panels on")}${tick("live", d.live, "Also on live inputs (needs extra.displayAspect on the input's library entry)")}</div>
      <div class="mu sm" style="margin-bottom:8px">When the picture on air is narrower than <b>${E(d.maxAspect ?? 1.5)}:1</b> (9:16 = 0.56, 4:3 = 1.33, 1:1 = 1.0, 16:9 = 1.78 never), layer 11 fills both side bars with a slow starfield and the panels below. The picture itself is untouched; bugs, straps and tickers stay on top. A media record can force it with <code>extra.pillarPanels</code> true / false, or give its real shape with <code>extra.displayAspect</code> ("9:16").</div>
      <div class="ovForm" style="grid-template-columns:1fr 1fr 1fr">
        ${inp("maxAspect", d.maxAspect, "Panels below aspect", 'type="number" step="0.05" min="0.3" max="1.76"')}
        ${inp("station", d.station, "Station name (blank = call letters)")}
        <label class="f">Accent<input type="color" style="padding:0;height:30px" value="${E(hex6(d.accent || "#ffc94a"))}" onchange="pp.ovPSet('accent',this.value)"></label>
        <label class="f wide">Channels (comma, blank = every channel)<input value="${E((d.channels || []).join(", "))}" oninput="pp.ovPList('channels',this.value)"></label>
        <label class="f" style="grid-column:span 2">Only these types (comma, blank = any)<input value="${E((d.types || []).join(", "))}" oninput="pp.ovPList('types',this.value)"></label>
        <label class="f">Never on types<input value="${E((d.excludeTypes || []).join(", "))}" placeholder="ad, promo" oninput="pp.ovPList('excludeTypes',this.value)"></label>
      </div>
      <div class="mu sm" style="margin:10px 0 4px">PANELS</div><div class="row" style="gap:14px">${tick("panels.weather", pn.weather !== false, "Weather")}${tick("panels.history", pn.history !== false, "This day in history")}${tick("panels.upNext", pn.upNext !== false, "Coming up next")}${tick("panels.sponsors", pn.sponsors !== false, "Sponsor banners")}</div>
      <div class="ovForm" style="grid-template-columns:1.4fr 1fr 1fr 0.8fr;margin-top:8px">
        ${inp("weather.place", w.place, "Weather: place")}${inp("weather.lat", w.lat, "Latitude", 'type="number" step="0.0001"')}${inp("weather.lon", w.lon, "Longitude", 'type="number" step="0.0001"')}${inp("weather.refreshMin", w.refreshMin, "Refresh (min)", 'type="number" min="5"')}
        <label class="f">History source<select onchange="pp.ovPSet('history.source',this.value)"><option value="wikipedia"${h.source !== "local" ? " selected" : ""}>Wikipedia (on this day)</option><option value="local"${h.source === "local" ? " selected" : ""}>our own list</option></select></label>
        ${inp("historySec", d.historySec, "History rotates (s)", 'type="number" min="4"')}${inp("upNext.count", u.count, "Up next items", 'type="number" min="1" max="6"')}${inp("sponsorSec", d.sponsorSec, "Sponsors rotate (s)", 'type="number" min="3"')}
        <label class="f wide">Up next skips these types (comma)<input value="${E((u.skipTypes || []).join(", "))}" oninput="pp.ovPList('upNext.skipTypes',this.value)"></label>
        ${h.source !== "local" ? `<label class="f wide">History: skip entries with these words (comma; kill* = any word starting with kill)<input value="${E((h.avoid || []).join(", "))}" oninput="pp.ovPList('history.avoid',this.value)"></label>` : ""}
        ${h.source === "local" ? `<label class="f wide">Our own history entries (one per line: <code>1958 | The text</code>)<textarea rows="4" class="mono" oninput="pp.ovPHist(this.value)">${E((h.items || []).map(x => (x.year || "") + " | " + (x.text || "")).join("\n"))}</textarea></label>` : ""}
        ${inp("stars.speed", stars.speed, "Star speed (0 = still)", 'type="number" step="0.1" min="0" max="4"')}${inp("stars.density", stars.density, "Star density", 'type="number" step="0.1" min="0.2" max="3"')}${inp("stars.fps", stars.fps, "Starfield fps", 'type="number" min="8" max="60"')}
      </div>
      <div class="mu sm" style="margin:10px 0 4px">SPONSOR BANNERS (two slots rotate through this list; images from the web root, e.g. /brand/x.png, or any URL)</div>
      <table><tbody>${sp || `<tr><td class="empty">No sponsors: the right bar shows only Coming up.</td></tr>`}</tbody></table>
      <div class="row" style="margin-top:6px"><button class="small" onclick="pp.ovPSpAdd()">+ sponsor</button><span class="fill"></span><button class="ghost small" onclick="pp.ovPReset()">Discard changes</button><button class="small success" onclick="pp.ovPSave()">Save pillar panels</button></div>
    </div><div>
      <div class="mu sm" style="margin-bottom:4px">PREVIEW (demo data; the panels draw only in the bars, the middle is the picture)</div>
      <div style="position:relative;width:100%;aspect-ratio:16/9;background:repeating-conic-gradient(#222 0 25%,#2c2c2c 0 50%) 0 0/24px 24px;border:1px solid var(--b0);overflow:hidden">
        <iframe src="${E(pageUrl)}" style="position:absolute;left:0;top:0;width:1920px;height:1080px;border:0;transform-origin:0 0" onload="this.style.transform='scale('+(this.parentNode.clientWidth/1920)+')'"></iframe></div>
      <div class="row" style="margin-top:6px;gap:6px"><select class="small" onchange="OV.pillar.aspect=this.value;pp.render()">${[["0.5625", "9:16 vertical"], ["0.8", "4:5"], ["1", "1:1 square"], ["1.3333", "4:3"]].map(([v, l]) => `<option value="${v}"${P.aspect === v ? " selected" : ""}>${l}</option>`).join("")}</select>
        <button class="small primary" onclick="pp.ovPPreview(true)">Show on ${E(chName(ch))} for 60 s</button>${pv ? `<button class="small" onclick="pp.ovPPreview(false)">Stop preview</button>` : ""}<button class="ghost small" onclick="pp.ovPRefresh()">Fetch weather + history now</button></div>
      <div class="mu sm" style="margin-top:4px">The on-air preview covers the sides of whatever is playing now, so use it on a narrow item or off air.</div>
      <div class="mu sm" style="margin:10px 0 3px">DATA</div>
      <div class="sm">Weather: ${wx.tempF !== undefined ? E(wx.tempF + "°F " + (wx.cond || "")) + " · " + (wx.updatedMs ? "updated " + fmtAge(Date.now() - wx.updatedMs) + " ago" : "") : `<span class="mu">not fetched yet</span>`}${wx.error ? ` <span style="color:var(--danger)">${E(wx.error)}</span>` : ""}</div>
      <div class="sm">History: ${(hs.items || []).length ? E((hs.items || []).length + " entries for " + (hs.date || "today")) : `<span class="mu">not fetched yet</span>`}${hs.error ? ` <span style="color:var(--danger)">${E(hs.error)}</span>` : ""}</div>
      <div class="mu sm" style="margin:10px 0 3px">LAST DECISION PER CHANNEL</div>
      <table><thead><tr><th>Channel</th><th></th><th>Aspect</th><th>Why</th><th>Item</th></tr></thead><tbody>${dec || `<tr><td colspan="5" class="empty">No item change seen yet.</td></tr>`}</tbody></table>
    </div></div>`;
}

// ---------------------------------------------------------------------------------------------------
// CG slots CG1, CG2 ... (docs/overlays.md "CG slots"): stored graphics, each on all switcher inputs, some or none
// ---------------------------------------------------------------------------------------------------
async function loadCg() {
  const C = OV.cg;
  try { const d = await quiet("GET", "/overlays/cgslots"); C.slots = d.slots || []; C.inputs = d.inputs || 8; C.err = ""; }
  catch (e) { C.err = e.status === 404 ? "This Supervisor has no CG slots yet (GET /api/v1/overlays/cgslots answered 404): update and restart it." : e.message || String(e); }
  try { const s = await quiet("GET", "/settings/ui.inputMap"); const o = s && s.value ? JSON.parse(s.value) : null; C.names = ((o && o.slots) || []).map(x => (x && x.title) || ""); } catch { C.names = []; }
  C.loaded = true; if (pp.S.view === "overlays") pp.render();
}
const cgLayer = (ch, id) => layersOf(ch).find(l => l.state !== "hiding" && (l.presetId === "cg:" + id || l.ruleId === "cg:" + id));
async function saveCg(list, msg) {
  try { const d = await pp.api("PUT", "/overlays/cgslots", { slots: list }); OV.cg.slots = d.slots || list; if (msg) toast(msg, "ok"); }
  catch (e) { toast("Could not save the CGs: " + (e.message || e), "err"); }
  pp.render();
}
const cgPatch = (id, patch, msg) => saveCg(OV.cg.slots.map(s => (s.id === id ? Object.assign({}, s, patch) : s)), msg);
function cgTab(ch) {
  const C = OV.cg; if (!C.loaded) { loadCg(); return `<div class="body mu">Loading CG slots…</div>`; }
  if (C.err) return `<div class="body" style="color:var(--danger)">${E(C.err)}</div>`;
  const n = C.inputs || 8;
  const rows = C.slots.map(s => {
    const all = (s.inputs || []).includes("*"), on = cgLayer(ch, s.id), empty = !s.template;
    const box = (key, label, checked, title) => `<label class="ovCgIn" title="${E(title || "")}"><input type="checkbox" style="width:auto" ${checked ? "checked" : ""} ${empty ? "disabled" : ""} onchange="pp.ovCgIn(${jsq(s.id)},${jsq(key)})">${E(label)}</label>`;
    const ins = box("*", "ALL", all, "every switcher input") + Array.from({ length: n }, (_, k) => box("in" + (k + 1), String(k + 1), all || (s.inputs || []).includes("in" + (k + 1)), C.names[k] || "not mapped")).join("");
    return `<tr><td><b>${E(s.name)}</b>${on ? ` <span class="ovCgOn">ON AIR</span>` : ""}</td>
      <td>${empty ? `<span class="mu">empty</span>` : `${E(tplName(s.template))}<div class="mu sm mono">${E(summary(s.params))}</div>`}</td>
      <td style="white-space:nowrap">${ins}</td>
      <td><input type="number" min="0" step="0.5" style="width:56px" value="${E(s.startOffset ?? 1)}" ${empty ? "disabled" : ""} onchange="pp.ovCgNum(${jsq(s.id)},'startOffset',this.value)"></td>
      <td><input type="number" min="0" step="1" style="width:56px" value="${E(s.duration ?? 0)}" ${empty ? "disabled" : ""} onchange="pp.ovCgNum(${jsq(s.id)},'duration',this.value)"></td>
      <td style="white-space:nowrap"><button class="success small" ${empty ? "disabled" : ""} onclick="pp.ovCgFire(${jsq(s.id)})">${on ? "Hide" : "Fire"}</button> <button class="small" onclick="pp.ovCgStore(${jsq(s.id)})" title="Store the Deploy form's template and fields here">Store form</button> <button class="small" ${empty ? "disabled" : ""} onclick="pp.ovCgLoad(${jsq(s.id)})">Edit</button> <button class="ghost small" ${empty ? "disabled" : ""} onclick="pp.ovCgClear(${jsq(s.id)})">clear</button> <button class="ghost small" ${C.slots.length > 1 ? "" : "disabled"} onclick="pp.ovCgDel(${jsq(s.id)})">&#10005;</button></td></tr>`;
  }).join("");
  return `<div class="scroll" style="max-height:48vh"><table><thead><tr><th>CG</th><th>Graphic</th><th>On inputs</th><th title="seconds after the take">Start s</th><th title="0 = while the input is on air">Stay s</th><th></th></tr></thead><tbody>${rows}</tbody></table></div>
    <div class="row" style="margin-top:6px"><button class="small" onclick="pp.ovCgAdd()">+ CG</button><span class="fill"></span><button class="ghost small" onclick="pp.ovCgReload()">reload</button></div>
    <div class="body mu sm">A CG assigned to an input goes up <b>Start</b> seconds after that input is taken to air (On Air page, keys 1–${n}) and stays while it is on air (or <b>Stay</b> seconds). An “all inputs” CG stays up across a cut to another input that has it. Fire puts it on <b>${E(chName(ch))}</b> by hand. In the native app: right-click an input > CG on this input, or right-click CG1… in the Graphics deck.</div>`;
}

const TABS = [["deploy", "Deploy"], ["presets", "Presets"], ["cg", "CG slots"], ["defaults", "Defaults"], ["rules", "Rules"], ["cues", "Cues"], ["pillar", "Pillar panels"], ["renderer", "Renderer"]];
export function viewOverlays() {
  hookOnce(); if (typeof queueMicrotask === "function") queueMicrotask(postRender);
  const ch = cur(); let body;
  if (OV.tab === "presets") body = presetsTab(ch); else if (OV.tab === "cg") body = cgTab(ch); else if (OV.tab === "defaults") body = defaultsTab(ch); else if (OV.tab === "rules") body = rulesTab(); else if (OV.tab === "cues") body = cuesTab(ch); else if (OV.tab === "pillar") body = pillarTab(ch); else if (OV.tab === "renderer") body = rendererTab(ch);
  else body = `<div class="ovDeploy">${galleryHtml()}<div>${formHtml(ch)}</div>${stageHtml()}</div>`;
  return `<div class="ov"><style>${CSS}</style>${boardHtml(ch)}
    <div class="panel"><h3>Graphics <span class="right"><button class="ghost small" title="Generate a template with AI from a prompt and examples, or design one by hand" onclick="window.open('/cg/designer.html')">✦ CG Designer</button>${chPicker()}</span></h3><div class="ovTabs">${TABS.map(([k, l]) => `<button class="ghost small${OV.tab === k ? " active" : ""}" onclick="pp.ovTab('${k}')">${l}${k === "presets" ? " " + OV.presets.length : k === "defaults" ? " " + defaultsOf(ch).length : k === "rules" ? " " + OV.rules.length : ""}</button>`).join("")}</div>${body}</div></div>`;
}

// ---------------------------------------------------------------------------------------------------
// Handlers (window.pp.*)
// ---------------------------------------------------------------------------------------------------
const setPath = (obj, path, v) => { const parts = path.split("."); let o = obj; for (let k = 0; k < parts.length - 1; k++) { o[parts[k]] = o[parts[k]] || {}; o = o[parts[k]]; } o[parts[parts.length - 1]] = v; };
const getPath = (obj, path) => path.split(".").reduce((o, k) => (o ? o[k] : undefined), obj);
const confirmDo = (t, fn) => pp.confirmDo(t, fn);
const tplMenuSpec = id => { const t = tplOf(id); return t ? (id === OV.sel ? specFromForm() : defaultSpec(t)) : null; };
function newPresetFrom(spec, name) {
  const hk = normHotkey(prompt("Hotkey (1, F2, ctrl+3; blank = none)", "")); if (hk === null) { toast("That is not a usable hotkey", "err"); return null; }
  if (hk && RESERVED_KEYS.includes(hk)) { toast(hkDisp(hk) + " is reserved by the browser or the dashboard", "err"); return null; }
  return { id: uid("p"), name, hotkey: hk, template: spec.template, params: clone(spec.params || {}), animation: normAnim(spec.animation), role: spec.role || "", mode: spec.mode || "auto", holdSec: num(spec.holdSec, 0) };
}
function addPresetFrom(spec, title) {
  if (!spec) { toast("Pick a template first"); return; }
  const name = prompt("Preset name", title || tplName(spec.template)); if (!name) return; const p = newPresetFrom(spec, name.trim()); if (p) savePresets(OV.presets.concat([p])).then(() => toast("Preset “" + p.name + "” saved", "ok")).catch(() => {});
}
function addDefaultFrom(ch, spec) {
  if (!spec) { toast("Pick a template first"); return; }
  const d = { id: uid("d"), template: spec.template, params: clone(spec.params || {}), animation: normAnim(spec.animation), role: spec.role || "", mode: spec.mode || "auto", enabled: true };
  if (spec.slot > 0) d.slot = spec.slot;
  saveDefaults(ch, defaultsOf(ch).concat([d])).then(() => toast("Default added to " + chName(ch), "ok")).catch(() => {});
}
const dupRole = (ch, spec) => Object.assign({}, spec, { slot: freeSlot(ch, spec.role || (tplOf(spec.template) || {}).role || "") });

export const ppExtOverlays = {
  // --- navigation / form -----------------------------------------------------------------------------
  ovTab(t) { OV.tab = t; if (t === "cues") loadCues(); if (t === "renderer") loadAll(); if (t === "pillar") loadPillar(); if (t === "cg") loadCg(); pp.render(); },
  // --- CG slots ----------------------------------------------------------------------------------------
  ovCgReload() { loadCg(); },
  ovCgAdd() { const ids = OV.cg.slots.map(s => s.id); let n = ids.length + 1; while (ids.includes("cg" + n)) n++; saveCg(OV.cg.slots.concat([{ id: "cg" + n, name: "CG" + n }]), "CG" + n + " added"); },
  ovCgDel(id) { const s = OV.cg.slots.find(x => x.id === id); confirmDo("Remove " + (s ? s.name : id) + "?", () => saveCg(OV.cg.slots.filter(x => x.id !== id), (s ? s.name : id) + " removed")); },
  ovCgClear(id) { const s = OV.cg.slots.find(x => x.id === id); cgPatch(id, { template: "", params: {}, inputs: [] }, (s ? s.name : id) + " cleared"); },
  ovCgNum(id, k, v) { cgPatch(id, { [k]: Math.max(0, num(v, k === "startOffset" ? 1 : 0)) }); },
  ovCgIn(id, key) {
    const s = OV.cg.slots.find(x => x.id === id); if (!s) return; let a = (s.inputs || []).slice(); const all = a.includes("*");
    if (key === "*") a = all ? [] : ["*"];
    else if (all) { a = []; for (let k = 1; k <= (OV.cg.inputs || 8); k++) if ("in" + k !== key) a.push("in" + k); }
    else { const j = a.indexOf(key); if (j >= 0) a.splice(j, 1); else a.push(key); }
    cgPatch(id, { inputs: a });
  },
  ovCgStore(id) {
    const sp = specFromForm(); if (!sp) { toast("Pick a template in Deploy first"); return; }
    const s = OV.cg.slots.find(x => x.id === id); if (!s) return;
    const go = () => cgPatch(id, { template: sp.template, params: clone(sp.params || {}), animation: sp.animation, role: sp.role || "", mode: sp.mode || "auto", holdSec: sp.holdSec || 0 }, s.name + " ← " + tplName(sp.template));
    if (s.template) confirmDo("Replace " + s.name + " (" + tplName(s.template) + ") with " + tplName(sp.template) + "?", go); else go();
  },
  ovCgStorePick() {
    const sp = specFromForm(); if (!sp) { toast("Pick a template first"); return; }
    if (!OV.cg.loaded) { loadCg().then(() => { if (OV.cg.slots.length) pp.ovCgStorePick(); }); return; }
    const free = OV.cg.slots.find(s => !s.template), want = prompt("Store " + tplName(sp.template) + " in which CG? (" + OV.cg.slots.map(s => s.name).join(", ") + ")", free ? free.name : (OV.cg.slots[0] || {}).name || "CG1");
    if (!want) return; const s = OV.cg.slots.find(x => x.name.toLowerCase() === want.trim().toLowerCase() || x.id === want.trim().toLowerCase());
    if (!s) { toast("No CG called " + want, "err"); return; } pp.ovCgStore(s.id);
  },
  ovCgLoad(id) { const s = OV.cg.slots.find(x => x.id === id); if (!s) return; loadSpec(s); OV.editing = null; OV.tab = "deploy"; toast("Loaded " + s.name + " into the form: change it, then CG slots > Store form", "ok"); pp.render(); },
  ovCgFire(id) {
    const ch = cur(), on = cgLayer(ch, id), s = OV.cg.slots.find(x => x.id === id) || { name: id };
    pp.api("POST", "/overlays/" + enc(ch) + "/cgslots/" + enc(id) + (on ? "/hide" : "/fire"), on ? {} : { toggle: true }).then(() => { toast(s.name + (on ? " off" : " on air on " + chName(ch)), "ok"); refreshSoon(); }).catch(() => {});
  },
  ovRefresh() { loadAll(); loadCues(); },
  ovCat(c) { OV.cat = c; OV.q = ""; OV.sc.gal = 0; pp.render(); },
  ovQ(v) { OV.q = v; OV.sc.gal = 0; pp.render(); },
  ovLive(on) { OV.live = !!on; try { localStorage.setItem("pp.ov.live", on ? "1" : "0"); } catch {} pp.render(); },
  ovPick(id) { if (id !== OV.sel) { OV.sel = id; OV.params = {}; OV.role = ""; OV.slot = 0; OV.label = ""; } stageNow(); pp.render(); },
  ovParam(name, v) { OV.params[name] = v; previewSoon(); },
  ovAnim(k, v) { OV.anim[k] = (k === "duration" || k === "outDuration") ? Math.max(0, num(v, 0)) : v; previewSoon(); if (k === "in" || k === "out") pp.render(); },
  ovAnimPreset(id) { const p = OV.man.presets.find(x => x.id === id); if (!p) return; OV.anim = normAnim({ in: p.in, out: p.out, duration: p.duration, outDuration: p.duration, easing: p.easing }); stageNow(); pp.render(); },
  ovSet(k, v) { OV[k] = k === "slot" ? Math.max(0, Math.floor(num(v, 0))) : k === "hold" ? Math.max(0, num(v, 0)) : v; if (k === "role" || k === "mode") pp.render(); },
  ovReset() { OV.params = {}; OV.anim = normAnim(); OV.role = ""; OV.slot = 0; OV.hold = 0; OV.mode = "auto"; OV.label = ""; stageNow(); pp.render(); },
  ovReplay() { OV.nonce++; pp.render(); },
  ovCopyUrl() { const t = tplOf(OV.sel); if (t) copyText(absUrl(cgUrl(t, OV.params, OV.anim))); },
  // --- deploy ----------------------------------------------------------------------------------------
  ovDeploy(all) { deployTo(all ? allCh() : [cur()], specFromForm()); },
  ovSaveEdit() {
    const ed = OV.editing, s = specFromForm(); if (!ed || !s) return;
    if (ed.kind === "preset") { const i = OV.presets.findIndex(p => p.id === ed.id); if (i < 0) { toast("That preset is gone", "err"); return; } const old = OV.presets[i]; const list = OV.presets.slice(); list[i] = Object.assign({}, old, { template: s.template, params: s.params, animation: s.animation, role: s.role, mode: s.mode, holdSec: s.holdSec }); savePresets(list).then(() => { toast("Preset saved", "ok"); OV.editing = null; pp.render(); }).catch(() => {}); }
    else { const ch = cur(), list = defaultsOf(ch).slice(), i = list.findIndex(d => d.id === ed.id); if (i < 0) { toast("That default is gone", "err"); return; } list[i] = Object.assign({}, list[i], { template: s.template, params: s.params, animation: s.animation, role: s.role, mode: s.mode }); saveDefaults(ch, list).then(() => { toast("Default saved", "ok"); OV.editing = null; pp.render(); }).catch(() => {}); }
  },
  ovCancelEdit() { OV.editing = null; pp.render(); },
  ovSavePreset() { addPresetFrom(specFromForm()); },
  ovAddDefault() { addDefaultFrom(cur(), specFromForm()); },
  // --- layer board -------------------------------------------------------------------------------------
  ovUpd(ch, layer) { const key = ch + ":" + layer; if (OV.open[key]) delete OV.open[key]; else { const l = findLayer(ch, layer); const d = {}; for (const [k, v] of Object.entries((l && l.params) || {})) d[k] = sval(v); OV.open[key] = d; } pp.render(); },
  ovDraft(key, name, v) { if (OV.open[key]) OV.open[key][name] = v; },
  ovApply(ch, layer) { const key = ch + ":" + layer, d = OV.open[key]; if (!d) return; const l = findLayer(ch, layer) || {}; const params = Object.assign({}, l.params || {}); for (const [k, v] of Object.entries(d)) { if (sval(v) === "") delete params[k]; else params[k] = v; } delete OV.open[key]; layerCall(ch, layer, "update", { params }, "Layer " + layer + " updated"); },
  ovRetake(ch, layer) { layerCall(ch, layer, "retake", {}, "Layer " + layer + " re-taken"); },
  ovHide(ch, layer) { layerCall(ch, layer, "hide", {}, "Layer " + layer + " hidden"); },
  ovDel(ch, layer) { confirmDo("Delete layer " + layer + " on " + chName(ch) + "? It is removed from air at once.", () => pp.api("DELETE", "/overlays/" + enc(ch) + "/" + layer).then(() => { const c = OV.channels[ch]; if (c) c.layers = (c.layers || []).filter(x => x.layer !== layer); toast("Layer " + layer + " deleted", "ok"); pp.render(); refreshSoon(); }).catch(() => {})); },
  ovClearDefs(on) { OV.clearDefs = !!on; },
  ovClear(roles, chs) {
    const targets = chs || [cur()], what = roles && roles.length ? roles.join(", ") + " overlays" : "every overlay"; const withDefs = !!OV.clearDefs;
    confirmDo("Hide " + what + " on " + targets.map(chName).join(", ") + (withDefs ? ", always-on defaults included" : " (always-on defaults stay)") + "?", async () => {
      for (const ch of targets) { try { const body = { includeDefaults: withDefs }; if (roles && roles.length) body.roles = roles; const r = await pp.api("POST", "/overlays/" + enc(ch) + "/clear", body); if (r && r.layers) (OV.channels[ch] = OV.channels[ch] || { layers: [], defaults: [] }).layers = r.layers; } catch {} }
      toast("Cleared", "ok"); pp.render(); refreshSoon();
    });
  },
  ovClearMenu(ev) {
    const ch = cur(), roles = [...new Set(layersOf(ch).map(l => l.role).filter(Boolean))];
    ctxMenu(ev, [{ label: "Clear " + chName(ch), fn: () => pp.ovClear() }, { sep: true }, ...roles.map(r => ({ label: "Clear " + r, fn: () => pp.ovClear([r]) })), roles.length ? { sep: true } : null, { label: "Clear every channel", danger: true, fn: () => pp.ovClear(null, allCh()) }]);
  },
  // --- presets -----------------------------------------------------------------------------------------
  ovPresetFire(id, all) { const p = OV.presets.find(x => x.id === id); if (p) presetFire(p, all ? allCh() : [cur()]); },
  ovPresetHide(id) { const p = OV.presets.find(x => x.id === id); if (p) presetHide(p, [cur()]); },
  ovPresetEdit(id) { const p = OV.presets.find(x => x.id === id); if (!p) return; loadSpec(p); OV.editing = { kind: "preset", id }; OV.tab = "deploy"; pp.render(); },
  ovPresetKey(id, v) {
    const h = normHotkey(v); if (h === null) { toast("That is not a usable hotkey (1, F2, ctrl+3, alt+x)", "err"); pp.render(); return; }
    if (h && RESERVED_KEYS.includes(h)) { toast(hkDisp(h) + " is reserved by the browser or the dashboard", "err"); pp.render(); return; }
    const clash = h && OV.presets.find(p => p.id !== id && normHotkey(p.hotkey) === h); if (clash) toast(hkDisp(h) + " is also used by “" + clash.name + "”", "err");
    savePresets(OV.presets.map(p => (p.id === id ? Object.assign({}, p, { hotkey: h }) : p))).catch(() => {});
  },
  ovPresetDel(id) { const p = OV.presets.find(x => x.id === id); if (p) confirmDo("Delete preset “" + p.name + "”?", () => savePresets(OV.presets.filter(x => x.id !== id)).catch(() => {})); },
  // --- defaults ----------------------------------------------------------------------------------------
  ovDefToggle(id, on) { const ch = cur(); saveDefaults(ch, defaultsOf(ch).map(d => (d.id === id ? Object.assign({}, d, { enabled: !!on }) : d))).catch(() => {}); },
  ovDefNow(id) { const d = defaultsOf(cur()).find(x => x.id === id); if (d) deployTo([cur()], d); },
  ovDefEdit(id) { const d = defaultsOf(cur()).find(x => x.id === id); if (!d) return; loadSpec(d); OV.editing = { kind: "default", id }; OV.tab = "deploy"; pp.render(); },
  ovDefDel(id) { const ch = cur(), d = defaultsOf(ch).find(x => x.id === id); if (d) confirmDo("Remove the always-on " + tplName(d.template) + " from " + chName(ch) + "?", () => saveDefaults(ch, defaultsOf(ch).filter(x => x.id !== id)).catch(() => {})); },
  // --- rules -------------------------------------------------------------------------------------------
  ovRuleNew() { OV.ruleEdit = { isNew: true, r: { id: uid("r"), name: "", enabled: true, channels: [], when: { types: ["ad"], categories: [], keywords: [], series: [] }, then: { suppress: ["lower_third", "bug"] } }, paramsText: "" }; pp.render(); },
  ovRuleEdit(id) {
    const src = OV.rules.find(x => x.id === id); if (!src) return; const r = clone(src); let extra = [];
    if (r.then && Array.isArray(r.then.show)) { extra = r.then.show.slice(1); r.then.show = r.then.show[0]; }      // the editor edits the first graphic, the rest ride along
    const sh = (r.then || {}).show; OV.ruleEdit = { isNew: false, r, extraShows: extra, paramsText: sh && sh.params && Object.keys(sh.params).length ? JSON.stringify(sh.params, null, 2) : "" }; pp.render();
  },
  ovRulePreset(k) { const mk = RULE_PRESETS[k]; if (!mk) return; const b = mk(); const sh = (b.then || {}).show; OV.ruleEdit = { isNew: true, r: Object.assign({ id: uid("r"), enabled: true, channels: [] }, b), paramsText: sh && sh.params ? JSON.stringify(sh.params, null, 2) : "" }; OV.tab = "rules"; pp.render(); },
  ovRuleCancel() { OV.ruleEdit = null; pp.render(); },
  ovRuleSet(path, v) { if (OV.ruleEdit) setPath(OV.ruleEdit.r, path, v); },
  ovRuleList(path, v) { if (OV.ruleEdit) setPath(OV.ruleEdit.r, path, String(v).split(",").map(s => s.trim()).filter(Boolean)); },
  ovRuleTog(path, v) { if (!OV.ruleEdit) return; const cur_ = getPath(OV.ruleEdit.r, path) || []; setPath(OV.ruleEdit.r, path, cur_.includes(v) ? cur_.filter(x => x !== v) : cur_.concat([v])); pp.render(); },
  ovRuleShow(k, v) {
    if (!OV.ruleEdit) return; const th = (OV.ruleEdit.r.then = OV.ruleEdit.r.then || {}); th.show = th.show || {};
    if (k === "anim") { const p = OV.man.presets.find(x => x.id === v); th.show.animation = p ? normAnim({ in: p.in, out: p.out, duration: p.duration, outDuration: p.duration, easing: p.easing }) : normAnim(OV.anim); }
    else th.show[k] = v;
    if (!th.show.template) delete th.show; pp.render();
  },
  ovRuleSave() {
    const ed = OV.ruleEdit; if (!ed) return; const r = clone(ed.r); r.name = (r.name || "").trim() || "Rule " + (OV.rules.length + 1);
    if (r.then && r.then.show) { let params = {}; if (ed.paramsText.trim()) { try { params = JSON.parse(ed.paramsText); } catch { toast("Params JSON is not valid", "err"); return; } } r.then.show.params = params; if (!r.then.show.animation) r.then.show.animation = normAnim(OV.anim); }
    if ((ed.extraShows || []).length) r.then.show = (r.then.show ? [r.then.show] : []).concat(ed.extraShows);
    if (!((r.then || {}).suppress || []).length && !(r.then || {}).show) { toast("A rule needs something to suppress or show", "err"); return; }
    const list = OV.rules.slice(), i = list.findIndex(x => x.id === r.id); if (i >= 0) list[i] = r; else list.push(r);
    saveRules(list).then(() => { toast("Rule saved", "ok"); OV.ruleEdit = null; pp.render(); }).catch(() => {});
  },
  ovRuleEnable(id, on) { saveRules(OV.rules.map(r => (r.id === id ? Object.assign({}, r, { enabled: !!on }) : r))).catch(() => {}); },
  ovRuleDel(id) { const r = OV.rules.find(x => x.id === id); if (r) confirmDo("Delete rule “" + r.name + "”?", () => saveRules(OV.rules.filter(x => x.id !== id)).catch(() => {})); },
  ovRuleTestFor(id) {
    const rt = OV.ruleTest; const ch = rt.channel || cur(); const mid = rt.mediaId.trim() || (pp.S.telemetry[ch] || {}).assetId || ""; if (!mid) { toast("Type a media id, an input (in3) or type:ad (or let something play) to test against"); return; }
    rt.busy = true; rt.res = null; pp.render();
    const q = /^in\d{1,2}$/i.test(mid) ? "&input=" + enc(mid) + "&type=live&live=1" : /^type:/i.test(mid) ? "&type=" + enc(mid.slice(5)) : "&mediaId=" + enc(mid);   // in3 = switcher input 3; type:ad = an item of that type
    pp.api("GET", "/overlays/rules/test?channel=" + enc(ch) + q + (id ? "&rule=" + enc(id) : "")).then(r => { rt.res = r; }).catch(() => {}).finally(() => { rt.busy = false; pp.render(); });
  },
  // --- cues --------------------------------------------------------------------------------------------
  ovCuesReload() { loadCues(); pp.render(); },
  ovCueFire(xi, qi) { const x = OV.cues.items[xi], q = x && x.cues[qi]; if (q) deployTo([cur()], cueSpec(q.c, x.it, x.m)); },
  // --- pillar panels -----------------------------------------------------------------------------------
  ovPSet(path, v) { const P = OV.pillar; if (!P.draft) return; const numeric = /^(maxAspect|historySec|sponsorSec|weather\.(lat|lon|refreshMin)|upNext\.count|stars\.)/.test(path); setPath(P.draft, path, typeof v === "boolean" ? v : numeric ? num(v, 0) : v); if (typeof v === "boolean" || path === "history.source") pp.render(); },
  ovPList(path, v) { const P = OV.pillar; if (P.draft) setPath(P.draft, path, String(v).split(",").map(x => x.trim()).filter(Boolean)); },
  ovPHist(v) { const P = OV.pillar; if (!P.draft) return; const items = String(v).split("\n").map(l => { const m = l.split("|"); return { year: num(m[0], 0), text: m.slice(1).join("|").trim() }; }).filter(x => x.text); setPath(P.draft, "history.items", items); },
  ovPSp(i, k, v) { const P = OV.pillar; if (!P.draft) return; const a = P.draft.sponsors = P.draft.sponsors || []; a[i] = Object.assign({}, a[i], { [k]: v }); },
  ovPSpAdd() { const P = OV.pillar; if (!P.draft) return; (P.draft.sponsors = P.draft.sponsors || []).push({ name: "", tagline: "", image: "", color: "#ffc94a" }); pp.render(); },
  ovPSpDel(i) { const P = OV.pillar; if (!P.draft) return; (P.draft.sponsors || []).splice(i, 1); pp.render(); },
  ovPReset() { const P = OV.pillar; P.draft = clone((P.st && P.st.config) || {}); pp.render(); },
  ovPSave() {
    const P = OV.pillar; if (!P.draft) return; const d = clone(P.draft); d.sponsors = (d.sponsors || []).filter(x => (x.name || "").trim() || (x.image || "").trim());
    pp.api("PUT", "/pillar/config", { config: d }).then(r => { P.draft = clone((r && r.config) || d); toast("Pillar panels saved" + (d.enabled ? " — they appear on the next narrow item" : " (off)"), "ok"); loadPillar(); }).catch(() => {});
  },
  ovPPreview(on) { const ch = cur(); pp.api("POST", "/pillar/" + enc(ch) + "/preview", { on: !!on, aspect: OV.pillar.aspect, sec: 60 }).then(() => { toast(on ? "Pillar panels on " + chName(ch) + " for 60 s" : "Preview stopped", "ok"); setTimeout(loadPillar, 800); }).catch(() => {}); },
  ovPRefresh() { pp.api("POST", "/pillar/refresh", {}).then(() => { toast("Fetching weather and history…", "ok"); setTimeout(loadPillar, 4000); }).catch(() => {}); },
  // --- renderer ----------------------------------------------------------------------------------------
  ovRTest() { OV.rtestBusy = true; OV.rtest = null; pp.render(); pp.api("POST", "/overlays/renderer/test", { channel: cur() }).then(r => { OV.rtest = r; toast(r && r.ok === false ? "Renderer test failed" : "Renderer test finished", r && r.ok === false ? "err" : "ok"); }).catch(() => {}).finally(() => { OV.rtestBusy = false; pp.render(); loadAll(); }); },
  ovRSave() { const bp = (OV.rpath !== null ? OV.rpath : (OV.renderer || {}).preferred || "").trim(); pp.api("PUT", "/overlays/renderer", { browserPath: bp }).then(r => { OV.rpath = null; if (r && r.renderer) OV.renderer = Object.assign({}, OV.renderer, r.renderer); else if (r && r.available !== undefined) OV.renderer = Object.assign({}, OV.renderer, r); toast("Browser path saved — applies to the next engine start", "ok"); loadAll(); }).catch(() => {}); },
};

// ---------------------------------------------------------------------------------------------------
// Context menus (registerCtx: web/ctx.js appends Copy id / Copy as JSON / Properties… to every tagged row)
// ---------------------------------------------------------------------------------------------------
const chTargets = (label, fn) => ({ label, sub: [...chanList().map(c => ({ label: c.profile.name || c.profile.id, fn: () => fn([c.profile.id]) })), { sep: true }, { label: "All channels", fn: () => fn(allCh()) }] });
const moveSub = (list, id, save) => ({ label: "Move", sub: [["Up", -1], ["Down", 1], ["To top", -Infinity], ["To bottom", Infinity]].map(([label, d]) => ({ label, fn: () => { const a = list.slice(), i = a.findIndex(x => x.id === id); if (i < 0) return; const [it] = a.splice(i, 1); a.splice(Math.max(0, Math.min(a.length, d === -Infinity ? 0 : d === Infinity ? a.length : i + d)), 0, it); save(a).catch(() => {}); } })) });
const pasteDis = kinds => !clipFits(kinds);
const listMenu = (name, list, save, pasteKind, mk) => [
  { header: true, label: name },
  { label: "Paste as new", disabled: pasteDis(pasteKind === "rule" ? ["rule"] : SPEC_KINDS), fn: async () => { const o = await (pasteKind === "rule" ? readRuleClip() : readSpecClip()); if (o) save(list.concat([mk(pasteKind === "rule" ? o : asSpec(o))])).then(() => toast("Pasted", "ok")).catch(() => {}); } },
  { label: "Export all (copy JSON)", disabled: !list.length, fn: () => copyJson(list, name) },
  { label: "Import from clipboard JSON…", fn: async () => { try { let a = JSON.parse(await readClipboard()); if (!Array.isArray(a)) a = [a]; a = a.filter(x => x && typeof x === "object"); if (!a.length) return; save(list.concat(a.map(mk))).then(() => toast("Imported " + a.length, "ok")).catch(() => {}); } catch { toast("The clipboard does not hold a JSON list", "err"); } } },
  { label: "Sort by name", disabled: list.length < 2, fn: () => save(list.slice().sort((a, b) => String(a.name || tplName(a.template)).localeCompare(String(b.name || tplName(b.template))))).catch(() => {}) },
  { label: "Refresh", fn: () => loadAll() },
  { sep: true }, { label: "Delete all…", danger: true, disabled: !list.length, fn: () => confirmDo("Delete all " + list.length + " " + name.toLowerCase() + "?", () => save([]).catch(() => {})) },
];
const mkPreset = o => ({ id: uid("p"), name: o.name || tplName(o.template || o.templateId), hotkey: "", template: o.template || o.templateId, params: clone(o.params || {}), animation: normAnim(o.animation), role: o.role || "", mode: o.mode || "auto", holdSec: num(o.holdSec, 0) });
const mkDefault = o => ({ id: uid("d"), template: o.template || o.templateId, params: clone(o.params || {}), animation: normAnim(o.animation), role: o.role || "", mode: o.mode || "auto", enabled: true });
const mkRule = o => Object.assign(clone(o), { id: uid("r") });

registerCtx("ovLayer", ({ id }) => {
  const [ch, ls] = id.split(/:(?=[^:]*$)/), layer = +ls, l = findLayer(ch, layer); if (!l) return [];
  const spec = specOfLayer(l), same = layersOf(ch).filter(x => x.role === l.role && x.layer !== layer);
  return [{ header: true, label: "Layer " + layer + " · " + (l.role || "") },
    { label: "Update fields…", fn: () => pp.ovUpd(ch, layer) }, { label: "Re-take", fn: () => pp.ovRetake(ch, layer) }, { label: "Hide", disabled: !isUp(l.state), fn: () => pp.ovHide(ch, layer) }, { sep: true },
    { label: "Copy", sub: [{ label: "Copy overlay (paste elsewhere)", fn: () => setClipK("overlay", spec, l.name || l.template) }, { label: "Copy params JSON", fn: () => copyJson(l.params || {}, "params") }, { label: "Copy template id", fn: () => copyText(l.template) }, { label: "Copy page URL", disabled: !tplOf(l.template), fn: () => copyText(absUrl(cgUrl(tplOf(l.template), l.params || {}, l.animation))) }] },
    { label: "Paste params into this layer", disabled: pasteDis(SPEC_KINDS), fn: async () => { const s = await readSpecClip(); if (s) layerCall(ch, layer, "update", { params: Object.assign({}, l.params || {}, asSpec(s).params || {}) }, "Params pasted into layer " + layer); } },
    { label: "Paste as new overlay here", disabled: pasteDis(SPEC_KINDS), fn: async () => { const s = await readSpecClip(); if (s && asSpec(s).template) deployTo([ch], asSpec(s)); else if (s) toast("The clipboard has no template", "err"); } }, { sep: true },
    { label: "Duplicate (next free slot)", fn: () => deployTo([ch], dupRole(ch, spec)) }, chTargets("Copy to channel", chs => deployTo(chs, spec)),
    { label: "Save as preset…", fn: () => addPresetFrom(spec, l.label || l.name) }, { label: "Set as default", sub: [{ label: "This channel", fn: () => addDefaultFrom(ch, spec) }, { label: "All channels", fn: () => allCh().forEach(c => addDefaultFrom(c, spec)) }] },
    { label: "Load into the deploy form", fn: () => { loadSpec(spec); OV.tab = "deploy"; OV.editing = null; pp.render(); } }, { sep: true },
    { label: "Delete layer", danger: true, fn: () => pp.ovDel(ch, layer) }, { label: "Delete all " + (l.role || "other") + " layers (" + (same.length + 1) + ")", danger: true, disabled: !same.length, fn: () => pp.ovClear([l.role]) }];
});
registerCtx("ovBoard", () => { const ch = cur(); return [{ header: true, label: "Overlays on " + chName(ch) }, { label: "Paste as new overlay", disabled: pasteDis(SPEC_KINDS), fn: async () => { const s = await readSpecClip(); if (s && asSpec(s).template) deployTo([ch], asSpec(s)); } }, { label: "Copy all layers (JSON)", disabled: !layersOf(ch).length, fn: () => copyJson(layersOf(ch), "layers") }, { label: "Refresh", fn: () => pp.ovRefresh() }, { sep: true }, { label: "Clear " + chName(ch), danger: true, disabled: !layersOf(ch).length, fn: () => pp.ovClear() }, { label: "Clear every channel…", danger: true, fn: () => pp.ovClear(null, allCh()) }]; });
registerCtx("ovTpl", ({ id }) => {
  const t = tplOf(id); if (!t) return []; const ch = cur(), spec = () => (id === OV.sel ? specFromForm() : defaultSpec(t)), u = () => absUrl(cgUrl(t, id === OV.sel ? OV.params : {}, OV.anim));
  return [{ header: true, label: t.name }, { label: "Deploy to " + chName(ch), fn: () => deployTo([ch], spec()) }, chTargets("Deploy to", chs => deployTo(chs, spec())), { label: "Preview / edit in the form", fn: () => pp.ovPick(id) }, { label: t.custom ? "Edit in CG Designer" : "Edit a copy in CG Designer", fn: () => window.open("/cg/designer.html?" + (t.custom ? "open=" : "copy=") + encodeURIComponent(id)) }, { sep: true },
    { label: "Copy", sub: [{ label: "Copy URL", fn: () => copyText(u()) }, { label: "Copy template id", fn: () => copyText(id) }, { label: "Copy name", fn: () => copyText(t.name) }, { label: "Copy as overlay (paste elsewhere)", fn: () => setClipK("overlay", spec(), t.name) }, { label: "Copy manifest JSON", fn: () => copyJson(t, t.name) }] },
    { label: "Paste params into the form", disabled: pasteDis(SPEC_KINDS), fn: async () => { const s = await readSpecClip(); if (s) { loadSpec(Object.assign({}, asSpec(s), { template: id })); OV.editing = null; pp.render(); } } }, { sep: true },
    { label: "Add to presets…", fn: () => addPresetFrom(spec(), t.name) }, { label: "Add as default", sub: [{ label: "This channel", fn: () => addDefaultFrom(ch, spec()) }, { label: "All channels", fn: () => allCh().forEach(c => addDefaultFrom(c, spec())) }] },
    { label: "Open in a new tab", fn: () => window.open(u()) }, { label: "Delete", disabled: true, title: t.custom ? "Designed templates are deleted in the CG Designer" : "Templates are files in web/cg (templates.json): remove them there", fn: () => {} }, { sep: true }, { label: "Properties…", fn: () => showProps(t.name, t) }];
});
registerCtx("ovGallery", () => [{ header: true, label: "Template gallery" }, { label: "Refresh", fn: () => loadAll() }, { label: "Live previews", checked: OV.live, fn: () => pp.ovLive(!OV.live) }, { label: "Category", sub: [{ label: "All", checked: !OV.cat, fn: () => pp.ovCat("") }, ...[...new Set(OV.man.templates.map(t => t.category))].map(c => ({ label: c, checked: OV.cat === c, fn: () => pp.ovCat(c) }))] }, { label: "Copy template ids", fn: () => copyText(OV.man.templates.map(t => t.id).join("\n"), "ids") }, { label: "Copy manifest (JSON)", fn: () => copyJson(OV.man, "manifest") }]);
registerCtx("ovStage", () => { const ch = cur(), t = tplOf(OV.sel); return [{ header: true, label: "Preview" }, { label: "Replay", fn: () => pp.ovReplay() }, { label: "Deploy to " + chName(ch), disabled: !t, fn: () => pp.ovDeploy(false) }, { label: "Deploy to all channels", disabled: !t, fn: () => pp.ovDeploy(true) }, { label: "Copy URL", disabled: !t, fn: () => pp.ovCopyUrl() }, { label: "Open in a new tab", disabled: !t, fn: () => window.open(OV.stageUrl) }, { label: "Save as preset…", disabled: !t, fn: () => pp.ovSavePreset() }]; });
registerCtx("ovField", ({ el, ev }) => {
  const inp = ev && ev.target && ev.target.closest ? ev.target.closest("input,textarea") : null; if (!inp) return [];
  const put = s => { inp.focus(); if (inp.setRangeText) { inp.setRangeText(s, inp.selectionStart ?? inp.value.length, inp.selectionEnd ?? inp.value.length, "end"); inp.dispatchEvent(new Event("input", { bubbles: true })); } };
  const p = ((tplOf(el.dataset.ctxTpl) || {}).params || []).find(x => x.name === el.dataset.ctxParam);
  return [{ header: true, label: "Field" }, { label: "Insert token", sub: TOKENS.map(k => ({ label: k, fn: () => put(k) })) }, { label: "Reset to default", disabled: !p || inp.type === "checkbox", fn: () => { inp.value = sval(p.default); inp.dispatchEvent(new Event("input", { bubbles: true })); inp.dispatchEvent(new Event("change", { bubbles: true })); } }];
});
registerCtx("ovPreset", ({ id }) => {
  const p = OV.presets.find(x => x.id === id); if (!p) return []; const ch = cur();
  return [{ header: true, label: "Preset · " + p.name }, { label: "Fire on " + chName(ch), fn: () => presetFire(p, [ch]) }, chTargets("Fire on", chs => presetFire(p, chs)), { label: "Hide on " + chName(ch), fn: () => presetHide(p, [ch]) }, { label: "Edit…", fn: () => pp.ovPresetEdit(id) }, { sep: true },
    { label: "Copy", sub: [{ label: "Copy preset (paste elsewhere)", fn: () => setClipK("preset", p, p.name) }, { label: "Copy name", fn: () => copyText(p.name) }, { label: "Copy hotkey", disabled: !p.hotkey, fn: () => copyText(hkDisp(p.hotkey)) }, { label: "Copy template id", fn: () => copyText(p.template) }] },
    { label: "Paste as new preset", disabled: pasteDis(SPEC_KINDS), fn: async () => { const o = await readSpecClip(); if (o) savePresets(OV.presets.concat([mkPreset(asSpec(o))])).catch(() => {}); } },
    { label: "Duplicate", fn: () => savePresets(OV.presets.concat([Object.assign(clone(p), { id: uid("p"), name: p.name + " copy", hotkey: "" })])).catch(() => {}) },
    { label: "Set hotkey…", fn: () => { const v = prompt("Hotkey (1, F2, ctrl+3; blank = none)", hkDisp(p.hotkey)); if (v !== null) pp.ovPresetKey(id, v); } }, moveSub(OV.presets, id, savePresets), { sep: true },
    { label: "Delete preset", danger: true, fn: () => pp.ovPresetDel(id) }];
});
registerCtx("ovPresets", () => listMenu("Presets", OV.presets, savePresets, "spec", mkPreset));
registerCtx("ovDefault", ({ id }) => {
  const ch = cur(), d = defaultsOf(ch).find(x => x.id === id); if (!d) return [];
  return [{ header: true, label: "Default · " + tplName(d.template) }, { label: "Deploy now", fn: () => pp.ovDefNow(id) }, { label: d.enabled !== false ? "Disable" : "Enable", fn: () => pp.ovDefToggle(id, d.enabled === false) }, { label: "Edit…", fn: () => pp.ovDefEdit(id) }, { sep: true },
    { label: "Copy", sub: [{ label: "Copy default (paste elsewhere)", fn: () => setClipK("default", d, tplName(d.template)) }, { label: "Copy template id", fn: () => copyText(d.template) }] },
    { label: "Paste as new default", disabled: pasteDis(SPEC_KINDS), fn: async () => { const o = await readSpecClip(); if (o) saveDefaults(ch, defaultsOf(ch).concat([mkDefault(asSpec(o))])).catch(() => {}); } },
    { label: "Duplicate", fn: () => saveDefaults(ch, defaultsOf(ch).concat([Object.assign(clone(d), { id: uid("d") })])).catch(() => {}) },
    chTargets("Apply to channel", chs => Promise.all(chs.filter(c => c !== ch).map(c => saveDefaults(c, defaultsOf(c).concat([Object.assign(clone(d), { id: uid("d") })])).catch(() => {}))).then(() => toast("Copied", "ok"))), moveSub(defaultsOf(ch), id, l => saveDefaults(ch, l)), { sep: true },
    { label: "Delete default", danger: true, fn: () => pp.ovDefDel(id) }];
});
registerCtx("ovDefaults", () => { const ch = cur(); return listMenu("Defaults on " + chName(ch), defaultsOf(ch), l => saveDefaults(ch, l), "spec", mkDefault); });
registerCtx("ovRule", ({ id }) => {
  const r = OV.rules.find(x => x.id === id); if (!r) return [];
  const setCh = cs => saveRules(OV.rules.map(x => (x.id === id ? Object.assign({}, x, { channels: cs }) : x))).catch(() => {});
  return [{ header: true, label: "Rule · " + r.name }, { label: r.enabled !== false ? "Disable" : "Enable", fn: () => pp.ovRuleEnable(id, r.enabled === false) }, { label: "Edit…", fn: () => pp.ovRuleEdit(id) }, { label: "Explain against a media id", fn: () => pp.ovRuleTestFor(id) }, { sep: true },
    { label: "Copy", sub: [{ label: "Copy rule (paste elsewhere)", fn: () => setClipK("rule", r, r.name) }, { label: "Copy name", fn: () => copyText(r.name) }] },
    { label: "Paste as new rule", disabled: pasteDis(["rule"]), fn: async () => { const o = await readRuleClip(); if (o) saveRules(OV.rules.concat([mkRule(o)])).catch(() => {}); } },
    { label: "Duplicate", fn: () => saveRules(OV.rules.concat([Object.assign(clone(r), { id: uid("r"), name: r.name + " copy" })])).catch(() => {}) },
    { label: "Channels", sub: [{ label: "Apply to all channels", checked: !(r.channels || []).length, fn: () => setCh([]) }, ...chanList().map(c => ({ label: c.profile.name || c.profile.id, checked: (r.channels || []).includes(c.profile.id), fn: () => { const cs = r.channels || []; setCh(cs.includes(c.profile.id) ? cs.filter(y => y !== c.profile.id) : cs.concat([c.profile.id])); } }))] },
    moveSub(OV.rules, id, saveRules), { sep: true }, { label: "Delete rule", danger: true, fn: () => pp.ovRuleDel(id) }];
});
registerCtx("ovRules", () => listMenu("Rules", OV.rules, saveRules, "rule", mkRule).concat([{ label: "New rule…", fn: () => pp.ovRuleNew() }]));
registerCtx("ovCue", ({ id }) => {
  const [xi, qi] = id.split(":").map(Number), x = OV.cues.items[xi], q = x && x.cues[qi]; if (!q) return [];
  const spec = cueSpec(q.c, x.it, x.m), known = !!tplOf(spec.template);
  return [{ header: true, label: "Cue · " + (spec.template || "?") }, { label: "Test-fire on " + chName(cur()), disabled: !known, fn: () => pp.ovCueFire(xi, qi) }, chTargets("Test-fire on", chs => deployTo(chs, spec)), { sep: true },
    { label: "Copy", sub: [{ label: "Copy cue (paste elsewhere)", fn: () => setClipK("overlay", spec, spec.template) }, { label: "Copy cue JSON", fn: () => copyJson(q.c, "cue") }, { label: "Copy item title", fn: () => copyText(x.it.title || "") }] },
    { label: "Load into the deploy form", disabled: !known, fn: () => { loadSpec(spec); OV.tab = "deploy"; OV.editing = null; pp.render(); } }, { label: "Save as preset…", disabled: !known, fn: () => addPresetFrom(spec, tplName(spec.template)) }, { sep: true },
    { label: "Open media in Library", disabled: !x.it.mediaId, fn: () => { pp.nav("library"); setTimeout(() => pp.libOpen && pp.libOpen(x.it.mediaId), 300); } }, { label: "Open in Schedule", fn: () => pp.nav("schedule") },
    { label: "Delete cue", disabled: true, title: "Cues belong to the schedule item or the media record: edit them there", fn: () => {} }];
});
registerCtx("ovCues", () => [{ header: true, label: "Cues" }, { label: "Reload", fn: () => pp.ovCuesReload() }, { label: "Copy all cues (JSON)", disabled: !OV.cues.items.length, fn: () => copyJson(OV.cues.items.map(x => ({ item: x.it.title, cues: x.cues.map(q => q.c) })), "cues") }]);
registerCtx("ovRenderer", () => [{ header: true, label: "Renderer" }, { label: "Test on " + chName(cur()), fn: () => pp.ovRTest() }, { label: "Re-scan", fn: () => pp.ovRefresh() }, { label: "Copy browser path", disabled: !(OV.renderer || {}).path, fn: () => copyText(OV.renderer.path) }, { label: "Copy status (JSON)", fn: () => copyJson(OV.renderer, "renderer") }]);
