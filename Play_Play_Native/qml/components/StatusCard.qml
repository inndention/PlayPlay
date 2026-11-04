import QtQuick
import QtQuick.Layouts
import ".."

Rectangle {
    id: root
    
    property string title: "Status"
    property string value: ""
    property string detail: ""
    property color valueColor: Theme.textPrimary
    
    default property alias content: internalContentContainer.data
    
    color: Theme.surface
    radius: Theme.radius
    border.color: Theme.border
    border.width: 1
    
    implicitWidth: 300
    implicitHeight: 150
    
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 15
        spacing: 10
        
        Text {
            text: root.title
            color: Theme.textSecondary
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSizeNormal
            Layout.fillWidth: true
        }
        
        // Container for custom content
        ColumnLayout {
            id: internalContentContainer
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: children.length > 0
        }
        
        ColumnLayout {
            visible: internalContentContainer.children.length === 0
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 5
            
            Text {
                text: root.value
                color: root.valueColor
                font.family: Theme.fontFamily
                font.pixelSize: 32
                font.bold: true
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
            
            Text {
                text: root.detail
                color: Theme.textSecondary
                font.family: Theme.fontFamily
                font.pixelSize: 16
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
        }
    }
}
