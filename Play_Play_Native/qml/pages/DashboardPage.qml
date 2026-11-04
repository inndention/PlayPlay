import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".."
import "../components"

Page {
    background: Rectangle { color: "transparent" }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 20

        RowLayout {
            Layout.fillWidth: true
            Label {
                text: "Broadcast Dashboard"
                color: Theme.textPrimary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeHeader
                font.bold: true
                Layout.fillWidth: true
            }
            
            RowLayout {
                spacing: 10
                
                ModernButton {
                    text: "Play"
                    customColor: Theme.success
                    implicitWidth: 80
                    onClicked: controlService.play()
                }
                ModernButton {
                    text: "Pause"
                    customColor: Theme.warning
                    implicitWidth: 80
                    onClicked: controlService.pause()
                }
                ModernButton {
                    text: "Stop"
                    customColor: Theme.error
                    implicitWidth: 80
                    onClicked: controlService.stopPlayback()
                }
                ModernButton {
                    text: "Next"
                    customColor: Theme.accent
                    implicitWidth: 80
                    onClicked: controlService.skipNext()
                }
            }
        }

        GridLayout {
            columns: 3
            Layout.fillWidth: true
            columnSpacing: 20
            rowSpacing: 20

            // Status Card 1: On Air
            StatusCard {
                title: "Engine Status"
                ColumnLayout {
                    Layout.fillWidth: true
                    Label { 
                        text: systemMonitor.engineState
                        color: systemMonitor.engineState === "PLAYING" ? Theme.live : Theme.textSecondary
                        font.family: Theme.fontFamily
                        font.pixelSize: 32
                        font.bold: true
                        Layout.alignment: Qt.AlignLeft
                        
                        SequentialAnimation on opacity {
                            running: systemMonitor.engineState === "PLAYING"
                            loops: Animation.Infinite
                            PropertyAnimation { from: 1; to: 0.5; duration: 800 }
                            PropertyAnimation { from: 0.5; to: 1; duration: 800 }
                        }
                    }
                    Label { 
                        text: "00:42:15" // TODO: Connect to Playout Timer
                        color: Theme.textPrimary
                        font.family: Theme.fontFamily
                        font.pixelSize: 24 
                    }
                }
            }

            // Status Card 2: Next Item
            StatusCard {
                title: "Up Next"
                value: "News Intro"
                detail: "Starts in 00:05:00"
                valueColor: Theme.accent
            }
            
            // Status Card 3: System Health
             StatusCard {
                title: "System Health"
                ColumnLayout {
                    spacing: 10
                    Layout.fillWidth: true
                    
                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: "CPU"; color: Theme.textSecondary; font.family: Theme.fontFamily; width: 40 }
                        ProgressBar { 
                            from: 0; to: 100; value: systemMonitor.cpuUsage; Layout.fillWidth: true 
                            background: Rectangle { 
                                implicitHeight: 6
                                color: Theme.surfaceSelected
                                radius: 3
                            }
                            contentItem: Item {
                                implicitHeight: 6
                                Rectangle {
                                    width: parent.parent.visualPosition * parent.width
                                    height: parent.height
                                    radius: 3
                                    color: systemMonitor.cpuUsage > 80 ? Theme.error : Theme.success
                                }
                            }
                        }
                        Label { text: Math.round(systemMonitor.cpuUsage) + "%"; color: Theme.textPrimary; font.family: Theme.fontFamily; width: 30; horizontalAlignment: Text.AlignRight }
                    }
                    
                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: "RAM"; color: Theme.textSecondary; font.family: Theme.fontFamily; width: 40 }
                        ProgressBar { 
                            from: 0; to: 100; value: systemMonitor.ramUsage; Layout.fillWidth: true 
                            background: Rectangle { 
                                implicitHeight: 6
                                color: Theme.surfaceSelected
                                radius: 3
                            }
                            contentItem: Item {
                                implicitHeight: 6
                                Rectangle {
                                    width: parent.parent.visualPosition * parent.width
                                    height: parent.height
                                    radius: 3
                                    color: systemMonitor.ramUsage > 90 ? Theme.error : Theme.warning
                                }
                            }
                        }
                        Label { text: Math.round(systemMonitor.ramUsage) + "%"; color: Theme.textPrimary; font.family: Theme.fontFamily; width: 30; horizontalAlignment: Text.AlignRight }
                    }
                }
            }
        }
        
        // Activity Log & AI Assistant
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 20

            // Activity Log
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredWidth: 2
                color: Theme.surface
                radius: Theme.radius
                border.color: Theme.border
                border.width: 1
                
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 15
                    spacing: 10
                    
                    Label { 
                        text: "Activity Log"
                        color: Theme.textPrimary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeLarge
                        font.bold: true
                    }
                    
                    ListView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: activityLogModel
                        spacing: 8
                        delegate: RowLayout {
                            width: ListView.view.width
                            spacing: 15
                            Label { text: model.time; color: Theme.textDisabled; font.pixelSize: 10; width: 60 }
                            Label { text: model.event; color: Theme.textSecondary; Layout.fillWidth: true; elide: Text.ElideRight }
                            Rectangle { width: 70; height: 18; radius: 3; color: model.status === "ON_TIME" ? Theme.success : Theme.warning; opacity: 0.2
                                Label { anchors.centerIn: parent; text: model.status; color: parent.color; font.pixelSize: 10; font.bold: true }
                            }
                        }
                    }
                }
            }

            // AI Copilot
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredWidth: 1
                color: Theme.surface
                radius: Theme.radius
                border.color: Theme.accent
                border.width: 2
                
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 15
                    spacing: 10
                    
                    Label { 
                        text: "AI Playout Assistant"
                        color: Theme.accent
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeLarge
                        font.bold: true
                    }
                    
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        color: Theme.backgroundTertiary
                        radius: 8
                        ScrollView {
                            anchors.fill: parent
                            clip: true
                            Label {
                                width: parent.width - 20
                                anchors.centerIn: parent
                                text: copilotService.lastResponse || "Waiting for signal..."
                                color: Theme.textPrimary
                                wrapMode: Text.Wrap
                                font.italic: true
                                horizontalAlignment: Text.AlignHCenter
                            }
                        }
                    }
                    
                    RowLayout {
                        TextField {
                            id: copilotInput
                            placeholderText: "Chat with Control..."
                            Layout.fillWidth: true
                            onAccepted: { copilotService.ask(text); text = "" }
                        }
                        Button {
                            text: "Ask"
                            onClicked: { copilotService.ask(copilotInput.text); copilotInput.text = "" }
                        }
                    }
                }
            }
        }
    }
}
