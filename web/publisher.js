/* PLAY PLAY web dashboard — Publish page (docs/publisher.md): the publishing lane (Facebook page + WordPress site).
   Health header with the kill switch, the queue with filters, item detail and actions, a new-draft form, config
   (secrets write-only) and the audit. All state lives in PB (window.PBS); every input writes into PB as it is typed. */

const blankDraft = () => ({ kind: "text", title: "", body: "", caption: "", linkUrl: "", mediaId: "", channels: ["facebook"], sourceLinks: "", tags: "", aiText: false, aiImages: false, aiVideo: false, aiVoice: false, scheduleAt: "" });
const PB = {
  loaded: false, loading: false, status: {}, items: [], filter: { status: "open", kind: "", source: "", channel: "" }, sel: "", detail: null,
  act: { note: "", scheduleAt: "", title: "", body: "", caption: "" }, draftOpen: false, draft: blankDraft(), stop: { reason: "", scope: "all" },
  cfg: null, cfgDirty: false, secrets: { fbToken: "", wpKey: "" }, verify: null, audit: [], tab: "queue", busy: "",
};
window.PBS = PB;
const esc = s => String(s ?? "").replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
const js = v => esc(JSON.stringify(String(v ?? "")));
const bdg = (text, color) => `<span class="badge" style="color:${color};border-color:${color};background:color-mix(in srgb,${color} 16%,transparent)">${esc(text)}</span>`;
const STATUS = { draft: "var(--t2)", pending_approval: "var(--warn)", held: "var(--standby)", approved: "var(--ready)", scheduled: "var(--cued)", publishing: "var(--accent)", published: "var(--accent2)", failed: "var(--danger)", skipped: "var(--t3)", retracted: "var(--danger)", corrected: "var(--t3)" };
const when = ms => ms ? new Date(ms).toLocaleString([], { month: "short", day: "numeric", hour: "numeric", minute: "2-digit" }) : "—";
const opts = (list, cur) => list.map(o => { const v = Array.isArray(o) ? o[0] : o, l = Array.isArray(o) ? o[1] : o; return `<option value="${esc(v)}" ${String(v) === String(cur) ? "selected" : ""}>${esc(l)}</option>`; }).join("");
const pretty = v => typeof v === "string" ? v : JSON.stringify(v, null, 2);
const localDt = ms => { const d = new Date(ms); const p = n => String(n).padStart(2, "0"); return d.getFullYear() + "-" + p(d.getMonth() + 1) + "-" + p(d.getDate()) + "T" + p(d.getHours()) + ":" + p(d.getMinutes()); };
const safeUrl = u => /^https?:\/\//i.test(String(u || "").trim()) ? String(u).trim() : "#";
const enc = encodeURIComponent;
const KINDS = ["article", "spot", "video", "slate", "text", "link"], SOURCES = ["manual", "spot_job", "spot_studio", "agent_draft"];

function queuePath() { const f = PB.filter; const q = Object.entries(f).filter(([, v]) => v).map(([k, v]) => k + "=" + enc(v)).join("&"); return "/publish/queue" + (q ? "?" + q : ""); }
async function loadPublish() {
  if (PB.loading) return; PB.loading = true;
  try {
    const [st, q, c, a] = await Promise.all([pp.GET("/publish/status").catch(() => ({})), pp.GET(queuePath()).catch(() => ({})), pp.GET("/publish/config").catch(() => null), pp.GET("/publish/audit").catch(() => ({}))]);
    PB.status = st || {}; PB.items = q.items || []; if (c && !PB.cfgDirty) PB.cfg = JSON.parse(JSON.stringify(c.config || c)); PB.audit = a.audit || a.items || []; PB.loaded = true;
  } finally { PB.loading = false; }
  pp.render(); if (PB.sel) loadDetail(PB.sel);
}
const loadStatus = () => pp.GET("/publish/status").then(s => { PB.status = s || {}; pp.render(); }).catch(() => {});
const loadQueue = () => pp.GET(queuePath()).then(q => { PB.items = q.items || []; pp.render(); }).catch(() => {});
const loadAudit = () => pp.GET("/publish/audit").then(a => { PB.audit = a.audit || a.items || []; pp.render(); }).catch(() => {});
const loadDetail = id => pp.GET("/publish/items/" + enc(id)).then(r => { const it = r.item || r; if (PB.sel === id) { PB.detail = it; pp.render(); } }).catch(() => {});
let reloadT = 0; const reloadSoon = () => { clearTimeout(reloadT); reloadT = setTimeout(() => { loadStatus(); loadQueue(); if (PB.sel) loadDetail(PB.sel); }, 400); };

