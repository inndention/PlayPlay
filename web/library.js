/* PLAY PLAY web dashboard — Library console: the media library with facets, thumbnails, bulk edits,
   the metadata editor, transcription (faster-whisper) and transcript-driven segment editing, plus the
   ingest pipeline (upload, folder scan, watch folders, removable drives, jobs). */

import { syncSegEditor, segEditorHost, segEditorTranscriptReady } from "./segments.js";

const L = {
  rows: [], facets: {}, total: 0, q: "", type: "", status: "", client: "", series: "", flag: "", sort: "recent", view: "list", sel: new Set(),
  edit: null, editTab: "meta", transcript: null, segs: null, wordA: -1, wordB: -1, segLabel: "",
  jobs: [], pending: 0, tjobs: [], watch: [], removable: [], cfg: {}, tcfg: {}, levels: [], env: null, showIngest: false, collections: [],
};
window.L = L;
const q$ = s => document.querySelector(s);
const esc2 = s => String(s ?? "").replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
const fmtC = s => { s = Math.max(0, Math.floor(s || 0)); const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), x = s % 60; return (h ? h + ":" + (m < 10 ? "0" : "") + m : m) + ":" + (x < 10 ? "0" : "") + x; };
const fmtT = s => { const ms = Math.round(((s || 0) % 1) * 1000); return fmtC(s) + "." + String(ms).padStart(3, "0").slice(0, 2); };
const fmtB2 = b => b > 1e9 ? (b / 1e9).toFixed(2) + " GB" : b > 1e6 ? (b / 1e6).toFixed(1) + " MB" : b > 1e3 ? Math.round(b / 1e3) + " kB" : (b || 0) + " B";
const TYPES = ["program", "ad", "promo", "station_id", "bumper", "filler", "psa", "image", "live", "video"];

// ---- Upload specifics: what the operator chooses BEFORE uploading (applied to every file of the upload) --------------------------
// Remembered in this browser; sent with each file as ?meta=<JSON> (the Supervisor keeps only the keys it knows).
const UPKEY = "pp.uploadSpecifics";
const U = Object.assign({ type: "program", title: "", client: "", series: "", category: "", subcategory: "", tags: "", notes: "", priority: 0, status: "approved", channels: [], from: "", until: "", episodeDate: "", hold: false, open: true },
  (() => { try { return JSON.parse(localStorage.getItem(UPKEY) || "{}"); } catch { return {}; } })());
