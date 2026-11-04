import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    property string text: ""
    property string panel: ""

    Layout.fillWidth: true
    Layout.fillHeight: true
    radius: 4
    color: window.sidePanel === panel ? "#00d4ff" :
           (sidePanelMouse.containsMouse ? "#1a1a28" : "transparent")

    Text {
        anchors.centerIn: parent
        text: parent.text
        font.pixelSize: 11
        font.bold: true
        color: window.sidePanel === panel ? "white" : "#a0a0b8"
    }

    MouseArea {
        id: sidePanelMouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: {
            if (window.sidePanel === panel) {
                window.sidePanel = "none"
            } else {
                window.sidePanel = panel
            }
        }
    }
}
