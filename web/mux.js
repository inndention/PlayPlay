/* PLAY PLAY web dashboard — ATSC multiplexer console (ported from the Starfile broadcast_gui):
   Services (channel / NDI / ffmpeg input / UDP merge) with per-service encoder presets + advanced
   configuration, Encoders (presets + capability probe), Tables (PSIP summary, XML, overrides, compile,
   live analysis), Output (ASI/UDP/SRT/file + DekTec devices + dry run), Streams (per-channel streaming
   outputs: state, bitrate, start/stop/restart, autoStart, add/remove), Guide, Log. Talks to /api/v1/mux/*
   and /api/v1/channels/{id}/outputs*. `viewStreams()` is exported so app.js can embed it in the Outputs tab. */

import { FB, viewFacebook, loadFacebook, ppExtFacebook } from "./facebooklive.js";

const X = { status: {}, cfg: {}, services: [], alloc: {}, guide: null, log: [], tab: "services", dirty: false,
  presets: [], caps: null, tables: null, ovr: null, ovrDirty: false, dektec: null, dry: null, analysis: null, analyzing: false,
  streams: null, fb: FB, inputs: [], adv: {}, add: { ch: "", type: "rtmp", name: "", url: "rtmp://localhost/live/stream", preset: "stream_h264_aac_720p", kbps: "", enabled: true, autoStart: true }, showHidden: false, file: "", timer: null, encTab: "presets" };
window.X = X;
const e3 = s => String(s ?? "").replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
const mb = b => (Number(b || 0) / 1e6).toFixed(2) + " Mb/s";
const kb = k => k >= 1000 ? (k / 1000).toFixed(2) + " Mb/s" : Math.round(k || 0) + " kb/s";
const b3 = (text, color) => `<span class="badge" style="color:${color};border-color:${color};background:color-mix(in srgb,${color} 16%,transparent)">${e3(text)}</span>`;
const fmtUp = s => { s = Math.floor(Number(s || 0)); return s >= 3600 ? Math.floor(s / 3600) + "h " + Math.floor(s % 3600 / 60) + "m" : s >= 60 ? Math.floor(s / 60) + "m " + (s % 60) + "s" : s + "s"; };
const stateColor = st => st === "running" ? "var(--ready)" : st === "error" ? "var(--danger)" : st === "starting" || st === "pending" ? "var(--warn)" : st === "offline" ? "var(--danger)" : "var(--t3)";
const CODECS = ["libx264", "libx265", "mpeg2video", "h264_qsv", "hevc_qsv", "h264_nvenc", "hevc_nvenc"];
const ACODECS = ["ac3", "aac", "mp2", "eac3"];
const visibleChannels = () => (pp.S.channels || []).filter(c => X.showHidden || !(c.profile.extra || {}).hidden);

async function loadMux() {
  const [st, cfg, sv] = await Promise.all([pp.GET("/mux/status").catch(() => ({})), pp.GET("/mux/config").catch(() => ({})), pp.GET("/mux/services").catch(() => ({}))]);
  X.status = st; X.cfg = cfg; if (!X.dirty) X.services = sv.services || []; X.alloc = sv.allocation || {}; pp.render();
  if (!X.presets.length) pp.GET("/mux/encoder-presets").then(p => { X.presets = p.presets || []; pp.render(); }).catch(() => {});
  if (X.tab === "guide") pp.GET("/mux/guide").then(g => { X.guide = g; pp.render(); }).catch(() => {});
  if (X.tab === "log") pp.GET("/mux/log?n=300").then(l => { X.log = l.lines || []; pp.render(); }).catch(() => {});
  if (X.tab === "services") pp.GET("/inputs").then(i => { X.inputs = i.ndi || i.inputs || []; pp.render(); }).catch(() => {});
  if (X.tab === "encoders" && !X.caps) loadCaps(false);
  if (X.tab === "tables") loadTables();
  if (X.tab === "output" && !X.dektec) pp.GET("/mux/dektec?refresh=0").then(d => { X.dektec = d; pp.render(); }).catch(() => {});
  if (X.tab === "streams") loadStreams();
  if (X.tab === "scte35") loadS35();
}
function loadS35() { pp.GET("/mux/scte35").then(r => { X.s35 = r; pp.render(); }).catch(e => { X.s35 = { error: String(e.message || e) }; pp.render(); }); }
function loadCaps(refresh) { pp.GET("/mux/capabilities" + (refresh ? "?refresh=1" : "")).then(c => { X.caps = c; pp.render(); }).catch(e => pp.toast("capabilities: " + e, "err")); }
function loadTables() { pp.GET("/mux/tables").then(t => { X.tables = t; if (!X.ovrDirty) X.ovr = t.overrides || {}; if (!X.file && t.files && t.files.length) X.file = (t.files.find(f => f.name === "tvct.xml") || t.files[0]).name; pp.render(); }).catch(() => {}); }
function loadStreams() {
  pp.GET("/outputs").then(o => { X.streams = o.channels || []; pp.render(); }).catch(() => {});
  loadFacebook();
}
export function onEnterMux(v) {
  if (X.timer) { clearInterval(X.timer); X.timer = null; }
  if (v === "mux") { loadMux(); X.timer = setInterval(() => { if (X.tab === "streams") loadStreams(); else if (X.tab === "scte35") loadS35(); else if (X.tab === "log") pp.GET("/mux/log?n=300").then(l => { X.log = l.lines || []; pp.render(); }).catch(() => {}); }, 3000); }
  else if (v === "outputs") { loadStreams(); if (!X.presets.length) pp.GET("/mux/encoder-presets").then(p => { X.presets = p.presets || []; pp.render(); }).catch(() => {}); X.timer = setInterval(loadStreams, 3000); }
}
export function onWsMux(m) { if (m.type === "mux") { X.status = m; return true; } return false; }

// ---------------------------------------------------------------------------------------------------
// Services
// ---------------------------------------------------------------------------------------------------
const inp = (i, path, v, w, attrs = "") => `<input value="${e3(v ?? "")}" style="width:${w}px" ${attrs} onchange="pp.muxSet(${i},'${path}',${attrs.includes("type=\"number\"") ? "parseFloat(this.value)" : "this.value"})">`;
const num = (i, path, v, w) => `<input type="number" value="${e3(v ?? "")}" style="width:${w}px" onchange="pp.muxSet(${i},'${path}',parseFloat(this.value))">`;
const sel = (i, path, v, opts) => `<select class="small" onchange="pp.muxSet(${i},'${path}',this.value)">${opts.map(o => `<option value="${e3(o)}" ${String(o) === String(v) ? "selected" : ""}>${e3(o)}</option>`).join("")}</select>`;
const chk = (i, path, v, label) => `<label style="display:inline-flex;gap:4px;align-items:center;text-transform:none"><input type="checkbox" ${v ? "checked" : ""} onchange="pp.muxSet(${i},'${path}',this.checked)">${e3(label)}</label>`;
const f = (label, html) => `<label class="f" style="font-size:10px">${e3(label)}${html}</label>`;
const availCodecs = () => { const has = ((X.caps || {}).ffmpeg || {}).has || null; return has ? CODECS.filter(c => has[c] !== false) : CODECS; };

