import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".."

/**
 * Hour slot editor (PlayPlayClean HourSlotEditor): item list with timing mode,
 * in/out, transition and CG hooks; slot flags (loop, randomize ads, keep
 * program order, block type); validate, save as template or dated override.
 */
CPanel {
    id: ed
    property string channelId: supervisor.activeChannelId
    property string slotId: ""
    property var date: new Date()
    property int hour: 0
    property var slot: null
    property var items: []
    property int sel: -1
    property var issues: []
    property bool dirty: false
    property bool liveOpen: false
    property bool blockOpen: false
    property int blockEditIndex: -1
    // Stacked block conditions (top-level rules; nested groups from JSON/recipes are kept and shown as one row)
    property var blockRules: []
    readonly property var ruleFields: ["type", "category", "subcategory", "series", "tag", "client", "title", "filename", "path", "codec",
                                       "duration", "durationMin", "ageDays", "episodeAgeDays", "lastPlayedDays", "playCount", "height", "width", "priority"]
    readonly property var ruleOps: ["is", "is_not", "contains", "not_contains", "starts_with", "ends_with", "in", "not_in", "lt", "lte", "gt", "gte", "between", "exists", "empty"]
    property bool fillOpen: false
    function addRule() { var a = blockRules.slice(); a.push({ field: "ageDays", op: "lt", value: "14" }); blockRules = a }
    function removeRule(i) { var a = blockRules.slice(); a.splice(i, 1); blockRules = a }
    /// Age limits + rules from the block dialog, with explicit "none" values so an edit can clear what a block had.
    function blockCond() {
        var rules = blockRules.filter(function(r) { return r.rules || (r.field && r.op) }).map(function(r) {
            if (r.rules) return r
            var v = String(r.value === undefined ? "" : r.value)
            var num = ["lt", "lte", "gt", "gte", "between"].indexOf(r.op) >= 0
            var out = { field: r.field, op: r.op }
            if (r.op !== "exists" && r.op !== "empty") out.value = num && r.op !== "between" ? (parseFloat(v) || 0) : v
            if (r.not) out.not = true
            return out
        })
        return { maxAgeDays: parseFloat(blockNewer.text) || 0, minAgeDays: parseFloat(blockOlder.text) || 0, ageField: blockAgeField.currentText,
                 rules: rules, match: blockMatch.currentText }
    }
    function hasCond(c) { return c.maxAgeDays > 0 || c.minAgeDays > 0 || c.rules.length > 0 }
    /// Fill the hour from a type or a folder with timed ad breaks and an age limit (an ad-hoc recipe, laid unsaved).
    function fillHour() {
        var what = fillWhat.text.trim()
        if (!what.length) { window.toast(fillSrc.currentText === "folder" ? "Folder is required" : "Type is required", "error"); return }
        var every = parseFloat(fillEvery.text) || 0, brk = parseFloat(fillBreak.text) || 0
        var prog = { label: fillSrc.currentText === "folder" ? what.replace(/^.*[\\/]/, "") : what, minutes: every > 0 ? every : 60, prefer: fillPrefer.currentText,
                     maxAgeDays: parseFloat(fillNewer.text) || 0, minAgeDays: parseFloat(fillOlder.text) || 0 }
        if (fillSrc.currentText === "folder") prog.folder = what; else prog.types = toList(what)
        var parts = [prog]
        if (every > 0 && brk > 0) parts.push({ label: "Break", types: toList(fillAds.text).length ? toList(fillAds.text) : ["ad"], minutes: brk, prefer: "even" })
        var sid = slotIdFor(isDated())
        api.post("/schedule/" + channelId + "/hour-recipe", { recipe: { name: "Fill " + prog.label, parts: parts, idAtTop: fillId.checked }, scope: "hour", slotId: sid, keepLocks: true, apply: false }, function(ok, r) {
            if (!ok) { window.toast("Fill failed: " + (r && r.error ? r.error : "see log"), "error"); return }
            var sl = r.slots ? r.slots[sid] : null; if (!sl) { window.toast("Nothing laid", "error"); return }
            items = sl.items || []; dirty = true; sel = -1; fillOpen = false
            ed.resolved = r.runlists && r.runlists[sid] ? r.runlists[sid] : null
            var pv = r.previews && r.previews[sid] ? r.previews[sid] : null
            if (pv && pv.unresolved) window.toast(pv.unresolved + " block(s) found no files; see the red lines under them", "error"); else window.toast("Hour filled: " + items.length + " block(s)" + (parts.length > 1 ? ", a " + brk + " min break every " + every + " min" : "") + " — the files each block picks are listed under it; Save to keep", "ok")
        })
    }
    property var hourRecipes: []       // [{id,name}] from /programming/recipes (+ default)
    property var resolved: null        // runlist preview from /slot/{id}/resolved
    // What each block resolved to in that preview: blockItemId -> [runlist items]; unresolved blocks -> the builder's reason.
    property var picksByBlock: {
        var m = {}
        var list = resolved && resolved.items ? resolved.items : []
        for (var i = 0; i < list.length; ++i) { var it = list[i]; if (!it.queryFill || !it.blockItemId) continue; (m[it.blockItemId] = m[it.blockItemId] || []).push(it) }
        return m
    }
    property var whyByBlock: {
        var m = {}
        var notes = resolved && resolved.notes ? resolved.notes : []
        for (var i = 0; i < items.length; ++i) {
            var it = items[i]; if (!isBlock(it)) continue
            for (var k = 0; k < notes.length; ++k) {
                var n = String(notes[k])
                if (n.indexOf("unresolved") >= 0 && (n.indexOf("'" + (it.title || "") + "'") >= 0 || n.indexOf("'" + it.id + "'") >= 0)) { m[it.id] = n.substring(n.indexOf("unresolved") + 12); break }
            }
        }
        return m
    }
    function picksLine(it) {
        var p = picksByBlock[it.id]
        if (p && p.length) {
            var names = p.map(function(x) { return (x.title || String(x.path || "").replace(/^.*[\\/]/, "")) + (x.overrun ? " (cut)" : "") + (x.repeat ? " (repeat)" : "") })
            return "\u2192 " + names.slice(0, 6).join(" \u00B7 ") + (names.length > 6 ? "  +" + (names.length - 6) + " more" : "")
        }
        if (whyByBlock[it.id]) return "\u2192 nothing: " + whyByBlock[it.id]
        return ""
    }
    // Bumped by typed(): read-only displays that depend on a typed field (total, duration, lock badge) list it as a dependency, because
    // typed() changes a row in place instead of republishing `items` (see typed()).
    property int rev: 0
    function isBlock(it) { return !!(it && ((it.queryBlock && Object.keys(it.queryBlock).length) || (it.folderBlock && Object.keys(it.folderBlock).length))) }
    function blockLabel(it) {
        var q = it.queryBlock || {}, f = it.folderBlock || {}
        if (Object.keys(f).length) return "\uD83D\uDCC1 " + (f.folder || "") + " (" + (f.order && f.order !== "random" ? "by " + f.order : "random") + ")"
        var what = (q.series && q.series.length) ? q.series.join("/") : (q.subcategories && q.subcategories.length) ? q.subcategories.join("/") : (q.categories && q.categories.length) ? q.categories.join("/") : (q.types || []).join("/")
        return "\u26A1 " + what + (q.targetSec ? " ~" + Math.round(q.targetSec / 60) + "m" : "") + " (" + String(q.prefer || "least_played").replace("_", " ") + ")"
    }
    function toList(v) { return String(v || "").split(",").map(function(x) { return x.trim() }).filter(function(x) { return x.length }) }
    function loadHourRecipes() { api.get("/programming/recipes", function(ok, d) { if (!ok) return; var l = [{ id: "default", name: (d["default"] && d["default"].name) || "Default" }]; (d.builtin || []).forEach(function(b) { l.push({ id: b.id, name: "\u2605 " + b.name }) }); ed.hourRecipes = l.concat(d.recipes || []) }) }
    function slotIdFor(dated) { return dated ? Qt.formatDate(date, "yyyy-MM-dd") + "-" + hour : date.getDay() + "-" + hour }
    /// Lay a recipe / weighted fill into the editor (unsaved) so the operator can look before saving.
    function layRecipe(recipeId) {
        api.post("/schedule/" + channelId + "/hour-recipe", { recipeId: recipeId, scope: "hour", slotId: slotIdFor(isDated()), keepLocks: true, apply: false }, function(ok, r) {
            if (!ok) { window.toast("Recipe failed", "error"); return }
            var sid = slotIdFor(isDated())
            var sl = r.slots[sid]; if (!sl) { window.toast("Nothing laid", "error"); return }
            items = sl.items || []; dirty = true; sel = -1
            ed.resolved = r.runlists && r.runlists[sid] ? r.runlists[sid] : null
            var pv = r.previews && r.previews[sid] ? r.previews[sid] : null
            window.toast("Recipe laid" + (pv ? ": " + pv.picks + " file(s) picked" + (pv.unresolved ? ", " + pv.unresolved + " block(s) found nothing" : "") : "") + " — save to keep it", pv && pv.unresolved ? "error" : "ok")
        })
    }
    function layWeighted() {
        api.post("/schedule/" + channelId + "/weighted-fill", { scope: "hour", slotId: slotIdFor(isDated()), keepLocks: true, apply: false }, function(ok, r) {
            if (!ok) { window.toast("Weighted fill failed", "error"); return }
            var sl = r.slots[slotIdFor(isDated())]; if (!sl) return
            items = sl.items || []; dirty = true; sel = -1; window.toast((sl.items || []).length + " items — save to keep", "ok")
        })
    }
    function shuffleAds() {
        var free = []; for (var i = 0; i < items.length; ++i) { var it = items[i]; if (!it.isLocked && it.type !== "program" && it.type !== "live" && it.timingMode !== "hard_start") free.push(i) }
        var pool = free.map(function(i) { return items[i] }); for (var k = pool.length - 1; k > 0; --k) { var j = Math.floor(Math.random() * (k + 1)); var t = pool[k]; pool[k] = pool[j]; pool[j] = t }
        var a = items.slice(); for (var m = 0; m < free.length; ++m) a[free[m]] = pool[m]; items = a; dirty = true
    }
    function clearKeepLocks() { items = items.filter(function(it) { return it.isLocked }); dirty = true; sel = -1 }
    function resolveNow() {
        api.post("/schedule/" + channelId + "/slot/" + (slotId.length ? slotId : slotIdFor(false)) + "/resolved", body(isDated()), function(ok, r) { if (ok) ed.resolved = r; else window.toast("Resolve failed", "error") })
    }
    /// One user action = ONE atomic change: merge `fields` into row i and publish a new array once. A field set to undefined is removed from
    /// the row (never sent as -1: the server read -1 as ":00" and re-locked the item). Handlers used to call setField() twice; the first
    /// call republished `items`, and the delegate that was running the handler was torn down under the second ("ed is not defined").
    function patch(i, fields) {
        if (i < 0 || i >= items.length) return
        var a = items.slice(); var o = Object.assign({}, a[i])
        for (var k in fields) { if (fields[k] === undefined) delete o[k]; else o[k] = fields[k] }
        a[i] = o; items = a; dirty = true
    }
    /// Per-keystroke edit of a text field. The row is changed IN PLACE and `items` is not republished: republishing rebuilt the row (and the
    /// field being typed in lost its focus, or was rewritten to the normalised value mid-typing: "5" -> "00:05", "a," -> "a").
    /// The model is the item count and the rows read ed.items[index], so the next structural change picks the value up.
    function typed(i, fields) {
        if (i < 0 || i >= items.length) return
        var o = items[i]
        for (var k in fields) { if (fields[k] === undefined) delete o[k]; else o[k] = fields[k] }
        dirty = true; rev++
    }
    /// "mm:ss" or plain seconds -> seconds (NaN while the text is not a number yet, e.g. "05:").
    function parseMmSs(v) { var p = String(v).split(":"); return p.length === 2 ? parseInt(p[0]) * 60 + parseInt(p[1]) : parseFloat(v) }
    function setLock(i, locked, atSec) {
        var f = { isLocked: locked }
        if (!locked) { f.lockAtSec = undefined; f.lockLabel = undefined }              // unlocked: no anchor, no label
        else if (atSec !== undefined) f.lockAtSec = atSec >= 0 ? atSec : undefined     // a negative anchor means "none": drop the key
        patch(i, f)
    }
    function addBlock() {
        var secs = Math.max(0, (parseFloat(blockMin.text) || 0) * 60); var it
        if (blockKind.currentText === "folder") {
            if (!blockFolder.text.trim().length) { window.toast("Folder is required", "error"); return }
            it = { folderBlock: { folder: blockFolder.text.trim(), targetSec: secs, order: blockOrder.currentText }, type: "program" }
        } else {
            var types = toList(blockTypes.text)
            if (!types.length && !toList(blockCats.text).length && !toList(blockSeries.text).length && !toList(blockSubs.text).length && !hasCond(blockCond())) { window.toast("Types, categories, series, an age limit or a rule required", "error"); return }
            it = { queryBlock: { types: types, targetSec: secs, prefer: blockPrefer.currentText, categories: toList(blockCats.text), subcategories: toList(blockSubs.text), series: toList(blockSeries.text) }, type: types.length ? types[0] : "program" }
        }
        var cnd = blockCond()
        if (it.queryBlock) Object.assign(it.queryBlock, cnd); else Object.assign(it.folderBlock, cnd)
        var a = items.slice(); var old = blockEditIndex >= 0 ? a[blockEditIndex] : null
        if (old) {
            // Edit: merge the six dialog fields over the block that is already on the item. The dialog has no inputs for nthLatest, noRepeatMin,
            // tags, minSec, recursive, weight ... (recipes and generators set them) and a shallow replace silently dropped all of them.
            var m = Object.assign({}, old, { duration: secs, type: it.type })
            if (it.queryBlock) { m.queryBlock = Object.assign({}, old.queryBlock || {}, it.queryBlock); delete m.folderBlock }
            else { m.folderBlock = Object.assign({}, old.folderBlock || {}, it.folderBlock); delete m.queryBlock; if (old.folderBlock && old.type) m.type = old.type }
            m.title = blockLabel(m); a[blockEditIndex] = m
        } else {
            if (it.folderBlock) it.folderBlock.weight = "freshness"
            Object.assign(it, { mediaId: "", path: "", duration: secs, timeIn: 0, timeOut: 0, transitionType: "cut", priority: 0 }); it.title = blockLabel(it)
            a.push(Object.assign({ id: "q" + Date.now().toString(36) + Math.floor(Math.random() * 1000), timingMode: "soft_start" }, it))
        }
        items = a; dirty = true; blockOpen = false; blockEditIndex = -1
    }
    function addFolderBlockFor(m) {
        if (!slotId.length) { window.toast("Pick an hour in the grid first"); return }
        var folder = String(m.path || "").replace(/[\\/][^\\/]*$/, "")
        var it = { id: "q" + Date.now().toString(36), mediaId: "", path: "", title: "", type: m.type || "program", folderBlock: { folder: folder, targetSec: 0, order: "random", weight: "freshness" }, duration: m.duration || 0, timeIn: 0, timeOut: 0, timingMode: "soft_start", transitionType: "cut", priority: 0 }
        it.title = blockLabel(it)
        var a = items.slice(); a.push(it); items = a; dirty = true
    }
    function editBlock(i) {
        var it = items[i]; var q = it.queryBlock || {}, f = it.folderBlock || {}
        if (Object.keys(f).length) { blockKind.currentIndex = 1; blockFolder.text = f.folder || ""; blockOrder.currentIndex = Math.max(0, blockOrder.model.indexOf(f.order || "random")); blockMin.text = String((f.targetSec || 0) / 60) }
        else { blockKind.currentIndex = 0; blockTypes.text = (q.types || []).join(", "); blockCats.text = (q.categories || []).join(", "); blockSubs.text = (q.subcategories || []).join(", "); blockSeries.text = (q.series || []).join(", "); blockPrefer.currentIndex = Math.max(0, blockPrefer.model.indexOf(q.prefer || "least_played")); blockMin.text = String((q.targetSec || 0) / 60) }
        var c = Object.keys(f).length ? f : q
        blockNewer.text = c.maxAgeDays > 0 ? String(c.maxAgeDays) : ""; blockOlder.text = c.minAgeDays > 0 ? String(c.minAgeDays) : ""
        blockAgeField.currentIndex = c.ageField === "episode" ? 1 : 0
        blockMatch.currentIndex = c.match === "any" ? 1 : 0
        blockRules = JSON.parse(JSON.stringify(c.rules || []))
        blockEditIndex = i; blockOpen = true
    }
    property var recipes: ({})
    property var mediaDur: ({})
    property var mediaInfo: ({})
    property var recipeNames: Object.keys(recipes)
    signal saved()
    signal closed()
    title: slotId.length ? "Hour " + Theme.pad2(hour) + ":00 — " + Qt.formatDate(date, "ddd MMM d") + (isDated() ? " (dated override)" : " (weekly template)") : "Hour editor"
    padding: 0
    property real totalSec: { rev; var t = 0; for (var i = 0; i < items.length; ++i) t += itemDur(items[i]); return t }

    function isDated() { return slotId.length > 5 && slotId.indexOf("-") === 4 }
    function itemDur(it) { var full = it.duration || mediaDur[it.mediaId] || 0; var d = it.timeOut > 0 ? it.timeOut - (it.timeIn || 0) : full - (it.timeIn || 0); return Math.max(0, d) }
    function loadMedia() { api.get("/media", function(ok, d) { if (!ok) return; var m = {}, info = {}; var list = d.media || []; for (var i = 0; i < list.length; ++i) { m[list[i].id] = list[i].duration || 0; info[list[i].id] = list[i] } ed.mediaDur = m; ed.mediaInfo = info }) }
    /// Push a slot's (or recipe's / clipboard hour's) flags into the toggles and the block selector. CToggle assigns `checked` itself on a click,
    /// which removes any binding to the slot, and blockSel was only set once at creation: so every load must set them explicitly or hour B
    /// is edited and saved with hour A's flags (a looping hour A made the next hour loop on air).
    function applyFlags(s) {
        loopT.checked = !!(s && s.isLooping)
        randT.checked = !s || s.randomizeAds !== false
        keepT.checked = !s || s.keepProgramOrder !== false
        var bi = blockSel.model.indexOf((s && s.blockType) || "general"); blockSel.currentIndex = bi >= 0 ? bi : 0
    }
    function load(id, s, d, h) {
        slotId = id; date = d; hour = h; slot = s; issues = []; sel = -1; dirty = false; resolved = null
        items = s && s.items ? JSON.parse(JSON.stringify(s.items)) : []
        applyFlags(s)
    }
    // --- drop files / folders from Explorer straight into this hour (registered with the window drop router) ---
    property string dropLabel: slotId.length ? "Add to hour " + Theme.pad2(hour) + ":00" : "Pick an hour in the grid first"
    Component.onDestruction: { window.unregisterDropTarget(ed) }
    function dropPaths(paths, x, y) {
        if (!slotId.length) { window.toast("Pick an hour in the grid first"); return }
        // insert after the row under the pointer (end of the list when below the last row)
        var lp = list.mapFromItem(ed, x, y)
        var row = (lp.y >= 0 && lp.y < list.height) ? list.indexAt(lp.x + list.contentX, lp.y + list.contentY) : -1
        var target = row >= 0 ? list.itemAtIndex(row) : null
        var at = row >= 0 ? row + (target && lp.y + list.contentY - target.y < target.height / 2 ? 0 : 1) : items.length
        var tag = "drop" + Date.now().toString(36), sid = slotId
        var a = items.slice(), ph = []
        for (var k = 0; k < paths.length; ++k) {
            var name = String(paths[k]).replace(/^file:\/+/, "").replace(/[\/]+$/, "").replace(/^.*[\/]/, "")
            ph.push({ id: tag + "-" + k, pending: tag, title: "⏳ " + decodeURIComponent(name), path: "", mediaId: "", type: "program", duration: 0, timeIn: 0, timeOut: 0, timingMode: "soft_start", transitionType: "cut", priority: 0 })
        }
        Array.prototype.splice.apply(a, [at, 0].concat(ph)); items = a; dirty = true
        window.resolvePaths(paths, function(r) {
            var b = ed.items.slice(), pos = -1
            for (var i = b.length - 1; i >= 0; --i) if (b[i].pending === tag) { pos = i; b.splice(i, 1) }
            if (ed.slotId !== sid) { window.toast("Hour changed before the drop finished; nothing added", "error"); return }
            if (pos < 0) pos = b.length
            var add = [], bad = 0
            var got = r && r.items ? r.items : []
            for (var j = 0; j < got.length; ++j) { if (got[j].duration > 0) add.push(window.newPlaylistItem(got[j])); else ++bad }
            Array.prototype.splice.apply(b, [pos, 0].concat(add)); ed.items = b; ed.dirty = true; ed.sel = add.length ? pos : -1
            if (add.length) Qt.callLater(function() { ed.checkOverrun() })
            if (r) window.toast("Added " + add.length + " item(s) to " + Theme.pad2(ed.hour) + ":00" + (bad ? ", " + bad + " unreadable" : "") + (r.skipped ? ", " + r.skipped + " non-media skipped" : "") + " — Save to keep", add.length ? "ok" : "error")
        })
    }
    // --- a programme that runs past the top of the hour: ask what to do (continue / break up / cut / remove) ---
    function findOverrun() {
        var t = 0
        for (var i = 0; i < items.length; ++i) {
            var it = items[i], d = itemDur(it)
            if (t + d > 3602 && !it.overrunMode && !it.isLocked) return { index: i, item: it, offset: t, dur: d, over: t + d - 3600 }
            t += d
        }
        return null
    }
    /// Opens the overrun dialog when an item runs past the hour; true when it did (a save waits for the answer).
    function checkOverrun(forDate, fromSave) {
        if (!slotId.length) return false
        var o = findOverrun(); if (!o) return false
        overrunDlg.ask(o, forDate === undefined ? isDated() : forDate, !!fromSave)
        return true
    }
    function applyOverrun(mode) {
        var o = overrunDlg.info; if (!o) return
        if (mode === "remove") { remove(o.index); overrunDlg.close(); window.toast("Removed " + (o.item.title || "the item"), "ok"); return }
        if (mode === "asis") { patch(o.index, { overrunMode: "accepted" }); overrunDlg.close(); save(overrunDlg.dated, true); return }
        var target = overrunDlg.dated ? Qt.formatDate(date, "yyyy-MM-dd") + "-" + hour : date.getDay() + "-" + hour
        var nextMode = ["keep", "ads", "recipe"][ovNext.currentIndex]
        var b = { slot: body(overrunDlg.dated), itemId: o.item.id, mode: mode, next: nextMode,
                  breakMin: parseFloat(ovBreak.text) || 2, breakEveryMin: parseFloat(ovEvery.text) || 15 }
        if (nextMode === "recipe") b.recipeId = (ed.hourRecipes[ovRecipe.currentIndex] || { id: "default" }).id
        api.post("/schedule/" + channelId + "/slot/" + target + "/overrun", b, function(ok, r) {
            if (!ok) { window.toast("Could not apply: " + (r && r.error ? r.error : "see log"), "error"); return }
            overrunDlg.close()
            load(target, r.slot, date, hour)
            window.toast((r.notes && r.notes.length ? r.notes[0] : "Applied") + ". Saved this hour and " + r.nextSlotId, "ok")
            ed.saved()
        })
    }
    function addMedia(m, at) {
        if (!slotId.length) { window.toast("Pick an hour in the grid first"); return }
        var it = { id: "i" + Date.now().toString(36) + Math.floor(Math.random() * 1000), mediaId: m.id, title: m.title, path: m.path, type: m.type || "video",
                   duration: m.duration || 0, timeIn: 0, timeOut: 0, timingMode: "soft_start", transitionType: "cut", priority: 0 }
        if (m.type === "live" && m.liveSource) { it.liveSource = m.liveSource; it.timingMode = "fill_until"; it.duration = m.duration || 1800; it.path = "" }
        var a = items.slice(); var pos = at === undefined ? a.length : Math.max(0, Math.min(a.length, at)); a.splice(pos, 0, it); items = a; sel = pos; dirty = true
        if (it.type !== "live") Qt.callLater(function() { ed.checkOverrun() })
    }
    /// Live input / stream as a program item: NDI name, SDI port or a URL, with a duration or fill-until.
    function addLive(kind, ref, title, seconds) {
        if (!slotId.length) { window.toast("Pick an hour in the grid first"); return }
        var src = { type: kind }
        if (kind === "ndi") src.name = ref; else if (kind === "sdi_in") src.port = parseInt(ref) || 1; else { src.url = ref; src.realtime = true }
        var it = { id: "i" + Date.now().toString(36) + Math.floor(Math.random() * 1000), mediaId: "", title: title || (kind.toUpperCase() + " " + ref), path: "", type: "live",
                   liveSource: src, duration: seconds > 0 ? seconds : 1800, timeIn: 0, timeOut: 0, timingMode: seconds > 0 ? "soft_start" : "fill_until", transitionType: "mix", transitionSec: 0.5, priority: 0 }
        var a = items.slice(); a.push(it); items = a; sel = a.length - 1; dirty = true
    }
    function copyItems(idx) { var a = []; for (var k = 0; k < idx.length; ++k) a.push(items[idx[k]]); window.setClip("items", a, a.length + " item(s)") }
    function cutItems(idx) { copyItems(idx); var a = items.filter(function(it, k) { return idx.indexOf(k) < 0 }); items = a; sel = -1; dirty = true }
    function pasteItems(at) {
        if (!window.clip || window.clip.kind !== "items") { if (window.clip && window.clip.kind === "hour") { pasteHour(); return } window.toast("Clipboard has no items"); return }
        var a = items.slice(); var ins = JSON.parse(JSON.stringify(window.clip.data)).map(function(it) { it.id = "p" + Date.now().toString(36) + Math.floor(Math.random() * 10000); return it })
        var pos = at < 0 || at > a.length ? a.length : at
        for (var k = 0; k < ins.length; ++k) a.splice(pos + k, 0, ins[k])
        items = a; dirty = true; sel = pos; window.toast("Pasted " + ins.length + " item(s)", "ok")
        Qt.callLater(function() { ed.checkOverrun() })
    }
    function copyHour() { window.setClip("hour", { items: items, isLooping: loopT.checked, randomizeAds: randT.checked, keepProgramOrder: keepT.checked, blockType: blockSel.currentText }, "hour " + Theme.pad2(hour) + ":00") }
    function pasteHour() {
        if (!window.clip || window.clip.kind !== "hour") { window.toast("Clipboard has no hour"); return }
        var h = window.clip.data; items = JSON.parse(JSON.stringify(h.items || [])).map(function(it) { if (!it.isLocked) it.id = "p" + Date.now().toString(36) + Math.floor(Math.random() * 10000); return it })
        applyFlags(h)
        dirty = true; window.toast("Pasted hour — save to keep it", "ok")
    }
    function offsetOf(i) { var t = 0; for (var k = 0; k < i && k < items.length; ++k) t += itemDur(items[k]); return t }
    function moveTo(id, at) { var a = items.slice(), from = -1; for (var i = 0; i < a.length; ++i) if (a[i].id === id) { from = i; break }; if (from < 0) return; var it = a.splice(from, 1)[0]; if (from < at) --at; at = Math.max(0, Math.min(a.length, at)); a.splice(at, 0, it); items = a; sel = at; dirty = true }
    function estimatedStart(i) { var t = 0; for (var k = 0; k <= i; ++k) { var it = items[k]; if (it.timingMode === "hard_start" && it.fixedOffsetSec >= 0) t = Math.max(t, it.fixedOffsetSec); if (k === i) return Theme.fmtTime(new Date(date.getFullYear(), date.getMonth(), date.getDate(), hour, 0, 0).getTime() + t * 1000, true); if (isBlock(it) && !itemDur(it)) return "—"; t += itemDur(it) } return "—" }
    function setField(i, k, v) { var f = {}; f[k] = v; patch(i, f) }
    function move(i, d) { var j = i + d; if (j < 0 || j >= items.length) return; var a = items.slice(); var t = a[i]; a[i] = a[j]; a[j] = t; items = a; sel = j; dirty = true }
    function remove(i) { var a = items.slice(); a.splice(i, 1); items = a; sel = -1; dirty = true }
    function body(forDate) {
        var s = Object.assign({}, slot || {})
        s.channelId = channelId; s.hour = hour; s.items = items
        if (forDate) { s.date = Qt.formatDate(date, "yyyy-MM-dd"); s.dayOfWeek = -1 } else { s.dayOfWeek = date.getDay(); delete s.date }
        s.isLooping = loopT.checked; s.randomizeAds = randT.checked; s.keepProgramOrder = keepT.checked; s.blockType = blockSel.currentText
        return s
    }
    function save(forDate, skipOverrun) {
        if (!skipOverrun && checkOverrun(forDate, true)) return      // the dialog decides first (and saves both hours)
        var id = forDate ? Qt.formatDate(date, "yyyy-MM-dd") + "-" + hour : date.getDay() + "-" + hour
        api.put("/schedule/" + channelId + "/slot/" + id, body(forDate), function(ok, data) {
            if (ok) { window.toast("Saved " + id, "ok"); dirty = false; slotId = id; ed.saved() }
        })
    }
    function validate() {
        api.post("/schedule/" + channelId + "/slot/" + (slotId.length ? slotId : date.getDay() + "-" + hour) + "/validate", body(isDated()), function(ok, data) {
            if (ok) { issues = data.issues || []; if (!issues.length) window.toast("Hour validates clean", "ok") }
        })
    }
    // Recipes: a named hour layout (items + flags) stored in the Supervisor settings table.
    function loadRecipes() {
        api.get("/settings/hour_recipes", function(ok, d) {
            if (!ok) return
            try { ed.recipes = d.value ? JSON.parse(d.value) : {} } catch (e) { ed.recipes = {} }
        })
    }
    function saveRecipe(name) {
        if (!name.length) { window.toast("Name the recipe first"); return }
        // items are copied: typed() edits rows in place, so the stored recipe must not share them with the editor
        var r = Object.assign({}, recipes); r[name] = { items: JSON.parse(JSON.stringify(items)), isLooping: loopT.checked, randomizeAds: randT.checked, keepProgramOrder: keepT.checked, blockType: blockSel.currentText }
        api.put("/settings/hour_recipes", { value: JSON.stringify(r) }, function(ok) { if (ok) { ed.recipes = r; window.toast("Recipe saved: " + name, "ok") } })
    }
    function applyRecipe(name) {
        var r = recipes[name]; if (!r) return
        items = JSON.parse(JSON.stringify(r.items || [])).map(function(it) { it.id = "i" + Date.now().toString(36) + Math.floor(Math.random() * 10000); return it })
        applyFlags(r)
        dirty = true; window.toast("Applied recipe " + name, "ok")
    }
    function deleteRecipe(name) { var r = Object.assign({}, recipes); delete r[name]; api.put("/settings/hour_recipes", { value: JSON.stringify(r) }, function(ok) { if (ok) ed.recipes = r }) }
    Component.onCompleted: { loadRecipes(); loadMedia(); loadHourRecipes(); window.registerDropTarget(ed) }
    Shortcut { sequence: "Ctrl+C"; enabled: ed.visible && ed.sel >= 0 && !ed.activeFocusItemIsText(); onActivated: { ed.copyItems([ed.sel]) } }
    Shortcut { sequence: "Ctrl+X"; enabled: ed.visible && ed.sel >= 0 && !ed.activeFocusItemIsText(); onActivated: { ed.cutItems([ed.sel]) } }
    Shortcut { sequence: "Ctrl+V"; enabled: ed.visible && ed.slotId.length > 0 && !ed.activeFocusItemIsText(); onActivated: { ed.pasteItems(ed.sel >= 0 ? ed.sel + 1 : -1) } }
    Shortcut { sequence: "Delete"; enabled: ed.visible && ed.sel >= 0 && !ed.activeFocusItemIsText(); onActivated: { ed.remove(ed.sel) } }
    function activeFocusItemIsText() { var f = Window.activeFocusItem; return !!(f && (f.hasOwnProperty("cursorPosition") || f.hasOwnProperty("editText"))) }
    function deleteSlot() { api.del("/schedule/" + channelId + "/slot/" + slotId, function(ok) { if (ok) { window.toast("Deleted " + slotId, "ok"); items = []; slot = null; applyFlags(null); ed.saved() } }) }


    component OvRadio: RadioButton {
        id: orb
        Layout.fillWidth: true
        contentItem: Text { text: orb.text; color: orb.checked ? Theme.text0 : Theme.text1; font.pixelSize: Theme.fs12; font.bold: orb.checked
                            wrapMode: Text.WordWrap; verticalAlignment: Text.AlignVCenter; leftPadding: orb.indicator.width + orb.spacing }
        indicator: Rectangle { x: orb.leftPadding; y: (orb.height - height) / 2; width: 18; height: 18; radius: 9; color: Theme.bg4; border.color: orb.checked ? Theme.accent0 : Theme.border1; border.width: 2
                               Rectangle { anchors.centerIn: parent; width: 8; height: 8; radius: 4; color: Theme.accent0; visible: orb.checked } }
    }
    Popup {
        id: overrunDlg
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true; focus: true; closePolicy: Popup.CloseOnEscape
        width: Math.min(parent ? parent.width - 80 : 680, 680)
        padding: 20
        property var info: null
        property bool dated: false
        property bool fromSave: false
        property string mode: "continue"
        property int segCount: 0
        readonly property string nextLabel: Theme.pad2((ed.hour + 1) % 24) + ":00"
        background: Rectangle { color: Theme.bg2; radius: Theme.r8; border.color: Theme.accent0; border.width: 1 }
        function ask(o, forDate, save) {
            info = o; dated = forDate; fromSave = save; mode = "continue"; segCount = 0; ovNext.currentIndex = 0
            if (o.item.mediaId) api.get("/media/" + o.item.mediaId + "/segments", function(ok, sg) { if (ok) overrunDlg.segCount = (sg.segments || []).length })
            open()
        }
        contentItem: ColumnLayout {
            spacing: 10
            Text { Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.text0; font.pixelSize: Theme.fs16; font.bold: true
                   text: overrunDlg.info ? "\u201C" + (overrunDlg.info.item.title || "This item") + "\u201D runs " + Theme.fmtClock(overrunDlg.info.over) + " past the end of the hour" : "" }
            Text { Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.text2; font.pixelSize: Theme.fs12
                   text: overrunDlg.info ? "It starts " + Theme.fmtClock(overrunDlg.info.offset) + " into " + Theme.pad2(ed.hour) + ":00 and is " + Theme.fmtClock(overrunDlg.info.dur) + " long. What should happen?" : "" }
            ButtonGroup { id: ovGroup }
            OvRadio { ButtonGroup.group: ovGroup; checked: overrunDlg.mode === "continue"; onClicked: { overrunDlg.mode = "continue" }
                      text: "Continue into the next hour: it plays in full and " + overrunDlg.nextLabel + " picks up " + (overrunDlg.info ? Theme.fmtClock(overrunDlg.info.over) : "") + " late" }
            OvRadio { ButtonGroup.group: ovGroup; checked: overrunDlg.mode === "split"; onClicked: { overrunDlg.mode = "split" }
                      text: "Break it up with ad breaks " + (overrunDlg.segCount > 1 ? "at its " + overrunDlg.segCount + " segments" : "every " + (ovEvery.text || "15") + " minutes") + "; this hour ends on time and the rest opens " + overrunDlg.nextLabel }
            RowLayout { visible: overrunDlg.mode === "split"; Layout.leftMargin: 28; spacing: 8
                CField { id: ovEvery; visible: overrunDlg.segCount <= 1; Layout.preferredWidth: 110; label: "Break every (min)"; text: "15"; mono: true }
                CField { id: ovBreak; Layout.preferredWidth: 110; label: "Break length (min)"; text: "2"; mono: true } }
            OvRadio { ButtonGroup.group: ovGroup; checked: overrunDlg.mode === "cut"; onClicked: { overrunDlg.mode = "cut" }
                      text: "Cut it at the top of the hour" + (overrunDlg.info ? " (" + Theme.fmtClock(3600 - overrunDlg.info.offset) + " of it airs)" : "") }
            OvRadio { ButtonGroup.group: ovGroup; checked: overrunDlg.mode === "remove"; onClicked: { overrunDlg.mode = "remove" }
                      text: "Remove it from this hour" }
            RowLayout { visible: overrunDlg.mode === "continue" || overrunDlg.mode === "split"; Layout.fillWidth: true; spacing: 8
                Text { text: "Then the rest of " + overrunDlg.nextLabel; color: Theme.text1; font.pixelSize: Theme.fs12; Layout.alignment: Qt.AlignVCenter }
                CSelect { id: ovNext; Layout.preferredWidth: 230; model: ["keeps its own schedule", "is filled with ads and promos", "is filled from a recipe"] }
                CSelect { id: ovRecipe; visible: ovNext.currentIndex === 2; Layout.fillWidth: true; model: ed.hourRecipes.map(function(r) { return r.name }) } }
            Text { Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.text3; font.pixelSize: Theme.fs10
                   text: overrunDlg.mode === "remove" ? "Only this hour changes." : "This hour and " + overrunDlg.nextLabel + " are both saved when you apply." }
            RowLayout { Layout.fillWidth: true; spacing: 8
                Item { Layout.fillWidth: true }
                CButton { text: "Cancel"; small: true; variant: "ghost"; onClicked: { overrunDlg.close() } }
                CButton { visible: overrunDlg.fromSave; text: "Save as is"; small: true; onClicked: { ed.applyOverrun("asis") } ToolTip.text: "Save without deciding: at air the hour ends at :00 and the item is cut there" }
                CButton { text: overrunDlg.mode === "remove" ? "Remove" : "Apply"; small: true; variant: "primary"; onClicked: { ed.applyOverrun(overrunDlg.mode) } } }
        }
    }
    headerRight: [
        Text { text: Theme.fmtClock(ed.totalSec) + " / 60:00"; color: ed.totalSec > 3630 ? Theme.danger : ed.totalSec > 3570 ? Theme.ready : Theme.text1; font.family: Theme.fontMono; font.pixelSize: Theme.fs11; font.bold: true },
        CButton { icon2: "✕"; small: true; variant: "ghost"; onClicked: { ed.closed() } }
    ]

    // What the editor wants: its 34 px header, every row that is not the item list (they wrap, and the live / block / resolved panels open
    // and close) and room for about four collapsed items. ScheduleView sizes the editor from this; the old fixed 360 px left 40-88 px for the list.
    readonly property real wantHeight: 34 + edCol.implicitHeight + 150

    ColumnLayout {
        id: edCol
        anchors.fill: parent
        spacing: 0
        // flags row
        Rectangle {
            // a Flow, so the flags and buttons wrap onto more lines instead of running off the right edge (the editor is 462 px wide at the 1280 px window)
            Layout.fillWidth: true; implicitHeight: flagsFlow.implicitHeight + 14; color: Theme.bg2
            Flow {
                id: flagsFlow
                anchors.left: parent.left; anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; anchors.leftMargin: 10; anchors.rightMargin: 10; spacing: 10
                // initial state only: load() / pasteHour() / applyRecipe() set these through applyFlags() (a binding to ed.slot dies on the first click)
                CToggle { id: loopT; label: "Loop hour"; implicitWidth: 110; checked: false; onToggled: { ed.dirty = true } }
                CToggle { id: randT; label: "Randomize ads"; implicitWidth: 130; checked: true; onToggled: { ed.dirty = true } }
                CToggle { id: keepT; label: "Keep program order"; implicitWidth: 172; checked: true; onToggled: { ed.dirty = true } }
                CButton { height: 30; text: "+ Live"; small: true; icon2: "\u25CF"; variant: "ghost"; active: ed.liveOpen; onClicked: { ed.liveOpen = !ed.liveOpen } ToolTip.text: "Schedule an NDI input, SDI input or stream URL" }
                CButton { height: 30; text: "+ Block"; small: true; icon2: "\u26A1"; variant: "ghost"; active: ed.blockOpen; onClicked: { ed.blockEditIndex = -1; ed.blockOpen = !ed.blockOpen } ToolTip.text: "Add a late-bound query or folder block (resolved when the hour is built)" }
                CButton { height: 30; text: "Fill hour \u25BE"; small: true; variant: "ghost"; onClicked: { fillMenu.popup() } ToolTip.text: "Lay a recipe, weighted fill, shuffle or clear this hour" }
                CButton { height: 30; text: "Resolve"; small: true; variant: "ghost"; onClicked: { ed.resolveNow() } ToolTip.text: "Show what the executor would build from this hour right now" }
                Menu {
                    id: fillMenu
                    MenuItem { text: "Fill from a type or folder (ad breaks, age)\u2026"; onTriggered: { ed.fillOpen = true } }
                    MenuSeparator {}
                    Menu { id: recipeMenu; title: "Lay recipe into editor"
                        Instantiator { model: ed.hourRecipes; delegate: MenuItem { text: modelData.name; onTriggered: { ed.layRecipe(modelData.id) } } onObjectAdded: function(i, o) { recipeMenu.insertItem(i, o) } onObjectRemoved: function(i, o) { recipeMenu.removeItem(o) } } }
                    MenuItem { text: "Weighted fill (format clock)"; onTriggered: { ed.layWeighted() } }
                    MenuItem { text: "Shuffle ads"; onTriggered: { ed.shuffleAds() } }
                    MenuSeparator {}
                    MenuItem { text: "Clear (keep locked items)"; onTriggered: { ed.clearKeepLocks() } }
                    MenuSeparator {}
                    MenuItem { text: "Copy hour"; onTriggered: { ed.copyHour() } }
                    MenuItem { text: "Paste hour"; enabled: !!(window.clip && window.clip.kind === "hour"); onTriggered: { ed.pasteHour() } }
                    MenuItem { text: "Paste items at end"; enabled: !!(window.clip && window.clip.kind === "items"); onTriggered: { ed.pasteItems(-1) } }
                    MenuItem { text: "Recipes & format clock\u2026"; onTriggered: { window.navTo("programming") } }
                }
                CSelect { id: blockSel; width: 130; model: ["general", "show", "news", "sports", "paid_program", "maintenance"]
                    onActivated: ed.dirty = true }
            }
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.border0 }
        }
        // live input row
        Rectangle {
            visible: ed.liveOpen
            Layout.fillWidth: true; implicitHeight: liveFlow.implicitHeight + 16; color: Theme.bg3
            Flow {
                id: liveFlow
                anchors.left: parent.left; anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; anchors.leftMargin: 10; anchors.rightMargin: 10; spacing: 8
                CSelect { id: liveKind; width: 110; model: ["ndi", "sdi_in", "stream"] }
                CField { id: liveRef; width: 220; placeholder: liveKind.currentText === "ndi" ? "NDI source name (substring)" : liveKind.currentText === "sdi_in" ? "SDI port (1-4)" : "srt:// udp:// rtsp:// rtmp:// http:// URL" }
                CField { id: liveTitle; width: 140; placeholder: "title" }
                CField { id: liveDur; width: 90; placeholder: "secs / fill"; mono: true }
                CButton { height: 30; text: "Add live"; small: true; variant: "success"; onClicked: { ed.addLive(liveKind.currentText, liveRef.text.trim(), liveTitle.text.trim(), parseFloat(liveDur.text) || 0); ed.liveOpen = false } }
                Repeater { model: supervisor.activeState.liveInputs || []; CButton { height: 30; text: modelData.name; small: true; variant: "ghost"; onClicked: { liveKind.currentIndex = 0; liveRef.text = modelData.name } ToolTip.text: "Discovered NDI source" } }
            }
        }
        // fill the hour from a type or folder, with timed ad breaks and an age limit
        Rectangle {
            visible: ed.fillOpen
            Layout.fillWidth: true; implicitHeight: fillFlow.implicitHeight + 16; color: Theme.bg3
            Flow {
                id: fillFlow
                anchors.left: parent.left; anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; anchors.leftMargin: 10; anchors.rightMargin: 10; spacing: 8
                CSelect { id: fillSrc; width: 92; model: ["type", "folder"] }
                CField { id: fillWhat; width: 220; placeholder: fillSrc.currentIndex === 1 ? "folder path, category or tag" : "types: program, news"; text: "program" }
                CSelect { id: fillPrefer; width: 116; model: ["even", "least_played", "newest", "random", "oldest", "longest", "shortest"] }
                CField { id: fillNewer; width: 92; mono: true; placeholder: "newer than d" }
                CField { id: fillOlder; width: 88; mono: true; placeholder: "older than d" }
                CField { id: fillEvery; width: 96; mono: true; placeholder: "break every m"; text: "15" }
                CField { id: fillBreak; width: 84; mono: true; placeholder: "break min"; text: "2" }
                CField { id: fillAds; width: 120; placeholder: "break types"; text: "ad, promo" }
                CToggle { id: fillId; label: "ID at :00"; implicitWidth: 96; checked: true }
                CButton { height: 30; text: "Fill hour"; small: true; variant: "primary"; onClicked: { ed.fillHour() } }
                CButton { height: 30; text: "\u2715"; small: true; variant: "ghost"; onClicked: { ed.fillOpen = false } }
            }
        }
        // query / folder block row
        Rectangle {
            visible: ed.blockOpen
            Layout.fillWidth: true; implicitHeight: blockFlow.implicitHeight + 16; color: Theme.bg3
            Flow {
                id: blockFlow
                anchors.left: parent.left; anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; anchors.leftMargin: 10; anchors.rightMargin: 10; spacing: 8
                CSelect { id: blockKind; width: 100; model: ["query", "folder"] }
                CField { id: blockTypes; visible: blockKind.currentIndex === 0; width: 130; placeholder: "types: program, ad"; text: "program" }
                CField { id: blockCats; visible: blockKind.currentIndex === 0; width: 110; placeholder: "categories" }
                CField { id: blockSubs; visible: blockKind.currentIndex === 0; width: 110; placeholder: "subcategories" }
                CField { id: blockSeries; visible: blockKind.currentIndex === 0; width: 110; placeholder: "series" }
                CSelect { id: blockPrefer; visible: blockKind.currentIndex === 0; width: 120; model: ["least_played", "even", "newest", "oldest", "most_played", "random", "longest", "shortest"] }
                CField { id: blockFolder; visible: blockKind.currentIndex === 1; width: 260; placeholder: "folder path, category or tag" }
                CSelect { id: blockOrder; visible: blockKind.currentIndex === 1; width: 100; model: ["random", "even", "name", "date", "size", "added"] }
                CField { id: blockMin; width: 60; mono: true; placeholder: "min"; text: "10" }
                CField { id: blockNewer; width: 92; mono: true; placeholder: "newer than d" }
                CField { id: blockOlder; width: 88; mono: true; placeholder: "older than d" }
                CSelect { id: blockAgeField; width: 96; model: ["added", "episode"] }
                CSelect { id: blockMatch; width: 76; model: ["all", "any"] }
                CButton { height: 30; text: "+ Rule"; small: true; variant: "ghost"; onClicked: { ed.addRule() } ToolTip.text: "Stack another condition (field, test, value)" }
                Repeater {
                    model: ed.blockRules
                    delegate: Row {
                        spacing: 4
                        property var rule: modelData
                        // A nested group (from JSON or a recipe) stays intact; it can only be removed here.
                        Text { visible: !!rule.rules; height: 30; verticalAlignment: Text.AlignVCenter; text: "(" + (rule.match || "all") + " of " + (rule.rules ? rule.rules.length : 0) + " rules)"; color: Theme.accent0; font.pixelSize: Theme.fs11 }
                        CSelect { visible: !rule.rules; width: 124; model: ed.ruleFields; currentIndex: Math.max(0, ed.ruleFields.indexOf(rule.field)); onActivated: { ed.blockRules[index].field = currentText } }
                        CSelect { visible: !rule.rules; width: 104; model: ed.ruleOps; currentIndex: Math.max(0, ed.ruleOps.indexOf(rule.op)); onActivated: { ed.blockRules[index].op = currentText } }
                        CField { visible: !rule.rules; width: 110; mono: true; placeholder: "value"; text: rule.value === undefined ? "" : String(rule.value); onEdited: function(v) { ed.blockRules[index].value = v } }
                        CToggle { visible: !rule.rules; label: "not"; implicitWidth: 60; checked: !!rule.not; onToggled: { ed.blockRules[index].not = checked } }
                        CButton { height: 30; text: "\u2715"; small: true; variant: "ghost"; onClicked: { ed.removeRule(index) } }
                    }
                }
                CButton { height: 30; text: ed.blockEditIndex >= 0 ? "Update block" : "Add block"; small: true; variant: "primary"; onClicked: { ed.addBlock() } }
            }
        }
        // items
        ListView {
            id: list
            Layout.fillWidth: true; Layout.fillHeight: true
            // The model is the item COUNT and every row reads ed.items[index]: republishing `items` re-binds the rows instead of destroying
            // them, so the field being typed in keeps its focus and a handler can make several changes (patch() makes them atomic anyway).
            clip: true; model: ed.items.length
            ScrollBar.vertical: ScrollBar {}
            DropArea { anchors.fill: parent; visible: ed.items.length === 0; keys: ["playplay-media"]
                onDropped: function(drop) { if (drop.source && drop.source.dragMedia) ed.addMedia(drop.source.dragMedia) }
            }
            delegate: Rectangle {
                id: rowD
                property var rowItem: ed.items[index] || ({})
                width: list.width
                // collapsed 36; selected = header + the property grid, sized by what the grid needs (it wraps to the editor's width)
                height: ed.sel === index ? rowCol.implicitHeight + 8 : (rowPicks.visible ? 72 : 54)
                clip: true
                color: ed.sel === index ? Theme.bg4 : index % 2 ? Theme.bg2 : Theme.bg3
                Behavior on height { NumberAnimation { duration: 100 } }
                Rectangle { width: 3; height: parent.height; color: Theme.typeColor(rowItem.type) }
                ColumnLayout {
                    id: rowCol
                    anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; anchors.leftMargin: 10; anchors.rightMargin: 8; spacing: 4
                    RowLayout {
                        Layout.fillWidth: true; Layout.preferredHeight: 36; spacing: 8
                        Item { Layout.preferredWidth: 20; Layout.preferredHeight: 30
                            Item { id: slotGhost; width: 20; height: 30; property string dragItemId: rowItem.id || ""; property string dragSlotId: ed.slotId; Drag.active: slotHandle.drag.active; Drag.source: slotGhost; Drag.keys: ["playplay-slot"]; Drag.hotSpot.x: 10; Drag.hotSpot.y: 15
                                Text { anchors.centerIn: parent; text: "⋮⋮"; color: Theme.text2 }
                            }
                            MouseArea { id: slotHandle; anchors.fill: parent; drag.target: slotGhost; cursorShape: Qt.SizeAllCursor; onReleased: { slotGhost.Drag.drop(); slotGhost.x = 0; slotGhost.y = 0 } }
                        }
                        Text { text: index + 1; color: Theme.text3; font.family: Theme.fontMono; font.pixelSize: Theme.fs10; Layout.preferredWidth: 18 }
                        Text { text: rowItem.timingMode === "hard_start" ? "◆" : rowItem.timingMode === "fill_until" ? "→" : "•"; color: rowItem.timingMode === "hard_start" ? Theme.standby : Theme.text2; font.pixelSize: Theme.fs11 }
                        Text { text: ed.isBlock(rowItem) ? ed.blockLabel(rowItem) : (rowItem.title || rowItem.path || "(untitled)"); color: ed.isBlock(rowItem) ? Theme.accent0 : Theme.text0; font.pixelSize: Theme.fs12; font.italic: ed.isBlock(rowItem); elide: Text.ElideRight; Layout.fillWidth: true }
                        CBadge { visible: ed.rev >= 0 && !!rowItem.isLocked; text: ed.rev < 0 ? "" : "\uD83D\uDD12" + (rowItem.lockAtSec >= 0 ? " :" + Theme.pad2(Math.floor(rowItem.lockAtSec / 60)) : "") + (rowItem.lockLabel ? " " + rowItem.lockLabel : ""); tint: Theme.standby }
                        CBadge { text: ed.isBlock(rowItem) ? "block" : (rowItem.type || "video"); tint: ed.isBlock(rowItem) ? Theme.accent0 : Theme.typeColor(rowItem.type) }
                        Text { text: ed.rev < 0 ? "" : Theme.fmtClock(ed.itemDur(rowItem)); color: Theme.text1; font.family: Theme.fontMono; font.pixelSize: Theme.fs11; Layout.preferredWidth: 50; horizontalAlignment: Text.AlignRight }
                        CButton { icon2: "▲"; small: true; variant: "ghost"; onClicked: { ed.move(index, -1) } }
                        CButton { icon2: "▼"; small: true; variant: "ghost"; onClicked: { ed.move(index, 1) } }
                        CButton { icon2: "✕"; small: true; variant: "ghost"; onClicked: { ed.remove(index) } }
                    }
                    Text { Layout.fillWidth: true; Layout.leftMargin: 26; Layout.topMargin: -6; elide: Text.ElideRight; color: Theme.text3; font.pixelSize: Theme.fs10
                        text: { var m = ed.mediaInfo[rowItem.mediaId]; return "Est. " + ed.estimatedStart(index) + "  · Added " + (m && m.addedAtMs ? Qt.formatDate(new Date(m.addedAtMs), "yyyy-MM-dd") : "—") + "  · " + (m ? (m.playCount || 0) : "—") + " plays" }
                    }
                    // the files this block resolved to in the last preview (Resolve, a laid recipe or Fill hour)
                    Text {
                        id: rowPicks
                        readonly property string line: ed.isBlock(rowItem) ? ed.picksLine(rowItem) : ""
                        visible: line.length > 0
                        Layout.fillWidth: true; Layout.leftMargin: 26; Layout.topMargin: -12
                        text: line; elide: Text.ElideRight
                        color: line.indexOf("nothing") >= 0 ? Theme.danger : Theme.text2; font.pixelSize: Theme.fs10
                    }
                    // property grid: a Flow, so it wraps to the width the editor really has (462 px at the 1280 px minimum window)
                    Flow {
                        id: detail
                        visible: ed.sel === index
                        Layout.fillWidth: true; spacing: 8
                        CSelect { label: "Timing"; width: 120; model: ["soft_start", "hard_start", "fill_until"]; currentIndex: Math.max(0, model.indexOf(rowItem.timingMode || "soft_start"))
                            // a soft item that keeps an offset is turned back into a hard one by the server, so going soft drops the offset in the same change
                            onActivated: function(i) { ed.patch(index, { timingMode: model[i], fixedOffsetSec: model[i] === "soft_start" ? undefined : rowItem.fixedOffsetSec }) } }
                        CField { id: atF; label: "At :mm:ss"; width: 80; mono: true; text: rowItem.fixedOffsetSec >= 0 ? Theme.fmtClock(rowItem.fixedOffsetSec) : ""; placeholder: "mm:ss"
                            onEdited: function(v) { if (!v.trim().length) { ed.typed(index, { fixedOffsetSec: undefined }); return } var s = ed.parseMmSs(v); if (!isNaN(s)) ed.typed(index, { fixedOffsetSec: Math.max(0, s) }) } }
                        CField { label: "In (s)"; width: 70; mono: true; text: String(rowItem.timeIn || 0); onEdited: function(v) { ed.typed(index, { timeIn: parseFloat(v) || 0 }) } }
                        CField { label: "Out (s)"; width: 70; mono: true; text: rowItem.timeOut ? String(rowItem.timeOut) : ""; placeholder: "end"; onEdited: function(v) { ed.typed(index, { timeOut: parseFloat(v) || 0 }) } }
                        CSelect { label: "Transition"; width: 110; model: ["cut", "mix", "dip", "wipe_left", "wipe_right", "push_left", "push_up", "squeeze"]; currentIndex: Math.max(0, model.indexOf(rowItem.transitionType || "cut"))
                            onActivated: function(i) { var f = { transitionType: model[i] }; if (model[i] !== "cut" && !(rowItem.transitionSec > 0)) f.transitionSec = 0.5; ed.patch(index, f) } }
                        CField { label: "CG on start"; width: 150; text: rowItem.cgOnStart ? rowItem.cgOnStart.join(", ") : ""; placeholder: "lower_third, bug …"; onEdited: function(v) { ed.typed(index, { cgOnStart: v.split(",").map(function(s) { return s.trim() }).filter(function(s) { return s.length }) }) } }
                        Item { width: 90; height: atF.height
                            CToggle { id: lockT; anchors.bottom: parent.bottom; width: parent.width; label: "Locked"; onToggled: function(v) { ed.setLock(index, v) } }
                            // CToggle flips itself on a click (which kills a `checked:` binding) and a typed lock label can lock the item: a Binding keeps it true to the row
                            Binding { target: lockT; property: "checked"; value: ed.rev >= 0 && !!rowItem.isLocked }
                        }
                        CField { label: "Lock at :mm"; width: 80; mono: true; text: rowItem.lockAtSec >= 0 ? Theme.fmtClock(rowItem.lockAtSec) : ""; placeholder: "mm:ss"
                            onEdited: function(v) { if (!v.trim().length) { ed.typed(index, { lockAtSec: undefined }); return } var s = ed.parseMmSs(v); if (!isNaN(s)) ed.typed(index, { lockAtSec: Math.max(0, Math.min(3600, s)), isLocked: true }) } }
                        CField { label: "Lock label"; width: 120; text: rowItem.lockLabel || ""; onEdited: function(v) { ed.typed(index, v.length ? { lockLabel: v, isLocked: true } : { lockLabel: undefined }) } }
                        Item { visible: ed.isBlock(rowItem); width: editBlockBtn.implicitWidth; height: atF.height
                            CButton { id: editBlockBtn; anchors.bottom: parent.bottom; anchors.bottomMargin: 3; text: "Edit block"; small: true; onClicked: { ed.editBlock(index) } }
                        }
                    }
                }
                DropArea { id: slotDrop; anchors.fill: parent; keys: ["playplay-media", "playplay-slot"]; property bool before: drag.y < height / 2
                    Rectangle { visible: slotDrop.containsDrag; y: slotDrop.before ? 0 : parent.height - 3; width: parent.width; height: 3; color: Theme.accent0 }
                    onDropped: function(drop) { var src = drop.source, at = index + (drop.y < height / 2 ? 0 : 1); if (src && src.dragMedia) ed.addMedia(src.dragMedia, at); else if (src && src.dragItemId && src.dragSlotId === ed.slotId) ed.moveTo(src.dragItemId, at) }
                }
                MouseArea { anchors.fill: parent; z: -1; acceptedButtons: Qt.LeftButton | Qt.RightButton
                    onClicked: function(m) { if (m.button === Qt.RightButton) { rowMenu.popup() } else ed.sel = ed.sel === index ? -1 : index } }
                Menu {
                    id: rowMenu
                    MenuItem { text: "Move up"; enabled: index > 0; onTriggered: { ed.move(index, -1) } }
                    MenuItem { text: "Move down"; enabled: index < ed.items.length - 1; onTriggered: { ed.move(index, 1) } }
                    MenuItem { text: "Duplicate"; onTriggered: { var a = ed.items.slice(); var c = JSON.parse(JSON.stringify(a[index])); c.id = "w" + Date.now().toString(36); a.splice(index + 1, 0, c); ed.items = a; ed.dirty = true } }
                    MenuSeparator {}
                    MenuItem { text: "Cut\tCtrl+X"; onTriggered: { ed.cutItems([index]) } }
                    MenuItem { text: "Copy\tCtrl+C"; onTriggered: { ed.copyItems([index]) } }
                    MenuItem { text: "Paste above"; enabled: !!(window.clip && window.clip.kind === "items"); onTriggered: { ed.pasteItems(index) } }
                    MenuItem { text: "Paste below\tCtrl+V"; enabled: !!(window.clip && window.clip.kind === "items"); onTriggered: { ed.pasteItems(index + 1) } }
                    MenuItem { text: "Copy title"; onTriggered: { window.copyText(rowItem.title || "") } }
                    MenuItem { text: "Copy path"; enabled: !!rowItem.path; onTriggered: { window.copyText(rowItem.path) } }
                    MenuItem { text: "Copy media id"; enabled: !!rowItem.mediaId; onTriggered: { window.copyText(rowItem.mediaId) } }
                    MenuSeparator {}
                    MenuItem { text: "Properties"; onTriggered: { ed.sel = index } }
                    MenuItem { text: ed.rev >= 0 && rowItem.isLocked ? "Unlock" : "Lock (keep through fills)"; onTriggered: { ed.setLock(index, !rowItem.isLocked) } }
                    MenuItem { text: "Lock at this position (:" + Theme.pad2(Math.floor(ed.offsetOf(index) / 60)) + ")"; onTriggered: { ed.setLock(index, true, Math.round(ed.offsetOf(index))) } }
                    // one atomic change each: the second of two setField() calls used to run after the delegate was gone, so the offset was never written / cleared
                    MenuItem { text: "Hard start at this position"; onTriggered: { ed.patch(index, { timingMode: "hard_start", fixedOffsetSec: Math.round(ed.offsetOf(index)) }) } }
                    MenuItem { text: "Soft start"; onTriggered: { ed.patch(index, { timingMode: "soft_start", fixedOffsetSec: undefined }) } }
                    MenuItem { text: "Fill until next"; onTriggered: { ed.setField(index, "timingMode", "fill_until") } }
                    MenuSeparator {}
                    MenuItem { text: ed.isBlock(rowItem) ? "Edit block\u2026" : "Open media"; enabled: ed.isBlock(rowItem) || !!rowItem.mediaId; onTriggered: { if (ed.isBlock(rowItem)) ed.editBlock(index); else window.openMedia(rowItem.mediaId, "meta") } }
                    MenuItem { text: "Asset report"; enabled: !!rowItem.mediaId; onTriggered: { Qt.openUrlExternally(api.rawUrl("/reports/asset/" + rowItem.mediaId + ".html")) } }
                    MenuSeparator {}
                    MenuItem { text: "Remove"; onTriggered: { ed.remove(index) } }
                }
            }
            Text { anchors.centerIn: parent; visible: ed.items.length === 0; text: ed.slotId.length ? "Empty hour — add media from the library (+), a block (\u26A1) or Fill hour" : "Select an hour in the weekly grid"; color: Theme.text3; font.pixelSize: Theme.fs11 }
        }
        // resolved preview
        Rectangle {
            visible: ed.resolved !== null
            Layout.fillWidth: true; implicitHeight: 150; color: Theme.bg2
            Rectangle { width: parent.width; height: 1; color: Theme.border0 }
            ColumnLayout {
                anchors.fill: parent; anchors.margins: 6; spacing: 2
                RowLayout { Layout.fillWidth: true
                    Text { text: "RESOLVED NOW"; color: Theme.text2; font.pixelSize: Theme.fs10; font.bold: true }
                    Text { text: ed.resolved ? Theme.fmtClock(ed.resolved.scheduledSec || 0) + " sched · " + Theme.fmtClock(ed.resolved.filledSec || 0) + " fill · " + Theme.fmtClock(ed.resolved.gapSec || 0) + " gap" : ""; color: Theme.text3; font.family: Theme.fontMono; font.pixelSize: Theme.fs10 }
                    Item { Layout.fillWidth: true }
                    CButton { icon2: "\u2715"; small: true; variant: "ghost"; onClicked: { ed.resolved = null } } }
                ListView { Layout.fillWidth: true; Layout.fillHeight: true; clip: true; model: ed.resolved ? (ed.resolved.items || []) : []
                    delegate: RowLayout { width: ListView.view.width; spacing: 8
                        Text { text: Theme.fmtTime(modelData.plannedStartMs, true); color: Theme.text1; font.family: Theme.fontMono; font.pixelSize: Theme.fs10; Layout.preferredWidth: 70 }
                        Text { text: (modelData.hard ? "\u25C6 " : "") + (modelData.title || ""); color: Theme.text0; font.pixelSize: Theme.fs11; elide: Text.ElideRight; Layout.fillWidth: true }
                        Text { visible: !!modelData.blockLabel; text: "\u2190 " + (modelData.blockLabel || ""); color: Theme.accent0; font.pixelSize: Theme.fs10; elide: Text.ElideRight; Layout.preferredWidth: 200 }
                        Text { text: modelData.origin || ""; color: Theme.text3; font.pixelSize: Theme.fs10; Layout.preferredWidth: 60 }
                        Text { text: Theme.fmtClock(modelData.duration); color: Theme.text1; font.family: Theme.fontMono; font.pixelSize: Theme.fs10; Layout.preferredWidth: 46 } } }
            }
        }
        // issues
        Rectangle {
            visible: ed.issues.length > 0
            Layout.fillWidth: true; implicitHeight: Math.min(96, 8 + ed.issues.length * 18); color: Qt.rgba(1, 0.27, 0.23, 0.08)
            Rectangle { width: parent.width; height: 1; color: Theme.border0 }
            ListView { anchors.fill: parent; anchors.margins: 4; clip: true; model: ed.issues
                delegate: Text { text: "⚠ " + (modelData.code || "") + "  " + (modelData.message || modelData.detail || JSON.stringify(modelData)); color: modelData.severity === "error" ? Theme.danger : Theme.warn; font.pixelSize: Theme.fs11; elide: Text.ElideRight; width: ListView.view.width } }
        }
        // actions
        Rectangle {
            // a Flow: the Save buttons used to sit at the far end of a 1000+ px row and were clipped off the editor at the 1280 px window
            Layout.fillWidth: true; implicitHeight: actionsFlow.implicitHeight + 14; color: Theme.bg2
            Rectangle { width: parent.width; height: 1; color: Theme.border0 }
            Flow {
                id: actionsFlow
                anchors.left: parent.left; anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; anchors.leftMargin: 10; anchors.rightMargin: 10; spacing: 8
                CButton { text: "Validate"; icon2: "✓"; onClicked: { ed.validate() } }
                CButton { text: "Delete"; icon2: "✕"; variant: "ghost"; enabled: ed.slot !== null; onClicked: { ed.deleteSlot() } }
                Rectangle { width: 1; height: 30; color: Theme.border1 }
                CField { id: recipeName; width: 120; placeholder: "recipe name" }
                CButton { height: 30; text: "Save recipe"; small: true; onClicked: { ed.saveRecipe(recipeName.text) } ToolTip.text: "Store this hour's layout as a reusable recipe" }
                CSelect { id: recipeSel; width: 140; model: ed.recipeNames }
                CButton { height: 30; text: "Apply"; small: true; enabled: ed.recipeNames.length > 0; onClicked: { ed.applyRecipe(recipeSel.currentText) } }
                CButton { height: 30; icon2: "\u2715"; small: true; variant: "ghost"; enabled: ed.recipeNames.length > 0; onClicked: { ed.deleteRecipe(recipeSel.currentText) } ToolTip.text: "Delete recipe" }
                Text { visible: ed.dirty; height: 30; verticalAlignment: Text.AlignVCenter; text: "unsaved"; color: Theme.standby; font.pixelSize: Theme.fs10; font.bold: true }
                CButton { text: "Save template (" + ["Sun","Mon","Tue","Wed","Thu","Fri","Sat"][ed.date.getDay()] + " " + Theme.pad2(ed.hour) + ":00)"; onClicked: { ed.save(false) } }
                CButton { text: "Save for " + Qt.formatDate(ed.date, "MMM d"); variant: "primary"; onClicked: { ed.save(true) } }
            }
        }
    }
}
