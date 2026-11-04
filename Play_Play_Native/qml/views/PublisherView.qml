import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".."
import "../ui"

/**
 * Publish (docs/publisher.md): the social / website publication queue. Health header with the kill switch,
 * the queue with filters, a detail panel with every item action, a new-draft form, the publisher config and
 * the audit. Publications are immutable records: corrections are new rows, a retraction is a marker.
 * Talks to /api/v1/publish/* and reloads on the "publish" uiEvent.
 */
Item {
    id: view
    property var status: ({})
    property var cfg: ({})
    property var items: []
    property var audit: []
    property string selId: ""
    property var sel: null
    property string fStatus: "open"
    property string fKind: ""
    property string fSource: ""
    property string fChannel: ""
    property string draftId: ""             // revising an existing draft
    property var draftChannels: ["facebook"]
    property string verifyText: ""
    readonly property var killed: status.killed || ({})
    readonly property bool anyKilled: !!(killed.all || killed.facebook || killed.site)
    readonly property bool active: { for (var i = 0; i < items.length; ++i) if (items[i].status === "publishing" || items[i].status === "scheduled") return true; return false }

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

    function stColor(s) {
        switch (String(s || "")) {
        case "draft": return Theme.text1
        case "pending_approval": return Theme.standby
        case "held": return Theme.warn
        case "approved": return Theme.cued
        case "scheduled": return Theme.accent2
        case "publishing": return Theme.accent0
        case "published": return Theme.ready
        case "failed": return Theme.danger
        case "skipped": case "retracted": return Theme.text3
        case "corrected": return Theme.clsBumper
        default: return Theme.text2
        }
    }
    function chColor(st) { return st === "published" || st === "posted" || st === "done" ? Theme.ready : st === "failed" ? Theme.danger : st === "publishing" ? Theme.accent0 : Theme.text2 }
    function hasAi(it) { var a = it.ai || {}; return !!(a.text || a.voice || a.images || a.video) }
    function when(ms) { return ms ? new Date(ms).toLocaleString(Qt.locale(), "MM-dd HH:mm") : "" }
    function lines(s) { return String(s || "").split(/\r?\n/).map(function(x) { return x.trim() }).filter(function(x) { return x.length }) }
    function csv(s) { return String(s || "").split(",").map(function(x) { return x.trim() }).filter(function(x) { return x.length }) }

    function reload() {
        api.get("/publish/status", function(ok, d) { if (ok) view.status = d })
        api.get("/publish/config", function(ok, d) { if (ok) view.cfg = d.config || d })
        reloadQueue()
        reloadAudit()
        if (view.selId) loadSel()
    }
    function reloadQueue() {
        var q = "?status=" + encodeURIComponent(view.fStatus)
        if (view.fKind) q += "&kind=" + encodeURIComponent(view.fKind)
        if (view.fSource) q += "&source=" + encodeURIComponent(view.fSource)
        if (view.fChannel) q += "&channel=" + encodeURIComponent(view.fChannel)
        api.get("/publish/queue" + q, function(ok, d) { if (ok) view.items = d.items || [] })
    }
    function reloadAudit() { api.get("/publish/audit", function(ok, d) { if (ok) view.audit = (d.audit || []).slice().sort(function(a, b) { return (b.ms || 0) - (a.ms || 0) }) }) }
    function select(id) { view.selId = id; loadSel() }
    function loadSel() { api.get("/publish/items/" + encodeURIComponent(view.selId), function(ok, d) { if (ok) view.sel = d.item || d; else { view.sel = null; view.selId = "" } }) }
    function act(a, body, okMsg) {
        if (!view.sel) return
        api.post("/publish/items/" + encodeURIComponent(view.sel.id) + "/" + a, body || {}, function(ok, d) {
            if (!ok) return
            window.toast(okMsg || (a + " done"), "ok")
            var it = d && (d.item || (d.id ? d : null))
            if (it && it.id && it.id !== view.selId) view.selId = it.id      // a correction is a new row
            view.reload()
        })
    }
    function schedule() {
        var dt = Date.fromLocaleString(Qt.locale(), dSched.text.trim(), "yyyy-MM-dd HH:mm")
        if (isNaN(dt.getTime())) { window.toast("Schedule as yyyy-MM-dd HH:mm", "error"); return }
        act("schedule", { scheduleAtMs: dt.getTime() }, "Scheduled for " + dSched.text.trim())
    }
    function reviseDraft(it) {
        view.draftId = it.id
        nKind.currentIndex = Math.max(0, nKind.model.indexOf(it.kind || "text"))
        nTitle.text = it.title || ""; nBody.text = it.body || ""; nCaption.text = it.caption || ""
        nLink.text = it.linkUrl || ""; nMedia.text = it.mediaId || ""
        view.draftChannels = (it.channels || []).slice()
        nSources.text = (it.sourceLinks || []).join("\n"); nTags.text = (it.tags || []).join(", ")
        nAi.checked = !!(it.ai && it.ai.text)
    }
    function clearDraft() {
        view.draftId = ""; nTitle.text = ""; nBody.text = ""; nCaption.text = ""; nLink.text = ""; nMedia.text = ""
        nSources.text = ""; nTags.text = ""; nAi.checked = false; view.draftChannels = ["facebook"]
    }
    function submitDraft() {
        var b = { kind: nKind.currentText, title: nTitle.text.trim(), body: nBody.text, caption: nCaption.text, linkUrl: nLink.text.trim(), mediaId: nMedia.text.trim(),
                  channels: view.draftChannels, sourceLinks: view.lines(nSources.text), tags: view.csv(nTags.text), ai: { text: nAi.checked } }
        if (view.draftId) b.id = view.draftId
        api.post("/publish/draft", b, function(ok, d) {
            if (!ok) return
            var it = d.item || d
            window.toast((view.draftId ? "Draft revised" : "Draft created") + (it && it.status ? " — " + it.status : ""), "ok")
            view.clearDraft()
            if (it && it.id) view.selId = it.id
            view.reload()
        })
    }
    function toggleDraftChannel(c) { var l = view.draftChannels.slice(); var i = l.indexOf(c); if (i >= 0) l.splice(i, 1); else l.push(c); view.draftChannels = l }
    function saveCfg() {
        var fb = { pageId: cfPage.text.trim() }
        if (cfFbToken.text.length) fb.token = cfFbToken.text
        var web = { url: cfWebUrl.text.trim(), user: cfWebUser.text.trim(), autoPublish: cfWebAuto.checked }
        if (cfWebKey.text.length) web.key = cfWebKey.text
        api.put("/publish/config", { mode: cfMode.currentText, recipes: view.csv(cfRecipes.text), tagline: cfTagline.text, maxPerDay: parseInt(cfMax.text) || 0,
                                     aiLabel: cfAiLabel.text, graphBase: cfGraph.text.trim(), facebook: fb, website: web },
                function(ok, d) { if (ok) { view.cfg = d.config || d; cfFbToken.text = ""; cfWebKey.text = ""; window.toast("Publisher config saved", "ok"); view.reload() } })
    }

    Component.onCompleted: { reload() }
    Connections { target: supervisor; function onUiEvent(t, p) { if (t === "publish") view.reload() } }
    Timer { interval: 5000; repeat: true; running: view.active; onTriggered: { view.reloadQueue(); if (view.selId) view.loadSel() } }

    Flickable {
        anchors.fill: parent; clip: true
        contentWidth: width; contentHeight: page.implicitHeight + 12
        ScrollBar.vertical: ScrollBar {}
        ColumnLayout {
            id: page
            width: parent.width; spacing: 10

            // ---------------------------------------------------------------- health + kill switch
            CPanel {
                Layout.fillWidth: true; implicitHeight: 34 + hdr.implicitHeight + 24
                title: "Publisher"
                headerRight: [ CButton { text: "↻"; small: true; variant: "ghost"; onClicked: { view.reload() } } ]
                RowLayout {
                    id: hdr; anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; spacing: 14
                    ColumnLayout { Layout.fillWidth: true; spacing: 6
                        RowLayout { spacing: 8
                            Text { text: "MODE"; color: Theme.text2; font.pixelSize: Theme.fs10; font.bold: true }
                            CBadge { text: view.status.mode || (view.cfg.mode || "off"); tint: (view.status.mode || view.cfg.mode) === "auto" ? Theme.standby : (view.status.mode || view.cfg.mode) === "review" ? Theme.accent0 : Theme.text3; solid: true }
                            CBadge { text: "facebook " + ((view.status.configured || {}).facebook ? "configured" : "not set"); tint: (view.status.configured || {}).facebook ? Theme.ready : Theme.text3 }
                            CBadge { text: "website " + ((view.status.configured || {}).website ? "configured" : "not set"); tint: (view.status.configured || {}).website ? Theme.ready : Theme.text3 }
                            CBadge { visible: view.anyKilled; solid: true; tint: Theme.onAir
                                text: "STOPPED: " + (view.killed.all ? "all" : [view.killed.facebook ? "facebook" : "", view.killed.site ? "site" : ""].filter(function(x) { return x }).join(", ")) }
                        }
                        Flow { Layout.fillWidth: true; spacing: 4
                            Repeater { model: Object.keys(view.status.counts || {})
                                CBadge { text: modelData.replace("_", " ") + " " + view.status.counts[modelData]; tint: view.stColor(modelData) } } }
                        Repeater { model: view.status.warnings || []
                            Text { Layout.fillWidth: true; text: "⚠ " + (typeof modelData === "string" ? modelData : (modelData.message || JSON.stringify(modelData))); color: Theme.warn; font.pixelSize: Theme.fs11; wrapMode: Text.WordWrap } }
                        Repeater { model: (view.status.next || []).slice(0, 4)
                            Text { Layout.fillWidth: true; text: "next: " + view.when(modelData.scheduleAtMs || modelData.ms) + "  " + (modelData.title || modelData.id || ""); color: Theme.text2; font.pixelSize: Theme.fs11; elide: Text.ElideRight } }
                    }
                    ColumnLayout { Layout.preferredWidth: 330; Layout.maximumWidth: 340; spacing: 6
                        RowLayout { Layout.fillWidth: true; spacing: 6
                            CField { id: stopReason; Layout.fillWidth: true; placeholder: "reason (logged)" }
                            CSelect { id: stopScope; Layout.preferredWidth: 100; model: ["all", "facebook", "site"] } }
                        AbstractButton {
                            id: stopBtn; hoverEnabled: true
                            Layout.fillWidth: true; implicitHeight: 46
                            onClicked: { api.post("/publish/stop", { reason: stopReason.text || "operator stop", scope: stopScope.currentText }, function(ok) { if (ok) { window.toast("Publishing STOPPED (" + stopScope.currentText + ")", "warn"); view.reload() } }) }
                            background: Rectangle { radius: Theme.r8; color: stopBtn.pressed ? Qt.darker(Theme.onAir, 1.3) : stopBtn.hovered ? Qt.lighter(Theme.onAir, 1.1) : Theme.onAir }
                            contentItem: Text { text: "■  STOP PUBLISHING"; color: "white"; font.family: Theme.fontDisplay; font.pixelSize: Theme.fs16; font.bold: true; font.letterSpacing: 1.5; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                        }
                        CButton { visible: view.anyKilled; Layout.fillWidth: true; text: "Release (" + stopScope.currentText + ")"; variant: "success"
                            onClicked: { api.post("/publish/release", { scope: stopScope.currentText }, function(ok) { if (ok) { window.toast("Publishing released (" + stopScope.currentText + "); mode stays off until set", "ok"); view.reload() } }) } }
                    }
                }
            }

            // ---------------------------------------------------------------- queue + detail
            RowLayout {
                Layout.fillWidth: true; spacing: 10
                CPanel {
                    Layout.preferredWidth: 560; Layout.maximumWidth: 620; Layout.preferredHeight: 600; Layout.alignment: Qt.AlignTop; padding: 0
                    title: "Queue (" + view.items.length + ")"
                    ColumnLayout {
                        anchors.fill: parent; spacing: 0
                        RowLayout { Layout.fillWidth: true; Layout.margins: 8; spacing: 6
                            CSelect { label: "Status"; Layout.fillWidth: true; model: ["open", "waiting", "needs_attention", "done", "all"]; onActivated: function(i) { view.fStatus = model[i]; view.reloadQueue() } }
                            CSelect { label: "Kind"; Layout.fillWidth: true; model: ["any", "article", "spot", "video", "slate", "text", "link"]; onActivated: function(i) { view.fKind = i ? model[i] : ""; view.reloadQueue() } }
                            CSelect { label: "Source"; Layout.fillWidth: true; model: ["any", "manual", "spot_job", "spot_studio", "agent_draft"]; onActivated: function(i) { view.fSource = i ? model[i] : ""; view.reloadQueue() } }
                            CSelect { label: "Channel"; Layout.fillWidth: true; model: ["any", "facebook", "website"]; onActivated: function(i) { view.fChannel = i ? model[i] : ""; view.reloadQueue() } } }
                        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.border0 }
                        ListView {
                            id: qList
                            Layout.fillWidth: true; Layout.fillHeight: true; clip: true; model: view.items
                            ScrollBar.vertical: ScrollBar {}
                            delegate: Rectangle {
                                width: qList.width; height: 50; color: modelData.id === view.selId ? Qt.rgba(0, 0.83, 1, 0.12) : (index % 2 ? Theme.bg2 : Theme.bg3)
                                MouseArea { anchors.fill: parent; onClicked: { view.select(modelData.id) } }
                                ColumnLayout { anchors.fill: parent; anchors.leftMargin: 10; anchors.rightMargin: 8; anchors.topMargin: 5; anchors.bottomMargin: 5; spacing: 3
                                    RowLayout { Layout.fillWidth: true; spacing: 6
                                        Text { Layout.fillWidth: true; text: modelData.title || modelData.caption || modelData.id; color: Theme.text0; font.pixelSize: Theme.fs12; font.bold: true; elide: Text.ElideRight }
                                        CBadge { visible: modelData.tier === "absolute"; text: "absolute"; tint: Theme.onAir; solid: true }
                                        CBadge { visible: view.hasAi(modelData); text: "AI"; tint: Theme.accent2 }
                                        CBadge { text: String(modelData.status || "").replace("_", " "); tint: view.stColor(modelData.status) } }
                                    RowLayout { Layout.fillWidth: true; spacing: 6
                                        Text { text: (modelData.kind || "") + " · " + (modelData.source || ""); color: Theme.text3; font.pixelSize: Theme.fs10 }
                                        Repeater { model: modelData.channels || []
                                            CBadge { text: modelData === "facebook" ? "fb" : modelData === "website" ? "web" : modelData; tint: Theme.accent0 } }
                                        Item { Layout.fillWidth: true }
                                        Text { text: modelData.scheduleAtMs ? "⏰ " + view.when(modelData.scheduleAtMs) : view.when(modelData.updatedMs || modelData.createdMs); color: Theme.text3; font.pixelSize: Theme.fs10 } }
                                }
                            }
                            Text { anchors.centerIn: parent; visible: !view.items.length; text: "Nothing in this view"; color: Theme.text3; font.pixelSize: Theme.fs11 }
                        }
                    }
                }
                CPanel {
                    Layout.fillWidth: true; Layout.preferredHeight: 600; Layout.alignment: Qt.AlignTop; padding: 0
                    title: view.sel ? "Item " + view.sel.id + (view.sel.revision ? " r" + view.sel.revision : "") : "Item"
                    Text { anchors.centerIn: parent; visible: !view.sel; text: "Select an item"; color: Theme.text3; font.pixelSize: Theme.fs11 }
                    Flickable {
                        anchors.fill: parent; anchors.margins: 10; visible: !!view.sel; clip: true
                        contentWidth: width; contentHeight: det.implicitHeight + 8
                        ScrollBar.vertical: ScrollBar {}
                        ColumnLayout {
                            id: det; width: parent.width; spacing: 6
                            property var it: view.sel || ({})
                            RowLayout { Layout.fillWidth: true; spacing: 6
                                CBadge { text: String(det.it.status || "").replace("_", " "); tint: view.stColor(det.it.status); solid: true }
                                CBadge { visible: det.it.tier === "absolute"; text: "absolute tier"; tint: Theme.onAir; solid: true }
                                CBadge { visible: view.hasAi(det.it); text: "AI-assisted"; tint: Theme.accent2 }
                                CBadge { text: det.it.kind || ""; tint: Theme.text1 }
                                CBadge { text: det.it.source || ""; tint: Theme.text2 }
                                Item { Layout.fillWidth: true }
                                Text { text: det.it.scheduleAtMs ? "scheduled " + view.when(det.it.scheduleAtMs) : ""; color: Theme.accent2; font.pixelSize: Theme.fs11 } }
                            Text { Layout.fillWidth: true; visible: !!det.it.why; text: "Why: " + (det.it.why || ""); color: det.it.tier === "absolute" ? Theme.onAir : Theme.text2; font.pixelSize: Theme.fs11; wrapMode: Text.WordWrap }
                            Text { Layout.fillWidth: true; text: det.it.title || "(no title)"; color: Theme.text0; font.pixelSize: Theme.fs14; font.bold: true; wrapMode: Text.WordWrap }
                            Text { Layout.fillWidth: true; visible: !!det.it.body; text: det.it.body || ""; color: Theme.text1; font.pixelSize: Theme.fs12; wrapMode: Text.WordWrap }
                            Text { Layout.fillWidth: true; visible: !!det.it.caption; text: "Caption: " + (det.it.caption || ""); color: Theme.text1; font.pixelSize: Theme.fs11; wrapMode: Text.WordWrap }
                            Text { Layout.fillWidth: true; visible: !!det.it.linkUrl; text: "Link: " + (det.it.linkUrl || ""); color: Theme.accent0; font.pixelSize: Theme.fs11; font.family: Theme.fontMono; elide: Text.ElideMiddle }
                            RowLayout { Layout.fillWidth: true; visible: !!det.it.mediaId; spacing: 6
                                Text { text: "Media: " + (det.it.mediaId || ""); color: Theme.text1; font.pixelSize: Theme.fs11; font.family: Theme.fontMono }
                                CButton { text: "open"; small: true; variant: "ghost"; onClicked: { window.openMedia(det.it.mediaId, "meta") } } }
                            Text { Layout.fillWidth: true; visible: !!det.it.corrects; text: "Corrects " + (det.it.corrects || ""); color: Theme.clsBumper; font.pixelSize: Theme.fs11 }
                            SecLabel { visible: (det.it.sourceLinks || []).length > 0; text: "SOURCES" }
                            Repeater { model: det.it.sourceLinks || []
                                Text { Layout.fillWidth: true; text: modelData; color: Theme.text2; font.pixelSize: Theme.fs10; font.family: Theme.fontMono; elide: Text.ElideMiddle } }
                            Flow { Layout.fillWidth: true; spacing: 4; visible: (det.it.tags || []).length > 0
                                Repeater { model: det.it.tags || []; CBadge { text: modelData; tint: Theme.text2 } } }
                            SecLabel { text: "CHANNELS" }
                            Repeater { model: det.it.channels || []
                                RowLayout { Layout.fillWidth: true; spacing: 8
                                    property var pc: (det.it.perChannel || {})[modelData] || ({})
                                    CBadge { text: modelData; tint: Theme.accent0; Layout.preferredWidth: 70 }
                                    CBadge { text: pc.state || "pending"; tint: view.chColor(pc.state) }
                                    Text { text: (pc.attempts ? pc.attempts + " attempt(s)" : "") + (pc.postedMs ? "  posted " + view.when(pc.postedMs) : ""); color: Theme.text2; font.pixelSize: Theme.fs10 }
                                    Text { Layout.fillWidth: true; text: pc.error || pc.permalink || pc.remoteId || ""; color: pc.error ? Theme.danger : Theme.text3; font.pixelSize: Theme.fs10; font.family: Theme.fontMono; elide: Text.ElideMiddle } } }
                            Text { Layout.fillWidth: true; visible: !!det.it.metrics; text: det.it.metrics ? "Metrics: " + JSON.stringify(det.it.metrics) : ""; color: Theme.text2; font.pixelSize: Theme.fs10; font.family: Theme.fontMono; wrapMode: Text.WrapAnywhere }

                            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.border0 }
                            SecLabel { text: "ACTIONS" }
                            RowLayout { Layout.fillWidth: true; spacing: 6
                                CField { id: dNote; Layout.fillWidth: true; placeholder: "reason / correction note" }
                                CField { id: dSched; Layout.preferredWidth: 150; mono: true; placeholder: "yyyy-MM-dd HH:mm"; text: Qt.formatDateTime(new Date(Date.now() + 3600000), "yyyy-MM-dd HH:00") } }
                            Flow { Layout.fillWidth: true; spacing: 6
                                CButton { text: "Approve"; small: true; variant: "success"; onClicked: { view.act("approve", {}, "Approved") } }
                                CButton { text: "Hold"; small: true; variant: "warn"; onClicked: { view.act("hold", { reason: dNote.text }, "Held") } }
                                CButton { text: "Skip"; small: true; onClicked: { view.act("skip", {}, "Skipped") } }
                                CButton { text: "Schedule"; small: true; variant: "accent2"; onClicked: { view.schedule() } }
                                CButton { text: "Send now"; small: true; variant: "primary"; onClicked: { view.act("send", {}, "Sending") } }
                                CButton { text: "Retry"; small: true; onClicked: { view.act("retry", {}, "Retrying") } }
                                CButton { text: "Correct"; small: true; enabled: dNote.text.trim().length > 0; onClicked: { view.act("correct", { note: dNote.text }, "Correction created") } ToolTip.text: "Creates a correction row (needs a note)" }
                                CButton { text: "Retract"; small: true; variant: "danger"; enabled: dNote.text.trim().length > 0; onClicked: { view.act("retract", { reason: dNote.text }, "Retracted") } ToolTip.text: "Needs a reason" }
                                CButton { text: "Metrics"; small: true; variant: "ghost"; onClicked: { view.act("metrics", {}, "Metrics refreshed") } }
                                CButton { text: "Revise as draft"; small: true; variant: "ghost"; onClicked: { view.reviseDraft(det.it) } }
                                CButton { text: "Delete"; small: true; variant: "ghost"; enabled: det.it.status !== "published"; onClicked: { api.del("/publish/items/" + encodeURIComponent(det.it.id), function(ok) { if (ok) { view.selId = ""; view.sel = null; view.reload() } }) } }
                            }
                            SecLabel { text: "HISTORY" }
                            Repeater { model: (det.it.history || []).slice().reverse()
                                RowLayout { Layout.fillWidth: true; spacing: 8
                                    Text { text: view.when(modelData.ms); color: Theme.text3; font.pixelSize: Theme.fs10; font.family: Theme.fontMono; Layout.preferredWidth: 80 }
                                    Text { text: (modelData.action || "") + (modelData.revision ? " r" + modelData.revision : ""); color: Theme.text0; font.pixelSize: Theme.fs11; font.bold: true; Layout.preferredWidth: 120; elide: Text.ElideRight }
                                    Text { Layout.fillWidth: true; text: (modelData.actor ? modelData.actor + ": " : "") + (modelData.note || ""); color: Theme.text2; font.pixelSize: Theme.fs11; wrapMode: Text.WordWrap } } }
                        }
                    }
                }
            }

            // ---------------------------------------------------------------- draft + config + audit
            RowLayout {
                Layout.fillWidth: true; spacing: 10
                CPanel {
                    Layout.preferredWidth: 440; Layout.maximumWidth: 480; Layout.alignment: Qt.AlignTop; implicitHeight: 34 + drCol.implicitHeight + 24
                    title: view.draftId ? "Revise draft " + view.draftId : "New draft"
                    headerRight: [ CButton { visible: view.draftId.length > 0; text: "new instead"; small: true; variant: "ghost"; onClicked: { view.clearDraft() } } ]
                    ColumnLayout {
                        id: drCol; anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; spacing: 6
                        RowLayout { Layout.fillWidth: true; spacing: 6
                            CSelect { id: nKind; label: "Kind"; Layout.preferredWidth: 110; model: ["text", "article", "link", "spot", "video", "slate"] }
                            CField { id: nTitle; label: "Title"; Layout.fillWidth: true } }
                        AreaField { id: nBody; label: "Body"; Layout.fillWidth: true; areaHeight: 90 }
                        CField { id: nCaption; label: "Caption"; Layout.fillWidth: true }
                        RowLayout { Layout.fillWidth: true; spacing: 6
                            CField { id: nLink; label: "Link URL"; Layout.fillWidth: true; mono: true }
                            CField { id: nMedia; label: "Media id"; Layout.preferredWidth: 130; mono: true } }
                        RowLayout { Layout.fillWidth: true; spacing: 6
                            SecLabel { text: "CHANNELS" }
                            CButton { text: "facebook"; small: true; active: view.draftChannels.indexOf("facebook") >= 0; onClicked: { view.toggleDraftChannel("facebook") } }
                            CButton { text: "website"; small: true; active: view.draftChannels.indexOf("website") >= 0; onClicked: { view.toggleDraftChannel("website") } }
                            Item { Layout.fillWidth: true }
                            CToggle { id: nAi; label: "AI wrote the text"; implicitWidth: 150 } }
                        AreaField { id: nSources; label: "Source links (one per line)"; Layout.fillWidth: true; areaHeight: 56; mono: true; placeholder: "https://…" }
                        CField { id: nTags; label: "Tags (comma list)"; Layout.fillWidth: true }
                        CButton { text: view.draftId ? "Save revision" : "Create draft"; variant: "primary"; Layout.alignment: Qt.AlignRight; enabled: view.draftChannels.length > 0 && (nTitle.text.trim().length > 0 || nBody.text.trim().length > 0 || nCaption.text.trim().length > 0); onClicked: { view.submitDraft() } }
                    }
                }
                CPanel {
                    Layout.fillWidth: true; Layout.alignment: Qt.AlignTop; implicitHeight: 34 + cfCol.implicitHeight + 24
                    title: "Config"
                    headerRight: [ CButton { text: "Verify Facebook"; small: true; onClicked: { api.post("/publish/verify", {}, function(ok, d) { view.verifyText = (ok ? "✓ " : "✕ ") + (typeof d === "string" ? d : JSON.stringify(d)); if (ok) window.toast("Facebook verified" + (d && d.name ? ": " + d.name : ""), "ok") }) } } ]
                    ColumnLayout {
                        id: cfCol; anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; spacing: 6
                        property var fb: view.cfg.facebook || ({})
                        property var web: view.cfg.website || ({})
                        RowLayout { Layout.fillWidth: true; spacing: 6
                            CSelect { id: cfMode; label: "Mode"; Layout.preferredWidth: 110; model: ["off", "review", "auto"]; currentIndex: Math.max(0, model.indexOf(view.cfg.mode || "off")) }
                            CField { id: cfRecipes; label: "Recipes (comma list)"; Layout.fillWidth: true; text: (view.cfg.recipes || []).join(", ") }
                            CField { id: cfMax; label: "Max / day"; Layout.preferredWidth: 80; mono: true; text: String(view.cfg.maxPerDay !== undefined ? view.cfg.maxPerDay : "") } }
                        CField { id: cfTagline; label: "Tagline"; Layout.fillWidth: true; text: view.cfg.tagline || "" }
                        CField { id: cfAiLabel; label: "AI label"; Layout.fillWidth: true; text: view.cfg.aiLabel || ""; placeholder: "Made with AI assistance." }
                        SecLabel { text: "FACEBOOK" }
                        RowLayout { Layout.fillWidth: true; spacing: 6
                            CField { id: cfPage; label: "Page id"; Layout.fillWidth: true; mono: true; text: cfCol.fb.pageId || "" }
                            PassField { id: cfFbToken; label: "Page token"; Layout.fillWidth: true; placeholder: cfCol.fb.hasToken ? "(set) — blank keeps it" : "(not set)" } }
                        CField { id: cfGraph; label: "Graph base"; Layout.fillWidth: true; mono: true; text: view.cfg.graphBase || ""; placeholder: "https://graph.facebook.com/v19.0" }
                        SecLabel { text: "WEBSITE (WORDPRESS)" }
                        RowLayout { Layout.fillWidth: true; spacing: 6
                            CField { id: cfWebUrl; label: "URL"; Layout.fillWidth: true; mono: true; text: cfCol.web.url || "" }
                            CField { id: cfWebUser; label: "User"; Layout.preferredWidth: 120; text: cfCol.web.user || "" } }
                        RowLayout { Layout.fillWidth: true; spacing: 6
                            PassField { id: cfWebKey; label: "App password / key"; Layout.fillWidth: true; placeholder: cfCol.web.hasKey ? "(set) — blank keeps it" : "(not set)" }
                            CToggle { id: cfWebAuto; label: "Auto-publish (else draft)"; implicitWidth: 190; Layout.alignment: Qt.AlignBottom; checked: !!cfCol.web.autoPublish } }
                        Text { Layout.fillWidth: true; visible: view.verifyText.length > 0; text: view.verifyText; color: view.verifyText.indexOf("✓") === 0 ? Theme.ready : Theme.danger; font.pixelSize: Theme.fs10; font.family: Theme.fontMono; wrapMode: Text.WrapAnywhere }
                        CButton { text: "Save config"; variant: "primary"; Layout.alignment: Qt.AlignRight; onClicked: { view.saveCfg() } }
                    }
                }
                CPanel {
                    Layout.preferredWidth: 360; Layout.maximumWidth: 400; Layout.preferredHeight: 520; Layout.alignment: Qt.AlignTop; padding: 0
                    title: "Audit (" + view.audit.length + ")"
                    ListView {
                        id: auList
                        anchors.fill: parent; clip: true; model: view.audit
                        ScrollBar.vertical: ScrollBar {}
                        delegate: Rectangle {
                            width: auList.width; height: auc.implicitHeight + 8; color: index % 2 ? Theme.bg2 : Theme.bg3
                            ColumnLayout { id: auc; anchors.left: parent.left; anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; anchors.leftMargin: 10; anchors.rightMargin: 8; spacing: 1
                                RowLayout { Layout.fillWidth: true; spacing: 6
                                    Text { text: view.when(modelData.ms); color: Theme.text3; font.pixelSize: Theme.fs10; font.family: Theme.fontMono }
                                    Text { text: modelData.action || ""; color: Theme.text0; font.pixelSize: Theme.fs11; font.bold: true }
                                    Item { Layout.fillWidth: true }
                                    Text { text: modelData.actor || ""; color: Theme.text2; font.pixelSize: Theme.fs10 } }
                                Text { Layout.fillWidth: true; visible: text.length > 0; text: (modelData.itemId || modelData.id || "") + (modelData.note || modelData.reason ? "  " + (modelData.note || modelData.reason) : ""); color: Theme.text2; font.pixelSize: Theme.fs10; elide: Text.ElideRight } }
                        }
                        Text { anchors.centerIn: parent; visible: !view.audit.length; text: "No audit entries"; color: Theme.text3; font.pixelSize: Theme.fs11 }
                    }
                }
            }
        }
    }
}
