/* PLAY PLAY web dashboard: shared context-menu system.
 *
 * One menu engine for every surface, so a right-click always lands on something useful:
 *   - ctxMenu(ev, items)         explicit menus (items: {label, fn, hint, danger, disabled, checked, sep, header, sub:[...]})
 *   - registerCtx(kind, builder) menus for elements tagged  data-ctx="kind" data-ctx-id="..." [data-ctx-json='{...}']
 *   - automatic fallbacks (document level, only when nothing more specific handled the event):
 *       text fields   Undo / Cut / Copy / Paste / Delete / Select all / Clear
 *       selections    Copy selection, search the library for it
 *       tagged items  the registered builder + Copy id / Copy JSON / Properties
 *       table rows    Copy row / cell / column / table (TSV, JSON, CSV), Properties
 *       links, images Open / Copy address
 *       cards/panels  Copy text, Copy as JSON, Refresh view
 *       anywhere      Text size, Copy page link, Go to view
 *   - showProps(title, obj)      Properties dialog (flattened table + JSON, Copy / Copy JSON)
 *   - keyboard (Menu key, Shift+F10) and touch long-press (550 ms) open the same menus.
 *
 * Geometry under the page zoom (document.documentElement.style.zoom): pointer coordinates are viewport pixels,
 * CSS positions are multiplied by the zoom, bounding rectangles are viewport pixels. Menus are therefore placed at
 * pointer / zoom and clamped with viewport-pixel rectangles (measured in Chrome 155: clientX 800 at zoom 2 puts a
 * fixed element with left:400px exactly under the pointer).
 */

const esc = s => String(s ?? "").replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
const registry = new Map();
let openMenu = null;           // { root, items, closers }
let lastCtxEv = null;

export const getZoom = () => { const z = parseFloat(document.documentElement.style.zoom || getComputedStyle(document.documentElement).zoom || "1"); return z > 0 ? z : 1; };
export function setZoom(z) { z = Math.max(0.7, Math.min(3, z)); document.documentElement.style.zoom = z; try { localStorage.setItem("pp.zoom", String(z)); } catch {} const p = document.querySelector("#zoomPct"); if (p) p.textContent = Math.round(z * 100) + "%"; }
const ZSTEPS = [0.85, 1, 1.15, 1.3, 1.5, 1.75, 2, 2.5, 3];
export function stepZoom(d) { const cur = getZoom(); let i = 0, best = 1e9; ZSTEPS.forEach((z, k) => { const dd = Math.abs(z - cur); if (dd < best) { best = dd; i = k; } }); setZoom(ZSTEPS[Math.max(0, Math.min(ZSTEPS.length - 1, i + d))]); }

// ------------------------------------------------------------------ clipboard
export function toast(msg, kind) { try { if (window.pp && window.pp.toast) window.pp.toast(msg, kind); } catch {} }
export async function copyText(text, what) {
  text = String(text ?? "");
  try { await navigator.clipboard.writeText(text); }
  catch { const ta = document.createElement("textarea"); ta.value = text; ta.style.cssText = "position:fixed;left:-9999px;top:0"; document.body.appendChild(ta); ta.select(); try { document.execCommand("copy"); } catch {} ta.remove(); }
  toast("Copied " + (what || (text.length > 48 ? text.slice(0, 48) + "…" : text)), "ok");
}
export const copyJson = (obj, what) => copyText(JSON.stringify(obj, null, 2), what || "JSON");
export async function readClipboard() {
  try { return await navigator.clipboard.readText(); } catch { const v = prompt("Paste here (the browser blocked clipboard reads):", ""); return v === null ? "" : v; }
}
export async function pasteInto(el) {
  const t = await readClipboard(); if (!t || !el) return;
  el.focus();
  if (typeof el.setRangeText === "function" && el.selectionStart != null) { el.setRangeText(t, el.selectionStart, el.selectionEnd, "end"); el.dispatchEvent(new Event("input", { bubbles: true })); el.dispatchEvent(new Event("change", { bubbles: true })); }
  else document.execCommand("insertText", false, t);
}

