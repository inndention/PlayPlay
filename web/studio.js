/* PLAY PLAY web dashboard — Spot studio (docs/spot-studio.md): guided spot projects. Brief ► concepts + draft
   script + questions ► answers / scene edits / end card / instruction ► generate (local or studio engine) ► media
   lands archived ► review (submit / approve / changes / reject) ► publish (ad rotation contract, optional social
   draft). Plus "revise an auto spot". All state lives in ST (window.STS); every input writes into ST as it is
   typed, and the open project is an editable copy that a refresh only replaces when it holds no unsaved edits. */

const today = () => { const d = new Date(); return d.getFullYear() + "-" + String(d.getMonth() + 1).padStart(2, "0") + "-" + String(d.getDate()).padStart(2, "0"); };
const plusDays = n => { const d = new Date(); d.setDate(d.getDate() + n); return d.getFullYear() + "-" + String(d.getMonth() + 1).padStart(2, "0") + "-" + String(d.getDate()).padStart(2, "0"); };
const blankNew = () => ({ title: "", kind: "ad", client: "", brief: "", lengthSec: 30, aspect: "16:9", template: "", engine: "local", voice: "", music: "", tone: "", market: "", brandName: "", brandCta: "", brandUrl: "" });
const ST = {
  loaded: false, loading: false, projects: [], options: {}, markets: { markets: [] }, sel: "", p: null, dirty: false, newOpen: false, nw: blankNew(), busy: "",
  answers: {}, instruction: "", gen: { step: "all", engine: "" }, reviewNotes: "",
  pub: { type: "ad", client: "", title: "", channels: [], startDate: today(), endDate: plusDays(30), contractMode: "new", contractId: "", spotsPerDay: 4, spotsTotal: 0, priority: 5, socialQueue: false, socialMessage: "" },
  revise: { mediaId: "", notes: "", voice: "" },
};
window.STS = ST;
const esc = s => String(s ?? "").replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
const js = v => esc(JSON.stringify(String(v ?? "")));
const bdg = (text, color) => `<span class="badge" style="color:${color};border-color:${color};background:color-mix(in srgb,${color} 16%,transparent)">${esc(text)}</span>`;
const STATUS = { draft: "var(--t2)", rendering: "var(--accent)", rendered: "var(--cued)", in_review: "var(--warn)", changes_requested: "var(--standby)", approved: "var(--ready)", rejected: "var(--danger)", published: "var(--accent2)", failed: "var(--danger)" };
const when = ms => ms ? new Date(ms).toLocaleString([], { month: "short", day: "numeric", hour: "numeric", minute: "2-digit" }) : "—";
const opts = (list, cur) => list.map(o => { const v = Array.isArray(o) ? o[0] : o, l = Array.isArray(o) ? o[1] : o; return `<option value="${esc(v)}" ${String(v) === String(cur) ? "selected" : ""}>${esc(l)}</option>`; }).join("");
// Options lists may come as strings or objects: normalise to [value, label].
const norm = list => (Array.isArray(list) ? list : []).map(o => typeof o === "object" && o ? [o.id ?? o.value ?? o.path ?? o.name, o.title || o.name || o.label || o.id || o.path] : [o, o]);
const tokenQ = () => pp.S.token ? "?token=" + encodeURIComponent(pp.S.token) : "";
const mediaUrl = id => pp.downloadUrl ? pp.downloadUrl(id) : "/api/v1/download/" + encodeURIComponent(id) + tokenQ();
const clone = o => JSON.parse(JSON.stringify(o));
// Area market of a project (Martinsville / Reidsville / Danville): "" = the station default, "none" = generic. Localizes the voice (place-name respellings), the strap and the script writer.
const mktOpts = () => [["", "Station default" + (ST.markets.effectiveDefault ? " (" + ((ST.markets.markets || []).find(m => m.key === ST.markets.effectiveDefault) || {}).label + ")" : " (generic)")], ["none", "Generic (no market)"]].concat((ST.markets.markets || []).map(m => [m.key, m.label]));
const enc = encodeURIComponent;

