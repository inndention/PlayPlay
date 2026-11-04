/* PLAY PLAY web dashboard — Flight plan: the schedule as a "broadcast flight plan".
   Operate  big now / next, countdowns, time debt, actionable readiness problems
   Plan     the rundown with clock anchors, time-debt rows between them, readiness strip, "Why?"
   Fleet    every channel: on air, next hard start, over / under, readiness
   The correction drawer (click an overrun or a gap) lists concrete options with their savings, previews them as a ghost
   (what moves, what drops, what is left) and applies them as a dated override, audited.
   Server: playout/schedule/flightplan.*, routes in supervisorcore_flightplan.cpp. State lives in F; inputs are state-bound. */

const F = { tab: "operate", ch: "", fp: null, fleet: {}, fetchedAt: 0, drawer: null, why: null, loaded: false, busy: false, audit: [] };
window.XF = F;
const esc = s => String(s ?? "").replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
const tm = ms => ms ? new Date(ms).toLocaleTimeString([], { hour: "numeric", minute: "2-digit", second: "2-digit" }) : "—";
const dur = s => { s = Math.max(0, Math.round(s || 0)); const h = Math.floor(s / 3600), m = Math.floor(s / 60) % 60; return (h ? h + ":" + String(m).padStart(2, "0") : m) + ":" + String(s % 60).padStart(2, "0"); };
const sec = s => { s = Math.round(Math.abs(s)); return s < 60 ? s + " s" : s < 3600 ? Math.floor(s / 60) + " m " + String(s % 60).padStart(2, "0") + " s" : Math.floor(s / 3600) + " h " + String(Math.floor(s / 60) % 60).padStart(2, "0") + " m"; };
const COL = { ok: "var(--ready)", warn: "var(--warn)", fail: "var(--danger)", over: "var(--danger)", under: "var(--standby)" };
const chans = () => (pp.S.channels || []).map(c => c.profile.id);

if (!document.getElementById("fpStyle")) {
  const st = document.createElement("style"); st.id = "fpStyle";
  st.textContent = `
.fp-tabs{display:flex;gap:6px;align-items:center;margin-bottom:10px}
.fp-card{background:var(--bg2);border:1px solid var(--b0);border-radius:8px;padding:12px 14px}
.fp-card.now{border-color:var(--onair)}.fp-big{font:700 34px var(--mono)}.fp-title{font-size:18px;font-weight:700;margin:2px 0}
.fp-grid{display:grid;gap:12px;grid-template-columns:1.4fr 1fr}
.fp-row{display:grid;grid-template-columns:78px 1fr 70px 78px 26px;gap:8px;align-items:center;padding:4px 8px;border-top:1px solid var(--b0);font-size:12px}
.fp-row.prot{background:rgba(255,45,85,.07)}.fp-row.fill{color:var(--t2)}.fp-row.now{background:rgba(255,45,85,.16)}
.fp-anchor{display:flex;gap:8px;align-items:center;padding:3px 8px;border-top:2px solid var(--accent);background:rgba(0,212,255,.08);font-size:11px;color:var(--accent)}
.fp-edge{padding:3px 8px;border-top:2px dashed var(--warn);color:var(--warn);font-size:11px;background:rgba(255,214,10,.06)}
.fp-debt{display:flex;gap:8px;align-items:center;padding:5px 8px;font-size:12px;font-weight:700;border-top:1px solid var(--b0)}
.fp-debt.over{background:rgba(255,69,58,.16);color:var(--danger)}.fp-debt.under{background:rgba(255,149,0,.12);color:var(--standby)}.fp-debt.ok{color:var(--t3);font-weight:400}
.fp-dots{display:inline-flex;gap:2px}.fp-dot{width:8px;height:8px;border-radius:4px;display:inline-block;cursor:help}
.fp-drawer{position:fixed;right:0;top:44px;bottom:0;width:440px;background:var(--bg3);border-left:1px solid var(--b1);box-shadow:-10px 0 30px rgba(0,0,0,.5);z-index:60;overflow:auto;padding:14px}
.fp-opt{display:grid;grid-template-columns:22px 1fr 70px;gap:8px;align-items:start;padding:6px 4px;border-top:1px solid var(--b0);font-size:12px}
.fp-chip{display:inline-block;padding:1px 7px;border-radius:3px;font-size:10px;font-weight:700;letter-spacing:.6px;border:1px solid}
.fp-fleet{display:grid;gap:12px;grid-template-columns:repeat(auto-fill,minmax(300px,1fr))}
.fp-why{margin:6px 0 0;padding-left:16px;font-size:12px;color:var(--t1)}
`;
  document.head.appendChild(st);
}