U.staged = []; U.title = "";
const upSave = () => { try { const { staged, title, ...rest } = U; localStorage.setItem(UPKEY, JSON.stringify(rest)); } catch {} };
const uniq = k => [...new Set((L.rows || []).map(m => (m[k] || "").trim()).filter(Boolean))].sort().slice(0, 200);
function upMeta(title) {
  const m = {}; const t = (title ?? U.title).trim(); if (t) m.title = t;
  for (const k of ["client", "series", "category", "subcategory", "notes", "episodeDate"]) if (String(U[k] || "").trim()) m[k] = String(U[k]).trim();
  const tags = String(U.tags || "").split(",").map(x => x.trim().toLowerCase()).filter(Boolean); if (tags.length) m.tags = tags;
  if (+U.priority) m.priority = +U.priority;
  m.status = U.status === "review" ? "review" : "approved";
  if ((U.channels || []).length) m.allowedChannels = U.channels;
  const f = U.from ? Date.parse(U.from) : 0, u = U.until ? Date.parse(U.until) : 0; if (f > 0) m.availableStartMs = f; if (u > 0) m.availableEndMs = u;
  return m;
}
function upSummary() {
  const m = upMeta(""), bits = [bdg2(U.type, tcol(U.type))];
  if (m.client) bits.push("client " + esc2(m.client)); if (m.series) bits.push("series " + esc2(m.series)); if (m.category) bits.push(esc2(m.category) + (m.subcategory ? " / " + esc2(m.subcategory) : ""));
  if (m.tags) bits.push("#" + m.tags.map(esc2).join(" #")); if (m.allowedChannels) bits.push("only " + m.allowedChannels.map(esc2).join(", ")); if (m.status === "review") bits.push("needs review");
  if (m.availableEndMs) bits.push("expires " + new Date(m.availableEndMs).toLocaleString([], { dateStyle: "short", timeStyle: "short" }));
  return bits.join(" · ");
}
function uploadSpecifics() {
  const inp = (k, label, ph, list, type = "text") => `<label class="f">${label} <input type="${type}" ${list ? `list="up_${list}"` : ""} placeholder="${esc2(ph || "")}" value="${esc2(U[k] ?? "")}" oninput="pp.upSet('${k}',this.value,true)"></label>`;
  const dl = (id, k) => `<datalist id="up_${id}">${uniq(k).map(v => `<option value="${esc2(v)}">`).join("")}</datalist>`;
  const chans = (pp.S.channels || []).map(c => c.profile.id);
  return `<div class="row" style="margin-bottom:6px"><b style="font-size:11px;letter-spacing:1px;color:var(--t2)">UPLOAD SPECIFICS</b><span class="fill"></span><button class="ghost small" onclick="pp.upSet('open',!XU.open)">${U.open ? "hide" : "edit"}</button><button class="ghost small" title="forget the remembered choices" onclick="pp.upReset()">reset</button></div>
    <div style="font-size:11px;color:var(--t1);margin-bottom:6px">${upSummary()}</div>
    ${U.open ? `<div class="grid" style="gap:8px;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));margin-bottom:8px">
      <label class="f">Import as <select onchange="pp.upSet('type',this.value)">${TYPES.map(t => `<option ${t === U.type ? "selected" : ""}>${t}</option>`).join("")}</select></label>
      ${inp("client", "Client", "", "client")}${inp("series", "Series", "", "series")}${inp("category", "Category", "", "category")}
      ${inp("subcategory", "Subcategory", "", "subcategory")}${inp("tags", "Tags (comma)", "news, local")}${inp("notes", "Notes", "")}
      <label class="f">Priority <input type="number" min="-10" max="10" value="${+U.priority || 0}" oninput="pp.upSet('priority',this.value,true)"></label>
      <label class="f">Status <select onchange="pp.upSet('status',this.value)"><option value="approved" ${U.status !== "review" ? "selected" : ""}>approved (airable now)</option><option value="review" ${U.status === "review" ? "selected" : ""}>needs review first</option></select></label>
      ${inp("episodeDate", "Episode date", "", "", "date")}${inp("from", "Available from", "", "", "datetime-local")}${inp("until", "Expires", "", "", "datetime-local")}
      <label class="f" style="grid-column:1/-1">${U.channels.length ? "Only on these channels" : "Channels: all (tick to restrict)"}<span class="row">${chans.map(c => `<label><input type="checkbox" ${U.channels.includes(c) ? "checked" : ""} onchange="pp.upChan(${jq2(c)},this.checked)"> ${esc2(c)}</label>`).join("")}</span></label>
      <label class="f" style="grid-column:span 2">Title for these files <input value="${esc2(U.title)}" placeholder="blank = the file name" oninput="XU.title=this.value"></label>
      <label style="grid-column:1/-1"><input type="checkbox" ${U.hold ? "checked" : ""} onchange="pp.upSet('hold',this.checked)"> Hold files here until I press Upload (check the list and set titles first)</label></div>${dl("client", "client")}${dl("series", "programSeries")}${dl("category", "category")}${dl("subcategory", "subcategory")}` : ""}`;
}
function stagedList() {
  if (!U.staged.length) return "";
  return `<div style="margin:8px 0"><table><thead><tr><th>File</th><th>Size</th><th>Title</th><th></th></tr></thead><tbody>${U.staged.map((x, i) => `<tr><td title="${esc2(x.file.name)}">${esc2(x.file.name)}</td><td class="mono">${fmtB2(x.file.size)}</td><td><input style="width:100%" placeholder="${esc2(x.file.name.replace(/\.[^.]+$/, ""))}" value="${esc2(x.title)}" oninput="XU.staged[${i}].title=this.value"></td><td><button class="ghost small" onclick="pp.upStageDel(${i})">&#10005;</button></td></tr>`).join("")}</tbody></table>
    <div class="row" style="margin-top:6px"><button class="primary" onclick="pp.upStageGo()">Upload ${U.staged.length} file${U.staged.length === 1 ? "" : "s"}</button><button class="ghost small" onclick="pp.upStageClear()">clear</button><span style="color:var(--t3);font-size:11px">${upSummary()}</span></div></div>`;
}
window.XU = U;
const bdg2 = (text, color) => `<span class="badge" style="color:${color};border-color:${color};background:color-mix(in srgb,${color} 16%,transparent)">${esc2(text)}</span>`;
const jq2 = v => esc2(JSON.stringify(String(v ?? "")));   // a value as a JS string literal for an inline handler (esc2 alone turns back into a real quote when the attribute is decoded)
const tcol = t => ({ ad: "var(--ad)", promo: "var(--accent2)", station_id: "var(--gfx)", bumper: "var(--bumper)", live: "var(--live)", image: "var(--gfx)", filler: "var(--preroll)", psa: "var(--preroll)" }[t] || "var(--video)");

async function loadLibrary() {
  const p = new URLSearchParams(); if (L.q) p.set("q", L.q); if (L.type) p.set("type", L.type); if (L.status) p.set("status", L.status); if (L.client) p.set("client", L.client); if (L.series) p.set("series", L.series);
  if (L.flag === "missing") p.set("missing", "1"); if (L.flag === "archived") p.set("archived", "only"); if (L.flag === "review") p.set("review", "1"); p.set("sort", L.sort); p.set("limit", "1000");
  const d = await pp.GET("/library?" + p.toString()).catch(() => null); if (!d) return;
  L.rows = d.media || []; L.facets = d.facets || {}; L.total = d.total || 0; L.totalSec = d.totalSec; L.totalBytes = d.totalBytes; pp.render();
}
async function loadIngest() {
  const [j, w, c, t, tl, cols] = await Promise.all([pp.GET("/ingest/jobs?limit=60").catch(() => ({})), pp.GET("/ingest/watch").catch(() => ({})), pp.GET("/ingest/config").catch(() => ({})), pp.GET("/transcribe/jobs").catch(() => ({})), pp.GET("/transcribe/levels").catch(() => ({})), pp.GET("/media/collections").catch(() => ({}))]);
  L.jobs = j.jobs || []; L.pending = j.pending || 0; L.watch = w.folders || []; L.removable = w.removable || []; L.cfg = c; L.tjobs = t.jobs || []; L.levels = tl.levels || []; L.tcfg = tl.config || {}; L.collections = cols.collections || []; pp.render();
}
// Inline handlers (Sort, refresh, clear finished) call these by bare name; this file is a module, so they must be globals.
window.loadLibrary = loadLibrary; window.loadIngest = loadIngest;
export function onEnterLibrary(v) { if (v === "library") { pp.S.ingestType = U.type; loadLibrary(); loadIngest(); pp.GET("/media/upload-info").then(i => { pp.S.uploadInfo = i; pp.render(); }).catch(() => {}); } }
export function onWsLibrary(m) {
  if (m.type === "ingest") { L.jobs = m.jobs || L.jobs; L.pending = m.pending || 0; return true; }
  if (m.type === "transcribe") { L.tjobs = m.jobs || L.tjobs; return true; }
  if (m.type === "mediaIngested" || m.type === "mediaScanDone") { if (pp.S.view === "library") loadLibrary(); return true; }
  if (m.type === "transcriptReady") {
    // The open editor reloads in place (it used to jump to the transcript tab, pulling the segment editor away mid-edit).
    // On the segments tab the editor loads the words itself and the list gets the segments' spoken text.
    if (L.edit && L.edit.id === m.mediaId) {
      if (L.editTab !== "segments") pp.libOpen(m.mediaId, L.editTab);
      else {
        segEditorTranscriptReady(m.mediaId);
        pp.GET("/media/" + m.mediaId).then(x => { if (L.edit && L.edit.id === x.id) { L.edit = x; pp.render(); } }).catch(() => {});
        pp.GET("/media/" + m.mediaId + "/segments").then(s => { if (L.edit && L.edit.id === m.mediaId) { L.segs = s; pp.render(); } }).catch(() => {});
      }
    } else if (pp.S.view === "library") loadLibrary();
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// View
// ---------------------------------------------------------------------------
export function viewLibrary() {
  const S = pp.S; const f = L.facets; const chips = (obj, key, cur) => Object.entries(obj || {}).sort((a, b) => b[1] - a[1]).slice(0, 14).map(([k, n]) => `<button class="ghost small${cur === k ? " active" : ""}" onclick="pp.libFilter('${key}',${JSON.stringify(cur === k ? "" : k).replace(/"/g, "&quot;")})">${esc2(k)} <small>${n}</small></button>`).join("");
  const selN = L.sel.size;
  const row = m => { const miss = m.missing, st = m.status || "approved", tr = m.transcript || {}; const sel = L.sel.has(m.id);
    return `<tr draggable="true" ondragstart="pp.mediaDrag(event,${jq2(m.id)})" class="${sel ? "onair" : ""}${miss ? " past" : ""}" ondblclick="pp.libOpen(${jq2(m.id)})" oncontextmenu="pp.mediaMenu(event,${jq2(m.id)})"><td><input type="checkbox" ${sel ? "checked" : ""} onclick="event.stopPropagation();pp.libSel(${jq2(m.id)},this.checked)"></td>
      <td><div class="thumb" onclick="pp.libOpen(${jq2(m.id)})">${m.thumbnail ? `<img src="/thumbs/${esc2(m.id)}.jpg?${(m.transcript && m.transcript.atMs) || ""}" loading="lazy" alt="">` : `<span>${m.type === "live" ? "LIVE" : "&#9654;"}</span>`}</div></td>
      <td><b style="cursor:pointer" onclick="pp.libOpen(${jq2(m.id)})">${esc2(m.title)}</b><div style="color:var(--t3);font-size:10px">${esc2(m.programSeries || "")}${m.episodeDate ? " · " + esc2(m.episodeDate) : ""}${m.client ? " · " + esc2(m.client) : ""}</div></td>
      <td>${bdg2(m.type, tcol(m.type))}${st !== "approved" ? " " + bdg2(st, st === "review" ? "var(--warn)" : "var(--danger)") : ""}${miss ? " " + bdg2("missing", "var(--danger)") : ""}${m.archived ? " " + bdg2("archived", "var(--t3)") : ""}</td>
      <td class="mono">${fmtC(m.duration)}</td><td class="mono" style="color:var(--t2)">${m.width ? m.width + "x" + m.height : ""}</td><td class="mono" style="color:var(--t2)">${m.fileSizeBytes ? fmtB2(m.fileSizeBytes) : ""}</td>
      <td>${tr.status === "done" ? bdg2(tr.level + " ✓", "var(--ready)") : tr.status === "running" || tr.status === "queued" ? bdg2(tr.status, "var(--accent)") : tr.status === "failed" ? bdg2("failed", "var(--danger)") : `<span style="color:var(--t3)">—</span>`}${(m.segments || []).length ? " " + bdg2((m.segments || []).length + " seg", "var(--accent2)") : ""}</td>
      <td class="mono">${m.playCount || 0}</td><td class="mono" style="color:var(--t3);max-width:260px" title="${esc2(m.path)}">${esc2(m.path)}</td></tr>`; };
  const card = m => `<div draggable="true" ondragstart="pp.mediaDrag(event,${jq2(m.id)})" class="mcard${L.sel.has(m.id) ? " sel" : ""}${m.missing ? " miss" : ""}" onclick="pp.libOpen(${jq2(m.id)})" oncontextmenu="pp.mediaMenu(event,${jq2(m.id)})"><div class="mon">${m.thumbnail ? `<img src="/thumbs/${esc2(m.id)}.jpg" loading="lazy" alt="">` : `<div class="empty">${m.type === "live" ? "LIVE" : esc2(m.type)}</div>`}<span class="dur mono">${fmtC(m.duration)}</span><input type="checkbox" ${L.sel.has(m.id) ? "checked" : ""} onclick="event.stopPropagation();pp.libSel(${jq2(m.id)},this.checked)"></div><div class="t">${esc2(m.title)}</div><div class="s">${bdg2(m.type, tcol(m.type))} ${m.missing ? bdg2("missing", "var(--danger)") : ""}</div></div>`;
  const ingest = L.showIngest ? viewIngestPanels() : "";
  return `<div class="grid" style="grid-template-columns:230px minmax(280px,1fr)${L.edit ? (L.editTab === "segments" ? " minmax(640px,56vw)" : " 480px") : ""}">
    <div class="panel"><h3>Library <span class="right">${f.all || 0}</span></h3><div class="body" style="padding:8px">
      <input id="libQ" placeholder="search title, path, client, spoken words…" value="${esc2(L.q)}" oninput="L.q=this.value;pp.libSoon()" style="width:100%">
      <div class="fh">TYPE</div><div class="row" style="gap:3px">${chips(f.type, "type", L.type)}</div>
      <div class="fh">STATUS</div><div class="row" style="gap:3px">${chips(f.status, "status", L.status)}<button class="ghost small${L.flag === "review" ? " active" : ""}" onclick="pp.libFilter('flag',L.flag==='review'?'':'review')">review queue</button></div>
      <div class="fh">FLAGS</div><div class="row" style="gap:3px"><button class="ghost small${L.flag === "missing" ? " active" : ""}" onclick="pp.libFilter('flag',L.flag==='missing'?'':'missing')">missing <small>${f.missing || 0}</small></button><button class="ghost small${L.flag === "archived" ? " active" : ""}" onclick="pp.libFilter('flag',L.flag==='archived'?'':'archived')">archived <small>${f.archived || 0}</small></button><button class="ghost small" onclick="pp.libDupes()">duplicates</button></div>
      ${Object.keys(f.client || {}).length ? `<div class="fh">CLIENT</div><div class="row" style="gap:3px">${chips(f.client, "client", L.client)}</div>` : ""}
      ${Object.keys(f.series || {}).length ? `<div class="fh">SERIES</div><div class="row" style="gap:3px">${chips(f.series, "series", L.series)}</div>` : ""}
      ${L.collections.length ? `<div class="fh">COLLECTIONS</div>${L.collections.map(c => `<div class="row" style="gap:4px"><button class="ghost small" onclick="pp.libCollection(${jq2(c.id)})">${esc2(c.name)} <small>${(c.mediaIds || []).length}</small></button>${selN ? `<button class="small" title="add selection" onclick="pp.libCollectionAdd(${jq2(c.id)})">+</button>` : ""}</div>`).join("")}` : ""}
      <div class="fh">SORT</div><select class="small" style="width:100%" onchange="L.sort=this.value;loadLibrary()">${["recent", "title", "duration", "plays", "size", "type", "client", "series", "lastPlayed", "expires"].map(s => `<option ${s === L.sort ? "selected" : ""}>${s}</option>`).join("")}</select>
      <div class="row" style="margin-top:10px"><button class="small${L.showIngest ? " primary" : ""}" onclick="L.showIngest=!L.showIngest;pp.render()">Ingest ${L.pending ? "(" + L.pending + ")" : ""}</button><button class="ghost small" onclick="pp.libSweep()" title="check every file still exists">sweep</button><button class="ghost small" onclick="loadLibrary()">&#8635;</button></div>
    </div></div>
    <div class="grid" style="gap:12px;align-content:start">
      ${ingest}
      <div class="panel"><h3>${L.total} item${L.total === 1 ? "" : "s"} <span style="color:var(--t3);text-transform:none;letter-spacing:0">· ${fmtC(L.totalSec)} · ${fmtB2(L.totalBytes || 0)}</span>
        <span class="right">${selN ? `<span style="color:var(--t0)">${selN} selected</span><select class="small" onchange="if(this.value){pp.libBulk({type:this.value});this.value=''}"><option value="">set type…</option>${TYPES.map(t => `<option>${t}</option>`).join("")}</select><select class="small" onchange="if(this.value){pp.libBulk({status:this.value});this.value=''}"><option value="">set status…</option><option>approved</option><option>review</option><option>rejected</option></select><button class="small" onclick="pp.libBulk({archived:true})">archive</button><button class="small" onclick="pp.libTranscribeSel()">transcribe</button><button class="small" onclick="pp.libPushSel()">push to mesh</button><button class="ghost small" onclick="pp.libDeleteSel()">remove</button><button class="ghost small" onclick="L.sel.clear();pp.render()">clear</button>` : ""}
        <button class="ghost small${L.view === "list" ? " active" : ""}" onclick="L.view='list';pp.render()">&#9776;</button><button class="ghost small${L.view === "grid" ? " active" : ""}" onclick="L.view='grid';pp.render()">&#9638;</button></span></h3>
        ${L.view === "grid" ? `<div class="mgrid">${L.rows.map(card).join("") || `<div class="empty">Nothing matches</div>`}</div>` : `<div class="scroll" style="max-height:70vh"><table><thead><tr><th><input type="checkbox" onclick="pp.libSelAll(this.checked)"></th><th></th><th>Title</th><th>Type</th><th>Length</th><th>Raster</th><th>Size</th><th>Transcript</th><th>Plays</th><th>Path</th></tr></thead><tbody>${L.rows.map(row).join("") || `<tr><td colspan="10" class="empty">Nothing matches. Upload, scan a folder or add a watch folder under Ingest.</td></tr>`}</tbody></table></div>`}
      </div></div>
    ${L.edit ? viewEditor() : ""}
  </div>`;
}

