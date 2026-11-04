import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: multiviewPage
    
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

    property int gridLayout: 4 // 1, 2, 4, 6, 9

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        // Header
        RowLayout {
            Layout.fillWidth: true

            Text {
                text: "📺 Multiview Monitor"
                font.pixelSize: 24
                font.bold: true
                color: textPrimary
            }

            Item { Layout.fillWidth: true }

            // Layout Selector
            RowLayout {
                spacing: 4
                Repeater {
                    model: [
                        { layout: 1, icon: "◻" },
                        { layout: 2, icon: "▭" },
                        { layout: 4, icon: "⊞" },
                        { layout: 6, icon: "⊟" },
                        { layout: 9, icon: "⊡" }
                    ]

                    Rectangle {
                        width: 36
                        height: 36
                        radius: 6
                        color: gridLayout === modelData.layout ? accentColor : surfaceHighlight

                        Text {
                            anchors.centerIn: parent
                            text: modelData.icon
                            font.pixelSize: 16
                            color: gridLayout === modelData.layout ? "white" : textSecondary
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: gridLayout = modelData.layout
                        }
                    }
                }
            }
        }

        // Multiview Grid
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: "#000000"
            radius: 8

            GridLayout {
                anchors.fill: parent
                anchors.margins: 8
                columns: gridLayout <= 2 ? gridLayout : (gridLayout <= 4 ? 2 : 3)
                rows: gridLayout <= 1 ? 1 : (gridLayout <= 2 ? 1 : (gridLayout <= 4 ? 2 : (gridLayout <= 6 ? 2 : 3)))
                columnSpacing: 8
                rowSpacing: 8

                Repeater {
                    model: gridLayout

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        color: surfaceColor
                        radius: 6
                        border.color: index === 0 ? onAirColor : borderColor
                        border.width: index === 0 ? 2 : 1

                        ColumnLayout {
                            anchors.fill: parent
                            spacing: 0

                            // Preview Area
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                color: "#000000"
                                radius: 6

                                Rectangle {
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.bottom: parent.bottom
                                    height: 6
                                    radius: 6
                                    color: surfaceColor
                                }

                                Text {
                                    anchors.centerIn: parent
                                    text: index === 0 ? "▶ Program Output" : "Input " + (index + 1)
                                    font.pixelSize: 12
                                    color: textMuted
                                }

                                // On-Air Badge
                                Rectangle {
                                    visible: index === 0
                                    anchors.top: parent.top
                                    anchors.left: parent.left
                                    anchors.margins: 8
                                    width: 50
                                    height: 20
                                    radius: 3
                                    color: onAirColor

                                    Text {
                                        anchors.centerIn: parent
                                        text: "PGM"
                                        font.pixelSize: 9
                                        font.bold: true
                                        color: "white"
                                    }

                                    SequentialAnimation on opacity {
                                        running: true
                                        loops: Animation.Infinite
                                        NumberAnimation { to: 0.6; duration: 500 }
                                        NumberAnimation { to: 1.0; duration: 500 }
                                    }
                                }

                                // Preview Badge
                                Rectangle {
                                    visible: index === 1
                                    anchors.top: parent.top
                                    anchors.left: parent.left
                                    anchors.margins: 8
                                    width: 50
                                    height: 20
                                    radius: 3
                                    color: readyColor

                                    Text {
                                        anchors.centerIn: parent
                                        text: "PVW"
                                        font.pixelSize: 9
                                        font.bold: true
                                        color: "white"
                                    }
                                }

                                // Audio Meters
                                Row {
                                    anchors.right: parent.right
                                    anchors.top: parent.top
                                    anchors.margins: 8
                                    spacing: 2

                                    Repeater {
                                        model: 2
                                        Rectangle {
                                            width: 6
                                            height: 60
                                            color: surfaceHighlight
                                            radius: 2

                                            Rectangle {
                                                anchors.left: parent.left
                                                anchors.right: parent.right
                                                anchors.bottom: parent.bottom
                                                height: parent.height * (0.3 + Math.random() * 0.5)
                                                radius: 2
                                                gradient: Gradient {
                                                    GradientStop { position: 0.0; color: onAirColor }
                                                    GradientStop { position: 0.3; color: "#ffd60a" }
                                                    GradientStop { position: 1.0; color: readyColor }
                                                }
                                            }
                                        }
                                    }
                                }
                            }

                            // Label Bar
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 28
                                color: surfaceHighlight
                                radius: 6

                                Rectangle {
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.top: parent.top
                                    height: 6
                                    color: surfaceHighlight
                                }

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 8
                                    anchors.rightMargin: 8

                                    Text {
                                        text: index === 0 ? "PROGRAM" : 
                                              index === 1 ? "PREVIEW" :
                                              "INPUT " + (index + 1)
                                        font.pixelSize: 10
                                        font.bold: true
                                        color: index === 0 ? onAirColor : textPrimary
                                    }

                                    Item { Layout.fillWidth: true }

                                    Text {
                                        text: "1080p60"
                                        font.pixelSize: 9
                                        color: textMuted
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