function svcAdvanced(s, i) {
  const en = s.encoder || {}; const au = en.audio || {}; const ts = en.ts || {}; const sm = s.statmux || {};
  const codecs = availCodecs(); if (!codecs.includes(en.codec || "libx264")) codecs.push(en.codec || "libx264");
  return `<tr><td colspan="10" style="background:var(--bg3);padding:8px 12px"><div class="grid" style="gap:8px;grid-template-columns:repeat(8,minmax(90px,1fr))">
    <div style="grid-column:1/-1;color:var(--t2);font-size:10px;letter-spacing:1px">VIDEO ${en.encoderPreset ? b3("preset " + en.encoderPreset, "var(--accent)") : ""} ${X.caps && ((X.caps.ffmpeg || {}).has || {})[en.codec] === false ? b3(en.codec + " missing in ffmpeg → libx264", "var(--warn)") : ""}</div>
    ${f("Codec", sel(i, "encoder.codec", en.codec || "libx264", codecs))}${f("Width", num(i, "encoder.width", en.width, 70))}${f("Height", num(i, "encoder.height", en.height, 70))}${f("FPS", inp(i, "encoder.fps", en.fps || "30000/1001", 80))}
    ${f("Video kb/s", num(i, "encoder.vidBitrateKbps", en.vidBitrateKbps, 70))}${f("Rate control", sel(i, "encoder.rateControl", en.rateControl || "cbr", ["cbr", "vbr", "cqp"]))}${f("Maxrate kb/s (vbr)", num(i, "encoder.maxrateKbps", en.maxrateKbps ?? 0, 70))}${f("VBV buf kb", num(i, "encoder.bufsizeKbps", en.bufsizeKbps ?? 0, 70))}
    ${f("QP (cqp)", num(i, "encoder.qp", en.qp ?? 23, 50))}${f("GOP frames", num(i, "encoder.gop", en.gop ?? 30, 60))}${f("Keyint min (0=gop)", num(i, "encoder.keyintMin", en.keyintMin ?? 0, 60))}${f("B-frames", num(i, "encoder.bframes", en.bframes ?? 2, 50))}
    ${f("Speed preset", sel(i, "encoder.preset", en.preset || "veryfast", ["ultrafast", "superfast", "veryfast", "faster", "fast", "medium", "slow", "p1", "p2", "p4", "p6"]))}${f("Profile", sel(i, "encoder.profile", en.profile || "high", ["high", "main", "baseline"]))}${f("Level", sel(i, "encoder.level", en.level || "4.1", ["3", "3.1", "3.2", "4", "4.1", "4.2", "5", "5.1"]))}${f("AFD (-1 off)", num(i, "encoder.afd", en.afd ?? -1, 50))}
    <div style="grid-column:span 4;display:flex;gap:12px;align-items:end;padding-bottom:4px">${chk(i, "encoder.interlaced", en.interlaced, "interlaced (1080i/480i)")}${chk(i, "encoder.closedGop", en.closedGop !== false, "closed GOP")}${chk(i, "encoder.scenecut", en.scenecut, "scene-cut")}${chk(i, "encoder.ccPassthrough", en.ccPassthrough !== false, "CEA-708 pass-through (a53cc)")}</div>
    ${f("Extra x264/x265 params", inp(i, "encoder.extra", en.extra || "", 220, 'placeholder="key=val:key=val"'))}
    <div style="grid-column:1/-1;color:var(--t2);font-size:10px;letter-spacing:1px;margin-top:4px">AUDIO <span style="color:var(--t3);letter-spacing:0">(ATSC A/85 loudness target −24 LKFS; dialnorm tells the receiver the dialogue level)</span></div>
    ${f("Codec", sel(i, "encoder.audio.codec", au.codec || en.audCodec || "ac3", ACODECS))}${f("kb/s", num(i, "encoder.audio.bitrateKbps", au.bitrateKbps ?? en.audBitrateKbps ?? 192, 60))}${f("Channels", sel(i, "encoder.audio.channels", au.channels ?? en.audChannels ?? 2, [1, 2, 6]))}${f("Sample rate", sel(i, "encoder.audio.sampleRate", au.sampleRate || 48000, [48000, 44100, 32000]))}
    ${f("Dialnorm (ac3)", num(i, "encoder.audio.dialnorm", au.dialnorm ?? -27, 60))}${f("Mixing level (-1 off)", num(i, "encoder.audio.mixingLevel", au.mixingLevel ?? -1, 60))}${f("AAC profile", sel(i, "encoder.audio.aacProfile", au.aacProfile || "aac_low", ["aac_low", "aac_he", "aac_he_v2", "aac_ld"]))}<div style="display:flex;align-items:end;padding-bottom:4px">${chk(i, "encoder.audio.normalize", au.normalize, "loudnorm −24 (ffmpeg inputs)")}</div>
    <div style="grid-column:1/-1;color:var(--t2);font-size:10px;letter-spacing:1px;margin-top:4px">TRANSPORT / PIDs</div>
    ${f("Major", num(i, "major", s.major, 50))}${f("Minor", num(i, "minor", s.minor, 50))}${f("Service id", num(i, "serviceId", s.serviceId, 60))}${f("Video PID", num(i, "vidPid", s.vidPid, 60))}${f("Audio PID", num(i, "audPid", s.audPid, 60))}${f("PMT PID", num(i, "encoder.ts.pmtPid", ts.pmtPid ?? 4096, 60))}${f("PCR PID (=video)", num(i, "encoder.ts.pcrPid", ts.pcrPid ?? s.vidPid, 60))}${f("UDP port (feed)", num(i, "udpPort", s.udpPort, 60))}
    ${f("PCR period ms", num(i, "encoder.ts.pcrPeriodMs", ts.pcrPeriodMs ?? 20, 50))}${f("PAT period s", num(i, "encoder.ts.patPeriodSec", ts.patPeriodSec ?? 0.1, 50))}${f("Provider", inp(i, "encoder.ts.provider", ts.provider || "PlayPlay", 110))}${f("Feed muxrate (0=auto 1.15×)", num(i, "encoder.ts.muxrate", ts.muxrate ?? 0, 100))}
    <div style="grid-column:1/-1;color:var(--t2);font-size:10px;letter-spacing:1px;margin-top:4px">STATMUX</div>
    <div style="display:flex;align-items:end;padding-bottom:4px">${chk(i, "statmux.managed", sm.managed !== false, "managed")}</div>${f("Min kb/s", num(i, "statmux.minKbps", sm.minKbps, 60))}${f("Max kb/s", num(i, "statmux.maxKbps", sm.maxKbps, 60))}${f("Weight", num(i, "statmux.weight", sm.weight ?? 100, 50))}${f("Priority", sel(i, "statmux.priority", sm.priority || "normal", ["high", "normal", "low"]))}
    <div style="grid-column:span 3;display:flex;gap:6px;align-items:end;justify-content:end"><button class="ghost small" onclick="pp.muxAdv(${i})">close</button></div>
  </div></td></tr>`;
}

function svcRow(s, i) {
  const S = pp.S; const src = s.source || {}; const inpt = src.input || {}; const en = s.encoder || {}; const sm = s.statmux || {}; const a = X.alloc[s.id]; const per = ((X.status || {}).metrics || {}).perService || {}; const p = per[s.id] || {};
  const chans = visibleChannels().map(c => c.profile.id); const ndiNames = (X.inputs || []).map(n => n.name).filter(Boolean);
  const kinds = ["channel", "ndi", "input", "udp"]; const kind = src.kind || "input";
  const audK = (en.audio || {}).bitrateKbps ?? en.audBitrateKbps ?? 192; const audC = (en.audio || {}).codec || en.audCodec || "ac3";
  const main = `<tr><td class="mono">${e3(s.major)}.${e3(s.minor)}</td>
    <td><input value="${e3(s.shortName)}" maxlength="7" style="width:62px" onchange="pp.muxSet(${i},'shortName',this.value)"><input value="${e3(s.longName)}" style="width:120px" onchange="pp.muxSet(${i},'longName',this.value)"></td>
    <td>${sel(i, "source.kind", kind, kinds)}
      ${kind === "channel" ? `<select class="small" onchange="pp.muxSet(${i},'source.channelId',this.value)">${chans.map(c => `<option ${c === src.channelId ? "selected" : ""}>${e3(c)}</option>`).join("") || "<option>no channels</option>"}</select>`
      : kind === "ndi" ? `<select class="small" onchange="pp.muxSet(${i},'source.ndiName',this.value)"><option value="">— NDI source —</option>${[...new Set([src.ndiName, ...ndiNames].filter(Boolean))].map(n => `<option value="${e3(n)}" ${n === src.ndiName ? "selected" : ""}>${e3(n)}</option>`).join("")}</select><input value="${e3(src.ndiName || "")}" placeholder="MACHINE (Source)" style="width:150px" onchange="pp.muxSet(${i},'source.ndiName',this.value)"> ${chk(i, "source.audio", src.audio !== false, "audio")} <span class="mono" style="color:var(--t3);font-size:10px">→ channel mux-${e3(s.id)}</span>`
      : kind === "udp" ? `<input value="${e3(src.url || "")}" placeholder="udp://239.0.0.1:1234" style="width:150px" onchange="pp.muxSet(${i},'source.url',this.value)">`
      : `${sel(i, "source.input.type", inpt.type || "test", ["test", "file", "srt", "rtmp", "rtsp", "udp", "http", "dshow"])}<input value="${e3(inpt.location || "")}" placeholder="url / path / pattern" style="width:150px" onchange="pp.muxSet(${i},'source.input.location',this.value)">`}</td>
    <td><select class="small" onchange="pp.muxApplyPreset(${i},this.value)"><option value="">preset…</option>${X.presets.map(pr => `<option value="${e3(pr.id)}" ${pr.id === en.encoderPreset ? "selected" : ""}>${e3(pr.id)}</option>`).join("")}</select>
      <div style="color:var(--t2);font-size:10px;margin-top:2px">${e3(en.codec || "libx264")} ${e3(en.width)}×${e3(en.height)} @${e3(en.fps || "30000/1001")} ${e3(en.rateControl || "cbr")}${en.interlaced ? " i" : ""} gop ${e3(en.gop ?? 30)} <button class="ghost small" onclick="pp.muxAdv(${i})">${X.adv[i] ? "Advanced ▴" : "Advanced…"}</button></div></td>
    <td>${num(i, "encoder.vidBitrateKbps", en.vidBitrateKbps, 56)}k / ${num(i, "encoder.audio.bitrateKbps", audK, 44)}k ${e3(audC)}</td>
    <td>${chk(i, "statmux.managed", sm.managed !== false, "")} ${num(i, "statmux.minKbps", sm.minKbps, 50)}–${num(i, "statmux.maxKbps", sm.maxKbps, 50)} ${sel(i, "statmux.priority", sm.priority || "normal", ["high", "normal", "low"])}</td>
    <td class="mono">${a ? (a / 1e6).toFixed(2) : "—"}</td>
    <td><div class="meter" style="width:90px"><div class="bar" style="width:${a ? Math.min(100, 100 * (p.bitrate || 0) / (a + audK * 1000)) : 0}%;background:${p.bitrate ? "var(--ready)" : "var(--t3)"}"></div></div><span class="mono" style="font-size:10px;color:var(--t2)">${p.bitrate ? (p.bitrate / 1e6).toFixed(2) : "0.00"}</span></td>
    <td class="mono" style="color:var(--t3)">${e3(s.serviceId)} / ${e3(s.vidPid)},${e3(s.audPid)}</td>
    <td><button class="ghost small" onclick="pp.muxDel(${i})">&#10005;</button></td></tr>`;
  return main + (X.adv[i] ? svcAdvanced(s, i) : "");
}

