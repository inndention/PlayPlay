import QtQuick
import QtQuick.Layouts
import ".."

/// Card with a small-caps header row and an optional right-side header item.
Rectangle {
    id: p
    property string title: ""
    property alias headerRight: rightSlot.data
    default property alias content: body.data
    property int padding: Theme.s12
    property bool flat: false
    color: flat ? "transparent" : Theme.bg2
    radius: Theme.r8
    border.color: flat ? "transparent" : Theme.border0
    border.width: 1
    clip: true

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        Item {
            visible: p.title.length > 0
            Layout.fillWidth: true
            Layout.preferredHeight: 34
            Text {
                anchors.left: parent.left; anchors.leftMargin: p.padding
                anchors.verticalCenter: parent.verticalCenter
                text: p.title.toUpperCase()
                color: Theme.text1
                font.family: Theme.fontDisplay
                font.pixelSize: Theme.fs11
                font.bold: true
                font.letterSpacing: 1.2
            }
            Row { id: rightSlot; anchors.right: parent.right; anchors.rightMargin: p.padding; anchors.verticalCenter: parent.verticalCenter; spacing: 6 }
            Rectangle { anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right; height: 1; color: Theme.border0 }
        }
        Item {
            id: body
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: p.padding
        }
    }
}
