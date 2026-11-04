/* ui-buttons.js: the web dashboard's button runtime (button system design notes 3.9). Loaded as a module before app.js from
   index.html; it needs no change in the modules, which keep building HTML strings and replacing #view.innerHTML about once
   a second on telemetry. It
   - injects web/icons.svg once, hidden at the top of <body>, so icon() <use>s resolve (pending <use> references resolve when
     the sprite lands, so a view rendered before the fetch answers is fine);
   - exposes window.ppui: fire(el) with the safety model (none / arm / hold / modal), busy and the 250 ms debounce. The
     armed / holding / busy state lives in a Map keyed by the button's data-pb id, not in the DOM, and is re-applied to the
     fresh elements after every render, so an armed STOP stays armed through the re-render (and still drains on time);
   - upgrade(root): after each render, a <button> whose text is a single known glyph (or starts with one and a space) gets the
     icon, an aria-label and a title; a button with a data-icon attribute gets that icon in front. Only <button>s, never other content.
     This covers all 17 modules before their own migration (UXB-15);
   - openEditor(spec, {surface, floor, buttons}) -> Promise<spec|null>: the button editor as a <dialog>, the web twin of
     ButtonEditor.qml (label, icon, colour, size, hotkey, safety, reset).
   Legacy inline onclick handlers keep working: a button without an id runs its action straight from onclick. */
import * as Core from "./ui-buttons-core.js";
const { Spec, esc, icon, glyphToIcon, normHotkey, eventHotkey, hotkeyCheck, cleanButton, clampSafety, safetyIndex } = Core;

const T = Spec.timing;
const state = new Map();                      // id -> {armedAt, armTimer, holding, holdTimer, holdAt, busy, lastFire, error}
const st = id => { let s = state.get(id); if (!s) { s = {}; state.set(id, s); } return s; };
const byId = id => document.querySelector(`button[data-pb="${CSS.escape(id)}"]`);
const now = () => performance.now();