function viewServices(st) {
  const reserve = (X.cfg.statmux || {}).reservedBps || 900000; const tb = st.transportBps || 19392658;
  return `<div class="scroll" style="max-height:60vh"><table><thead><tr><th>Ch</th><th>Names</th><th>Source</th><th>Encoder</th><th>Bitrates</th><th>Statmux (min–max kb/s)</th><th>Alloc Mb/s</th><th>Live</th><th>SID / PIDs</th><th></th></tr></thead><tbody>${X.services.map(svcRow).join("") || `<tr><td colspan="10" class="empty">No services. Add one, or apply a channel-count preset.</td></tr>`}</tbody></table></div>
    <div class="row" style="padding:8px 12px;border-top:1px solid var(--b0)"><button class="small" onclick="pp.muxAdd()">+ service</button><label class="f" style="flex-direction:row;align-items:center;gap:6px">preset <select id="muxPreset" class="small">${[2, 4, 6, 8, 10, 12].map(n => `<option ${n === X.services.length ? "selected" : ""}>${n}</option>`).join("")}</select><button class="small" onclick="pp.muxPreset()">apply</button></label>
    <label style="display:flex;gap:4px;align-items:center;font-size:11px;color:var(--t2)"><input type="checkbox" ${X.showHidden ? "checked" : ""} onchange="pp.strShowHidden(this.checked)"> show mux-managed channels</label><span class="fill"></span>
    ${X.dirty ? `<span style="color:var(--warn)">unsaved</span><button class="primary small" onclick="pp.muxSave()">Save services</button><button class="ghost small" onclick="X.dirty=false;pp.muxReload()">discard</button>` : `<span style="color:var(--t3);font-size:11px">allocated video ${mb(st.allocatedVideoBps)} of ${mb(tb - reserve)} after the ${mb(reserve)} PSIP reserve · NDI services run in hidden channels (mux-&lt;id&gt;) fed by the NDI receiver</span>`}</div>`;
}

// ---------------------------------------------------------------------------------------------------
// Encoders: presets + capabilities
// ---------------------------------------------------------------------------------------------------
function viewEncoders() {
  const c = X.caps; const ff = (c || {}).ffmpeg || {}; const has = ff.has || {}; const build = ff.build || {}; const td = (c || {}).tsduck || {}; const dk = (c || {}).dektec || {};
  const flag = (n, ok) => b3(n, ok === undefined ? "var(--t3)" : ok ? "var(--ready)" : "var(--danger)");
  const presets = `<table><thead><tr><th>Preset</th><th>Codec</th><th>Raster</th><th>Video</th><th>GOP</th><th>Audio</th><th>Note</th><th>Apply to</th></tr></thead><tbody>${X.presets.map(p => `<tr><td><b>${e3(p.id)}</b><div style="color:var(--t3);font-size:10px">${e3(p.label)}</div></td><td class="mono">${e3(p.codec)} ${has[p.codec] === false ? b3("missing", "var(--warn)") : ""}</td><td class="mono">${e3(p.width)}×${e3(p.height)}${p.interlaced ? "i" : "p"}</td><td class="mono">${e3(p.vidBitrateKbps)}k ${e3(p.rateControl)}${p.maxrateKbps && p.rateControl === "vbr" ? " max " + e3(p.maxrateKbps) + "k" : ""} ${e3(p.profile)}@${e3(p.level)}</td><td class="mono">${e3(p.gop)}/${e3(p.bframes)}B</td><td class="mono">${e3((p.audio || {}).codec)} ${e3((p.audio || {}).bitrateKbps)}k dn ${e3((p.audio || {}).dialnorm)}</td><td style="color:var(--t2);font-size:11px">${e3(p.note || "")}</td><td><select class="small" onchange="if(this.value!==''){pp.muxApplyPreset(parseInt(this.value),'${e3(p.id)}');this.value=''}"><option value="">service…</option>${X.services.map((s, i) => `<option value="${i}">${e3(s.major)}.${e3(s.minor)} ${e3(s.longName)}</option>`).join("")}</select></td></tr>`).join("")}</tbody></table>`;
  const caps = !c ? `<div class="empty">Probing ffmpeg / TSDuck / DekTec…</div>` : `<div class="body grid" style="gap:10px;grid-template-columns:1fr 1fr">
    <div><div style="color:var(--t2);font-size:10px;letter-spacing:1px;margin-bottom:4px">FFMPEG ${ff.found ? b3("found", "var(--ready)") : b3("not found", "var(--danger)")}</div><div class="mono" style="font-size:11px;color:var(--t1)">${e3(ff.path)}</div><div style="font-size:11px;color:var(--t2)">${e3(ff.version)}</div>
      <div class="row" style="margin-top:6px;gap:4px">${["libx264", "libx265", "mpeg2video", "h264_qsv", "hevc_qsv", "h264_nvenc", "hevc_nvenc", "h264_amf", "ac3", "eac3", "aac", "mp2"].map(n => flag(n, has[n])).join("")}</div>
      <div class="row" style="margin-top:6px;gap:4px">${flag("srt protocol", build.srt)}${flag("rist", build.rist)}${flag("libndi", build.ndi)}${flag("qsv build", build.qsv)}${flag("nvenc build", build.nvenc)}</div>
      <div style="color:var(--t3);font-size:10px;margin-top:6px">${(ff.video || []).length} video / ${(ff.audio || []).length} audio encoders. Missing codecs fall back to libx264 (ffmpeg inputs and engine outputs) with a warning in the log.</div></div>
    <div><div style="color:var(--t2);font-size:10px;letter-spacing:1px;margin-bottom:4px">TSDUCK ${td.found ? b3(td.version || "found", "var(--ready)") : b3("not found", "var(--danger)")}</div><div class="mono" style="font-size:11px;color:var(--t1)">${e3(td.dir)}</div>
      <div class="row" style="margin-top:6px;gap:4px">${Object.entries(td.tools || {}).map(([n, t]) => flag(n, t.found)).join("")}</div>
      <div style="color:var(--t2);font-size:10px;letter-spacing:1px;margin:10px 0 4px">DEKTEC ${dk.dtapi ? b3("DTAPI " + dk.dtapi, "var(--ready)") : ""}</div>
      ${(dk.devices || []).length ? (dk.devices || []).map(d => `<div style="font-size:11px"><b>${e3(d.index)}: ${e3(d.model)}</b> <span style="color:var(--t2)">${e3(d.description)} · ${e3(d.ports)} ports · ${e3(d.inputs)} in / ${e3(d.outputs)} out · s/n ${e3(d.serial)} · fw ${e3(d.firmware)}</span><div class="mono" style="color:var(--t3);font-size:10px">${(d.channels || []).map(ch => `${ch.kind} ${ch.index} = port ${ch.port}${ch.label ? " (" + e3(ch.label) + ")" : ""} [${e3(ch.standards)}]`).join(" · ")}</div></div>`).join("") : `<div style="color:var(--t3);font-size:11px">no DekTec device found${dk.raw ? ": " + e3(String(dk.raw).slice(0, 200)) : ""}</div>`}</div>
    <div style="grid-column:1/-1" class="row"><button class="small" onclick="pp.muxRefreshCaps()">Re-probe</button><span style="color:var(--t3);font-size:11px">probed ${c.probedMs ? new Date(c.probedMs).toLocaleTimeString() : "—"} · cached 10 min · transport ${mb(c.transportBps)}</span></div></div>`;
  return `<div class="row" style="padding:6px 12px;border-bottom:1px solid var(--b0)">${["presets", "capabilities"].map(t => `<button class="ghost small${X.encTab === t ? " active" : ""}" onclick="X.encTab='${t}';pp.render()">${t}</button>`).join("")}</div>${X.encTab === "presets" ? `<div class="scroll" style="max-height:58vh">${presets}</div><div style="padding:8px 12px;color:var(--t3);font-size:11px">Presets fill the service encoder (codec, raster, rates, GOP, audio); Advanced… on the service row edits every field. ATSC main services: MPEG-2 1080i or 480i with AC-3; sub-channels may use H.264 (A/72). Streams: H.264/HEVC VBR + AAC.</div>` : caps}`;
}

