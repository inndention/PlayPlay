/* PLAY PLAY web dashboard — Spots console (auto-generated spots, ported from PlayPlay OBS newsroom +
   newsspots): recurring jobs with GEN markers, generate-now with script preview, the news scatter,
   recipes and configuration. All state lives in X; every input is state-bound so WS repaints are safe. */

const X = { tab: "jobs", jobs: [], markers: {}, recipes: [], status: {}, cfg: null, scatter: {}, markerList: [], edit: null, recipeEdit: null,
  gen: { recipe: "weather", channel: "", data: "", voice: "", market: "" }, preview: null, previewBusy: false, genBusy: false, loaded: false };
window.XS = X;
import { modelPicker, keyPlaceholder, keyNote } from "./aimodels.js";
const esc = s => String(s ?? "").replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
const bdg = (text, color) => `<span class="badge" style="color:${color};border-color:${color};background:color-mix(in srgb,${color} 16%,transparent)">${esc(text)}</span>`;
const DOWS = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"];
const MARK = { fresh: "var(--ready)", due: "var(--warn)", stale: "var(--danger)", off: "var(--t3)" };
const when = ms => { if (!ms) return "—"; const d = new Date(ms); const t = d.toLocaleTimeString([], { hour: "numeric", minute: "2-digit" }); return d.toDateString() === new Date().toDateString() ? t : DOWS[d.getDay()] + " " + t; };
const ago = ms => { if (!ms) return "never"; const m = Math.round((Date.now() - ms) / 60000); return m < 1 ? "just now" : m < 60 ? m + " min ago" : m < 1440 ? Math.round(m / 60) + " h ago" : Math.round(m / 1440) + " d ago"; };
const dur = s => { s = Math.round(s || 0); return Math.floor(s / 60) + ":" + String(s % 60).padStart(2, "0"); };
const cadence = j => { const e = j.every || {}; return e.kind === "clock" ? "at " + (e.times || []).join(", ") + ((j.dows || []).length ? " on " + j.dows.map(d => DOWS[d]).join("/") : " daily") : "every " + (e.minutes || 60) + " min"; };
const chans = () => (pp.S.channels || []).map(c => c.profile.id);
const chanSel = (cur, on, allowAll) => `<select class="small" onchange="${on}">${allowAll ? `<option value="" ${!cur ? "selected" : ""}>(all channels)</option>` : ""}${chans().map(c => `<option ${c === cur ? "selected" : ""}>${esc(c)}</option>`).join("")}</select>`;
// Area markets (Martinsville / Reidsville / Danville): the pick lists. "" = inherit (job -> recipe -> station default), "none" = generic.
const MKT = () => (X.cfg || {}).market || {};
const mktList = () => MKT().markets || [];
const mktLabel = k => (mktList().find(m => m.key === k) || {}).label || k;
const stationMkt = () => MKT().effectiveDefault || "";
const mktSel = (cur, on, station) => {
  const opts = station ? [["", "Generic (no market)"]] : [["", "Station default (" + (stationMkt() ? mktLabel(stationMkt()) : "generic") + ")"], ["none", "Generic (no market)"]];
  mktList().forEach(m => opts.push([m.key, m.label]));
  return `<select class="small" onchange="${on}">${opts.map(([v, l]) => `<option value="${esc(v)}" ${v === (cur || "") ? "selected" : ""}>${esc(l)}</option>`).join("")}</select>`;
};
const mktBlurb = m => !m ? "No market: recipes keep their generic Martinsville-flavoured defaults and the Weather place / coordinates below." :
  `<b>${esc(m.label)}</b> — ${esc(m.area)} · ${esc(m.region || "")}<br>Towns: ${esc((m.towns || []).join(", "))}<br>Voice: ${esc(m.voiceStyle || "")}`;
const recFields = ["label", "kicker", "voice", "subcategory", "category", "series", "type", "ttlMin", "targetSec", "source", "feeds", "prompt", "filter", "energy", "maxAgeHours"];
const recOver = r => { const o = {}; for (const k of recFields) if (r[k] !== undefined) o[k] = r[k]; if (r.marketOverride) o.market = r.marketOverride; return o; };
const recSel = (cur, on) => `<select class="small" onchange="${on}">${X.recipes.map(r => `<option value="${esc(r.key)}" ${r.key === cur ? "selected" : ""}>${esc(r.label)} (${esc(r.key)})</option>`).join("")}</select>`;

async function loadSpots() {
  const [j, r, st, c, m] = await Promise.all([pp.GET("/spots/jobs").catch(() => ({})), pp.GET("/spots/recipes").catch(() => ({})), pp.GET("/spots/status").catch(() => ({})), pp.GET("/spots/config").catch(() => null), pp.GET("/spots/markers").catch(() => ({}))]);
  X.jobs = j.jobs || []; X.markers = j.markers || {}; X.recipes = r.recipes || []; X.status = st; X.scatter = st.scatter || {}; if (c) X.cfg = c; X.markerList = m.markers || []; X.loaded = true;
  if (!X.gen.channel && chans().length) X.gen.channel = chans()[0];
  pp.render();
}
export function onEnterSpots(v) { if (v === "spots") loadSpots(); }
export function onWsSpots(m) {
  if (m.type === "spots") { X.status = m; X.markers = m.markers || X.markers; X.scatter = m.scatter || X.scatter; if (pp.S.view === "spots" && !X.edit && !X.recipeEdit) { pp.GET("/spots/jobs").then(j => { X.jobs = j.jobs || X.jobs; pp.render(); }).catch(() => pp.render()); } return true; }
  return false;
}

