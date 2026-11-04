import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import ".."
import "../components"

Page {
    background: Rectangle { color: "transparent" }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 20

        Label {
            text: "Output Configuration"
            color: Theme.textPrimary
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontSizeHeader
            font.bold: true
        }
        
        // Output Settings Card
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 200
            color: Theme.surface
            radius: Theme.radius
            border.color: Theme.border
            border.width: 1
            
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 20
                spacing: 15
                
                Label { 
                    text: "Active Outputs"
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontSizeLarge
                }
                
                Switch {
                    text: "DeckLink SDI Output"
                    checked: true
                    palette.active.mid: Theme.surfaceSelected
                    palette.active.highlight: Theme.accent
                }
                
                Switch {
                    text: "NDI Output (PLAYPLAY-OUT)"
                    checked: true
                }
                
                Switch {
                    text: "SRT Stream (Backup)"
                    checked: false
                }
            }
        }
        
        Item { Layout.fillHeight: true } // Fill rest of page
    }
}
