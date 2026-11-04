import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

Rectangle {
    id: talentMonitor
    color: "#1a202c"

    property bool isLive: false
    property string talentName: "Talent Name"
    property string showName: "Show Name"

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 15

        // Status label
        Text {
            id: statusLabel
            Layout.fillWidth: true
            Layout.preferredHeight: 60
            text: isLive ? "ON AIR" : "OFF AIR"
            color: isLive ? "#22c55e" : "#dc3545"
            font.pixelSize: 32
            font.bold: true
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }

        // Talent name
        Text {
            Layout.fillWidth: true
            text: "Talent: " + talentName
            color: "white"
            font.pixelSize: 18
            horizontalAlignment: Text.AlignHCenter
        }

        // Show name
        Text {
            Layout.fillWidth: true
            text: "Show: " + showName
            color: "white"
            font.pixelSize: 18
            horizontalAlignment: Text.AlignHCenter
        }

        // Time display
        Text {
            id: timeDisplay
            Layout.fillWidth: true
            Layout.preferredHeight: 80
            text: Qt.formatDateTime(new Date(), "hh:mm:ss")
            color: "white"
            font.pixelSize: 48
            font.bold: true
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }

        // Control buttons
        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            Button {
                Layout.fillWidth: true
                text: "Go Live"
                enabled: !isLive
                onClicked: {
                    isLive = true
                    // TODO: Emit signal to C++ backend
                }
                background: Rectangle {
                    color: parent.enabled ? "#22c55e" : "#4a5568"
                    radius: 4
                }
            }

            Button {
                Layout.fillWidth: true
                text: "Go Off Air"
                enabled: isLive
                onClicked: {
                    isLive = false
                    // TODO: Emit signal to C++ backend
                }
                background: Rectangle {
                    color: parent.enabled ? "#dc3545" : "#4a5568"
                    radius: 4
                }
            }
        }
    }

    // Timer to update time display
    Timer {
        id: timeTimer
        interval: 1000
        running: true
        repeat: true
        onTriggered: {
            timeDisplay.text = Qt.formatDateTime(new Date(), "hh:mm:ss")
        }
    }
}

