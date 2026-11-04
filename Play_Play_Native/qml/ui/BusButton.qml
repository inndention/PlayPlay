import QtQuick
import QtQuick.Layouts
import ".."

/// A switcher bus key: number, mapped input name, red lamp while the input is on PGM, green while it is on PVW.
/// tags: what the input brings with it ("CG1+CG2 · PTZ1"), shown in the key's corner.
Rectangle {
    id: b
    property int slot: 0
    property var entry: null
    property string tags: ""
    property bool onPgm: false
    property bool onPvw: false
    signal clicked()
    signal doubleClicked()
    signal rightClicked()
    implicitHeight: 52
    radius: Theme.r5
    color: onPgm ? Qt.rgba(1, 0.18, 0.33, 0.30) : onPvw ? Qt.rgba(0.19, 0.82, 0.35, 0.26) : ma.pressed ? Theme.bg5 : ma.containsMouse ? Theme.bg4 : entry ? Theme.bg3 : Theme.bg2
    border.width: onPgm || onPvw ? 2 : 1
    border.color: onPgm ? Theme.onAir : onPvw ? Theme.ready : Theme.border1
    Rectangle { anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right; anchors.margins: 1; height: 4; radius: 2; color: b.onPgm ? Theme.onAir : b.onPvw ? Theme.ready : "transparent" }
    RowLayout {
        anchors.fill: parent; anchors.leftMargin: 10; anchors.rightMargin: 8; anchors.topMargin: 4; spacing: 8
        Text { text: String(b.slot + 1); color: b.onPgm ? Theme.onAir : b.onPvw ? Theme.ready : b.entry ? Theme.text0 : Theme.text3; font.family: Theme.fontMono; font.pixelSize: Theme.fs28; font.bold: true }
        ColumnLayout {
            Layout.fillWidth: true; spacing: 0
            Text { Layout.fillWidth: true; text: b.entry ? b.entry.title : "empty"; color: b.entry ? Theme.text0 : Theme.text3; font.pixelSize: Theme.fs12; font.bold: !!b.entry; elide: Text.ElideRight }
            Text { Layout.fillWidth: true; text: b.entry ? (b.entry.kind || "").toUpperCase() + (b.onPgm ? "  ·  ON AIR" : b.onPvw ? "  ·  PREVIEW" : "") : "right-click to map"; color: b.onPgm ? Theme.onAir : b.onPvw ? Theme.ready : Theme.text3; font.pixelSize: Theme.fs10; font.bold: b.onPgm || b.onPvw; elide: Text.ElideRight }
        }
    }
    Text {
        visible: b.tags.length > 0
        anchors.right: parent.right; anchors.top: parent.top; anchors.rightMargin: 6; anchors.topMargin: 5
        width: Math.min(implicitWidth, parent.width - 40); elide: Text.ElideRight; horizontalAlignment: Text.AlignRight
        text: b.tags; color: Theme.accent0; font.pixelSize: Theme.fs10 - 1; font.bold: true; font.family: Theme.fontMono
    }
    MouseArea {
        id: ma
        anchors.fill: parent; hoverEnabled: true; acceptedButtons: Qt.LeftButton | Qt.RightButton; cursorShape: Qt.PointingHandCursor
        onClicked: function(m) { if (m.button === Qt.RightButton) b.rightClicked(); else b.clicked() }
        onDoubleClicked: function(m) { if (m.button === Qt.LeftButton) b.doubleClicked() }
    }
}
