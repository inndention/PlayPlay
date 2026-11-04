import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: masterControlPage
    
    property color bgColor: "#05050a"
    property color surfaceColor: "#0f0f18"
    property color surfaceHighlight: "#1a1a28"
    property color borderColor: "#2a2a40"
    property color textPrimary: "#f0f0f8"
    property color textSecondary: "#a0a0b8"
    property color textMuted: "#505068"
    property color accentColor: "#00d4ff"
    property color onAirColor: "#ff2d55"
    property color readyColor: "#30d158"

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 16

        // Header
        RowLayout {
            Layout.fillWidth: true

            ColumnLayout {
                spacing: 4
                Text {
                    text: "🎛 Master Control"
                    font.pixelSize: 24
                    font.bold: true
                    color: textPrimary
                }
                Text {
                    text: "System Status: Online • 3 nodes connected"
                    font.pixelSize: 12
                    color: textSecondary
                }
            }

            Item { Layout.fillWidth: true }

            // Layout Mode Buttons
            RowLayout {
                spacing: 4
                Repeater {
                    model: ["⊞", "◻", "≡", "⧉"]
                    Rectangle {
                        width: 36
                        height: 36
                        radius: 6
                        color: index === 0 ? accentColor : surfaceHighlight
                        Text {
                            anchors.centerIn: parent
                            text: modelData
                            font.pixelSize: 16
                            color: index === 0 ? "white" : textSecondary
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                        }
                    }
                }
            }
        }

        // Main Grid
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 16

            // Primary Panel
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 16

                // Status Cards Grid
                GridLayout {
                    Layout.fillWidth: true
                    columns: 3
                    columnSpacing: 16
                    rowSpacing: 16

                    // Sync Health Card
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 180
                        color: surfaceColor
                        radius: 8
                        border.color: borderColor

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 16
                            spacing: 12

                            RowLayout {
                                Rectangle { width: 8; height: 8; radius: 4; color: accentColor }
                                Text { text: "Sync Health"; font.pixelSize: 14; font.bold: true; color: textPrimary }
                            }

                            RowLayout {
                                Text { text: "Download"; font.pixelSize: 11; color: textMuted; Layout.preferredWidth: 80 }
                                Text { text: "45.2 MB/s"; font.pixelSize: 11; font.bold: true; color: textPrimary }
                            }
                            RowLayout {
                                Text { text: "Upload"; font.pixelSize: 11; color: textMuted; Layout.preferredWidth: 80 }
                                Text { text: "12.8 MB/s"; font.pixelSize: 11; font.bold: true; color: textPrimary }
                            }

                            Item { Layout.fillHeight: true }

                            Text { text: "No active transfers"; font.pixelSize: 11; color: textMuted }
                        }
                    }

                    // Devices Card
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 180
                        color: surfaceColor
                        radius: 8
                        border.color: borderColor

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 16
                            spacing: 12

                            RowLayout {
                                Rectangle { width: 8; height: 8; radius: 4; color: "#a855f7" }
                                Text { text: "Discovered Devices"; font.pixelSize: 14; font.bold: true; color: textPrimary }
                            }

                            Repeater {
                                model: [
                                    { icon: "🎬", type: "OBS Studio", ip: "192.168.1.100:4455" },
                                    { icon: "📡", type: "NDI Source", ip: "192.168.1.101" },
                                    { icon: "🎛", type: "ATEM Mini", ip: "192.168.1.102" }
                                ]

                                RowLayout {
                                    spacing: 8
                                    Text { text: modelData.icon; font.pixelSize: 16 }
                                    ColumnLayout {
                                        spacing: 0
                                        Text { text: modelData.type; font.pixelSize: 11; font.bold: true; color: textPrimary }
                                        Text { text: modelData.ip; font.pixelSize: 10; color: textMuted }
                                    }
                                }
                            }
                        }
                    }

                    // Quick Actions Card
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 180
                        color: surfaceColor
                        radius: 8
                        border.color: borderColor

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 16
                            spacing: 12

                            RowLayout {
                                Rectangle { width: 8; height: 8; radius: 4; color: readyColor }
                                Text { text: "Quick Actions"; font.pixelSize: 14; font.bold: true; color: textPrimary }
                            }

                            Button {
                                Layout.fillWidth: true
                                text: "🔄 Sync All Nodes"
                                flat: true
                                background: Rectangle { color: surfaceHighlight; radius: 4 }
                                contentItem: Text { text: parent.text; color: textPrimary; font.pixelSize: 11 }
                            }

                            Button {
                                Layout.fillWidth: true
                                text: "📤 Push Schedule"
                                flat: true
                                background: Rectangle { color: surfaceHighlight; radius: 4 }
                                contentItem: Text { text: parent.text; color: textPrimary; font.pixelSize: 11 }
                            }

                            Button {
                                Layout.fillWidth: true
                                text: "⚡ Emergency Stop All"
                                flat: true
                                background: Rectangle { color: Qt.rgba(255, 45, 85, 0.2); radius: 4 }
                                contentItem: Text { text: parent.text; color: onAirColor; font.pixelSize: 11 }
                            }
                        }
                    }
                }

                // Remote Nodes
                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    color: surfaceColor
                    radius: 8
                    border.color: borderColor

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 16
                        spacing: 16

                        RowLayout {
                            Rectangle { width: 8; height: 8; radius: 4; color: "#ff9f0a" }
                            Text { text: "Remote Nodes"; font.pixelSize: 14; font.bold: true; color: textPrimary }
                        }

                        GridLayout {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            columns: 3
                            columnSpacing: 16
                            rowSpacing: 16

                            Repeater {
                                model: [
                                    { name: "PLAYOUT-01", status: "playing", playing: "Evening News" },
                                    { name: "PLAYOUT-02", status: "idle", playing: "" },
                                    { name: "ENCODER-01", status: "encoding", playing: "Stream Output" }
                                ]

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    color: surfaceHighlight
                                    radius: 8

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 12
                                        spacing: 8

                                        RowLayout {
                                            Rectangle {
                                                width: 8
                                                height: 8
                                                radius: 4
                                                color: modelData.status === "playing" ? onAirColor :
                                                       modelData.status === "encoding" ? readyColor : textMuted
                                            }
                                            Text {
                                                text: modelData.name
                                                font.pixelSize: 12
                                                font.bold: true
                                                color: textPrimary
                                            }
                                        }

                                        // Confidence Monitor
                                        Rectangle {
                                            Layout.fillWidth: true
                                            Layout.preferredHeight: 100
                                            color: "#000000"
                                            radius: 4

                                            Text {
                                                anchors.centerIn: parent
                                                text: modelData.status === "playing" ? "▶ " + modelData.playing : 
                                                      modelData.status === "encoding" ? "⚡ " + modelData.playing : "No Signal"
                                                font.pixelSize: 11
                                                color: textMuted
                                            }

                                            Rectangle {
                                                visible: modelData.status === "playing"
                                                anchors.top: parent.top
                                                anchors.right: parent.right
                                                anchors.margins: 6
                                                width: 40
                                                height: 16
                                                radius: 2
                                                color: onAirColor
                                                Text {
                                                    anchors.centerIn: parent
                                                    text: "LIVE"
                                                    font.pixelSize: 8
                                                    font.bold: true
                                                    color: "white"
                                                }
                                            }
                                        }

                                        Text {
                                            text: "Last seen: 2s ago"
                                            font.pixelSize: 10
                                            color: textMuted
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // Quick Bucket Sidebar
            Rectangle {
                Layout.preferredWidth: 200
                Layout.fillHeight: true
                color: surfaceColor
                radius: 8
                border.color: borderColor

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 16
                    spacing: 12

                    Text {
                        text: "🪣 Quick Bucket"
                        font.pixelSize: 14
                        font.bold: true
                        color: textPrimary
                    }

                    Text {
                        text: "Drop files or commands here for quick access"
                        font.pixelSize: 10
                        color: textMuted
                        wrapMode: Text.WordWrap
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        color: surfaceHighlight
                        radius: 6
                        border.color: borderColor
                        border.width: 2

                        Text {
                            anchors.centerIn: parent
                            text: "Drop items here"
                            font.pixelSize: 11
                            color: textMuted
                        }
                    }
                }
            }
        }
    }
}
