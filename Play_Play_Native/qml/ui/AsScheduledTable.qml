import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".."

/// As-Scheduled: what the schedule says will air on one day (dated overrides over the weekly template).
CPanel {
    id: tbl
    property string channel: supervisor.activeChannelId
    property var day: new Date()
    property var rows: []
    property var mediaDur: ({})
    property real totalSec: { var t = 0; for (var i = 0; i < rows.length; ++i) t += rows[i].dur; return t }
    title: "As scheduled — " + Qt.formatDate(day, "dddd MMM d") + "  ·  " + rows.length + " items  ·  " + Theme.fmtDur(totalSec)
    padding: 0
    function iso(d) { return d.getFullYear() + "-" + Theme.pad2(d.getMonth() + 1) + "-" + Theme.pad2(d.getDate()) }
    function reload() {
        api.get("/schedule/" + channel + "/week?start=" + iso(day), function(ok, data) {
            if (!ok) return
            var slots = data.slots || {}; var out = []
            var dayStart = new Date(day.getFullYear(), day.getMonth(), day.getDate()).getTime()
            for (var h = 0; h < 24; ++h) {
                var s = slots[iso(day) + "-" + h] || slots[day.getDay() + "-" + h]
                if (!s || !s.items) continue
                var hourStart = dayStart + h * 3600000; var cursor = hourStart
                for (var i = 0; i < s.items.length; ++i) {
                    var it = s.items[i]
                    var full = it.duration || tbl.mediaDur[it.mediaId] || 0
                    var dur = it.timeOut > 0 ? it.timeOut - (it.timeIn || 0) : full - (it.timeIn || 0); dur = Math.max(0, dur)
                    var start = cursor
                    if (it.timingMode === "hard_start" && it.fixedOffsetSec >= 0) start = Math.max(cursor, hourStart + it.fixedOffsetSec * 1000)
                    if (it.timingMode === "fill_until") { var next = null; for (var j = i + 1; j < s.items.length; ++j) if (s.items[j].timingMode === "hard_start" && s.items[j].fixedOffsetSec >= 0) { next = hourStart + s.items[j].fixedOffsetSec * 1000; break } dur = ((next || hourStart + 3600000) - start) / 1000 }
                    out.push({ start: start, title: it.title || it.path, type: it.type, dur: dur, timing: it.timingMode || "soft_start", hour: h, slot: s.id, overrun: (start + dur * 1000) > hourStart + 3600000 + 1000, cg: (it.cgOnStart || []).join(", ") })
                    cursor = start + dur * 1000
                }
            }
            tbl.rows = out
        })
    }
    onDayChanged: reload()
    onChannelChanged: reload()
    Component.onCompleted: api.get("/media", function(ok, d) { if (ok) { var m = {}; var list = d.media || []; for (var i = 0; i < list.length; ++i) m[list[i].id] = list[i].duration || 0; tbl.mediaDur = m } tbl.reload() })
    Connections { target: supervisor; function onScheduleChanged(ch, id) { if (ch === tbl.channel) tbl.reload() } }
    headerRight: [ CButton { icon2: "↻"; small: true; variant: "ghost"; onClicked: { tbl.reload() } } ]

    Rectangle { width: parent.width; height: 26; color: Theme.bg1
        RowLayout { anchors.fill: parent; anchors.leftMargin: 10; anchors.rightMargin: 10; spacing: 8
            Repeater { model: [["HOUR", 50], ["PLANNED", 86], ["TITLE", -1], ["TYPE", 80], ["TIMING", 90], ["DURATION", 70], ["CG", 160]]
                Text { text: modelData[0]; color: Theme.text3; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1; Layout.preferredWidth: modelData[1] > 0 ? modelData[1] : -1; Layout.fillWidth: modelData[1] < 0 } } } }
    ListView {
        anchors.fill: parent; anchors.topMargin: 26; clip: true; model: tbl.rows
        ScrollBar.vertical: ScrollBar {}
        delegate: Rectangle {
            width: ListView.view.width; height: 26
            property bool past: modelData.start + modelData.dur * 1000 < Date.now()
            color: index % 2 ? Theme.bg2 : Theme.bg3
            RowLayout { anchors.fill: parent; anchors.leftMargin: 10; anchors.rightMargin: 10; spacing: 8
                Text { text: Theme.pad2(modelData.hour) + ":00"; color: Theme.text3; font.family: Theme.fontMono; font.pixelSize: Theme.fs10; Layout.preferredWidth: 50 }
                Text { text: Theme.fmtTime(modelData.start, true); color: past ? Theme.text3 : Theme.text0; font.family: Theme.fontMono; font.pixelSize: Theme.fs11; Layout.preferredWidth: 86 }
                Text { text: modelData.title || ""; color: past ? Theme.text2 : Theme.text0; font.pixelSize: Theme.fs11; elide: Text.ElideRight; Layout.fillWidth: true }
                CBadge { text: modelData.type || "video"; tint: Theme.typeColor(modelData.type); Layout.preferredWidth: 80 }
                Text { text: modelData.timing.replace("_", " "); color: modelData.timing === "hard_start" ? Theme.standby : Theme.text2; font.pixelSize: Theme.fs10; Layout.preferredWidth: 90 }
                Text { text: Theme.fmtClock(modelData.dur) + (modelData.overrun ? " ⚠" : ""); color: modelData.overrun ? Theme.danger : Theme.text1; font.family: Theme.fontMono; font.pixelSize: Theme.fs11; Layout.preferredWidth: 70 }
                Text { text: modelData.cg; color: Theme.clsGraphics; font.pixelSize: Theme.fs10; elide: Text.ElideRight; Layout.preferredWidth: 160 } }
        }
        Text { anchors.centerIn: parent; visible: tbl.rows.length === 0; text: "Nothing scheduled for this day"; color: Theme.text3; font.pixelSize: Theme.fs11 }
    }
}
