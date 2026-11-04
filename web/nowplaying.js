/* PLAY PLAY web dashboard — "Now Playing": the desktop On Air panel (Play_Play_Native/qml/views/OnAirView.qml) on the web.
   PGM / PVW monitors, the eight-key input wall (the same ui.inputMap setting the desktop InputMap saves, so both UIs agree),
   CUT / AUTO, the control deck (on air, countdown, next, transport), the graphics strip (layers on air, CG slots, quick
   templates with Text 1-3), the hour rundown, a status line, the OBS scenes card (libobs.js), the Reporter feed card (reporter.js)
   and the Facebook Live box (shared with the Outputs tab through facebooklive.js). Data: one GET /api/v1/nowplaying/{id} a second (the Supervisor lane bundle; docs/nowplaying.md), with a
   client-side fallback that assembles the same shape from the ordinary routes when the bundle route is not installed.
   Pictures: JPEG snapshots polled at 1 Hz (/preview/<ch>.jpg for program, /preview/<ch>:in<N>.jpg for every input tile),
   never an MJPEG stream per tile; the input tiles exist only while this page leases the engine's input monitors
   (POST /channels/{id}/monitor every 15 s, released on leave), exactly like the desktop page. */
import { FB, viewFacebook, loadFacebook, loadFacebookPresets, setFacebookState } from "./facebooklive.js";
import { libobsStrip, libobsStripEnter, libobsStripLeave } from "./libobs.js";
import { reporterStrip, reporterStripEnter, reporterRefresh } from "./reporter.js";

const esc = s => String(s ?? "").replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
const jsq = v => esc(JSON.stringify(String(v ?? "")));
const pad2 = n => (n < 10 ? "0" : "") + n;
const fmtClock = s => { s = Math.max(0, Math.floor(s || 0)); const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), x = s % 60; return (h ? h + ":" + pad2(m) : m) + ":" + pad2(x); };
const fmtTime = ms => ms ? new Date(ms).toLocaleTimeString([], { hour: "numeric", minute: "2-digit", second: "2-digit" }) : "—";
const badge = (text, color, solid) => `<span class="badge${solid ? " solid" : ""}" style="${solid ? "background:" + color : "color:" + color + ";border-color:" + color + ";background:color-mix(in srgb," + color + " 16%,transparent)"}">${esc(text)}</span>`;
const stateColor = s => ({ ON_AIR: "var(--onair)", PREROLL: "var(--cued)", PAUSED: "var(--warn)", HOLD: "var(--standby)", IDLE: "var(--standby)", ERROR: "var(--danger)", OFFLINE: "var(--danger)" }[s] || "var(--t2)");
const typeColor = t => ({ ad: "var(--ad)", commercial: "var(--ad)", promo: "var(--accent2)", station_id: "var(--gfx)", bumper: "var(--bumper)", live: "var(--live)", cg: "var(--gfx)", image: "var(--gfx)", filler: "var(--preroll)", psa: "var(--preroll)", gap: "var(--t3)" }[String(t || "").toLowerCase()] || "var(--video)");
const kbps = k => !(k > 0) ? "—" : k >= 1000 ? (k / 1000).toFixed(k >= 10000 ? 1 : 2) + " Mb/s" : Math.round(k) + " kb/s";
const bits = bps => { const b = Math.max(0, (bps || 0) * 8); return b >= 1e9 ? (b / 1e9).toFixed(2) + " Gb/s" : b >= 1e6 ? (b / 1e6).toFixed(1) + " Mb/s" : b >= 1e3 ? (b / 1e3).toFixed(0) + " kb/s" : b.toFixed(0) + " b/s"; };
const tokenQ = () => { const t = pp.S.token; return t ? "&token=" + encodeURIComponent(t) : ""; };
const TRANS = [["mix", "MIX"], ["dip", "DIP"], ["wipe_left", "WIPE"], ["push_left", "PUSH"], ["squeeze", "SQZ"]];
const DURS = [0.25, 0.5, 1, 2];
const QUICK = [["starnews_lower_third", "STAR NEWS"], ["starnews_bug", "SN BUG"], ["starnews_headline", "SN HEADLINE"], ["lower_third_modern", "LOWER 3RD"], ["banner_breaking_news", "BREAKING"], ["bug_live_indicator", "BUG"], ["ticker_news", "TICKER"], ["fullscreen_title_card", "TITLE"]];
const COUNT = 8;
const layoutDefaults = [{id:"monitors",name:"Program & preview",width:8},{id:"deck",name:"Control deck",width:4},{id:"switcher",name:"Inputs & switching",width:8},{id:"facebook",name:"Facebook Live",width:4},{id:"obs",name:"OBS scenes",width:6},{id:"reporter",name:"Reporter feed",width:6},{id:"operators",name:"Audio & PTZ",width:8},{id:"graphics",name:"Graphics",width:4},{id:"status",name:"Status",width:8},{id:"rundown",name:"Rundown",width:4}];
function loadLayout() {
  try { const saved = JSON.parse(localStorage.getItem("pp.onair.layout") || "[]"); if (Array.isArray(saved)) { const seen = new Set(); const items = saved.filter(x => x && layoutDefaults.some(d => d.id === x.id) && !seen.has(x.id) && seen.add(x.id)).map(x => { const d = layoutDefaults.find(v => v.id === x.id); return {...d, width: Math.max(3, Math.min(12, Math.round(Number(x.width) || d.width)))}; }); return items.concat(layoutDefaults.filter(d => !seen.has(d.id)).map(d => ({...d}))); } } catch (_) {}
  return layoutDefaults.map(d => ({...d}));
}
function saveLayout() { try { localStorage.setItem("pp.onair.layout", JSON.stringify(NP.layout)); } catch (_) { pp.toast("Could not save the layout", "err"); } }

export const NP = {
  ch: "", b: null, err: "", tick: 0, pvw: -1, timer: null, leaseTimer: null, leaseCh: "", lastAt: 0, polling: false,
  slots: [], trans: { type: "mix", durationSec: 0.5 }, mapLoaded: false,       // the shared map, as last read from the bundle (or /settings/ui.inputMap)
  cat: null, pick: -1, pickQ: "",                                              // mappable inputs (library live entries, NDI on the network, cameras) and the open picker
  templates: null, t1: "", t2: "", t3: "", fallback: false, keysOn: false, skipped: {},      // skipped: key -> why the Supervisor would not open a monitor for it
  mixerOpen: false, ptzOpen: false, meters: null, cameras: [], camera: "", speed: 0.5, interacting: false,
  layout: loadLayout(), editLayout: false, dragging: "",
  shows: [], selectedShow: "", rundownLibrary: false, rundownSearch: "",
};
window.NP = NP;