// --- safety ----------------------------------------------------------------------------------------------------------
function runAction(el, s) {
  const expr = el.getAttribute("data-action") || "";
  if (!expr) return;
  let r;
  try { r = new Function("event", expr).call(el, null); } catch (e) { console.error("ppui: action failed", expr, e); flashError(el.getAttribute("data-pb")); return; }
  if (r && typeof r.then === "function" && s) {
    setBusy(el.getAttribute("data-pb"), true);
    r.then(() => setBusy(el.getAttribute("data-pb"), false), e => { setBusy(el.getAttribute("data-pb"), false); flashError(el.getAttribute("data-pb")); if (e) console.warn("ppui: action rejected", expr, e); });
  }
}
function flashError(id) {
  const s = st(id); s.error = true; apply(id);
  setTimeout(() => { s.error = false; apply(id); }, T.animMs + 510);     // the 600 ms error flash of the design
}
function setBusy(id, on) { const s = st(id); s.busy = !!on; apply(id); }
function disarm(id) { const s = state.get(id); if (!s || !s.armedAt) return; clearTimeout(s.armTimer); s.armedAt = 0; s.armTimer = 0; apply(id); }
function disarmAll() { for (const id of state.keys()) disarm(id); }
function arm(id) {
  disarmAll();                                   // one armed key at a time, like RemotePad.armedKey
  const s = st(id); s.armedAt = now(); s.armTimer = setTimeout(() => disarm(id), T.armMs); apply(id);
}
function confirmModal(text) {
  const d = document.getElementById("confirm"), p = document.getElementById("confirmText");
  if (!d || !p || typeof d.showModal !== "function") return Promise.resolve(window.confirm(text));
  return new Promise(res => { p.textContent = text; d.returnValue = ""; d.showModal(); d.onclose = () => res(d.returnValue === "ok"); });
}
// The click of a safety button: none fires (after the debounce); arm arms, then a second click within armMs fires; modal
// asks first; hold ignores clicks (it fires from the pointer timer).
function fire(el, ev) {
  if (!(el instanceof Element)) el = byId(String(el));
  if (!el || el.disabled) return;
  const id = el.getAttribute("data-pb") || ""; const s = st(id);
  if (s.busy) return;
  const safety = el.getAttribute("data-safety") || "none";
  const t = now();
  if (safety === "hold") return;
  if (safety === "arm") {
    if (!s.armedAt) { arm(id); return; }
    disarm(id);
  } else if (safety === "modal") {
    const text = el.getAttribute("data-confirm") || ((el.getAttribute("aria-label") || el.textContent.trim() || "Proceed") + "?");
    if (s.lastFire && t - s.lastFire < T.minIntervalMs) return;
    s.lastFire = t;
    confirmModal(text).then(ok => { if (ok) runAction(el, s); });
    return;
  }
  if (s.lastFire && t - s.lastFire < T.minIntervalMs) return;
  s.lastFire = t;
  runAction(el, s);
}
function holdStart(el) {
  const id = el.getAttribute("data-pb") || ""; const s = st(id);
  if (s.busy || s.holding) return;
  s.holding = true; s.holdAt = now();
  s.holdTimer = setTimeout(() => { s.holding = false; s.holdTimer = 0; apply(id); const cur = byId(id); if (cur && !cur.disabled) { s.lastFire = now(); runAction(cur, s); } }, T.holdMs);
  apply(id);
}
function holdEnd(id) { const s = state.get(id); if (!s || !s.holding) return; clearTimeout(s.holdTimer); s.holding = false; s.holdTimer = 0; apply(id); }
function holdEndAll() { for (const id of state.keys()) holdEnd(id); }
// Classes and the elapsed time (so the CSS drain / fill animation continues where it was) on the current element of a key.
function apply(id) {
  const el = byId(id); if (!el) return;
  const s = state.get(id) || {};
  el.classList.toggle("is-armed", !!s.armedAt);
  el.classList.toggle("is-holding", !!s.holding);
  el.classList.toggle("is-busy", !!s.busy);
  el.classList.toggle("is-error", !!s.error);
  if (s.armedAt) el.style.setProperty("--pb-elapsed", Math.round(now() - s.armedAt) + "ms");
  else if (s.holding) el.style.setProperty("--pb-elapsed", Math.round(now() - s.holdAt) + "ms");
  else el.style.removeProperty("--pb-elapsed");
  if (s.busy) el.setAttribute("aria-busy", "true"); else el.removeAttribute("aria-busy");
}
function applyAll(root) {
  for (const [id, s] of state) if (s.armedAt || s.holding || s.busy || s.error) { const el = byId(id); if (el && (!root || root.contains(el))) apply(id); }
}

// --- upgrade: glyph buttons become icon buttons ---------------------------------------------------------------------
function upgrade(root = document) {
  const btns = root.querySelectorAll ? root.querySelectorAll("button:not([data-up])") : [];
  for (const b of btns) {
    b.setAttribute("data-up", "1");
    const named = b.getAttribute("data-icon");
    if (named) {
      if (Spec.icons[named] && !b.querySelector("svg.ic")) {
        b.insertAdjacentHTML("afterbegin", icon(named));
        if (!b.textContent.trim()) b.classList.add("pb-icon");
      }
      if (!b.getAttribute("aria-label")) b.setAttribute("aria-label", b.getAttribute("title") || b.textContent.trim() || named);
      continue;
    }
    if (b.classList.contains("pb") || b.children.length) continue;          // already built by btn(), or has markup of its own
    const text = b.textContent;
    const g = glyphToIcon(text);
    if (!g) continue;
    const tip = b.getAttribute("title") || "";
    b.innerHTML = icon(g.icon) + (g.rest ? `<span class="pb-label">${esc(g.rest)}</span>` : "");
    if (!g.rest) { b.classList.add("pb-icon"); b.setAttribute("aria-label", tip || g.tip); if (!tip) b.setAttribute("title", g.tip); }
    else if (!b.getAttribute("aria-label") && tip) b.setAttribute("aria-label", tip);
  }
}
let pending = false;
function scheduleUpgrade(root) { if (pending) return; pending = true; requestAnimationFrame(() => { pending = false; upgrade(root); applyAll(root); }); }
function watch(root) {
  if (!root || root._ppuiWatched) return;
  root._ppuiWatched = true;
  new MutationObserver(() => scheduleUpgrade(root)).observe(root, { childList: true, subtree: true });
  scheduleUpgrade(root);
}

