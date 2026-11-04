import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

Page {
    title: "CG Control"
    background: Rectangle { color: "transparent" }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 20

        Label {
            text: "Character Generator Control"
            font.pixelSize: 24
            font.bold: true
            color: "white"
        }

        GridView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            cellWidth: 200
            cellHeight: 150
            model: 8 // 8 Template slots

            delegate: Rectangle {
                width: 180
                height: 130
                color: "#333333"
                radius: 5
                
                ColumnLayout {
                    anchors.centerIn: parent
                    Label { text: "Template " + (index + 1); color: "white" }
                    Button { text: "PUSH LIVE"; highlighted: true; flat: false }
                }
            }
        }
    }
}
