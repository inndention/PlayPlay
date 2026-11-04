import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".."

/**
 * Graphics deck: a quick strip that fires a lower third (or breaking banner, bug, ticker, title) in one click, and below it a template
 * picker with Text 1 / 2 / 3. Any template takes text1..3: the Supervisor maps them onto the template's own text fields (cgtext.h; the
 * manifest's `textFields` names the target of each), so the same three boxes drive every graphic. SHOW deploys, UPDATE rewrites the text of
 * the layer already on air, HIDE takes it off. The picture is an approximate preview of where the words land.
 * ON (shown when the channel has extra output mixes): which mixes the graphic appears in. ALL = every mix; otherwise the graphic
 * is sent to the engine with `mixes` (a web-only bug, a crawl that stays off the web stream...).
 */
CPanel {
    id: g
    property string channelId: supervisor.activeChannelId
    property var onAirLayers: []            // overlay manager layers of this channel
    property var templates: []
    property var presets: []
    property int idx: 0
    property string t1: ""
    property string t2: ""
    property string t3: ""
    property string hold: "0"
    property var target: []                  // [] = every mix; else the mix ids the next SHOW goes to
    readonly property var mixList: supervisor.activeMixes || []   // perf-app PA-3: [{id, name}] that changes only with the mix list, not the levels
    signal changed()

    readonly property var tpl: templates.length ? templates[Math.max(0, Math.min(idx, templates.length - 1))] : null
    readonly property var tf: tpl && tpl.textFields ? tpl.textFields : []
    readonly property var quick: [
        { id: "lower_third_modern", label: "LOWER 3RD" },
        { id: "banner_breaking_news", label: "BREAKING" },
        { id: "bug_live_indicator", label: "BUG" },
        { id: "ticker_news", label: "TICKER" },
        { id: "fullscreen_title_card", label: "TITLE" }
    ]
    readonly property var labels: { var a = []; for (var i = 0; i < templates.length; ++i) a.push(templates[i].name + "   ·   " + templates[i].category); return a }
    readonly property color accent: {
        var ps = tpl && tpl.params ? tpl.params : []
        for (var i = 0; i < ps.length; ++i) if ((ps[i].name === "color1" || ps[i].name === "accent" || ps[i].name === "color") && /^#?[0-9a-fA-F]{6}$/.test(String(ps[i].default || ""))) return String(ps[i].default).charAt(0) === "#" ? ps[i].default : "#" + ps[i].default
        return Theme.accent0
    }

    function fieldLabel(n) {
        if (n >= tf.length) return "TEXT " + (n + 1)
        var ps = tpl && tpl.params ? tpl.params : []
        for (var i = 0; i < ps.length; ++i) if (ps[i].name === tf[n]) return "TEXT " + (n + 1) + "  ·  " + String(ps[i].label || ps[i].name)
        return "TEXT " + (n + 1) + "  ·  " + tf[n]
    }
    function typedParams() {
        var p = {}
        if (t1.length) p.text1 = t1
        if (t2.length) p.text2 = t2
        if (t3.length) p.text3 = t3
        return p
    }
    function persist() { var s = window.cgDeckState; s.tplId = tpl ? tpl.id : ""; s.t1 = t1; s.t2 = t2; s.t3 = t3; s.hold = hold }
    function indexOfTemplate(id) { for (var i = 0; i < templates.length; ++i) if (templates[i].id === id) return i; return -1 }
    function layerOf(tplId, presetId) {
        for (var i = 0; i < onAirLayers.length; ++i) {
            var l = onAirLayers[i]
            if (l.state === "hiding") continue
            if (presetId ? l.presetId === presetId : l.template === tplId) return l
        }
        return null
    }

    function load() {
        api.get("/cg/templates", function(ok, d) {
            if (!ok || !d) return
            var list = (d.templates || []).filter(function(t) { return t.listed !== false })
            list.sort(function(a, b) { return (a.category + a.name).localeCompare(b.category + b.name) })
            g.templates = list
            var i = g.indexOfTemplate(window.cgDeckState.tplId)
            g.idx = i >= 0 ? i : Math.max(0, g.indexOfTemplate("lower_third_modern"))
        })
        api.get("/overlays/presets", function(ok, d) { if (ok && d) g.presets = (d.presets || []).slice(0, 6) })
    }
    function show(t) {
        var body = { template: t.id, params: typedParams(), role: t.role || "secondary" }
        var h = parseFloat(hold)
        if (h > 0) body.holdSec = h
        var tgt = g.target.slice()
        api.post("/overlays/" + channelId + "/deploy", body, function(ok, d) {
            if (!ok) return
            g.assignMixes(d, tgt)
            window.toast(t.name + " on air" + (tgt.length ? "  ·  " + tgt.join(", ").toUpperCase() : ""), "ok"); g.changed()
        })
    }
    /// Restrict a deployed graphic to the target mixes (the engine holds the assignment until the layer attaches).
    function assignMixes(d, tgt) {
        if (!tgt.length || !d) return
        var layer = d.layer !== undefined ? d.layer : d.overlay && d.overlay.layer !== undefined ? d.overlay.layer : undefined
        if (layer === undefined) { window.toast("Shown in every mix: the deploy did not report its layer", "info"); return }
        supervisor.cg({ action: "update", layer: layer, mixes: tgt }, g.channelId)
    }
    function toggleTarget(id) {
        if (id === "") { g.target = []; return }
        var a = g.target.slice(), i = a.indexOf(id)
        if (i >= 0) a.splice(i, 1); else a.push(id)
        g.target = a.length === g.mixList.length ? [] : a        // every mix picked = ALL
    }
    function update() {
        var l = tpl ? layerOf(tpl.id, "") : null
        if (!l) { window.toast("This template is not on air: use SHOW", "info"); return }
        api.post("/overlays/" + channelId + "/" + l.layer + "/update", { params: typedParams() }, function(ok) { if (ok) { window.toast("Text updated", "ok"); g.changed() } })
    }
    function hideTemplate(t, presetId) {
        var l = layerOf(t ? t.id : "", presetId || "")
        if (!l) return
        api.post("/overlays/" + channelId + "/" + l.layer + "/hide", {}, function(ok) { if (ok) g.changed() })
    }
    function fireQuick(id) {
        var i = indexOfTemplate(id)
        if (i < 0) { window.toast("Template " + id + " is not installed", "error"); return }
        var t = templates[i]
        if (layerOf(t.id, "")) { hideTemplate(t); return }      // second press takes it off
        idx = i; persist(); show(t)
    }
    // ---- CG slots CG1, CG2 ... (stored graphics; assigned to inputs they show when the input goes on air) --------------------------------
    readonly property var cgs: inputMap.cgSlots
    function cgLayer(id) {
        for (var i = 0; i < onAirLayers.length; ++i) { var l = onAirLayers[i]; if (l.state !== "hiding" && (l.presetId === "cg:" + id || l.ruleId === "cg:" + id)) return l }
        return null
    }
    function cgInputsText(s) {
        var a = s.inputs || []
        if (a.indexOf("*") >= 0) return "ALL INPUTS"
        var n = a.filter(function(x) { return /^in\d+$/.test(x) }).map(function(x) { return x.substring(2) })
        return n.length ? "IN " + n.join(",") : ""
    }
    function cgReplace(id, patch, msg) {
        var list = g.cgs.map(function(s) { return s.id === id ? Object.assign({}, s, patch) : s })
        inputMap.putCg(list, msg)
    }
    function cgStore(s) {
        if (!tpl) return
        var h = parseFloat(hold)
        g.cgReplace(s.id, { template: tpl.id, role: tpl.role || "", params: typedParams(), holdSec: h > 0 ? h : 0, duration: h > 0 ? h : (s.duration || 0), mixes: g.target.slice() },
                    s.name + " ← " + tpl.name + (t1.length ? "  ·  " + t1 : ""))
    }
    function cgLoad(s) {
        var i = indexOfTemplate(s.template)
        if (i < 0) { window.toast("Template " + s.template + " is not installed", "error"); return }
        var p = s.params || {}
        idx = i; t1 = p.text1 || ""; t2 = p.text2 || ""; t3 = p.text3 || ""; hold = String(s.holdSec || 0); persist()
    }
    function cgFire(s) {
        if (!s.template) { window.toast(s.name + " is empty: pick a template and text, then right-click " + s.name + " > Store", "info"); return }
        var on = !!cgLayer(s.id)
        api.post("/overlays/" + channelId + "/cgslots/" + s.id + (on ? "/hide" : "/fire"), on ? {} : { toggle: true }, function(ok) { if (ok) { window.toast(s.name + (on ? " off" : " on air"), "ok"); g.changed() } })
    }
    function cgToggleInput(s, key) {
        var a = (s.inputs || []).slice(), all = a.indexOf("*") >= 0
        if (key === "*") a = all ? [] : ["*"]
        else if (all) { a = []; for (var k = 1; k <= inputMap.count; ++k) if ("in" + k !== key) a.push("in" + k) }
        else { var j = a.indexOf(key); if (j >= 0) a.splice(j, 1); else a.push(key) }
        g.cgReplace(s.id, { inputs: a }, s.name + (a.length ? " on " + g.cgInputsText({ inputs: a }).toLowerCase() : " on no input"))
    }
    function cgAdd() {
        var n = g.cgs.length + 1, ids = g.cgs.map(function(s) { return s.id })
        while (ids.indexOf("cg" + n) >= 0) ++n
        inputMap.putCg(g.cgs.concat([{ id: "cg" + n, name: "CG" + n }]), "CG" + n + " added")
    }
    function cgMenu(s) {
        var ins = [{ text: "All inputs", checked: (s.inputs || []).indexOf("*") >= 0, fn: function() { g.cgToggleInput(s, "*") } }, { sep: true }]
        for (var k = 0; k < inputMap.count; ++k) (function(key, e) {
            ins.push({ text: "Input " + key.substring(2) + (e ? "   " + e.title : "   (not mapped)"), checked: (s.inputs || []).indexOf("*") >= 0 || (s.inputs || []).indexOf(key) >= 0, fn: function() { g.cgToggleInput(s, key) } })
        })("in" + (k + 1), inputMap.slots[k])
        ins.push({ sep: true }); ins.push({ text: "No inputs (fire by hand only)", checked: !(s.inputs || []).length, fn: function() { g.cgReplace(s.id, { inputs: [] }, s.name + " on no input") } })
        var starts = [0, 1, 2, 5, 10].map(function(v) { return { text: v === 0 ? "At the take" : v + " s after the take", checked: (s.startOffset === undefined ? 1 : s.startOffset) === v, fn: function() { g.cgReplace(s.id, { startOffset: v }) } } })
        var durs = [0, 5, 8, 10, 15, 30].map(function(v) { return { text: v === 0 ? "While the input is on air" : "For " + v + " s", checked: (s.duration || 0) === v, fn: function() { g.cgReplace(s.id, { duration: v }) } } })
        var on = !!cgLayer(s.id)
        ctxCg.show([
            { header: s.name + (s.template ? "  ·  " + s.template : "  ·  empty") },
            { text: "Store the deck's graphic in " + s.name, hint: "template + Text 1/2/3", enabled: !!g.tpl, fn: function() { g.cgStore(s) } },
            { text: "Load " + s.name + " into the deck", enabled: !!s.template, fn: function() { g.cgLoad(s) } },
            { text: on ? "Hide " + s.name : "Show " + s.name + " now", enabled: !!s.template, fn: function() { g.cgFire(s) } },
            { sep: true },
            { text: "On inputs   (" + (g.cgInputsText(s) || "none") + ")", sub: ins },
            { text: "Start", sub: starts },
            { text: "Stay", sub: durs },
            { sep: true },
            { text: "Clear " + s.name, danger: true, enabled: !!s.template, fn: function() { g.cgReplace(s.id, { template: "", params: {}, inputs: [] }, s.name + " cleared") } },
            { text: "Remove " + s.name, danger: true, enabled: g.cgs.length > 1, fn: function() { inputMap.putCg(g.cgs.filter(function(x) { return x.id !== s.id }), s.name + " removed") } }
        ])
    }
    function storeMenu() {
        var a = [{ header: "Store " + (g.tpl ? g.tpl.name : "") + " in…" }]
        for (var i = 0; i < g.cgs.length; ++i) (function(s) { a.push({ text: s.name + (s.template ? "   (replaces " + s.template + ")" : "   (empty)"), fn: function() { g.cgStore(s) } }) })(g.cgs[i])
        a.push({ sep: true }); a.push({ text: "New CG", fn: function() { g.cgAdd() } })
        ctxCg.show(a)
    }
    CMenu { id: ctxCg }

    function firePreset(p) {
        if (layerOf("", p.id)) { hideTemplate(null, p.id); return }
        var tgt = g.target.slice()
        api.post("/overlays/" + channelId + "/presets/" + p.id + "/fire", { params: typedParams() }, function(ok, d) { if (ok) { g.assignMixes(d, tgt); window.toast(p.name + " on air", "ok"); g.changed() } })
    }

    title: "Graphics"
    headerRight: [ Text { text: "Text 1 · 2 · 3 fill any template"; color: Theme.text3; font.pixelSize: Theme.fs10 } ]
    Component.onCompleted: {
        var s = window.cgDeckState
        t1 = s.t1 || ""; t2 = s.t2 || ""; t3 = s.t3 || ""; hold = s.hold || "0"
        load()
    }

    Flickable {
        anchors.fill: parent; contentHeight: col.implicitHeight; clip: true
        boundsBehavior: Flickable.StopAtBounds
        ColumnLayout {
            id: col
            width: parent.width; spacing: 8

            // quick strip
            Text { text: "QUICK  ·  press to fire with the text below, press again to hide"; color: Theme.text3; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 0.8 }
            Flow {
                Layout.fillWidth: true; spacing: 5
                Repeater {
                    model: g.quick
                    CButton { text: modelData.label; small: true; variant: "default"; active: !!g.layerOf(modelData.id, ""); onClicked: { g.fireQuick(modelData.id) } }
                }
                Repeater {
                    model: g.presets
                    CButton { text: "★ " + modelData.name; small: true; variant: "accent2"; active: !!g.layerOf("", modelData.id); onClicked: { g.firePreset(modelData) } ToolTip.text: "Saved overlay preset"; ToolTip.visible: hovered }
                }
            }
            // CG slots
            Text { text: "CG SLOTS  ·  click = show / hide, right-click = store, inputs, timing"; color: Theme.text3; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 0.8 }
            Flow {
                Layout.fillWidth: true; spacing: 5
                Repeater {
                    model: g.cgs
                    Rectangle {
                        readonly property bool on: !!g.cgLayer(modelData.id)
                        readonly property bool empty: !modelData.template
                        readonly property string where: g.cgInputsText(modelData)
                        width: Math.max(64, cgCol.implicitWidth + 14); height: 34; radius: Theme.r3
                        color: on ? Qt.rgba(Theme.ready.r, Theme.ready.g, Theme.ready.b, 0.25) : cgMa.containsMouse ? Theme.bg4 : Theme.bg3
                        border.color: on ? Theme.ready : empty ? Theme.border0 : Theme.border1
                        Column {
                            id: cgCol; anchors.centerIn: parent; spacing: 0
                            Text { anchors.horizontalCenter: parent.horizontalCenter; text: modelData.name; color: parent.parent.empty ? Theme.text3 : Theme.text0; font.pixelSize: Theme.fs11; font.bold: true }
                            Text { anchors.horizontalCenter: parent.horizontalCenter; text: parent.parent.empty ? "empty" : (parent.parent.where || "by hand"); color: parent.parent.where ? Theme.accent0 : Theme.text3; font.pixelSize: Theme.fs10 - 1; font.bold: true }
                        }
                        MouseArea { id: cgMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; acceptedButtons: Qt.LeftButton | Qt.RightButton
                            onClicked: function(m) { if (m.button === Qt.RightButton) g.cgMenu(modelData); else g.cgFire(modelData) } }
                        ToolTip.visible: cgMa.containsMouse; ToolTip.delay: 500
                        ToolTip.text: modelData.template ? modelData.template + ((modelData.params || {}).text1 ? "  ·  " + modelData.params.text1 : "") : "Empty: right-click > Store"
                    }
                }
                Rectangle {
                    width: 26; height: 34; radius: Theme.r3; color: cgAddMa.containsMouse ? Theme.bg4 : Theme.bg3; border.color: Theme.border1
                    Text { anchors.centerIn: parent; text: "+"; color: Theme.text1; font.pixelSize: Theme.fs14; font.bold: true }
                    MouseArea { id: cgAddMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: { g.cgAdd() } }
                }
            }
            // target mixes (only when the channel has more than main)
            Flow {
                visible: g.mixList.length > 1
                Layout.fillWidth: true; spacing: 4
                Text { text: "ON"; color: Theme.text3; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1; height: 20; verticalAlignment: Text.AlignVCenter }
                Repeater {
                    model: [{ id: "", name: "All mixes" }].concat(g.mixList)
                    Rectangle {
                        readonly property bool on: modelData.id === "" ? g.target.length === 0 : g.target.indexOf(modelData.id) >= 0
                        width: mt.implicitWidth + 14; height: 20; radius: 3
                        color: on ? Qt.rgba(Theme.ready.r, Theme.ready.g, Theme.ready.b, 0.25) : mtMa.containsMouse ? Theme.bg4 : Theme.bg3
                        border.color: on ? Theme.ready : Theme.border1
                        Text { id: mt; anchors.centerIn: parent; text: String(modelData.name || modelData.id).toUpperCase(); color: parent.on ? Theme.text0 : Theme.text2; font.pixelSize: Theme.fs10; font.bold: true }
                        MouseArea { id: mtMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: { g.toggleTarget(modelData.id) } }
                    }
                }
            }
            Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border0 }

            CSelect {
                Layout.fillWidth: true; label: "Template"
                model: g.labels
                currentIndex: g.idx
                onActivated: function(i) { g.idx = i; g.persist() }
            }

            RowLayout {
                Layout.fillWidth: true; spacing: 10
                CgPreview {
                    Layout.preferredWidth: 236; Layout.preferredHeight: 133; Layout.alignment: Qt.AlignTop
                    channelId: g.channelId; role: g.tpl ? g.tpl.role : "lower_third"; accent: g.accent
                    t1: g.t1; t2: g.tf.length > 1 ? g.t2 : ""; t3: g.tf.length > 2 ? g.t3 : ""
                }
                ColumnLayout {
                    Layout.fillWidth: true; Layout.alignment: Qt.AlignTop; spacing: 4
                    Text { text: g.tpl ? g.tpl.description || "" : ""; Layout.fillWidth: true; wrapMode: Text.WordWrap; maximumLineCount: 3; elide: Text.ElideRight; color: Theme.text2; font.pixelSize: Theme.fs10 }
                    CBadge { text: g.tpl ? g.tpl.role : ""; tint: Theme.accent0 }
                    CField { label: "Auto-hide (s, 0 = manual)"; Layout.fillWidth: true; mono: true; text: g.hold; onEdited: function(v) { g.hold = v; g.persist() } }
                }
            }

            CField { Layout.fillWidth: true; label: g.fieldLabel(0); enabled: g.tf.length > 0; opacity: enabled ? 1 : 0.45; placeholder: g.tf.length > 0 ? "" : "this template has no text field"; text: g.t1; onEdited: function(v) { g.t1 = v; g.persist() } onAccepted: { g.show(g.tpl) } }
            CField { Layout.fillWidth: true; label: g.fieldLabel(1); enabled: g.tf.length > 1; opacity: enabled ? 1 : 0.45; placeholder: g.tf.length > 1 ? "" : "not used by this template"; text: g.t2; onEdited: function(v) { g.t2 = v; g.persist() } onAccepted: { g.show(g.tpl) } }
            CField { Layout.fillWidth: true; label: g.fieldLabel(2); enabled: g.tf.length > 2; opacity: enabled ? 1 : 0.45; placeholder: g.tf.length > 2 ? "" : "not used by this template"; text: g.t3; onEdited: function(v) { g.t3 = v; g.persist() } onAccepted: { g.show(g.tpl) } }

            RowLayout {
                Layout.fillWidth: true; spacing: 6
                CButton { text: "SHOW"; icon2: "▶"; variant: "success"; Layout.fillWidth: true; enabled: !!g.tpl; onClicked: { g.show(g.tpl) } }
                CButton { text: "UPDATE"; icon2: "↻"; variant: "info"; Layout.fillWidth: true; enabled: !!g.tpl && !!g.layerOf(g.tpl.id, ""); onClicked: { g.update() } ToolTip.text: "Rewrite the text of the layer on air"; ToolTip.visible: hovered }
                CButton { text: "HIDE"; icon2: "■"; variant: "danger"; Layout.fillWidth: true; enabled: !!g.tpl && !!g.layerOf(g.tpl.id, ""); onClicked: { g.hideTemplate(g.tpl) } }
                CButton { text: "→ CG"; variant: "ghost"; enabled: !!g.tpl; onClicked: { g.storeMenu() } ToolTip.text: "Store this template and Text 1/2/3 in CG1, CG2…"; ToolTip.visible: hovered }
            }
        }
    }
}
