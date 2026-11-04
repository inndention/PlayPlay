import QtQuick
import QtQuick.Layouts
import ".."

/// R128 loudness readout from the engine's compositor telemetry.
Rectangle {
    id: m
    property var audio: ({})      // {momentaryLufs, shortTermLufs, integratedLufs, peakDbfs, duckGainDb, clips}
    property real target: -24.0   // ATSC A/85 dialnorm target
    color: Theme.bg2; radius: Theme.r5; border.color: Theme.border0
    implicitHeight: 74
    function v(k, d) { return audio && audio[k] !== undefined ? audio[k] : d }
    function pos(lufs) { return Math.max(0, Math.min(1, (lufs + 60) / 60)) }

    ColumnLayout {
        anchors.fill: parent; anchors.margins: 8; spacing: 4
        RowLayout {
            Layout.fillWidth: true
            Text { text: "LOUDNESS"; color: Theme.text2; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1 }
            Item { Layout.fillWidth: true }
            Text { text: "M " + m.v("momentaryLufs", -70).toFixed(1); color: Theme.text0; font.family: Theme.fontMono; font.pixelSize: Theme.fs11 }
            Text { text: "S " + m.v("shortTermLufs", -70).toFixed(1); color: Theme.text1; font.family: Theme.fontMono; font.pixelSize: Theme.fs11 }
            Text { text: "I " + m.v("integratedLufs", -70).toFixed(1) + " LUFS"; color: Theme.accent0; font.family: Theme.fontMono; font.pixelSize: Theme.fs11; font.bold: true }
        }
        Rectangle {
            Layout.fillWidth: true; height: 10; radius: 2; color: Theme.bg4
            Rectangle { width: parent.width * m.pos(m.v("momentaryLufs", -70)); height: parent.height; radius: 2
                color: m.v("momentaryLufs", -70) > m.target + 2 ? Theme.onAir : m.v("momentaryLufs", -70) > m.target - 2 ? Theme.ready : Theme.accent0 }
            Rectangle { x: parent.width * m.pos(m.target) - 1; width: 2; height: parent.height; color: "#fff"; opacity: 0.7 }
            Rectangle { x: parent.width * m.pos(m.v("shortTermLufs", -70)) - 1; width: 2; height: parent.height; color: Theme.warn }
        }
        RowLayout {
            Layout.fillWidth: true
            Text { text: "PEAK " + m.v("peakDbfs", -100).toFixed(1) + " dBFS"; color: m.v("peakDbfs", -100) > -1 ? Theme.onAir : Theme.text2; font.family: Theme.fontMono; font.pixelSize: Theme.fs10 }
            Text { text: "DUCK " + m.v("duckGainDb", 0).toFixed(1) + " dB"; color: m.v("duckGainDb", 0) < -0.5 ? Theme.standby : Theme.text2; font.family: Theme.fontMono; font.pixelSize: Theme.fs10 }
            Item { Layout.fillWidth: true }
            Text { text: "CLIPS " + Math.round(m.v("clips", 0)); color: m.v("clips", 0) > 0 ? Theme.onAir : Theme.text3; font.family: Theme.fontMono; font.pixelSize: Theme.fs10 }
            Text { text: "TARGET " + m.target.toFixed(0); color: Theme.text3; font.family: Theme.fontMono; font.pixelSize: Theme.fs10 }
        }
    }
}
