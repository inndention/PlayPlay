import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".."

/// "Map to…" dialog: search the catalogue of live inputs (library entries, NDI sources on the network, ONVIF cameras) and map one to a bus key.
Popup {
    id: pk
    property var im: inputMap
    property int slot: 0
    property string query: ""
    readonly property var filtered: {
        var q = pk.query.trim().toLowerCase(), out = []
        var cat = pk.im ? pk.im.catalog : []
        for (var i = 0; i < cat.length; ++i)
            if (!q.length || (cat[i].title + " " + cat[i].sub + " " + cat[i].kind).toLowerCase().indexOf(q) >= 0) out.push(cat[i])
        return out
    }
    function openFor(i) { slot = i; query = ""; search.text = ""; if (im) im.refresh(); open(); search.forceActiveFocus() }

    parent: Overlay.overlay
    modal: true; focus: true; padding: 0
    anchors.centerIn: Overlay.overlay
    width: 560
    height: Math.min(600, (Overlay.overlay ? Overlay.overlay.height : 700) - 80)
    background: Rectangle { color: Theme.bg2; border.color: Theme.border2; radius: Theme.r8 }
    Overlay.modal: Rectangle { color: "#000000a0" }

    ColumnLayout {
        anchors.fill: parent; anchors.margins: 12; spacing: 8
        RowLayout {
            Layout.fillWidth: true
            Text { text: "MAP INPUT " + (pk.slot + 1) + " TO…"; color: Theme.text0; font.family: Theme.fontDisplay; font.pixelSize: Theme.fs14; font.bold: true; font.letterSpacing: 1 }
            Item { Layout.fillWidth: true }
            Text { text: pk.filtered.length + " source(s)"; color: Theme.text3; font.pixelSize: Theme.fs10 }
        }
        CField { id: search; Layout.fillWidth: true; placeholder: "Search name, address or type (ndi, camera, sdi, url)"; onEdited: function(v) { pk.query = v } onAccepted: { if (pk.filtered.length) pk.choose(pk.filtered[0]) } }
        ListView {
            id: list
            Layout.fillWidth: true; Layout.fillHeight: true; clip: true
            model: pk.filtered
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { }
            section.property: "group"
            section.delegate: Rectangle {
                width: list.width; height: 24; color: Theme.bg1
                Text { anchors.left: parent.left; anchors.leftMargin: 10; anchors.verticalCenter: parent.verticalCenter; text: section.toUpperCase(); color: Theme.text2; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1.2 }
            }
            delegate: Rectangle {
                width: list.width; height: 40
                color: ma.containsMouse ? Theme.bg5 : index % 2 ? Theme.bg2 : Theme.bg3
                RowLayout {
                    anchors.fill: parent; anchors.leftMargin: 10; anchors.rightMargin: 10; spacing: 10
                    CBadge { text: modelData.kind; tint: modelData.kind === "ndi" ? Theme.accent2 : modelData.kind === "camera" ? Theme.clsPreroll : modelData.kind === "sdi" ? Theme.standby : Theme.clsEffects }
                    ColumnLayout {
                        Layout.fillWidth: true; spacing: 0
                        Text { Layout.fillWidth: true; text: modelData.title; color: Theme.text0; font.pixelSize: Theme.fs12; font.bold: true; elide: Text.ElideRight }
                        Text { Layout.fillWidth: true; text: modelData.sub; color: Theme.text3; font.family: Theme.fontMono; font.pixelSize: Theme.fs10; elide: Text.ElideMiddle }
                    }
                }
                MouseArea { id: ma; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: { pk.choose(modelData) } }
            }
            Text { anchors.centerIn: parent; visible: pk.filtered.length === 0; width: parent.width - 40; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.WordWrap; color: Theme.text3; font.pixelSize: Theme.fs11
                text: "No live inputs found. Scan on the Inputs page, or add a stream URL or camera to the library." }
        }
        RowLayout {
            Layout.fillWidth: true
            Text { Layout.fillWidth: true; text: "Double-click an input tile or its bus key to take it to PGM. Keys 1–8 select, Alt+1–8 take, Ctrl+Enter cuts the preview."; color: Theme.text3; font.pixelSize: Theme.fs10; wrapMode: Text.WordWrap }
            CButton { text: "Cancel"; small: true; onClicked: { pk.close() } }
        }
    }
    function choose(entry) { if (im) im.map(slot, entry); close() }
}
