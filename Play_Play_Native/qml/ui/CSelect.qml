import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".."

/// Labelled combo box.
ColumnLayout {
    id: s
    property string label: ""
    property alias model: box.model
    property alias currentIndex: box.currentIndex
    property alias currentText: box.currentText
    property alias textRole: box.textRole
    property alias valueRole: box.valueRole
    property alias currentValue: box.currentValue
    signal activated(int index)
    spacing: 4
    Text { visible: s.label.length > 0; text: s.label.toUpperCase(); color: Theme.text2; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1 }
    ComboBox {
        id: box
        Layout.fillWidth: true
        implicitHeight: 30
        font.pixelSize: Theme.fs12
        onActivated: function(i) { s.activated(i) }
        background: Rectangle { color: Theme.bg4; radius: Theme.r5; border.color: box.activeFocus ? Theme.accent0 : Theme.border1 }
        contentItem: Text { leftPadding: 8; text: box.displayText; color: Theme.text0; font.pixelSize: Theme.fs12; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight }
        indicator: Text { x: box.width - width - 8; y: (box.height - height) / 2; text: "▾"; color: Theme.text2; font.pixelSize: Theme.fs12 }
        delegate: ItemDelegate {
            width: box.width
            contentItem: Text { text: String((box.textRole ? (Array.isArray(box.model) ? modelData[box.textRole] : model[box.textRole]) : modelData) || ""); color: Theme.text0; font.pixelSize: Theme.fs12 }
            background: Rectangle { color: highlighted ? Theme.bg5 : Theme.bg3 }
        }
        popup.background: Rectangle { color: Theme.bg3; border.color: Theme.border1; radius: Theme.r5 }
    }
}