export function onEnterPublish(v) { if (v === "publish") loadPublish(); }
export function onWsPublish(m) {
  if (m.type !== "publish") return false;
  if (m.item && m.item.id) { const i = PB.items.findIndex(x => x.id === m.item.id); if (i >= 0) PB.items[i] = m.item; if (PB.sel === m.item.id) PB.detail = m.item; }
  if (m.killed || m.counts) PB.status = Object.assign({}, PB.status, m);
  if (pp.S.view === "publish") reloadSoon();
  return true;
}

// ---------------------------------------------------------------------------------------------
export function viewPublish() {
  if (!PB.loaded) loadPublish();
  const st = PB.status || {}, k = st.killed || {}, cf = st.configured || {}, c = st.counts || {};
  const killed = k.all || k.facebook || k.site;
  const nx = st.next; const nextTxt = !nx ? "nothing scheduled" : typeof nx === "number" ? "next " + when(nx) : "next " + when(nx.scheduleAtMs || nx.atMs) + " · " + (nx.title || nx.id || "");
  const warns = (st.warnings || []).map(w => `<div class="alert warning">${esc(typeof w === "string" ? w : w.message || JSON.stringify(w))}</div>`).join("");
  const head = `<div class="panel" style="${killed ? "border-color:var(--danger)" : ""}"><h3>Publisher <span class="right">${bdg("mode " + (st.mode || (st.config && st.config.mode) || "?"), st.mode === "auto" ? "var(--warn)" : st.mode === "review" ? "var(--accent)" : "var(--t3)")}
      ${bdg("facebook", cf.facebook ? "var(--ready)" : "var(--t3)")}${bdg("website", cf.website ? "var(--ready)" : "var(--t3)")}
      ${k.all ? bdg("STOPPED: all", "var(--danger)") : ""}${k.facebook ? bdg("STOPPED: facebook", "var(--danger)") : ""}${k.site ? bdg("STOPPED: site", "var(--danger)") : ""}
      ${Object.entries(c).map(([s, n]) => n ? bdg(s.replace(/_/g, " ") + " " + n, STATUS[s] || "var(--t2)") : "").join("")}<span style="color:var(--t3);font-size:11px">${esc(nextTxt)}</span><button class="ghost small" onclick="pp.pubReload()">&#8635;</button></span></h3>
    <div class="body row" style="padding:8px 12px"><button class="danger" style="font-size:14px;padding:8px 18px" onclick="pp.pubStop()">&#9632; STOP PUBLISHING</button>
      <label class="f">Scope <select onchange="PBS.stop.scope=this.value">${opts([["all", "all"], ["facebook", "facebook"], ["site", "website"]], PB.stop.scope)}</select></label>
      <label class="f" style="flex:1;min-width:200px">Reason <input value="${esc(PB.stop.reason)}" placeholder="why sending stops" oninput="PBS.stop.reason=this.value"></label>
      ${killed ? `<button class="warn" onclick="pp.pubRelease()">Release ${esc(PB.stop.scope)}</button>` : ""}<span style="color:var(--t3);font-size:11px;max-width:320px">STOP halts every send at once and lowers the mode to off; releasing is a separate, explicit step.</span></div>
    ${warns ? `<div class="body" style="padding:4px 12px 8px">${warns}</div>` : ""}
    <div class="row" style="padding:6px 12px;border-top:1px solid var(--b0)">${["queue", "config", "audit"].map(t => `<button class="ghost small${PB.tab === t ? " active" : ""}" onclick="PBS.tab='${t}';pp.render()">${t}</button>`).join("")}<span class="fill"></span><button class="small" onclick="PBS.draftOpen=!PBS.draftOpen;PBS.tab='queue';pp.render()">+ new draft</button></div></div>`;
  const body = PB.tab === "config" ? viewConfig() : PB.tab === "audit" ? viewAudit() : `${PB.draftOpen ? viewDraft() : ""}<div class="grid" style="gap:12px;grid-template-columns:3fr 2fr;align-items:start">${viewQueue()}${viewDetail()}</div>`;
  return `<div class="grid" style="gap:12px">${head}${body}</div>`;
}

