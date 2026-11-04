import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".."

/**
 * MediaEditor: a modal editor for one library item — metadata, transcript (faster-whisper with
 * accuracy levels; click a word, shift-click the last word, "+ Segment") and segments
 * (collections, auto split by interval or by transcript pauses, reorder/duplicate/delete, and the
 * frame-accurate SegmentTimeline with the transcript beside it: words, find, cuts on the words).
 * Open with `open(mediaId, tab)`; everything goes through the Supervisor REST surface.
 */
Popup {
    id: ed
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: ed.tab === "segments" ? Math.min(parent ? parent.width - 40 : 1240, 1480) : Math.min(parent ? parent.width - 80 : 1000, 1040)
    height: ed.tab === "segments" ? Math.min(parent ? parent.height - 30 : 700, 940) : Math.min(parent ? parent.height - 60 : 700, 720)
    modal: true; focus: true; closePolicy: Popup.CloseOnEscape
    padding: 0
    background: Rectangle { color: Theme.bg2; radius: Theme.r8; border.color: Theme.border1 }
    property var media: ({})
    property string tab: "meta"
    property var transcript: null
    property var segs: ({ segments: [], collections: [], activeCollection: "default" })
    property var levels: []
    property var tcfg: ({})
    property int wordA: -1
    property int wordB: -1
    property var words: []
    signal saved(string mediaId)

    function fmtT(s) { return Theme.fmtClock(s) }
    function open(id, t) {
        ed.tab = t || "meta"; ed.transcript = null; ed.wordA = -1; ed.wordB = -1; ed.words = []
        api.get("/media/" + id, function(ok, m) { if (!ok) return; ed.media = m; ed.visible = true; ed.loadTab() })
        if (!ed.levels.length) api.get("/transcribe/levels", function(ok, d) { if (ok) { ed.levels = d.levels || []; ed.tcfg = d.config || {} } })
    }
    function loadTab() {
        var id = ed.media.id; if (!id) return
        if (ed.tab === "transcript" && (ed.media.transcript || {}).status === "done") api.get("/media/" + id + "/transcript", function(ok, t) { if (!ok) return; var ws = []; var segs = t.segments || []; for (var i = 0; i < segs.length; ++i) { var w = segs[i].words || []; for (var j = 0; j < w.length; ++j) { w[j].gi = ws.length; ws.push(w[j]) } } ed.words = ws; ed.transcript = t })
        if (ed.tab === "segments") api.get("/media/" + id + "/segments", function(ok, s) { if (ok) ed.segs = s })
    }
    function reload(t) { open(ed.media.id, t || ed.tab) }
    function save() {
        var m = ed.media
        api.put("/media/" + m.id, { title: fTitle.text, type: fType.currentText, client: fClient.text, programSeries: fSeries.text, episodeDate: fDate.text, priority: parseInt(fPrio.text) || 0, status: fStatus.currentText, notes: fNotes.text, category: fCat.text, allowedChannels: fChans.text.split(",").map(function(s) { return s.trim() }).filter(function(s) { return s.length }), tags: fTags.text.split(",").map(function(s) { return s.trim().toLowerCase() }).filter(function(s) { return s.length }) },
                function(ok) { if (ok) { window.toast("Saved", "ok"); ed.saved(m.id); ed.reload("meta") } })
    }
    function saveSegs() { api.put("/media/" + ed.media.id + "/segments", { segments: ed.segs.segments }, function(ok, s) { if (ok) ed.segs = s }) }
    function autoSplit(mode) { api.post("/media/" + ed.media.id + "/segments/auto", { mode: mode, intervalSec: parseFloat(segInt.text) || 600, overlapSec: parseFloat(segOv.text) || 0, minPauseSec: parseFloat(segPause.text) || 1.5, minSegmentSec: 20, replace: true }, function(ok, s) { if (ok) { ed.segs = s; window.toast((s.segments || []).length + " segments", "ok") } else window.toast(s && s.error ? s.error : "auto split failed", "error") }) }
    function transcribe() { api.post("/media/" + ed.media.id + "/transcribe", { level: lvl.currentText }, function(ok, d) { if (ok) { window.toast("Transcription queued (" + lvl.currentText + ")", "ok"); ed.reload("transcript") } else window.toast(d && d.error ? d.error : "failed", "error") }) }
    function segFromWords() { api.post("/media/" + ed.media.id + "/segments/from-words", { firstWord: ed.wordA, lastWord: ed.wordB, label: segLabel.text }, function(ok, s) { if (ok) { ed.segs = s; ed.wordA = -1; ed.wordB = -1; window.toast("Segment added", "ok"); ed.tab = "segments" } }) }
    // Automation / test hook: PlayPlay.exe --open-media <mediaId> [tab] opens this editor at start-up
    // (with --seg-seek <frame> and --seg-inout <a>,<b> the segment timeline positions itself; used for screenshots).
    Component.onCompleted: {
        var a = Qt.application.arguments, i = a.indexOf("--open-media")
        if (i > 0 && i + 1 < a.length) { var tab = i + 2 < a.length && a[i + 2].indexOf("--") !== 0 ? a[i + 2] : "segments"; Qt.callLater(function() { ed.open(a[i + 1], tab) }) }
    }
    // A finished transcription reloads the tab that is open (it used to jump to the transcript tab, pulling the segment
    // editor away mid-edit). On the segments tab the timeline loads the words and the list gets the segments' spoken text.
    Connections {
        target: supervisor; ignoreUnknownSignals: true
        function onTranscriptReady(id) {
            if (!ed.visible || id !== ed.media.id) return
            if (ed.tab !== "segments") { ed.reload(); return }
            segTl.reloadTranscript()
            api.get("/media/" + id, function(ok, m) { if (ok && m.id === ed.media.id) ed.media = m })
            ed.loadTab()
        }
    }

    ColumnLayout {
        anchors.fill: parent; anchors.margins: 14; spacing: 10
        RowLayout {
            Layout.fillWidth: true; spacing: 10
            Rectangle { width: 96; height: 54; radius: 3; color: "#000"; border.color: Theme.border1
                Image { anchors.fill: parent; anchors.margins: 1; source: ed.media.thumbnail ? api.rawUrl("/thumbs/" + ed.media.id + ".jpg") : ""; fillMode: Image.PreserveAspectCrop; asynchronous: true; cache: false } }
            ColumnLayout { Layout.fillWidth: true; spacing: 2
                Text { text: ed.media.title || ""; color: Theme.text0; font.pixelSize: Theme.fs16; font.bold: true; elide: Text.ElideRight; Layout.fillWidth: true }
                Text { text: (ed.media.path || "") + "   " + Theme.fmtClock(ed.media.duration || 0) + (ed.media.width ? "   " + ed.media.width + "x" + ed.media.height : "") + (ed.media.missing ? "   MISSING" : ""); color: ed.media.missing ? Theme.danger : Theme.text3; font.pixelSize: Theme.fs10; font.family: Theme.fontMono; elide: Text.ElideMiddle; Layout.fillWidth: true } }
            Repeater { model: ["meta", "transcript", "segments"]; CButton { text: modelData; small: true; variant: "ghost"; active: ed.tab === modelData; onClicked: { ed.tab = modelData; ed.loadTab() } } }
            CButton { text: "✕"; small: true; variant: "ghost"; onClicked: { ed.visible = false } }
        }
        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border0 }

        // ---------------- meta ----------------
        Flickable {
            visible: ed.tab === "meta"; Layout.fillWidth: true; Layout.fillHeight: true; clip: true; contentHeight: metaCol.implicitHeight
            ColumnLayout {
                id: metaCol; width: parent.width; spacing: 8
                GridLayout {
                    Layout.fillWidth: true; columns: 4; columnSpacing: 10; rowSpacing: 8
                    CField { id: fTitle; label: "Title"; Layout.columnSpan: 2; Layout.fillWidth: true; text: ed.media.title || "" }
                    CSelect { id: fType; label: "Type"; Layout.fillWidth: true; model: ["program", "ad", "promo", "station_id", "bumper", "filler", "psa", "image", "live", "video"]; currentIndex: Math.max(0, model.indexOf(ed.media.type)) }
                    CSelect { id: fStatus; label: "Status"; Layout.fillWidth: true; model: ["approved", "review", "rejected"]; currentIndex: Math.max(0, model.indexOf(ed.media.status || "approved")) }
                    CField { id: fClient; label: "Client"; Layout.fillWidth: true; text: ed.media.client || "" }
                    CField { id: fSeries; label: "Series"; Layout.fillWidth: true; text: ed.media.programSeries || "" }
                    CField { id: fDate; label: "Episode date"; Layout.fillWidth: true; text: ed.media.episodeDate || ""; placeholder: "YYYY-MM-DD"; mono: true }
                    CField { id: fPrio; label: "Priority"; Layout.fillWidth: true; text: String(ed.media.priority || 0); mono: true }
                    CField { id: fCat; label: "Category"; Layout.fillWidth: true; text: ed.media.category || "" }
                    CField { id: fChans; label: "Allowed channels (empty = all)"; Layout.fillWidth: true; text: (ed.media.allowedChannels || []).join(",") }
                    CField { id: fTags; label: "Tags"; Layout.columnSpan: 2; Layout.fillWidth: true; text: (ed.media.tags || []).join(", ") }
                    CField { id: fNotes; label: "Notes"; Layout.columnSpan: 4; Layout.fillWidth: true; text: ed.media.notes || "" }
                }
                Text { Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.text3; font.pixelSize: Theme.fs10
                    text: "hash " + ((ed.media.hash || "").substring(0, 12) || "—") + "   origin " + (ed.media.origin || "?") + "   plays " + (ed.media.playCount || 0) + "   " + (ed.media.videoCodec || "") + "/" + (ed.media.audioCodec || "") + "   " + (ed.media.transcript && ed.media.transcript.status === "done" ? "transcript: " + ed.media.transcript.level + " (" + ed.media.transcript.words + " words)" : "no transcript") }
                RowLayout { Layout.fillWidth: true; spacing: 6
                    CButton { text: "SAVE"; variant: "success"; onClicked: { ed.save() } }
                    CButton { text: ed.media.archived ? "Unarchive" : "Archive"; variant: "ghost"; onClicked: { api.post("/media/bulk", { ids: [ed.media.id], set: { archived: !ed.media.archived } }, function(ok) { if (ok) { ed.saved(ed.media.id); ed.reload("meta") } }) } }
                    CButton { text: "Re-probe"; variant: "ghost"; small: true; onClicked: { api.post("/media/" + ed.media.id + "/reprobe", {}, function(ok) { if (ok) window.toast("Re-probing", "ok") }) } }
                    CButton { text: "Thumbnail"; variant: "ghost"; small: true; onClicked: { api.post("/media/" + ed.media.id + "/thumbnail", {}, function(ok) { if (ok) window.toast("Thumbnail queued", "ok") }) } }
                    CButton { text: "Preview file"; variant: "ghost"; small: true; onClicked: { if (!api.openMediaFile(ed.media.path)) window.toast("Cannot open this file locally", "error") } }
                    CButton { text: "Reveal"; variant: "ghost"; small: true; onClicked: { api.revealInExplorer(ed.media.path) } }
                    Item { Layout.fillWidth: true }
                    CField { id: relinkPath; visible: !!ed.media.missing; Layout.preferredWidth: 320; placeholder: "new path for the missing file"; mono: true }
                    CButton { visible: !!ed.media.missing; text: "Relink"; small: true; onClicked: { api.post("/media/" + ed.media.id + "/relink", { path: relinkPath.text }, function(ok) { if (ok) { window.toast("Relinked", "ok"); ed.saved(ed.media.id); ed.reload("meta") } }) } }
                    CButton { text: "Remove"; variant: "danger"; small: true; onClicked: { api.post("/media/delete", { ids: [ed.media.id] }, function(ok) { if (ok) { ed.visible = false; ed.saved(ed.media.id) } }) } }
                }
            }
        }

        // ---------------- transcript ----------------
        ColumnLayout {
            visible: ed.tab === "transcript"; Layout.fillWidth: true; Layout.fillHeight: true; spacing: 8
            RowLayout { Layout.fillWidth: true; spacing: 8
                CSelect { id: lvl; label: "Accuracy"; Layout.preferredWidth: 220; model: ed.levels; textRole: "id"; currentIndex: { var want = (ed.media.transcript || {}).level || ed.tcfg.defaultLevel || "balanced"; for (var i = 0; i < ed.levels.length; ++i) if (ed.levels[i].id === want) return i; return 1 } }
                CButton { text: (ed.media.transcript || {}).status === "done" ? "Re-transcribe" : "Transcribe"; variant: "primary"; Layout.alignment: Qt.AlignBottom; onClicked: { ed.transcribe() } }
                CBadge { visible: !!(ed.media.transcript && (ed.media.transcript.status === "queued" || ed.media.transcript.status === "running")); text: (ed.media.transcript || {}).status || ""; tint: Theme.accent0; Layout.alignment: Qt.AlignBottom }
                Text { visible: !!(ed.media.transcript && ed.media.transcript.status === "failed"); text: (ed.media.transcript || {}).error || "failed"; color: Theme.danger; font.pixelSize: Theme.fs11; Layout.alignment: Qt.AlignBottom }
                Text { Layout.fillWidth: true; Layout.alignment: Qt.AlignBottom; wrapMode: Text.WordWrap; color: Theme.text3; font.pixelSize: Theme.fs10; text: lvl.currentIndex >= 0 && ed.levels[lvl.currentIndex] ? ed.levels[lvl.currentIndex].model + " — " + ed.levels[lvl.currentIndex].note : "" }
                Text { visible: !!(ed.media.transcript && ed.media.transcript.status === "done"); text: (ed.media.transcript || {}).model + " · " + (ed.media.transcript || {}).language + " · " + (ed.media.transcript || {}).words + " words"; color: Theme.text3; font.pixelSize: Theme.fs10; Layout.alignment: Qt.AlignBottom }
            }
            RowLayout { Layout.fillWidth: true; spacing: 8
                Text { text: ed.wordA < 0 ? "Click a word, then shift-click the last word of the segment" : "Segment: " + Theme.fmtClock(ed.words[ed.wordA] ? ed.words[ed.wordA].start : 0) + (ed.wordB >= 0 ? " → " + Theme.fmtClock(ed.words[ed.wordB] ? ed.words[ed.wordB].end : 0) : " → …"); color: Theme.text2; font.pixelSize: Theme.fs11 }
                Item { Layout.fillWidth: true }
                CField { id: segLabel; visible: ed.wordA >= 0 && ed.wordB >= 0; Layout.preferredWidth: 200; placeholder: "segment label" }
                CButton { visible: ed.wordA >= 0 && ed.wordB >= 0; text: "+ Segment"; variant: "success"; small: true; onClicked: { ed.segFromWords() } }
                CButton { visible: ed.wordA >= 0; text: "clear"; variant: "ghost"; small: true; onClicked: { ed.wordA = -1; ed.wordB = -1 } }
            }
            Flickable {
                Layout.fillWidth: true; Layout.fillHeight: true; clip: true; contentHeight: trCol.implicitHeight
                Rectangle { anchors.fill: parent; color: Theme.bg3; radius: Theme.r5; z: -1 }
                ColumnLayout {
                    id: trCol; width: parent.width; spacing: 4
                    Text { visible: !ed.transcript; Layout.margins: 10; text: (ed.media.transcript || {}).status === "done" ? "Loading transcript…" : "No transcript yet. Levels trade speed for accuracy: fast is enough for search and rough cuts, accurate for captions."; color: Theme.text3; font.pixelSize: Theme.fs11 }
                    Repeater {
                        model: ed.transcript ? ed.transcript.segments : []
                        Flow { Layout.fillWidth: true; Layout.leftMargin: 10; Layout.rightMargin: 10; spacing: 3
                            property var seg: modelData
                            Text { text: Theme.fmtClock(seg.start); color: Theme.accent0; font.pixelSize: Theme.fs10; font.family: Theme.fontMono; rightPadding: 6 }
                            Repeater { model: seg.words || []
                                Rectangle { property bool inSel: ed.wordA >= 0 && modelData.gi >= Math.min(ed.wordA, ed.wordB < 0 ? ed.wordA : ed.wordB) && modelData.gi <= Math.max(ed.wordA, ed.wordB)
                                    width: wt.implicitWidth + 4; height: 20; radius: 3; color: inSel ? Qt.rgba(0, 0.83, 1, 0.3) : (wm.containsMouse ? Theme.bg4 : "transparent")
                                    Text { id: wt; anchors.centerIn: parent; text: modelData.word.trim(); color: modelData.probability < 0.5 ? Theme.text2 : Theme.text0; font.pixelSize: Theme.fs12 }
                                    MouseArea { id: wm; anchors.fill: parent; hoverEnabled: true; onClicked: function(e) { if (!(e.modifiers & Qt.ShiftModifier) || ed.wordA < 0) { ed.wordA = modelData.gi; ed.wordB = -1 } else { ed.wordB = modelData.gi; if (ed.wordB < ed.wordA) { var t = ed.wordA; ed.wordA = ed.wordB; ed.wordB = t } } }
                                        ToolTip.visible: containsMouse; ToolTip.text: Theme.fmtClock(modelData.start) + " – " + Theme.fmtClock(modelData.end) + "  p=" + modelData.probability } } }
                        }
                    }
                }
            }
        }

        // ---------------- segments ----------------
        ColumnLayout {
            visible: ed.tab === "segments"; Layout.fillWidth: true; Layout.fillHeight: true; spacing: 8
            RowLayout { Layout.fillWidth: true; spacing: 6
                CSelect { id: colSel; label: "Collection"; Layout.preferredWidth: 200; model: ed.segs.collections && ed.segs.collections.length ? ed.segs.collections : [{ id: "default", name: "Default" }]; textRole: "name"; valueRole: "id"
                    currentIndex: { var cs = model; for (var i = 0; i < cs.length; ++i) if (cs[i].id === ed.segs.activeCollection) return i; return 0 }
                    onActivated: function(i) { api.put("/media/" + ed.media.id + "/segments/collections", { collections: ed.segs.collections, activeCollection: model[i].id }, function(ok, s) { if (ok) ed.segs = s }) } }
                CButton { text: "+ collection"; small: true; variant: "ghost"; Layout.alignment: Qt.AlignBottom; onClicked: { var cols = (ed.segs.collections || []).slice(); cols.push({ name: "Collection " + (cols.length + 1), segments: [] }); api.put("/media/" + ed.media.id + "/segments/collections", { collections: cols, activeCollection: "" }, function(ok, s) { if (ok) ed.segs = s }) } }
                Item { Layout.fillWidth: true }
                CField { id: segInt; label: "Interval s"; Layout.preferredWidth: 80; text: "600"; mono: true }
                CField { id: segOv; label: "Overlap s"; Layout.preferredWidth: 70; text: "0"; mono: true }
                CField { id: segPause; label: "Min pause s"; Layout.preferredWidth: 80; text: "1.5"; mono: true }
                CButton { text: "Auto: interval"; small: true; Layout.alignment: Qt.AlignBottom; onClicked: { ed.autoSplit("interval") } }
                CButton { text: "Auto: pauses"; small: true; Layout.alignment: Qt.AlignBottom; onClicked: { ed.autoSplit("transcript") } ToolTip.text: "Split where the transcript has silence (needs a transcript)" }
                CButton { text: "+ segment"; small: true; variant: "primary"; Layout.alignment: Qt.AlignBottom; onClicked: { var s = ed.segs.segments.slice(); s.push({ label: "Segment " + (s.length + 1), startTime: 0, endTime: Math.min(ed.media.duration || 0, 30) }); ed.segs.segments = s; ed.saveSegs() } }
            }
            // Frame-accurate timeline: preview, filmstrip, hover thumbnails, draggable segment edges (SegmentTimeline.qml)
            SegmentTimeline {
                id: segTl
                Layout.fillWidth: true
                mediaId: ed.tab === "segments" && ed.visible ? (ed.media.id || "") : ""
                duration: ed.media.duration || 0
                segments: ed.segs.segments || []
                onSegmentsEdited: function(segs) { ed.segs.segments = segs; ed.segs = Object.assign({}, ed.segs); ed.saveSegs() }
            }
            ListView {
                id: segList; Layout.fillWidth: true; Layout.fillHeight: true; clip: true; model: ed.segs.segments || []; spacing: 2
                delegate: Rectangle {
                    width: segList.width; height: 36; color: segTl.sel === index ? Theme.bg5 : index % 2 ? Theme.bg2 : Theme.bg3; radius: Theme.r3
                    border.color: segTl.sel === index ? Theme.accent0 : "transparent"
                    MouseArea { anchors.fill: parent; z: -1; onClicked: { segTl.sel = index; segTl.seek(segTl.ft(modelData.startTime)); segTl.forceActiveFocus() } }
                    RowLayout { anchors.fill: parent; anchors.leftMargin: 8; anchors.rightMargin: 6; spacing: 6
                        Text { text: index + 1; color: Theme.text2; font.pixelSize: Theme.fs11; font.family: Theme.fontMono; Layout.preferredWidth: 22 }
                        TextField { Layout.preferredWidth: 220; implicitHeight: 26; text: modelData.label || ""; color: Theme.text0; font.pixelSize: Theme.fs12; background: Rectangle { color: Theme.bg4; radius: Theme.r3; border.color: Theme.border1 }
                            onEditingFinished: { ed.segs.segments[index].label = text; ed.saveSegs() } }
                        Text { Layout.fillWidth: true; text: modelData.text || ""; color: Theme.text3; font.pixelSize: Theme.fs10; elide: Text.ElideRight }
                        TextField { Layout.preferredWidth: 80; implicitHeight: 26; text: Number(modelData.startTime).toFixed(2); color: Theme.text0; font.pixelSize: Theme.fs11; font.family: Theme.fontMono; background: Rectangle { color: Theme.bg4; radius: Theme.r3; border.color: Theme.border1 }
                            onEditingFinished: { ed.segs.segments[index].startTime = parseFloat(text) || 0; ed.saveSegs() } }
                        TextField { Layout.preferredWidth: 80; implicitHeight: 26; text: Number(modelData.endTime).toFixed(2); color: Theme.text0; font.pixelSize: Theme.fs11; font.family: Theme.fontMono; background: Rectangle { color: Theme.bg4; radius: Theme.r3; border.color: Theme.border1 }
                            onEditingFinished: { ed.segs.segments[index].endTime = parseFloat(text) || 0; ed.saveSegs() } }
                        Text { text: Theme.fmtClock(modelData.endTime - modelData.startTime); color: Theme.text1; font.pixelSize: Theme.fs11; font.family: Theme.fontMono; Layout.preferredWidth: 56 }
                        CButton { text: "↑"; small: true; variant: "ghost"; onClicked: { if (index > 0) { var a = ed.segs.segments.slice(); var t = a[index - 1]; a[index - 1] = a[index]; a[index] = t; ed.segs.segments = a; ed.saveSegs() } } }
                        CButton { text: "↓"; small: true; variant: "ghost"; onClicked: { var a = ed.segs.segments.slice(); if (index < a.length - 1) { var t = a[index + 1]; a[index + 1] = a[index]; a[index] = t; ed.segs.segments = a; ed.saveSegs() } } }
                        CButton { text: "⤙"; small: true; variant: "ghost"; onClicked: { var a = ed.segs.segments.slice(); var c = Object.assign({}, a[index]); c.id = ""; c.label = (c.label || "") + " copy"; a.splice(index + 1, 0, c); ed.segs.segments = a; ed.saveSegs() } ToolTip.text: "Duplicate" }
                        CButton { text: "✕"; small: true; variant: "ghost"; onClicked: { var a = ed.segs.segments.slice(); a.splice(index, 1); ed.segs.segments = a; ed.saveSegs() } }
                    }
                }
                Text { anchors.centerIn: parent; visible: !(ed.segs.segments && ed.segs.segments.length); width: parent.width - 40; wrapMode: Text.WordWrap; horizontalAlignment: Text.AlignHCenter; color: Theme.text3; font.pixelSize: Theme.fs11; text: "No segments. Add one, auto-split, or select words in the transcript. Segments are virtual clips (in/out on this file) the hour editor can place." }
            }
        }
    }
}