// --- editor ------------------------------------------------------------------------------------------------------------
let dlg = null;
function editorDialog() {
  if (dlg) return dlg;
  dlg = document.createElement("dialog"); dlg.id = "ppuiEditor"; dlg.className = "ppui-editor";
  document.body.appendChild(dlg);
  return dlg;
}
const roleChips = Spec.roleOrder;
function editorHtml(spec, opt) {
  const sizes = Core.sizesOf(opt.surface);
  const floor = opt.floor || "none";
  const groups = Spec.iconGroups.map(g => `<optgroup label="${esc(g.label)}">${g.icons.map(i => `<option value="${esc(i)}" ${spec.icon === i ? "selected" : ""}>${esc(i)}</option>`).join("")}</optgroup>`).join("");
  return `<form method="dialog" class="ppui-form">
    <h3>Customize ${esc(opt.title || spec.label || opt.id || "button")}</h3>
    <div class="ppui-preview" id="ppuiPreview"></div>
    <label class="f">Label <input name="label" maxlength="${Spec.limits.labelMax}" value="${esc(spec.label || "")}" placeholder="${esc(opt.defaults && opt.defaults.label || "")}"></label>
    <label class="f">Icon <select name="icon"><option value="">none</option>${groups}</select></label>
    <div class="f"><span>Colour</span><div class="row ppui-swatches">${["", ...Spec.swatches].map(c => `<button type="button" class="ppui-swatch${(spec.color || "") === c ? " sel" : ""}" data-color="${esc(c)}" title="${esc(c || "role colour")}" aria-label="${esc(c || "role colour")}" style="${c ? "background:" + esc(c) : ""}"></button>`).join("")}
      <input name="color" size="8" maxlength="7" placeholder="#rrggbb" value="${esc(spec.color || "")}"></div></div>
    <div class="f"><span>Size</span><div class="row">${sizes.map(s => `<label class="ppui-radio"><input type="radio" name="size" value="${s}" ${(spec.size || sizes[0]) === s ? "checked" : ""}> ${s.toUpperCase()}</label>`).join("")}</div></div>
    <label class="f">Hotkey <input name="hotkey" value="${esc(spec.hotkey || "")}" placeholder="press a key chord" autocomplete="off"><small id="ppuiHotkeyMsg" class="ppui-msg"></small></label>
    <label class="f">Safety <select name="safety">${Spec.safetyLevels.map(l => `<option value="${l}" ${safetyIndex(l) < safetyIndex(floor) ? "disabled" : ""} ${(spec.safety || floor) === l ? "selected" : ""}>${l}${safetyIndex(l) < safetyIndex(floor) ? " (below the floor " + esc(floor) + ")" : ""}</option>`).join("")}</select></label>
    <div class="row end ppui-actions"><button type="button" class="ghost" value="reset" id="ppuiReset">Reset</button><span class="fill"></span><button value="cancel" class="ghost">Cancel</button><button value="ok" class="primary">Apply</button></div>
  </form>`;
}
// spec: {label, icon, color, size, hotkey, safety}; opt: {surface, id, floor, title, defaults, buttons (the other stored
// overrides for the hotkey conflict check)}. Resolves with the cleaned spec, {} on Reset, null on Cancel.
function openEditor(spec = {}, opt = {}) {
  const d = editorDialog();
  const surface = opt.surface || "", id = opt.id || "", floor = opt.floor || "none";
  d.innerHTML = editorHtml(spec, { ...opt, floor });
  const f = d.querySelector("form"), prev = d.querySelector("#ppuiPreview"), msg = d.querySelector("#ppuiHotkeyMsg");
  const read = () => {
    const fd = new FormData(f);
    return { label: String(fd.get("label") || ""), icon: String(fd.get("icon") || ""), color: String(fd.get("color") || "").trim(), size: String(fd.get("size") || ""), hotkey: String(fd.get("hotkey") || ""), safety: String(fd.get("safety") || "") };
  };
  const preview = () => {
    const v = read(), dflt = opt.defaults || {};
    const r = Core.resolvePrefs({ ...dflt, floor }, cleanButton(surface, id, v, opt.buttons).clean, surface);
    prev.innerHTML = Core.btn({ label: r.label, icon: r.icon, role: dflt.role || "neutral", size: r.size, color: r.color, hotkey: r.hotkey, tip: dflt.tip, sublabel: r.safety !== "none" ? r.safety : "" });
    const c = hotkeyCheck(v.hotkey, surface, id, opt.buttons);
    msg.textContent = c.ok ? (c.seq ? c.seq : "") : c.reason; msg.classList.toggle("bad", !c.ok);
    d.querySelectorAll(".ppui-swatch").forEach(s => s.classList.toggle("sel", s.getAttribute("data-color") === v.color));
  };
  f.addEventListener("input", preview); f.addEventListener("change", preview);
  d.querySelectorAll(".ppui-swatch").forEach(s => s.addEventListener("click", () => { f.elements.color.value = s.getAttribute("data-color"); preview(); }));
  const hk = f.elements.hotkey;
  hk.addEventListener("keydown", e => {
    if (e.key === "Tab") return;
    e.preventDefault();
    if (e.key === "Backspace" || e.key === "Delete") { hk.value = ""; preview(); return; }
    const seq = eventHotkey(e); if (seq) { hk.value = seq; preview(); }
  });
  preview();
  return new Promise(res => {
    d.querySelector("#ppuiReset").onclick = () => { d.returnValue = "reset"; d.close("reset"); };
    d.onclose = () => {
      if (d.returnValue === "ok") { const v = read(); const c = cleanButton(surface, id, v, opt.buttons); c.errors.forEach(e => console.warn("ppui editor:", e)); res(c.clean); }
      else if (d.returnValue === "reset") res({});
      else res(null);
    };
    d.showModal();
  });
}