async function loadStudio() {
  if (ST.loading) return; ST.loading = true;
  try {
    const [p, o, mk] = await Promise.all([pp.GET("/spotstudio/projects").catch(() => ({})), pp.GET("/spotstudio/options").catch(() => ({})), pp.GET("/spots/markets").catch(() => ({ markets: [] }))]);
    ST.projects = p.projects || (Array.isArray(p) ? p : []); ST.options = o || {}; ST.markets = mk || { markets: [] }; ST.loaded = true;
    if (ST.sel) { const cur = ST.projects.find(x => x.id === ST.sel); if (!cur) { ST.sel = ""; ST.p = null; ST.dirty = false; } else if (!ST.dirty) takeProject(cur); }
  } finally { ST.loading = false; }
  pp.render();
}
function takeProject(p) { ST.p = clone(p); ST.dirty = false; const i = ST.projects.findIndex(x => x.id === p.id); if (i >= 0) ST.projects[i] = p; else ST.projects.unshift(p); }
const loadProject = id => pp.GET("/spotstudio/projects/" + enc(id)).then(r => { const p = r.project || r; if (p && p.id) { if (ST.sel === p.id && ST.dirty) { const i = ST.projects.findIndex(x => x.id === p.id); if (i >= 0) ST.projects[i] = p; ST.p.status = p.status; ST.p.render = p.render; ST.p.review = p.review; ST.p.history = p.history; ST.p.published = p.published; ST.p.stale = p.stale; } else takeProject(p); } pp.render(); }).catch(() => {});
let reloadT = 0; const reloadSoon = () => { clearTimeout(reloadT); reloadT = setTimeout(loadStudio, 400); };
// While the open project renders, follow it (the WS "spotstudio" event may also push it).
setInterval(() => { if (pp.S && pp.S.view === "studio" && ST.p && ST.p.status === "rendering") loadProject(ST.p.id); }, 3000);

export function onEnterStudio(v) { if (v === "studio") loadStudio(); }
export function onWsStudio(m) {
  if (m.type !== "spotstudio") return false;
  const p = m.project || (m.id && m.status ? m : null);
  if (p && p.id) { if (ST.sel === p.id) { if (ST.dirty) { const i = ST.projects.findIndex(x => x.id === p.id); if (i >= 0) ST.projects[i] = p; Object.assign(ST.p, { status: p.status, render: p.render, review: p.review, history: p.history, published: p.published, stale: p.stale }); } else takeProject(p); } else { const i = ST.projects.findIndex(x => x.id === p.id); if (i >= 0) ST.projects[i] = p; else ST.projects.unshift(p); } }
  else if (Array.isArray(m.projects)) { ST.projects = m.projects; }
  else if (pp.S.view === "studio") reloadSoon();
  return true;
}

// ---------------------------------------------------------------------------------------------
export function viewStudio() {
  if (!ST.loaded) loadStudio();
  const o = ST.options || {}; const eng = o.engines || {};
  const engChip = (k, v) => bdg(k + (typeof v === "object" && v ? ((v.ready ?? v.ok) ? " ready" : " not ready") : v ? " ready" : " not ready"), (typeof v === "object" && v ? (v.ready ?? v.ok) : v) ? "var(--ready)" : "var(--danger)");
  const chips = Array.isArray(eng) ? eng.map(e => engChip(e.id || e.name, e)).join("") : Object.entries(eng).map(([k, v]) => engChip(k, v)).join("");
  const list = ST.projects.map(p => `<tr class="${p.id === ST.sel ? "selrow" : ""}" style="cursor:pointer" onclick="pp.studioOpen(${js(p.id)})"><td><b>${esc(p.title || "(untitled)")}</b><div style="color:var(--t3);font-size:10px">${esc(p.kind || "")} · ${p.lengthSec || "?"} s · ${esc(p.client || "no client")}</div></td>
    <td>${bdg(String(p.status || "draft").replace(/_/g, " "), STATUS[p.status] || "var(--t3)")}${(p.stale && (typeof p.stale === "object" ? p.stale.compose : p.stale)) ? " " + bdg("stale", "var(--warn)") : ""}</td><td class="mono" style="color:var(--t3);font-size:10px">${when(p.updatedMs || p.createdMs)}</td></tr>`).join("");
  return `<div class="grid" style="gap:12px">
    <div class="panel"><h3>Spot studio <span class="right">${chips}<button class="small" onclick="STS.newOpen=!STS.newOpen;pp.render()">+ new spot</button><button class="ghost small" onclick="pp.studioReload()">&#8635;</button></span></h3>${ST.newOpen ? viewNew() : ""}</div>
    <div class="grid" style="gap:12px;grid-template-columns:minmax(260px,1fr) 3fr;align-items:start">
      <div class="grid" style="gap:12px"><div class="panel"><h3>Projects (${ST.projects.length})</h3><div class="scroll" style="max-height:60vh"><table><tbody>${list || `<tr><td class="empty">No guided spots yet. Start one with a brief.</td></tr>`}</tbody></table></div></div>${viewRevise()}</div>
      <div>${ST.p ? viewProject() : `<div class="panel"><div class="empty">Pick a project, or start a new spot.</div></div>`}</div></div></div>`;
}

