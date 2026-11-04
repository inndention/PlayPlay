/* PLAY PLAY web dashboard — integration consoles: live inputs (NDI scan + ONVIF cameras with PTZ),
   the HTML CG template pack (web/cg) and the AN-FS mesh (multi-node media library).
   Loaded by app.js; shares its S state, api helpers and render loop through window.pp. */

const I = {
  inputs: { ndi: [], cameras: [], ndiConfig: {} }, camSel: "", camPresetName: "", ptzSpeed: 0.5, camForm: { host: "", name: "", user: "", pass: "" },
  cg: { templates: [], presets: [], animations: [] }, cgSel: "", cgParams: {}, cgAnim: { in: "fade", out: "fade", duration: 600, autoHide: 0 }, cgCat: "", cgOnAir: "", cgFilter: "", cgOutput: 1, cgDrafts: {},
  mesh: { status: {}, nodes: [], node: "", path: "", listing: null, hits: [], q: "", jobs: [], fetchType: "program", pushNode: "", pushFolder: "" },
  // captions: karaoke (layer 31) follow state per channel + fact box (layer 32) form and presets
  // (colours are Qt #AARRGGBB, alpha first, exactly what the engine parses and the QML panel sends: the box default is #be05050a)
  cap: { k: {}, kCh: "", kSrvMode: "", kSrvPos: "", kMediaId: "", kPosition: "bottom", kMode: "word", kFontSize: 46, kColor: "#ebebf5", kHighlight: "#ffd60a", kPast: "#ffffff", kBg: "#be05050a", kSync: 0,
         f: { title: "Fact box", lines: "First fact\nSecond fact\nThird fact", position: "right", accentColor: "#00d4ff", width: 520, reveal: false, revealIntervalSec: 1.2, footer: "" },
         presets: [], presetId: "", presetName: "" },
};
window.I = I;
const $$ = s => document.querySelector(s);
const E = s => String(s ?? "").replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
const ago = ms => !ms ? "never" : Math.max(0, Math.round((Date.now() - ms) / 1000)) + " s ago";
const fmtBytes = b => b > 1e9 ? (b / 1e9).toFixed(2) + " GB" : b > 1e6 ? (b / 1e6).toFixed(1) + " MB" : b > 1e3 ? Math.round(b / 1e3) + " kB" : (b || 0) + " B";
const bdg = (text, color) => `<span class="badge" style="color:${color};border-color:${color};background:color-mix(in srgb,${color} 16%,transparent)">${E(text)}</span>`;

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------
async function loadInputs() { const d = await pp.GET("/inputs").catch(() => null); if (d) { I.inputs = d; if (!I.camSel && d.cameras && d.cameras.length) I.camSel = d.cameras[0].id; pp.render(); } }
async function loadCgTemplates() { if (I.cg.templates.length) return; const d = await pp.GET("/cg/templates").catch(() => null); if (d) { I.cg = d; pp.render(); } }
async function loadMesh() {
  const st = await pp.GET("/mesh/status").catch(() => null); if (st) I.mesh.status = st;
  pp.GET("/mesh/jobs").then(j => { I.mesh.jobs = j.jobs || []; pp.render(); }).catch(() => {});
  const n = await pp.GET("/mesh/nodes").catch(e => { I.mesh.status.lastError = String(e.message || e); return null; });
  if (n) { I.mesh.nodes = n.nodes || []; if (!I.mesh.node && I.mesh.nodes.length) { const first = I.mesh.nodes.find(x => x.online) || I.mesh.nodes[0]; I.mesh.node = first.id; I.mesh.path = (first.sharedFolders && first.sharedFolders[0] ? first.sharedFolders[0].path : "") || ""; } }
  pp.render();
  if (I.mesh.node && I.mesh.listing === null) meshBrowse(I.mesh.node, I.mesh.path);
}
async function meshBrowse(node, path) {
  I.mesh.node = node; I.mesh.path = path || ""; I.mesh.hits = [];
  const d = await pp.GET("/mesh/browse?node=" + encodeURIComponent(node) + "&path=" + encodeURIComponent(I.mesh.path)).catch(e => ({ error: String(e.message || e), items: [] }));
  I.mesh.listing = d; if (d.currentPath) I.mesh.path = d.currentPath; pp.render();
}
async function loadCaptions() {
  const ch = pp.S.active; if (!ch) return;
  const C = I.cap;
  if (C.kCh !== ch) { C.kCh = ch; C.kSrvMode = ""; C.kSrvPos = ""; }
  const k = await pp.GET("/channels/" + ch + "/cg/karaoke").catch(() => null);
  if (k) {
    C.k = k;
    // Mode / Position are seeded from the server only when the SERVER's value changed (first load, channel switch, another
    // client, a SHOW): a pick that has not been SHOWn yet survives every refresh (each cgKaraoke event used to reset it).
    const pos = (k.style && k.style.position) || "";
    if (k.mode && k.mode !== C.kSrvMode) { C.kSrvMode = k.mode; C.kMode = k.mode; }
    if (pos && pos !== C.kSrvPos) { C.kSrvPos = pos; C.kPosition = pos; }
  }
  const p = await pp.GET("/cg/factbox/presets").catch(() => null);
  if (p) I.cap.presets = p.presets || [];
  pp.render();
}
export function onEnterIntegration(v) {
  if (v === "inputs") loadInputs();
  if (v === "mesh") loadMesh();
  if (v === "cg") { loadCgTemplates(); loadCaptions(); }
}
export function onWsIntegration(m) {
  if (m.type === "cameras") { I.inputs.cameras = m.cameras || []; return true; }
  if (m.type === "meshJobs") { I.mesh.jobs = m.jobs || []; return true; }
  if (m.type === "cgHtml") { const key = m.channelId + (Number(m.output || 1) === 1 ? "" : ":" + Number(m.output)); (pp.S.snapshot.cgHtml = pp.S.snapshot.cgHtml || {})[key] = m.url ? m : undefined; return true; }
  if (m.type === "cgKaraoke") { if (m.channelId === pp.S.active) { I.cap.k = Object.assign({}, I.cap.k, m); if (pp.S.view === "cg") loadCaptions(); } return true; }
  if (m.type === "factboxPresets") { I.cap.presets = m.presets || []; if (pp.S.view === "cg") pp.render(); return true; }
  return false;
}