function viewQueue() {
  const f = PB.filter; const fs = (k, list) => `<select class="small" onchange="PBS.filter.${k}=this.value;pp.pubQueue()">${opts(list, f[k])}</select>`;
  const rows = PB.items.map(it => { const pc = it.perChannel || {};
    return `<tr class="${it.id === PB.sel ? "selrow" : ""}" style="cursor:pointer" onclick="pp.pubSelect(${js(it.id)})"><td>${bdg(String(it.status || "?").replace(/_/g, " "), STATUS[it.status] || "var(--t3)")}${it.tier === "absolute" ? " " + bdg("absolute", "var(--danger)") : ""}</td>
      <td><b>${esc(it.title || "(untitled)")}</b><div style="color:var(--t3);font-size:10px">${esc(it.kind || "")} · ${esc(it.source || "")}${it.revision > 1 ? " · rev " + it.revision : ""}${it.corrects ? " · corrects " + esc(it.corrects) : ""}</div></td>
      <td>${(it.channels || []).map(ch => { const s = (pc[ch] || {}).state; return bdg(ch + (s ? " " + s : ""), s === "posted" || s === "published" ? "var(--ready)" : s === "failed" ? "var(--danger)" : "var(--t2)"); }).join(" ")}</td>
      <td class="mono" style="color:var(--t3)">${it.scheduleAtMs ? when(it.scheduleAtMs) : when(it.updatedMs || it.createdMs)}</td></tr>`; }).join("");
  return `<div class="panel"><h3>Queue (${PB.items.length}) <span class="right">${fs("status", [["open", "open"], ["waiting", "waiting"], ["needs_attention", "needs attention"], ["done", "done"], ["all", "all"]])}${fs("kind", [["", "any kind"]].concat(KINDS))}${fs("source", [["", "any source"]].concat(SOURCES))}${fs("channel", [["", "any channel"], "facebook", "website"])}</span></h3>
    <div class="scroll" style="max-height:60vh"><table><thead><tr><th>Status</th><th>Item</th><th>Channels</th><th>When</th></tr></thead><tbody>${rows || `<tr><td colspan="4" class="empty">Nothing in this view.</td></tr>`}</tbody></table></div></div>`;
}

