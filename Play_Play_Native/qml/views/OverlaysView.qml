import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".."
import "../ui"

/**
 * Overlays: the HTML CG pack on air through the Supervisor's overlay manager (/api/v1/overlays/*). Template gallery
 * (native cards: role placement mock-up + name), a deploy form generated from the manifest params, the live layer board
 * (update / re-take / hide / delete), hotkey presets (Shortcut), always-on defaults and the renderer status.
 * Every row and card has a CMenu context menu (copy, paste, duplicate, copy to all channels, move, delete, properties).
 * The web dashboard's Overlays page additionally has live iframe previews, rules and cues.
 */
Item {
    id: view
    property var channelsData: ({})
    property var presets: []
    property var templates: []
    property var animPresets: []
    property var animations: ["none", "fade", "slide-left", "slide-right", "slide-up", "slide-down", "wipe-left", "wipe-right", "bounce", "zoom", "flip", "typewriter"]
    property var renderer: ({})
    property string down: ""
    property string tab: "presets"
    property string cat: ""
    property string query: ""
    property string selId: ""
    property var params: ({})
    property string animIn: "fade"
    property string animOut: "fade"
    property int inMs: 600
    property int outMs: 400
    property string role: ""
    property int slot: 0
    property real hold: 0
    property string mode: "auto"
    property var editing: null          // {kind: "preset"|"default"|"layer", id, ch, layer}
    property bool busy: false
    property bool clearDefs: false
    property int tick: 0
    readonly property string ch: supervisor.activeChannelId
    readonly property var roles: ["lower_third", "bug", "ticker", "secondary", "legal_id", "emergency"]
    readonly property var modes: ["auto", "html", "native"]
    readonly property var reserved: ["F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "Space", "Ctrl+Right", "Ctrl+L", "Ctrl+=", "Ctrl+-", "Ctrl+0", "Ctrl+C", "Ctrl+X", "Ctrl+V", "Delete"]
    readonly property var layers: { var c = channelsData[ch]; var l = c && c.layers ? c.layers.slice() : []; l.sort(function(a, b) { return a.layer - b.layer }); return l }
    readonly property var defaults: { var c = channelsData[ch]; return c && c.defaults ? c.defaults : [] }
    readonly property var sel: { for (var i = 0; i < templates.length; ++i) if (templates[i].id === selId) return templates[i]; return null }
    readonly property var categories: { var c = []; for (var i = 0; i < templates.length; ++i) if (c.indexOf(templates[i].category) < 0) c.push(templates[i].category); return c }
    readonly property var gallery: {
        var q = query.toLowerCase(), out = []
        for (var i = 0; i < templates.length; ++i) { var t = templates[i]; if (t.listed === false) continue
            if (q.length ? (t.name + " " + t.id + " " + (t.description || "") + " " + t.category).toLowerCase().indexOf(q) >= 0 : (!cat || t.category === cat)) out.push(t) }
        return out
    }
    readonly property var hotkeyPresets: {
        var out = []
        for (var i = 0; i < presets.length; ++i) { var s = seqOf(presets[i].hotkey); if (s.length && reserved.indexOf(s) < 0) out.push({ id: presets[i].id, seq: s }) }
        return out
    }
    // Text input only (cursorPosition, or an editable combo's editText): buttons have `text` too and take focus on click, so testing
    // `text` silenced every preset hotkey after any click (UXB-0). Same text-input test as main.qml's Space guard.
    readonly property bool typing: { var w = view.Window.window; var f = w ? w.activeFocusItem : null; return !!(f && (f.hasOwnProperty("cursorPosition") || (f.hasOwnProperty("editText") && f.editable === true))) }

    // ---- helpers -------------------------------------------------------------------------------------------------------
    function clone(o) { return o === undefined ? undefined : JSON.parse(JSON.stringify(o)) }
    function enc(s) { return encodeURIComponent(s) }
    function seqOf(h) {
        h = String(h || "").trim(); if (!h.length) return ""
        var parts = h.split("+"), ctrl = false, alt = false, key = ""
        for (var i = 0; i < parts.length; ++i) { var p = parts[i].trim().toLowerCase(); if (!p.length) continue
            if (p === "ctrl" || p === "control" || p === "cmd" || p === "meta") ctrl = true; else if (p === "alt" || p === "option") alt = true; else if (p !== "shift") key = p }
        if (!key.length) return ""
        key = key.length > 1 ? key.charAt(0).toUpperCase() + key.slice(1) : key.toUpperCase()
        return (ctrl ? "Ctrl+" : "") + (alt ? "Alt+" : "") + key
    }
    function chName(id) { var l = supervisor.channels || []; for (var i = 0; i < l.length; ++i) if (l[i].profile && l[i].profile.id === id) return l[i].profile.name || id; return id }
    function allCh() { var out = [], l = supervisor.channels || []; for (var i = 0; i < l.length; ++i) if (l[i].profile && !(l[i].profile.extra && l[i].profile.extra.hidden)) out.push(l[i].profile.id); return out }
    function tplOf(id) { for (var i = 0; i < templates.length; ++i) if (templates[i].id === id) return templates[i]; return null }
    function tplName(id) { var t = tplOf(id); return t ? t.name : (id || "") }
    function pval(p) { return params[p.name] !== undefined ? String(params[p.name]) : (p["default"] !== undefined && p["default"] !== null ? String(p["default"]) : "") }
    function paramsOf(t, vals) {
        var o = {}; vals = vals || {}; var ps = t ? t.params || [] : []
        for (var i = 0; i < ps.length; ++i) { var v = vals[ps[i].name] !== undefined ? String(vals[ps[i].name]) : (ps[i]["default"] !== undefined ? String(ps[i]["default"]) : ""); if (v.length) o[ps[i].name] = v }
        for (var k in vals) { var known = false; for (var j = 0; j < ps.length; ++j) if (ps[j].name === k) known = true; if (!known && String(vals[k]).length) o[k] = String(vals[k]) }
        return o
    }
    function summary(p) { var out = []; for (var k in (p || {})) { if (/^(color|colou?r\d?|logo|image|url)$/i.test(k)) continue; var v = String(p[k]); if (v.length) out.push(v.length > 26 ? v.substring(0, 25) + "…" : v); if (out.length >= 3) break } return out.join(" · ") }
    function anim() { return { "in": animIn, out: animOut, duration: inMs, outDuration: outMs, easing: "ease-out" } }
    function normAnim(a) { a = a || {}; return { "in": a["in"] || "fade", out: a.out || "fade", duration: a.duration !== undefined ? a.duration : (a.inDuration !== undefined ? a.inDuration : 600), outDuration: a.outDuration !== undefined ? a.outDuration : (a.duration !== undefined ? a.duration : 400), easing: a.easing || "ease-out" } }
    function formSpec() {
        var t = sel; if (!t) return null
        var s = { template: t.id, params: paramsOf(t, params), animation: anim(), role: role || t.role || "", mode: mode, holdSec: hold }
        if (slot > 0) s.slot = slot
        return s
    }
    function defaultSpec(t) { return { template: t.id, params: paramsOf(t, {}), animation: anim(), role: t.role || "", mode: mode, holdSec: 0 } }
    function specOfLayer(l) { var s = { template: l.template, params: clone(l.params || {}), animation: normAnim(l.animation), role: l.role || "", mode: (l.mode === "html" || l.mode === "native") ? l.mode : "auto", holdSec: 0 }; if (l.slot > 0) s.slot = l.slot; return s }
    function loadSpec(s) {
        selId = s.template || s.templateId || selId; var p = {}; var src = s.params || {}; for (var k in src) p[k] = String(src[k]); params = p
        var a = normAnim(s.animation); animIn = a["in"]; animOut = a.out; inMs = a.duration; outMs = a.outDuration
        role = s.role || ""; slot = s.slot || 0; hold = s.holdSec || 0; mode = s.mode || "auto"
    }
    function deployBody(s) { var b = { template: s.template, params: s.params || {}, animation: s.animation || anim(), mode: s.mode || "auto" }; if (s.role) b.role = s.role; if (s.slot > 0) b.slot = s.slot; if (s.holdSec > 0) b.holdSec = s.holdSec; return b }
    function listOf(v, key) { if (Array.isArray(v)) return v; if (v && Array.isArray(v[key])) return v[key]; if (v && Array.isArray(v.items)) return v.items; if (v && Array.isArray(v._array)) return v._array; return [] }
    function uid(p) { return p + Date.now().toString(36) + Math.floor(Math.random() * 1296).toString(36) }
    function upsert(c, rec) {
        if (!rec || rec.layer === undefined) return
        var d = clone(channelsData); d[c] = d[c] || { layers: [], defaults: [] }; var L = d[c].layers || []; var f = false
        for (var i = 0; i < L.length; ++i) if (L[i].layer === rec.layer) { L[i] = rec; f = true }
        if (!f) L.push(rec); d[c].layers = L; channelsData = d
    }
    function isUp(s) { return s === "on_air" || s === "shown" || s === "pending" }
    function stateColor(s) { return s === "on_air" || s === "shown" ? Theme.ready : s === "lost" || s === "failed" || s === "error" ? Theme.danger : s === "pending" ? Theme.warn : s === "hiding" ? Theme.standby : s === "suppressed" ? Theme.accent2 : Theme.text3 }
    function fmtAge(ms) { var s = Math.max(0, Math.floor(ms / 1000)), m = Math.floor(s / 60); s = s % 60; return m + ":" + (s < 10 ? "0" : "") + s }
    function clipOk(kinds) { var c = window.clip; return !c || kinds.indexOf(c.kind) >= 0 }
    function toastErr(prefix, d) { window.toast(prefix + (d && d.error ? ": " + d.error : ""), "error") }

    // ---- data ------------------------------------------------------------------------------------------------------------
    function reload() {
        api.get("/overlays", function(ok, d) {
            if (!ok || !d) { down = "The overlay manager is not available on this Supervisor (GET /overlays failed). Update and restart the Supervisor."; if (!templates.length) api.get("/cg/templates", function(ok2, m) { if (ok2 && m) setManifest(m) }); return }
            down = ""; renderer = d.renderer || {}
            if (d.channels) { var n = {}; for (var c in d.channels) n[c] = { layers: listOf(d.channels[c].layers, "layers"), defaults: listOf(d.channels[c].defaults, "defaults") }; channelsData = n }
            if (d.presets) presets = listOf(d.presets, "presets")
            if (d.templates) setManifest(d.templates)
        })
    }
    function setManifest(m) { var man = Array.isArray(m) ? { templates: m } : m; templates = man.templates || []; animPresets = man.presets || []; if (man.animations && man.animations.length) animations = man.animations }
    function putList(path, key, list, cb) { var b = {}; b[key] = list; b.items = list; b._array = list; api.put(path, b, function(ok, d) { if (ok) { if (cb) cb(d, list) } else toastErr("Save failed", d) }) }
    function savePresets(list) { putList("/overlays/presets", "presets", list, function(d, l) { var v = d && (Array.isArray(d) ? d : (d.presets || d.items || d._array)); presets = Array.isArray(v) ? v : l }) }
    function saveDefaults(c, list) { putList("/overlays/" + enc(c) + "/defaults", "defaults", list, function(d, l) { var v = d && (Array.isArray(d) ? d : (d.defaults || d.items || d._array)); var n = clone(channelsData); n[c] = n[c] || { layers: [], defaults: [] }; n[c].defaults = Array.isArray(v) ? v : l; channelsData = n }) }
    function deployTo(chs, spec) {
        if (!spec || !spec.template) { window.toast("Pick a template first", "info"); return }
        busy = true; var left = chs.length, ok = 0
        for (var i = 0; i < chs.length; ++i) (function(c) {
            api.post("/overlays/" + enc(c) + "/deploy", deployBody(spec), function(good, d) {
                if (good) { upsert(c, d); ok++ } else toastErr("Deploy to " + chName(c) + " failed", d)
                if (--left === 0) { busy = false; if (ok) window.toast("Deployed " + tplName(spec.template) + " → " + ok + " channel(s)", "ok"); reload() }
            })
        })(chs[i])
    }
    function layerCall(c, layer, op, body, msg) { api.post("/overlays/" + enc(c) + "/" + layer + "/" + op, body || {}, function(ok, d) { if (ok) { upsert(c, d); if (msg) window.toast(msg, "ok") } else toastErr(op + " failed", d); reload() }) }
    function deleteLayer(c, layer) { api.del("/overlays/" + enc(c) + "/" + layer, function(ok, d) { if (ok) window.toast("Layer " + layer + " deleted", "ok"); else toastErr("Delete failed", d); reload() }) }
    function clearCh(chs, roles) {
        for (var i = 0; i < chs.length; ++i) { var b = { includeDefaults: clearDefs }; if (roles && roles.length) b.roles = roles; api.post("/overlays/" + enc(chs[i]) + "/clear", b, function(ok, d) { if (!ok) toastErr("Clear failed", d); reload() }) }
        window.toast("Cleared", "ok")
    }
    function fire(p, chs, hide) { for (var i = 0; i < chs.length; ++i) (function(c) { api.post("/overlays/" + enc(c) + "/presets/" + enc(p.id) + (hide ? "/hide" : "/fire"), {}, function(ok, d) { if (ok) upsert(c, d); else toastErr((hide ? "Hide " : "Fire ") + p.name + " failed", d); reload() }) })(chs[i]); window.toast((hide ? "Hide " : "Fire ") + p.name, "ok") }
    function newPreset(spec, name) { return { id: uid("p"), name: name || tplName(spec.template), hotkey: "", template: spec.template, params: clone(spec.params || {}), animation: normAnim(spec.animation), role: spec.role || "", mode: spec.mode || "auto", holdSec: spec.holdSec || 0 } }
    function newDefault(spec) { return { id: uid("d"), template: spec.template, params: clone(spec.params || {}), animation: normAnim(spec.animation), role: spec.role || "", mode: spec.mode || "auto", enabled: true } }
    function addPreset(spec, name) { if (!spec) { window.toast("Pick a template first", "info"); return } savePresets(presets.concat([newPreset(spec, name)])) }
    function addDefault(c, spec) {
        if (!spec) { window.toast("Pick a template first", "info"); return }
        var cd = channelsData[c] ? channelsData[c].defaults || [] : []; saveDefaults(c, cd.concat([newDefault(spec)]))
    }
    function pasteSpec() {
        var c = window.clip; if (c && (c.kind === "overlay" || c.kind === "preset" || c.kind === "default")) return clone(c.data)
        var t = ctx.pasteText(); if (!t.length) return null
        try { var o = JSON.parse(t); if (Array.isArray(o)) o = o[0]; if (o && typeof o === "object" && (o.template || o.templateId || o.params)) return o } catch (e) { }
        window.toast("The clipboard does not hold an overlay JSON", "error"); return null
    }
    function move(list, id, d) { var a = list.slice(), i = -1; for (var k = 0; k < a.length; ++k) if (a[k].id === id) i = k; if (i < 0) return a; var it = a.splice(i, 1)[0]; var to = d < -1000 ? 0 : d > 1000 ? a.length : Math.max(0, Math.min(a.length, i + d)); a.splice(to, 0, it); return a }
    function chTargets(label, fn) { var subs = allCh().map(function(c) { return { text: chName(c), fn: function() { fn([c]) } } }); subs.push({ sep: true }); subs.push({ text: "All channels", fn: function() { fn(allCh()) } }); return { text: label, sub: subs } }
    function moveSub(list, id, save) { return { text: "Move", sub: [{ text: "Up", fn: function() { save(view.move(list, id, -1)) } }, { text: "Down", fn: function() { save(view.move(list, id, 1)) } }, { text: "To top", fn: function() { save(view.move(list, id, -9999)) } }, { text: "To bottom", fn: function() { save(view.move(list, id, 9999)) } }] } }
    function pickTemplate(id) { if (id !== selId) { selId = id; params = {}; role = ""; slot = 0 } }
    function applyEdit() {
        var e = editing, s = formSpec(); if (!e || !s) return
        if (e.kind === "layer") { var l = null, L = layers; for (var i = 0; i < L.length; ++i) if (L[i].layer === e.layer) l = L[i]; var merged = l ? clone(l.params || {}) : {}; for (var k in s.params) merged[k] = s.params[k]; layerCall(e.ch, e.layer, "update", { params: merged }, "Layer " + e.layer + " updated"); editing = null; return }
        if (e.kind === "preset") { var a = presets.slice(); for (var j = 0; j < a.length; ++j) if (a[j].id === e.id) { var n = clone(a[j]); n.template = s.template; n.params = s.params; n.animation = s.animation; n.role = s.role; n.mode = s.mode; n.holdSec = s.holdSec; a[j] = n } savePresets(a) }
        else { var d = defaults.slice(); for (var m = 0; m < d.length; ++m) if (d[m].id === e.id) { var q = clone(d[m]); q.template = s.template; q.params = s.params; q.animation = s.animation; q.role = s.role; q.mode = s.mode; d[m] = q } saveDefaults(ch, d) }
        editing = null; window.toast("Saved", "ok")
    }

    // ---- menus ---------------------------------------------------------------------------------------------------------
    function layerMenu(l) {
        var spec = specOfLayer(l), c = ch, same = 0
        for (var i = 0; i < layers.length; ++i) if (layers[i].role === l.role && layers[i].layer !== l.layer) same++
        ctx.show([{ header: "Layer " + l.layer + " · " + (l.role || "") },
            { text: "Update fields…", fn: function() { view.loadSpec(spec); view.editing = { kind: "layer", ch: c, layer: l.layer } } },
            { text: "Re-take", fn: function() { view.layerCall(c, l.layer, "retake", {}, "Layer " + l.layer + " re-taken") } },
            { text: "Hide", enabled: isUp(l.state), fn: function() { view.layerCall(c, l.layer, "hide", {}, "Layer " + l.layer + " hidden") } }, { sep: true },
            { text: "Copy", sub: [{ text: "Copy overlay (paste elsewhere)", fn: function() { window.setClip("overlay", spec, l.name || l.template); ctx.copyJson(spec, l.name || l.template) } }, { text: "Copy params JSON", fn: function() { ctx.copyJson(l.params || {}, "params") } }, { text: "Copy template id", fn: function() { ctx.copyText(l.template) } }] },
            { text: "Paste params into this layer", enabled: clipOk(["overlay", "preset", "default"]), fn: function() { var s = view.pasteSpec(); if (s) { var m = view.clone(l.params || {}), p = s.params || {}; for (var k in p) m[k] = p[k]; view.layerCall(c, l.layer, "update", { params: m }, "Params pasted into layer " + l.layer) } } },
            { text: "Paste as new overlay here", enabled: clipOk(["overlay", "preset", "default"]), fn: function() { var s = view.pasteSpec(); if (s && (s.template || s.templateId)) view.deployTo([c], s) } }, { sep: true },
            { text: "Duplicate (next slot)", fn: function() { var s = view.clone(spec); s.slot = (spec.slot || 0) + 1; view.deployTo([c], s) } },
            chTargets("Copy to channel", function(chs) { view.deployTo(chs, spec) }),
            { text: "Save as preset…", fn: function() { view.addPreset(spec, l.label || l.name) } },
            { text: "Set as default", sub: [{ text: "This channel", fn: function() { view.addDefault(c, spec) } }, { text: "All channels", fn: function() { var a = view.allCh(); for (var i = 0; i < a.length; ++i) view.addDefault(a[i], spec) } }] },
            { text: "Load into the deploy form", fn: function() { view.loadSpec(spec); view.editing = null } }, { sep: true },
            { text: "Delete layer", danger: true, fn: function() { view.deleteLayer(c, l.layer) } },
            { text: "Delete all " + (l.role || "other") + " layers (" + (same + 1) + ")", danger: true, enabled: same > 0, fn: function() { view.clearCh([c], [l.role]) } }, { sep: true },
            { text: "Properties…", fn: function() { ctx.showProps("Layer " + l.layer, l) } }])
    }
    function tplMenu(t) {
        var c = ch, spec = function() { return t.id === selId ? view.formSpec() : view.defaultSpec(t) }
        ctx.show([{ header: t.name },
            { text: "Deploy to " + chName(c), fn: function() { view.deployTo([c], spec()) } }, chTargets("Deploy to", function(chs) { view.deployTo(chs, spec()) }),
            { text: "Edit in the form", fn: function() { view.pickTemplate(t.id); view.editing = null } }, { sep: true },
            { text: "Copy", sub: [{ text: "Copy URL", fn: function() { ctx.copyText(api.rawUrl("/cg/" + t.id + ".html")) } }, { text: "Copy template id", fn: function() { ctx.copyText(t.id) } }, { text: "Copy name", fn: function() { ctx.copyText(t.name) } }, { text: "Copy as overlay (paste elsewhere)", fn: function() { var s = spec(); window.setClip("overlay", s, t.name); ctx.copyJson(s, t.name) } }, { text: "Copy manifest JSON", fn: function() { ctx.copyJson(t, t.name) } }] },
            { text: "Paste params into the form", enabled: clipOk(["overlay", "preset", "default"]), fn: function() { var s = view.pasteSpec(); if (s) { s.template = t.id; view.loadSpec(s); view.editing = null } } }, { sep: true },
            { text: "Add to presets…", fn: function() { view.addPreset(spec(), t.name) } },
            { text: "Add as default", sub: [{ text: "This channel", fn: function() { view.addDefault(c, spec()) } }, { text: "All channels", fn: function() { var a = view.allCh(); for (var i = 0; i < a.length; ++i) view.addDefault(a[i], spec()) } }] },
            { text: "Open preview in the browser", fn: function() { Qt.openUrlExternally(api.rawUrl("/cg/" + t.id + ".html")) } },
            { text: "Delete (templates are files in web/cg)", enabled: false }, { sep: true },
            { text: "Properties…", fn: function() { ctx.showProps(t.name, t) } }])
    }
    function presetMenu(p) {
        var c = ch
        ctx.show([{ header: "Preset · " + p.name },
            { text: "Fire on " + chName(c), fn: function() { view.fire(p, [c], false) } }, chTargets("Fire on", function(chs) { view.fire(p, chs, false) }),
            { text: "Hide on " + chName(c), fn: function() { view.fire(p, [c], true) } },
            { text: "Edit in the form", fn: function() { view.loadSpec(p); view.editing = { kind: "preset", id: p.id } } }, { sep: true },
            { text: "Copy", sub: [{ text: "Copy preset (paste elsewhere)", fn: function() { window.setClip("preset", p, p.name); ctx.copyJson(p, p.name) } }, { text: "Copy name", fn: function() { ctx.copyText(p.name) } }, { text: "Copy template id", fn: function() { ctx.copyText(p.template) } }] },
            { text: "Paste as new preset", enabled: clipOk(["overlay", "preset", "default"]), fn: function() { var s = view.pasteSpec(); if (s) view.addPreset(s, s.name) } },
            { text: "Duplicate", fn: function() { var n = view.clone(p); n.id = view.uid("p"); n.name = p.name + " copy"; n.hotkey = ""; view.savePresets(view.presets.concat([n])) } },
            moveSub(presets, p.id, savePresets), { sep: true },
            { text: "Delete preset", danger: true, fn: function() { var a = []; for (var i = 0; i < view.presets.length; ++i) if (view.presets[i].id !== p.id) a.push(view.presets[i]); view.savePresets(a) } }, { sep: true },
            { text: "Properties…", fn: function() { ctx.showProps("Preset " + p.name, p) } }])
    }
    function defaultMenu(d) {
        var c = ch
        ctx.show([{ header: "Default · " + tplName(d.template) },
            { text: "Deploy now", fn: function() { view.deployTo([c], d) } },
            { text: d.enabled !== false ? "Disable" : "Enable", fn: function() { var a = view.clone(view.defaults); for (var i = 0; i < a.length; ++i) if (a[i].id === d.id) a[i].enabled = d.enabled === false; view.saveDefaults(c, a) } },
            { text: "Edit in the form", fn: function() { view.loadSpec(d); view.editing = { kind: "default", id: d.id } } }, { sep: true },
            { text: "Copy", sub: [{ text: "Copy default (paste elsewhere)", fn: function() { window.setClip("default", d, tplName(d.template)); ctx.copyJson(d, tplName(d.template)) } }, { text: "Copy template id", fn: function() { ctx.copyText(d.template) } }] },
            { text: "Paste as new default", enabled: clipOk(["overlay", "preset", "default"]), fn: function() { var s = view.pasteSpec(); if (s) view.addDefault(c, s) } },
            { text: "Duplicate", fn: function() { var n = view.clone(d); n.id = view.uid("d"); view.saveDefaults(c, view.defaults.concat([n])) } },
            chTargets("Apply to channel", function(chs) { for (var i = 0; i < chs.length; ++i) if (chs[i] !== c) view.addDefault(chs[i], d) }),
            moveSub(defaults, d.id, function(l) { view.saveDefaults(c, l) }), { sep: true },
            { text: "Delete default", danger: true, fn: function() { var a = []; for (var i = 0; i < view.defaults.length; ++i) if (view.defaults[i].id !== d.id) a.push(view.defaults[i]); view.saveDefaults(c, a) } }, { sep: true },
            { text: "Properties…", fn: function() { ctx.showProps("Default " + tplName(d.template), d) } }])
    }
    function listMenu(name, list, save, mk) {
        ctx.show([{ header: name },
            { text: "Paste as new", enabled: clipOk(["overlay", "preset", "default"]), fn: function() { var s = view.pasteSpec(); if (s) save(list.concat([mk(s)])) } },
            { text: "Export all (copy JSON)", enabled: list.length > 0, fn: function() { ctx.copyJson(list, name) } },
            { text: "Refresh", fn: function() { view.reload() } }, { sep: true },
            { text: "Delete all", danger: true, enabled: list.length > 0, fn: function() { save([]) } }])
    }
    function boardMenu() {
        var c = ch
        ctx.show([{ header: "Overlays on " + chName(c) },
            { text: "Paste as new overlay", enabled: clipOk(["overlay", "preset", "default"]), fn: function() { var s = view.pasteSpec(); if (s && (s.template || s.templateId)) view.deployTo([c], s) } },
            { text: "Copy all layers (JSON)", enabled: layers.length > 0, fn: function() { ctx.copyJson(layers, "layers") } },
            { text: "Refresh", fn: function() { view.reload() } }, { sep: true },
            { text: "Clear " + chName(c), danger: true, enabled: layers.length > 0, fn: function() { view.clearCh([c], null) } },
            { text: "Clear every channel", danger: true, fn: function() { view.clearCh(view.allCh(), null) } }])
    }

    Component.onCompleted: reload()
    Connections {
        target: supervisor
        function onUiEvent(type, payload) {
            if (type === "overlays" && payload && payload.channelId) { var n = view.clone(view.channelsData); n[payload.channelId] = n[payload.channelId] || { layers: [], defaults: [] }; if (payload.layers) n[payload.channelId].layers = view.listOf(payload.layers, "layers"); if (payload.defaults) n[payload.channelId].defaults = view.listOf(payload.defaults, "defaults"); view.channelsData = n }
            else if (type === "overlayConfig") view.reload()
        }
    }
    Timer { interval: 6000; running: true; repeat: true; onTriggered: view.reload() }
    Timer { interval: 1000; running: true; repeat: true; onTriggered: view.tick++ }
    CMenu { id: ctx }
    Instantiator {
        model: view.hotkeyPresets
        delegate: Shortcut {
            sequence: modelData.seq; enabled: view.visible && !view.typing
            onActivated: { for (var i = 0; i < view.presets.length; ++i) if (view.presets[i].id === modelData.id) view.fire(view.presets[i], [view.ch], false) }
        }
    }

    RowLayout {
        anchors.fill: parent; spacing: 10

        // ---- gallery ------------------------------------------------------------------------------------------------------
        CPanel {
            Layout.preferredWidth: 330; Layout.fillHeight: true
            title: "Templates (" + view.gallery.length + ")"
            ColumnLayout {
                anchors.fill: parent; spacing: 6
                RowLayout { Layout.fillWidth: true; spacing: 6
                    CField { Layout.fillWidth: true; placeholder: "search"; text: view.query; onEdited: function(v) { view.query = v } }
                    CButton { icon2: "⟳"; small: true; variant: "ghost"; onClicked: view.reload() } }
                Flow { Layout.fillWidth: true; spacing: 4
                    CButton { text: "all"; small: true; variant: "ghost"; active: !view.cat; onClicked: { view.cat = ""; view.query = "" } }
                    Repeater { model: view.categories; CButton { text: modelData; small: true; variant: "ghost"; active: view.cat === modelData; onClicked: { view.cat = modelData; view.query = "" } } } }
                GridView {
                    id: grid
                    Layout.fillWidth: true; Layout.fillHeight: true; clip: true; cellWidth: 150; cellHeight: 112; model: view.gallery
                    delegate: Rectangle {
                        width: 142; height: 104; radius: Theme.r5; color: modelData.id === view.selId ? Theme.bg5 : Theme.bg3; border.color: modelData.id === view.selId ? Theme.accent0 : Theme.border0
                        Rectangle {   // native card: where the role puts the graphic on a 16:9 raster
                            x: 6; y: 6; width: 130; height: 58; color: "#000"; radius: 3
                            Rectangle {
                                color: modelData.role === "emergency" ? Theme.danger : modelData.role === "bug" || modelData.role === "legal_id" ? Theme.clsGraphics : Theme.accent0; opacity: 0.85; radius: 2
                                x: modelData.role === "bug" ? 100 : modelData.role === "secondary" ? 25 : modelData.role === "legal_id" ? 4 : modelData.role === "ticker" ? 0 : 6
                                y: modelData.role === "bug" || modelData.role === "legal_id" ? 4 : modelData.role === "secondary" ? 12 : modelData.role === "ticker" ? 48 : 38
                                width: modelData.role === "bug" || modelData.role === "legal_id" ? 26 : modelData.role === "secondary" ? 80 : modelData.role === "ticker" ? 130 : 70
                                height: modelData.role === "bug" || modelData.role === "legal_id" ? 14 : modelData.role === "secondary" ? 34 : modelData.role === "ticker" ? 10 : 14
                            }
                            Text { anchors.centerIn: parent; visible: modelData.role === "secondary"; text: modelData.icon || "▭"; color: "#05050a"; font.pixelSize: Theme.fs16 }
                        }
                        Text { x: 6; y: 68; width: 130; text: modelData.name; color: Theme.text0; font.pixelSize: Theme.fs11; font.bold: true; elide: Text.ElideRight }
                        Text { x: 6; y: 84; width: 130; text: modelData.category + " · " + (modelData.role || ""); color: Theme.text3; font.pixelSize: Theme.fs10; elide: Text.ElideRight }
                        MouseArea {
                            anchors.fill: parent; acceptedButtons: Qt.LeftButton | Qt.RightButton; hoverEnabled: true
                            onClicked: function(m) { if (m.button === Qt.RightButton) view.tplMenu(modelData); else { view.pickTemplate(modelData.id); view.editing = null } }
                            ToolTip.visible: containsMouse; ToolTip.delay: 600; ToolTip.text: modelData.description || ""
                        }
                    }
                    MouseArea { anchors.fill: parent; z: -1; acceptedButtons: Qt.RightButton; onClicked: ctx.show([{ header: "Template gallery" }, { text: "Refresh", fn: function() { view.reload() } }, { text: "Copy template ids", fn: function() { ctx.copyText(view.templates.map(function(t) { return t.id }).join("\n"), "ids") } }]) }
                    Text { anchors.centerIn: parent; visible: !view.gallery.length; text: view.templates.length ? "No template matches" : "No template pack loaded"; color: Theme.text3; font.pixelSize: Theme.fs11 }
                }
            }
        }

        // ---- deploy form ---------------------------------------------------------------------------------------------------
        CPanel {
            Layout.preferredWidth: 380; Layout.fillHeight: true
            title: view.sel ? view.sel.name : "Deploy"
            Flickable {
                anchors.fill: parent; contentHeight: formCol.implicitHeight; clip: true
                ColumnLayout {
                    id: formCol; width: parent.width - 8; spacing: 8
                    Rectangle {
                        visible: !!view.editing; Layout.fillWidth: true; implicitHeight: 28; radius: Theme.r5; color: Qt.rgba(1, 0.84, 0.04, 0.15); border.color: Theme.warn
                        Text { anchors.fill: parent; anchors.leftMargin: 8; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight; color: Theme.warn; font.pixelSize: Theme.fs11; text: view.editing ? "Editing " + view.editing.kind + (view.editing.layer !== undefined ? " " + view.editing.layer : "") : "" }
                    }
                    Text { visible: !view.sel; text: "Pick a template from the gallery"; color: Theme.text3; font.pixelSize: Theme.fs12 }
                    Text { visible: !!view.sel; Layout.fillWidth: true; wrapMode: Text.WordWrap; text: view.sel ? (view.sel.description || "") : ""; color: Theme.text2; font.pixelSize: Theme.fs11 }
                    Repeater {
                        model: view.sel ? view.sel.params : []
                        delegate: ColumnLayout {
                            Layout.fillWidth: true; spacing: 2
                            property string ptype: modelData.type || "text"
                            CSelect { visible: ptype === "select"; label: modelData.label || modelData.name; Layout.fillWidth: true; model: modelData.options || []; currentIndex: Math.max(0, (modelData.options || []).indexOf(view.pval(modelData)))
                                onActivated: function(i) { var p = view.clone(view.params); p[modelData.name] = String((modelData.options || [])[i]); view.params = p } }
                            CToggle { visible: ptype === "boolean"; Layout.fillWidth: true; label: modelData.label || modelData.name; checked: view.pval(modelData) === "1" || view.pval(modelData) === "true"
                                onToggled: function(v) { var p = view.clone(view.params); p[modelData.name] = v ? "1" : "0"; view.params = p } }
                            CField { visible: ptype !== "select" && ptype !== "boolean"; Layout.fillWidth: true; label: (modelData.label || modelData.name) + (modelData.required ? " *" : ""); mono: ptype === "color" || ptype === "number"; text: view.pval(modelData); placeholder: modelData["default"] !== undefined ? String(modelData["default"]) : ""
                                onEdited: function(v) { var p = view.clone(view.params); p[modelData.name] = ptype === "color" ? v.replace("#", "") : v; view.params = p } }
                        }
                    }
                    Text { visible: !!view.sel; text: "ANIMATION"; color: Theme.text2; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1 }
                    Flow { visible: !!view.sel; Layout.fillWidth: true; spacing: 4
                        Repeater { model: view.animPresets; CButton { text: (modelData.icon || "") + " " + modelData.name; small: true; variant: "ghost"; onClicked: { view.animIn = modelData["in"]; view.animOut = modelData.out; view.inMs = modelData.duration; view.outMs = modelData.duration } } } }
                    GridLayout { visible: !!view.sel; Layout.fillWidth: true; columns: 2; columnSpacing: 8; rowSpacing: 6
                        CSelect { label: "In"; Layout.fillWidth: true; model: view.animations; currentIndex: Math.max(0, view.animations.indexOf(view.animIn)); onActivated: function(i) { view.animIn = view.animations[i] } }
                        CSelect { label: "Out"; Layout.fillWidth: true; model: view.animations; currentIndex: Math.max(0, view.animations.indexOf(view.animOut)); onActivated: function(i) { view.animOut = view.animations[i] } }
                        CField { label: "In ms"; Layout.fillWidth: true; mono: true; text: String(view.inMs); onEdited: function(v) { view.inMs = parseInt(v) || 0 } }
                        CField { label: "Out ms"; Layout.fillWidth: true; mono: true; text: String(view.outMs); onEdited: function(v) { view.outMs = parseInt(v) || 0 } }
                        CSelect { label: "Role"; Layout.fillWidth: true; model: [""].concat(view.roles); currentIndex: Math.max(0, view.roles.indexOf(view.role) + 1); onActivated: function(i) { view.role = i > 0 ? view.roles[i - 1] : "" } }
                        CSelect { label: "Mode"; Layout.fillWidth: true; model: view.modes; currentIndex: Math.max(0, view.modes.indexOf(view.mode)); onActivated: function(i) { view.mode = view.modes[i] } }
                        CField { label: "Slot (0 = main)"; Layout.fillWidth: true; mono: true; text: String(view.slot); onEdited: function(v) { view.slot = Math.max(0, parseInt(v) || 0) } }
                        CField { label: "Hold (s, 0 = until hidden)"; Layout.fillWidth: true; mono: true; text: String(view.hold); onEdited: function(v) { view.hold = Math.max(0, parseFloat(v) || 0) } }
                    }
                    RowLayout { visible: !!view.sel; Layout.fillWidth: true; spacing: 6
                        CButton { text: view.busy ? "DEPLOYING…" : "DEPLOY"; icon2: "▶"; variant: "success"; enabled: !view.busy; Layout.fillWidth: true; onClicked: view.deployTo([view.ch], view.formSpec()) }
                        CButton { text: "All channels"; variant: "primary"; enabled: !view.busy; Layout.fillWidth: true; onClicked: view.deployTo(view.allCh(), view.formSpec()) }
                    }
                    RowLayout { visible: !!view.sel && !view.editing; Layout.fillWidth: true; spacing: 6
                        CButton { text: "Save as preset"; small: true; onClicked: view.addPreset(view.formSpec(), "") }
                        CButton { text: "Add as default"; small: true; onClicked: view.addDefault(view.ch, view.formSpec()) }
                        CButton { text: "Reset"; small: true; variant: "ghost"; onClicked: { view.params = {}; view.role = ""; view.slot = 0; view.hold = 0; view.mode = "auto" } }
                    }
                    RowLayout { visible: !!view.editing; Layout.fillWidth: true; spacing: 6
                        CButton { text: view.editing && view.editing.kind === "layer" ? "Apply update" : "Save changes"; variant: "primary"; Layout.fillWidth: true; onClicked: view.applyEdit() }
                        CButton { text: "Cancel"; variant: "ghost"; onClicked: view.editing = null }
                    }
                }
            }
        }

        // ---- board + tabs ----------------------------------------------------------------------------------------------------
        ColumnLayout {
            Layout.fillWidth: true; Layout.fillHeight: true; Layout.minimumWidth: 420; spacing: 10
            CPanel {
                Layout.fillWidth: true; Layout.fillHeight: true; padding: 0
                title: "On air — " + view.chName(view.ch) + " (" + view.layers.length + ")"
                headerRight: [
                    CBadge { text: view.renderer.available === undefined ? "renderer ?" : view.renderer.available ? "HTML " + (view.renderer.version || "ready") : "no browser"; tint: view.renderer.available ? Theme.ready : view.renderer.available === undefined ? Theme.text3 : Theme.danger },
                    CToggle { label: "incl. defaults"; implicitWidth: 120; checked: view.clearDefs; onToggled: function(v) { view.clearDefs = v } },
                    CButton { text: "Clear"; small: true; variant: "danger"; onClicked: view.clearCh([view.ch], null) }
                ]
                ColumnLayout {
                    anchors.fill: parent; spacing: 0
                    Rectangle { visible: view.down.length > 0; Layout.fillWidth: true; implicitHeight: 28; color: Qt.rgba(1, 0.84, 0.04, 0.15)
                        Text { anchors.fill: parent; anchors.leftMargin: 10; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight; text: view.down; color: Theme.warn; font.pixelSize: Theme.fs11 } }
                    ListView {
                        id: board
                        Layout.fillWidth: true; Layout.fillHeight: true; clip: true; model: view.layers
                        delegate: Rectangle {
                            width: ListView.view.width; height: 46; color: index % 2 ? Theme.bg2 : Theme.bg3
                            RowLayout {
                                anchors.fill: parent; anchors.leftMargin: 10; anchors.rightMargin: 8; spacing: 8
                                Text { text: modelData.layer; color: Theme.text2; font.family: Theme.fontMono; font.pixelSize: Theme.fs11; Layout.preferredWidth: 26 }
                                Text { text: (modelData.role || "") + (modelData.slot ? " #" + modelData.slot : ""); color: Theme.text0; font.pixelSize: Theme.fs11; font.bold: true; Layout.preferredWidth: 96; elide: Text.ElideRight }
                                ColumnLayout { Layout.fillWidth: true; spacing: 1
                                    Text { text: modelData.name || view.tplName(modelData.template); color: Theme.text0; font.pixelSize: Theme.fs12; elide: Text.ElideRight; Layout.fillWidth: true }
                                    Text { text: view.summary(modelData.params); color: Theme.text3; font.pixelSize: Theme.fs10; elide: Text.ElideRight; Layout.fillWidth: true } }
                                Text { text: { var t = view.tick; return modelData.shownAtMs ? view.fmtAge(Date.now() - modelData.shownAtMs) : "—" } color: Theme.text1; font.family: Theme.fontMono; font.pixelSize: Theme.fs11; Layout.preferredWidth: 44 }
                                CBadge { text: modelData.mode || "?"; tint: modelData.mode === "html" ? Theme.accent0 : Theme.accent2 }
                                CBadge { text: modelData.state || "on_air"; tint: view.stateColor(modelData.state) }
                                CButton { text: "Update"; small: true; onClicked: { view.loadSpec(view.specOfLayer(modelData)); view.editing = { kind: "layer", ch: view.ch, layer: modelData.layer } } }
                                CButton { text: "Re-take"; small: true; onClicked: view.layerCall(view.ch, modelData.layer, "retake", {}, "Layer " + modelData.layer + " re-taken") }
                                CButton { text: "Hide"; small: true; enabled: view.isUp(modelData.state); onClicked: view.layerCall(view.ch, modelData.layer, "hide", {}, "Layer " + modelData.layer + " hidden") }
                                CButton { icon2: "✕"; small: true; variant: "ghost"; onClicked: view.deleteLayer(view.ch, modelData.layer) }
                            }
                            MouseArea { anchors.fill: parent; z: -1; acceptedButtons: Qt.RightButton; onClicked: view.layerMenu(modelData) }
                        }
                        MouseArea { anchors.fill: parent; z: -1; acceptedButtons: Qt.RightButton; onClicked: view.boardMenu() }
                        Text { anchors.centerIn: parent; visible: !view.layers.length; text: "No overlays on " + view.chName(view.ch) + ". Deploy one, fire a preset, or enable a default."; color: Theme.text3; font.pixelSize: Theme.fs11 }
                    }
                }
            }
            CPanel {
                Layout.fillWidth: true; Layout.preferredHeight: 270; padding: 0
                title: "Graphics"
                headerRight: [
                    CButton { text: "Presets " + view.presets.length; small: true; variant: "ghost"; active: view.tab === "presets"; onClicked: view.tab = "presets" },
                    CButton { text: "Defaults " + view.defaults.length; small: true; variant: "ghost"; active: view.tab === "defaults"; onClicked: view.tab = "defaults" },
                    CButton { text: "Renderer"; small: true; variant: "ghost"; active: view.tab === "renderer"; onClicked: view.tab = "renderer" }
                ]
                ListView {
                    anchors.fill: parent; clip: true; visible: view.tab === "presets"; model: view.presets
                    delegate: Rectangle {
                        width: ListView.view.width; height: 40; color: index % 2 ? Theme.bg2 : Theme.bg3
                        RowLayout { anchors.fill: parent; anchors.leftMargin: 10; anchors.rightMargin: 8; spacing: 8
                            Text { text: modelData.hotkey ? view.seqOf(modelData.hotkey) : ""; visible: !!modelData.hotkey; color: view.reserved.indexOf(view.seqOf(modelData.hotkey)) >= 0 ? Theme.danger : Theme.accent0; font.family: Theme.fontMono; font.pixelSize: Theme.fs11; Layout.preferredWidth: 56 }
                            Item { visible: !modelData.hotkey; Layout.preferredWidth: 56 }
                            ColumnLayout { Layout.fillWidth: true; spacing: 1
                                Text { text: modelData.name; color: Theme.text0; font.pixelSize: Theme.fs12; font.bold: true; elide: Text.ElideRight; Layout.fillWidth: true }
                                Text { text: view.tplName(modelData.template) + " · " + (modelData.role || "") + " · " + view.summary(modelData.params); color: Theme.text3; font.pixelSize: Theme.fs10; elide: Text.ElideRight; Layout.fillWidth: true } }
                            CButton { text: "Fire"; small: true; variant: "success"; onClicked: view.fire(modelData, [view.ch], false) }
                            CButton { text: "Hide"; small: true; onClicked: view.fire(modelData, [view.ch], true) }
                            CButton { text: "Edit"; small: true; onClicked: { view.loadSpec(modelData); view.editing = { kind: "preset", id: modelData.id } } }
                        }
                        MouseArea { anchors.fill: parent; z: -1; acceptedButtons: Qt.RightButton; onClicked: view.presetMenu(modelData) }
                    }
                    MouseArea { anchors.fill: parent; z: -1; acceptedButtons: Qt.RightButton; onClicked: view.listMenu("Presets", view.presets, view.savePresets, function(s) { return view.newPreset(s, s.name) }) }
                    Text { anchors.centerIn: parent; visible: !view.presets.length; text: "No presets: build a graphic and press Save as preset"; color: Theme.text3; font.pixelSize: Theme.fs11 }
                }
                ListView {
                    anchors.fill: parent; clip: true; visible: view.tab === "defaults"; model: view.defaults
                    delegate: Rectangle {
                        width: ListView.view.width; height: 40; color: index % 2 ? Theme.bg2 : Theme.bg3
                        RowLayout { anchors.fill: parent; anchors.leftMargin: 10; anchors.rightMargin: 8; spacing: 8
                            CToggle { label: ""; implicitWidth: 40; checked: modelData.enabled !== false; onToggled: function(v) { var a = view.clone(view.defaults); a[index].enabled = v; view.saveDefaults(view.ch, a) } }
                            ColumnLayout { Layout.fillWidth: true; spacing: 1
                                Text { text: view.tplName(modelData.template); color: Theme.text0; font.pixelSize: Theme.fs12; font.bold: true; elide: Text.ElideRight; Layout.fillWidth: true }
                                Text { text: (modelData.role || "") + " · " + (modelData.mode || "auto") + " · " + view.summary(modelData.params); color: Theme.text3; font.pixelSize: Theme.fs10; elide: Text.ElideRight; Layout.fillWidth: true } }
                            CButton { text: "Deploy now"; small: true; onClicked: view.deployTo([view.ch], modelData) }
                            CButton { text: "Edit"; small: true; onClicked: { view.loadSpec(modelData); view.editing = { kind: "default", id: modelData.id } } }
                        }
                        MouseArea { anchors.fill: parent; z: -1; acceptedButtons: Qt.RightButton; onClicked: view.defaultMenu(modelData) }
                    }
                    MouseArea { anchors.fill: parent; z: -1; acceptedButtons: Qt.RightButton; onClicked: view.listMenu("Defaults on " + view.chName(view.ch), view.defaults, function(l) { view.saveDefaults(view.ch, l) }, function(s) { return view.newDefault(s) }) }
                    Text { anchors.centerIn: parent; visible: !view.defaults.length; text: "No always-on graphics on " + view.chName(view.ch); color: Theme.text3; font.pixelSize: Theme.fs11 }
                }
                Flickable {
                    anchors.fill: parent; anchors.margins: 10; visible: view.tab === "renderer"; clip: true; contentHeight: rcol.implicitHeight
                    ColumnLayout {
                        id: rcol; width: parent.width; spacing: 6
                        Text { text: (view.renderer.available ? "browser found: " : "no browser: ") + (view.renderer.path || "—") + "   version " + (view.renderer.version || "—"); color: view.renderer.available ? Theme.ready : Theme.danger; font.family: Theme.fontMono; font.pixelSize: Theme.fs11; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true }
                        Repeater { model: view.renderer.candidates || []; Text { text: (modelData.exists ? "✓ " : "· ") + modelData.path; color: modelData.exists ? Theme.ready : Theme.text3; font.family: Theme.fontMono; font.pixelSize: Theme.fs10; elide: Text.ElideMiddle; Layout.fillWidth: true } }
                        RowLayout { Layout.fillWidth: true; spacing: 6
                            CField { id: rpath; Layout.fillWidth: true; label: "Preferred browser (cg.browserPath)"; mono: true; text: view.renderer.preferred || "" }
                            CButton { text: "Save"; small: true; variant: "primary"; Layout.alignment: Qt.AlignBottom; onClicked: api.put("/overlays/renderer", { browserPath: rpath.text }, function(ok, d) { if (ok) { window.toast("Saved: applies to the next engine start", "ok"); view.reload() } else view.toastErr("Save failed", d) }) }
                            CButton { text: "Test on " + view.chName(view.ch); small: true; Layout.alignment: Qt.AlignBottom; onClicked: api.post("/overlays/renderer/test", { channel: view.ch }, function(ok, d) { window.toast(ok && d && d.ok !== false ? "Renderer test finished" : "Renderer test failed" + (d && d.error ? ": " + d.error : ""), ok && d && d.ok !== false ? "ok" : "error"); view.reload() }) }
                        }
                        Repeater { model: Object.keys(view.renderer.engines || {}); Text { property var h: ((view.renderer.engines || {})[modelData] || {}).html || ({}); text: view.chName(modelData) + ": " + (h.state || "—") + (h.fps ? "  " + Number(h.fps).toFixed(0) + " fps" : "") + (h.restarts ? "  " + h.restarts + " restarts" : "") + (h.lastError ? "  " + h.lastError : ""); color: h.lastError ? Theme.danger : Theme.text1; font.pixelSize: Theme.fs11; wrapMode: Text.Wrap; Layout.fillWidth: true } }
                    }
                    MouseArea { anchors.fill: parent; z: -1; acceptedButtons: Qt.RightButton; onClicked: ctx.show([{ header: "Renderer" }, { text: "Re-scan", fn: function() { view.reload() } }, { text: "Copy browser path", enabled: !!view.renderer.path, fn: function() { ctx.copyText(view.renderer.path) } }, { text: "Copy status (JSON)", fn: function() { ctx.copyJson(view.renderer, "renderer") } }]) }
                }
            }
        }
    }
}