function viewNew() {
  const n = ST.nw; const o = ST.options || {}; const b = k => `STS.nw.${k}`;
  return `<div class="body grid cols4" style="gap:8px;border-bottom:1px solid var(--b0)">
    <label class="f">Title <input value="${esc(n.title)}" oninput="${b("title")}=this.value"></label>
    <label class="f">Kind <select onchange="${b("kind")}=this.value">${opts(["ad", "promo", "psa", "announcement"], n.kind)}</select></label>
    <label class="f">Client <input value="${esc(n.client)}" oninput="${b("client")}=this.value"></label>
    <label class="f">Length <select onchange="${b("lengthSec")}=parseInt(this.value)||30">${opts((o.lengths || [6, 15, 30, 60]).map(l => [l, l + " s"]), n.lengthSec)}</select></label>
    <label class="f" style="grid-column:1/-1">Brief <textarea rows="3" placeholder="What is it for, who should act, the offer (never invent prices or offers)" oninput="${b("brief")}=this.value">${esc(n.brief)}</textarea></label>
    <label class="f">Aspect <select onchange="${b("aspect")}=this.value">${opts(o.aspects || ["16:9", "9:16", "1:1"], n.aspect)}</select></label>
    <label class="f">Engine <select onchange="${b("engine")}=this.value">${opts([["local", "local (SpotGenerator)"], ["studio", "studio (spot-node)"]], n.engine)}</select></label>
    <label class="f">Template <select onchange="${b("template")}=this.value">${opts([["", "(none / motion spot)"]].concat(norm(o.templates)), n.template)}</select></label>
    <label class="f">Voice <select onchange="${b("voice")}=this.value">${opts([["", "default"]].concat(norm(o.voices)), n.voice)}</select></label>
    <label class="f">Music bed <select onchange="${b("music")}=this.value">${opts([["", "auto"]].concat(norm(o.music || o.musicBeds)), n.music)}</select></label>
    <label class="f">Tone <input value="${esc(n.tone)}" placeholder="warm, upbeat, urgent…" oninput="${b("tone")}=this.value"></label>
    <label class="f">Market <select onchange="${b("market")}=this.value">${opts(mktOpts(), n.market)}</select></label>
    <label class="f">Brand name <input value="${esc(n.brandName)}" oninput="${b("brandName")}=this.value"></label>
    <label class="f">Brand CTA <input value="${esc(n.brandCta)}" oninput="${b("brandCta")}=this.value"></label>
    <label class="f" style="grid-column:span 2">Brand URL <input value="${esc(n.brandUrl)}" oninput="${b("brandUrl")}=this.value"></label>
    <div class="row" style="grid-column:span 2;align-self:end"><span class="fill"></span><button class="success" ${ST.busy === "new" ? "disabled" : ""} onclick="pp.studioCreate()">${ST.busy === "new" ? "Writing…" : "START PROJECT"}</button><button class="ghost" onclick="STS.newOpen=false;pp.render()">cancel</button></div></div>`;
}

