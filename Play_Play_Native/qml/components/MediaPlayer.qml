import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
// QtMultimedia import - use version available in your Qt installation
// For Qt6: import QtMultimedia 6.0
// For Qt5: import QtMultimedia 5.15
import QtMultimedia 5.15

Rectangle {
    id: mediaPlayer
    color: "#09090b"

    property alias source: videoOutput.source
    property alias playing: mediaPlayerControl.playing
    property alias position: mediaPlayerControl.position
    property alias duration: mediaPlayerControl.duration

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Video output area
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: "#000000"

            VideoOutput {
                id: videoOutput
                anchors.fill: parent
                fillMode: VideoOutput.PreserveAspectFit
                // TODO: Connect to actual media source
            }

            // Placeholder text when no video
            Text {
                anchors.centerIn: parent
                text: "No Video"
                color: "#94a3b8"
                font.pixelSize: 24
                visible: !videoOutput.source
            }
        }

        // Controls
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 100
            color: "#1e293b"

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 10
                spacing: 10

                // Progress bar
                Slider {
                    id: progressSlider
                    Layout.fillWidth: true
                    from: 0
                    to: mediaPlayerControl.duration
                    value: mediaPlayerControl.position
                    onMoved: {
                        mediaPlayerControl.seek(value)
                    }
                }

                // Time display and controls
                RowLayout {
                    Layout.fillWidth: true

                    Text {
                        text: formatTime(mediaPlayerControl.position) + " / " + formatTime(mediaPlayerControl.duration)
                        color: "white"
                        font.pixelSize: 12
                    }

                    Item { Layout.fillWidth: true }

                    Button {
                        text: "⏮"
                        onClicked: {
                            // TODO: Previous
                        }
                    }

                    Button {
                        id: playPauseButton
                        text: mediaPlayerControl.playing ? "⏸" : "▶"
                        onClicked: {
                            mediaPlayerControl.playing = !mediaPlayerControl.playing
                        }
                    }

                    Button {
                        text: "⏭"
                        onClicked: {
                            // TODO: Next
                        }
                    }

                    Button {
                        text: "⏹"
                        onClicked: {
                            mediaPlayerControl.playing = false
                            mediaPlayerControl.position = 0
                        }
                    }

                    Slider {
                        id: volumeSlider
                        Layout.preferredWidth: 100
                        from: 0
                        to: 100
                        value: 50
                        onMoved: {
                            // TODO: Set volume
                        }
                    }
                }
            }
        }
    }

    QtObject {
        id: mediaPlayerControl
        property bool playing: false
        property real position: 0
        property real duration: 0

        function seek(pos) {
            position = pos
            // TODO: Actually seek in media
        }
    }

    Timer {
        id: positionTimer
        interval: 100
        running: mediaPlayerControl.playing
        repeat: true
        onTriggered: {
            if (mediaPlayerControl.position < mediaPlayerControl.duration) {
                mediaPlayerControl.position += 0.1
            } else {
                mediaPlayerControl.playing = false
            }
        }
    }

    function formatTime(seconds) {
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

