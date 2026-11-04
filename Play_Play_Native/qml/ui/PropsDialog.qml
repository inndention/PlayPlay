import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".."

/// Properties dialog: flattened key/value table + raw JSON, Copy JSON, optional extra actions. Opened by CMenu.showProps().
Popup {
    id: d
    parent: Overlay.overlay
    anchors.centerIn: parent
    modal: true; focus: true; closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    width: Math.min(parent ? parent.width - 80 : 760, 780); height: Math.min(parent ? parent.height - 80 : 560, 620)
    padding: 0
    property var menuHelper: null
    property string heading: "Properties"
    property var value: ({})
    property var actions: []
    property var rows: []
    property string json: ""
    background: Rectangle { color: Theme.bg2; radius: Theme.r8; border.color: Theme.border1 }

    function flatten(v, prefix, out) {
        if (v === null || v === undefined) { out.push([prefix || "(value)", String(v)]); return out }
        if (Array.isArray(v)) {
            if (!v.length) { out.push([prefix, "[]"]); return out }
            var prim = true; for (var i = 0; i < v.length; ++i) if (v[i] !== null && typeof v[i] === "object") prim = false
            if (prim) { out.push([prefix, v.join(", ")]); return out }
            for (var j = 0; j < v.length; ++j) flatten(v[j], prefix + "[" + j + "]", out)
            return out
        }
        if (typeof v === "object") {
            var ks = Object.keys(v); if (!ks.length) out.push([prefix, "{}"])
            for (var k = 0; k < ks.length; ++k) flatten(v[ks[k]], prefix ? prefix + "." + ks[k] : ks[k], out)
            return out
        }
        out.push([prefix || "(value)", String(v)]); return out
    }
    function show(title, obj, acts) {
        heading = title || "Properties"; value = obj; actions = acts || []
        json = JSON.stringify(obj, null, 2)
        var r = flatten(obj, "", []); rows = r.length > 400 ? r.slice(0, 400) : r
        open()
    }

    ColumnLayout {
        anchors.fill: parent; spacing: 0
        Rectangle {
            Layout.fillWidth: true; implicitHeight: 44; color: Theme.bg3; radius: Theme.r8
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: Theme.r8; color: Theme.bg3 }
            RowLayout {
                anchors.fill: parent; anchors.leftMargin: 14; anchors.rightMargin: 10
                Text { text: d.heading; color: Theme.text0; font.pixelSize: Theme.fs14; font.bold: true; Layout.fillWidth: true; elide: Text.ElideRight }
                CButton { icon2: "✕"; small: true; variant: "ghost"; onClicked: { d.close() } }
            }
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.border1 }
        }
        ListView {
            id: list
            Layout.fillWidth: true; Layout.fillHeight: true; Layout.minimumHeight: 120
            clip: true; model: d.rows
            ScrollBar.vertical: ScrollBar {}
            delegate: Rectangle {
                width: list.width; height: Math.max(26, valueText.implicitHeight + 8)
                color: index % 2 ? Theme.bg2 : Theme.bg3
                RowLayout {
                    anchors.fill: parent; anchors.leftMargin: 14; anchors.rightMargin: 14; spacing: 12
                    Text { text: modelData[0]; color: Theme.text2; font.pixelSize: Theme.fs11; Layout.preferredWidth: 220; elide: Text.ElideMiddle; verticalAlignment: Text.AlignVCenter; Layout.alignment: Qt.AlignTop; topPadding: 4 }
                    Text { id: valueText; text: modelData[1]; color: Theme.text0; font.family: Theme.fontMono; font.pixelSize: Theme.fs11; wrapMode: Text.WrapAnywhere; Layout.fillWidth: true; topPadding: 4 }
                }
                MouseArea { anchors.fill: parent; acceptedButtons: Qt.RightButton; onClicked: { if (d.menuHelper) d.menuHelper.copyText(modelData[1], modelData[0]) } }
            }
            Text { anchors.centerIn: parent; visible: d.rows.length === 0; text: "(empty)"; color: Theme.text3; font.pixelSize: Theme.fs12 }
        }
        Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.border1 }
        RowLayout {
            Layout.fillWidth: true; Layout.margins: 10; spacing: 8
            Text { text: d.rows.length + " propert" + (d.rows.length === 1 ? "y" : "ies") + "  ·  right-click a value to copy it"; color: Theme.text3; font.pixelSize: Theme.fs10; Layout.fillWidth: true }
            Repeater { model: d.actions; CButton { text: modelData.text; small: true; onClicked: { d.close(); if (modelData.fn) modelData.fn() } } }
            CButton { text: "Copy JSON"; small: true; onClicked: { if (d.menuHelper) d.menuHelper.copyText(d.json, "properties JSON") } }
            CButton { text: "Close"; small: true; variant: "primary"; onClicked: { d.close() } }
        }
    }
}