// ---------------------------------------------------------------------------
// Inputs: NDI + cameras
// ---------------------------------------------------------------------------
export function viewInputs() {
  const S = pp.S; const inp = I.inputs; const ndi = inp.ndi || [], cams = inp.cameras || []; const cfg = inp.ndiConfig || {};
  const cam = cams.find(c => c.id === I.camSel) || cams[0] || null; if (cam) I.camSel = cam.id;
  const ptzBtn = (x, y, z, label) => `<button class="ptz" onmousedown="pp.ptzMove(${x},${y},${z})" onmouseup="pp.ptzStop()" onmouseleave="pp.ptzStop()" ontouchstart="pp.ptzMove(${x},${y},${z})" ontouchend="pp.ptzStop()">${label}</button>`;
  const ndiRows = ndi.map(s => `<tr><td><b>${E(s.name)}</b></td><td class="mono" style="color:var(--t2)">${E(s.host || "")}</td><td class="mono">${E(s.url || "")}</td><td style="color:var(--t3)">${(s.seenBy || []).join(", ")}</td>
    <td class="row" style="flex-wrap:nowrap"><button class="small" onclick="pp.ndiPip('${E(s.name).replace(/'/g, "&#39;")}')">PiP</button><button class="small" onclick="pp.registerLive('ndi','${E(s.name).replace(/'/g, "&#39;")}')">+ Library</button></td></tr>`).join("");
  const camRows = cams.map(c => `<tr class="${c.id === I.camSel ? "onair" : ""}" style="cursor:pointer" onclick="I.camSel='${c.id}';pp.render()"><td><span class="dot${c.online ? "" : " off"}"></span></td><td><b>${E(c.name)}</b><div style="color:var(--t3);font-size:10px">${E(c.hardware || "")} ${E(c.location || "")}</div></td><td class="mono">${E(c.host)}</td>
    <td>${c.hasPtz ? bdg("PTZ", "var(--ready)") : ""} ${c.configured ? bdg("saved", "var(--accent)") : bdg("found", "var(--t2)")}${c.error ? ` <span style="color:var(--danger)" title="${E(c.error)}">!</span>` : ""}</td>
    <td class="row" style="flex-wrap:nowrap"><button class="small" onclick="event.stopPropagation();pp.camRefresh('${c.id}')">&#8635;</button>${c.streamUri ? `<button class="small" onclick="event.stopPropagation();pp.camRegister('${c.id}')">+ Library</button>` : ""}${c.configured ? `<button class="ghost small" onclick="event.stopPropagation();pp.camRemove('${c.id}')">&#10005;</button>` : ""}</td></tr>`).join("");
  return `<div class="grid cols2">
    <div class="panel"><h3>NDI sources (${ndi.length}) <span class="right">${inp.engines || 0} engine(s) · scanned ${ago(inp.lastScanMs)} <button class="small" onclick="pp.scanInputs()">Rescan</button></span></h3>
      <div class="scroll" style="max-height:38vh"><table><thead><tr><th>Source</th><th>Host</th><th>Address</th><th>Seen by</th><th></th></tr></thead><tbody>${ndiRows || `<tr><td colspan="5" class="empty">${inp.engines ? "No NDI sources on the network (engines scan every 10 s)" : "No engine connected: NDI discovery runs inside each channel engine"}</td></tr>`}</tbody></table></div>
      <div class="body"><div class="row" style="align-items:flex-end"><label class="f">NDI groups <input id="ndiGroups" value="${E(cfg.groups || "")}" placeholder="public,studio"></label><label class="f" style="flex:1">Extra IPs (other subnets) <input id="ndiIps" value="${E(cfg.extraIps || "")}" placeholder="192.168.1.66,192.168.1.67"></label><button class="primary small" onclick="pp.saveNdiConfig()">Save & rescan</button></div>
      <div style="color:var(--t3);font-size:11px;margin-top:6px">The finder uses mDNS on the local subnet; add the IPs of NDI hosts on other subnets (or a Discovery Server) and the group names private sources belong to.</div></div></div>
    <div class="panel"><h3>Cameras — ONVIF (${cams.length}) <span class="right">${inp.camerasDiscovering ? "discovering…" : "probed " + ago(inp.camerasLastMs)} <button class="small" onclick="pp.camDiscover()">Discover</button></span></h3>
      <div class="scroll" style="max-height:38vh"><table><thead><tr><th></th><th>Camera</th><th>Host</th><th></th><th></th></tr></thead><tbody>${camRows || `<tr><td colspan="5" class="empty">No ONVIF cameras answered WS-Discovery. Add one by address below.</td></tr>`}</tbody></table></div>
      <div class="body"><div class="row" style="align-items:flex-end"><label class="f">Host or XAddr <input id="camHost" value="${E(I.camForm.host)}" placeholder="192.168.1.64 or http://ip/onvif/device_service"></label><label class="f">Name <input id="camName" value="${E(I.camForm.name)}" style="width:120px"></label><label class="f">User <input id="camUser" value="${E(I.camForm.user)}" style="width:100px" autocomplete="off"></label><label class="f">Password <input id="camPass" type="password" style="width:110px" autocomplete="new-password"></label><button class="primary small" onclick="pp.camSave()">Save</button></div>
      <div style="color:var(--t3);font-size:11px;margin-top:6px">Saved cameras keep their credentials in the Supervisor settings (key onvif.devices); select a found camera first to attach credentials to it.</div></div></div>
    ${cam ? `<div class="panel" style="grid-column:1/-1"><h3>PTZ — ${E(cam.name)} <span class="right">${cam.hasPtz ? "" : "no PTZ service · "}${E(cam.xaddr)}</span></h3><div class="body" style="display:grid;grid-template-columns:auto 1fr 1fr;gap:16px;align-items:start">
      <div class="ptzpad">${ptzBtn(-1, 1, 0, "&#8598;")}${ptzBtn(0, 1, 0, "&#8593;")}${ptzBtn(1, 1, 0, "&#8599;")}${ptzBtn(-1, 0, 0, "&#8592;")}<button class="ptz home" onclick="pp.ptz({action:'home'})" title="Home">&#8962;</button>${ptzBtn(1, 0, 0, "&#8594;")}${ptzBtn(-1, -1, 0, "&#8601;")}${ptzBtn(0, -1, 0, "&#8595;")}${ptzBtn(1, -1, 0, "&#8600;")}
        <div style="grid-column:1/-1;display:flex;gap:6px;justify-content:center">${ptzBtn(0, 0, 1, "Zoom +")}${ptzBtn(0, 0, -1, "Zoom &minus;")}</div>
        <label class="f" style="grid-column:1/-1">Speed ${Math.round(I.ptzSpeed * 100)}% <input type="range" min="0.1" max="1" step="0.05" value="${I.ptzSpeed}" oninput="I.ptzSpeed=parseFloat(this.value)"></label></div>
      <div><div style="font-size:10px;letter-spacing:1px;color:var(--t2);margin-bottom:6px">PRESETS (${(cam.presets || []).length})</div>
        <div class="row">${(cam.presets || []).map(p => `<button class="small" onclick="pp.ptz({action:'preset',preset:'${E(p.token)}'})" title="token ${E(p.token)}">${E(p.name || p.token)}</button>`).join("") || `<span class="empty" style="padding:4px">none</span>`}</div>
        <div class="row" style="margin-top:10px;align-items:flex-end"><label class="f">New preset <input id="presetName" value="${E(I.camPresetName)}" oninput="I.camPresetName=this.value" placeholder="Wide shot" style="width:160px"></label><button class="small" onclick="pp.ptz({action:'setPreset',name:I.camPresetName||'PlayPlay'})">Store current position</button><button class="ghost small" onclick="pp.ptz({action:'setHome'})">Set home</button></div>
        <div style="color:var(--t3);font-size:11px;margin-top:10px">Schedule a preset from an hour's device commands with <code>ptz:${E(cam.id)}:preset:&lt;token&gt;</code> (or by camera name), <code>ptz:${E(cam.id)}:home</code>, <code>ptz:${E(cam.id)}:move:x,y,z,secs</code>.</div></div>
      <div><div style="font-size:10px;letter-spacing:1px;color:var(--t2);margin-bottom:6px">STREAM</div>
        <div class="mono" style="word-break:break-all;color:var(--t1)">${E((cam.streamUri || "").replace(/:\/\/[^@]*@/, "://•••@")) || "<span style='color:var(--t3)'>unknown — set credentials and refresh</span>"}</div>
        <div class="row" style="margin-top:8px">${cam.streamUri ? `<button class="small" onclick="pp.camRegister('${cam.id}')">Register as live source</button><button class="small" onclick="pp.camPip('${cam.id}')">PiP on ${E(S.active)}</button>` : ""}<button class="ghost small" onclick="pp.camRefresh('${cam.id}')">Refresh capabilities</button></div>
        ${cam.error ? `<div class="alert error" style="margin-top:8px">${E(cam.error)}</div>` : ""}
        <div style="color:var(--t3);font-size:11px;margin-top:10px">Profile ${E(cam.profileToken || "?")} · media ${E(cam.mediaUrl || "?")}<br>PTZ ${E(cam.ptzUrl || "none")}</div></div>
    </div></div>` : ""}
  </div>`;
}

// ---------------------------------------------------------------------------
// CG: HTML template pack
// ---------------------------------------------------------------------------
export function viewCgHtml() {
  const S = pp.S; const tpls = I.cg.templates || []; const cats = [...new Set(tpls.map(t => t.category))];
  if (!I.cgCat && cats.length) I.cgCat = cats[0];
  const list = tpls.filter(t => (!I.cgCat || t.category === I.cgCat) && (!I.cgFilter || t.name.toLowerCase().includes(I.cgFilter.toLowerCase()) || t.id.includes(I.cgFilter.toLowerCase())));
  const sel = tpls.find(t => t.id === I.cgSel);
  const cur = (S.snapshot.cgHtml || {})[S.active + (I.cgOutput === 1 ? "" : ":" + I.cgOutput)];
  const url = sel ? cgPreviewUrl(sel) : "";
  const field = p => { const v = I.cgParams[p.name] !== undefined ? I.cgParams[p.name] : (p.default || ""); const on = `oninput="I.cgParams['${p.name}']=this.value;pp.cgPreviewSoon()"`;
    if (p.type === "select") return `<label class="f">${E(p.label)} <select ${on.replace("oninput", "onchange")}>${(p.options || []).map(o => `<option ${o === v ? "selected" : ""}>${E(o)}</option>`).join("")}</select></label>`;
    if (p.type === "color") return `<label class="f">${E(p.label)} <span class="row"><input type="color" value="#${String(v).replace("#", "") || "00d4ff"}" onchange="I.cgParams['${p.name}']=this.value.replace('#','');pp.cgPreviewSoon()"><input value="${E(v)}" ${on} style="width:90px"></span></label>`;
    if (p.type === "number") return `<label class="f">${E(p.label)} <input type="number" value="${E(v)}" ${on}></label>`;
    return `<label class="f">${E(p.label)} <input value="${E(v)}" ${on} placeholder="${E(p.default || "")}"></label>`; };
  return `<div class="panel" style="grid-column:1/-1"><h3>HTML templates (${tpls.length}) <span class="right">${cur && cur.url ? bdg("deployed: " + cur.name, "var(--ready)") + `<button class="ghost small" onclick="pp.cgHtmlClear()">clear</button>` : bdg("nothing deployed", "var(--t3)")}
      <a class="btn small" target="_blank" href="/cg/output.html?channel=${encodeURIComponent(S.active)}&output=${I.cgOutput}${S.token ? "&token=" + encodeURIComponent(S.token) : ""}&debug=1">Open output page &#8599;</a></span></h3>
    <div class="row" style="padding:8px">${Array.from({length:8},(_,i)=>`<button class="small ${I.cgOutput===i+1?'active':''}" onclick="pp.cgOutput(${i+1})">CG ${i+1}</button>`).join('')}</div>
    <div class="body" style="display:grid;grid-template-columns:260px 1fr 1.2fr;gap:12px">
      <div><input placeholder="filter" value="${E(I.cgFilter)}" oninput="I.cgFilter=this.value;pp.render()" style="width:100%;margin-bottom:6px">
        <div class="row" style="margin-bottom:6px">${cats.map(c => `<button class="ghost small${c === I.cgCat ? " active" : ""}" onclick="I.cgCat='${c}';pp.render()">${E(c)}</button>`).join("")}</div>
        <ul class="tpls">${list.map(t => `<li class="${t.id === I.cgSel ? "sel" : ""}" onclick="pp.cgPick('${t.id}')"><span>${E(t.icon || "▭")}</span><b>${E(t.name)}</b><small>${E(t.id)}</small></li>`).join("") || `<li class="empty">no templates</li>`}</ul></div>
      <div>${sel ? `<div style="font-weight:700;margin-bottom:2px">${E(sel.name)} <span style="color:var(--t3);font-weight:400">${E(sel.description || "")}</span></div>
        <div class="grid" style="gap:8px;grid-template-columns:1fr 1fr">${cgTextParams(sel).concat(sel.params).map(field).join("")}</div>
        <div class="grid" style="gap:8px;grid-template-columns:1fr 1fr 1fr 1fr;margin-top:10px">
          <label class="f">Anim in <select onchange="I.cgAnim.in=this.value;pp.cgPreviewSoon()">${(I.cg.animations || ["none", "fade"]).map(a => `<option ${a === I.cgAnim.in ? "selected" : ""}>${a}</option>`).join("")}</select></label>
          <label class="f">Anim out <select onchange="I.cgAnim.out=this.value">${(I.cg.animations || ["none", "fade"]).map(a => `<option ${a === I.cgAnim.out ? "selected" : ""}>${a}</option>`).join("")}</select></label>
          <label class="f">Duration ms <input type="number" value="${I.cgAnim.duration}" oninput="I.cgAnim.duration=parseInt(this.value)||600"></label>
          <label class="f">Auto-hide ms <input type="number" value="${I.cgAnim.autoHide}" oninput="I.cgAnim.autoHide=parseInt(this.value)||0"></label></div>
        <div class="row" style="margin-top:6px">${(I.cg.presets || []).map(p => `<button class="ghost small" onclick="I.cgAnim={in:'${p.in}',out:'${p.out}',duration:${p.duration},autoHide:I.cgAnim.autoHide,easing:'${E(p.easing)}'};pp.cgPreviewSoon()" title="${p.in} / ${p.out} ${p.duration} ms">${E(p.icon)} ${E(p.name)}</button>`).join("")}</div>
        <div class="row" style="margin-top:12px"><label class="f">Deliver <select onchange="I.cgOnAir=this.value"><option value="" ${I.cgOnAir === "" ? "selected" : ""}>output page only (browser source / NDI browser)</option><option value="native" ${I.cgOnAir === "native" ? "selected" : ""}>+ native equivalent on air</option><option value="html" ${I.cgOnAir === "html" ? "selected" : ""}>+ HTML layer on air (WebEngine build)</option></select></label>
          <span class="fill"></span><button class="success" onclick="pp.cgHtmlDeploy()">DEPLOY to ${E(S.active)}</button><button class="ghost" onclick="pp.cgHtmlClear()">CLEAR</button></div>
        <div style="color:var(--t3);font-size:11px;margin-top:8px">Role ${E(sel.role)} · <span class="mono" style="word-break:break-all">${E(url)}</span></div>` : `<div class="empty">Pick a template</div>`}</div>
      <div><div class="cgprev"><iframe id="cgPrev" src="${E(url)}" title="preview"></iframe></div><div style="color:var(--t3);font-size:11px;margin-top:4px">Live preview (1920×1080 scaled). The deployed template plays on CG output ${I.cgOutput}.</div></div>
    </div></div>` + viewCgCaptions();
}

// ---------------------------------------------------------------------------
// CG: captions (karaoke from transcripts, layer 31) and fact box (layer 32)
// ---------------------------------------------------------------------------
export function viewCgCaptions() {
  const S = pp.S; const C = I.cap; const k = C.k || {}; const f = C.f;
  const sel = (id, opts, cur, on) => `<select onchange="${on}">${opts.map(o => `<option value="${E(o)}" ${o === cur ? "selected" : ""}>${E(o)}</option>`).join("")}</select>`;
  const presetOpts = (C.presets || []).map(p => `<option value="${E(p.id)}" ${p.id === C.presetId ? "selected" : ""}>${E(p.name)}</option>`).join("");
  return `<style>
    .capgrid { display:grid; grid-template-columns:1fr 1fr; gap:14px } .capgrid .col { display:flex; flex-direction:column; gap:8px }
    .capgrid .two { display:grid; grid-template-columns:1fr 1fr; gap:8px } .capgrid textarea { width:100%; min-height:96px; resize:vertical; font:inherit; font-size:12px }
    .capgrid .sw { display:inline-block; width:12px; height:12px; border-radius:3px; border:1px solid var(--b1); vertical-align:middle; margin-right:4px }
    @media (max-width: 1100px) { .capgrid { grid-template-columns:1fr } }
  </style>
  <div class="panel" style="grid-column:1/-1"><h3>Captions &amp; Fact box <span class="right">${k.shown ? bdg("captions on air", "var(--ready)") : bdg("captions off", "var(--t3)")} ${k.follow ? bdg("following program", "var(--accent)") : ""}</span></h3>
    <div class="body capgrid">
      <div class="col">
        <div style="font-size:10px;letter-spacing:1px;color:var(--t2)">KARAOKE CAPTIONS — layer 31, synced to the program item from its transcript</div>
        <div class="row" style="align-items:center;gap:8px">${k.hasTranscript ? bdg("transcript ✓", "var(--ready)") : bdg("no transcript", "var(--t3)")}${k.itemAsks ? bdg("item asks for captions", "var(--accent)") : ""}<span style="color:var(--t1);font-size:12px">${E(k.onAirTitle || k.onAirAssetId || "nothing on air")}</span></div>
        <div class="two"><label class="f">Media id (blank = asset on air) <input class="mono" value="${E(C.kMediaId)}" placeholder="${E(k.onAirAssetId || "")}" oninput="I.cap.kMediaId=this.value"></label>
          <label class="f">Follow program <select onchange="pp.karaokeFollow(this.value==='1')"><option value="0" ${!k.follow ? "selected" : ""}>off — one shot</option><option value="1" ${k.follow ? "selected" : ""}>on — re-issue as items change</option></select></label></div>
        <div class="two"><label class="f">Position ${sel("kpos", ["bottom", "lower_third", "top", "center"], C.kPosition, "I.cap.kPosition=this.value")}</label>
          <label class="f">Mode ${sel("kmode", ["word", "line"], C.kMode, "I.cap.kMode=this.value")}</label>
          <label class="f">Font px (1080p) <input type="number" value="${C.kFontSize}" oninput="I.cap.kFontSize=parseInt(this.value)||46"></label>
          <label class="f">Sync trim (s) <input type="number" step="0.05" value="${C.kSync}" oninput="I.cap.kSync=parseFloat(this.value)||0"></label>
          <label class="f">Text <span class="row"><input type="color" value="${E(C.kColor)}" onchange="I.cap.kColor=this.value"><input class="mono" value="${E(C.kColor)}" oninput="I.cap.kColor=this.value" style="width:90px"></span></label>
          <label class="f">Highlight <span class="row"><input type="color" value="${E(C.kHighlight)}" onchange="I.cap.kHighlight=this.value"><input class="mono" value="${E(C.kHighlight)}" oninput="I.cap.kHighlight=this.value" style="width:90px"></span></label>
          <label class="f">Spoken <span class="row"><input type="color" value="${E(C.kPast)}" onchange="I.cap.kPast=this.value"><input class="mono" value="${E(C.kPast)}" oninput="I.cap.kPast=this.value" style="width:90px"></span></label>
          <label class="f">Box (#aarrggbb) <input class="mono" value="${E(C.kBg)}" oninput="I.cap.kBg=this.value"></label></div>
        <div class="row"><button class="success" onclick="pp.karaokeShow()">SHOW on ${E(S.active)}</button><button class="ghost" onclick="pp.karaokeHide()">HIDE</button><span class="fill"></span><a class="btn small" target="_blank" href="/cg/karaoke.html?channel=${encodeURIComponent(S.active)}${S.token ? "&token=" + encodeURIComponent(S.token) : ""}&position=${encodeURIComponent(C.kPosition)}">HTML version &#8599;</a></div>
        <div style="color:var(--t3);font-size:11px">Words come from the media's transcript (Library → Transcribe). The engine adds the item's in point so file-relative word times line up with the program position. A schedule item can carry <code>"karaoke"</code> in <code>cgOnStart</code>: the Supervisor expands it when the item airs and hides the layer when the next asset has no transcript.</div>
      </div>
      <div class="col">
        <div style="font-size:10px;letter-spacing:1px;color:var(--t2)">FACT BOX — layer 32</div>
        <label class="f">Title <input value="${E(f.title)}" oninput="I.cap.f.title=this.value"></label>
        <label class="f">Lines (one per line; "label: value" rows become label/value pairs when every line has one) <textarea oninput="I.cap.f.lines=this.value">${E(f.lines)}</textarea></label>
        <div class="two"><label class="f">Position ${sel("fpos", ["right", "left", "top-right", "top-left", "bottom-left", "bottom-right", "center"], f.position, "I.cap.f.position=this.value")}</label>
          <label class="f">Accent <span class="row"><input type="color" value="${E(f.accentColor)}" onchange="I.cap.f.accentColor=this.value"><input class="mono" value="${E(f.accentColor)}" oninput="I.cap.f.accentColor=this.value" style="width:90px"></span></label>
          <label class="f">Width px (1080p) <input type="number" value="${f.width}" oninput="I.cap.f.width=parseInt(this.value)||520"></label>
          <label class="f">Reveal <select onchange="I.cap.f.reveal=this.value==='lines'"><option value="all" ${!f.reveal ? "selected" : ""}>all at once</option><option value="lines" ${f.reveal ? "selected" : ""}>line by line</option></select></label>
          <label class="f">Reveal every (s) <input type="number" step="0.1" value="${f.revealIntervalSec}" oninput="I.cap.f.revealIntervalSec=parseFloat(this.value)||1.2"></label>
          <label class="f">Footer / source <input value="${E(f.footer)}" oninput="I.cap.f.footer=this.value"></label></div>
        <div class="row"><button class="success" onclick="pp.factboxShow('show')">SHOW</button><button class="primary" onclick="pp.factboxShow('update')">UPDATE</button><button class="ghost" onclick="pp.factboxShow('hide')">HIDE</button></div>
        <div class="row" style="align-items:flex-end"><label class="f" style="flex:1">Presets (${(C.presets || []).length}) <select id="fbPreset" onchange="I.cap.presetId=this.value">${presetOpts || `<option value="">none saved</option>`}</select></label><button class="small" onclick="pp.factboxPresetLoad()">Load</button><button class="ghost small" onclick="pp.factboxPresetDelete()">&#10005;</button></div>
        <div class="row" style="align-items:flex-end"><label class="f" style="flex:1">Save as <input value="${E(C.presetName)}" placeholder="${E(f.title)}" oninput="I.cap.presetName=this.value"></label><button class="small primary" onclick="pp.factboxPresetSave()">Save preset</button></div>
      </div>
    </div></div>`;
}
function factboxSpecFromForm() {
  const f = I.cap.f; const rawLines = String(f.lines || "").split("\n").map(s => s.trim()).filter(Boolean);
  const spec = { title: f.title, position: f.position, accentColor: f.accentColor, width: f.width, reveal: f.reveal ? "lines" : "all", revealIntervalSec: f.revealIntervalSec, footer: f.footer };
  const pairs = rawLines.map(l => l.match(/^([^:]{1,40}):\s*(.+)$/));
  if (rawLines.length && pairs.every(Boolean)) spec.items = pairs.map(m => ({ label: m[1].trim(), value: m[2].trim() })); else spec.lines = rawLines;
  return spec;
}
// Text 1/2/3: any template takes text1..3; the manifest's textFields names the template fields they fill (the Supervisor maps them on deploy too).
const cgTextParams = t => (t.textFields || []).map((f, i) => ({ name: "text" + (i + 1), label: "Text " + (i + 1) + " → " + ((t.params.find(p => p.name === f) || {}).label || f), type: "text", default: "" }));
function cgPreviewUrl(t) {
  const q = new URLSearchParams();
  for (const p of t.params) { const v = I.cgParams[p.name] !== undefined ? I.cgParams[p.name] : (p.default || ""); if (v !== "") q.set(p.name, v); }
  (t.textFields || []).forEach((f, i) => { const v = I.cgParams["text" + (i + 1)]; if (v && !I.cgParams[f]) q.set(f, v); });
  const a = I.cgAnim; if (a.in && a.in !== "none") { q.set("animIn", a.in); q.set("animInDuration", a.duration); } if (a.out && a.out !== "none") { q.set("animOut", a.out); q.set("animOutDuration", a.duration); } if (a.easing && a.easing !== "ease-out") q.set("animEasing", a.easing);
  return "/cg/" + t.id + ".html" + (q.toString() ? "?" + q.toString() : "");
}
let prevTimer;
export const ppExtIntegrations = {
  // inputs
  scanInputs() { pp.POST("/inputs/scan", {}).then(() => { pp.toast("Scan requested", "ok"); setTimeout(loadInputs, 2500); setTimeout(loadInputs, 5000); }); },
  saveNdiConfig() { pp.PUT("/inputs/ndi-config", { groups: $$("#ndiGroups").value.trim(), extraIps: $$("#ndiIps").value.trim() }).then(() => { pp.toast("NDI scan settings saved", "ok"); setTimeout(loadInputs, 4000); }); },
  ndiPip(name) { pp.cg(pp.S.active, { action: "show", role: "secondary", source: { type: "ndi", name }, placement: { x: 1280, y: 80, width: 560, height: 315 }, in: { type: "fade", durationSec: 0.3 }, audioMute: true }).then(() => pp.toast("PiP " + name, "ok")); },
  registerLive(kind, ref) { const src = kind === "ndi" ? { type: "ndi", name: ref } : { type: "stream", url: ref, realtime: true }; pp.POST("/media", { path: "live://" + kind + "/" + ref, title: ref, type: "live", duration: 1800, liveSource: src }).then(() => pp.toast("Registered in the library", "ok")); },
  camDiscover() { pp.POST("/cameras/discover", { waitMs: 3000 }).then(() => { pp.toast("Discovering cameras…"); setTimeout(loadInputs, 3500); }); },
  camSave() { const host = $$("#camHost").value.trim(); const body = { name: $$("#camName").value.trim(), user: $$("#camUser").value.trim(), pass: $$("#camPass").value }; if (host) { if (/^https?:/i.test(host)) body.xaddr = host; else body.host = host; } else if (I.camSel) body.id = I.camSel; else { pp.toast("Host required"); return; }
    pp.POST("/cameras", body).then(d => { I.camSel = d.id; I.camForm = { host: "", name: "", user: "", pass: "" }; pp.toast("Camera saved", "ok"); setTimeout(loadInputs, 1500); setTimeout(loadInputs, 4000); }); },
  camRemove(id) { pp.confirmDo("Forget this camera and its credentials?", () => pp.api("DELETE", "/cameras/" + id).then(() => loadInputs())); },
  camRefresh(id) { pp.POST("/cameras/" + id + "/refresh").then(() => { pp.toast("Refreshing…"); setTimeout(loadInputs, 2500); }); },
  camRegister(id) { pp.POST("/cameras/" + id + "/register", {}).then(d => pp.toast("Registered " + d.title + " as a live source", "ok")); },
  camPip(id) { const c = (I.inputs.cameras || []).find(x => x.id === id); if (!c || !c.streamUri) return; pp.cg(pp.S.active, { action: "show", role: "secondary", source: { type: "stream", url: c.streamUri, realtime: true }, placement: { x: 1280, y: 80, width: 560, height: 315 }, in: { type: "fade", durationSec: 0.3 }, audioMute: true }).then(() => pp.toast("Camera PiP", "ok")); },
  ptz(cmd) { if (!I.camSel) return; pp.POST("/cameras/" + I.camSel + "/ptz", cmd).then(() => { if (cmd.action === "setPreset") setTimeout(loadInputs, 1200); }).catch(() => {}); },
  ptzMove(x, y, z) { pp.ptz({ action: "move", x: x * I.ptzSpeed, y: y * I.ptzSpeed, z: z * I.ptzSpeed }); },
  ptzStop() { pp.ptz({ action: "stop" }); },
  // cg html
  cgOutput(n) { I.cgDrafts[I.cgOutput] = {sel:I.cgSel, params:I.cgParams, anim:I.cgAnim, cat:I.cgCat, onAir:I.cgOnAir}; I.cgOutput = n; const d = I.cgDrafts[n] || {}; I.cgSel = d.sel || ''; I.cgParams = d.params || {}; I.cgAnim = d.anim || {in:'fade',out:'fade',duration:600,autoHide:0}; I.cgCat = d.cat || I.cgCat; I.cgOnAir = d.onAir || ''; pp.render(); },
  cgPick(id) { I.cgSel = id; I.cgParams = {}; pp.render(); },
  cgPreviewSoon() { clearTimeout(prevTimer); prevTimer = setTimeout(() => { const t = (I.cg.templates || []).find(x => x.id === I.cgSel); const f = $$("#cgPrev"); if (t && f) f.src = cgPreviewUrl(t); }, 400); },
  cgHtmlDeploy() { const t = (I.cg.templates || []).find(x => x.id === I.cgSel); if (!t) return; const params = {}; for (const p of t.params) { const v = I.cgParams[p.name] !== undefined ? I.cgParams[p.name] : (p.default || ""); if (v !== "") params[p.name] = v; }
    for (const p of cgTextParams(t)) if (I.cgParams[p.name]) params[p.name] = I.cgParams[p.name];
    const output = I.cgOutput, ch = pp.S.active; pp.POST("/cg/html/" + ch, { template: t.id, params, animation: I.cgAnim, onAir: I.cgOnAir, output }).then(d => { (pp.S.snapshot.cgHtml = pp.S.snapshot.cgHtml || {})[ch + (output === 1 ? '' : ':' + output)] = d; pp.toast("Deployed " + t.name + " to CG " + output, "ok"); pp.render(); }); },
  cgHtmlClear() { const output=I.cgOutput, ch=pp.S.active; pp.POST("/cg/html/" + ch + "/clear", {output}).then(() => { if (pp.S.snapshot.cgHtml) delete pp.S.snapshot.cgHtml[ch + (output === 1 ? '' : ':' + output)]; pp.toast("CG " + output + " cleared", "ok"); pp.render(); }); },
  // captions
  karaokeBody(extra) { const C = I.cap; const b = { layer: 31, role: "lower_third", mode: C.kMode, position: C.kPosition, fontSize: C.kFontSize, color: C.kColor, highlightColor: C.kHighlight, pastColor: C.kPast, bgColor: C.kBg, syncOffsetSec: C.kSync }; if (C.kMediaId.trim()) b.mediaId = C.kMediaId.trim(); return Object.assign(b, extra || {}); },
  karaokeShow() { pp.POST("/channels/" + pp.S.active + "/cg/karaoke", pp.karaokeBody({ action: "show" })).then(d => { pp.toast(d.shown ? "Captions on: " + d.words + " words from " + d.mediaId : "Follow armed" + (d.warning ? " — " + d.warning : ""), "ok"); loadCaptions(); }).catch(e => pp.toast("Captions: " + (e.message || e), "err")); },
  karaokeHide() { pp.POST("/channels/" + pp.S.active + "/cg/karaoke", { action: "hide" }).then(() => { pp.toast("Captions off", "ok"); loadCaptions(); }); },
  karaokeFollow(on) { if (on) pp.POST("/channels/" + pp.S.active + "/cg/karaoke", pp.karaokeBody({ follow: true, action: "show" })).then(d => { pp.toast(d.shown ? "Following program; captions on" : "Following program" + (d.warning ? " — " + d.warning : ""), "ok"); loadCaptions(); }).catch(e => pp.toast("Captions: " + (e.message || e), "err"));
    else pp.POST("/channels/" + pp.S.active + "/cg/karaoke", { follow: false, action: "hide" }).then(() => { pp.toast("Follow off", "ok"); loadCaptions(); }); },
  factboxShow(action) { const body = Object.assign({ layer: 32, action: action || "show" }, factboxSpecFromForm()); pp.POST("/channels/" + pp.S.active + "/cg/factbox", body).then(() => pp.toast("Fact box " + (action || "show"), "ok")).catch(e => pp.toast("Fact box: " + (e.message || e), "err")); },
  factboxPresetSave() { const C = I.cap; pp.PUT("/cg/factbox/presets", { name: C.presetName.trim() || C.f.title || "Fact box", spec: factboxSpecFromForm() }).then(d => { C.presets = d.presets || []; pp.toast("Preset saved", "ok"); pp.render(); }); },
  factboxPresetLoad() { const C = I.cap; const id = ($$("#fbPreset") || {}).value || C.presetId; const p = (C.presets || []).find(x => x.id === id); if (!p) { pp.toast("Pick a preset"); return; } const s = p.spec || {}; C.presetId = id; C.presetName = p.name || "";
    const lines = Array.isArray(s.items) && s.items.length ? s.items.map(i => i.label + ": " + i.value) : Array.isArray(s.lines) ? s.lines : String(s.lines || "").split("\n");
    C.f = { title: s.title || "", lines: lines.join("\n"), position: s.position || "right", accentColor: s.accentColor || "#00d4ff", width: s.width || 520, reveal: s.reveal === "lines", revealIntervalSec: s.revealIntervalSec || 1.2, footer: s.footer || "" }; pp.toast("Loaded " + p.name, "ok"); pp.render(); },
  factboxPresetDelete() { const C = I.cap; const id = ($$("#fbPreset") || {}).value || C.presetId; if (!id) return; pp.confirmDo("Delete this fact box preset?", () => pp.PUT("/cg/factbox/presets", { deleteId: id }).then(d => { C.presets = d.presets || []; C.presetId = ""; pp.render(); })); },
  // mesh
  meshBrowse(node, path) { meshBrowse(node, path); },
  meshUp() { const p = I.mesh.path.replace(/[\\/]+$/, ""); const i = Math.max(p.lastIndexOf("\\"), p.lastIndexOf("/")); meshBrowse(I.mesh.node, i > 1 ? p.slice(0, i + 1) : (p.length > 2 ? p.slice(0, 3) : "")); },
  meshGo() { meshBrowse(I.mesh.node, $$("#meshPath").value.trim()); },
  meshSearch() { const q = $$("#meshQ").value.trim(); I.mesh.q = q; if (!q) return; pp.GET("/mesh/search?q=" + encodeURIComponent(q)).then(d => { I.mesh.hits = d.hits || []; I.mesh.listing = { items: [] , search: true }; pp.render(); }); },
  meshFetch(node, path) { pp.POST("/mesh/fetch", { node, path, type: I.mesh.fetchType }).then(j => { pp.toast("Fetching " + j.name + (j.state === "done" ? " (already here)" : ""), "ok"); I.mesh.jobs = I.mesh.jobs.filter(x => x.id !== j.id).concat([j]); pp.render(); }); },
  meshCancel(id) { pp.api("DELETE", "/mesh/jobs/" + id).then(() => pp.GET("/mesh/jobs").then(j => { I.mesh.jobs = j.jobs || []; pp.render(); })); },
  meshClear() { pp.POST("/mesh/jobs/clear", {}).then(j => { I.mesh.jobs = j.jobs || []; pp.render(); }); },
  meshPush(mediaId) { const node = $$("#pushNode") ? $$("#pushNode").value : I.mesh.node; const folder = $$("#pushFolder") ? $$("#pushFolder").value.trim() : ""; pp.POST("/mesh/push", { mediaId, node, folder }).then(j => { pp.toast("Pushing " + j.name + " to " + (j.nodeName || node), "ok"); I.mesh.jobs.push(j); pp.render(); }); },
  meshConfig() { pp.PUT("/mesh/config", { url: $$("#meshUrl").value.trim(), token: $$("#meshToken").value.trim(), cookie: $$("#meshCookie").value.trim() }).then(() => { pp.toast("Mesh settings saved", "ok"); I.mesh.nodes = []; I.mesh.listing = null; loadMesh(); }); },
};

// ---------------------------------------------------------------------------
// Mesh console
// ---------------------------------------------------------------------------
export function viewMesh() {
  const S = pp.S; const M = I.mesh; const st = M.status || {}; const node = M.nodes.find(n => n.id === M.node) || {};
  const nodeRows = M.nodes.map(n => `<li class="${n.id === M.node ? "sel" : ""}" onclick="pp.meshBrowse('${n.id}','${E((n.sharedFolders && n.sharedFolders[0] ? n.sharedFolders[0].path : "") || "").replace(/\\/g, "\\\\")}')"><span class="dot${n.online ? "" : " off"}"></span><b>${E(n.machineName || n.hostname)}</b><small class="mono">${E(n.ip)}${n.isLocal ? " · bridge" : ""}${n.relay ? " · relay" : ""}</small>
    <div class="drives">${(n.drives || []).map(d => `<span title="${d.freeGb} GB free of ${d.sizeGb}">${E(d.letter)} <i style="width:${d.sizeGb ? Math.round(100 * (1 - d.freeGb / d.sizeGb)) : 0}%"></i></span>`).join("")}</div></li>`).join("");
  const shares = (node.sharedFolders || []).map(s => `<button class="ghost small" onclick="pp.meshBrowse('${node.id}','${E(s.path).replace(/\\/g, "\\\\")}')">${E(s.name)}</button>`).join("");
  const L = M.listing; const items = L && !L.search ? (L.items || []) : [];
  const dirs = items.filter(i => i.isDirectory), files = items.filter(i => !i.isDirectory);
  const fileRow = (f, nodeId, nodeName) => `<tr><td>${E(f.name)}</td><td class="mono" style="color:var(--t2)">${fmtBytes(f.size)}</td><td class="mono" style="color:var(--t3)">${f.mtime ? new Date(f.mtime).toLocaleDateString() : ""}</td>${nodeName !== undefined ? `<td>${E(nodeName)}</td>` : ""}<td>${f.swarmNodes ? bdg(f.swarmNodes.length + " copies", "var(--accent2)") : ""}${f.publicLink ? bdg("public", "var(--warn)") : ""}</td>
    <td><button class="small" onclick="pp.meshFetch('${E(nodeId)}','${E(f.path).replace(/\\/g, "\\\\").replace(/'/g, "&#39;")}')">&#8681; Fetch</button></td></tr>`;
  const crumbs = (M.path || "").split(/[\\/]/).filter(Boolean); let acc = "";
  const crumbHtml = crumbs.map((c, i) => { acc += c + "\\"; const p = acc; return `<a href="#" onclick="pp.meshBrowse('${M.node}','${E(p).replace(/\\/g, "\\\\")}');return false">${E(c)}</a>`; }).join(" \\ ");
  const jobRow = j => `<tr><td>${j.kind === "fetch" ? "&#8681;" : "&#8679;"} <b>${E(j.name)}</b><div style="color:var(--t3);font-size:10px">${E(j.kind === "fetch" ? (j.nodeName || j.node || "server") + ": " + j.remotePath : "→ " + (j.nodeName || j.node) + " " + j.remotePath)}</div></td>
    <td style="width:30%"><div class="meter"><div class="bar" style="width:${j.total ? Math.min(100, 100 * j.bytes / j.total) : (j.state === "done" ? 100 : 0)}%;background:${j.state === "failed" ? "var(--danger)" : j.state === "done" ? "var(--ready)" : j.state === "cancelled" ? "var(--t3)" : "var(--accent)"}"></div></div></td>
    <td class="mono" style="color:var(--t2)">${fmtBytes(j.bytes)}${j.total ? " / " + fmtBytes(j.total) : ""}${j.state === "running" && j.rateBps ? " · " + fmtBytes(j.rateBps) + "/s" : ""}</td>
    <td>${j.state === "failed" ? `<span style="color:var(--danger)" title="${E(j.error)}">failed: ${E(j.error).slice(0, 60)}</span>` : bdg(j.state, j.state === "done" ? "var(--ready)" : j.state === "running" ? "var(--accent)" : "var(--t2)")}</td>
    <td>${j.state === "running" || j.state === "queued" ? `<button class="ghost small" onclick="pp.meshCancel('${j.id}')">&#10005;</button>` : j.state === "failed" && j.kind === "fetch" ? `<button class="small" onclick="pp.meshFetch('${E(j.node)}','${E(j.remotePath).replace(/\\/g, "\\\\").replace(/'/g, "&#39;")}')">Resume</button>` : `<button class="ghost small" onclick="pp.meshCancel('${j.id}')">&#10005;</button>`}</td></tr>`;
  const myMedia = (S.media || []).filter(m => m.type !== "live").slice(0, 400);
  return `<div class="grid" style="grid-template-columns:260px 1fr">
    <div class="panel"><h3>Mesh nodes (${M.nodes.length}) <span class="right"><span class="dot${st.lastError ? " off" : ""}" title="${E(st.lastError || st.url)}"></span><button class="small" onclick="pp.render();I.mesh.nodes=[];I.mesh.listing=null;onEnterIntegration('mesh')">&#8635;</button></span></h3>
      <ul class="nodes">${nodeRows || `<li class="empty">${st.lastError ? E(st.lastError) : "loading…"}</li>`}</ul>
      <div class="body"><details><summary style="cursor:pointer;color:var(--t2);font-size:11px">Bridge settings</summary>
        <label class="f" style="margin-top:6px">AN-FS bridge URL <input id="meshUrl" value="${E(st.url || "")}"></label>
        <label class="f">Bearer token (uploads) <input id="meshToken" type="password" placeholder="${st.hasToken ? "•••• set" : "optional"}"></label>
        <label class="f">Session cookie (alternative) <input id="meshCookie" type="password" placeholder="${st.hasCookie ? "•••• set" : "optional"}"></label>
        <button class="primary small" style="margin-top:6px" onclick="pp.meshConfig()">Save</button>
        <div style="color:var(--t3);font-size:11px;margin-top:6px">Fetched files land in ${E(st.downloadRoot || "")} and are probed into the library automatically.</div></details></div></div>
    <div class="grid" style="gap:12px">
      <div class="panel"><h3>${E(node.machineName || node.hostname || "Browse")} <span class="right">${shares}<input id="meshQ" placeholder="search the whole mesh" value="${E(M.q)}" onkeydown="if(event.key==='Enter')pp.meshSearch()" style="width:200px"><button class="small" onclick="pp.meshSearch()">Search</button></span></h3>
        <div class="body" style="padding:8px 12px"><div class="row"><button class="ghost small" onclick="pp.meshUp()">&#8593;</button><span class="mono" style="color:var(--t1)">${E(node.hostname || "")}: ${crumbHtml || E(M.path)}</span><span class="fill"></span><input id="meshPath" value="${E(M.path)}" style="width:280px" onkeydown="if(event.key==='Enter')pp.meshGo()"><button class="small" onclick="pp.meshGo()">Go</button><label class="f">Import as <select class="small" onchange="I.mesh.fetchType=this.value">${["program", "ad", "promo", "station_id", "bumper", "filler", "psa", "image"].map(t => `<option ${t === M.fetchType ? "selected" : ""}>${t}</option>`).join("")}</select></label></div></div>
        <div class="scroll" style="max-height:46vh">${L && L.search ? `<table><thead><tr><th>File</th><th>Size</th><th>Modified</th><th>Node</th><th></th><th></th></tr></thead><tbody>${M.hits.length ? M.hits.map(h => fileRow(h, h.nodeId, h.nodeName)).join("") : `<tr><td colspan="6" class="empty">No matches for “${E(M.q)}”</td></tr>`}</tbody></table>`
          : L && L.error ? `<div class="alert error">${E(L.error)}</div>` : !L ? `<div class="empty">loading…</div>` : `<table><thead><tr><th>Name</th><th>Size</th><th>Modified</th><th></th><th></th></tr></thead><tbody>
          ${dirs.map(d => `<tr style="cursor:pointer" onclick="pp.meshBrowse('${M.node}','${E(d.path).replace(/\\/g, "\\\\").replace(/'/g, "&#39;")}')"><td>&#128193; <b>${E(d.name)}</b></td><td></td><td class="mono" style="color:var(--t3)">${d.mtime ? new Date(d.mtime).toLocaleDateString() : ""}</td><td></td><td></td></tr>`).join("")}
          ${files.map(f => fileRow(f, M.node)).join("")}${!items.length ? `<tr><td colspan="5" class="empty">Empty folder</td></tr>` : ""}</tbody></table>`}</div></div>
      <div class="grid cols2">
        <div class="panel"><h3>Transfers (${M.jobs.length}) <span class="right"><button class="ghost small" onclick="pp.meshClear()">clear finished</button></span></h3><div class="scroll" style="max-height:30vh"><table><tbody>${M.jobs.slice().reverse().map(jobRow).join("") || `<tr><td class="empty">No transfers yet</td></tr>`}</tbody></table></div></div>
        <div class="panel"><h3>Push library media to a node <span class="right"><select id="pushNode" class="small">${M.nodes.map(n => `<option value="${n.id}" ${n.id === M.node ? "selected" : ""}>${E(n.machineName || n.hostname)}</option>`).join("")}</select><input id="pushFolder" placeholder="folder (default: drop folder)" style="width:200px"></span></h3>
          <div class="scroll" style="max-height:30vh"><table><tbody>${myMedia.map(m => `<tr><td><b>${E(m.title)}</b><div class="mono" style="color:var(--t3);font-size:10px">${E(m.path)}</div></td><td class="mono">${m.fileSizeBytes ? fmtBytes(m.fileSizeBytes) : ""}</td><td><button class="small" onclick="pp.meshPush('${m.id}')">&#8679; Push</button></td></tr>`).join("") || `<tr><td class="empty">Library is empty</td></tr>`}</tbody></table></div>
          <div class="body" style="color:var(--t3);font-size:11px">Uploads use the bridge's resumable 2 MiB chunk protocol and need a mesh login (token or cookie in Bridge settings).</div></div>
      </div></div></div>`;
}