function viewProject() {
  const p = ST.p; const o = ST.options || {}; const b = k => `STS.p.${k}`; const d = ";STS.dirty=true";
  const sum = (p.scenes || []).reduce((a, s) => a + (+s.durationSec || 0), 0);
  const concepts = (p.concepts || []).map((c, i) => { const inUse = p.concept === i + 1 || p.concept === i || p.concept === c.title;
    return `<div class="panel" style="background:var(--bg3);${inUse ? "border-color:var(--accent)" : ""}"><div class="body" style="padding:8px 10px"><b>${esc(c.title)}</b>${inUse ? " " + bdg("in use", "var(--accent)") : ""}<div style="margin:4px 0">${esc(c.hook || "")}</div><div style="color:var(--t3);font-size:11px">${esc(c.angle || "")}</div>
      ${inUse ? "" : `<button class="ghost small" style="margin-top:6px" onclick="pp.studioConcept(${i + 1})">use this concept</button>`}</div></div>`; }).join("");
  const qs = (p.questions || []).map(q => { const v = ST.answers[q.id] ?? (p.answers && p.answers[q.id]) ?? ""; const bnd = `STS.answers[${js(q.id)}]`;
    return `<label class="f" style="grid-column:1/-1">${esc(q.q)} ${(q.options || []).length ? `<select onchange="${bnd}=this.value">${opts([["", "(choose)"]].concat(q.options.map(x => typeof x === "object" ? [x.value ?? x.id, x.label ?? x.value] : x)), v)}</select>` : `<input value="${esc(v)}" oninput="${bnd}=this.value">`}</label>`; }).join("");
  const scenes = (p.scenes || []).map((s, i) => { const sb = k => `STS.p.scenes[${i}].${k}`;
    return `<tr><td class="mono">${s.n ?? i + 1}</td><td><input value="${esc(s.beat)}" style="width:110px" oninput="${sb("beat")}=this.value${d}"></td>
      <td style="white-space:normal"><textarea rows="2" style="min-width:180px" oninput="${sb("visual")}=this.value${d}">${esc(s.visual)}</textarea></td><td style="white-space:normal"><textarea rows="2" style="min-width:220px" oninput="${sb("vo")}=this.value${d}">${esc(s.vo)}</textarea></td>
      <td><input value="${esc(s.onScreen)}" style="width:140px" oninput="${sb("onScreen")}=this.value${d}"></td><td><input type="number" step="0.1" min="0.8" value="${esc(s.durationSec)}" style="width:64px" oninput="${sb("durationSec")}=parseFloat(this.value)||0${d}"></td>
      <td><button class="ghost small" onclick="STS.p.scenes.splice(${i},1);STS.dirty=true;pp.render()">&#10005;</button></td></tr>`; }).join("");
  const ec = p.endCard || {}; const eb = k => `STS.p.endCard.${k}`;
  const r = p.render || {}; const rendering = p.status === "rendering" || r.state === "running" || r.state === "queued";
  const video = r.mediaId ? `<div class="row" style="align-items:flex-start"><div class="thumb" style="width:192px;height:108px" onclick="pp.studioPreview(${js(r.mediaId)})"><img src="/thumbs/${esc(r.mediaId)}.jpg" alt="" onerror="this.remove()"></div>
      <div class="grid" style="gap:6px"><button class="small" onclick="pp.studioPreview(${js(r.mediaId)})">&#9654; Play preview</button><div style="color:var(--t3);font-size:11px">media <a href="#" onclick="pp.studioOpenMedia(${js(r.mediaId)});return false">${esc(r.mediaId)}</a>${r.durationSec ? " · " + Number(r.durationSec).toFixed(1) + " s" : ""}${r.path ? "<br>" + esc(r.path) : ""}<br>archived until published</div></div></div>`
    : r.path ? `<div class="mono" style="font-size:11px">${esc(r.path)}</div>` : "";
  const hist = [].concat((p.review && p.review.history) || [], p.history || []).sort((a, c) => (c.ms || 0) - (a.ms || 0)).slice(0, 40)
    .map(h => `<tr><td class="mono" style="color:var(--t3)">${when(h.ms)}</td><td>${esc(h.actor || h.by || "")}</td><td>${esc(h.action || h.event || h.status || "")}</td><td style="white-space:normal">${esc(h.notes || h.note || "")}</td></tr>`).join("");
  const canPublish = p.status === "approved";
  return `<div class="grid" style="gap:12px">
  <div class="panel"><h3>${esc(p.title || "(untitled)")} ${bdg(String(p.status || "draft").replace(/_/g, " "), STATUS[p.status] || "var(--t3)")}${(p.stale && (typeof p.stale === "object" ? p.stale.compose : p.stale)) ? " " + bdg("compose stale", "var(--warn)") : ""}${ST.dirty ? " " + bdg("unsaved", "var(--warn)") : ""}
    <span class="right"><button class="success small" ${ST.dirty ? "" : "disabled"} onclick="pp.studioSave()">SAVE</button><button class="ghost small" ${ST.dirty ? "" : "disabled"} onclick="pp.studioRevert()">revert</button><button class="ghost small" onclick="pp.studioDelete()">&#10005; delete</button></span></h3>
    <div class="body grid cols4" style="gap:8px">
      <label class="f">Title <input value="${esc(p.title)}" oninput="${b("title")}=this.value${d}"></label><label class="f">Client <input value="${esc(p.client)}" oninput="${b("client")}=this.value${d}"></label>
      <label class="f">Length <select onchange="${b("lengthSec")}=parseInt(this.value)||30${d};pp.render()">${opts((o.lengths || [6, 15, 30, 60]).map(l => [l, l + " s"]), p.lengthSec)}</select></label>
      <label class="f">Engine <select onchange="${b("engine")}=this.value${d}">${opts(["local", "studio"], p.engine)}</select></label>
      <label class="f">Voice <select onchange="${b("voice")}=this.value${d}">${opts([["", "default"]].concat(norm(o.voices)), p.voice)}</select></label>
      <label class="f">Market <select onchange="${b("market")}=this.value${d}">${opts(mktOpts(), p.market || "")}</select></label>
      <label class="f">Music <select onchange="${b("music")}=this.value${d}">${opts([["", "auto"]].concat(norm(o.music || o.musicBeds)), p.music)}</select></label>
      <label class="f">Template <select onchange="${b("template")}=this.value${d}">${opts([["", "(none)"]].concat(norm(o.templates)), p.template)}</select></label>
      <label class="f">Tone <input value="${esc(p.tone)}" oninput="${b("tone")}=this.value${d}"></label>
      <label class="f" style="grid-column:1/-1">Brief <textarea rows="2" oninput="${b("brief")}=this.value${d}">${esc(p.brief)}</textarea></label></div></div>
  ${concepts ? `<div class="panel"><h3>Concepts</h3><div class="body grid cols3" style="gap:8px">${concepts}</div></div>` : ""}
  ${qs ? `<div class="panel"><h3>Questions from the writer</h3><div class="body grid" style="gap:8px">${qs}<div class="row"><button class="small" onclick="pp.studioAnswers()">Apply answers</button></div></div></div>` : ""}
  <div class="panel"><h3>Script <span class="right"><span class="mono" style="color:${Math.abs(sum - (p.lengthSec || 0)) > 0.05 ? "var(--warn)" : "var(--t3)"}">${sum.toFixed(1)} / ${p.lengthSec || "?"} s (rebalanced on save)</span></span></h3>
    <div class="scroll" style="max-height:50vh"><table><thead><tr><th>#</th><th>Beat</th><th>Visual</th><th>VO</th><th>On screen</th><th>Sec</th><th></th></tr></thead><tbody>${scenes || `<tr><td colspan="7" class="empty">No scenes yet.</td></tr>`}</tbody></table></div>
    <div class="row" style="padding:8px 12px;border-top:1px solid var(--b0)"><button class="ghost small" onclick="pp.studioAddScene()">+ scene</button></div>
    <div class="body grid cols4" style="gap:8px;border-top:1px solid var(--b0)"><div class="fh" style="grid-column:1/-1;margin:0">END CARD</div>
      <label class="f">Headline <input value="${esc(ec.headline)}" oninput="${eb("headline")}=this.value${d}"></label><label class="f">CTA <input value="${esc(ec.cta)}" oninput="${eb("cta")}=this.value${d}"></label>
      <label class="f">URL <input value="${esc(ec.url)}" oninput="${eb("url")}=this.value${d}"></label><label class="f">VO <input value="${esc(ec.vo)}" oninput="${eb("vo")}=this.value${d}"></label></div>
    <div class="body grid" style="gap:8px;border-top:1px solid var(--b0)"><label class="f">Instruction to the writer <textarea rows="2" placeholder="e.g. make the hook about the Saturday sale, shorter VO in scene 2" oninput="STS.instruction=this.value">${esc(ST.instruction)}</textarea></label>
      <div class="row"><button class="small" ${ST.busy === "instr" ? "disabled" : ""} onclick="pp.studioInstruct()">${ST.busy === "instr" ? "Rewriting…" : "Send instruction"}</button><span style="color:var(--t3);font-size:11px">With an LLM configured the script is rewritten; without one the instruction is kept as a review note.</span></div></div></div>
  <div class="panel"><h3>Generate <span class="right">${r.state ? bdg(r.state, r.state === "failed" ? "var(--danger)" : r.state === "done" ? "var(--ready)" : "var(--accent)") : ""}</span></h3><div class="body grid" style="gap:8px">
    <div class="row"><label class="f">Step <select onchange="STS.gen.step=this.value">${opts([["all", "all (script + compose)"], ["compose", "compose (render)"], ["script", "script only"]], ST.gen.step)}</select></label>
      <label class="f">Engine <select onchange="STS.gen.engine=this.value">${opts([["", "project (" + (p.engine || "local") + ")"], "local", "studio"], ST.gen.engine)}</select></label>
      <button class="primary" style="align-self:end" ${rendering || ST.busy === "gen" ? "disabled" : ""} onclick="pp.studioGenerate()">${rendering ? "Rendering…" : "Generate"}</button>${ST.dirty ? `<span style="color:var(--warn);font-size:11px;align-self:end">unsaved edits are saved first</span>` : ""}</div>
    ${rendering || r.pct ? `<div class="row"><div class="meter" style="width:260px"><div class="bar" style="width:${Math.max(0, Math.min(100, r.pct || 0))}%;background:var(--accent)"></div></div><span class="mono" style="font-size:11px;color:var(--t2)">${Math.round(r.pct || 0)}% · ${esc(r.engine || p.engine || "")}${r.jobId ? " · job " + esc(r.jobId) : r.genId ? " · gen " + esc(r.genId) : ""}</span></div>` : ""}
    ${r.error ? `<div class="alert error">${esc(r.error)}</div>` : ""}${video}</div></div>
  <div class="grid cols2">
  <div class="panel"><h3>Review</h3><div class="body grid" style="gap:8px">
    <label class="f">Notes <textarea rows="3" oninput="STS.reviewNotes=this.value">${esc(ST.reviewNotes)}</textarea></label>
    <div class="row"><button class="small" onclick="pp.studioReview('submit')">Submit for review</button><button class="success small" onclick="pp.studioReview('approve')">Approve</button><button class="warn small" onclick="pp.studioReview('changes')">Request changes</button><button class="danger small" onclick="pp.studioReview('reject')">Reject</button></div>
    ${p.review && p.review.notes ? `<div class="alert">${esc(p.review.notes)}</div>` : ""}</div></div>
  ${viewPublish(p, canPublish)}</div>
  <div class="panel"><h3>History</h3><div class="scroll" style="max-height:30vh"><table><tbody>${hist || `<tr><td class="empty">No history yet.</td></tr>`}</tbody></table></div></div></div>`;
}

