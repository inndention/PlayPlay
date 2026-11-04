import QtQuick
import ".."

/**
 * InputMap: the operator's switcher bus. Eight slots (buttons 1..8) that can each be mapped to a live input, a PVW selection and the
 * catalogue of everything that can be mapped. One instance lives in main.qml (`inputMap`) so the buttons, the monitor tiles and the
 * keyboard shortcuts act on the same state.
 *
 *   slots     [8] of null | { kind, title, path, mediaId, enginePath, ptz }   (saved as Supervisor setting `ui.inputMap`; ptz: "auto"
 *             (default) | "none" | "ptz1".., reset to AUTO when the key is mapped to another source)
 *   cgSlots   CG1, CG2 ... from the Supervisor, each assigned to all inputs or some (shown when the input goes on air)
 *   ptzSlots  PTZ1, PTZ2 ... bound to ONVIF cameras (setting `ui.ptzMap`)
 *   catalog   library live entries (type "live"), NDI sources found on the network and ONVIF cameras not yet in the library
 *   take(i)   POST /api/v1/channels/{active}/transport {action:"take", path:"live://..."}: cuts immediately, the schedule queues behind
 *             it, and Skip returns to the schedule (see resolveTake in supervisorcore.cpp)
 */
QtObject {
    id: im

    readonly property int count: 8
    property var slots: [null, null, null, null, null, null, null, null]
    property int pvw: -1                      // slot on the preview bus, -1 = none
    property var catalog: []
    property var cameras: []                  // ONVIF cameras (PTZ panel)
    property bool mapLoaded: false
    property var _lib: null
    property var _inputs: null
    // AUTO transition (CUT is always a hard cut). Saved with the map. types: the program-bus transitions the engine runs.
    readonly property var transTypes: ["mix", "dip", "wipe_left", "push_left", "squeeze"]
    readonly property var transDurations: [0.25, 0.5, 1.0, 2.0]
    property string transType: "mix"
    property real transSec: 0.5
    // Input monitors: the engine of the controlled channel receives every mapped input at low bandwidth (the PVW one at full
    // quality, warm for an instant take). Leased while the On Air page is open; the Supervisor files pictures as "<ch>:in<N>".
    property var monitorSkipped: ({})          // key -> why the input shows as a card
    property string _leaseCh: ""

    // ---- sources -----------------------------------------------------------------------------------------------------------------------------
    /// What the engine opens for a live source spec: same order as resolveTake() in supervisorcore.cpp.
    function enginePathOf(src) {
        if (!src) return ""
        if (src.url) return String(src.url)
        if (src.ndiName) return "ndi://" + src.ndiName
        if (src.name) return "ndi://" + src.name
        if (src.port > 0) return "sdi://" + src.port
        return ""
    }
    function kindOf(src, path) {
        if (String(path).indexOf("live://camera/") === 0) return "camera"
        var t = src && src.type ? String(src.type) : ""
        if (t === "ndi") return "ndi"
        if (t === "sdi_in" || t === "sdi") return "sdi"
        return "stream"
    }
    function rebuild() {
        var out = [], seen = {}, lib = im._lib || [], inp = im._inputs || ({})
        for (var i = 0; i < lib.length; ++i) {
            var m = lib[i], path = m.path || m.filePath || "", ep = enginePathOf(m.liveSource)
            if (ep) seen[ep.toLowerCase()] = true
            out.push({ kind: kindOf(m.liveSource, path), title: m.title || path, path: path, mediaId: m.id, enginePath: ep, group: "Library", sub: ep })
        }
        var ndi = inp.ndi || []
        for (var n = 0; n < ndi.length; ++n) {
            var np = "ndi://" + ndi[n].name
            if (seen[np.toLowerCase()]) continue
            out.push({ kind: "ndi", title: ndi[n].name, path: "live://ndi/" + ndi[n].name, mediaId: "", enginePath: np, group: "NDI on the network", sub: ndi[n].url || "" })
        }
        var cams = inp.cameras || []
        for (var c = 0; c < cams.length; ++c) {
            var uri = cams[c].streamUri
            if (!uri || seen[String(uri).toLowerCase()]) continue
            out.push({ kind: "camera", title: cams[c].name || cams[c].host, path: "live://stream/" + uri, mediaId: "", enginePath: uri, group: "Cameras", sub: cams[c].host || "" })
        }
        im.catalog = out
        im.cameras = inp.cameras || []
    }
    function refresh() {
        api.get("/media?type=live", function(ok, d) { if (ok && d) { im._lib = d.media || []; im.rebuild() } })
        api.get("/inputs", function(ok, d) { if (ok && d) { im._inputs = d; im.rebuild() } })
    }

    // ---- the saved map ---------------------------------------------------------------------------------------------------------------------------
    function load() {
        api.get("/settings/ui.inputMap", function(ok, d) {
            try {
                var o = ok && d && d.value ? JSON.parse(d.value) : null
                if (o && o.slots) { var a = []; for (var i = 0; i < im.count; ++i) a.push(o.slots[i] || null); im.slots = a }
                if (o && o.transition) {
                    if (im.transTypes.indexOf(o.transition.type) >= 0) im.transType = o.transition.type
                    if (o.transition.durationSec > 0) im.transSec = o.transition.durationSec
                }
            } catch (e) { console.warn("[inputMap] saved map unreadable: " + e) }
            im.mapLoaded = true
        })
    }
    function save() {
        api.put("/settings/ui.inputMap", { value: { slots: im.slots, transition: { type: im.transType, durationSec: im.transSec } } }, function(ok) { if (!ok) window.toast("Could not save the input map", "error") })
    }
    function map(i, entry) {
        var a = im.slots.slice()
        a[i] = { kind: entry.kind, title: entry.title, path: entry.path, mediaId: entry.mediaId || "", enginePath: entry.enginePath || "", sub: entry.sub || "" }
        im.slots = a
        save()
        window.toast("Input " + (i + 1) + " → " + entry.title, "ok")
    }
    function clear(i) {
        var a = im.slots.slice(); a[i] = null; im.slots = a
        if (im.pvw === i) im.pvw = -1
        save()
    }

    // ---- bus actions -------------------------------------------------------------------------------------------------------------------------
    function norm(p) { return String(p || "").toLowerCase().replace(/\/+$/, "") }
    /// Slot whose input is what `enginePath` (the channel's current source path) is playing, -1 when none.
    function indexOnAir(enginePath) {
        var n = norm(enginePath)
        if (!n) return -1
        for (var i = 0; i < im.slots.length; ++i) if (im.slots[i] && norm(im.slots[i].enginePath) === n) return i
        return -1
    }
    function select(i) {
        if (i < 0 || i >= im.count) return
        if (!im.slots[i]) { window.toast("Input " + (i + 1) + " is not mapped: right-click it to map an input", "info"); return }
        im.pvw = im.pvw === i ? -1 : i
    }
    /// how: "cut" (default: hot cut, double-click, Alt+N, CUT) or "auto" (the selected transition). The engine runs the transition on
    /// its A/B program bus; when the input is the warm PVW monitor, the receiver is handed over and the take starts on the next frame.
    function take(i, channelId, how) {
        var e = im.slots[i]
        if (!e) { window.toast("Input " + (i + 1) + " is not mapped", "info"); return }
        var ch = channelId || supervisor.activeChannelId
        if (!ch) return
        var body = { action: "take", path: e.path, title: e.title, duration: 1800 }
        if (e.mediaId) body.mediaId = e.mediaId
        if (!mixer.afv) body.audioMute = true            // audio follows video OFF: the taken input is silent for this item
        var auto = how === "auto" && im.transType !== "cut"
        if (auto) body.transition = { type: im.transType, durationSec: im.transSec }
        api.post("/channels/" + ch + "/transport", body, function(ok, d) {
            if (ok) { window.toast((auto ? "AUTO " + im.transLabel(im.transType) + "  " : "CUT  ") + e.title + "  →  " + ch.toUpperCase() + " PGM", "ok"); im.pvw = -1 }
        })
    }
    function cut(channelId) { if (im.pvw >= 0) im.take(im.pvw, channelId, "cut"); else window.toast("Nothing on preview: click an input first", "info") }
    function autoTake(channelId) { if (im.pvw >= 0) im.take(im.pvw, channelId, "auto"); else window.toast("Nothing on preview: click an input first", "info") }
    function transLabel(t) { return t === "mix" ? "MIX" : t === "dip" ? "DIP" : t === "wipe_left" ? "WIPE" : t === "push_left" ? "PUSH" : t === "squeeze" ? "SQZ" : "CUT" }
    function setTransition(type, sec) {
        if (type !== undefined && im.transTypes.indexOf(type) >= 0) im.transType = type
        if (sec !== undefined && sec > 0) im.transSec = sec
        save()
    }

    // ---- CG slots on inputs (Supervisor /overlays/cgslots; an assigned CG shows when its input goes on air) ------------------------------------
    property var cgSlots: []                   // [{id, name, template, params, inputs:["*"] | ["in1",...], ...}]
    function loadCg() { api.get("/overlays/cgslots", function(ok, d) { if (ok && d) im.cgSlots = d.slots || [] }) }
    function cgCovers(s, i) { var a = s && s.inputs ? s.inputs : []; return a.indexOf("*") >= 0 || a.indexOf("in" + (i + 1)) >= 0 }
    /// ids of the CGs input i shows when it is taken
    function cgOf(i) { var out = []; for (var k = 0; k < im.cgSlots.length; ++k) if (im.cgCovers(im.cgSlots[k], i)) out.push(im.cgSlots[k].id); return out }
    function cgName(id) { for (var k = 0; k < im.cgSlots.length; ++k) if (im.cgSlots[k].id === id) return im.cgSlots[k].name || id.toUpperCase(); return id.toUpperCase() }
    /// Give input i exactly these CGs (the Supervisor turns an "all inputs" CG into the list of the other inputs when one leaves it).
    function setInputCg(i, ids) {
        api.post("/overlays/cgslots/assign", { input: "in" + (i + 1), cg: ids }, function(ok, d) {
            if (!ok) { window.toast("Could not assign the CG: " + (d && d.error ? d.error : "failed"), "error"); return }
            im.cgSlots = d.slots || []
            window.toast("Input " + (i + 1) + (ids.length ? " → " + ids.map(im.cgName).join(" + ") : ": no CG"), "ok")
        })
    }
    /// Save the whole CG list (store / clear / timing / inputs edited in the Graphics deck).
    function putCg(list, msg) {
        api.put("/overlays/cgslots", { slots: list }, function(ok, d) {
            if (!ok) { window.toast("Could not save the CGs: " + (d && d.error ? d.error : "failed"), "error"); return }
            im.cgSlots = d.slots || []
            if (msg) window.toast(msg, "ok")
        })
    }
    function toggleInputCg(i, id) { var a = im.cgOf(i), k = a.indexOf(id); if (k >= 0) a.splice(k, 1); else a.push(id); im.setInputCg(i, a) }
    property Connections _cgEv: Connections {
        target: supervisor
        function onUiEvent(type, payload) {
            if (type === "overlayConfig" && payload && payload.kind === "cgSlots") im.loadCg()
            if (type === "showTemplate") { im.slots = Array(im.count).fill(null); im.pvw = -1; im.transType = "mix"; im.transSec = 0.5; im.load(); im.loadPtz(); im.loadCg() }
        }
    }

    // ---- PTZ slots: PTZ1, PTZ2 ... bind to ONVIF cameras (setting ui.ptzMap); every input is PTZn, AUTO or none (slot field `ptz`) --------------
    // A camera's picture can arrive as NDI, SDI, RTSP... while its control is ONVIF, so the input does not always say which camera it is.
    // AUTO finds it from the input: a library camera (live://camera/<id>), the camera's own stream URI, or the camera's address inside the
    // NDI name / URL / title ("BIRDDOG (NDIHX2-192.168.1.64)"). SDI inputs need PTZn.
    property var ptzSlots: [{ id: "ptz1", name: "PTZ1", cameraId: "" }, { id: "ptz2", name: "PTZ2", cameraId: "" }, { id: "ptz3", name: "PTZ3", cameraId: "" }, { id: "ptz4", name: "PTZ4", cameraId: "" }]
    function loadPtz() {
        api.get("/settings/ui.ptzMap", function(ok, d) {
            try { var o = ok && d && d.value ? JSON.parse(d.value) : null; if (o && o.slots && o.slots.length) im.ptzSlots = o.slots } catch (e) { console.warn("[inputMap] ui.ptzMap unreadable: " + e) }
        })
    }
    function savePtz() { api.put("/settings/ui.ptzMap", { value: { slots: im.ptzSlots } }, function(ok) { if (!ok) window.toast("Could not save the PTZ map", "error") }) }
    function ptzSlot(id) { for (var k = 0; k < im.ptzSlots.length; ++k) if (im.ptzSlots[k].id === id) return im.ptzSlots[k]; return null }
    function camById(id) { var c = im.cameras || []; for (var k = 0; k < c.length; ++k) if (c[k].id === id) return c[k]; return null }
    function camLabel(c) { return c ? (c.name || c.host || c.id) : "" }
    function bindPtz(id, cameraId) {
        var a = im.ptzSlots.slice()
        for (var k = 0; k < a.length; ++k) if (a[k].id === id) a[k] = Object.assign({}, a[k], { cameraId: cameraId || "" })
        im.ptzSlots = a; im.savePtz()
        var c = im.camById(cameraId)
        window.toast((im.ptzSlot(id) || {}).name + (c ? " → " + im.camLabel(c) : ": unbound"), "ok")
    }
    function addPtz() {
        var n = im.ptzSlots.length + 1
        while (im.ptzSlot("ptz" + n)) ++n
        im.ptzSlots = im.ptzSlots.concat([{ id: "ptz" + n, name: "PTZ" + n, cameraId: "" }]); im.savePtz()
        return "ptz" + n
    }
    function removePtz(id) {
        im.ptzSlots = im.ptzSlots.filter(function(s) { return s.id !== id }); im.savePtz()
        var a = im.slots.slice(), ch = false
        for (var i = 0; i < a.length; ++i) if (a[i] && a[i].ptz === id) { a[i] = Object.assign({}, a[i], { ptz: "auto" }); ch = true }
        if (ch) { im.slots = a; im.save() }
    }
    function ptzSlotOfCam(cameraId) { for (var k = 0; k < im.ptzSlots.length; ++k) if (cameraId && im.ptzSlots[k].cameraId === cameraId) return im.ptzSlots[k]; return null }
    /// v: "auto" | "none" | "ptz1"...
    function setInputPtz(i, v) {
        if (!im.slots[i]) return
        var a = im.slots.slice(); a[i] = Object.assign({}, a[i], { ptz: v }); im.slots = a; im.save()
        var r = im.ptzFor(i)
        window.toast("Input " + (i + 1) + " PTZ: " + (v === "auto" ? "AUTO" + (r.cam ? " → " + im.camLabel(r.cam) : " (no camera found yet)") : v === "none" ? "none" : (im.ptzSlot(v) || {}).name), "ok")
    }
    function _hostIn(hay, host) {
        var h = String(host || "").toLowerCase()
        if (h.length < 3) return false
        for (var i = hay.indexOf(h); i >= 0; i = hay.indexOf(h, i + 1)) {
            var b = i > 0 ? hay.charAt(i - 1) : " ", a = hay.charAt(i + h.length)
            if (!/[0-9a-z.]/.test(b) && !/[0-9a-z]/.test(a)) return true       // 192.168.1.6 is not 192.168.1.64
        }
        return false
    }
    /// AUTO: which ONVIF camera an input shows. -> {cam, why} or null
    function autoCam(e) {
        if (!e) return null
        var cams = im.cameras || []
        var m = /^live:\/\/camera\/(.+)$/.exec(String(e.path || ""))
        if (m) { var c0 = im.camById(m[1]); if (c0) return { cam: c0, why: "library camera" } }
        var ep = im.norm(e.enginePath)
        for (var k = 0; k < cams.length; ++k) if (ep && cams[k].streamUri && im.norm(cams[k].streamUri) === ep) return { cam: cams[k], why: "camera stream" }
        var hay = [e.enginePath, e.path, e.title, e.sub].join(" ").toLowerCase()
        for (var j = 0; j < cams.length; ++j) if (cams[j].host && im._hostIn(hay, cams[j].host)) return { cam: cams[j], why: "address " + cams[j].host }
        return null
    }
    /// The camera input i controls: {mode: "auto"|"none"|"ptz1"..., cam, slot (PTZ slot or null), label}
    function ptzFor(i) {
        var e = i >= 0 ? im.slots[i] : null
        if (!e) return { mode: "none", cam: null, slot: null, label: "" }
        var v = e.ptz || "auto"
        if (v === "none") return { mode: v, cam: null, slot: null, label: "no PTZ" }
        if (v !== "auto") {
            var s = im.ptzSlot(v), c = s ? im.camById(s.cameraId) : null
            return { mode: v, cam: c, slot: s, label: s ? s.name + (c ? "" : " (unbound)") : v.toUpperCase() + " (removed)" }
        }
        var r = im.autoCam(e)
        var ps = r ? im.ptzSlotOfCam(r.cam.id) : null
        // label: "AUTO·PTZ2" when the detected camera is bound to a PTZ slot, else "PTZ AUTO" (controllable, not on a slot)
        return { mode: "auto", cam: r ? r.cam : null, slot: ps, why: r ? r.why : "", label: r ? (ps ? "AUTO·" + ps.name : "PTZ AUTO") : "" }
    }

    // ---- input monitors (live pictures of the bus) -------------------------------------------------------------------------------------------
    function monitorKey(i) { return supervisor.activeChannelId ? supervisor.activeChannelId + ":in" + (i + 1) : "" }
    function _engineUp(ch) { var c = ch ? supervisor.channelOf(ch) : null; return !!(c && c.process && c.process.connected) }
    /// Send the lease for the controlled channel (and release the channel controlled before). Called on a timer and on every change.
    function lease() {
        var ch = supervisor.activeChannelId
        var want = window.currentPage === "onair" && im.mapLoaded && im._engineUp(ch)
        if (im._leaseCh && (im._leaseCh !== ch || !want) && im._engineUp(im._leaseCh)) api.post("/channels/" + im._leaseCh + "/monitor", { inputs: [] })
        if (!want) { im._leaseCh = ""; return }
        var ins = []
        for (var i = 0; i < im.count; ++i) {
            var e = im.slots[i]
            if (!e) continue
            var body = { key: "in" + (i + 1), path: e.path, title: e.title, quality: im.pvw === i ? "high" : "low" }
            if (e.mediaId) body.mediaId = e.mediaId
            ins.push(body)
        }
        api.post("/channels/" + ch + "/monitor", { inputs: ins, leaseSec: 45 }, function(ok, d) {
            if (!ok || !d) return
            var sk = {}, a = d.skipped || []
            for (var k = 0; k < a.length; ++k) sk[a[k].key] = a[k].why
            im.monitorSkipped = sk
        })
        // The PGM monitor gets the sharper, faster picture profile while this page is up (it lapses on its own).
        api.post("/channels/" + ch + "/cg", { action: "preview", boostSec: 40 })
        im._leaseCh = ch
    }
    property Timer _leaseSoon: Timer { interval: 150; onTriggered: { im.lease() } }
    property Timer _leaseTimer: Timer { interval: 15000; repeat: true; running: true; onTriggered: { im.lease() } }
    onPvwChanged: _leaseSoon.restart()
    onSlotsChanged: _leaseSoon.restart()
    onMapLoadedChanged: _leaseSoon.restart()
    property Connections _sup: Connections {
        target: supervisor
        function onActiveChannelChanged() { im._leaseSoon.restart() }
    }
    property Connections _win: Connections {
        target: window
        function onCurrentPageChanged() { im._leaseSoon.restart() }
    }

    Component.onCompleted: { load(); refresh(); loadCg(); loadPtz() }
    property Timer poll: Timer { interval: 8000; running: window.currentPage === "onair"; repeat: true; onTriggered: { im.refresh() } }
}
