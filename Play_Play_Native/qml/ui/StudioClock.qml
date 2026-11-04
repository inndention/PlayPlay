import QtQuick
import QtQuick.Layouts
import ".."

/// Local time, UTC, date and the top-of-hour countdown (warning <5 min, critical <1 min).
Rectangle {
    id: clock
    color: Theme.bg2
    radius: Theme.r8
    border.color: Theme.border0
    implicitHeight: 92
    property date now: new Date()
    property int tohSec: 3600 - (now.getMinutes() * 60 + now.getSeconds())
    property color tohColor: tohSec < 60 ? Theme.onAir : tohSec < 300 ? Theme.standby : Theme.ready
    property string tz: ""
    Timer { interval: 250; running: true; repeat: true; onTriggered: { clock.now = new Date() } }

    RowLayout {
        anchors.fill: parent
        anchors.margins: 10
        spacing: 12
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 0
            Text { text: "LOCAL TIME"; color: Theme.text3; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1.2 }
            RowLayout {
                spacing: 6
                Text { text: Qt.formatTime(clock.now, "hh:mm:ss"); color: Theme.text0; font.family: Theme.fontMono; font.pixelSize: Theme.fs40; font.bold: true }
                Text { text: Qt.formatTime(clock.now, "AP"); color: Theme.text2; font.pixelSize: Theme.fs12; font.bold: true; Layout.alignment: Qt.AlignBottom; Layout.bottomMargin: 8 }
            }
            Text { text: "UTC " + Qt.formatTime(new Date(clock.now.getTime() + clock.now.getTimezoneOffset() * 60000), "hh:mm:ss") + (clock.tz.length ? "  ·  " + clock.tz : ""); color: Theme.text2; font.family: Theme.fontMono; font.pixelSize: Theme.fs10 }
        }
        ColumnLayout {
            spacing: 0
            Layout.alignment: Qt.AlignRight
            Text { text: Qt.formatDate(clock.now, "ddd d MMM yyyy").toUpperCase(); color: Theme.text3; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1; Layout.alignment: Qt.AlignRight }
            Text { text: Theme.pad2(Math.floor(clock.tohSec / 60)) + ":" + Theme.pad2(clock.tohSec % 60); color: clock.tohColor; font.family: Theme.fontMono; font.pixelSize: Theme.fs28; font.bold: true; Layout.alignment: Qt.AlignRight }
            Text { text: "TO TOP OF HOUR"; color: Theme.text3; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1; Layout.alignment: Qt.AlignRight }
        }
    }
}