// ---------------------------------------------------------------------------------------------
export function viewSpots() {
  if (!X.loaded) loadSpots();
  const st = X.status || {}; const cur = st.current; const env = st.env || {};
  const chip = (ok, label) => bdg(label, ok ? "var(--ready)" : "var(--danger)");
  const head = `<div class="panel"><h3>Generated spots <span class="right">${chip(env.python, "python")}${chip(env.ttsScript && env.cardScript, "scripts")}${chip(env.ffmpeg, "ffmpeg")}${bdg((env.music || 0) + " music beds", "var(--t2)")}${bdg((env.backgrounds || 0) + " backgrounds", "var(--t2)")}
      ${cur ? bdg("rendering " + cur.recipe + " · " + cur.step, "var(--accent)") : bdg("idle", "var(--t3)")}${(st.queue || []).length ? bdg((st.queue || []).length + " queued", "var(--warn)") : ""}<button class="ghost small" onclick="pp.spotsReload()">&#8635;</button></span></h3>
    <div class="row" style="padding:6px 12px;border-bottom:1px solid var(--b0)">${["jobs", "scatter", "generate", "recipes", "config"].map(t => `<button class="ghost small${X.tab === t ? " active" : ""}" onclick="XS.tab='${t}';pp.render()">${t === "generate" ? "generate now" : t}</button>`).join("")}<span class="fill"></span>
      ${cur ? `<div class="meter" style="width:160px"><div class="bar" style="width:${cur.pct || 0}%;background:var(--accent)"></div></div><span class="mono" style="font-size:11px;color:var(--t2)">${esc(cur.headline || cur.recipe)} · ${esc(cur.step)}</span><button class="ghost small" onclick="pp.spotsCancel('${cur.id}')">&#10005;</button>` : ""}</div>
    ${X.tab === "jobs" ? viewJobs() : X.tab === "scatter" ? viewScatter() : X.tab === "generate" ? viewGenerate() : X.tab === "recipes" ? viewRecipes() : viewConfig()}</div>`;
  return `<div class="grid" style="gap:12px">${head}${viewRecent()}</div>`;
}

function viewJobs() {
  const rows = X.jobs.map(j => { const mk = X.markers[j.id] || {}; const rec = X.recipes.find(r => r.key === j.recipe) || {};
    return `<tr><td><input type="checkbox" ${j.enabled ? "checked" : ""} onchange="pp.spotsJobSet('${j.id}',{enabled:this.checked})"></td>
      <td><span title="${esc(mk.reason || mk.state || "")}" style="color:${MARK[mk.state] || "var(--t3)"};font-size:16px">&#10022;</span> ${bdg(mk.state || "?", MARK[mk.state] || "var(--t3)")}</td>
      <td class="mono" style="color:var(--accent);font-weight:700">GEN ${j.genNum || "?"}</td>
      <td><b>${esc(j.label || rec.label || j.recipe)}</b>${j.market ? " " + bdg(j.market === "none" ? "generic" : mktLabel(j.market), "var(--accent2)") : ""}<div style="color:var(--t3);font-size:10px">${esc(rec.label || j.recipe)} · ${esc(j.channel || "any channel")} · ${(mk.slotsPerWeek || 0)} slots/wk · TTL ${dur((j.ttlMin || 0) * 60)}</div></td>
      <td>${esc(cadence(j))}</td>
      <td class="mono" style="color:${j.due ? "var(--warn)" : "var(--t2)"}">${j.enabled ? (j.due ? "due now" : when(j.nextRunMs)) : "—"}<div style="font-size:10px;color:var(--t3)">last ${ago(j.lastRunMs)}</div></td>
      <td>${j.lastMediaId ? `<a href="#" onclick="pp.spotsOpenMedia('${j.lastMediaId}');return false">${esc(j.lastHeadline || "open")}</a>` : "<span style='color:var(--t3)'>no spot yet</span>"}${j.lastError ? `<div style="color:var(--danger);font-size:10px" title="${esc(j.lastError)}">&#9888; ${esc(j.lastError.slice(0, 80))}</div>` : ""}</td>
      <td class="row" style="flex-wrap:nowrap"><button class="small" onclick="pp.spotsRun('${j.id}')">&#9654; Run now</button><button class="ghost small" onclick="pp.spotsEdit('${j.id}')">&#9998;</button><button class="ghost small" onclick="pp.spotsJobDel('${j.id}')">&#10005;</button></td></tr>`; }).join("");
  return `<div class="scroll" style="max-height:48vh"><table><thead><tr><th>On</th><th>Marker</th><th>#</th><th>Job</th><th>Made</th><th>Next</th><th>Latest spot</th><th></th></tr></thead><tbody>${rows || `<tr><td colspan="8" class="empty">No jobs. Add one: a recipe, how often it is made, and which hours its GEN marker pins into the grid.</td></tr>`}</tbody></table></div>
    <div class="row" style="padding:8px 12px;border-top:1px solid var(--b0)"><button class="small" onclick="pp.spotsEdit('')">+ job</button><button class="ghost small" onclick="pp.spotsEnforce()">enforce markers</button><button class="ghost small" onclick="pp.POST('/spots/sweep').then(r=>pp.toast(r.changed+' expired/old spot(s) swept','ok'))">sweep expired</button><span class="fill"></span><span style="color:var(--t3);font-size:11px">&#10022; fresh = latest spot inside its window · due = older than the cadence · stale = nothing airable behind the pin · off = disabled</span></div>
    ${X.edit ? viewJobEditor() : ""}`;
}
function viewJobEditor() {
  const j = X.edit; const ev = j.every || {}; const set = (k, v) => `XS.edit.${k}=${v}`;
  const pl = (j.placements || []).map((p, i) => `<tr><td>${chanSel(p.channel, `XS.edit.placements[${i}].channel=this.value`)}</td><td><input value="${esc(Array.isArray(p.hours) ? p.hours.join(",") : "all")}" style="width:160px" placeholder="all or 5,9,13,17" onchange="XS.edit.placements[${i}].hours=this.value.trim()==='all'||!this.value.trim()?'all':this.value.split(',').map(x=>parseInt(x)).filter(x=>x>=0&&x<=23)"></td>
      <td><input type="number" min="0" max="59" value="${p.atMin || 0}" style="width:60px" onchange="XS.edit.placements[${i}].atMin=parseInt(this.value)||0"></td><td><input value="${esc((p.dows || []).join(","))}" style="width:90px" placeholder="all days" onchange="XS.edit.placements[${i}].dows=this.value.split(',').map(x=>parseInt(x)).filter(x=>x>=0&&x<=6)"></td><td><button class="ghost small" onclick="XS.edit.placements.splice(${i},1);pp.render()">&#10005;</button></td></tr>`).join("");
  return `<div class="body" style="border-top:1px solid var(--b0)"><div class="grid" style="gap:8px;grid-template-columns:1fr 1fr 1fr 1fr">
      <label class="f">Recipe ${recSel(j.recipe, set("recipe", "this.value"))}</label><label class="f">Label <input value="${esc(j.label || "")}" oninput="${set("label", "this.value")}"></label>
      <label class="f">Channel ${chanSel(j.channel, set("channel", "this.value"), true)}</label><label class="f">Voice <input value="${esc(j.voice || "")}" placeholder="recipe default (af_heart, am_michael…)" oninput="${set("voice", "this.value")}"></label>
      <label class="f" title="Localizes this job's spots: place names, feeds, schools, traffic centre, weather point, wording and voice notes">Market ${mktSel(j.market, set("market", "this.value"))}</label>
      <label class="f">Made <select onchange="XS.edit.every.kind=this.value;pp.render()"><option value="interval" ${ev.kind !== "clock" ? "selected" : ""}>every N minutes</option><option value="clock" ${ev.kind === "clock" ? "selected" : ""}>at clock times</option></select></label>
      ${ev.kind === "clock" ? `<label class="f">Times (24 h) <input value="${esc((ev.times || []).join(", "))}" placeholder="06:05, 12:05, 17:05" onchange="XS.edit.every.times=this.value.split(',').map(s=>s.trim()).filter(Boolean)"></label>` : `<label class="f">Minutes <input type="number" min="5" value="${ev.minutes || 60}" onchange="XS.edit.every.minutes=parseInt(this.value)||60"></label>`}
      <label class="f">Days <span class="row" style="gap:4px">${DOWS.map((d, i) => `<label style="font-size:11px"><input type="checkbox" ${(j.dows || []).includes(i) ? "checked" : ""} onchange="const s=new Set(XS.edit.dows||[]);this.checked?s.add(${i}):s.delete(${i});XS.edit.dows=[...s].sort()"> ${d}</label>`).join("")}</span></label>
      <label class="f">Target length (s) <input type="number" min="0" value="${j.targetSec || 0}" placeholder="recipe default" onchange="XS.edit.targetSec=parseInt(this.value)||0"></label>
      <label class="f" style="grid-column:span 2">Topic / material (free text, read when the recipe has no feed) <textarea rows="3" oninput="${set("data", "this.value")}">${esc(j.data || "")}</textarea></label>
      <label class="f" style="grid-column:span 2">Feeds (one URL per line; empty = recipe defaults) <textarea rows="3" onchange="XS.edit.feeds=this.value.split('\\n').map(s=>s.trim()).filter(s=>s.startsWith('http'))">${esc((j.feeds || []).join("\n"))}</textarea></label>
      <div style="grid-column:1/-1"><div class="fh">PLACEMENTS — which hours carry this job's GEN marker (a locked query block anchored at :atMin: it airs at the item boundary nearest that minute and never cuts a program; markers appear once the job has made its first spot)</div><table><thead><tr><th>Channel</th><th>Hours</th><th>At minute</th><th>Days (0=Sun)</th><th></th></tr></thead><tbody>${pl}</tbody></table>
        <button class="ghost small" onclick="XS.edit.placements.push({channel:XS.edit.channel||(pp.S.channels[0]||{profile:{}}).profile.id,hours:'all',atMin:5,dows:[]});pp.render()">+ placement</button></div></div>
    <div class="row" style="margin-top:8px"><button class="success" onclick="pp.spotsSave()">SAVE</button><button class="ghost" onclick="XS.edit=null;pp.render()">cancel</button></div></div>`;
}