// ---------------------------------------------------------------------------------------------------
// Tables: PSIP summary, XML viewer, overrides, compile, analyze
// ---------------------------------------------------------------------------------------------------
function viewTables() {
  const t = X.tables; if (!t) return `<div class="empty">Loading tables…</div>`;
  const ps = t.psip || {}; const ov = X.ovr || {}; const ovT = ov.tvct || {}; const ovS = ov.services || {}; const rr = ov.rrt || {};
  const o = (path, v, w = 70, type = "text") => `<input type="${type}" value="${e3(v ?? "")}" style="width:${w}px" onchange="pp.muxOvrSet('${path}',${type === "number" ? "parseFloat(this.value)" : "this.value"})">`;
  const oc = (path, v, label) => `<label style="display:inline-flex;gap:4px;align-items:center;text-transform:none;font-size:11px"><input type="checkbox" ${v ? "checked" : ""} onchange="pp.muxOvrSet('${path}',this.checked)">${e3(label)}</label>`;
  const file = (t.files || []).find(x => x.name === X.file) || (t.files || [])[0] || {};
  const eit0 = (ps.eit || [])[0] || {};
  return `<div class="body grid" style="gap:12px;grid-template-columns:1.1fr 1fr">
    <div>
      <div style="color:var(--t2);font-size:10px;letter-spacing:1px;margin-bottom:4px">TVCT — TSID ${e3(ps.tsid)} (0x${Number(ps.tsid || 0).toString(16)}) · ${e3(ps.modulation || "8-VSB")} · version ${e3(ps.version)} · generated ${ps.generatedMs ? new Date(ps.generatedMs).toLocaleTimeString() : "—"}</div>
      <table><thead><tr><th>Ch</th><th>Short</th><th>Long name</th><th>Prog#</th><th>Source id</th><th>PMT</th><th>Type</th><th>Flags</th><th>Overrides</th></tr></thead><tbody>${(ps.tvct || []).map(r => { const so = ovS[r.id] || {}; return `<tr><td class="mono">${e3(r.major)}.${e3(r.minor)}</td><td class="mono">${e3(r.shortName)}</td><td>${e3(r.longName)}</td><td class="mono">${e3(r.serviceId)}</td><td class="mono">${e3(r.sourceId)}</td><td class="mono">0x${Number(r.pmtPid || 0).toString(16)}</td><td>${e3(r.serviceType)}</td><td style="font-size:10px">${r.hidden ? b3("hidden", "var(--warn)") : ""}${r.hideGuide ? b3("hide guide", "var(--warn)") : ""}${r.accessControlled ? b3("CA", "var(--danger)") : ""}${r.etm ? b3("ETM " + r.etm, "var(--t2)") : ""}</td>
        <td style="white-space:nowrap">${o("services." + r.id + ".sourceId", so.sourceId ?? "", 50, "number")} <select class="small" onchange="pp.muxOvrSet('services.${e3(r.id)}.serviceType',this.value)">${["dtv", "audio", "data", "analog"].map(x => `<option ${x === (so.serviceType || "dtv") ? "selected" : ""}>${x}</option>`).join("")}</select> ${oc("services." + r.id + ".hidden", so.hidden, "hidden")} ${oc("services." + r.id + ".hideGuide", so.hideGuide, "hide guide")} ${oc("services." + r.id + ".accessControlled", so.accessControlled, "CA")} ETM ${o("services." + r.id + ".etm", so.etm ?? 0, 36, "number")}</td></tr>`; }).join("") || `<tr><td colspan="9" class="empty">no services</td></tr>`}</tbody></table>
      <div class="grid" style="gap:8px;grid-template-columns:repeat(4,1fr);margin-top:10px">
        <label class="f">Modulation <select onchange="pp.muxOvrSet('tvct.modulation',this.value)">${["8-VSB", "16-VSB", "64-QAM", "256-QAM", "analog"].map(x => `<option ${x === (ovT.modulation || "8-VSB") ? "selected" : ""}>${x}</option>`).join("")}</select></label>
        <label class="f">Channel TSID (empty = TSID) ${o("tvct.channelTsid", ovT.channelTsid ?? "", 80, "number")}</label><label class="f">Carrier freq (Hz, 0) ${o("tvct.carrierFrequency", ovT.carrierFrequency ?? 0, 90, "number")}</label>
        <label class="f">EIT depth (days) ${o("eitDays", ov.eitDays ?? 1, 50, "number")}</label>
        <label class="f">EIT-0 interval s (0 = default) ${o("eitIntervalSec", ov.eitIntervalSec ?? 0, 60, "number")}</label><label class="f">STT interval s ${o("sttIntervalSec", ov.sttIntervalSec ?? 5, 50, "number")}</label><label class="f">MGT/TVCT interval s (0 = default) ${o("mgtIntervalSec", ov.mgtIntervalSec ?? 0, 60, "number")}</label>
        <label class="f">RRT ${oc("rrt.enabled", rr.enabled, "transmit US RRT (region 1)")}</label>
        <label class="f" style="grid-column:1/-1">TVCT extra descriptors (TSDuck XML, inside &lt;TVCT&gt;) <input value="${e3(ovT.extraXml || "")}" onchange="pp.muxOvrSet('tvct.extraXml',this.value)"></label>
        <label class="f" style="grid-column:1/-1">Custom tables (TSDuck XML &lt;tsduck&gt;…, injected on ${e3(ov.customPid || "0x1FFA")}) <textarea rows="3" class="mono" style="font-size:10px" onchange="pp.muxOvrSet('customXml',this.value)">${e3(ov.customXml || "")}</textarea></label>
      </div>
      <div class="row" style="margin-top:8px"><button class="primary small" ${X.ovrDirty ? "" : "disabled"} onclick="pp.muxOvrSave()">Save overrides</button><button class="small" onclick="pp.muxCompile()">Compile (tstabcomp)</button>
        <select id="muxAnMode" class="small"><option value="tables">analyze: PSIP tables</option><option value="analyze">analyze: tsp analyzer</option></select><select id="muxAnSvc" class="small"><option value="">mux output (UDP) / first service</option>${X.services.map(s => `<option value="${e3(s.id)}">service ${e3(s.id)} multicast</option>`).join("")}</select><button class="small" ${X.analyzing ? "disabled" : ""} onclick="pp.muxAnalyze()">${X.analyzing ? "analyzing…" : "Analyze live"}</button>
        <span class="fill"></span><span style="color:var(--t3);font-size:11px">${e3(t.dir)} · EIT-0..${(t.eitTables || 8) - 1} · STT GPS ${e3((ps.stt || {}).systemTimeGps)} (${e3((ps.stt || {}).utc)})</span></div>
      ${X.dry && X.dry.compile ? `<div style="margin-top:8px"><div style="color:var(--t2);font-size:10px;letter-spacing:1px">COMPILE ${X.dry.compile.compiled ? b3("ok", "var(--ready)") : b3("failed", "var(--danger)")}</div><table><tbody>${(X.dry.compile.files || []).map(r => `<tr><td class="mono">${e3(r.file)}</td><td>${r.ok ? b3("ok", "var(--ready)") : b3("rc " + r.exitCode, "var(--danger)")}</td><td class="mono" style="font-size:10px;color:var(--t2)">${e3(r.output || "")}</td></tr>`).join("")}</tbody></table></div>` : ""}
      ${X.analysis ? `<div style="margin-top:8px"><div style="color:var(--t2);font-size:10px;letter-spacing:1px">ANALYSIS ${X.analysis.error ? b3(X.analysis.error, "var(--danger)") : b3("rc " + X.analysis.exitCode, X.analysis.exitCode === 0 ? "var(--ready)" : "var(--warn)")} <span style="letter-spacing:0;color:var(--t3)">${e3(X.analysis.source || "")} ${e3(X.analysis.note || "")}</span></div><div class="mono" style="font-size:10px;color:var(--t3);word-break:break-all">${e3(X.analysis.command || "")}</div><pre class="scroll mono" style="max-height:30vh;margin:4px 0 0;font-size:10px;white-space:pre-wrap">${e3(X.analysis.output || "(no output)")}</pre></div>` : ""}
    </div>
    <div>
      <div style="color:var(--t2);font-size:10px;letter-spacing:1px;margin-bottom:4px">MGT (${(ps.mgt || []).length} tables) · EIT-0 ${eit0.fromMs ? new Date(eit0.fromMs).toLocaleTimeString([], { hour: "numeric" }) + "–" + new Date(eit0.toMs).toLocaleTimeString([], { hour: "numeric" }) : ""}</div>
      <div class="mono" style="font-size:10px;color:var(--t2);margin-bottom:6px">${(ps.mgt || []).map(m => e3(m.type) + "@" + e3(m.pid)).join(" · ")}</div>
      <div class="scroll" style="max-height:18vh"><table><tbody>${(eit0.events || []).map(ev => `<tr><td class="mono">${new Date(ev.startMs).toLocaleTimeString([], { hour: "numeric", minute: "2-digit" })}</td><td class="mono" style="color:var(--t2)">src ${e3(ev.sourceId)}</td><td>${e3(ev.title)}</td></tr>`).join("") || `<tr><td class="empty">no EIT events</td></tr>`}</tbody></table></div>
      <div class="row" style="margin:8px 0 4px"><select class="small" onchange="pp.muxPickFile(this.value)">${(t.files || []).map(fl => `<option value="${e3(fl.name)}" ${fl.name === file.name ? "selected" : ""}>${e3(fl.name)} (${e3(fl.size)} B)</option>`).join("")}</select><span style="color:var(--t3);font-size:11px">${file.modifiedMs ? "written " + new Date(file.modifiedMs).toLocaleTimeString() : ""} · tsp re-reads changed files (--poll-files)</span></div>
      <textarea readonly class="mono" style="width:100%;height:40vh;font-size:10px;white-space:pre">${e3(file.xml || "")}</textarea>
    </div></div>`;
}

