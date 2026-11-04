import QtQuick
import ".."

/// Hour strip across the top of the content area: items as blocks, now-marker, minute ticks.
Rectangle {
    id: tk
    property var runlist: ({})
    property var state: ({})
    property real hourStart: runlist && runlist.current && runlist.current.hourStartMs ? runlist.current.hourStartMs : Math.floor(Date.now() / 3600000) * 3600000
    property real spanMs: 3600000 * (showNext ? 2 : 1)
    property bool showNext: true
    property real nowMs: Date.now()
    color: Theme.bg1
    border.color: Theme.border0
    Timer { interval: 1000; running: true; repeat: true; onTriggered: { tk.nowMs = Date.now() } }

    Text { x: 10; anchors.verticalCenter: parent.verticalCenter; text: "SCHEDULE TICKER"; color: Theme.text2; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1.2; width: 110 }
    Item {
        id: strip
        anchors.left: parent.left; anchors.leftMargin: 130
        anchors.right: parent.right; anchors.rightMargin: 12
        anchors.top: parent.top; anchors.bottom: parent.bottom
        anchors.topMargin: 6; anchors.bottomMargin: 6
        clip: true
        function xOf(ms) { return (ms - tk.hourStart) / tk.spanMs * width }
        Repeater {
            model: tk.showNext ? 25 : 13
            Rectangle {
                x: index * strip.width / (tk.showNext ? 24 : 12); width: 1; height: index % 6 === 0 ? strip.height : 6; y: index % 6 === 0 ? 0 : strip.height - 6
                color: index % 6 === 0 ? Theme.border1 : Theme.border0
                Text { visible: index % 6 === 0; x: 3; y: 0; text: Theme.fmtTime(tk.hourStart + index * 5 * 60000); color: Theme.text3; font.pixelSize: 9; font.family: Theme.fontMono }
            }
        }
        Repeater {
            model: {
                var cur = tk.runlist && tk.runlist.current && tk.runlist.current.items ? tk.runlist.current.items : []
                var nx = tk.showNext && tk.runlist && tk.runlist.next && tk.runlist.next.items ? tk.runlist.next.items : []
                return cur.concat(nx)
            }
            Rectangle {
                x: strip.xOf(modelData.plannedStartMs)
                width: Math.max(2, strip.xOf(modelData.plannedEndMs) - x - 1)
                y: 12; height: strip.height - 14
                radius: 2
                property bool onAir: tk.state && tk.state.itemId === modelData.id
                color: onAir ? Theme.onAir : Qt.rgba(Theme.typeColor(modelData.type).r, Theme.typeColor(modelData.type).g, Theme.typeColor(modelData.type).b, 0.55)
                border.color: onAir ? "#fff" : "transparent"
                Text { anchors.fill: parent; anchors.leftMargin: 4; text: modelData.title || ""; color: "#fff"; font.pixelSize: Theme.fs10; elide: Text.ElideRight; verticalAlignment: Text.AlignVCenter; visible: parent.width > 30 }
            }
        }
        Rectangle { x: strip.xOf(tk.nowMs); width: 2; height: strip.height; color: Theme.accent0; visible: x >= 0 && x <= strip.width
            Rectangle { width: 54; height: 12; radius: 2; color: Theme.accent0; x: -26; y: -1
                Text { anchors.centerIn: parent; text: Theme.fmtTime(tk.nowMs, true); color: "#05050a"; font.pixelSize: 8; font.bold: true; font.family: Theme.fontMono } }
        }
    }
}
