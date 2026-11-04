import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

Rectangle {
    id: playlistView
    color: "#1e293b"

    property alias model: listView.model
    property int currentIndex: listView.currentIndex

    signal itemClicked(int index)
    signal itemDoubleClicked(int index)

    ScrollView {
        anchors.fill: parent

        ListView {
            id: listView
            anchors.fill: parent
            clip: true

            delegate: Rectangle {
                width: listView.width
                height: 50
                color: ListView.isCurrentItem ? "#3b82f6" : (mouseArea.containsMouse ? "#334155" : "#1e293b")
                border.color: "#3b82f6"
                border.width: ListView.isCurrentItem ? 2 : 0

                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 10

                    Text {
                        text: String(index + 1)
                        font.pixelSize: 14
                        color: "#94a3b8"
                        Layout.preferredWidth: 30
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 5

                        Text {
                            text: model.title || "Untitled"
                            font.pixelSize: 14
                            font.bold: true
                            color: "white"
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                        }

                        Text {
                            text: "Duration: " + (model.duration ? formatDuration(model.duration) : "00:00")
                            font.pixelSize: 12
                            color: "#94a3b8"
                        }
                    }

                    Text {
                        text: model.status || "Ready"
                        font.pixelSize: 12
                        color: model.status === "Playing" ? "#22c55e" : "#94a3b8"
                        Layout.preferredWidth: 80
                    }
                }

                MouseArea {
                    id: mouseArea
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: {
                        listView.currentIndex = index
                        playlistView.itemClicked(index)
                    }
                    onDoubleClicked: {
                        playlistView.itemDoubleClicked(index)
                    }
                }
            }
        }
    }

    function formatDuration(seconds) {
        var hours = Math.floor(seconds / 3600)
        var minutes = Math.floor((seconds % 3600) / 60)
        var secs = Math.floor(seconds % 60)
        
        if (hours > 0) {
            return String(hours).padStart(2, '0') + ":" +
                   String(minutes).padStart(2, '0') + ":" +
                   String(secs).padStart(2, '0')
        }
        return String(minutes).padStart(2, '0') + ":" +
               String(secs).padStart(2, '0')
    }
}

