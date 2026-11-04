import QtQuick
import ".."

/// Vertical channel fader in dB (-60 .. +12, bottom = off). Drag or click to set, double-click for 0 dB, wheel for 1 dB steps.
Item {
    id: f
    property real value: 0                 // dB
    property real from: -60
    property real to: 12
    property bool dragging: ma.pressed
    signal moved(real db)
    signal released()
    implicitWidth: 34; implicitHeight: 120
    function frac(db) { return Math.max(0, Math.min(1, (db - from) / (to - from))) }
    function dbAt(y) { return from + (1 - Math.max(0, Math.min(1, (y - 8) / Math.max(1, height - 16)))) * (to - from) }

    Rectangle { x: parent.width / 2 - 2; y: 8; width: 4; height: parent.height - 16; radius: 2; color: Theme.bg4 }
    // 0 dB mark
    Rectangle { x: parent.width / 2 - 10; y: 8 + (1 - f.frac(0)) * (parent.height - 16); width: 20; height: 1; color: Theme.accent0 }
    Rectangle {
        id: handle
        x: parent.width / 2 - 11; width: 22; height: 14; radius: 3
        y: 8 + (1 - f.frac(f.value)) * (parent.height - 16) - height / 2
        color: ma.pressed ? Theme.accent0 : ma.containsMouse ? Theme.text0 : Theme.text1
        border.color: Theme.border2
        Rectangle { anchors.centerIn: parent; width: 14; height: 2; color: "#05050a" }
    }
    MouseArea {
        id: ma
        anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
        function set(y) { var db = f.dbAt(y); if (db < f.from + 0.5) db = f.from; if (Math.abs(db) < 0.8) db = 0; f.moved(db) }
        onPressed: function(m) { set(m.y) }
        onPositionChanged: function(m) { if (pressed) set(m.y) }
        onReleased: { f.released() }
        onDoubleClicked: { f.moved(0); f.released() }
        onWheel: function(w) { var db = Math.max(f.from, Math.min(f.to, f.value + (w.angleDelta.y > 0 ? 1 : -1))); f.moved(db); f.released() }
    }
}
