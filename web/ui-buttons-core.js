/* ui-buttons-core.js: the web dashboard's button builders (button system design notes 3.9), pure functions with no DOM, so
   tests/test_web_uibuttons.cpp runs them in QJSEngine without Node. ui-buttons.js adds the sprite, the safety state and the
   editor on top. Every builder returns an HTML string and escapes with the same rules as app.js's esc() / jsq(): esc() for
   text and attribute values, jsq() for a value inside an inline handler (the attribute is entity-decoded before the script
   is parsed, so an id like a');evil();// must become a JSON string literal, then be entity-escaped). The look comes from
   ui-buttons.css and the generated ui-tokens.css; the catalogue, roles, sizes, hotkey rules and safety defaults from the
   generated ui-spec.js. The hotkey and prefs logic mirrors Play_Play_Native/qml/PlayPlay/Buttons/ButtonPrefs.qml, so a
   setting both UIs read (`ui.buttons`) is cleaned the same way on both sides. */
import * as Spec from "./ui-spec.js";

export { Spec };
export const esc = s => String(s ?? "").replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
export const jsq = v => esc(JSON.stringify(String(v ?? "")));
const isObj = o => o !== null && typeof o === "object" && !Array.isArray(o);

// The customisable surfaces (design 3.8), the same table as ButtonPrefs.surfaces: the sizes the editor may offer, the page
// whose reserved keys apply ("" = visible on every page, so every reserved key applies) and whether order / hidden apply.
export const surfaces = {
  "onair.deck": { sizes: ["l", "xl"], page: "onair", deck: true },
  "onair.switch": { sizes: ["l"], page: "onair", deck: true },
  "onair.bus": { sizes: ["l"], page: "onair", deck: false },
  "column.transport": { sizes: ["s", "m"], page: "", deck: true },
  "graphics.cg": { sizes: ["m", "l"], page: "onair", deck: false },
  "graphics.quick": { sizes: ["s", "m"], page: "onair", deck: false },
  "remote.pad": { sizes: Spec.limits.padSizes, page: "remote", deck: false }
};
export const sizesOf = surface => (surfaces[surface] ? surfaces[surface].sizes : Spec.sizeOrder);
export const pageOf = surface => (surfaces[surface] ? surfaces[surface].page : "");
export const safetyIndex = level => Spec.safetyLevels.indexOf(level);
// Never below the floor; raising is always allowed; an unknown level counts as none.
export function clampSafety(level, floor) {
  const l = Math.max(0, safetyIndex(level)), f = Math.max(0, safetyIndex(floor));
  return Spec.safetyLevels[Math.max(l, f)];
}
export const colorOk = c => typeof c === "string" && new RegExp(Spec.limits.colorPattern).test(c);
export const iconOk = name => typeof name === "string" && (name === "" || Spec.icons[name] !== undefined);
export const roleOk = r => typeof r === "string" && Spec.roles[r] !== undefined;
export const sizeOk = s => typeof s === "string" && Spec.sizes[s] !== undefined;

// ---------------------------------------------------------------------------------------------------------------------
// Icons and glyphs
// ---------------------------------------------------------------------------------------------------------------------
// <use> of a sprite symbol (ui-buttons.js injects web/icons.svg once). "" for an unknown name, so a typo shows nothing rather
// than a broken image; test_uispec's source scan catches literal names that are not drawn.
export function icon(name, opts = {}) {
  if (!iconOk(name) || name === "") return "";
  const cls = "ic" + (opts.cls ? " " + opts.cls : "");
  const style = opts.size ? ` style="width:${Number(opts.size)}px;height:${Number(opts.size)}px"` : "";
  return `<svg class="${esc(cls)}"${style} aria-hidden="true" focusable="false"><use href="#i-${esc(name)}"/></svg>`;
}
// Glyph keys longest first, so "◀❘" wins over "◀" and "❘▶" is not split.
const glyphKeys = Object.keys(Spec.glyphs).sort((a, b) => b.length - a.length);
// icon name -> the glyph map's verb ("close" -> "Remove"), the fallback accessible name of an icon-only button.
export const iconTips = {};
for (const g of Object.keys(Spec.glyphs)) if (iconTips[Spec.glyphs[g].icon] === undefined) iconTips[Spec.glyphs[g].icon] = Spec.glyphs[g].tip;
// A button's text split into its leading glyph and the rest (design 3.6): "✕" -> {icon:"close", tip:"Remove", rest:""};
// "+ Add node" -> {icon:"plus", tip:"Add", rest:"Add node"}; "Save" -> null. The glyph must be the whole text or be followed
// by whitespace, so "A+" and "+1" are left alone.
export function glyphToIcon(text) {
  const s = String(text ?? "").trim();
  if (s === "") return null;
  for (const g of glyphKeys) {
    if (!s.startsWith(g)) continue;
    const rest = s.slice(g.length);
    if (rest !== "" && !/^\s/.test(rest)) continue;
    const m = Spec.glyphs[g];
    return { glyph: g, icon: m.icon, tip: m.tip, rest: rest.trim() };
  }
  return null;
}

