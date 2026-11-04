import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".."
import "../ui"

/// Tools: media ingest (folder scan with ffprobe), media maintenance, engine control.
Item {
    id: view
    property var media: []
    property string scanStatus: ""
    property int pending: 0
    property var watch: []
    property var icfg: ({})
    property var tcfg: ({})
    property var levels: []
    property var tjobs: []
    property var env: ({})
    function reloadMedia() { api.get("/media", function(ok, d) { if (ok) view.media = d.media || [] }) }
    function reloadIngest() {
        api.get("/ingest/watch", function(ok, d) { if (ok) view.watch = d.folders || [] })
        api.get("/ingest/config", function(ok, d) { if (ok) view.icfg = d })
        api.get("/transcribe/levels", function(ok, d) { if (ok) { view.levels = d.levels || []; view.tcfg = d.config || {} } })
        api.get("/transcribe/jobs", function(ok, d) { if (ok) view.tjobs = d.jobs || [] })
    }
    function saveWatch(list) { api.put("/ingest/watch", { folders: list }, function(ok, d) { if (ok) view.watch = d.folders || [] }) }
    function addWatch(p, t) { var l = view.watch.slice(); l.push({ path: p, type: t, recursive: true, enabled: true }); saveWatch(l) }
    function setWatch(i, k, v) { var l = view.watch.slice(); l[i][k] = v; saveWatch(l) }
    function delWatch(i) { var l = view.watch.slice(); l.splice(i, 1); saveWatch(l) }
    Component.onCompleted: { reloadMedia(); reloadIngest() }
    Connections { target: supervisor; function onUiEvent(t, p) { if (t === "transcribe") view.tjobs = p.jobs || []; if (t === "mediaIngested") view.reloadMedia() } }
    Connections { target: supervisor; function onMediaScanDone() { view.scanStatus = "Scan finished"; view.pending = 0; view.reloadMedia() } }

    RowLayout {
        anchors.fill: parent
        spacing: 10
        ColumnLayout {
            // Bounded width: the discovered-sources Flow would otherwise report its unwrapped width and
            // starve the media table (the layout then aborts with "recursive rearrange").
            Layout.preferredWidth: 560; Layout.minimumWidth: 460; Layout.maximumWidth: 620; Layout.fillHeight: true; spacing: 10
            CPanel {
                Layout.fillWidth: true; implicitHeight: 34 + scanCol.implicitHeight + 24
                title: "Ingest \u2014 scan a folder"
                ColumnLayout {
                    id: scanCol; anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; spacing: 8
                    CField { id: folder; label: "Folder"; Layout.fillWidth: true; placeholder: "D:\\Media\\Programs"; text: "" }
                    RowLayout {
                        Layout.fillWidth: true; spacing: 8
                        CSelect { id: scanType; label: "Import as"; Layout.preferredWidth: 150; model: ["program", "ad", "promo", "station_id", "bumper", "filler", "psa"] }
                        CToggle { id: recursive; label: "Recursive"; checked: true; Layout.alignment: Qt.AlignBottom; implicitWidth: 110 }
                        Item { Layout.fillWidth: true }
                        CButton { text: "Scan"; icon2: "\u2315"; variant: "primary"; Layout.alignment: Qt.AlignBottom; enabled: folder.text.length > 0
                            onClicked: { api.post("/media/scan", { folder: folder.text, recursive: recursive.checked, type: scanType.currentText }, function(ok, d) { if (ok) { view.pending = d.pending; view.scanStatus = "Queued " + d.queued + " file(s), " + d.pending + " pending" } }) } }
                    }
                    Text { text: view.scanStatus; color: Theme.text2; font.pixelSize: Theme.fs11 }
                    Text { Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.text3; font.pixelSize: Theme.fs10
                        text: "Each file is probed with ffprobe on the Supervisor (duration, raster, frame rate, codecs) and added to the media table. Rescans update changed files in place." }
                }
            }
            CPanel {
                Layout.fillWidth: true; implicitHeight: 34 + liveCol.implicitHeight + 24
                title: "Live sources \u2014 register for the library"
                ColumnLayout {
                    id: liveCol; anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; spacing: 8
                    RowLayout {
                        Layout.fillWidth: true; spacing: 8
                        CSelect { id: lsKind; label: "Kind"; Layout.preferredWidth: 110; model: ["ndi", "sdi_in", "stream"] }
                        CField { id: lsRef; label: lsKind.currentText === "ndi" ? "NDI name" : lsKind.currentText === "sdi_in" ? "SDI port" : "URL"; Layout.fillWidth: true }
                        CField { id: lsTitle; label: "Title"; Layout.preferredWidth: 180 }
                        CField { id: lsDur; label: "Default secs"; Layout.preferredWidth: 100; mono: true; text: "1800" }
                        CButton { text: "Register"; variant: "primary"; Layout.alignment: Qt.AlignBottom; enabled: lsRef.text.length > 0
                            onClicked: {
                                var src = { type: lsKind.currentText }
                                if (lsKind.currentText === "ndi") src.name = lsRef.text; else if (lsKind.currentText === "sdi_in") src.port = parseInt(lsRef.text) || 1; else { src.url = lsRef.text; src.realtime = true }
                                api.post("/media", { path: "live://" + lsKind.currentText + "/" + lsRef.text, title: lsTitle.text || (lsKind.currentText.toUpperCase() + " " + lsRef.text), type: "live", duration: parseFloat(lsDur.text) || 1800, liveSource: src }, function(ok) { if (ok) { window.toast("Live source registered", "ok"); view.reloadMedia() } })
                            } }
                    }
                    Flow { Layout.fillWidth: true; spacing: 4
                        Text { text: "Discovered:"; color: Theme.text3; font.pixelSize: Theme.fs10 }
                        Repeater { model: supervisor.activeState.liveInputs || []; CButton { text: modelData.name; small: true; variant: "ghost"; onClicked: { lsKind.currentIndex = 0; lsRef.text = modelData.name; lsTitle.text = modelData.name } } } }
                }
            }
            CPanel {
                Layout.fillWidth: true; implicitHeight: 34 + watchCol.implicitHeight + 24
                title: "Watch folders (" + view.watch.length + ")"
                headerRight: [ CButton { text: "rescan"; small: true; variant: "ghost"; onClicked: { api.post("/ingest/rescan", {}, function(ok) { if (ok) window.toast("Rescan requested", "ok") }) } } ]
                ColumnLayout {
                    id: watchCol; anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; spacing: 6
                    Repeater {
                        model: view.watch
                        RowLayout { Layout.fillWidth: true; spacing: 6
                            CToggle { checked: modelData.enabled !== false; implicitWidth: 44; onToggled: function(v) { view.setWatch(index, "enabled", v) } }
                            Text { Layout.fillWidth: true; text: modelData.path; color: Theme.text0; font.pixelSize: Theme.fs11; font.family: Theme.fontMono; elide: Text.ElideMiddle }
                            CBadge { text: modelData.type || "program"; tint: Theme.typeColor(modelData.type || "program") }
                            CButton { text: "\u2715"; small: true; variant: "ghost"; onClicked: { view.delWatch(index) } } }
                    }
                    RowLayout { Layout.fillWidth: true; spacing: 6
                        CField { id: wfPath; Layout.fillWidth: true; placeholder: "D:\\Ingest\\Drop  (files are imported once they stop growing)"; mono: true }
                        CSelect { id: wfType; Layout.preferredWidth: 110; model: ["program", "ad", "promo", "station_id", "bumper", "filler", "psa"] }
                        CButton { text: "+ watch"; small: true; variant: "primary"; enabled: wfPath.text.length > 0; onClicked: { view.addWatch(wfPath.text, wfType.currentText); wfPath.text = "" } } }
                    RowLayout { Layout.fillWidth: true; spacing: 10
                        CToggle { label: "Removable drives (PLAYPLAY_INGEST\\)"; implicitWidth: 220; checked: !!(view.icfg.removable && view.icfg.removable.enabled !== false); onToggled: function(v) { api.put("/ingest/config", { removable: Object.assign({}, view.icfg.removable || {}, { enabled: v }) }, function(ok, d) { if (ok) view.icfg = d }) } }
                        CToggle { label: "Thumbnails"; implicitWidth: 100; checked: view.icfg.thumbnails !== false; onToggled: function(v) { api.put("/ingest/config", { thumbnails: v }, function(ok, d) { if (ok) view.icfg = d }) } }
                        CToggle { label: "Review queue"; implicitWidth: 110; checked: view.icfg.review === "require"; onToggled: function(v) { api.put("/ingest/config", { review: v ? "require" : "auto" }, function(ok, d) { if (ok) view.icfg = d }) } }
                        Item { Layout.fillWidth: true }
                        CButton { text: "sweep missing"; small: true; variant: "ghost"; onClicked: { api.post("/ingest/sweep", {}, function(ok, d) { if (ok) { window.toast(d.missing + " missing file(s)", d.missing ? "warn" : "ok"); view.reloadMedia() } }) } } }
                }
            }
            CPanel {
                Layout.fillWidth: true; implicitHeight: 34 + trCol.implicitHeight + 24
                title: "Transcription (faster-whisper)"
                ColumnLayout {
                    id: trCol; anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; spacing: 6
                    RowLayout { Layout.fillWidth: true; spacing: 8
                        CSelect { id: trLevel; label: "Default accuracy"; Layout.preferredWidth: 200; model: view.levels; textRole: "id"; currentIndex: { for (var i = 0; i < view.levels.length; ++i) if (view.levels[i].id === view.tcfg.defaultLevel) return i; return 1 } onActivated: function(i) { api.put("/transcribe/config", { defaultLevel: view.levels[i].id }, function(ok, d) { if (ok) view.tcfg = d }) } }
                        CToggle { label: "Auto-transcribe on ingest"; implicitWidth: 190; Layout.alignment: Qt.AlignBottom; checked: !!view.tcfg.autoTranscribe; onToggled: function(v) { api.put("/transcribe/config", { autoTranscribe: v }, function(ok, d) { if (ok) view.tcfg = d }) } }
                        CButton { text: "check"; small: true; variant: "ghost"; Layout.alignment: Qt.AlignBottom; onClicked: { api.post("/transcribe/probe", {}, function(ok, e) { view.env = e || {}; window.toast(e && e.ok ? "faster-whisper " + e.fasterWhisper + " ready (" + (e.cudaDevices ? "GPU" : "CPU") + ")" : (e ? e.error : "probe failed"), e && e.ok ? "ok" : "error") }) } } }
                    Text { Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.text3; font.pixelSize: Theme.fs10; text: (trLevel.currentIndex >= 0 && view.levels[trLevel.currentIndex] ? view.levels[trLevel.currentIndex].model + " \u2014 " + view.levels[trLevel.currentIndex].note : "") + (view.env.cachedModels ? "   cached: " + view.env.cachedModels.join(", ") : "") }
                    Repeater { model: view.tjobs.slice(-5).reverse()
                        RowLayout { Layout.fillWidth: true; spacing: 8
                            Text { Layout.preferredWidth: 220; text: modelData.title; color: Theme.text0; font.pixelSize: Theme.fs11; elide: Text.ElideRight }
                            Text { text: modelData.level; color: Theme.text2; font.pixelSize: Theme.fs10 }
                            Rectangle { Layout.fillWidth: true; height: 6; radius: 2; color: Theme.bg4; Rectangle { height: parent.height; radius: 2; width: parent.width * (modelData.state === "done" ? 1 : (modelData.pct || 0) / 100); color: modelData.state === "failed" ? Theme.danger : modelData.state === "done" ? Theme.ready : Theme.accent0 } }
                            CBadge { text: modelData.state; tint: modelData.state === "done" ? Theme.ready : modelData.state === "failed" ? Theme.danger : Theme.accent0; ToolTip.text: modelData.error || "" } } }
                }
            }
            CPanel {
                Layout.fillWidth: true; implicitHeight: 34 + engCol.implicitHeight + 24
                title: "Engine \u2014 " + supervisor.activeChannelId
                ColumnLayout {
                    id: engCol; anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; spacing: 8
                    RowLayout { spacing: 6
                        CButton { text: "Restart engine"; onClicked: { api.post("/channels/" + supervisor.activeChannelId + "/restart", {}) } }
                        CButton { text: "Reload profile"; onClicked: { supervisor.command("reload") } }
                        CButton { text: "Resync hour"; onClicked: { supervisor.loadHour() } } }
                    RowLayout { spacing: 6
                        CButton { text: "GPU compositing"; small: true; variant: "ghost"; active: !!(supervisor.activeState.compositor && supervisor.activeState.compositor.gpu); onClicked: { supervisor.command("config", { gpu: !(supervisor.activeState.compositor && supervisor.activeState.compositor.gpu) }) } }
                        CButton { text: "Slate captions\u2026"; small: true; variant: "ghost"; onClicked: { supervisor.command("config", { slateCaptions: [ (supervisor.channelOf(supervisor.activeChannelId).profile || {}).name || "PLAY PLAY", "STAND BY" ] }) } } }
                    Text { Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.text3; font.pixelSize: Theme.fs10; text: "Restart kills the channel engine; the Supervisor respawns it and re-pushes the hour with join-in-progress." }
                }
            }
            Item { Layout.fillHeight: true }
        }
        CPanel {
            Layout.fillWidth: true; Layout.fillHeight: true; padding: 0
            title: "Media table (" + view.media.length + ")"
            headerRight: [ CButton { icon2: "\u21BB"; small: true; variant: "ghost"; onClicked: { view.reloadMedia() } } ]
            ListView {
                id: list
                anchors.fill: parent; clip: true; model: view.media
                ScrollBar.vertical: ScrollBar {}
                delegate: Rectangle {
                    width: list.width; height: 34; color: index % 2 ? Theme.bg2 : Theme.bg3
                    RowLayout { anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 8; spacing: 8
                        Text { text: modelData.title; color: Theme.text0; font.pixelSize: Theme.fs11; elide: Text.ElideRight; Layout.preferredWidth: 220 }
                        Text { text: modelData.path; color: Theme.text3; font.family: Theme.fontMono; font.pixelSize: Theme.fs10; elide: Text.ElideMiddle; Layout.fillWidth: true }
                        Text { text: Theme.fmtClock(modelData.duration); color: Theme.text1; font.family: Theme.fontMono; font.pixelSize: Theme.fs10; Layout.preferredWidth: 50 }
                        ComboBox {
                            Layout.preferredWidth: 110; implicitHeight: 24; font.pixelSize: Theme.fs10
                            model: ["program", "ad", "promo", "station_id", "bumper", "filler", "psa", "live", "video"]
                            currentIndex: Math.max(0, model.indexOf(modelData.type))
                            onActivated: function(i) { api.put("/media/" + modelData.id, { type: model[i] }, function(ok) { if (ok) window.toast(modelData.title + " \u2192 " + model[i], "ok") }) }
                            background: Rectangle { color: Theme.bg4; radius: Theme.r3; border.color: Theme.border1 }
                            contentItem: Text { leftPadding: 6; text: parent.displayText; color: Theme.typeColor(parent.displayText); font.pixelSize: Theme.fs10; verticalAlignment: Text.AlignVCenter }
                        }
                        CButton { icon2: "\u270E"; small: true; variant: "ghost"; onClicked: { window.openMedia(modelData.id, "meta") } ToolTip.text: "Edit" }
                        CButton { icon2: modelData.archived ? "\u21A9" : "\u2612"; small: true; variant: "ghost"; onClicked: { api.put("/media/" + modelData.id, { archived: !modelData.archived }, function(ok) { if (ok) view.reloadMedia() }) } ToolTip.text: modelData.archived ? "Unarchive" : "Archive" }
                        CButton { icon2: "\u2715"; small: true; variant: "ghost"; onClicked: { api.del("/media/" + modelData.id, function(ok) { if (ok) view.reloadMedia() }) } ToolTip.text: "Remove from library" } }
                }
                Text { anchors.centerIn: parent; visible: view.media.length === 0; text: "Media table is empty"; color: Theme.text3; font.pixelSize: Theme.fs11 }
            }
        }
    }
}