async function loadFlight() {
  const list = chans(); if (!list.length) return;
  if (!F.ch || !list.includes(F.ch)) F.ch = list[0];
  try {
    if (F.tab === "fleet") { const all = await Promise.all(list.map(c => pp.GET("/flightplan/" + c).catch(() => null))); list.forEach((c, i) => { F.fleet[c] = all[i]; }); F.fp = F.fleet[F.ch]; }
    else F.fp = await pp.GET("/flightplan/" + F.ch);
  } catch (e) { F.fp = { error: e.message || String(e) }; }
  F.fetchedAt = Date.now(); F.loaded = true;
  if (pp.S.view === "flight" && !F.drawer) pp.render();
}
export function onEnterFlight(v) { if (v === "flight") loadFlight(); }
export function onWsFlight(m) { if (m.type === "alert" || m.type === "schedule") { if (pp.S.view === "flight") loadFlight(); } return false; }
setInterval(() => { if (pp.S && pp.S.view === "flight" && !F.busy) loadFlight(); }, 3000);

// ---------------------------------------------------------------- shared pieces
const dots = r => {
  const checks = (r && r.checks) || {}, keys = Object.keys(checks); if (!keys.length) return `<span class="fp-dots" title="not checked yet">${"<span class='fp-dot' style='background:var(--b2)'></span>".repeat(1)}</span>`;
  return `<span class="fp-dots">${keys.map(k => `<span class="fp-dot" style="background:${COL[checks[k].state] || "var(--b2)"}" title="${esc(k)}: ${esc(checks[k].message)}"></span>`).join("")}</span>`;
};
const typeChip = t => `<span class="fp-chip" style="color:var(--t2);border-color:var(--b2)">${esc(t || "")}</span>`;

function allItems(fp) { return [fp.now, fp.next, ...(fp.later || [])].filter(Boolean); }

/** The rundown with clock anchors, time-debt rows between them and the safe edit boundary. */
function rundown(fp) {
  const items = allItems(fp), ivs = fp.intervals || [], anchors = fp.anchors || [];
  const elapsed = (Date.now() - F.fetchedAt) / 1000; let out = ""; let edgeDone = false;
  for (const it of items) {
    const a = anchors.find(x => x.itemId === it.id);
    if (a) out += `<div class="fp-anchor"><b>${esc(a.label)}</b> hard start · ${esc(a.title)} · ${a.policy === "finish_previous" ? "waits for the clip" : a.policy === "drop_previous" ? "drops the clip" : "cuts the clip"}${a.backtime ? " · back-timed" : ""}${a.lateSec > 1 ? ` · <span style="color:var(--danger)">${sec(a.lateSec)} late</span>` : ""}</div>`;
    if (!edgeDone && !it.protected) { edgeDone = true; out += `<div class="fp-edge">&#128274; safe edit boundary — everything above is protected until ${tm(fp.protectedUntilMs)}</div>`; }
    const rem = it.startsInSec <= 0 ? Math.max(0, it.remainingSec - elapsed) : it.startsInSec - elapsed;
    out += `<div class="fp-row${it.protected ? " prot" : ""}${it.fill ? " fill" : ""}${fp.now && fp.now.id === it.id ? " now" : ""}">
      <span class="mono">${tm(it.startMs)}</span><span style="overflow:hidden;text-overflow:ellipsis;white-space:nowrap" title="${esc(it.title)}">${it.protected ? "&#128274; " : ""}${esc(it.title)} ${typeChip(it.type)}${it.truncated ? ` <span style="color:var(--warn)" title="${esc(it.note || "")}">cut</span>` : ""}</span>
      <span class="mono" style="text-align:right">${it.startsInSec <= 0 ? dur(rem) + " left" : "in " + dur(rem)}</span><span>${dots(it.ready)}</span><button class="ghost small" title="why is this here?" onclick="pp.fpWhy('${esc(it.id).replace(/'/g, "\\'")}')">?</button></div>`;
    for (const v of ivs) if (Math.abs(v.endMs - it.endMs) < 1500 || (v.endMs > it.startMs && v.endMs <= it.endMs + 1500 && v.itemIds && v.itemIds[v.itemIds.length - 1] === it.id)) out += debtRow(v);
  }
  return out || `<div class="empty">Nothing is planned for the next hours.</div>`;
}
function debtRow(v) {
  const act = v.state === "ok" ? "" : `<span class="fill"></span><button class="small ${v.state === "over" ? "danger" : ""}" onclick="pp.fpDrawer(${v.index})">${v.state === "over" ? "Correct…" : "Fix this gap…"}</button>`;
  return `<div class="fp-debt ${v.state}">${v.state === "over" ? "&#9650;" : v.state === "under" ? "&#9660;" : "&#10003;"} ${esc(v.label)}${v.endsAtName && v.endsAtName !== "end of hour" ? " (" + esc(v.endsAtName) + ")" : ""}${act}</div>`;
}
function readinessIssues(fp) {
  const bad = allItems(fp).filter(i => i.ready && i.ready.state !== "ok");
  if (!bad.length) return `<div style="color:var(--ready);padding:6px 0">&#10003; Everything checked is ready for air (${fp.summary ? fp.summary.checked : 0} events).</div>`;
  return bad.map(i => `<div class="alert ${i.ready.state === "fail" ? "error" : "warning"}"><b>${esc(i.title)}</b> at ${tm(i.startMs)}<br>${(i.ready.problems || []).map(esc).join("<br>")}</div>`).join("");
}