// ---------------------------------------------------------------------------------------------------
// Data: the bundle, or the same shape assembled here when the lane route is missing
// ---------------------------------------------------------------------------------------------------
const quiet = p => pp.GET(p).catch(() => null);
async function loadShows() { const s = await quiet('/shows'); if (!s) return; NP.shows = s.items || []; if (!NP.selectedShow) NP.selectedShow = s.active || ''; if (pp.S.view === 'nowplaying') pp.render(); }
const norm = p => String(p || "").toLowerCase().replace(/\/+$/, "");
/// The desktop InputMap's enginePathOf(): what the engine opens for a library live source, in resolveTake() order.
const enginePathOf = src => !src ? "" : src.url ? String(src.url) : src.ndiName ? "ndi://" + src.ndiName : src.name ? "ndi://" + src.name : src.port > 0 ? "sdi://" + src.port : "";
const kindOf = (src, path) => String(path).startsWith("live://camera/") ? "camera" : src && src.type === "ndi" ? "ndi" : src && (src.type === "sdi_in" || src.type === "sdi") ? "sdi" : "stream";
export function assembleClient(ch, parts) {
  const { channel = {}, state = {}, runlist = {}, overlays = {}, cgslots = {}, stats = {}, facebook = {}, map = {} } = parts;
  const profile = channel.profile || {}, process = channel.process || {}, executor = channel.executor || {};
  const cur = runlist.current || {}, nxt = runlist.next || {};
  const find = id => id ? (cur.items || []).concat(nxt.items || []).find(it => it.id === id) || null : null;
  const onItem = find(state.itemId), nxItem = find(state.nextItemId);
  const mon = {}; (state.monitors || []).forEach(m => mon[m.key] = m);
  const onPath = norm(state.path); let onAirIndex = -1;
  const saved = map.slots || [];
  const slots = []; for (let i = 0; i < COUNT; i++) {
    const s = saved[i] && typeof saved[i] === "object" ? saved[i] : null; const key = "in" + (i + 1);
    const o = { index: i, key, mapped: !!s };
    if (s) { Object.assign(o, { kind: s.kind, title: s.title, path: s.path, mediaId: s.mediaId || "", enginePath: s.enginePath || "", sub: s.sub || "" });
      const ep = norm(s.enginePath), lp = norm(s.path); o.onAir = !!onPath && ((!!ep && ep === onPath) || (!!lp && lp === onPath)); if (o.onAir && onAirIndex < 0) onAirIndex = i;
      o.snapshotUrl = "/preview/" + ch + ":" + key + ".jpg"; if (mon[key]) o.monitor = mon[key]; }
    slots.push(o);
  }
  const mine = (stats.channels || []).find(c => c.id === ch) || {};
  return {
    atMs: Date.now(), channelId: ch, ok: true, errors: parts.errors || [],
    channel: { id: ch, name: profile.name || ch, callLetters: profile.callLetters, width: profile.width, height: profile.height, frameRate: profile.frameRate, hidden: !!(profile.extra || {}).hidden, engineConnected: !!process.connected, engineState: state.engineState || channel.engineState || "OFFLINE", armed: !!executor.armed, lastPushReason: executor.lastPushReason, found: !!profile.id },
    state: Object.assign({}, state, { state: state.state || state.engineState || channel.engineState || "OFFLINE" }),
    onAir: Object.assign({ itemId: state.itemId, title: state.title, path: state.path, position: state.position, duration: state.duration, remaining: state.remaining, timecode: state.timecode, live: String(state.path || "").startsWith("live://") || (onItem && onItem.type === "live") }, onItem ? { type: onItem.type, plannedStartMs: onItem.plannedStartMs, plannedEndMs: onItem.plannedEndMs, mediaId: onItem.mediaId, note: onItem.note } : {}),
    next: Object.assign({ itemId: state.nextItemId, title: state.nextTitle, ready: !!state.nextReady }, nxItem ? { type: nxItem.type, plannedStartMs: nxItem.plannedStartMs, duration: nxItem.duration } : {}),
    runlist: { current: cur, next: nxt },
    inputs: { count: COUNT, slots, onAirIndex, transition: map.transition && map.transition.type ? map.transition : { type: "mix", durationSec: 0.5 }, mapLoaded: !!map.slots },
    cg: { layers: overlays.layers || [], engineLayers: state.layers || [], item: overlays.item || {}, slots: cgslots.slots || [], emergency: !!overlays.emergency },
    audio: ((state.compositor || {}).audio) || {}, outputs: state.outputs || [], stats, channelStats: mine, previewUrl: "/preview/" + ch + ".jpg", facebook,
  };
}
async function clientBundle(ch) {
  const e = encodeURIComponent(ch);
  const [channel, state, runlist, overlays, cgslots, stats, facebook, mapR] = await Promise.all([quiet("/channels/" + e), quiet("/channels/" + e + "/state"), quiet("/channels/" + e + "/runlist"), quiet("/overlays/" + e), quiet("/overlays/cgslots"), quiet("/system/stats"), quiet("/live/facebook"), quiet("/settings/ui.inputMap")]);
  let map = {}; try { map = mapR && mapR.value ? (typeof mapR.value === "string" ? JSON.parse(mapR.value) : mapR.value) : {}; } catch { map = {}; }
  const errors = [["channel", channel], ["state", state], ["runlist", runlist], ["overlays", overlays], ["cgslots", cgslots], ["stats", stats], ["facebook", facebook]].filter(([, v]) => !v).map(([k]) => k + ": unavailable");
  return assembleClient(ch, { channel: channel || {}, state: state || {}, runlist: runlist || {}, overlays: overlays || {}, cgslots: cgslots || {}, stats: stats || {}, facebook: facebook || {}, map, errors });
}
async function poll() {
  const ch = NP.ch; if (!ch || NP.polling) return; NP.polling = true;
  try {
    let b = null;
    if (!NP.fallback) {
      try { b = await pp.api("GET", "/nowplaying/" + encodeURIComponent(ch)); }
      catch (e) { if (String(e.message || e).includes("404") || /not found|no such route/i.test(String(e.message || e))) { NP.fallback = true; } else { NP.err = "bundle: " + (e.message || e); } }
    }
    if (!b && NP.fallback) b = await clientBundle(ch);
    if (b && b.channelId === NP.ch) {
      NP.b = b; NP.err = b.errors && b.errors.length ? "partial: " + b.errors.join(", ") : ""; NP.lastAt = Date.now(); NP.tick++; if (NP.tick % 10 === 0) reporterRefresh();
      const inp = b.inputs || {}; if (inp.mapLoaded) { NP.slots = (inp.slots || []).map(s => s.mapped ? { kind: s.kind, title: s.title, path: s.path, mediaId: s.mediaId || "", enginePath: s.enginePath || "", sub: s.sub || "" } : null); NP.trans = inp.transition || NP.trans; NP.mapLoaded = true; }
      else if (!NP.mapLoaded) { NP.slots = Array(COUNT).fill(null); NP.mapLoaded = true; lease(); }
      if (b.facebook) setFacebookState(b.facebook);
      if (NP.pvw >= 0 && !NP.slots[NP.pvw]) NP.pvw = -1;
      if (NP.mixerOpen) { const meters = await quiet("/channels/" + encodeURIComponent(ch) + "/meters"); if (NP.ch === ch) NP.meters = meters && Array.isArray(meters.strips) ? meters : null; }
      if (!NP.interacting) pp.render();
    }
  } finally { NP.polling = false; }
}
// Input monitors: lease every mapped input (the PVW one at full quality, warm for the take) and boost the PGM picture, like the desktop page.
function engineUp() { return !!(NP.b && NP.b.channel && NP.b.channel.engineConnected); }
function release() { if (!NP.leaseCh) return; pp.POST("/channels/" + encodeURIComponent(NP.leaseCh) + "/monitor", { inputs: [] }).catch(() => {}); NP.leaseCh = ""; }
function lease() {
  const ch = NP.ch, want = pp.S.view === "nowplaying" && NP.mapLoaded && engineUp();
  if (NP.leaseCh && (NP.leaseCh !== ch || !want)) release();
  if (!want) return;
  const inputs = NP.slots.map((e, i) => e ? Object.assign({ key: "in" + (i + 1), path: e.path, title: e.title, quality: NP.pvw === i ? "high" : "low" }, e.mediaId ? { mediaId: e.mediaId } : {}) : null).filter(Boolean);
  pp.POST("/channels/" + encodeURIComponent(ch) + "/monitor", { inputs, leaseSec: 45 }).then(r => { if (NP.ch !== ch) return; const sk = {}; ((r && r.skipped) || []).forEach(s => { if (s && s.key) sk[s.key] = String(s.why || "not shown"); }); NP.skipped = sk; }).catch(() => {});
  pp.POST("/channels/" + encodeURIComponent(ch) + "/cg", { action: "preview", boostSec: 40 }).catch(() => {});
  NP.leaseCh = ch;
}
function saveMap() { return pp.PUT("/settings/ui.inputMap", { value: { slots: NP.slots, transition: NP.trans } }).catch(() => pp.toast("Could not save the input map", "err")); }
async function loadCatalog() {
  const [lib, inp] = await Promise.all([quiet("/media?type=live"), quiet("/inputs")]);
  const out = [], seen = {};
  for (const m of ((lib || {}).media || [])) { const path = m.path || m.filePath || "", ep = enginePathOf(m.liveSource); if (ep) seen[ep.toLowerCase()] = true; out.push({ kind: kindOf(m.liveSource, path), title: m.title || path, path, mediaId: m.id, enginePath: ep, group: "Library", sub: ep }); }
  for (const n of ((inp || {}).ndi || [])) { const np = "ndi://" + n.name; if (seen[np.toLowerCase()]) continue; out.push({ kind: "ndi", title: n.name, path: "live://ndi/" + n.name, mediaId: "", enginePath: np, group: "NDI on the network", sub: n.url || "" }); }
  for (const c of ((inp || {}).cameras || [])) { const uri = c.streamUri; if (!uri || seen[String(uri).toLowerCase()]) continue; out.push({ kind: "camera", title: c.name || c.host, path: "live://stream/" + uri, mediaId: "", enginePath: uri, group: "Cameras", sub: c.host || "" }); }
  NP.cat = out; pp.render();
}

