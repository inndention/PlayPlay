import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".."
import "../ui"

/**
 * Render: the render farm (docs/render-nodes.md). Nodes (spot-node / PlayPlay peers / StarAgent segment
 * workers) with health and queue, the dispatch config, a new-job form (spot / transcode / split) and the job
 * list with per-segment progress. Talks to /api/v1/render/* and follows the "render" uiEvent ({jobs, counts}).
 */
Item {
    id: view
    property var status: ({})
    property var cfg: ({})
    property var nodes: []
    property var templates: []
    property var jobs: []
    property var counts: ({})
    property var edit: null                 // node being edited / added (a copy), null = form hidden
    property bool editIsNew: false
    property string jobKind: "spot"
    property var workersSel: ["local"]
    readonly property bool active: { for (var i = 0; i < jobs.length; ++i) if (jobs[i].state === "running" || jobs[i].state === "queued") return true; return false }

    component SecLabel: Text { color: Theme.text2; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1 }
    component PassField: ColumnLayout {
        id: pf
        property string label: ""
        property alias text: pfInput.text
        property alias placeholder: pfInput.placeholderText
        spacing: 4
        Text { visible: pf.label.length > 0; text: pf.label.toUpperCase(); color: Theme.text2; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1 }
        TextField {
            id: pfInput
            Layout.fillWidth: true; implicitHeight: 30
            echoMode: TextInput.Password
            color: Theme.text0; placeholderTextColor: Theme.text3; font.pixelSize: Theme.fs12
            leftPadding: 8; rightPadding: 8
            background: Rectangle { color: Theme.bg4; radius: Theme.r5; border.color: pfInput.activeFocus ? Theme.accent0 : Theme.border1 }
        }
    }
    component AreaField: ColumnLayout {
        id: af
        property string label: ""
        property alias text: afInput.text
        property alias placeholder: afInput.placeholderText
        property int areaHeight: 70
        property bool mono: false
        spacing: 4
        Text { visible: af.label.length > 0; text: af.label.toUpperCase(); color: Theme.text2; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1 }
        Rectangle {
            Layout.fillWidth: true; Layout.preferredHeight: af.areaHeight
            color: Theme.bg4; radius: Theme.r5; border.color: afInput.activeFocus ? Theme.accent0 : Theme.border1
            ScrollView {
                anchors.fill: parent; anchors.margins: 4; clip: true
                TextArea { id: afInput; color: Theme.text0; placeholderTextColor: Theme.text3; font.pixelSize: Theme.fs12; font.family: af.mono ? Theme.fontMono : Theme.fontSans; wrapMode: TextEdit.Wrap; background: null; selectByMouse: true }
            }
        }
    }
    component Bar: Rectangle {
        property real pct: 0
        property color tint: Theme.accent0
        implicitHeight: 6; radius: 2; color: Theme.bg4
        Rectangle { height: parent.height; radius: 2; width: parent.width * Math.max(0, Math.min(1, parent.pct / 100)); color: parent.tint }
    }

    function stColor(s) {
        switch (String(s || "")) {
        case "done": return Theme.ready
        case "failed": return Theme.danger
        case "running": return Theme.accent0
        case "queued": return Theme.standby
        case "cancelled": return Theme.text3
        default: return Theme.text2
        }
    }
    function kindColor(k) { return k === "spotnode" ? Theme.accent0 : k === "playplay" ? Theme.accent2 : k === "staragent" ? Theme.standby : Theme.text1 }
    function health(n) { return n.health || n }
    function queueOf(n) { var h = health(n); return h.queue || n.queue || {} }
    function sortJobs(l) { return l.slice().sort(function(a, b) { return (b.createdMs || 0) - (a.createdMs || 0) }) }
    function ago(ms) { if (!ms) return ""; var s = Math.max(0, Math.round((Date.now() - ms) / 1000)); return s < 60 ? s + " s ago" : s < 3600 ? Math.round(s / 60) + " min ago" : (s / 3600).toFixed(1) + " h ago" }
    function idx(list, v, def) { var i = list.indexOf(v); return i >= 0 ? i : (def || 0) }

    function reload() {
        api.get("/render/status", function(ok, d) { if (ok) { view.status = d; if (d.counts) view.counts = d.counts } })
        api.get("/render/config", function(ok, d) { if (ok) view.cfg = d.config || d })
        reloadNodes()
        reloadJobs()
        api.get("/render/templates", function(ok, d) { if (ok) view.templates = d.templates || [] })
    }
    function reloadNodes() { api.get("/render/nodes", function(ok, d) { if (ok) view.nodes = Array.isArray(d) ? d : (d.nodes || []) }) }
    function reloadJobs() { api.get("/render/jobs?limit=100", function(ok, d) { if (ok) { view.jobs = view.sortJobs(d.jobs || []); if (d.counts) view.counts = d.counts } }) }
    function mergeJobs(list) {
        var out = view.jobs.slice()
        for (var i = 0; i < list.length; ++i) {
            var j = list[i], found = false
            for (var k = 0; k < out.length; ++k) if (out[k].id === j.id) { out[k] = j; found = true; break }
            if (!found) out.push(j)
        }
        view.jobs = view.sortJobs(out)
    }
    function nodeIds() { var r = []; for (var i = 0; i < nodes.length; ++i) if (nodes[i].kind !== "local") r.push(nodes[i].id); return r }
    function startEdit(n) {
        view.editIsNew = !n
        view.edit = n ? { id: n.id, name: n.name || "", kind: n.kind || "spotnode", url: n.url || "", hasToken: !!n.hasToken, enabled: n.enabled !== false, render: !!n.render, primary: !!n.primary, maxBusy: n.maxBusy || 1, notes: n.notes || "" }
                      : { id: "", name: "", kind: "spotnode", url: "", hasToken: false, enabled: true, render: false, primary: false, maxBusy: 1, notes: "" }
        nfId.text = view.edit.id; nfName.text = view.edit.name; nfUrl.text = view.edit.url; nfToken.text = ""
        nfMax.text = String(view.edit.maxBusy); nfNotes.text = view.edit.notes
        nfKind.currentIndex = idx(["spotnode", "playplay", "staragent"], view.edit.kind)
        nfEnabled.checked = view.edit.enabled; nfRender.checked = view.edit.render; nfPrimary.checked = view.edit.primary
    }
    function saveNode() {
        var b = { id: nfId.text.trim(), name: nfName.text.trim(), kind: nfKind.currentText, url: nfUrl.text.trim(), enabled: nfEnabled.checked,
                  render: nfRender.checked, primary: nfPrimary.checked, maxBusy: parseInt(nfMax.text) || 1, notes: nfNotes.text }
        if (nfToken.text.length) b.token = nfToken.text
        var cb = function(ok) { if (ok) { window.toast("Node saved", "ok"); view.edit = null; view.reloadNodes() } }
        if (view.editIsNew) api.post("/render/nodes", b, cb)
        else api.put("/render/nodes/" + encodeURIComponent(view.edit.id), b, cb)
    }
    function saveCfg() {
        api.put("/render/config", { studioMode: cfStudio.currentText, renderMode: cfRender.currentText, waitSec: parseInt(cfWait.text) || 0,
                                    maxLocal: parseInt(cfMaxLocal.text) || 1, meshDispatch: cfMesh.currentText, allowCustomArgs: cfCustom.checked },
                function(ok, d) { if (ok) { view.cfg = d.config || d; window.toast("Render config saved", "ok") } })
    }
    function toggleWorker(w) { var l = view.workersSel.slice(); var i = l.indexOf(w); if (i >= 0) l.splice(i, 1); else l.push(w); view.workersSel = l }
    function submitJob() {
        var b = { kind: view.jobKind }
        var ingest = {}
        if (jIngType.currentText !== "(none)") ingest.type = jIngType.currentText
        if (jIngTitle.text.trim().length) ingest.title = jIngTitle.text.trim()
        if (view.jobKind === "spot") {
            b.type = jType.currentText
            var t = jTemplate.currentIndex > 0 ? view.templates[jTemplate.currentIndex - 1] : null
            if (t) b.template = t.id
            b.brief = jBrief.text
            b.lengthSec = parseInt(jLen.text) || 15
            b.aspect = jAspect.currentText
            b.mode = jMode.currentText
            if (jVoice.text.trim().length) b.voice = jVoice.text.trim()
            b.target = jTarget.currentText
        } else if (view.jobKind === "transcode") {
            b.input = jInput.text.trim()
            b.preset = jPreset.currentText
        } else {
            b.input = jInput.text.trim()
            b.segments = parseInt(jSegs.text) || 4
            b.workers = view.workersSel.length ? view.workersSel : ["local"]
            b.preset = jPreset.currentText
            b.copy = jCopy.checked
        }
        if (Object.keys(ingest).length) b.ingest = ingest
        api.post("/render/jobs", b, function(ok, d) { if (ok) { window.toast("Render job queued" + (d && d.id ? " (" + d.id + ")" : ""), "ok"); view.reloadJobs() } })
    }

    Component.onCompleted: { reload() }
    Connections {
        target: supervisor
        function onUiEvent(t, p) {
            if (t !== "render") return
            if (p.jobs) view.mergeJobs(p.jobs)
            if (p.counts) view.counts = p.counts
        }
    }
    Timer { interval: 5000; repeat: true; running: view.active; onTriggered: { view.reloadJobs() } }

    Flickable {
        anchors.fill: parent; clip: true
        contentWidth: width; contentHeight: page.implicitHeight + 12
        ScrollBar.vertical: ScrollBar {}
        ColumnLayout {
            id: page
            width: parent.width; spacing: 10

            // ---------------------------------------------------------------- status strip
            CPanel {
                Layout.fillWidth: true; implicitHeight: 34 + stripRow.implicitHeight + 24
                title: "Render farm"
                headerRight: [
                    CButton { text: "Probe all"; small: true; onClicked: { api.post("/render/probe", {}, function(ok) { if (ok) { window.toast("Probing render nodes", "ok"); view.reloadNodes() } }) } },
                    CButton { text: "\u21BB"; small: true; variant: "ghost"; onClicked: { view.reload() } }
                ]
                RowLayout {
                    id: stripRow; anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; spacing: 14
                    Text { color: Theme.text1; font.pixelSize: Theme.fs12
                        text: "studio " + (view.cfg.studioMode || "\u2014") + " \u00B7 render " + (view.cfg.renderMode || "\u2014") + " \u00B7 mesh " + (view.cfg.meshDispatch || "\u2014") + " \u00B7 max local " + (view.cfg.maxLocal || 2) }
                    Text { color: Theme.text2; font.pixelSize: Theme.fs12
                        text: { var r = 0; for (var i = 0; i < view.nodes.length; ++i) { var h = view.health(view.nodes[i]); if (h.ready) r++ } return view.nodes.length + " node(s), " + r + " ready" } }
                    Item { Layout.fillWidth: true }
                    Repeater {
                        model: Object.keys(view.counts || {})
                        CBadge { text: modelData + " " + view.counts[modelData]; tint: view.stColor(modelData) }
                    }
                }
            }

            // ---------------------------------------------------------------- nodes + config
            RowLayout {
                Layout.fillWidth: true; spacing: 10
                CPanel {
                    Layout.fillWidth: true; Layout.alignment: Qt.AlignTop
                    implicitHeight: 34 + nodesCol.implicitHeight + 24
                    title: "Nodes (" + view.nodes.length + ")"
                    headerRight: [ CButton { text: "+ node"; small: true; variant: "primary"; onClicked: { view.startEdit(null) } } ]
                    ColumnLayout {
                        id: nodesCol; anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; spacing: 4
                        RowLayout { Layout.fillWidth: true; spacing: 8
                            SecLabel { text: "KIND"; Layout.preferredWidth: 70 }
                            SecLabel { text: "NAME / URL"; Layout.fillWidth: true }
                            SecLabel { text: "FLAGS"; Layout.preferredWidth: 150 }
                            SecLabel { text: "HEALTH"; Layout.preferredWidth: 200 }
                            SecLabel { text: "QUEUE"; Layout.preferredWidth: 80 }
                            Item { Layout.preferredWidth: 150 } }
                        Repeater {
                            model: view.nodes
                            Rectangle {
                                Layout.fillWidth: true; implicitHeight: 40; radius: Theme.r3; color: index % 2 ? Theme.bg2 : Theme.bg3
                                property var h: view.health(modelData)
                                property var q: view.queueOf(modelData)
                                RowLayout {
                                    anchors.fill: parent; anchors.leftMargin: 8; anchors.rightMargin: 6; spacing: 8
                                    CBadge { text: modelData.kind || "?"; tint: view.kindColor(modelData.kind); Layout.preferredWidth: 70 }
                                    ColumnLayout { Layout.fillWidth: true; spacing: 1
                                        Text { text: (modelData.name || modelData.id) + (modelData.discovered ? "  (discovered)" : ""); color: Theme.text0; font.pixelSize: Theme.fs12; font.bold: true; elide: Text.ElideRight; Layout.fillWidth: true }
                                        Text { text: (modelData.url || "") + (modelData.hasToken ? "  \u00B7 token set" : ""); color: Theme.text3; font.pixelSize: Theme.fs10; font.family: Theme.fontMono; elide: Text.ElideMiddle; Layout.fillWidth: true } }
                                    Row { Layout.preferredWidth: 150; spacing: 4
                                        CBadge { text: modelData.enabled === false ? "off" : "on"; tint: modelData.enabled === false ? Theme.text3 : Theme.ready }
                                        CBadge { visible: !!modelData.render; text: "render"; tint: Theme.accent0 }
                                        CBadge { visible: !!modelData.primary; text: "primary"; tint: Theme.accent2 } }
                                    RowLayout { Layout.preferredWidth: 200; spacing: 6
                                        Rectangle { width: 8; height: 8; radius: 4; color: h.ready ? Theme.ready : h.ok ? Theme.standby : Theme.danger }
                                        Text { Layout.fillWidth: true; text: h.ready ? "ready" : h.ok ? "ok, not ready" : "down"; color: Theme.text1; font.pixelSize: Theme.fs11 }
                                        Text { Layout.fillWidth: true; visible: !!h.reason; text: h.reason || ""; color: Theme.text3; font.pixelSize: Theme.fs10; elide: Text.ElideRight
                                            ToolTip.visible: reasonMa.containsMouse; ToolTip.text: h.reason || ""
                                            MouseArea { id: reasonMa; anchors.fill: parent; hoverEnabled: true } } }
                                    Text { Layout.preferredWidth: 80; text: (q.running || 0) + "/" + (q.queued || 0) + "/" + (q.max !== undefined ? q.max : (modelData.maxBusy || 1)); color: Theme.text1; font.pixelSize: Theme.fs11; font.family: Theme.fontMono
                                        ToolTip.visible: qMa.containsMouse; ToolTip.text: "running / queued / max"
                                        MouseArea { id: qMa; anchors.fill: parent; hoverEnabled: true } }
                                    Row { Layout.preferredWidth: 150; spacing: 4
                                        CButton { text: "probe"; small: true; variant: "ghost"; onClicked: { api.post("/render/nodes/" + encodeURIComponent(modelData.id) + "/probe", {}, function(ok) { if (ok) view.reloadNodes() }) } }
                                        CButton { visible: modelData.kind !== "local" && !modelData.discovered; text: "\u270E"; small: true; variant: "ghost"; onClicked: { view.startEdit(modelData) } ToolTip.text: "Edit" }
                                        CButton { visible: modelData.kind !== "local" && !modelData.discovered; text: "\u2715"; small: true; variant: "ghost"; onClicked: { api.del("/render/nodes/" + encodeURIComponent(modelData.id), function(ok) { if (ok) view.reloadNodes() }) } ToolTip.text: "Remove" } }
                                }
                            }
                        }
                        Text { visible: !view.nodes.length; text: "No render nodes yet"; color: Theme.text3; font.pixelSize: Theme.fs11 }

                        // ---- inline node form
                        Rectangle { visible: view.edit !== null; Layout.fillWidth: true; Layout.topMargin: 6; height: 1; color: Theme.border0 }
                        SecLabel { visible: view.edit !== null; text: view.editIsNew ? "ADD NODE" : "EDIT NODE " + (view.edit ? view.edit.id : "") }
                        GridLayout {
                            visible: view.edit !== null
                            Layout.fillWidth: true; columns: 4; columnSpacing: 8; rowSpacing: 6
                            CField { id: nfId; label: "Id"; Layout.fillWidth: true; mono: true; readOnly: !view.editIsNew; placeholder: "node-1" }
                            CField { id: nfName; label: "Name"; Layout.fillWidth: true }
                            CSelect { id: nfKind; label: "Kind"; Layout.fillWidth: true; model: ["spotnode", "playplay", "staragent"] }
                            CField { id: nfMax; label: "Max busy"; Layout.fillWidth: true; mono: true; text: "1" }
                            CField { id: nfUrl; label: "URL"; Layout.fillWidth: true; Layout.columnSpan: 2; mono: true; placeholder: nfKind.currentText === "spotnode" ? "http://host:8775" : nfKind.currentText === "staragent" ? "http://host:8780" : "http://host:7700" }
                            PassField { id: nfToken; label: "Token"; Layout.fillWidth: true; Layout.columnSpan: 2; placeholder: view.edit && view.edit.hasToken ? "(set) \u2014 blank keeps it" : "(none)" }
                            CField { id: nfNotes; label: "Notes"; Layout.fillWidth: true; Layout.columnSpan: 4 }
                        }
                        RowLayout {
                            visible: view.edit !== null
                            Layout.fillWidth: true; spacing: 10
                            CToggle { id: nfEnabled; label: "Enabled"; implicitWidth: 100 }
                            CToggle { id: nfRender; label: "Render (opt-in)"; implicitWidth: 140 }
                            CToggle { id: nfPrimary; label: "Primary"; implicitWidth: 100 }
                            Item { Layout.fillWidth: true }
                            CButton { text: "Cancel"; small: true; variant: "ghost"; onClicked: { view.edit = null } }
                            CButton { text: "Save node"; small: true; variant: "primary"; enabled: nfId.text.trim().length > 0 && nfUrl.text.trim().length > 0; onClicked: { view.saveNode() } }
                        }
                    }
                }
                CPanel {
                    Layout.preferredWidth: 340; Layout.maximumWidth: 360; Layout.alignment: Qt.AlignTop
                    implicitHeight: 34 + cfgCol.implicitHeight + 24
                    title: "Dispatch config"
                    ColumnLayout {
                        id: cfgCol; anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; spacing: 6
                        CSelect { id: cfStudio; label: "Studio mode (spot-node)"; Layout.fillWidth: true; model: ["off", "prefer", "only"]; currentIndex: view.idx(model, view.cfg.studioMode, 1) }
                        CSelect { id: cfRender; label: "Render mode"; Layout.fillWidth: true; model: ["offline", "hybrid"]; currentIndex: view.idx(model, view.cfg.renderMode, 0) }
                        CSelect { id: cfMesh; label: "Mesh dispatch"; Layout.fillWidth: true; model: ["auto", "local", "off"]; currentIndex: view.idx(model, view.cfg.meshDispatch, 0) }
                        RowLayout { Layout.fillWidth: true; spacing: 6
                            CField { id: cfWait; label: "Wait (s)"; Layout.fillWidth: true; mono: true; text: String(view.cfg.waitSec !== undefined ? view.cfg.waitSec : 600) }
                            CField { id: cfMaxLocal; label: "Max local"; Layout.fillWidth: true; mono: true; text: String(view.cfg.maxLocal !== undefined ? view.cfg.maxLocal : 2) } }
                        CToggle { id: cfCustom; Layout.fillWidth: true; label: "Allow custom ffmpeg args"; hint: "transcode preset \"custom\""; checked: !!view.cfg.allowCustomArgs }
                        CButton { text: "Save config"; variant: "primary"; Layout.alignment: Qt.AlignRight; onClicked: { view.saveCfg() } }
                    }
                }
            }

            // ---------------------------------------------------------------- new job + jobs
            RowLayout {
                Layout.fillWidth: true; spacing: 10
                CPanel {
                    Layout.preferredWidth: 440; Layout.maximumWidth: 480; Layout.alignment: Qt.AlignTop
                    implicitHeight: 34 + jobCol.implicitHeight + 24
                    title: "New job"
                    ColumnLayout {
                        id: jobCol; anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; spacing: 6
                        RowLayout { Layout.fillWidth: true; spacing: 4
                            Repeater { model: ["spot", "transcode", "split"]
                                CButton { text: modelData; small: true; active: view.jobKind === modelData; onClicked: { view.jobKind = modelData } } } }
                        // spot
                        GridLayout {
                            visible: view.jobKind === "spot"
                            Layout.fillWidth: true; columns: 2; columnSpacing: 8; rowSpacing: 6
                            CSelect { id: jType; label: "Type"; Layout.fillWidth: true; model: ["motion_spot", "template_render"] }
                            CSelect { id: jTemplate; label: "Template"; Layout.fillWidth: true
                                model: [{ id: "", title: "(none)" }].concat(view.templates.map(function(t) { return { id: t.id, title: (t.title || t.id) + (t.category ? "  \u00B7 " + t.category : "") } }))
                                textRole: "title" }
                            Text { Layout.columnSpan: 2; Layout.fillWidth: true; visible: jTemplate.currentIndex > 0; wrapMode: Text.WordWrap; color: Theme.text3; font.pixelSize: Theme.fs10
                                text: { var t = view.templates[jTemplate.currentIndex - 1]; if (!t) return ""; return (t.description || "") + (t.aspects ? "  aspects: " + t.aspects.join(", ") : "") + (t.lengths ? "  lengths: " + t.lengths.join(", ") : "") } }
                            AreaField { id: jBrief; label: "Brief"; Layout.columnSpan: 2; Layout.fillWidth: true; placeholder: "What the spot says / sells" }
                            CField { id: jLen; label: "Length (s)"; Layout.fillWidth: true; mono: true; text: "15" }
                            CSelect { id: jAspect; label: "Aspect"; Layout.fillWidth: true; model: ["16:9", "9:16", "1:1", "4:5"] }
                            CSelect { id: jMode; label: "Mode"; Layout.fillWidth: true; model: ["offline", "hybrid"] }
                            CField { id: jVoice; label: "Voice"; Layout.fillWidth: true; placeholder: "default" }
                            CSelect { id: jTarget; label: "Target"; Layout.columnSpan: 2; Layout.fillWidth: true; model: ["auto"].concat(view.nodeIds()) }
                        }
                        // transcode / split
                        GridLayout {
                            visible: view.jobKind !== "spot"
                            Layout.fillWidth: true; columns: 2; columnSpacing: 8; rowSpacing: 6
                            CField { id: jInput; label: "Input (path or media id)"; Layout.columnSpan: 2; Layout.fillWidth: true; mono: true; placeholder: "D:\\Media\\show.mxf" }
                            CSelect { id: jPreset; label: "Preset"; Layout.fillWidth: true
                                model: view.cfg.allowCustomArgs ? ["h264_1080p30", "h264_720p30", "proxy_540p", "audio_wav", "loudnorm", "custom"] : ["h264_1080p30", "h264_720p30", "proxy_540p", "audio_wav", "loudnorm"] }
                            CField { id: jSegs; visible: view.jobKind === "split"; label: "Segments"; Layout.fillWidth: true; mono: true; text: "4" }
                            CToggle { id: jCopy; visible: view.jobKind === "split"; label: "Stream copy"; Layout.columnSpan: 2; implicitWidth: 140 }
                            SecLabel { visible: view.jobKind === "split"; text: "WORKERS"; Layout.columnSpan: 2 }
                            Flow { visible: view.jobKind === "split"; Layout.columnSpan: 2; Layout.fillWidth: true; spacing: 4
                                Repeater { model: ["local"].concat(view.nodeIds())
                                    CButton { text: modelData; small: true; active: view.workersSel.indexOf(modelData) >= 0; onClicked: { view.toggleWorker(modelData) } } } }
                        }
                        RowLayout { Layout.fillWidth: true; spacing: 8
                            CSelect { id: jIngType; label: "Ingest as"; Layout.preferredWidth: 130; model: ["(none)", "ad", "promo", "psa", "program", "station_id", "bumper", "filler"] }
                            CField { id: jIngTitle; label: "Ingest title"; Layout.fillWidth: true } }
                        CButton { text: "Queue job"; icon2: "\u25B6"; variant: "primary"; Layout.alignment: Qt.AlignRight
                            enabled: view.jobKind === "spot" ? (jBrief.text.trim().length > 0 || jTemplate.currentIndex > 0) : jInput.text.trim().length > 0
                            onClicked: { view.submitJob() } }
                    }
                }
                CPanel {
                    Layout.fillWidth: true; Layout.alignment: Qt.AlignTop; Layout.preferredHeight: 620; padding: 0
                    title: "Jobs (" + view.jobs.length + ")"
                    headerRight: [
                        CButton { text: "Clear finished"; small: true; variant: "ghost"; onClicked: { api.post("/render/jobs/clear", {}, function(ok) { if (ok) view.reloadJobs() }) } },
                        CButton { text: "\u21BB"; small: true; variant: "ghost"; onClicked: { view.reloadJobs() } }
                    ]
                    ListView {
                        id: jobList
                        anchors.fill: parent; clip: true; model: view.jobs; spacing: 1
                        ScrollBar.vertical: ScrollBar {}
                        delegate: Rectangle {
                            width: jobList.width; height: jc.implicitHeight + 12; color: index % 2 ? Theme.bg2 : Theme.bg3
                            property var j: modelData
                            ColumnLayout {
                                id: jc; anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 6; anchors.leftMargin: 12; spacing: 3
                                RowLayout { Layout.fillWidth: true; spacing: 8
                                    CBadge { text: j.state || "?"; tint: view.stColor(j.state) }
                                    CBadge { text: j.kind || ""; tint: j.kind === "spot" ? Theme.clsAd : j.kind === "split" ? Theme.accent2 : Theme.clsPreroll }
                                    Text { text: j.id; color: Theme.text2; font.pixelSize: Theme.fs10; font.family: Theme.fontMono }
                                    Text { Layout.fillWidth: true; text: (j.step || "") + (j.nodeName || j.node ? "  \u00B7 " + (j.nodeName || j.node) : ""); color: Theme.text0; font.pixelSize: Theme.fs11; elide: Text.ElideRight }
                                    Text { text: view.ago(j.createdMs); color: Theme.text3; font.pixelSize: Theme.fs10 }
                                    CButton { visible: j.state === "running" || j.state === "queued"; text: "cancel"; small: true; variant: "warn"; onClicked: { api.post("/render/jobs/" + encodeURIComponent(j.id) + "/cancel", {}, function(ok) { if (ok) view.reloadJobs() }) } }
                                    CButton { text: "\u2715"; small: true; variant: "ghost"; onClicked: { api.del("/render/jobs/" + encodeURIComponent(j.id), function(ok) { if (ok) view.reloadJobs() }) } ToolTip.text: "Delete" }
                                }
                                RowLayout { Layout.fillWidth: true; spacing: 8
                                    Bar { Layout.fillWidth: true; pct: j.state === "done" ? 100 : (j.pct || 0); tint: view.stColor(j.state) }
                                    Text { text: Math.round(j.state === "done" ? 100 : (j.pct || 0)) + "%"; color: Theme.text2; font.pixelSize: Theme.fs10; font.family: Theme.fontMono; Layout.preferredWidth: 36 } }
                                Text { Layout.fillWidth: true; visible: !!(j.input || j.output); text: (j.input ? "in " + j.input : "") + (j.output ? "   out " + j.output : ""); color: Theme.text3; font.pixelSize: Theme.fs10; font.family: Theme.fontMono; elide: Text.ElideMiddle }
                                RowLayout { Layout.fillWidth: true; spacing: 8; visible: !!(j.mediaId || j.costUsd)
                                    Text { visible: !!j.mediaId; text: "media " + (j.mediaId || ""); color: Theme.ready; font.pixelSize: Theme.fs10; font.family: Theme.fontMono }
                                    CButton { visible: !!j.mediaId; text: "open"; small: true; variant: "ghost"; onClicked: { window.openMedia(j.mediaId, "meta") } }
                                    Text { visible: !!j.costUsd; text: "$" + Number(j.costUsd || 0).toFixed(2); color: Theme.text2; font.pixelSize: Theme.fs10 } }
                                Text { Layout.fillWidth: true; visible: !!j.error; text: j.error || ""; color: Theme.danger; font.pixelSize: Theme.fs10; wrapMode: Text.WordWrap }
                                Repeater {
                                    model: j.segments || []
                                    RowLayout { Layout.fillWidth: true; Layout.leftMargin: 16; spacing: 6
                                        Text { text: "#" + modelData.index; color: Theme.text2; font.pixelSize: Theme.fs10; font.family: Theme.fontMono; Layout.preferredWidth: 26 }
                                        Text { text: Theme.fmtClock(modelData.startSec) + " +" + Math.round(modelData.durSec || 0) + "s"; color: Theme.text3; font.pixelSize: Theme.fs10; font.family: Theme.fontMono; Layout.preferredWidth: 90 }
                                        Text { text: modelData.worker || ""; color: Theme.text1; font.pixelSize: Theme.fs10; Layout.preferredWidth: 80; elide: Text.ElideRight }
                                        Bar { Layout.fillWidth: true; pct: modelData.state === "done" ? 100 : (modelData.pct || 0); tint: view.stColor(modelData.state) }
                                        CBadge { text: modelData.state || ""; tint: view.stColor(modelData.state); ToolTip.visible: segMa.containsMouse && !!modelData.error; ToolTip.text: modelData.error || ""
                                            MouseArea { id: segMa; anchors.fill: parent; hoverEnabled: true } } }
                                }
                            }
                        }
                        Text { anchors.centerIn: parent; visible: !view.jobs.length; text: "No render jobs"; color: Theme.text3; font.pixelSize: Theme.fs11 }
                    }
                }
            }
        }
    }
}