// Headline News preview: the scenes in air order, each story with the photo it will render over and the words the voice reads.
function storiesPreview(p) {
  const scenes = p.scenes || [], storyScenes = scenes.filter(s => s.kind === "story");
  const intro = scenes.find(s => s.kind === "intro"), outro = scenes.find(s => s.kind === "outro");
  const st = p.stories || [], pics = st.filter(s => s.image).length;
  const io = (mark, s) => s ? `<div style="color:var(--t2);font-size:12px;margin:4px 0">${mark} ${esc(s.narration)}</div>` : "";
  const rows = st.map((s, i) => `<div style="display:grid;grid-template-columns:128px 1fr;gap:10px;padding:8px 0;border-top:1px solid var(--b0)">
      ${s.image ? `<img src="${esc(s.image)}" alt="" loading="lazy" referrerpolicy="no-referrer" style="width:128px;height:72px;object-fit:cover;border-radius:4px;background:#000">`
                : `<div style="width:128px;height:72px;border-radius:4px;background:var(--bg1);color:var(--t3);font-size:10px;display:flex;align-items:center;justify-content:center">no photo · station bed</div>`}
      <div style="min-width:0"><div style="font-size:10px;letter-spacing:.8px;text-transform:uppercase;color:var(--t3)">${i + 1} / ${st.length}${s.place ? " · " + esc(s.place) : ""}${s.category ? " · " + esc(s.category) : ""}${s.pubMs ? " · " + esc(ago(s.pubMs)) : ""}</div>
        <div style="font-weight:700;margin:2px 0">${s.link ? `<a href="${esc(s.link)}" target="_blank" rel="noopener" style="color:var(--t0);text-decoration:none">${esc(s.title)}</a>` : esc(s.title)}</div>
        ${s.detail ? `<div style="color:#e0b040;font-weight:600;margin-bottom:2px">${esc(s.detail)}</div>` : ""}
        <div style="color:var(--t1);font-size:12px;line-height:1.45">${esc((storyScenes[i] || {}).narration || s.summary || "")}</div></div></div>`).join("");
  // local spots (cinema, traffic, sports, featured home) name their package; the Headline News wording stays for the news render
  const noun = p.package ? (st.length === 1 ? "scene" : "scenes") : (st.length === 1 ? "story" : "stories");
  return `<div style="font-size:15px;font-weight:700">${esc(p.package || "Headline News")} · ${st.length} ${noun}, ${pics} with ${pics === 1 ? "its photo" : "their photos"}</div>
    <div class="scroll" style="max-height:60vh">${io("&#9654;", intro)}${rows}${io("&#9632;", outro)}</div>
    <div style="color:var(--t3);font-size:11px;margin-top:6px">${p.words || 0} words · ~${Math.round(p.estimatedSec || 0)} s${p.package ? "" : ` · ${p.items ?? 0} posts checked · newest posts first`}</div>`;
}