export function onEnterNowPlaying(v) {
  if (NP.timer) { clearInterval(NP.timer); NP.timer = null; } if (NP.leaseTimer) { clearInterval(NP.leaseTimer); NP.leaseTimer = null; }
  if (v !== "nowplaying") { if (NP.interacting) pp.npPtzStop(); release(); libobsStripLeave(); return; }
  loadShows();
  NP.ch = pp.S.active; NP.b = null; NP.fallback = false; NP.pvw = -1; NP.mapLoaded = false;
  poll(); NP.timer = setInterval(poll, 1000);
  NP.leaseTimer = setInterval(lease, 15000); setTimeout(lease, 1200);
  if (!NP.templates) quiet("/cg/templates").then(d => { NP.templates = ((d || {}).templates || []).filter(t => t.listed !== false); pp.render(); });
  loadFacebookPresets().then(() => pp.render()); if (!FB.st) loadFacebook();
  libobsStripEnter(NP.ch, true); reporterStripEnter();
  if (!NP.keysOn) { NP.keysOn = true; document.addEventListener("keydown", onKey); window.addEventListener("blur", () => { if (NP.movingCamera) pp.npPtzStop(); else NP.interacting = false; }); }
}
export function onWsNowPlaying(m) { if (m.type !== 'showTemplate') return false; NP.selectedShow=m.id || ''; NP.pvw=-1; NP.mapLoaded=false; if (Array.isArray(m.layout?.web)) { try { localStorage.setItem('pp.onair.layout',JSON.stringify(m.layout.web)); NP.layout=loadLayout(); } catch {} } loadShows(); poll(); return true; }

// ---------------------------------------------------------------------------------------------------
// Keyboard (this page only, no field focused): 1-8 preview, Alt+1-8 hot cut, Ctrl+Enter CUT, Ctrl+Shift+Enter AUTO, Esc clears PVW
// ---------------------------------------------------------------------------------------------------
function onKey(e) {
  if (pp.S.view !== "nowplaying") return;
  const t = e.target; if (t && (t.tagName === "INPUT" || t.tagName === "TEXTAREA" || t.tagName === "SELECT" || t.isContentEditable)) return;
  if (document.querySelector("dialog[open]")) return;
  if (/^[1-8]$/.test(e.key) && !e.ctrlKey && !e.metaKey) { const i = parseInt(e.key) - 1; if (e.altKey) take(i, "cut"); else select(i); e.preventDefault(); }
  else if (e.key === "Enter" && (e.ctrlKey || e.metaKey)) { if (NP.pvw < 0) pp.toast("Nothing on preview: click an input first"); else take(NP.pvw, e.shiftKey ? "auto" : "cut"); e.preventDefault(); }
  else if (e.key === "Escape" && NP.pvw >= 0) { NP.pvw = -1; lease(); pp.render(); }
}
function select(i) { if (!NP.slots[i]) { pp.npPick(i); return; } NP.pvw = NP.pvw === i ? -1 : i; lease(); pp.render(); }
function take(i, how) {
  const e = NP.slots[i]; if (!e) { pp.toast("Input " + (i + 1) + " is not mapped"); return; }
  const ch = NP.ch; if (!ch) return;
  if (!engineUp()) { pp.toast("Engine is offline; take was not sent", "err"); return; }
  const body = { action: "take", path: e.path, title: e.title, duration: 1800 }; if (e.mediaId) body.mediaId = e.mediaId;
  const auto = how === "auto" && NP.trans.type !== "cut"; if (auto) body.transition = { type: NP.trans.type, durationSec: NP.trans.durationSec };
  pp.POST("/channels/" + encodeURIComponent(ch) + "/transport", body).then(() => { pp.toast((auto ? "AUTO " + (TRANS.find(t => t[0] === NP.trans.type) || ["", "MIX"])[1] + "  " : "CUT  ") + e.title + "  →  " + ch.toUpperCase() + " (command sent)", "ok"); NP.pvw = -1; lease(); setTimeout(poll, 300); }).catch(() => {});
}

