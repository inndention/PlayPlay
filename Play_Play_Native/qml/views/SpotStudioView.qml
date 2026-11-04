import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".."
import "../ui"

/**
 * Spot studio (docs/spot-studio.md): guided spot creation. Brief → concepts + timed script + questions →
 * edit scenes / end card / instruct the writer → generate (local SpotGenerator or the studio spot-node) →
 * review (submit / approve / changes / reject) → publish into ad rotation (+ optional social draft).
 * Talks to /api/v1/spotstudio/* and /api/v1/spots/spot/{mediaId}/revise.
 */
Item {
    id: view
    property var options: ({})
    property var marketInfo: ({ markets: [], effectiveDefault: "" })      // GET /spots/markets: Martinsville / Reidsville / Danville (+ operator-defined)
    property var projects: []
    property string selId: ""
    property var sel: null
    property var pubChannels: []
    readonly property var kinds: options.kinds || ["ad", "promo", "psa", "announcement"]
    readonly property var lengths: (options.lengths || [6, 15, 30, 60]).map(function(x) { return String(x) })
    readonly property var aspects: options.aspects || ["16:9", "9:16", "1:1"]
    readonly property bool busy: !!sel && (sel.status === "rendering" || (sel.render && (sel.render.state === "running" || sel.render.state === "queued")))

    component SecLabel: Text { color: Theme.text2; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1 }
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

    function statusColor(s) {
        switch (String(s || "")) {
        case "draft": return Theme.text1
        case "rendering": return Theme.accent0
        case "rendered": return Theme.cued
        case "in_review": return Theme.standby
        case "changes_requested": return Theme.warn
        case "approved": return Theme.ready
        case "rejected": case "failed": return Theme.danger
        case "published": return Theme.accent2
        default: return Theme.text2
        }
    }
    function lbl(x) { return x && typeof x === "object" ? String(x.title || x.name || x.label || x.id || "") : String(x === undefined || x === null ? "" : x) }
    function val(x) { return x && typeof x === "object" ? String(x.id || x.name || "") : String(x === undefined || x === null ? "" : x) }
    function withNone(list) { return [{ id: "", title: "(default)" }].concat((list || []).map(function(x) { return { id: view.val(x), title: view.lbl(x) } })) }
    function idxOf(list, v) { for (var i = 0; i < list.length; ++i) if (list[i].id === v || list[i] === v) return i; return 0 }
    function when(ms) { return ms ? new Date(ms).toLocaleString(Qt.locale(), "MM-dd HH:mm") : "" }
    function channelIds() { return supervisor.channels.map(function(c) { return c.profile.id }) }
    function today(offsetDays) { var d = new Date(); d.setDate(d.getDate() + (offsetDays || 0)); return Qt.formatDate(d, "yyyy-MM-dd") }
    function engineOk(e) { var en = (options.engines || {})[e]; return en ? !!en.ok : true }
    function engineNote(e) { var en = (options.engines || {})[e]; if (!en) return ""; return (en.ok ? "ready" : "not ready") + (en.node ? " · " + en.node : "") + (en.reason ? " · " + en.reason : "") }

    // A project's area market: "" = the station default, "none" = generic, else a market key. It localizes the voice (place-name respellings), the strap and the script writer.
    function marketModel() {
        var a = view.marketInfo.markets || [], d = null
        for (var i = 0; i < a.length; ++i) if (a[i].key === view.marketInfo.effectiveDefault) d = a[i]
        return [{ id: "", title: "Station default (" + (d ? d.label : "generic") + ")" }, { id: "none", title: "Generic (no market)" }].concat(a.map(function(m) { return { id: m.key, title: m.label } }))
    }
    function marketIndex(k) { var m = view.marketModel(); for (var i = 0; i < m.length; ++i) if (m[i].id === (k || "")) return i; return 0 }
    function marketLabel(k) { if (!k || k === "none") return ""; var a = view.marketInfo.markets || []; for (var i = 0; i < a.length; ++i) if (a[i].key === k) return a[i].label; return k }
    function reload() {
        api.get("/spotstudio/options", function(ok, d) { if (ok) view.options = d })
        api.get("/spots/markets", function(ok, d) { if (ok) view.marketInfo = d })
        reloadList()
        if (view.selId) loadSel()
    }
    function reloadList() { api.get("/spotstudio/projects", function(ok, d) { if (ok) view.projects = (d.projects || []).slice().sort(function(a, b) { return (b.updatedMs || b.createdMs || 0) - (a.updatedMs || a.createdMs || 0) }) }) }
    function select(id) { view.selId = id; view.sel = null; loadSel() }
    function loadSel() { if (!view.selId) return; api.get("/spotstudio/projects/" + encodeURIComponent(view.selId), function(ok, d) { if (ok) view.setSel(d.project || d) }) }
    function setSel(p) {
        view.sel = p
        if (!p) return
        var pub = p.published || {}
        pubType.currentIndex = Math.max(0, ["ad", "promo", "psa"].indexOf(p.kind === "announcement" ? "psa" : p.kind))
        pubClient.text = p.client || ""
        pubTitle.text = p.title || ""
        var ec = p.endCard || {}
        ecHeadline.text = ec.headline || ""; ecCta.text = ec.cta || ""; ecUrl.text = ec.url || ""; ecVo.text = ec.vo || ""
        if (!view.pubChannels.length) view.pubChannels = view.channelIds().slice(0, 1)
    }
    function put(body, okMsg) {
        if (!view.sel) return
        api.put("/spotstudio/projects/" + encodeURIComponent(view.sel.id), body, function(ok, d) {
            if (!ok) return
            if (d && (d.id || d.project)) view.setSel(d.project || d); else view.loadSel()
            if (okMsg) window.toast(okMsg, "ok")
            view.reloadList()
        })
    }
    function post(sub, body, okMsg) {
        if (!view.sel) return
        api.post("/spotstudio/projects/" + encodeURIComponent(view.sel.id) + "/" + sub, body, function(ok, d) {
            if (!ok) return
            if (d && (d.id || d.project) && (d.project || d).scenes !== undefined) view.setSel(d.project || d); else view.loadSel()
            if (okMsg) window.toast(okMsg, "ok")
            view.reloadList()
        })
    }
    function createProject() {
        var b = { title: nTitle.text.trim(), kind: nKind.currentText, client: nClient.text.trim(), brief: nBrief.text, lengthSec: parseInt(nLen.currentText) || 30,
                  aspect: nAspect.currentText, engine: nEngine.currentText, tone: nTone.text.trim(),
                  brand: { name: nBrandName.text.trim(), cta: nBrandCta.text.trim(), url: nBrandUrl.text.trim() } }
        var t = nTemplate.model[nTemplate.currentIndex]; if (t && t.id) b.template = t.id
        var v = nVoice.model[nVoice.currentIndex]; if (v && v.id) b.voice = v.id
        var m = nMusic.model[nMusic.currentIndex]; if (m && m.id) b.music = m.id
        var mk = nMarket.model[nMarket.currentIndex]; if (mk && mk.id) b.market = mk.id
        api.post("/spotstudio/projects", b, function(ok, d) {
            if (!ok) return
            var p = d.project || d
            window.toast("Spot project created", "ok")
            nTitle.text = ""; nBrief.text = ""
            view.reloadList()
            if (p && p.id) { view.selId = p.id; view.setSel(p) }
        })
    }
    function saveScenes() {
        var edits = []
        for (var i = 0; i < sceneRep.count; ++i) { var it = sceneRep.itemAt(i); if (it) edits.push(it.collect()) }
        put({ edits: edits }, "Scenes saved")
    }
    function addScene() {
        var sc = (view.sel && view.sel.scenes) || []
        var n = 1; for (var i = 0; i < sc.length; ++i) n = Math.max(n, (sc[i].n || 0) + 1)
        put({ edits: [{ scene: n, add: true, visual: "", vo: "", onScreen: "", durationSec: 3 }] }, "Scene added")
    }
    function toggleChannel(c) { var l = view.pubChannels.slice(); var i = l.indexOf(c); if (i >= 0) l.splice(i, 1); else l.push(c); view.pubChannels = l }
    function publish() {
        var b = { type: pubType.currentText, client: pubClient.text.trim(), title: pubTitle.text.trim(), channels: view.pubChannels, startDate: pubStart.text.trim(), endDate: pubEnd.text.trim() }
        if (pubContract.text.trim().length) b.contractId = pubContract.text.trim()
        else b.contract = { spotsPerDay: parseInt(pubPerDay.text) || 0, spotsTotal: parseInt(pubTotal.text) || 0, priority: parseInt(pubPrio.text) || 0 }
        if (pubSocial.checked) b.social = { queue: true, message: pubSocialMsg.text }
        post("publish", b, "Published to rotation")
    }

    Component.onCompleted: { reload() }
    Connections { target: supervisor; function onUiEvent(t, p) { if ((t === "spotstudio" || t === "render" || t === "spots") && view.busy) view.loadSel(); if (t === "spotstudio") view.reloadList() } }
    Timer { interval: 5000; repeat: true; running: view.busy; onTriggered: { view.loadSel(); view.reloadList() } }

    RowLayout {
        anchors.fill: parent; spacing: 10

        // ======================================================================= left: projects + new
        Flickable {
            Layout.preferredWidth: 400; Layout.minimumWidth: 360; Layout.maximumWidth: 420; Layout.fillHeight: true
            clip: true; contentWidth: width; contentHeight: leftCol.implicitHeight + 12
            ScrollBar.vertical: ScrollBar {}
            ColumnLayout {
                id: leftCol; width: parent.width; spacing: 10
                CPanel {
                    Layout.fillWidth: true; Layout.preferredHeight: 300; padding: 0
                    title: "Projects (" + view.projects.length + ")"
                    headerRight: [ CButton { text: "↻"; small: true; variant: "ghost"; onClicked: { view.reload() } } ]
                    ListView {
                        id: projList
                        anchors.fill: parent; clip: true; model: view.projects
                        ScrollBar.vertical: ScrollBar {}
                        delegate: Rectangle {
                            width: projList.width; height: 46; color: modelData.id === view.selId ? Qt.rgba(0, 0.83, 1, 0.12) : (index % 2 ? Theme.bg2 : Theme.bg3)
                            MouseArea { anchors.fill: parent; onClicked: { view.select(modelData.id) } }
                            ColumnLayout { anchors.fill: parent; anchors.leftMargin: 10; anchors.rightMargin: 8; anchors.topMargin: 5; anchors.bottomMargin: 5; spacing: 2
                                RowLayout { Layout.fillWidth: true; spacing: 6
                                    Text { text: modelData.title || "(untitled)"; color: Theme.text0; font.pixelSize: Theme.fs12; font.bold: true; elide: Text.ElideRight; Layout.fillWidth: true }
                                    CBadge { text: String(modelData.status || "").replace("_", " "); tint: view.statusColor(modelData.status) } }
                                Text { Layout.fillWidth: true; text: (modelData.kind || "") + " · " + (modelData.lengthSec || "?") + " s" + (modelData.client ? " · " + modelData.client : "") + "  " + view.when(modelData.updatedMs || modelData.createdMs); color: Theme.text3; font.pixelSize: Theme.fs10; elide: Text.ElideRight }
                            }
                        }
                        Text { anchors.centerIn: parent; visible: !view.projects.length; text: "No spot projects yet"; color: Theme.text3; font.pixelSize: Theme.fs11 }
                    }
                }
                CPanel {
                    Layout.fillWidth: true; implicitHeight: 34 + newCol.implicitHeight + 24
                    title: "New spot"
                    ColumnLayout {
                        id: newCol; anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; spacing: 6
                        CField { id: nTitle; label: "Title"; Layout.fillWidth: true; placeholder: "Spring sale at Main St Hardware" }
                        RowLayout { Layout.fillWidth: true; spacing: 6
                            CSelect { id: nKind; label: "Kind"; Layout.fillWidth: true; model: view.kinds }
                            CField { id: nClient; label: "Client"; Layout.fillWidth: true } }
                        AreaField { id: nBrief; label: "Brief"; Layout.fillWidth: true; areaHeight: 90; placeholder: "Who, what, offer, dates, tone… (never invent offers or prices)" }
                        RowLayout { Layout.fillWidth: true; spacing: 6
                            CSelect { id: nLen; label: "Length (s)"; Layout.fillWidth: true; model: view.lengths; currentIndex: Math.max(0, view.lengths.indexOf("30")) }
                            CSelect { id: nAspect; label: "Aspect"; Layout.fillWidth: true; model: view.aspects }
                            CSelect { id: nEngine; label: "Engine"; Layout.fillWidth: true; model: ["local", "studio"] } }
                        Text { Layout.fillWidth: true; text: nEngine.currentText + ": " + view.engineNote(nEngine.currentText); visible: text.length > nEngine.currentText.length + 2; color: view.engineOk(nEngine.currentText) ? Theme.text3 : Theme.standby; font.pixelSize: Theme.fs10; wrapMode: Text.WordWrap }
                        CSelect { id: nTemplate; label: "Template"; Layout.fillWidth: true; model: view.withNone(view.options.templates); textRole: "title" }
                        RowLayout { Layout.fillWidth: true; spacing: 6
                            CSelect { id: nVoice; label: "Voice"; Layout.fillWidth: true; model: view.withNone(view.options.voices); textRole: "title" }
                            CSelect { id: nMusic; label: "Music"; Layout.fillWidth: true; model: view.withNone(view.options.music); textRole: "title" } }
                        CField { id: nTone; label: "Tone"; Layout.fillWidth: true; placeholder: "warm, upbeat" }
                        CSelect { id: nMarket; label: "Market"; Layout.fillWidth: true; model: view.marketModel(); textRole: "title" }
                        SecLabel { text: "BRAND" }
                        RowLayout { Layout.fillWidth: true; spacing: 6
                            CField { id: nBrandName; label: "Name"; Layout.fillWidth: true }
                            CField { id: nBrandCta; label: "Call to action"; Layout.fillWidth: true } }
                        CField { id: nBrandUrl; label: "URL"; Layout.fillWidth: true; mono: true }
                        CButton { text: "Create project"; icon2: "✦"; variant: "primary"; Layout.alignment: Qt.AlignRight; enabled: nBrief.text.trim().length > 0 || nTitle.text.trim().length > 0; onClicked: { view.createProject() } }
                    }
                }
                CPanel {
                    Layout.fillWidth: true; implicitHeight: 34 + revCol.implicitHeight + 24
                    title: "Revise an auto spot"
                    ColumnLayout {
                        id: revCol; anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; spacing: 6
                        CField { id: revMedia; label: "Media id"; Layout.fillWidth: true; mono: true }
                        AreaField { id: revNotes; label: "Editor's note"; Layout.fillWidth: true; areaHeight: 60; placeholder: "Say the phone number slower; drop the second scene" }
                        CButton { text: "Revise"; variant: "primary"; Layout.alignment: Qt.AlignRight; enabled: revMedia.text.trim().length > 0 && revNotes.text.trim().length > 0
                            onClicked: { api.post("/spots/spot/" + encodeURIComponent(revMedia.text.trim()) + "/revise", { notes: revNotes.text }, function(ok) { if (ok) { window.toast("Revision queued", "ok"); revNotes.text = "" } }) } }
                    }
                }
            }
        }

        // ======================================================================= right: the selected project
        Item {
            Layout.fillWidth: true; Layout.fillHeight: true
            Text { anchors.centerIn: parent; visible: !view.sel; text: view.selId ? "loading…" : "Select a project or create a new spot"; color: Theme.text3; font.pixelSize: Theme.fs12 }
            Flickable {
                anchors.fill: parent; visible: !!view.sel
                clip: true; contentWidth: width; contentHeight: rightCol.implicitHeight + 12
                ScrollBar.vertical: ScrollBar {}
                ColumnLayout {
                    id: rightCol; width: parent.width; spacing: 10
                    property var p: view.sel || ({})
                    property var r: (view.sel && view.sel.render) || ({})

                    // ---------------------------------------------------------------- header + generate
                    CPanel {
                        Layout.fillWidth: true; implicitHeight: 34 + hdrCol.implicitHeight + 24
                        title: (rightCol.p.title || "(untitled)")
                        headerRight: [
                            CBadge { text: String(rightCol.p.status || "").replace("_", " "); tint: view.statusColor(rightCol.p.status); solid: true },
                            CButton { text: "✕"; small: true; variant: "ghost"; ToolTip.text: "Delete project"
                                onClicked: { api.del("/spotstudio/projects/" + encodeURIComponent(rightCol.p.id), function(ok) { if (ok) { view.selId = ""; view.sel = null; view.reloadList() } }) } }
                        ]
                        ColumnLayout {
                            id: hdrCol; anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; spacing: 6
                            Text { Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.text1; font.pixelSize: Theme.fs11
                                text: [rightCol.p.kind, (rightCol.p.lengthSec || "?") + " s", rightCol.p.aspect, rightCol.p.engine, rightCol.p.client, rightCol.p.template, rightCol.p.voice, view.marketLabel(rightCol.p.market)].filter(function(x) { return !!x }).join("  ·  ") }
                            Text { Layout.fillWidth: true; visible: !!rightCol.p.brief; wrapMode: Text.WordWrap; color: Theme.text2; font.pixelSize: Theme.fs11; text: rightCol.p.brief || "" }
                            Rectangle { visible: !!(rightCol.p.stale && (typeof rightCol.p.stale === "object" ? rightCol.p.stale.compose : rightCol.p.stale)); Layout.fillWidth: true; implicitHeight: 24; radius: Theme.r3; color: Qt.rgba(1, 0.84, 0.04, 0.12)
                                Text { anchors.verticalCenter: parent.verticalCenter; anchors.left: parent.left; anchors.leftMargin: 8; text: "The script changed since the last render — generate again."; color: Theme.warn; font.pixelSize: Theme.fs11 } }
                            RowLayout { Layout.fillWidth: true; spacing: 8
                                CSelect { id: genEngine; label: "Engine"; Layout.preferredWidth: 130; model: ["local", "studio"]; currentIndex: rightCol.p.engine === "studio" ? 1 : 0 }
                                CSelect { id: genMarket; label: "Market"; Layout.preferredWidth: 220; model: view.marketModel(); textRole: "title"; currentIndex: view.marketIndex(rightCol.p.market); onActivated: function(i) { view.put({ market: view.marketModel()[i].id }, "Market set — generate again to apply it") } }
                                Text { Layout.fillWidth: true; Layout.alignment: Qt.AlignBottom; text: view.engineNote(genEngine.currentText); color: view.engineOk(genEngine.currentText) ? Theme.text3 : Theme.standby; font.pixelSize: Theme.fs10; elide: Text.ElideRight }
                                CButton { text: "Compose"; Layout.alignment: Qt.AlignBottom; enabled: !view.busy; onClicked: { view.post("generate", { step: "compose", engine: genEngine.currentText }, "Compose queued") } ToolTip.text: "Voice + picture from the current script" }
                                CButton { text: "Generate all"; icon2: "▶"; variant: "primary"; Layout.alignment: Qt.AlignBottom; enabled: !view.busy; onClicked: { view.post("generate", { step: "all", engine: genEngine.currentText }, "Generation queued") } }
                            }
                            // render progress + preview
                            RowLayout { Layout.fillWidth: true; spacing: 10; visible: !!rightCol.r.state
                                Rectangle {
                                    Layout.preferredWidth: 160; Layout.preferredHeight: 90; color: Theme.bg1; radius: Theme.r3; border.color: Theme.border1
                                    Image { id: thumb; anchors.fill: parent; anchors.margins: 1; fillMode: Image.PreserveAspectFit; asynchronous: true; cache: false
                                        source: rightCol.r.mediaId ? api.rawUrl("/thumbs/" + rightCol.r.mediaId + ".jpg") : "" }
                                    Text { anchors.centerIn: parent; visible: thumb.status !== Image.Ready; text: rightCol.r.state === "done" ? "no thumbnail" : "…"; color: Theme.text3; font.pixelSize: Theme.fs10 }
                                }
                                ColumnLayout { Layout.fillWidth: true; spacing: 4
                                    RowLayout { Layout.fillWidth: true; spacing: 8
                                        CBadge { text: rightCol.r.state || ""; tint: rightCol.r.state === "done" ? Theme.ready : rightCol.r.state === "failed" ? Theme.danger : Theme.accent0 }
                                        Text { text: (rightCol.r.engine || "") + (rightCol.r.jobId ? " job " + rightCol.r.jobId : rightCol.r.genId ? " gen " + rightCol.r.genId : ""); color: Theme.text2; font.pixelSize: Theme.fs10; font.family: Theme.fontMono; Layout.fillWidth: true; elide: Text.ElideRight }
                                        Text { text: rightCol.r.durationSec ? Number(rightCol.r.durationSec).toFixed(1) + " s" : ""; color: Theme.text1; font.pixelSize: Theme.fs11 } }
                                    Bar { Layout.fillWidth: true; pct: rightCol.r.state === "done" ? 100 : (rightCol.r.pct || 0); tint: rightCol.r.state === "failed" ? Theme.danger : rightCol.r.state === "done" ? Theme.ready : Theme.accent0 }
                                    Text { Layout.fillWidth: true; visible: !!rightCol.r.error; text: rightCol.r.error || ""; color: Theme.danger; font.pixelSize: Theme.fs10; wrapMode: Text.WordWrap }
                                    RowLayout { Layout.fillWidth: true; spacing: 6
                                        Text { Layout.fillWidth: true; text: rightCol.r.path || ""; color: Theme.text3; font.pixelSize: Theme.fs10; font.family: Theme.fontMono; elide: Text.ElideMiddle }
                                        CButton { visible: !!rightCol.r.path; text: "Reveal file"; small: true; onClicked: { api.revealInExplorer(rightCol.r.path) } }
                                        CButton { visible: !!rightCol.r.mediaId; text: "Open media"; small: true; variant: "ghost"; onClicked: { window.openMedia(rightCol.r.mediaId, "meta") } } }
                                }
                            }
                        }
                    }

                    // ---------------------------------------------------------------- concepts + questions
                    RowLayout {
                        Layout.fillWidth: true; spacing: 10
                        visible: (rightCol.p.concepts || []).length > 0 || (rightCol.p.questions || []).length > 0
                        CPanel {
                            Layout.fillWidth: true; Layout.alignment: Qt.AlignTop; implicitHeight: 34 + conCol.implicitHeight + 24
                            title: "Concepts"
                            ColumnLayout {
                                id: conCol; anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; spacing: 6
                                Repeater {
                                    model: rightCol.p.concepts || []
                                    Rectangle {
                                        Layout.fillWidth: true; implicitHeight: cc.implicitHeight + 12; radius: Theme.r5
                                        color: rightCol.p.concept === index ? Qt.rgba(0, 0.83, 1, 0.10) : Theme.bg3; border.color: rightCol.p.concept === index ? Theme.accent0 : Theme.border0
                                        ColumnLayout { id: cc; anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 6; spacing: 2
                                            RowLayout { Layout.fillWidth: true
                                                Text { Layout.fillWidth: true; text: (index + 1) + ". " + (modelData.title || ""); color: Theme.text0; font.pixelSize: Theme.fs12; font.bold: true; elide: Text.ElideRight }
                                                CButton { text: rightCol.p.concept === index ? "in use" : "Use this concept"; small: true; active: rightCol.p.concept === index; onClicked: { view.put({ answers: { concept: index } }, "Concept " + (index + 1) + " selected") } } }
                                            Text { Layout.fillWidth: true; visible: !!modelData.hook; text: "Hook: " + (modelData.hook || ""); color: Theme.text1; font.pixelSize: Theme.fs11; wrapMode: Text.WordWrap }
                                            Text { Layout.fillWidth: true; visible: !!modelData.angle; text: "Angle: " + (modelData.angle || ""); color: Theme.text2; font.pixelSize: Theme.fs11; wrapMode: Text.WordWrap }
                                        }
                                    }
                                }
                                Text { visible: !(rightCol.p.concepts || []).length; text: "No concepts"; color: Theme.text3; font.pixelSize: Theme.fs11 }
                            }
                        }
                        CPanel {
                            Layout.fillWidth: true; Layout.alignment: Qt.AlignTop; implicitHeight: 34 + qCol.implicitHeight + 24
                            title: "Open questions (" + (rightCol.p.questions || []).length + ")"
                            ColumnLayout {
                                id: qCol; anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; spacing: 8
                                Repeater {
                                    model: rightCol.p.questions || []
                                    ColumnLayout { Layout.fillWidth: true; spacing: 4
                                        property var qq: modelData
                                        Text { Layout.fillWidth: true; text: qq.q || qq.id; color: Theme.text0; font.pixelSize: Theme.fs12; wrapMode: Text.WordWrap }
                                        Flow { Layout.fillWidth: true; spacing: 4; visible: (qq.options || []).length > 0
                                            Repeater { model: qq.options || []
                                                CButton { text: view.lbl(modelData); small: true; onClicked: { var a = {}; a[qq.id] = view.val(modelData); view.put({ answers: a }, "Answered") } } } }
                                        RowLayout { Layout.fillWidth: true; spacing: 6
                                            CField { id: qAns; Layout.fillWidth: true; placeholder: "your answer" }
                                            CButton { text: "Answer"; small: true; enabled: qAns.text.trim().length > 0; onClicked: { var a = {}; a[qq.id] = qAns.text.trim(); view.put({ answers: a }, "Answered") } } }
                                    }
                                }
                                Text { visible: !(rightCol.p.questions || []).length; text: "Nothing to ask"; color: Theme.text3; font.pixelSize: Theme.fs11 }
                            }
                        }
                    }

                    // ---------------------------------------------------------------- script
                    CPanel {
                        Layout.fillWidth: true; implicitHeight: 34 + scCol.implicitHeight + 24
                        title: "Script — " + (rightCol.p.scenes || []).length + " scenes, " + (rightCol.p.lengthSec || "?") + " s"
                        headerRight: [
                            CButton { text: "+ scene"; small: true; variant: "ghost"; onClicked: { view.addScene() } },
                            CButton { text: "Save scenes"; small: true; variant: "primary"; onClicked: { view.saveScenes() } }
                        ]
                        ColumnLayout {
                            id: scCol; anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; spacing: 4
                            RowLayout { Layout.fillWidth: true; spacing: 6
                                SecLabel { text: "#"; Layout.preferredWidth: 22 }
                                SecLabel { text: "BEAT"; Layout.preferredWidth: 80 }
                                SecLabel { text: "VISUAL"; Layout.fillWidth: true; Layout.preferredWidth: 3 }
                                SecLabel { text: "VOICE-OVER"; Layout.fillWidth: true; Layout.preferredWidth: 3 }
                                SecLabel { text: "ON SCREEN"; Layout.fillWidth: true; Layout.preferredWidth: 2 }
                                SecLabel { text: "SEC"; Layout.preferredWidth: 50 }
                                Item { Layout.preferredWidth: 26 } }
                            Repeater {
                                id: sceneRep
                                model: rightCol.p.scenes || []
                                RowLayout {
                                    id: sceneRow
                                    Layout.fillWidth: true; spacing: 6
                                    property bool removed: false
                                    opacity: removed ? 0.4 : 1
                                    function collect() {
                                        var e = { scene: modelData.n, visual: scVisual.text, vo: scVo.text, onScreen: scOn.text, durationSec: parseFloat(scDur.text) || modelData.durationSec }
                                        if (sceneRow.removed) e.remove = true
                                        return e
                                    }
                                    Text { text: String(modelData.n); color: Theme.text2; font.pixelSize: Theme.fs11; font.family: Theme.fontMono; Layout.preferredWidth: 22 }
                                    Text { text: modelData.beat || ""; color: Theme.accent0; font.pixelSize: Theme.fs10; font.bold: true; Layout.preferredWidth: 80; elide: Text.ElideRight }
                                    CField { id: scVisual; Layout.fillWidth: true; Layout.preferredWidth: 3; text: modelData.visual || "" }
                                    CField { id: scVo; Layout.fillWidth: true; Layout.preferredWidth: 3; text: modelData.vo || "" }
                                    CField { id: scOn; Layout.fillWidth: true; Layout.preferredWidth: 2; text: modelData.onScreen || "" }
                                    CField { id: scDur; Layout.preferredWidth: 50; mono: true; text: String(modelData.durationSec !== undefined ? modelData.durationSec : "") }
                                    CButton { text: sceneRow.removed ? "↩" : "✕"; small: true; variant: "ghost"; Layout.preferredWidth: 26; onClicked: { sceneRow.removed = !sceneRow.removed } ToolTip.text: sceneRow.removed ? "Keep" : "Remove (on save)" }
                                }
                            }
                            Text { visible: !(rightCol.p.scenes || []).length; text: "No script yet — answer the questions or generate"; color: Theme.text3; font.pixelSize: Theme.fs11 }
                            Text { Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.text3; font.pixelSize: Theme.fs10
                                text: "Durations are rebalanced to the exact spot length on save; editing makes the last render stale." }
                        }
                    }

                    // ---------------------------------------------------------------- end card + instruction
                    RowLayout {
                        Layout.fillWidth: true; spacing: 10
                        CPanel {
                            Layout.fillWidth: true; Layout.alignment: Qt.AlignTop; implicitHeight: 34 + ecCol.implicitHeight + 24
                            title: "End card"
                            ColumnLayout {
                                id: ecCol; anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; spacing: 6
                                RowLayout { Layout.fillWidth: true; spacing: 6
                                    CField { id: ecHeadline; label: "Headline"; Layout.fillWidth: true }
                                    CField { id: ecCta; label: "CTA"; Layout.fillWidth: true } }
                                CField { id: ecUrl; label: "URL"; Layout.fillWidth: true; mono: true }
                                CField { id: ecVo; label: "Voice-over"; Layout.fillWidth: true }
                                CButton { text: "Save end card"; small: true; variant: "primary"; Layout.alignment: Qt.AlignRight
                                    onClicked: { view.put({ endCard: { headline: ecHeadline.text, cta: ecCta.text, url: ecUrl.text, vo: ecVo.text } }, "End card saved") } }
                            }
                        }
                        CPanel {
                            Layout.fillWidth: true; Layout.alignment: Qt.AlignTop; implicitHeight: 34 + insCol.implicitHeight + 24
                            title: "Instruction to the writer"
                            ColumnLayout {
                                id: insCol; anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; spacing: 6
                                AreaField { id: insText; Layout.fillWidth: true; areaHeight: 100; placeholder: "Make it funnier; open on the storefront; mention free parking" }
                                CButton { text: "Rewrite"; small: true; variant: "primary"; Layout.alignment: Qt.AlignRight; enabled: insText.text.trim().length > 0
                                    onClicked: { view.put({ instruction: insText.text }, "Instruction sent"); insText.text = "" } }
                            }
                        }
                    }

                    // ---------------------------------------------------------------- review + publish
                    RowLayout {
                        Layout.fillWidth: true; spacing: 10
                        CPanel {
                            Layout.preferredWidth: 360; Layout.maximumWidth: 400; Layout.alignment: Qt.AlignTop; implicitHeight: 34 + rvCol.implicitHeight + 24
                            title: "Review"
                            ColumnLayout {
                                id: rvCol; anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; spacing: 6
                                AreaField { id: rvNotes; label: "Notes"; Layout.fillWidth: true; areaHeight: 70 }
                                Flow { Layout.fillWidth: true; spacing: 6
                                    CButton { text: "Submit"; small: true; onClicked: { view.post("review", { action: "submit", notes: rvNotes.text }, "Submitted for review"); rvNotes.text = "" } }
                                    CButton { text: "Approve"; small: true; variant: "success"; onClicked: { view.post("review", { action: "approve", notes: rvNotes.text }, "Approved"); rvNotes.text = "" } }
                                    CButton { text: "Request changes"; small: true; variant: "warn"; onClicked: { view.post("review", { action: "changes", notes: rvNotes.text }, "Changes requested"); rvNotes.text = "" } }
                                    CButton { text: "Reject"; small: true; variant: "danger"; onClicked: { view.post("review", { action: "reject", notes: rvNotes.text }, "Rejected"); rvNotes.text = "" } } }
                                Text { Layout.fillWidth: true; visible: !!(rightCol.p.review && rightCol.p.review.notes); text: "Last notes: " + ((rightCol.p.review && rightCol.p.review.notes) || ""); color: Theme.text2; font.pixelSize: Theme.fs11; wrapMode: Text.WordWrap }
                                Repeater { model: ((rightCol.p.review && rightCol.p.review.history) || []).slice(-5).reverse()
                                    Text { Layout.fillWidth: true; text: view.when(modelData.ms) + "  " + (modelData.action || "") + (modelData.actor ? " (" + modelData.actor + ")" : "") + (modelData.notes ? ": " + modelData.notes : ""); color: Theme.text3; font.pixelSize: Theme.fs10; wrapMode: Text.WordWrap } }
                            }
                        }
                        CPanel {
                            Layout.fillWidth: true; Layout.alignment: Qt.AlignTop; implicitHeight: 34 + pbCol.implicitHeight + 24
                            title: "Publish to rotation"
                            ColumnLayout {
                                id: pbCol; anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; spacing: 6
                                Text { Layout.fillWidth: true; visible: !!(rightCol.p.published && rightCol.p.published.mediaId); color: Theme.accent2; font.pixelSize: Theme.fs11
                                    text: rightCol.p.published ? "Published " + view.when(rightCol.p.published.ms) + " · media " + (rightCol.p.published.mediaId || "") + (rightCol.p.published.contractId ? " · contract " + rightCol.p.published.contractId : "") : "" }
                                RowLayout { Layout.fillWidth: true; spacing: 6
                                    CSelect { id: pubType; label: "Type"; Layout.preferredWidth: 100; model: ["ad", "promo", "psa"] }
                                    CField { id: pubClient; label: "Client"; Layout.fillWidth: true }
                                    CField { id: pubTitle; label: "Title"; Layout.fillWidth: true } }
                                SecLabel { text: "CHANNELS" }
                                Flow { Layout.fillWidth: true; spacing: 4
                                    Repeater { model: view.channelIds()
                                        CButton { text: modelData; small: true; active: view.pubChannels.indexOf(modelData) >= 0; onClicked: { view.toggleChannel(modelData) } } } }
                                RowLayout { Layout.fillWidth: true; spacing: 6
                                    CField { id: pubStart; label: "Start (yyyy-MM-dd)"; Layout.fillWidth: true; mono: true; text: view.today(0) }
                                    CField { id: pubEnd; label: "End (yyyy-MM-dd)"; Layout.fillWidth: true; mono: true; text: view.today(30) } }
                                RowLayout { Layout.fillWidth: true; spacing: 6
                                    CField { id: pubContract; label: "Contract id (existing)"; Layout.fillWidth: true; mono: true; placeholder: "blank = new contract" }
                                    CField { id: pubPerDay; label: "Spots/day"; Layout.preferredWidth: 80; mono: true; text: "4"; readOnly: pubContract.text.trim().length > 0 }
                                    CField { id: pubTotal; label: "Spots total"; Layout.preferredWidth: 80; mono: true; text: "0"; readOnly: pubContract.text.trim().length > 0 }
                                    CField { id: pubPrio; label: "Priority"; Layout.preferredWidth: 70; mono: true; text: "5"; readOnly: pubContract.text.trim().length > 0 } }
                                RowLayout { Layout.fillWidth: true; spacing: 6
                                    CToggle { id: pubSocial; label: "Queue a social draft"; implicitWidth: 170 }
                                    CField { id: pubSocialMsg; visible: pubSocial.checked; Layout.fillWidth: true; placeholder: "post message" } }
                                RowLayout { Layout.fillWidth: true
                                    Text { Layout.fillWidth: true; visible: rightCol.p.status !== "approved" && rightCol.p.status !== "published"; text: "Only an approved spot can be published."; color: Theme.standby; font.pixelSize: Theme.fs10 }
                                    CButton { text: "Publish"; icon2: "↗"; variant: "primary"; enabled: view.pubChannels.length > 0 && pubStart.text.length === 10 && pubEnd.text.length === 10; onClicked: { view.publish() } } }
                            }
                        }
                    }

                    // ---------------------------------------------------------------- history
                    CPanel {
                        Layout.fillWidth: true; implicitHeight: 34 + hiCol.implicitHeight + 24
                        title: "History"
                        ColumnLayout {
                            id: hiCol; anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; spacing: 2
                            Repeater { model: (rightCol.p.history || []).slice().reverse().slice(0, 40)
                                RowLayout { Layout.fillWidth: true; spacing: 8
                                    Text { text: view.when(modelData.ms); color: Theme.text3; font.pixelSize: Theme.fs10; font.family: Theme.fontMono; Layout.preferredWidth: 80 }
                                    Text { text: modelData.action || modelData.event || ""; color: Theme.text0; font.pixelSize: Theme.fs11; font.bold: true; Layout.preferredWidth: 130; elide: Text.ElideRight }
                                    Text { Layout.fillWidth: true; text: (modelData.actor ? modelData.actor + ": " : "") + (modelData.note || modelData.notes || ""); color: Theme.text2; font.pixelSize: Theme.fs11; elide: Text.ElideRight } } }
                            Text { visible: !(rightCol.p.history || []).length; text: "No history"; color: Theme.text3; font.pixelSize: Theme.fs11 }
                        }
                    }
                }
            }
        }
    }
}
