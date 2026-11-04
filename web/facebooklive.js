/* PLAY PLAY web dashboard — Facebook Live card, shared by the Outputs tab (mux.js viewStreams) and the Now Playing page.
   One state object (FB), one card renderer (viewFacebook) and one set of pp.* handlers (ppExtFacebook), so both pages agree on
   the saved channel / server / quality, whether the key is saved, and the live state. Routes: GET/PUT /api/v1/live/facebook,
   POST /api/v1/live/facebook/start | /stop. The stream key is write-only: the server keeps it and only ever answers
   "key saved …abcd"; this module never shows a saved key, only what the operator is typing (show / hide). */

export const FB = { st: null, d: {}, dirty: false, show: false, presets: [], onChange: null };
window.FB = FB;
const e3 = s => String(s ?? "").replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
const b3 = (text, color) => `<span class="badge" style="color:${color};border-color:${color};background:color-mix(in srgb,${color} 16%,transparent)">${e3(text)}</span>`;
const kb = k => k >= 1000 ? (k / 1000).toFixed(2) + " Mb/s" : Math.round(k || 0) + " kb/s";
const fmtUp = s => { s = Math.floor(Number(s || 0)); return s >= 3600 ? Math.floor(s / 3600) + "h " + Math.floor(s % 3600 / 60) + "m" : s >= 60 ? Math.floor(s / 60) + "m " + (s % 60) + "s" : s + "s"; };
const stateColor = st => st === "running" ? "var(--ready)" : st === "error" ? "var(--danger)" : st === "starting" || st === "pending" ? "var(--warn)" : st === "offline" ? "var(--danger)" : "var(--t3)";

/// Fetches the state; a typed-but-unsaved form is kept. `after` (or FB.onChange) runs once the state is in.
export function loadFacebook(after) {
  return pp.GET("/live/facebook").then(f => { FB.st = f; if (!FB.dirty) FB.d = { channel: f.channel || "", serverUrl: f.serverUrl || "", preset: f.preset || "", streamKey: "" }; (after || FB.onChange || pp.render)(); return f; }).catch(() => {});
}
/// Encoder presets for the Quality picker (loaded once; the Outputs tab shares the same list through mux.js).
export function loadFacebookPresets() {
  if (FB.presets.length) return Promise.resolve(FB.presets);
  return pp.GET("/mux/encoder-presets").then(p => { FB.presets = p.presets || []; return FB.presets; }).catch(() => []);
}
/// Applies a fresh state (e.g. from a Now Playing bundle) without a fetch.
export function setFacebookState(f) { if (!f) return; FB.st = f; if (!FB.dirty) FB.d = { channel: f.channel || "", serverUrl: f.serverUrl || "", preset: f.preset || "", streamKey: "" }; }
const refresh = () => (FB.onChange || pp.render)();