function viewPublish(p, can) {
  const u = ST.pub; const b = k => `STS.pub.${k}`;
  const chans = (pp.S.channels || []).map(c => [c.profile.id, c.profile.name || c.profile.id]);
  if (p.published && p.published.mediaId) return `<div class="panel"><h3>Published</h3><div class="body">${bdg("published", "var(--accent2)")} media ${esc(p.published.mediaId)}${p.published.contractId ? " · contract " + esc(p.published.contractId) : ""} · ${when(p.published.ms)}</div></div>`;
  return `<div class="panel"><h3>Publish to rotation ${can ? "" : `<span class="right">${bdg("approve first", "var(--t3)")}</span>`}</h3><div class="body grid cols2" style="gap:8px">
    <label class="f">Type <select onchange="${b("type")}=this.value">${opts(["ad", "promo", "psa"], u.type)}</select></label><label class="f">Client <input value="${esc(u.client)}" placeholder="${esc(p.client || "")}" oninput="${b("client")}=this.value"></label>
    <label class="f" style="grid-column:1/-1">Title <input value="${esc(u.title)}" placeholder="${esc(p.title || "")}" oninput="${b("title")}=this.value"></label>
    <label class="f" style="grid-column:1/-1">Channels <span class="row">${chans.map(([id, l]) => `<label style="font-size:12px"><input type="checkbox" ${u.channels.includes(id) ? "checked" : ""} onchange="const s=new Set(STS.pub.channels);this.checked?s.add(${js(id)}):s.delete(${js(id)});STS.pub.channels=[...s]"> ${esc(l)}</label>`).join("") || "<span style='color:var(--t3)'>no channels</span>"}</span></label>
    <label class="f">Start <input type="date" value="${esc(u.startDate)}" oninput="${b("startDate")}=this.value"></label><label class="f">End <input type="date" value="${esc(u.endDate)}" oninput="${b("endDate")}=this.value"></label>
    <label class="f">Contract <select onchange="${b("contractMode")}=this.value;pp.render()">${opts([["new", "new contract"], ["existing", "existing contract id"]], u.contractMode)}</select></label>
    ${u.contractMode === "existing" ? `<label class="f">Contract id <input value="${esc(u.contractId)}" oninput="${b("contractId")}=this.value"></label>`
      : `<label class="f">Priority <input type="number" min="1" max="10" value="${esc(u.priority)}" oninput="${b("priority")}=parseInt(this.value)||5"></label><label class="f">Spots / day <input type="number" min="0" value="${esc(u.spotsPerDay)}" oninput="${b("spotsPerDay")}=parseInt(this.value)||0"></label><label class="f">Spots total (0 = no cap) <input type="number" min="0" value="${esc(u.spotsTotal)}" oninput="${b("spotsTotal")}=parseInt(this.value)||0"></label>`}
    <label class="f" style="grid-column:1/-1">Social <label style="text-transform:none"><input type="checkbox" ${u.socialQueue ? "checked" : ""} onchange="${b("socialQueue")}=this.checked;pp.render()"> also queue a Publisher draft</label></label>
    ${u.socialQueue ? `<label class="f" style="grid-column:1/-1">Social message <textarea rows="2" oninput="${b("socialMessage")}=this.value">${esc(u.socialMessage)}</textarea></label>` : ""}
    <div class="row" style="grid-column:1/-1"><button class="primary" ${can && ST.busy !== "pub" ? "" : "disabled"} onclick="pp.studioPublish()">${ST.busy === "pub" ? "Publishing…" : "Publish"}</button></div></div></div>`;
}

