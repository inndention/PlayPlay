import QtQuick
import QtQuick.Controls
import ".."

Button {
    id: control
    
    property color customColor: Theme.accent
    property bool isIconOnly: false
    property string iconSource: ""
    
    // Default size if not specified
    implicitWidth: isIconOnly ? 40 : 120
    implicitHeight: 36
    
    contentItem: Row {
        spacing: 8
        anchors.centerIn: parent
        
        Image {
            visible: control.iconSource !== ""
            source: control.iconSource
            width: 16
            height: 16
            sourceSize.width: 16
            sourceSize.height: 16
            fillMode: Image.PreserveAspectFit
        }
        
        Text {
            visible: !control.isIconOnly
            text: control.text
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSizeNormal
            color: Theme.textPrimary
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
    }

    background: Rectangle {
        color: control.down ? Qt.darker(control.customColor, 1.2) : 
               control.hovered ? Qt.lighter(control.customColor, 1.2) : 
               control.customColor
        
        radius: Theme.radius
        opacity: control.enabled ? 1 : 0.5
        
        Behavior on color { ColorAnimation { duration: 100 } }
    }
}