function viewGenerate() {
  const g = X.gen; const p = X.preview; const rec = X.recipes.find(r => r.key === g.recipe) || {};
  return `<div class="body"><div class="grid" style="gap:10px;grid-template-columns:1fr 1fr">
    <div class="grid" style="gap:8px;grid-template-columns:1fr 1fr">
      <label class="f">Recipe ${recSel(g.recipe, "XS.gen.recipe=this.value;XS.preview=null;pp.render()")}</label><label class="f">Channel ${chanSel(g.channel, "XS.gen.channel=this.value", true)}</label>
      <label class="f">Market ${mktSel(g.market, "XS.gen.market=this.value;XS.preview=null;pp.render()")}</label><label class="f">Voice <input value="${esc(g.voice)}" placeholder="${esc(rec.voice || "af_heart")}" oninput="XS.gen.voice=this.value"></label><div class="f" style="font-size:11px;color:var(--t3);align-self:end">${esc(rec.source || "")} · ${esc(rec.subcategory || "")} · ${rec.targetSec || 0}s · TTL ${dur((rec.ttlMin || 0) * 60)}</div>
      <label class="f" style="grid-column:1/-1">Topic / material (optional; replaces the feed for rss recipes) <textarea rows="6" oninput="XS.gen.data=this.value">${esc(g.data)}</textarea></label>
      <div class="row" style="grid-column:1/-1"><button class="small" ${X.previewBusy ? "disabled" : ""} onclick="pp.spotsPreview()">${X.previewBusy ? "writing…" : "Preview script"}</button><button class="primary" ${X.genBusy ? "disabled" : ""} onclick="pp.spotsGenerate()">${X.genBusy ? "Generating…" : "Generate spot"}</button><span style="color:var(--t3);font-size:11px">${esc(rec.prompt || "").slice(0, 160)}</span></div></div>
    <div class="panel" style="background:var(--bg3)"><h3>Script preview</h3><div class="body" style="padding:10px">${p && !p.error && p.market ? `<div style="color:var(--accent2);font-size:11px;font-weight:700;margin-bottom:4px" title="${esc(p.marketContext || "")}">&#9873; ${esc(p.market.label)} — ${esc(p.market.area)}</div>` : ""}${p ? (p.error ? `<span style="color:var(--danger)">${esc(p.error)}</span>` : (p.stories || []).length ? storiesPreview(p) : `<div style="font-size:16px;font-weight:700">${esc(p.headline)}</div><div style="color:var(--t2);margin:4px 0">${(p.lines || []).map(l => `&#9642; ${esc(l)}`).join("<br>")}</div><div style="white-space:pre-wrap;line-height:1.5">${esc(p.narration)}</div><div style="color:var(--t3);font-size:11px;margin-top:6px">${p.words || 0} words · ~${Math.round(p.estimatedSec || 0)} s · writer ${esc(p.writer || "")} · ${(p.sources || []).length} source(s) · ${p.items ?? 0} items</div>`) : `<span class="empty">Press Preview to see what the writer would say (no render).</span>`}</div></div></div></div>`;
}

