import QtQuick
import QtQuick.Controls
import "../../../../Play_Play_Native/qml"

// Frozen copy of Play_Play_Native/qml/ui/CButton.qml as it was before the UXB-5 shim (refs/baseline/pre-lanes-2026-10-26), kept
// for tst_shims and the button gallery's shim panel to measure the shim against. Only the Theme import differs. Do not edit.

/// Pill button in the PlayPlayClean style. variant: default|primary|danger|success|warn|ghost|accent2
AbstractButton {
    id: b
    property string variant: "default"
    property string icon2: ""
    property bool small: false
    property bool active: false
    property color tint: variant === "primary" ? Theme.accent0
                       : variant === "danger" ? Theme.onAir
                       : variant === "success" ? Theme.ready
                       : variant === "warn" ? Theme.standby
                       : variant === "accent2" ? Theme.accent2
                       : variant === "info" ? "#0a84ff"
                       : Theme.text1
    property bool filled: variant === "primary" || variant === "danger" || variant === "success" || variant === "warn" || variant === "info" || variant === "accent2"
    implicitHeight: small ? 24 : 30
    implicitWidth: row.implicitWidth + (small ? 16 : 22)
    hoverEnabled: true
    padding: 0
    background: Rectangle {
        radius: Theme.r5
        color: !b.enabled ? Theme.bg3
             : b.filled ? (b.pressed ? Qt.darker(b.tint, 1.25) : b.hovered ? Qt.lighter(b.tint, 1.12) : b.tint)
             : b.active ? Qt.rgba(b.tint.r, b.tint.g, b.tint.b, 0.18)
             : (b.pressed ? Theme.bg5 : b.hovered ? Theme.bg4 : variant === "ghost" ? "transparent" : Theme.bg3)
        border.color: b.filled ? "transparent" : b.active ? b.tint : Theme.border1
        border.width: variant === "ghost" && !b.active ? 0 : 1
    }
    contentItem: Row {
        id: row
        spacing: 6
        anchors.centerIn: parent
        Text {
            visible: b.icon2.length > 0
            text: b.icon2
            color: b.filled ? "#05050a" : b.active ? b.tint : Theme.text1
            font.pixelSize: b.small ? Theme.fs11 : Theme.fs13
            anchors.verticalCenter: parent.verticalCenter
        }
        Text {
            visible: b.text.length > 0
            text: b.text
            color: !b.enabled ? Theme.text3 : b.filled ? "#05050a" : b.active ? b.tint : Theme.text0
            font.family: Theme.fontDisplay
            font.pixelSize: b.small ? Theme.fs11 : Theme.fs12
            font.bold: true
            font.letterSpacing: 0.4
            anchors.verticalCenter: parent.verticalCenter
        }
    }
    ToolTip.visible: hovered && ToolTip.text.length > 0
    ToolTip.delay: 600
}
