import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".."

/// The graphics layers on air right now (engine compositor layers, annotated with the overlay manager's template / text). Each row hides its layer.
CPanel {
    id: p
    property var layers: []                 // [{layer, role, name, detail, state, managed}]
    signal hideLayer(var entry)
    signal clearAll()
    function roleColor(r) {
        switch (String(r)) {
        case "lower_third": return Theme.accent0
        case "bug": return Theme.clsGraphics
        case "ticker": return Theme.clsBumper
        case "secondary": return Theme.accent2
        case "legal_id": return Theme.clsPreroll
        case "emergency": return Theme.onAir
        default: return Theme.text1
        }
    }
    title: "Graphics on air"
    padding: 0
    headerRight: [
        CBadge { visible: p.layers.length > 0; text: p.layers.length + " live"; tint: Theme.onAir; solid: true },
        CButton { text: "CLEAR ALL"; small: true; variant: "danger"; enabled: p.layers.length > 0; onClicked: { p.clearAll() } ToolTip.text: "Take every graphics layer off air" },
        Item { width: 4; height: 1 }
    ]
    ListView {
        id: list
        anchors.fill: parent; clip: true
        model: p.layers
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { }
        delegate: Rectangle {
            width: list.width; height: 38
            color: index % 2 ? Theme.bg2 : Theme.bg3
            Rectangle { width: 3; height: parent.height; color: p.roleColor(modelData.role) }
            RowLayout {
                anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 8; spacing: 8
                Text { text: modelData.layer; color: Theme.text3; font.family: Theme.fontMono; font.pixelSize: Theme.fs11; Layout.preferredWidth: 22 }
                CBadge { text: String(modelData.role).replace("_", " "); tint: p.roleColor(modelData.role) }
                ColumnLayout {
                    Layout.fillWidth: true; spacing: 0
                    Text { Layout.fillWidth: true; text: modelData.name; color: Theme.text0; font.pixelSize: Theme.fs12; font.bold: true; elide: Text.ElideRight }
                    Text { Layout.fillWidth: true; visible: text.length > 0; text: modelData.detail; color: Theme.text2; font.pixelSize: Theme.fs10; elide: Text.ElideRight }
                }
                CBadge { text: modelData.state; tint: modelData.state === "shown" || modelData.state === "on_air" ? Theme.ready : modelData.state === "hiding" ? Theme.standby : Theme.cued }
                CButton { icon2: "✕"; small: true; variant: "ghost"; onClicked: { p.hideLayer(modelData) } ToolTip.text: "Take this layer off air"; ToolTip.visible: hovered }
            }
        }
        Column {
            anchors.centerIn: parent; visible: p.layers.length === 0; spacing: 4
            Text { anchors.horizontalCenter: parent.horizontalCenter; text: "◩"; color: Theme.text3; font.pixelSize: 24 }
            Text { anchors.horizontalCenter: parent.horizontalCenter; text: "No graphics on air"; color: Theme.text3; font.pixelSize: Theme.fs11 }
        }
    }
}
