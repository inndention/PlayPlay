import QtQuick 2.15
import QtQuick.Controls 2.15

Rectangle {
    id: timeDisplay
    color: "#0f172a"
    border.color: "#3b82f6"
    border.width: 1
    radius: 4

    property bool showTimecode: false
    property int timecodeFrames: 0
    property int timecodeHours: 0
    property int timecodeMinutes: 0
    property int timecodeSeconds: 0

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 10
        spacing: 5

        // Clock time
        Text {
            id: clockTime
            Layout.fillWidth: true
            text: Qt.formatDateTime(new Date(), "hh:mm:ss")
            color: "white"
            font.pixelSize: 24
            font.bold: true
            horizontalAlignment: Text.AlignHCenter
        }

        // Timecode (if enabled)
        Text {
            id: timecodeText
            Layout.fillWidth: true
            visible: showTimecode
            text: formatTimecode()
            color: "#3b82f6"
            font.pixelSize: 18
            font.bold: true
            horizontalAlignment: Text.AlignHCenter
        }
    }

    // Timer to update time
    Timer {
        id: updateTimer
        interval: 1000
        running: true
        repeat: true
        onTriggered: {
            clockTime.text = Qt.formatDateTime(new Date(), "hh:mm:ss")
            if (showTimecode) {
                updateTimecode()
            }
        }
    }

    function formatTimecode() {
        return String(timecodeHours).padStart(2, '0') + ":" +
               String(timecodeMinutes).padStart(2, '0') + ":" +
               String(timecodeSeconds).padStart(2, '0') + ":" +
               String(timecodeFrames).padStart(2, '0')
    }

    function updateTimecode() {
        // TODO: Get timecode from C++ backend (TimeAuthority)
        // For now, increment frames
        timecodeFrames++
        if (timecodeFrames >= 30) { // Assuming 30 fps
            timecodeFrames = 0
            timecodeSeconds++
            if (timecodeSeconds >= 60) {
                timecodeSeconds = 0
                timecodeMinutes++
                if (timecodeMinutes >= 60) {
                    timecodeMinutes = 0
                    timecodeHours++
                    if (timecodeHours >= 24) {
                        timecodeHours = 0
                    }
                }
            }
        }
    }
}