// ---------------------------------------------------------------------------------------------------------------------
// Builders
// ---------------------------------------------------------------------------------------------------------------------
// A DOM id for a button id: stable across renders (app.js's render() restores focus by element id) and safe as an id
// selector. "ch1/transport.stop" -> "b-ch1-transport.stop".
export const domId = id => "b-" + String(id).replace(/[^A-Za-z0-9_.:-]/g, "-");
const legacyFill = { neutral: "outline", ghost: "ghost" };
// fill "auto" keeps today's mapping (design 3.2): neutral -> outline on bg3, coloured roles -> solid, active -> tonal.
export function fillFor(role, fill, active) {
  if (fill && fill !== "auto") return fill;
  if (active) return Spec.autoFill.active;
  if (role === "neutral") return Spec.autoFill.neutral;
  return Spec.autoFill.coloured;
}
const ledOk = l => l === "red" || l === "green" || l === "amber" || l === "blue";

// btn(opts) -> <button>. opts: id (needed for anything stateful: safety, busy, focus survival), label, sublabel, icon,
// iconRight, role, fill, size, tip, hotkey, safety, confirm, action (an inline handler expression string, already built
// with jsq() for its arguments), disabled, active, led, ledBlink, badge, busy, cls, attrs (extra attribute string),
// armLabel, color (#rrggbb override of the role tint), span (pad: 1|2).
// With safety "none" and no id the click runs `action` directly, exactly like today's inline onclick. A button with an id
// goes through ppui.fire (ui-buttons.js), which keeps the armed / holding / busy state in a Map keyed by id, so the state
// survives the once-a-second re-render; the action string travels in data-action.
export function btn(o = {}) {
  const role = roleOk(o.role) ? o.role : "neutral";
  const size = sizeOk(o.size) ? o.size : "m";
  const fill = fillFor(role, o.fill, !!o.active);
  const safety = safetyIndex(o.safety) >= 0 ? o.safety : "none";
  const label = String(o.label ?? "");
  const iconOnly = label === "" && !!o.icon;
  const tip = String(o.tip ?? "");
  const name = tip || label || (o.icon && iconTips[o.icon]) || "";
  const hotkey = o.hotkey ? normHotkey(o.hotkey) : "";
  const title = tip ? tip + (hotkey ? " (" + hotkey + ")" : "") : (hotkey ? (label || name) + " (" + hotkey + ")" : (iconOnly ? name : ""));
  const cls = ["pb", "pb-" + size, "r-" + role, "f-" + fill];
  if (iconOnly) cls.push("pb-icon");
  if (o.active) cls.push("is-active");
  if (o.busy) cls.push("is-busy");
  if (o.span === 2) cls.push("pb-span2");
  if (o.cls) cls.push(String(o.cls));
  const stateful = o.id !== undefined && o.id !== null && o.id !== "";
  const a = [`type="button"`, `class="${esc(cls.join(" "))}"`];
  if (stateful) a.push(`id="${esc(domId(o.id))}"`, `data-pb="${esc(o.id)}"`);
  if (title) a.push(`title="${esc(title)}"`);
  if (iconOnly || tip) a.push(`aria-label="${esc(name)}"`);
  if (o.active !== undefined && o.toggle) a.push(`aria-pressed="${o.active ? "true" : "false"}"`);
  if (o.disabled || o.busy) a.push("disabled");
  if (o.busy) a.push(`aria-busy="true"`);
  if (safety !== "none" || stateful) {
    a.push(`data-safety="${esc(safety)}"`);
    if (o.confirm) a.push(`data-confirm="${esc(o.confirm)}"`);
    if (safety === "arm" || safety === "hold") a.push(`data-arm-label="${esc(o.armLabel || Spec.states.armSublabel)}"`);
    if (o.action) a.push(`data-action="${o.action}"`);
    if (o.action) a.push(safety === "hold" ? `onclick="return false"` : `onclick="ppui.fire(this,event)"`);
  } else if (o.action) a.push(`onclick="${o.action}"`);
  const style = [];
  if (colorOk(o.color)) style.push("--pb-tint:" + o.color.toLowerCase());
  if (style.length) a.push(`style="${esc(style.join(";"))}"`);
  if (o.attrs) a.push(String(o.attrs));
  const parts = [];
  if (o.led && ledOk(o.led)) parts.push(`<span class="led led-${esc(o.led)}${o.ledBlink === "slow" || o.ledBlink === "fast" ? " blink-" + o.ledBlink : ""}" aria-hidden="true"></span>`);
  if (o.icon) parts.push(icon(o.icon));
  if (label !== "") parts.push(`<span class="pb-label">${esc(label)}</span>`);
  if (o.sublabel) parts.push(`<span class="pb-sub">${esc(o.sublabel)}</span>`);
  if (o.iconRight) parts.push(icon(o.iconRight, { cls: "ic-right" }));
  if (o.badge) parts.push(`<span class="pb-badge">${esc(o.badge)}</span>`);
  return `<button ${a.join(" ")}>${parts.join("")}</button>`;
}
// A square icon-only button. `tip` is its accessible name: without one the glyph map's verb is used, and failing that the
// icon name, so a screen reader never gets an empty button.
export function iconBtn(o = {}) {
  const tip = o.tip || (o.icon && iconTips[o.icon]) || String(o.icon || "Button");
  return btn(Object.assign({ fill: "ghost", size: "s" }, o, { label: "", tip }));
}
// A chip (xs): tonal when selected, ghost otherwise; aria-pressed says which.
export function chip(o = {}) {
  return btn(Object.assign({ size: "xs" }, o, { toggle: true, active: !!o.active, fill: o.active ? "tonal" : (o.fill && o.fill !== "auto" ? o.fill : "outline") }));
}
// A remote-pad key (design 3.8, web/remote.js): xl, solid in the pad theme's colour for the button's role, or the button's
// own `color`; size and span from the (optional) style fields; the sub line names the node / channel / hotkey.
const padRoles = ["neutral", "go", "stop", "warn", "info", "accent"];
export function padBtn(b = {}, theme = {}, o = {}) {
  const role = padRoles.includes(b.role) ? b.role : "neutral";
  const color = colorOk(b.color) ? b.color : (colorOk(theme[role]) ? theme[role] : "");
  const size = Spec.limits.padSizes.includes(b.size) ? b.size : "xl";
  const sub = [o.node, b.channel, b.hotkey ? normHotkey(b.hotkey) || b.hotkey : ""].filter(Boolean).join(" · ");
  return btn({
    id: o.id !== undefined ? o.id : (b.id !== undefined ? "remote.pad/" + b.id : undefined),
    label: b.label, sublabel: sub, icon: iconOk(b.icon) ? b.icon : "", role, fill: "solid", size, color, span: b.span === 2 ? 2 : 1,
    tip: o.tip, hotkey: b.hotkey, safety: b.confirm ? "arm" : "none", action: o.action, disabled: o.disabled, busy: o.busy,
    active: o.active, cls: "pad-key" + (o.cls ? " " + o.cls : ""), attrs: o.attrs
  });
}