function viewDetail() {
  const it = PB.detail; if (!it) return `<div class="panel"><div class="empty">Pick an item to see its copy, channels, history and actions.</div></div>`;
  const a = PB.act; const pc = it.perChannel || {}; const ai = it.ai || {};
  const unpublished = !["published", "publishing", "retracted", "corrected"].includes(it.status);
  const btn = (act, label, cls = "ghost") => `<button class="${cls} small" ${PB.busy ? "disabled" : ""} onclick="pp.pubAct(${js(it.id)},'${act}')">${label}</button>`;
  const chRows = Object.entries(pc).map(([ch, s]) => `<tr><td>${esc(ch)}</td><td>${bdg(s.state || "?", s.state === "failed" ? "var(--danger)" : "var(--t2)")}</td><td>${s.permalink ? `<a href="${esc(safeUrl(s.permalink))}" target="_blank" rel="noopener">open</a>` : esc(s.remoteId || "")}</td><td class="mono" style="color:var(--t3)">${s.attempts ? s.attempts + "×" : ""} ${when(s.postedMs)}</td><td style="color:var(--danger);font-size:10px;white-space:normal">${esc(s.error || "")}</td></tr>`).join("");
  const hist = (it.history || []).slice().reverse().map(h => `<tr><td class="mono" style="color:var(--t3)">${when(h.ms)}</td><td>${esc(h.actor || "")}</td><td>${esc(h.action || "")}${h.revision ? " · r" + h.revision : ""}</td><td style="white-space:normal">${esc(h.note || "")}</td></tr>`).join("");
  return `<div class="panel"><h3>${esc(it.title || "(untitled)")} <span class="right">${bdg(String(it.status || "?").replace(/_/g, " "), STATUS[it.status] || "var(--t3)")}<button class="ghost small" onclick="PBS.sel='';PBS.detail=null;pp.render()">&#10005;</button></span></h3>
    <div class="body grid" style="gap:8px">
      ${it.why ? `<div class="alert ${it.tier === "absolute" ? "error" : ""}">${esc(it.why)}</div>` : ""}
      <div style="white-space:pre-wrap;max-height:22vh;overflow:auto;background:var(--bg1);padding:8px;border-radius:5px">${esc(it.body || it.caption || "")}</div>
      ${it.caption && it.body ? `<div style="color:var(--t2);font-size:11px">caption: ${esc(it.caption)}</div>` : ""}
      <div style="color:var(--t3);font-size:11px">${esc(it.kind)} · ${esc(it.source)}${it.mediaId ? " · media " + esc(it.mediaId) : ""}${it.linkUrl ? ` · <a href="${esc(safeUrl(it.linkUrl))}" target="_blank" rel="noopener">${esc(it.linkUrl)}</a>` : ""} · tier ${esc(it.tier || "default")}
        ${Object.entries(ai).filter(([, v]) => v === true).map(([k2]) => " · AI " + esc(k2)).join("")}${ai.model ? " · " + esc(ai.model) : ""}</div>
      ${(it.sourceLinks || []).length ? `<div style="font-size:11px">sources: ${it.sourceLinks.map(u => `<a href="${esc(safeUrl(u))}" target="_blank" rel="noopener">${esc(u)}</a>`).join(" · ")}</div>` : ""}
      ${(it.tags || []).length ? `<div>${it.tags.map(t => bdg(t, "var(--t2)")).join(" ")}</div>` : ""}
      ${chRows ? `<table><tbody>${chRows}</tbody></table>` : ""}
      ${it.metrics ? `<pre class="mono" style="margin:0;font-size:10px;max-height:14vh;overflow:auto">${esc(pretty(it.metrics))}</pre>` : ""}
      <label class="f">Note (recorded with the action) <input value="${esc(a.note)}" oninput="PBS.act.note=this.value"></label>
      <div class="row">${btn("approve", "Approve", "success")}${btn("hold", "Hold")}${btn("skip", "Skip")}${btn("send", "Send now", "primary")}${btn("retry", "Retry")}${btn("metrics", "Metrics")}</div>
      <div class="row"><input type="datetime-local" value="${esc(a.scheduleAt)}" oninput="PBS.act.scheduleAt=this.value">${btn("schedule", "Schedule")}</div>
      <details><summary style="cursor:pointer;color:var(--t2);font-size:11px">Correct (a new revision; the original stays on record)</summary><div class="grid" style="gap:6px;margin-top:6px">
        <label class="f">Title <input value="${esc(a.title)}" oninput="PBS.act.title=this.value"></label><label class="f">Body <textarea rows="4" oninput="PBS.act.body=this.value">${esc(a.body)}</textarea></label><label class="f">Caption <input value="${esc(a.caption)}" oninput="PBS.act.caption=this.value"></label>
        <div class="row">${btn("correct", "Publish correction", "warn")}</div></div></details>
      <div class="row">${btn("retract", "Retract", "danger")}<span class="fill"></span>${unpublished ? `<button class="ghost small" onclick="pp.pubDelete(${js(it.id)})">&#10005; delete</button>` : ""}</div>
      ${hist ? `<div class="fh">HISTORY</div><div class="scroll" style="max-height:24vh"><table><tbody>${hist}</tbody></table></div>` : ""}</div></div>`;
}

function viewDraft() {
  const d = PB.draft; const b = k => `PBS.draft.${k}`;
  return `<div class="panel"><h3>New draft</h3><div class="body grid cols4" style="gap:8px">
    <label class="f">Kind <select onchange="${b("kind")}=this.value;pp.render()">${opts(KINDS, d.kind)}</select></label>
    <label class="f" style="grid-column:span 3">Title <input value="${esc(d.title)}" oninput="${b("title")}=this.value"></label>
    <label class="f" style="grid-column:1/-1">Body <textarea rows="4" oninput="${b("body")}=this.value">${esc(d.body)}</textarea></label>
    <label class="f" style="grid-column:span 2">Caption <input value="${esc(d.caption)}" oninput="${b("caption")}=this.value"></label>
    <label class="f">Link URL <input value="${esc(d.linkUrl)}" placeholder="https://…" oninput="${b("linkUrl")}=this.value"></label>
    <label class="f">Media id ${d.kind === "video" || d.kind === "spot" ? "(must be approved)" : ""} <input value="${esc(d.mediaId)}" oninput="${b("mediaId")}=this.value"></label>
    <label class="f">Channels <span class="row">${["facebook", "website"].map(ch => `<label style="font-size:12px"><input type="checkbox" ${d.channels.includes(ch) ? "checked" : ""} onchange="const s=new Set(PBS.draft.channels);this.checked?s.add('${ch}'):s.delete('${ch}');PBS.draft.channels=[...s]"> ${ch}</label>`).join("")}</span></label>
    <label class="f">Tags (comma) <input value="${esc(d.tags)}" oninput="${b("tags")}=this.value"></label>
    <label class="f" style="grid-column:span 2">Source links (one per line; https) <textarea rows="2" oninput="${b("sourceLinks")}=this.value">${esc(d.sourceLinks)}</textarea></label>
    <label class="f" style="grid-column:span 2">AI made <span class="row">${[["aiText", "text"], ["aiImages", "images"], ["aiVideo", "video"], ["aiVoice", "voice"]].map(([k, l]) => `<label style="font-size:12px"><input type="checkbox" ${d[k] ? "checked" : ""} onchange="${b(k)}=this.checked"> ${l}</label>`).join("")}</span></label>
    <label class="f">Schedule (optional) <input type="datetime-local" value="${esc(d.scheduleAt)}" oninput="${b("scheduleAt")}=this.value"></label>
    <div class="row" style="align-self:end"><button class="success" ${PB.busy === "draft" ? "disabled" : ""} onclick="pp.pubDraft()">SAVE DRAFT</button><button class="ghost" onclick="PBS.draftOpen=false;pp.render()">cancel</button></div>
    <div style="grid-column:1/-1;color:var(--t3);font-size:11px">Obituaries, crime, injury or death, minors and allegations are always held for a person. The AI label is appended to every outgoing copy AI helped make.</div></div></div>`;
}

function viewConfig() {
  const c = PB.cfg; if (!c) return `<div class="panel"><div class="empty">loading…</div></div>`;
  const fb = c.facebook || {}, wb = c.website || {}; const d = ";PBS.cfgDirty=true";
  const hasTok = fb.hasToken ?? c.hasToken, hasKey = wb.hasKey ?? c.hasKey;
  const v = PB.verify;
  return `<div class="panel"><h3>Publisher config ${PB.cfgDirty ? bdg("unsaved", "var(--warn)") : ""}</h3><div class="body grid cols4" style="gap:8px">
    <label class="f">Mode <select onchange="PBS.cfg.mode=this.value${d};pp.render()">${opts([["off", "off (nothing queued automatically)"], ["review", "review (spots queued for approval)"], ["auto", "auto (approved unless absolute tier)"]], c.mode)}</select></label>
    <label class="f">Recipes (comma) <input value="${esc((c.recipes || []).join(", "))}" placeholder="spot recipes queued from" oninput="PBS.cfg.recipes=this.value.split(',').map(s=>s.trim()).filter(Boolean)${d}"></label>
    <label class="f">Max per day <input type="number" min="0" value="${esc(c.maxPerDay ?? "")}" oninput="PBS.cfg.maxPerDay=parseInt(this.value)||0${d}"></label>
    <label class="f">AI label <input value="${esc(c.aiLabel ?? "")}" placeholder="Made with AI assistance." oninput="PBS.cfg.aiLabel=this.value${d}"></label>
    <label class="f" style="grid-column:span 2">Tagline <input value="${esc(c.tagline ?? "")}" oninput="PBS.cfg.tagline=this.value${d}"></label>
    <label class="f" style="grid-column:span 2">Graph base <input value="${esc(c.graphBase ?? "")}" placeholder="https://graph.facebook.com/v19.0" oninput="PBS.cfg.graphBase=this.value${d}"></label>
    <div class="fh" style="grid-column:1/-1">FACEBOOK PAGE</div>
    <label class="f">Page id <input value="${esc(fb.pageId ?? "")}" oninput="PBS.cfg.facebook=PBS.cfg.facebook||{};PBS.cfg.facebook.pageId=this.value${d}"></label>
    <label class="f">Page token <input type="password" autocomplete="new-password" value="${esc(PB.secrets.fbToken)}" placeholder="${hasTok ? "•••••• (blank keeps it)" : "not set"}" oninput="PBS.secrets.fbToken=this.value${d}"></label>
    <div class="row" style="grid-column:span 2;align-self:end"><button class="small" onclick="pp.pubVerify()">Verify Facebook</button>${v ? `<span style="color:${v.ok === false ? "var(--danger)" : "var(--ready)"};font-size:11px">${esc(v.text)}</span>` : ""}</div>
    <div class="fh" style="grid-column:1/-1">WEBSITE (WordPress REST)</div>
    <label class="f">URL <input value="${esc(wb.url ?? "")}" placeholder="https://site/wp-json" oninput="PBS.cfg.website=PBS.cfg.website||{};PBS.cfg.website.url=this.value${d}"></label>
    <label class="f">User <input value="${esc(wb.user ?? "")}" oninput="PBS.cfg.website=PBS.cfg.website||{};PBS.cfg.website.user=this.value${d}"></label>
    <label class="f">Application password / key <input type="password" autocomplete="new-password" value="${esc(PB.secrets.wpKey)}" placeholder="${hasKey ? "•••••• (blank keeps it)" : "not set"}" oninput="PBS.secrets.wpKey=this.value${d}"></label>
    <label class="f">Publishing <label style="text-transform:none"><input type="checkbox" ${wb.autoPublish ? "checked" : ""} onchange="PBS.cfg.website=PBS.cfg.website||{};PBS.cfg.website.autoPublish=this.checked${d}"> publish posts (else WordPress drafts)</label></label>
    <div class="row" style="grid-column:1/-1"><button class="success" ${PB.cfgDirty ? "" : "disabled"} onclick="pp.pubCfgSave()">SAVE</button><button class="ghost" ${PB.cfgDirty ? "" : "disabled"} onclick="pp.pubCfgRevert()">revert</button></div></div></div>`;
}

function viewAudit() {
  const rows = PB.audit.slice(0, 300).map(a => `<tr><td class="mono" style="color:var(--t3)">${when(a.ms || a.atMs)}</td><td>${esc(a.actor || "")}</td><td>${esc(a.action || "")}</td><td class="mono" style="font-size:11px">${esc(a.itemId || a.id || "")}</td><td>${esc(a.channel || a.scope || "")}</td><td style="white-space:normal">${esc(a.note || a.reason || a.error || "")}</td></tr>`).join("");
  return `<div class="panel"><h3>Audit <span class="right"><button class="ghost small" onclick="pp.pubAudit()">&#8635;</button></span></h3><div class="scroll" style="max-height:60vh"><table><thead><tr><th>When</th><th>Actor</th><th>Action</th><th>Item</th><th>Channel</th><th>Note</th></tr></thead><tbody>${rows || `<tr><td colspan="6" class="empty">No audit entries.</td></tr>`}</tbody></table></div></div>`;
}

