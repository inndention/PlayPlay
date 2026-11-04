/* CG Designer (docs/cg-designer.md): AI generator (full prompt + optional examples) and WYSIWYG editor for CG templates.

   Model: D.doc is the SOURCE page, parsed with DOMParser (its scripts never run). Every editable element carries a
   temporary data-cge id. The preview iframe is rendered from the source (srcdoc + <base href="/cg/">), so the page's
   own scripts run there only; a click in the preview maps back to the source element by its data-cge id. Style and
   text edits go to both copies (smooth dragging, no reload); structural edits re-render the preview (double-buffered,
   no flicker). Save strips the ids, so script-generated DOM never leaks into the saved page. */

const $ = s => document.querySelector(s);
const E = s => String(s ?? "").replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
const W = 1920, H = 1080;
const SAFE = { title: { x: 96, y: 54 }, action: { x: 48, y: 27 } };
const SVG_NS = "http://www.w3.org/2000/svg";
const SKIP = new Set(["SCRIPT", "STYLE", "META", "LINK", "TITLE", "BASE", "NOSCRIPT", "TEMPLATE", "BR"]);
const PX = new Set(["font-size", "letter-spacing", "border-radius", "padding", "width", "height", "left", "top", "right", "bottom", "gap", "margin", "max-width", "min-width", "min-height", "max-height"]);
const ROLES = [["lower_third", "Lower third"], ["bug", "Bug"], ["ticker", "Ticker"], ["secondary", "Secondary / full-screen"], ["legal_id", "Legal ID"], ["emergency", "Emergency"]];
const CATS = ["custom", "lower-thirds", "bugs", "tickers", "banners", "fullscreen", "sports", "weather", "social", "advertising", "news", "election"];
const FONTS = ['"Segoe UI", Arial, sans-serif', 'Bahnschrift, "Segoe UI", sans-serif', '"Arial Black", Arial, sans-serif', 'Impact, "Arial Black", sans-serif', 'Arial, Helvetica, sans-serif', '"Trebuchet MS", Arial, sans-serif', 'Verdana, Tahoma, sans-serif', '"Franklin Gothic Medium", Arial, sans-serif', 'Georgia, "Times New Roman", serif', '"Times New Roman", Georgia, serif', 'Consolas, "Courier New", monospace'];
const KINDS = ["", "lower third", "station bug", "ticker / crawl", "breaking banner", "full-screen", "score bug", "weather", "sponsor bump", "social / QR", "countdown", "election results"];
const IDEAS = [
  ["Reporter lower third", "Lower third for a local news reporter: name and title, navy plate with a thin gold accent line, clean sans-serif, the accent line sweeps across on entry."],
  ["Breaking banner", "Breaking news banner across the lower third: red BREAKING tag on the left, white headline bar with dark text, a subtle shine that passes every few seconds."],
  ["Station bug", "Top-right station bug: logo slot above a small red LIVE pill with a pulsing dot. Compact, sits inside title-safe."],
  ["Score bug", "Sports score bug top-left: two team names with their colours, scores, period and game clock in a compact two-row layout."],
  ["Weather 3-day", "Full-screen 3-day weather forecast: city name, three day columns each with an inline-SVG icon, condition and high/low; every value is a field."],
  ["Sponsor bump", "Bottom-right sponsor bump: 'Brought to you by' line, sponsor name and a logo slot on a frosted dark plate."],
  ["News crawl", "Crawling news ticker along the bottom: a LATEST label on the left and one text field whose items are separated by | and crawl right to left continuously."],
];
const BLANK = `<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<title>New graphic</title>
<style>
:root { --accent: #0b5fff; --plate: #0d1220; --text: #ffffff; }
* { margin: 0; padding: 0; box-sizing: border-box; }
html, body { width: 1920px; height: 1080px; overflow: hidden; background: transparent; font-family: "Segoe UI", Arial, sans-serif; }
.cg-root { position: absolute; inset: 0; }
</style>
</head>
<body>
<div class="cg-root" data-cg-root></div>
<script src="js/cg-animations.js"></script>
<script src="js/cg-fields.js"></script>
</body>
</html>`;

// ------------------------------------------------------------------------------------------------ state
const LS = { get(k, d) { try { const v = localStorage.getItem(k); return v === null ? d : v; } catch { return d; } }, set(k, v) { try { localStorage.setItem(k, v); } catch {} }, del(k) { try { localStorage.removeItem(k); } catch {} } };
const token = new URLSearchParams(location.search).get("token") || LS.get("pp.token", "");
const blankMeta = () => ({ id: null, name: "", description: "", category: "custom", role: "lower_third", prompt: "", examples: [], model: "", versionCount: 0 });
const D = {
  info: null, infoErr: "", manifest: null, channels: [], channel: LS.get("pp.channel", ""), models: [],
  doc: null, meta: blankMeta(), nextId: 1, sel: null, hover: null, editing: null,
  undo: [], redo: [], lastKey: null, lastAt: 0, dirty: false,
  frame: null, seq: 0, shown: 0, afters: [], scale: 0.5, zoom: LS.get("cgd.zoom", "fit"), guides: LS.get("cgd.guides", "1") === "1", snap: LS.get("cgd.snap", "1") === "1",
  view: "design", ltab: "ai", rtab: "element", drag: null, snapLines: [], lastLayer: {}, packNote: false,
  gen: { mode: "new", prompt: LS.get("cgd.prompt", ""), kind: "", count: 1, model: "", brandColors: "", brandFont: "", brandNotes: "", examples: [], job: null, drafts: [], err: "", timer: null },
};
window.CGD = D;

