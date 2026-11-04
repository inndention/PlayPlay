/* PLAY PLAY web dashboard — no build step. Talks to the Supervisor's REST (/api/v1) and
   WebSocket (:7701) surfaces, the same ones the native operator app uses. */

import { viewInputs, viewMesh, viewCgHtml, onEnterIntegration, onWsIntegration, ppExtIntegrations } from "./integrations.js";
import { viewLibrary, onEnterLibrary, onWsLibrary, ppExtLibrary } from "./library.js";
import { viewMux, viewStreams, onEnterMux, onWsMux, ppExtMux } from "./mux.js";
import "./ctx.js";
import "./tabsbar.js";
import { viewOverlays, onEnterOverlays, onWsOverlays, ppExtOverlays } from "./overlays.js";
import { viewSuites, onEnterSuites, onWsSuites, ppExtSuites } from "./suites.js";
import { viewSpots, onEnterSpots, onWsSpots, ppExtSpots } from "./spots.js";
import { viewFlight, onEnterFlight, onWsFlight, ppExtFlight } from "./flight.js";
import { NP, viewNowPlaying, onEnterNowPlaying, onWsNowPlaying, ppExtNowPlaying } from "./nowplaying.js";
import { viewReporter, onEnterReporter, onWsReporter, ppExtReporter } from "./reporter.js";
import { viewReportsExt, onEnterReports, onWsReports, ppExtReports } from "./reports.js";
import { viewRemote, onEnterRemote, onWsRemote, ppExtRemote } from "./remote.js";
import { viewProgramming, onEnterProgramming, onWsProgramming, ppExtProgramming } from "./programming.js";
import { viewAutomationChannels, ppExtAutomation } from "./automation.js";

const $ = (s, el = document) => el.querySelector(s);
const esc = s => String(s ?? "").replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
// A value as a JS string literal for an inline handler attribute: onclick="pp.x(${jsq(id)})". esc() alone is not enough inside '...' because
// the attribute is entity-decoded before the script is parsed, so &#39; becomes a real quote again and an id like a');evil();// runs.
const jsq = v => esc(JSON.stringify(String(v ?? "")));
const pad2 = n => (n < 10 ? "0" : "") + n;
const fmtClock = s => { s = Math.max(0, Math.floor(s || 0)); const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), x = s % 60; return (h ? h + ":" + pad2(m) : m) + ":" + pad2(x); };
const fmtTime = (ms, sec) => ms ? new Date(ms).toLocaleTimeString([], sec ? { hour: "numeric", minute: "2-digit", second: "2-digit" } : { hour: "numeric", minute: "2-digit" }) : "—";
const iso = d => d.getFullYear() + "-" + pad2(d.getMonth() + 1) + "-" + pad2(d.getDate());
const stateColor = s => ({ ON_AIR: "var(--onair)", PREROLL: "var(--cued)", PAUSED: "var(--warn)", HOLD: "var(--standby)", IDLE: "var(--standby)", ERROR: "var(--danger)", OFFLINE: "var(--danger)" }[s] || "var(--t2)");
const typeColor = t => ({ ad: "var(--ad)", commercial: "var(--ad)", promo: "var(--accent2)", station_id: "var(--gfx)", bumper: "var(--bumper)", live: "var(--live)", cg: "var(--gfx)", image: "var(--gfx)", filler: "var(--preroll)", psa: "var(--preroll)", gap: "var(--t3)" }[String(t || "").toLowerCase()] || "var(--video)");
const badge = (text, color, solid) => `<span class="badge${solid ? " solid" : ""}" style="${solid ? "background:" + color : "color:" + color + ";border-color:" + color + ";background:color-mix(in srgb," + color + " 16%,transparent)"}">${esc(text)}</span>`;

// ---------------------------------------------------------------------------
// API + token
// ---------------------------------------------------------------------------
const S = {
  token: localStorage.getItem("pp.token") || "",
  view: (location.hash.replace("#", "") || "nowplaying").replace(/^ingest$/, "library"),
  channels: [], telemetry: {}, runlists: {}, alerts: [], asrun: [], snapshot: {}, media: [],
  active: localStorage.getItem("pp.channel") || "", wsUp: false,
  uploads: [], uploadInfo: {}, ingestType: "program", mediaQuery: "",
  week: {}, weekStart: null, selSlot: null, editItems: null, editFlags: {}, editDirty: false, rulesOpen: false, liveKind: "ndi", libQuery: "", reportDay: new Date(), reportRows: [], reportSummary: {}, cgTitle: "", cgSubtitle: "", cgTicker: "", propIndex: -1, blockOpen: false, block: { kind: "query", types: "program", minutes: 10, prefer: "least_played", categories: "", subcategories: "", series: "", folder: "", order: "random", nthLatest: 0, noRepeatMin: 30, newer: "", older: "", ageField: "added", rulesText: "", match: "all" }, resolved: null,
};
async function api(method, path, body) {
  const r = await fetch("/api/v1" + path, { method, headers: { "Content-Type": "application/json", ...(S.token ? { Authorization: "Bearer " + S.token } : {}) }, body: body !== undefined ? JSON.stringify(body) : undefined });
  if (r.status === 401) { askToken(); throw new Error("unauthorized"); }
  const t = await r.text();
  let d; try { d = JSON.parse(t); } catch { d = t; }
  if (!r.ok) { toast((d && d.error) || (method + " " + path + " → " + r.status), "err"); throw new Error(d && d.error || r.status); }
  return d;
}
const GET = p => api("GET", p), POST = (p, b) => api("POST", p, b || {}), PUT = (p, b) => api("PUT", p, b || {});
const tokenQ = () => S.token ? "?token=" + encodeURIComponent(S.token) : "";
function askToken() { const d = $("#tokenDlg"); $("#tokenInput").value = S.token; d.showModal(); }
$("#tokenDlg form").addEventListener("submit", e => { if (e.submitter && e.submitter.value === "ok") { S.token = $("#tokenInput").value.trim(); localStorage.setItem("pp.token", S.token); location.reload(); } });
$("#tokenBtn").onclick = askToken;
// Text size: CSS zoom on the document, persisted per browser.
const ZOOMS = [0.85, 1, 1.15, 1.3, 1.5, 1.75, 2, 2.5, 3];
function applyZoom(z) { z = Math.max(0.7, Math.min(2.5, z)); document.documentElement.style.zoom = z; try { localStorage.setItem("pp.zoom", String(z)); } catch {} const p = $("#zoomPct"); if (p) p.textContent = Math.round(z * 100) + "%"; }
function stepZoom(d) { const cur = parseFloat(document.documentElement.style.zoom || "1") || 1; let i = 0, best = 1e9; ZOOMS.forEach((z, k) => { const dd = Math.abs(z - cur); if (dd < best) { best = dd; i = k; } }); applyZoom(ZOOMS[Math.max(0, Math.min(ZOOMS.length - 1, i + d))]); }
try { applyZoom(parseFloat(localStorage.getItem("pp.zoom") || "1.3") || 1.3); } catch { applyZoom(1); }
$("#zoomIn").onclick = () => stepZoom(1); $("#zoomOut").onclick = () => stepZoom(-1); $("#zoomPct").onclick = () => applyZoom(1);
document.addEventListener("keydown", e => { if ((e.ctrlKey || e.metaKey) && !e.shiftKey && (e.key === "=" || e.key === "+")) { e.preventDefault(); stepZoom(1); } else if ((e.ctrlKey || e.metaKey) && e.key === "-") { e.preventDefault(); stepZoom(-1); } else if ((e.ctrlKey || e.metaKey) && e.key === "0") { e.preventDefault(); applyZoom(1); } });
// Clipboard for the context menus: items, an hour or a whole day.
S.clip = null; const setClip = (kind, data, label) => { S.clip = { kind, data: JSON.parse(JSON.stringify(data)), label: label || kind }; toast("Copied " + (label || kind), "ok"); };
const freshIds = items => items.map(it => { const c = JSON.parse(JSON.stringify(it)); if (!c.isLocked) c.id = "p" + Date.now().toString(36) + Math.floor(Math.random() * 10000); return c; });
document.addEventListener("keydown", e => {
  const t = e.target; if (t && (t.tagName === "INPUT" || t.tagName === "TEXTAREA" || t.tagName === "SELECT" || t.tagName === "BUTTON" || t.tagName === "A" || t.isContentEditable)) return;
  if (document.querySelector("dialog[open]")) return;                      // a confirm or token dialog owns the keys
  if (S.view !== "schedule" || !S.selSlot || !S.editItems) return;         // no open hour (or still loading): nothing to edit
  const mod = e.ctrlKey || e.metaKey, key = e.key.toLowerCase();
  if (mod && key === "c" && String(window.getSelection() || "").length) return;   // copying page text, not a row
  const sel = S.propIndex; const items = S.editItems;
  if (mod && key === "c" && sel >= 0 && items[sel]) { setClip("items", [items[sel]], items[sel].title); e.preventDefault(); }
  else if (mod && key === "x" && sel >= 0 && items[sel]) { setClip("items", [items[sel]], items[sel].title); items.splice(sel, 1); S.propIndex = -1; S.editDirty = true; render(); e.preventDefault(); }
  else if (mod && key === "v" && S.clip && S.clip.kind === "items") { const at = sel >= 0 ? sel + 1 : items.length; items.splice(at, 0, ...freshIds(S.clip.data)); S.editDirty = true; render(); e.preventDefault(); }
  else if (e.key === "Delete" && sel >= 0 && items[sel]) { items.splice(sel, 1); S.propIndex = -1; S.editDirty = true; render(); }
});

let toastTimer; function toast(msg, cls) { const t = $("#toast"); t.textContent = msg; t.className = "show " + (cls || ""); clearTimeout(toastTimer); toastTimer = setTimeout(() => t.className = "", 3500); }
function confirmDo(text, fn) { const d = $("#confirm"); $("#confirmText").textContent = text; d.returnValue = ""; d.showModal(); d.onclose = () => { if (d.returnValue === "ok") fn(); }; }

// ---------------------------------------------------------------------------
// Live state (WebSocket with REST fallback)
// ---------------------------------------------------------------------------
const chan = id => S.channels.find(c => c.profile && c.profile.id === id) || {};
const tele = id => S.telemetry[id] || {};
function ensureActive() { if (!chan(S.active).profile && S.channels.length) S.active = S.channels[0].profile.id; }
async function loadSnapshot() {
  const st = await GET("/status").catch(() => null); if (!st) return;
  const ch = await GET("/channels"); S.channels = ch.channels || []; S.snapshot = st; ensureActive();
  for (const c of S.channels) { if (c.engine && c.engine.state) S.telemetry[c.profile.id] = c.engine; GET("/channels/" + c.profile.id + "/runlist").then(r => { S.runlists[c.profile.id] = r; render(); }).catch(() => {}); }
  render();
}
let ws, wsTimer;
function connectWs() {
  const port = S.snapshot.wsPort || 7701;
  const url = (location.protocol === "https:" ? "wss://" : "ws://") + location.hostname + ":" + port + "/" + tokenQ();
  try { ws = new WebSocket(url); } catch { scheduleWs(); return; }
  ws.onopen = () => { S.wsUp = true; setConn(true); ws.send(JSON.stringify({ type: "subscribe", rate: "low" })); };
  ws.onclose = () => { S.wsUp = false; setConn(false); scheduleWs(); };
  ws.onmessage = ev => { let m; try { m = JSON.parse(ev.data); } catch { return; } onWs(m); };
}
function scheduleWs() { clearTimeout(wsTimer); wsTimer = setTimeout(connectWs, 2500); }
function setConn(up) { $("#conn").className = "dot" + (up ? "" : " off"); $("#connText").textContent = up ? "live" : "reconnecting"; }
function onWs(m) {
  const id = m.channelId || m._channelId;
  switch (m.type) {
    case "snapshot": S.snapshot = m; S.channels = m.channels || S.channels; ensureActive(); break;
    case "telemetry": if (id && m.frame !== undefined) S.telemetry[id] = m; else if (id && m.state && m.reason) { (S.telemetry[id] = S.telemetry[id] || {}).state = m.state; } break;
    case "runlist": if (id) S.runlists[id] = m; break;
    case "asrun": S.asrun.unshift(m); if (S.asrun.length > 300) S.asrun.pop(); if (S.view === "reports") loadReport(); break;
    case "alert": S.alerts.unshift(m); if (S.alerts.length > 300) S.alerts.pop(); if (m.level === "error" || m.level === "critical") toast((id || "") + ": " + m.message, "err"); break;
    case "schedule": if (S.view === "schedule") loadWeek(); break;
    case "mediaScanDone": loadMedia(); break;
    default: if (!onWsIntegration(m) && !onWsLibrary(m) && !onWsMux(m) && !onWsSpots(m) && !onWsReports(m) && !onWsRemote(m) && !onWsProgramming(m) && !onWsOverlays(m) && !onWsSuites(m) && !onWsFlight(m) && !onWsReporter(m) && !onWsNowPlaying(m)) return;
  }
  renderSoon();
}
let renderPending = false; function renderSoon() { if (renderPending) return; renderPending = true; requestAnimationFrame(() => { renderPending = false; render(); }); }
setInterval(() => { if (!S.wsUp) loadSnapshot(); else GET("/channels").then(ch => { S.channels = ch.channels || S.channels; renderSoon(); }).catch(() => {}); }, 5000);

