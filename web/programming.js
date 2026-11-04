/* PLAY PLAY web dashboard — Programming console (PlayPlay OBS parity):
   hour recipes (time patterns laid as query blocks), the ad-break recipe builder, the format clock
   (weighted fill buckets), the autofill policy, ad contracts with pacing, and a query-block tester. */

const P = { tab: "recipes", recipes: [], builtins: [], filesPreview: null, def: null, facets: null, weights: [], weightsDefault: true, autofill: {}, contracts: [], tester: { types: "program", minutes: 10, prefer: "least_played", categories: "", subcategories: "", series: "", folder: "" }, testerOut: null,
  edit: null, adb: { programMin: 12, adMin: 3, programTypes: "program", adTypes: "ad,promo,psa", prefer: "least_played", adPrefer: "least_played", autogenMin: 0, idEveryBreak: false, idAtTop: true, name: "" },
  apply: { scope: "day", dow: new Date().getDay(), hours: "", dated: false, emptyOnly: false, keepLocks: true, recipeId: "default", weekStart: "" }, preview: null, lastChannel: "" };
window.P = P;
const e4 = s => String(s ?? "").replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
// A value as a JS string literal for an inline handler (e4 alone is decoded back into a real quote before the script is parsed).
const jq4 = v => e4(JSON.stringify(String(v ?? "")));
const fmtM = s => { s = Math.round(Number(s) || 0); return Math.floor(s / 60) + ":" + String(s % 60).padStart(2, "0"); };
const listStr = v => Array.isArray(v) ? v.join(", ") : String(v || "");
const toList = s => String(s || "").split(",").map(x => x.trim()).filter(Boolean);
const b4 = (text, color) => `<span class="badge" style="color:${color};border-color:${color};background:color-mix(in srgb,${color} 16%,transparent)">${e4(text)}</span>`;
const DOWS = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"];

async function loadProgramming() {
  const ch = pp.S.active; P.lastChannel = ch;
  const [r, f, w, a, c] = await Promise.all([pp.GET("/programming/recipes").catch(() => ({})), pp.GET("/programming/facets").catch(() => null),
    ch ? pp.GET("/channels/" + ch + "/weights").catch(() => ({})) : {}, ch ? pp.GET("/channels/" + ch + "/autofill").catch(() => ({})) : {}, pp.GET("/traffic/contracts" + (ch ? "?channel=" + ch : "")).catch(() => ({}))]);
  P.recipes = r.recipes || []; P.builtins = r.builtin || []; P.def = r.default || null; P.facets = f; P.weights = w.buckets || []; P.weightsDefault = !!w.isDefault; P.autofill = a || {}; P.contracts = c.contracts || [];
  pp.render();
}
export function onEnterProgramming(v) { if (v === "programming") loadProgramming(); }
export function onWsProgramming(m) { return false; }