// ---------------------------------------------------------------------------------------------------
// Output: ASI / UDP / SRT / file, DekTec devices, dry run
// ---------------------------------------------------------------------------------------------------
function viewOutput(st) {
  const out = X.cfg.output || {}; const kind = out.kind || "dektec"; const dk = X.dektec || {}; const devs = dk.devices || [];
  const o = (k, v, type = "text", w = "") => `<input type="${type}" value="${e3(v ?? "")}" ${w ? `style="width:${w}px"` : ""} onchange="pp.muxCfg({output:{${k}:${type === "number" ? "parseFloat(this.value)" : "this.value"}}})">`;
  const dev = devs.find(d => d.index === (out.device ?? 0)); const outChans = dev ? (dev.channels || []).filter(c => c.kind === "output") : [];
  return `<div class="body grid" style="gap:10px;grid-template-columns:repeat(4,1fr)">
    <label class="f">Output <select onchange="pp.muxCfg({output:{kind:this.value}})">${["dektec", "udp", "srt", "file"].map(k => `<option ${k === kind ? "selected" : ""}>${k}</option>`).join("")}</select></label>
    ${kind === "dektec" ? `<label class="f">DekTec device ${devs.length ? `<select onchange="pp.muxCfg({output:{device:parseInt(this.value)}})">${devs.map(d => `<option value="${d.index}" ${d.index === (out.device ?? 0) ? "selected" : ""}>${d.index}: ${e3(d.model)} s/n ${e3(d.serial)}</option>`).join("")}</select>` : o("device", out.device ?? 0, "number")}</label>
      <label class="f">Output channel (tsp --channel) ${outChans.length ? `<select onchange="pp.muxCfg({output:{channel:parseInt(this.value),port:${JSON.stringify(outChans.map(c => c.port))}[this.selectedIndex]}})">${outChans.map(c => `<option value="${c.index}" ${c.index === (out.channel ?? 0) ? "selected" : ""}>channel ${c.index} = port ${c.port}${c.label ? " " + e3(c.label) : ""}</option>`).join("")}</select>` : o("channel", out.channel ?? 0, "number")}</label>
      <label class="f">ASI port (tsdektec --output, 1-based) ${o("port", out.port ?? 2, "number")}</label>`
    : kind === "file" ? `<label class="f" style="grid-column:span 3">File path ${o("url", out.url || "")}</label>`
    : kind === "srt" ? `<label class="f" style="grid-column:span 2">SRT URL (srt://host:port?mode=caller|listener&latency=µs&passphrase=) ${o("url", out.url || "srt://0.0.0.0:9000?mode=listener")}</label><label class="f">Latency ms ${o("latencyMs", out.latencyMs ?? 200, "number")}</label>`
    : `<label class="f" style="grid-column:span 2">UDP URL ${o("url", out.url || "udp://239.1.1.1:1234")}</label><label class="f">TTL (0 = default) ${o("ttl", out.ttl ?? 0, "number")}</label>`}
    <label class="f">Transport bitrate (b/s) <div class="row" style="gap:4px">${o("bitrate", out.bitrate ?? 19392658, "number", 110)}<button class="ghost small" onclick="pp.muxCfg({output:{bitrate:19392658}})">ATSC</button></div></label>
    ${kind === "dektec" ? `<label class="f">FIFO preload ms (0 = off) ${o("preloadMs", out.preloadMs ?? 4000, "number")}</label><label class="f">Preload <label style="text-transform:none"><input type="checkbox" ${out.maintainPreload !== false ? "checked" : ""} onchange="pp.muxCfg({output:{maintainPreload:this.checked}})"> maintain preload</label><label style="text-transform:none"><input type="checkbox" ${out.stuffing !== false ? "checked" : ""} onchange="pp.muxCfg({output:{stuffing:this.checked}})"> null stuffing</label></label>` : `<div></div><div></div>`}
    <label class="f">TSID <input value="${e3(X.cfg.tsid)}" onchange="pp.muxCfg({tsid:parseInt(this.value)})"></label><label class="f">Timezone (EPG) <input value="${e3(X.cfg.timezone)}" onchange="pp.muxCfg({timezone:this.value})"></label><label class="f">Multicast base <input value="${e3(X.cfg.mcastBase)}" onchange="pp.muxCfg({mcastBase:this.value})"></label>
    <label class="f">Statmux <label style="display:flex;gap:6px;align-items:center;text-transform:none"><input type="checkbox" ${(X.cfg.statmux || {}).enabled !== false ? "checked" : ""} onchange="pp.muxCfg({statmux:{enabled:this.checked}})"> enabled</label></label>
    <label class="f">PSIP reserve (b/s) <input value="${e3((X.cfg.statmux || {}).reservedBps)}" onchange="pp.muxCfg({statmux:{reservedBps:parseInt(this.value)}})"></label>
    <label class="f">Auto-start with the Supervisor <label style="text-transform:none"><input type="checkbox" ${X.cfg.autoStart ? "checked" : ""} onchange="pp.muxCfg({autoStart:this.checked,enabled:true})"> yes (3 s after start, 5 retries with backoff)</label></label>
    <label class="f">TSDuck bin dir <input value="${e3(X.cfg.tsduckDir)}" onchange="pp.muxCfg({tsduckDir:this.value})"></label><label class="f">FFmpeg (optional) <input value="${e3(X.cfg.ffmpegPath || "")}" placeholder="on PATH" onchange="pp.muxCfg({ffmpegPath:this.value})"></label>
    <div style="grid-column:1/-1;color:var(--t3);font-size:11px">tsp ${st.tspFound ? "found" : "<span style='color:var(--danger)'>not found</span>"}: ${e3(st.tsp)} · ffmpeg ${e3(st.ffmpeg)} ${dk.dtapi ? "· DTAPI " + e3(dk.dtapi) + " / Dta " + e3(dk.dta) : ""}</div>
    <div style="grid-column:1/-1"><div style="color:var(--t2);font-size:10px;letter-spacing:1px;margin-bottom:4px">DEKTEC DEVICES (tsdektec -a -v) <button class="ghost small" onclick="pp.muxDektec()">refresh</button></div>${devs.length ? devs.map(d => `<div style="font-size:11px"><b>${e3(d.index)}: ${e3(d.model)}</b> <span style="color:var(--t2)">${e3(d.description)} · ${e3(d.ports)} ports · s/n ${e3(d.serial)} · fw ${e3(d.firmware)}</span><div class="mono" style="color:var(--t3);font-size:10px">${(d.channels || []).map(ch => `${ch.kind} ${ch.index} → port ${ch.port}${ch.label ? " (" + e3(ch.label) + ")" : ""}`).join(" · ")}</div></div>`).join("") : `<span style="color:var(--t3);font-size:11px">${X.dektec ? "no DekTec device found" + (dk.raw ? ": " + e3(String(dk.raw).slice(0, 160)) : "") : "loading…"}</span>`}</div>
    <div style="grid-column:1/-1" class="row"><button class="small" onclick="pp.muxValidate()">Validate PSIP tables</button><button class="small" onclick="pp.muxDryRun()">Dry run (command lines)</button><span style="color:var(--t3);font-size:11px">Twilio SMS for critical alerts: ${X.cfg.twilio ? "enabled" : "off"} (PUT /api/v1/alerts/twilio {enabled, accountSid, authToken, from, to[]})</span></div>
    ${X.dry && X.dry.tsp ? `<div style="grid-column:1/-1"><div style="color:var(--t2);font-size:10px;letter-spacing:1px">DRY RUN · transport ${mb(X.dry.transportBps)} · ${(X.dry.warnings || []).length ? X.dry.warnings.map(w => b3(w, "var(--warn)")).join(" ") : b3("no warnings", "var(--ready)")}</div>
      ${X.dry.dektecPrep ? `<div style="color:var(--t3);font-size:10px;margin-top:4px">1. port setup</div><pre class="mono" style="margin:0;font-size:10px;white-space:pre-wrap;word-break:break-all">${e3(X.dry.dektecPrep)}</pre>` : ""}
      <div style="color:var(--t3);font-size:10px;margin-top:4px">${X.dry.dektecPrep ? "2." : "1."} tsp master (${(X.dry.tsp.args || []).length} args)</div><pre class="mono scroll" style="margin:0;font-size:10px;white-space:pre-wrap;word-break:break-all;max-height:22vh">${e3(X.dry.tsp.commandLine)}</pre>
      ${(X.dry.encoders || []).map(en => `<div style="color:var(--t3);font-size:10px;margin-top:4px">ffmpeg encoder · service ${e3(en.serviceId)} ${en.error ? b3(en.error, "var(--warn)") : ""}</div><pre class="mono" style="margin:0;font-size:10px;white-space:pre-wrap;word-break:break-all">${e3(en.commandLine || "(not runnable)")}</pre>`).join("")}
      ${(X.dry.channels || []).map(ch => `<div style="color:var(--t3);font-size:10px;margin-top:4px">${ch.channelId ? "channel " + e3(ch.channelId) + (ch.managed ? " (mux-managed, NDI " + e3((ch.fixedSource || {}).name) + ")" : "") + " · engine output profile" : "merge " + e3(ch.merge)} · service ${e3(ch.serviceId)}</div>${ch.outputs ? `<pre class="mono" style="margin:0;font-size:10px;white-space:pre-wrap;word-break:break-all">${e3(JSON.stringify(ch.outputs[0] || {}, null, 0))}</pre>` : ""}`).join("")}</div>` : ""}
  </div>`;
}

// ---------------------------------------------------------------------------------------------------
// Streams: per-channel streaming outputs (also embedded in the Outputs tab via viewStreams())
// ---------------------------------------------------------------------------------------------------
// SDI baseband fields for the add form: DekTec (port, standard, embedded audio) and DeckLink (device, mode, audio, keyer).
const SDI_MODES = ["auto", "1080i59.94", "1080p29.97", "720p59.94", "1080i50", "1080p25", "1080p23.98", "1080p30", "1080i60", "720p50"];
function sdiFields(A) {
  const sel = (k, label, opts, cur, num) => `<label class="f">${label} <select onchange="pp.strAddSet('${k}',${num ? "parseInt(this.value)" : "this.value"})">${opts.map(v => `<option ${String(v) === String(cur) ? "selected" : ""}>${v}</option>`).join("")}</select></label>`;
  if (A.type === "dektec_asi") return sel("port", "Port", [1, 2, 3, 4], A.port ?? 2, true);
  if (A.type === "dektec_sdi") return sel("port", "Port", [1, 2, 3, 4], A.port ?? 2, true) + sel("videoStd", "Video std", SDI_MODES, A.videoStd || "auto") + sel("audioChannels", "Audio ch", [2, 4, 8, 16], A.audioChannels ?? 2, true);
  return `<label class="f">Device # <input value="${e3(A.deviceIndex ?? 0)}" style="width:40px" title="0-based DeckLink device index" oninput="pp.strAddSet('deviceIndex',parseInt(this.value)||0)"></label>`
    + `<label class="f">Device name <input value="${e3(A.deviceName || "")}" placeholder="optional, overrides #" style="width:130px" oninput="pp.strAddSet('deviceName',this.value)"></label>`
    + sel("videoMode", "Video mode", SDI_MODES, A.videoMode || "auto") + sel("audioChannels", "Audio ch", [2, 8, 16], A.audioChannels ?? 2, true)
    + sel("keyer", "Keyer", ["off", "internal", "external"], A.keyer || "off")
    + `<label class="f">Key level <input value="${e3(A.keyLevel ?? 255)}" style="width:44px" oninput="pp.strAddSet('keyLevel',parseInt(this.value))"></label>`;
}


// viewFacebook lives in facebooklive.js (shared with the Now Playing page); X.fb is its FB state.