// ---------------------------------------------------------------------------
// Clock
// ---------------------------------------------------------------------------
setInterval(() => {
  const n = new Date(); $("#clock").textContent = n.toLocaleTimeString();
  const toh = 3600 - (n.getMinutes() * 60 + n.getSeconds()); const el = $("#toh"); el.textContent = pad2(Math.floor(toh / 60)) + ":" + pad2(toh % 60);
  el.className = "mono toh" + (toh < 60 ? " crit" : toh < 300 ? " warn" : "");
}, 250);

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------
async function transport(id, action, params) { await POST("/channels/" + id + "/transport", { action, ...(params || {}) }); toast(action + " → " + id, "ok"); }
async function cg(id, cmd) { await POST("/channels/" + id + "/cg", cmd); }
window.pp = {
  get S() { return S; }, api, GET, POST, PUT, render, toast, confirmDo, cg,
  // The open hour belongs to one channel: switching drops it (after a confirm when it holds unsaved edits) and reloads what the view shows.
  setActive(id, then) {
    if (id === S.active) { if (then) then(); else render(); return; }
    leaveHour(() => { S.active = id; localStorage.setItem("pp.channel", id); S.selSlot = null; S.week = {};
      if (S.view === "schedule") loadWeek(); else if (S.view === "reports") loadReport();
      render(); if (then) then(); });
  },
  play(id) { transport(id, "play"); }, pause(id) { transport(id, "pause"); },
  skip(id) { confirmDo("Skip to the next item on " + id + "?", () => transport(id, "skip")); },
  stop(id) { confirmDo("Stop " + id + " and go to slate?", () => transport(id, "stop")); },
  hold(id) { confirmDo("Hold " + id + " on slate?", () => transport(id, "hold")); },
  loadHour(id) { POST("/channels/" + id + "/load-hour").then(() => toast("Hour pushed", "ok")); },
  snap(id) { POST("/channels/" + id + "/snap").then(() => toast("Snapped to schedule", "ok")); },
  arm(id, on) { POST("/channels/" + id + (on ? "/arm" : "/disarm")).then(() => { toast(on ? "Armed" : "Disarmed", "ok"); loadSnapshot(); }); },
  restart(id) { confirmDo("Restart the engine process for " + id + "?", () => POST("/channels/" + id + "/restart").then(() => toast("Engine restarting", "ok"))); },
  cgShow(kind) {
    const id = S.active, t = $("#cgTitle") ? $("#cgTitle").value : "", st = $("#cgSub") ? $("#cgSub").value : "", tk = $("#cgTicker") ? $("#cgTicker").value : "";
    const cmds = {
      bug: { action: "show", role: "bug", source: { type: "template", kind: "bug", text: t || chan(id).profile.callLetters || chan(id).profile.name }, placement: { anchor: "top_right", margin: 60 }, in: { type: "fade", durationSec: 0.5 } },
      lower_third: { action: "show", role: "lower_third", source: { type: "template", kind: "lower_third", title: t || "Lower third", subtitle: st }, placement: { anchor: "bottom_left", marginX: 120, marginY: 90 }, in: { type: "slide_left", durationSec: 0.5 }, out: { type: "fade", durationSec: 0.4 }, holdSec: 0 },
      ticker: { action: "show", role: "ticker", source: { type: "template", kind: "ticker", text: tk || t || "PLAY PLAY" }, placement: { anchor: "bottom" }, in: { type: "wipe_right", durationSec: 0.5 } },
      clock: { action: "show", role: "legal_id", source: { type: "template", kind: "clock", format: "h:mm AP" }, placement: { anchor: "top_left", margin: 60 }, in: { type: "fade", durationSec: 0.3 } },
      emergency: { action: "show", role: "emergency", source: { type: "bars", captions: ["TECHNICAL DIFFICULTIES", "PLEASE STAND BY"] }, in: { type: "cut" } },
    };
    cg(id, cmds[kind]).then(() => toast("CG " + kind + " shown", "ok"));
  },
  cgHide(role) { cg(S.active, { action: "hide", role, transition: { type: "fade", durationSec: 0.4 } }); },
  cgLayer(layer, state) { cg(S.active, { action: state === "hidden" ? "show" : "hide", layer, transition: { type: "fade", durationSec: 0.3 } }); },
  cgRemove(layer) { cg(S.active, { action: "remove", layer }); },
  cgClear() { confirmDo("Clear every CG layer on " + S.active + "?", () => cg(S.active, { action: "clear" })); },
  programSlate() { confirmDo("Mix the program to slate on " + S.active + "?", () => cg(S.active, { action: "program_clear", transition: { type: "mix", durationSec: 1 } })); },
  // --- schedule editing (web) ---
  editLoad() { const cs = currentSlot(); S.editItems = JSON.parse(JSON.stringify(cs.slot && cs.slot.items ? cs.slot.items : [])); const f = cs.slot || {}; S.editFlags = { isLooping: !!f.isLooping, randomizeAds: f.randomizeAds !== false, keepProgramOrder: f.keepProgramOrder !== false, blockType: f.blockType || "general" }; S.editDirty = false; S.propIndex = -1; S.propOpen = false; render(); },
  editAddMedia(id, at) { if (!S.selSlot) { toast("Pick an hour first"); return; } if (!S.editItems) pp.editLoad(); const m = mediaById(id); if (!m) return;
    const it = { id: "w" + Date.now().toString(36), mediaId: m.id, title: m.title, path: m.path, type: m.type || "video", duration: m.duration || 0, timeIn: 0, timeOut: 0, timingMode: "soft_start", transitionType: "cut", priority: 0 };
    if (m.type === "live" && m.liveSource) { it.liveSource = m.liveSource; it.timingMode = "fill_until"; it.duration = m.duration || 1800; it.path = ""; }
    S.editItems.splice(at === undefined ? S.editItems.length : at, 0, it); S.editDirty = true; render(); addedNote("\"" + it.title + "\"");
    if (it.type !== "live") setTimeout(() => pp.overrunCheck(), 0); },
  editAddLive() { if (!S.selSlot) { toast("Pick an hour first"); return; } if (!S.editItems) pp.editLoad(); const kind = $("#lvKind").value, ref = $("#lvRef").value.trim(), title = $("#lvTitle").value.trim(), secs = parseFloat($("#lvDur").value) || 0; if (!ref) return;
    const src = { type: kind }; if (kind === "ndi") src.name = ref; else if (kind === "sdi_in") src.port = parseInt(ref) || 1; else { src.url = ref; src.realtime = true; }
    S.editItems.push({ id: "w" + Date.now().toString(36), mediaId: "", title: title || (kind.toUpperCase() + " " + ref), path: "", type: "live", liveSource: src, duration: secs > 0 ? secs : 1800, timeIn: 0, timeOut: 0, timingMode: secs > 0 ? "soft_start" : "fill_until", transitionType: "mix", transitionSec: 0.5, priority: 0 });
    S.editDirty = true; render(); },
  editSet(i, k, v) { if (!S.editItems || !S.editItems[i]) return; if (k === "lockAtSec") { const n = parseFloat(v); if (isNaN(n)) delete S.editItems[i].lockAtSec; else { S.editItems[i].lockAtSec = Math.max(0, Math.min(3600, n)); S.editItems[i].isLocked = true; } } else if (k === "fixedOffsetSec") { const n = parseFloat(v); S.editItems[i].fixedOffsetSec = isNaN(n) ? -1 : n; if (!isNaN(n)) S.editItems[i].timingMode = "hard_start"; } else S.editItems[i][k] = (k === "timeIn" || k === "timeOut" || k === "duration" || k === "priority" || k === "transitionSec" || k === "maxLateSec" || k === "minDurationSec") ? (parseFloat(v) || 0) : v; S.editDirty = true; },
  editMove(i, d) { if (!S.editItems) return; const j = i + d; if (j < 0 || j >= S.editItems.length) return; const t = S.editItems[i]; S.editItems[i] = S.editItems[j]; S.editItems[j] = t; S.editDirty = true; render(); },
  mediaDrag(ev, id) { ev.dataTransfer.setData("application/x-playplay-media", id); ev.dataTransfer.effectAllowed = "copy"; },
  slotDrag(ev, i) { ev.dataTransfer.setData("application/x-playplay-slot", String(i)); ev.dataTransfer.effectAllowed = "move"; },
  slotOver(ev) { if (![...ev.dataTransfer.types].some(t => t === "application/x-playplay-slot" || t === "application/x-playplay-media")) return; ev.preventDefault(); const r = ev.currentTarget.getBoundingClientRect(); ev.currentTarget.style.boxShadow = ev.clientY < r.top + r.height / 2 ? "inset 0 3px var(--accent)" : "inset 0 -3px var(--accent)"; },
  slotLeave(ev) { ev.currentTarget.style.boxShadow = ""; },
  slotDrop(ev, i) { ev.preventDefault(); ev.stopPropagation(); const r = ev.currentTarget.getBoundingClientRect(); ev.currentTarget.style.boxShadow = ""; let at = i + (ev.clientY < r.top + r.height / 2 ? 0 : 1); const id = ev.dataTransfer.getData("application/x-playplay-media"), from = ev.dataTransfer.getData("application/x-playplay-slot"); if (id) { pp.editAddMedia(id, at); return; } if (from !== "" && S.editItems) { const n = Number(from); if (!Number.isInteger(n) || n < 0 || n >= S.editItems.length) return; const item = S.editItems.splice(n, 1)[0]; if (n < at) --at; S.editItems.splice(at, 0, item); S.editDirty = true; S.propIndex = at; render(); } },
  editRemove(i) { if (!S.editItems) return; S.editItems.splice(i, 1); S.editDirty = true; render(); },
  editFlag(k, v) { S.editFlags[k] = v; S.editDirty = true; },
  editSave(dated, skipOverrun) { const cs = currentSlot(); if (!cs.date) return; if (!S.editItems) { toast("The hour is still loading, try again", "err"); return; }    // never save an unloaded editor as an empty hour
    if (!skipOverrun && pp.overrunCheck(dated, true)) return;     // a programme past the top of the hour: the dialog decides first
    const id = dated ? iso(cs.date) + "-" + S.selSlot.hour : cs.date.getDay() + "-" + S.selSlot.hour;
    const body = Object.assign({}, cs.slot || {}, S.editFlags, { channelId: S.active, hour: S.selSlot.hour, items: cleanItems(S.editItems) }); if (dated) { body.date = iso(cs.date); body.dayOfWeek = -1; } else { body.dayOfWeek = cs.date.getDay(); delete body.date; }
    PUT("/schedule/" + S.active + "/slot/" + id, body).then(() => { toast("Saved " + id, "ok"); S.editDirty = false; loadWeek(); }); },
  overrunCheck(dated, fromSave) {
    if (!S.selSlot || !S.editItems) return false;
    const o = findOverrun(S.editItems); if (!o) return false;
    overrunDialog(o, dated === undefined ? !!(currentSlot().slot && currentSlot().slot.date) : dated, !!fromSave);
    return true;
  },
  async overrunApply(o, dated, mode, next, recipeId, breakMin, breakEveryMin) {
    const cs = currentSlot(); if (!cs.date) return;
    const id = dated ? iso(cs.date) + "-" + S.selSlot.hour : cs.date.getDay() + "-" + S.selSlot.hour;
    const slot = Object.assign({}, cs.slot || {}, S.editFlags, { channelId: S.active, hour: S.selSlot.hour, items: cleanItems(S.editItems) });
    if (dated) { slot.date = iso(cs.date); slot.dayOfWeek = -1; } else { slot.dayOfWeek = cs.date.getDay(); delete slot.date; }
    const body = { slot, itemId: o.item.id, mode, next, breakMin, breakEveryMin }; if (next === "recipe") body.recipeId = recipeId;
    try {
      const r = await POST("/schedule/" + S.active + "/slot/" + id + "/overrun", body);
      S.editItems = (r.slot && r.slot.items) || S.editItems; S.editDirty = false; render(); loadWeek();
      toast(((r.notes || [])[0] || "Applied") + ". Saved this hour and " + r.nextSlotId, "ok");
    } catch (e) { toast("Could not apply: " + (e && e.message ? e.message : e), "err"); }
  },
  editDelete(slotId) { const id = typeof slotId === "string" ? slotId : currentSlot().id; if (!id) return; const c = cellOf(id);
    hourOp(s => s.day === c.day && s.hour === c.hour, () => api("DELETE", "/schedule/" + S.active + "/slot/" + id), "Delete hour " + id + "?"); },
  rule(k, v) { const c = chan(S.active); const f = Object.assign({}, (c.profile && c.profile.fill) || {}); f[k] = v; PUT("/channels/" + S.active, { fill: f }).then(() => { toast("Rule saved", "ok"); loadSnapshot(); }); },
  ruleProf(k, v) { const o = {}; o[k] = v; PUT("/channels/" + S.active, o).then(() => { toast("Rule saved", "ok"); loadSnapshot(); }); },
  // The open hour is a date in one week: moving to another week reloads the editor for the same grid cell (after a confirm when it is dirty).
  weekShift(n) { leaveHour(() => { const d = new Date(S.weekStart); d.setDate(d.getDate() + 7 * n); S.weekStart = d; weekAndHour(); }); },
  weekToday() { leaveHour(() => { S.weekStart = startOfWeek(new Date()); weekAndHour(); }); },
  editToday(id) {          // channel menu: today's hour on that channel, loaded into the editor
    const go = () => { const n = new Date(); S.weekStart = startOfWeek(n); S.selSlot = { day: n.getDay(), hour: n.getHours() }; S.editItems = null; pp.nav("schedule"); weekAndHour(); };
    if (id === S.active) leaveHour(go); else pp.setActive(id, go);
  },
  editRundownHour(id, hourStartMs) {
    const date = new Date(Number(hourStartMs));
    if (!Number.isFinite(date.getTime())) { toast("This rundown item has no schedule hour", "err"); return; }
    const go = () => { S.weekStart = startOfWeek(date); S.selSlot = { day: date.getDay(), hour: date.getHours() }; pp.nav("schedule"); weekAndHour(); };
    if (id === S.active) leaveHour(go); else pp.setActive(id, go);
  },
  selSlot(day, hour) { S.selSlot = { day, hour }; S.editItems = null; pp.editLoad(); },
  validateSlot() { const s = currentSlot(); if (!s.id) return; POST("/schedule/" + S.active + "/slot/" + s.id + "/validate", s.slot || {}).then(r => toast(r.issues && r.issues.length ? r.issues.length + " issue(s): " + r.issues.map(i => i.code).join(", ") : "Hour validates clean", r.issues && r.issues.length ? "err" : "ok")); },
  reportShift(n) { S.reportDay = new Date(S.reportDay.getTime() + n * 86400000); loadReport(); },
  reportCsv() {
    const rows = S.reportRows; let s = "Planned,Actual,Variance s,Event,Status,Title,Type,Origin,Duration s\n";
    for (const r of rows) s += [r.plannedStart || "", r.actualStart || "", r.eventType === "START" ? (r.varianceMs / 1000).toFixed(2) : "", r.eventType, r.status, '"' + String(r.title || "").replace(/"/g, '""') + '"', r.type, r.origin, r.actualDurationMs ? (r.actualDurationMs / 1000).toFixed(1) : ""].join(",") + "\n";
    const a = document.createElement("a"); a.href = URL.createObjectURL(new Blob([s], { type: "text/csv" })); a.download = iso(S.reportDay) + "_" + S.active + "_AsPlayed.csv"; a.click();
  },
  pickFiles() { const i = document.createElement("input"); i.type = "file"; i.multiple = true; i.onchange = () => pp.uploadFiles(i.files); i.click(); },
  uploadFiles(files) { if (pp.upHold && pp.upHold()) pp.upStageAdd([...files]); else for (const f of files) uploadOne(f); },
  dropFiles(ev) { ev.preventDefault(); ev.stopPropagation(); const d = $("#drop"); if (d) d.classList.remove("over"); collectDropped(ev.dataTransfer).then(files => pp.uploadFiles(files.filter(f => isMediaName(f.name)))); },
  scanFolder() { const f = $("#scanFolder").value.trim(); if (!f) return; POST("/media/scan", { folder: f, recursive: $("#scanRec").checked, type: S.ingestType }).then(r => toast("Queued " + r.queued + " file(s), " + r.pending + " pending", "ok")); },
  setMediaType(id, type) { PUT("/media/" + id, { type }).then(() => { toast("Type set", "ok"); mediaChanged(); }); },
  archiveMedia(id, on) { PUT("/media/" + id, { archived: on }).then(mediaChanged); },
  deleteMedia(id, title) { confirmDo("Remove \"" + title + "\" from the library? The file stays on disk.", () => api("DELETE", "/media/" + id).then(mediaChanged)); },
  downloadUrl(id) { return "/api/v1/download/" + id + tokenQ(); },
  // --- context menus & programming helpers -------------------------------------------------------------
  ctx: ctxMenu,
  cellMenu(ev, d, h) { const cs = slotAt(d, h); const id = cs.id; const tplId = d + "-" + h; const datedId = cs.date ? iso(cs.date) + "-" + h : tplId;
    // Every server-side write goes through hourOp: it asks before replacing unsaved edits of the open hour and reloads the editor
    // afterwards (a stale or empty editor would revert or wipe the result on the next Save). `touches` says which open hour a write can reach.
    const cell = s => s.day === d && s.hour === h, wholeDay = s => s.day === d, thisHour = s => s.hour === h;
    if (!S.editDirty && !(S.selSlot && cell(S.selSlot))) pp.selSlot(d, h);     // a clean editor follows the right-click like a left-click; a dirty one is left alone
    const rl = recipeList(); loadRecipes();                                       // the menu uses what is cached; the refresh serves the next one
    const fill = (recipeId, dated) => hourOp(cell, () => POST("/schedule/" + S.active + "/hour-recipe", { recipeId, scope: "hour", slotId: dated ? datedId : tplId, keepLocks: true, apply: true }).then(() => toast("Recipe laid", "ok")));
    const hourBody = (hh, extra) => Object.assign({ channelId: S.active, hour: h, items: freshIds(hh.items || []), isLooping: !!hh.isLooping, randomizeAds: hh.randomizeAds !== false, keepProgramOrder: hh.keepProgramOrder !== false, blockType: hh.blockType || "general" }, extra);
    ctxMenu(ev, [{ label: "Edit hour", fn: () => openHour(d, h) }, { sep: true },
      { label: "Fill with recipe (template)", sub: rl.map(r => ({ label: r.name, fn: () => fill(r.id, false) })) }, { label: "Fill with recipe (this date only)", sub: rl.map(r => ({ label: r.name, fn: () => fill(r.id, true) })) },
      { label: "Weighted fill (format clock)", fn: () => hourOp(cell, () => POST("/schedule/" + S.active + "/weighted-fill", { scope: "hour", slotId: tplId, keepLocks: true, apply: true }).then(r => toast(((r.summary && r.summary.placed) || 0) + " items placed", "ok"))) },
      { label: "Shuffle ads", fn: () => hourOp(cell, () => POST("/schedule/" + S.active + "/slot/" + id + "/shuffle", {}).then(() => toast("Shuffled", "ok"))), disabled: !(cs.slot && cs.slot.items && cs.slot.items.length) },
      { label: "Resolve now…", fn: () => openHour(d, h, () => pp.previewResolved()) }, { label: "Validate", fn: () => openHour(d, h, () => pp.validateSlot()) }, { sep: true },
      { label: "Copy hour", fn: () => { const sl = cs.slot || { items: [] }; setClip("hour", { items: sl.items || [], isLooping: !!sl.isLooping, randomizeAds: sl.randomizeAds !== false, keepProgramOrder: sl.keepProgramOrder !== false, blockType: sl.blockType || "general" }, "hour " + pad2(h) + ":00"); } },
      { label: "Paste hour (template)", disabled: !(S.clip && S.clip.kind === "hour"), fn: () => { const hh = S.clip.data; hourOp(cell, () => PUT("/schedule/" + S.active + "/slot/" + tplId, hourBody(hh, { dayOfWeek: d })).then(() => toast("Pasted into " + tplId, "ok"))); } },
      { label: "Paste hour (this date only)", disabled: !(S.clip && S.clip.kind === "hour"), fn: () => { const hh = S.clip.data; hourOp(cell, () => PUT("/schedule/" + S.active + "/slot/" + datedId, hourBody(hh, { date: iso(cs.date), dayOfWeek: -1 })).then(() => toast("Pasted into " + datedId, "ok"))); } },
      { label: "Copy day", fn: () => { const hours = {}; for (let hh = 0; hh < 24; hh++) { const dt2 = new Date(S.weekStart); dt2.setDate(dt2.getDate() + d); const dated = S.week[iso(dt2) + "-" + hh], tpl = S.week[d + "-" + hh]; const sl = (dated && dated.items && dated.items.length) ? dated : tpl; if (sl && sl.items && sl.items.length) hours[hh] = sl; } setClip("day", hours, ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"][d] + " (" + Object.keys(hours).length + " hours)"); } },
      { label: "Paste day (template)", disabled: !(S.clip && S.clip.kind === "day"), fn: () => hourOp(wholeDay, () => Promise.all(Object.entries(S.clip.data).map(([hh, sl]) => PUT("/schedule/" + S.active + "/slot/" + d + "-" + hh, Object.assign({}, sl, { id: undefined, date: undefined, channelId: S.active, hour: +hh, dayOfWeek: d, items: freshIds(sl.items || []) })))).then(r => toast("Pasted " + r.length + " hour(s)", "ok"))) },
      { label: "Paste day (dated)", disabled: !(S.clip && S.clip.kind === "day"), fn: () => hourOp(wholeDay, () => Promise.all(Object.entries(S.clip.data).map(([hh, sl]) => PUT("/schedule/" + S.active + "/slot/" + iso(cs.date) + "-" + hh, Object.assign({}, sl, { id: undefined, channelId: S.active, hour: +hh, date: iso(cs.date), dayOfWeek: -1, items: freshIds(sl.items || []) })))).then(r => toast("Pasted " + r.length + " hour(s)", "ok"))) }, { sep: true },
      { label: "Copy hour to all days (template)", fn: () => hourOp(thisHour, () => Promise.all([0, 1, 2, 3, 4, 5, 6].filter(x => x !== d).map(x => POST("/schedule/" + S.active + "/copy", { from: { scope: "hour", slotId: tplId }, to: { dow: x } }))).then(() => toast("Copied", "ok")), "Copy " + tplId + " to every day of the week?") },
      { label: "Copy hour to every hour of this day", fn: () => hourOp(wholeDay, () => Promise.all([...Array(24).keys()].filter(x => x !== h).map(x => POST("/schedule/" + S.active + "/copy", { from: { scope: "hour", slotId: tplId }, to: { dow: d, hour: x } }))).then(() => toast("Copied", "ok")), "Copy " + tplId + " to every hour of this day?") },
      { label: "Copy day to next week (dated)", fn: () => { const nd = new Date(cs.date); nd.setDate(nd.getDate() + 7); POST("/schedule/" + S.active + "/copy", { from: { scope: "day", dow: d }, to: { date: iso(nd), dated: true } }).then(r => toast(r.written + " hours written", "ok")); } }, { sep: true },
      { label: "Clear hour (keep locks)", fn: () => hourOp(cell, () => POST("/schedule/" + S.active + "/clear", { scope: "hour", slotId: id, keepLocks: true }).then(() => toast("Hour cleared", "ok"))) },
      { label: "Delete hour", danger: true, fn: () => pp.editDelete(id) }, { sep: true }, { label: "Properties (hour flags)", fn: () => openHour(d, h, () => window.scrollTo(0, document.body.scrollHeight)) }]); },
  itemMenu(ev, i) { const it = S.editItems && S.editItems[i]; if (!it) return;
    const setLock = (locked, at) => { it.isLocked = locked; if (at !== undefined) it.lockAtSec = at; if (!locked) { delete it.lockAtSec; delete it.lockLabel; } S.editDirty = true; render(); };   // an unlocked item carries no anchor: the server clamps a negative lockAtSec to 0 and locks the item at :00
    S.propIndex = i;
    ctxMenu(ev, [{ label: "Properties…", fn: () => { S.propIndex = i; S.propOpen = true; render(); } }, { sep: true },
      { label: "Cut", hint: "Ctrl+X", fn: () => { setClip("items", [it], it.title); S.editItems.splice(i, 1); S.editDirty = true; render(); } }, { label: "Copy", hint: "Ctrl+C", fn: () => setClip("items", [it], it.title) },
      { label: "Paste above", disabled: !(S.clip && S.clip.kind === "items"), fn: () => { S.editItems.splice(i, 0, ...freshIds(S.clip.data)); S.editDirty = true; render(); } }, { label: "Paste below", hint: "Ctrl+V", disabled: !(S.clip && S.clip.kind === "items"), fn: () => { S.editItems.splice(i + 1, 0, ...freshIds(S.clip.data)); S.editDirty = true; render(); } },
      { label: "Copy title", fn: () => copyText(it.title || "") }, { label: "Copy path", disabled: !it.path, fn: () => copyText(it.path) }, { sep: true },
      { label: "Move up", fn: () => pp.editMove(i, -1), disabled: i === 0 }, { label: "Move down", fn: () => pp.editMove(i, 1), disabled: i >= S.editItems.length - 1 }, { label: "Duplicate", fn: () => { const c = JSON.parse(JSON.stringify(it)); c.id = "w" + Date.now().toString(36); S.editItems.splice(i + 1, 0, c); S.editDirty = true; render(); } }, { sep: true },
      { label: it.isLocked ? "Unlock" : "Lock (keep through fills)", fn: () => setLock(!it.isLocked) },
      { label: "Lock at :mm…", hint: "soft anchor", fn: () => { const s = askMmss("Anchor offset into the hour (mm:ss)", it.lockAtSec >= 0 ? it.lockAtSec : 0); if (s !== null) setLock(true, Math.max(0, Math.min(3600, s))); } },
      { label: "Lock label…", fn: () => { const v = prompt("Lock label", it.lockLabel || ""); if (v !== null) { it.lockLabel = v; it.isLocked = true; S.editDirty = true; render(); } } }, { sep: true },
      { label: "Hard start at :mm:ss…", fn: () => { const s = askMmss("Hard start offset into the hour", it.fixedOffsetSec >= 0 ? it.fixedOffsetSec : 0); if (s !== null) { it.timingMode = "hard_start"; it.fixedOffsetSec = s; S.editDirty = true; render(); } } },
      { label: "Soft start", fn: () => { it.timingMode = "soft_start"; it.fixedOffsetSec = -1; S.editDirty = true; render(); } }, { label: "Fill until next", fn: () => { it.timingMode = "fill_until"; S.editDirty = true; render(); } }, { sep: true },
      isBlock(it) ? { label: "Edit block…", fn: () => pp.blockEdit(i) } : { label: "Open media", fn: () => pp.libOpen && pp.libOpen(it.mediaId), disabled: !it.mediaId },
      it.mediaId ? { label: "Asset report", fn: () => window.open(reportUrl("asset/" + it.mediaId + ".html")) } : null, it.mediaId ? { label: "Copy media id", fn: () => copyText(it.mediaId) } : null, { sep: true },
      { label: "Remove", danger: true, fn: () => pp.editRemove(i) }]); },
  mediaMenu(ev, id) { const m = mediaById(id); if (!m) { ev.preventDefault(); toast("That media record is not loaded yet, refresh the library", "err"); return; }
    ctxMenu(ev, [{ label: "Add to selected hour", fn: () => pp.editAddMedia(id), disabled: !S.selSlot }, { label: "Add folder block (this file's folder)", fn: () => { if (!S.selSlot) { toast("Pick an hour first"); return; } if (!S.editItems) pp.editLoad(); const folder = String(m.path || "").replace(/[\\/][^\\/]*$/, ""); S.editItems.push({ id: "q" + Date.now().toString(36), mediaId: "", title: "\uD83D\uDCC1 " + folder.split(/[\\/]/).pop(), type: m.type || "program", folderBlock: { folder, targetSec: 0, order: "random", weight: "freshness" }, duration: m.duration || 0, timingMode: "soft_start", transitionType: "cut" }); S.editDirty = true; render(); addedNote("folder block"); } },
      { label: "Take now on " + S.active, fn: () => confirmDo("Take \"" + m.title + "\" to air now on " + S.active + "?", () => POST("/channels/" + S.active + "/transport", { action: "take", path: m.path, title: m.title, duration: m.duration })) }, { sep: true },
      { label: "Properties (metadata, transcript, segments)", fn: () => { pp.nav("library"); setTimeout(() => pp.libOpen && pp.libOpen(id), 300); } }, { label: "Asset report", fn: () => window.open(reportUrl("asset/" + id + ".html")) },
      { label: "Copy as playlist item", fn: () => setClip("items", [{ id: "c", mediaId: m.id, title: m.title, path: m.path, type: m.type || "video", duration: m.duration || 0, timeIn: 0, timeOut: 0, timingMode: "soft_start", transitionType: "cut", priority: 0 }], m.title) }, { label: "Copy title", fn: () => copyText(m.title || "") },
      { label: "Transcribe (fast)", fn: () => POST("/media/" + encodeURIComponent(id) + "/transcribe", { level: "fast" }).then(() => toast("Transcription queued", "ok")) }, { label: "Copy media id", fn: () => copyText(id) }, { label: "Copy path", fn: () => copyText(m.path || "") }, { sep: true },
      { label: "Set type", sub: ["program", "ad", "promo", "psa", "station_id", "bumper", "filler", "news", "weather", "image"].map(t => ({ label: t, fn: () => pp.setMediaType(id, t) })) },
      { label: m.archived ? "Unarchive" : "Archive", fn: () => pp.archiveMedia(id, !m.archived) }, { label: "Remove from library", danger: true, fn: () => pp.deleteMedia(id, m.title) }]); },
  channelMenu(ev, id) { const c = chan(id); const armed = c.executor && c.executor.armed;
    ctxMenu(ev, [{ label: "Play", fn: () => pp.play(id) }, { label: "Pause", fn: () => pp.pause(id) }, { label: "Skip to next", fn: () => pp.skip(id) }, { label: "Hold on slate", fn: () => pp.hold(id) }, { label: "Stop", danger: true, fn: () => pp.stop(id) }, { sep: true },
      { label: armed ? "Disarm schedule" : "Arm schedule", fn: () => pp.arm(id, !armed) }, { label: "Load hour now", fn: () => pp.loadHour(id) }, { label: "Snap to schedule", fn: () => pp.snap(id) }, { label: "Re-roll query blocks", hint: "fresh picks for this hour", fn: () => POST("/channels/" + id + "/reroll").then(() => toast("Blocks re-rolled", "ok")) }, { sep: true },
      { label: "Edit today's schedule", fn: () => pp.editToday(id) }, { label: "Programming (recipes, clock)", fn: () => pp.setActive(id, () => pp.nav("programming")) }, { label: "As-played report (today)", fn: () => window.open(reportUrl("asplayed.html?channel=" + id)) }, { label: "Open preview stream", fn: () => window.open("/preview/" + id + ".mjpeg" + tokenQ()) }, { sep: true },
      { label: "Channel properties (outputs, identity)", fn: () => pp.setActive(id, () => pp.nav("outputs")) }, { label: "Copy channel id", fn: () => copyText(id) }, { sep: true },
      { label: "Restart engine", danger: true, fn: () => pp.restart(id) }]); },
  rundownMenu(ev, id, runlistId, mediaId, hourStartMs) { ctxMenu(ev, [{ label: "Edit this hour", disabled: !hourStartMs, fn: () => pp.editRundownHour(id, hourStartMs) }, { label: "Skip to next", fn: () => pp.skip(id) }, { label: "Re-roll query blocks", fn: () => POST("/channels/" + id + "/reroll").then(() => toast("Blocks re-rolled", "ok")) }, { sep: true },
      mediaId ? { label: "Open media", fn: () => { pp.nav("library"); setTimeout(() => pp.libOpen && pp.libOpen(mediaId), 300); } } : null, mediaId ? { label: "Asset report", fn: () => window.open(reportUrl("asset/" + mediaId + ".html")) } : null, { label: "Copy runlist id", fn: () => copyText(runlistId) }]); },
  fillMenu(ev) { if (!S.selSlot) { toast("Pick an hour first"); return; } const cs = currentSlot(); const tplId = cs.date.getDay() + "-" + S.selSlot.hour, datedId = iso(cs.date) + "-" + S.selSlot.hour;
    const lay = (recipeId, dated) => POST("/schedule/" + S.active + "/hour-recipe", { recipeId, scope: "hour", slotId: dated ? datedId : tplId, keepLocks: true, apply: false }).then(r => { const sl = r.slots[dated ? datedId : tplId]; if (!sl) { toast("Nothing laid", "err"); return; } if (!S.editItems) pp.editLoad(); S.editItems = sl.items || []; S.editDirty = true; const sid = dated ? datedId : tplId; S.resolved = r.runlists && r.runlists[sid] ? r.runlists[sid] : null; const pv = r.previews && r.previews[sid]; render(); toast("Recipe laid" + (pv ? ": " + pv.picks + " file(s) picked" + (pv.unresolved ? ", " + pv.unresolved + " block(s) found nothing" : "") : "") + " — save to keep it", pv && pv.unresolved ? "err" : "ok"); });
    loadRecipes();
    ctxMenu(ev, [{ label: "Lay recipe into editor", sub: recipeList().map(r => ({ label: r.name, fn: () => lay(r.id) })) },
      { label: "Weighted fill into editor", fn: () => POST("/schedule/" + S.active + "/weighted-fill", { scope: "hour", slotId: tplId, keepLocks: true, apply: false }).then(r => { const sl = r.slots[tplId]; if (!sl) return; if (!S.editItems) pp.editLoad(); S.editItems = sl.items || []; S.editDirty = true; render(); toast((sl.items || []).length + " items — save to keep", "ok"); }) },
      { label: "Shuffle ads (editor)", fn: () => { if (!S.editItems) pp.editLoad(); const free = S.editItems.map((x, i) => [x, i]).filter(([x]) => !x.isLocked && !["program", "live"].includes(x.type) && x.timingMode !== "hard_start"); const pool = free.map(([x]) => x); for (let i = pool.length - 1; i > 0; i--) { const j = Math.floor(Math.random() * (i + 1)); [pool[i], pool[j]] = [pool[j], pool[i]]; } free.forEach(([, i], k) => S.editItems[i] = pool[k]); S.editDirty = true; render(); } }, { sep: true },
      { label: "Clear editor (keep locks)", fn: () => { if (!S.editItems) pp.editLoad(); S.editItems = S.editItems.filter(x => x.isLocked); S.editDirty = true; render(); } },
      { label: "Manage recipes & format clock…", fn: () => pp.nav("programming") }]); },
  blockToggle() { if (!S.selSlot) { toast("Pick an hour first"); return; } S.blockOpen = !S.blockOpen; S.blockEditIndex = -1; if (S.blockOpen) { S.block.nthLatest = 0; S.block.noRepeatMin = 30; } render(); },
  // The form shows the fields an operator tunes; everything else on the block (tags, clients, min/max length, folder weight and recursion) rides along from the stored block.
  // A query block wins over a folder block, exactly as RunlistBuilder resolves an item that holds both.
  // Rules as text, one per line or ";"-separated: "[not] field op value", e.g. "ageDays lt 14; not tag is expired; category in news, weather".
  rulesFromText(t) { return String(t || "").split(/[;\n]/).map(x => x.trim()).filter(Boolean).map(line => { let w = line.split(/\s+/); let neg = false; if (w[0].toLowerCase() === "not") { neg = true; w = w.slice(1); }
      const r = { field: w[0] || "", op: (w[1] || "is").toLowerCase() }; const v = w.slice(2).join(" "); if (r.op !== "exists" && r.op !== "empty") r.value = ["lt", "lte", "gt", "gte"].includes(r.op) ? (parseFloat(v) || 0) : v; if (neg) r.not = true; return r; }); },
  rulesToText(rules) { return (rules || []).filter(r => !r.rules).map(r => (r.not ? "not " : "") + r.field + " " + (r.op || "is") + (r.value !== undefined ? " " + r.value : "")).join("; "); },
  blockCond(b) { return { maxAgeDays: parseFloat(b.newer) || 0, minAgeDays: parseFloat(b.older) || 0, ageField: b.ageField || "added", rules: pp.rulesFromText(b.rulesText), match: b.match || "all" }; },
  blockEdit(i) { const it = S.editItems[i]; const q = it.queryBlock || {}, f = it.folderBlock || {}; const asQuery = Object.keys(q).length > 0 || !Object.keys(f).length;
    S.block = asQuery ? { kind: "query", types: (q.types || []).join(", "), categories: (q.categories || []).join(", "), subcategories: (q.subcategories || []).join(", "), series: (q.series || []).join(", "), prefer: q.prefer || "least_played", minutes: (q.targetSec || 0) / 60, nthLatest: q.nthLatest || 0, noRepeatMin: q.noRepeatMin !== undefined ? q.noRepeatMin : 30, folder: "", order: "random" }
      : { kind: "folder", folder: f.folder || "", order: f.order || "random", minutes: (f.targetSec || 0) / 60, types: "", categories: "", subcategories: "", series: "", prefer: "least_played", nthLatest: 0, noRepeatMin: 30 };
    { const c = asQuery ? q : f; Object.assign(S.block, { newer: c.maxAgeDays > 0 ? c.maxAgeDays : "", older: c.minAgeDays > 0 ? c.minAgeDays : "", ageField: c.ageField || "added", rulesText: pp.rulesToText(c.rules), match: c.match || "all" }); }
    S.blockOpen = true; S.blockEditIndex = i; render(); },
  blockAdd() { if (!S.editItems) pp.editLoad(); const b = S.block; const L = v => String(v || "").split(",").map(x => x.trim()).filter(Boolean); const secs = Math.max(0, (parseFloat(b.minutes) || 0) * 60);
    const old = S.blockEditIndex >= 0 && S.editItems[S.blockEditIndex] ? S.editItems[S.blockEditIndex] : null;
    let it; if (b.kind === "folder") { if (!b.folder.trim()) { toast("Folder is required", "err"); return; } it = { folderBlock: Object.assign({ weight: "freshness" }, old && old.folderBlock, { folder: b.folder.trim(), targetSec: secs, order: b.order }), type: "program" }; }
    else { const base = (old && old.queryBlock) || {}; const types = L(b.types); if (!types.length && !L(b.categories).length && !L(b.series).length && !L(b.subcategories).length && !(base.tags || []).length && !(base.clients || []).length) { const c0 = pp.blockCond(b); if (!(c0.maxAgeDays > 0 || c0.minAgeDays > 0 || c0.rules.length)) { toast("Types, categories, series, an age limit or a rule required", "err"); return; } }
      const nth = parseInt(b.nthLatest), rep = parseFloat(b.noRepeatMin);
      it = { queryBlock: Object.assign({}, base, { types, targetSec: secs, prefer: b.prefer, categories: L(b.categories), subcategories: L(b.subcategories), series: L(b.series), nthLatest: nth > 0 ? nth : 0, noRepeatMin: rep >= 0 ? rep : 30 }), type: types[0] || "program" }; }
    Object.assign(it.queryBlock || it.folderBlock, pp.blockCond(b));
    it.duration = secs; it.title = blockLabel(it);
    if (old) { const next = Object.assign({}, old, it); if (it.folderBlock) delete next.queryBlock; else delete next.folderBlock; S.editItems[S.blockEditIndex] = next; }    // the other kind must not linger: it would air while the label shows this one
    else S.editItems.push(Object.assign({ id: "q" + Date.now().toString(36), timingMode: "soft_start", mediaId: "", path: "", timeIn: 0, timeOut: 0, transitionType: "cut", priority: 0 }, it));
    S.blockOpen = false; S.blockEditIndex = -1; S.editDirty = true; render(); },
  previewResolved() { const cs = currentSlot(); if (!cs.id) { toast("Pick an hour first"); return; } const body = S.editItems ? Object.assign({}, cs.slot || {}, S.editFlags, { channelId: S.active, hour: S.selSlot.hour, items: cleanItems(S.editItems) }) : {}; POST("/schedule/" + S.active + "/slot/" + cs.id + "/resolved", body).then(r => { S.resolved = r; render(); }); },
  nav(v) { S.view = v; location.hash = v; document.querySelectorAll("#tabs button").forEach(b => b.classList.toggle("active", b.dataset.view === v)); onEnter(v); render(); },
};
// Console modules are evaluated before this file (ES module imports), so they hand their handlers over here.
window.pp.uploadOne = (f, t) => uploadOne(f, t);
Object.assign(window.pp, ppExtIntegrations, ppExtLibrary, ppExtMux, ppExtSpots, ppExtReports, ppExtRemote, ppExtProgramming, ppExtOverlays, ppExtSuites, ppExtFlight, ppExtReporter, ppExtNowPlaying);
Object.assign(window.pp, ppExtAutomation);
document.querySelectorAll("#tabs button").forEach(b => b.onclick = () => pp.nav(b.dataset.view));

// ---------------------------------------------------------------------------
// Data loaders per view
// ---------------------------------------------------------------------------
function startOfWeek(d) { const s = new Date(d.getFullYear(), d.getMonth(), d.getDate()); s.setDate(s.getDate() - s.getDay()); return s; }
// The newest request wins: an answer overtaken by a later request, or for a channel or week that is no longer shown, is dropped. The promise follows the newest request.
let weekSeq = 0, weekLatest = Promise.resolve();
function loadWeek() {
  if (!S.weekStart) S.weekStart = startOfWeek(new Date());
  const seq = ++weekSeq, ch = S.active, start = iso(S.weekStart);
  return weekLatest = GET("/schedule/" + ch + "/week?start=" + start).then(w => { if (seq !== weekSeq) return weekLatest; if (ch !== S.active || start !== iso(S.weekStart)) return; S.week = w.slots || {}; render(); });
}
async function loadMedia() { const m = await GET("/media").catch(() => null); if (m) { S.media = m.media || []; render(); } }
async function loadReport() { const from = new Date(S.reportDay); from.setHours(0, 0, 0, 0); const to = new Date(S.reportDay); to.setHours(23, 59, 59, 0); const r = await GET("/asrun?channel=" + S.active + "&from=" + from.getTime() + "&to=" + to.getTime() + "&limit=2000"); S.reportRows = r.rows || []; S.reportSummary = r.summary || {}; render(); }
function onEnter(v) { if (v === "ingest") v = "library"; onEnterIntegration(v); onEnterLibrary(v); onEnterMux(v); onEnterSpots(v); onEnterReports(v); onEnterRemote(v); onEnterProgramming(v); onEnterOverlays(v); onEnterSuites(v); onEnterFlight(v); onEnterReporter(v); onEnterNowPlaying(v); if ((v === "mesh" || v === "nowplaying" || v === "channels") && !S.media.length) loadMedia(); if (v === "schedule") { loadWeek().then(() => { if (S.selSlot && !S.editDirty) pp.editLoad(); }); loadRecipes(true); if (!S.media.length) loadMedia(); } if (v === "reports") loadReport(); if (v === "ingest") { loadMedia(); GET("/media/upload-info").then(i => { S.uploadInfo = i; render(); }).catch(() => {}); } }
// The hour at grid cell (day, hour) of the shown week: the dated override when it holds items, else the weekly template.
function slotAt(day, hour) {
  if (!S.weekStart) return {};
  const d = new Date(S.weekStart); d.setDate(d.getDate() + day);
  const dated = iso(d) + "-" + hour, tpl = day + "-" + hour;
  if (S.week[dated] && S.week[dated].items && S.week[dated].items.length) return { id: dated, slot: S.week[dated], date: d };
  return { id: tpl, slot: S.week[tpl], date: d };
}
function currentSlot() { return S.selSlot ? slotAt(S.selSlot.day, S.selSlot.hour) : {}; }

// ---------------------------------------------------------------------------
// Open-hour helpers. The editor holds a copy of one hour of one channel and week; these keep that copy honest about the server
// (a null or stale S.editItems makes the next Save wipe or revert what a server-side operation just wrote) and about unsaved work.
// ---------------------------------------------------------------------------
function cellOf(slotId) {      // grid cell {day, hour} of slot id "D-H" or "YYYY-MM-DD-H" in the shown week (day -1 when the date is outside it)
  const p = String(slotId).split("-");
  if (p.length === 2) return { day: +p[0], hour: +p[1] };
  if (p.length === 4 && S.weekStart) { const dt = new Date(+p[0], +p[1] - 1, +p[2]), ws = new Date(S.weekStart.getFullYear(), S.weekStart.getMonth(), S.weekStart.getDate()); const day = Math.round((dt - ws) / 86400000); return { day: day >= 0 && day < 7 ? day : -1, hour: +p[3] }; }
  return { day: -1, hour: -1 };
}
function leaveHour(then) {     // drop the open hour's editor state (confirming first when it holds unsaved edits), then continue
  const go = () => { S.editItems = null; S.editFlags = {}; S.editDirty = false; S.propIndex = -1; S.propOpen = false; S.blockOpen = false; S.blockEditIndex = -1; S.resolved = null; then(); };
  if (S.editDirty) confirmDo("Discard the unsaved edits in the open hour?", go); else go();
}
function openHour(d, h, then) {    // open grid cell d/h in the editor, asking first when that would drop unsaved edits to another hour
  const same = S.selSlot && S.selSlot.day === d && S.selSlot.hour === h;
  const go = () => { if (!same || !S.editItems) pp.selSlot(d, h); if (then) then(); };
  if (!same && S.editDirty) confirmDo("Discard the unsaved edits in the open hour?", go); else go();
}
function afterWrite(touches) { return loadWeek().then(() => { if (S.selSlot && touches(S.selSlot)) pp.editLoad(); }); }
// A server-side write that can change hours in the grid. `touches(S.selSlot)` says whether it can reach the hour open in the editor:
// then it asks before replacing unsaved edits, and reloads the editor from the server once the week is back.
function hourOp(touches, run, ask) {
  const dirty = !!(S.editDirty && S.selSlot && touches(S.selSlot));
  const go = () => run().then(() => afterWrite(touches), () => loadWeek());      // api() already toasted a failure; the grid may still have changed (partial multi-hour writes)
  if (ask || dirty) confirmDo(((ask || "") + (dirty ? " Unsaved edits in the open hour will be replaced." : "")).trim(), go); else go();
}
function weekAndHour() { return loadWeek().then(() => { if (S.selSlot) pp.editLoad(); }); }
// Lock anchors travel as a number >= 0 or not at all: the server clamps a negative lockAtSec to 0 and locks the item at :00.

// --- a programme that runs past the top of the hour: ask what to do (continue / break up / cut / remove) ---
const ovItemSec = it => {
  const q = it.queryBlock || {}, f = it.folderBlock || {};
  if (Object.keys(q).length || Object.keys(f).length) return +(q.targetSec || f.targetSec || it.duration || 0);
  const full = +(it.duration || (mediaById(it.mediaId) || {}).duration || 0), tin = +it.timeIn || 0, tout = +it.timeOut || 0;
  return Math.max(0, tout > tin ? tout - tin : full - tin);
};
const findOverrun = items => {
  let t = 0;
  for (let i = 0; i < (items || []).length; i++) {
    const it = items[i], d = ovItemSec(it);
    if (t + d > 3602 && !it.overrunMode && !it.isLocked) return { index: i, item: it, offset: t, dur: d, over: t + d - 3600 };
    t += d;
  }
  return null;
};
function overrunDialog(o, dated, fromSave) {
  let d = document.getElementById("overrunDlg");
  if (!d) { d = document.createElement("dialog"); d.id = "overrunDlg"; document.body.appendChild(d); }
  const next = pad2((S.selSlot.hour + 1) % 24) + ":00";
  d.innerHTML = `<style>#overrunDlg .ovopt{display:flex;gap:10px;align-items:baseline;cursor:pointer;line-height:1.5}#overrunDlg .ovopt input[type=radio]{width:auto;flex:none;margin:0;position:relative;top:2px}#overrunDlg .ovopt input[type=number]{display:inline-block}</style>
  <form method="dialog" style="min-width:520px;max-width:660px;display:grid;gap:10px">
    <h3 style="margin:0">&#8220;${esc(o.item.title || "This item")}&#8221; runs ${fmtClock(o.over)} past the end of the hour</h3>
    <div class="muted">It starts ${fmtClock(o.offset)} into ${pad2(S.selSlot.hour)}:00 and is ${fmtClock(o.dur)} long. What should happen?</div>
    <label class="ovopt"><input type="radio" name="ovm" value="continue" checked> <span>Continue into the next hour: it plays in full and ${next} picks up ${fmtClock(o.over)} late</span></label>
    <label class="ovopt"><input type="radio" name="ovm" value="split"> <span>Break it up with ad breaks <span id="ovSegTxt">every <input id="ovEvery" class="small mono" type="number" min="1" value="15" style="width:56px"> min</span>, breaks of <input id="ovBreak" class="small mono" type="number" min="0.25" step="0.25" value="2" style="width:56px"> min; this hour ends on time and the rest opens ${next}</span></label>
    <label class="ovopt"><input type="radio" name="ovm" value="cut"> <span>Cut it at the top of the hour (${fmtClock(3600 - o.offset)} of it airs)</span></label>
    <label class="ovopt"><input type="radio" name="ovm" value="remove"> <span>Remove it from this hour</span></label>
    <div class="row" id="ovNextRow" style="gap:8px">Then the rest of ${next}
      <select id="ovNext"><option value="keep">keeps its own schedule</option><option value="ads">is filled with ads and promos</option><option value="recipe">is filled from a recipe</option></select>
      <select id="ovRecipe" style="display:none">${(hourRecipes || [{ id: "default", name: "Default" }]).map(r => `<option value="${esc(r.id)}">${esc(r.name)}</option>`).join("")}</select></div>
    <div class="muted" id="ovNote" style="font-size:11px">This hour and ${next} are both saved when you apply.</div>
    <div class="row end" style="gap:6px"><button value="cancel" class="ghost">Cancel</button>${fromSave ? '<button value="asis" title="Save without deciding: at air the hour ends at :00 and the item is cut there">Save as is</button>' : ""}<button value="apply" class="primary">Apply</button></div>
  </form>`;
  const mode = () => (d.querySelector('input[name="ovm"]:checked') || {}).value || "continue";
  const sync = () => { const m = mode(); d.querySelector("#ovNextRow").style.display = (m === "continue" || m === "split") ? "" : "none";
    d.querySelector("#ovRecipe").style.display = d.querySelector("#ovNext").value === "recipe" ? "" : "none";
    d.querySelector("#ovNote").textContent = m === "remove" ? "Only this hour changes." : "This hour and " + next + " are both saved when you apply."; };
  d.querySelectorAll('input[name="ovm"]').forEach(r => r.addEventListener("change", sync)); d.querySelector("#ovNext").addEventListener("change", sync); sync();
  if (o.item.mediaId) GET("/media/" + o.item.mediaId + "/segments").then(sg => { const n = (sg.segments || []).length; if (n > 1) d.querySelector("#ovSegTxt").textContent = "at its " + n + " segments"; }).catch(() => {});
  d.returnValue = "";
  d.onclose = () => {
    const v = d.returnValue; if (v !== "apply" && v !== "asis") return;
    if (v === "asis") { S.editItems[o.index] = Object.assign({}, S.editItems[o.index], { overrunMode: "accepted" }); pp.editSave(dated, true); return; }
    const m = mode();
    if (m === "remove") { S.editItems.splice(o.index, 1); S.editDirty = true; render(); toast("Removed " + (o.item.title || "the item"), "ok"); return; }
    pp.overrunApply(o, dated, m, d.querySelector("#ovNext").value, d.querySelector("#ovRecipe").value, parseFloat(d.querySelector("#ovBreak").value) || 2, parseFloat((d.querySelector("#ovEvery") || {}).value) || 15);
  };
  d.showModal();
}
const cleanItems = items => items.map(it => { if (it.lockAtSec === undefined || it.lockAtSec >= 0) return it; const c = Object.assign({}, it); delete c.lockAtSec; return c; });
// The Schedule picker lists S.media, the Library lists L.rows: a menu on either must find its record in whichever list is loaded.
const mediaById = id => S.media.find(x => x.id === id) || ((window.L && window.L.rows) || []).find(x => x.id === id);
const mediaChanged = () => { loadMedia(); if (S.view === "library" && pp.libReload) pp.libReload(); };
function addedNote(what) { if (S.view !== "schedule" && S.selSlot) toast("Added " + what + " to " + ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"][S.selSlot.day] + " " + pad2(S.selSlot.hour) + ":00 (open Schedule to review and save)", "ok"); }
const mediaDur = {}; function dur(it) { const full = it.duration || mediaDur[it.mediaId] || 0; return Math.max(0, it.timeOut > 0 ? it.timeOut - (it.timeIn || 0) : full - (it.timeIn || 0)); }

function uploadOne(file, title) {
  const u = { name: file.name, size: file.size, sent: 0, done: false, error: "" }; S.uploads.unshift(u); render();
  const xhr = new XMLHttpRequest();
  xhr.open("PUT", "/api/v1/media/upload?name=" + encodeURIComponent(file.name) + "&type=" + encodeURIComponent(S.ingestType) + (pp.upQuery ? pp.upQuery(title) : "") + (S.token ? "&token=" + encodeURIComponent(S.token) : ""));
  xhr.setRequestHeader("Content-Type", "application/octet-stream");
  xhr.upload.onprogress = e => { u.sent = e.loaded; renderSoon(); };
  xhr.onload = () => { u.done = true; if (xhr.status >= 200 && xhr.status < 300) { toast("Uploaded " + file.name, "ok"); setTimeout(loadMedia, 1500); setTimeout(loadMedia, 6000); } else { u.error = "HTTP " + xhr.status; toast("Upload failed: " + file.name, "err"); } render(); };
  xhr.onerror = () => { u.done = true; u.error = "network"; render(); };
  xhr.send(file);
}
// Drop anywhere on the page: files and whole folders (directory entries are walked in the browser).
async function collectDropped(dt) {
  const out = [];
  const walk = async (entry, prefix) => {
    if (entry.isFile) { const f = await new Promise((res, rej) => entry.file(res, rej)); out.push(f); }
    else if (entry.isDirectory) { const r = entry.createReader(); let batch; do { batch = await new Promise((res, rej) => r.readEntries(res, rej)); for (const e of batch) await walk(e, prefix + entry.name + "/"); } while (batch.length); }
  };
  const items = dt.items ? [...dt.items] : [];
  const entries = items.map(i => i.webkitGetAsEntry ? i.webkitGetAsEntry() : null);
  if (entries.some(Boolean)) { for (const e of entries) if (e) await walk(e, ""); } else out.push(...dt.files);
  return out;
}
const isMediaName = n => /\.(mp4|mov|mxf|mkv|ts|m2ts|mpg|mpeg|avi|wmv|webm|m4v|mts|mp3|wav|aac|flac|m4a|png|jpg|jpeg)$/i.test(n);
let dragDepth = 0;
const hasFiles = e => e.dataTransfer && [...e.dataTransfer.types].includes("Files");
document.addEventListener("dragenter", e => { if (!hasFiles(e)) return; e.preventDefault(); if (++dragDepth === 1) $("#dropAll").classList.add("show"); });
document.addEventListener("dragover", e => { if (hasFiles(e)) { e.preventDefault(); e.dataTransfer.dropEffect = "copy"; } });
document.addEventListener("dragleave", () => { if (--dragDepth <= 0) { dragDepth = 0; $("#dropAll").classList.remove("show"); } });
document.addEventListener("drop", async e => {
  dragDepth = 0; $("#dropAll").classList.remove("show");
  if (!hasFiles(e)) return;
  e.preventDefault();
  if (e.target && e.target.closest && e.target.closest("#drop")) return;      // the Library's own zone handles it
  const files = (await collectDropped(e.dataTransfer)).filter(f => isMediaName(f.name));
  if (!files.length) { toast("No media files in the drop"); return; }
  toast("Uploading " + files.length + " file(s)", "ok"); pp.uploadFiles(files);
});
// ---------------------------------------------------------------------------
// Context menus (right-click): one floating <div id="ctx">; items [{label, fn, danger, sep, disabled, sub:[...]}]
// ---------------------------------------------------------------------------
// The text-size zoom is CSS zoom on <html>: a fixed element's left/top and its offsetWidth are in zoomed CSS px, while clientX/Y, innerWidth/Height and
// getBoundingClientRect are viewport px. So menus are placed from viewport coordinates and divided by the zoom, never mixed with offset sizes.
const zoomNow = () => parseFloat(document.documentElement.style.zoom || "1") || 1;
// Put a fixed menu at viewport point (x, y), kept inside the viewport. A menu taller than the viewport scrolls instead of losing its last items.
function placeFixed(m, x, y) {
  const z = zoomNow(), vw = window.innerWidth, vh = window.innerHeight; let r = m.getBoundingClientRect();
  if (r.height > vh - 16) { m.style.maxHeight = ((vh - 16) / z) + "px"; m.style.overflowY = "auto"; r = m.getBoundingClientRect(); }
  m.style.left = (Math.max(4, Math.min(x, vw - r.width - 8)) / z) + "px"; m.style.top = (Math.max(4, Math.min(y, vh - r.height - 8)) / z) + "px";
}
// A submenu opens beside its item (left of the parent when the right side has no room) and is fixed-positioned, so neither the viewport edge
// nor the scrolling of a tall parent menu clips it.
function openSub(item, sub) {
  const z = zoomNow(), vw = window.innerWidth, vh = window.innerHeight;
  sub.style.maxHeight = ""; sub.style.overflowY = ""; sub.style.display = "block"; sub.style.position = "fixed"; sub.style.margin = "0"; sub.style.left = "0px"; sub.style.top = "0px";
  const ir = item.getBoundingClientRect(); let sr = sub.getBoundingClientRect();
  if (sr.height > vh - 16) { sub.style.maxHeight = ((vh - 16) / z) + "px"; sub.style.overflowY = "auto"; sr = sub.getBoundingClientRect(); }
  const x = ir.right + sr.width > vw - 4 ? Math.max(4, ir.left - sr.width) : ir.right, y = Math.max(4, Math.min(ir.top - 4, vh - sr.height - 8));
  sub.style.left = (x / z) + "px"; sub.style.top = (y / z) + "px";
}
function ctxMenu(ev, items) {
  ev.preventDefault(); ev.stopPropagation(); closeCtx();
  const m = document.createElement("div"); m.id = "ctx"; m.className = "ctx";
  const build = (list, into) => { for (const it of list) { if (!it) continue; if (it.sep) { into.appendChild(Object.assign(document.createElement("div"), { className: "sep" })); continue; }
    const d = document.createElement("div"); d.className = "it" + (it.danger ? " danger" : "") + (it.disabled ? " dis" : "") + (it.sub ? " has-sub" : ""); d.innerHTML = `<span>${esc(it.label)}</span>${it.hint ? `<small>${esc(it.hint)}</small>` : ""}${it.sub ? "<b>&#9656;</b>" : ""}`;
    if (it.sub) { const sub = document.createElement("div"); sub.className = "ctx sub"; build(it.sub, sub); d.appendChild(sub); d.onmouseenter = () => openSub(d, sub); d.onmouseleave = () => { sub.style.display = ""; }; }
    else if (!it.disabled) d.onclick = e => { e.stopPropagation(); closeCtx(); it.fn && it.fn(); };
    into.appendChild(d); } };
  build(items, m); document.body.appendChild(m);
  placeFixed(m, ev.clientX, ev.clientY);
  setTimeout(() => { document.addEventListener("click", closeCtx, { once: true }); document.addEventListener("contextmenu", closeCtx, { once: true }); }, 0);
}
function closeCtx() { const m = $("#ctx"); if (m) m.remove(); }
document.addEventListener("keydown", e => { if (e.key === "Escape") closeCtx(); });
// navigator.clipboard exists only in secure contexts, and the dashboard is usually opened over plain http://<lan-ip>: fall back to a hidden textarea and execCommand.
function copyText(t) {
  t = String(t ?? "");
  const legacy = () => {
    const ta = document.createElement("textarea"); ta.value = t; ta.setAttribute("readonly", ""); ta.style.cssText = "position:fixed;left:-9999px;top:0;opacity:0";
    document.body.appendChild(ta); ta.focus(); ta.select(); let ok = false; try { ok = document.execCommand("copy"); } catch {} ta.remove();
    toast(ok ? "Copied" : "Copy blocked by the browser: " + t.slice(0, 120), ok ? "ok" : "err");
  };
  if (navigator.clipboard && window.isSecureContext) navigator.clipboard.writeText(t).then(() => toast("Copied", "ok"), legacy); else legacy();
}
const reportUrl = p => "/reports/" + p + (tokenQ() ? (p.includes("?") ? "&" : "?") + tokenQ().slice(1) : "");
const askNum = (label, def) => { const v = prompt(label, def); if (v === null) return null; const n = parseFloat(v); return isNaN(n) ? null : n; };
const askMmss = (label, defSec) => { const v = prompt(label, fmtClock(defSec || 0)); if (v === null) return null; const p = v.split(":").map(Number); if (p.some(isNaN)) return null; return p.length === 3 ? p[0] * 3600 + p[1] * 60 + p[2] : p.length === 2 ? p[0] * 60 + p[1] : p[0]; };
const isBlock = it => !!(it && ((it.queryBlock && Object.keys(it.queryBlock).length) || (it.folderBlock && Object.keys(it.folderBlock).length)));
// Files a block resolved to in the last preview (Resolve, or a recipe laid into the editor); red when it found nothing.
const blockPicks = it => {
  const r = S.resolved; if (!r || !isBlock(it)) return "";
  const picks = (r.items || []).filter(x => x.queryFill && x.blockItemId === it.id);
  if (picks.length) return `<div style="font-size:10px;color:var(--t2);white-space:nowrap;overflow:hidden;text-overflow:ellipsis;max-width:520px">&#8594; ${picks.map(x => esc(x.title || "") + (x.overrun ? " (cut)" : "") + (x.repeat ? " (repeat)" : "")).join(" · ")}</div>`;
  const why = (r.notes || []).find(n => n.includes("unresolved") && (n.includes("'" + (it.title || "") + "'") || n.includes("'" + it.id + "'")));
  return why ? `<div style="font-size:10px;color:var(--danger,#e55)">&#8594; nothing: ${esc(why.slice(why.indexOf("unresolved") + 12))}</div>` : "";
};
const blockLabel = it => { const q = it.queryBlock || {}, f = it.folderBlock || {}; if (!Object.keys(q).length && Object.keys(f).length) return "\uD83D\uDCC1 " + (f.folder || "") + " (" + (f.order && f.order !== "random" ? "by " + f.order : "random") + ")"; const what = (q.series && q.series.length) ? q.series.join("/") : (q.subcategories && q.subcategories.length) ? q.subcategories.join("/") : (q.categories && q.categories.length) ? q.categories.join("/") : (q.types || []).join("/"); return "\u26A1 " + what + (q.targetSec ? " ~" + Math.round(q.targetSec / 60) + "m" : "") + " (" + String(q.prefer || "least_played").replace("_", " ") + ")"; };
// Recipes for the fill menus: loaded when the Schedule opens, re-read at most every 10 s when a menu opens, and on demand after the Programming tab saves.
// A failed read keeps the last list (the Default-only fallback until one succeeds) and is retried by the next call.
let hourRecipes = null, recipesAt = 0;
const recipeList = () => hourRecipes || [{ id: "default", name: "Default" }];
async function loadRecipes(force) { if (!force && hourRecipes && Date.now() - recipesAt < 10000) return; try { const r = await GET("/programming/recipes"); hourRecipes = [{ id: "default", name: (r.default && r.default.name) || "Default" }].concat((r.builtin || []).map(b => ({ id: b.id, name: "★ " + b.name }))).concat(r.recipes || []); recipesAt = Date.now(); } catch {} }
window.pp.reloadRecipes = () => loadRecipes(true);
const fmtB = b => b > 1e9 ? (b / 1e9).toFixed(2) + " GB" : b > 1e6 ? (b / 1e6).toFixed(1) + " MB" : Math.round(b / 1e3) + " kB";
const MEDIA_TYPES = ["program", "ad", "promo", "station_id", "bumper", "filler", "psa", "image", "live", "video"];
function viewIngest() {
  const q = S.mediaQuery.toLowerCase(); const list = S.media.filter(m => !q || String(m.title || "").toLowerCase().includes(q) || String(m.path || "").toLowerCase().includes(q));
  const typeSel = (cur, onchange) => `<select class="small" onchange="${onchange}">${MEDIA_TYPES.map(t => `<option ${t === cur ? "selected" : ""}>${t}</option>`).join("")}</select>`;
  const upRows = S.uploads.map(u => `<tr><td>${esc(u.name)}</td><td class="mono">${fmtB(u.size)}</td><td><div class="meter"><div class="bar" style="width:${u.size ? Math.min(100, 100 * u.sent / u.size) : 0}%;background:${u.error ? "var(--danger)" : u.done ? "var(--ready)" : "var(--accent)"}"></div></div></td><td>${u.error ? `<span style="color:var(--danger)">${esc(u.error)}</span>` : u.done ? "&#10003; probing" : Math.round(100 * u.sent / Math.max(1, u.size)) + "%"}</td></tr>`).join("");
  const rows = list.map(m => `<tr style="${m.archived ? "opacity:.5" : ""}"><td><b>${esc(m.title)}</b></td><td>${typeSel(m.type, "pp.setMediaType('" + m.id + "',this.value)")}</td><td class="mono">${fmtClock(m.duration)}</td><td class="mono" style="color:var(--t2)">${m.width ? m.width + "x" + m.height + " " + esc(m.frameRate || "") : ""}</td><td class="mono">${m.fileSizeBytes ? fmtB(m.fileSizeBytes) : ""}</td><td class="mono">${m.playCount || 0}</td><td class="mono" style="color:var(--t3);max-width:320px">${esc(m.path)}</td><td class="row" style="flex-wrap:nowrap"><a class="btn small" href="${pp.downloadUrl(m.id)}" download>&#8681; Download</a><button class="ghost small" onclick="pp.archiveMedia('${m.id}',${!m.archived})">${m.archived ? "Unarchive" : "Archive"}</button><button class="ghost small" onclick="pp.deleteMedia('${m.id}','${esc(m.title).replace(/'/g, "&#39;")}')">&#10005;</button></td></tr>`).join("");
  return `<div class="grid cols2">
    <div class="panel"><h3>Upload to the Supervisor <span class="right">${esc(S.uploadInfo.uploadDir || "")} · free ${S.uploadInfo.freeBytes ? fmtB(S.uploadInfo.freeBytes) : "?"}</span></h3><div class="body">
      <div id="drop" class="drop" ondragover="event.preventDefault();this.classList.add('over')" ondragleave="this.classList.remove('over')" ondrop="pp.dropFiles(event)" onclick="pp.pickFiles()">
        <div style="font-size:26px">&#8679;</div><div>Drop media files here or click to choose</div><div style="color:var(--t3);font-size:11px">Files are probed with ffprobe and added to the library as <b>${esc(S.ingestType)}</b></div></div>
      <div class="row" style="margin-top:8px"><label class="f">Import as ${typeSel(S.ingestType, "S.ingestType=this.value")}</label></div>
      ${S.uploads.length ? `<table style="margin-top:10px"><thead><tr><th>File</th><th>Size</th><th style="width:40%">Progress</th><th></th></tr></thead><tbody>${upRows}</tbody></table>` : ""}
    </div></div>
    <div class="panel"><h3>Scan a folder on the Supervisor</h3><div class="body grid" style="gap:10px">
      <label class="f">Folder <input id="scanFolder" placeholder="${esc(S.uploadInfo.mediaRoot || "D:\\Media\\Programs")}"></label>
      <div class="row"><label><input type="checkbox" id="scanRec" checked> recursive</label><span class="fill"></span><button class="primary" onclick="pp.scanFolder()">Scan</button></div>
      <div style="color:var(--t3);font-size:11px">Pending probes: ${S.uploadInfo.pendingProbes || 0}. The type comes from "Import as"; folder names like ads/, promo, bumper and ids/ override it.</div>
    </div></div>
    <div class="panel" style="grid-column:1/-1"><h3>Media library (${list.length}) <span class="right"><input id="mq" placeholder="search" value="${esc(S.mediaQuery)}" oninput="S.mediaQuery=this.value;renderSoon()"><button class="small" onclick="loadMedia()">&#8635;</button></span></h3>
      <div class="scroll"><table><thead><tr><th>Title</th><th>Type</th><th>Duration</th><th>Raster</th><th>Size</th><th>Plays</th><th>Path</th><th></th></tr></thead><tbody>${rows || `<tr><td colspan="8" class="empty">Library is empty: upload files or scan a folder</td></tr>`}</tbody></table></div></div>
  </div>`;
}
window.loadMedia = loadMedia;

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
function channelPicker() {
  return `<div class="row">${S.channels.map(c => `<button class="ghost small${c.profile.id === S.active ? " active" : ""}" onclick="pp.setActive(${jsq(c.profile.id)})">${esc(c.profile.name)}</button>`).join("")}</div>`;
}
function rundownHtml(id, showNext) {
  const rl = S.runlists[id] || {}, t = tele(id); const cur = rl.current && rl.current.items ? rl.current.items : []; const nx = showNext && rl.next && rl.next.items ? rl.next.items : [];
  const items = cur.concat(nx); const now = Date.now();
  if (!items.length) return `<div class="empty">No items pushed — schedule the hour or arm the channel</div>`;
  return `<ul class="rund">${items.map((it, index) => `<li class="${t.itemId === it.id ? "onair" : it.plannedEndMs < now ? "past" : ""}" ondragover="pp.npRundownOver(event)" ondragleave="event.currentTarget.style.boxShadow=''" ondrop="pp.npRundownDrop(event,${jsq(it.id)},${jsq(id)})" oncontextmenu="pp.rundownMenu(event,${jsq(id)},${jsq(it.id)},${jsq(it.mediaId || it.assetId || "")},${Number(((index < cur.length ? rl.current : rl.next) || {}).hourStartMs) || 0})"><span class="mono">${fmtTime(it.plannedStartMs, true)}</span><span style="overflow:hidden;text-overflow:ellipsis">${it.hard ? '<span style="color:var(--standby)">&#9670; </span>' : ""}${esc(it.title || it.path)}${mediaById(it.mediaId) ? ` <small style="color:var(--t3)"> · Added ${esc(mediaById(it.mediaId).addedAtMs ? new Date(mediaById(it.mediaId).addedAtMs).toLocaleDateString() : "—")} · ${Number(mediaById(it.mediaId).playCount || 0)} plays</small>` : ""}${it.note ? ` <span style="color:var(--t3)">${esc(it.note)}</span>` : ""}</span>${badge(it.type || "video", typeColor(it.type))}<span class="mono" style="text-align:right">${fmtClock(it.duration)}</span></li>`).join("")}</ul>`;
}
function channelCard(c) {
  const id = c.profile.id, t = tele(id), st = t.state || c.engineState || "OFFLINE", up = c.process && c.process.connected, armed = c.executor && c.executor.armed;
  const audio = (t.compositor && t.compositor.audio) || {}; const lufs = audio.momentaryLufs !== undefined ? audio.momentaryLufs : -70; const pos = Math.max(0, Math.min(1, (lufs + 60) / 60));
  return `<div class="card${st === "ON_AIR" ? " onair" : ""}" oncontextmenu="pp.channelMenu(event,${jsq(id)})">
    <div class="hd"><span class="name">${esc(c.profile.name)}</span>${badge(st === "ON_AIR" ? "ON AIR" : st, stateColor(st), st === "ON_AIR")}${armed ? badge("armed", "var(--ready)") : badge("disarmed", "var(--t3)")}<span class="fill"></span><span style="color:var(--t3);font-size:10px" class="mono">${esc(c.profile.width + "x" + c.profile.height + " @" + c.profile.frameRate)}</span></div>
    <div class="mon">${up ? `<img src="/preview/${esc(id)}.mjpeg${tokenQ()}" alt="">` : `<div class="empty" style="padding-top:60px">ENGINE OFFLINE</div>`}
      <div class="prog" style="width:${t.duration > 0 ? Math.min(100, 100 * t.position / t.duration) : 0}%"></div>
      <div class="cap"><b>${esc(t.title || "")}</b><span class="mono" style="color:var(--accent)">${esc(t.timecode || "")}</span></div></div>
    <div class="kv"><span class="k">CURRENT</span><span class="v">${esc(t.title || "No event loaded")}</span><span class="mono" style="color:var(--onair)">${st === "ON_AIR" ? "-" + fmtClock(t.remaining) : ""}</span>
      <span class="k">NEXT</span><span class="v" style="color:var(--t1)">${esc(t.nextTitle || "No event loaded")}</span><span class="mono" style="color:var(--t3)">${t.nextReady ? "cued" : ""}</span>
      <span class="k">AUDIO</span><span class="v"><div class="meter" style="margin-top:4px"><div class="bar" style="width:${pos * 100}%"></div><div class="tgt" style="left:${(100 * (36 / 60))}%"></div></div></span><span class="mono">${lufs > -69 ? lufs.toFixed(1) + " LUFS" : "—"}</span></div>
    <div class="tr">
      <button class="warn small" onclick="pp.arm(${jsq(id)},${!armed})">${armed ? "DISARM" : "ARM"}</button>
      <button class="small" onclick="pp.loadHour(${jsq(id)})">LOAD HOUR</button>
      <button class="small" onclick="pp.snap(${jsq(id)})">SNAP</button>
      <span class="fill"></span>
      <button class="small" ${up ? "" : "disabled"} onclick="pp.stop(${jsq(id)})">&#9632;</button>
      <button class="success small" ${up ? "" : "disabled"} onclick="pp.${st === "ON_AIR" ? "pause" : "play"}(${jsq(id)})">${st === "ON_AIR" ? "&#8214;" : "&#9654;"}</button>
      <button class="small" ${up ? "" : "disabled"} onclick="pp.skip(${jsq(id)})">&#9197;</button>
      <button class="ghost small" onclick="pp.restart(${jsq(id)})" title="Restart engine">&#8635;</button>
    </div>
    <div class="panel" style="border:0;border-top:1px solid var(--b0);border-radius:0"><h3>Hour rundown <span class="right">${esc((c.executor && c.executor.lastPushReason) || "")}</span></h3>${rundownHtml(id, false)}</div>
  </div>`;
}
function viewChannels() {
  if (!S.channels.length) return `<div class="panel"><div class="body empty">${S.snapshot.supervisor ? "No channels configured" : "Connecting to the Supervisor…"}</div></div>`;
  const n = S.channels.length; const cols = n <= 4 ? "cols2" : "cols3";
  return `${viewAutomationChannels()}<div class="grid ${cols}">${S.channels.filter(c => !(c.profile.extra && c.profile.extra.hidden)).map(channelCard).join("")}</div>
    <div class="panel"><h3>Supervisor <span class="right">${esc(S.snapshot.dataDir || "")}</span></h3><div class="body row">
      <span>Uptime <b class="mono">${fmtClock((S.snapshot.uptimeMs || 0) / 1000)}</b></span><span>NTP offset <b class="mono">${Number(S.snapshot.ntpOffsetMs || 0).toFixed(1)} ms</b></span>
      <span>Health <b>${S.snapshot.health ? esc(S.snapshot.health.status + " (" + S.snapshot.health.score + ")") : "—"}</b></span><span>Engines <b>${S.channels.filter(c => c.process && c.process.connected).length}/${n}</b></span></div></div>`;
}
function itemProps(it, i) {
  const f = (label, k, v, w, ph) => `<label>${label} <input class="mono" style="width:${w || 70}px" value="${esc(v === undefined || v === null ? "" : v)}" placeholder="${ph || ""}" onchange="pp.editSet(${i},'${k}',this.value)"></label>`;
  return `<div class="body" style="border-top:1px solid var(--b0);background:var(--bg3)"><div class="row" style="justify-content:space-between"><b>Properties — ${esc(isBlock(it) ? blockLabel(it) : (it.title || it.path || ""))}</b><button class="ghost small" onclick="S.propOpen=false;render()">&#10005;</button></div>
    <div class="row" style="gap:10px;flex-wrap:wrap;margin-top:6px">
      <label>Title <input style="width:220px" value="${esc(it.title || "")}" onchange="pp.editSet(${i},'title',this.value)"></label>
      <label>Type <select onchange="pp.editSet(${i},'type',this.value)">${["program", "ad", "promo", "psa", "station_id", "bumper", "filler", "news", "weather", "live", "image", "video"].map(t => `<option ${(it.type || "video") === t ? "selected" : ""}>${t}</option>`).join("")}</select></label>
      <label>Timing <select onchange="pp.editSet(${i},'timingMode',this.value)">${["soft_start", "hard_start", "fill_until"].map(t => `<option ${(it.timingMode || "soft_start") === t ? "selected" : ""}>${t}</option>`).join("")}</select></label>
      ${f("Hard at (s)", "fixedOffsetSec", it.fixedOffsetSec >= 0 ? it.fixedOffsetSec : "", 70)}${f("In (s)", "timeIn", it.timeIn || 0, 64)}${f("Out (s)", "timeOut", it.timeOut || "", 64, "end")}${f("Duration (s)", "duration", it.duration || "", 70)}
      <label>Transition <select onchange="pp.editSet(${i},'transitionType',this.value)">${["cut", "mix", "dip", "wipe_left", "wipe_right", "push_left", "push_up", "squeeze"].map(t => `<option ${(it.transitionType || "cut") === t ? "selected" : ""}>${t}</option>`).join("")}</select></label>${f("Trans (s)", "transitionSec", it.transitionSec || "", 56)}
      ${(it.timingMode || "soft_start") === "hard_start" ? `<label title="what happens to the clip still playing when this exact start arrives">If a clip is playing <select onchange="pp.editSet(${i},'conflictPolicy',this.value);render()">${[["truncate", "cut it at the start"], ["finish_previous", "let it finish, start late"], ["drop_previous", "drop it"]].map(([v, l]) => `<option value="${v}" ${(it.conflictPolicy || "truncate") === v ? "selected" : ""}>${l}</option>`).join("")}</select></label>${it.conflictPolicy === "finish_previous" ? f("Max late (s, blank = any)", "maxLateSec", it.maxLateSec || "", 64) : ""}<label title="the events before it are timed backward from this start, so they end exactly on it"><input type="checkbox" ${it.backtime ? "checked" : ""} onchange="S.editItems[${i}].backtime=this.checked;S.editDirty=true"> Back-time</label>` : ""}<label title="a time correction may drop this event first"><input type="checkbox" ${it.optional ? "checked" : ""} onchange="S.editItems[${i}].optional=this.checked;if(this.checked)delete S.editItems[${i}].mandatory;S.editDirty=true;render()"> Optional</label><label title="a time correction never touches this event"><input type="checkbox" ${it.mandatory ? "checked" : ""} onchange="S.editItems[${i}].mandatory=this.checked;if(this.checked)delete S.editItems[${i}].optional;S.editDirty=true;render()"> Mandatory</label>${f("Min length (s, trim)", "minDurationSec", it.minDurationSec || "", 64)}${f("Priority", "priority", it.priority || 0, 50)}<label><input type="checkbox" ${it.loop ? "checked" : ""} onchange="S.editItems[${i}].loop=this.checked;S.editDirty=true"> Loop</label>
      <label><input type="checkbox" ${it.isLocked ? "checked" : ""} onchange="S.editItems[${i}].isLocked=this.checked;if(!this.checked){delete S.editItems[${i}].lockAtSec;delete S.editItems[${i}].lockLabel}S.editDirty=true;render()"> Locked</label>${f("Lock at (s)", "lockAtSec", it.lockAtSec >= 0 ? it.lockAtSec : "", 70)}<label>Lock label <input style="width:120px" value="${esc(it.lockLabel || "")}" onchange="pp.editSet(${i},'lockLabel',this.value)"></label>
      <label>CG on start <input style="width:200px" value="${esc((it.cgOnStart || []).join(", "))}" placeholder="lower_third, bug …" onchange="S.editItems[${i}].cgOnStart=this.value.split(',').map(x=>x.trim()).filter(Boolean);S.editDirty=true"></label>
      <label>CG on end <input style="width:160px" value="${esc((it.cgOnEnd || []).join(", "))}" onchange="S.editItems[${i}].cgOnEnd=this.value.split(',').map(x=>x.trim()).filter(Boolean);S.editDirty=true"></label>
      <label>Note <input style="width:220px" value="${esc(it.scheduleNote || it.note || "")}" onchange="pp.editSet(${i},'scheduleNote',this.value)"></label>
      ${it.mediaId ? `<span class="muted" style="font-size:10px">media ${esc(it.mediaId)} <button class="ghost small" onclick="pp.libOpen&&pp.libOpen(${jsq(it.mediaId)})">open</button></span>` : ""}${it.path ? `<span class="muted mono" style="font-size:10px">${esc(it.path)}</span>` : ""}
    </div></div>`;
}
function viewSchedule() {
  if (!S.weekStart) S.weekStart = startOfWeek(new Date());
  for (const m of S.media) mediaDur[m.id] = m.duration || 0;
  const days = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"]; const today = iso(new Date()), nowH = new Date().getHours();
  let g = `<div class="week"><div class="h"></div>`;
  for (let d = 0; d < 7; d++) { const dt = new Date(S.weekStart); dt.setDate(dt.getDate() + d); g += `<div class="h${iso(dt) === today ? " today" : ""}">${days[d]} ${dt.getMonth() + 1}/${dt.getDate()}</div>`; }
  for (let h = 0; h < 24; h++) {
    g += `<div class="hr">${pad2(h)}:00</div>`;
    for (let d = 0; d < 7; d++) {
      const dt = new Date(S.weekStart); dt.setDate(dt.getDate() + d); const dated = S.week[iso(dt) + "-" + h], tpl = S.week[d + "-" + h];
      const sl = (dated && dated.items && dated.items.length) ? dated : tpl; const items = sl && sl.items ? sl.items : [];
      const sec = items.reduce((a, it) => a + dur(it), 0), pct = Math.min(1.2, sec / 3600);
      const title = sl && sl.showBlock && sl.showBlock.title ? sl.showBlock.title : items.length ? items[0].title : "";
      const sel = S.selSlot && S.selSlot.day === d && S.selSlot.hour === h;
      g += `<div class="c${sel ? " sel" : ""}${iso(dt) === today && h === nowH ? " now" : ""}" onclick="pp.selSlot(${d},${h})" oncontextmenu="pp.cellMenu(event,${d},${h})"><span class="t">${items.some(i => i.type === "live") ? '<span style="color:var(--live)">&#9679; </span>' : ""}${esc(title)}</span><span class="p${pct > 1.01 ? " over" : pct === 0 ? " z" : ""}">${Math.round(pct * 100)}%</span>${pct > 0 ? `<div class="bar" style="right:${100 - Math.min(100, pct * 100)}%;background:${pct > 1.01 ? "var(--danger)" : pct > .95 ? "var(--ready)" : "var(--accent)"}"></div>` : ""}</div>`;
    }
  }
  g += `</div>`;
  // library
  const q = S.libQuery.toLowerCase(); const lib = S.media.filter(m => !m.archived && (!q || String(m.title || "").toLowerCase().includes(q) || String(m.type || "").includes(q)));
  const library = `<div class="panel"><h3>Media library <span class="right"><input id="libSearch" placeholder="search" value="${esc(S.libQuery)}" oninput="S.libQuery=this.value;renderSoon()" style="width:120px"></span></h3>
    <div class="scroll" style="max-height:60vh">${lib.length ? lib.map(m => `<div class="row" draggable="true" ondragstart="pp.mediaDrag(event,${jsq(m.id)})" style="padding:5px 10px;border-top:1px solid var(--b0);gap:8px" oncontextmenu="pp.mediaMenu(event,${jsq(m.id)})"><span style="width:3px;height:26px;background:${typeColor(m.type)};border-radius:1px"></span><span class="fill" style="overflow:hidden"><div style="white-space:nowrap;overflow:hidden;text-overflow:ellipsis">${esc(m.title)}</div><div style="color:var(--t3);font-size:10px">${badge(m.type || "video", typeColor(m.type))} ${m.type === "live" && m.liveSource ? esc(m.liveSource.type + " " + (m.liveSource.name || m.liveSource.url || "port " + m.liveSource.port)) : fmtClock(m.duration)}</div></span><button class="small" onclick="pp.editAddMedia(${jsq(m.id)})" title="Add to the selected hour">+</button></div>`).join("") : `<div class="empty">No media</div>`}</div>
    <div class="body" style="border-top:1px solid var(--b0)"><div style="color:var(--t2);font-size:10px;letter-spacing:1px;font-weight:700;margin-bottom:6px">LIVE INPUT / STREAM</div>
      <div class="grid" style="gap:6px"><div class="row"><select id="lvKind" onchange="S.liveKind=this.value"><option ${S.liveKind === "ndi" ? "selected" : ""}>ndi</option><option ${S.liveKind === "sdi_in" ? "selected" : ""}>sdi_in</option><option ${S.liveKind === "stream" ? "selected" : ""}>stream</option></select><input id="lvRef" class="fill" placeholder="${S.liveKind === "ndi" ? "NDI source name" : S.liveKind === "sdi_in" ? "SDI port" : "srt:// udp:// rtsp:// rtmp:// URL"}"></div>
      <div class="row"><input id="lvTitle" class="fill" placeholder="title"><input id="lvDur" style="width:90px" placeholder="secs / fill"><button class="success small" onclick="pp.editAddLive()">Add live</button></div>
      ${(tele(S.active).liveInputs || []).length ? `<div class="row">${(tele(S.active).liveInputs || []).map(l => `<button class="ghost small" onclick="document.getElementById('lvKind').value='ndi';S.liveKind='ndi';document.getElementById('lvRef').value=${jsq(l.name)}">&#9679; ${esc(l.name)}</button>`).join("")}</div>` : ""}</div></div></div>`;
  // editor
  const cs = currentSlot(); const items = S.editItems || (cs.slot && cs.slot.items ? cs.slot.items : []); const f = S.editFlags || {};
  const tot = items.reduce((a, it) => a + dur(it), 0);
  let cursor = 0, uncertain = false; const starts = items.map(it => { if (it.timingMode === "hard_start" && it.fixedOffsetSec >= 0) cursor = Math.max(cursor, Number(it.fixedOffsetSec)); const d = cs.date && new Date(cs.date.getFullYear(),cs.date.getMonth(),cs.date.getDate(),S.selSlot.hour,0,0,0); const value = d && !uncertain ? fmtTime(d.getTime() + cursor * 1000, true) : "—"; if (isBlock(it) && !dur(it)) uncertain = true; cursor += dur(it); return value; });
  const editor = S.selSlot ? `<div class="panel"><h3>Hour ${pad2(S.selSlot.hour)}:00 — ${cs.date ? cs.date.toDateString() : ""} <span class="right"><span style="color:${tot > 3630 ? "var(--danger)" : tot > 3570 ? "var(--ready)" : "var(--t1)"}" class="mono">${fmtClock(tot)} / 60:00</span>${S.editDirty ? '<span style="color:var(--standby)">unsaved</span>' : ""} <button class="small" onclick="pp.blockToggle()" title="Add a late-bound query or folder block">&#9889; Add block</button><button class="small" onclick="pp.fillMenu(event)" title="Lay a recipe, weighted fill, clear or shuffle this hour">Fill hour &#9662;</button><button class="small ghost" onclick="pp.previewResolved()" title="What the executor would build from this hour right now">Resolve</button><button class="small" onclick="pp.validateSlot()">Validate</button><button class="small ghost" onclick="pp.editDelete()">Delete</button><button class="small" onclick="pp.editSave(false)">Save template</button><button class="small primary" onclick="pp.editSave(true)">Save for ${cs.date ? (cs.date.getMonth() + 1) + "/" + cs.date.getDate() : "date"}</button></span></h3>
    <div class="row" style="padding:8px 12px;border-bottom:1px solid var(--b0)">
      <label><input type="checkbox" ${f.isLooping ? "checked" : ""} onchange="pp.editFlag('isLooping',this.checked)"> Loop hour</label>
      <label><input type="checkbox" ${f.randomizeAds !== false ? "checked" : ""} onchange="pp.editFlag('randomizeAds',this.checked)"> Randomize ads</label>
      <label><input type="checkbox" ${f.keepProgramOrder !== false ? "checked" : ""} onchange="pp.editFlag('keepProgramOrder',this.checked)"> Keep program order</label>
      <span class="fill"></span><label>Block <select onchange="pp.editFlag('blockType',this.value)">${["general", "show", "news", "sports", "paid_program", "maintenance"].map(b => `<option ${f.blockType === b ? "selected" : ""}>${b}</option>`).join("")}</select></label></div>
    ${S.blockOpen ? `<div class="row" style="padding:8px 12px;border-bottom:1px solid var(--b0);background:var(--bg3);gap:8px"><select class="small" onchange="S.block.kind=this.value;render()"><option value="query" ${S.block.kind === "query" ? "selected" : ""}>Query block</option><option value="folder" ${S.block.kind === "folder" ? "selected" : ""}>Folder block</option></select>
      ${S.block.kind === "query" ? `<input value="${esc(S.block.types)}" placeholder="types: program, ad" style="width:130px" onchange="S.block.types=this.value"><input value="${esc(S.block.categories)}" placeholder="categories" style="width:110px" onchange="S.block.categories=this.value"><input value="${esc(S.block.subcategories)}" placeholder="subcategories" style="width:110px" onchange="S.block.subcategories=this.value"><input value="${esc(S.block.series)}" placeholder="series" style="width:110px" onchange="S.block.series=this.value">
        <select class="small" onchange="S.block.prefer=this.value">${["least_played", "even", "newest", "oldest", "most_played", "random", "longest", "shortest"].map(x => `<option ${S.block.prefer === x ? "selected" : ""}>${x}</option>`).join("")}</select><label title="0 = the latest, 1 = the one before it (series blocks)">nth latest <input class="small mono" type="number" min="0" step="1" value="${esc(S.block.nthLatest)}" style="width:52px" onchange="S.block.nthLatest=parseInt(this.value)||0"></label><label title="items aired within this window rank last">no-repeat <input class="small mono" type="number" min="0" step="5" value="${esc(S.block.noRepeatMin)}" style="width:56px" onchange="S.block.noRepeatMin=parseFloat(this.value)"> min</label>` : `<input value="${esc(S.block.folder)}" placeholder="folder path, category or tag" style="width:260px" onchange="S.block.folder=this.value"><select class="small" onchange="S.block.order=this.value">${["random", "even", "name", "date", "size", "added"].map(x => `<option ${S.block.order === x ? "selected" : ""}>${x}</option>`).join("")}</select>`}
      <label>min <input class="small mono" type="number" step="0.5" min="0" value="${S.block.minutes}" style="width:56px" onchange="S.block.minutes=parseFloat(this.value)||0"></label>
      <label title="only files newer than this many days">newer than <input class="small mono" type="number" min="0" value="${esc(S.block.newer)}" style="width:56px" oninput="S.block.newer=this.value"> d</label>
      <label title="only files older than this many days">older than <input class="small mono" type="number" min="0" value="${esc(S.block.older)}" style="width:56px" oninput="S.block.older=this.value"> d</label>
      <select class="small" title="age from library date or episode date" onchange="S.block.ageField=this.value">${["added", "episode"].map(x => `<option ${S.block.ageField === x ? "selected" : ""}>${x}</option>`).join("")}</select>
      <select class="small" title="rules: all must hold, or any one" onchange="S.block.match=this.value">${["all", "any"].map(x => `<option ${S.block.match === x ? "selected" : ""}>${x}</option>`).join("")}</select>
      <input value="${esc(S.block.rulesText)}" placeholder="rules: ageDays lt 14; not tag is expired; durationMin lte 30" title="[not] field op value, separated by ; — fields: type category subcategory series tag client title filename path duration durationMin ageDays episodeAgeDays lastPlayedDays playCount height; ops: is is_not contains not_contains starts_with in not_in lt lte gt gte between exists empty" style="width:320px" oninput="S.block.rulesText=this.value">
      <button class="small primary" onclick="pp.blockAdd()">Add to hour</button><button class="small ghost" onclick="S.blockOpen=false;render()">&#10005;</button></div>` : ""}
    ${items.length ? `<table><thead><tr><th>#</th><th>Title</th><th>Est. start</th><th>Added</th><th>Plays</th><th>Type</th><th>Timing</th><th>At</th><th>In</th><th>Out</th><th>Transition</th><th>Dur</th><th></th></tr></thead><tbody>${items.map((it, i) => `<tr ondragover="pp.slotOver(event)" ondragleave="pp.slotLeave(event)" ondrop="pp.slotDrop(event,${i})" oncontextmenu="pp.itemMenu(event,${i})" onclick="S.propIndex=${i}" class="${isBlock(it) ? "blockrow" : ""}${S.propIndex === i ? " selrow" : ""}"><td draggable="true" ondragstart="pp.slotDrag(event,${i})" title="Drag to reorder" style="cursor:grab">⋮⋮ ${i + 1}</td><td>${it.type === "live" ? '<span style="color:var(--live)">&#9679; </span>' : ""}${isBlock(it) ? `<span style="color:var(--accent)" title="late bound: resolved when the hour is built">${esc(blockLabel(it))}</span>${blockPicks(it)}` : esc(it.title || it.path)}${it.isLocked ? ` <span title="locked${it.lockAtSec >= 0 ? " at :" + pad2(Math.floor(it.lockAtSec / 60)) : ""}" style="color:var(--standby)">&#128274;${it.lockAtSec >= 0 ? " :" + pad2(Math.floor(it.lockAtSec / 60)) : ""}${it.lockLabel ? " " + esc(it.lockLabel) : ""}</span>` : ""}</td><td class="mono">${starts[i]}</td><td>${mediaById(it.mediaId)?.addedAtMs ? esc(new Date(mediaById(it.mediaId).addedAtMs).toLocaleDateString()) : "—"}</td><td>${mediaById(it.mediaId) ? Number(mediaById(it.mediaId).playCount || 0) : "—"}</td><td>${badge(isBlock(it) ? "block" : (it.type || "video"), isBlock(it) ? "var(--accent)" : typeColor(it.type))}</td>
      <td><select class="small" onchange="pp.editSet(${i},'timingMode',this.value)">${["soft_start", "hard_start", "fill_until"].map(t => `<option ${(it.timingMode || "soft_start") === t ? "selected" : ""}>${t}</option>`).join("")}</select></td>
      <td><input class="small mono" style="width:60px" value="${it.fixedOffsetSec >= 0 ? it.fixedOffsetSec : ""}" placeholder="s" onchange="pp.editSet(${i},'fixedOffsetSec',this.value)"></td>
      <td><input class="small mono" style="width:56px" value="${it.timeIn || 0}" onchange="pp.editSet(${i},'timeIn',this.value)"></td><td><input class="small mono" style="width:56px" value="${it.timeOut || ""}" placeholder="end" onchange="pp.editSet(${i},'timeOut',this.value)"></td>
      <td><select class="small" onchange="pp.editSet(${i},'transitionType',this.value)">${["cut", "mix", "dip", "wipe_left", "wipe_right", "push_left", "push_up", "squeeze"].map(t => `<option ${(it.transitionType || "cut") === t ? "selected" : ""}>${t}</option>`).join("")}</select></td>
      <td class="mono">${fmtClock(dur(it))}</td><td class="row" style="flex-wrap:nowrap"><button class="ghost small" onclick="pp.editMove(${i},-1)">&#9650;</button><button class="ghost small" onclick="pp.editMove(${i},1)">&#9660;</button><button class="ghost small" onclick="pp.editRemove(${i})">&#10005;</button></td></tr>`).join("")}</tbody></table>` : `<div class="empty" ondragover="pp.slotOver(event)" ondragleave="pp.slotLeave(event)" ondrop="pp.slotDrop(event,0)">Empty hour: drop media here, add from the library (+), a block (&#9889;) or Fill hour.</div>`}${S.propOpen && items[S.propIndex] ? itemProps(items[S.propIndex], S.propIndex) : ""}</div>` : `<div class="panel"><div class="empty">Select an hour in the grid to edit it</div></div>`;
  const resolved = S.resolved && S.selSlot ? `<div class="panel"><h3>Resolved runlist — ${esc(S.resolved.slotId || "")} ${S.resolved.date || ""} <span class="right"><span class="mono">${fmtClock(S.resolved.scheduledSec || 0)} sched · ${fmtClock(S.resolved.filledSec || 0)} fill · ${fmtClock(S.resolved.gapSec || 0)} gap</span><button class="small ghost" onclick="S.resolved=null;render()">&#10005;</button></span></h3>
    <div class="scroll" style="max-height:40vh"><table><thead><tr><th>At</th><th>Title</th><th>Type</th><th>Origin</th><th>Dur</th><th>Note</th></tr></thead><tbody>${(S.resolved.items || []).map(it => `<tr><td class="mono">${fmtTime(it.plannedStartMs, true)}</td><td>${it.hard ? '<span style="color:var(--standby)">&#9670; </span>' : ""}${esc(it.title)}${it.blockLabel ? ` <span class="muted" style="font-size:10px">&#8592; ${esc(it.blockLabel)}</span>` : ""}</td><td>${badge(it.type || "video", typeColor(it.type))}</td><td style="color:var(--t2)">${esc(it.origin)}</td><td class="mono">${fmtClock(it.duration)}</td><td style="color:var(--t3);font-size:10px">${esc(it.note || "")}</td></tr>`).join("")}</tbody></table>
    ${(S.resolved.notes || []).length ? `<div class="body" style="font-size:10px;color:var(--t2)">${S.resolved.notes.map(esc).join("<br>")}</div>` : ""}</div></div>` : "";
  // rules
  const prof = chan(S.active).profile || {}, fl = prof.fill || {};
  const tg = (label, k, on) => `<label><input type="checkbox" ${on ? "checked" : ""} onchange="pp.rule('${k}',this.checked)"> ${label}</label>`;
  const rules = S.rulesOpen ? `<div class="panel"><h3>Scheduling rules — ${esc(prof.name || S.active)}</h3><div class="body row" style="gap:14px">
    ${tg("Fill gaps with IDs", "fillGapsWithIds", fl.fillGapsWithIds !== false)}${tg("Fill with bumpers", "fillGapsWithBumpers", fl.fillGapsWithBumpers !== false)}${tg("Fill with filler / PSA", "fillWithFiller", fl.fillWithFiller !== false)}${tg("ID at top of hour", "idAtTopOfHour", fl.idAtTopOfHour !== false)}${tg("Shuffle ads in hour", "shuffleAdsInHour", fl.shuffleAdsInHour !== false)}${tg("Latest episode for series", "alwaysPlayLatestEpisode", fl.alwaysPlayLatestEpisode !== false)}
    <label><input type="checkbox" ${prof.strict ? "checked" : ""} onchange="pp.ruleProf('strict',this.checked)"> Strict wall-clock</label>
    <label>Max fill (s) <input class="mono" style="width:70px" value="${fl.maxFillSec !== undefined ? fl.maxFillSec : 60}" onchange="pp.rule('maxFillSec',parseFloat(this.value)||60)"></label>
    <label>Emergency type <input style="width:110px" value="${esc(fl.emergencyType || "station_id")}" onchange="pp.rule('emergencyType',this.value)"></label>
    <label title="more media types that gap fill inside an hour may use, e.g. news, weather (the generated spots) when the library has no filler clips">Gap fill may also use <input style="width:150px" placeholder="news, weather" value="${esc((fl.fillExtraTypes || []).join(", "))}" onchange="pp.rule('fillExtraTypes',this.value.split(',').map(s=>s.trim().toLowerCase()).filter(Boolean))"></label></div></div>` : "";
  return `<div class="grid" style="grid-template-columns:320px 1fr"><div>${library}</div><div class="grid">
    <div class="panel"><h3>Weekly schedule <span class="right">${channelPicker()} <button class="small" onclick="pp.weekShift(-1)">&lsaquo;</button><button class="small" onclick="pp.weekToday()">Today</button><button class="small" onclick="pp.weekShift(1)">&rsaquo;</button> <span style="color:var(--accent)">${S.weekStart.toLocaleDateString()} – ${new Date(S.weekStart.getTime() + 6 * 86400000).toLocaleDateString()}</span> <button class="small ghost${S.rulesOpen ? " active" : ""}" onclick="S.rulesOpen=!S.rulesOpen;render()">Rules</button><button class="small info" onclick="pp.loadHour(${jsq(S.active)})">Load hour now</button></span></h3><div class="scroll" style="max-height:48vh">${g}</div></div>
    ${rules}${editor}${resolved}</div></div>`;
}
function viewReports() {
  const s = S.reportSummary, rows = S.reportRows;
  const stats = [["EVENTS", rows.length], ["STARTS", s.events || 0], ["ON TIME", s.onTime || 0], ["LATE", s.late || 0], ["FAILED", s.failed || 0], ["SKIPPED", s.skipped || 0], ["FILL", s.fill || 0], ["ON TIME %", (s.onTimePct !== undefined ? Number(s.onTimePct).toFixed(0) : "100") + "%"]];
  return `<div class="panel"><h3>As-played report <span class="right">${channelPicker()} <button class="small" onclick="pp.reportShift(-1)">&lsaquo;</button><span style="color:var(--accent)">${S.reportDay.toDateString()}</span><button class="small" onclick="pp.reportShift(1)">&rsaquo;</button> <button class="small" onclick="pp.reportCsv()">Download CSV</button></span></h3>
    <div class="body grid cols4">${stats.map(([l, n]) => `<div class="stat"><div class="n">${esc(n)}</div><div class="l">${l}</div></div>`).join("")}</div>
    <div class="scroll"><table><thead><tr><th>Planned</th><th>Actual</th><th>Var</th><th>Event</th><th>Status</th><th>Title</th><th>Type</th><th>Origin</th><th>Dur</th><th>TC</th></tr></thead><tbody>
    ${rows.length ? rows.map(r => { const v = (r.varianceMs || 0) / 1000; return `<tr><td class="mono">${r.plannedStartMs ? fmtTime(r.plannedStartMs, true) : "—"}</td><td class="mono">${r.actualStartMs ? fmtTime(r.actualStartMs, true) : "—"}</td><td class="mono" style="color:${Math.abs(v) > 1 ? "var(--standby)" : "var(--t2)"}">${r.eventType === "START" ? (v > 0 ? "+" : "") + v.toFixed(2) : ""}</td><td style="color:${r.eventType === "START" ? "var(--ready)" : r.eventType === "FAILED" ? "var(--danger)" : "var(--t1)"}"><b>${esc(r.eventType)}</b></td><td>${badge(r.status || "", r.status === "ON_TIME" ? "var(--ready)" : r.status === "LATE_START" ? "var(--standby)" : r.status === "FAILED" ? "var(--danger)" : "var(--t2)")}</td><td>${esc(r.title)}</td><td style="color:${typeColor(r.type)}">${esc(r.type)}</td><td style="color:var(--t2)">${esc(r.origin)}</td><td class="mono">${r.actualDurationMs ? fmtClock(r.actualDurationMs / 1000) : ""}</td><td class="mono" style="color:var(--t3)">${esc(r.timecode || "")}</td></tr>`; }).join("") : `<tr><td colspan="10" class="empty">No as-run events for this day</td></tr>`}</tbody></table></div></div>`;
}
function viewCg() {
  const t = tele(S.active), layers = t.layers || [], c = chan(S.active);
  return `<div class="grid cols2">
    <div class="panel"><h3>CG quick actions <span class="right">${channelPicker()}</span></h3><div class="body grid" style="gap:10px">
      <label class="f">Title <input id="cgTitle" value="${esc(S.cgTitle)}" oninput="S.cgTitle=this.value" placeholder="Lower third title / bug text"></label>
      <label class="f">Subtitle <input id="cgSub" value="${esc(S.cgSubtitle)}" oninput="S.cgSubtitle=this.value"></label>
      <label class="f">Ticker text <input id="cgTicker" value="${esc(S.cgTicker)}" oninput="S.cgTicker=this.value"></label>
      <div class="row"><button class="success" onclick="pp.cgShow('lower_third')">Lower third</button><button class="ghost" onclick="pp.cgHide('lower_third')">hide</button>
        <button class="success" onclick="pp.cgShow('bug')">Bug</button><button class="ghost" onclick="pp.cgHide('bug')">hide</button>
        <button class="success" onclick="pp.cgShow('ticker')">Ticker</button><button class="ghost" onclick="pp.cgHide('ticker')">hide</button>
        <button class="success" onclick="pp.cgShow('clock')">Clock</button><button class="ghost" onclick="pp.cgHide('legal_id')">hide</button></div>
      <div class="row"><button class="danger" onclick="pp.cgShow('emergency')">Emergency slate</button><button class="ghost" onclick="pp.cgHide('emergency')">clear slate</button><span class="fill"></span><button class="warn" onclick="pp.programSlate()">Program to slate</button><button class="danger" onclick="pp.cgClear()">Clear all layers</button></div>
    </div></div>
    <div class="panel"><h3>Program ${esc(S.active)} <span class="right">${badge(t.state || "OFFLINE", stateColor(t.state), t.state === "ON_AIR")}</span></h3>
      <div class="mon" style="background:#000;aspect-ratio:16/9">${c.process && c.process.connected ? `<img style="width:100%;height:100%;object-fit:contain;display:block" src="/preview/${esc(S.active)}.mjpeg${tokenQ()}">` : ""}</div></div>
    <div class="panel" style="grid-column:1/-1"><h3>Layers on air (${layers.length})</h3><div class="body">${layers.length ? `<ul class="layers">${layers.map(l => `<li><span class="mono" style="color:var(--t2);width:26px">${l.id}</span><b style="width:100px">${esc(l.role || "layer")}</b><span class="fill" style="color:var(--t1)">${esc(l.source ? l.source.type + (l.source.kind ? "/" + l.source.kind : "") + (l.source.title ? " “" + l.source.title + "”" : "") : "")}</span>${badge(l.state, l.state === "shown" ? "var(--ready)" : l.state === "hidden" ? "var(--t3)" : "var(--cued)")}<button class="small" onclick="pp.cgLayer(${l.id},'${l.state}')">${l.state === "hidden" ? "Show" : "Hide"}</button><button class="ghost small" onclick="pp.cgRemove(${l.id})">&#10005;</button></li>`).join("")}</ul>` : `<div class="empty">No CG layers</div>`}</div></div></div>`;
}
function viewOutputs() {
  return `<div class="panel" style="margin-bottom:12px"><h3>Streaming outputs</h3>${viewStreams()}</div><div class="grid">${S.channels.map(c => { const t = tele(c.profile.id), outs = t.outputs || []; return `<div class="panel"><h3>${esc(c.profile.name)} outputs <span class="right">${outs.filter(o => o.connected).length} / ${(c.profile.outputs || []).length} up · GPU ${t.compositor && t.compositor.gpu ? "D3D11" : "CPU"} · tick ${t.telemetry ? Number(t.telemetry.tickMsLast).toFixed(1) + " ms, late " + t.telemetry.lateTicks : "—"}</span></h3>
    <table><thead><tr><th></th><th>Name</th><th>Kind</th><th>Statistics</th></tr></thead><tbody>${outs.length ? outs.map(o => `<tr><td><span class="dot${o.kind === "encoder" ? "" : o.connected ? "" : " off"}" style="${o.kind === "encoder" ? "background:var(--accent2)" : ""}"></span></td><td><b>${esc(o.kind === "encoder" ? "ENCODER " + o.profile : o.name || o.kind)}</b></td><td style="color:var(--t2)">${esc(o.kind)}</td><td class="mono" style="color:var(--t1)">${o.kind === "encoder" ? `in ${o.framesIn} enc ${o.encoded} drop ${o.dropped} ${Number(o.avgEncodeMs || 0).toFixed(1)} ms q ${o.queue}` : `${o.framesOut !== undefined ? "frames " + o.framesOut : "pkts " + o.packetsOut} drop ${o.dropped}${o.bytesOut ? " " + (o.bytesOut / 1048576).toFixed(1) + " MB" : ""}${o.fifoLoadPct !== undefined ? " fifo " + Number(o.fifoLoadPct).toFixed(1) + "%" : ""}${o.underruns !== undefined ? " underruns " + o.underruns : ""}${o.error ? " " + esc(o.error) : ""}`}</td></tr>`).join("") : `<tr><td colspan="4" class="empty">Engine offline or no outputs</td></tr>`}</tbody></table></div>`; }).join("")}</div>`;
}
function viewLogs() {
  return `<div class="grid cols2"><div class="panel"><h3>Alerts (${S.alerts.length})</h3><div class="body scroll">${S.alerts.length ? S.alerts.map(a => `<div class="alert ${esc(a.level)}"><span class="mono" style="color:var(--t2)">${fmtTime(a.utcMs, true)}</span> ${badge(a.level || "info", a.level === "error" || a.level === "critical" ? "var(--danger)" : a.level === "warning" ? "var(--warn)" : "var(--accent)")} <span style="color:var(--t2)">${esc(a.channelId || "")}</span> ${esc(a.message)}</div>`).join("") : `<div class="empty">No alerts since this page opened</div>`}</div></div>
    <div class="panel"><h3>As-run feed (live)</h3><div class="scroll"><table><thead><tr><th>Time</th><th>Ch</th><th>Event</th><th>Title</th><th>Status</th><th>Var</th></tr></thead><tbody>${S.asrun.length ? S.asrun.map(r => `<tr><td class="mono">${fmtTime(r.actualStartMs || r.plannedStartMs, true)}</td><td>${esc(r.channelId)}</td><td style="color:${r.eventType === "START" ? "var(--ready)" : r.eventType === "FAILED" ? "var(--danger)" : "var(--t1)"}"><b>${esc(r.eventType)}</b></td><td>${esc(r.title)}</td><td>${esc(r.status || "")}</td><td class="mono">${r.eventType === "START" && r.varianceMs !== undefined ? ((r.varianceMs > 0 ? "+" : "") + (r.varianceMs / 1000).toFixed(2) + " s") : ""}</td></tr>`).join("") : `<tr><td colspan="6" class="empty">Waiting for events…</td></tr>`}</tbody></table></div></div></div>`;
}
function render() {
  if (S.view === "nowplaying" && NP.interacting) return;
  const v = S.view; const el = $("#view");
  const html = viewSuites(v) || (v === "nowplaying" ? viewNowPlaying() : v === "schedule" ? viewSchedule() : v === "reports" ? viewReports() + viewReportsExt() : v === "spots" ? viewSpots() : v === "flight" ? viewFlight() : v === "remote" ? viewRemote() : v === "programming" ? viewProgramming() : v === "overlays" ? viewOverlays() : v === "cg" ? viewCg() + viewCgHtml() : v === "ingest" || v === "library" ? viewLibrary() : v === "mux" ? viewMux() : v === "outputs" ? viewOutputs() : v === "logs" ? viewLogs() : v === "inputs" ? viewInputs() : v === "reporter" ? viewReporter() : v === "mesh" ? viewMesh() : viewChannels());
  // Telemetry re-renders the page about once a second. Never lose what the operator is typing: an open <select> defers the
  // render until it closes; the focused text field is found again after the rebuild (by id, else by position + placeholder)
  // and gets back its caret, and its typed-but-uncommitted value when it was edited since its last change event.
  const ae = document.activeElement;
  if (ae && el.contains(ae) && ae.tagName === "SELECT") { if (!render._wait) { render._wait = true; const go = () => { render._wait = false; renderSoon(); }; ae.addEventListener("change", go, { once: true }); ae.addEventListener("blur", go, { once: true }); } return; }
  let keep = null;
  if (ae && el.contains(ae) && (ae.tagName === "TEXTAREA" || (ae.tagName === "INPUT" && !["checkbox", "radio", "button", "submit", "range", "file"].includes(ae.type)))) {
    const all = [...el.querySelectorAll("input,textarea")];
    keep = { idx: all.indexOf(ae), id: ae.id, ph: ae.getAttribute("placeholder") || "", name: ae.name || "", value: ae.value, s: ae.selectionStart, e: ae.selectionEnd, dirty: typingDirty.has(ae) };
  }
  // Keep MJPEG <img> elements alive across renders (re-creating them restarts the stream).
  const focused = document.activeElement && document.activeElement.id;
  const imgs = {}; el.querySelectorAll("img[src*='/preview/'], iframe#cgPrev").forEach(i => imgs[i.id || i.getAttribute("src")] = i);
  el.innerHTML = html;
  el.querySelectorAll("img[src*='/preview/'], iframe#cgPrev").forEach(i => { const old = imgs[i.id || i.getAttribute("src")]; if (old && (old.id || old.getAttribute("src") === i.getAttribute("src"))) { if (old.id && old.getAttribute("src") !== i.getAttribute("src")) old.src = i.getAttribute("src"); i.replaceWith(old); } });
  if (keep) {
    let f = keep.id ? document.getElementById(keep.id) : null;
    if (!f) { const all = [...el.querySelectorAll("input,textarea")]; f = all[keep.idx] || null; if (f && ((f.getAttribute("placeholder") || "") !== keep.ph || (f.name || "") !== keep.name)) f = (keep.ph && all.find(x => (x.getAttribute("placeholder") || "") === keep.ph)) || null; }
    if (f) { if (keep.dirty && f.value !== keep.value) { f.value = keep.value; typingDirty.add(f); } f.focus(); try { f.setSelectionRange(keep.s, keep.e); } catch {} }
  } else if (focused) { const f = document.getElementById(focused); if (f) { f.focus(); if (f.setSelectionRange) try { f.setSelectionRange(f.value.length, f.value.length); } catch {} } }
}
// Fields typed into since their last change event (their value is not yet in module state).
const typingDirty = new WeakSet();
document.addEventListener("input", e => { if (e.target && (e.target.tagName === "INPUT" || e.target.tagName === "TEXTAREA")) typingDirty.add(e.target); }, true);
document.addEventListener("change", e => { if (e.target) typingDirty.delete(e.target); }, true);
window.S = S;
// Inline on*="..." handlers only see globals, and this file is a module: hand over the module-level functions they call by bare name.
// (app.js: render, renderSoon; library.js: loadLibrary, loadIngest; integrations.js: onEnterIntegration.)
window.render = render; window.renderSoon = renderSoon; window.onEnterIntegration = onEnterIntegration;

// ---------------------------------------------------------------------------
// Boot
// ---------------------------------------------------------------------------
document.querySelectorAll("#tabs button").forEach(b => b.classList.toggle("active", b.dataset.view === S.view));
loadSnapshot().then(() => { connectWs(); onEnter(S.view); render(); }).catch(() => { setConn(false); scheduleWs(); render(); });