// ------------------------------------------------------------------ pieces
function facetOpts(key, cur) { const f = P.facets ? P.facets[key] || [] : []; return `<datalist id="dl_${key}">${f.map(x => `<option value="${e4(x.name)}">${e4(x.name)} (${x.count})</option>`).join("")}</datalist>`; }
function partRow(p, i) {
  const set = (k, v) => `pp.progPartSet(${i},'${k}',${v})`;
  return `<tr><td><input value="${e4(p.label)}" style="width:110px" onchange="${set("label", "this.value")}"></td>
    <td><input value="${e4(listStr(p.types))}" style="width:150px" list="dl_types" onchange="${set("types", "this.value")}" placeholder="program, ad"></td>
    <td><input class="mono" type="number" step="0.5" min="0.25" max="60" value="${p.minutes}" style="width:64px" onchange="${set("minutes", "this.value")}"></td>
    <td><select class="small" onchange="${set("prefer", "this.value")}">${(P.facets ? P.facets.prefers : ["least_played", "newest", "oldest", "most_played", "random"]).map(x => `<option ${x === p.prefer ? "selected" : ""}>${x}</option>`).join("")}</select></td>
    <td><input value="${e4(listStr(p.categories))}" style="width:130px" list="dl_categories" onchange="${set("categories", "this.value")}" placeholder="any"></td>
    <td><input value="${e4(listStr(p.subcategories))}" style="width:120px" list="dl_subcategories" onchange="${set("subcategories", "this.value")}" placeholder="any"></td>
    <td><input value="${e4(listStr(p.series))}" style="width:120px" list="dl_series" onchange="${set("series", "this.value")}" placeholder="any"></td>
    <td class="row" style="flex-wrap:nowrap"><button class="ghost small" onclick="pp.progPartMove(${i},-1)">&#9650;</button><button class="ghost small" onclick="pp.progPartMove(${i},1)">&#9660;</button><button class="ghost small" onclick="pp.progPartDel(${i})">&#10005;</button></td></tr>`;
}
function recipeCard(r, i, builtin) {
  const shape = (r.parts || []).map(p => `${e4(p.label)} ${p.minutes}m${p.maxAgeDays ? " ≤" + p.maxAgeDays + "d" : ""}${p.minAgeDays ? " ≥" + p.minAgeDays + "d" : ""}`).join(" · ");
  const tools = builtin ? `<button class="ghost small" onclick="pp.progCopyRecipe(${jq4(r.id)})" title="Copy into your own recipes to edit">Copy</button>`
                        : `<button class="ghost small" onclick="pp.progEdit(${i})">Edit</button><button class="ghost small" onclick="pp.progRecipeDel(${i})">&#10005;</button>`;
  return `<div class="row" style="padding:7px 10px;border-top:1px solid var(--b0);gap:8px"><span style="width:3px;height:30px;background:${builtin ? "var(--t3)" : "var(--accent)"};border-radius:1px"></span>
    <span class="fill" style="overflow:hidden"><div><b>${e4(r.name)}</b>${builtin ? ' <span class="muted" style="font-size:10px">built in</span>' : ""} <span class="muted" style="font-size:10px">cycle ${fmtM(r.cycleSec || 0)}${r.idAtTop ? " · ID at :00" : ""}</span></div><div style="color:var(--t3);font-size:10px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis" title="${e4(r.description || "")}">${shape}</div></span>
    <button class="small" onclick="pp.progFiles(${jq4(r.id || "default")})" title="Which files this recipe would play in the next hour (nothing is written)">Files</button><button class="small" onclick="pp.progApplyPick(${jq4(r.id || "default")})" title="Lay this recipe into the hours chosen on the right">Apply…</button>${tools}</div>`;
}
// Blocks of one previewed hour with the files each resolved to; red when a block found nothing.
function blocksHtml(blocks) {
  return (blocks || []).map(b => {
    const picks = b.picks || [];
    const files = picks.length ? picks.map(x => e4(x.title) + ' <span class="muted">' + fmtM(x.duration || 0) + (x.overrun ? " cut" : "") + (x.repeat ? " repeat" : "") + "</span>").join(" · ")
                               : `<span style="color:var(--danger,#e55)">nothing: ${e4(b.note || "no match")}</span>`;
    return `<div style="font-size:11px;padding:1px 0;display:flex;gap:8px"><b style="min-width:130px;max-width:130px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;color:${b.kind === "fill" ? "var(--t3)" : "var(--accent)"}">${e4(b.label)}</b><span style="overflow:hidden;text-overflow:ellipsis;white-space:nowrap">${files}</span></div>`;
  }).join("");
}
function applyPanel(kind) {
  const a = P.apply; const chan = pp.S.active || "";
  const hoursSel = `<input value="${e4(a.hours)}" placeholder="all (e.g. 6-23 or 6,7,8)" style="width:150px" onchange="P.apply.hours=this.value">`;
  return `<div class="panel"><h3>${kind === "weighted" ? "Weighted fill" : "Lay recipe"} → ${e4(chan)} <span class="right">${kind === "weighted" ? "concrete media from the format clock" : "late-bound query blocks"}</span></h3><div class="body grid" style="gap:8px">
    ${kind !== "weighted" ? `<label>Recipe <select onchange="P.apply.recipeId=this.value">${[{ id: "default", name: "Default (built in)" }].concat(P.builtins.map(b => Object.assign({}, b, { name: "★ " + b.name }))).concat(P.recipes).map(r => `<option value="${e4(r.id)}" ${a.recipeId === r.id ? "selected" : ""}>${e4(r.name)}</option>`).join("")}</select></label>` : ""}
    <div class="row"><label>Scope <select onchange="P.apply.scope=this.value;pp.render()">${["hour", "day", "week", "hours"].map(s => `<option ${a.scope === s ? "selected" : ""}>${s}</option>`).join("")}</select></label>
      ${a.scope === "day" || a.scope === "hour" ? `<label>Day <select onchange="P.apply.dow=parseInt(this.value)">${DOWS.map((d, i) => `<option value="${i}" ${a.dow === i ? "selected" : ""}>${d}</option>`).join("")}</select></label>` : ""}
      ${a.scope === "hour" ? `<label>Hour <input class="mono" type="number" min="0" max="23" value="${a.hour || 0}" style="width:56px" onchange="P.apply.hour=parseInt(this.value)||0"></label>` : `<label>Hours ${hoursSel}</label>`}
      ${a.scope === "hours" ? `<label>Days <input value="${e4(a.dows || "")}" placeholder="0-6 (Sun=0)" style="width:90px" onchange="P.apply.dows=this.value"></label>` : ""}</div>
    <div class="row"><label><input type="checkbox" ${a.dated ? "checked" : ""} onchange="P.apply.dated=this.checked;pp.render()"> Dated overrides</label>${a.dated ? `<label>Week of <input type="date" value="${e4(a.weekStart)}" onchange="P.apply.weekStart=this.value"></label>` : '<span class="muted" style="font-size:10px">(weekly template)</span>'}
      <label><input type="checkbox" ${a.emptyOnly ? "checked" : ""} onchange="P.apply.emptyOnly=this.checked"> Empty hours only</label><label><input type="checkbox" ${a.keepLocks ? "checked" : ""} onchange="P.apply.keepLocks=this.checked"> Keep locked items</label>
      ${kind === "weighted" ? `<label><input type="checkbox" ${a.onePer !== false ? "checked" : ""} onchange="P.apply.onePer=this.checked"> One program per hour</label><label>Seed <input class="mono" type="number" value="${a.seed || 1}" style="width:60px" onchange="P.apply.seed=parseInt(this.value)||1"></label>` : ""}</div>
    <div class="row"><button class="small" onclick="pp.progApply('${kind}',false)">Preview</button><button class="small primary" onclick="pp.progApply('${kind}',true)">Apply to schedule</button><button class="small ghost" onclick="pp.progClear()">Clear scope (keep locks)</button></div>
    ${P.filesPreview && kind !== "weighted" ? `<div class="scroll" style="max-height:300px"><div class="row" style="font-size:11px"><b>${e4(P.filesPreview.recipe.name)}</b><span class="muted">next hour on ${e4(P.filesPreview.channel)}: ${P.filesPreview.summary.picks} file(s)${P.filesPreview.summary.unresolvedBlocks ? ", " + P.filesPreview.summary.unresolvedBlocks + " block(s) found nothing" : ""}</span><span class="fill"></span><button class="ghost small" onclick="P.filesPreview=null;pp.render()">&#10005;</button></div>
      ${(P.filesPreview.hours || []).map(h => `<div style="padding:4px 0;border-top:1px solid var(--b0)"><b class="mono">${e4(h.date)} ${String(h.hour).padStart(2, "0")}:00</b>${blocksHtml(h.blocks)}</div>`).join("")}</div>` : ""}
    ${P.preview ? `<div class="scroll" style="max-height:260px"><div class="muted" style="font-size:10px;margin:4px 0">${e4(P.preview.summary ? JSON.stringify(P.preview.summary.targets !== undefined ? { targets: P.preview.summary.targets, written: P.preview.summary.written, placed: P.preview.summary.placed, applied: P.preview.summary.applied } : P.preview.summary) : "")}</div>
      ${Object.entries(P.preview.slots || {}).slice(0, 48).map(([id, s]) => `<div style="padding:4px 0;border-top:1px solid var(--b0)"><b class="mono">${e4(id)}</b> <span class="muted" style="font-size:10px">${fmtM(s.placedSec || 0)} · ${(s.items || []).length} item(s)</span>${P.preview.previews && P.preview.previews[id] ? blocksHtml(P.preview.previews[id].blocks) : `<div style="font-size:11px;color:var(--t1);white-space:nowrap;overflow:hidden;text-overflow:ellipsis">${(s.items || []).map(it => (it.isLocked ? "&#128274; " : "") + e4(it.title)).join(" · ")}</div>`}</div>`).join("")}</div>` : ""}</div></div>`;
}