export function viewStreams() {
  const chans = (X.streams || []).filter(c => X.showHidden || !c.hidden); const A = X.add; if ((!A.ch || !chans.some(c => c.channelId === A.ch)) && chans.length) A.ch = (chans.find(c => c.channelId === pp.S.active) || chans[0]).channelId;   // default: the channel being operated
  const row = (c, o) => { const l = o.live || {}; const st = l.state || "pending"; const prof = typeof o.profile === "string" ? o.profile : (o.profile || {}).preset || (o.profile || {}).name || (o.type === "ndi" || o.type === "dektec_sdi" || o.type === "decklink" ? "" : "h264_aac");
    const act = (op, label, cls = "small") => `<button class="${cls}" ${c.engineConnected ? "" : "disabled"} onclick="pp.strOp('${e3(c.channelId)}','${e3(o.name)}','${op}')">${label}</button>`;
    return `<tr><td>${b3(st, stateColor(st))}</td><td><b>${e3(o.name)}</b>${l.lastError ? `<div style="color:var(--danger);font-size:10px">${e3(l.lastError)}</div>` : ""}${l.retryInSec !== undefined ? `<div style="color:var(--warn);font-size:10px">retry in ${e3(l.retryInSec)} s (attempt ${e3(l.attempts)})</div>` : ""}</td><td style="color:var(--t2)">${e3(o.type)}</td><td class="mono" style="font-size:10px;max-width:260px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap" title="${e3(o.url || "")}">${e3(o.url || (o.type === "decklink" ? "DeckLink " + ((o.options || {}).deviceName || "#" + ((o.options || {}).deviceIndex ?? 0)) + " " + ((o.options || {}).videoMode || "auto") : (o.options || {}).port !== undefined ? "port " + (o.options || {}).port + ((o.options || {}).videoStd ? " " + (o.options || {}).videoStd : "") : ""))}</td><td class="mono" style="font-size:10px">${e3(prof)}${l.videoCodec ? ` <span style="color:var(--t3)">${e3(l.videoCodec)}${l.encodedWidth ? " " + l.encodedWidth + "×" + l.encodedHeight : ""}</span>` : ""}</td>
      <td class="mono">${st === "running" ? kb(l.bitrateKbps) : "—"}</td><td class="mono" style="color:var(--t2);font-size:10px">${l.bytesOut ? (l.bytesOut / 1048576).toFixed(1) + " MB" : ""}${l.dropped ? ` <span style="color:var(--warn)">drop ${l.dropped}</span>` : ""}${l.fifoLoadPct !== undefined ? ` fifo ${Number(l.fifoLoadPct).toFixed(0)}%` : ""}</td><td class="mono" style="color:var(--t2);font-size:10px">${st === "running" ? fmtUp(l.uptimeSec) : ""}${l.restarts ? ` · ${l.restarts} restarts` : ""}</td>
      <td><label style="display:inline-flex;gap:3px;align-items:center;font-size:10px"><input type="checkbox" ${o.autoStart !== false ? "checked" : ""} onchange="pp.strToggle('${e3(c.channelId)}','${e3(o.name)}','autoStart',this.checked)">auto</label> <label style="display:inline-flex;gap:3px;align-items:center;font-size:10px"><input type="checkbox" ${o.enabled !== false ? "checked" : ""} onchange="pp.strToggle('${e3(c.channelId)}','${e3(o.name)}','enabled',this.checked)">enabled</label></td>
      <td style="white-space:nowrap">${st === "running" || st === "starting" ? act("stop", "Stop", "danger small") + act("restart", "Restart") : act("start", "Start", "success small")} <button class="ghost small" onclick="pp.strRemove('${e3(c.channelId)}','${e3(o.name)}')" title="remove from the channel">&#10005;</button></td></tr>`; };
  const tsTypes = ["rtmp", "srt", "udp", "file", "dektec_asi"];
  return `<style>.strm table td{vertical-align:middle}</style><div class="strm">${viewFacebook(chans, { presets: X.presets })}${chans.map(c => `<div style="padding:6px 12px 2px;display:flex;gap:8px;align-items:center"><b>${e3(c.channelName || c.channelId)}</b><span style="color:var(--t3);font-size:11px">${c.engineConnected ? c.running + " / " + c.defined + " running" : b3("engine offline", "var(--danger)")}${c.hidden ? " " + b3("mux-managed", "var(--accent)") : ""} ${(c.encoders || []).map(en => `· enc ${e3(en.profile)} ${e3(en.videoCodec || "")} ${e3(en.width)}×${e3(en.height)} q${e3(en.queue)} ${Number(en.avgEncodeMs || 0).toFixed(1)} ms${en.dropped ? " drop " + en.dropped : ""}`).join(" ")}</span></div>
    <table><thead><tr><th>State</th><th>Name</th><th>Type</th><th>URL / target</th><th>Profile</th><th>Bitrate</th><th>Out</th><th>Up</th><th>Policy</th><th></th></tr></thead><tbody>${(c.outputs || []).map(o => row(c, o)).join("") || `<tr><td colspan="10" class="empty">no outputs defined</td></tr>`}</tbody></table>`).join("") || `<div class="empty">${X.streams ? "No channels" : "Loading…"}</div>`}
    <div class="row" style="padding:10px 12px;border-top:1px solid var(--b0);gap:6px;align-items:end">
      <label class="f">Channel <select onchange="pp.strAddSet('ch',this.value)">${chans.map(c => `<option value="${e3(c.channelId)}" ${c.channelId === A.ch ? "selected" : ""}>${e3(c.channelName || c.channelId)}</option>`).join("")}</select></label>
      <label class="f">Type <select onchange="pp.strAddSet('type',this.value)">${["rtmp", "srt", "udp", "file", "ndi", "dektec_asi", "dektec_sdi", "decklink"].map(t => `<option ${t === A.type ? "selected" : ""}>${t}</option>`).join("")}</select></label>
      <label class="f">Name <input value="${e3(A.name)}" placeholder="${e3(A.type)}" style="width:110px" oninput="pp.strAddSet('name',this.value)"></label>
      ${["rtmp", "srt", "udp", "file"].includes(A.type) ? `<label class="f">URL <input value="${e3(A.url)}" style="width:260px" oninput="pp.strAddSet('url',this.value)"></label>` : A.type.startsWith("dektec") || A.type === "decklink" ? sdiFields(A) : `<label class="f">NDI name <input value="${e3(A.sourceName || "")}" placeholder="channel name" style="width:160px" oninput="pp.strAddSet('sourceName',this.value)"></label>`}
      ${tsTypes.includes(A.type) ? `<label class="f">Profile <select onchange="pp.strAddSet('preset',this.value)">${X.presets.map(p => `<option value="${e3(p.id)}" ${p.id === A.preset ? "selected" : ""}>${e3(p.id)}</option>`).join("") || `<option>h264_aac</option>`}</select></label><label class="f">Video kb/s <input value="${e3(A.kbps)}" placeholder="preset" style="width:60px" oninput="pp.strAddSet('kbps',this.value)"></label>` : ""}
      <label style="display:flex;gap:4px;align-items:center;font-size:11px"><input type="checkbox" ${A.enabled ? "checked" : ""} onchange="pp.strAddSet('enabled',this.checked)"> enabled</label><label style="display:flex;gap:4px;align-items:center;font-size:11px"><input type="checkbox" ${A.autoStart ? "checked" : ""} onchange="pp.strAddSet('autoStart',this.checked)"> auto-start</label>
      <button class="primary small" onclick="pp.strAdd()">+ Add output</button><span class="fill"></span><label style="display:flex;gap:4px;align-items:center;font-size:11px;color:var(--t2)"><input type="checkbox" ${X.showHidden ? "checked" : ""} onchange="pp.strShowHidden(this.checked)"> show mux-managed channels</label><span style="color:var(--t3);font-size:11px">Start/Stop touch one output; the others stay on air. Failed outputs retry (10 × 5 s backoff by default).</span></div></div>`;
}