// ---------------------------------------------------------------------------------------------------------------------
// Hotkeys (the same rules as ButtonPrefs.qml, with hotkeyRules.allowedWeb: single keys are allowed on the web, but only
// while no field has focus, which is the caller's rule as in remote.js)
// ---------------------------------------------------------------------------------------------------------------------
const named = ["Return", "Enter", "Escape", "Delete", "Insert", "Space", "Tab", "Backspace", "Home", "End", "PgUp", "PgDown", "Left", "Right", "Up", "Down", "Pause", "Print", "Menu"];
function keyName(p) {
  if (p.length === 1) return p.toUpperCase();
  const f = /^f(\d{1,2})$/i.exec(p);
  if (f) { const n = parseInt(f[1], 10); return n >= 1 && n <= 35 ? "F" + n : ""; }
  const n = named.find(x => x.toLowerCase() === p.toLowerCase());
  return n !== undefined ? n : "";
}
// Canonical chord: modifiers in modifierOrder, aliases folded, one key (a character upper-cased, Fn or a named key). "" for
// nothing, a chord without a key ("Ctrl+") or two keys.
export function normHotkey(seq) {
  const s = String(seq ?? "").trim();
  if (s === "") return "";
  const parts = []; let cur = "";
  for (const c of s) { if (c === "+" && cur !== "") { parts.push(cur); cur = ""; } else if (c === "+") cur = "+"; else cur += c; }
  if (cur !== "") parts.push(cur);
  const rules = Spec.hotkeyRules, aliases = {};
  for (const k in rules.aliases) aliases[k.toLowerCase()] = rules.aliases[k];
  const mods = {}; let key = "";
  for (let p of parts) {
    p = p.trim(); if (p === "") return "";
    const low = p.toLowerCase();
    if (aliases[low] !== undefined) p = aliases[low];
    const asMod = rules.modifierOrder.find(m => m.toLowerCase() === p.toLowerCase());
    if (asMod !== undefined) { mods[asMod] = true; continue; }
    if (key !== "") return "";
    key = keyName(p); if (key === "") return "";
  }
  if (key === "") return "";
  const out = rules.modifierOrder.filter(m => mods[m]); out.push(key);
  return out.join("+");
}
// A KeyboardEvent as a chord ("Ctrl+Shift+S", "F9", "Num+Enter"); "" for a modifier alone.
export function eventHotkey(e) {
  if (!e || !e.key || ["Control", "Shift", "Alt", "Meta", "OS"].includes(e.key)) return "";
  const parts = [];
  if (e.ctrlKey) parts.push("Ctrl"); if (e.altKey) parts.push("Alt"); if (e.shiftKey) parts.push("Shift"); if (e.metaKey) parts.push("Meta");
  if (e.location === 3) parts.push("Num");
  let k = e.key;
  if (k === " ") k = "Space"; else if (k === "ArrowLeft") k = "Left"; else if (k === "ArrowRight") k = "Right"; else if (k === "ArrowUp") k = "Up"; else if (k === "ArrowDown") k = "Down";
  else if (k === "PageUp") k = "PgUp"; else if (k === "PageDown") k = "PgDown";
  else if (k.length === 1 && e.code && /^(Key|Digit)/.test(e.code)) k = e.code.replace(/^(Key|Digit)/, "");   // the unshifted key, so Shift+1 is not "!"
  parts.push(k);
  return normHotkey(parts.join("+"));
}
export const hotkeyAllowed = seq => { const n = normHotkey(seq); return n !== "" && new RegExp(Spec.hotkeyRules.allowedWeb).test(n); };
export function reservedFor(page) {
  const r = Spec.reservedKeys; let out = r.global.slice();
  if (page === "") { for (const k in r) if (k !== "global") out = out.concat(r[k]); }
  else if (r[page] !== undefined) out = out.concat(r[page]);
  return out.map(normHotkey).filter(x => x !== "");
}
export const hotkeyReserved = (seq, surface) => { const n = normHotkey(seq); return n !== "" && reservedFor(pageOf(surface)).includes(n); };
// The other binding of `seq` that clashes with surface/id (same page, or either side visible on every page); null when free.
export function hotkeyConflict(seq, surface, id, buttons) {
  const n = normHotkey(seq); if (n === "" || !isObj(buttons)) return null;
  const page = pageOf(surface);
  for (const key in buttons) {
    const o = buttons[key];
    if (!isObj(o) || o.hotkey === undefined || normHotkey(o.hotkey) !== n) continue;
    const at = key.indexOf("/"), s2 = at > 0 ? key.slice(0, at) : "", id2 = at > 0 ? key.slice(at + 1) : key;
    if (s2 === surface && id2 === id) continue;
    const p2 = pageOf(s2);
    if (page === "" || p2 === "" || page === p2) return { surface: s2, id: id2, hotkey: n };
  }
  return null;
}
// {ok, seq, reason, conflict}: the normalised chord and why it is refused; an empty sequence is ok (no binding).
export function hotkeyCheck(seq, surface, id, buttons) {
  if (String(seq ?? "").trim() === "") return { ok: true, seq: "", reason: "", conflict: null };
  const n = normHotkey(seq);
  if (n === "") return { ok: false, seq: "", reason: "not a key chord", conflict: null };
  if (!hotkeyAllowed(n)) return { ok: false, seq: n, reason: "not allowed: use a letter, digit, F9-F12, Ctrl / Alt / Shift + a letter or digit, or a keypad key", conflict: null };
  if (hotkeyReserved(n, surface)) return { ok: false, seq: n, reason: "reserved by the application", conflict: null };
  const c = hotkeyConflict(n, surface, id, buttons);
  if (c) return { ok: false, seq: n, reason: "taken by " + c.surface + "/" + c.id, conflict: c };
  return { ok: true, seq: n, reason: "", conflict: null };
}
export const hotkeyOk = (seq, surface, id, buttons) => hotkeyCheck(seq, surface, id, buttons).ok;

