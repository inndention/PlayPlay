import QtQuick
import QtQuick.Controls
import "../../../../Play_Play_Native/qml"

// Frozen copy of Play_Play_Native/qml/ui/CToggle.qml as it was before the UXB-5 shim (refs/baseline/pre-lanes-2026-10-26), kept
// for tst_shims and the button gallery's shim panel to measure the shim against. Only the Theme import differs. Do not edit.

/// Switch row: cyan pill like the Clean config pages.
Item {
    id: t
    property string label: ""
    property string hint: ""
    property bool checked: false
    property bool enabled2: true
    signal toggled(bool value)
    implicitHeight: hint.length > 0 ? 44 : 30
    implicitWidth: 300
    Rectangle {
        id: pill
        width: 36; height: 18; radius: 9
        anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter
        color: t.checked ? Theme.accent0 : Theme.bg5
        border.color: t.checked ? Theme.accent0 : Theme.border2
        opacity: t.enabled2 ? 1 : 0.4
        Rectangle { width: 14; height: 14; radius: 7; y: 2; x: t.checked ? 20 : 2; color: t.checked ? "#05050a" : Theme.text1; Behavior on x { NumberAnimation { duration: 120 } } }
    }
    Column {
        anchors.left: pill.right; anchors.leftMargin: 10; anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
        spacing: 1
        Text { text: t.label; color: Theme.text0; font.pixelSize: Theme.fs12; elide: Text.ElideRight; width: parent.width }
        Text { visible: t.hint.length > 0; text: t.hint; color: Theme.text2; font.pixelSize: Theme.fs10; elide: Text.ElideRight; width: parent.width }
    }
    MouseArea { anchors.fill: parent; enabled: t.enabled2; cursorShape: Qt.PointingHandCursor; onClicked: { t.checked = !t.checked; t.toggled(t.checked) } }
}