// ---------------------------------------------------------------------------------------------------
// SCTE-35 (docs/mux.md "SCTE-35"): cue messages per service, automatic on ad breaks or by hand
// ---------------------------------------------------------------------------------------------------
function viewScte35(st) {
  const r = X.s35;
  if (!r) return `<div class="empty" style="padding:12px">loading…</div>`;
  if (r.error) return `<div class="alert error" style="margin:12px">${e3(r.error)}${/404/.test(r.error) ? " — this Supervisor predates SCTE-35: update and restart it." : ""}</div>`;
  const c = r.config || {}, svcs = r.services || [], log = r.log || [];
  const tog = (k, label, hint) => `<label style="display:inline-flex;gap:6px;align-items:center;text-transform:none" title="${e3(hint || "")}"><input type="checkbox" ${c[k] ? "checked" : ""} onchange="pp.muxS35({${k}:this.checked})">${e3(label)}</label>`;
  const nin = (k, label, w = 70) => `<label class="f" style="font-size:10px">${e3(label)}<input type="number" value="${e3(c[k] ?? "")}" style="width:${w}px" onchange="pp.muxS35({${k}:parseFloat(this.value)})"></label>`;
  const dur = X.cueDur ?? 120;
  const rows = svcs.map(s => `<tr><td><b>${e3(s.name)}</b> <span class="mono" style="color:var(--t3)">${e3(s.id)} · sid ${e3(s.serviceId)}</span></td>
      <td><label style="display:inline-flex;gap:4px;align-items:center;text-transform:none"><input type="checkbox" ${s.enabled ? "checked" : ""} onchange="pp.muxS35Svc('${e3(s.id)}',{enabled:this.checked})">on</label></td>
      <td><input class="mono" value="${e3(s.pidHex)}" style="width:70px" onchange="pp.muxS35Svc('${e3(s.id)}',{pid:this.value})"></td>
      <td class="mono">${e3(s.port)}</td><td class="mono">${e3(s.channelId || "—")}</td>
      <td>${!s.active ? b3("off", "var(--t3)") : s.live ? b3("injecting", "var(--ready)") : st.state === "running" ? b3("restart needed", "var(--warn)") : b3("ready", "var(--t2)")}</td>
      <td>${s.inBreak ? b3("IN BREAK" + (s.breakSec ? " " + Math.round(s.breakSec) + " s" : ""), "var(--danger)") : s.lastAction ? `<span style="color:var(--t3);font-size:11px">${e3(s.lastAction)} ${s.lastCueMs ? new Date(s.lastCueMs).toLocaleTimeString() : ""}</span>` : "—"}</td>
      <td style="white-space:nowrap"><button class="danger small" ${s.active ? "" : "disabled"} onclick="pp.muxCue('${e3(s.id)}','out')">CUE OUT</button><button class="success small" ${s.active ? "" : "disabled"} onclick="pp.muxCue('${e3(s.id)}','in')">CUE IN</button><button class="ghost small" ${s.active && s.eventId ? "" : "disabled"} onclick="pp.muxCue('${e3(s.id)}','cancel')">cancel</button></td></tr>`).join("");
  const lg = log.slice(0, 30).map(x => `<tr><td class="mono">${new Date(x.atMs).toLocaleTimeString()}</td><td>${e3(x.service)}</td><td>${b3(x.action, x.action === "out" ? "var(--danger)" : x.action === "in" ? "var(--ready)" : "var(--t2)")}</td><td class="mono">${e3(x.eventId)}</td><td class="mono">${x.durationSec ? e3(Math.round(x.durationSec)) + " s" : "—"}</td><td>${e3(x.why || "")}</td><td>${x.delivered ? b3("sent", "var(--ready)") : b3("no injector", "var(--warn)")}</td></tr>`).join("");
  return `<div style="padding:10px 12px">
      ${r.restartRequired ? `<div class="alert warn" style="margin-bottom:8px">SCTE-35 settings changed while the mux runs: the PMT components and injectors change at the next mux start. <button class="small" onclick="pp.muxRestart()">Restart mux now</button> (a few seconds of black on air)</div>` : ""}
      <div class="row" style="gap:18px;flex-wrap:wrap;align-items:center">
        <label style="display:inline-flex;gap:6px;align-items:center;text-transform:none;font-weight:600"><input type="checkbox" ${c.enabled ? "checked" : ""} onchange="pp.muxS35({enabled:this.checked})">SCTE-35 on</label>
        ${tog("autoBreaks", "cue out on ad breaks", "When an item of a break type starts on the channel feeding a service, send splice_insert out_of_network with the break's planned length")}
        ${tog("cueIn", "cue in when the break ends", "Send an explicit return (out_of_network = 0) when the first non-break item starts")}
        ${tog("autoReturn", "auto return (break_duration)", "Downstream splicers return by themselves when break_duration runs out")}
        <label class="f" style="font-size:10px">Break types (comma)<input value="${e3((c.breakTypes || []).join(", "))}" style="width:150px" onchange="pp.muxS35({breakTypes:this.value.split(',').map(x=>x.trim()).filter(Boolean)})"></label>
        ${nin("minBreakSec", "Min break (s)", 60)}${nin("offsetMs", "Send delay (ms)", 70)}${nin("injectCount", "Repeats", 50)}${nin("heartbeatSec", "splice_null every (s, 0 = off)", 60)}${nin("basePort", "UDP base port", 70)}
      </div>
      <div style="color:var(--t3);font-size:11px;margin:6px 0 10px">Each service with SCTE-35 gets a CUEI-registered component (stream type 0x86) in its PMT and a TSDuck <code>spliceinject</code> listening on 127.0.0.1:port; cues are immediate <code>splice_insert</code> sections. Turning SCTE-35 or a service on/off, or changing a PID, applies at the next mux start; the cue toggles apply at once. Send delay lines the cue up with encoder latency.</div>
      <table><thead><tr><th>Service</th><th>SCTE-35</th><th>PID</th><th>UDP</th><th>Fed by</th><th>Injector</th><th>Break</th><th>Manual <input type="number" value="${e3(dur)}" style="width:60px" title="CUE OUT break length (s); 0 = open-ended" onchange="X.cueDur=parseFloat(this.value)||0"> s</th></tr></thead><tbody>${rows || `<tr><td colspan="8" class="empty">no services</td></tr>`}</tbody></table>
      <div class="row" style="margin-top:6px"><button class="danger small" ${c.enabled ? "" : "disabled"} onclick="pp.muxCue('*','out')">CUE OUT all</button><button class="success small" ${c.enabled ? "" : "disabled"} onclick="pp.muxCue('*','in')">CUE IN all</button></div>
      <div style="color:var(--t2);font-size:10px;letter-spacing:1px;margin:12px 0 4px">CUE LOG</div>
      <div class="scroll" style="max-height:30vh"><table><thead><tr><th>Time</th><th>Service</th><th>Action</th><th>Event</th><th>Break</th><th>Why</th><th></th></tr></thead><tbody>${lg || `<tr><td colspan="7" class="empty">no cues yet</td></tr>`}</tbody></table></div></div>`;
}

// ---------------------------------------------------------------------------------------------------
// View
// ---------------------------------------------------------------------------------------------------
export function viewMux() {
  const st = X.status || {}; const met = st.metrics || {}; const out = (X.cfg.output || {});
  const state = st.state || "stopped"; const sc = state === "running" ? "var(--ready)" : state === "error" ? "var(--danger)" : state === "starting" ? "var(--warn)" : "var(--t3)";
  const ind = (label, v) => `<div class="stat"><div class="n" style="font-size:14px;color:${v === "OK" || v === "ok" ? "var(--ready)" : v === "ERROR" || v === "underrun" ? "var(--danger)" : v === "preloading" ? "var(--warn)" : "var(--t2)"}">${e3(v || "—")}</div><div class="l">${label}</div></div>`;
  let body = "";
  if (X.tab === "services") body = viewServices(st);
  else if (X.tab === "encoders") body = viewEncoders();
  else if (X.tab === "tables") body = viewTables();
  else if (X.tab === "output") body = viewOutput(st);
  else if (X.tab === "streams") body = viewStreams();
  else if (X.tab === "scte35") body = viewScte35(st);
  else if (X.tab === "guide") body = `<div class="scroll" style="max-height:60vh"><table><thead><tr><th>Hour</th>${(X.guide ? X.guide.services : []).map(s => `<th>${e3(s.channel)}</th>`).join("")}</tr></thead><tbody>${X.guide ? Array.from({ length: 24 }, (_, h) => `<tr><td class="mono">${new Date(X.guide.from + h * 3600000).toLocaleTimeString([], { hour: "numeric" })}</td>${X.guide.services.map(s => `<td title="${e3((s.events[h] || {}).description || "")}">${e3((s.events[h] || {}).title || "")}</td>`).join("")}</tr>`).join("") : `<tr><td class="empty">loading…</td></tr>`}</tbody></table></div><div style="padding:8px 12px;color:var(--t3);font-size:11px">EIT-0..${(st.eitTables || 8) - 1} are rebuilt from the schedule every 10 minutes while the mux runs: the hour's show block title, else the first program; the ETT carries the programme list. NDI services announce their long name.</div>`;
  else body = `<pre class="scroll mono" style="max-height:60vh;margin:0;padding:10px;font-size:11px;white-space:pre-wrap">${e3(X.log.join("\n")) || "no log yet"}</pre>`;
  const encs = (st.encoders || []);
  return `<div class="grid" style="gap:12px">
    <div class="grid cols4" style="grid-template-columns:repeat(7,1fr)">
      <div class="stat"><div class="n" style="color:${sc}">${e3(state.toUpperCase())}</div><div class="l">MUX${st.autoStart ? " · AUTO" : ""}</div></div>
      <div class="stat"><div class="n">${met.muxBitrate ? mb(met.muxBitrate) : "—"}</div><div class="l">TRANSPORT ${st.transportBps && st.transportBps !== 19392658 ? "(" + mb(st.transportBps) + ")" : ""}</div></div>
      <div class="stat"><div class="n">${met.stuffing !== undefined ? mb(met.stuffing) : "—"}</div><div class="l">NULL PADDING</div></div>
      ${ind("ASI FIFO", met.fifo)}${ind("CONTINUITY", met.continuity)}${ind("PCR", met.pcr)}${ind("PAT/PMT", met.patPmt)}</div>
    <div class="panel"><h3>ATSC multiplexer — ${X.services.length} service(s) on ${e3(out.kind || "dektec")} <span class="right">${st.restarts ? b3(st.restarts + " restarts", "var(--warn)") : ""}${st.error ? `<span style="color:var(--danger)">${e3(st.error)}</span>` : ""}${encs.length ? encs.map(e => b3("enc " + e.service + (e.running ? "" : " down"), e.running ? "var(--ready)" : "var(--danger)")).join("") : ""}${(st.managedChannels || []).length ? b3((st.managedChannels || []).length + " NDI channel(s)", "var(--accent)") : ""}
        ${state === "running" || state === "starting" ? `<button class="danger small" onclick="pp.muxStop()">STOP</button><button class="small" onclick="pp.muxRestart()">restart</button>` : `<button class="success small" onclick="pp.muxStart()">START MUX</button>`}</span></h3>
      <div class="row" style="padding:6px 12px;border-bottom:1px solid var(--b0)">${["services", "encoders", "tables", "output", "streams", "scte35", "guide", "log"].map(t => `<button class="ghost small${X.tab === t ? " active" : ""}" onclick="pp.muxTab('${t}')">${t === "scte35" ? "SCTE-35" + ((st.scte35 || {}).enabled ? " ●" : "") : t}</button>`).join("")}<span class="fill"></span><span style="color:var(--t3);font-size:11px">packets ${met.packets ? Number(met.packets).toLocaleString() : "—"}</span></div>${body}</div></div>`;
}