// ---------------------------------------------------------------------------------------------------------------------
// Prefs (the `ui.buttons` setting, design 3.8)
// ---------------------------------------------------------------------------------------------------------------------
// One button's overrides cleaned for its surface: {clean, errors}. Unknown fields are dropped silently, invalid values with
// a note; null and "" mean "no override".
export function cleanButton(surface, id, given, buttons) {
  const out = {}, errors = [], where = surface + "/" + id;
  if (!isObj(given)) return { clean: out, errors: [where + ": not an object"] };
  const patch = {};
  for (const k in given) if (given[k] !== null && given[k] !== "") patch[k] = given[k];
  if (patch.label !== undefined) {
    if (typeof patch.label !== "string") errors.push(where + ": label is not a string");
    else { const l = patch.label.trim(); if (l.length > Spec.limits.labelMax) { errors.push(where + ": label cut to " + Spec.limits.labelMax + " characters"); out.label = l.slice(0, Spec.limits.labelMax); } else if (l !== "") out.label = l; }
  }
  if (patch.icon !== undefined) { if (iconOk(patch.icon)) { if (patch.icon !== "") out.icon = patch.icon; } else errors.push(where + ": unknown icon \"" + patch.icon + "\""); }
  if (patch.color !== undefined) { if (colorOk(patch.color)) out.color = patch.color.toLowerCase(); else errors.push(where + ": colour \"" + patch.color + "\" is not #rrggbb"); }
  if (patch.size !== undefined) { if (sizesOf(surface).includes(patch.size)) out.size = patch.size; else errors.push(where + ": size \"" + patch.size + "\" is not offered on " + surface); }
  if (patch.safety !== undefined) { if (safetyIndex(patch.safety) >= 0) out.safety = patch.safety; else errors.push(where + ": unknown safety \"" + patch.safety + "\""); }
  if (patch.hotkey !== undefined && String(patch.hotkey).trim() !== "") {
    const c = hotkeyCheck(patch.hotkey, surface, id, buttons);
    if (c.ok) out.hotkey = c.seq; else errors.push(where + ": hotkey \"" + patch.hotkey + "\" " + c.reason);
  }
  return { clean: out, errors };
}
export const emptyPrefs = () => ({ version: 1, surfaces: {}, buttons: {} });
// The whole setting cleaned: {ok, clean, errors}; ok is false only when it is not an object (or not JSON) at all.
export function validatePrefs(obj) {
  const errors = [], clean = emptyPrefs();
  if (typeof obj === "string") { try { obj = JSON.parse(obj); } catch (e) { return { ok: false, clean, errors: ["not JSON: " + e] }; } }
  if (!isObj(obj)) return { ok: false, clean, errors: ["not an object"] };
  if (obj.version !== undefined && obj.version !== 1) errors.push("version " + obj.version + " (this is 1)");
  if (obj.surfaces !== undefined) {
    if (!isObj(obj.surfaces)) errors.push("surfaces is not an object");
    else for (const s in obj.surfaces) {
      if (!surfaces[s]) { errors.push("unknown surface \"" + s + "\""); continue; }
      const v = obj.surfaces[s], o = {};
      if (!isObj(v)) { errors.push(s + ": not an object"); continue; }
      if (v.order !== undefined) {
        if (!Array.isArray(v.order)) errors.push(s + ": order is not a list");
        else { const seen = {}; o.order = v.order.filter(x => typeof x === "string" && x !== "" && (x === "|" || !seen[x]) && (seen[x] = true)); if (o.order.length !== v.order.length) errors.push(s + ": order had entries that are not key ids"); }
      }
      if (v.hidden !== undefined) { if (!Array.isArray(v.hidden)) errors.push(s + ": hidden is not a list"); else o.hidden = v.hidden.filter(x => typeof x === "string" && x !== "" && x !== "|"); }
      if (Object.keys(o).length) clean.surfaces[s] = o;
    }
  }
  if (obj.buttons !== undefined) {
    if (!isObj(obj.buttons)) errors.push("buttons is not an object");
    else for (const key in obj.buttons) {
      const at = key.indexOf("/");
      if (at <= 0 || at === key.length - 1) { errors.push("button key \"" + key + "\" is not surface/id"); continue; }
      const s = key.slice(0, at), id = key.slice(at + 1);
      if (!surfaces[s]) { errors.push("unknown surface in \"" + key + "\""); continue; }
      const r = cleanButton(s, id, obj.buttons[key], clean.buttons);
      errors.push(...r.errors);
      if (Object.keys(r.clean).length) clean.buttons[key] = r.clean;
    }
  }
  return { ok: true, clean, errors };
}
// The key's look and behaviour: `defaults` ({label, icon, role, color, size, hotkey, safety, floor, tip ...}) with the valid
// overrides applied; `safety` never below `floor`; `custom` lists the fields an override changed. `surface` picks the sizes
// that are offered ("" = any size).
export function resolvePrefs(defaults, overrides, surface = "") {
  const d = isObj(defaults) ? defaults : {}, r = {};
  for (const k in d) r[k] = d[k];
  if (r.label === undefined) r.label = ""; if (r.icon === undefined) r.icon = ""; if (r.color === undefined) r.color = "";
  if (r.size === undefined) r.size = sizesOf(surface)[0]; if (r.hotkey === undefined) r.hotkey = ""; if (r.floor === undefined) r.floor = "none";
  r.safety = clampSafety(r.safety === undefined ? "none" : r.safety, r.floor);
  r.custom = [];
  const o = overrides;
  if (isObj(o)) {
    if (typeof o.label === "string" && o.label !== "" && o.label.length <= Spec.limits.labelMax) { r.label = o.label; r.custom.push("label"); }
    if (typeof o.icon === "string" && o.icon !== "" && Spec.icons[o.icon] !== undefined) { r.icon = o.icon; r.custom.push("icon"); }
    if (colorOk(o.color)) { r.color = o.color.toLowerCase(); r.custom.push("color"); }
    if (typeof o.size === "string" && sizesOf(surface).includes(o.size)) { r.size = o.size; r.custom.push("size"); }
    if (typeof o.hotkey === "string" && o.hotkey !== "" && hotkeyAllowed(o.hotkey) && !hotkeyReserved(o.hotkey, surface)) { r.hotkey = normHotkey(o.hotkey); r.custom.push("hotkey"); }
    if (typeof o.safety === "string" && safetyIndex(o.safety) >= 0) { const s = clampSafety(o.safety, r.floor); if (s !== r.safety) r.custom.push("safety"); r.safety = s; }
  }
  return r;
}
// The defaults of a station action from the spec (safetyDefaults), for the web: {label, icon, role, tip, hotkey, safety,
// floor}; `state` "off" picks the stateful variant (automation.arm when disarmed, engine.toggle when off).
export function actionDefaults(actionId, state) {
  const a = Spec.safetyDefaults[actionId];
  if (!a) return null;
  const v = state === "off" && isObj(a.off) ? Object.assign({}, a, a.off) : a;
  return { label: v.label || "", icon: v.icon || "", role: roleOk(v.role) ? v.role : "neutral", tip: v.tip || "", hotkey: v.hotkey || "",
    safety: v.web || "none", floor: v.floor || "none" };
}
