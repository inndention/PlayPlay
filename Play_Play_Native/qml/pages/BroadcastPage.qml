import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: broadcastPage
    
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
        Text {
            text: "📡 Broadcast Output Configuration"
            font.pixelSize: 24
            font.bold: true
            color: textPrimary
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 16

            // Output Destinations
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

                    Text {
                        text: "Output Destinations"
                        font.pixelSize: 16
                        font.bold: true
                        color: textPrimary
                    }

                    Repeater {
                        model: [
                            { name: "DeckLink SDI Output", type: "SDI", status: "Active", icon: "🔌" },
                            { name: "NDI Output", type: "NDI", status: "Active", icon: "📡" },
                            { name: "RTMP Stream", type: "Stream", status: "Ready", icon: "🌐" },
                            { name: "SRT Output", type: "Stream", status: "Ready", icon: "📺" },
                            { name: "HLS Output", type: "Stream", status: "Disabled", icon: "📱" }
                        ]

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 70
                            color: surfaceHighlight
                            radius: 6

                            RowLayout {
                                anchors.fill: parent
                                anchors.margins: 12
                                spacing: 12

                                Text {
                                    text: modelData.icon
                                    font.pixelSize: 24
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 2

                                    Text {
                                        text: modelData.name
                                        font.pixelSize: 13
                                        font.bold: true
                                        color: textPrimary
                                    }
                                    Text {
                                        text: modelData.type
                                        font.pixelSize: 11
                                        color: textMuted
                                    }
                                }

                                Rectangle {
                                    width: 70
                                    height: 24
                                    radius: 4
                                    color: modelData.status === "Active" ? Qt.rgba(48, 209, 88, 0.2) :
                                           modelData.status === "Ready" ? Qt.rgba(0, 212, 255, 0.2) :
                                           surfaceColor

                                    Text {
                                        anchors.centerIn: parent
                                        text: modelData.status
                                        font.pixelSize: 10
                                        font.bold: true
                                        color: modelData.status === "Active" ? readyColor :
                                               modelData.status === "Ready" ? accentColor : textMuted
                                    }
                                }

                                Switch {
                                    checked: modelData.status !== "Disabled"
                                }
                            }
                        }
                    }

                    Item { Layout.fillHeight: true }

                    Button {
                        Layout.fillWidth: true
                        text: "+ Add Output Destination"
                        flat: true
                        background: Rectangle {
                            color: surfaceHighlight
                            radius: 6
                            border.color: borderColor
                        }
                        contentItem: Text {
                            text: parent.text
                            font.pixelSize: 12
                            color: accentColor
                            horizontalAlignment: Text.AlignHCenter
                        }
                    }
                }
            }

            // Stream Settings
            Rectangle {
                Layout.preferredWidth: 350
                Layout.fillHeight: true
                color: surfaceColor
                radius: 8
                border.color: borderColor

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 16
                    spacing: 16

                    Text {
                        text: "Stream Settings"
                        font.pixelSize: 16
                        font.bold: true
                        color: textPrimary
                    }

                    // Video Settings
                    GroupBox {
                        Layout.fillWidth: true
                        title: "Video"
                        
                        background: Rectangle {
                            color: surfaceHighlight
                            radius: 6
                            border.color: borderColor
                        }

                        label: Text {
                            text: "Video"
                            font.pixelSize: 12
                            font.bold: true
                            color: textSecondary
                        }

                        ColumnLayout {
                            anchors.fill: parent
                            spacing: 8

                            RowLayout {
                                Text { text: "Resolution"; font.pixelSize: 11; color: textMuted; Layout.preferredWidth: 80 }
                                ComboBox {
                                    Layout.fillWidth: true
                                    model: ["1920x1080", "1280x720", "3840x2160"]
                                    currentIndex: 0
                                }
                            }

                            RowLayout {
                                Text { text: "Frame Rate"; font.pixelSize: 11; color: textMuted; Layout.preferredWidth: 80 }
                                ComboBox {
                                    Layout.fillWidth: true
                                    model: ["29.97 fps", "30 fps", "59.94 fps", "60 fps"]
                                    currentIndex: 0
                                }
                            }

                            RowLayout {
                                Text { text: "Bitrate"; font.pixelSize: 11; color: textMuted; Layout.preferredWidth: 80 }
                                TextField {
                                    Layout.fillWidth: true
                                    text: "8000"
                                    placeholderText: "kbps"
                                }
                            }
                        }
                    }

                    // Audio Settings
                    GroupBox {
                        Layout.fillWidth: true
                        title: "Audio"
                        
                        background: Rectangle {
                            color: surfaceHighlight
                            radius: 6
                            border.color: borderColor
                        }

                        label: Text {
                            text: "Audio"
                            font.pixelSize: 12
                            font.bold: true
                            color: textSecondary
                        }

                        ColumnLayout {
                            anchors.fill: parent
                            spacing: 8

                            RowLayout {
                                Text { text: "Sample Rate"; font.pixelSize: 11; color: textMuted; Layout.preferredWidth: 80 }
                                ComboBox {
                                    Layout.fillWidth: true
                                    model: ["48000 Hz", "44100 Hz"]
                                    currentIndex: 0
                                }
                            }

                            RowLayout {
                                Text { text: "Channels"; font.pixelSize: 11; color: textMuted; Layout.preferredWidth: 80 }
                                ComboBox {
                                    Layout.fillWidth: true
                                    model: ["Stereo", "5.1 Surround", "7.1 Surround"]
                                    currentIndex: 0
                                }
                            }

                            RowLayout {
                                Text { text: "Bitrate"; font.pixelSize: 11; color: textMuted; Layout.preferredWidth: 80 }
                                TextField {
                                    Layout.fillWidth: true
                                    text: "320"
                                    placeholderText: "kbps"
                                }
                            }
                        }
                    }

                    Item { Layout.fillHeight: true }

                    Button {
                        Layout.fillWidth: true
                        text: "Apply Settings"
                        flat: true
                        background: Rectangle {
                            color: accentColor
                            radius: 6
                        }
                        contentItem: Text {
                            text: parent.text
                            font.pixelSize: 12
                            font.bold: true
                            color: "white"
                            horizontalAlignment: Text.AlignHCenter
                        }
                    }
                }
            }
        }
    }
}