// ---------------------------------------------------------------- views
function chanSel() { return `<select class="small" onchange="XF.ch=this.value;XF.fp=null;pp.fpReload()">${chans().map(c => `<option ${c === F.ch ? "selected" : ""}>${esc(c)}</option>`).join("")}</select>`; }
function tabs() { return `<div class="fp-tabs">${["operate", "plan", "fleet"].map(t => `<button class="ghost${F.tab === t ? " active" : ""}" onclick="XF.tab='${t}';XF.fp=null;pp.fpReload()">${t[0].toUpperCase() + t.slice(1)}</button>`).join("")}<span class="fill"></span>${F.tab === "fleet" ? "" : chanSel()}
    <span style="color:var(--t3);font-size:11px">${F.fp && F.fp.summary ? "debt: " + esc(F.fp.summary.label) : ""}</span></div>`; }

function viewOperate(fp) {
  const elapsed = (Date.now() - F.fetchedAt) / 1000, now = fp.now, next = fp.next;
  const nowRem = now ? Math.max(0, now.remainingSec - elapsed) : 0, nextIn = next ? Math.max(0, next.startsInSec - elapsed) : 0;
  const over = (fp.intervals || []).find(v => v.state === "over") || (fp.intervals || []).find(v => v.state === "under" && v.gapSec > 30);
  return `<div class="fp-grid"><div style="display:grid;gap:12px">
    <div class="fp-card now"><div style="color:var(--onair);font-size:11px;letter-spacing:1px;font-weight:700">ON AIR</div>
      ${now ? `<div class="fp-title">${esc(now.title)} ${typeChip(now.type)}</div><div class="fp-big">${dur(nowRem)}</div><div style="color:var(--t2);font-size:12px">remaining · ends ${tm(now.endMs)} · ${dots(now.ready)}</div>` : `<div class="empty">Nothing from the plan is on air.</div>`}</div>
    <div class="fp-card"><div style="color:var(--accent);font-size:11px;letter-spacing:1px;font-weight:700">NEXT</div>
      ${next ? `<div class="fp-title">${esc(next.title)} ${typeChip(next.type)}</div><div class="fp-big" style="font-size:24px">in ${dur(nextIn)}</div><div style="color:var(--t2);font-size:12px">${tm(next.startMs)} · ${dots(next.ready)} ${next.ready && next.ready.state !== "ok" ? "<b style='color:" + COL[next.ready.state] + "'>" + esc((next.ready.problems || [])[0] || "") + "</b>" : ""}</div>` : "<div class='empty'>—</div>"}</div>
    ${over ? `<div class="fp-card" style="border-color:${over.state === "over" ? "var(--danger)" : "var(--standby)"}"><div style="font-weight:700;color:${over.state === "over" ? "var(--danger)" : "var(--standby)"}">${esc(over.label)}</div><div class="row" style="margin-top:6px"><button class="${over.state === "over" ? "danger" : "primary"}" onclick="pp.fpDrawer(${over.index})">${over.state === "over" ? "Open the correction drawer" : "Fix this gap"}</button></div></div>` : ""}
    <div class="panel"><h3>Readiness</h3><div class="body">${readinessIssues(fp)}</div></div></div>
    <div class="panel"><h3>Later</h3><div class="scroll" style="max-height:72vh">${rundown(fp)}</div></div></div>`;
}
function viewPlan(fp) {
  return `<div class="panel"><h3>Rundown <span class="right">${(fp.anchors || []).length} hard start(s) · ${esc(fp.summary ? fp.summary.label : "")}</span></h3><div class="scroll" style="max-height:78vh">${rundown(fp)}</div></div>`;
}
function viewFleet() {
  const list = chans(), elapsed = (Date.now() - F.fetchedAt) / 1000;
  return `<div class="fp-fleet">${list.map(c => {
    const fp = F.fleet[c]; if (!fp || fp.error) return `<div class="fp-card"><b>${esc(c)}</b><div class="empty">no data</div></div>`;
    const a = (fp.anchors || []).find(x => x.atMs > fp.nowMs) || null, s = fp.summary || {}, now = fp.now;
    const state = s.readinessFails ? "fail" : s.overSec > 1 ? "over" : s.readinessWarnings ? "warn" : "ok";
    return `<div class="fp-card" style="border-color:${state === "ok" ? "var(--b0)" : COL[state]}"><div class="row"><b style="font-size:15px">${esc(c)}</b><span class="fill"></span><span class="fp-chip" style="color:${COL[state]};border-color:${COL[state]}">${state === "ok" ? "READY" : state === "over" ? "OVER" : state === "warn" ? "CHECK" : "NOT READY"}</span></div>
      <div style="margin:6px 0;font-size:13px">${now ? esc(now.title) + " <span class='mono' style='color:var(--t2)'>" + dur(Math.max(0, now.remainingSec - elapsed)) + " left</span>" : "<span style='color:var(--t3)'>nothing planned on air</span>"}</div>
      <div style="font-size:12px;color:var(--t2)">next hard start: ${a ? "<b>" + esc(a.label) + "</b> " + esc(a.title) : "—"}<br>time: <b style="color:${s.overSec > 1 ? "var(--danger)" : "var(--t1)"}">${esc(s.label || "")}</b> · readiness: ${s.readinessFails || 0} failed, ${s.readinessWarnings || 0} to check<br>
      <a style="cursor:pointer" onclick="XF.ch='${esc(c)}';XF.tab='operate';XF.fp=null;pp.fpReload()">open →</a></div></div>`;
  }).join("")}</div>`;
}