// ------------------------------------------------------------------ the menu engine
const CSS = `
.ppctx, .ppctx * { box-sizing:border-box }
.ppctx { position:fixed; z-index:100000; min-width:210px; max-width:420px; max-height:calc((100vh - 16px) / var(--ppz,1)); overflow:auto; background:var(--bg3,#141420); color:var(--t0,#f0f0f8); border:1px solid var(--b2,#3f3f60); border-radius:9px; padding:4px; box-shadow:0 14px 40px rgba(0,0,0,.65); font:12px/1.3 var(--sans, system-ui, sans-serif); outline:none }
.ppctx .it { position:relative; display:flex; align-items:center; gap:8px; padding:6px 10px; border-radius:6px; cursor:pointer; white-space:nowrap; user-select:none }
.ppctx .it .ck { width:12px; color:var(--accent,#00d4ff); text-align:center }
.ppctx .it .lb { flex:1; overflow:hidden; text-overflow:ellipsis }
.ppctx .it small { color:var(--t3,#505068); margin-left:14px; font-size:10px }
.ppctx .it b { color:var(--t2,#707088); margin-left:10px }
.ppctx .it:hover, .ppctx .it.on { background:var(--bg5,#22223a) }
.ppctx .it.danger { color:var(--onair,#ff3b30) }
.ppctx .it.dis { color:var(--t3,#505068); cursor:default } .ppctx .it.dis:hover { background:transparent }
.ppctx .sep { height:1px; background:var(--b1,#2a2a40); margin:4px 2px }
.ppctx .hd { padding:5px 10px 3px; font-size:10px; letter-spacing:1px; text-transform:uppercase; color:var(--t2,#707088); font-weight:700; cursor:default }
.ppctx .sub { display:none; position:absolute; left:100%; top:-5px; margin-left:3px }
.ppctx .it.open > .sub { display:block }
.ppprops::backdrop { background:rgba(0,0,0,.6) }
.ppprops, .ppprops * { box-sizing:border-box }
.ppprops { background:var(--bg3,#141420); color:var(--t0,#f0f0f8); border:1px solid var(--b2,#3f3f60); border-radius:12px; padding:0; width:min(760px, 92vw); max-height:86vh }
.ppprops .hd { display:flex; align-items:center; gap:8px; padding:12px 16px; border-bottom:1px solid var(--b1,#2a2a40); font-weight:700 }
.ppprops .body { padding:12px 16px; overflow:auto; max-height:62vh }
.ppprops table { width:100%; border-collapse:collapse; font-size:12px } .ppprops td { padding:3px 8px; border-top:1px solid var(--b0,#1f1f35); vertical-align:top } .ppprops td:first-child { color:var(--t2,#707088); white-space:nowrap; width:34% }
.ppprops td:last-child { font-family:var(--mono, Consolas, monospace); word-break:break-all }
.ppprops pre { margin:10px 0 0; padding:10px; background:var(--bg1,#0a0a12); border:1px solid var(--b0,#1f1f35); border-radius:8px; max-height:220px; overflow:auto; font:11px/1.4 var(--mono, Consolas, monospace) }
.ppprops .ft { display:flex; gap:8px; justify-content:flex-end; padding:10px 16px; border-top:1px solid var(--b1,#2a2a40) }
`;
function ensureStyle() { if (document.getElementById("ppctxStyle")) return; const s = document.createElement("style"); s.id = "ppctxStyle"; s.textContent = CSS; document.head.appendChild(s); }

export function closeCtx() {
  if (!openMenu) return;
  const { root, closers } = openMenu; openMenu = null;
  for (const c of closers) c();
  root.remove();
}

function visibleItems(items) {   // drop falsy entries and collapse duplicate/leading/trailing separators
  const out = [];
  for (const it of items || []) { if (!it) continue; if (it.sep) { if (out.length && !out[out.length - 1].sep) out.push(it); continue; } out.push(it); }
  while (out.length && out[out.length - 1].sep) out.pop();
  return out;
}

