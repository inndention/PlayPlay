import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".."
import "../ui"

CPanel {
    title: "Help"
    Flickable {
        anchors.fill: parent; contentHeight: col.implicitHeight; clip: true
        ColumnLayout {
            id: col; width: parent.width; spacing: 14
            Text { text: "PLAY PLAY broadcast automation"; color: Theme.text0; font.family: Theme.fontDisplay; font.pixelSize: Theme.fs20; font.bold: true }
            Text { Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.text1; font.pixelSize: Theme.fs12
                text: "The Supervisor is the automation host: it owns the schedule database, spawns one channel engine per channel and pushes each hour's runlist with join-in-progress and hard starts at :00. This app is a client; the web dashboard served by the Supervisor shows the same state." }
            Text { text: "KEYBOARD"; color: Theme.text2; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1.2 }
            GridLayout {
                columns: 4; columnSpacing: 24; rowSpacing: 4
                Repeater {
                    model: [ ["F1", "On Air"], ["F2", "Schedule"], ["F3", "Reports"], ["F4", "Timeline"], ["F5", "Output"], ["F6", "CG"], ["F7", "Config"], ["F8", "Logs"],
                             ["Space", "Play / pause"], ["Ctrl+→", "Next item"], ["Ctrl+L", "Hide control column"], ["Esc", "Close editor"] ]
                    Row { spacing: 8
                        Rectangle { width: 64; height: 20; radius: 3; color: Theme.bg4; border.color: Theme.border1; Text { anchors.centerIn: parent; text: modelData[0]; color: Theme.text0; font.family: Theme.fontMono; font.pixelSize: Theme.fs10 } }
                        Text { text: modelData[1]; color: Theme.text1; font.pixelSize: Theme.fs11; anchors.verticalCenter: parent.verticalCenter } }
                }
            }
            Text { text: "LAYER ROLES"; color: Theme.text2; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1.2 }
            Text { Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.text1; font.family: Theme.fontMono; font.pixelSize: Theme.fs11
                text: "10 program   15 secondary (PiP / live)   20 bug   30 lower third   40 ticker   80 legal ID   90 emergency slate" }
            Text { text: "ENDPOINTS"; color: Theme.text2; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1.2 }
            Text { Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.text1; font.family: Theme.fontMono; font.pixelSize: Theme.fs11
                text: api.baseUrl + "/api/v1/status\n" + api.baseUrl + "/api/v1/channels/{id}/state | runlist | transport | cg | outputs\n" + api.baseUrl + "/api/v1/schedule/{ch}/week | slot/{id}\n" + api.baseUrl + "/api/v1/media | /media/scan\n" + api.baseUrl + "/api/v1/asrun\n" + api.baseUrl + "/preview/{id}.mjpeg" }
        }
    }
}
