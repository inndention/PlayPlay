import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".."
import "../components"

Page {
    id: root
    background: Rectangle { color: "transparent" }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 15

        // Header
        RowLayout {
            Layout.fillWidth: true
            Label {
                text: "Playlist Editor"
                color: Theme.textPrimary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeHeader
                font.bold: true
            }
            
            Item { Layout.fillWidth: true }
            
            ModernButton {
                text: "Load Slot"
                onClicked: playlistModel.loadSlot(0, 0)
            }
            ModernButton {
                text: "Save Slot"
                customColor: Theme.success
                onClicked: playlistModel.saveSlot()
            }
        }

        // Playlist Table Header
        Rectangle {
            Layout.fillWidth: true
            height: 30
            color: Theme.surfaceHighlight
            radius: Theme.radius
            
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                spacing: 10
                
                Label { text: "TIME"; width: 80; color: Theme.textSecondary; font.family: Theme.fontFamily; font.bold: true }
                Label { text: "TITLE"; Layout.fillWidth: true; color: Theme.textSecondary; font.family: Theme.fontFamily; font.bold: true }
                Label { text: "DURATION"; width: 80; color: Theme.textSecondary; font.family: Theme.fontFamily; font.bold: true }
                Label { text: "TYPE"; width: 60; color: Theme.textSecondary; font.family: Theme.fontFamily; font.bold: true }
                Item { width: 80 } // Controls spacer
            }
        }

        // Playlist View
        ListView {
            id: playlistView
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: playlistModel
            spacing: 2
            
            delegate: Rectangle {
                width: ListView.view.width
                height: 40
                color: index % 2 === 0 ? Theme.surface : Theme.background
                radius: Theme.radius
                
                // Hover effect
                Rectangle {
                    anchors.fill: parent
                    color: Theme.accent
                    opacity: mouseArea.containsMouse ? 0.1 : 0
                }

                MouseArea {
                    id: mouseArea
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: playlistView.currentIndex = index
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10
                    spacing: 10
                    
                    // Time (Placeholder or binding if model supports it)
                    Label { 
                        text: "12:00:00" 
                        color: Theme.textPrimary
                        font.family: Theme.fontFamily
                        width: 80
                    }
                    
                    Label {
                        text: model.title
                        color: Theme.textPrimary
                        font.family: Theme.fontFamily
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                    }
                    
                    Label {
                        text: model.duration
                        color: Theme.textSecondary
                        font.family: Theme.fontFamily
                        width: 80
                    }
                    
                    Rectangle {
                        width: 60
                        height: 20
                        radius: 3
                        color: model.type === "video" ? Theme.accent : Theme.warning
                        opacity: 0.2
                        border.color: model.type === "video" ? Theme.accent : Theme.warning
                        border.width: 1
                        
                        Label {
                            anchors.centerIn: parent
                            text: model.type ? model.type.toUpperCase() : "UNK"
                            color: model.type === "video" ? Theme.accent : Theme.warning
                            font.pixelSize: 10
                            font.bold: true
                        }
                    }
                    
                    Row {
                        width: 110 
                        spacing: 5
                        ModernButton { 
                            isIconOnly: true
                            text: "▲"
                            implicitWidth: 30
                            implicitHeight: 30
                            onClicked: playlistModel.moveItem(index, index - 1)
                            enabled: index > 0
                        }
                        ModernButton { 
                            isIconOnly: true
                            text: "▼"
                            implicitWidth: 30
                            implicitHeight: 30
                            onClicked: playlistModel.moveItem(index, index + 1)
                            enabled: index < playlistModel.rowCount() - 1
                        }
                        ModernButton { 
                            isIconOnly: true
                            text: "✕"
                            implicitWidth: 30
                            implicitHeight: 30
                            customColor: Theme.error
                            onClicked: playlistModel.removeItem(index)
                        }
                    }
                }
            }
        }
    }
}