// --- wiring --------------------------------------------------------------------------------------------------------
document.addEventListener("pointerdown", e => {
  const b = e.target && e.target.closest ? e.target.closest("button[data-pb][data-safety='hold']") : null;
  if (b && !b.disabled && e.button === 0) { holdStart(b); }
}, true);
for (const ev of ["pointerup", "pointercancel"]) document.addEventListener(ev, holdEndAll, true);
document.addEventListener("pointerleave", e => { if (e.target && e.target.matches && e.target.matches("button[data-pb][data-safety='hold']")) holdEnd(e.target.getAttribute("data-pb")); }, true);
document.addEventListener("keydown", e => { if (e.key === "Escape") disarmAll(); }, true);
document.addEventListener("click", e => {
  // A click anywhere else disarms (the armed key fires only from its own second click).
  const b = e.target && e.target.closest ? e.target.closest("button[data-pb]") : null;
  for (const [id, s] of state) if (s.armedAt && (!b || b.getAttribute("data-pb") !== id)) disarm(id);
}, true);

window.ppui = {
  ...Core, fire, arm, disarm, disarmAll, busy: setBusy, flashError, isArmed: id => !!(state.get(id) || {}).armedAt, isBusy: id => !!(state.get(id) || {}).busy,
  upgrade, watch, apply: applyAll, openEditor, state
};

// The sprite: fetched once, inserted hidden at the top of <body>. Failing that the icons are just absent; the buttons keep
// their labels and titles.
async function loadSprite() {
  try {
    const r = await fetch("icons.svg", { cache: "force-cache" });
    if (!r.ok) throw new Error("HTTP " + r.status);
    const txt = await r.text();
    const box = document.createElement("div"); box.id = "ppuiSprite"; box.setAttribute("aria-hidden", "true"); box.style.cssText = "position:absolute;width:0;height:0;overflow:hidden";
    box.innerHTML = txt.replace(/<!--[\s\S]*?-->/g, "");
    document.body.insertBefore(box, document.body.firstChild);
  } catch (e) { console.warn("ppui: icons.svg not loaded:", e); }
}
const start = () => { watch(document.getElementById("view")); upgrade(document.getElementById("top") || document); };
if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", start); else start();
await loadSprite();