function viewScatter() {
  const s = X.scatter || {}; const c = X.cfg ? X.cfg.scatter || {} : {}; const last = s.last || {};
  const places = X.markerList.filter(m => m.newsScatter).sort((a, b) => a.hour - b.hour || a.fixedOffsetSec - b.fixedOffsetSec);
  const byHour = {}; for (const m of places) (byHour[m.hour] = byHour[m.hour] || []).push(m);
  const f = (k, label, type = "text", extra = "") => `<label class="f">${label} <input type="${type}" value="${esc(c[k] ?? "")}" ${extra} onchange="pp.spotsCfg({scatter:{${k}:${type === "number" ? "parseFloat(this.value)||0" : "this.value"}}})"></label>`;
  return `<div class="body"><div class="grid" style="gap:10px;grid-template-columns:1fr 1fr">
    <div class="grid" style="gap:8px;grid-template-columns:1fr 1fr 1fr">
      <label class="f">Enabled <label style="text-transform:none"><input type="checkbox" ${c.enabled ? "checked" : ""} onchange="pp.spotsCfg({scatter:{enabled:this.checked}})"> generate + scatter every ${c.everyMin || 45} min</label></label>
      <label class="f">Channel ${chanSel(c.channel, "pp.spotsCfg({scatter:{channel:this.value}})")}</label>${f("perHour", "Per hour", "number", 'min="0" max="12"')}
      ${f("everyMin", "Every (min)", "number", 'min="5"')}${f("ttlMin", "Story TTL (min)", "number", 'min="30"')}${f("maxNewPerRun", "Max new per run", "number", 'min="1"')}
      ${f("minSec", "Min spot length (s)", "number", 'min="8"')}${f("voice", "Voice")}
      <label class="f" style="grid-column:1/-1">Feeds (one per line) <textarea rows="3" onchange="pp.spotsCfg({scatter:{feeds:this.value.split('\\n').map(s=>s.trim()).filter(s=>s.startsWith('http'))}})">${esc((c.feeds || []).join("\n"))}</textarea></label>
      <div class="row" style="grid-column:1/-1"><button class="small" ${s.busy ? "disabled" : ""} onclick="pp.spotsScatterGen()">Generate stories</button><button class="primary small" onclick="pp.spotsScatterApply()">Apply scatter now</button>
        <span style="color:var(--t3);font-size:11px">pool ${s.pool || 0} airable · ${s.pending || 0} rendering · ${s.seenCount || 0} seen · last fetch ${ago(s.lastGenMs)} (${last.considered || 0} considered, ${last.fresh || 0} fresh, ${last.queued || 0} queued) · last apply ${ago(s.lastApplyMs)} (${last.placed || 0} placed over ${last.slots || 0} h)</span></div></div>
    <div class="panel" style="background:var(--bg3)"><h3>Today's scatter placements (${places.length})</h3><div class="scroll" style="max-height:40vh"><table><tbody>${Object.keys(byHour).map(h => `<tr><td class="mono" style="width:60px">${String(h).padStart(2, "0")}:00</td><td>${byHour[h].map(m => `<div>:${String(Math.round(m.fixedOffsetSec / 60)).padStart(2, "0")} ${esc(m.title)} <span style="color:var(--t3)">${dur(m.durationSec)} · ${esc(m.channelId)}${m.dated ? "" : " (template)"}</span></div>`).join("")}</td></tr>`).join("") || `<tr><td class="empty">Nothing scattered yet. Stories are verbatim reads of unseen feed items over their photo; the apply pass fills the remaining hours of today, skipping the hour on air.</td></tr>`}</tbody></table></div></div></div></div>`;
}

