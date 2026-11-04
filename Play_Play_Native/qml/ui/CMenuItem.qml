import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".."

/// A themed menu row for CMenu: label, optional shortcut hint on the right, optional check mark, danger tint.
MenuItem {
    id: mi
    property bool danger: false
    property string hint: ""
    property bool header: false
    implicitHeight: header ? 22 : 28
    implicitWidth: Math.max(220, row.implicitWidth + 28)
    enabled: !header && enabled2
    property bool enabled2: true
    leftPadding: 8; rightPadding: 8; topPadding: 0; bottomPadding: 0
    contentItem: RowLayout {
        id: row
        spacing: 8
        Text { text: mi.checkable ? (mi.checked ? "✓" : "") : ""; color: Theme.accent0; font.pixelSize: Theme.fs12; Layout.preferredWidth: 12; horizontalAlignment: Text.AlignHCenter; visible: mi.checkable }
        Text {
            text: mi.header ? mi.text.toUpperCase() : mi.text
            color: mi.header ? Theme.text2 : !mi.enabled ? Theme.text3 : mi.danger ? Theme.danger : Theme.text0
            font.pixelSize: mi.header ? Theme.fs10 : Theme.fs12; font.bold: mi.header; font.letterSpacing: mi.header ? 1 : 0
            elide: Text.ElideRight; Layout.fillWidth: true; verticalAlignment: Text.AlignVCenter
        }
        Text { visible: mi.hint.length > 0; text: mi.hint; color: Theme.text3; font.pixelSize: Theme.fs10 }
        Text { visible: mi.subMenu !== null && mi.subMenu !== undefined; text: "▸"; color: Theme.text2; font.pixelSize: Theme.fs11 }
    }
    background: Rectangle { radius: Theme.r3; color: mi.highlighted && mi.enabled ? Theme.bg5 : "transparent" }
}
