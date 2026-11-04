import QtQuick 2.15
import QtQuick.Controls 2.15

Rectangle {
    id: scheduleTicker
    height: 50
    color: "#0f172a"
    clip: true

    property var scheduleItems: []
    property int currentIndex: 0

    // Scrolling text
    Text {
        id: tickerText
        anchors.verticalCenter: parent.verticalCenter
        x: parent.width
        text: scheduleItems.length > 0 && currentIndex < scheduleItems.length
              ? formatScheduleItem(scheduleItems[currentIndex])
              : "No schedule"
        color: "white"
        font.pixelSize: 18
        font.bold: true

        SequentialAnimation on x {
            id: scrollAnimation
            running: scheduleItems.length > 0
            loops: Animation.Infinite

            PropertyAnimation {
                from: parent.width
                to: -tickerText.width
                duration: 20000 // 20 seconds for full scroll
            }

            ScriptAction {
                script: {
                    currentIndex = (currentIndex + 1) % scheduleItems.length
                }
            }
        }
    }

    // Timer to update schedule
    Timer {
        id: updateTimer
        interval: 1000
        running: true
        repeat: true
        onTriggered: {
            // TODO: Update scheduleItems from C++ backend
            updateCurrentSchedule()
        }
    }

    function formatScheduleItem(item) {
        if (!item) return ""
        
        var time = item.startTime || "00:00"
        var title = item.title || "Untitled"
        var duration = item.duration ? formatDuration(item.duration) : ""
        
        return time + " - " + title + (duration ? " (" + duration + ")" : "")
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

    function updateCurrentSchedule() {
        // TODO: Fetch current and upcoming schedule items from C++ backend
        // For now, use placeholder data
        if (scheduleItems.length === 0) {
            scheduleItems = [
                { startTime: "10:00", title: "News Program", duration: 1800 },
                { startTime: "10:30", title: "Ad Break", duration: 300 },
                { startTime: "10:35", title: "Talk Show", duration: 3600 }
            ]
        }
    }

    Component.onCompleted: {
        updateCurrentSchedule()
    }
}

