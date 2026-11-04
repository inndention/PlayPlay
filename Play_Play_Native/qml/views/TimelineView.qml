import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".."
import "../ui"

/// Timeline: tracks for the pushed runlist (Video 1, CG Gfx, Audio, Switching) with a playhead.
Item {
    id: view
    property var run: supervisor.activeRunlist
    property var st: supervisor.activeState
    property real hourStart: run && run.current && run.current.hourStartMs ? run.current.hourStartMs : Math.floor(Date.now() / 3600000) * 3600000
    property real zoom: 1.0          // 1 = one hour fits
    property real nowMs: Date.now()
    property var items: { var c = run && run.current && run.current.items ? run.current.items : []; var n = run && run.next && run.next.items ? run.next.items : []; return c.concat(n) }
    Timer { interval: 500; running: true; repeat: true; onTriggered: { view.nowMs = Date.now() } }

    ColumnLayout {
        anchors.fill: parent
        spacing: 10
        Rectangle {
            Layout.fillWidth: true; implicitHeight: 44; color: Theme.bg2; radius: Theme.r8; border.color: Theme.border0
            RowLayout {
                anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 12; spacing: 8
                CButton { icon2: "■"; small: true; onClicked: { supervisor.stop(supervisor.activeChannelId) } }
                CButton { icon2: view.st.state === "ON_AIR" ? "‖" : "▶"; small: true; variant: "success"; onClicked: { view.st.state === "ON_AIR" ? supervisor.pause() : supervisor.play() } }
                CButton { icon2: "⏭"; small: true; onClicked: { supervisor.skip() } }
                Item { width: 12 }
                Text { text: "CURRENT"; color: Theme.text3; font.pixelSize: Theme.fs10; font.bold: true }
                Text { text: view.st.timecode || "00:00:00:00"; color: Theme.text0; font.family: Theme.fontMono; font.pixelSize: Theme.fs16; font.bold: true }
                Text { text: "POSITION"; color: Theme.text3; font.pixelSize: Theme.fs10; font.bold: true; Layout.leftMargin: 10 }
                Text { text: Theme.fmtClock(view.st.position || 0) + " / " + Theme.fmtClock(view.st.duration || 0); color: Theme.accent0; font.family: Theme.fontMono; font.pixelSize: Theme.fs14 }
                Item { Layout.fillWidth: true }
                Text { text: "⌕"; color: Theme.text2 }
                Slider { from: 0.5; to: 6; value: view.zoom; Layout.preferredWidth: 160; onMoved: { view.zoom = value } }
                CButton { text: "Now"; small: true; onClicked: { flick.contentX = Math.max(0, view.xOf(view.nowMs) - flick.width / 3) } }
            }
        }
        CPanel {
            Layout.fillWidth: true; Layout.fillHeight: true; padding: 0
            Flickable {
                id: flick
                anchors.fill: parent
                contentWidth: tracks.width; contentHeight: tracks.height; clip: true
                ScrollBar.horizontal: ScrollBar {}
                Item {
                    id: tracks
                    width: Math.max(flick.width, flick.width * view.zoom * 2)
                    height: 44 + 4 * 64
                    // time ruler
                    Rectangle { width: parent.width; height: 24; color: Theme.bg1
                        Repeater { model: 121
                            Item { x: index * tracks.width / 120; Rectangle { width: 1; height: index % 10 === 0 ? 24 : index % 5 === 0 ? 12 : 6; y: 24 - height; color: index % 10 === 0 ? Theme.border2 : Theme.border1 }
                                Text { visible: index % 10 === 0; x: 3; y: 2; text: Theme.fmtTime(view.hourStart + index * 60000); color: Theme.text2; font.family: Theme.fontMono; font.pixelSize: 9 } } } }
                    // tracks
                    Column {
                        y: 28; width: parent.width
                        Repeater {
                            model: [ { name: "VIDEO 1", kind: "video" }, { name: "CG GFX OVERLAY", kind: "cg" }, { name: "AUDIO", kind: "audio" }, { name: "SWITCHING", kind: "switch" } ]
                            Rectangle {
                                width: tracks.width; height: 64; color: index % 2 ? Theme.bg2 : Theme.bg3
                                property string kind: modelData.kind
                                Rectangle { width: parent.width; height: 1; color: Theme.border0 }
                                Text { x: 8; y: 4; text: modelData.name; color: Theme.text3; font.pixelSize: 9; font.bold: true; font.letterSpacing: 1 }
                                Repeater {
                                    model: view.items
                                    Rectangle {
                                        property var it: modelData
                                        property bool onAir: view.st.itemId === it.id
                                        visible: !!(kind === "video" || kind === "audio" || (kind === "cg" && it.cgOnStart && it.cgOnStart.length) || (kind === "switch" && (it.hard || (it.transition && it.transition.type && it.transition.type !== "cut"))))
                                        x: view.xOf(it.plannedStartMs); width: kind === "switch" ? 6 : Math.max(2, view.xOf(it.plannedEndMs) - x - 1)
                                        y: 18; height: kind === "cg" ? 20 : 40; radius: 3
                                        color: kind === "video" ? (onAir ? Theme.onAir : Theme.typeColor(it.type)) : kind === "audio" ? Theme.clsAudio : kind === "cg" ? Theme.clsGraphics : (it.hard ? Theme.standby : Theme.clsEffects)
                                        opacity: kind === "audio" ? 0.5 : 0.9
                                        border.color: onAir ? "#fff" : "transparent"
                                        Text { anchors.fill: parent; anchors.margins: 4; visible: kind === "video" && parent.width > 40; text: it.title || ""; color: "#fff"; font.pixelSize: Theme.fs10; font.bold: true; elide: Text.ElideRight; wrapMode: Text.NoWrap }
                                        Text { anchors.fill: parent; anchors.margins: 2; visible: kind === "cg" && parent.width > 30; text: it.cgOnStart ? it.cgOnStart.join(", ") : ""; color: "#000"; font.pixelSize: 9; elide: Text.ElideRight }
                                        Text { anchors.bottom: parent.bottom; anchors.right: parent.right; anchors.margins: 3; visible: kind === "video" && parent.width > 60; text: Theme.fmtClock(it.duration); color: "#ffffffb0"; font.family: Theme.fontMono; font.pixelSize: 9 }
                                    }
                                }
                            }
                        }
                    }
                    // playhead
                    Rectangle { x: view.xOf(view.nowMs); width: 2; height: tracks.height; color: Theme.onAir; z: 5
                        Rectangle { width: 10; height: 10; rotation: 45; x: -4; y: 18; color: Theme.onAir } }
                }
            }
        }
    }
    function xOf(ms) { return (ms - hourStart) / 7200000 * tracks.width }
}