// ---------------------------------------------------------------------------------------------------
// View
// ---------------------------------------------------------------------------------------------------
const pic = (id, url, cls, alt) => `<img id="${id}" class="np-pic${cls ? " " + cls : ""}" src="${esc(url)}?t=${NP.tick}${tokenQ()}" alt="" onerror="this.classList.add('nopic')" onload="this.classList.remove('nopic')"><div class="np-nopic">${esc(alt || "no picture")}</div>`;
function monitorTile(kind, title, url, sub, tally) {
  return `<div class="np-mon ${kind}${tally ? " tally" : ""}"><div class="np-mon-hd"><b>${esc(kind.toUpperCase())}</b><span>${esc(title || "")}</span><span class="fill"></span><span class="mono">${esc(sub || "")}</span></div>
    <div class="np-frame">${url ? pic("np-" + kind, url, "", kind === "pvw" ? "select an input (keys 1-8)" : "engine offline") : `<div class="np-nopic show">${esc(kind === "pvw" ? "select an input (keys 1-8)" : "engine offline")}</div>`}</div></div>`;
}
const skipOf = i => NP.skipped["in" + (i + 1)] || "";
const skipIsCard = why => /show as a card/.test(why || "");
function inputTile(s, i) {
  const e = NP.slots[i]; const m = (s && s.monitor) || {}; const on = !!(s && s.onAir), pv = NP.pvw === i, skip = e ? skipOf(i) : "";
  const st = !e ? "" : on ? badge("ON AIR", "var(--onair)", true) : skip && !skipIsCard(skip) ? badge("no source", "var(--warn)") : m.state === "open" ? badge(m.warm ? "WARM" : "LIVE", "var(--ready)") : m.state === "opening" ? badge("connecting…", "var(--warn)") : m.state === "failed" ? badge("failed", "var(--danger)") : m.state === "card" ? badge(s.kind || "card", "var(--t2)") : badge(s.kind || "mapped", "var(--t3)");
  return `<div class="np-in${on ? " onair" : pv ? " pvw" : ""}${e ? "" : " empty"}" data-ctx="npInput" data-ctx-id="${i}" title="${e ? esc(e.title + (skip ? " (" + skip + ")" : "") + (m.error ? " — " + m.error : "")) + "\nclick = preview, double-click = take to program, use MAP to change input" : "unmapped: click to map an input"}"
      onclick="pp.npSelect(${i})" ondblclick="pp.npTake(${i},'cut')" oncontextmenu="pp.npInputMenu(event,${i})">
    <div class="np-frame">${e && engineUp() && !skip ? pic("np-in" + (i + 1), "/preview/" + NP.ch + ":in" + (i + 1) + ".jpg", "", m.state === "failed" ? "no signal" : m.state === "card" || m.state === "skipped" ? (e.kind || "source").toUpperCase() : "connecting…") : `<div class="np-nopic show">${e ? (!engineUp() ? "engine offline" : skipIsCard(skip) ? esc((e.kind || "source").toUpperCase()) : "no source") : "+ map"}</div>`}<span class="np-key">${i + 1}</span></div>
    <div class="np-in-cap"><span class="np-in-title">${esc(e ? e.title : "unmapped")}</span>${st}<button class="np-map" type="button" title="Map input ${i + 1}" aria-label="Map input ${i + 1}" onclick="event.stopPropagation();pp.npPick(${i})">MAP</button></div></div>`;
}
function picker() {
  if (NP.pick < 0) return "";
  const q = NP.pickQ.toLowerCase(); const cat = NP.cat;
  const rows = !cat ? `<div class="empty">Loading inputs…</div>` : cat.filter(c => !q || (c.title + " " + c.sub + " " + c.kind + " " + c.group).toLowerCase().includes(q)).map(c => `<div class="np-pick-row" onclick="pp.npMap(${NP.pick},${jsq(c.path)})"><b>${esc(c.title)}</b>${badge(c.kind, c.kind === "ndi" ? "var(--accent2)" : c.kind === "camera" ? "var(--preroll)" : c.kind === "sdi" ? "var(--standby)" : "var(--accent)")}<span class="mono" style="color:var(--t3);font-size:10px;overflow:hidden;text-overflow:ellipsis">${esc(c.sub || c.path)}</span><span style="color:var(--t3);font-size:10px">${esc(c.group)}</span></div>`).join("") || `<div class="empty">Nothing matches</div>`;
  return `<div class="np-pick panel"><h3>Map input ${NP.pick + 1} <span class="right"><input id="npPickQ" placeholder="search" value="${esc(NP.pickQ)}" oninput="NP.pickQ=this.value;renderSoon()" style="width:160px"> ${NP.slots[NP.pick] ? `<button class="danger small" onclick="pp.npClear(${NP.pick})">Unmap</button>` : ""}<button class="ghost small" onclick="pp.npPick(-1)">&#10005;</button></span></h3><div class="np-pick-list">${rows}</div></div>`;
}
function cgStrip(b) {
  const ch = NP.ch; const cg = b.cg || {}; const ov = cg.layers || [], seen = {}; const rows = [];
  for (const l of ov) { if (l.state === "gone") continue; seen[l.layer] = true; const p = l.params || {}; rows.push({ layer: l.layer, role: l.role, name: l.label || l.name || l.template, detail: [p.text1, p.text2, p.text3, p.title, p.subtitle, p.text].filter(Boolean).join(" · "), state: l.state, managed: true }); }
  for (const l of (cg.engineLayers || [])) { if (seen[l.id] || l.state === "hidden" || l.role === "audio") continue; const src = l.source || {}; rows.push({ layer: l.id, role: l.role || "layer", name: src.kind || src.type || "graphic", detail: src.title || src.text || "", state: l.state, managed: false }); }
  rows.sort((a, b2) => a.layer - b2.layer);
  const onAir = rows.map(r => `<div class="np-cg-row"><span class="mono" style="color:var(--t3)">L${esc(r.layer)}</span><b>${esc(r.name || "")}</b><span style="color:var(--t2);overflow:hidden;text-overflow:ellipsis;white-space:nowrap">${esc(r.detail || "")}</span>${badge(r.state || "", r.state === "shown" ? "var(--ready)" : "var(--warn)")}<button class="small" onclick="pp.npHideLayer(${esc(r.layer)})">HIDE</button></div>`).join("") || `<div class="empty" style="padding:8px">No graphics on air</div>`;
  const slotOn = id => ov.some(l => l.state !== "hiding" && l.state !== "gone" && (l.presetId === "cg:" + id || l.ruleId === "cg:" + id));
  const slots = (cg.slots || []).map(s => { const on = slotOn(s.id), empty = !s.template; const where = (s.inputs || []).includes("*") ? "ALL INPUTS" : (s.inputs || []).filter(x => /^in\d+$/.test(x)).map(x => x.slice(2)).join(",");
    return `<button class="np-cgslot${on ? " on" : ""}" ${empty ? "disabled" : ""} title="${esc(s.template ? s.template + ((s.params || {}).text1 ? " · " + s.params.text1 : "") : "empty: store a graphic in it from the Overlays page")}" onclick="pp.npCgSlot(${jsq(s.id)},${on})"><b>${esc(s.name || s.id.toUpperCase())}</b><span>${esc(empty ? "empty" : where ? "IN " + where : "by hand")}</span></button>`; }).join("");
  const tpls = NP.templates || []; const tplOn = id => ov.find(l => l.template === id && l.state !== "hiding" && l.state !== "gone");
  const quick = QUICK.map(([id, label]) => { const has = tpls.some(t => t.id === id), on = !!tplOn(id); return `<button class="np-quick${on ? " on" : ""}" ${has ? "" : "disabled"} title="${has ? (on ? "press again to hide" : "fire with Text 1-3 below") : "template not installed"}" onclick="pp.npQuick(${jsq(id)})">${esc(label)}</button>`; }).join("");
  return `<div class="panel np-cg"><h3>Graphics on air ${rows.length ? badge(rows.length + " live", "var(--onair)", true) : ""}<span class="right"><button class="danger small" ${rows.length ? "" : "disabled"} onclick="pp.npCgClear()">CLEAR ALL</button></span></h3>
    <div class="np-cg-list">${onAir}</div>
    <div class="np-cg-sub">CG SLOTS · click = show / hide</div><div class="np-cgslots">${slots || `<span class="empty" style="padding:4px">no CG slots</span>`}</div>
    <div class="np-cg-sub">QUICK · fire with the text below, press again to hide</div><div class="np-quicks">${quick}</div>
    <div class="np-texts"><input id="np-t1" placeholder="Text 1" value="${esc(NP.t1)}" oninput="NP.t1=this.value"><input id="np-t2" placeholder="Text 2" value="${esc(NP.t2)}" oninput="NP.t2=this.value"><input id="np-t3" placeholder="Text 3" value="${esc(NP.t3)}" oninput="NP.t3=this.value"></div></div>`;
}
function rundownOver(ev) { if (![...ev.dataTransfer.types].includes("application/x-playplay-media")) return; ev.preventDefault(); const r = ev.currentTarget.getBoundingClientRect(); ev.currentTarget.style.boxShadow = ev.clientY < r.top + r.height / 2 ? "inset 0 3px var(--accent)" : "inset 0 -3px var(--accent)"; }
async function rundownDrop(ev, anchorId, channelId) { ev.preventDefault(); ev.stopPropagation(); const r = ev.currentTarget.getBoundingClientRect(); ev.currentTarget.style.boxShadow = ""; const side = ev.clientY < r.top + r.height / 2 ? "before" : "after"; const id = ev.dataTransfer.getData("application/x-playplay-media"); const m = pp.S.media.find(x => x.id === id); if (!m) { pp.toast("Drag a media-library item onto a rundown row", "err"); return; } const item = {mediaId:m.id,title:m.title,path:m.path,type:m.type || "video",duration:m.duration || 0,timingMode:"soft_start",transitionType:"cut"}; if (m.liveSource) { item.liveSource=m.liveSource; item.timingMode="fill_until"; item.duration=m.duration || 1800; } try { await pp.POST("/channels/" + (channelId || NP.ch) + "/rundown/insert", {anchorId,side,items:[item]}); pp.toast("Inserted into the rundown", "ok"); poll(); } catch(e) { pp.toast("Could not insert: " + (e.message || e), "err"); } }
function rundown(b) {
  const rl = b.runlist || {}; const cur = (rl.current || {}).items || [], nx = (rl.next || {}).items || []; const st = b.state || {}; const now = Date.now();
  const row = (it, nextHour) => { const on = st.itemId === it.id, next = st.nextItemId === it.id; const pct = on && st.duration > 0 ? Math.min(100, 100 * (st.position || 0) / st.duration) : 0;
    return `<li ondragover="pp.npRundownOver(event)" ondragleave="event.currentTarget.style.boxShadow=''" ondrop="pp.npRundownDrop(event,${jsq(it.id)})" class="${on ? "onair" : next ? "next" : it.plannedEndMs && it.plannedEndMs < now && !nextHour ? "past" : ""}" oncontextmenu="pp.rundownMenu&&pp.rundownMenu(event,${jsq(NP.ch)},${jsq(it.id)},${jsq(it.mediaId || it.assetId || "")},${Number(((nextHour ? rl.next : rl.current) || {}).hourStartMs) || 0})">${on ? `<i class="np-prog" style="width:${pct}%"></i>` : ""}<span class="mono">${fmtTime(it.plannedStartMs)}</span><span style="overflow:hidden;text-overflow:ellipsis;white-space:nowrap">${on ? '<span style="color:var(--onair)">&#9679; </span>' : next ? '<span style="color:var(--cued)">&#9654; </span>' : ""}${it.hard ? '<span style="color:var(--standby)">&#9670; </span>' : ""}${esc(it.title || it.path)}${((pp.S.media || []).find(m => m.id === it.mediaId)) ? ` <small style="color:var(--t3)"> · Added ${esc(((pp.S.media || []).find(m => m.id === it.mediaId)).addedAtMs ? new Date(((pp.S.media || []).find(m => m.id === it.mediaId)).addedAtMs).toLocaleDateString() : "—")} · ${Number(((pp.S.media || []).find(m => m.id === it.mediaId)).playCount || 0)} plays</small>` : ""}${it.note ? ` <span style="color:var(--t3)">${esc(it.note)}</span>` : ""}</span>${badge(it.type || "video", typeColor(it.type))}<span class="mono" style="text-align:right;color:${on ? "var(--onair)" : "inherit"}">${on ? "-" + fmtClock(st.remaining) : fmtClock(it.duration)}</span></li>`; };
  const items = cur.map(it => row(it, false)).concat(nx.length ? [`<li class="np-hourbreak"><span></span><span>NEXT HOUR</span><span></span><span></span></li>`].concat(nx.map(it => row(it, true))) : []);
  const q = NP.rundownSearch.toLowerCase(); const media = (pp.S.media || []).filter(m => !m.archived && (!q || (m.title || "").toLowerCase().includes(q))).slice(0, 60);
  const tray = NP.rundownLibrary ? `<div style="padding:8px;border-bottom:1px solid var(--b0)"><input placeholder="Search media to drag" value="${esc(NP.rundownSearch)}" onchange="NP.rundownSearch=this.value;pp.render()" style="width:100%"><div style="max-height:150px;overflow:auto">${media.map(m => `<div draggable="true" ondragstart="pp.mediaDrag(event,${jsq(m.id)})" style="padding:5px;cursor:grab">⋮⋮ ${esc(m.title || m.path)} <small>${fmtClock(m.duration)}</small></div>`).join("") || "No matching media"}</div></div>` : "";
  return `<div class="panel np-rund"><h3>Hour rundown <span class="right">${esc((b.channel || {}).lastPushReason || "")} <button class="ghost small" onclick="NP.rundownLibrary=!NP.rundownLibrary;pp.render()">${NP.rundownLibrary ? "Hide media" : "Drag media"}</button> <button class="ghost small" onclick="pp.loadHour(${jsq(NP.ch)})">LOAD HOUR</button></span></h3>${tray}${items.length ? `<ul class="rund np-rund-list">${items.join("")}</ul>` : `<div class="empty">No items pushed — schedule the hour or arm the channel</div>`}</div>`;
}
function statusLine(b) {
  const s = b.stats || {}, c = b.channelStats || {}, cpu = s.cpu || {}, mem = s.memory || {}, net = s.net || {};
  const slow = c.nominalFps > 0 && c.fps < c.nominalFps * 0.97; const lvl = p => p >= 90 ? "bad" : p >= 75 ? "warn" : "";
  const outs = (b.outputs || []).map(o => `<span title="${esc(o.name)}" class="np-out ${o.state === "running" || o.connected ? "ok" : "bad"}">${esc(o.name || o.kind || "out")}</span>`).join("");
  return `<div class="np-status mono"><span class="k">${esc(String(NP.ch).toUpperCase())}</span><span class="${slow ? "warn" : ""}">${c.fps !== undefined ? Number(c.fps).toFixed(2) + " fps" : "—"}</span><span class="k">OUT</span><span>${kbps(c.bitrateKbps)}</span>${c.outputCount > 0 && c.outputsUp < c.outputCount ? `<span class="warn">▲${c.outputsUp}/${c.outputCount}</span>` : ""}${c.drops > 0 ? `<span class="bad">DROP ${Math.round(c.drops)}</span>` : ""}<span class="np-outs">${outs}</span><span class="fill"></span>
    <span class="k">CPU</span><span class="${lvl(cpu.systemPct || 0)}">${Math.round(cpu.systemPct || 0)}%</span><span class="mu">eng ${(cpu.enginesPct ?? 0).toFixed(1)}%</span><span class="k">MEM</span><span class="${lvl(mem.usedPct || 0)}">${mem.totalMB ? (mem.usedMB / 1024).toFixed(1) + " / " + (mem.totalMB / 1024).toFixed(1) + " GB" : "—"}</span><span class="k">NET</span><span class="ok">↓ ${bits(net.inBps)}</span><span class="acc">↑ ${bits(net.outBps)}</span>
    <span class="mu" title="${esc(NP.err)}">${NP.fallback ? "· no bundle route, assembling client-side" : ""}${NP.err ? " · " + esc(NP.err.slice(0, 60)) : ""}${NP.lastAt && Date.now() - NP.lastAt > 4000 ? " · STALE" : ""}</span></div>`;
}
function deck(b) {
  const st = b.state || {}, s = st.state || "OFFLINE", ch = b.channel || {}, on = b.onAir || {}, nx = b.next || {}, up = !!ch.engineConnected, armed = !!ch.armed, id = NP.ch;
  const audio = b.audio || {}; const lufs = audio.momentaryLufs !== undefined ? audio.momentaryLufs : -70; const pos = Math.max(0, Math.min(1, (lufs + 60) / 60));
  const drift = on.plannedEndMs && s === "ON_AIR" ? Math.round((Date.now() + (st.remaining || 0) * 1000 - on.plannedEndMs) / 1000) : 0;
  return `<div class="panel np-deck${s === "ON_AIR" ? " onair" : ""}">
    <div class="np-deck-hd">${badge(s === "ON_AIR" ? "ON AIR" : s, stateColor(s), s === "ON_AIR")}${armed ? badge("armed", "var(--ready)") : badge("disarmed", "var(--t3)")}${on.live ? badge("LIVE", "var(--live)") : ""}<span class="fill"></span><span class="mono" style="color:var(--t3);font-size:10px">${esc((ch.width || "") + "x" + (ch.height || "") + " @" + (ch.frameRate || ""))}</span></div>
    <div class="np-title">${esc(on.title || st.title || "No event loaded")}</div>
    <div class="np-count${s === "ON_AIR" && st.remaining < 10 ? " crit" : ""}">${s === "ON_AIR" ? "-" + fmtClock(st.remaining) : "--:--"}</div>
    <div class="np-deck-row"><span class="k">NEXT</span><span class="v">${esc(nx.title || "No event loaded")}</span><span class="mono" style="color:var(--t3)">${nx.ready ? "cued" : ""}${nx.plannedStartMs ? " " + fmtTime(nx.plannedStartMs) : ""}</span></div>
    <div class="np-deck-row"><span class="k">TC</span><span class="mono v" style="color:var(--accent)">${esc(st.timecode || "")}</span><span class="mono" style="color:${Math.abs(drift) > 5 ? "var(--warn)" : "var(--t3)"}">${drift ? "drift " + (drift > 0 ? "+" : "") + drift + " s" : ""}</span></div>
    <div class="np-deck-row"><span class="k">AUDIO</span><span class="v"><div class="meter"><div class="bar" style="width:${pos * 100}%"></div><div class="tgt" style="left:60%"></div></div></span><span class="mono">${lufs > -69 ? lufs.toFixed(1) + " LUFS" : "—"}</span></div>
    <div class="np-tr">
      <button class="success big" ${up ? "" : "disabled"} onclick="pp.${s === "ON_AIR" ? "pause" : "play"}(${jsq(id)})">${s === "ON_AIR" ? "&#8214; PAUSE" : "&#9654; PLAY"}</button>
      <button class="big" ${up ? "" : "disabled"} onclick="pp.skip(${jsq(id)})">NEXT &#9197;</button>
      <button ${up ? "" : "disabled"} onclick="pp.stop(${jsq(id)})">&#9632; STOP</button>
      <button ${up ? "" : "disabled"} onclick="pp.hold(${jsq(id)})">HOLD</button>
      <button onclick="pp.snap(${jsq(id)})">SNAP</button>
      <button class="warn" onclick="pp.arm(${jsq(id)},${!armed})">${armed ? "DISARM" : "ARM"}</button>
      <button onclick="pp.loadHour(${jsq(id)})">LOAD HOUR</button>
      <button class="ghost" onclick="pp.restart(${jsq(id)})" title="Restart the engine process">&#8635;</button></div></div>`;
}
function recordingControls(b) {
  const r = (b.state || b.engine || {}).recording || (pp.S.telemetry[NP.ch] || {}).recording || {};
  return `<div class="panel"><h3>RECORDING</h3><div class="body row"><button class="${r.active ? 'danger' : 'primary'} small" ${!engineUp() || r.finalizing ? 'disabled' : ''} onclick="pp.npRecord(${!r.active})">${r.active ? 'STOP RECORDING' : r.finalizing ? 'FINALIZING…' : 'RECORD PROGRAM + LAYERS'}</button><button class="small" onclick="pp.npRecompose()">Recompose…</button><span style="color:${r.error ? 'var(--danger)' : 'var(--t2)'}">${esc(r.error || ((r.active ? 'REC · ' : r.complete ? 'SAVED · ' : '') + (r.directory || 'Program MKV + editable layer archive')))}</span>${NP.recordJob ? `<span>${esc('Export: '+NP.recordJob.state+' '+(NP.recordJob.error || NP.recordJob.output || ''))}</span>` : ''}</div></div>`;
}
const link = (view, text) => `<a class="ghost small btn" href="#${view}" onclick="pp.nav('${view}');return false">${esc(text)} &rarr;</a>`;
function obsPanel() { return `<div class="panel np-obs"><h3>OBS scenes <span class="right">${link("libobs", "scene sets")}</span></h3><div class="body">${libobsStrip(NP.ch, true)}</div></div>`; }
function reporterPanel() { return `<div class="panel np-rf-panel"><h3>Reporter feed <span class="right">${link("reporter", "reporter page")}</span></h3><div class="body">${reporterStrip(NP.ch)}</div></div>`; }
export function viewNowPlaying() {
  const S = pp.S; if (NP.ch !== S.active) { NP.ch = S.active; NP.b = null; NP.meters = null; NP.skipped = {}; NP.pvw = -1; NP.mapLoaded = false; if (NP.timer) { poll(); libobsStripEnter(NP.ch, true); } }
  const chans = S.channels.filter(c => !(c.profile.extra && c.profile.extra.hidden));
  const switcher = `<div class="np-chans">${chans.map(c => { const t = S.telemetry[c.profile.id] || {}; const st = t.state || c.engineState || "OFFLINE"; return `<button class="np-chan${c.profile.id === NP.ch ? " active" : ""}" onclick="pp.setActive(${jsq(c.profile.id)})"><i class="dot" style="background:${stateColor(st)}"></i>${esc(c.profile.name)}</button>`; }).join("") || `<span class="empty">${S.snapshot.supervisor ? "No channels configured" : "Connecting to the Supervisor…"}</span>`}<span class="fill"></span><a class="ghost small btn" href="#channels" onclick="pp.nav('channels');return false" title="the channel cards">channel cards &rarr;</a></div>`;
  const b = NP.b; if (!b) return `<div class="np">${switcher}<div class="panel"><div class="body empty">${NP.ch ? "Loading " + esc(NP.ch) + "…" : "Pick a channel"}</div></div></div>`;
  const up = engineUp(); const pv = NP.pvw >= 0 ? NP.slots[NP.pvw] : null; const on = b.onAir || {};
  const pgm = monitorTile("pgm", (b.channel || {}).name, up ? b.previewUrl : "", on.title ? (on.live ? "LIVE · " : "") + on.title : "", (b.state || {}).state === "ON_AIR");
  const pvw = monitorTile("pvw", pv ? pv.title : "", pv && up && !skipOf(NP.pvw) ? "/preview/" + NP.ch + ":in" + (NP.pvw + 1) + ".jpg" : "", pv ? "input " + (NP.pvw + 1) : "", !!pv);
  const bus = `<div class="np-bus">${(b.inputs && b.inputs.slots || []).slice(0, COUNT).map((s, i) => inputTile(s, i)).join("")}</div>`;
  const programBus = `<div class="row" style="gap:5px;margin:8px 0"><b style="color:var(--onair);min-width:58px">PGM CUT</b>${Array.from({length:COUNT},(_,i)=>`<button class="small ${(b.inputs?.slots || [])[i]?.onAir ? 'danger' : ''}" style="flex:1;overflow:hidden;text-overflow:ellipsis" ${NP.slots[i] && up ? '' : 'disabled'} onclick="pp.npTake(${i},'cut')" title="Cut directly to program">${i+1} ${esc(NP.slots[i]?.title || 'empty')}</button>`).join('')}</div>`;
  const trans = `<div class="np-trans"><button class="danger big" ${pv && up ? "" : "disabled"} onclick="pp.npCut()" title="Ctrl+Enter">CUT</button><button class="info big" ${pv && up ? "" : "disabled"} onclick="pp.npAuto()" title="Ctrl+Shift+Enter">AUTO</button>
    <span class="np-trans-opts">${TRANS.map(([t, l]) => `<button class="small${NP.trans.type === t ? " active" : ""}" onclick="pp.npTrans(${jsq(t)})">${l}</button>`).join("")}<span class="sep"></span>${DURS.map(d => `<button class="small${Number(NP.trans.durationSec) === d ? " active" : ""}" onclick="pp.npTrans(undefined,${d})">${d} s</button>`).join("")}</span>
    <span class="fill"></span><span style="color:var(--t3);font-size:10px">1-8 preview · Alt+N hot cut · Ctrl+Enter CUT · Ctrl+Shift+Enter AUTO · Esc clear</span></div>`;
  const fbChans = chans.map(c => ({ channelId: c.profile.id, channelName: c.profile.name }));
  const panels = {monitors:`<div class="np-mons">${pgm}${pvw}</div>`, switcher:bus+programBus+trans+picker(), status:statusLine(b), operators:operatorControls(), deck:deck(b), facebook:`<div class="np-fbwrap">${viewFacebook(fbChans, { big: true })}</div>`, graphics:cgStrip(b), rundown:rundown(b), obs:obsPanel(), reporter:reporterPanel()};
  panels.deck += recordingControls(b);
  const tiles = NP.layout.map((p,i) => `<section class="np-layout-tile" style="--span:${p.width}" ondragover="event.preventDefault()" ondrop="pp.npLayoutDrop(event,${jsq(p.id)})">${NP.editLayout ? `<div class="np-layout-tools"><span class="np-layout-handle" draggable="true" ondragstart="pp.npLayoutStart(event,${jsq(p.id)})" ondragend="pp.npLayoutEnd()" title="Drag to reorder">⠿ ${esc(p.name)}</span><span class="fill"></span><button class="small" ${i===0?'disabled':''} onclick="pp.npLayoutMove(${jsq(p.id)},-1)" aria-label="Move ${esc(p.name)} up">↑</button><button class="small" ${i===NP.layout.length-1?'disabled':''} onclick="pp.npLayoutMove(${jsq(p.id)},1)" aria-label="Move ${esc(p.name)} down">↓</button><label>Width <select aria-label="${esc(p.name)} width" onchange="pp.npLayoutWidth(${jsq(p.id)},this.value)">${[3,4,6,8,9,12].map(n=>`<option value="${n}" ${p.width===n?'selected':''}>${Math.round(n/12*100)}%</option>`).join("")}</select></label></div>` : ''}<div class="np-layout-content">${panels[p.id]}</div></section>`).join("");
  return `<div class="np">${switcher}<div class="row"><b>SHOW</b><select onchange="pp.npShowSelect(this.value)"><option value="">Select a show…</option>${NP.shows.map(s=>`<option value="${esc(s.id)}" ${NP.selectedShow===s.id?'selected':''}>${esc(s.name)}</option>`).join('')}</select><button ${NP.selectedShow?'':'disabled'} onclick="pp.npShowApply()">Load show</button><button onclick="pp.npShowSave(false)">Save current…</button><button ${NP.selectedShow?'':'disabled'} onclick="pp.npShowSave(true)">Update selected</button><button ${NP.selectedShow?'':'disabled'} onclick="pp.npShowDelete()">Delete</button><span class="fill"></span><button onclick="pp.npLayoutEdit()">${NP.editLayout ? 'DONE ARRANGING' : 'ARRANGE LAYOUT'}</button>${NP.editLayout ? '<span>Drag a panel or use the arrows; choose its width.</span><button class="ghost small" onclick="pp.npLayoutReset()">Reset layout</button>' : ''}</div><div class="np-layout-grid${NP.editLayout ? ' editing' : ''}">${tiles}</div></div>`;
}

