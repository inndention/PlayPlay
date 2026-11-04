import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".."

/// "+ Line in" dialog: the machine's sound-card inputs (line in, microphones, virtual cables) and, as loopback, what a playback device is playing.
Popup {
    id: pk
    property string query: ""
    readonly property var filtered: {
        var q = pk.query.trim().toLowerCase(), out = [], all = mixer ? mixer.devices : []
        for (var i = 0; i < all.length; ++i) if (!q.length || all[i].name.toLowerCase().indexOf(q) >= 0 || all[i].kind.indexOf(q) >= 0) out.push(all[i])
        return out
    }
    function openPicker() { query = ""; search.text = ""; mixer.refreshDevices(); open(); search.forceActiveFocus() }

    parent: Overlay.overlay
    modal: true; focus: true; padding: 0
    anchors.centerIn: Overlay.overlay
    width: 520
    height: Math.min(520, (Overlay.overlay ? Overlay.overlay.height : 640) - 80)
    background: Rectangle { color: Theme.bg2; border.color: Theme.border2; radius: Theme.r8 }
    Overlay.modal: Rectangle { color: "#000000a0" }

    ColumnLayout {
        anchors.fill: parent; anchors.margins: 12; spacing: 8
        RowLayout {
            Layout.fillWidth: true
            Text { text: "ADD A LINE INPUT"; color: Theme.text0; font.family: Theme.fontDisplay; font.pixelSize: Theme.fs14; font.bold: true; font.letterSpacing: 1 }
            Item { Layout.fillWidth: true }
            Text { text: pk.filtered.length + " device(s)"; color: Theme.text3; font.pixelSize: Theme.fs10 }
        }
        CField { id: search; Layout.fillWidth: true; placeholder: "Search devices"; onEdited: function(v) { pk.query = v } }
        ListView {
            id: list
            Layout.fillWidth: true; Layout.fillHeight: true; clip: true
            model: pk.filtered
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { }
            delegate: Rectangle {
                width: list.width; height: 38
                color: ma.containsMouse ? Theme.bg5 : index % 2 ? Theme.bg2 : Theme.bg3
                RowLayout {
                    anchors.fill: parent; anchors.leftMargin: 10; anchors.rightMargin: 10; spacing: 10
                    CBadge { text: modelData.kind === "capture" ? "input" : "loopback"; tint: modelData.kind === "capture" ? Theme.accent0 : Theme.accent2 }
                    Text { Layout.fillWidth: true; text: modelData.name; color: Theme.text0; font.pixelSize: Theme.fs12; elide: Text.ElideRight }
                    CBadge { visible: modelData.isDefault; text: "default"; tint: Theme.ready }
                }
                MouseArea { id: ma; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: { mixer.addLine(supervisor.activeChannelId, modelData); pk.close() } }
            }
            Text { anchors.centerIn: parent; visible: pk.filtered.length === 0; width: parent.width - 40; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.WordWrap; color: Theme.text3; font.pixelSize: Theme.fs11
                text: "No audio devices found" }
        }
        RowLayout {
            Layout.fillWidth: true
            Text { Layout.fillWidth: true; text: "“Input” devices are line inputs and microphones. “Loopback” mixes in what a playback device is playing (a program from another application)."; color: Theme.text3; font.pixelSize: Theme.fs10; wrapMode: Text.WordWrap }
            CButton { text: "Cancel"; small: true; onClicked: { pk.close() } }
        }
    }
}
