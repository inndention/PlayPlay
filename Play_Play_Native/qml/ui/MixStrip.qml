import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".."

/**
 * One mixer channel strip: name, meters, fader, dB readout and M / S keys (plus AFV on the program strip, and a remove key on
 * line-ins). `strip` is the engine's strip object {id, name, kind, gainDb, mute, itemMute, solo, peaks[], pre:{peaks[], rms[]}}; the strip
 * shows what the engine reports and only follows the local fader while it is being dragged.
 * Meters: the thin pair is the source's own level (pre-fader, what comes in), the wide pair is what this strip puts into the mix
 * (post-fader, mute and solo applied).
 */
Rectangle {
    id: s
    property var strip: ({})
    property bool master: false
    property bool afv: true                  // program strip: audio follows video
    property bool soloActive: false          // some strip is soloed (this one dims when it is not it)
    property bool showAfv: true              // the AFV key (main mix only)
    readonly property real engineDb: strip.gainDb !== undefined ? strip.gainDb : 0
    property real dragDb: 0
    property bool dragging: false             // the local fader value is shown until the engine reports it back
    readonly property real gainDb: dragging ? dragDb : engineDb
    Timer { id: settle; interval: 500; onTriggered: { s.dragging = false } }
    readonly property string kind: strip.kind || (master ? "master" : "layer")
    readonly property color tint: kind === "program" ? Theme.onAir : kind === "line" ? Theme.accent0 : kind === "master" ? Theme.text0 : Theme.accent2
    signal gain(real db)
    signal gainDone(real db)
    signal muteToggled()
    signal soloToggled()
    signal removeClicked()
    signal afvToggled()
    signal menuClicked()
    property real _last: 0

    implicitWidth: 96
    color: Theme.bg3; radius: Theme.r5; border.color: Theme.border0
    opacity: soloActive && !strip.solo && !master ? 0.55 : 1

    ColumnLayout {
        anchors.fill: parent; anchors.margins: 6; spacing: 4
        Rectangle { Layout.fillWidth: true; height: 3; radius: 1; color: s.tint }
        Text { Layout.fillWidth: true; text: master ? "MASTER" : (strip.name || strip.id || ""); color: Theme.text0; font.pixelSize: Theme.fs11; font.bold: true; elide: Text.ElideRight; horizontalAlignment: Text.AlignHCenter
            MouseArea { anchors.fill: parent; acceptedButtons: Qt.RightButton | Qt.LeftButton; onClicked: { s.menuClicked() } } }
        Text { Layout.fillWidth: true; text: master ? "output" : strip.followsMain ? "follows main" : kind === "program" ? (strip.itemMute ? "take muted" : "on-air item") : kind === "line" ? (strip.connected === false ? "NO SIGNAL" : "line in") : "layer " + (strip.layer || ""); color: strip.itemMute || strip.connected === false ? Theme.standby : Theme.text3; font.pixelSize: 9; font.bold: true; font.letterSpacing: 0.8; horizontalAlignment: Text.AlignHCenter; elide: Text.ElideRight }
        RowLayout {
            Layout.fillWidth: true; Layout.fillHeight: true; spacing: 6
            // source meter (pre-fader): thin, dimmer
            Item {
                visible: !master && !!strip.pre && !!strip.pre.peaks
                Layout.fillHeight: true; Layout.preferredWidth: 8
                Row {
                    anchors.fill: parent; spacing: 2
                    Repeater {
                        model: strip.pre && strip.pre.peaks ? strip.pre.peaks.length : 0
                        Rectangle {
                            width: 3; height: parent.height; radius: 1; color: Theme.bg4
                            property real db: strip.pre.peaks[index] !== undefined ? strip.pre.peaks[index] : -100
                            property real fr: Math.max(0, Math.min(1, (db + 60) / 60))
                            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: parent.height * parent.fr; radius: 1; opacity: 0.7
                                color: parent.db > -3 ? Theme.onAir : parent.db > -12 ? Theme.warn : Theme.accent0 }
                        }
                    }
                }
                MouseArea { id: preMa; anchors.fill: parent; hoverEnabled: true; acceptedButtons: Qt.NoButton }
                ToolTip.visible: preMa.containsMouse; ToolTip.text: "Source level (before the fader)"
            }
            // mix meter (post-fader)
            Row {
                Layout.fillHeight: true; Layout.preferredWidth: 18; spacing: 3
                Repeater {
                    model: Math.max(2, strip.peaks ? strip.peaks.length : 0)
                    Rectangle {
                        width: 7; height: parent.height; radius: 2; color: Theme.bg4
                        property real db: strip.peaks && strip.peaks[index] !== undefined ? strip.peaks[index] : -100
                        property real fr: Math.max(0, Math.min(1, (db + 60) / 60))
                        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: parent.height * parent.fr; radius: 2
                            color: parent.db > -3 ? Theme.onAir : parent.db > -12 ? Theme.warn : Theme.ready }
                    }
                }
            }
            Fader {
                Layout.fillHeight: true; Layout.fillWidth: true; Layout.minimumHeight: 70
                value: s.gainDb
                onMoved: function(db) { s.dragDb = db; s.dragging = true; settle.restart(); var now = Date.now(); if (now - s._last > 40) { s._last = now; s.gain(db >= -59.5 ? db : -100) } }
                onReleased: { s.gain(s.dragDb >= -59.5 ? s.dragDb : -100); s.gainDone(s.dragDb); settle.restart() }
            }
        }
        Text { Layout.fillWidth: true; text: s.gainDb <= -59.5 ? "−∞ dB" : (s.gainDb > 0 ? "+" : "") + s.gainDb.toFixed(1) + " dB"; color: Theme.text1; font.family: Theme.fontMono; font.pixelSize: Theme.fs10; horizontalAlignment: Text.AlignHCenter }
        RowLayout {
            Layout.fillWidth: true; spacing: 4; visible: !master
            Rectangle { Layout.fillWidth: true; height: 22; radius: 3; color: strip.mute ? Theme.onAir : Theme.bg4; border.color: strip.mute ? Theme.onAir : Theme.border1
                Text { anchors.centerIn: parent; text: "M"; color: strip.mute ? "#05050a" : Theme.text1; font.pixelSize: Theme.fs11; font.bold: true }
                MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: { s.muteToggled() } } }
            Rectangle { Layout.fillWidth: true; height: 22; radius: 3; color: strip.solo ? Theme.warn : Theme.bg4; border.color: strip.solo ? Theme.warn : Theme.border1
                Text { anchors.centerIn: parent; text: "S"; color: strip.solo ? "#05050a" : Theme.text1; font.pixelSize: Theme.fs11; font.bold: true }
                MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: { s.soloToggled() } } }
        }
        Rectangle {
            Layout.fillWidth: true; height: 20; radius: 3; visible: kind === "program" && s.showAfv
            color: s.afv ? Qt.rgba(0.19, 0.82, 0.35, 0.25) : Theme.bg4; border.color: s.afv ? Theme.ready : Theme.border1
            Text { anchors.centerIn: parent; text: "AFV"; color: s.afv ? Theme.ready : Theme.text2; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1 }
            MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; hoverEnabled: true; onClicked: { s.afvToggled() }
                ToolTip.visible: containsMouse; ToolTip.text: "Audio follows video: ON = a taken input brings its audio; OFF = a take is silent so a line-in can be the voice-over" }
        }
        Rectangle {
            Layout.fillWidth: true; height: 20; radius: 3; visible: kind === "line"; color: Theme.bg4; border.color: Theme.border1
            Text { anchors.centerIn: parent; text: "✕ remove"; color: Theme.text2; font.pixelSize: Theme.fs10 }
            MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: { s.removeClicked() } }
        }
    }
}