function operatorControls() {
  const enabled = engineUp();
  const m = NP.meters;
  const strips = m && Array.isArray(m.strips) ? m.strips : [];
  const fader = (label, id, gain, mute, solo, layer, peaks) => { const peak = peaks && peaks.length ? Math.max(...peaks.map(Number).filter(Number.isFinite)) : -100; const level = Number.isFinite(peak) ? Math.max(0, Math.min(100, (peak + 60) / 60 * 100)) : 0; return `<div class="np-mixstrip"><b>${esc(label)}</b><div class="meter" title="Peak ${Number.isFinite(peak) ? peak.toFixed(1) : '—'} dBFS"><div class="bar" style="width:${level}%"></div></div><span class="mono">${Number(gain || 0).toFixed(1)} dB</span><input aria-label="${esc(label)} gain" type="range" min="-60" max="12" step="0.5" value="${Math.max(-60, Math.min(12, Number(gain || 0)))}" ${enabled ? "" : "disabled"} onpointerdown="NP.interacting=true" onpointercancel="NP.interacting=false" onchange="NP.interacting=false;pp.npGain(${jsq(id)},${Number(layer || 0)},this.value)" onpointerup="NP.interacting=false"><div class="row">${id === "master" ? "" : `<button class="small ${mute ? "danger" : ""}" ${enabled ? "" : "disabled"} onclick="pp.npMute(${jsq(id)},${Number(layer || 0)},${!mute})">${mute ? "UNMUTE" : "MUTE"}</button><button class="small ${solo ? "warn" : ""}" ${enabled ? "" : "disabled"} onclick="pp.npSolo(${jsq(solo ? "" : id)})">${solo ? "SOLO ON" : "SOLO"}</button>`}</div></div>`; };
  const mixer = NP.mixerOpen ? `<div class="body"><div class="np-mixrow">${m ? strips.map(s => fader(s.name || s.id, s.id, s.gainDb, s.mute, s.solo, s.layer, s.peaks)).join("") + fader("MASTER", "master", m.masterGainDb, false, false, 0, m.channelPeaksDbfs) : `<span class="empty">${enabled ? "Mixer data unavailable. Update the Supervisor to enable these controls." : "Channel engine is offline."}</span>`}</div>${m && m.solo ? `<button class="warn small" onclick="pp.npSolo('')">Clear solo</button>` : ""}</div>` : "";
  const cam = NP.cameras.find(c => c.id === NP.camera);
  const btn = (x,y,z,label) => `<button class="ptz" ${cam && cam.hasPtz ? "" : "disabled"} onpointerdown="this.setPointerCapture(event.pointerId);pp.npPtzMove(${x},${y},${z})" onpointerup="pp.npPtzStop()" onpointercancel="pp.npPtzStop()" onlostpointercapture="pp.npPtzStop()">${label}</button>`;
  const ptz = NP.ptzOpen ? `<div class="body"><label class="f">Camera <select onchange="NP.camera=this.value;pp.render()"><option value="">Select camera</option>${NP.cameras.map(c => `<option value="${esc(c.id)}" ${c.id === NP.camera ? "selected" : ""}>${esc(c.name || c.host)}</option>`).join("")}</select></label><div class="row" style="align-items:start;margin-top:8px"><div class="ptzpad">${btn(-1,1,0,"↖")}${btn(0,1,0,"↑")}${btn(1,1,0,"↗")}${btn(-1,0,0,"←")}<button class="ptz home" ${cam && cam.hasPtz ? "" : "disabled"} onclick="pp.npPtz({action:'home'})">⌂</button>${btn(1,0,0,"→")}${btn(-1,-1,0,"↙")}${btn(0,-1,0,"↓")}${btn(1,-1,0,"↘")}<div class="row" style="grid-column:1/-1">${btn(0,0,1,"Zoom +")}${btn(0,0,-1,"Zoom −")}</div></div><div><label class="f">Speed <input type="range" min="0.1" max="1" step="0.05" value="${NP.speed}" onpointerdown="NP.interacting=true" onpointerup="NP.interacting=false" onpointercancel="NP.interacting=false" oninput="NP.speed=Number(this.value)"></label><div class="row">${cam ? (cam.presets || []).map(p => `<button class="small" onclick="pp.npPtz({action:'preset',preset:${jsq(p.token)}})">${esc(p.name || p.token)}</button>`).join("") : ""}</div><a href="#inputs" onclick="pp.nav('inputs');return false">Camera setup &amp; presets →</a></div></div></div>` : "";
  return `<div class="np-operator"><div class="row"><button onclick="pp.npMixer()">${NP.mixerOpen ? "▾" : "▸"} AUDIO MIXER</button><button onclick="pp.npCameras()">${NP.ptzOpen ? "▾" : "▸"} PTZ</button></div>${NP.mixerOpen ? `<div class="panel"><h3>Audio mixer · main ${m && m.solo ? ' · SOLO ON' : ''}</h3>${mixer}</div>` : ""}${NP.ptzOpen ? `<div class="panel"><h3>Camera control</h3>${ptz}</div>` : ""}</div>`;
}

