import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".."

/// Labelled text input (label above, Clean input styling).
ColumnLayout {
    id: f
    property string label: ""
    property alias text: input.text
    property alias placeholder: input.placeholderText
    property alias readOnly: input.readOnly
    property alias validator: input.validator
    property alias inputMethodHints: input.inputMethodHints
    property alias echoMode: input.echoMode
    property bool mono: false
    signal edited(string value)
    signal accepted()
    spacing: 4
    Text { visible: f.label.length > 0; text: f.label.toUpperCase(); color: Theme.text2; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1 }
    TextField {
        id: input
        Layout.fillWidth: true
        implicitHeight: 30
        color: Theme.text0
        placeholderTextColor: Theme.text3
        font.family: f.mono ? Theme.fontMono : Theme.fontSans
        font.pixelSize: Theme.fs12
        selectByMouse: true
        leftPadding: 8; rightPadding: 8
        background: Rectangle { color: input.readOnly ? Theme.bg3 : Theme.bg4; radius: Theme.r5; border.color: input.activeFocus ? Theme.accent0 : Theme.border1 }
        onTextEdited: { f.edited(text) }
        onAccepted: { f.accepted() }
        TextMenu {}
    }
}