// ------------------------------------------------------------------------------------------------ helpers
async function api(method, path, body) {
  const r = await fetch("/api/v1" + path, { method, headers: { "Content-Type": "application/json", ...(token ? { Authorization: "Bearer " + token } : {}) }, body: body !== undefined ? JSON.stringify(body) : undefined });
  const t = await r.text(); let j = {}; try { j = t ? JSON.parse(t) : {}; } catch { j = { raw: t }; }
  if (!r.ok) { const e = new Error(j.error || ("HTTP " + r.status)); e.status = r.status; e.body = j; throw e; }
  return j;
}
function toast(msg, cls) { const t = $("#toast"); t.textContent = msg; t.className = "show " + (cls || ""); clearTimeout(toast.t); toast.t = setTimeout(() => t.className = "", cls === "err" ? 6000 : 3200); }
const human = n => String(n).replace(/[-_]+/g, " ").replace(/([a-z])([A-Z])/g, "$1 $2").replace(/^./, c => c.toUpperCase());
const isColor = v => /^\s*(#[0-9a-f]{3,8}|rgba?\(|hsla?\()/i.test(String(v));
const r1 = v => Math.round(v);
function toHex(c) {
  c = String(c || "").trim(); if (/^#[0-9a-f]{6}$/i.test(c)) return c.toLowerCase();
  if (/^#[0-9a-f]{3}$/i.test(c)) return "#" + c.slice(1).split("").map(x => x + x).join("").toLowerCase();
  const m = c.match(/rgba?\(\s*(\d+)[\s,]+(\d+)[\s,]+(\d+)/i); if (!m) return "#000000";
  return "#" + [m[1], m[2], m[3]].map(x => (+x).toString(16).padStart(2, "0")).join("");
}
function dlg(title, body, buttons, onOpen) {
  const d = $("#dlg"); $("#dlgH").innerHTML = title; $("#dlgB").innerHTML = body; const f = $("#dlgF"); f.innerHTML = "";
  const close = () => d.close();
  for (const b of buttons || [{ label: "Close" }]) { const el = document.createElement("button"); el.textContent = b.label; if (b.cls) el.className = b.cls; el.onclick = async () => { if (b.fn) { const keep = await b.fn(close); if (keep === true) return; } close(); }; f.appendChild(el); }
  d.showModal(); if (onOpen) onOpen(d); return close;
}
const confirmDlg = (title, text, okLabel, cls) => new Promise(res => { dlg(E(title), `<div>${text}</div>`, [{ label: "Cancel", fn: () => res(false) }, { label: okLabel || "OK", cls: cls || "primary", fn: () => res(true) }]); $("#dlg").addEventListener("close", () => res(false), { once: true }); });
function readFile(file, asText) { return new Promise((res, rej) => { const r = new FileReader(); r.onload = () => res(r.result); r.onerror = rej; asText ? r.readAsText(file) : r.readAsDataURL(file); }); }
async function shrinkImage(file, max = 1600) {   // reference images travel as JPEG data URLs, at most 1600 px wide
  const url = await readFile(file); const img = new Image(); await new Promise((res, rej) => { img.onload = res; img.onerror = rej; img.src = url; });
  const s = Math.min(1, max / Math.max(img.width, img.height)); if (s === 1 && url.length < 900000) return url;
  const c = document.createElement("canvas"); c.width = Math.round(img.width * s); c.height = Math.round(img.height * s); c.getContext("2d").drawImage(img, 0, 0, c.width, c.height);
  return c.toDataURL("image/jpeg", 0.86);
}

// ------------------------------------------------------------------------------------------------ source document
const parse = html => new DOMParser().parseFromString(html, "text/html");
const srcEl = id => id && D.doc ? D.doc.querySelector(`[data-cge="${id}"]`) : null;
const frameDoc = () => { try { return D.frame && D.frame.contentDocument; } catch { return null; } };
const fel = id => { const d = frameDoc(); return id && d ? d.querySelector(`[data-cge="${id}"]`) : null; };
const editable = el => !SKIP.has(el.tagName) && !(el.namespaceURI === SVG_NS && el.localName !== "svg");
function assignIds() { D.doc.body.querySelectorAll("*").forEach(el => { if (editable(el) && !el.hasAttribute("data-cge")) el.setAttribute("data-cge", String(D.nextId++)); }); }
function stripEditor(doc) {
  doc.querySelectorAll("[data-cge]").forEach(el => el.removeAttribute("data-cge"));
  doc.querySelectorAll("[contenteditable]").forEach(el => el.removeAttribute("contenteditable"));
  doc.querySelectorAll("base[data-cge-base], style[data-cge-style]").forEach(el => el.remove());
}
function serialize(clean) {
  let h = "<!DOCTYPE html>\n" + D.doc.documentElement.outerHTML;
  if (clean) h = h.replace(/\sdata-cge="\d+"/g, "");
  return h;
}
const EDITOR_CSS = `<base href="/cg/" data-cge-base><style data-cge-style>img[data-field]:not([src]),img[data-field][src=""]{visibility:visible!important;min-width:48px;min-height:48px;outline:2px dashed rgba(170,170,210,.85);outline-offset:-2px;background:rgba(120,120,160,.18)}</style>`;
const withBase = html => /<head[^>]*>/i.test(html) ? html.replace(/<head([^>]*)>/i, m => m + EDITOR_CSS) : EDITOR_CSS + html;
function loadDoc(html, meta, opts = {}) {
  D.doc = parse(html); stripEditor(D.doc); D.nextId = 1; assignIds();
  if (!opts.keepUndo) { D.undo = []; D.redo = []; }
  D.sel = null; D.hover = null; D.editing = null;
  if (meta) D.meta = Object.assign(blankMeta(), meta);
  $("#tplName").value = D.meta.name || "";
  setDirty(!!opts.dirty); render(); refreshAll();
}
function restoreSnap(html) {
  D.doc = parse(html); let mx = 0; D.doc.querySelectorAll("[data-cge]").forEach(el => mx = Math.max(mx, +el.getAttribute("data-cge") || 0)); D.nextId = mx + 1; assignIds();
  if (D.sel && !srcEl(D.sel)) D.sel = null;
  setDirty(true); render(); refreshAll();
}
/** Every change to the source goes through here: one undo step per change (inspector typing on one property within
 *  1.2 s counts as one step), then the preview is updated in place or re-rendered. */
function mutate(fn, o = {}) {
  const now = Date.now();
  if (!(o.key && o.key === D.lastKey && now - D.lastAt < 1200)) { D.undo.push(serialize(false)); if (D.undo.length > 120) D.undo.shift(); D.redo = []; }
  D.lastKey = o.key || null; D.lastAt = now;
  fn(); setDirty(true);
  if (o.rerender) render(o.after); else drawOverlay();
  if (o.inspector === false) { renderLayersIfShown(); updateStatus(); } else refreshAll();
}
function undo() { if (!D.undo.length) return; D.redo.push(serialize(false)); D.lastKey = null; restoreSnap(D.undo.pop()); }
function redo() { if (!D.redo.length) return; D.undo.push(serialize(false)); D.lastKey = null; restoreSnap(D.redo.pop()); }
function replaceDoc(html, metaPatch) {
  D.undo.push(serialize(false)); D.redo = []; D.lastKey = null;
  const doc = parse(html); stripEditor(doc); D.doc = doc; D.nextId = 1; assignIds(); D.sel = null;
  if (metaPatch) Object.assign(D.meta, metaPatch);
  $("#tplName").value = D.meta.name || "";
  setDirty(true); render(); refreshAll();
}
function setDirty(v) {
  D.dirty = v; $("#dirty").textContent = v ? "● unsaved" : (D.meta.id ? "saved" : "");
  document.title = (v ? "● " : "") + (D.meta.name || "Untitled") + " — CG Designer";
  if (v) { clearTimeout(setDirty.t); setDirty.t = setTimeout(() => { try { localStorage.setItem("cgd.draft", JSON.stringify({ html: serialize(true), meta: D.meta, at: Date.now() })); } catch {} }, 1500); }
  else LS.del("cgd.draft");
}

// translate (move) and the source/frame pair
function getT(el) { const p = String(el && el.style.translate || "").trim().split(/\s+/); return [parseFloat(p[0]) || 0, parseFloat(p[1]) || 0]; }
function setT(el, x, y) { if (!el) return; x = r1(x); y = r1(y); if (!x && !y) el.style.removeProperty("translate"); else el.style.translate = `${x}px ${y}px`; }
function setStyleBoth(id, prop, v) { for (const el of [srcEl(id), fel(id)]) { if (!el) continue; if (v === "" || v == null) el.style.removeProperty(prop); else el.style.setProperty(prop, v); } }
const rectOf = id => { const f = fel(id); if (!f) return null; const r = f.getBoundingClientRect(); return { x: r.left, y: r.top, w: r.width, h: r.height }; };
const rootEl = () => D.doc.querySelector("[data-cg-root]");

// ------------------------------------------------------------------------------------------------ preview (double-buffered iframes)
function render(after) {
  if (after) D.afters.push(after);
  const seq = ++D.seq, f = document.createElement("iframe");
  f.className = "pending"; f.setAttribute("scrolling", "no"); f.title = "preview"; f.tabIndex = -1;
  f.style.transform = `scale(${D.scale})`;
  $("#stage").insertBefore(f, $("#overlay"));
  f.addEventListener("load", () => {
    if (seq < D.shown) { f.remove(); return; }
    D.shown = seq; D.frame = f; f.classList.remove("pending");
    document.querySelectorAll("#stage iframe").forEach(x => { if (x !== f && !x.classList.contains("pending")) x.remove(); });
    const d = frameDoc(); if (d) { d.addEventListener("keydown", onKey, true); }
    const a = D.afters.splice(0); setTimeout(() => { a.forEach(fn => { try { fn(); } catch (e) { console.warn(e); } }); drawOverlay(); updateStatus(); }, 30);
  }, { once: true });
  f.srcdoc = withBase(serialize(false));
}
function fit() {
  const wrap = $("#stageWrap"); let s = D.zoom === "fit" ? Math.min((wrap.clientWidth - 32) / W, (wrap.clientHeight - 32) / H) : parseFloat(D.zoom);
  s = Math.max(0.1, Math.min(2, s || 0.5)); D.scale = s;
  const st = $("#stage"); st.style.width = W * s + "px"; st.style.height = H * s + "px";
  document.querySelectorAll("#stage iframe").forEach(f => f.style.transform = `scale(${s})`);
  drawOverlay();
}
function setBackdrop(v) {
  const st = $("#stage"), img = $("#backdrop"); st.className = "bg-" + (["program", "image"].includes(v) ? "black" : v);
  img.style.display = "none"; img.removeAttribute("src");
  if (v === "program") { const ch = D.channel; if (!ch) { toast("Pick a channel first", "err"); return; } img.src = "/preview/" + encodeURIComponent(ch) + ".mjpeg" + (token ? "?token=" + encodeURIComponent(token) : ""); img.style.display = "block"; }
  if (v === "image") { const p = $("#fileImg"); p.onchange = async () => { const f = p.files[0]; p.value = ""; if (!f) return; img.src = await readFile(f); img.style.display = "block"; }; p.click(); }
}

// ------------------------------------------------------------------------------------------------ overlay: selection, guides, drag
function drawOverlay() {
  const ov = $("#overlay"), s = D.scale; if (!ov) return;
  let h = "";
  if (D.guides) {
    const g = (x, y, w, hh, cls) => `<div class="guide ${cls}" style="left:${x * s}px;top:${y * s}px;width:${w * s}px;height:${hh * s}px"></div>`;
    h += g(SAFE.action.x, SAFE.action.y, W - 2 * SAFE.action.x, H - 2 * SAFE.action.y, "safe act") + g(SAFE.title.x, SAFE.title.y, W - 2 * SAFE.title.x, H - 2 * SAFE.title.y, "safe");
    h += `<div class="guide cross" style="left:${W / 2 * s}px;top:0;width:1px;height:100%"></div><div class="guide cross" style="left:0;top:${H / 2 * s}px;width:100%;height:1px"></div>`;
  }
  for (const l of D.snapLines) h += l.v ? `<div class="guide snap" style="left:${l.v * s}px;top:0;width:1px;height:100%"></div>` : `<div class="guide snap" style="left:0;top:${l.h * s}px;width:100%;height:1px"></div>`;
  if (D.hover && D.hover !== D.sel) { const r = rectOf(D.hover); if (r) h += `<div class="hov" style="left:${r.x * s}px;top:${r.y * s}px;width:${r.w * s}px;height:${r.h * s}px"></div>`; }
  if (D.sel) {
    const r = rectOf(D.sel), se = srcEl(D.sel);
    if (r && se) {
      const hs = ["nw", "n", "ne", "e", "se", "s", "sw", "w"].map(k => { const x = k.includes("w") ? 0 : k.includes("e") ? 100 : 50, y = k.includes("n") ? 0 : k.includes("s") ? 100 : 50; return `<div class="h" data-h="${k}" style="left:${x}%;top:${y}%;cursor:${k}-resize"></div>`; }).join("");
      h += `<div class="selbox" style="left:${r.x * s}px;top:${r.y * s}px;width:${r.w * s}px;height:${r.h * s}px"><span class="lbl${r.y * s < 22 ? " below" : ""}">${E(label(se))} · ${r1(r.w)}×${r1(r.h)}</span>${D.editing ? "" : hs}</div>`;
    }
  }
  ov.innerHTML = h;
}
function label(el) {
  if (!el) return "";
  const f = el.getAttribute("data-field"), cls = (el.getAttribute("class") || "").split(/\s+/).filter(Boolean)[0];
  return (el.hasAttribute("data-cg-root") ? "ROOT " : "") + el.tagName.toLowerCase() + (cls ? "." + cls : "") + (f ? ` {${f}}` : "");
}
function toFrame(e) { const r = $("#stage").getBoundingClientRect(); return { x: (e.clientX - r.left) / D.scale, y: (e.clientY - r.top) / D.scale }; }
function depth(el) { let n = 0; while (el && el.parentElement) { n++; el = el.parentElement; } return n; }
function hitTest(x, y) {
  const d = frameDoc(); if (!d) return null;
  let best = null, bestA = Infinity, bestD = -1; const win = d.defaultView;
  d.querySelectorAll("[data-cge]").forEach(el => {
    const r = el.getBoundingClientRect(); if (r.width < 1 || r.height < 1 || x < r.left || x > r.right || y < r.top || y > r.bottom) return;
    const cs = win.getComputedStyle(el); if (cs.display === "none") return;
    const a = r.width * r.height, dp = depth(el);
    if (a < bestA - 4 || (Math.abs(a - bestA) <= 4 && dp > bestD)) { best = el; bestA = a; bestD = dp; }
  });
  // a full-frame root is the canvas itself: clicking empty space deselects (pick it from Layers instead)
  if (best && best.hasAttribute("data-cg-root") && bestA >= 0.9 * W * H) return null;
  return best ? best.getAttribute("data-cge") : null;
}
function select(id) { D.sel = id; if (D.rtab !== "element") D.rtab = "element"; drawOverlay(); refreshAll(); }
function snapAxis(edges, lines, thr) {   // edges: [{pos, off}] (off = distance from the box start); returns [delta, line] or null
  let best = null;
  for (const e of edges) for (const l of lines) { const d = l - e.pos; if (Math.abs(d) <= thr && (!best || Math.abs(d) < Math.abs(best[0]))) best = [d, l]; }
  return best;
}
const LINES_X = [0, SAFE.action.x, SAFE.title.x, W / 2, W - SAFE.title.x, W - SAFE.action.x, W];
const LINES_Y = [0, SAFE.action.y, SAFE.title.y, H / 2, H - SAFE.title.y, H - SAFE.action.y, H];

function onDown(e) {
  if (e.button !== 0 || D.editing) return;
  const p = toFrame(e), hk = e.target.getAttribute && e.target.getAttribute("data-h");
  $("#overlay").focus();
  if (hk && D.sel) { const r = rectOf(D.sel), se = srcEl(D.sel); D.drag = { kind: "resize", h: hk, id: D.sel, sx: e.clientX, sy: e.clientY, r0: r, t0: getT(se), moved: false }; e.preventDefault(); return; }
  let id = hitTest(p.x, p.y);
  if (id && e.altKey) { const se = srcEl(id), par = se && se.parentElement && se.parentElement.closest("[data-cge]"); if (par) id = par.getAttribute("data-cge"); }
  if (!id) { if (D.sel) { D.sel = null; drawOverlay(); refreshAll(); } return; }
  if (id !== D.sel) select(id);
  const se = srcEl(id); D.drag = { kind: "move", id, sx: e.clientX, sy: e.clientY, r0: rectOf(id), t0: getT(se), moved: false };
  e.preventDefault();
}
function onMove(e) {
  const dr = D.drag;
  if (!dr) {
    if (D.view !== "design" || D.editing) return;
    const st = $("#stage").getBoundingClientRect(); if (e.clientX < st.left || e.clientX > st.right || e.clientY < st.top || e.clientY > st.bottom) { if (D.hover) { D.hover = null; drawOverlay(); } $("#stPos").textContent = ""; return; }
    const p = toFrame(e); $("#stPos").textContent = `x ${r1(p.x)}  y ${r1(p.y)}`;
    const id = hitTest(p.x, p.y); if (id !== D.hover) { D.hover = id; drawOverlay(); }
    return;
  }
  let dx = (e.clientX - dr.sx) / D.scale, dy = (e.clientY - dr.sy) / D.scale;
  if (!dr.moved && Math.hypot(dx, dy) * D.scale < 3) return;
  if (!dr.moved) { dr.moved = true; D.undo.push(serialize(false)); D.redo = []; D.lastKey = null; }
  const thr = 7 / D.scale, r = dr.r0, fe = fel(dr.id); D.snapLines = [];
  if (!r || !fe) return;
  if (dr.kind === "move") {
    if (e.shiftKey) { if (Math.abs(dx) > Math.abs(dy)) dy = 0; else dx = 0; }
    if (D.snap && !e.altKey) {
      const sx = snapAxis([{ pos: r.x + dx }, { pos: r.x + dx + r.w / 2 }, { pos: r.x + dx + r.w }], LINES_X, thr); if (sx) { dx += sx[0]; D.snapLines.push({ v: sx[1] }); }
      const sy = snapAxis([{ pos: r.y + dy }, { pos: r.y + dy + r.h / 2 }, { pos: r.y + dy + r.h }], LINES_Y, thr); if (sy) { dy += sy[0]; D.snapLines.push({ h: sy[1] }); }
    }
    setT(fe, dr.t0[0] + dx, dr.t0[1] + dy);
    $("#stPos").textContent = `x ${r1(r.x + dx)}  y ${r1(r.y + dy)}`;
  } else {
    const k = dr.h; let x = r.x, y = r.y, w = r.w, h = r.h;
    if (k.includes("e")) w = r.w + dx; if (k.includes("s")) h = r.h + dy;
    if (k.includes("w")) { w = r.w - dx; x = r.x + dx; } if (k.includes("n")) { h = r.h - dy; y = r.y + dy; }
    if (D.snap && !e.altKey) {
      if (k.includes("e")) { const s = snapAxis([{ pos: x + w }], LINES_X, thr); if (s) { w += s[0]; D.snapLines.push({ v: s[1] }); } }
      if (k.includes("w")) { const s = snapAxis([{ pos: x }], LINES_X, thr); if (s) { x += s[0]; w -= s[0]; D.snapLines.push({ v: s[1] }); } }
      if (k.includes("s")) { const s = snapAxis([{ pos: y + h }], LINES_Y, thr); if (s) { h += s[0]; D.snapLines.push({ h: s[1] }); } }
      if (k.includes("n")) { const s = snapAxis([{ pos: y }], LINES_Y, thr); if (s) { y += s[0]; h -= s[0]; D.snapLines.push({ h: s[1] }); } }
    }
    if (e.shiftKey && k.length === 2 && r.h > 0) { const ar = r.w / r.h; if (w / h > ar) w = h * ar; else h = w / ar; if (k.includes("w")) x = r.x + r.w - w; if (k.includes("n")) y = r.y + r.h - h; }
    w = Math.max(4, w); h = Math.max(4, h);
    if (k.includes("e") || k.includes("w")) fe.style.width = r1(w) + "px";
    if (k.includes("n") || k.includes("s")) fe.style.height = r1(h) + "px";
    setT(fe, dr.t0[0] + (x - r.x), dr.t0[1] + (y - r.y));
    $("#stPos").textContent = `w ${r1(w)}  h ${r1(h)}`;
  }
  drawOverlay();
}
function onUp() {
  const dr = D.drag; D.drag = null; if (!dr) return;
  D.snapLines = [];
  if (dr.moved) {
    const fe = fel(dr.id), se = srcEl(dr.id);
    if (fe && se) { const t = getT(fe); setT(se, t[0], t[1]); if (dr.kind === "resize") { for (const p of ["width", "height"]) { if (fe.style[p]) se.style[p] = fe.style[p]; } } }
    setDirty(true); refreshAll();
  }
  drawOverlay();
}

// text editing in place (double-click)
const textEditable = se => se && !["IMG", "SVG", "svg", "VIDEO", "CANVAS", "IFRAME"].includes(se.tagName) && se.getAttribute("data-type") !== "image" && ![...se.children].some(c => c.tagName !== "BR");
function startTextEdit(id) {
  const se = srcEl(id), fe = fel(id);
  if (!textEditable(se) || !fe) { toast("Only text elements can be edited in place (pick the text itself, not its box)"); return; }
  D.editing = id; $("#overlay").classList.add("editing"); drawOverlay();
  const before = fe.textContent;
  fe.setAttribute("contenteditable", "plaintext-only"); fe.focus();
  try { const d = frameDoc(), rg = d.createRange(); rg.selectNodeContents(fe); const s = d.defaultView.getSelection(); s.removeAllRanges(); s.addRange(rg); } catch {}
  let cancelled = false;
  const key = k => { if (k.key === "Escape") { cancelled = true; fe.textContent = before; fe.blur(); } else if (k.key === "Enter" && !k.shiftKey && se.getAttribute("data-type") !== "textarea") { k.preventDefault(); fe.blur(); } k.stopPropagation(); };
  fe.addEventListener("keydown", key);
  fe.addEventListener("blur", () => {
    fe.removeEventListener("keydown", key); fe.removeAttribute("contenteditable");
    D.editing = null; $("#overlay").classList.remove("editing");
    const txt = fe.innerText.replace(/\n$/, "");
    if (!cancelled && txt !== before) mutate(() => { se.textContent = txt; fe.textContent = txt; }); else drawOverlay();
  }, { once: true });
}

// ------------------------------------------------------------------------------------------------ element actions
function uniqueField(base) {
  base = String(base || "field").replace(/[^A-Za-z0-9_]/g, "") || "field"; if (/^\d/.test(base)) base = "f" + base;
  const used = new Set([...D.doc.querySelectorAll("[data-field]")].map(e => e.getAttribute("data-field")));
  if (!used.has(base)) return base; let n = 2; while (used.has(base + n)) n++; return base + n;
}
function delSel() {
  const se = srcEl(D.sel); if (!se) return;
  if (se.hasAttribute("data-cg-root")) { toast("The root holds the in/out animation: it cannot be deleted", "err"); return; }
  const par = se.parentElement && se.parentElement.closest("[data-cge]");
  mutate(() => se.remove(), { rerender: true }); D.sel = par ? par.getAttribute("data-cge") : null; refreshAll();
}
function dupSel() {
  const se = srcEl(D.sel); if (!se || se.hasAttribute("data-cg-root")) return;
  let nid = null;
  mutate(() => {
    const c = se.cloneNode(true); c.removeAttribute("data-cge"); c.querySelectorAll("[data-cge]").forEach(x => x.removeAttribute("data-cge"));
    [c, ...c.querySelectorAll("[data-field]")].forEach(x => { if (x.hasAttribute && x.hasAttribute("data-field")) x.setAttribute("data-field", uniqueField(x.getAttribute("data-field"))); });
    const t = getT(se); setT(c, t[0] + 24, t[1] + 24); se.after(c); assignIds(); nid = c.getAttribute("data-cge");
  }, { rerender: true, after: () => select(nid) });
}
function zSel(dir) {
  const se = srcEl(D.sel); if (!se) return;
  const sib = dir > 0 ? se.nextElementSibling : se.previousElementSibling;
  if (!sib || SKIP.has(sib.tagName)) { toast(dir > 0 ? "Already on top of its group" : "Already at the back of its group"); return; }
  mutate(() => { if (dir > 0) sib.after(se); else sib.before(se); }, { rerender: true });
}
function selParent() { const se = srcEl(D.sel), p = se && se.parentElement && se.parentElement.closest("[data-cge]"); if (p) select(p.getAttribute("data-cge")); }
function nudge(dx, dy) { const se = srcEl(D.sel), fe = fel(D.sel); if (!se) return; mutate(() => { const t = getT(se); setT(se, t[0] + dx, t[1] + dy); setT(fe, t[0] + dx, t[1] + dy); }, { key: "nudge:" + D.sel, inspector: false }); renderRight(); }
function alignSel(how) {
  const se = srcEl(D.sel), fe = fel(D.sel), r = rectOf(D.sel); if (!se || !r) return;
  const tx = { left: SAFE.title.x - r.x, hcenter: W / 2 - (r.x + r.w / 2), right: W - SAFE.title.x - (r.x + r.w) }[how], ty = { top: SAFE.title.y - r.y, vcenter: H / 2 - (r.y + r.h / 2), bottom: H - SAFE.title.y - (r.y + r.h) }[how];
  mutate(() => { const t = getT(se); setT(se, t[0] + (tx || 0), t[1] + (ty || 0)); setT(fe, t[0] + (tx || 0), t[1] + (ty || 0)); });
}

// insert blocks
const SN = {
  text: { name: "Text", hint: "static text", at: "center", html: () => `<div style="position:absolute;left:0;top:0;font:600 48px 'Segoe UI',Arial,sans-serif;color:#fff;text-shadow:0 2px 8px rgba(0,0,0,.55);white-space:nowrap">New text</div>` },
  field: { name: "Text field", hint: "operator-editable text", at: "center", html: () => { const f = uniqueField("text"); return `<div data-field="${f}" data-label="${human(f)}" style="position:absolute;left:0;top:0;font:700 52px 'Segoe UI',Arial,sans-serif;color:#fff;text-shadow:0 2px 8px rgba(0,0,0,.55);white-space:nowrap">Field text</div>`; } },
  plate: { name: "Plate", hint: "box behind text", at: "center", html: () => `<div style="position:absolute;left:0;top:0;width:760px;height:120px;background:var(--accent,#0b5fff);border-radius:8px;box-shadow:0 10px 30px rgba(0,0,0,.45)"></div>` },
  gradient: { name: "Gradient plate", hint: "two-tone box", at: "center", html: () => `<div style="position:absolute;left:0;top:0;width:760px;height:120px;background:linear-gradient(90deg,var(--accent,#0b5fff),var(--plate,#0d1220));border-radius:8px;box-shadow:0 10px 30px rgba(0,0,0,.45)"></div>` },
  image: { name: "Image field", hint: "logo / headshot slot", at: "center", html: () => { const f = uniqueField("logo"); return `<img data-field="${f}" data-type="image" data-label="${human(f)}" src="" alt="" style="position:absolute;left:0;top:0;width:180px;height:180px;object-fit:contain">`; } },
  line: { name: "Line", hint: "accent rule", at: "center", html: () => `<div style="position:absolute;left:0;top:0;width:640px;height:4px;background:var(--accent,#0b5fff);border-radius:2px"></div>` },
  circle: { name: "Circle", hint: "dot / badge", at: "center", html: () => `<div style="position:absolute;left:0;top:0;width:120px;height:120px;border-radius:50%;background:var(--accent,#0b5fff)"></div>` },
  live: { name: "LIVE pill", hint: "red badge", at: "tr", html: () => `<div style="position:absolute;left:0;top:0;display:flex;align-items:center;gap:10px;background:#e11d48;color:#fff;font:800 26px 'Segoe UI',Arial,sans-serif;letter-spacing:3px;padding:6px 16px;border-radius:4px"><span style="width:12px;height:12px;border-radius:50%;background:#fff"></span>LIVE</div>` },
  lt: { name: "Lower third", hint: "name + title block", at: "bl", html: () => { const n = uniqueField("name"), t = uniqueField("title"); return `<div class="lt" style="position:absolute;left:0;top:0;display:flex;flex-direction:column;align-items:flex-start">
  <div style="background:var(--accent,#0b5fff);padding:14px 32px;border-radius:6px 6px 0 0"><div data-field="${n}" data-label="Name" style="font:700 54px 'Segoe UI',Arial,sans-serif;color:#fff;white-space:nowrap">Jane Smith</div></div>
  <div style="background:rgba(10,14,28,.92);padding:10px 32px;border-radius:0 0 6px 6px"><div data-field="${t}" data-label="Title" data-hide-empty style="font:500 30px 'Segoe UI',Arial,sans-serif;color:#dfe6ff;white-space:nowrap">Reporter, Martinsville</div></div>
</div>`; } },
  bug: { name: "Corner bug", hint: "logo top-right", at: "tr", html: () => { const f = uniqueField("logo"); return `<img data-field="${f}" data-type="image" data-label="Logo" src="" alt="" style="position:absolute;left:0;top:0;width:160px;height:160px;object-fit:contain;opacity:.9">`; } },
  clock: { name: "Clock", hint: "live time of day", at: "tr", script: "clock", html: () => `<div data-clock style="position:absolute;left:0;top:0;font:700 40px Bahnschrift,'Segoe UI',sans-serif;color:#fff;text-shadow:0 2px 8px rgba(0,0,0,.6);background:rgba(10,14,28,.75);padding:6px 16px;border-radius:4px">12:00</div>` },
};
const SCRIPTS = { clock: `(function(){function t(){var d=new Date();document.querySelectorAll('[data-clock]').forEach(function(e){e.textContent=d.toLocaleTimeString([],{hour:'numeric',minute:'2-digit'})})}t();setInterval(t,1000)})();` };
function insertParent() {
  const se = srcEl(D.sel);
  if (se && !se.matches("img,svg") && !textEditable(se)) return se;
  return rootEl() || D.doc.body;
}
function insertBlock(k) {
  const sn = SN[k]; if (!sn) return; let nid = null;
  mutate(() => {
    const tmp = D.doc.createElement("div"); tmp.innerHTML = sn.html(); const el = tmp.firstElementChild; insertParent().appendChild(el);
    if (sn.script && !D.doc.querySelector(`script[data-cgd="${sn.script}"]`)) { const s = D.doc.createElement("script"); s.setAttribute("data-cgd", sn.script); s.textContent = SCRIPTS[sn.script]; D.doc.body.appendChild(s); }
    assignIds(); nid = el.getAttribute("data-cge");
  }, { rerender: true, after: () => {
    const r = rectOf(nid); if (!r) return; const se = srcEl(nid), fe = fel(nid);
    const tx = sn.at === "bl" ? SAFE.title.x : sn.at === "tr" ? W - SAFE.title.x - r.w : (W - r.w) / 2, ty = sn.at === "bl" ? H - SAFE.title.y - 60 - r.h : sn.at === "tr" ? SAFE.title.y : (H - r.h) / 2;
    const t = getT(se); setT(se, t[0] + tx - r.x, t[1] + ty - r.y); setT(fe, t[0] + tx - r.x, t[1] + ty - r.y); select(nid);
  } });
}

// ------------------------------------------------------------------------------------------------ template-level: :root variables, params, contract
function rootVars() {
  const out = [], seen = new Set();
  D.doc.querySelectorAll("style").forEach(st => { const re = /:root\s*\{([^}]*)\}/g; let m; while ((m = re.exec(st.textContent))) { const dre = /--([\w-]+)\s*:\s*([^;]+?)\s*(?:;|$)/g; let d; while ((d = dre.exec(m[1]))) if (!seen.has(d[1])) { seen.add(d[1]); out.push({ name: d[1], value: d[2].trim() }); } } });
  return out;
}
const escRe = s => s.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
function writeVar(name, value, remove) {
  value = String(value).replace(/[;{}]/g, ""); let done = false;
  D.doc.querySelectorAll("style").forEach(st => {
    if (done) return;
    const t = st.textContent, nt = t.replace(/(:root\s*\{)([^}]*)(\})/g, (all, a, body, c) => {
      const re = new RegExp("--" + escRe(name) + "\\s*:\\s*[^;}]*;?");
      if (done || !re.test(body)) return all; done = true;
      return a + (remove ? body.replace(re, "") : body.replace(re, `--${name}: ${value};`)) + c;
    });
    if (done) st.textContent = nt;
  });
  if (!done && !remove) {
    let st = D.doc.querySelector("style"); if (!st) { st = D.doc.createElement("style"); D.doc.head.appendChild(st); }
    if (/:root\s*\{/.test(st.textContent)) st.textContent = st.textContent.replace(/:root\s*\{/, m => `${m} --${name}: ${value};`); else st.textContent = `:root { --${name}: ${value}; }\n` + st.textContent;
  }
  const d = frameDoc(); if (d) { if (remove) d.documentElement.style.removeProperty("--" + name); else d.documentElement.style.setProperty("--" + name, value); }
}
function deriveParams() {
  const out = [], seen = new Set();
  D.doc.body.querySelectorAll("[data-field]").forEach(el => {
    const n = el.getAttribute("data-field"); if (!n || seen.has(n)) return; seen.add(n);
    const img = el.tagName === "IMG" || el.getAttribute("data-type") === "image";
    out.push({ name: n, label: el.getAttribute("data-label") || human(n), type: img ? "image" : el.getAttribute("data-type") === "textarea" ? "textarea" : "text", default: img ? (el.getAttribute("src") || "") : el.textContent.trim() });
  });
  for (const v of rootVars()) if (!seen.has(v.name)) { seen.add(v.name); out.push({ name: v.name, label: human(v.name), type: isColor(v.value) ? "color" : "text", default: v.value }); }
  return out;
}
function contractIssues() {
  const out = [];
  if (!D.doc.querySelector("[data-cg-root]")) out.push({ k: "root", t: "No element has data-cg-root: in/out animations will not run" });
  if (!D.doc.querySelector('script[src$="cg-animations.js"]')) out.push({ k: "anim", t: "js/cg-animations.js is not included" });
  if (!D.doc.querySelector('script[src$="cg-fields.js"]')) out.push({ k: "fields", t: "js/cg-fields.js is not included: fields and colour variables will not follow the operator's values" });
  const ext = [...D.doc.querySelectorAll("script[src],link[href],img[src]")].filter(e => /^(https?:)?\/\//i.test(e.getAttribute("src") || e.getAttribute("href") || ""));
  if (ext.length) out.push({ k: "ext", t: `${ext.length} external resource(s): the playout machine may be offline (save removes external scripts and stylesheets)` });
  if (!D.doc.querySelector("[data-field]") && !rootVars().length) out.push({ k: "nofield", t: "No data-field elements: the operator cannot change any text" });
  return out;
}
function fixContract() {
  mutate(() => {
    if (!D.doc.querySelector("[data-cg-root]")) { const f = [...D.doc.body.children].find(e => !SKIP.has(e.tagName)); if (f) f.setAttribute("data-cg-root", ""); }
    for (const s of ["js/cg-animations.js", "js/cg-fields.js"]) if (!D.doc.querySelector(`script[src$="${s.split("/")[1]}"]`)) { const el = D.doc.createElement("script"); el.setAttribute("src", s); const after = D.doc.querySelector('script[src$="cg-animations.js"]'); if (s.endsWith("fields.js") && after) after.after(el); else { const first = D.doc.body.querySelector("script"); if (first) first.before(el); else D.doc.body.appendChild(el); } }
  }, { rerender: true });
}

// ------------------------------------------------------------------------------------------------ left pane
function refreshAll() { renderLeft(); renderRight(); updateStatus(); }
function renderLayersIfShown() { if (D.ltab === "layers") renderLeft(); }
function renderLeft() {
  document.querySelectorAll("[data-ltab]").forEach(b => b.classList.toggle("on", b.dataset.ltab === D.ltab));
  const p = $("#leftPane"); if (!p) return;
  if (D.ltab === "layers") p.innerHTML = layersHtml(); else if (D.ltab === "insert") p.innerHTML = insertHtml(); else { if (p.dataset.ai !== "1" || !p.querySelector("#prompt")) { p.innerHTML = aiHtml(); p.dataset.ai = "1"; } updateAi(); }
  if (D.ltab !== "ai") p.dataset.ai = "";
}
function layersHtml() {
  if (!D.doc) return "";
  const rows = [];
  const walk = (el, d) => { for (const c of el.children) { if (!c.hasAttribute("data-cge")) continue; const own = [...c.childNodes].filter(n => n.nodeType === 3).map(n => n.textContent).join(" ").trim().slice(0, 28), f = c.getAttribute("data-field"), id = c.getAttribute("data-cge");
    rows.push(`<div class="layer${id === D.sel ? " sel" : ""}" data-act="pick" data-id="${id}" style="padding-left:${6 + d * 12}px">${c.hasAttribute("data-cg-root") ? '<span class="badge" style="color:var(--ready)">root</span>' : ""}<span class="tg">${E(c.tagName.toLowerCase())}${c.getAttribute("class") ? "." + E(c.getAttribute("class").split(/\s+/)[0]) : ""}</span>${f ? `<span class="fd">${E(f)}</span>` : ""}<span class="tx">${E(own)}</span></div>`); walk(c, d + 1); } };
  walk(D.doc.body, 0);
  return `<h4>Layers <span class="r mu sm">click to select · Alt+click in the canvas picks the parent</span></h4>${rows.join("") || '<div class="mu">Empty page</div>'}
    <div class="mu sm" style="margin-top:10px">Later items draw on top. Use the inspector's ▲ ▼ to reorder.</div>`;
}
function insertHtml() {
  const par = insertParent(), pl = par === D.doc.body ? "the page" : label(par);
  return `<h4>Insert</h4><div class="mu sm" style="margin-bottom:8px">Goes into <b>${E(pl)}</b> (select a box first to insert inside it).</div>
    <div class="ins">${Object.entries(SN).map(([k, s]) => `<button data-act="insert" data-k="${k}">${E(s.name)}<small>${E(s.hint)}</small></button>`).join("")}</div>
    <h4 style="margin-top:14px">Tips</h4><div class="mu sm">Drag to move, handles to resize (Shift keeps the aspect ratio, Alt skips snapping). Double-click text to type. Arrows nudge 1 px (Shift: 10). Ctrl+D duplicates, Delete removes.</div>`;
}
function aiReady() { return D.info && D.info.ai && D.info.ai.hasKey; }
function aiHtml() {
  const g = D.gen, opt = (v, cur, l) => `<option value="${E(v)}"${String(v) === String(cur) ? " selected" : ""}>${E(l || v || "any")}</option>`;
  const key = !D.info ? `<div class="note${D.infoErr ? " err" : ""}">${D.infoErr ? E(D.infoErr) : "Loading…"}</div>` : !aiReady() ? `<div class="note err">No OpenRouter key: set the <code>OPENROUTER_API_KEY</code> environment variable for the Supervisor (or save a key under AI settings in the dashboard). The editor works without it.</div>` : "";
  return `${key}
  <div class="row" style="margin-bottom:8px"><button class="small ${g.mode === "new" ? "on" : "ghost"}" data-act="mode" data-v="new">New design</button><button class="small ${g.mode === "refine" ? "on" : "ghost"}" data-act="mode" data-v="refine" title="Send the current design with an instruction">Refine current</button><span class="fill"></span><span class="mu sm" id="modeHint"></span></div>
  <textarea id="prompt" data-gen="prompt" placeholder="${g.mode === "refine" ? "What to change, e.g. make the plate red, add a logo slot on the left, bigger name" : "Describe the graphic in full: what it is, where it sits, what text the operator changes, colours, type, mood, motion…"}">${E(g.prompt)}</textarea>
  <div class="chips">${IDEAS.map(([l], i) => `<button class="chip" data-act="idea" data-i="${i}">${E(l)}</button>`).join("")}</div>
  <div class="grid2"><label class="f">Kind<select data-gen="kind">${KINDS.map(k => opt(k, g.kind)).join("")}</select></label><label class="f">Variations<select data-gen="count">${[1, 2, 3].map(n => opt(n, g.count)).join("")}</select></label></div>
  <details style="margin:8px 0" ${g.brandColors || g.brandFont || g.brandNotes ? "open" : ""}><summary class="mu sm" style="cursor:pointer">Brand (optional)</summary>
    <label class="f" style="margin-top:6px">Colours<input data-gen="brandColors" value="${E(g.brandColors)}" placeholder="#0b3d91 navy, #ffd60a gold"></label>
    <label class="f">Font<input data-gen="brandFont" list="fontList" value="${E(g.brandFont)}" placeholder="Bahnschrift"></label>
    <label class="f">Notes<input data-gen="brandNotes" value="${E(g.brandNotes)}" placeholder="station: Star News, tone: bold, local"></label></details>
  <h4>Examples <span class="r mu sm">optional · match their look</span></h4>
  <div id="exList"></div>
  <div class="row wrap" style="margin:4px 0 10px"><button class="small" data-act="exTpl">+ Template</button><button class="small" data-act="exImg" title="Or paste an image here (Ctrl+V)">+ Image</button><button class="small" data-act="exHtml">+ HTML</button><button class="small" data-act="exCur" title="Use the design on the canvas as a style example">+ Current</button></div>
  <label class="f">Model<input data-gen="model" list="modelList" value="${E(g.model)}" placeholder="${E((D.info && D.info.ai && D.info.ai.model) || "default")}"></label>
  <details style="margin:2px 0 8px" ${g.houseOpen ? "open" : ""} ontoggle="CGD.gen.houseOpen=this.open"><summary class="mu sm" style="cursor:pointer">Station house style <span class="mu">(sent with every generation)</span></summary>
    <textarea data-house rows="4" style="width:100%;margin-top:6px" placeholder="e.g. Navy #0b3d91 and gold #ffd60a, Bahnschrift for numbers, plates with 6 px corners, our bug sits top-right">${E((D.info && D.info.config && D.info.config.houseStyle) || "")}</textarea>
    <div class="row" style="justify-content:flex-end;margin-top:4px"><button class="small" data-act="saveHouse">Save house style</button></div></details>
  <div class="row" style="margin:6px 0 10px"><button class="primary" data-act="generate" id="genBtn" style="flex:1">✦ Generate</button><button class="ghost small" data-act="viewPrompt" title="Show exactly what is sent to the model">View full prompt</button></div>
  <div id="genStatus"></div>
  <div class="drafts" id="drafts"></div>`;
}
function updateAi() {   // keeps the prompt box (and its caret) while the rest of the pane changes
  const g = D.gen, ex = $("#exList"); if (!ex) return;
  ex.innerHTML = g.examples.map((x, i) => `<div class="ex">${x.kind === "image" ? `<img src="${E(x.dataUrl)}" alt="">` : ""}<span class="k">${E(x.kind)}</span><span class="sm" style="max-width:110px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap">${E(x.title || "")}</span><input data-exnote="${i}" value="${E(x.note || "")}" placeholder="what to take from it"><button class="ghost small" data-act="exDel" data-i="${i}">✕</button></div>`).join("") || `<div class="mu sm">None: the model designs from the prompt alone.</div>`;
  const busy = g.job && g.job.state === "running";
  const b = $("#genBtn"); if (b) { b.disabled = busy || !aiReady(); b.innerHTML = busy ? `<span class="spin"></span> Generating…` : g.mode === "refine" ? "✦ Refine design" : `✦ Generate${g.count > 1 ? " " + g.count + " variations" : ""}`; }
  const st = $("#genStatus"); if (st) {
    if (busy) st.innerHTML = `<div class="note">Working with <b>${E(g.job.model || "the model")}</b> · ${Math.round((Date.now() - g.job.started) / 1000)} s (a full page usually takes 30–90 s) <button class="ghost small" data-act="stopWait">stop waiting</button></div>`;
    else if (g.err) st.innerHTML = `<div class="note err">${E(g.err)}</div>`;
    else if (g.job && g.job.state === "done") st.innerHTML = `<div class="note ok">Done in ${Math.round((g.job.ms || 0) / 1000)} s${g.job.usage && g.job.usage.total_tokens ? " · " + g.job.usage.total_tokens + " tokens" : ""}. ${g.drafts.length > 1 ? "Pick a draft to open it." : ""}</div>`;
    else st.innerHTML = "";
  }
  const dr = $("#drafts"); if (dr && dr.dataset.n !== String(g.drafts.length) + (g.job && g.job.id)) {
    dr.dataset.n = String(g.drafts.length) + (g.job && g.job.id);
    dr.innerHTML = g.drafts.map((d, i) => `<div class="draft" data-act="openDraft" data-i="${i}"><div class="th"><iframe tabindex="-1" scrolling="no" data-thumb="${i}"></iframe></div><div class="cap"><b>${E((d.meta && d.meta.name) || "Draft " + (i + 1))}</b><span class="fill"></span>${(d.warnings || []).length ? `<span class="badge" style="color:var(--standby)" title="${E(d.warnings.join("\n"))}">${d.warnings.length} fix</span>` : ""}<span class="mu sm">open ›</span></div></div>`).join("");
    dr.querySelectorAll("iframe[data-thumb]").forEach(f => { f.srcdoc = withBase(g.drafts[+f.dataset.thumb].html); });
  }
  fitThumbs();
}
function fitThumbs() { document.querySelectorAll(".draft .th").forEach(th => { const f = th.querySelector("iframe"); if (f) f.style.transform = `scale(${th.clientWidth / W})`; }); }

// ------------------------------------------------------------------------------------------------ right pane
function renderRight() {
  document.querySelectorAll("[data-rtab]").forEach(b => b.classList.toggle("on", b.dataset.rtab === D.rtab));
  const p = $("#rightPane"); if (!p || !D.doc) return;
  const ae = document.activeElement; if (ae && p.contains(ae) && /INPUT|TEXTAREA|SELECT/.test(ae.tagName) && D.rightKey === (D.rtab + D.sel)) { syncRightValues(); return; }
  D.rightKey = D.rtab + D.sel;
  p.innerHTML = D.rtab === "template" ? templateHtml() : elementHtml();
}
function syncRightValues() {   // while typing in the pane: refresh geometry only
  const se = srcEl(D.sel); if (!se) return; const t = getT(se), r = rectOf(D.sel);
  const set = (k, v) => { const i = document.querySelector(`[data-geo="${k}"]`); if (i && i !== document.activeElement) i.value = v; };
  set("tx", r1(t[0])); set("ty", r1(t[1])); if (r) { const w = document.querySelector('[data-geo="w"]'), h = document.querySelector('[data-geo="h"]'); if (w) w.placeholder = r1(r.w); if (h) h.placeholder = r1(r.h); }
}
function cssIn(se, prop) { return se.style.getPropertyValue(prop) || ""; }
function cssComp(prop) { const fe = fel(D.sel); try { return fe ? fe.ownerDocument.defaultView.getComputedStyle(fe).getPropertyValue(prop) : ""; } catch { return ""; } }
function inp(prop, ph, extra) { const se = srcEl(D.sel); return `<input data-css="${prop}" value="${E(cssIn(se, prop))}" placeholder="${E(ph ?? cssComp(prop))}" ${extra || ""}>`; }
function colorF(prop, lbl) { const se = srcEl(D.sel), v = cssIn(se, prop), comp = prop === "background" ? (cssComp("background-color") || "") : cssComp(prop); return `<label class="f">${lbl}<div class="colorf"><input type="color" data-css="${prop}" value="${toHex(v && isColor(v) ? v : comp)}"><input type="text" data-css="${prop}" value="${E(v)}" placeholder="${E(prop === "background" ? (cssComp("background-image") !== "none" ? cssComp("background-image") : comp) : comp)}"></div></label>`; }
function sel(prop, opts, lbl) { const se = srcEl(D.sel), v = cssIn(se, prop), comp = cssComp(prop); return `<label class="f">${lbl}<select data-css="${prop}"><option value="">${E(comp ? "(" + comp + ")" : "inherit")}</option>${opts.map(o => { const [val, l] = Array.isArray(o) ? o : [o, o]; return `<option value="${E(val)}"${val === v ? " selected" : ""}>${E(l)}</option>`; }).join("")}</select></label>`; }
function elementHtml() {
  const se = srcEl(D.sel);
  if (!se) return `<div class="mu" style="margin-bottom:10px">Click an element in the canvas (or in Layers) to edit it.</div>${D.packNote ? `<div class="note">This is a copy of a pack template. Its own script may set some texts from URL params, so in-place edits to those texts can be overridden: convert them into fields (Element ▸ Operator field) for designer-style templates.</div>` : ""}<h4>Template</h4><button data-act="rtab" data-v="template">Template settings, colours and fields ›</button>`;
  const fe = fel(D.sel), t = getT(se), r = rectOf(D.sel), isImg = se.tagName === "IMG" || se.getAttribute("data-type") === "image", txt = textEditable(se), f = se.getAttribute("data-field");
  const scriptMade = !fe;
  let h = `<div class="ins-head"><span class="tag" title="${E(label(se))}">${E(label(se))}</span><span class="fill"></span>
    <button class="ghost small" data-act="parent" title="Select parent">⤴</button><button class="ghost small" data-act="zdown" title="Send backward">▼</button><button class="ghost small" data-act="zup" title="Bring forward">▲</button><button class="ghost small" data-act="dup" title="Duplicate (Ctrl+D)">⧉</button><button class="ghost small" data-act="del" title="Delete">🗑</button></div>`;
  if (scriptMade) h += `<div class="note">Not in the preview right now (hidden, or replaced by the page's script).</div>`;
  if (txt || isImg) {
    h += `<h4>Content</h4>`;
    if (txt) h += `<label class="f">${f ? "Default text" : "Text"}<textarea data-text rows="2">${E(se.textContent)}</textarea></label>`;
    if (isImg) h += `<label class="f">Image<div class="row"><input class="fill" data-attr="src" value="${E((se.getAttribute("src") || "").startsWith("data:") ? "(embedded image)" : se.getAttribute("src") || "")}" placeholder="URL, /cg/... or upload"><button class="small" data-act="imgUp">Upload</button></div></label>${sel("object-fit", ["contain", "cover", "fill"], "Fit")}`;
    h += `<label class="row sm" style="margin:2px 0 6px"><input type="checkbox" data-act="fieldToggle" ${f ? "checked" : ""}> Operator field <span class="mu">(changeable on air, Text 1/2/3)</span></label>`;
    if (f) h += `<div class="grid2"><label class="f">Field name<input data-attr="data-field" value="${E(f)}"></label><label class="f">Label<input data-attr="data-label" value="${E(se.getAttribute("data-label") || "")}" placeholder="${E(human(f))}"></label></div>
      <div class="grid2" style="margin-top:6px"><label class="f">Type<select data-attr="data-type">${[["", isImg ? "image" : "text"], ["textarea", "multi-line"], ["image", "image"]].map(([v, l]) => `<option value="${v}"${(se.getAttribute("data-type") || "") === v ? " selected" : ""}>${l}</option>`).join("")}</select></label><label class="row sm" style="margin-top:16px"><input type="checkbox" data-act="hideEmpty" ${se.hasAttribute("data-hide-empty") ? "checked" : ""}> hide when empty</label></div>`;
  }
  h += `<h4>Position &amp; size <span class="r"><button class="ghost small" data-act="align" data-v="left" title="Left title-safe">⇤</button><button class="ghost small" data-act="align" data-v="hcenter" title="Centre horizontally">↔</button><button class="ghost small" data-act="align" data-v="right" title="Right title-safe">⇥</button><button class="ghost small" data-act="align" data-v="top" title="Top title-safe">⤒</button><button class="ghost small" data-act="align" data-v="vcenter" title="Centre vertically">↕</button><button class="ghost small" data-act="align" data-v="bottom" title="Bottom title-safe">⤓</button></span></h4>
    <div class="grid4"><label class="f">Move X<input data-geo="tx" type="number" value="${r1(t[0])}"></label><label class="f">Move Y<input data-geo="ty" type="number" value="${r1(t[1])}"></label><label class="f">W<input data-geo="w" value="${E(cssIn(se, "width"))}" placeholder="${r ? r1(r.w) : ""}"></label><label class="f">H<input data-geo="h" value="${E(cssIn(se, "height"))}" placeholder="${r ? r1(r.h) : ""}"></label></div>
    <div class="mu sm" style="margin:4px 0">${r ? `On screen at ${r1(r.x)}, ${r1(r.y)}` : ""}. Move = offset from where the layout puts it.</div>`;
  if (!isImg) h += `<h4>Text</h4>
    <label class="f">Font<input data-css="font-family" list="fontList" value="${E(cssIn(se, "font-family"))}" placeholder="${E(cssComp("font-family"))}"></label>
    <div class="grid2"><label class="f">Size${inp("font-size")}</label>${sel("font-weight", [["300", "Light"], ["400", "Regular"], ["500", "Medium"], ["600", "Semibold"], ["700", "Bold"], ["800", "Extra bold"], ["900", "Black"]], "Weight")}</div>
    <div class="grid2" style="margin-top:6px">${colorF("color", "Colour")}<label class="f">Spacing${inp("letter-spacing")}</label></div>
    <div class="grid2">${sel("text-align", ["left", "center", "right"], "Align")}${sel("text-transform", [["none", "As typed"], ["uppercase", "UPPERCASE"], ["capitalize", "Capitalize"]], "Case")}</div>
    <div class="grid2" style="margin-top:6px"><label class="f">Line height${inp("line-height")}</label>${sel("font-style", ["normal", "italic"], "Style")}</div>
    <label class="f" style="margin-top:6px">Text shadow${inp("text-shadow")}</label>`;
  h += `<h4>Box</h4>${colorF("background", "Fill (colour or gradient)")}
    <div class="grid2"><label class="f">Radius${inp("border-radius")}</label><label class="f">Padding${inp("padding")}</label></div>
    <div class="grid2" style="margin-top:6px"><label class="f">Opacity<input data-css="opacity" type="number" min="0" max="1" step="0.05" value="${E(cssIn(se, "opacity"))}" placeholder="${E(cssComp("opacity"))}"></label><label class="f">Layer (z)${inp("z-index")}</label></div>
    <label class="f" style="margin-top:6px">Border${inp("border")}</label><label class="f">Shadow${inp("box-shadow")}</label>
    <label class="f">Extra CSS (inline)<textarea data-attr="style" rows="3" class="mono" style="font-size:11px">${E(se.getAttribute("style") || "")}</textarea></label>`;
  return h;
}
function templateHtml() {
  const m = D.meta, params = deriveParams(), vars = rootVars(), iss = contractIssues();
  const roles = ROLES.map(([v, l]) => `<option value="${v}"${m.role === v ? " selected" : ""}>${l}</option>`).join("");
  const cats = [...new Set(CATS.concat((D.info && D.info.categories) || []))].map(c => `<option${m.category === c ? " selected" : ""}>${E(c)}</option>`).join("");
  return `<h4>Template</h4>
    <label class="f">Name<input data-meta="name" value="${E(m.name)}" placeholder="Untitled graphic"></label>
    <label class="f">Description<textarea data-meta="description" rows="2">${E(m.description)}</textarea></label>
    <div class="grid2"><label class="f">Role (layer band)<select data-meta="role">${roles}</select></label><label class="f">Category<select data-meta="category">${cats}</select></label></div>
    <div class="mu sm" style="margin:4px 0">${m.id ? `Saved as <code>${E(m.id)}</code>${m.versionCount ? ` · ${m.versionCount} earlier version(s)` : ""}` : "Not saved yet."}</div>
    ${iss.length ? `<h4>Checks</h4>${iss.map(i => `<div class="note">${E(i.t)}</div>`).join("")}${iss.some(i => ["root", "anim", "fields"].includes(i.k)) ? `<button class="small" data-act="fix">Fix: add root and runtime scripts</button>` : ""}` : `<div class="note ok">Follows the template contract.</div>`}
    <h4>Colours &amp; variables <span class="r mu sm">:root · each one is a param</span></h4>
    <div class="vars">${vars.map(v => `<div class="row"><code>--${E(v.name)}</code>${isColor(v.value) ? `<input type="color" data-var="${E(v.name)}" value="${toHex(v.value)}">` : ""}<input class="fill" data-var="${E(v.name)}" value="${E(v.value)}"><button class="ghost small" data-act="varDel" data-n="${E(v.name)}">✕</button></div>`).join("") || '<div class="mu sm">None yet.</div>'}
      <div class="row" style="margin-top:6px"><input id="nvName" placeholder="name e.g. accent" style="width:110px"><input id="nvVal" class="fill" placeholder="#e11d48"><button class="small" data-act="varAdd">Add</button></div></div>
    <h4>Operator fields <span class="r mu sm">derived on save</span></h4>
    <table class="params"><tr><th>Field</th><th>Type</th><th>Default</th></tr>${params.map((p, i) => `<tr><td><b>${E(p.name)}</b>${i < 3 && p.type !== "image" && p.type !== "color" && params.slice(0, i).filter(x => x.type !== "image" && x.type !== "color").length === i ? ` <span class="mu sm">text${i + 1}</span>` : ""}</td><td class="mu">${E(p.type)}</td><td class="mu" style="max-width:120px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap">${E(String(p.default).startsWith("data:") ? "(image)" : p.default)}</td></tr>`).join("") || '<tr><td colspan="3" class="mu">No fields</td></tr>'}</table>
    ${m.prompt ? `<h4>Prompt used</h4><div class="mu sm" style="white-space:pre-wrap">${E(m.prompt)}</div>` : ""}`;
}

// ------------------------------------------------------------------------------------------------ pane events
function onPaneInput(e) {
  const t = e.target, se = srcEl(D.sel), fe = fel(D.sel);
  if (t.dataset.gen !== undefined) { D.gen[t.dataset.gen] = t.dataset.gen === "count" ? +t.value : t.value; if (t.dataset.gen === "prompt") LS.set("cgd.prompt", t.value); if (t.dataset.gen === "count") updateAi(); return; }
  if (t.dataset.exnote !== undefined) { const x = D.gen.examples[+t.dataset.exnote]; if (x) x.note = t.value; return; }
  if (t.dataset.meta) { D.meta[t.dataset.meta] = t.value; if (t.dataset.meta === "name") $("#tplName").value = t.value; setDirty(true); return; }
  if (t.dataset.var) {
    const n = t.dataset.var; mutate(() => writeVar(n, t.value), { key: "var:" + n, inspector: false });
    const sib = t.parentElement.querySelectorAll(`[data-var="${CSS.escape(n)}"]`); sib.forEach(s => { if (s !== t) s.value = s.type === "color" ? toHex(t.value) : t.value; });
    return;
  }
  if (!se) return;
  if (t.dataset.css) {
    let v = String(t.value).trim(); const prop = t.dataset.css;
    if (PX.has(prop) && /^-?\d+(\.\d+)?$/.test(v)) v += "px";
    mutate(() => setStyleBoth(D.sel, prop, v), { key: "css:" + D.sel + prop, inspector: false });
    t.parentElement.querySelectorAll(`[data-css="${prop}"]`).forEach(s => { if (s !== t) s.value = s.type === "color" ? toHex(v) : v; });
    return;
  }
  if (t.dataset.geo) {
    const k = t.dataset.geo, v = String(t.value).trim();
    mutate(() => {
      if (k === "tx" || k === "ty") { const c = getT(se), x = k === "tx" ? (+v || 0) : c[0], y = k === "ty" ? (+v || 0) : c[1]; setT(se, x, y); setT(fe, x, y); }
      else setStyleBoth(D.sel, k === "w" ? "width" : "height", /^-?\d+(\.\d+)?$/.test(v) ? v + "px" : v);
    }, { key: "geo:" + D.sel + k, inspector: false });
    return;
  }
  if (t.dataset.text !== undefined) { mutate(() => { se.textContent = t.value; if (fe) fe.textContent = t.value; }, { key: "text:" + D.sel, inspector: false }); return; }
  if (t.dataset.attr) {
    const a = t.dataset.attr; let v = t.value;
    if (a === "src" && v === "(embedded image)") return;
    if (a === "data-field") { v = v.replace(/[^A-Za-z0-9_]/g, ""); if (!v) return; }
    if (e.type === "input" && (a === "data-type" || a === "style")) return;   // applied on change
    mutate(() => { for (const el of [se, fe]) { if (!el) continue; if (v === "" && a !== "src") el.removeAttribute(a); else el.setAttribute(a, v); } }, { key: "attr:" + D.sel + a, inspector: false, rerender: a === "data-type" });
  }
}
const ACT = {
  pick: el => select(el.dataset.id),
  insert: el => insertBlock(el.dataset.k),
  parent: () => selParent(), del: () => delSel(), dup: () => dupSel(), zup: () => zSel(1), zdown: () => zSel(-1), align: el => alignSel(el.dataset.v),
  rtab: el => { D.rtab = el.dataset.v; renderRight(); },
  fix: () => fixContract(),
  varDel: el => mutate(() => writeVar(el.dataset.n, "", true), { rerender: true }),
  varAdd: () => { const n = ($("#nvName").value || "").trim().replace(/^--/, "").replace(/[^\w-]/g, ""), v = ($("#nvVal").value || "").trim(); if (!n || !v) { toast("Name and value", "err"); return; } mutate(() => writeVar(n, v)); },
  fieldToggle: el => { const se = srcEl(D.sel); if (!se) return; mutate(() => { if (el.checked) { const base = (se.getAttribute("class") || "").split(/\s+/)[0] || (se.tagName === "IMG" ? "image" : se.textContent.trim().split(/\s+/).slice(0, 2).join("")).toLowerCase() || "text"; se.setAttribute("data-field", uniqueField(base.replace(/[^a-z0-9]/gi, "").slice(0, 20) || "text")); if (se.tagName === "IMG") se.setAttribute("data-type", "image"); } else { ["data-field", "data-label", "data-hide-empty"].forEach(a => se.removeAttribute(a)); } }, { rerender: true }); },
  hideEmpty: el => { const se = srcEl(D.sel); mutate(() => { if (el.checked) se.setAttribute("data-hide-empty", ""); else se.removeAttribute("data-hide-empty"); }, { rerender: true }); },
  imgUp: () => { const p = $("#fileImg"); p.onchange = async () => { const f = p.files[0]; p.value = ""; if (!f) return; const url = await shrinkImage(f, 1000); if (url.length > 600000) toast("Large image embedded in the page (" + Math.round(url.length / 1024) + " KB)"); mutate(() => { srcEl(D.sel).setAttribute("src", url); const fe = fel(D.sel); if (fe) { fe.setAttribute("src", url); fe.style.visibility = ""; } }); }; p.click(); },
  mode: el => { D.gen.mode = el.dataset.v; const p = $("#leftPane"); p.dataset.ai = ""; renderLeft(); },
  idea: el => { const t = IDEAS[+el.dataset.i][1]; D.gen.prompt = D.gen.prompt.trim() ? D.gen.prompt.trim() + "\n" + t : t; const pr = $("#prompt"); if (pr) pr.value = D.gen.prompt; LS.set("cgd.prompt", D.gen.prompt); },
  exDel: el => { D.gen.examples.splice(+el.dataset.i, 1); updateAi(); },
  exTpl: () => pickTemplates(),
  exImg: () => { const p = $("#fileImg"); p.onchange = async () => { const f = p.files[0]; p.value = ""; if (f) addImageExample(f); }; p.click(); },
  exHtml: () => dlg("Paste example HTML", `<textarea id="exHtmlTa" class="mono" style="width:100%;min-height:300px;font-size:12px" placeholder="<!DOCTYPE html>…"></textarea>`, [{ label: "Cancel" }, { label: "Add", cls: "primary", fn: () => { const v = $("#exHtmlTa").value.trim(); if (!v) return true; D.gen.examples.push({ kind: "html", html: v.slice(0, 60000), title: "pasted HTML", note: "" }); updateAi(); } }]),
  exCur: () => { D.gen.examples.push({ kind: "html", html: serialize(true), title: D.meta.name || "current design", note: "keep this look" }); updateAi(); },
  generate: () => generate(),
  viewPrompt: () => viewPrompt(),
  saveHouse: async () => { const v = ($("[data-house]") || {}).value || ""; try { await api("PUT", "/cg/designer/config", { houseStyle: v.slice(0, 4000) }); if (D.info) { D.info.config = Object.assign({}, D.info.config, { houseStyle: v }); } toast("House style saved: it goes with every generation", "ok"); } catch (e) { toast("Save failed: " + e.message, "err"); } },
  stopWait: () => { clearInterval(D.gen.timer); D.gen.job = null; D.gen.err = "Stopped waiting (the job may still finish on the server)."; updateAi(); },
  openDraft: el => openDraft(+el.dataset.i),
};
function onPaneClick(e) {
  const el = e.target.closest("[data-act]"); if (!el) return;
  const f = ACT[el.dataset.act]; if (!f) return;
  if (el.type === "checkbox") { f(el); return; }
  e.preventDefault(); f(el);
}

// ------------------------------------------------------------------------------------------------ AI generation
async function addImageExample(file) { try { const url = await shrinkImage(file); D.gen.examples.push({ kind: "image", dataUrl: url, title: file.name || "image", note: "" }); updateAi(); } catch { toast("Could not read the image", "err"); } }
async function pickTemplates() {
  const list = allTemplates(); const picked = new Set();
  dlg("Example templates <span class='mu sm'>pick up to 3</span>", `<div class="tlist">${list.map(t => `<div class="tcard" data-tid="${E(t.id)}"><div class="n">${E(t.name)}</div><div class="c">${E(t.custom ? "my design" : t.category || "")}</div></div>`).join("")}</div>`,
    [{ label: "Cancel" }, { label: "Add", cls: "primary", fn: () => { for (const id of picked) { const t = list.find(x => x.id === id); D.gen.examples.push({ kind: "template", id, title: t ? t.name : id, note: "" }); } updateAi(); } }],
    d => d.querySelectorAll(".tcard").forEach(c => c.onclick = () => { const id = c.dataset.tid; if (picked.has(id)) picked.delete(id); else if (picked.size + D.gen.examples.filter(x => x.kind === "template").length < 3) picked.add(id); else toast("At most 3 template examples"); c.classList.toggle("picked", picked.has(id)); }));
}
function allTemplates() {
  const mine = ((D.info && D.info.templates) || []).map(t => ({ id: t.id, name: t.name, category: t.category, custom: true, file: t.id + ".html" }));
  const pack = ((D.info && D.info.pack) || ((D.manifest && D.manifest.templates) || []).filter(t => !t.custom && t.listed !== false)).map(t => ({ id: t.id, name: t.name, category: t.category, file: t.file, role: t.role }));
  return mine.concat(pack);
}
function genBody() {
  const g = D.gen;
  const prompt = (($("#prompt") || {}).value || g.prompt || "").trim(); g.prompt = prompt;
  if (!prompt) { toast(g.mode === "refine" ? "Say what to change" : "Write a prompt first", "err"); return null; }
  const body = { prompt, mode: g.mode, count: g.mode === "refine" ? 1 : g.count, examples: g.examples.map(x => x.kind === "template" ? { kind: "template", id: x.id, note: x.note || "" } : x.kind === "image" ? { kind: "image", dataUrl: x.dataUrl, note: x.note || "" } : { kind: "html", html: x.html, note: x.note || "" }) };
  if (g.kind) body.kind = g.kind; if (g.model.trim()) body.model = g.model.trim();
  if (g.brandColors || g.brandFont || g.brandNotes) body.brand = { colors: g.brandColors, font: g.brandFont, notes: g.brandNotes };
  if (g.mode === "refine") { body.baseHtml = serialize(true); body.name = D.meta.name; }
  return body;
}
async function viewPrompt() {
  const body = genBody(); if (!body) return;
  try {
    const r = await api("POST", "/cg/designer/generate", Object.assign(body, { dryRun: true }));
    const msgs = (r.messages || []).map(m => { const parts = Array.isArray(m.content) ? m.content : [{ type: "text", text: m.content }]; return `<h4>${E(m.role)}</h4>${parts.map(p => p.type === "text" ? `<pre class="mono" style="white-space:pre-wrap;font-size:11.5px;background:var(--bg3);padding:8px;border-radius:5px;margin:0 0 6px">${E(p.text)}</pre>` : `<div class="note">${E(typeof p.image_url === "object" ? p.image_url.url : p.image_url || "image")}</div>`).join("")}`; }).join("");
    const all = (r.messages || []).map(m => "### " + m.role + "\n" + (Array.isArray(m.content) ? m.content.map(p => p.text || (p.image_url && (p.image_url.url || p.image_url)) || "").join("\n") : m.content)).join("\n\n");
    dlg(`Full prompt <span class="mu sm">${E(r.model || "")} · max ${E(r.maxTokens || "")} tokens · temperature ${E(r.temperature ?? "")}</span>`, msgs || "<div class='mu'>No messages</div>", [{ label: "Copy", fn: () => { navigator.clipboard.writeText(all).then(() => toast("Copied", "ok")); return true; } }, { label: "Close" }]);
  } catch (e) { toast(e.status === 404 || e.status === 400 ? "This Supervisor cannot show the prompt (" + e.message + ")" : e.message, "err"); }
}
async function generate() {
  const g = D.gen; g.err = "";
  const body = genBody(); if (!body) return;
  try {
    const r = await api("POST", "/cg/designer/generate", body);
    g.job = { id: r.jobId || r.id, state: "running", started: Date.now(), model: r.model || body.model || (D.info && D.info.ai && D.info.ai.model) }; g.drafts = []; g.req = body;
    clearInterval(g.timer); g.timer = setInterval(pollJob, 2000); updateAi();
  } catch (e) { g.err = e.message; updateAi(); }
}
async function pollJob() {
  const g = D.gen; if (!g.job) { clearInterval(g.timer); return; }
  updateAi();
  let j; try { j = await api("GET", "/cg/designer/jobs/" + encodeURIComponent(g.job.id)); } catch (e) { if (e.status === 404) { clearInterval(g.timer); g.err = "The job was lost (Supervisor restarted?)"; g.job = null; updateAi(); } return; }
  if (j.state === "running") return;
  clearInterval(g.timer); g.job = Object.assign(g.job, j);
  if (j.state === "failed") { g.err = j.error || "Generation failed"; updateAi(); return; }
  g.drafts = j.drafts || [];
  updateAi();
  if (g.drafts.length === 1) openDraft(0, true);
}
async function openDraft(i, auto) {
  const d = D.gen.drafts[i]; if (!d) return; const g = D.gen, m = d.meta || {};
  const req = g.req || {}, ex = (req.examples || []).map(x => x.kind === "image" ? { kind: "image", note: x.note } : x.kind === "html" ? { kind: "html", note: x.note } : x);
  if (req.mode === "refine") { replaceDoc(d.html, { prompt: (D.meta.prompt ? D.meta.prompt + "\n— refine: " : "") + req.prompt, model: g.job && g.job.model }); toast("Refined: Undo (Ctrl+Z) goes back to the previous design", "ok"); return; }
  if (D.dirty && !auto && !(await confirmDlg("Open draft", "The canvas has unsaved changes. Replace it with this draft? (Undo still works.)", "Open draft"))) return;
  if (D.dirty || D.meta.id) { replaceDoc(d.html, Object.assign(blankMeta(), { name: m.name || "", description: m.description || "", category: m.category || "custom", role: m.role || "lower_third", prompt: req.prompt, examples: ex, model: g.job && g.job.model })); D.meta.id = null; setDirty(true); }
  else loadDoc(d.html, { name: m.name || "", description: m.description || "", category: m.category || "custom", role: m.role || "lower_third", prompt: req.prompt, examples: ex, model: g.job && g.job.model }, { dirty: true });
  D.packNote = false;
  if ((d.warnings || []).length) toast("Draft opened · fixed on the server: " + d.warnings.join("; ")); else toast("Draft opened: edit it, then Save", "ok");
}

// ------------------------------------------------------------------------------------------------ top bar actions
async function loadInfo() {
  try { D.info = await api("GET", "/cg/designer"); D.infoErr = ""; }
  catch (e) { D.info = null; D.infoErr = e.status === 404 ? "This Supervisor has no CG Designer service (older build): editing works, saving and AI do not." : e.status === 401 ? "Not authorised: open the dashboard once and enter the token." : "Designer service: " + e.message; }
  try { D.manifest = await api("GET", "/cg/templates"); } catch {}
  const anims = (D.manifest && D.manifest.animations) || ["none", "fade", "slide-left", "slide-right", "slide-up", "slide-down", "wipe-left", "wipe-right", "bounce", "zoom", "flip", "typewriter"];
  $("#animIn").innerHTML = anims.map(a => `<option${a === "slide-left" ? " selected" : ""}>${E(a)}</option>`).join(""); $("#animOut").innerHTML = anims.map(a => `<option${a === "fade" ? " selected" : ""}>${E(a)}</option>`).join("");
  $("#leftPane").dataset.ai = ""; renderLeft();
}
async function loadChannels() {
  try { const c = await api("GET", "/channels"); D.channels = c.channels || []; } catch { D.channels = []; }
  if (!D.channels.find(c => c.id === D.channel)) D.channel = (D.channels[0] || {}).id || "";
  $("#chSel").innerHTML = D.channels.map(c => `<option value="${E(c.id)}"${c.id === D.channel ? " selected" : ""}>${E(c.name || c.id)}</option>`).join("") || `<option value="">no channels</option>`;
}
async function loadModels() {
  try { const m = await api("GET", "/ai/models"); D.models = (m.models || []).map(x => x.id).filter(Boolean); } catch {}
  let dl = $("#modelList"); if (!dl) { dl = document.createElement("datalist"); dl.id = "modelList"; document.body.appendChild(dl); }
  dl.innerHTML = D.models.map(id => `<option value="${E(id)}">`).join("");
}
async function guardDirty(what) { return !D.dirty || await confirmDlg(what, "The canvas has unsaved changes. Discard them?", "Discard", "onair"); }
async function openCustom(id) {
  try { const rec = await api("GET", "/cg/designer/templates/" + encodeURIComponent(id)); loadDoc(rec.html, Object.assign(blankMeta(), rec, { versionCount: (rec.versions || []).length })); D.packNote = false; history.replaceState(null, "", "?open=" + encodeURIComponent(id)); toast("Opened " + rec.name, "ok"); renderRight(); }
  catch (e) { toast("Open failed: " + e.message, "err"); }
}
async function openPackCopy(t) {
  try {
    const r = await fetch("/cg/" + t.file, { cache: "no-store" }); if (!r.ok) throw new Error("HTTP " + r.status);
    loadDoc(await r.text(), Object.assign(blankMeta(), { name: t.name + " (copy)", category: t.category || "custom", role: t.role || "lower_third", description: "Based on " + t.name }), { dirty: true });
    D.packNote = true; history.replaceState(null, "", "?copy=" + encodeURIComponent(t.id)); toast("Opened a copy of " + t.name + ": Save stores it as your own template");
  } catch (e) { toast("Open failed: " + e.message, "err"); }
}
async function openDialog() {
  if (!(await guardDirty("Open"))) return;
  const list = allTemplates(), mine = list.filter(t => t.custom), pack = list.filter(t => !t.custom);
  const card = t => `<div class="tcard" data-tid="${E(t.id)}" data-custom="${t.custom ? 1 : ""}"><div class="n">${E(t.name)}</div><div class="c">${E(t.custom ? (t.category || "custom") : t.category || "")}</div></div>`;
  dlg("Open", `<h4>My designs</h4>${mine.length ? `<div class="tlist">${mine.map(card).join("")}</div>` : '<div class="mu">None saved yet.</div>'}<h4 style="margin-top:14px">Pack templates <span class="mu sm">(opens a copy)</span></h4><div class="tlist">${pack.map(card).join("")}</div>`, [{ label: "Close" }],
    d => d.querySelectorAll(".tcard").forEach(c => c.onclick = () => { d.close(); const t = list.find(x => x.id === c.dataset.tid); if (c.dataset.custom) openCustom(t.id); else openPackCopy(t); }));
}
async function save(asCopy) {
  if (!D.info) { toast(D.infoErr || "Designer service not reachable", "err"); return; }
  let name = ($("#tplName").value || D.meta.name || "").trim();
  if (!name) { name = await new Promise(res => dlg("Name this graphic", `<input id="nmIn" style="width:100%" placeholder="e.g. Reporter lower third (navy)">`, [{ label: "Cancel", fn: () => res("") }, { label: "Save", cls: "primary", fn: () => res(($("#nmIn").value || "").trim()) }], d => setTimeout(() => d.querySelector("#nmIn").focus(), 30))); if (!name) return; }
  if (asCopy && D.meta.id && name === D.meta.name) name += " copy";
  D.meta.name = name; $("#tplName").value = name;
  const sent = serialize(true);
  const body = { name, html: sent, description: D.meta.description || "", category: D.meta.category || "custom", role: D.meta.role || "lower_third", prompt: D.meta.prompt || "", examples: D.meta.examples || [], model: D.meta.model || "", note: asCopy ? "saved as copy" : "" };
  if (D.meta.id && !asCopy) body.id = D.meta.id;
  try {
    const r = await api("POST", "/cg/designer/templates", body);
    D.meta.id = r.id; D.meta.versionCount = (r.versions || []).length || r.versionCount || D.meta.versionCount; D.packNote = false;
    if (r.html && r.html.trim() !== sent.trim()) { const keepU = D.undo.slice(); loadDoc(r.html, D.meta, {}); D.undo = keepU; }
    setDirty(false); history.replaceState(null, "", "?open=" + encodeURIComponent(r.id));
    toast((asCopy ? "Saved a copy as " : "Saved ") + r.id + ((r.warnings || []).length ? " · " + r.warnings.join("; ") : ""), (r.warnings || []).length ? "" : "ok");
    loadInfo(); renderRight();
  } catch (e) { toast("Save failed: " + e.message, "err"); }
}
async function showHistory() {
  if (!D.meta.id) { toast("Save first: versions are kept per saved template"); return; }
  let rec; try { rec = await api("GET", "/cg/designer/templates/" + encodeURIComponent(D.meta.id)); } catch (e) { toast(e.message, "err"); return; }
  const vs = (rec.versions || []).slice().reverse();
  dlg("History · " + E(rec.name), vs.length ? `<div class="drafts" style="grid-template-columns:repeat(auto-fill,minmax(220px,1fr))">${vs.map(v => `<div class="draft" data-vn="${v.n}"><div class="th"><iframe tabindex="-1" scrolling="no" data-vh="${v.n}"></iframe></div><div class="cap"><b>v${v.n}</b><span class="mu sm">${new Date(v.savedMs).toLocaleString()}</span><span class="fill"></span><span class="mu sm">${E(v.note || "")}</span></div></div>`).join("")}</div>` : '<div class="mu">No earlier versions yet: each save keeps the previous page (last 10).</div>', [{ label: "Close" }],
    d => { d.querySelectorAll("iframe[data-vh]").forEach(f => { const v = vs.find(x => String(x.n) === f.dataset.vh); f.srcdoc = withBase(v.html); }); setTimeout(() => d.querySelectorAll(".draft .th").forEach(th => th.querySelector("iframe").style.transform = `scale(${th.clientWidth / W})`), 20);
      d.querySelectorAll(".draft").forEach(c => c.onclick = () => { const v = vs.find(x => String(x.n) === c.dataset.vn); d.close(); replaceDoc(v.html); toast("Loaded v" + v.n + " into the canvas: Save makes it current, Undo goes back", "ok"); }); });
}
async function delTemplate() {
  if (!D.meta.id) { toast("Not saved: nothing to delete"); return; }
  if (!(await confirmDlg("Delete template", `Delete <b>${E(D.meta.name)}</b> (${E(D.meta.id)})? Presets, defaults and CG slots that use it stop working.`, "Delete", "onair"))) return;
  try { await api("DELETE", "/cg/designer/templates/" + encodeURIComponent(D.meta.id)); toast("Deleted", "ok"); D.meta.id = null; setDirty(true); history.replaceState(null, "", location.pathname); loadInfo(); renderRight(); } catch (e) { toast(e.message, "err"); }
}
function exportHtml() {
  const a = document.createElement("a"); a.href = URL.createObjectURL(new Blob([serialize(true)], { type: "text/html" }));
  a.download = (D.meta.id || (D.meta.name || "graphic").toLowerCase().replace(/[^a-z0-9]+/g, "_")) + ".html"; a.click(); setTimeout(() => URL.revokeObjectURL(a.href), 2000);
}
function animSpec() { return { in: $("#animIn").value, out: $("#animOut").value, duration: +$("#animDur").value || 600 }; }
async function take() {
  if (!D.meta.id) { toast("Save the template first", "err"); return; }
  if (D.dirty) { toast("Save first: Take uses the saved page", "err"); return; }
  const ch = D.channel, chn = (D.channels.find(c => c.id === ch) || {}).name || ch; if (!ch) { toast("No channel", "err"); return; }
  if (!(await confirmDlg("Take on air", `Deploy <b>${E(D.meta.name)}</b> on <b>${E(chn)}</b>? It goes <b style="color:var(--onair)">on air</b> with its default text.`, "Take", "onair"))) return;
  try { const r = await api("POST", "/overlays/" + encodeURIComponent(ch) + "/deploy", { template: D.meta.id, params: {}, animation: animSpec(), role: D.meta.role, mode: "auto", label: D.meta.name }); D.lastLayer[ch] = r.layer ?? (r.state && r.state.layer); toast("On air: layer " + (D.lastLayer[ch] ?? "?"), "ok"); }
  catch (e) { toast("Take failed: " + e.message, "err"); }
}
async function hideOnAir() {
  const ch = D.channel, layer = D.lastLayer[ch];
  if (layer === undefined || layer === null) { toast("Nothing taken from here on this channel (use the Overlays page)"); return; }
  try { await api("POST", "/overlays/" + encodeURIComponent(ch) + "/" + layer + "/hide", {}); toast("Hidden", "ok"); delete D.lastLayer[ch]; } catch (e) { toast("Hide failed: " + e.message, "err"); }
}
function playAnim(dir) {
  const d = frameDoc(), w = d && d.defaultView, root = d && d.querySelector("[data-cg-root]"); if (!w || !w.CGAnimations || !root) { toast("The page has no root / animation runtime (Template ▸ Checks ▸ Fix)", "err"); return; }
  const a = animSpec();
  if (dir === "in") { root.style.opacity = "0"; root.style.visibility = "hidden"; root.style.animation = ""; setTimeout(() => w.CGAnimations.animateIn(root, a.in, a.duration, "ease-out", 0, () => drawOverlay()), 60); }
  else w.CGAnimations.animateOut(root, a.out, a.duration, "ease-in", () => setTimeout(() => { root.style.animation = ""; w.CGAnimations.animateIn(root, "none", 0, "linear", 0); drawOverlay(); }, 700));
}
function updateStatus() {
  const se = srcEl(D.sel), r = D.sel && rectOf(D.sel);
  $("#stSel").textContent = se ? `${label(se)}${r ? `  ${r1(r.x)},${r1(r.y)}  ${r1(r.w)}×${r1(r.h)}` : ""}` : "Nothing selected";
  const p = D.doc ? deriveParams() : []; $("#stInfo").textContent = `${p.length} field(s) · ${D.undo.length} undo`;
  $("#bUndo").disabled = !D.undo.length; $("#bRedo").disabled = !D.redo.length;
}
function setView(v) {
  D.view = v; document.querySelectorAll("[data-view]").forEach(b => b.classList.toggle("on", b.dataset.view === v));
  $("#stageWrap").classList.toggle("hide", v === "code"); $("#codeWrap").classList.toggle("show", v === "code");
  if (v === "code") $("#code").value = serialize(true); else fit();
}

// ------------------------------------------------------------------------------------------------ keyboard
function onKey(e) {
  const t = e.target, typing = t && (/^(INPUT|TEXTAREA|SELECT)$/.test(t.tagName) || t.isContentEditable);
  const k = e.key.toLowerCase(), mod = e.ctrlKey || e.metaKey;
  if (mod && k === "s") { e.preventDefault(); save(e.shiftKey); return; }
  if (typing || D.editing || $("#dlg").open) return;
  if (mod && k === "z") { e.preventDefault(); e.shiftKey ? redo() : undo(); return; }
  if (mod && k === "y") { e.preventDefault(); redo(); return; }
  if (D.view !== "design") return;
  if (mod && k === "d") { e.preventDefault(); dupSel(); return; }
  if ((k === "delete" || k === "backspace") && D.sel) { e.preventDefault(); delSel(); return; }
  if (k === "escape") { if (D.sel) { const se = srcEl(D.sel); const p = se && se.parentElement && se.parentElement.closest("[data-cge]"); D.sel = e.shiftKey && p ? p.getAttribute("data-cge") : null; drawOverlay(); refreshAll(); } return; }
  if (k === "enter" && D.sel) { e.preventDefault(); startTextEdit(D.sel); return; }
  const step = e.shiftKey ? 10 : 1, arrows = { arrowleft: [-step, 0], arrowright: [step, 0], arrowup: [0, -step], arrowdown: [0, step] };
  if (arrows[k] && D.sel) { e.preventDefault(); nudge(...arrows[k]); }
}

// ------------------------------------------------------------------------------------------------ boot
function wire() {
  const ov = $("#overlay");
  ov.addEventListener("mousedown", onDown);
  ov.addEventListener("dblclick", e => { const p = toFrame(e), id = hitTest(p.x, p.y); if (id) { select(id); startTextEdit(id); } });
  window.addEventListener("mousemove", onMove); window.addEventListener("mouseup", onUp);
  window.addEventListener("keydown", onKey);
  window.addEventListener("resize", () => { fit(); fitThumbs(); });
  window.addEventListener("beforeunload", e => { if (D.dirty) { e.preventDefault(); e.returnValue = ""; } });
  document.addEventListener("paste", e => { if (D.ltab !== "ai") return; const it = [...(e.clipboardData || {}).items || []].find(i => i.type.startsWith("image/")); if (it) { e.preventDefault(); addImageExample(it.getAsFile()); toast("Image added as an example", "ok"); } });
  for (const id of ["#leftPane", "#rightPane"]) { const p = $(id); p.addEventListener("input", onPaneInput); p.addEventListener("change", onPaneInput); p.addEventListener("click", onPaneClick); }
  document.querySelectorAll("[data-ltab]").forEach(b => b.onclick = () => { D.ltab = b.dataset.ltab; renderLeft(); });
  document.querySelectorAll("[data-rtab]").forEach(b => b.onclick = () => { D.rtab = b.dataset.rtab; renderRight(); });
  document.querySelectorAll("[data-view]").forEach(b => b.onclick = () => setView(b.dataset.view));
  $("#tplName").addEventListener("input", e => { D.meta.name = e.target.value; setDirty(true); });
  $("#bNew").onclick = async () => { if (await guardDirty("New")) { loadDoc(BLANK, blankMeta()); D.packNote = false; history.replaceState(null, "", location.pathname); } };
  $("#bOpen").onclick = openDialog;
  $("#bImport").onclick = async () => { if (!(await guardDirty("Import"))) return; const p = $("#fileHtml"); p.onchange = async () => { const f = p.files[0]; p.value = ""; if (!f) return; loadDoc(await readFile(f, true), Object.assign(blankMeta(), { name: f.name.replace(/\.html?$/i, "") }), { dirty: true }); }; p.click(); };
  $("#bSave").onclick = () => save(false); $("#bSaveAs").onclick = () => save(true);
  $("#bHistory").onclick = showHistory; $("#bExport").onclick = exportHtml; $("#bDelete").onclick = delTemplate;
  $("#bUndo").onclick = undo; $("#bRedo").onclick = redo;
  $("#chSel").onchange = e => { D.channel = e.target.value; if ($("#bgSel").value === "program") setBackdrop("program"); };
  $("#bTake").onclick = take; $("#bHide").onclick = hideOnAir;
  $("#zoomSel").value = D.zoom; $("#zoomSel").onchange = e => { D.zoom = e.target.value; LS.set("cgd.zoom", D.zoom); fit(); };
  $("#bgSel").onchange = e => setBackdrop(e.target.value);
  const tog = (id, k, ls) => { const b = $(id); b.classList.toggle("on", D[k]); b.onclick = () => { D[k] = !D[k]; LS.set(ls, D[k] ? "1" : "0"); b.classList.toggle("on", D[k]); drawOverlay(); }; };
  tog("#bGuides", "guides", "cgd.guides"); tog("#bSnap", "snap", "cgd.snap");
  $("#bPlayIn").onclick = () => playAnim("in"); $("#bPlayOut").onclick = () => playAnim("out");
  $("#bCodeApply").onclick = () => { const v = $("#code").value; if (!/<html|<body|<div/i.test(v)) { toast("That does not look like a page", "err"); return; } replaceDoc(v); setView("design"); toast("Applied (Undo goes back)", "ok"); };
  $("#bCodeReset").onclick = () => { $("#code").value = serialize(true); };
  const fl = document.createElement("datalist"); fl.id = "fontList"; fl.innerHTML = FONTS.map(f => `<option value="${E(f)}">`).join(""); document.body.appendChild(fl);
}
async function boot() {
  wire(); fit();
  const q = new URLSearchParams(location.search);
  await Promise.all([loadInfo(), loadChannels()]); loadModels();
  if (q.get("open")) { loadDoc(BLANK, blankMeta()); await openCustom(q.get("open")); }
  else if (q.get("copy")) { const t = allTemplates().find(x => x.id === q.get("copy")); loadDoc(BLANK, blankMeta()); if (t) await openPackCopy(t); }
  else {
    let draft = null; try { draft = JSON.parse(localStorage.getItem("cgd.draft") || "null"); } catch {}
    loadDoc(BLANK, blankMeta());
    if (draft && draft.html && Date.now() - draft.at < 7 * 86400000 && await confirmDlg("Unsaved work", `Restore the unsaved design${draft.meta && draft.meta.name ? " <b>" + E(draft.meta.name) + "</b>" : ""} from ${new Date(draft.at).toLocaleString()}?`, "Restore")) loadDoc(draft.html, draft.meta, { dirty: true });
  }
  if (q.get("prompt")) { D.gen.prompt = q.get("prompt"); D.ltab = "ai"; $("#leftPane").dataset.ai = ""; renderLeft(); }
}
boot();