function viewRevise() {
  const r = ST.revise; const b = k => `STS.revise.${k}`;
  return `<div class="panel"><h3>Revise an auto spot</h3><div class="body grid" style="gap:8px">
    <label class="f">Media id <input value="${esc(r.mediaId)}" placeholder="library id of a generated spot" oninput="${b("mediaId")}=this.value"></label>
    <label class="f">Editor's note <textarea rows="3" placeholder="what to change" oninput="${b("notes")}=this.value">${esc(r.notes)}</textarea></label>
    <label class="f">Voice (optional) <input value="${esc(r.voice)}" oninput="${b("voice")}=this.value"></label>
    <div class="row"><button class="small" ${ST.busy === "rev" ? "disabled" : ""} onclick="pp.studioRevise()">${ST.busy === "rev" ? "Regenerating…" : "Regenerate with note"}</button></div></div></div>`;
}

// ---------------------------------------------------------------------------------------------
function editBody(p) { return { title: p.title, client: p.client, brief: p.brief, lengthSec: p.lengthSec, engine: p.engine, voice: p.voice, music: p.music, template: p.template, tone: p.tone, market: p.market || "", scenes: p.scenes, endCard: p.endCard }; }
const putProject = body => pp.PUT("/spotstudio/projects/" + enc(ST.p.id), body).then(r => { const p = (r && (r.project || r)) || null; if (p && p.id) takeProject(p); else ST.dirty = false; pp.render(); return p; });
export const ppExtStudio = {
  studioReload() { loadStudio(); },
  // Playback lives in a dialog on <body>: #view is rebuilt by every telemetry repaint, which would restart an inline <video>.
  studioPreview(id) {
    let d = document.getElementById("stPreviewDlg");
    if (!d) { d = document.createElement("dialog"); d.id = "stPreviewDlg"; d.style.width = "min(960px,92vw)";
      d.innerHTML = `<div class="row" style="margin-bottom:8px"><b id="stPreviewTitle"></b><span class="fill"></span><button class="ghost small" id="stPreviewClose">&#10005; close</button></div><video id="stPreviewVideo" controls preload="metadata" style="width:100%;max-height:70vh;background:#000;border-radius:6px"></video>`;
      document.body.appendChild(d); d.querySelector("#stPreviewClose").onclick = () => d.close(); d.addEventListener("close", () => { const v = d.querySelector("video"); v.pause(); v.removeAttribute("src"); v.load(); }); }
    const v = d.querySelector("video"); v.poster = "/thumbs/" + encodeURIComponent(id) + ".jpg"; v.src = mediaUrl(id); d.querySelector("#stPreviewTitle").textContent = (ST.p && ST.p.title ? ST.p.title + " · " : "") + id;
    d.showModal(); v.play().catch(() => {});
  },
  studioOpenMedia(id) { pp.nav("library"); setTimeout(() => { if (pp.libOpen) pp.libOpen(id); }, 350); },
  studioOpen(id) { if (id === ST.sel) return; const go = () => { ST.sel = id; ST.answers = {}; ST.instruction = ""; ST.reviewNotes = ""; const p = ST.projects.find(x => x.id === id); if (p) takeProject(p);
      ST.pub = Object.assign(ST.pub, { client: "", title: "", type: p && ["ad", "promo", "psa"].includes(p.kind) ? p.kind : "ad", socialMessage: "" }); pp.render(); loadProject(id); };
    if (ST.dirty) pp.confirmDo("Discard the unsaved edits in \"" + (ST.p.title || "this project") + "\"?", go); else go(); },
  studioCreate() {
    const n = ST.nw; if (!n.brief.trim()) { pp.toast("A brief is required", "err"); return; }
    const body = { title: n.title.trim() || n.brief.trim().slice(0, 40), kind: n.kind, client: n.client, brief: n.brief, lengthSec: n.lengthSec, aspect: n.aspect, template: n.template, engine: n.engine, voice: n.voice, music: n.music, tone: n.tone, market: n.market,
      brand: { name: n.brandName, cta: n.brandCta, url: n.brandUrl } };
    ST.busy = "new"; pp.render();
    pp.POST("/spotstudio/projects", body).then(r => { const p = r.project || r; ST.nw = blankNew(); ST.newOpen = false; if (p && p.id) { ST.sel = p.id; takeProject(p); } pp.toast("Project started", "ok"); })
      .catch(() => {}).finally(() => { ST.busy = ""; pp.render(); });
  },
  studioSave() { if (!ST.p) return; putProject(editBody(ST.p)).then(() => pp.toast("Saved; timing rebalanced", "ok")).catch(() => {}); },
  studioRevert() { const p = ST.projects.find(x => x.id === ST.sel); if (p) takeProject(p); pp.render(); },
  studioDelete() { const p = ST.p; if (!p) return; pp.confirmDo("Delete spot project \"" + (p.title || p.id) + "\"? Rendered media stays in the library.", () => pp.api("DELETE", "/spotstudio/projects/" + enc(p.id)).then(() => { ST.projects = ST.projects.filter(x => x.id !== p.id); ST.sel = ""; ST.p = null; ST.dirty = false; pp.render(); }).catch(() => {})); },
  studioConcept(n) { const go = () => putProject({ concept: n }).then(() => pp.toast("Concept " + n + " in use", "ok")).catch(() => {}); if (ST.dirty) pp.confirmDo("Switching concept rewrites the script; unsaved edits are lost. Continue?", go); else go(); },
  studioAnswers() { const a = Object.fromEntries(Object.entries(ST.answers).filter(([, v]) => v !== "")); if (!Object.keys(a).length) { pp.toast("No answers yet", "err"); return; } putProject({ answers: a }).then(() => { ST.answers = {}; pp.toast("Answers applied", "ok"); }).catch(() => {}); },
  studioAddScene() { const p = ST.p; p.scenes = p.scenes || []; p.scenes.push({ n: p.scenes.length + 1, beat: "", visual: "", vo: "", onScreen: "", durationSec: 3 }); ST.dirty = true; pp.render(); },
  studioInstruct() { const t = ST.instruction.trim(); if (!t) { pp.toast("Type an instruction first", "err"); return; } ST.busy = "instr"; pp.render();
    putProject(ST.dirty ? Object.assign(editBody(ST.p), { instruction: t }) : { instruction: t }).then(() => { ST.instruction = ""; pp.toast("Instruction sent", "ok"); }).catch(() => {}).finally(() => { ST.busy = ""; pp.render(); }); },
  studioGenerate() { const p = ST.p; if (!p) return; ST.busy = "gen"; pp.render();
    const body = { step: ST.gen.step }; if (ST.gen.engine) body.engine = ST.gen.engine;
    (ST.dirty ? putProject(editBody(p)) : Promise.resolve()).then(() => pp.POST("/spotstudio/projects/" + enc(p.id) + "/generate", body)).then(r => { const np = r && (r.project || (r.id && r.status ? r : null)); if (np) takeProject(np); pp.toast("Generating (" + body.step + ")", "ok"); loadProject(p.id); })
      .catch(() => {}).finally(() => { ST.busy = ""; pp.render(); }); },
  studioReview(action) { const p = ST.p; if (!p) return; const go = () => pp.POST("/spotstudio/projects/" + enc(p.id) + "/review", { action, notes: ST.reviewNotes }).then(r => { const np = r && (r.project || (r.id && r.status ? r : null)); if (np) takeProject(np); else loadProject(p.id); ST.reviewNotes = ""; pp.toast("Review: " + action, "ok"); pp.render(); }).catch(() => {});
    if (action === "reject") pp.confirmDo("Reject \"" + (p.title || p.id) + "\"? It will not air.", go); else go(); },
  studioPublish() { const p = ST.p, u = ST.pub; if (!p) return; if (!u.channels.length) { pp.toast("Pick at least one channel", "err"); return; } if (u.endDate && u.startDate && u.endDate < u.startDate) { pp.toast("End date is before the start date", "err"); return; }
    const body = { type: u.type, client: u.client.trim() || p.client || "", title: u.title.trim() || p.title || "", channels: u.channels, startDate: u.startDate, endDate: u.endDate };
    if (u.contractMode === "existing") { if (!u.contractId.trim()) { pp.toast("Contract id is required", "err"); return; } body.contractId = u.contractId.trim(); } else body.contract = { spotsPerDay: u.spotsPerDay, spotsTotal: u.spotsTotal, priority: u.priority };
    if (u.socialQueue) body.social = { queue: true, message: u.socialMessage };
    pp.confirmDo("Publish \"" + body.title + "\" to " + u.channels.join(", ") + " from " + u.startDate + " to " + u.endDate + "? The media is un-archived and enters ad rotation.", () => { ST.busy = "pub"; pp.render();
      pp.POST("/spotstudio/projects/" + enc(p.id) + "/publish", body).then(r => { const np = r && (r.project || (r.id && r.status ? r : null)); if (np) takeProject(np); else loadProject(p.id); pp.toast("Published", "ok"); }).catch(() => {}).finally(() => { ST.busy = ""; pp.render(); }); }); },
  studioRevise() { const r = ST.revise; const id = r.mediaId.trim(); if (!id || !r.notes.trim()) { pp.toast("Media id and a note are required", "err"); return; } ST.busy = "rev"; pp.render();
    const body = { notes: r.notes }; if (r.voice.trim()) body.voice = r.voice.trim();
    pp.POST("/spots/spot/" + enc(id) + "/revise", body).then(res => { pp.toast(res && res.ok === false ? "Revise failed: " + (res.error || "?") : "Revision queued" + (res && (res.mediaId || res.id) ? " → " + (res.mediaId || res.id) : ""), res && res.ok === false ? "err" : "ok"); if (!(res && res.ok === false)) ST.revise = { mediaId: "", notes: "", voice: "" }; })
      .catch(() => {}).finally(() => { ST.busy = ""; pp.render(); }); },
};