// ---------------------------------------------------------------------------------------------------
// pp.* handlers (inline on*="" attributes only see globals)
// ---------------------------------------------------------------------------------------------------
export const ppExtNowPlaying = {
  npRundownOver: rundownOver, npRundownDrop: rundownDrop,
  npShowSelect(id) { NP.selectedShow=id; pp.render(); },
  async npShowSave(update) { const existing=NP.shows.find(s=>s.id===NP.selectedShow); const name=prompt('Show name',update && existing ? existing.name : ''); if (!name || !name.trim()) return; try { const body={name:name.trim(),layout:{web:NP.layout}}; if(update && existing) body.id=existing.id; const s=await pp.POST('/shows',body); NP.selectedShow=s.id; await loadShows(); pp.toast('Saved show '+s.name,'ok'); } catch(e) { pp.toast(e.message || 'Could not save show','err'); } },
  async npShowApply() { if(!NP.selectedShow) return; try { const s=await pp.POST('/shows/'+encodeURIComponent(NP.selectedShow)+'/apply',{}); if(Array.isArray(s.layout?.web)) { localStorage.setItem('pp.onair.layout',JSON.stringify(s.layout.web)); NP.layout=loadLayout(); } NP.pvw=-1; NP.mapLoaded=false; await poll(); pp.toast('Loaded show '+s.name,'ok'); } catch(e) { pp.toast(e.message || 'Could not load show','err'); } },
  async npShowDelete() { if(!NP.selectedShow) return; try { await pp.api('DELETE','/shows/'+encodeURIComponent(NP.selectedShow)); NP.selectedShow=''; await loadShows(); pp.toast('Show template deleted','ok'); } catch(e) { pp.toast(e.message || 'Could not delete show','err'); } },
  async npRecompose() {
    const rec = (pp.S.telemetry[NP.ch] || {}).recording || {};
    const session = prompt('Layered recording session folder on this node', rec.directory || ''); if (!session) return;
    const output = prompt('New output MKV path on this node', session.replace(/[\\/]$/,'') + '/recomposed-' + Date.now() + '.mkv'); if (!output) return;
    const hide = prompt('Layer IDs to hide, separated by commas (optional)', ''); if (hide === null) return;
    const omitLayers = hide.split(',').map(s=>parseInt(s.trim().replace(/^L/i,''))).filter(Number.isFinite);
    try {
      NP.recordJob = await pp.POST('/recordings/recompose', {session, output, edits:{omitLayers}}); pp.render();
      const timer = setInterval(async()=>{ try { NP.recordJob = await pp.GET('/recordings/jobs/'+NP.recordJob.id); pp.render(); if(NP.recordJob.state !== 'running') { clearInterval(timer); pp.toast(NP.recordJob.state === 'complete' ? 'Recording exported' : NP.recordJob.error || 'Export failed', NP.recordJob.state === 'complete'?'ok':'err'); } } catch(e) { clearInterval(timer); pp.toast(e.message,'err'); } }, 1000);
    } catch(e) { pp.toast(e.message,'err'); }
  },
  async npRecord(start) { try { await pp.POST('/channels/' + encodeURIComponent(NP.ch) + '/transport', {action: 'record', params: {op: start ? 'start' : 'stop'}}); pp.toast('Recording command sent; status confirms capture', 'ok'); } catch(e) { pp.toast(e.message || 'Recording command failed', 'err'); } },
  npLayoutEdit() { NP.editLayout = !NP.editLayout; NP.interacting = false; pp.render(); },
  npLayoutReset() { NP.layout = layoutDefaults.map(d => ({...d})); saveLayout(); pp.render(); },
  npLayoutMove(id, dir) { const i=NP.layout.findIndex(p=>p.id===id), j=i+dir; if(i<0 || j<0 || j>=NP.layout.length) return; const a=NP.layout.slice(); [a[i],a[j]]=[a[j],a[i]]; NP.layout=a; saveLayout(); pp.render(); },
  npLayoutWidth(id, width) { const n=Number(width); if(![3,4,6,8,9,12].includes(n)) return; NP.layout=NP.layout.map(p=>p.id===id ? {...p,width:n} : p); saveLayout(); pp.render(); },
  npLayoutStart(ev,id) { NP.dragging=id; NP.interacting=true; ev.dataTransfer.effectAllowed='move'; ev.dataTransfer.setData('text/plain',id); },
  npLayoutEnd() { NP.dragging=''; NP.interacting=false; pp.render(); },
  npLayoutDrop(ev,id) { ev.preventDefault(); const from=NP.layout.findIndex(p=>p.id===NP.dragging), to=NP.layout.findIndex(p=>p.id===id); NP.interacting=false; NP.dragging=''; if(from<0 || to<0 || from===to) { pp.render(); return; } const a=NP.layout.slice(); a.splice(to,0,a.splice(from,1)[0]); NP.layout=a; saveLayout(); pp.render(); },
  npMixer() { NP.mixerOpen = !NP.mixerOpen; if (NP.mixerOpen) { const ch = NP.ch; quiet("/channels/" + encodeURIComponent(ch) + "/meters").then(m => { if (NP.ch === ch) NP.meters = m && Array.isArray(m.strips) ? m : null; pp.render(); }); } pp.render(); },
  npCameras() { NP.ptzOpen = !NP.ptzOpen; if (NP.ptzOpen) quiet("/inputs").then(d => { NP.cameras = (d || {}).cameras || []; if (!NP.cameras.some(c => c.id === NP.camera)) NP.camera = (NP.cameras[0] || {}).id || ""; pp.render(); }); pp.render(); },
  npGain(id, layer, value) { if (!engineUp() || !NP.meters) return; const gainDb = Math.max(-60, Math.min(12, Number(value))); if (!Number.isFinite(gainDb)) return; const cmd = id === "master" ? {action:"audio",master:{gainDb}} : id === "program" ? {action:"audio",program:{gainDb}} : {action:"update",layer,audioGainDb:gainDb}; pp.cg(NP.ch, cmd).catch(() => {}); },
  npMute(id, layer, mute) { if (!engineUp() || !NP.meters) return; pp.cg(NP.ch, id === "program" ? {action:"audio",program:{mute}} : {action:"update",layer,audioMute:mute}).catch(() => {}); },
  npSolo(id) { if (!engineUp() || !NP.meters) return; pp.cg(NP.ch, {action:"audio",solo:id}).catch(() => {}); },
  npPtz(cmd) { if (!NP.camera) return; pp.POST("/cameras/" + encodeURIComponent(NP.camera) + "/ptz", cmd).catch(() => {}); },
  npPtzMove(x,y,z) { if (!NP.camera) return; NP.interacting = true; NP.movingCamera = NP.camera; pp.npPtz({action:"move",x:x*NP.speed,y:y*NP.speed,z:z*NP.speed,timeoutSec:2}); },
  npPtzStop() { const id = NP.movingCamera; NP.movingCamera = ""; NP.interacting = false; if (id) pp.POST("/cameras/" + encodeURIComponent(id) + "/ptz", {action:"stop"}).catch(() => {}); },
  npSelect(i) { select(i); },
  npTake(i, how) { take(i, how); },
  npCut() { if (NP.pvw < 0) { pp.toast("Nothing on preview: click an input first"); return; } take(NP.pvw, "cut"); },
  npAuto() { if (NP.pvw < 0) { pp.toast("Nothing on preview: click an input first"); return; } take(NP.pvw, "auto"); },
  npTrans(type, sec) { if (type !== undefined && TRANS.some(t => t[0] === type)) NP.trans = Object.assign({}, NP.trans, { type }); if (sec !== undefined && sec > 0) NP.trans = Object.assign({}, NP.trans, { durationSec: sec }); saveMap(); pp.render(); },
  npPick(i) { NP.pick = i; NP.pickQ = ""; if (i >= 0 && !NP.cat) loadCatalog(); pp.render(); if (i >= 0) setTimeout(() => { const q = document.getElementById("npPickQ"); if (q) q.focus(); }, 0); },
  npMap(i, path) { const c = (NP.cat || []).find(x => x.path === path); if (!c) return; NP.slots = NP.slots.slice(); NP.slots[i] = { kind: c.kind, title: c.title, path: c.path, mediaId: c.mediaId || "", enginePath: c.enginePath || "", sub: c.sub || "" }; NP.pick = -1; saveMap().then(() => { pp.toast("Input " + (i + 1) + " → " + c.title, "ok"); lease(); poll(); }); pp.render(); },
  npClear(i) { NP.slots = NP.slots.slice(); NP.slots[i] = null; if (NP.pvw === i) NP.pvw = -1; NP.pick = -1; saveMap().then(() => { lease(); poll(); }); pp.render(); },
  npInputMenu(ev, i) { ev.preventDefault(); const e = NP.slots[i]; const items = [{ label: e ? "Map input " + (i + 1) + " to…" : "Map input " + (i + 1) + "…", fn: () => pp.npPick(i) }];
    if (e) items.push({ label: "Preview (key " + (i + 1) + ")", fn: () => select(i) }, { label: "CUT to program", fn: () => take(i, "cut") }, { label: "AUTO to program", fn: () => take(i, "auto") }, { sep: true }, { label: "Open picture stream", fn: () => window.open("/preview/" + NP.ch + ":in" + (i + 1) + ".mjpeg" + (pp.S.token ? "?token=" + encodeURIComponent(pp.S.token) : "")) }, { sep: true }, { label: "Unmap", danger: true, fn: () => pp.npClear(i) });
    if (window.ppCtx && window.ppCtx.ctxMenu) window.ppCtx.ctxMenu(ev, items); else pp.npPick(i); },
  npHideLayer(layer) { pp.POST("/overlays/" + encodeURIComponent(NP.ch) + "/" + layer + "/hide", {}).then(() => { pp.toast("Layer " + layer + " hidden", "ok"); setTimeout(poll, 400); }).catch(() => {}); },
  npCgClear() { pp.confirmDo("Clear every graphics layer on " + NP.ch + "?", () => pp.cg(NP.ch, { action: "clear" }).then(() => setTimeout(poll, 400))); },
  npCgSlot(id, on) { pp.POST("/overlays/" + encodeURIComponent(NP.ch) + "/cgslots/" + encodeURIComponent(id) + (on ? "/hide" : "/fire"), on ? {} : { toggle: true }).then(() => { pp.toast(id.toUpperCase() + (on ? " off" : " on air"), "ok"); setTimeout(poll, 400); }).catch(() => {}); },
  npQuick(id) {
    const ov = ((NP.b || {}).cg || {}).layers || []; const l = ov.find(x => x.template === id && x.state !== "hiding" && x.state !== "gone");
    if (l) return pp.npHideLayer(l.layer);
    const t = (NP.templates || []).find(x => x.id === id); if (!t) { pp.toast("Template " + id + " is not installed", "err"); return; }
    const params = {}; if (NP.t1) params.text1 = NP.t1; if (NP.t2) params.text2 = NP.t2; if (NP.t3) params.text3 = NP.t3;
    pp.POST("/overlays/" + encodeURIComponent(NP.ch) + "/deploy", { template: id, params, role: t.role || "secondary" }).then(() => { pp.toast(t.name + " on air", "ok"); setTimeout(poll, 500); }).catch(() => {});
  },
};
