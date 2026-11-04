import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: settingsPage
    
    property color bgColor: "#05050a"
    property color surfaceColor: "#0f0f18"
    property color surfaceElevated: "#141420"
    property color surfaceHighlight: "#1a1a28"
    property color borderColor: "#2a2a40"
    property color textPrimary: "#f0f0f8"
    property color textSecondary: "#a0a0b8"
    property color textMuted: "#505068"
    property color accentColor: "#00d4ff"
    property color readyColor: "#30d158"
    property color warningColor: "#ffd60a"

    property string currentSection: "general"

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // Settings Sidebar
        Rectangle {
            Layout.preferredWidth: 240
            Layout.fillHeight: true
            color: surfaceColor

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 16
                spacing: 4

                Text {
                    text: "⚙ Settings"
                    font.pixelSize: 20
                    font.bold: true
                    color: textPrimary
                    Layout.bottomMargin: 12
                }

                Repeater {
                    model: [
                        { id: "general", icon: "🏠", name: "General" },
                        { id: "video", icon: "🎬", name: "Video Output" },
                        { id: "audio", icon: "🔊", name: "Audio" },
                        { id: "network", icon: "🌐", name: "Network" },
                        { id: "storage", icon: "💾", name: "Storage" },
                        { id: "integrations", icon: "🔌", name: "Integrations" },
                        { id: "automation", icon: "⚡", name: "Automation" },
                        { id: "appearance", icon: "🎨", name: "Appearance" },
                        { id: "hotkeys", icon: "⌨", name: "Hotkeys" },
                        { id: "advanced", icon: "🔧", name: "Advanced" }
                    ]

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 40
                        radius: 6
                        color: currentSection === modelData.id ? accentColor : 
                               (settingItemMouse.containsMouse ? surfaceHighlight : "transparent")

                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: 10
                            spacing: 10

                            Text {
                                text: modelData.icon
                                font.pixelSize: 16
                            }
                            Text {
                                text: modelData.name
                                font.pixelSize: 13
                                color: currentSection === modelData.id ? "white" : textPrimary
                            }
                        }

                        MouseArea {
                            id: settingItemMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: currentSection = modelData.id
                        }
                    }
                }

                Item { Layout.fillHeight: true }

                // Version Info
                Text {
                    text: "PLAY PLAY v1.0.0"
                    font.pixelSize: 10
                    color: textMuted
                }
            }
        }

        // Settings Content
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: bgColor

            ScrollView {
                anchors.fill: parent
                anchors.margins: 24

                ColumnLayout {
                    width: parent.width - 48
                    spacing: 24

                    // Dynamic content based on section
                    Loader {
                        Layout.fillWidth: true
                        sourceComponent: {
                            switch(currentSection) {
                                case "general": return generalSettings
                                case "video": return videoSettings
                                case "network": return networkSettings
                                case "integrations": return integrationsSettings
                                case "appearance": return appearanceSettings
                                default: return generalSettings
                            }
                        }
                    }
                }
            }
        }
    }

    // General Settings
    Component {
        id: generalSettings
        ColumnLayout {
            spacing: 20

            Text {
                text: "General Settings"
                font.pixelSize: 24
                font.bold: true
                color: textPrimary
            }

            SettingsGroup {
                title: "Application"

                ColumnLayout {
                    spacing: 12

                    SettingsRow {
                        label: "Station Name"
                        TextField {
                            Layout.fillWidth: true
                            text: "PLAY PLAY Station"
                            color: textPrimary
                            background: Rectangle { color: surfaceHighlight; radius: 4 }
                        }
                    }

                    SettingsRow {
                        label: "Station ID"
                        TextField {
                            Layout.fillWidth: true
                            text: "WPLAY"
                            color: textPrimary
                            background: Rectangle { color: surfaceHighlight; radius: 4 }
                        }
                    }

                    SettingsRow {
                        label: "Time Zone"
                        ComboBox {
                            Layout.fillWidth: true
                            model: ["UTC", "America/New_York", "America/Los_Angeles", "Europe/London"]
                            currentIndex: 1
                        }
                    }

                    SettingsRow {
                        label: "Auto-start on boot"
                        Switch { checked: true }
                    }
                }
            }

            SettingsGroup {
                title: "Playout"

                ColumnLayout {
                    spacing: 12

                    SettingsRow {
                        label: "Default frame rate"
                        ComboBox {
                            Layout.fillWidth: true
                            model: ["29.97 fps", "30 fps", "59.94 fps", "60 fps"]
                            currentIndex: 0
                        }
                    }

                    SettingsRow {
                        label: "Pre-roll duration (seconds)"
                        SpinBox {
                            from: 0
                            to: 30
                            value: 5
                        }
                    }

                    SettingsRow {
                        label: "Auto-advance playlist"
                        Switch { checked: true }
                    }

                    SettingsRow {
                        label: "Show countdown overlay"
                        Switch { checked: true }
                    }
                }
            }
        }
    }

    // Video Settings
    Component {
        id: videoSettings
        ColumnLayout {
            spacing: 20

            Text {
                text: "Video Output Settings"
                font.pixelSize: 24
                font.bold: true
                color: textPrimary
            }

            SettingsGroup {
                title: "Output Format"

                ColumnLayout {
                    spacing: 12

                    SettingsRow {
                        label: "Resolution"
                        ComboBox {
                            Layout.fillWidth: true
                            model: ["1920x1080 (Full HD)", "1280x720 (HD)", "3840x2160 (4K UHD)"]
                            currentIndex: 0
                        }
                    }

                    SettingsRow {
                        label: "Frame Rate"
                        ComboBox {
                            Layout.fillWidth: true
                            model: ["29.97 fps (NTSC)", "30 fps", "25 fps (PAL)", "59.94 fps", "60 fps"]
                            currentIndex: 0
                        }
                    }

                    SettingsRow {
                        label: "Color Space"
                        ComboBox {
                            Layout.fillWidth: true
                            model: ["Rec. 709 (HD)", "Rec. 2020 (HDR)", "sRGB"]
                            currentIndex: 0
                        }
                    }
                }
            }

            SettingsGroup {
                title: "Hardware Output"

                ColumnLayout {
                    spacing: 12

                    SettingsRow {
                        label: "DeckLink Device"
                        ComboBox {
                            Layout.fillWidth: true
                            model: ["DeckLink Mini Monitor 4K", "DeckLink SDI 4K", "None"]
                            currentIndex: 0
                        }
                    }

                    SettingsRow {
                        label: "NDI Output"
                        Switch { checked: true }
                    }

                    SettingsRow {
                        label: "NDI Source Name"
                        TextField {
                            Layout.fillWidth: true
                            text: "PLAYPLAY-OUTPUT"
                            color: textPrimary
                            background: Rectangle { color: surfaceHighlight; radius: 4 }
                        }
                    }
                }
            }
        }
    }

    // Network Settings
    Component {
        id: networkSettings
        ColumnLayout {
            spacing: 20

            Text {
                text: "Network Settings"
                font.pixelSize: 24
                font.bold: true
                color: textPrimary
            }

            SettingsGroup {
                title: "API Server"

                ColumnLayout {
                    spacing: 12

                    SettingsRow {
                        label: "Enable API Server"
                        Switch { checked: true }
                    }

                    SettingsRow {
                        label: "API Port"
                        SpinBox { from: 1024; to: 65535; value: 3000 }
                    }

                    SettingsRow {
                        label: "Enable Remote Control"
                        Switch { checked: false }
                    }
                }
            }

            SettingsGroup {
                title: "Streaming"

                ColumnLayout {
                    spacing: 12

                    SettingsRow {
                        label: "RTMP Server URL"
                        TextField {
                            Layout.fillWidth: true
                            text: "rtmp://localhost/live"
                            color: textPrimary
                            background: Rectangle { color: surfaceHighlight; radius: 4 }
                        }
                    }

                    SettingsRow {
                        label: "Stream Key"
                        TextField {
                            Layout.fillWidth: true
                            echoMode: TextInput.Password
                            text: "your-stream-key"
                            color: textPrimary
                            background: Rectangle { color: surfaceHighlight; radius: 4 }
                        }
                    }
                }
            }
        }
    }

    // Integrations Settings
    Component {
        id: integrationsSettings
        ColumnLayout {
            spacing: 20

            Text {
                text: "Integrations"
                font.pixelSize: 24
                font.bold: true
                color: textPrimary
            }

            SettingsGroup {
                title: "OBS Studio"

                ColumnLayout {
                    spacing: 12

                    SettingsRow {
                        label: "Enable OBS Integration"
                        Switch { checked: true }
                    }

                    SettingsRow {
                        label: "WebSocket Host"
                        TextField {
                            Layout.fillWidth: true
                            text: "localhost"
                            color: textPrimary
                            background: Rectangle { color: surfaceHighlight; radius: 4 }
                        }
                    }

                    SettingsRow {
                        label: "WebSocket Port"
                        SpinBox { from: 1024; to: 65535; value: 4455 }
                    }

                    SettingsRow {
                        label: "Password"
                        TextField {
                            Layout.fillWidth: true
                            echoMode: TextInput.Password
                            placeholderText: "Leave empty if not set"
                            color: textPrimary
                            placeholderTextColor: textMuted
                            background: Rectangle { color: surfaceHighlight; radius: 4 }
                        }
                    }
                }
            }

        }
    }

    // Appearance Settings
    Component {
        id: appearanceSettings
        ColumnLayout {
            spacing: 20

            Text {
                text: "Appearance"
                font.pixelSize: 24
                font.bold: true
                color: textPrimary
            }

            SettingsGroup {
                title: "Theme"

                ColumnLayout {
                    spacing: 12

                    SettingsRow {
                        label: "Color Theme"
                        ComboBox {
                            Layout.fillWidth: true
                            model: ["Dark (Default)", "Light", "Broadcast Blue", "Studio Red"]
                            currentIndex: 0
                        }
                    }

                    SettingsRow {
                        label: "Accent Color"
                        RowLayout {
                            spacing: 8
                            Repeater {
                                model: ["#00d4ff", "#ff2d55", "#30d158", "#ff9500", "#5856d6"]
                                Rectangle {
                                    width: 28
                                    height: 28
                                    radius: 14
                                    color: modelData
                                    border.color: index === 0 ? "white" : "transparent"
                                    border.width: 2

                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                    }
                                }
                            }
                        }
                    }

                    SettingsRow {
                        label: "UI Scale"
                        Slider {
                            Layout.fillWidth: true
                            from: 80
                            to: 120
                            value: 100
                            stepSize: 10
                        }
                    }
                }
            }

            SettingsGroup {
                title: "Clock Display"

                ColumnLayout {
                    spacing: 12

                    SettingsRow {
                        label: "Time Format"
                        ComboBox {
                            Layout.fillWidth: true
                            model: ["24-hour", "12-hour (AM/PM)"]
                            currentIndex: 0
                        }
                    }

                    SettingsRow {
                        label: "Show Milliseconds"
                        Switch { checked: true }
                    }

                    SettingsRow {
                        label: "Show Date"
                        Switch { checked: true }
                    }
                }
            }
        }
    }

    // Settings Group Component
    component SettingsGroup: Rectangle {
        property string title: ""
        default property alias content: contentLayout.data

        Layout.fillWidth: true
        implicitHeight: contentColumn.height + 32
        color: surfaceColor
        radius: 8
        border.color: borderColor

        ColumnLayout {
            id: contentColumn
            anchors.fill: parent
            anchors.margins: 16
            spacing: 16

            Text {
                text: title
                font.pixelSize: 14
                font.bold: true
                color: textSecondary
            }

            ColumnLayout {
                id: contentLayout
                Layout.fillWidth: true
                spacing: 12
            }
        }
    }

    // Settings Row Component
    component SettingsRow: RowLayout {
        property string label: ""
        default property alias content: contentItem.data

        Layout.fillWidth: true
        spacing: 16

        Text {
            text: label
            font.pixelSize: 13
            color: textPrimary
            Layout.preferredWidth: 180
        }

        Item {
            id: contentItem
            Layout.fillWidth: true
            Layout.preferredHeight: childrenRect.height
        }
    }
}
