import QtQuick
import QtQuick.Controls

Rectangle {
    property string icon: ""
    property bool isPrimary: false
    property bool isPlaying: false
    signal clicked()

    width: isPrimary ? 48 : 40
    height: isPrimary ? 48 : 40
    radius: isPrimary ? 24 : 6
    color: isPrimary ? (isPlaying ? "#ff2d55" : "#00d4ff") :
           (transportMouse.containsMouse ? "#1a1a28" : "#0f0f18")
    border.color: isPrimary ? "transparent" : "#2a2a40"

    Text {
        anchors.centerIn: parent
        text: parent.icon
        font.pixelSize: parent.isPrimary ? 20 : 16
        color: parent.isPrimary ? "white" : "#f0f0f8"
    }

    MouseArea {
        id: transportMouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: parent.clicked()
    }
}
