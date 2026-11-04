import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".."

/// Hour Rundown: the runlist the Supervisor pushed (current hour), with the on-air row highlighted.
CPanel {
    id: rd
    property var runlist: ({})          // {current:{items[], hourStartMs, scheduledSec, gapSec}, next:{...}}
    property var state: ({})            // engine telemetry (itemId = on-air runlist id)
    property bool showNext: false
    property bool compact: false
    property string channelId: supervisor.activeChannelId
    property var menuItem: null
    Menu {
        id: rdMenu
        MenuItem { text: "Skip to next"; onTriggered: { supervisor.skip(rd.channelId) } }
        MenuItem { text: "Re-roll query blocks"; onTriggered: { api.post("/channels/" + rd.channelId + "/reroll", {}, function(ok) { if (ok) window.toast("Blocks re-rolled", "ok") }) } }
        MenuItem { text: "Load hour now"; onTriggered: { supervisor.loadHour(rd.channelId) } }
        MenuSeparator {}
        MenuItem { text: "Open media"; enabled: !!(rd.menuItem && (rd.menuItem.mediaId || rd.menuItem.assetId)); onTriggered: { window.openMedia(rd.menuItem.mediaId || rd.menuItem.assetId, "meta") } }
        MenuItem { text: "Asset report"; enabled: !!(rd.menuItem && (rd.menuItem.mediaId || rd.menuItem.assetId)); onTriggered: { Qt.openUrlExternally(api.rawUrl("/reports/asset/" + (rd.menuItem.mediaId || rd.menuItem.assetId) + ".html")) } }
        MenuItem { text: "Edit this hour"; enabled: !!rd.menuItem; onTriggered: { rd.editMenuHour() } }
    }
    // --- drop files / folders onto the live rundown: they play next (after the on-air item, or after the row dropped on) ---
    // The current hour is saved as today's dated override (the weekly template is left alone); the executor re-pushes
    // the hour and keeps the item on air (runlist ids are keyed by the hour, so it is not re-cued).
    property string dropLabel: "Play next on " + (supervisor.activeChannelId || "this channel")
    function editMenuHour() {
        if (!menuItem) return
        var current = runlist && runlist.current ? runlist.current : ({})
        var next = runlist && runlist.next ? runlist.next : ({})
        var isNext = (next.items || []).some(function(x) { return x.id === rd.menuItem.id })
        var hourRun = isNext ? next : current
        var slotId = menuItem.slotId || hourRun.slotId || ""
        if (!/^\d-\d{1,2}$/.test(slotId) && !/^\d{4}-\d{2}-\d{2}-\d{1,2}$/.test(slotId)) {
            window.toast("This rundown item has no editable schedule hour", "error"); return
        }
        window.openScheduleHour(rd.channelId, slotId, hourRun.hourStartMs || menuItem.plannedStartMs || Date.now())
    }
    Component.onCompleted: { window.registerDropTarget(rd); loadMediaInfo() }
    Component.onDestruction: { window.unregisterDropTarget(rd) }
    property var mediaInfo: ({})
    function loadMediaInfo() { api.get("/media", function(ok, d) { if (!ok) return; var m = {}; (d.media || []).forEach(function(x) { m[x.id] = x }); rd.mediaInfo = m }) }
    function insertAt(anchor, side, additions) {
        if (!anchor || !additions.length) { window.toast("Drop on a rundown row", "error"); return }
        api.post("/channels/" + rd.channelId + "/rundown/insert", { anchorId: anchor.id, side: side, items: additions }, function(ok, r) {
            window.toast(ok ? "Inserted " + additions.length + " item(s) into the rundown" : (r && r.error || "Could not insert into the rundown"), ok ? "ok" : "error")
        })
    }
    function dropPaths(paths, x, y) {
        var lp = list.mapFromItem(rd, x, y), row = list.indexAt(lp.x + list.contentX, lp.y + list.contentY)
        if (row < 0 || row >= items.length) { window.toast("Drop on a rundown row", "error"); return }
        var target = list.itemAtIndex(row), anchor = items[row]
        var side = target && lp.y + list.contentY - target.y < target.height / 2 ? "before" : "after"
        window.resolvePaths(paths, function(r) {
            if (!r) return
            var add = (r.items || []).filter(function(m) { return m.duration > 0 }).map(window.newPlaylistItem)
            if (!add.length) { window.toast("No playable files in the drop", "error"); return }
            rd.insertAt(anchor, side, add)
        })
    }
    property var items: {
        var cur = runlist && runlist.current && runlist.current.items ? runlist.current.items : []
        if (!showNext) return cur
        var nx = runlist && runlist.next && runlist.next.items ? runlist.next.items : []
        return cur.concat(nx)
    }
    property real totalSec: { var t = 0; for (var i = 0; i < items.length; ++i) t += (items[i].duration || 0); return t }
    title: "Hour Rundown (" + items.length + " items)"
    headerRight: [
        Text { text: "⏱ " + Theme.fmtClock(rd.totalSec); color: Theme.text1; font.family: Theme.fontMono; font.pixelSize: Theme.fs11 }
    ]
    padding: 0

    ListView {
        id: list
        anchors.fill: parent
        clip: true
        model: rd.items
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { }
        delegate: Rectangle {
            width: list.width
            height: rd.compact ? 30 : 46
            property bool onAir: rd.state && rd.state.itemId === modelData.id
            property bool isNext: !onAir && rd.state && rd.state.nextItemId === modelData.id
            property bool past: modelData.plannedEndMs && modelData.plannedEndMs < Date.now() && !onAir
            color: onAir ? Qt.rgba(1, 0.18, 0.33, 0.16) : isNext ? Qt.rgba(1, 0.58, 0, 0.10) : index % 2 ? Theme.bg2 : Theme.bg3
            Rectangle { width: onAir ? 5 : 3; height: parent.height; color: onAir ? Theme.onAir : isNext ? Theme.standby : Theme.typeColor(modelData.type) }
            // on-air row: elapsed share of the item
            Rectangle { visible: onAir && !rd.compact; anchors.left: parent.left; anchors.bottom: parent.bottom; height: 3; color: Theme.onAir
                width: rd.state && rd.state.duration > 0 ? parent.width * Math.min(1, rd.state.position / rd.state.duration) : 0 }
            RowLayout {
                anchors.fill: parent; anchors.leftMargin: 10; anchors.rightMargin: 10; spacing: 8
                Text { text: Theme.fmtTime(modelData.plannedStartMs, true); color: onAir ? Theme.onAir : past ? Theme.text3 : Theme.text1; font.family: Theme.fontMono; font.pixelSize: rd.compact ? Theme.fs11 : Theme.fs13; font.bold: onAir; Layout.preferredWidth: rd.compact ? 78 : 92 }
                Text { visible: modelData.hard; text: "◆"; color: Theme.standby; font.pixelSize: Theme.fs10; ToolTip.text: "Hard start" }
                ColumnLayout {
                    Layout.fillWidth: true; spacing: 0
                    Text { text: modelData.title || modelData.path || "(untitled)"; color: past ? Theme.text2 : Theme.text0; font.pixelSize: rd.compact ? Theme.fs12 : Theme.fs14; font.bold: onAir; elide: Text.ElideRight; Layout.fillWidth: true }
                    Text { visible: !rd.compact; text: { var m = rd.mediaInfo[modelData.mediaId]; return (modelData.note || "") + (modelData.truncated ? "  • truncated" : "") + (m ? "  · Added " + (m.addedAtMs ? Qt.formatDate(new Date(m.addedAtMs), "yyyy-MM-dd") : "—") + "  · " + (m.playCount || 0) + " plays" : "") } color: Theme.text3; font.pixelSize: Theme.fs10; elide: Text.ElideRight; Layout.fillWidth: true }
                }
                CBadge { visible: !!modelData.queryFill; text: "\u26A1"; tint: Theme.accent0; ToolTip.text: "resolved from " + (modelData.blockLabel || "a block") }
                CBadge { visible: isNext; text: "next"; tint: Theme.standby; solid: true }
                CBadge { text: modelData.type || "video"; tint: Theme.typeColor(modelData.type) }
                Text { text: onAir && !rd.compact && rd.state ? "−" + Theme.fmtClock(rd.state.remaining) : Theme.fmtClock(modelData.duration); color: onAir ? Theme.onAir : Theme.text1; font.family: Theme.fontMono; font.pixelSize: rd.compact ? Theme.fs11 : Theme.fs14; font.bold: onAir; Layout.preferredWidth: rd.compact ? 52 : 70; horizontalAlignment: Text.AlignRight }
            }
            DropArea {
                id: rowDrop; anchors.fill: parent; keys: ["playplay-media"]
                property bool before: drag.y < height / 2
                Rectangle { visible: rowDrop.containsDrag; x: 0; y: rowDrop.before ? 0 : parent.height - 3; width: parent.width; height: 3; color: Theme.accent0 }
                onDropped: function(drop) { var m = drop.source && drop.source.dragMedia; if (!m) return; var it = window.newPlaylistItem({mediaId:m.id, title:m.title, path:m.path, type:m.type, duration:m.duration}); if (m.liveSource) it.liveSource = m.liveSource; rd.insertAt(modelData, drop.y < height / 2 ? "before" : "after", [it]) }
            }
            MouseArea { anchors.fill: parent; acceptedButtons: Qt.RightButton; onClicked: { rd.menuItem = modelData; rdMenu.popup() } }
        }
        Column {
            anchors.centerIn: parent
            visible: rd.items.length === 0
            spacing: 6
            Text { anchors.horizontalCenter: parent.horizontalCenter; text: "▦"; color: Theme.text3; font.pixelSize: 28 }
            Text { anchors.horizontalCenter: parent.horizontalCenter; text: "No items in playlist"; color: Theme.text2; font.pixelSize: Theme.fs12 }
            Text { anchors.horizontalCenter: parent.horizontalCenter; text: "Schedule the hour or arm the channel"; color: Theme.text3; font.pixelSize: Theme.fs11 }
        }
    }
}
