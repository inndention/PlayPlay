import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".."
import "../ui"

/**
 * CG Templates: a template builder for the engine's native graphics (lower
 * third, bug, ticker, clock, countdown, text, box) plus live inputs (NDI,
 * SDI, file) as layers; preview picture; show / update / hide per role.
 */
Item {
    id: view
    property var st: supervisor.activeState
    property string kind: "lower_third"
    property string role: "lower_third"
    property string anchor: "bottom_left"
    property string inType: "slide_left"
    property string outType: "fade"
    property real durIn: 0.5
    property real durOut: 0.4
    property real hold: 0
    property string title: "Lower third title"
    property string subtitle: "Subtitle"
    property string text: "Ticker text • PLAY PLAY"
    property string accent: "#00d4ff"
    property real marginX: 120
    property real marginY: 90
    readonly property var kinds: ["lower_third", "bug", "ticker", "clock", "countdown", "text", "box"]
    readonly property var roles: ["lower_third", "bug", "ticker", "legal_id", "secondary", "emergency"]
    readonly property var anchors9: ["top_left", "top", "top_right", "left", "center", "right", "bottom_left", "bottom", "bottom_right"]
    readonly property var trans: ["cut", "fade", "slide_left", "slide_right", "slide_up", "slide_down", "wipe_left", "wipe_right", "wipe_up", "wipe_down"]

    function spec() {
        var src = { type: "template", kind: kind, title: title, subtitle: subtitle, text: text, accentColor: accent }
        if (kind === "clock") src.format = "hh:mm:ss"
        if (kind === "countdown") src.seconds = 60
        return src
    }
    function show() {
        supervisor.cg({ action: "show", role: role, source: spec(), placement: { anchor: anchor, marginX: marginX, marginY: marginY },
                        "in": { type: inType, durationSec: durIn }, out: { type: outType, durationSec: durOut }, holdSec: hold })
        window.toast("CG show " + role, "ok")
    }
    function update() { supervisor.cg({ action: "update", role: role, source: spec() }) }
    function hide(r) { supervisor.cg({ action: "hide", role: r || role, transition: { type: outType, durationSec: durOut } }) }
    function showLive(type, extra) {
        var src = Object.assign({ type: type }, extra || {})
        supervisor.cg({ action: "show", role: "secondary", source: src, placement: { x: 1280, y: 80, width: 560, height: 315 }, "in": { type: "fade", durationSec: 0.3 }, audioMute: true })
    }

    RowLayout {
        anchors.fill: parent
        spacing: 10
        CPanel {
            Layout.preferredWidth: 440; Layout.fillHeight: true
            title: "Template builder"
            Flickable {
                anchors.fill: parent; contentHeight: formCol.implicitHeight; clip: true
                ColumnLayout {
                    id: formCol; width: parent.width; spacing: 10
                    Flow { Layout.fillWidth: true; spacing: 4
                        Repeater { model: view.kinds; CButton { text: modelData.replace("_", " "); small: true; variant: "ghost"; active: view.kind === modelData; onClicked: { view.kind = modelData; view.role = modelData === "bug" ? "bug" : modelData === "ticker" ? "ticker" : modelData === "clock" || modelData === "countdown" ? "legal_id" : "lower_third" } } } }
                    CField { label: "Title / text"; Layout.fillWidth: true; text: view.title; onEdited: function(v) { view.title = v } }
                    CField { visible: view.kind === "lower_third"; label: "Subtitle"; Layout.fillWidth: true; text: view.subtitle; onEdited: function(v) { view.subtitle = v } }
                    CField { visible: view.kind === "ticker" || view.kind === "text" || view.kind === "bug"; label: "Text"; Layout.fillWidth: true; text: view.text; onEdited: function(v) { view.text = v } }
                    GridLayout {
                        Layout.fillWidth: true; columns: 2; columnSpacing: 10; rowSpacing: 8
                        CSelect { label: "Layer role"; Layout.fillWidth: true; model: view.roles; currentIndex: Math.max(0, view.roles.indexOf(view.role)); onActivated: function(i) { view.role = view.roles[i] } }
                        CSelect { label: "Anchor"; Layout.fillWidth: true; model: view.anchors9; currentIndex: Math.max(0, view.anchors9.indexOf(view.anchor)); onActivated: function(i) { view.anchor = view.anchors9[i] } }
                        CField { label: "Margin X"; Layout.fillWidth: true; mono: true; text: String(view.marginX); onEdited: function(v) { view.marginX = parseFloat(v) || 0 } }
                        CField { label: "Margin Y"; Layout.fillWidth: true; mono: true; text: String(view.marginY); onEdited: function(v) { view.marginY = parseFloat(v) || 0 } }
                        CSelect { label: "Animate in"; Layout.fillWidth: true; model: view.trans; currentIndex: Math.max(0, view.trans.indexOf(view.inType)); onActivated: function(i) { view.inType = view.trans[i] } }
                        CField { label: "In (s)"; Layout.fillWidth: true; mono: true; text: String(view.durIn); onEdited: function(v) { view.durIn = parseFloat(v) || 0 } }
                        CSelect { label: "Animate out"; Layout.fillWidth: true; model: view.trans; currentIndex: Math.max(0, view.trans.indexOf(view.outType)); onActivated: function(i) { view.outType = view.trans[i] } }
                        CField { label: "Out (s)"; Layout.fillWidth: true; mono: true; text: String(view.durOut); onEdited: function(v) { view.durOut = parseFloat(v) || 0 } }
                        CField { label: "Auto-hide after (s, 0 = manual)"; Layout.fillWidth: true; mono: true; text: String(view.hold); onEdited: function(v) { view.hold = parseFloat(v) || 0 } }
                        CField { label: "Accent colour"; Layout.fillWidth: true; mono: true; text: view.accent; onEdited: function(v) { view.accent = v } }
                    }
                    RowLayout {
                        Layout.fillWidth: true; spacing: 6
                        CButton { text: "SHOW"; icon2: "▶"; variant: "success"; Layout.fillWidth: true; onClicked: { view.show() } }
                        CButton { text: "UPDATE"; icon2: "↻"; variant: "info"; Layout.fillWidth: true; onClicked: { view.update() } }
                        CButton { text: "HIDE"; icon2: "■"; variant: "danger"; Layout.fillWidth: true; onClicked: { view.hide() } }
                    }
                    Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border0 }
                    Text { text: "LIVE INPUTS AS LAYERS (secondary, 15)"; color: Theme.text2; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1 }
                    RowLayout {
                        Layout.fillWidth: true; spacing: 6
                        CField { id: ndiName; Layout.fillWidth: true; placeholder: "NDI source name (substring)"; text: "" }
                        CButton { text: "NDI PiP"; small: true; onClicked: { view.showLive("ndi", { name: ndiName.text }) } }
                    }
                    RowLayout {
                        Layout.fillWidth: true; spacing: 6
                        CField { id: sdiPort; Layout.preferredWidth: 90; placeholder: "port"; text: "1"; mono: true }
                        CButton { text: "SDI in PiP"; small: true; onClicked: { view.showLive("sdi_in", { port: parseInt(sdiPort.text) || 1 }) } }
                        CField { id: filePath; Layout.fillWidth: true; placeholder: "file path for PiP loop"; text: "" }
                        CButton { text: "File PiP"; small: true; onClicked: { view.showLive("file", { path: filePath.text, loop: true }) } }
                    }
                    Text { text: "DISCOVERED (" + ((view.st.liveInputs || []).length) + ")"; color: Theme.text2; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1 }
                    Flow {
                        Layout.fillWidth: true; spacing: 4
                        Repeater {
                            model: view.st.liveInputs || []
                            CButton { text: modelData.name; small: true; icon2: "\u25CF"; variant: "ghost"; onClicked: { view.showLive(modelData.type, { name: modelData.name }) } ToolTip.text: "Show as picture-in-picture" }
                        }
                        Text { visible: !(view.st.liveInputs && view.st.liveInputs.length); text: "No NDI sources found on the network yet"; color: Theme.text3; font.pixelSize: Theme.fs10 }
                    }
                    RowLayout {
                        Layout.fillWidth: true; spacing: 6
                        CButton { text: "Bars + tone"; small: true; onClicked: { supervisor.cg({ action: "show", role: "emergency", source: { type: "bars", captions: ["TECHNICAL DIFFICULTIES", "PLEASE STAND BY"] }, "in": { type: "fade", durationSec: 0.5 } }) } }
                        CButton { text: "Program to slate"; small: true; variant: "warn"; onClicked: { supervisor.cg({ action: "program_clear", transition: { type: "mix", durationSec: 1.0 } }) } }
                        Item { Layout.fillWidth: true }
                        CButton { text: "Clear all layers"; small: true; variant: "danger"; onClicked: { supervisor.cg({ action: "clear" }) } }
                    }
                }
            }
        }
        CPanel {
            id: htmlPanel
            Layout.preferredWidth: 380; Layout.fillHeight: true
            title: "HTML templates (" + htmlPanel.templates.length + ")"
            property var manifest: ({})
            property var templates: manifest.templates || []
            property var categories: { var c = []; for (var i = 0; i < templates.length; ++i) if (c.indexOf(templates[i].category) < 0) c.push(templates[i].category); return c }
            property string category: ""
            property string selId: ""
            property var sel: { for (var i = 0; i < templates.length; ++i) if (templates[i].id === selId) return templates[i]; return null }
            property var params: ({})
            property string animIn: "fade"
            property string animOut: "fade"
            property int animMs: 600
            property string deliver: ""
            property int outputNumber: 1
            property var outputDrafts: ({})
            property var deployed: (supervisor.channelOf(supervisor.activeChannelId).cgHtml || null)
            function load() { api.get("/cg/templates", function(ok, d) { if (ok) { htmlPanel.manifest = d; if (!htmlPanel.category && d.templates && d.templates.length) htmlPanel.category = d.templates[0].category } }) }
            function body() {
                var p = {}; var t = htmlPanel.sel; if (!t) return null
                for (var i = 0; i < t.params.length; ++i) { var n = t.params[i].name; var v = htmlPanel.params[n] !== undefined ? htmlPanel.params[n] : (t.params[i]["default"] || ""); if (v !== "") p[n] = v }
                return { template: t.id, params: p, animation: { "in": animIn, out: animOut, duration: animMs }, onAir: deliver, output: outputNumber }
            }
            function selectOutput(n) {
                var all = Object.assign({}, outputDrafts)
                all[String(outputNumber)] = { category: category, selId: selId, params: params, animIn: animIn, animOut: animOut, animMs: animMs, deliver: deliver, lastUrl: lastUrl }
                outputDrafts = all; outputNumber = n
                var d = all[String(n)] || {}; category = d.category || category; selId = d.selId || ""; params = d.params || ({}); animIn = d.animIn || "fade"; animOut = d.animOut || "fade"; animMs = d.animMs || 600; deliver = d.deliver || ""; lastUrl = d.lastUrl || ""
            }
            function deploy() { var b = body(); if (!b) return; api.post("/cg/html/" + supervisor.activeChannelId, b, function(ok, d) { if (ok) { window.toast("Deployed " + htmlPanel.sel.name + (d.onAir ? " (" + d.onAir + " on air)" : ""), "ok"); htmlPanel.lastUrl = d.url } else window.toast("Deploy failed: " + (d && d.error ? d.error : ""), "error") }) }
            function clear() { var n = outputNumber; api.post("/cg/html/" + supervisor.activeChannelId + "/clear", {output:n}, function(ok) { if (ok) { window.toast("CG output " + n + " cleared", "ok"); if (htmlPanel.outputNumber === n) htmlPanel.lastUrl = "" } }) }
            property string lastUrl: ""
            Component.onCompleted: { load() }
            headerRight: [ CButton { text: "Designer \u2197"; small: true; variant: "ghost"; onClicked: Qt.openUrlExternally(api.rawUrl("/cg/designer.html")); ToolTip.text: "Open the standalone CG designer and generator" }, CButton { text: "Output page \u2197"; small: true; variant: "ghost"; onClicked: { Qt.openUrlExternally(api.rawUrl("/cg/output.html?channel=" + supervisor.activeChannelId + "&output=" + htmlPanel.outputNumber + "&debug=1")) } ToolTip.text: "Browser-source page for the selected CG output" } ]
            ColumnLayout {
                anchors.fill: parent; spacing: 8
                Flow { Layout.fillWidth: true; spacing: 4
                    Repeater { model: 8; CButton { text: "CG " + (index + 1); small: true; variant: "ghost"; active: htmlPanel.outputNumber === index + 1; onClicked: htmlPanel.selectOutput(index + 1) } }
                }
                Flow { Layout.fillWidth: true; spacing: 4
                    Repeater { model: htmlPanel.categories; CButton { text: modelData; small: true; variant: "ghost"; active: htmlPanel.category === modelData; onClicked: { htmlPanel.category = modelData } } }
                }
                ListView {
                    Layout.fillWidth: true; Layout.preferredHeight: 150; clip: true
                    model: { var out = []; for (var i = 0; i < htmlPanel.templates.length; ++i) if (htmlPanel.templates[i].category === htmlPanel.category) out.push(htmlPanel.templates[i]); return out }
                    delegate: Rectangle {
                        width: ListView.view.width; height: 26; radius: Theme.r5; color: modelData.id === htmlPanel.selId ? Theme.bg4 : "transparent"; border.color: modelData.id === htmlPanel.selId ? Theme.border1 : "transparent"
                        MouseArea { anchors.fill: parent; onClicked: { htmlPanel.selId = modelData.id; htmlPanel.params = {} } }
                        RowLayout { anchors.fill: parent; anchors.leftMargin: 6; anchors.rightMargin: 6; spacing: 6
                            Text { text: modelData.icon || "\u25AD"; color: Theme.text2; font.pixelSize: Theme.fs11; Layout.preferredWidth: 16 }
                            Text { text: modelData.name; color: Theme.text0; font.pixelSize: Theme.fs12; elide: Text.ElideRight; Layout.fillWidth: true }
                            Text { text: modelData.id; color: Theme.text3; font.pixelSize: Theme.fs10; font.family: Theme.fontMono; elide: Text.ElideRight; Layout.preferredWidth: 120 } }
                    }
                    Text { anchors.centerIn: parent; visible: !htmlPanel.templates.length; color: Theme.text3; font.pixelSize: Theme.fs11; text: htmlPanel.manifest.error || "No template pack under the web root" }
                }
                Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border0 }
                Flickable {
                    Layout.fillWidth: true; Layout.fillHeight: true; clip: true; contentHeight: pcol.implicitHeight
                    ColumnLayout {
                        id: pcol; width: parent.width; spacing: 6
                        Text { visible: !!htmlPanel.sel; Layout.fillWidth: true; wrapMode: Text.WordWrap; text: htmlPanel.sel ? htmlPanel.sel.name + "  \u2014  " + (htmlPanel.sel.description || "") : ""; color: Theme.text1; font.pixelSize: Theme.fs11 }
                        Repeater {
                            model: htmlPanel.sel ? htmlPanel.sel.params : []
                            CField { Layout.fillWidth: true; label: modelData.label || modelData.name; text: htmlPanel.params[modelData.name] !== undefined ? String(htmlPanel.params[modelData.name]) : ""; placeholder: modelData["default"] || ""; mono: modelData.type === "color"
                                onEdited: function(v) { var p = Object.assign({}, htmlPanel.params); p[modelData.name] = v; htmlPanel.params = p } }
                        }
                        GridLayout { visible: !!htmlPanel.sel; Layout.fillWidth: true; columns: 3; columnSpacing: 6; rowSpacing: 6
                            CSelect { label: "Anim in"; Layout.fillWidth: true; model: htmlPanel.manifest.animations || ["none", "fade"]; currentIndex: Math.max(0, (htmlPanel.manifest.animations || ["none", "fade"]).indexOf(htmlPanel.animIn)); onActivated: function(i) { htmlPanel.animIn = model[i] } }
                            CSelect { label: "Anim out"; Layout.fillWidth: true; model: htmlPanel.manifest.animations || ["none", "fade"]; currentIndex: Math.max(0, (htmlPanel.manifest.animations || ["none", "fade"]).indexOf(htmlPanel.animOut)); onActivated: function(i) { htmlPanel.animOut = model[i] } }
                            CField { label: "ms"; Layout.preferredWidth: 70; mono: true; text: String(htmlPanel.animMs); onEdited: function(v) { htmlPanel.animMs = parseInt(v) || 600 } }
                        }
                        CSelect { visible: !!htmlPanel.sel; label: "Deliver"; Layout.fillWidth: true; model: ["output page only (browser source / NDI browser)", "+ native equivalent on air", "+ HTML layer on air (WebEngine build)"]; onActivated: function(i) { htmlPanel.deliver = i === 1 ? "native" : i === 2 ? "html" : "" } }
                        RowLayout { visible: !!htmlPanel.sel; Layout.fillWidth: true; spacing: 6
                            CButton { text: "DEPLOY"; icon2: "\u25B6"; variant: "success"; Layout.fillWidth: true; onClicked: { htmlPanel.deploy() } }
                            CButton { text: "CLEAR"; icon2: "\u25A0"; variant: "danger"; Layout.fillWidth: true; onClicked: { htmlPanel.clear() } }
                            CButton { text: "Preview \u2197"; small: true; variant: "ghost"; onClicked: { var b = htmlPanel.body(); var q = []; for (var k in b.params) q.push(encodeURIComponent(k) + "=" + encodeURIComponent(b.params[k])); if (htmlPanel.animIn !== "none") q.push("animIn=" + htmlPanel.animIn + "&animInDuration=" + htmlPanel.animMs); Qt.openUrlExternally(api.rawUrl("/cg/" + htmlPanel.sel.id + ".html" + (q.length ? "?" + q.join("&") : ""))) } }
                        }
                        Text { Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; visible: htmlPanel.lastUrl.length > 0; text: "deployed: " + htmlPanel.lastUrl; color: Theme.text3; font.pixelSize: Theme.fs10; font.family: Theme.fontMono }
                    }
                }
            }
        }
        CPanel {
            id: capPanel
            Layout.preferredWidth: 340; Layout.fillHeight: true
            title: "Captions & Fact box"
            // --- karaoke (layer 31) ---
            property var kstate: ({})
            property string kMediaId: ""
            property bool kFollow: false
            property string kPosition: "bottom"
            property string kMode: "word"
            // Last Mode / Position the SERVER reported: the refresh adopts them only when they change there (first load,
            // channel switch, another client, a SHOW), so an unsent pick in the combos survives the 5 s refresh.
            property string kServerMode: ""
            property string kServerPosition: ""
            property int kFontSize: 46
            property string kColor: "#ebebf5"
            property string kHighlight: "#ffd60a"
            property string kPast: "#ffffff"
            property string kBg: "#be05050a"
            property real kSync: 0
            readonly property var kPositions: ["bottom", "lower_third", "top", "center"]
            readonly property var kModes: ["word", "line"]
            // --- fact box (layer 32) ---
            property string fTitle: "Fact box"
            property string fLines: "First fact\nSecond fact\nThird fact"
            property string fPosition: "right"
            property string fAccent: "#00d4ff"
            property int fWidth: 520
            property bool fReveal: false
            property real fInterval: 1.2
            property string fFooter: ""
            readonly property var fPositions: ["right", "left", "top-right", "top-left", "bottom-left", "bottom-right", "center"]
            property var presets: []
            property string presetName: ""
            property int presetIndex: 0
            function ch() { return supervisor.activeChannelId }
            function kBody(extra) {
                var b = { follow: kFollow, mode: kMode, position: kPosition, fontSize: kFontSize, color: kColor, highlightColor: kHighlight, pastColor: kPast, bgColor: kBg, syncOffsetSec: kSync, layer: 31, role: "lower_third" }
                if (kMediaId.trim().length) b.mediaId = kMediaId.trim()
                for (var k in (extra || {})) b[k] = extra[k]
                return b
            }
            function kRefresh() {
                api.get("/channels/" + ch() + "/cg/karaoke", function(ok, d) {
                    if (!ok || !d) return
                    capPanel.kstate = d; capPanel.kFollow = !!d.follow
                    var pos = (d.style && d.style.position) ? d.style.position : ""
                    if (d.mode && d.mode !== capPanel.kServerMode) { capPanel.kServerMode = d.mode; capPanel.kMode = d.mode }
                    if (pos && pos !== capPanel.kServerPosition) { capPanel.kServerPosition = pos; capPanel.kPosition = pos }
                })
            }
            function kShow() { api.post("/channels/" + ch() + "/cg/karaoke", kBody({ action: "show" }), function(ok, d) { if (ok) { window.toast(d.shown ? "Captions on: " + d.words + " words from " + d.mediaId : "Follow armed" + (d.warning ? " — " + d.warning : ""), "ok"); capPanel.kRefresh() } else window.toast("Captions: " + (d && d.error ? d.error : "failed"), "error") }) }
            function kHide() { api.post("/channels/" + ch() + "/cg/karaoke", { action: "hide" }, function(ok) { if (ok) { window.toast("Captions off", "ok"); capPanel.kRefresh() } }) }
            function kSetFollow(on) { capPanel.kFollow = on; if (on) kShow(); else api.post("/channels/" + ch() + "/cg/karaoke", { follow: false, action: "hide" }, function(ok) { if (ok) { window.toast("Follow off", "ok"); capPanel.kRefresh() } }) }
            function fSpec() { return { title: fTitle, lines: fLines, position: fPosition, accentColor: fAccent, width: fWidth, reveal: fReveal ? "lines" : "all", revealIntervalSec: fInterval, footer: fFooter } }
            function fSend(action) {
                var b = fSpec(); b.layer = 32; b.action = action
                api.post("/channels/" + ch() + "/cg/factbox", b, function(ok, d) { if (ok) window.toast("Fact box " + action, "ok"); else window.toast("Fact box: " + (d && d.error ? d.error : "failed"), "error") })
            }
            function loadPresets() { api.get("/cg/factbox/presets", function(ok, d) { if (ok && d) capPanel.presets = d.presets || [] }) }
            function savePreset() { api.put("/cg/factbox/presets", { name: presetName.trim().length ? presetName.trim() : fTitle, spec: fSpec() }, function(ok, d) { if (ok) { capPanel.presets = d.presets || []; window.toast("Preset saved", "ok") } }) }
            function loadPreset(i) {
                var p = presets[i]; if (!p) return; var s = p.spec || {}
                fTitle = s.title || ""; fLines = Array.isArray(s.lines) ? s.lines.join("\n") : (s.lines || ""); fPosition = s.position || "right"; fAccent = s.accentColor || "#00d4ff"
                fWidth = s.width || 520; fReveal = s.reveal === "lines"; fInterval = s.revealIntervalSec || 1.2; fFooter = s.footer || ""; presetName = p.name || ""
                window.toast("Loaded preset " + p.name, "ok")
            }
            function deletePreset(i) { var p = presets[i]; if (!p) return; api.put("/cg/factbox/presets", { deleteId: p.id }, function(ok, d) { if (ok) { capPanel.presets = d.presets || []; window.toast("Preset deleted", "ok") } }) }
            Component.onCompleted: { kRefresh(); loadPresets() }
            Connections {
                target: supervisor
                function onUiEvent(type, payload) {
                    if (type === "cgKaraoke" && payload && payload.channelId === supervisor.activeChannelId) capPanel.kRefresh()
                    if (type === "factboxPresets" && payload) capPanel.presets = payload.presets || []
                }
            }
            Connections { target: supervisor; function onActiveChannelChanged() { capPanel.kServerMode = ""; capPanel.kServerPosition = ""; capPanel.kRefresh() } }
            Timer { interval: 5000; running: true; repeat: true; onTriggered: capPanel.kRefresh() }
            Flickable {
                anchors.fill: parent; contentHeight: capCol.implicitHeight; clip: true
                ColumnLayout {
                    id: capCol; width: parent.width; spacing: 8
                    Text { text: "KARAOKE CAPTIONS (layer 31, from transcripts)"; color: Theme.text2; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1 }
                    RowLayout {
                        Layout.fillWidth: true; spacing: 6
                        CBadge { text: capPanel.kstate.hasTranscript ? "transcript ✓" : "no transcript"; tint: capPanel.kstate.hasTranscript ? Theme.ready : Theme.text3 }
                        CBadge { visible: !!capPanel.kstate.shown; text: "ON AIR"; tint: Theme.ready; solid: true }
                        CBadge { visible: !!capPanel.kstate.itemAsks; text: "item asks"; tint: Theme.accent0 }
                        Text { Layout.fillWidth: true; elide: Text.ElideRight; text: capPanel.kstate.onAirTitle || (capPanel.kstate.onAirAssetId || "nothing on air"); color: Theme.text1; font.pixelSize: Theme.fs11 }
                    }
                    CField { Layout.fillWidth: true; label: "Media id (blank = asset on air)"; mono: true; text: capPanel.kMediaId; placeholder: capPanel.kstate.onAirAssetId || ""; onEdited: function(v) { capPanel.kMediaId = v } }
                    CToggle { Layout.fillWidth: true; label: "Follow program"; hint: "re-issue captions whenever the on-air asset changes"; checked: capPanel.kFollow; onToggled: function(v) { capPanel.kSetFollow(v) } }
                    GridLayout {
                        Layout.fillWidth: true; columns: 2; columnSpacing: 8; rowSpacing: 6
                        CSelect { label: "Position"; Layout.fillWidth: true; model: capPanel.kPositions; currentIndex: Math.max(0, capPanel.kPositions.indexOf(capPanel.kPosition)); onActivated: function(i) { capPanel.kPosition = capPanel.kPositions[i] } }
                        CSelect { label: "Mode"; Layout.fillWidth: true; model: capPanel.kModes; currentIndex: Math.max(0, capPanel.kModes.indexOf(capPanel.kMode)); onActivated: function(i) { capPanel.kMode = capPanel.kModes[i] } }
                        CField { label: "Font px (1080p)"; Layout.fillWidth: true; mono: true; text: String(capPanel.kFontSize); onEdited: function(v) { capPanel.kFontSize = parseInt(v) || 46 } }
                        CField { label: "Sync trim (s)"; Layout.fillWidth: true; mono: true; text: String(capPanel.kSync); onEdited: function(v) { capPanel.kSync = parseFloat(v) || 0 } }
                        CField { label: "Text colour"; Layout.fillWidth: true; mono: true; text: capPanel.kColor; onEdited: function(v) { capPanel.kColor = v } }
                        CField { label: "Highlight"; Layout.fillWidth: true; mono: true; text: capPanel.kHighlight; onEdited: function(v) { capPanel.kHighlight = v } }
                        CField { label: "Spoken"; Layout.fillWidth: true; mono: true; text: capPanel.kPast; onEdited: function(v) { capPanel.kPast = v } }
                        CField { label: "Box (#aarrggbb)"; Layout.fillWidth: true; mono: true; text: capPanel.kBg; onEdited: function(v) { capPanel.kBg = v } }
                    }
                    RowLayout {
                        Layout.fillWidth: true; spacing: 6
                        CButton { text: "SHOW"; icon2: "▶"; variant: "success"; Layout.fillWidth: true; onClicked: { capPanel.kShow() } }
                        CButton { text: "HIDE"; icon2: "■"; variant: "danger"; Layout.fillWidth: true; onClicked: { capPanel.kHide() } }
                    }
                    Text { Layout.fillWidth: true; wrapMode: Text.WordWrap; text: "Schedule items can carry \"karaoke\" in cgOnStart: the Supervisor expands it when the item airs."; color: Theme.text3; font.pixelSize: Theme.fs10 }
                    Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border0 }
                    Text { text: "FACT BOX (layer 32)"; color: Theme.text2; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1 }
                    CField { Layout.fillWidth: true; label: "Title"; text: capPanel.fTitle; onEdited: function(v) { capPanel.fTitle = v } }
                    Text { text: "LINES (ONE PER LINE)"; color: Theme.text2; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1 }
                    Rectangle {
                        Layout.fillWidth: true; Layout.preferredHeight: 104; color: Theme.bg4; radius: Theme.r5; border.color: fLinesEdit.activeFocus ? Theme.accent0 : Theme.border1
                        ScrollView {
                            anchors.fill: parent; anchors.margins: 4; clip: true
                            TextArea { id: fLinesEdit; text: capPanel.fLines; color: Theme.text0; font.pixelSize: Theme.fs12; wrapMode: TextEdit.Wrap; background: null; selectByMouse: true; onTextChanged: { if (text !== capPanel.fLines) capPanel.fLines = text } }
                        }
                    }
                    GridLayout {
                        Layout.fillWidth: true; columns: 2; columnSpacing: 8; rowSpacing: 6
                        CSelect { label: "Position"; Layout.fillWidth: true; model: capPanel.fPositions; currentIndex: Math.max(0, capPanel.fPositions.indexOf(capPanel.fPosition)); onActivated: function(i) { capPanel.fPosition = capPanel.fPositions[i] } }
                        CField { label: "Accent"; Layout.fillWidth: true; mono: true; text: capPanel.fAccent; onEdited: function(v) { capPanel.fAccent = v } }
                        CField { label: "Width px (1080p)"; Layout.fillWidth: true; mono: true; text: String(capPanel.fWidth); onEdited: function(v) { capPanel.fWidth = parseInt(v) || 520 } }
                        CField { label: "Reveal every (s)"; Layout.fillWidth: true; mono: true; text: String(capPanel.fInterval); onEdited: function(v) { capPanel.fInterval = parseFloat(v) || 1.2 } }
                    }
                    CToggle { Layout.fillWidth: true; label: "Reveal lines one by one"; checked: capPanel.fReveal; onToggled: function(v) { capPanel.fReveal = v } }
                    CField { Layout.fillWidth: true; label: "Footer / source"; text: capPanel.fFooter; onEdited: function(v) { capPanel.fFooter = v } }
                    RowLayout {
                        Layout.fillWidth: true; spacing: 6
                        CButton { text: "SHOW"; icon2: "▶"; variant: "success"; Layout.fillWidth: true; onClicked: { capPanel.fSend("show") } }
                        CButton { text: "UPDATE"; icon2: "↻"; variant: "info"; Layout.fillWidth: true; onClicked: { capPanel.fSend("update") } }
                        CButton { text: "HIDE"; icon2: "■"; variant: "danger"; Layout.fillWidth: true; onClicked: { capPanel.fSend("hide") } }
                    }
                    Text { text: "PRESETS (" + capPanel.presets.length + ")"; color: Theme.text2; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1 }
                    RowLayout {
                        Layout.fillWidth: true; spacing: 6
                        CSelect { Layout.fillWidth: true; model: capPanel.presets.map(function(p) { return p.name }); currentIndex: Math.min(capPanel.presetIndex, Math.max(0, capPanel.presets.length - 1)); onActivated: function(i) { capPanel.presetIndex = i } }
                        CButton { text: "Load"; small: true; onClicked: { capPanel.loadPreset(capPanel.presetIndex) } }
                        CButton { icon2: "✕"; small: true; variant: "ghost"; onClicked: { capPanel.deletePreset(capPanel.presetIndex) } ToolTip.text: "Delete preset" }
                    }
                    RowLayout {
                        Layout.fillWidth: true; spacing: 6
                        CField { Layout.fillWidth: true; placeholder: "preset name"; text: capPanel.presetName; onEdited: function(v) { capPanel.presetName = v } }
                        CButton { text: "Save"; small: true; variant: "primary"; onClicked: { capPanel.savePreset() } }
                    }
                }
            }
        }
        ColumnLayout {
            Layout.fillWidth: true; Layout.fillHeight: true; spacing: 10
            CPanel {
                Layout.fillWidth: true; Layout.fillHeight: true
                title: "Program monitor"
                padding: 8
                headerRight: [ CBadge { text: view.st.state || "OFFLINE"; tint: Theme.stateColor(view.st.state); solid: view.st.state === "ON_AIR" } ]
                Rectangle {
                    anchors.fill: parent; color: "#000"; radius: Theme.r3; clip: true
                    ProgramImage { anchors.fill: parent; channelId: supervisor.activeChannelId }
                }
            }
            CPanel {
                Layout.fillWidth: true; Layout.preferredHeight: 220; padding: 0
                title: "Layers on " + supervisor.activeChannelId
                ListView {
                    anchors.fill: parent; clip: true; model: view.st.layers || []
                    delegate: Rectangle {
                        width: ListView.view.width; height: 32; color: index % 2 ? Theme.bg2 : Theme.bg3
                        RowLayout { anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 8; spacing: 10
                            Text { text: modelData.id; color: Theme.text2; font.family: Theme.fontMono; font.pixelSize: Theme.fs11; Layout.preferredWidth: 26 }
                            Text { text: modelData.role || ""; color: Theme.text0; font.pixelSize: Theme.fs12; font.bold: true; Layout.preferredWidth: 100 }
                            Text { text: modelData.source ? (modelData.source.type + (modelData.source.kind ? "/" + modelData.source.kind : "") + (modelData.source.title ? "  “" + modelData.source.title + "”" : modelData.source.name ? "  " + modelData.source.name : "")) : ""; color: Theme.text1; font.pixelSize: Theme.fs11; elide: Text.ElideRight; Layout.fillWidth: true }
                            Text { visible: modelData.sourceStatus && modelData.sourceStatus.connected !== undefined; text: modelData.sourceStatus && modelData.sourceStatus.connected ? "● live" : "○ waiting"; color: modelData.sourceStatus && modelData.sourceStatus.connected ? Theme.ready : Theme.standby; font.pixelSize: Theme.fs10 }
                            CBadge { text: modelData.state; tint: modelData.state === "shown" ? Theme.ready : modelData.state === "hidden" ? Theme.text3 : Theme.cued }
                            CButton { text: modelData.state === "hidden" ? "Show" : "Hide"; small: true; onClicked: { supervisor.cg({ action: modelData.state === "hidden" ? "show" : "hide", layer: modelData.id, transition: { type: "fade", durationSec: 0.3 } }) } }
                            CButton { icon2: "✕"; small: true; variant: "ghost"; onClicked: { supervisor.cg({ action: "remove", layer: modelData.id }) } } }
                    }
                    Text { anchors.centerIn: parent; visible: !(view.st.layers && view.st.layers.length); text: "No layers on air"; color: Theme.text3; font.pixelSize: Theme.fs11 }
                }
            }
        }
    }
}
