import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".."

/**
 * The persistent left column of PlayPlayClean: studio clock, channel stack,
 * context bar (selected / current / next / countdown), transport rows, and the
 * hour rundown. Everything binds to `supervisor` (IPC) and sends commands
 * through it; the active channel drives the rest of the window.
 */
Rectangle {
    id: col
    color: Theme.bg1
    property var channels: supervisor.channelModel   // perf-app PA-3: a list model (rows updated in place); .length still works
    property string activeId: supervisor.activeChannelId
    property var st: supervisor.activeState
    property var run: supervisor.activeRunlist
    property var active: supervisor.activeChannelData
    property bool engineUp: (st && st.state && st.state !== "OFFLINE") || (active && active.process ? active.process.connected : false)
    property bool armed: active && active.executor ? active.executor.armed : false
    property string engineState: st && st.state ? st.state : "OFFLINE"
    property string tz: active && active.profile ? active.profile.timezone : ""
    signal openOutput()

    Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.border0 }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 10
        anchors.rightMargin: 11
        spacing: 10

        StudioClock { Layout.fillWidth: true; tz: col.tz }

        // --- channel stack -------------------------------------------------------------
        CPanel {
            Layout.fillWidth: true
            Layout.preferredHeight: 34 + Math.max(1, Math.min(3, col.channels.length)) * 92 + 10
            title: "Channels"
            padding: 6
            headerRight: [
                Text { text: col.engineUp ? "CONTROLLING " + col.activeId.toUpperCase() : "ENGINE OFFLINE"; color: col.engineUp ? Theme.accent0 : Theme.danger; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1 }
            ]
            ScrollView {
                anchors.fill: parent
                clip: true
                contentWidth: availableWidth
                ColumnLayout {
                width: parent.width
                spacing: 6
                Repeater {
                    model: col.channels
                    ChannelCard {
                        Layout.fillWidth: true
                        index: model.index
                        channel: modelData
                        state: supervisor.telemetryOf(modelData.profile.id)
                        selected: modelData.profile.id === col.activeId
                        onClicked: { supervisor.activeChannelId = modelData.profile.id }
                        Connections { target: supervisor; function onTelemetryChanged(id) { if (id === modelData.profile.id) state = supervisor.telemetryOf(id) } }
                    }
                }
                Text { visible: col.channels.length === 0; text: supervisor.connected ? "No channels configured (Config → Channels)" : "Connecting to Supervisor…"; color: Theme.text2; font.pixelSize: Theme.fs11; Layout.alignment: Qt.AlignHCenter; Layout.topMargin: 10 }
                }
            }
        }

        // --- context bar -------------------------------------------------------------------
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: ctx.implicitHeight + 16
            color: Theme.bg2; radius: Theme.r8; border.color: Theme.border0
            GridLayout {
                id: ctx
                anchors.fill: parent; anchors.margins: 8
                columns: 2; columnSpacing: 12; rowSpacing: 6
                ColumnLayout { spacing: 0; Layout.fillWidth: true
                    Text { text: "SELECTED"; color: Theme.text3; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1 }
                    Text { text: col.active && col.active.profile ? col.active.profile.name : "—"; color: Theme.text0; font.pixelSize: Theme.fs12; font.bold: true; elide: Text.ElideRight; Layout.fillWidth: true } }
                ColumnLayout { spacing: 0; Layout.fillWidth: true
                    Text { text: "CURRENT"; color: Theme.text3; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1 }
                    Text { text: col.st && col.st.title ? col.st.title : "No event loaded"; color: Theme.text0; font.pixelSize: Theme.fs12; font.bold: true; elide: Text.ElideRight; Layout.fillWidth: true } }
                ColumnLayout { spacing: 0; Layout.fillWidth: true
                    Text { text: "NEXT"; color: Theme.text3; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1 }
                    Text { text: col.st && col.st.nextTitle ? col.st.nextTitle : "No event loaded"; color: Theme.text1; font.pixelSize: Theme.fs12; elide: Text.ElideRight; Layout.fillWidth: true } }
                ColumnLayout { spacing: 0; Layout.fillWidth: true
                    Text { text: "COUNTDOWN"; color: Theme.text3; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1 }
                    RowLayout { spacing: 8
                        Text { text: col.engineState === "ON_AIR" ? Theme.fmtClock(col.st.remaining) : "--:--"; color: col.engineState === "ON_AIR" ? Theme.onAir : Theme.text2; font.family: Theme.fontMono; font.pixelSize: Theme.fs14; font.bold: true }
                        Text { text: col.st && col.st.timecode ? col.st.timecode : ""; color: Theme.text3; font.family: Theme.fontMono; font.pixelSize: Theme.fs10 } } }
            }
        }

        // --- transport ---------------------------------------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            spacing: 6
            CButton { text: "ARMED"; icon2: "⚡"; variant: col.armed ? "warn" : "default"; Layout.fillWidth: true; onClicked: { supervisor.arm(!col.armed) } ToolTip.text: "Schedule execution on/off" }
            CButton { text: "SCHED"; icon2: "↻"; variant: "warn"; Layout.fillWidth: true; onClicked: { supervisor.snap() } ToolTip.text: "Snap engine to the wall clock" }
            CButton { icon2: "■"; variant: "default"; enabled: col.engineUp; implicitWidth: 40; onClicked: { supervisor.stop(col.activeId) } ToolTip.text: "Stop (slate)" }
            CButton { icon2: col.engineState === "ON_AIR" ? "‖" : "▶"; variant: "success"; enabled: col.engineUp; implicitWidth: 40; onClicked: { col.engineState === "ON_AIR" ? supervisor.pause() : supervisor.play() } ToolTip.text: col.engineState === "ON_AIR" ? "Pause" : "Play" }
            CButton { icon2: "⏭"; variant: "default"; enabled: col.engineUp; implicitWidth: 40; onClicked: { supervisor.skip() } ToolTip.text: "Next item" }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 6
            CButton { text: col.engineUp ? "ENGINE ON" : "ENGINE OFF"; icon2: "●"; variant: col.engineUp ? "danger" : "default"; Layout.fillWidth: true
                onClicked: { api.post("/channels/" + col.activeId + (col.engineUp ? "/stop" : "/start"), {}, function(ok) { supervisor.refresh() }) } ToolTip.text: col.engineUp ? "Stop the channel engine process" : "Start the channel engine process" }
            CButton { text: "LOAD HOUR"; icon2: "⤓"; Layout.fillWidth: true; enabled: col.engineUp; onClicked: { supervisor.loadHour() } ToolTip.text: "Push this hour (join in progress)" }
            CButton { text: "PUSH"; icon2: "⇧"; variant: "success"; Layout.fillWidth: true; enabled: col.engineUp; onClicked: { supervisor.arm(true); supervisor.loadHour() } ToolTip.text: "Arm and push now" }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 6
            Item { Layout.fillWidth: true }
            CButton { icon2: "⛶"; small: true; variant: "ghost"; onClicked: { window.visibility = window.visibility === Window.FullScreen ? Window.Windowed : Window.FullScreen } ToolTip.text: "Fullscreen" }
            CButton { icon2: "↗"; small: true; variant: "ghost"; onClicked: { Qt.openUrlExternally(api.rawUrl("/")) } ToolTip.text: "Open web dashboard" }
            CButton { icon2: "⎘"; small: true; variant: "ghost"; onClicked: { clip.text = api.rawUrl("/preview/" + col.activeId + ".mjpeg"); clip.selectAll(); clip.copy() } ToolTip.text: "Copy preview URL" }
            CButton { icon2: "⬇"; small: true; variant: "ghost"; onClicked: { supervisor.command("reload") } ToolTip.text: "Reload channel profile" }
            TextEdit { id: clip; visible: false }
        }

        VuMeter { Layout.fillWidth: true; meters: supervisor.activeMeters; channels: col.active && col.active.profile ? col.active.profile.audioChannels : 2 }

        HourRundown {
            Layout.fillWidth: true
            Layout.fillHeight: true
            runlist: col.run
            state: col.st
            compact: true
        }
    }
}
