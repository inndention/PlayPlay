/* PLAY PLAY web dashboard — Reporter feed: a field reporter goes live from the getstarnews app (RTMP push from the phone)
   into a PlayPlay channel. Create a session (the publish URL and the stream key show ONCE, with copy buttons), watch the
   feed arrive, GO starts the countdown (big digits, driven by WS "reporter" events), the Supervisor takes the feed to
   program at zero; END returns the channel to its schedule. Server: playout/supervisor/reporterfeed.*, routes under
   /api/v1/reporter. Loaded by app.js like the other modules (viewReporter / onEnterReporter / onWsReporter / ppExtReporter). */

// The state is window.RF (not R): web/remote.js already publishes its own state as window.R and the inline handlers below read the global.
const RF = { sessions: [], host: "", mode: "", cfg: null, fresh: null, form: { reporter: "", title: "LIVE", channel: "", countdownSec: 10 }, showEnded: false, busy: false, loaded: false };
window.RF = RF;
const E = s => String(s ?? "").replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
const chans = () => (pp.S.channels || []).map(c => c.profile.id);
const STATUS_COLOR = { idle: "var(--t2)", countdown: "var(--standby)", live: "var(--onair)", ended: "var(--t3)" };
const bdg = (text, color) => `<span class="badge" style="color:${color};border-color:${color};background:color-mix(in srgb,${color} 16%,transparent)">${E(text)}</span>`;
const tm = ms => ms ? new Date(ms).toLocaleTimeString([], { hour: "numeric", minute: "2-digit", second: "2-digit" }) : "—";

if (!document.getElementById("rfStyle")) {
  const st = document.createElement("style"); st.id = "rfStyle";
  st.textContent = `
.rf-card{background:var(--bg2);border:1px solid var(--b0);border-radius:8px;padding:12px 14px;display:grid;gap:8px}
.rf-card.countdown{border-color:var(--standby)}.rf-card.live{border-color:var(--onair);background:rgba(255,45,85,.08)}
.rf-digits{font:700 64px var(--mono);line-height:1;letter-spacing:2px;text-align:center;padding:10px 0;color:var(--standby)}
.rf-digits.live{color:var(--onair);font-size:40px}.rf-digits.idle{color:var(--t3);font-size:28px}
.rf-key{display:grid;grid-template-columns:auto 1fr auto;gap:8px;align-items:center;font-size:12px}
.rf-key code{background:var(--bg3);padding:4px 8px;border-radius:4px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.rf-once{border:1px solid var(--warn);background:rgba(255,214,10,.08);border-radius:8px;padding:10px 12px;display:grid;gap:8px}
.rf-dot{display:inline-block;width:8px;height:8px;border-radius:50%;background:var(--t3);margin-right:6px}.rf-dot.up{background:var(--ready);box-shadow:0 0 6px var(--ready)}`;
  document.head.appendChild(st);
}

// ---------------------------------------------------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------------------------------------------------
async function load(quiet) {
  const d = await pp.GET("/reporter/sessions?all=" + (RF.showEnded ? 1 : 0)).catch(e => { if (!quiet) pp.toast("Reporter feed: " + (e.message || e), "err"); return null; });
  if (d) { RF.sessions = d.sessions || []; RF.host = d.host || ""; RF.mode = d.mode || ""; RF.loaded = true; }
  if (!RF.cfg) RF.cfg = await pp.GET("/reporter/config").catch(() => null);
  if (!RF.form.channel && chans().length) RF.form.channel = pp.S.active || chans()[0];
  pp.render();
}
function sessionOf(id) { return RF.sessions.find(s => s.id === id); }
async function act(id, what, body) {
  if (RF.busy) return; RF.busy = true;
  try { const s = await pp.POST("/reporter/sessions/" + id + "/" + what, body || {}); if (s && s.status) merge(s); }
  catch (e) { pp.toast(e.message || String(e), "err"); }
  RF.busy = false; load();
}
function merge(s) { const i = RF.sessions.findIndex(x => x.id === s.id); if (i >= 0) RF.sessions[i] = Object.assign({}, RF.sessions[i], s); else RF.sessions.unshift(s); }