function viewRecipes() {
  const e = X.recipeEdit;
  const rows = X.recipes.map(r => `<tr><td class="mono">${esc(r.key)}${r.overridden ? " " + bdg("edited", "var(--accent2)") : ""}${r.builtin === false ? " " + bdg("custom", "var(--accent)") : ""}</td><td><b>${esc(r.label)}</b><div style="color:var(--t3);font-size:10px">${esc(r.kicker)} · ${esc(r.series || "")}</div></td><td>${esc(r.source)}</td><td>${esc(r.voice)}</td><td>${esc(r.subcategory)} / ${esc(r.type)}</td><td class="mono">${r.targetSec}s</td><td class="mono">${dur((r.ttlMin || 0) * 60)}</td><td style="max-width:260px;color:var(--t3);font-size:10px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap" title="${esc((r.feeds || []).join("\n"))}">${esc((r.feeds || []).join(" · "))}</td><td><button class="ghost small" onclick="pp.spotsRecipeEdit('${esc(r.key)}')">&#9998;</button></td></tr>`).join("");
  return `<div class="scroll" style="max-height:48vh"><table><thead><tr><th>Key</th><th>Recipe</th><th>Source</th><th>Voice</th><th>Subcat / type</th><th>Target</th><th>TTL</th><th>Feeds</th><th></th></tr></thead><tbody>${rows}</tbody></table></div>
    ${e ? `<div class="body" style="border-top:1px solid var(--b0)"><div class="grid" style="gap:8px;grid-template-columns:1fr 1fr 1fr 1fr">
      <label class="f">Label <input value="${esc(e.label)}" oninput="XS.recipeEdit.label=this.value"></label><label class="f">Kicker <input value="${esc(e.kicker)}" oninput="XS.recipeEdit.kicker=this.value"></label><label class="f">Voice <input value="${esc(e.voice)}" oninput="XS.recipeEdit.voice=this.value"></label><label class="f">Subcategory <input value="${esc(e.subcategory)}" oninput="XS.recipeEdit.subcategory=this.value"></label>
      <label class="f">Target (s) <input type="number" value="${e.targetSec}" onchange="XS.recipeEdit.targetSec=parseInt(this.value)||60"></label><label class="f">TTL (min) <input type="number" value="${e.ttlMin}" onchange="XS.recipeEdit.ttlMin=parseInt(this.value)||720"></label><label class="f">Source <select onchange="XS.recipeEdit.source=this.value">${["rss", "weather", "history", "text"].map(s => `<option ${s === e.source ? "selected" : ""}>${s}</option>`).join("")}</select></label><label class="f">Keyword filter (regex) <input value="${esc(e.filter || "")}" oninput="XS.recipeEdit.filter=this.value"></label>
      <label class="f" style="grid-column:span 2">Feeds (one per line) <textarea rows="3" onchange="XS.recipeEdit.feeds=this.value.split('\\n').map(s=>s.trim()).filter(s=>s.startsWith('http'))">${esc((e.feeds || []).join("\n"))}</textarea></label>
      <label class="f" style="grid-column:span 2">Writer prompt (LLM only) <textarea rows="3" oninput="XS.recipeEdit.prompt=this.value">${esc(e.prompt || "")}</textarea></label>
      <label class="f">Max item age (h, 0 = any) <input type="number" min="0" step="1" value="${e.maxAgeHours ?? 0}" title="feed items older than this are never read as current news; undated items are kept" onchange="XS.recipeEdit.maxAgeHours=parseFloat(this.value)||0"></label>
      <label class="f" title="Pins this recipe to one market whatever the job or station default says">Market (this recipe) ${mktSel(e.market, "XS.recipeEdit.market=this.value")}</label></div>
      <div class="row" style="margin-top:8px"><button class="success" onclick="pp.spotsRecipeSave()">SAVE OVERRIDE</button><button class="ghost" onclick="pp.spotsRecipeReset('${esc(e.key)}')">reset to built-in</button><button class="ghost" onclick="XS.recipeEdit=null;pp.render()">cancel</button><span class="fill"></span><button class="ghost small" onclick="pp.spotsRecipeNew()">+ custom recipe</button></div></div>` : `<div class="row" style="padding:8px 12px;border-top:1px solid var(--b0)"><button class="small" onclick="pp.spotsRecipeNew()">+ custom recipe</button><span style="color:var(--t3);font-size:11px">Built-ins can be overridden field by field (settings spots.recipes); the deterministic writer reads feeds verbatim, the LLM writer uses the prompt.</span></div>`}`;
}

