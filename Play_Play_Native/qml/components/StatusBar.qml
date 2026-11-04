import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

Rectangle {
    id: statusBar
    height: 40
    color: "#1e293b"
    border.color: "#3b82f6"
    border.width: 1

    property string statusText: "Ready"
    property bool isPlaying: false
    property string currentTime: Qt.formatDateTime(new Date(), "hh:mm:ss")
    property string nowPlaying: ""

    RowLayout {
        anchors.fill: parent
        anchors.margins: 10
        spacing: 15

        // Status indicator
        Rectangle {
            Layout.preferredWidth: 12
            Layout.preferredHeight: 12
            radius: 6
            color: isPlaying ? "#22c55e" : "#64748b"
        }

        // Status text
        Text {
            Layout.preferredWidth: 150
            text: statusText
            color: "white"
            font.pixelSize: 12
            elide: Text.ElideRight
        }

        // Now playing
        Text {
            Layout.fillWidth: true
            text: nowPlaying ? "Now Playing: " + nowPlaying : ""
            color: "#94a3b8"
            font.pixelSize: 12
            elide: Text.ElideRight
        }

        // Time display
        Text {
            Layout.preferredWidth: 100
            text: currentTime
            color: "white"
            font.pixelSize: 12
            horizontalAlignment: Text.AlignRight
        }
    }

    // Timer to update time
    Timer {
        id: timeTimer
        interval: 1000
        running: true
        repeat: true
        onTriggered: {
            currentTime = Qt.formatDateTime(new Date(), "hh:mm:ss")
        }
    }
}