export function onEnterReporter(v) { if (v === "reporter") load(); }
/** WS type "reporter": {event, id, session, remainingSec}. The countdown digits repaint from these every second. */
export function onWsReporter(m) {
  if (m.type !== "reporter") return false;
  if (m.session) { if (m.event === "ended" && !RF.showEnded) RF.sessions = RF.sessions.filter(s => s.id !== m.session.id); else merge(m.session); }
  if (m.event === "live") pp.toast("Reporter take accepted: " + ((m.session || {}).reporter || ""), "ok");
  if (pp.S.view === "reporter") pp.render();
  return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// View
// ---------------------------------------------------------------------------------------------------------------------
function digits(s) {
  if (s.status === "countdown") return `<div class="rf-digits">${String(s.remainingSec).padStart(2, "0")}</div>`;
  if (s.status === "taking") return `<div class="rf-digits">TAKING…</div>`;
  if (s.status === "live") return `<div class="rf-digits live">TAKE SENT</div>`;
  if (s.status === "ended") return `<div class="rf-digits idle">ENDED</div>`;
  return `<div class="rf-digits idle">${s.feedUp ? "FEED READY" : "WAITING FOR FEED"}</div>`;
}
function card(s) {
  const btns = s.status === "idle" ? `<button class="primary" onclick="pp.rfGo('${s.id}')" ${s.channel ? "" : "disabled title='no channel'"}>GO (${s.countdownSec} s)</button><button class="warn" onclick="pp.rfTakeNow('${s.id}')">Take now</button>`
    : s.status === "countdown" ? `<button class="warn" onclick="pp.rfTakeNow('${s.id}')">Take now</button><button class="ghost" onclick="pp.rfCancel('${s.id}')">Cancel</button>`
    : s.status === "live" ? `<button class="danger" onclick="pp.rfEnd('${s.id}')">END · back to schedule</button>` : "";
  const del = s.status === "ended" || s.status === "idle" ? `<button class="ghost small" onclick="pp.rfDelete('${s.id}')" title="remove">&#10005;</button>` : "";
  return `<div class="rf-card ${s.status}">
    <div class="row"><b style="font-size:15px">${E(s.reporter)}</b> ${bdg(s.status, STATUS_COLOR[s.status] || "var(--t2)")} <span class="mono" style="color:var(--t2)">${E(s.title)}</span><span class="right" style="margin-left:auto;color:var(--t3);font-size:11px">${E(s.channel || "no channel")} · port ${(RF.cfg ? RF.cfg.rtmpPortBase : 1936) + (s.slot || 0)} · key …${E(s.keyHint)}${del}</span></div>
    ${digits(s)}
    <div class="row" style="font-size:12px"><span><span class="rf-dot${s.feedUp ? " up" : ""}"></span>${s.feedUp ? "phone is publishing" : "no publisher yet"}</span><span style="color:var(--t3)">· created ${tm(s.createdMs)}${s.liveMs ? " · live " + tm(s.liveMs) : ""}${s.ingestRestarts ? " · ingest restarts " + s.ingestRestarts : ""}</span>${s.lastError ? `<span style="color:var(--danger)">${E(s.lastError)}</span>` : ""}</div>
    <div class="row">${btns}</div></div>`;
}
function freshBox() {
  const f = RF.fresh; if (!f) return "";
  const line = (label, val) => `<div class="rf-key"><span style="color:var(--t2)">${label}</span><code id="rf_${label.replace(/\W/g, "")}">${E(val)}</code><button class="small" onclick="pp.rfCopy(${JSON.stringify(val).replace(/"/g, "&quot;")})">Copy</button></div>`;
  return `<div class="rf-once"><div class="row"><b>Session for ${E(f.reporter)} created</b> ${bdg("shown once", "var(--warn)")}<span class="right" style="margin-left:auto"><button class="ghost small" onclick="RF.fresh=null;pp.render()">Dismiss</button></span></div>
    <div style="font-size:11px;color:var(--t2)">Give the reporter the publish URL (the app's "PlayPlay station" target takes the whole URL; any RTMP app takes server + stream key). The key is not stored and will not be shown again: create a new session if it is lost.</div>
    ${line("Publish URL", f.publishUrl)}${line("RTMP server", f.publishUrl.replace(/\/[^/]+$/, ""))}${line("Stream key", f.streamKey)}${f.whipUrl ? line("WHIP URL", f.whipUrl) : ""}
    <div style="font-size:11px;color:var(--t3)">Engine input: <code>${E(f.enginePath)}</code> · listed as "Reporter: ${E(f.reporter)}" on the On Air input map and under Inputs.</div></div>`;
}
export function viewReporter() {
  const f = RF.form, cfg = RF.cfg || {};
  const active = RF.sessions.filter(s => s.status !== "ended"), ended = RF.sessions.filter(s => s.status === "ended");
  return `<div class="grid cols2">
    <div class="panel" style="grid-column:1/-1"><h3>Reporter feed <span class="right">${RF.mode ? "mode " + E(RF.mode) + " · " : ""}publish host ${E(RF.host || "?")} · ports ${cfg.rtmpPortBase || 1936}–${(cfg.rtmpPortBase || 1936) + (cfg.portCount || 8) - 1} <button class="small" onclick="pp.rfReload()">Refresh</button><label style="font-size:11px"><input type="checkbox" ${RF.showEnded ? "checked" : ""} onchange="RF.showEnded=this.checked;pp.rfReload()"> show ended</label></span></h3>
      <div class="body"><div class="row" style="align-items:flex-end">
        <label class="f">Reporter <input id="rfName" value="${E(f.reporter)}" oninput="RF.form.reporter=this.value" placeholder="Jane Doe" style="width:180px"></label>
        <label class="f">Title / lower third <input value="${E(f.title)}" oninput="RF.form.title=this.value" placeholder="LIVE" style="width:220px"></label>
        <label class="f">Channel <select onchange="RF.form.channel=this.value">${chans().map(c => `<option value="${E(c)}" ${c === f.channel ? "selected" : ""}>${E(c)}</option>`).join("")}</select></label>
        <label class="f">Countdown s <input type="number" min="0" max="600" value="${f.countdownSec}" oninput="RF.form.countdownSec=parseInt(this.value)||0" style="width:70px"></label>
        <button class="primary" onclick="pp.rfCreate()" ${RF.busy ? "disabled" : ""}>Create session</button></div>
      <div style="color:var(--t3);font-size:11px;margin-top:6px">A session opens an RTMP listener the phone publishes to right away (so the picture is already there before the cut). GO shows the countdown here and on the phone, then takes the feed to program on the channel; END sends the channel back to its schedule.</div>
      ${freshBox()}</div></div>
    ${active.length ? active.map(card).join("") : `<div class="panel" style="grid-column:1/-1"><div class="empty">${RF.loaded ? "No reporter session. Create one above, then send the reporter the publish URL." : "Loading…"}</div></div>`}
    ${RF.showEnded && ended.length ? `<div class="panel" style="grid-column:1/-1"><h3>Ended (${ended.length})</h3><div class="scroll" style="max-height:30vh"><table><thead><tr><th>Reporter</th><th>Title</th><th>Channel</th><th>Live</th><th>Ended</th><th></th></tr></thead><tbody>${ended.map(s => `<tr><td>${E(s.reporter)}</td><td>${E(s.title)}</td><td>${E(s.channel)}</td><td>${tm(s.liveMs)}</td><td>${tm(s.endedMs)}</td><td><button class="ghost small" onclick="pp.rfDelete('${s.id}')">&#10005;</button></td></tr>`).join("")}</tbody></table></div></div>` : ""}
  </div>`;
}

// ---------------------------------------------------------------------------------------------------------------------
// The compact panel (the Now Playing dashboard embeds it): one line per live session with GO / Take now / Cancel / END and the
// countdown, a one-line "New feed" form, and the one-time publish URL + key. Returns HTML; the host page calls
// reporterStripEnter() when it opens and reporterRefresh() now and then (the WS "reporter" events keep the rows current).
// ---------------------------------------------------------------------------------------------------------------------
const jq = v => E(JSON.stringify(String(v ?? "")));
export function reporterStripEnter() { load(true); }
export function reporterRefresh() { return load(true); }
export function reporterStrip(channelId) {
  const ch = channelId || pp.S.active;
  const live = RF.sessions.filter(s => s.status !== "ended");
  const mark = s => s.status === "countdown" ? `<b class="mono np-rf-count">${String(s.remainingSec).padStart(2, "0")}</b>` : s.status === "taking" ? `<b class="mono np-rf-count">TAKING…</b>` : s.status === "live" ? `<b class="mono np-rf-count live">LIVE</b>` : "";
  const btns = s => s.status === "idle" ? `<button class="primary small" onclick="pp.rfGo(${jq(s.id)})" ${s.channel ? "" : "disabled"} title="countdown ${s.countdownSec} s, then the feed is taken to program">GO ${s.countdownSec} s</button><button class="warn small" onclick="pp.rfTakeNow(${jq(s.id)})">Take now</button>`
    : s.status === "countdown" ? `<button class="warn small" onclick="pp.rfTakeNow(${jq(s.id)})">Take now</button><button class="ghost small" onclick="pp.rfCancel(${jq(s.id)})">Cancel</button>`
    : s.status === "live" ? `<button class="danger small" onclick="pp.rfEnd(${jq(s.id)})" title="return the channel to its schedule">END</button>` : "";
  const rows = live.map(s => `<div class="np-rf-row ${s.status}"><span class="rf-dot${s.feedUp ? " up" : ""}" title="${s.feedUp ? "the phone is publishing" : "no publisher yet"}"></span><b>${E(s.reporter)}</b><span class="np-rf-sub">${E(s.channel || "no channel")} · ${E(s.title)}</span>${bdg(s.status, STATUS_COLOR[s.status] || "var(--t2)")}${mark(s)}<span class="fill"></span>${btns(s)}${s.status === "idle" ? `<button class="ghost small" onclick="pp.rfDelete(${jq(s.id)})" title="remove this session">&#10005;</button>` : ""}</div>`).join("");
  return `<div class="np-rf">${rows || `<div class="empty" style="padding:6px 0">${RF.loaded ? "No reporter session. Name the reporter below, press New feed, and give the getstarnews app the publish URL." : "Loading…"}</div>`}
    <div class="row np-rf-new"><input id="rfsName" placeholder="Reporter name" value="${E(RF.form.reporter)}" oninput="RF.form.reporter=this.value" onkeydown="if(event.key==='Enter')pp.rfsCreate(${jq(ch)})"><button class="primary small" ${RF.busy ? "disabled" : ""} onclick="pp.rfsCreate(${jq(ch)})" title="opens an RTMP listener for ${E(ch)} and shows the publish URL once">New feed &rarr; ${E(ch)}</button></div>${freshBox()}</div>`;
}

export const ppExtReporter = {
  rfReload: () => load(),
  rfsCreate(ch) { RF.form.channel = ch; return pp.rfCreate(); },
  async rfCreate() {
    const f = RF.form; if (!f.reporter.trim()) { pp.toast("Reporter name required", "err"); return; }
    RF.busy = true; pp.render();
    try { const o = await pp.POST("/reporter/sessions", { reporter: f.reporter.trim(), title: f.title, channel: f.channel, countdownSec: f.countdownSec }); RF.fresh = o; merge(o); pp.toast("Session created: give the reporter the URL and key now", "ok"); }
    catch (e) { pp.toast(e.message || String(e), "err"); }
    RF.busy = false; load();
  },
  rfGo: id => act(id, "go"), rfTakeNow: id => act(id, "take-now"), rfCancel: id => act(id, "cancel"),
  rfEnd: id => pp.confirmDo ? pp.confirmDo("End the reporter feed and return the channel to its schedule?", () => act(id, "end")) : act(id, "end"),
  async rfDelete(id) { try { await pp.api("DELETE", "/reporter/sessions/" + id); } catch (e) { pp.toast(e.message || String(e), "err"); } if (RF.fresh && RF.fresh.id === id) RF.fresh = null; load(); },
  rfCopy(text) { (navigator.clipboard ? navigator.clipboard.writeText(text) : Promise.reject()).then(() => pp.toast("Copied", "ok"), () => { window.prompt("Copy:", text); }); },
};