function build(items, into, level) {
  const list = visibleItems(items);
  for (const it of list) {
    if (it.sep) { const d = document.createElement("div"); d.className = "sep"; into.appendChild(d); continue; }
    if (it.header) { const d = document.createElement("div"); d.className = "hd"; d.textContent = it.label; into.appendChild(d); continue; }
    const d = document.createElement("div");
    d.className = "it" + (it.danger ? " danger" : "") + (it.disabled ? " dis" : "");
    d.setAttribute("role", "menuitem"); if (it.title) d.title = it.title;
    d.innerHTML = `<span class="ck">${it.checked ? "&#10003;" : ""}</span><span class="lb">${esc(it.label)}</span>${it.hint ? `<small>${esc(it.hint)}</small>` : ""}${it.sub ? "<b>&#9656;</b>" : ""}`;
    if (it.sub) {
      const sub = document.createElement("div"); sub.className = "ppctx sub"; sub.style.position = "absolute"; sub.style.maxHeight = "70vh";
      build(it.sub, sub, level + 1); d.appendChild(sub);
      d.addEventListener("mouseenter", () => { openSub(d, sub); });
      d.addEventListener("mouseleave", () => d.classList.remove("open"));
      d.addEventListener("click", e => { e.stopPropagation(); openSub(d, sub); });
    } else if (!it.disabled) {
      d.addEventListener("click", e => { e.stopPropagation(); closeCtx(); try { const r = it.fn && it.fn(); if (r && r.catch) r.catch(err => toast(String(err && err.message || err), "err")); } catch (err) { toast(String(err && err.message || err), "err"); } });
    }
    d._spec = it; into.appendChild(d);
  }
}
function openSub(d, sub) {
  for (const s of d.parentElement.querySelectorAll(":scope > .it.open")) if (s !== d) s.classList.remove("open");
  d.classList.add("open");
  sub.style.left = "100%"; sub.style.right = "auto"; sub.style.marginLeft = "3px";
  const r = sub.getBoundingClientRect();
  if (r.right > window.innerWidth - 4) { sub.style.left = "auto"; sub.style.right = "100%"; sub.style.marginLeft = "0"; sub.style.marginRight = "3px"; }
  const r2 = sub.getBoundingClientRect(); if (r2.bottom > window.innerHeight - 4) sub.style.top = Math.max(-4 - (r2.bottom - window.innerHeight + 8) / getZoom(), -(r2.height / getZoom()) + 20) + "px";
}

export function ctxMenu(ev, items, opts = {}) {
  if (ev && ev.preventDefault) { ev.preventDefault(); ev.stopPropagation && ev.stopPropagation(); }
  ensureStyle(); closeCtx();
  const list = visibleItems(items); if (!list.length) return;
  const root = document.createElement("div"); root.className = "ppctx"; root.tabIndex = -1; root.setAttribute("role", "menu");
  const z = getZoom(); root.style.setProperty("--ppz", z);
  build(list, root, 0); document.body.appendChild(root);
  // pointer in viewport px (Menu key / Shift+F10 deliver 0,0: use the focused element)
  let px = ev && ev.clientX, py = ev && ev.clientY;
  if ((!px && !py) && ev && ev.target && ev.target.getBoundingClientRect) { const r = ev.target.getBoundingClientRect(); px = r.left + Math.min(r.width, 24); py = r.bottom; }
  if (px == null) { px = 20; py = 20; }
  root.style.left = px / z + "px"; root.style.top = py / z + "px";
  const r = root.getBoundingClientRect();
  if (r.right > window.innerWidth - 6) root.style.left = Math.max(2, (window.innerWidth - 6 - r.width) / z) + "px";
  if (r.bottom > window.innerHeight - 6) root.style.top = Math.max(2, (window.innerHeight - 6 - r.height) / z) + "px";
  // close on outside click / scroll / resize / blur / Escape; arrow-key navigation
  const closers = [];
  const on = (t, e, f, o) => { t.addEventListener(e, f, o); closers.push(() => t.removeEventListener(e, f, o)); };
  on(document, "mousedown", e => { if (!root.contains(e.target)) closeCtx(); }, true);
  on(document, "contextmenu", e => { if (!root.contains(e.target)) closeCtx(); }, true);
  on(window, "blur", closeCtx); on(window, "resize", closeCtx); on(window, "wheel", e => { if (!root.contains(e.target)) closeCtx(); }, { passive: true });
  on(document, "keydown", e => {
    if (e.key === "Escape") { e.preventDefault(); closeCtx(); return; }
    const level = root.querySelector(".it.open > .sub") || root;
    const items = [...level.querySelectorAll(":scope > .it:not(.dis)")]; if (!items.length) return;
    let i = items.findIndex(x => x.classList.contains("on"));
    if (e.key === "ArrowDown") { e.preventDefault(); i = (i + 1) % items.length; }
    else if (e.key === "ArrowUp") { e.preventDefault(); i = (i - 1 + items.length) % items.length; }
    else if (e.key === "ArrowRight" && i >= 0 && items[i]._spec && items[i]._spec.sub) { e.preventDefault(); openSub(items[i], items[i].querySelector(":scope > .sub")); return; }
    else if (e.key === "ArrowLeft" && level !== root) { e.preventDefault(); level.parentElement.classList.remove("open"); return; }
    else if ((e.key === "Enter" || e.key === " ") && i >= 0) { e.preventDefault(); items[i].click(); return; }
    else if (e.key.length === 1 && /\S/.test(e.key)) { const n = items.findIndex((x, k) => k > i && x.textContent.trim().toLowerCase().startsWith(e.key.toLowerCase())); if (n >= 0) i = n; else return; }
    else return;
    items.forEach(x => x.classList.remove("on")); items[i].classList.add("on"); items[i].scrollIntoView({ block: "nearest" });
  }, true);
  openMenu = { root, items: list, closers };
  root.focus({ preventScroll: true });
  lastCtxEv = ev;
}

