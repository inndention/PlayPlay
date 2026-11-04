import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".."
import "../ui"

/// Logs: Supervisor alerts and the live as-run feed (all channels).
Item {
    id: view
    property string filter: ""
    RowLayout {
        anchors.fill: parent
        spacing: 10
        CPanel {
            Layout.fillWidth: true; Layout.fillHeight: true; padding: 0
            title: "Alerts (" + supervisor.alerts.length + ")"
            headerRight: [
                CButton { text: "Clear"; small: true; variant: "ghost"; onClicked: { supervisor.clearAlerts() } }
            ]
            ListView {
                anchors.fill: parent; clip: true; model: supervisor.alerts
                ScrollBar.vertical: ScrollBar {}
                delegate: Rectangle {
                    width: ListView.view.width; height: 40; color: index % 2 ? Theme.bg2 : Theme.bg3
                    property color lvl: modelData.level === "critical" || modelData.level === "error" ? Theme.danger : modelData.level === "warning" ? Theme.warn : Theme.accent0
                    Rectangle { width: 3; height: parent.height; color: lvl }
                    RowLayout { anchors.fill: parent; anchors.leftMargin: 10; anchors.rightMargin: 10; spacing: 10
                        Text { text: Theme.fmtTime(modelData.utcMs, true); color: Theme.text2; font.family: Theme.fontMono; font.pixelSize: Theme.fs10; Layout.preferredWidth: 80 }
                        CBadge { text: modelData.level || "info"; tint: lvl }
                        Text { text: modelData.channelId || ""; color: Theme.text2; font.pixelSize: Theme.fs10; Layout.preferredWidth: 50 }
                        Text { text: modelData.message || ""; color: Theme.text0; font.pixelSize: Theme.fs11; elide: Text.ElideRight; wrapMode: Text.NoWrap; Layout.fillWidth: true } }
                }
                Text { anchors.centerIn: parent; visible: supervisor.alerts.length === 0; text: "No alerts this session"; color: Theme.text3; font.pixelSize: Theme.fs11 }
            }
        }
        CPanel {
            Layout.fillWidth: true; Layout.fillHeight: true; padding: 0
            title: "As-run feed (live)"
            ListView {
                anchors.fill: parent; clip: true; model: supervisor.recentAsRun
                ScrollBar.vertical: ScrollBar {}
                delegate: Rectangle {
                    width: ListView.view.width; height: 30; color: index % 2 ? Theme.bg2 : Theme.bg3
                    RowLayout { anchors.fill: parent; anchors.leftMargin: 10; anchors.rightMargin: 10; spacing: 8
                        Text { text: Theme.fmtTime(modelData.actualStartMs || modelData.plannedStartMs, true); color: Theme.text2; font.family: Theme.fontMono; font.pixelSize: Theme.fs10; Layout.preferredWidth: 80 }
                        Text { text: modelData.channelId || ""; color: Theme.text2; font.pixelSize: Theme.fs10; Layout.preferredWidth: 40 }
                        Text { text: modelData.eventType || ""; color: modelData.eventType === "START" ? Theme.ready : modelData.eventType === "FAILED" ? Theme.danger : Theme.text1; font.pixelSize: Theme.fs11; font.bold: true; Layout.preferredWidth: 64 }
                        Text { text: modelData.title || ""; color: Theme.text0; font.pixelSize: Theme.fs11; elide: Text.ElideRight; Layout.fillWidth: true }
                        Text { text: modelData.status || ""; color: modelData.status === "ON_TIME" ? Theme.ready : modelData.status === "LATE_START" ? Theme.standby : Theme.text2; font.pixelSize: Theme.fs10 }
                        Text { text: modelData.eventType === "START" && modelData.varianceMs !== undefined ? ((modelData.varianceMs > 0 ? "+" : "") + (modelData.varianceMs / 1000).toFixed(2) + " s") : ""; color: Theme.text2; font.family: Theme.fontMono; font.pixelSize: Theme.fs10 } }
                }
                Text { anchors.centerIn: parent; visible: supervisor.recentAsRun.length === 0; text: "Waiting for events…"; color: Theme.text3; font.pixelSize: Theme.fs11 }
            }
        }
    }
}