function tabRecipes() {
  const ed = P.edit; const adb = P.adb;
  const editor = ed ? `<div class="panel"><h3>${ed.index >= 0 ? "Edit recipe" : "New recipe"} <span class="right"><button class="small ghost" onclick="P.edit=null;pp.render()">Cancel</button><button class="small primary" onclick="pp.progRecipeSave()">Save recipe</button></span></h3><div class="body">
      <div class="row"><label>Name <input value="${e4(ed.r.name)}" style="width:220px" onchange="P.edit.r.name=this.value"></label><label><input type="checkbox" ${ed.r.idAtTop ? "checked" : ""} onchange="P.edit.r.idAtTop=this.checked"> Station ID at :00</label><span class="muted" style="font-size:10px">cycle ${fmtM((ed.r.parts || []).reduce((a, p) => a + (parseFloat(p.minutes) || 0) * 60, 0))} — repeats until the hour is full</span></div>
      <table><thead><tr><th>Label</th><th>Types</th><th>Min</th><th>Prefer</th><th>Categories</th><th>Subcategories</th><th>Series</th><th></th></tr></thead><tbody>${(ed.r.parts || []).map(partRow).join("")}</tbody></table>
      <div class="row" style="margin-top:6px"><button class="small" onclick="pp.progPartAdd()">+ part</button></div></div></div>` : "";
  return `<div class="grid cols2">
    <div><div class="panel"><h3>Hour recipes <span class="right"><button class="small" onclick="pp.progEdit(-1)">+ New</button><button class="small ghost" onclick="pp.progEditDefault()">Copy default</button></span></h3>
      ${P.recipes.length ? P.recipes.map(recipeCard).join("") : `<div class="empty">No saved recipes. The built-in default (10 programs / 5 ads / 3 autogen / 1 ID) is always available.</div>`}
      ${P.def ? recipeCard(P.def, -1, true) : ""}
      ${P.builtins.length ? `<div class="muted" style="font-size:10px;padding:8px 10px 2px;text-transform:uppercase;letter-spacing:1px">Built-in recipes</div>` + P.builtins.map(r => recipeCard(r, -1, true)).join("") : ""}</div>
    ${editor}
    <div class="panel"><h3>Ad-break recipe builder <span class="right">"programs with a break every N minutes"</span></h3><div class="body grid" style="gap:8px">
      <div class="row"><label>Program block <input class="mono" type="number" min="1" max="59" value="${adb.programMin}" style="width:60px" onchange="P.adb.programMin=parseFloat(this.value)||12"> min</label><label>Break <input class="mono" type="number" min="0.5" step="0.5" max="30" value="${adb.adMin}" style="width:60px" onchange="P.adb.adMin=parseFloat(this.value)||3"> min</label><label>Autogen <input class="mono" type="number" min="0" step="0.5" max="15" value="${adb.autogenMin}" style="width:60px" onchange="P.adb.autogenMin=parseFloat(this.value)||0"> min</label></div>
      <div class="row"><label>Program types <input value="${e4(adb.programTypes)}" style="width:120px" list="dl_types" onchange="P.adb.programTypes=this.value"></label><label>Play <select onchange="P.adb.prefer=this.value">${["least_played", "newest", "oldest", "most_played", "random", "longest", "shortest"].map(x => `<option ${adb.prefer === x ? "selected" : ""}>${x}</option>`).join("")}</select></label>
        <label>Break types <input value="${e4(adb.adTypes)}" style="width:130px" list="dl_types" onchange="P.adb.adTypes=this.value"></label><label>Break order <select onchange="P.adb.adPrefer=this.value">${["least_played", "newest", "oldest", "most_played", "random"].map(x => `<option ${adb.adPrefer === x ? "selected" : ""}>${x}</option>`).join("")}</select></label></div>
      <div class="row"><label>Program categories <input value="${e4(adb.programCategories || "")}" style="width:140px" list="dl_categories" placeholder="any" onchange="P.adb.programCategories=this.value"></label><label>Series <input value="${e4(adb.programSeries || "")}" style="width:140px" list="dl_series" placeholder="any" onchange="P.adb.programSeries=this.value"></label>
        <label><input type="checkbox" ${adb.idEveryBreak ? "checked" : ""} onchange="P.adb.idEveryBreak=this.checked"> ID before each break</label><label><input type="checkbox" ${adb.idAtTop ? "checked" : ""} onchange="P.adb.idAtTop=this.checked"> ID at :00</label></div>
      <div class="row"><input value="${e4(adb.name)}" placeholder="recipe name (optional)" style="width:240px" onchange="P.adb.name=this.value"><button class="small primary" onclick="pp.progAdBreak(true)">Build &amp; save recipe</button><button class="small" onclick="pp.progAdBreak(false)">Preview shape</button></div>
      ${P.adbPreview ? `<div class="muted" style="font-size:11px">${e4(P.adbPreview.name)}: ${(P.adbPreview.preview.items || []).map(it => e4(it.title)).join(" · ")}</div>` : ""}</div></div></div>
    <div>${applyPanel("recipe")}</div></div>`;
}