export function viewFlight() {
  if (!F.loaded) return `<div class="empty">Loading the flight plan…</div>`;
  const fp = F.fp;
  const body = !fp ? `<div class="empty">Loading…</div>` : fp.error ? `<div class="alert error">${esc(fp.error)}</div>` : F.tab === "fleet" ? viewFleet() : F.tab === "plan" ? viewPlan(fp) : viewOperate(fp);
  return tabs() + body + drawer() + whyBox();
}

// ---------------------------------------------------------------- correction drawer and "why"
function drawer() {
  const d = F.drawer; if (!d) return "";
  const c = d.data, over = c.kind === "over", opts = c.options || [], sel = d.sel;
  const saved = [...sel].reduce((a, i) => a + (opts[i] ? opts[i].savingSec : 0), 0), need = over ? c.debtSec : c.gapSec;
  const p = d.preview;
  return `<div class="fp-drawer"><div class="row"><b style="font-size:15px">${over ? "Correct the overrun" : "Fix this gap"}</b><span class="fill"></span><button class="ghost small" onclick="pp.fpClose()">&#10005;</button></div>
    <div style="margin:6px 0 10px;color:var(--t1)">${over ? `${sec(c.debtSec)} over before the next anchor. Mandatory events (ads, IDs, locked) and the protected window are never touched.` : `${sec(c.gapSec)} empty before the next anchor. Eligible clips that fit, least recently played first.`}</div>
    ${opts.length ? opts.map((o, i) => `<label class="fp-opt"><input type="checkbox" ${sel.has(i) ? "checked" : ""} onchange="pp.fpToggle(${i},this.checked)"><span>${esc(o.label)}</span><span class="mono" style="text-align:right;color:var(--ready)">${over ? "−" : ""}${sec(o.savingSec)}</span></label>`).join("") : `<div class="empty">${over ? "No safe correction found. Everything left is mandatory or protected." : "No eligible clip fits this gap. Filler, promo, bumper, PSA and station-ID clips that are approved, inside their window and allowed on this channel are used: add some to the library."}</div>`}
    <div class="row" style="margin:10px 0;font-size:12px"><span>Selected: <b>${over ? "−" : ""}${sec(saved)}</b> of ${sec(need)}${over ? (saved + 0.5 >= need ? ` <span style="color:var(--ready)">covers it${saved - need > 1 ? ", overshoots by " + sec(saved - need) : ""}</span>` : ` <span style="color:var(--danger)">${sec(need - saved)} still over</span>`) : ` · leaves ${sec(Math.max(0, need - saved))}`}</span></div>
    <div class="row"><button class="small" ${!sel.size || F.busy ? "disabled" : ""} onclick="pp.fpPreview()">Preview correction</button><button class="primary" ${!sel.size || F.busy || !p || !p.applied ? "disabled" : ""} onclick="pp.fpApply()">Apply</button><span style="color:var(--t3);font-size:11px">${!p ? "preview first: nothing is saved until Apply" : ""}</span></div>
    ${p ? `<div class="panel" style="margin-top:10px;background:var(--bg2)"><h3>Ghost preview</h3><div class="body" style="padding:10px;font-size:12px">${p.errors && p.errors.length ? p.errors.map(e => `<div class="alert error">${esc(e)}</div>`).join("") : ""}
      <b>${esc(p.diff ? p.diff.summary : "")}</b>${p.after && p.after.summary ? `<div style="color:var(--t2);margin:4px 0">after: ${esc(p.after.summary.label)}</div>` : ""}
      ${(p.diff ? p.diff.details : []).map(x => `<div><span class="fp-chip" style="color:${x.kind === "removed" ? "var(--danger)" : x.kind === "added" ? "var(--ready)" : "var(--warn)"};border-color:currentColor">${esc(x.kind)}</span> ${esc(x.title)} <span style="color:var(--t3)">${esc(x.text)}</span></div>`).join("")}</div></div>` : ""}</div>`;
}
function whyBox() {
  const w = F.why; if (!w) return "";
  return `<div class="fp-drawer" style="width:380px"><div class="row"><b>Why is this here?</b><span class="fill"></span><button class="ghost small" onclick="XF.why=null;pp.render()">&#10005;</button></div>
    <div style="font-size:15px;font-weight:700;margin:6px 0">${esc(w.title)}</div><ul class="fp-why">${(w.why || []).map(x => `<li>${esc(x)}</li>`).join("")}</ul>
    <div style="margin-top:10px;font-size:11px;letter-spacing:1px;color:var(--t3);font-weight:700">READINESS</div>${w.ready ? Object.entries(w.ready.checks || {}).map(([k, v]) => `<div style="font-size:12px"><span class="fp-dot" style="background:${COL[v.state]}"></span> <b>${esc(k)}</b> ${esc(v.message)}</div>`).join("") : ""}</div>`;
}

