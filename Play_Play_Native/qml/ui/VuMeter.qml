import QtQuick
import QtQuick.Layouts
import ".."

/// Program audio VU: one bar per channel from the engine's per-frame peaks (fed at the preview rate), with peak hold.
Rectangle {
    id: vu
    property var meters: ({})
    property int channels: 2
    property var peaks: meters && meters.channelPeaksDbfs ? meters.channelPeaksDbfs : []
    // Peak hold in C++ (perf-app PA-5): the shared meterTicker decays every meter's holds with one timer, and only while one
    // is above the floor (a 150 ms Timer per meter ran for ever, copying and reassigning this array on every fire).
    property QtObject peakHold: null
    readonly property var hold: peakHold ? peakHold.hold : []
    color: Theme.bg2; radius: Theme.r8; border.color: Theme.border0
    implicitHeight: 14 + Math.max(channels, peaks.length) * 14 + 8
    function pos(db) { return Math.max(0, Math.min(1, (db + 60) / 60)) }
    onPeaksChanged: { if (peakHold) peakHold.feed(peaks) }
    Component.onCompleted: { if (typeof meterTicker !== "undefined") { peakHold = meterTicker.createHold(vu); peakHold.feed(peaks) } }

    ColumnLayout {
        anchors.fill: parent; anchors.margins: 6; spacing: 2
        RowLayout {
            Layout.fillWidth: true
            Text { text: "AUDIO"; color: Theme.text2; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1 }
            Item { Layout.fillWidth: true }
            Text { text: vu.meters && vu.meters.momentaryLufs !== undefined ? "M " + Number(vu.meters.momentaryLufs).toFixed(1) + " LUFS" : ""; color: Theme.text1; font.family: Theme.fontMono; font.pixelSize: Theme.fs10 }
            Text { visible: vu.meters && vu.meters.duckGainDb < -0.5; text: "DUCK"; color: Theme.standby; font.pixelSize: Theme.fs10; font.bold: true }
        }
        Repeater {
            model: Math.max(vu.channels, vu.peaks.length)
            RowLayout {
                Layout.fillWidth: true; spacing: 6
                property real db: vu.peaks[index] !== undefined ? vu.peaks[index] : -100
                property real hd: vu.hold[index] !== undefined ? vu.hold[index] : -100
                Text { text: index < 2 ? (index === 0 ? "L" : "R") : String(index + 1); color: Theme.text3; font.family: Theme.fontMono; font.pixelSize: Theme.fs10; Layout.preferredWidth: 10 }
                Rectangle {
                    Layout.fillWidth: true; height: 10; radius: 2; color: Theme.bg4
                    Rectangle { width: parent.width * vu.pos(db); height: parent.height; radius: 2
                        gradient: Gradient { orientation: Gradient.Horizontal
                            GradientStop { position: 0.0; color: Theme.ready } GradientStop { position: 0.66; color: Theme.ready } GradientStop { position: 0.8; color: Theme.warn } GradientStop { position: 1.0; color: Theme.onAir } }
                        Behavior on width { NumberAnimation { duration: 80 } } }
                    Rectangle { x: parent.width * vu.pos(hd) - 1; width: 2; height: parent.height; color: hd > -3 ? Theme.onAir : "#fff"; visible: hd > -60 }
                    Repeater { model: [-40, -20, -10, -3]; Rectangle { x: parent.width * vu.pos(modelData); width: 1; height: parent.height; color: Theme.border1; opacity: 0.8 } }
                }
                Text { text: db > -99 ? db.toFixed(0) : "−∞"; color: db > -3 ? Theme.onAir : Theme.text2; font.family: Theme.fontMono; font.pixelSize: Theme.fs10; Layout.preferredWidth: 26; horizontalAlignment: Text.AlignRight }
            }
        }
    }
}
