import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".."

/**
 * Media library (Supervisor media table): search, type filter, list with type
 * badge, duration and play count. Emits `picked(media)` for the slot editor and
 * `take(media)` for an immediate on-air take.
 */
CPanel {
    id: lib
    title: "Media Library"
    padding: 0
    property var media: []
    property string query: ""
    property string typeFilter: ""
    property string sort: "recent"
    property bool loading: false
    property int dropCount: 0
    signal picked(var media)
    signal take(var media)
    signal pickedFolder(var media)      // "add this file's folder as a folder block"
    property var menuMedia: null
    property bool gridView: false
    function preview(m) {
        if (!m || !m.path || m.type === "live" || !api.openMediaFile(m.path))
            window.toast("Cannot open this file locally. Check that the media is available on this computer.", "error")
    }
    Menu {
        id: rowMenu
        MenuItem { text: "Add to the open hour"; onTriggered: { lib.picked(lib.menuMedia) } }
        MenuItem { text: "Add folder block (this file's folder)"; onTriggered: { lib.pickedFolder(lib.menuMedia) } }
        MenuItem { text: "TAKE now"; onTriggered: { lib.take(lib.menuMedia) } }
        MenuSeparator {}
        MenuItem { text: "Preview file"; onTriggered: { lib.preview(lib.menuMedia) } }
        MenuItem { text: "Edit metadata, transcript, segments"; onTriggered: { window.openMedia(lib.menuMedia.id, "meta") } }
        MenuItem { text: "Edit segments"; onTriggered: { window.openMedia(lib.menuMedia.id, "segments") } }
        MenuItem { text: "Asset report"; onTriggered: { Qt.openUrlExternally(api.rawUrl("/reports/asset/" + lib.menuMedia.id + ".html")) } }
        // POST /media/{id}/transcribe {level}: the same route MediaEditor and the web library use (/transcribe/jobs is GET-only: it lists the queue)
        MenuItem { text: "Transcribe (fast)"; onTriggered: { api.post("/media/" + lib.menuMedia.id + "/transcribe", { level: "fast" }, function(ok) { if (ok) window.toast("Transcription queued", "ok") }) } }
        MenuItem { text: "Reveal in Explorer"; onTriggered: { api.revealInExplorer(lib.menuMedia.path) } }
        MenuSeparator {}
        MenuItem { text: "Copy title"; onTriggered: { window.copyText(lib.menuMedia.title || "") } }
        MenuItem { text: "Copy path"; onTriggered: { window.copyText(lib.menuMedia.path || "") } }
        MenuItem { text: "Copy media id"; onTriggered: { window.copyText(lib.menuMedia.id) } }
        MenuItem { text: "Copy as playlist item"; onTriggered: { var m = lib.menuMedia; window.setClip("items", [{ id: "c", mediaId: m.id, title: m.title, path: m.path, type: m.type || "video", duration: m.duration || 0, timeIn: 0, timeOut: 0, timingMode: "soft_start", transitionType: "cut", priority: 0 }], m.title) } }
        MenuSeparator {}
        Menu { title: "Set type"
            Repeater { model: ["program", "ad", "promo", "psa", "station_id", "bumper", "filler", "news", "weather", "image"]
                MenuItem { text: modelData; onTriggered: { api.put("/media/" + lib.menuMedia.id, { type: modelData }, function(ok) { if (ok) { window.toast("Type set", "ok"); lib.reload() } }) } } } }
        MenuItem { text: lib.menuMedia && lib.menuMedia.archived ? "Unarchive" : "Archive"; onTriggered: { api.put("/media/" + lib.menuMedia.id, { archived: !lib.menuMedia.archived }, function(ok) { if (ok) lib.reload() }) } }
        MenuItem { text: "Remove from library"; onTriggered: { api.del("/media/" + lib.menuMedia.id, function(ok) { if (ok) { window.toast("Removed (file kept)", "ok"); lib.reload() } }) } }
    }
    property var filtered: {
        var out = []
        var q = query.toLowerCase()
        for (var i = 0; i < media.length; ++i) {
            var m = media[i]
            if (typeFilter === "missing") { if (!m.missing) continue } else if (typeFilter === "review") { if (m.status !== "review") continue } else if (typeFilter.length && m.type !== typeFilter) continue
            if (q.length && String(m.title || "").toLowerCase().indexOf(q) < 0 && String(m.programSeries || "").toLowerCase().indexOf(q) < 0 && String(m.path || "").toLowerCase().indexOf(q) < 0 && String(m.client || "").toLowerCase().indexOf(q) < 0) continue
            out.push(m)
        }
        var s = sort
        out.sort(function(a, b) {
            if (s === "title") return String(a.title || "").localeCompare(String(b.title || ""))
            if (s === "duration") return (b.duration || 0) - (a.duration || 0)
            if (s === "plays") return (b.playCount || 0) - (a.playCount || 0)
            if (s === "series") return String(a.programSeries || "").localeCompare(String(b.programSeries || "")) || String(b.episodeDate || "").localeCompare(String(a.episodeDate || ""))
            return (b.addedAtMs || 0) - (a.addedAtMs || 0)
        })
        return out
    }
    property real totalSec: { var t = 0; for (var i = 0; i < filtered.length; ++i) t += filtered[i].duration || 0; return t }
    function reload() {
        loading = true
        api.get("/media", function(ok, data) { loading = false; if (ok) lib.media = data.media || [] })
    }
    Component.onCompleted: { reload(); window.registerDropTarget(lib) }
    Component.onDestruction: { window.unregisterDropTarget(lib) }
    property string dropLabel: "Import into the library" + (lib.typeFilter.length ? " as " + lib.typeFilter : "")
    function dropPaths(paths, x, y) {
        api.post("/media/scan", { files: paths, type: lib.typeFilter.length ? lib.typeFilter : "program" }, function(ok, r) { if (ok) window.toast("Importing " + r.queued + " file(s)", "ok"); else window.toast("Import failed", "error") })
        lib.dropCount++
    }
    Connections { target: supervisor; function onMediaScanDone() { lib.reload() } function onUiEvent(t, p) { if (t === "mediaIngested" || t === "transcriptReady") lib.reload() } }
    Connections { target: mediaEditor; function onSaved(id) { lib.reload() } }
    headerRight: [
        Text { text: lib.filtered.length + " items · " + Theme.fmtDur(lib.totalSec); color: Theme.text3; font.pixelSize: Theme.fs10 },
        CButton { icon2: "↻"; small: true; variant: "ghost"; onClicked: { lib.reload() } ToolTip.text: "Reload" }
    ]

    // Drag files or folders from Windows Explorer onto the library to import them.
    DropArea {
        id: drop
        anchors.fill: parent
        z: 10
        keys: ["text/uri-list"]
        onDropped: function(d) {
            var files = []
            for (var i = 0; i < d.urls.length; ++i) files.push(String(d.urls[i]))
            if (!files.length) return
            api.post("/media/scan", { files: files, type: lib.typeFilter.length ? lib.typeFilter : "program" }, function(ok, r) { if (ok) window.toast("Importing " + r.queued + " file(s)", "ok") })
            lib.dropCount++
        }
        Rectangle { anchors.fill: parent; visible: drop.containsDrag; color: Qt.rgba(0, 0.83, 1, 0.12); border.color: Theme.accent0; border.width: 2; radius: Theme.r8
            Text { anchors.centerIn: parent; text: "Drop to import into the library"; color: Theme.accent0; font.pixelSize: Theme.fs14; font.bold: true } }
    }
    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        Rectangle {
            Layout.fillWidth: true; implicitHeight: 96; color: Theme.bg2
            ColumnLayout {
                anchors.fill: parent; anchors.margins: 8; spacing: 6
                TextField {
                    Layout.fillWidth: true; implicitHeight: 28
                    placeholderText: "Search title, series or filename…"; placeholderTextColor: Theme.text3; color: Theme.text0; font.pixelSize: Theme.fs12
                    background: Rectangle { color: Theme.bg4; radius: Theme.r5; border.color: parent.activeFocus ? Theme.accent0 : Theme.border1 }
                    leftPadding: 8; onTextChanged: { lib.query = text }
                }
                Flow {
                    Layout.fillWidth: true; spacing: 4
                    Repeater {
                        model: [ ["", "All"], ["program", "Programs"], ["ad", "Ads"], ["promo", "Promos"], ["station_id", "IDs"], ["bumper", "Bumpers"], ["filler", "Filler"], ["image", "Images"], ["live", "Live"], ["missing", "Missing"], ["review", "Review"] ]
                        CButton { text: modelData[1]; small: true; variant: "ghost"; active: lib.typeFilter === modelData[0]; onClicked: { lib.typeFilter = modelData[0] } }
                    }
                }
                RowLayout {
                    Layout.fillWidth: true; spacing: 4
                    Text { text: "SORT"; color: Theme.text3; font.pixelSize: Theme.fs10; font.bold: true }
                    Repeater {
                        model: [ ["recent", "Recent"], ["title", "A-Z"], ["series", "Series"], ["duration", "Length"], ["plays", "Plays"] ]
                        CButton { text: modelData[1]; small: true; variant: "ghost"; active: lib.sort === modelData[0]; onClicked: { lib.sort = modelData[0] } }
                    }
                    Item { Layout.fillWidth: true }
                    CButton { text: "List"; small: true; variant: "ghost"; active: !lib.gridView; onClicked: lib.gridView = false }
                    CButton { text: "Grid"; small: true; variant: "ghost"; active: lib.gridView; onClicked: lib.gridView = true }
                    Text { text: "drop files here to import"; color: Theme.text3; font.pixelSize: Theme.fs10 }
                }
            }
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.border0 }
        }
        ListView {
            id: list
            visible: !lib.gridView
            Layout.fillWidth: true; Layout.fillHeight: true
            clip: true
            model: lib.filtered
            ScrollBar.vertical: ScrollBar {}
            delegate: Rectangle {
                width: list.width; height: 46
                color: ma.containsMouse ? Theme.bg4 : index % 2 ? Theme.bg2 : Theme.bg3
                Rectangle { width: 3; height: parent.height; color: Theme.typeColor(modelData.type) }
                RowLayout {
                    anchors.fill: parent; anchors.leftMargin: 10; anchors.rightMargin: 8; spacing: 8
                    Item { Layout.preferredWidth: 20; Layout.preferredHeight: 30
                        Item { id: mediaGhost; width: 20; height: 30; property var dragMedia: modelData; Drag.active: mediaHandle.drag.active; Drag.source: mediaGhost; Drag.keys: ["playplay-media"]; Drag.hotSpot.x: 10; Drag.hotSpot.y: 15
                            Text { anchors.centerIn: parent; text: "⋮⋮"; color: Theme.text2 }
                        }
                        MouseArea { id: mediaHandle; anchors.fill: parent; drag.target: mediaGhost; cursorShape: Qt.SizeAllCursor; onReleased: { mediaGhost.Drag.drop(); mediaGhost.x = 0; mediaGhost.y = 0 } }
                    }
                    Rectangle { width: 40; height: 24; radius: 2; color: "#000"; border.color: Theme.border1
                        Image { anchors.fill: parent; anchors.margins: 1; source: modelData.thumbnail ? api.rawUrl("/thumbs/" + modelData.id + ".jpg") : ""; fillMode: Image.PreserveAspectCrop; asynchronous: true; cache: true }
                        Text { anchors.centerIn: parent; visible: !modelData.thumbnail; text: modelData.type === "live" ? "LIVE" : "▶"; color: Theme.text3; font.pixelSize: 9 }
                        MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: lib.preview(modelData) } }
                    ColumnLayout {
                        Layout.fillWidth: true; spacing: 0
                        Text { text: modelData.title || modelData.path; color: Theme.text0; font.pixelSize: Theme.fs12; elide: Text.ElideRight; Layout.fillWidth: true }
                        RowLayout { spacing: 6
                            CBadge { text: modelData.type || "video"; tint: Theme.typeColor(modelData.type) }
                            CBadge { visible: !!modelData.missing; text: "missing"; tint: Theme.danger }
                            CBadge { visible: !!(modelData.status && modelData.status !== "approved"); text: modelData.status || ""; tint: modelData.status === "review" ? Theme.warn : Theme.danger }
                            CBadge { visible: !!(modelData.transcript && modelData.transcript.status === "done"); text: "T"; tint: Theme.ready; ToolTip.text: "transcribed (" + (modelData.transcript ? modelData.transcript.level : "") + ")" }
                            CBadge { visible: !!(modelData.segments && modelData.segments.length); text: (modelData.segments ? modelData.segments.length : 0) + " seg"; tint: Theme.accent2 }
                            Text { text: modelData.type === "live" && modelData.liveSource ? (modelData.liveSource.type + " " + (modelData.liveSource.name || modelData.liveSource.url || ("port " + modelData.liveSource.port))) : Theme.fmtClock(modelData.duration) + (modelData.programSeries ? "  ·  " + modelData.programSeries : "") + (modelData.width ? "  ·  " + modelData.width + "x" + modelData.height : ""); color: Theme.text2; font.pixelSize: Theme.fs10; elide: Text.ElideRight; Layout.fillWidth: true }
                            Text { text: (modelData.playCount || 0) + " plays"; color: Theme.text3; font.pixelSize: Theme.fs10 } }
                    }
                    CButton { text: "▶"; small: true; variant: "ghost"; onClicked: lib.preview(modelData); ToolTip.text: "Preview file" }
                    CButton { icon2: "\u270E"; small: true; variant: "ghost"; onClicked: { window.openMedia(modelData.id, "meta") } ToolTip.text: "Edit metadata, transcript and segments" }
                    CButton { icon2: "\u2197"; small: true; variant: "ghost"; onClicked: { api.revealInExplorer(modelData.path) } ToolTip.text: "Reveal in Explorer" }
                    CButton { icon2: "+"; small: true; variant: "ghost"; onClicked: { lib.picked(modelData) } ToolTip.text: "Add to the open hour" }
                    CButton { text: "TAKE"; small: true; variant: "ghost"; onClicked: { lib.take(modelData) } ToolTip.text: "Play now on the active channel" }
                }
                MouseArea { id: ma; anchors.fill: parent; hoverEnabled: true; acceptedButtons: Qt.LeftButton | Qt.RightButton; z: -1
                    onClicked: function(m) { if (m.button === Qt.RightButton) { lib.menuMedia = modelData; rowMenu.popup() } }
                    onDoubleClicked: function(m) { if (m.button === Qt.LeftButton) lib.preview(modelData) } }
            }
            Text { anchors.centerIn: parent; visible: lib.filtered.length === 0; text: lib.loading ? "Loading…" : "No media. Tools → Scan folder."; color: Theme.text3; font.pixelSize: Theme.fs11 }
        }
        GridView {
            id: grid
            visible: lib.gridView; Layout.fillWidth: true; Layout.fillHeight: true; clip: true
            cellWidth: 180; cellHeight: 158; model: lib.filtered
            ScrollBar.vertical: ScrollBar {}
            delegate: Rectangle {
                width: grid.cellWidth - 8; height: grid.cellHeight - 8; radius: Theme.r5; color: Theme.bg3; border.color: Theme.border1
                MouseArea { anchors.fill: parent; acceptedButtons: Qt.LeftButton | Qt.RightButton; cursorShape: Qt.PointingHandCursor
                    onClicked: function(mouse) { if (mouse.button === Qt.RightButton) { lib.menuMedia = modelData; rowMenu.popup() } else lib.preview(modelData) } }
                ColumnLayout { anchors.fill: parent; anchors.margins: 6; spacing: 4
                    Rectangle { Layout.fillWidth: true; Layout.fillHeight: true; color: "#000"; radius: 3
                        Image { anchors.fill: parent; source: modelData.thumbnail ? api.rawUrl("/thumbs/" + modelData.id + ".jpg") : ""; fillMode: Image.PreserveAspectFit; asynchronous: true }
                        Text { anchors.centerIn: parent; visible: !modelData.thumbnail; text: "▶"; color: Theme.text2; font.pixelSize: 24 } }
                    Text { Layout.fillWidth: true; text: modelData.title || modelData.path; color: Theme.text0; font.pixelSize: Theme.fs11; elide: Text.ElideRight }
                    Text { Layout.fillWidth: true; text: (modelData.type || "video") + "  ·  " + Theme.fmtClock(modelData.duration); color: Theme.text2; font.pixelSize: Theme.fs10 }
                }
            }
            Text { anchors.centerIn: parent; visible: !lib.filtered.length; text: lib.loading ? "Loading…" : "No media."; color: Theme.text3 }
        }
    }
}