export const ppExtFlight = {
  fpReload() { loadFlight(); },
  async fpWhy(id) { try { F.why = await pp.GET("/flightplan/" + F.ch + "/why?id=" + encodeURIComponent(id)); } catch (e) { pp.toast(e.message || String(e), "err"); return; } pp.render(); },
  async fpDrawer(i) {
    try { const c = await pp.GET("/flightplan/" + F.ch + "/corrections?interval=" + i); const best = new Set(((c.best && c.best.ops) || []).map(o => JSON.stringify(o)));
      F.drawer = { i, data: c, sel: new Set((c.options || []).map((o, k) => k).filter(k => best.has(JSON.stringify(c.options[k].op)))), preview: null }; } catch (e) { pp.toast(e.message || String(e), "err"); return; }
    pp.render();
  },
  fpClose() { F.drawer = null; F.busy = false; loadFlight(); },
  fpToggle(i, on) { const d = F.drawer; if (!d) return; if (on) d.sel.add(i); else d.sel.delete(i); d.preview = null; pp.render(); },
  async fpPreview() {
    const d = F.drawer; if (!d) return; F.busy = true; pp.render();
    try { d.preview = await pp.POST("/flightplan/" + F.ch + "/preview", { ops: [...d.sel].map(i => d.data.options[i].op) }); } catch (e) { d.preview = { errors: [e.message || String(e)] }; }
    F.busy = false; pp.render();
  },
  fpApply() {
    const d = F.drawer; if (!d || !d.preview) return;
    pp.confirmDo("Apply this correction? " + (d.preview.diff ? d.preview.diff.summary : "") + ". It is saved as a dated override and recorded in the audit log.", async () => {
      F.busy = true; pp.render();
      try { const r = await pp.POST("/flightplan/" + F.ch + "/apply", { ops: [...d.sel].map(i => d.data.options[i].op), reason: "correction drawer", who: "operator" }); pp.toast("Corrected: " + (r.diff ? r.diff.summary : "saved"), "ok"); F.drawer = null; }
      catch (e) { pp.toast(e.message || String(e), "err"); }
      F.busy = false; loadFlight();
    });
  },
};