// ---------------------------------------------------------------------------------------------
const after = r => { const it = r && (r.item || (r.id && r.status ? r : null)); if (it && PB.sel === it.id) PB.detail = it; loadStatus(); loadQueue(); if (PB.sel) loadDetail(PB.sel); };
export const ppExtPublish = {
  pubReload() { loadPublish(); },
  pubQueue() { loadQueue(); },
  pubAudit() { loadAudit(); },
  pubSelect(id) { PB.sel = id; PB.detail = PB.items.find(x => x.id === id) || null; PB.act = { note: "", scheduleAt: PB.detail && PB.detail.scheduleAtMs ? localDt(PB.detail.scheduleAtMs) : "", title: (PB.detail && PB.detail.title) || "", body: (PB.detail && PB.detail.body) || "", caption: (PB.detail && PB.detail.caption) || "" }; pp.render(); loadDetail(id); },
  pubStop() { const s = PB.stop; pp.confirmDo("STOP all " + (s.scope === "all" ? "" : s.scope + " ") + "publishing now? Sending halts at once and the mode drops to off." + (s.reason ? "\n\nReason: " + s.reason : ""), () => pp.POST("/publish/stop", { reason: s.reason || "operator stop", scope: s.scope }).then(() => { pp.toast("Publishing stopped (" + s.scope + ")", "ok"); loadStatus(); }).catch(() => {})); },
  pubRelease() { const s = PB.stop; pp.confirmDo("Release the " + s.scope + " stop? The mode stays off until it is set again in the config.", () => pp.POST("/publish/release", { scope: s.scope }).then(() => { pp.toast("Released (" + s.scope + ")", "ok"); loadStatus(); }).catch(() => {})); },
  pubAct(id, action) {
    const a = PB.act; let body = { note: a.note };
    if (action === "schedule") { if (!a.scheduleAt) { pp.toast("Pick a date and time", "err"); return; } const ms = new Date(a.scheduleAt).getTime(); if (!(ms > 0)) { pp.toast("Invalid date", "err"); return; } body.scheduleAtMs = ms; }
    if (action === "correct") body = Object.assign(body, { title: a.title, body: a.body, caption: a.caption });
    const go = () => { PB.busy = action; pp.render(); pp.POST("/publish/items/" + enc(id) + "/" + action, body).then(r => { pp.toast(action + " → " + (((r && (r.item || r)) || {}).status || "ok"), "ok"); PB.act.note = ""; after(r); }).catch(() => {}).finally(() => { PB.busy = ""; pp.render(); }); };
    const it = PB.detail || {};
    if (action === "retract") pp.confirmDo("Retract \"" + (it.title || id) + "\"? It is taken down where possible and marked retracted (the record stays).", go);
    else if (action === "send") pp.confirmDo("Send \"" + (it.title || id) + "\" to " + (it.channels || []).join(", ") + " now?", go);
    else if (action === "correct") pp.confirmDo("Publish a correction of \"" + (it.title || id) + "\"? A new revision is recorded.", go);
    else go();
  },
  pubDelete(id) { const it = PB.detail || {}; pp.confirmDo("Delete the unpublished item \"" + (it.title || id) + "\"?", () => pp.api("DELETE", "/publish/items/" + enc(id)).then(() => { PB.sel = ""; PB.detail = null; loadQueue(); loadStatus(); }).catch(() => {})); },
  pubDraft() {
    const d = PB.draft; if (!d.title.trim() && !d.body.trim()) { pp.toast("A title or body is required", "err"); return; } if (!d.channels.length) { pp.toast("Pick a channel", "err"); return; }
    const body = { kind: d.kind, source: "manual", title: d.title, body: d.body, caption: d.caption, linkUrl: d.linkUrl.trim(), mediaId: d.mediaId.trim(), channels: d.channels,
      sourceLinks: d.sourceLinks.split("\n").map(s => s.trim()).filter(Boolean), tags: d.tags.split(",").map(s => s.trim()).filter(Boolean), ai: { text: d.aiText, images: d.aiImages, video: d.aiVideo, voice: d.aiVoice } };
    if (d.scheduleAt) { const ms = new Date(d.scheduleAt).getTime(); if (ms > 0) body.scheduleAtMs = ms; }
    PB.busy = "draft"; pp.render();
    pp.POST("/publish/draft", body).then(r => { const it = r.item || r; PB.draft = blankDraft(); PB.draftOpen = false; pp.toast("Draft saved" + (it && it.status ? " (" + it.status.replace(/_/g, " ") + ")" : ""), "ok"); loadQueue(); loadStatus(); if (it && it.id) ppExtPublish.pubSelect(it.id); })
      .catch(() => {}).finally(() => { PB.busy = ""; pp.render(); });
  },
  pubCfgSave() {
    const c = PB.cfg; const fb = Object.assign({}, c.facebook || {}), wb = Object.assign({}, c.website || {});
    delete fb.hasToken; delete fb.token; delete wb.hasKey; delete wb.key;
    if (PB.secrets.fbToken) fb.token = PB.secrets.fbToken; if (PB.secrets.wpKey) wb.key = PB.secrets.wpKey;      // blank = keep the stored secret
    const body = { mode: c.mode, recipes: c.recipes || [], tagline: c.tagline || "", maxPerDay: c.maxPerDay || 0, aiLabel: c.aiLabel || "", graphBase: c.graphBase || "", facebook: fb, website: wb };
    const go = () => pp.PUT("/publish/config", body).then(r => { PB.cfgDirty = false; PB.secrets = { fbToken: "", wpKey: "" }; if (r && typeof r === "object") PB.cfg = JSON.parse(JSON.stringify(r.config || r)); pp.toast("Publisher config saved", "ok"); loadStatus(); pp.render(); }).catch(() => {});
    const was = (PB.status.config && PB.status.config.mode) || PB.status.mode;
    if (c.mode === "auto" && was !== "auto") pp.confirmDo("Switch to auto mode? Finished spots are queued approved (absolute-tier items are still held).", go); else go();
  },
  pubCfgRevert() { PB.cfgDirty = false; PB.secrets = { fbToken: "", wpKey: "" }; pp.GET("/publish/config").then(c => { PB.cfg = JSON.parse(JSON.stringify(c.config || c)); pp.render(); }).catch(() => {}); },
  pubVerify() { PB.verify = { ok: true, text: "verifying…" }; pp.render(); pp.POST("/publish/verify").then(r => { PB.verify = { ok: !(r && r.ok === false), text: r && (r.name || r.page || r.message) ? "ok: " + (r.name || r.page || r.message) : JSON.stringify(r) }; }).catch(e => { PB.verify = { ok: false, text: e.message || String(e) }; }).finally(() => pp.render()); },
};
