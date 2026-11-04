import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import ".."
import "../components"

Page {
    id: root
    background: Rectangle { color: "transparent" }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 20

        // Toolbar
        RowLayout {
            Layout.fillWidth: true
            spacing: 15

            Label {
                text: "Media Library"
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeHeader
                font.bold: true
                color: Theme.textPrimary
            }

            Item { Layout.fillWidth: true } // Spacer

            FolderDialog {
                id: folderDialog
                title: "Select Media Folder"
                onAccepted: {
                    mediaLibraryModel.scanFolder(folderDialog.folder.toString())
                }
            }

            ModernTextField {
                placeholderText: "Search media..."
                Layout.preferredWidth: 300
                implicitHeight: 36
                onTextChanged: mediaLibraryModel.filter = text
            }

            ModernButton {
                text: "Scan Folder"
                customColor: Theme.accent
                onClicked: {
                    folderDialog.open()
                }
            }
        }

        // Filter Bar
        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            
            ModernButton { text: "All"; customColor: Theme.surfaceSelected; implicitWidth: 80 }
            ModernButton { text: "Video"; customColor: "transparent"; implicitWidth: 80 }
            ModernButton { text: "Audio"; customColor: "transparent"; implicitWidth: 80 }
            ModernButton { text: "Graphics"; customColor: "transparent"; implicitWidth: 80 }
        }

        // Media Grid
        GridView {
            id: mediaGrid
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            cellWidth: 240
            cellHeight: 220
            
            model: mediaLibraryModel

            delegate: Item {
                width: mediaGrid.cellWidth
                height: mediaGrid.cellHeight
                
                Rectangle {
                    anchors.fill: parent
                    anchors.margins: 8
                    color: Theme.surface
                    radius: Theme.radius
                    border.width: parent.hovered || parent.selected ? 1 : 0
                    border.color: Theme.accent
                    
                    property bool hovered: false
                    property bool selected: mediaGrid.currentIndex === index
                    
                    Behavior on border.width { NumberAnimation { duration: 100 } }

                    MouseArea {
                        anchors.fill: parent
                        hoverEnabled: true
                        onEntered: parent.hovered = true
                        onExited: parent.hovered = false
                        onClicked: {
                            mediaGrid.currentIndex = index
                        }
                    }

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 0

                        // Thumbnail Mockup
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 140
                            color: "black"
                            radius: Theme.radius
                            
                            // Top-left/right radius only? Qt Rectangle radius is all-or-nothing usually without complex clipping
                            // Just ensure inner content clips
                            clip: true
                            
                            Label {
                                anchors.centerIn: parent
                                text: model.type === "video" ? "VIDEO" : "AUDIO"
                                color: "#444444"
                                font.bold: true
                            }
                            
                            // Duration Badge
                            Rectangle {
                                anchors.right: parent.right
                                anchors.bottom: parent.bottom
                                anchors.margins: 5
                                color: Qt.rgba(0,0,0,0.8)
                                radius: 3
                                width: durationText.width + 10
                                height: durationText.height + 4
                                Label {
                                    id: durationText
                                    anchors.centerIn: parent
                                    text: model.duration
                                    color: "white"
                                    font.family: Theme.fontFamily
                                    font.pixelSize: 10
                                    font.bold: true
                                }
                            }
                        }

                        // Info
                        RowLayout {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Layout.margins: 10
                            spacing: 10
                            
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Label {
                                    Layout.fillWidth: true
                                    text: model.title
                                    color: Theme.textPrimary
                                    font.family: Theme.fontFamily
                                    font.pixelSize: 14
                                    font.bold: true
                                    elide: Text.ElideRight
                                }
                                Label {
                                    text: model.type
                                    color: Theme.textSecondary
                                    font.family: Theme.fontFamily
                                    font.pixelSize: 12
                                }
                            }

                            ModernButton {
                                text: "+"
                                implicitWidth: 30
                                implicitHeight: 30
                                customColor: Theme.surfaceHighlight
                                onClicked: {
                                    playlistModel.addItemFromMedia(model.mediaItem)
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