/// The card. chans: [{channelId, channelName}] to pick from; opts.presets overrides FB.presets; opts.big = Now Playing styling (large GO LIVE).
export function viewFacebook(chans, opts = {}) {
  const f = FB.st || {}; const d = FB.d; const st = f.state || "idle"; const live = f.live === true; const presets = opts.presets || FB.presets || [];
  const pre = presets.map(p => p.id).filter(id => /^stream_|h264/.test(id)); if (d.preset && !pre.includes(d.preset)) pre.unshift(d.preset);
  const pill = live ? b3("LIVE", "var(--danger)") : b3(st, stateColor(st));
  const ready = f.hasKey && d.channel && f.engineConnected;
  const big = opts.big ? " big" : "";
  return `<div class="fb${big}" style="${opts.big ? "" : "margin:8px 12px;"}padding:10px 12px;border:1px solid ${live ? "var(--danger)" : "var(--b1)"};border-radius:6px;background:var(--bg2)">
    <div style="display:flex;gap:8px;align-items:center;margin-bottom:8px"><b>Facebook Live</b>${pill}
      ${live ? `<span class="mono" style="font-size:11px;color:var(--t2)">${e3(fmtUp(f.uptimeSec))} · ${kb(f.bitrateKbps)}${f.width ? " · " + e3(f.width) + "×" + e3(f.height) : ""}</span>` : ""}
      ${f.retryInSec !== undefined ? `<span style="color:var(--warn);font-size:11px">retrying in ${e3(f.retryInSec)} s (attempt ${e3(f.attempts)})</span>` : ""}
      <span class="fill"></span>
      ${live ? `<button class="danger${big}" onclick="pp.fbOp('stop')">END LIVE</button>` : `<button class="success${big}" ${ready ? "" : "disabled"} title="${ready ? "start publishing to Facebook" : f.hasKey ? "pick a channel whose engine is running" : "paste the stream key and save first"}" onclick="pp.fbOp('start')">GO LIVE</button>`}</div>
    ${f.lastError ? `<div style="color:var(--danger);font-size:11px;margin-bottom:6px">${e3(f.lastError)}</div>` : ""}
    <div class="row" style="gap:8px;align-items:end;flex-wrap:wrap">
      <label class="f">Channel <select onchange="pp.fbSet('channel',this.value)"><option value="">— pick —</option>${chans.map(c => `<option value="${e3(c.channelId)}" ${c.channelId === d.channel ? "selected" : ""}>${e3(c.channelName || c.channelId)}</option>`).join("")}</select></label>
      <label class="f">Stream key <input id="fbKey" type="${FB.show ? "text" : "password"}" autocomplete="off" spellcheck="false" value="${e3(d.streamKey || "")}" placeholder="${f.hasKey ? "saved " + e3(f.keyHint) + " — type to replace" : "paste the key from Facebook Live Producer"}" style="width:${opts.big ? 260 : 300}px" oninput="pp.fbSet('streamKey',this.value,true)"></label>
      <label style="display:flex;gap:4px;align-items:center;font-size:11px"><input type="checkbox" ${FB.show ? "checked" : ""} onchange="pp.fbShow(this.checked)"> show</label>
      <label class="f">Quality <select onchange="pp.fbSet('preset',this.value)">${pre.map(id => `<option ${id === d.preset ? "selected" : ""}>${e3(id)}</option>`).join("") || `<option>${e3(d.preset || "stream_h264_aac_720p")}</option>`}</select></label>
      <label class="f">Server <input id="fbServer" value="${e3(d.serverUrl || "")}" style="width:${opts.big ? 220 : 300}px" oninput="pp.fbSet('serverUrl',this.value,true)"></label>
      <button class="primary small" ${FB.dirty ? "" : "disabled"} onclick="pp.fbSave()">Save</button>
      ${f.hasKey ? `<button class="ghost small" onclick="pp.fbClear()" title="forget the saved key">Forget key</button>` : ""}
    </div>
    <div style="color:var(--t3);font-size:11px;margin-top:6px">Create the live video in Facebook (Live Producer → Streaming software), copy its <b>stream key</b> here, then press GO LIVE and, once Facebook shows the preview, <b>Go live</b> there too. Keys are single-use per live video and are never shown again by this page.</div></div>`;
}

export const ppExtFacebook = {
  fbSet(k, v, quiet) { FB.d[k] = v; FB.dirty = true; if (!quiet) refresh(); else { document.querySelectorAll(".fb button.primary").forEach(b => b.disabled = false); } },
  fbShow(v) { FB.show = v; refresh(); },
  fbSave() {
    const d = FB.d; const b = { channel: d.channel, serverUrl: d.serverUrl, preset: d.preset }; if (d.streamKey) b.streamKey = d.streamKey;
    pp.PUT("/live/facebook", b).then(f => { FB.st = f; FB.dirty = false; FB.d.streamKey = ""; pp.toast("Facebook Live saved" + (f.hasKey ? " (key " + f.keyHint + ")" : ""), "ok"); loadFacebook(); }).catch(e => pp.toast("Facebook Live: " + e, "err"));
  },
  fbClear() { pp.confirmDo("Forget the saved Facebook stream key?", () => pp.PUT("/live/facebook", { clearKey: true }).then(f => { FB.st = f; pp.toast("Key forgotten", "ok"); loadFacebook(); }).catch(e => pp.toast("Facebook Live: " + e, "err"))); },
  fbOp(op) {
    if (op === "stop") return pp.confirmDo("End the Facebook live stream?", () => pp.POST("/live/facebook/stop").then(() => { pp.toast("Facebook stream stopped", "ok"); setTimeout(loadFacebook, 700); }).catch(e => pp.toast("stop failed: " + e, "err")));
    pp.POST("/live/facebook/start").then(() => { pp.toast("Connecting to Facebook…", "ok"); setTimeout(loadFacebook, 900); }).catch(e => pp.toast("go live failed: " + e, "err"));
  },
};
