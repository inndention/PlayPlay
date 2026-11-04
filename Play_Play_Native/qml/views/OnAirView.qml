import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qt.labs.settings
import ".."
import "../ui"

/**
 * On Air: an automation system and a video switcher in one screen.
 *
 *   monitor wall   PGM (the controlled channel, red tally) · PVW (the selected input, green tally) · a bank with every channel's program
 *                  output and the eight mapped inputs. Click an input to put it on preview, double-click to take it to PGM.
 *   input bus      keys 1–8, mappable (right-click → Map to…), saved as the Supervisor setting `ui.inputMap`; keys 1–8 select,
 *                  Alt+1–8 take, Ctrl+Enter cuts the preview, Ctrl+Shift+Enter takes it with the AUTO transition (shortcuts in main.qml,
 *                  state in ui/InputMap.qml). Inputs show live pictures from the engine's input monitors; the PVW input is received at
 *                  full quality and kept warm, so its take is immediate.
 *   control deck   what is on air and the countdown, transport keys, then the hour rundown for this hour and the next.
 *   right column   graphics on air (with clear), the graphics deck (Text 1/2/3 + preview), audio and engine health.
 * A take cuts immediately and the schedule queues behind it; NEXT (skip) goes back to the schedule.
 */
Item {
    id: view
    property var st: supervisor.activeState
    property var run: supervisor.activeRunlist
    property var active: supervisor.activeChannelData     // perf-app PA-3: notifies only when this channel's entry changes
    property var cur: run && run.current ? run.current : ({})
    property var exec: active && active.executor ? active.executor : ({})
    property var comp: st && st.compositor ? st.compositor : ({})
    property string ch: supervisor.activeChannelId
    readonly property string engineState: st && st.state ? st.state : "OFFLINE"
    readonly property bool engineUp: engineState !== "OFFLINE" || (active && active.process ? active.process.connected === true : false)
    readonly property bool armed: exec.armed === true
    readonly property bool onTake: String(st.itemId || "").indexOf("take-") === 0
    readonly property int pgmSlot: inputMap.indexOnAir(st.path)
    property var overlayLayers: []
    readonly property bool mixerOpen: window.onAirDrawers.mixer
    readonly property bool ptzOpen: window.onAirDrawers.ptz
    readonly property bool drawersOpen: mixerOpen || ptzOpen
    function toggleDrawer(k) { var d = Object.assign({}, window.onAirDrawers); d[k] = !d[k]; window.onAirDrawers = d }
    // the mix list without levels (PA-3): the MIX tiles below are no longer rebuilt at the meter rate
    readonly property var extraMixes: { var a = supervisor.activeMixes || [], out = []; for (var i = 0; i < a.length; ++i) if (a[i].id !== "main") out.push(a[i]); return out }
    readonly property var monByKey: { var o = {}, a = view.st.monitors || []; for (var i = 0; i < a.length; ++i) o[a[i].key] = a[i]; return o }
    property var livePaths: ({})              // channelId -> the source path that channel is playing (to show an input's picture when it is on air)
    property real tohSec: 0
    property date now: new Date()
    property var deckRef: null
    property var shows: []
    property int showIndex: -1
    function loadShows() { api.get("/shows", function(ok,d) { if (ok) { view.shows = d.items || []; if (view.showIndex < 0) for (var i=0;i<view.shows.length;i++) if(view.shows[i].id===d.active) view.showIndex=i } }) }
    function applyShowLayout(layout) { var l = layout && layout.native ? layout.native : null; if (!l) return
        layoutPrefs.sideWidth = Math.max(300,Math.min(650,l.sideWidth || 440)); layoutPrefs.wallHeight = Math.max(25,Math.min(55,l.wallHeight || 37)); layoutPrefs.drawerHeight = Math.max(180,Math.min(400,l.drawerHeight || 232)); layoutPrefs.sideLeft = !!l.sideLeft
        if (typeof l.order === "string") { layoutPrefs.order = l.order; loadLayoutOrder() }
    }
    function showLayout() { return { native: {sideWidth:layoutPrefs.sideWidth, wallHeight:layoutPrefs.wallHeight, drawerHeight:layoutPrefs.drawerHeight, sideLeft:layoutPrefs.sideLeft, order:layoutPrefs.order} } }
    property var sideOrder: ["reporter", "current", "graphics"]
    Settings {
        id: layoutPrefs
        category: "OnAirLayout"
        property int sideWidth: 440
        property int wallHeight: 37
        property int drawerHeight: 232
        property bool sideLeft: false
        property string order: "reporter,current,graphics"
    }
    function loadLayoutOrder() {
        var a = layoutPrefs.order.split(","), valid = ["reporter", "current", "graphics"]
        if (a.length !== 3 || a.some(function(x, i) { return valid.indexOf(x) < 0 || a.indexOf(x) !== i })) a = valid
        sideOrder = a
    }
    function moveSide(i, delta) {
        var a = sideOrder.slice(), j = i + delta
        if (j < 0 || j >= a.length) return
        var t = a[i]; a[i] = a[j]; a[j] = t
        sideOrder = a; layoutPrefs.order = a.join(",")
    }

    // ---- derived -----------------------------------------------------------------------------------------------------------------------------
    readonly property var cgLayers: {
        var out = [], seen = {}, ov = view.overlayLayers || []
        function detail(p) {
            var parts = [], keys = Object.keys(p || {})
            for (var k = 0; k < keys.length; ++k) {
                var v = p[keys[k]]
                if (typeof v !== "string" || !v.length || /color|logo|position|icon|image|avatar/i.test(keys[k]) || /^text\d$/.test(keys[k])) continue
                parts.push(v)
                if (parts.length === 3) break
            }
            return parts.join("  ·  ")
        }
        for (var i = 0; i < ov.length; ++i) {
            if (ov[i].state === "gone") continue
            out.push({ layer: ov[i].layer, role: ov[i].role, name: ov[i].label || ov[i].name || ov[i].template, detail: detail(ov[i].params), state: ov[i].state, managed: true })
            seen[ov[i].layer] = true
        }
        var el = view.st.layers || []
        for (var j = 0; j < el.length; ++j) {
            if (seen[el[j].id] || el[j].state === "hidden" || el[j].role === "audio") continue      // mixer line-ins are not graphics
            var src = el[j].source || {}
            out.push({ layer: el[j].id, role: el[j].role || "layer", name: src.kind || src.type || "graphic", detail: src.title || src.text || "", state: el[j].state, managed: false })
        }
        out.sort(function(a, b) { return a.layer - b.layer })
        return out
    }
    function liveChannelFor(entry) {
        if (!entry) return ""
        var n = inputMap.norm(entry.enginePath), ids = Object.keys(view.livePaths)
        for (var i = 0; i < ids.length; ++i) if (n && inputMap.norm(view.livePaths[ids[i]]) === n) return ids[i]
        return ""
    }
    function reloadLayers() { if (view.ch) api.get("/overlays/" + view.ch, function(ok, d) { if (ok && d && d.channelId === view.ch) view.overlayLayers = d.layers || [] }) }
    function stateWord(s) { return s === "ON_AIR" ? "ON AIR" : s === "PREROLL" ? "PREROLL" : s === "PAUSED" ? "PAUSED" : s === "HOLD" ? "HOLD" : s === "IDLE" ? "IDLE" : s }
    function hideLayer(e) {
        if (e.managed) api.post("/overlays/" + view.ch + "/" + e.layer + "/hide", {}, function(ok) { if (ok) view.reloadLayers() })
        else supervisor.cg({ action: "hide", layer: e.layer, transition: { type: "fade", durationSec: 0.3 } })
    }
    function clearGraphics() {
        api.post("/overlays/" + view.ch + "/clear", { includeDefaults: true }, function(ok) { view.reloadLayers() })
        supervisor.cg({ action: "clear" })
    }
    function toggleEngine() { api.post("/channels/" + view.ch + (view.engineUp ? "/stop" : "/start"), {}, function(ok) { supervisor.refresh() }) }

    Component.onCompleted: {
        loadShows()
        loadLayoutOrder()
        reloadLayers()
        var chs = supervisor.channels, n = {}
        for (var i = 0; i < chs.length; ++i) n[chs[i].profile.id] = supervisor.telemetryOf(chs[i].profile.id).path || ""
        livePaths = n
    }
    onChChanged: { overlayLayers = []; reloadLayers() }
    Timer { interval: 250; running: true; repeat: true; onTriggered: { view.now = new Date(); view.tohSec = 3600 - (view.now.getMinutes() * 60 + view.now.getSeconds()) } }
    Connections {
        target: supervisor
        function onUiEvent(type, payload) { if (type === "overlays" && payload.channelId === view.ch) view.overlayLayers = payload.layers || []; if (type === "showTemplate") { view.applyShowLayout(payload.layout); view.showIndex = -1; view.loadShows() } }
        function onTelemetryChanged(id) {
            var p = supervisor.telemetryOf(id).path || ""
            if (view.livePaths[id] !== p) { var n = Object.assign({}, view.livePaths); n[id] = p; view.livePaths = n }
        }
    }

    // ---- menus / dialogs ---------------------------------------------------------------------------------------------------------------------
    CMenu { id: ctx }
    Connections {                                   // --test-menu (main.qml): open a menu for a screenshot
        target: window
        function onTestMenuChanged() {
            var t = window.testMenu, m = /^cg(\d+)$/.exec(t)
            if (m) { var s = inputMap.cgSlots[parseInt(m[1]) - 1]; if (s && view.deckRef) view.deckRef.cgMenu(s) }
            else if (parseInt(t) > 0) view.slotMenu(parseInt(t) - 1)
        }
    }
    InputPicker { id: picker }
    Dialog {
        id: showDialog; parent: Overlay.overlay; anchors.centerIn: parent; modal: true; title: "Save show template"; standardButtons: Dialog.Close; width: 460
        ColumnLayout { width: parent.width; spacing: 10
            Text { text: "Save the current input map, camera assignments, CG slots, overlay presets, CG rules and On Air layout. Applying this show replaces the current overlay presets and rules."; color: Theme.text2; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            CField { id: showName; label: "Show name"; placeholder: "Evening News"; Layout.fillWidth: true }
            RowLayout { Layout.fillWidth: true
                CButton { text: "Save new"; variant: "primary"; enabled: showName.text.trim().length > 0; onClicked: view.saveShow(false) }
                CButton { text: "Update selected"; enabled: view.showIndex >= 0 && showName.text.trim().length > 0; onClicked: view.saveShow(true) }
            }
        }
    }
    function saveShow(update) { var b = {name:showName.text.trim(), layout:view.showLayout()}; if (update && shows[showIndex]) b.id = shows[showIndex].id
        api.post("/shows", b, function(ok,d) { if (!ok) { window.toast(d.error || "Could not save show", "error"); return } showDialog.close(); view.showIndex = -1; view.loadShows(); window.toast("Saved show " + d.name, "ok") }) }
    function applyShow() { var s = shows[showIndex]; if (!s) return; api.post("/shows/" + s.id + "/apply", {}, function(ok,d) {
        if (ok) { view.applyShowLayout(d.layout); inputMap.load(); inputMap.loadPtz(); inputMap.loadCg(); window.toast("Loaded show " + d.name, "ok") }
        else window.toast(d.error || "Could not load show", "error") }) }
    Dialog {
        id: exportDialog; parent: Overlay.overlay; anchors.centerIn: parent; modal: true
        title: "Recompose layered recording"; standardButtons: Dialog.Close; width: 630
        property string jobId: ""
        property string exportMessage: ""
        function send() {
            var edits = {omitLayers: hideLayers.text.split(",").map(function(s) { return parseInt(s.trim().replace(/^L/i,"")) }).filter(function(n) { return !isNaN(n) })}
            if (moveLayer.checked) {
                var id = parseInt(layerId.text), x = parseFloat(layerX.text), y = parseFloat(layerY.text), w = parseFloat(layerW.text), h = parseFloat(layerH.text), opacity = parseFloat(layerOpacity.text)
                if (isNaN(id) || isNaN(x) || isNaN(y) || !(w > 0) || !(h > 0) || isNaN(opacity)) { exportMessage = "Enter a layer ID, position, positive size and opacity."; return }
                edits.layers = {}; edits.layers[String(id)] = {rect:[x,y,w,h],opacity:Math.max(0,Math.min(1,opacity))}
            }
            api.post("/recordings/recompose", {session: recordingSession.text, output: recordingOutput.text, edits: edits}, function(ok,d) {
                if (ok) { exportDialog.jobId = d.id; exportDialog.exportMessage = "Exporting…"; exportPoll.start() } else exportDialog.exportMessage = d.error || "Export could not start"
            })
        }
        Timer { id: exportPoll; interval: 1000; repeat: true; onTriggered: api.get("/recordings/jobs/" + exportDialog.jobId, function(ok,d) {
            if (!ok) { exportPoll.stop(); exportDialog.exportMessage = d.error || "Cannot read export status"; return }
            if (d.state !== "running") { exportPoll.stop(); exportDialog.exportMessage = d.state === "complete" ? "Saved " + d.output : d.error || "Export failed" }
        }) }
        ColumnLayout { width: parent.width; spacing: 8
            RowLayout { Layout.fillWidth: true
                CField { id: recordingSession; label: "Recording session folder"; Layout.fillWidth: true }
                CButton { text: "Browse"; small: true; Layout.alignment: Qt.AlignBottom; onClicked: { var path = nativeTools.recordingDirectory(recordingSession.text); if (path) recordingSession.text = path } }
            }
            RowLayout { Layout.fillWidth: true
                CField { id: recordingOutput; label: "New MKV file"; Layout.fillWidth: true }
                CButton { text: "Browse"; small: true; Layout.alignment: Qt.AlignBottom; onClicked: { var path = nativeTools.recordingOutput(recordingOutput.text); if (path) recordingOutput.text = path } }
            }
            CField { id: hideLayers; label: "Hide layer IDs (comma separated, optional)"; placeholder: "30, 40"; Layout.fillWidth: true }
            CheckBox { id: moveLayer; text: "Change a layer's placement and opacity" }
            RowLayout { Layout.fillWidth: true; visible: moveLayer.checked
                CField { id: layerId; label: "Layer"; text: "30"; Layout.fillWidth: true }
                CField { id: layerX; label: "X"; text: "0"; Layout.fillWidth: true }
                CField { id: layerY; label: "Y"; text: "0"; Layout.fillWidth: true }
            }
            RowLayout { Layout.fillWidth: true; visible: moveLayer.checked
                CField { id: layerW; label: "Width"; text: String(view.st.width || 1920); Layout.fillWidth: true }
                CField { id: layerH; label: "Height"; text: String(view.st.height || 1080); Layout.fillWidth: true }
                CField { id: layerOpacity; label: "Opacity 0–1"; text: "1"; Layout.fillWidth: true }
            }
            Text { text: "Records visible compositor layers. An embedded OBS scene is a single layer."; color: Theme.text2; wrapMode: Text.WordWrap; Layout.fillWidth: true; font.pixelSize: Theme.fs11 }
            CButton { text: "Export scene"; variant: "primary"; enabled: !exportPoll.running && recordingSession.text.length > 0 && recordingOutput.text.length > 0; onClicked: exportDialog.send() }
            Text { text: exportDialog.exportMessage; color: Theme.text1; wrapMode: Text.WordWrap; Layout.fillWidth: true; font.pixelSize: Theme.fs11 }
        }
    }
    Dialog {
        id: layoutDialog
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        title: "Arrange On Air layout"
        standardButtons: Dialog.Close
        width: 490
        ColumnLayout {
            width: parent.width; spacing: 10
            Text { Layout.fillWidth: true; text: "Size panels and arrange the graphics column for this workstation."; color: Theme.text2; wrapMode: Text.WordWrap }
            RowLayout { Layout.fillWidth: true
                Text { text: "Graphics column"; color: Theme.text1; Layout.preferredWidth: 145 }
                Slider { Layout.fillWidth: true; from: 300; to: 650; stepSize: 10; value: layoutPrefs.sideWidth; onMoved: layoutPrefs.sideWidth = Math.round(value) }
                Text { text: layoutPrefs.sideWidth + " px"; color: Theme.text1 }
            }
            RowLayout { Layout.fillWidth: true
                Text { text: "Monitor height"; color: Theme.text1; Layout.preferredWidth: 145 }
                Slider { Layout.fillWidth: true; from: 25; to: 55; stepSize: 1; value: layoutPrefs.wallHeight; onMoved: layoutPrefs.wallHeight = Math.round(value) }
                Text { text: layoutPrefs.wallHeight + "%"; color: Theme.text1 }
            }
            RowLayout { Layout.fillWidth: true
                Text { text: "Audio / PTZ height"; color: Theme.text1; Layout.preferredWidth: 145 }
                Slider { Layout.fillWidth: true; from: 180; to: 400; stepSize: 10; value: layoutPrefs.drawerHeight; onMoved: layoutPrefs.drawerHeight = Math.round(value) }
                Text { text: layoutPrefs.drawerHeight + " px"; color: Theme.text1 }
            }
            CheckBox { text: "Put graphics column on the left"; checked: layoutPrefs.sideLeft; onToggled: layoutPrefs.sideLeft = checked }
            Text { text: "GRAPHICS COLUMN ORDER"; color: Theme.text2; font.bold: true; font.pixelSize: Theme.fs10 }
            Repeater {
                model: view.sideOrder
                RowLayout { Layout.fillWidth: true
                    Text { Layout.fillWidth: true; text: modelData === "reporter" ? "Reporter feed" : modelData === "current" ? "Graphics on air" : "Graphics deck"; color: Theme.text1 }
                    CButton { text: "↑"; small: true; enabled: index > 0; onClicked: view.moveSide(index, -1) }
                    CButton { text: "↓"; small: true; enabled: index < view.sideOrder.length - 1; onClicked: view.moveSide(index, 1) }
                }
            }
            CButton { text: "Reset layout"; small: true; onClicked: { layoutPrefs.sideWidth = 440; layoutPrefs.wallHeight = 37; layoutPrefs.drawerHeight = 232; layoutPrefs.sideLeft = false; layoutPrefs.order = "reporter,current,graphics"; view.loadLayoutOrder() } }
        }
    }
    function slotMenu(i) {
        var e = inputMap.slots[i], quick = []
        var cat = inputMap.catalog
        for (var k = 0; k < cat.length && quick.length < 14; ++k) (function(c) { quick.push({ text: c.title + "   (" + c.kind + ")", fn: function() { inputMap.map(i, c) } }) })(cat[k])
        // CG: which stored graphics show when this input goes on air (checked = this input has it; "all inputs" CGs say so)
        var mine = inputMap.cgOf(i), cgMenu = []
        for (var g = 0; g < inputMap.cgSlots.length; ++g) (function(s) {
            var empty = !s.template, all = (s.inputs || []).indexOf("*") >= 0
            cgMenu.push({ text: s.name + "   " + (empty ? "(empty)" : view.cgSummary(s)) + (all ? "   · all inputs" : ""), checked: mine.indexOf(s.id) >= 0, enabled: !empty || mine.indexOf(s.id) >= 0,
                          fn: function() { inputMap.toggleInputCg(i, s.id) } })
        })(inputMap.cgSlots[g])
        cgMenu.push({ sep: true })
        cgMenu.push({ text: "No CG on this input", checked: mine.length === 0, fn: function() { inputMap.setInputCg(i, []) } })
        cgMenu.push({ text: "Store and edit CGs in the Graphics deck", enabled: false })
        // PTZ: which camera the PTZ drawer moves while this input is on preview / program
        var pz = inputMap.ptzFor(i), cur = e && e.ptz ? e.ptz : "auto", ptzMenu = []
        var autoTxt = "AUTO" + (e ? (pz.mode === "auto" && pz.cam ? "   → " + inputMap.camLabel(pz.cam) + "  (" + pz.why + ")" : "   (no camera found for this input)") : "")
        ptzMenu.push({ text: autoTxt, checked: cur === "auto", fn: function() { inputMap.setInputPtz(i, "auto") } })
        for (var p = 0; p < inputMap.ptzSlots.length; ++p) (function(s) {
            var c = inputMap.camById(s.cameraId)
            ptzMenu.push({ text: s.name + "   " + (c ? inputMap.camLabel(c) : "(no camera bound)"), checked: cur === s.id, fn: function() { inputMap.setInputPtz(i, s.id) } })
        })(inputMap.ptzSlots[p])
        ptzMenu.push({ sep: true })
        ptzMenu.push({ text: "None (no camera control)", checked: cur === "none", fn: function() { inputMap.setInputPtz(i, "none") } })
        ptzMenu.push({ text: "Bind PTZ1, PTZ2… to cameras in the PTZ drawer", fn: function() { if (!view.ptzOpen) view.toggleDrawer("ptz") } })
        ctx.show([
            { header: "Input " + (i + 1) + (e ? "  ·  " + e.title : "  ·  not mapped") },
            { text: "Take to PGM now", hint: "Alt+" + (i + 1), enabled: !!e, fn: function() { inputMap.take(i) } },
            { text: inputMap.pvw === i ? "Remove from preview" : "Select as preview", hint: String(i + 1), enabled: !!e, fn: function() { inputMap.select(i) } },
            { sep: true },
            { text: "Map to…", fn: function() { picker.openFor(i) } },
            quick.length ? { text: "Quick map", sub: quick } : null,
            { sep: true },
            { text: "CG on this input" + (mine.length ? "   (" + mine.map(inputMap.cgName).join(", ") + ")" : ""), sub: cgMenu },
            e ? { text: "PTZ camera   (" + (cur === "auto" ? "AUTO" : cur === "none" ? "none" : (inputMap.ptzSlot(cur) || { name: cur }).name) + ")", sub: ptzMenu } : null,
            { sep: true },
            { text: "Clear mapping", danger: true, enabled: !!e, fn: function() { inputMap.clear(i) } }
        ])
    }
    /// "Lower third · Jane Doe" for a CG slot
    function cgSummary(s) {
        var p = s.params || {}, t = [p.text1, p.text2].filter(function(x) { return x && String(x).length }).join(" / ")
        return String(s.template || "").replace(/_/g, " ") + (t ? "  ·  " + (t.length > 32 ? t.substring(0, 32) + "…" : t) : "")
    }
    /// Short tag on a bus key: "CG1+CG2 · PTZ1"
    function inputTags(i) {
        if (!inputMap.slots[i]) return ""
        var cg = inputMap.cgOf(i).map(inputMap.cgName), pz = inputMap.ptzFor(i), parts = []
        // CG1, CG2, CG4 -> "CG1,2,4" (names that are not CGn stay as they are)
        if (cg.length) parts.push(cg.every(function(n) { return /^CG\d+$/.test(n) }) ? "CG" + cg.map(function(n) { return n.substring(2) }).join(",") : cg.join("+"))
        if (pz.label) parts.push(pz.label)
        return parts.join("  ·  ")
    }
    function channelMenu(c) {
        var id = c.profile.id, armed = c.executor && c.executor.armed
        ctx.show([
            { header: c.profile.name },
            { text: "Control this channel", fn: function() { supervisor.activeChannelId = id } },
            { sep: true },
            { text: "Play", fn: function() { supervisor.play(id) } },
            { text: "Pause", fn: function() { supervisor.pause(id) } },
            { text: "Skip to next", fn: function() { supervisor.skip(id) } },
            { text: "Hold on slate", fn: function() { supervisor.hold(id) } },
            { text: "Stop", fn: function() { supervisor.stop(id) } },
            { sep: true },
            { text: armed ? "Disarm schedule" : "Arm schedule", fn: function() { supervisor.arm(!armed, id) } },
            { text: "Load hour now", fn: function() { supervisor.loadHour(id) } },
            { text: "Snap to schedule", fn: function() { supervisor.snap(id) } },
            { text: "Re-roll query blocks", fn: function() { api.post("/channels/" + id + "/reroll", {}, function(ok) { if (ok) window.toast("Blocks re-rolled", "ok") }) } },
            { sep: true },
            { text: "Edit schedule", fn: function() { supervisor.activeChannelId = id; window.navTo("schedule") } },
            { text: "As-played report (today)", fn: function() { Qt.openUrlExternally(api.rawUrl("/reports/asplayed.html?channel=" + id)) } },
            { text: "Copy preview URL", fn: function() { ctx.copyText(api.rawUrl("/preview/" + id + ".mjpeg"), "preview URL") } },
            { sep: true },
            { text: "Restart engine", danger: true, fn: function() { api.post("/channels/" + id + "/restart", {}, function(ok) { if (ok) window.toast("Engine restarting", "ok") }) } }
        ])
    }

    // ===========================================================================================================================================
    RowLayout {
        anchors.fill: parent
        spacing: 10
        layoutDirection: layoutPrefs.sideLeft ? Qt.RightToLeft : Qt.LeftToRight

        // ---------------------------------------------------------------------- monitors, bus, deck, rundown
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumWidth: 640
            spacing: 8

            // ---- monitor wall
            Item {
                id: wall
                Layout.fillWidth: true
                // the wall gives up height while a drawer (mixer / PTZ) is open
                Layout.preferredHeight: Math.max(180, Math.min(view.height * 0.55, view.height * layoutPrefs.wallHeight / 100 - (view.drawersOpen ? 60 : 0)))
                readonly property real gap: 8
                readonly property real pgmW: Math.min(height * 16 / 9, width * 0.40)
                readonly property real pvwW: Math.min(300, width * 0.22)
                readonly property real bankW: Math.max(120, width - pgmW - pvwW - 2 * gap)
                readonly property int tiles: supervisor.channels.length + view.extraMixes.length + inputMap.count
                // fewest columns whose rows of 16:9 tiles still fit the wall height (the biggest tiles that show everything)
                readonly property int cols: {
                    for (var c = 2; c <= 6; ++c) {
                        var tw = (bankW - (c - 1) * 6) / c, th = tw * 9 / 16 + 16, rows = Math.ceil(tiles / c)
                        if (rows * th + (rows - 1) * 6 <= height) return c
                    }
                    return 6
                }

                // PGM
                MonitorTile {
                    id: pgmTile
                    x: 0; y: 0; width: wall.pgmW; height: wall.height
                    tally: "pgm"
                    label: (view.active && view.active.profile ? view.active.profile.name : view.ch.toUpperCase()) + "  ·  " + (view.st.title || "no event loaded")
                    sub: view.st.timecode || ""
                    badge: view.stateWord(view.engineState)
                    badgeTint: Theme.stateColor(view.engineState)
                    captionH: 24
                    ProgramImage { anchors.fill: parent; channelId: view.ch }
                    Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; height: 4; color: Theme.bg4
                        Rectangle { width: view.st.duration > 0 ? parent.width * Math.min(1, view.st.position / view.st.duration) : 0; height: parent.height; color: Theme.onAir } }
                    Rectangle {
                        anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 8; anchors.topMargin: 12
                        width: remainText.implicitWidth + 16; height: remainText.implicitHeight + 6; radius: Theme.r5; color: "#000000b0"
                        visible: view.engineState === "ON_AIR"
                        Text { id: remainText; anchors.centerIn: parent; text: "−" + Theme.fmtClock(view.st.remaining); color: (view.st.remaining || 0) < 10 ? Theme.onAir : "#fff"; font.family: Theme.fontMono; font.pixelSize: Theme.fs20; font.bold: true }
                    }
                    Rectangle {
                        visible: view.onTake
                        anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: 8; anchors.bottomMargin: 32
                        width: takeText.implicitWidth + 14; height: 20; radius: 3; color: Theme.standby
                        Text { id: takeText; anchors.centerIn: parent; text: "LIVE TAKE  ·  NEXT returns to schedule"; color: "#05050a"; font.pixelSize: Theme.fs10; font.bold: true }
                    }
                    Text { anchors.centerIn: parent; visible: !view.engineUp; text: "NO ENGINE"; color: Theme.text2; font.pixelSize: Theme.fs16; font.bold: true }
                }

                // PVW + cut
                ColumnLayout {
                    x: wall.pgmW + wall.gap; y: 0; width: wall.pvwW; height: wall.height; spacing: 6
                    MonitorTile {
                        Layout.fillWidth: true; Layout.preferredHeight: Math.min(width * 9 / 16 + 20, wall.height - 84)     // leaves room for the keys and transition chips
                        tally: inputMap.pvw >= 0 ? "pvw" : ""
                        label: inputMap.pvw >= 0 && inputMap.slots[inputMap.pvw] ? "PREVIEW  ·  " + inputMap.slots[inputMap.pvw].title : "PREVIEW"
                        badge: inputMap.pvw >= 0 ? "INPUT " + (inputMap.pvw + 1) : ""
                        badgeTint: Theme.ready
                        onDoubleClicked: { if (inputMap.pvw >= 0) inputMap.cut() }
                        InputCard {
                            anchors.fill: parent; big: true; slot: Math.max(0, inputMap.pvw)
                            visible: inputMap.pvw >= 0
                            entry: inputMap.pvw >= 0 ? inputMap.slots[inputMap.pvw] : null
                            liveChannel: view.liveChannelFor(entry)
                            monitorId: inputMap.pvw >= 0 ? inputMap.monitorKey(inputMap.pvw) : ""
                            monitor: view.monByKey["in" + (inputMap.pvw + 1)] || null
                            skipped: inputMap.monitorSkipped["in" + (inputMap.pvw + 1)] || ""
                        }
                        Text { visible: inputMap.pvw < 0; anchors.centerIn: parent; width: parent.width - 20; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.WordWrap
                            text: "Click an input to preview it,\ndouble-click to take it to PGM"; color: Theme.text3; font.pixelSize: Theme.fs11 }
                    }
                    RowLayout {
                        Layout.fillWidth: true; spacing: 6
                        DeckButton { Layout.fillWidth: true; implicitHeight: 46; text: "CUT"; glyph: "⇥"; variant: "danger"; active: inputMap.pvw >= 0; enabled: inputMap.pvw >= 0 && view.engineUp; onClicked: { inputMap.cut() } ToolTip.text: "Hard cut the preview to program (Ctrl+Enter)"; ToolTip.visible: hovered }
                        DeckButton { Layout.fillWidth: true; implicitHeight: 46; text: "AUTO " + inputMap.transLabel(inputMap.transType); glyph: "⧉"; variant: "success"; active: inputMap.pvw >= 0; enabled: inputMap.pvw >= 0 && view.engineUp; onClicked: { inputMap.autoTake() } ToolTip.text: "Take the preview with a " + inputMap.transSec + " s " + inputMap.transLabel(inputMap.transType).toLowerCase() + " (Ctrl+Shift+Enter)"; ToolTip.visible: hovered }
                        DeckButton { Layout.fillWidth: true; implicitHeight: 46; text: "SCHEDULE"; glyph: "↩"; variant: "warn"; active: view.onTake; enabled: view.onTake; onClicked: { supervisor.skip() } ToolTip.text: "Leave the live take; the schedule resumes from the current time"; ToolTip.visible: hovered }
                    }
                    // AUTO transition: type chips + duration (click cycles 0.25 / 0.5 / 1 / 2 s). Saved with the input map.
                    RowLayout {
                        Layout.fillWidth: true; spacing: 3
                        Repeater {
                            model: inputMap.transTypes
                            Rectangle {
                                Layout.fillWidth: true; implicitHeight: 20; radius: 3
                                readonly property bool on: inputMap.transType === modelData
                                color: on ? Qt.rgba(Theme.ready.r, Theme.ready.g, Theme.ready.b, 0.28) : chipMa.containsMouse ? Theme.bg4 : Theme.bg3
                                border.color: on ? Theme.ready : Theme.border1
                                Text { anchors.centerIn: parent; text: inputMap.transLabel(modelData); color: parent.on ? Theme.text0 : Theme.text2; font.pixelSize: Theme.fs10; font.bold: true }
                                MouseArea { id: chipMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: { inputMap.setTransition(modelData) } }
                            }
                        }
                        Rectangle {
                            implicitWidth: durTxt.implicitWidth + 12; implicitHeight: 20; radius: 3
                            color: durMa.containsMouse ? Theme.bg4 : Theme.bg3; border.color: Theme.border1
                            Text { id: durTxt; anchors.centerIn: parent; text: inputMap.transSec + " s"; color: Theme.text1; font.family: Theme.fontMono; font.pixelSize: Theme.fs10; font.bold: true }
                            MouseArea {
                                id: durMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                onClicked: { var d = inputMap.transDurations, i = d.indexOf(inputMap.transSec); inputMap.setTransition(undefined, d[(i + 1) % d.length]) }
                            }
                        }
                    }
                    AudioStrip {
                        visible: !view.drawersOpen
                        Layout.fillWidth: true; Layout.fillHeight: true; Layout.minimumHeight: 52; Layout.maximumHeight: 64
                        meters: supervisor.activeMeters; audio: view.comp.audio || ({}); channels: view.active && view.active.profile ? view.active.profile.audioChannels : 2
                    }
                }

                // bank: every channel's program + the eight mapped inputs
                Flow {
                    id: bank
                    x: wall.pgmW + wall.pvwW + 2 * wall.gap; y: 0; width: wall.bankW; height: wall.height
                    spacing: 6
                    clip: true
                    readonly property real tw: Math.floor((width - (wall.cols - 1) * spacing) / wall.cols)
                    Repeater {
                        model: supervisor.channelModel   // rows updated in place: the tiles (and their pictures) survive snapshots
                        MonitorTile {
                            width: bank.tw; height: Math.round(bank.tw * 9 / 16) + 16
                            captionH: 16
                            readonly property string cid: modelData.profile.id
                            property var tel: supervisor.telemetryOf(cid)
                            readonly property string cst: tel && tel.state ? tel.state : (modelData.engineState || "OFFLINE")
                            tally: cst === "ON_AIR" ? "pgm" : ""
                            selected: cid === view.ch
                            label: modelData.profile.name
                            badge: cid === view.ch ? "CTRL" : view.stateWord(cst)
                            badgeTint: cid === view.ch ? Theme.accent0 : Theme.stateColor(cst)
                            onClicked: { supervisor.activeChannelId = cid }
                            onRightClicked: { view.channelMenu(modelData) }
                            Connections { target: supervisor; function onTelemetryChanged(id) { if (id === cid) tel = supervisor.telemetryOf(cid) } }
                            ProgramImage { anchors.fill: parent; channelId: cid }
                            Text { anchors.centerIn: parent; visible: !(modelData.process && modelData.process.connected); text: "NO ENGINE"; color: Theme.text3; font.pixelSize: Theme.fs10; font.bold: true }
                        }
                    }
                    // the controlled channel's extra output mixes (their own graphics): confidence pictures from the engine
                    Repeater {
                        model: view.extraMixes
                        MonitorTile {
                            width: bank.tw; height: Math.round(bank.tw * 9 / 16) + 16
                            captionH: 16
                            label: "MIX · " + String(modelData.name || modelData.id).toUpperCase()
                            badge: view.ch.toUpperCase()
                            badgeTint: Theme.accent2
                            onRightClicked: { ctx.copyText(api.rawUrl("/preview/" + view.ch + ":mix-" + modelData.id + ".mjpeg"), "mix preview URL") }
                            ProgramImage { id: mixImg; anchors.fill: parent; channelId: view.ch + ":mix-" + modelData.id }
                            Text { anchors.centerIn: parent; visible: !mixImg.hasFrame; text: "NO PICTURE"; color: Theme.text3; font.pixelSize: Theme.fs10; font.bold: true }
                        }
                    }
                    Repeater {
                        model: inputMap.count
                        MonitorTile {
                            id: inTile
                            width: bank.tw; height: Math.round(bank.tw * 9 / 16) + 16
                            captionH: 16
                            readonly property var entry: inputMap.slots[index]
                            tally: view.pgmSlot === index ? "pgm" : inputMap.pvw === index ? "pvw" : ""
                            label: (index + 1) + (entry ? "  ·  " + entry.title : "  ·  empty")
                            badge: entry ? (entry.kind || "").toUpperCase() : ""
                            badgeTint: Theme.text2
                            onClicked: { if (entry) inputMap.select(index); else picker.openFor(index) }
                            onDoubleClicked: { if (entry) inputMap.take(index) }
                            onRightClicked: { view.slotMenu(index) }
                            InputCard {
                                anchors.fill: parent; slot: index; entry: inTile.entry; liveChannel: view.pgmSlot === index ? view.ch : view.liveChannelFor(inTile.entry)
                                monitorId: inTile.entry ? inputMap.monitorKey(index) : ""
                                monitor: view.monByKey["in" + (index + 1)] || null
                            }
                        }
                    }
                }
            }

            RowLayout { Layout.fillWidth: true; spacing: 6
                CSelect { label: "SHOW TEMPLATE"; Layout.preferredWidth: 220; model: view.shows.map(function(s) { return s.name }); currentIndex: view.showIndex; onActivated: function(i) { view.showIndex = i } }
                CButton { text: "Load show"; small: true; Layout.alignment: Qt.AlignBottom; enabled: view.showIndex >= 0; onClicked: view.applyShow() }
                CButton { text: "Save current…"; small: true; Layout.alignment: Qt.AlignBottom; onClicked: { showName.text = view.shows[view.showIndex] ? view.shows[view.showIndex].name : ""; showDialog.open() } }
                CButton { text: "Delete"; small: true; variant: "ghost"; Layout.alignment: Qt.AlignBottom; enabled: view.showIndex >= 0; onClicked: { var s = view.shows[view.showIndex]; api.del("/shows/" + s.id, function(ok) { if (ok) { view.showIndex = -1; view.loadShows(); window.toast("Deleted show " + s.name, "ok") } }) } }
                Text { Layout.fillWidth: true; Layout.alignment: Qt.AlignBottom; text: "Loading a show changes mappings and presets; it does not cut program."; color: Theme.text3; font.pixelSize: Theme.fs10; elide: Text.ElideRight }
            }
            // ---- preview and program buses
            RowLayout {
                Layout.fillWidth: true; spacing: 6
                Text { text: "PVW\nBUS"; color: Theme.ready; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1; Layout.preferredWidth: 38 }
                Repeater {
                    model: inputMap.count
                    BusButton {
                        Layout.fillWidth: true; Layout.preferredWidth: 100
                        slot: index; entry: inputMap.slots[index]; tags: view.inputTags(index)
                        onPgm: view.pgmSlot === index; onPvw: inputMap.pvw === index
                        onClicked: { if (entry) inputMap.select(index); else picker.openFor(index) }
                        onDoubleClicked: { if (entry) inputMap.take(index) }
                        onRightClicked: { view.slotMenu(index) }
                    }
                }
            }
            RowLayout { Layout.fillWidth: true; spacing: 6
                Text { text: "PGM\nCUT"; color: Theme.onAir; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1; Layout.preferredWidth: 38 }
                Repeater { model: inputMap.count
                    Rectangle { Layout.fillWidth: true; Layout.preferredWidth: 100; implicitHeight: 27; radius: 3
                        readonly property var entry: inputMap.slots[index]
                        color: view.pgmSlot === index ? Theme.onAir : directMa.containsMouse && entry ? Theme.bg5 : Theme.bg3
                        border.color: view.pgmSlot === index ? Theme.onAir : Theme.border1
                        Text { anchors.centerIn: parent; width: parent.width - 8; elide: Text.ElideRight; horizontalAlignment: Text.AlignHCenter; text: (index+1) + "  " + (parent.entry ? parent.entry.title : "empty"); color: parent.entry ? Theme.text0 : Theme.text3; font.pixelSize: Theme.fs10; font.bold: true }
                        MouseArea { id: directMa; anchors.fill: parent; hoverEnabled: true; enabled: !!parent.entry && view.engineUp; onClicked: inputMap.take(index, undefined, "cut") }
                        ToolTip.visible: directMa.containsMouse && directMa.enabled; ToolTip.text: "Cut this input directly to program"
                    }
                }
            }

            // ---- drawers: audio mixer and PTZ, collapsed by default
            RowLayout {
                Layout.fillWidth: true; spacing: 6
                Rectangle {
                    implicitWidth: mixLbl.implicitWidth + 24; implicitHeight: 24; radius: Theme.r5
                    color: view.mixerOpen ? Theme.bg5 : mixMa.containsMouse ? Theme.bg4 : Theme.bg3; border.color: view.mixerOpen ? Theme.accent0 : Theme.border1
                    Text { id: mixLbl; anchors.centerIn: parent; text: (view.mixerOpen ? "▾" : "▸") + "  AUDIO MIXER" + (supervisor.activeMeters.solo ? "  ·  SOLO" : "") + (!mixer.afv ? "  ·  AFV OFF" : ""); color: supervisor.activeMeters.solo || !mixer.afv ? Theme.warn : view.mixerOpen ? Theme.text0 : Theme.text1; font.pixelSize: Theme.fs11; font.bold: true; font.letterSpacing: 1 }
                    MouseArea { id: mixMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: { view.toggleDrawer("mixer") } }
                }
                Rectangle {
                    implicitWidth: ptzLbl.implicitWidth + 24; implicitHeight: 24; radius: Theme.r5
                    color: view.ptzOpen ? Theme.bg5 : ptzMa.containsMouse ? Theme.bg4 : Theme.bg3; border.color: view.ptzOpen ? Theme.accent0 : Theme.border1
                    Text { id: ptzLbl; anchors.centerIn: parent; text: (view.ptzOpen ? "▾" : "▸") + "  PTZ"; color: view.ptzOpen ? Theme.text0 : Theme.text1; font.pixelSize: Theme.fs11; font.bold: true; font.letterSpacing: 1 }
                    MouseArea { id: ptzMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: { view.toggleDrawer("ptz") } }
                }
                Item { Layout.fillWidth: true }
                CButton { text: "Arrange layout"; small: true; onClicked: layoutDialog.open() }
            }
            RowLayout {
                visible: view.drawersOpen
                Layout.fillWidth: true; Layout.preferredHeight: layoutPrefs.drawerHeight; spacing: 8
                MixerPanel { visible: view.mixerOpen; Layout.fillWidth: true; Layout.fillHeight: true }
                PtzPanel { visible: view.ptzOpen; Layout.preferredWidth: 420; Layout.fillHeight: true; pgmSlot: view.pgmSlot }
            }

            // ---- control deck
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 164
                color: Theme.bg2; radius: Theme.r8; border.color: Theme.border0
                ColumnLayout {
                    anchors.fill: parent; anchors.margins: 10; spacing: 8
                    RowLayout {
                        Layout.fillWidth: true; spacing: 14
                        ColumnLayout {
                            Layout.fillWidth: true; spacing: 2
                            RowLayout {
                                spacing: 8
                                CBadge { text: view.stateWord(view.engineState); tint: Theme.stateColor(view.engineState); solid: view.engineState === "ON_AIR" }
                                CBadge { visible: view.onTake; text: "live take"; tint: Theme.standby; solid: true }
                                CBadge { visible: view.exec.emergency === true; text: "emergency loop"; tint: Theme.onAir; solid: true }
                                Text { Layout.fillWidth: true; text: view.st.title || "No event loaded"; color: Theme.text0; font.family: Theme.fontDisplay; font.pixelSize: Theme.fs16; font.bold: true; elide: Text.ElideRight }
                            }
                            Rectangle { Layout.fillWidth: true; height: 5; radius: 2; color: Theme.bg4
                                Rectangle { width: view.st.duration > 0 ? parent.width * Math.min(1, view.st.position / view.st.duration) : 0; height: parent.height; radius: 2; color: view.engineState === "ON_AIR" ? Theme.onAir : Theme.text3 } }
                            RowLayout {
                                spacing: 8
                                Text { text: "NEXT"; color: Theme.text3; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1 }
                                Text { Layout.fillWidth: true; text: view.st.nextTitle || "—"; color: view.st.nextReady ? Theme.text1 : Theme.standby; font.pixelSize: Theme.fs12; elide: Text.ElideRight }
                            }
                            Text {
                                Layout.fillWidth: true; elide: Text.ElideRight; color: Theme.text3; font.family: Theme.fontMono; font.pixelSize: Theme.fs10
                                text: (view.cur.slotId ? "HOUR " + view.cur.slotId + "   " : "") + Theme.fmtTime(view.cur.hourStartMs, true) + " – " + Theme.fmtTime(view.cur.hourEndMs, true)
                                      + "   SCHED " + Theme.fmtDur(view.cur.scheduledSec) + "   GAP " + Theme.fmtDur(view.cur.gapSec)
                                      + "   DRIFT " + (view.exec.driftSec !== undefined ? view.exec.driftSec.toFixed(2) : "0.00") + " s"
                                      + (view.exec.lastPushReason ? "   PUSH " + view.exec.lastPushReason : "")
                            }
                            Text {
                                Layout.fillWidth: true; elide: Text.ElideRight; color: Theme.text3; font.family: Theme.fontMono; font.pixelSize: Theme.fs10
                                text: {
                                    var t = view.st.telemetry || {}, outs = view.st.outputs || [], up = 0, n = 0
                                    for (var i = 0; i < outs.length; ++i) { if (outs[i].kind === "encoder") continue; ++n; if (outs[i].connected) ++up }
                                    return "ENGINE " + view.ch.toUpperCase() + "   TICK " + (t.tickMsLast || 0).toFixed(1) + "/" + (t.tickMsMax || 0).toFixed(0) + " ms   LATE " + (t.lateTicks || 0)
                                         + "   MISSED CUTS " + (t.missedCuts || 0) + "   DECODER Q " + (t.decoderQueue || 0) + "   OUTPUTS " + up + "/" + n + " up   " + (view.comp.gpu ? "GPU D3D11" : "CPU")
                                }
                            }
                        }
                        // countdown + clocks
                        ColumnLayout {
                            spacing: 0; Layout.alignment: Qt.AlignVCenter
                            Text { Layout.alignment: Qt.AlignRight; text: "REMAINING"; color: Theme.text3; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1 }
                            Text { Layout.alignment: Qt.AlignRight; text: view.engineState === "ON_AIR" || view.engineState === "PAUSED" ? Theme.fmtClock(view.st.remaining) : "--:--"
                                color: view.engineState !== "ON_AIR" ? Theme.text2 : (view.st.remaining || 0) < 10 ? Theme.onAir : (view.st.remaining || 0) < 30 ? Theme.standby : Theme.text0
                                font.family: Theme.fontMono; font.pixelSize: Theme.fs40; font.bold: true }
                        }
                        Rectangle { width: 1; Layout.fillHeight: true; color: Theme.border1 }
                        ColumnLayout {
                            spacing: 0; Layout.alignment: Qt.AlignVCenter
                            Text { Layout.alignment: Qt.AlignRight; text: Qt.formatTime(view.now, "hh:mm:ss"); color: Theme.text0; font.family: Theme.fontMono; font.pixelSize: Theme.fs20; font.bold: true }
                            Text { Layout.alignment: Qt.AlignRight; text: "TOH  " + Theme.pad2(Math.floor(view.tohSec / 60)) + ":" + Theme.pad2(view.tohSec % 60); color: view.tohSec < 60 ? Theme.onAir : view.tohSec < 300 ? Theme.standby : Theme.ready; font.family: Theme.fontMono; font.pixelSize: Theme.fs14; font.bold: true }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true; spacing: 6
                        DeckButton { Layout.fillWidth: true; text: view.engineState === "ON_AIR" ? "PAUSE" : "PLAY"; glyph: view.engineState === "ON_AIR" ? "‖" : "▶"; variant: "success"; active: view.engineState === "ON_AIR"; enabled: view.engineUp; onClicked: { view.engineState === "ON_AIR" ? supervisor.pause() : supervisor.play() } ToolTip.text: "Space"; ToolTip.visible: hovered }
                        DeckButton { Layout.fillWidth: true; text: "STOP"; glyph: "■"; enabled: view.engineUp; onClicked: { supervisor.stop(view.ch) } ToolTip.text: "Stop (slate)"; ToolTip.visible: hovered }
                        DeckButton { Layout.fillWidth: true; text: "NEXT"; glyph: "⏭"; variant: "info"; enabled: view.engineUp; onClicked: { supervisor.skip() } ToolTip.text: "Take the next item now. During a live take this returns to the schedule (Ctrl+Right)"; ToolTip.visible: hovered }
                        DeckButton { Layout.fillWidth: true; text: "HOLD"; glyph: "⏸"; variant: "warn"; active: view.engineState === "HOLD"; enabled: view.engineUp; onClicked: { supervisor.hold() } ToolTip.text: "Hold on slate"; ToolTip.visible: hovered }
                        DeckButton { Layout.fillWidth: true; text: "SNAP"; glyph: "↻"; enabled: view.engineUp; onClicked: { supervisor.snap() } ToolTip.text: "Seek to the wall-clock position"; ToolTip.visible: hovered }
                        Rectangle { width: 1; Layout.fillHeight: true; color: Theme.border1 }
                        DeckButton { Layout.fillWidth: true; text: view.armed ? "ARMED" : "DISARMED"; glyph: "⚡"; variant: "warn"; active: view.armed; onClicked: { supervisor.arm(!view.armed) } ToolTip.text: "Schedule execution on / off"; ToolTip.visible: hovered }
                        DeckButton { Layout.fillWidth: true; text: "LOAD HOUR"; glyph: "⤓"; enabled: view.engineUp; onClicked: { supervisor.loadHour() } ToolTip.text: "Push this hour (join in progress)"; ToolTip.visible: hovered }
                        DeckButton { Layout.fillWidth: true; text: "TOH"; glyph: "⏰"; variant: "primary"; enabled: view.engineUp; onClicked: { supervisor.arm(true); supervisor.loadHour() } ToolTip.text: "Arm and resync at the top of the hour"; ToolTip.visible: hovered }
                        Rectangle { width: 1; Layout.fillHeight: true; color: Theme.border1 }
                        DeckButton { Layout.fillWidth: true; text: view.engineUp ? "ENGINE ON" : "ENGINE OFF"; glyph: "●"; variant: view.engineUp ? "danger" : "default"; active: view.engineUp; onClicked: { view.toggleEngine() } ToolTip.text: view.engineUp ? "Stop the channel engine process" : "Start the channel engine process"; ToolTip.visible: hovered }
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                readonly property var rec: view.st.recording || ({})
                CButton { text: parent.rec.active ? "STOP RECORDING" : parent.rec.finalizing ? "FINALIZING…" : "RECORD PROGRAM + LAYERS"; small: true; variant: parent.rec.active ? "danger" : "primary"; enabled: view.engineUp && !parent.rec.finalizing
                    onClicked: api.post("/channels/" + view.ch + "/transport", {action: "record", params: {op: view.st.recording && view.st.recording.active ? "stop" : "start"}}, function(ok, d) { window.toast(ok ? "Recording command sent; status confirms capture" : (d.error || "Recording command failed"), ok ? "ok" : "error") }) }
                CButton { text: "Recompose…"; small: true; onClicked: { recordingSession.text = view.st.recording ? view.st.recording.directory || "" : ""; recordingOutput.text = recordingSession.text ? recordingSession.text + "/recomposed-" + Qt.formatDateTime(new Date(),"yyyyMMdd-hhmmss") + ".mkv" : ""; exportDialog.open() } }
                Text { Layout.fillWidth: true; elide: Text.ElideMiddle; color: parent.rec.error ? Theme.danger : Theme.text2; font.pixelSize: Theme.fs10
                    text: parent.rec.error || ((parent.rec.active ? "REC · " : parent.rec.complete ? "SAVED · " : "") + (parent.rec.directory || "Program MKV + editable layer archive")) }
            }
            // ---- rundown
            HourRundown {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumHeight: 120
                runlist: view.run
                state: view.st
                showNext: true
                headerRight: [
                    Text { text: view.ch.toUpperCase() + "  ·  now + next hour"; color: Theme.text3; font.pixelSize: Theme.fs10 },
                    CButton { text: "Load hour"; small: true; variant: "info"; onClicked: { supervisor.loadHour() } }
                ]
            }
        }

        // ---------------------------------------------------------------------- graphics, audio, engine
        ColumnLayout {
            Layout.preferredWidth: Math.max(300, Math.min(layoutPrefs.sideWidth, view.width - 650))
            Layout.minimumWidth: 300
            Layout.maximumWidth: Math.max(300, Math.min(layoutPrefs.sideWidth, view.width - 650))
            Layout.fillHeight: true
            spacing: 8
            Repeater {
                model: view.sideOrder
                Loader {
                    Layout.fillWidth: true
                    Layout.fillHeight: modelData === "graphics"
                    Layout.preferredHeight: modelData === "current" ? Math.max(130, Math.min(190, view.height * 0.17)) : modelData === "reporter" ? Math.min(220, view.height * 0.3) : 200
                    sourceComponent: modelData === "reporter" ? reporterComponent : modelData === "current" ? currentComponent : graphicsComponent
                }
            }
        }
    }
    Component { id: reporterComponent; ReporterFeedPanel {} }
    Component { id: currentComponent; CurrentCg { layers: view.cgLayers; onHideLayer: function(e) { view.hideLayer(e) }; onClearAll: view.clearGraphics() } }
    Component { id: graphicsComponent; GraphicsDeck {
        channelId: view.ch; onAirLayers: view.overlayLayers
        onChanged: view.reloadLayers()
        Component.onCompleted: view.deckRef = this
        Component.onDestruction: if (view.deckRef === this) view.deckRef = null
    } }
}
