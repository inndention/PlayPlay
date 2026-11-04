import QtQuick
import QtQuick.Controls
import "../../../../Play_Play_Native/qml"

// Frozen copy of Play_Play_Native/qml/ui/DeckButton.qml as it was before the UXB-5 shim (refs/baseline/pre-lanes-2026-10-26), kept
// for tst_shims and the button gallery's shim panel to measure the shim against. Only the Theme import differs. Do not edit.

/// Big tactile transport key: glyph over label. variant: default|success|danger|warn|info|primary; `active` lights it.
AbstractButton {
    id: b
    property string glyph: ""
    property string variant: "default"
    property bool active: false
    readonly property color tint: variant === "success" ? Theme.ready : variant === "danger" ? Theme.onAir : variant === "warn" ? Theme.standby : variant === "info" ? "#0a84ff" : variant === "primary" ? Theme.accent0 : Theme.text1
    implicitWidth: 74; implicitHeight: 52
    hoverEnabled: true
    padding: 0
    background: Rectangle {
        radius: Theme.r5
        color: !b.enabled ? Theme.bg2 : b.active ? Qt.rgba(b.tint.r, b.tint.g, b.tint.b, 0.30) : b.pressed ? Theme.bg5 : b.hovered ? Theme.bg4 : Theme.bg3
        border.width: b.active ? 2 : 1
        border.color: !b.enabled ? Theme.border0 : b.active ? b.tint : b.hovered ? Theme.border2 : Theme.border1
    }
    contentItem: Column {
        spacing: 1
        Text { anchors.horizontalCenter: parent.horizontalCenter; text: b.glyph; color: !b.enabled ? Theme.text3 : b.active || b.variant !== "default" ? b.tint : Theme.text0; font.pixelSize: Theme.fs20 }
        Text { anchors.horizontalCenter: parent.horizontalCenter; text: b.text; color: !b.enabled ? Theme.text3 : Theme.text1; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 0.8 }
    }
}