function tabClock() {
  const tot = P.weights.reduce((a, b) => a + (parseFloat(b.pct) || 0), 0);
  const set = (i, k, v) => `pp.progBucketSet(${i},'${k}',${v})`;
  return `<div class="grid cols2"><div><div class="panel"><h3>Format clock — program mix for ${e4(pp.S.active || "")} <span class="right">${P.weightsDefault ? b4("default", "var(--t3)") : ""}<b class="mono" style="color:${Math.abs(tot - 100) < 0.5 ? "var(--ready)" : "var(--warn)"}">${tot.toFixed(0)}%</b><button class="small" onclick="pp.progBucketAdd()">+ bucket</button><button class="small primary" onclick="pp.progWeightsSave()">Save</button></span></h3>
    <div class="body"><p class="muted" style="font-size:11px;margin:0 0 8px">Shares are AIRTIME of the hour. First matching bucket wins, so put specific buckets (a series, a category) before broad ones. Rotation: least played → longest since aired → newest in library.</p>
    <table><thead><tr><th>Label</th><th>Types</th><th>Categories</th><th>Subcategories</th><th>Series</th><th>%</th><th>Max/h</th><th></th></tr></thead><tbody>
    ${P.weights.map((b, i) => `<tr><td><input value="${e4(b.label)}" style="width:120px" onchange="${set(i, "label", "this.value")}"></td><td><input value="${e4(listStr(b.types))}" list="dl_types" style="width:150px" onchange="${set(i, "types", "this.value")}"></td><td><input value="${e4(listStr(b.categories))}" list="dl_categories" style="width:120px" placeholder="any" onchange="${set(i, "categories", "this.value")}"></td><td><input value="${e4(listStr(b.subcategories))}" list="dl_subcategories" style="width:110px" placeholder="any" onchange="${set(i, "subcategories", "this.value")}"></td><td><input value="${e4(b.series || "")}" list="dl_series" style="width:110px" placeholder="any" onchange="${set(i, "series", "this.value")}"></td>
      <td><input class="mono" type="number" min="0" max="100" value="${b.pct}" style="width:56px" onchange="${set(i, "pct", "this.value")}"></td><td><input class="mono" type="number" min="0" value="${b.maxPerHour || 0}" style="width:52px" onchange="${set(i, "maxPerHour", "this.value")}"></td>
      <td class="row" style="flex-wrap:nowrap"><button class="ghost small" onclick="pp.progBucketMove(${i},-1)">&#9650;</button><button class="ghost small" onclick="pp.progBucketMove(${i},1)">&#9660;</button><button class="ghost small" onclick="pp.progBucketDel(${i})">&#10005;</button></td></tr>`).join("")}</tbody></table>
    <div style="margin-top:8px;height:14px;display:flex;border-radius:4px;overflow:hidden">${P.weights.map((b, i) => `<div title="${e4(b.label)} ${b.pct}%" style="width:${Math.max(0, Math.min(100, (parseFloat(b.pct) || 0) / Math.max(tot, 1) * 100))}%;background:hsl(${(i * 67) % 360} 60% 45%)"></div>`).join("")}</div></div></div></div>
    <div>${applyPanel("weighted")}</div></div>`;
}

function tabAutofill() {
  const a = P.autofill;
  return `<div class="panel"><h3>Autofill policy — ${e4(pp.S.active || "")} <span class="right"><button class="small primary" onclick="pp.progAutofillSave()">Save</button></span></h3><div class="body grid" style="gap:10px">
    <label><input type="checkbox" ${a.enabled ? "checked" : ""} onchange="P.autofill.enabled=this.checked"> <b>Fill empty hours automatically</b> when the executor builds them (nothing is written to the schedule; the hour is shaped at air)</label>
    <div class="row"><label>Mode <select onchange="P.autofill.mode=this.value">${["recipe", "weighted"].map(m => `<option ${a.mode === m ? "selected" : ""}>${m}</option>`).join("")}</select></label>
      <label>Recipe <select onchange="P.autofill.recipeId=this.value">${[{ id: "", name: "Default (built in)" }].concat(P.recipes).map(r => `<option value="${e4(r.id)}" ${(a.recipeId || "") === r.id ? "selected" : ""}>${e4(r.name)}</option>`).join("")}</select></label>
      <label>No-repeat window <input class="mono" type="number" min="0" value="${a.noRepeatMin !== undefined ? a.noRepeatMin : 30}" style="width:60px" onchange="P.autofill.noRepeatMin=parseFloat(this.value)||0"> min</label>
      <label><input type="checkbox" ${a.adPacing !== false ? "checked" : ""} onchange="P.autofill.adPacing=this.checked"> Ad-contract pacing</label></div>
    <label>Interstitial order at air <input value="${e4(listStr(a.order))}" list="dl_types" style="width:320px" onchange="P.autofill.order=this.value"> <span class="muted" style="font-size:10px">(gap fill picks by this type order, then pacing, then least recently played)</span></label>
    <div class="row"><button class="small" onclick="pp.progReroll()">Re-roll this channel's blocks now</button><span class="muted" style="font-size:10px">forgets the current hour's query-block picks and rebuilds</span></div></div></div>`;
}

function tabContracts() {
  const set = (i, k, v) => `pp.progContractSet(${i},'${k}',${v})`;
  const pacing = c => c.target !== undefined ? `<div class="meter" style="margin-top:3px"><div class="bar" style="width:${Math.min(100, c.pacingPct || 0)}%;background:${(c.pacingPct || 0) < 90 ? "var(--warn)" : "var(--ready)"}"></div></div><span class="muted" style="font-size:10px">${c.aired}/${c.spotsTotal} aired · target ${Math.round(c.target)} · ${c.daysLeft} d left${c.projectedShortfall > 0 ? ` · short ${Math.round(c.projectedShortfall)}` : ""}</span>` : (c.spotsPerDay ? `<span class="muted" style="font-size:10px">today ${c.airedToday}/${c.spotsPerDay}</span>` : `<span class="muted" style="font-size:10px">${c.aired} aired</span>`);
  return `<div class="panel"><h3>Ad contracts <span class="right"><button class="small" onclick="pp.progContractAdd()">+ contract</button><button class="small primary" onclick="pp.progContractsSave()">Save</button><a class="btn small ghost" href="/reports/contracts.html" target="_blank">Report</a></span></h3><div class="body">
    <p class="muted" style="font-size:11px;margin:0 0 8px">Behind-pace campaigns float to the top of their tier when ad blocks resolve and gaps fill; fulfilled ones back off. Media ids: pick from the library (right-click → copy id) or type titles below.</p>
    <table><thead><tr><th>Client / title</th><th>Media ids</th><th>Spots</th><th>Per day</th><th>Start</th><th>End</th><th>Channels</th><th>Status</th><th>Pacing</th><th></th></tr></thead><tbody>
    ${P.contracts.map((c, i) => `<tr><td><input value="${e4(c.client)}" placeholder="client" style="width:110px" onchange="${set(i, "client", "this.value")}"><input value="${e4(c.title)}" placeholder="title" style="width:110px" onchange="${set(i, "title", "this.value")}"></td>
      <td><input value="${e4(listStr(c.mediaIds))}" style="width:180px" onchange="${set(i, "mediaIds", "this.value")}"></td><td><input class="mono" type="number" value="${c.spotsTotal || 0}" style="width:60px" onchange="${set(i, "spotsTotal", "this.value")}"></td><td><input class="mono" type="number" value="${c.spotsPerDay || 0}" style="width:56px" onchange="${set(i, "spotsPerDay", "this.value")}"></td>
      <td><input type="date" value="${e4(c.startDate || "")}" onchange="${set(i, "startDate", "this.value")}"></td><td><input type="date" value="${e4(c.endDate || "")}" onchange="${set(i, "endDate", "this.value")}"></td><td><input value="${e4(listStr(c.channels))}" placeholder="all" style="width:80px" onchange="${set(i, "channels", "this.value")}"></td>
      <td><select class="small" onchange="${set(i, "status", "this.value")}">${["active", "paused", "done"].map(s => `<option ${c.status === s ? "selected" : ""}>${s}</option>`).join("")}</select></td><td style="min-width:140px">${pacing(c)}</td><td><button class="small" data-id="${e4(c.id)}" onclick="pp.progContractReport(this.dataset.id)">Report</button> <button class="ghost small" onclick="pp.progContractDel(${i})">&#10005;</button></td></tr>`).join("") || `<tr><td colspan="10" class="empty">No contracts</td></tr>`}</tbody></table></div></div>`;
}

function tabTester() {
  const t = P.tester; const o = P.testerOut;
  return `<div class="grid cols2"><div class="panel"><h3>Query block tester <span class="right">what would this block resolve to, right now, on ${e4(pp.S.active || "")}?</span></h3><div class="body grid" style="gap:8px">
    <div class="row"><label>Types <input value="${e4(t.types)}" list="dl_types" style="width:150px" onchange="P.tester.types=this.value"></label><label>Target <input class="mono" type="number" step="0.5" value="${t.minutes}" style="width:60px" onchange="P.tester.minutes=parseFloat(this.value)||0"> min</label><label>Prefer <select onchange="P.tester.prefer=this.value">${["least_played", "newest", "oldest", "most_played", "random", "longest", "shortest"].map(x => `<option ${t.prefer === x ? "selected" : ""}>${x}</option>`).join("")}</select></label></div>
    <div class="row"><label>Categories <input value="${e4(t.categories)}" list="dl_categories" style="width:140px" onchange="P.tester.categories=this.value"></label><label>Subcategories <input value="${e4(t.subcategories)}" list="dl_subcategories" style="width:130px" onchange="P.tester.subcategories=this.value"></label><label>Series <input value="${e4(t.series)}" list="dl_series" style="width:140px" onchange="P.tester.series=this.value"></label></div>
    <div class="row"><label>…or folder block: <input value="${e4(t.folder)}" list="dl_folders" placeholder="folder path, category or tag" style="width:260px" onchange="P.tester.folder=this.value"></label><button class="small primary" onclick="pp.progTest()">Resolve</button></div>
    ${o ? `<div><b>${e4(o.label)}</b>${o.why ? ` <span style="color:var(--warn)">${e4(o.why)}</span>` : ""}<div class="muted" style="font-size:10px">picks: ${(o.picks || []).map(p => e4(p.title) + " (" + fmtM(p.duration) + (p.overrun ? ", overrun" : "") + (p.repeat ? ", repeat" : "") + ")").join(" · ") || "none"}</div></div>
      <div class="scroll" style="max-height:300px"><table><thead><tr><th>#</th><th>Title</th><th>Type</th><th>Len</th><th>Plays</th><th>Last</th><th>Cat / sub / series</th></tr></thead><tbody>${(o.candidates || []).map((c, i) => `<tr class="${(o.picks || []).some(p => p.id === c.id) ? "onair" : ""}"><td class="mono">${i + 1}</td><td>${e4(c.title)}${c.repeat ? " " + b4("repeat", "var(--warn)") : ""}</td><td>${e4(c.type || "")}</td><td class="mono">${fmtM(c.duration)}</td><td class="mono">${c.playCount || 0}</td><td class="mono">${c.lastPlayedMs ? new Date(c.lastPlayedMs).toLocaleString() : "never"}</td><td class="muted" style="font-size:10px">${e4([c.category, c.subcategory, c.series].filter(Boolean).join(" / "))}</td></tr>`).join("")}</tbody></table></div>` : ""}</div></div>
    <div class="panel"><h3>Library facets</h3><div class="body" style="font-size:11px">${P.facets ? ["types", "categories", "subcategories", "series", "tags", "clients"].map(k => `<div style="margin-bottom:6px"><b style="text-transform:uppercase;font-size:10px;letter-spacing:1px;color:var(--t2)">${k}</b><div>${(P.facets[k] || []).slice(0, 30).map(x => `<span class="badge" style="margin:2px;color:var(--t1);border-color:var(--b1)">${e4(x.name)} <span class="mono">${x.count}</span></span>`).join("") || '<span class="muted">none</span>'}</div></div>`).join("") : "loading…"}</div></div></div>`;
}

export function viewProgramming() {
  if (pp.S.active && P.lastChannel && P.lastChannel !== pp.S.active) { P.lastChannel = pp.S.active; setTimeout(loadProgramming, 0); }
  const tabs = [["recipes", "Hour recipes"], ["clock", "Format clock"], ["autofill", "Autofill"], ["contracts", "Ad contracts"], ["tester", "Query tester"]];
  return `<div class="row" style="margin-bottom:10px">${tabs.map(([k, l]) => `<button class="small ${P.tab === k ? "primary" : "ghost"}" onclick="P.tab='${k}';pp.render()">${l}</button>`).join("")}<span class="fill"></span><span class="muted" style="font-size:10px">channel ${e4(pp.S.active || "")}</span></div>
    ${facetOpts("types")}${facetOpts("categories")}${facetOpts("subcategories")}${facetOpts("series")}${facetOpts("folders")}
    ${P.tab === "recipes" ? tabRecipes() : P.tab === "clock" ? tabClock() : P.tab === "autofill" ? tabAutofill() : P.tab === "contracts" ? tabContracts() : tabTester()}`;
}

// ------------------------------------------------------------------ handlers
function applyBody(extra) {
  const a = P.apply; const body = Object.assign({ scope: a.scope, dated: a.dated, emptyOnly: a.emptyOnly, keepLocks: a.keepLocks }, extra || {});
  if (a.scope === "day" || a.scope === "hour") body.dow = a.dow;
  if (a.scope === "hour") body.hour = a.hour || 0;
  const hrs = parseRange(a.hours, 0, 23); if (hrs.length && a.scope !== "hour") body.hours = hrs;
  const dows = parseRange(a.dows, 0, 6); if (dows.length && a.scope === "hours") body.dows = dows;
  if (a.dated && a.weekStart) body.weekStart = a.weekStart;
  if (a.dated && a.scope === "hour") body.date = dateOfDow(a.weekStart, a.dow);   // scope hour takes its date (or slotId) from the body, not from weekStart
  return body;
}
// YYYY-MM-DD of weekday `dow` (Sunday = 0) in the week holding `weekStart` (YYYY-MM-DD; default: this week).
function dateOfDow(weekStart, dow) { const m = String(weekStart || "").match(/^(\d{4})-(\d{2})-(\d{2})/); const d = m ? new Date(+m[1], +m[2] - 1, +m[3]) : new Date(); d.setDate(d.getDate() - d.getDay() + (dow || 0)); return d.getFullYear() + "-" + String(d.getMonth() + 1).padStart(2, "0") + "-" + String(d.getDate()).padStart(2, "0"); }
function parseRange(s, lo, hi) { const out = []; for (const part of String(s || "").split(",")) { const p = part.trim(); if (!p) continue; const m = p.match(/^(\d+)\s*-\s*(\d+)$/); if (m) { for (let i = +m[1]; i <= +m[2]; i++) if (i >= lo && i <= hi) out.push(i); } else if (!isNaN(+p) && +p >= lo && +p <= hi) out.push(+p); } return out; }

export const ppExtProgramming = {
  progEdit(i) { P.edit = i >= 0 ? { index: i, r: JSON.parse(JSON.stringify(P.recipes[i])) } : { index: -1, r: { name: "", idAtTop: false, parts: [{ label: "Programs", types: ["program"], minutes: 10, prefer: "least_played" }] } }; pp.render(); },
  progEditDefault() { P.edit = { index: -1, r: Object.assign(JSON.parse(JSON.stringify(P.def || { parts: [] })), { id: undefined, name: "My default" }) }; pp.render(); },
  progPartSet(i, k, v) { const p = P.edit.r.parts[i]; if (k === "minutes") p.minutes = parseFloat(v) || 0; else if (["types", "categories", "subcategories", "series"].includes(k)) p[k] = toList(v); else p[k] = v; },
  progPartAdd() { P.edit.r.parts.push({ label: "Ads", types: ["ad"], minutes: 3, prefer: "least_played" }); pp.render(); },
  progPartDel(i) { P.edit.r.parts.splice(i, 1); pp.render(); },
  progPartMove(i, d) { const a = P.edit.r.parts, j = i + d; if (j < 0 || j >= a.length) return; [a[i], a[j]] = [a[j], a[i]]; pp.render(); },
  async progRecipeSave() { const ed = P.edit; if (!ed) return; if (!ed.r.name.trim()) { pp.toast("Name the recipe", "err"); return; } const list = P.recipes.slice(); if (ed.index >= 0) list[ed.index] = ed.r; else list.push(ed.r); const r = await pp.PUT("/programming/recipes", { recipes: list }); P.recipes = r.recipes || []; P.edit = null; pp.reloadRecipes && pp.reloadRecipes(); pp.toast("Recipes saved", "ok"); pp.render(); },
  progRecipeDel(i) { if (i < 0) return; pp.confirmDo("Delete recipe \"" + P.recipes[i].name + "\"?", async () => { const list = P.recipes.slice(); list.splice(i, 1); const r = await pp.PUT("/programming/recipes", { recipes: list }); P.recipes = r.recipes || []; pp.reloadRecipes && pp.reloadRecipes(); pp.render(); }); },
  async progFiles(id) {
    const ch = pp.S.active; if (!ch) { pp.toast("Pick a channel first", "err"); return; }
    const r = await pp.POST("/programming/recipes/preview", { recipeId: id, channel: ch, hours: 1 });
    P.filesPreview = r; P.tab = "recipes"; pp.render();
    pp.toast(r.recipe.name + ": " + r.summary.picks + " file(s)" + (r.summary.unresolvedBlocks ? ", " + r.summary.unresolvedBlocks + " block(s) found nothing" : ""), r.summary.unresolvedBlocks ? "err" : "ok");
  },
  progCopyRecipe(id) {
    const src = id === "default" || !id ? P.def : P.builtins.find(b => b.id === id); if (!src) return;
    const r = JSON.parse(JSON.stringify(src)); delete r.id; delete r.cycleSec; r.name = (id === "default" || !id ? "My default" : src.name + " (copy)");
    P.edit = { index: -1, r }; pp.render();
  },
  progApplyPick(id) { P.apply.recipeId = id; P.tab = "recipes"; pp.render(); pp.toast("Recipe selected — choose the scope and Apply", "ok"); },
  async progAdBreak(save) { const b = P.adb; const body = { programMin: b.programMin, adMin: b.adMin, autogenMin: b.autogenMin, programTypes: toList(b.programTypes), adTypes: toList(b.adTypes), prefer: b.prefer, adPrefer: b.adPrefer, idEveryBreak: b.idEveryBreak, idAtTop: b.idAtTop, name: b.name || undefined, programCategories: toList(b.programCategories), programSeries: toList(b.programSeries), save };
    const r = await pp.POST("/programming/recipes/ad-break", body); P.adbPreview = r; if (save) { await loadProgramming(); pp.reloadRecipes && pp.reloadRecipes(); P.apply.recipeId = r.id; pp.toast("Recipe saved: " + r.name, "ok"); } else pp.render(); },
  async progApply(kind, apply) {
    const ch = pp.S.active; if (!ch) return;
    const body = applyBody(kind === "weighted" ? { oneProgramPerHour: P.apply.onePer !== false, seed: P.apply.seed || 1, apply } : { recipeId: P.apply.recipeId || "default", apply });
    const r = await pp.POST("/schedule/" + ch + "/" + (kind === "weighted" ? "weighted-fill" : "hour-recipe"), body);
    P.preview = r; pp.render();
    if (apply) { const s = r.summary || {}; if (!s.targets) pp.toast(s.note || "No hours in scope: nothing written", "err"); else pp.toast((s.written || 0) + " hour(s) written", s.written ? "ok" : "err"); }
  },
  progClear() { const ch = pp.S.active; if (!ch) return; pp.confirmDo("Clear the chosen scope on " + ch + " (locked items stay)?", async () => { const r = await pp.POST("/schedule/" + ch + "/clear", applyBody({ keepLocks: true })); pp.toast((r.cleared || 0) + " hour(s) cleared" + (r.note ? ": " + r.note : ""), r.note ? "err" : "ok"); }); },
  progBucketSet(i, k, v) { const b = P.weights[i]; if (k === "pct" || k === "maxPerHour") b[k] = parseFloat(v) || 0; else if (["types", "categories", "subcategories"].includes(k)) b[k] = toList(v); else b[k] = v; pp.render(); },
  progBucketAdd() { P.weights.push({ label: "New", types: ["promo"], categories: [], subcategories: [], series: "", pct: 5 }); pp.render(); },
  progBucketDel(i) { P.weights.splice(i, 1); pp.render(); },
  progBucketMove(i, d) { const a = P.weights, j = i + d; if (j < 0 || j >= a.length) return; [a[i], a[j]] = [a[j], a[i]]; pp.render(); },
  async progWeightsSave() { const r = await pp.PUT("/channels/" + pp.S.active + "/weights", { buckets: P.weights }); P.weights = r.buckets || []; P.weightsDefault = false; pp.toast("Program mix saved", "ok"); pp.render(); },
  async progAutofillSave() { const a = Object.assign({}, P.autofill); if (typeof a.order === "string") a.order = toList(a.order); const r = await pp.PUT("/channels/" + pp.S.active + "/autofill", a); P.autofill = r; pp.toast("Autofill policy saved", "ok"); pp.render(); },
  async progReroll() { await pp.POST("/channels/" + pp.S.active + "/reroll", {}); pp.toast("Blocks re-rolled", "ok"); },
  progContractSet(i, k, v) { const c = P.contracts[i]; if (k === "spotsTotal" || k === "spotsPerDay") c[k] = parseInt(v) || 0; else if (k === "mediaIds" || k === "channels") c[k] = toList(v); else c[k] = v; },
  progContractAdd() { P.contracts.push({ id: "c" + Date.now().toString(36), client: "", title: "", mediaIds: [], spotsTotal: 0, spotsPerDay: 0, startDate: new Date().toISOString().slice(0, 10), endDate: "", channels: [], status: "active" }); pp.render(); },
  progContractDel(i) { P.contracts.splice(i, 1); pp.render(); },
  progContractReport(id) { const token = pp.S.token ? "&token=" + encodeURIComponent(pp.S.token) : ""; window.open("/reports/contracts.html?id=" + encodeURIComponent(id) + token, "_blank"); },
  async progContractsSave() { await pp.PUT("/traffic/contracts", { contracts: P.contracts }); await loadProgramming(); pp.toast("Contracts saved", "ok"); },
  async progTest() { const t = P.tester; const body = { channel: pp.S.active, n: 60 }; if (t.folder.trim()) body.folder = { folder: t.folder.trim(), targetSec: (t.minutes || 0) * 60 }; else body.block = { types: toList(t.types), targetSec: (t.minutes || 0) * 60, prefer: t.prefer, categories: toList(t.categories), subcategories: toList(t.subcategories), series: toList(t.series) };
    P.testerOut = await pp.POST("/programming/query/preview", body); pp.render(); },
  progReload() { loadProgramming(); },
};
