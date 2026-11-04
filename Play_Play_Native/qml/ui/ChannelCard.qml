import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".."

/// One channel row of the control column: preview thumbnail, state, current/next.
Rectangle {
    id: card
    property var channel: ({})          // snapshot entry {profile, process, executor, engineState}
    property var state: ({})            // engine telemetry
    property bool selected: false
    property int index: 0
    signal clicked()
    property string chId: channel && channel.profile ? channel.profile.id : ""
    property string st: state && state.state ? state.state : (channel && channel.engineState ? channel.engineState : "OFFLINE")
    property bool connected: channel && channel.process ? channel.process.connected : false
    property bool armed: channel && channel.executor ? channel.executor.armed : false
    property real remaining: state && state.remaining ? state.remaining : 0

    implicitHeight: 86
    radius: Theme.r5
    color: selected ? Theme.bg3 : Theme.bg2
    border.color: selected ? Theme.accent0 : Theme.border0
    border.width: 1

    RowLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 10
        Rectangle {
            Layout.preferredWidth: 112; Layout.preferredHeight: 63
            color: "#000"; radius: Theme.r3; clip: true
            border.color: card.st === "ON_AIR" ? Theme.onAir : Theme.border1
            // double-buffered, and repaints on this channel's own frames only (previewSerial counts every source's frames)
            ProgramImage { anchors.fill: parent; anchors.margins: 1; channelId: card.chId; live: card.connected; radius: Theme.r3 }
            Text { anchors.centerIn: parent; visible: !card.connected; text: "NO ENGINE"; color: Theme.text3; font.pixelSize: Theme.fs10; font.bold: true }
            Rectangle { anchors.left: parent.left; anchors.bottom: parent.bottom; anchors.margins: 3; width: 24; height: 12; radius: 2; color: "#000000a0"
                Text { anchors.centerIn: parent; text: "CH" + (card.index + 1); color: "#fff"; font.pixelSize: 8; font.bold: true } }
        }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2
            RowLayout {
                Layout.fillWidth: true
                Text { text: card.channel && card.channel.profile ? card.channel.profile.name : "Channel"; color: Theme.text0; font.family: Theme.fontDisplay; font.pixelSize: Theme.fs13; font.bold: true; elide: Text.ElideRight; Layout.fillWidth: true }
                CBadge { text: card.st === "ON_AIR" ? "ON AIR" : card.st; tint: Theme.stateColor(card.st); solid: card.st === "ON_AIR" }
            }
            RowLayout {
                spacing: 6
                Text { text: "CURRENT"; color: Theme.text3; font.pixelSize: Theme.fs10; font.bold: true; Layout.preferredWidth: 52 }
                Text { text: card.state && card.state.title ? card.state.title : "No event loaded"; color: Theme.text0; font.pixelSize: Theme.fs11; elide: Text.ElideRight; Layout.fillWidth: true }
                Text { visible: card.st === "ON_AIR"; text: "-" + Theme.fmtClock(card.remaining); color: Theme.onAir; font.family: Theme.fontMono; font.pixelSize: Theme.fs11; font.bold: true }
            }
            RowLayout {
                spacing: 6
                Text { text: "NEXT"; color: Theme.text3; font.pixelSize: Theme.fs10; font.bold: true; Layout.preferredWidth: 52 }
                Text { text: card.state && card.state.nextTitle ? card.state.nextTitle : "No event loaded"; color: Theme.text1; font.pixelSize: Theme.fs11; elide: Text.ElideRight; Layout.fillWidth: true }
                CBadge { visible: card.armed; text: "ARMED"; tint: Theme.ready }
                CBadge { visible: !!(card.channel.profile && card.channel.profile.slave); text: "SLAVE"; tint: Theme.standby }
            }
        }
    }
    MouseArea { anchors.fill: parent; acceptedButtons: Qt.LeftButton | Qt.RightButton; onClicked: function(m) { card.clicked(); if (m.button === Qt.RightButton) cardMenu.popup() } cursorShape: Qt.PointingHandCursor; z: -1 }
    Menu {
        id: cardMenu
        MenuItem { text: "Play"; onTriggered: { supervisor.play(card.chId) } }
        MenuItem { text: "Pause"; onTriggered: { supervisor.pause(card.chId) } }
        MenuItem { text: "Skip to next"; onTriggered: { supervisor.skip(card.chId) } }
        MenuItem { text: "Hold on slate"; onTriggered: { supervisor.hold(card.chId) } }
        MenuItem { text: "Stop"; onTriggered: { supervisor.stop(card.chId) } }
        MenuSeparator {}
        MenuItem { text: card.armed ? "Disarm schedule" : "Arm schedule"; onTriggered: { supervisor.arm(!card.armed, card.chId) } }
        MenuItem { text: "Load hour now"; onTriggered: { supervisor.loadHour(card.chId) } }
        MenuItem { text: "Snap to schedule"; onTriggered: { supervisor.snap(card.chId) } }
        MenuItem { text: "Re-roll query blocks"; onTriggered: { api.post("/channels/" + card.chId + "/reroll", {}, function(ok) { if (ok) window.toast("Blocks re-rolled", "ok") }) } }
        MenuSeparator {}
        MenuItem { text: "Edit schedule"; onTriggered: { supervisor.activeChannelId = card.chId; window.navTo("schedule") } }
        MenuItem { text: "Programming (recipes, format clock)"; onTriggered: { supervisor.activeChannelId = card.chId; window.navTo("programming") } }
        MenuItem { text: "As-played report (today)"; onTriggered: { Qt.openUrlExternally(api.rawUrl("/reports/asplayed.html?channel=" + card.chId)) } }
        MenuSeparator {}
        MenuItem { text: "Restart engine"; onTriggered: { api.post("/channels/" + card.chId + "/restart", {}, function(ok) { if (ok) window.toast("Engine restarting", "ok") }) } }
    }
}