function viewConfig() {
  const c = X.cfg; if (!c) return `<div class="empty">loading…</div>`; const w = c.weather || {}, l = c.llm || {}, t = c.tts || {}, b = c.brand || {};
  const f = (grp, k, label, type = "text", extra = "") => `<label class="f">${label} <input type="${type}" value="${esc((c[grp] || {})[k] ?? "")}" ${extra} onchange="pp.spotsCfg({${grp}:{${k}:${type === "number" ? "parseFloat(this.value)||0" : "this.value"}}})"></label>`;
  return `<div class="body grid" style="gap:10px;grid-template-columns:1fr 1fr 1fr 1fr">
    <div class="fh" style="grid-column:1/-1">AREA MARKET — station default (localizes language, place names, sources and the weather point of every spot that does not name its own market)</div>
    <label class="f">Station default market ${mktSel(MKT().default, "pp.spotsCfg({market:{default:this.value}})", true)}</label><div class="f" style="grid-column:span 3;font-size:11px;color:var(--t2);line-height:1.5">${mktBlurb(mktList().find(m => m.key === stationMkt()))}</div>
    <div style="grid-column:1/-1;min-width:0;overflow-x:auto"><table><thead><tr><th>Market</th><th>Area</th><th>Towns</th><th>News feeds</th><th>Schools (MaxPreps)</th><th>Traffic</th><th>Cinema</th><th>Realty</th></tr></thead><tbody>${mktList().map(m => { const s = m.sources || {}; return `<tr><td><b>${esc(m.label)}</b>${m.key === stationMkt() ? " " + bdg("default", "var(--ready)") : ""}${m.customized ? " " + bdg("edited", "var(--accent2)") : ""}</td><td>${esc(m.area)}<div style="color:var(--t3);font-size:10px">${esc(m.county)} · ${esc(m.weather ? m.weather.place : "")} ${(+m.lat).toFixed(3)}, ${(+m.lon).toFixed(3)}</div></td><td style="max-width:200px;font-size:11px">${esc((m.towns || []).join(", "))}</td><td style="font-size:11px" title="${esc((s.feeds || []).join("\n"))}">${(s.feeds || []).length} feeds<div style="color:var(--t3);font-size:10px">${esc((s.feeds || []).map(f => { try { return new URL(f).hostname.replace(/^www\./, ""); } catch (_) { return f; } }).join(" · "))}</div></td><td style="font-size:11px">${esc((s.schools || []).join(", "))}</td><td style="font-size:11px">${s.trafficConfigured === false ? bdg("not set up", "var(--warn)") : ""} ${esc(s.traffic || "")}</td><td style="font-size:11px">${s.cinema === "" ? bdg("none", "var(--t3)") : s.cinema === "default" ? "recipe" : esc(s.cinema)}</td><td style="font-size:11px">${s.realty === "" ? bdg("none", "var(--t3)") : s.realty === "default" ? "recipe" : esc(s.realty)}</td></tr>`; }).join("")}</tbody></table>
      <div style="color:var(--t3);font-size:11px;margin-top:4px">Markets are data (settings <span class="mono">spots.markets</span> amends or adds one: towns, feeds, schools, traffic, wording, voice notes). Pick per job, per recipe, or per one-off on the Generate tab; a market's weather point beats the Weather place below for the spots it makes.</div></div>
    <div class="fh" style="grid-column:1/-1">WEATHER (Open-Meteo)</div>${f("weather", "place", "Place name")}${f("weather", "lat", "Latitude", "number", 'step="0.0001"')}${f("weather", "lon", "Longitude", "number", 'step="0.0001"')}<label class="f">Units <select onchange="pp.spotsCfg({weather:{units:this.value}})"><option value="us" ${w.units !== "metric" ? "selected" : ""}>°F / mph / in</option><option value="metric" ${w.units === "metric" ? "selected" : ""}>°C / km/h / mm</option></select></label>
    <div class="fh" style="grid-column:1/-1">LLM WRITER (optional, OpenRouter-compatible; templates are used when off or on any failure)</div>
    <label class="f">Enabled <label style="text-transform:none"><input type="checkbox" ${l.enabled ? "checked" : ""} onchange="pp.spotsCfg({llm:{enabled:this.checked}})"> use the model ${l.hasKey ? "(key set)" : "(no key yet)"}</label></label><label class="f">OpenRouter key (shared) <input type="password" placeholder="${esc(keyPlaceholder())}" onchange="if(this.value)pp.spotsCfg({llm:{apiKey:this.value}})"><span style="font-size:10px;color:var(--t3);text-transform:none">${esc(keyNote())}</span></label>${modelPicker("spots", l.model || "", "pp.spotsCfg({llm:{model:this.value}})", "Writer model")}
    <div class="fh" style="grid-column:1/-1">VOICE (spot_tts.py: Kokoro when installed, else Windows SAPI)</div>
    <label class="f">Engine <select onchange="pp.spotsCfg({tts:{engine:this.value}})">${["auto", "kokoro", "sapi"].map(e => `<option ${e === (t.engine || "auto") ? "selected" : ""}>${e}</option>`).join("")}</select></label>${f("tts", "kokoroDir", "Kokoro dir (venv / models)")}${f("tts", "defaultVoice", "Default voice")}${f("tts", "speed", "Speed", "number", 'step="0.05" min="0.5" max="2"')}
    ${f("tts", "python", "Python (blank = auto)")}<div class="f" style="font-size:11px;color:var(--t3);align-self:end;grid-column:span 3">python: ${esc(t.pythonResolved || "")} · ffmpeg: ${esc(c.ffmpegResolved || "")}</div>
    <div class="fh" style="grid-column:1/-1">RENDER + RETENTION</div>
    <label class="f">FFmpeg path (blank = auto) <input value="${esc(c.ffmpeg || "")}" onchange="pp.spotsCfg({ffmpeg:this.value})"></label><label class="f">Keep spots (days) <input type="number" min="0.5" step="0.5" value="${c.keepDays}" onchange="pp.spotsCfg({keepDays:parseFloat(this.value)||3})"></label>${f("brand", "accent", "Accent colour")}${f("brand", "ink", "Ink colour")}
    <label class="f">Music beds folder <input value="${esc(c.musicDir || "")}" onchange="pp.spotsCfg({musicDir:this.value})"></label><label class="f">Backgrounds folder <input value="${esc(c.backgroundsDir || "")}" onchange="pp.spotsCfg({backgroundsDir:this.value})"></label><div class="f" style="font-size:11px;color:var(--t3);align-self:end;grid-column:span 2">output: ${esc(c.outputDir || "")} · music files named "… energy 7.mp3" are matched to each recipe's energy; backgrounds named &lt;recipe&gt;.jpg or default.jpg become the photo plane</div></div>`;
}

function viewRecent() {
  const rec = (X.status || {}).recent || []; if (!rec.length) return "";
  return `<div class="panel"><h3>Recent generations</h3><div class="scroll" style="max-height:26vh"><table><thead><tr><th>When</th><th>Recipe</th><th>Headline</th><th>Length</th><th>Engine</th><th>Result</th></tr></thead><tbody>${rec.slice(0, 20).map(g => `<tr><td class="mono" style="color:var(--t3)">${when(g.finishedMs)}</td><td>${esc(g.recipe)}${g.origin === "scatter" ? " " + bdg("scatter", "var(--accent2)") : ""}</td><td>${g.mediaId ? `<a href="#" onclick="pp.spotsOpenMedia('${g.mediaId}');return false">${esc(g.headline)}</a>` : esc(g.headline)}</td><td class="mono">${g.durationSec ? dur(g.durationSec) : "—"}</td><td>${esc(g.ttsEngine || "")}</td><td>${g.ok ? bdg("done", "var(--ready)") : g.step === "skipped" ? bdg("skipped", "var(--t3)") : `<span style="color:var(--danger)" title="${esc(g.error)}">${esc((g.error || "failed").slice(0, 90))}</span>`}</td></tr>`).join("")}</tbody></table></div></div>`;
}