function viewIngestPanels() {
  const S = pp.S; const u = S.uploadInfo || {}; const typeSel = (cur, on) => `<select class="small" onchange="${on}">${TYPES.map(t => `<option ${t === cur ? "selected" : ""}>${t}</option>`).join("")}</select>`;
  const jobs = L.jobs.slice().reverse().slice(0, 40).map(j => `<tr><td>${esc2(j.name)}</td><td>${bdg2(j.origin, "var(--t2)")}</td><td>${bdg2(j.type, tcol(j.type))}</td><td>${j.state === "failed" ? `<span style="color:var(--danger)">${esc2(j.error)}</span>` : j.state === "skipped" ? `<span style="color:var(--t3)">${esc2(j.error)}</span>` : bdg2(j.state, j.state === "done" ? "var(--ready)" : "var(--accent)")}</td><td>${j.mediaId ? `<button class="ghost small" onclick="pp.libOpen(${jq2(j.mediaId)})">open</button>` : ""}</td></tr>`).join("");
  const tj = L.tjobs.slice().reverse().slice(0, 20).map(j => `<tr><td>${esc2(j.title)}</td><td>${esc2(j.level)}</td><td style="width:30%"><div class="meter"><div class="bar" style="width:${j.state === "done" ? 100 : j.pct}%;background:${j.state === "failed" ? "var(--danger)" : j.state === "done" ? "var(--ready)" : "var(--accent)"}"></div></div></td><td>${j.state === "failed" ? `<span style="color:var(--danger)">${esc2(j.error)}</span>` : bdg2(j.state, j.state === "done" ? "var(--ready)" : "var(--accent)")}</td><td>${j.state === "running" || j.state === "queued" ? `<button class="ghost small" onclick="pp.api('DELETE','/transcribe/jobs/${j.id}').then(loadIngest)">&#10005;</button>` : ""}</td></tr>`).join("");
  const uploads = (S.uploads || []).map(x => `<tr><td>${esc2(x.name)}</td><td class="mono">${fmtB2(x.size)}</td><td><div class="meter"><div class="bar" style="width:${x.size ? Math.min(100, 100 * x.sent / x.size) : 0}%;background:${x.error ? "var(--danger)" : x.done ? "var(--ready)" : "var(--accent)"}"></div></div></td><td>${x.error ? `<span style="color:var(--danger)">${esc2(x.error)}</span>` : x.done ? "&#10003; probing" : Math.round(100 * x.sent / Math.max(1, x.size)) + "%"}</td></tr>`).join("");
  const rc = L.cfg.removable || {};
  return `<div class="grid" style="grid-template-columns:repeat(auto-fit,minmax(min(100%,420px),1fr))">
    <div class="panel"><h3>Upload <span class="right">${esc2(u.uploadDir || "")} · free ${u.freeBytes ? fmtB2(u.freeBytes) : "?"}</span></h3><div class="body">
      ${uploadSpecifics()}<div id="drop" class="drop" style="padding:16px" ondragover="event.preventDefault();this.classList.add('over')" ondragleave="this.classList.remove('over')" ondrop="pp.dropFiles(event)" onclick="pp.pickFiles()"><div>&#8679; Drop media files or click to choose</div><div style="color:var(--t3);font-size:11px">probed with ffprobe, hashed, thumbnailed, added as <b>${esc2(U.type)}</b> with the specifics above</div></div>
      ${stagedList()}<div class="row" style="margin-top:6px"><span class="fill"></span><label class="f">Folder on the Supervisor <span class="row"><input id="scanFolder" placeholder="D:\\Media\\Programs" style="width:200px"><button class="small" onclick="pp.scanFolder()">Scan</button></span></label></div>
      ${uploads ? `<table style="margin-top:8px"><tbody>${uploads}</tbody></table>` : ""}</div></div>
    <div class="panel"><h3>Watch folders (${L.watch.length}) <span class="right"><button class="small" onclick="pp.libWatchAdd()">+ folder</button><button class="ghost small" onclick="pp.POST('/ingest/rescan').then(()=>pp.toast('Rescan requested','ok'))">rescan</button></span></h3>
      <div class="body" style="padding:8px"><table><tbody>${L.watch.map((w, i) => `<tr><td><input type="checkbox" ${w.enabled !== false ? "checked" : ""} onchange="pp.libWatchSet(${i},'enabled',this.checked)"></td><td><input value="${esc2(w.path)}" style="width:100%" onchange="pp.libWatchSet(${i},'path',this.value)"></td><td>${typeSel(w.type || "program", "pp.libWatchSet(" + i + ",'type',this.value)")}</td><td><input type="checkbox" ${w.recursive !== false ? "checked" : ""} title="recursive" onchange="pp.libWatchSet(${i},'recursive',this.checked)"></td><td><button class="ghost small" onclick="pp.libWatchDel(${i})">&#10005;</button></td></tr>`).join("") || `<tr><td class="empty">No watch folders. Files that appear in one are imported once they stop growing. The AN-FS bridge's landing dir (C:\\webroot\\Dropbox\\PlayPlay Ingest) is a good first one.</td></tr>`}</tbody></table>
      <div class="row" style="margin-top:8px;font-size:11px;color:var(--t2)"><label><input type="checkbox" ${rc.enabled !== false ? "checked" : ""} onchange="pp.libCfg({removable:{...L.cfg.removable,enabled:this.checked}})"> removable drives: import <code>${esc2(rc.marker || "PLAYPLAY_INGEST")}</code>\\ when a drive appears</label><span class="fill"></span><label><input type="checkbox" ${L.cfg.thumbnails !== false ? "checked" : ""} onchange="pp.libCfg({thumbnails:this.checked})"> thumbnails</label><label><input type="checkbox" ${L.cfg.review === "require" ? "checked" : ""} onchange="pp.libCfg({review:this.checked?'require':'auto'})"> review queue</label></div>
      ${L.removable.length ? `<div style="font-size:11px;color:var(--t3);margin-top:4px">volumes: ${L.removable.map(v => `${esc2(v.root)} ${esc2(v.name)} ${fmtB2(v.bytesFree)} free${v.hasMarker ? " ✓marker" : ""}`).join(" · ")}</div>` : ""}</div></div>
    <div class="panel"><h3>Transcription <span class="right"><select class="small" onchange="pp.libTCfg({defaultLevel:this.value})">${L.levels.map(l => `<option value="${l.id}" ${l.id === L.tcfg.defaultLevel ? "selected" : ""}>${l.id} (${l.model})</option>`).join("")}</select><label style="font-size:11px"><input type="checkbox" ${L.tcfg.autoTranscribe ? "checked" : ""} onchange="pp.libTCfg({autoTranscribe:this.checked})"> auto on ingest</label><button class="ghost small" onclick="pp.libProbeEnv()">check</button></span></h3>
      <div class="scroll" style="max-height:220px"><table><tbody>${tj || `<tr><td class="empty">No transcription jobs. ${L.env ? (L.env.ok ? "faster-whisper " + esc2(L.env.fasterWhisper) + " · " + (L.env.cudaDevices ? "GPU" : "CPU") + " · cached: " + esc2((L.env.cachedModels || []).join(", ") || "none") : "<span style='color:var(--danger)'>" + esc2(L.env.error) + "</span>") : "Select items and press transcribe, or open an item."}</td></tr>`}</tbody></table></div></div>
    <div class="panel" style="grid-column:1/-1"><h3>Ingest jobs (${L.jobs.length}, ${L.pending} pending) <span class="right"><button class="ghost small" onclick="pp.POST('/ingest/jobs/clear').then(loadIngest)">clear finished</button></span></h3><div class="scroll" style="max-height:220px"><table><tbody>${jobs || `<tr><td class="empty">No ingest jobs yet</td></tr>`}</tbody></table></div></div>
  </div>`;
}

// ---------------------------------------------------------------------------
// Editor drawer: metadata · transcript · segments
// ---------------------------------------------------------------------------
function viewEditor() {
  const m = L.edit; const tr = m.transcript || {}; const tabs = ["meta", "transcript", "segments"];
  const f = (k, label, type = "text", extra = "") => `<label class="f">${label} <input type="${type}" value="${esc2(m[k] ?? "")}" onchange="L.edit['${k}']=${type === "number" ? "parseFloat(this.value)||0" : "this.value"}" ${extra}></label>`;
  let body = "";
  if (L.editTab === "meta") body = `<div class="grid" style="gap:8px;grid-template-columns:1fr 1fr">
      ${f("title", "Title")}<label class="f">Type <select onchange="L.edit.type=this.value">${TYPES.map(t => `<option ${t === m.type ? "selected" : ""}>${t}</option>`).join("")}</select></label>
      ${f("client", "Client")}${f("programSeries", "Series")}${f("episodeDate", "Episode date", "date")}${f("priority", "Priority", "number")}
      <label class="f">Status <select onchange="L.edit.status=this.value">${["approved", "review", "rejected"].map(t => `<option ${t === (m.status || "approved") ? "selected" : ""}>${t}</option>`).join("")}</select></label>
      <label class="f">Allowed channels <input value="${esc2((m.allowedChannels || []).join(","))}" placeholder="all" onchange="L.edit.allowedChannels=this.value.split(',').map(s=>s.trim()).filter(Boolean)"></label>
      <label class="f">Available from <input type="datetime-local" value="${m.availableStartMs ? new Date(m.availableStartMs - new Date().getTimezoneOffset() * 60000).toISOString().slice(0, 16) : ""}" onchange="L.edit.availableStartMs=this.value?new Date(this.value).getTime():0"></label>
      <label class="f">Expires <input type="datetime-local" value="${m.availableEndMs ? new Date(m.availableEndMs - new Date().getTimezoneOffset() * 60000).toISOString().slice(0, 16) : ""}" onchange="L.edit.availableEndMs=this.value?new Date(this.value).getTime():0"></label>
      <label class="f" style="grid-column:1/-1">Tags <input value="${esc2((m.tags || []).join(", "))}" onchange="L.edit.tags=this.value.split(',').map(s=>s.trim().toLowerCase()).filter(Boolean)"></label>
      <label class="f" style="grid-column:1/-1">Notes <textarea rows="3" onchange="L.edit.notes=this.value">${esc2(m.notes || "")}</textarea></label>
      <label class="f">Category ${`<input value="${esc2(m.category || "")}" onchange="L.edit.category=this.value">`}</label><label class="f">Subcategory <input value="${esc2(m.subcategory || "")}" onchange="L.edit.subcategory=this.value"></label>
      <label class="f" style="grid-column:1/-1">Path <span class="row"><input value="${esc2(m.path)}" id="edPath" style="flex:1" ${m.missing ? "" : "readonly"}>${m.missing ? `<button class="small" onclick="pp.libRelink()">Relink</button>` : `<button class="ghost small" onclick="pp.POST('/media/'+${jq2(m.id)}+'/reprobe').then(()=>pp.toast('Re-probing','ok'))">re-probe</button><button class="ghost small" onclick="pp.POST('/media/'+${jq2(m.id)}+'/thumbnail').then(()=>pp.toast('Thumbnail queued','ok'))">thumb</button>`}</span></label>
      <div style="grid-column:1/-1;color:var(--t3);font-size:11px">${m.width ? `${m.width}x${m.height} ${esc2(m.frameRate || "")} ${esc2(m.videoCodec || "")}/${esc2(m.audioCodec || "")} · ` : ""}${fmtB2(m.fileSizeBytes || 0)} · hash ${esc2((m.hash || "").slice(0, 12) || "—")} · origin ${esc2(m.origin || "?")} · plays ${m.playCount || 0}${m.meshNode ? ` · from mesh ${esc2(m.meshNode)}` : ""}</div></div>
    <div class="row" style="margin-top:10px"><button class="success" onclick="pp.libSave()">SAVE</button><button class="ghost" onclick="pp.libBulkOne({archived:!${!!m.archived}})">${m.archived ? "Unarchive" : "Archive"}</button><a class="btn" href="/api/v1/download/${esc2(m.id)}${pp.S.token ? "?token=" + encodeURIComponent(pp.S.token) : ""}" download>&#8681; Download</a><span class="fill"></span><button class="ghost" onclick="pp.libDeleteOne()">Remove</button></div>`;
  else if (L.editTab === "transcript") {
    const t = L.transcript;
    body = `<div class="row"><select id="trLevel">${L.levels.map(l => `<option value="${l.id}" ${l.id === (tr.level || L.tcfg.defaultLevel) ? "selected" : ""}>${l.id} — ${l.model}</option>`).join("")}</select><button class="primary small" onclick="pp.libTranscribe()">${tr.status === "done" ? "Re-transcribe" : "Transcribe"}</button>${tr.status === "running" || tr.status === "queued" ? bdg2(tr.status + "…", "var(--accent)") : ""}${tr.status === "failed" ? `<span style="color:var(--danger)">${esc2(tr.error || "failed")}</span>` : ""}<span class="fill"></span>${tr.status === "done" ? `<span style="color:var(--t3);font-size:11px">${esc2(tr.model)} · ${esc2(tr.language)} · ${tr.words} words · ${tr.elapsedSec}s</span><button class="ghost small" onclick="pp.api('DELETE','/media/'+${jq2(m.id)}+'/transcript').then(()=>pp.libOpen(${jq2(m.id)},'transcript'))">&#10005;</button>` : ""}</div>
      <div style="color:var(--t3);font-size:11px;margin:6px 0">${(L.levels.find(l => l.id === (tr.level || L.tcfg.defaultLevel)) || {}).note || ""}</div>
      ${t ? `<div class="row" style="margin-bottom:6px"><span style="font-size:11px;color:var(--t2)">Click a word, then shift-click the last word to make a segment${L.wordA >= 0 ? `: <b class="mono">${fmtT(wordAt(L.wordA).start)}</b>${L.wordB >= 0 ? ` → <b class="mono">${fmtT(wordAt(L.wordB).end)}</b>` : " → …"}` : ""}</span><span class="fill"></span>${L.wordA >= 0 && L.wordB >= 0 ? `<input id="segLabel" placeholder="segment label" style="width:150px"><button class="success small" onclick="pp.libSegFromWords()">+ Segment</button>` : ""}<button class="ghost small" onclick="L.wordA=-1;L.wordB=-1;pp.render()">clear</button></div>
        <div class="transcript">${(t.segments || []).map(s => `<p><span class="ts mono" onclick="pp.libSeek(${s.start})">${fmtC(s.start)}</span>${(s.words || []).map((w, i) => { const gi = w._gi; const inSel = L.wordA >= 0 && gi >= Math.min(L.wordA, L.wordB < 0 ? L.wordA : L.wordB) && gi <= Math.max(L.wordA, L.wordB); return `<span class="w${inSel ? " sel" : ""}${w.probability < 0.5 ? " low" : ""}" data-gi="${gi}" title="${fmtT(w.start)}–${fmtT(w.end)} p=${w.probability}" onclick="pp.libWord(${gi},event.shiftKey)">${esc2(w.word)}</span>`; }).join("")}</p>`).join("")}</div>` : `<div class="empty">${tr.status === "done" ? "loading transcript…" : "No transcript yet. Levels trade speed for accuracy; ‘fast’ is enough for search and rough cuts."}</div>`}`;
  } else {
    const sg = L.segs || { segments: [], collections: [] }; const segs = sg.segments || [];
    syncSegEditor(m.id, m.duration || 0, segs, edited => { L.segs.segments = edited; pp.libSegSave(); });
    body = `<div class="row" style="margin-bottom:6px"><select class="small" onchange="pp.libSegCollection(this.value)">${(sg.collections.length ? sg.collections : [{ id: "default", name: "Default" }]).map(c => `<option value="${esc2(c.id)}" ${c.id === sg.activeCollection ? "selected" : ""}>${esc2(c.name)} (${(c.segments || []).length})</option>`).join("")}</select><button class="ghost small" onclick="pp.libSegNewCollection()">+ collection</button><span class="fill"></span>
        <button class="small" onclick="pp.libSegAuto('interval')" title="fixed interval">auto: interval</button><button class="small" onclick="pp.libSegAuto('transcript')" title="split at pauses in the transcript">auto: pauses</button><button class="small" onclick="pp.libSegAdd()">+ segment</button></div>
      <div class="row" style="margin-bottom:6px;font-size:11px;color:var(--t2)"><label>interval <input id="segInt" type="number" value="600" style="width:64px"> s</label><label>overlap <input id="segOv" type="number" value="0" style="width:52px"> s</label><label>min pause <input id="segPause" type="number" value="1.5" step="0.1" style="width:52px"> s</label><label>min seg <input id="segMin" type="number" value="20" style="width:52px"> s</label></div>
      ${segEditorHost()}
      <div class="scroll" style="max-height:48vh"><table><thead><tr><th>#</th><th>Label</th><th>In</th><th>Out</th><th>Len</th><th></th></tr></thead><tbody>${segs.map((s, i) => `<tr id="seg${i}"><td class="mono">${i + 1}</td><td><input value="${esc2(s.label)}" onchange="L.segs.segments[${i}].label=this.value;pp.libSegSave()" style="width:150px">${s.text ? `<div style="color:var(--t3);font-size:10px;max-width:220px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap">${esc2(s.text)}</div>` : ""}</td>
        <td><input class="mono" value="${s.startTime.toFixed(2)}" style="width:72px" onchange="L.segs.segments[${i}].startTime=parseFloat(this.value)||0;pp.libSegSave()"></td><td><input class="mono" value="${s.endTime.toFixed(2)}" style="width:72px" onchange="L.segs.segments[${i}].endTime=parseFloat(this.value)||0;pp.libSegSave()"></td><td class="mono">${fmtC(s.endTime - s.startTime)}</td>
        <td class="row" style="flex-wrap:nowrap"><button class="ghost small" onclick="pp.libSegMove(${i},-1)">&#8593;</button><button class="ghost small" onclick="pp.libSegMove(${i},1)">&#8595;</button><button class="ghost small" title="duplicate" onclick="pp.libSegDup(${i})">&#10697;</button><button class="ghost small" onclick="pp.libSegDel(${i})">&#10005;</button></td></tr>`).join("") || `<tr><td colspan="6" class="empty">No segments. Add one, auto-split, or select words in the transcript. Segments become virtual clips the schedule can place (in/out on the same file).</td></tr>`}</tbody></table></div>`;
  }
  return `<div class="panel" style="position:sticky;top:56px;align-self:start"><h3><span style="text-transform:none;letter-spacing:0;color:var(--t0)">${esc2(m.title)}</span><span class="right"><button class="ghost small" onclick="L.edit=null;pp.render()">&#10005;</button></span></h3>
    <div class="body" style="padding:8px 12px"><div class="row" style="margin-bottom:8px">${tabs.map(t => `<button class="ghost small${L.editTab === t ? " active" : ""}" onclick="pp.libTab('${t}')">${t}</button>`).join("")}<span class="fill"></span>${m.thumbnail ? `<img src="/thumbs/${esc2(m.id)}.jpg" style="height:40px;border-radius:3px">` : ""}</div>${body}</div></div>`;
}
let flatWords = [];
function wordAt(gi) { return flatWords[gi] || { start: 0, end: 0 }; }
function indexWords(t) { flatWords = []; for (const s of t.segments || []) for (const w of s.words || []) { w._gi = flatWords.length; flatWords.push(w); } }
let libTimer;
export const ppExtLibrary = {
  upQuery(title) { const m = upMeta(title); return Object.keys(m).length ? "&meta=" + encodeURIComponent(JSON.stringify(m)) : ""; },
  upHold() { return !!U.hold; },
  upSet(k, v, quiet) { U[k] = k === "priority" ? (parseInt(v) || 0) : v; if (k === "type") pp.S.ingestType = v; upSave(); if (!quiet) pp.render(); },
  upChan(c, on) { U.channels = on ? [...new Set([...U.channels, c])] : U.channels.filter(x => x !== c); upSave(); pp.render(); },
  upReset() { Object.assign(U, { type: "program", title: "", client: "", series: "", category: "", subcategory: "", tags: "", notes: "", priority: 0, status: "approved", channels: [], from: "", until: "", episodeDate: "", hold: false }); pp.S.ingestType = "program"; upSave(); pp.render(); },
  upStageAdd(files) { for (const f of files) U.staged.push({ file: f, title: "" }); pp.toast(U.staged.length + " file(s) held: set the specifics, then Upload", "ok"); pp.render(); },
  upStageDel(i) { U.staged.splice(i, 1); pp.render(); },
  upStageClear() { U.staged = []; pp.render(); },
  upStageGo() { const list = U.staged; U.staged = []; for (const x of list) pp.uploadOne(x.file, x.title); pp.render(); },
  libSoon() { clearTimeout(libTimer); libTimer = setTimeout(loadLibrary, 300); },
  libReload() { loadLibrary(); },     // the context menus (app.js) changed a record: refresh the list the Library shows
  libFilter(k, v) { L[k] = v; if (k === "flag" && v) { L.status = ""; } loadLibrary(); },
  libSel(id, on) { if (on) L.sel.add(id); else L.sel.delete(id); pp.render(); },
  libSelAll(on) { L.sel.clear(); if (on) for (const m of L.rows) L.sel.add(m.id); pp.render(); },
  async libOpen(id, tab) {
    const m = await pp.GET("/media/" + id).catch(() => null); if (!m) return;
    L.edit = m; L.editTab = tab || (L.edit && L.editTab) || "meta"; L.transcript = null; L.segs = null; L.wordA = -1; L.wordB = -1;
    pp.render();
    if (L.editTab === "transcript" && (m.transcript || {}).status === "done") pp.GET("/media/" + id + "/transcript").then(t => { indexWords(t); L.transcript = t; pp.render(); }).catch(() => {});
    if (L.editTab === "segments") pp.GET("/media/" + id + "/segments").then(s => { L.segs = s; pp.render(); });
  },
  libTab(t) { L.editTab = t; if (L.edit) pp.libOpen(L.edit.id, t); },
  libSave() { const m = L.edit; const body = { title: m.title, type: m.type, client: m.client, programSeries: m.programSeries, episodeDate: m.episodeDate, priority: m.priority, status: m.status || "approved", allowedChannels: m.allowedChannels || [], availableStartMs: m.availableStartMs || 0, availableEndMs: m.availableEndMs || 0, tags: m.tags || [], notes: m.notes || "", category: m.category || "", subcategory: m.subcategory || "" };
    pp.PUT("/media/" + m.id, body).then(() => { pp.toast("Saved", "ok"); loadLibrary(); }); },
  libBulkOne(set) { pp.POST("/media/bulk", { ids: [L.edit.id], set }).then(() => { pp.toast("Updated", "ok"); pp.libOpen(L.edit.id); loadLibrary(); }); },
  libBulk(set) { pp.POST("/media/bulk", { ids: [...L.sel], set }).then(r => { pp.toast(r.updated + " updated", "ok"); loadLibrary(); }); },
  libDeleteSel() { pp.confirmDo("Remove " + L.sel.size + " item(s) from the library? Files stay on disk.", () => pp.POST("/media/delete", { ids: [...L.sel] }).then(() => { L.sel.clear(); loadLibrary(); })); },
  libDeleteOne() { pp.confirmDo("Remove \"" + L.edit.title + "\" from the library? The file stays on disk.", () => pp.POST("/media/delete", { ids: [L.edit.id] }).then(() => { L.edit = null; loadLibrary(); })); },
  libRelink() { const p = q$("#edPath").value.trim(); pp.POST("/media/" + L.edit.id + "/relink", { path: p }).then(() => { pp.toast("Relinked", "ok"); pp.libOpen(L.edit.id); loadLibrary(); }); },
  libSweep() { pp.POST("/ingest/sweep").then(r => { pp.toast(r.missing + " missing file(s)", r.missing ? "err" : "ok"); loadLibrary(); }); },
  libDupes() { pp.GET("/media/duplicates").then(d => { const ids = new Set(); for (const g of d.groups || []) for (const id of g.ids) ids.add(id); if (!ids.size) { pp.toast("No duplicates found", "ok"); return; } L.sel = ids; L.q = ""; L.flag = ""; loadLibrary().then(() => pp.toast(d.groups.length + " duplicate group(s) selected")); }); },
  libCollection(id) { const c = L.collections.find(x => x.id === id); if (!c) return; L.sel = new Set(c.mediaIds || []); pp.render(); },
  libCollectionAdd(id) { const c = L.collections.find(x => x.id === id); if (!c) return; const ids = [...new Set([...(c.mediaIds || []), ...L.sel])]; pp.PUT("/media/collections/" + id, { mediaIds: ids }).then(loadIngest); },
  libCollectionNew() { const name = prompt("Collection name"); if (!name) return; pp.PUT("/media/collections/" + name.toLowerCase().replace(/[^a-z0-9]+/g, "-"), { name, mediaIds: [...L.sel] }).then(loadIngest); },
  libTranscribe() { const lv = q$("#trLevel") ? q$("#trLevel").value : L.tcfg.defaultLevel; pp.POST("/media/" + L.edit.id + "/transcribe", { level: lv }).then(() => { pp.toast("Transcription queued (" + lv + ")", "ok"); pp.libOpen(L.edit.id, "transcript"); loadIngest(); }); },
  libTranscribeSel() { const lv = L.tcfg.defaultLevel || "balanced"; pp.POST("/transcribe/bulk", { ids: [...L.sel], level: lv }).then(r => { pp.toast(r.queued + " queued (" + lv + ")", "ok"); loadIngest(); }); },
  libPushSel() { pp.nav("mesh"); pp.toast("Use Push on the Mesh page for each item", "ok"); },
  libTCfg(c) { pp.PUT("/transcribe/config", c).then(cfg => { L.tcfg = cfg; pp.render(); }); },
  libProbeEnv() { pp.POST("/transcribe/probe").then(e => { L.env = e; pp.render(); pp.toast(e.ok ? "faster-whisper " + e.fasterWhisper + " ready" : e.error, e.ok ? "ok" : "err"); }); },
  libCfg(c) { pp.PUT("/ingest/config", c).then(cfg => { L.cfg = cfg; pp.render(); }); },
  libWatchAdd() { const p = prompt("Folder to watch (on the Supervisor's machine)"); if (!p) return; L.watch.push({ path: p, type: "program", recursive: true, enabled: true }); pp.PUT("/ingest/watch", { folders: L.watch }).then(loadIngest); },
  libWatchSet(i, k, v) { L.watch[i][k] = v; pp.PUT("/ingest/watch", { folders: L.watch }).then(loadIngest); },
  libWatchDel(i) { L.watch.splice(i, 1); pp.PUT("/ingest/watch", { folders: L.watch }).then(loadIngest); },
  libSeek(t) { pp.toast("@ " + fmtC(t)); },
  libWord(gi, shift) { if (!shift || L.wordA < 0) { L.wordA = gi; L.wordB = -1; } else { L.wordB = gi; if (L.wordB < L.wordA) { const t = L.wordA; L.wordA = L.wordB; L.wordB = t; } } pp.render(); },
  libSegFromWords() { const label = q$("#segLabel") ? q$("#segLabel").value : ""; pp.POST("/media/" + L.edit.id + "/segments/from-words", { firstWord: L.wordA, lastWord: L.wordB, label }).then(s => { L.segs = s; L.wordA = -1; L.wordB = -1; pp.toast("Segment added", "ok"); pp.libTab("segments"); }); },
  libSegSave() { pp.PUT("/media/" + L.edit.id + "/segments", { segments: L.segs.segments }).then(s => { L.segs = s; pp.render(); }); },
  libSegAdd() { const d = L.edit.duration || 0; L.segs.segments.push({ label: "Segment " + (L.segs.segments.length + 1), startTime: 0, endTime: Math.min(d, 30) }); pp.libSegSave(); },
  libSegAddAt(ev) { const r = ev.currentTarget.getBoundingClientRect(); const t = (ev.clientX - r.left) / r.width * (L.edit.duration || 0); L.segs.segments.push({ label: "Segment " + (L.segs.segments.length + 1), startTime: Math.max(0, t), endTime: Math.min(L.edit.duration || 0, t + 30) }); pp.libSegSave(); },
  libSegDel(i) { L.segs.segments.splice(i, 1); pp.libSegSave(); },
  libSegDup(i) { const s = { ...L.segs.segments[i], id: undefined, label: L.segs.segments[i].label + " copy" }; L.segs.segments.splice(i + 1, 0, s); pp.libSegSave(); },
  libSegMove(i, d) { const a = L.segs.segments; const j = i + d; if (j < 0 || j >= a.length) return; [a[i], a[j]] = [a[j], a[i]]; pp.libSegSave(); },
  libSegAuto(mode) { const body = { mode, intervalSec: parseFloat(q$("#segInt").value) || 600, overlapSec: parseFloat(q$("#segOv").value) || 0, minPauseSec: parseFloat(q$("#segPause").value) || 1.5, minSegmentSec: parseFloat(q$("#segMin").value) || 20, replace: true }; pp.POST("/media/" + L.edit.id + "/segments/auto", body).then(s => { L.segs = s; pp.toast((s.segments || []).length + " segments", "ok"); pp.render(); }); },
  libSegCollection(id) { const sg = L.segs; pp.PUT("/media/" + L.edit.id + "/segments/collections", { collections: sg.collections, activeCollection: id }).then(s => { L.segs = s; pp.render(); }); },
  libSegNewCollection() { const name = prompt("Collection name"); if (!name) return; const cols = (L.segs.collections || []).slice(); cols.push({ name, segments: [] }); pp.PUT("/media/" + L.edit.id + "/segments/collections", { collections: cols, activeCollection: "" }).then(s => { L.segs = s; pp.render(); }); },
};