// ------------------------------------------------------------------ properties dialog
function flatten(v, prefix = "", out = []) {
  if (v === null || v === undefined) { out.push([prefix || "(value)", String(v)]); return out; }
  if (Array.isArray(v)) { if (!v.length) out.push([prefix, "[]"]); else if (v.every(x => x === null || typeof x !== "object")) out.push([prefix, v.join(", ")]); else v.forEach((x, i) => flatten(x, prefix + "[" + i + "]", out)); return out; }
  if (typeof v === "object") { const ks = Object.keys(v); if (!ks.length) out.push([prefix, "{}"]); for (const k of ks) flatten(v[k], prefix ? prefix + "." + k : k, out); return out; }
  out.push([prefix || "(value)", String(v)]); return out;
}
export function showProps(title, obj, opts = {}) {
  ensureStyle();
  let d = document.getElementById("ppProps"); if (d) d.remove();
  d = document.createElement("dialog"); d.id = "ppProps"; d.className = "ppprops";
  const rows = flatten(obj).slice(0, 400);
  const json = JSON.stringify(obj, null, 2);
  d.innerHTML = `<div class="hd"><span style="flex:1">${esc(title || "Properties")}</span><button class="ghost small" data-act="x">&#10005;</button></div>
    <div class="body">${opts.note ? `<p class="muted" style="margin:0 0 8px;font-size:11px">${esc(opts.note)}</p>` : ""}<table>${rows.map(([k, v]) => `<tr><td>${esc(k)}</td><td>${esc(v.length > 600 ? v.slice(0, 600) + "…" : v)}</td></tr>`).join("") || "<tr><td>(empty)</td><td></td></tr>"}</table>${rows.length >= 400 ? '<p class="muted" style="font-size:10px">first 400 properties shown; Copy JSON has everything</p>' : ""}<pre>${esc(json.length > 20000 ? json.slice(0, 20000) + "\n…" : json)}</pre></div>
    <div class="ft">${(opts.actions || []).map((a, i) => `<button class="small" data-act="a${i}">${esc(a.label)}</button>`).join("")}<button class="small" data-act="copy">Copy JSON</button><button class="small primary" data-act="x">Close</button></div>`;
  d.addEventListener("click", e => { const b = e.target.closest("[data-act]"); if (!b) { if (e.target === d) d.close(); return; } const a = b.dataset.act; if (a === "x") d.close(); else if (a === "copy") copyText(json, "properties JSON"); else if (a[0] === "a") { const act = (opts.actions || [])[+a.slice(1)]; if (act) { d.close(); act.fn(); } } });
  d.addEventListener("close", () => d.remove());
  document.body.appendChild(d); d.showModal();
}

// ------------------------------------------------------------------ registered kinds
export function registerCtx(kind, builder) { registry.set(kind, builder); }
const dataOf = el => { try { return el.dataset.ctxJson ? JSON.parse(el.dataset.ctxJson) : null; } catch { return null; } };