// ---------------------------------------------------------------------------------------------
export const ppExtSpots = {
  spotsReload() { loadSpots(); },
  spotsOpenMedia(id) { pp.nav("library"); setTimeout(() => { if (pp.libOpen) pp.libOpen(id); }, 350); },
  spotsJobSet(id, patch) { pp.PUT("/spots/jobs/" + id, patch).then(() => loadSpots()); },
  spotsJobDel(id) { const j = X.jobs.find(x => x.id === id) || {}; pp.confirmDo("Delete job \"" + (j.label || id) + "\"? Its GEN markers are removed from the grid.", () => pp.api("DELETE", "/spots/jobs/" + id).then(() => loadSpots())); },
  spotsEdit(id) { const j = X.jobs.find(x => x.id === id); X.edit = j ? JSON.parse(JSON.stringify(j)) : { recipe: X.recipes.length ? X.recipes[0].key : "weather", label: "", channel: chans()[0] || "", enabled: false, every: { kind: "interval", minutes: 60, times: [] }, dows: [], voice: "", data: "", feeds: [], targetSec: 0, market: "", placements: [{ channel: chans()[0] || "", hours: "all", atMin: 5, dows: [] }] }; if (!X.edit.every) X.edit.every = { kind: "interval", minutes: 60, times: [] }; pp.render(); },
  spotsSave() { const j = X.edit; const body = { recipe: j.recipe, label: j.label, channel: j.channel, enabled: j.enabled, every: j.every, dows: j.dows, voice: j.voice, data: j.data, feeds: j.feeds, targetSec: j.targetSec, market: j.market || "", placements: j.placements };
    (j.id ? pp.PUT("/spots/jobs/" + j.id, body) : pp.POST("/spots/jobs", body)).then(r => { if (r.error) { pp.toast(r.error, "err"); return; } X.edit = null; pp.toast("Job saved; markers refreshed", "ok"); loadSpots(); }); },
  spotsRun(id) { pp.toast("Generating… (a minute or two)", "ok"); pp.POST("/spots/jobs/" + id + "/run", { wait: true }).then(r => { pp.toast(r.ok ? "Spot ready: " + r.headline : "Spot failed: " + (r.error || "?"), r.ok ? "ok" : "err"); loadSpots(); }).catch(e => { pp.toast("Spot failed: " + (e.message || e), "err"); loadSpots(); }); },
  spotsCancel(id) { pp.api("DELETE", "/spots/generation/" + id).then(() => loadSpots()); },
  spotsEnforce() { pp.POST("/spots/markers/enforce").then(r => { pp.toast("Markers: " + r.placed + " placed, " + r.updated + " updated, " + r.removed + " removed", "ok"); loadSpots(); }); },
  spotsPreview() { X.previewBusy = true; X.preview = null; pp.render(); pp.POST("/spots/preview-script", { recipe: X.gen.recipe, channel: X.gen.channel, data: X.gen.data, market: X.gen.market }).then(p => { X.preview = p; }).catch(e => { X.preview = { error: e.message || String(e) }; }).finally(() => { X.previewBusy = false; pp.render(); }); },
  spotsGenerate() { X.genBusy = true; pp.render(); pp.POST("/spots/generate", { recipe: X.gen.recipe, channel: X.gen.channel, data: X.gen.data, voice: X.gen.voice, market: X.gen.market, wait: true }).then(r => { pp.toast(r.ok ? "Spot ready: " + r.headline + " (" + dur(r.durationSec) + ")" : "Spot failed: " + (r.error || "?"), r.ok ? "ok" : "err"); }).catch(e => pp.toast("Spot failed: " + (e.message || e), "err")).finally(() => { X.genBusy = false; loadSpots(); }); },
  spotsCfg(patch) { pp.PUT("/spots/config", patch).then(c => { X.cfg = c; pp.render(); }); },
  spotsScatterGen() { pp.POST("/spots/scatter/generate").then(r => { pp.toast(r.ok ? r.queued + " story spot(s) queued (" + r.fresh + " fresh of " + r.considered + ")" : r.error, r.ok ? "ok" : "err"); loadSpots(); }).catch(e => pp.toast(String(e.message || e), "err")); },
  spotsScatterApply() { const c = X.cfg ? X.cfg.scatter || {} : {}; pp.POST("/spots/scatter/apply", { channel: c.channel, perHour: c.perHour }).then(r => { pp.toast("Scattered " + r.placed + " placement(s) over " + r.slots + " hour(s) from a pool of " + r.pool, "ok"); loadSpots(); }); },
  spotsRecipeEdit(key) { const r = X.recipes.find(x => x.key === key); if (r) { X.recipeEdit = JSON.parse(JSON.stringify(r)); X.recipeEdit.market = r.marketOverride || ""; pp.render(); } },
  spotsRecipeNew() { const key = prompt("Recipe key (letters, digits, underscore)"); if (!key) return; const k = key.toLowerCase().replace(/[^a-z0-9_]+/g, "_"); X.recipeEdit = { key: k, label: key, kicker: key.toUpperCase(), voice: "af_heart", subcategory: "News", category: "local_news", series: key, type: "news", ttlMin: 720, targetSec: 60, source: "rss", feeds: [], prompt: "", filter: "", energy: 5, maxAgeHours: 48, builtin: false }; pp.render(); },
  spotsRecipeSave() { const e = X.recipeEdit; const over = {}; for (const k of recFields) if (e[k] !== undefined) over[k] = e[k]; if (e.market) over.market = e.market;
    const current = X.recipes.reduce((a, r) => { if ((r.overridden || r.builtin === false) && r.key !== e.key) a[r.key] = recOver(r); return a; }, {}); current[e.key] = over;
    pp.PUT("/spots/config", { recipes: current }).then(() => { X.recipeEdit = null; pp.toast("Recipe saved", "ok"); loadSpots(); }); },
  spotsRecipeReset(key) { const current = X.recipes.reduce((a, r) => { if ((r.overridden || r.builtin === false) && r.key !== key) a[r.key] = recOver(r); return a; }, {}); pp.PUT("/spots/config", { recipes: current }).then(() => { X.recipeEdit = null; pp.toast("Recipe reset", "ok"); loadSpots(); }); },
};
