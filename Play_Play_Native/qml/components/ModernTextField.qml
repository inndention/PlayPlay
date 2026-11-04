import QtQuick
import QtQuick.Controls
import ".."

TextField {
    id: control
    
    placeholderTextColor: Theme.textDisabled
    color: Theme.textPrimary
    font.family: Theme.fontFamily
    font.pixelSize: Theme.fontSizeNormal
    selectByMouse: true
    selectionColor: Theme.accent
    
    background: Rectangle {
        color: Theme.surfaceSelected
        radius: Theme.radius
        border.color: control.activeFocus ? Theme.accent : Theme.border
        border.width: 1
    }
}