// ------------------------------------------------------------------ generic builders
function editItems(el) {
  const ro = el.readOnly || el.disabled, hasSel = el.selectionStart != null && el.selectionEnd !== el.selectionStart, pw = el.type === "password";
  return [
    { header: true, label: "Text" },
    { label: "Undo", hint: "Ctrl+Z", disabled: ro, fn: () => { el.focus(); document.execCommand("undo"); } },
    { label: "Redo", hint: "Ctrl+Y", disabled: ro, fn: () => { el.focus(); document.execCommand("redo"); } }, { sep: true },
    { label: "Cut", hint: "Ctrl+X", disabled: ro || !hasSel || pw, fn: () => { el.focus(); document.execCommand("cut"); } },
    { label: "Copy", hint: "Ctrl+C", disabled: !hasSel || pw, fn: () => { el.focus(); document.execCommand("copy"); } },
    { label: "Paste", hint: "Ctrl+V", disabled: ro, fn: () => pasteInto(el) },
    { label: "Paste as plain line", disabled: ro, fn: async () => { const t = (await readClipboard()).replace(/\s*[\r\n]+\s*/g, " ").trim(); if (t) { el.focus(); if (el.setRangeText && el.selectionStart != null) { el.setRangeText(t, el.selectionStart, el.selectionEnd, "end"); el.dispatchEvent(new Event("input", { bubbles: true })); el.dispatchEvent(new Event("change", { bubbles: true })); } } } },
    { label: "Delete", disabled: ro || !hasSel, fn: () => { el.focus(); document.execCommand("delete"); } }, { sep: true },
    { label: "Select all", hint: "Ctrl+A", fn: () => { el.focus(); el.select && el.select(); } },
    { label: "Copy all", disabled: pw || !String(el.value || "").length, fn: () => copyText(el.value, "field text") },
    { label: "Clear field", danger: true, disabled: ro || !String(el.value || "").length, fn: () => { el.focus(); el.value = ""; el.dispatchEvent(new Event("input", { bubbles: true })); el.dispatchEvent(new Event("change", { bubbles: true })); } },
  ];
}

