pragma Singleton
import QtQuick
import "spec.js" as Spec

// ButtonPrefs: the station's button customisations (docs/plans/ux-buttons.md 3.8), one object in the Supervisor setting
// `ui.buttons`, read and written through the generic GET / PUT /api/v1/settings/{key} (no Supervisor change: the value must be
// an object, and the route stores it as a JSON string, which load() parses back).
//
//   { "version": 1,
//     "surfaces": { "onair.deck": { "order": ["transport.playpause", "|", ...], "hidden": ["automation.snap"] } },
//     "buttons":  { "onair.deck/transport.stop": { "label": "STOP", "icon": "stop", "color": "#ff2d55", "size": "l",
//                                                  "hotkey": "Ctrl+Shift+S", "safety": "arm" } } }
//
// - resolve(surface, id, defaults) merges a button's overrides over the defaults the owner gives (Actions.qml's spec for an
//   action, a bus key's title, a CG slot's name); order(surface, defaultOrder) is the surface's key order with hidden keys
//   removed and new default keys appended.
// - validate(obj) cleans a stored object field by field (3.8): icon in the catalogue, colour #rrggbb, size allowed on the
//   surface, label at most limits.labelMax, safety a known level (the floor is applied in resolve, since a stateful action's
//   floor depends on its state), hotkey normalised, allowed natively, not reserved on the surface's page and not bound
//   twice on one page. Nothing invalid reaches a key; the problems are listed in `errors` and warned once.
// - The backend (the app's `api`, or a fake in tests: {get(path, cb), put(path, body, cb)}) is injected with attach(); a
//   page that shows customised keys calls touch() when it opens (one reload per 5 s at most), and a timer reloads every
//   refreshMs (30 s) because no WebSocket event exists for generic settings. A local unsaved edit (dirty) is never
//   overwritten by a reload. save() writes the whole object.
// - Every change bumps `revision`; resolve() and order() read it, so a binding that calls them follows the prefs.
QtObject {
    id: prefs

    readonly property string settingKey: "ui.buttons"
    readonly property string path: "/settings/" + settingKey
    readonly property int version: 1
    readonly property var safetyLevels: Spec.safetyLevels
    readonly property int labelMax: Spec.limits.labelMax
    readonly property int touchMs: 5000

    // The customisable surfaces (design 3.8): the sizes the editor may offer, the page whose reserved keys apply ("" = the
    // key is visible on every page, so every reserved key applies and every binding on any page conflicts), and whether the
    // surface is a deck (order and hidden apply).
    readonly property var surfaces: ({
        "onair.deck":       { sizes: ["l", "xl"], page: "onair", deck: true },
        "onair.switch":     { sizes: ["l"], page: "onair", deck: true },
        "onair.bus":        { sizes: ["l"], page: "onair", deck: false },
        "column.transport": { sizes: ["s", "m"], page: "", deck: true },
        "graphics.cg":      { sizes: ["m", "l"], page: "onair", deck: false },
        "graphics.quick":   { sizes: ["s", "m"], page: "onair", deck: false },
        "remote.pad":       { sizes: Spec.limits.padSizes, page: "remote", deck: false }
    })
    readonly property var fields: ["label", "icon", "color", "size", "hotkey", "safety"]

    property var backend: null             // {get(path, cb), put(path, body, cb)}; null until attach()
    property var data: prefs.empty()
    property bool loaded: false            // a load answered (with a valid object or not)
    property bool dirty: false             // local edits not saved yet; a reload leaves them alone
    property bool loading: false
    property int revision: 0               // bumps on every change of `data`
    property var errors: []                // what validate() dropped from the last loaded object
    property string lastError: ""
    property int refreshMs: 30000
    property bool autoRefresh: true
    property double _touched: 0
    property var _warned: ({})

    signal changed()
    signal loadDone(bool ok)
    signal saveDone(bool ok)

    readonly property Timer _refresh: Timer {
        interval: prefs.refreshMs; repeat: true
        running: prefs.backend !== null && prefs.autoRefresh && prefs.refreshMs > 0
        onTriggered: prefs.load()
    }

    function empty() { return { version: prefs.version, surfaces: {}, buttons: {} } }
    function clone(o) { return JSON.parse(JSON.stringify(o)) }
    function isObj(o) { return o !== null && typeof o === "object" && !Array.isArray(o) }
    function surfaceOf(name) { return prefs.surfaces[name] !== undefined ? prefs.surfaces[name] : null }
    function pageOf(surface) { const s = prefs.surfaceOf(surface); return s ? s.page : "" }
    function sizesOf(surface) { const s = prefs.surfaceOf(surface); return s ? s.sizes : Spec.sizeOrder }
    function safetyIndex(level) { return Spec.safetyLevels.indexOf(level) }
    // The level a key gets for a wanted level and its floor: never below the floor; raising is always allowed; unknown
    // levels count as none.
    function clampSafety(level, floor) {
        const l = Math.max(0, prefs.safetyIndex(level)), f = Math.max(0, prefs.safetyIndex(floor))
        return Spec.safetyLevels[Math.max(l, f)]
    }
    function warnOnce(key, msg) {
        if (prefs._warned[key]) return
        prefs._warned[key] = true
        console.warn("ButtonPrefs: " + msg)
    }

    // --- hotkeys ----------------------------------------------------------------------------------------------------
    // Canonical form of a key chord: modifiers in hotkeyRules.modifierOrder (Ctrl, Alt, Shift, Meta, Num), aliases folded
    // (Esc -> Escape, Control -> Ctrl, Cmd -> Meta ...), one key: a single character upper-cased, Fn, or a named key
    // (Return, Enter, Escape, Delete, Space, PgUp ...). "" for nothing, a chord without a key ("Ctrl+") or two keys.
    function normalizeHotkey(seq) {
        const s = String(seq === undefined || seq === null ? "" : seq).trim()
        if (s === "") return ""
        const parts = []
        let cur = ""
        for (let i = 0; i < s.length; ++i) {      // "+" splits, except as the key itself ("Ctrl++")
            const c = s[i]
            if (c === "+" && cur !== "") { parts.push(cur); cur = "" }
            else if (c === "+") cur = "+"
            else cur += c
        }
        if (cur !== "") parts.push(cur)
        const rules = Spec.hotkeyRules
        const aliases = {}
        for (const k in rules.aliases) aliases[k.toLowerCase()] = rules.aliases[k]
        const mods = {}
        let key = ""
        for (let p of parts) {
            p = p.trim()
            if (p === "") return ""
            const low = p.toLowerCase()
            if (aliases[low] !== undefined) p = aliases[low]
            const asMod = rules.modifierOrder.find(m => m.toLowerCase() === p.toLowerCase())
            if (asMod !== undefined) { mods[asMod] = true; continue }
            if (key !== "") return ""                   // two keys
            key = prefs._keyName(p)
            if (key === "") return ""
        }
        if (key === "") return ""
        const out = rules.modifierOrder.filter(m => mods[m])
        out.push(key)
        return out.join("+")
    }
    readonly property var _named: ["Return", "Enter", "Escape", "Delete", "Insert", "Space", "Tab", "Backspace", "Home", "End",
                                   "PgUp", "PgDown", "Left", "Right", "Up", "Down", "Pause", "Print", "Menu"]
    function _keyName(p) {
        if (p.length === 1) return p.toUpperCase()
        const f = /^f(\d{1,2})$/i.exec(p)
        if (f) { const n = parseInt(f[1]); return n >= 1 && n <= 35 ? "F" + n : "" }
        const n = prefs._named.find(x => x.toLowerCase() === p.toLowerCase())
        return n !== undefined ? n : ""
    }
    // Allowed for a custom binding in the native app (hotkeyRules.allowedNative): F9-F12, Ctrl / Alt / Shift / Ctrl+Shift
    // + letter or digit, keypad keys. A bare letter would steal typing and F1-F8 are the pages.
    function hotkeyAllowed(seq) {
        const n = prefs.normalizeHotkey(seq)
        return n !== "" && new RegExp(Spec.hotkeyRules.allowedNative).test(n)
    }
    // The reserved chords (buttons.json reservedKeys) for a page: the global list plus the page's; "" = every page.
    function reservedFor(page) {
        const r = Spec.reservedKeys
        let out = r.global.slice()
        if (page === "") { for (const k in r) if (k !== "global") out = out.concat(r[k]) }
        else if (r[page] !== undefined) out = out.concat(r[page])
        return out.map(prefs.normalizeHotkey).filter(x => x !== "")
    }
    function hotkeyReserved(seq, surface) {
        const n = prefs.normalizeHotkey(seq)
        return n !== "" && prefs.reservedFor(prefs.pageOf(surface)).indexOf(n) >= 0
    }
    // The other binding of `seq` that would clash with surface/id: on the same page, or anywhere when either side is visible
    // on every page (the column). null when free. `buttons` defaults to the stored ones.
    function conflict(seq, surface, id, buttons) {
        const n = prefs.normalizeHotkey(seq)
        if (n === "") return null
        const b = buttons !== undefined ? buttons : prefs.data.buttons
        const page = prefs.pageOf(surface)
        for (const key in b) {
            const o = b[key]
            if (!prefs.isObj(o) || o.hotkey === undefined || prefs.normalizeHotkey(o.hotkey) !== n) continue
            const at = key.indexOf("/")
            const s2 = at > 0 ? key.slice(0, at) : "", id2 = at > 0 ? key.slice(at + 1) : key
            if (s2 === surface && id2 === id) continue
            const p2 = prefs.pageOf(s2)
            if (page === "" || p2 === "" || page === p2) return { surface: s2, id: id2, hotkey: n }
        }
        return null
    }
    // {ok, seq, reason, conflict}: the normalised chord and why it is refused ("empty" is ok: no binding).
    function hotkeyCheck(seq, surface, id, buttons) {
        const n = prefs.normalizeHotkey(seq)
        if (String(seq === undefined || seq === null ? "" : seq).trim() === "") return { ok: true, seq: "", reason: "", conflict: null }
        if (n === "") return { ok: false, seq: "", reason: "not a key chord", conflict: null }
        if (!prefs.hotkeyAllowed(n)) return { ok: false, seq: n, reason: "not allowed: use F9-F12, Ctrl / Alt / Shift + a letter or digit, or a keypad key", conflict: null }
        if (prefs.hotkeyReserved(n, surface)) return { ok: false, seq: n, reason: "reserved by the application", conflict: null }
        const c = prefs.conflict(n, surface, id, buttons)
        if (c) return { ok: false, seq: n, reason: "taken by " + c.surface + "/" + c.id, conflict: c }
        return { ok: true, seq: n, reason: "", conflict: null }
    }
    function hotkeyOk(seq, surface, id) { return prefs.hotkeyCheck(seq, surface, id).ok }

    // --- validation -------------------------------------------------------------------------------------------------
    function colorOk(c) { return typeof c === "string" && new RegExp(Spec.limits.colorPattern).test(c) }
    function iconOk(name) { return typeof name === "string" && (name === "" || Spec.icons[name] !== undefined) }
    // One button's overrides, cleaned: {clean, errors}. Unknown fields are dropped silently (a later version may add some);
    // an invalid value is dropped with a note. `hotkey` is checked against `buttons` (the ones accepted so far) for clashes.
    function cleanButton(surface, id, given, buttons) {
        const out = {}, errors = []
        const where = surface + "/" + id
        if (!prefs.isObj(given)) return { clean: out, errors: [where + ": not an object"] }
        const patch = {}                       // null and "" mean "no override" (setButton clears the field on them)
        for (const k in given) if (given[k] !== null && given[k] !== "") patch[k] = given[k]
        if (patch.label !== undefined) {
            if (typeof patch.label !== "string") errors.push(where + ": label is not a string")
            else {
                const l = patch.label.trim()
                if (l.length > prefs.labelMax) { errors.push(where + ": label cut to " + prefs.labelMax + " characters"); out.label = l.slice(0, prefs.labelMax) }
                else if (l !== "") out.label = l
            }
        }
        if (patch.icon !== undefined) {
            if (prefs.iconOk(patch.icon)) { if (patch.icon !== "") out.icon = patch.icon }
            else errors.push(where + ": unknown icon \"" + patch.icon + "\"")
        }
        if (patch.color !== undefined && patch.color !== "") {
            if (prefs.colorOk(patch.color)) out.color = patch.color.toLowerCase()
            else errors.push(where + ": colour \"" + patch.color + "\" is not #rrggbb")
        }
        if (patch.size !== undefined && patch.size !== "") {
            if (prefs.sizesOf(surface).indexOf(patch.size) >= 0) out.size = patch.size
            else errors.push(where + ": size \"" + patch.size + "\" is not offered on " + surface)
        }
        if (patch.safety !== undefined && patch.safety !== "") {
            if (prefs.safetyIndex(patch.safety) >= 0) out.safety = patch.safety
            else errors.push(where + ": unknown safety \"" + patch.safety + "\"")
        }
        if (patch.hotkey !== undefined && String(patch.hotkey).trim() !== "") {
            const c = prefs.hotkeyCheck(patch.hotkey, surface, id, buttons)
            if (c.ok) out.hotkey = c.seq
            else errors.push(where + ": hotkey \"" + patch.hotkey + "\" " + c.reason)
        }
        return { clean: out, errors: errors }
    }
    // The whole setting, cleaned: {ok, clean, errors}. ok is false only when the input is not an object at all (the clean
    // result is then empty); dropped fields leave ok true, so a half-valid setting still applies.
    function validate(obj) {
        const errors = [], clean = prefs.empty()
        if (typeof obj === "string") { try { obj = JSON.parse(obj) } catch (e) { return { ok: false, clean: clean, errors: ["not JSON: " + e] } } }
        if (!prefs.isObj(obj)) return { ok: false, clean: clean, errors: ["not an object"] }
        if (obj.version !== undefined && obj.version !== prefs.version) errors.push("version " + obj.version + " (this is " + prefs.version + ")")
        if (obj.surfaces !== undefined) {
            if (!prefs.isObj(obj.surfaces)) errors.push("surfaces is not an object")
            else for (const s in obj.surfaces) {
                if (!prefs.surfaceOf(s)) { errors.push("unknown surface \"" + s + "\""); continue }
                const v = obj.surfaces[s], o = {}
                if (!prefs.isObj(v)) { errors.push(s + ": not an object"); continue }
                if (v.order !== undefined) {
                    if (!Array.isArray(v.order)) errors.push(s + ": order is not a list")
                    else {
                        const seen = {}
                        o.order = v.order.filter(x => typeof x === "string" && x !== "" && (x === "|" || !seen[x]) && (seen[x] = true))
                        if (o.order.length !== v.order.length) errors.push(s + ": order had entries that are not key ids")
                    }
                }
                if (v.hidden !== undefined) {
                    if (!Array.isArray(v.hidden)) errors.push(s + ": hidden is not a list")
                    else o.hidden = v.hidden.filter(x => typeof x === "string" && x !== "" && x !== "|")
                }
                if (Object.keys(o).length) clean.surfaces[s] = o
            }
        }
        if (obj.buttons !== undefined) {
            if (!prefs.isObj(obj.buttons)) errors.push("buttons is not an object")
            else for (const key in obj.buttons) {
                const at = key.indexOf("/")
                if (at <= 0 || at === key.length - 1) { errors.push("button key \"" + key + "\" is not surface/id"); continue }
                const s = key.slice(0, at), id = key.slice(at + 1)
                if (!prefs.surfaceOf(s)) { errors.push("unknown surface in \"" + key + "\""); continue }
                const r = prefs.cleanButton(s, id, obj.buttons[key], clean.buttons)
                for (const e of r.errors) errors.push(e)
                if (Object.keys(r.clean).length) clean.buttons[key] = r.clean
            }
        }
        return { ok: true, clean: clean, errors: errors }
    }

    // --- reading ----------------------------------------------------------------------------------------------------
    function buttonKey(surface, id) { return surface + "/" + id }
    // The stored overrides of a key (a copy), {} when none.
    function overrides(surface, id) {
        void prefs.revision
        const o = prefs.data.buttons[prefs.buttonKey(surface, id)]
        return prefs.isObj(o) ? prefs.clone(o) : {}
    }
    // The key's look and behaviour: `defaults` ({label, icon, role, color, size, hotkey, safety, floor, tip ...}) with the
    // stored overrides applied where they are valid for this surface and key; `safety` never below `floor`; `custom` lists
    // the fields an override changed. Reads `revision`, so a binding follows the prefs.
    function resolve(surface, id, defaults) {
        void prefs.revision
        const d = prefs.isObj(defaults) ? defaults : {}
        const r = {}
        for (const k in d) r[k] = d[k]
        if (r.label === undefined) r.label = ""
        if (r.icon === undefined) r.icon = ""
        if (r.color === undefined) r.color = ""
        if (r.size === undefined) r.size = prefs.sizesOf(surface)[0]
        if (r.hotkey === undefined) r.hotkey = ""
        if (r.floor === undefined) r.floor = "none"
        r.safety = prefs.clampSafety(r.safety === undefined ? "none" : r.safety, r.floor)
        r.custom = []
        const o = prefs.data.buttons[prefs.buttonKey(surface, id)]
        if (prefs.isObj(o)) {
            if (typeof o.label === "string" && o.label !== "" && o.label.length <= prefs.labelMax) { r.label = o.label; r.custom.push("label") }
            if (typeof o.icon === "string" && o.icon !== "" && Spec.icons[o.icon] !== undefined) { r.icon = o.icon; r.custom.push("icon") }
            if (prefs.colorOk(o.color)) { r.color = o.color; r.custom.push("color") }
            if (typeof o.size === "string" && prefs.sizesOf(surface).indexOf(o.size) >= 0) { r.size = o.size; r.custom.push("size") }
            if (typeof o.hotkey === "string" && o.hotkey !== "" && prefs.hotkeyAllowed(o.hotkey) && !prefs.hotkeyReserved(o.hotkey, surface)) {
                r.hotkey = prefs.normalizeHotkey(o.hotkey); r.custom.push("hotkey")
            }
            if (typeof o.safety === "string" && prefs.safetyIndex(o.safety) >= 0) {
                const s = prefs.clampSafety(o.safety, r.floor)
                if (s !== r.safety) r.custom.push("safety")
                r.safety = s
            }
        }
        return r
    }
    function hidden(surface) {
        void prefs.revision
        const s = prefs.data.surfaces[surface]
        return prefs.isObj(s) && Array.isArray(s.hidden) ? s.hidden.slice() : []
    }
    // The stored order of a surface (a copy), [] when none.
    function storedOrder(surface) {
        void prefs.revision
        const s = prefs.data.surfaces[surface]
        return prefs.isObj(s) && Array.isArray(s.order) ? s.order.slice() : []
    }
    // The keys of a deck in the order to show them: the stored order, keeping only ids in `defaultOrder` (plus "|"
    // separators), then the default ids the stored order does not know (new actions appear at the end), minus the hidden
    // ones; the default order when nothing is stored. Two separators never touch and none ends the row.
    function order(surface, defaultOrder) {
        void prefs.revision
        const def = Array.isArray(defaultOrder) ? defaultOrder : []
        const known = {}
        for (const k of def) if (k !== "|") known[k] = true
        const stored = prefs.storedOrder(surface)
        let out = []
        if (stored.length) {
            const seen = {}
            for (const k of stored) if (k === "|" || (known[k] && !seen[k])) { out.push(k); seen[k] = true }
            for (const k of def) if (k !== "|" && !seen[k]) out.push(k)
        } else out = def.slice()
        const hide = prefs.hidden(surface)
        out = out.filter(k => k === "|" || hide.indexOf(k) < 0)
        const tidy = []
        for (const k of out) { if (k === "|" && (tidy.length === 0 || tidy[tidy.length - 1] === "|")) continue; tidy.push(k) }
        while (tidy.length && tidy[tidy.length - 1] === "|") tidy.pop()
        return tidy
    }
    // The custom hotkeys of a surface for `ids` (every id of the deck): [{id, seq}], valid ones only, in deck order.
    function bindings(surface, ids) {
        void prefs.revision
        const out = []
        for (const id of (Array.isArray(ids) ? ids : [])) {
            if (id === "|") continue
            const o = prefs.data.buttons[prefs.buttonKey(surface, id)]
            if (!prefs.isObj(o) || typeof o.hotkey !== "string" || o.hotkey === "") continue
            if (!prefs.hotkeyAllowed(o.hotkey) || prefs.hotkeyReserved(o.hotkey, surface)) continue
            out.push({ id: id, seq: prefs.normalizeHotkey(o.hotkey) })
        }
        return out
    }
    function isCustomised(surface, id) { return Object.keys(prefs.overrides(surface, id)).length > 0 }

    // --- editing (local; save() writes) -------------------------------------------------------------------------------
    function _bump() { prefs.revision++; prefs.dirty = true; prefs.changed() }
    // Set (merge) a key's overrides. Fields are cleaned as on load; an invalid field is refused and listed in the result's
    // errors. Returns {ok, errors}; ok when every given field was taken.
    function setButton(surface, id, patch) {
        if (!prefs.surfaceOf(surface)) return { ok: false, errors: ["unknown surface \"" + surface + "\""] }
        const cur = prefs.overrides(surface, id)
        const others = prefs.clone(prefs.data.buttons)
        delete others[prefs.buttonKey(surface, id)]
        const r = prefs.cleanButton(surface, id, patch, others)
        for (const k in patch) {                       // an explicit "" clears the field
            if (prefs.fields.indexOf(k) < 0) continue
            if (patch[k] === "" || patch[k] === null) delete cur[k]
            else if (r.clean[k] !== undefined) cur[k] = r.clean[k]
        }
        const d = prefs.clone(prefs.data)
        if (Object.keys(cur).length) d.buttons[prefs.buttonKey(surface, id)] = cur
        else delete d.buttons[prefs.buttonKey(surface, id)]
        prefs.data = d
        prefs._bump()
        return { ok: r.errors.length === 0, errors: r.errors }
    }
    function resetButton(surface, id) {
        const key = prefs.buttonKey(surface, id)
        if (prefs.data.buttons[key] === undefined) return
        const d = prefs.clone(prefs.data)
        delete d.buttons[key]
        prefs.data = d
        prefs._bump()
    }
    function setOrder(surface, orderList) {
        if (!prefs.surfaceOf(surface) || !Array.isArray(orderList)) return
        const d = prefs.clone(prefs.data)
        if (!prefs.isObj(d.surfaces[surface])) d.surfaces[surface] = {}
        d.surfaces[surface].order = orderList.filter(x => typeof x === "string" && x !== "")
        prefs.data = d
        prefs._bump()
    }
    function setHidden(surface, id, hide) {
        if (!prefs.surfaceOf(surface) || id === "|") return
        const d = prefs.clone(prefs.data)
        if (!prefs.isObj(d.surfaces[surface])) d.surfaces[surface] = {}
        const h = Array.isArray(d.surfaces[surface].hidden) ? d.surfaces[surface].hidden.filter(x => x !== id) : []
        if (hide) h.push(id)
        d.surfaces[surface].hidden = h
        prefs.data = d
        prefs._bump()
    }
    // Move a key by `delta` places among the visible keys of the deck (separators move with the gaps between them).
    function moveKey(surface, id, delta, defaultOrder) {
        const cur = prefs.order(surface, defaultOrder)
        const keys = cur.filter(k => k !== "|")
        const i = keys.indexOf(id)
        if (i < 0) return false
        const j = Math.max(0, Math.min(keys.length - 1, i + delta))
        if (j === i) return false
        // rebuild: the separators stay where they are by position, the keys change places
        const out = cur.slice()
        const positions = []
        for (let k = 0; k < out.length; ++k) if (out[k] !== "|") positions.push(k)
        keys.splice(i, 1); keys.splice(j, 0, id)
        for (let k = 0; k < positions.length; ++k) out[positions[k]] = keys[k]
        const hide = prefs.hidden(surface)
        prefs.setOrder(surface, out.concat(hide))
        return true
    }
    function resetSurface(surface) {
        const d = prefs.clone(prefs.data)
        delete d.surfaces[surface]
        for (const key in d.buttons) if (key.indexOf(surface + "/") === 0) delete d.buttons[key]
        prefs.data = d
        prefs._bump()
    }
    function resetAll() { prefs.data = prefs.empty(); prefs._bump() }

    // --- the backend ------------------------------------------------------------------------------------------------
    // Use `backend` ({get, put}) and load now. The app passes `api`; a test passes a fake.
    function attach(backend) {
        prefs.backend = backend !== undefined ? backend : null
        if (prefs.backend) prefs.load()
    }
    // Apply a stored value (an object, or the JSON string the Supervisor keeps). Returns validate()'s ok.
    function apply(value) {
        const v = prefs.validate(value)
        prefs.errors = v.errors
        for (const e of v.errors) prefs.warnOnce(e, e)
        if (v.ok) { prefs.data = v.clean; prefs.revision++; prefs.changed() }
        return v.ok
    }
    // GET the setting. A local unsaved edit is kept (the reload is skipped); the setting absent is an empty object.
    function load(cb) {
        if (!prefs.backend) { if (cb) cb(false); return }
        if (prefs.dirty) { if (cb) cb(true); return }
        prefs.loading = true
        prefs.backend.get(prefs.path, function(ok, d) {
            prefs.loading = false
            let fine = false
            if (ok) {
                const raw = prefs.isObj(d) && d.value !== undefined ? d.value : null
                if (prefs.dirty) fine = true                         // an edit began while the answer was on its way: keep it
                else if (raw === null || raw === "") { prefs.data = prefs.empty(); prefs.errors = []; prefs.revision++; prefs.changed(); fine = true }
                else fine = prefs.apply(raw)
                if (!fine) prefs.lastError = "ui.buttons: " + (prefs.errors.length ? prefs.errors[0] : "unreadable")
            } else prefs.lastError = "ui.buttons: could not load (" + (prefs.isObj(d) && d.error ? d.error : "no answer") + ")"
            prefs.loaded = true
            prefs.loadDone(fine)
            if (cb) cb(fine)
        })
    }
    // A page with customised keys opened: reload unless one ran in the last touchMs.
    function touch() {
        const now = Date.now()
        if (prefs._touched !== 0 && now - prefs._touched >= 0 && now - prefs._touched < prefs.touchMs) return false
        prefs._touched = now
        prefs.load()
        return true
    }
    // PUT the whole object (the route keeps objects only: an array would be stored as {}).
    function save(cb) {
        if (!prefs.backend) { prefs.saveDone(false); if (cb) cb(false); return }
        const body = { value: prefs.clone(prefs.data) }
        body.value.version = prefs.version
        prefs.backend.put(prefs.path, body, function(ok, d) {
            if (ok) prefs.dirty = false
            else prefs.lastError = "ui.buttons: could not save (" + (prefs.isObj(d) && d.error ? d.error : "no answer") + ")"
            prefs.saveDone(!!ok)
            if (cb) cb(!!ok)
        })
    }
    // Drop local edits and reload.
    function revert() { prefs.dirty = false; prefs.load() }
}