// ---------------------------------------------------------------------------------------------------
// Handlers (window.pp.*)
// ---------------------------------------------------------------------------------------------------
const setPath = (obj, path, v) => { const parts = path.split("."); let o = obj; for (let k = 0; k < parts.length - 1; k++) { o[parts[k]] = o[parts[k]] || {}; o = o[parts[k]]; } o[parts[parts.length - 1]] = v; };
export const ppExtMux = {
  muxTab(t) { X.tab = t; loadMux(); },
  muxS35(c) { pp.PUT("/mux/scte35", c).then(r => { X.s35 = r; pp.render(); if (r && r.restartRequired) pp.toast("Saved: applies at the next mux start", "ok"); }).catch(e => pp.toast("SCTE-35: " + e, "err")); },
  muxS35Svc(id, c) { pp.PUT("/mux/scte35", { services: { [id]: c } }).then(r => { X.s35 = r; pp.render(); }).catch(e => pp.toast("SCTE-35: " + e, "err")); },
  muxCue(svc, action) {
    const go = () => pp.POST("/mux/scte35/cue", { service: svc, action, durationSec: action === "out" ? (X.cueDur ?? 120) : 0 }).then(r => { if (r && r.status) X.s35 = r.status; pp.toast("SCTE-35 " + action.toUpperCase() + " sent" + (r && r.sent && r.sent.some(x => !x.delivered) ? " (no injector running for some services: restart the mux)" : ""), "ok"); pp.render(); }).catch(e => pp.toast("SCTE-35: " + e, "err"));
    if (action === "out" && pp.confirmDo) pp.confirmDo("Send a SCTE-35 CUE OUT on " + (svc === "*" ? "every service with SCTE-35" : svc) + "? Downstream splicers switch to their local break.", go); else go();
  },
  muxReload() { loadMux(); },
  muxSet(i, path, v) { const s = X.services[i]; if (typeof v === "number" && Number.isNaN(v)) v = 0; setPath(s, path, v); if (path === "source.kind") { s.source = s.source || {}; if (v === "input" && !s.source.input) s.source.input = { type: "test" }; if (v === "ndi" && s.source.audio === undefined) s.source.audio = true; } if (path.startsWith("encoder.") && path !== "encoder.encoderPreset" && s.encoder) delete s.encoder.encoderPreset; X.dirty = true; pp.render(); },
  muxAdv(i) { X.adv[i] = !X.adv[i]; if (X.adv[i] && !X.caps) loadCaps(false); pp.render(); },
  muxApplyPreset(i, id) { const p = X.presets.find(x => x.id === id); if (!p || !X.services[i]) return; const s = X.services[i]; const en = Object.assign({}, s.encoder || {}); for (const [k, v] of Object.entries(p)) if (!["id", "label", "note"].includes(k)) en[k] = typeof v === "object" && v !== null ? JSON.parse(JSON.stringify(v)) : v; en.encoderPreset = id; delete en.audCodec; delete en.audBitrateKbps; delete en.audChannels; s.encoder = en; X.dirty = true; pp.toast("Preset " + id + " applied to " + s.longName + " (save services to persist)", "ok"); pp.render(); },
  muxAdd() { const n = X.services.length + 1; const ch = visibleChannels(); X.services.push({ id: "svc" + n, major: 25, minor: n, shortName: "25-" + n, longName: "Service " + n, source: { kind: ch.length ? "channel" : "input", channelId: ch.length ? ch[0].profile.id : "", input: { type: "test" } } }); X.dirty = true; pp.render(); },
  muxDel(i) { X.services.splice(i, 1); delete X.adv[i]; X.dirty = true; pp.render(); },
  muxSave() { pp.PUT("/mux/services", { services: X.services }).then(r => { X.dirty = false; X.services = r.services; X.alloc = r.allocation; pp.toast("Services saved", "ok"); loadMux(); }).catch(e => pp.toast("save failed: " + e, "err")); },
  muxPreset() { const n = parseInt(document.getElementById("muxPreset").value); pp.confirmDo("Apply the " + n + "-channel preset? Resolution and bitrate tiers change; services with real sources are kept.", () => pp.POST("/mux/preset", { count: n }).then(r => { pp.toast("Preset: " + r.kept + " kept, " + r.added + " added, " + r.dropped + " dropped", "ok"); X.dirty = false; loadMux(); })); },
  muxCfg(c) { pp.PUT("/mux/config", c).then(() => loadMux()).catch(e => pp.toast("config: " + e, "err")); },
  muxStart() { pp.POST("/mux/start").then(() => { pp.toast("Mux starting", "ok"); loadMux(); }).catch(e => pp.toast("start: " + e, "err")); },
  muxStop() { pp.confirmDo("Stop the multiplexer? The ASI output goes dark.", () => pp.POST("/mux/stop").then(loadMux)); },
  muxRestart() { pp.confirmDo("Restart the multiplexer?", () => pp.POST("/mux/restart").then(loadMux)); },
  muxValidate() { pp.POST("/mux/validate").then(r => pp.toast(r.compiled ? "PSIP tables compile (" + r.services + " services)" : "Tables: " + r.error, r.compiled ? "ok" : "err")); },
  muxRefreshCaps() { X.caps = null; pp.render(); loadCaps(true); },
  muxDektec() { pp.GET("/mux/dektec?refresh=1").then(d => { X.dektec = d; pp.render(); }).catch(e => pp.toast("tsdektec: " + e, "err")); },
  muxDryRun() { pp.POST("/mux/dryrun").then(r => { X.dry = Object.assign({}, X.dry || {}, r); pp.toast((r.warnings || []).length ? r.warnings.length + " warning(s) — see the command lines" : "Dry run ready", (r.warnings || []).length ? "err" : "ok"); pp.render(); }).catch(e => pp.toast("dry run: " + e, "err")); },
  muxPickFile(n) { X.file = n; pp.render(); },
  muxOvrSet(path, v) { X.ovr = X.ovr || {}; if (typeof v === "number" && Number.isNaN(v)) v = 0; setPath(X.ovr, path, v); X.ovrDirty = true; pp.render(); },
  muxOvrSave() { pp.PUT("/mux/tables/overrides", X.ovr || {}).then(o => { X.ovr = o; X.ovrDirty = false; pp.toast("Table overrides saved; tables rewritten", "ok"); loadTables(); loadMux(); }).catch(e => pp.toast("overrides: " + e, "err")); },
  muxCompile() { pp.POST("/mux/tables/compile").then(r => { X.dry = Object.assign({}, X.dry || {}, { compile: r }); pp.toast(r.compiled ? "All tables compile" : "Compile failed: " + r.error, r.compiled ? "ok" : "err"); loadTables(); }).catch(e => pp.toast("compile: " + e, "err")); },
  muxAnalyze() { const mode = (document.getElementById("muxAnMode") || {}).value || "tables"; const svc = (document.getElementById("muxAnSvc") || {}).value || ""; X.analyzing = true; X.analysis = null; pp.render(); pp.GET("/mux/tables/analyze?mode=" + mode + (svc ? "&service=" + encodeURIComponent(svc) : "")).then(r => { X.analysis = r; }).catch(e => { X.analysis = { error: String(e) }; }).finally(() => { X.analyzing = false; pp.render(); }); },
  // Facebook Live: fbSet / fbShow / fbSave / fbClear / fbOp come from facebooklive.js
  ...ppExtFacebook,
  // streams
  strShowHidden(v) { X.showHidden = v; pp.render(); },
  strOp(ch, name, op) { pp.POST("/channels/" + encodeURIComponent(ch) + "/outputs/" + encodeURIComponent(name) + "/" + op).then(() => { pp.toast(name + ": " + op, "ok"); setTimeout(loadStreams, 700); }).catch(e => pp.toast(op + " failed: " + e, "err")); },
  strToggle(ch, name, key, v) { const b = {}; b[key] = v; pp.PUT("/channels/" + encodeURIComponent(ch) + "/outputs/" + encodeURIComponent(name), b).then(() => { pp.toast(name + ": " + key + " " + (v ? "on" : "off"), "ok"); loadStreams(); }).catch(e => pp.toast("update failed: " + e, "err")); },
  strRemove(ch, name) { pp.confirmDo("Remove output '" + name + "' from " + ch + "? It stops if running.", () => pp.api("DELETE", "/channels/" + encodeURIComponent(ch) + "/outputs/" + encodeURIComponent(name)).then(() => { pp.toast("Removed " + name, "ok"); loadStreams(); }).catch(e => pp.toast("remove failed: " + e, "err"))); },
  strAddSet(k, v) { X.add[k] = v; if (k === "type") { const d = { rtmp: "rtmp://localhost/live/stream", srt: "srt://0.0.0.0:9000?mode=listener", udp: "udp://239.1.1.1:1234?pkt_size=1316", file: "C:/PlayPlay/record/" + (X.add.ch || "channel") + ".ts" }; if (d[v]) X.add.url = d[v]; X.add.preset = { dektec_asi: "atsc_mpeg2_ac3_hd", rtmp: "stream_h264_aac_720p", srt: "stream_h264_aac_1080p", udp: "h264_aac", file: "h264_aac" }[v] || X.add.preset; } if (k === "type" || k === "ch") pp.render(); },   // text fields store live (oninput) without a re-render
  strAdd() {
    const A = X.add; if (!A.ch) return pp.toast("pick a channel", "err");
    const o = { type: A.type, name: A.name || A.type, enabled: !!A.enabled, autoStart: !!A.autoStart };
    if (["rtmp", "srt", "udp", "file"].includes(A.type)) { o.url = A.url; if (!o.url) return pp.toast("url required", "err"); }
    if (["rtmp", "srt", "udp", "file", "dektec_asi"].includes(A.type)) { o.profile = { preset: A.preset || "h264_aac" }; if (A.kbps) o.profile.videoBitrateKbps = parseInt(A.kbps); if (A.type === "udp") o.options = { muxrate: 0 }; }
    if (A.type.startsWith("dektec")) o.options = Object.assign(o.options || {}, { port: A.port || 2 }, A.type === "dektec_asi" ? { muxrate: 19392658 } : { videoStd: A.videoStd || "auto", audioChannels: A.audioChannels || 2 });
    if (A.type === "decklink") { o.options = { deviceIndex: A.deviceIndex || 0, videoMode: A.videoMode || "auto", audioChannels: A.audioChannels || 2, keyer: A.keyer || "off", keyLevel: isNaN(A.keyLevel) ? 255 : (A.keyLevel ?? 255) }; if (A.deviceName) o.options.deviceName = A.deviceName; }
    if (A.type === "ndi") { o.sourceName = A.sourceName || ""; }
    pp.POST("/channels/" + encodeURIComponent(A.ch) + "/outputs/add", { output: o }).then(() => { pp.toast("Output " + o.name + " added to " + A.ch, "ok"); X.add.name = ""; loadStreams(); }).catch(e => pp.toast("add failed: " + e, "err"));
  },
};