function tableInfo(td) {
  const tr = td.closest("tr"), table = td.closest("table"); if (!tr || !table) return null;
  const cells = [...tr.children]; const idx = td.closest("td,th") ? cells.indexOf(td.closest("td,th")) : -1;
  const headers = [...table.querySelectorAll("thead th")].map(h => h.textContent.trim());
  const txt = c => c.textContent.replace(/\s+/g, " ").trim();
  const row = cells.map(txt);
  return { tr, table, idx, headers, row, cell: idx >= 0 ? row[idx] : "", isHead: !!tr.closest("thead") };
}
const tsv = rows => rows.map(r => r.map(c => String(c).replace(/[\t\r\n]+/g, " ")).join("\t")).join("\n");
const csv = rows => rows.map(r => r.map(c => '"' + String(c).replace(/"/g, '""') + '"').join(",")).join("\r\n");
function tableRows(table) { return [...table.querySelectorAll("tr")].map(tr => [...tr.children].map(c => c.textContent.replace(/\s+/g, " ").trim())); }
function download(name, text, mime) { const a = document.createElement("a"); a.href = URL.createObjectURL(new Blob([text], { type: mime })); a.download = name; a.click(); setTimeout(() => URL.revokeObjectURL(a.href), 2000); }

function tableItems(ti) {
  const obj = {}; ti.row.forEach((v, i) => { obj[ti.headers[i] || "col" + (i + 1)] = v; });
  const rows = tableRows(ti.table);
  return [
    { header: true, label: "Row" },
    { label: "Copy cell", disabled: !ti.cell, fn: () => copyText(ti.cell) },
    { label: "Copy row", fn: () => copyText(tsv([ti.row]), "row") },
    { label: "Copy row as JSON", fn: () => copyJson(obj, "row JSON") },
    { label: "Copy column", disabled: ti.idx < 0, fn: () => copyText(ti.table.querySelectorAll("tbody tr").length ? [...ti.table.querySelectorAll("tbody tr")].map(r => (r.children[ti.idx] ? r.children[ti.idx].textContent.replace(/\s+/g, " ").trim() : "")).join("\n") : "", "column") },
    { sep: true },
    { header: true, label: "Table" },
    { label: "Copy table (tab separated)", fn: () => copyText(tsv(rows), "table") },
    { label: "Copy table as CSV", fn: () => copyText(csv(rows), "table CSV") },
    { label: "Download table as CSV", fn: () => download("table.csv", "﻿" + csv(rows), "text/csv") },
    { label: "Select this row", fn: () => { const r = document.createRange(); r.selectNodeContents(ti.tr); const s = getSelection(); s.removeAllRanges(); s.addRange(r); } },
    { sep: true },
    { label: "Properties…", fn: () => showProps("Row properties", obj) },
  ];
}

const panelOf = el => el.closest(".card, .panel, .stat, .mcard");
function panelItems(p) {
  const title = (p.querySelector("h3, .hd .name, .name, .t") || {}).textContent;
  return [
    { header: true, label: (title || "Panel").trim().slice(0, 40) },
    { label: "Copy panel text", fn: () => copyText(p.innerText.trim(), "panel text") },
    { label: "Copy panel as JSON", fn: () => { const o = {}; p.querySelectorAll("input,select,textarea").forEach((e, i) => { o[e.id || e.name || e.getAttribute("placeholder") || "field" + (i + 1)] = e.type === "checkbox" ? e.checked : e.value; }); copyJson({ title: (title || "").trim(), fields: o, text: p.innerText.trim().slice(0, 4000) }, "panel JSON"); } },
    { label: "Properties…", fn: () => showProps(title || "Panel", { title: (title || "").trim(), text: p.innerText.trim().slice(0, 2000), inputs: [...p.querySelectorAll("input,select,textarea")].map(e => ({ id: e.id, name: e.name, type: e.type, value: e.type === "checkbox" ? e.checked : e.value })) }) },
  ];
}

function viewItems() {
  const views = [...document.querySelectorAll("#tabs button[data-view]")];
  const cur = (location.hash || "").replace("#", "");
  return [
    { header: true, label: "View" },
    { label: "Refresh view", hint: "F5", fn: () => { try { window.pp && window.pp.nav ? window.pp.nav(cur || "channels") : location.reload(); } catch { location.reload(); } } },
    { label: "Copy page link", fn: () => copyText(location.href, "page link") },
    { label: "Text size", sub: [{ label: "Bigger", hint: "Ctrl+=", fn: () => stepZoom(1) }, { label: "Smaller", hint: "Ctrl+-", fn: () => stepZoom(-1) }, { label: "Reset (100%)", hint: "Ctrl+0", fn: () => setZoom(1) }, { sep: true }, ...ZSTEPS.map(z => ({ label: Math.round(z * 100) + "%", checked: Math.abs(getZoom() - z) < 0.01, fn: () => setZoom(z) }))] },
    views.length ? { label: "Go to", sub: views.map(b => ({ label: b.textContent.trim(), checked: b.classList.contains("active"), fn: () => b.click() })) } : null,
    { label: "Set API token…", fn: () => { const b = document.getElementById("tokenBtn"); if (b) b.click(); } },
  ];
}

function selectionText() { const s = getSelection(); return s ? s.toString().trim() : ""; }

// ------------------------------------------------------------------ the document-level fallback
function resolveMenu(ev) {
  const t = ev.target instanceof Element ? ev.target : (ev.target && ev.target.parentElement); if (!t) return null;
  const out = [];
  // 1. text fields keep a real edit menu
  const f = t.closest("input, textarea, [contenteditable=''], [contenteditable='true']");
  if (f && !(f.tagName === "INPUT" && ["checkbox", "radio", "button", "submit", "range", "color", "file", "date", "time", "datetime-local"].includes(f.type))) {
    out.push(...editItems(f));
    const tagged = f.closest("[data-ctx]"); if (tagged && registry.has(tagged.dataset.ctx)) out.push({ sep: true }, ...(registry.get(tagged.dataset.ctx)({ id: tagged.dataset.ctxId, el: tagged, ev, data: dataOf(tagged) }) || []));
    return out;
  }
  if (f && f.tagName === "SELECT") { out.push({ header: true, label: "Choice" }, { label: "Copy selected value", fn: () => copyText(f.options[f.selectedIndex] ? f.options[f.selectedIndex].text : f.value) }, { label: "Copy all options", fn: () => copyText([...f.options].map(o => o.text).join("\n"), "options") }); }
  // 2. a text selection
  const sel = selectionText();
  if (sel) { out.push({ header: true, label: "Selection" }, { label: "Copy", hint: "Ctrl+C", fn: () => copyText(sel) }, { label: "Copy as one line", fn: () => copyText(sel.replace(/\s+/g, " ")) }, { label: "Search the library for it", fn: () => { try { window.pp.nav("library"); setTimeout(() => { const q = document.querySelector("#q, input[type=search], input[placeholder*=earch]"); if (q) { q.value = sel; q.dispatchEvent(new Event("input", { bubbles: true })); q.dispatchEvent(new Event("change", { bubbles: true })); } }, 400); } catch {} } }); }
  // 3. tagged elements: the module's own builder first
  const tagged = t.closest("[data-ctx]");
  if (tagged) {
    const b = registry.get(tagged.dataset.ctx);
    const items = b ? b({ id: tagged.dataset.ctxId, el: tagged, ev, data: dataOf(tagged) }) : null;
    if (items && items.length) { if (out.length) out.push({ sep: true }); out.push(...items); }
    const id = tagged.dataset.ctxId, data = dataOf(tagged);
    const std = [];
    if (id) std.push({ label: "Copy id", fn: () => copyText(id, "id") });
    if (data) std.push({ label: "Copy as JSON", fn: () => copyJson(data) }, { label: "Properties…", fn: () => showProps((tagged.dataset.ctxTitle || tagged.dataset.ctx) + " properties", data) });
    if (std.length) { out.push({ sep: true }, ...std); }
  }
  // 4. table rows
  const ti = tableInfo(t); if (ti) { if (out.length) out.push({ sep: true }); out.push(...tableItems(ti)); }
  // 5. links and images
  const a = t.closest("a[href]"); if (a) { out.push({ sep: true }, { label: "Open link", fn: () => window.open(a.href, "_blank") }, { label: "Copy link address", fn: () => copyText(a.href, "link") }); }
  const img = t.closest("img"); if (img && img.src) { out.push({ sep: true }, { label: "Open image", fn: () => window.open(img.src, "_blank") }, { label: "Copy image address", fn: () => copyText(img.src, "image address") }); }
  // 6. panels / cards
  const p = panelOf(t); if (p && !ti && !tagged) { if (out.length) out.push({ sep: true }); out.push(...panelItems(p)); }
  // 7. always: view-level
  if (out.length) out.push({ sep: true }); out.push(...viewItems());
  return out;
}

let started = false;
export function initCtx() {
  if (started) return; started = true; ensureStyle();
  document.addEventListener("contextmenu", ev => {
    if (ev.defaultPrevented) return;                       // a specific handler already took it
    if (ev.shiftKey && ev.ctrlKey) return;                 // Ctrl+Shift+right-click: the browser's own menu (inspect)
    if (ev.target && ev.target.closest && ev.target.closest(".ppctx, dialog")) { if (!ev.target.closest("input, textarea")) { ev.preventDefault(); } if (ev.target.closest(".ppctx")) return; }
    const items = resolveMenu(ev); if (!items || !items.length) return;
    ctxMenu(ev, items);
  });
  // touch: press-and-hold opens the same menu (OBS docks and tablets have no right button)
  let tm = null, tp = null;
  document.addEventListener("touchstart", e => { if (e.touches.length !== 1) return; const t = e.touches[0]; tp = { x: t.clientX, y: t.clientY, target: e.target }; clearTimeout(tm); tm = setTimeout(() => { if (!tp) return; tp.target.dispatchEvent(new MouseEvent("contextmenu", { bubbles: true, cancelable: true, clientX: tp.x, clientY: tp.y })); tp = null; }, 550); }, { passive: true });
  for (const n of ["touchend", "touchcancel", "touchmove"]) document.addEventListener(n, () => { clearTimeout(tm); tp = null; }, { passive: true });
  window.ppCtx = { ctxMenu, closeCtx, registerCtx, showProps, copyText, copyJson, readClipboard, pasteInto, getZoom, setZoom, stepZoom };
}
initCtx();
