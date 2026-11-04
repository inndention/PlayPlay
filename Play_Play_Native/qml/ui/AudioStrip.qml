import QtQuick
import QtQuick.Layouts
import ".."

/// Compact program audio: per-channel peak bars with peak hold and the R128 readout, in one strip (the wall's PVW column).
Rectangle {
    id: a
    property var meters: ({})                 // {channelPeaksDbfs[], ...}
    property var audio: ({})                  // compositor audio: {momentaryLufs, shortTermLufs, integratedLufs, peakDbfs, duckGainDb, clips}
    property int channels: 2
    property var peaks: meters && meters.channelPeaksDbfs ? meters.channelPeaksDbfs : []
    // Peak hold in C++ (perf-app PA-5): the shared meterTicker decays every meter's holds with one timer, and only while one
    // is above the floor (a 150 ms Timer per meter ran for ever, also while this strip is hidden behind the drawers).
    property QtObject peakHold: null
    readonly property var hold: peakHold ? peakHold.hold : []
    property real target: -24.0
    color: Theme.bg2; radius: Theme.r5; border.color: Theme.border0
    implicitHeight: 58
    function pos(db) { return Math.max(0, Math.min(1, (db + 60) / 60)) }
    function v(k, d) { return audio && audio[k] !== undefined ? audio[k] : d }
    onPeaksChanged: { if (peakHold) peakHold.feed(peaks) }
    Component.onCompleted: { if (typeof meterTicker !== "undefined") { peakHold = meterTicker.createHold(a); peakHold.feed(peaks) } }

    ColumnLayout {
        anchors.fill: parent; anchors.margins: 6; spacing: 2
        RowLayout {
            Layout.fillWidth: true; spacing: 8
            Text { text: "AUDIO"; color: Theme.text3; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1 }
            Item { Layout.fillWidth: true }
            Text { text: "M " + a.v("momentaryLufs", -70).toFixed(1); color: Theme.text0; font.family: Theme.fontMono; font.pixelSize: Theme.fs10 }
            Text { text: "S " + a.v("shortTermLufs", -70).toFixed(1); color: Theme.text1; font.family: Theme.fontMono; font.pixelSize: Theme.fs10 }
            Text { text: "I " + a.v("integratedLufs", -70).toFixed(1) + " LUFS"; color: Theme.accent0; font.family: Theme.fontMono; font.pixelSize: Theme.fs10; font.bold: true }
            Text { visible: a.v("clips", 0) > 0; text: "CLIPS " + Math.round(a.v("clips", 0)); color: Theme.onAir; font.family: Theme.fontMono; font.pixelSize: Theme.fs10; font.bold: true }
            Text { visible: a.v("duckGainDb", 0) < -0.5; text: "DUCK"; color: Theme.standby; font.pixelSize: Theme.fs10; font.bold: true }
        }
        Repeater {
            model: Math.max(a.channels, a.peaks.length, 2)
            RowLayout {
                Layout.fillWidth: true; spacing: 6
                property real db: a.peaks[index] !== undefined ? a.peaks[index] : -100
                property real hd: a.hold[index] !== undefined ? a.hold[index] : -100
                Text { text: index === 0 ? "L" : index === 1 ? "R" : String(index + 1); color: Theme.text3; font.family: Theme.fontMono; font.pixelSize: Theme.fs10; Layout.preferredWidth: 10 }
                Rectangle {
                    Layout.fillWidth: true; height: 8; radius: 2; color: Theme.bg4
                    Rectangle { width: parent.width * a.pos(db); height: parent.height; radius: 2
                        gradient: Gradient { orientation: Gradient.Horizontal
                            GradientStop { position: 0.0; color: Theme.ready } GradientStop { position: 0.66; color: Theme.ready } GradientStop { position: 0.8; color: Theme.warn } GradientStop { position: 1.0; color: Theme.onAir } }
                        Behavior on width { NumberAnimation { duration: 80 } } }
                    Rectangle { x: parent.width * a.pos(hd) - 1; width: 2; height: parent.height; color: hd > -3 ? Theme.onAir : "#fff"; visible: hd > -60 }
                    Rectangle { x: parent.width * a.pos(a.target) - 1; width: 1; height: parent.height; color: Theme.accent0; opacity: 0.8 }
                }
                Text { text: db > -99 ? db.toFixed(0) : "−∞"; color: db > -3 ? Theme.onAir : Theme.text2; font.family: Theme.fontMono; font.pixelSize: Theme.fs10; Layout.preferredWidth: 24; horizontalAlignment: Text.AlignRight }
            }
        }
    }
}
