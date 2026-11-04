import QtQuick
import ".."

/// Small status chip (ON AIR / STANDBY / type tags).
Rectangle {
    property string text: ""
    property color tint: Theme.text1
    property bool solid: false
    implicitHeight: 18
    implicitWidth: label.implicitWidth + 12
    radius: Theme.r3
    color: solid ? tint : Qt.rgba(tint.r, tint.g, tint.b, 0.16)
    border.color: solid ? "transparent" : Qt.rgba(tint.r, tint.g, tint.b, 0.5)
    Text {
        id: label
        anchors.centerIn: parent
        text: parent.text.toUpperCase()
        color: solid ? "#05050a" : tint
        font.family: Theme.fontDisplay
        font.pixelSize: Theme.fs10
        font.bold: true
        font.letterSpacing: 0.8
    }
}
