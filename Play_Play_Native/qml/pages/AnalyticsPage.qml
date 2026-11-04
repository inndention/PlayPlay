import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: analyticsPage
    
    property color bgColor: "#05050a"
    property color surfaceColor: "#0f0f18"
    property color surfaceHighlight: "#1a1a28"
    property color borderColor: "#2a2a40"
    property color textPrimary: "#f0f0f8"
    property color textSecondary: "#a0a0b8"
    property color textMuted: "#505068"
    property color accentColor: "#00d4ff"
    property color readyColor: "#30d158"

    ScrollView {
        anchors.fill: parent
        anchors.margins: 20
        
        ColumnLayout {
            width: parent.width
            spacing: 20

            // Header
            Text {
                text: "📊 Analytics Dashboard"
                font.pixelSize: 24
                font.bold: true
                color: textPrimary
            }

            // Stats Cards Row
            RowLayout {
                Layout.fillWidth: true
                spacing: 16

                StatCard { title: "Total Airtime"; value: "847h 23m"; change: "+12%"; isPositive: true }
                StatCard { title: "Media Items Played"; value: "12,847"; change: "+8%"; isPositive: true }
                StatCard { title: "Average Fill Rate"; value: "98.7%"; change: "+2.1%"; isPositive: true }
                StatCard { title: "System Uptime"; value: "99.99%"; change: "0%"; isPositive: true }
            }

            // Charts Row
            RowLayout {
                Layout.fillWidth: true
                Layout.preferredHeight: 300
                spacing: 16

                // Playback Chart
                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    color: surfaceColor
                    radius: 8
                    border.color: borderColor

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 16
                        spacing: 12

                        Text {
                            text: "Playback Activity (24h)"
                            font.pixelSize: 14
                            font.bold: true
                            color: textPrimary
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            color: surfaceHighlight
                            radius: 4

                            // Simple bar chart placeholder
                            Row {
                                anchors.fill: parent
                                anchors.margins: 16
                                spacing: 4

                                Repeater {
                                    model: 24
                                    Rectangle {
                                        width: (parent.width - 23 * 4) / 24
                                        height: parent.height * (0.3 + (index % 7) / 10)
                                        anchors.bottom: parent.bottom
                                        color: accentColor
                                        radius: 2
                                        opacity: 0.7 + (index / 24) * 0.3
                                    }
                                }
                            }
                        }
                    }
                }

                // Content Type Distribution
                Rectangle {
                    Layout.preferredWidth: 300
                    Layout.fillHeight: true
                    color: surfaceColor
                    radius: 8
                    border.color: borderColor

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 16
                        spacing: 12

                        Text {
                            text: "Content Distribution"
                            font.pixelSize: 14
                            font.bold: true
                            color: textPrimary
                        }

                        Repeater {
                            model: [
                                { name: "Programs", percent: 65, color: "#5856d6" },
                                { name: "Commercials", percent: 20, color: "#ff375f" },
                                { name: "Promos", percent: 10, color: "#ff9f0a" },
                                { name: "Fillers", percent: 5, color: "#64d2ff" }
                            ]

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8

                                Rectangle {
                                    width: 12
                                    height: 12
                                    radius: 2
                                    color: modelData.color
                                }

                                Text {
                                    Layout.fillWidth: true
                                    text: modelData.name
                                    font.pixelSize: 12
                                    color: textSecondary
                                }

                                Text {
                                    text: modelData.percent + "%"
                                    font.pixelSize: 12
                                    font.bold: true
                                    color: textPrimary
                                }
                            }
                        }

                        Item { Layout.fillHeight: true }
                    }
                }
            }

            // Recent Activity
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 300
                color: surfaceColor
                radius: 8
                border.color: borderColor

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 16
                    spacing: 12

                    RowLayout {
                        Layout.fillWidth: true

                        Text {
                            text: "Recent Playback Log"
                            font.pixelSize: 14
                            font.bold: true
                            color: textPrimary
                        }

                        Item { Layout.fillWidth: true }

                        Button {
                            text: "Export CSV"
                            flat: true
                            background: Rectangle {
                                color: parent.hovered ? surfaceHighlight : "transparent"
                                radius: 4
                            }
                            contentItem: Text {
                                text: "📥 Export CSV"
                                font.pixelSize: 11
                                color: accentColor
                            }
                        }
                    }

                    // Log Table
                    ListView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true

                        model: ListModel {
                            ListElement { time: "14:32:15"; title: "Evening News Intro"; duration: "00:00:15"; status: "Completed" }
                            ListElement { time: "14:32:00"; title: "Commercial Break"; duration: "00:02:30"; status: "Completed" }
                            ListElement { time: "14:29:30"; title: "Weather Segment"; duration: "00:02:30"; status: "Completed" }
                            ListElement { time: "14:27:00"; title: "News Update"; duration: "00:02:30"; status: "Completed" }
                            ListElement { time: "14:24:30"; title: "Interview: Local Business"; duration: "00:02:30"; status: "Completed" }
                        }

                        delegate: Rectangle {
                            width: parent.width
                            height: 36
                            color: index % 2 === 0 ? surfaceColor : surfaceHighlight

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 12
                                anchors.rightMargin: 12

                                Text { text: model.time; font.pixelSize: 11; font.family: "JetBrains Mono"; color: textMuted; Layout.preferredWidth: 80 }
                                Text { text: model.title; font.pixelSize: 11; color: textPrimary; Layout.fillWidth: true; elide: Text.ElideRight }
                                Text { text: model.duration; font.pixelSize: 11; font.family: "JetBrains Mono"; color: textSecondary; Layout.preferredWidth: 80 }
                                Rectangle {
                                    width: 70
                                    height: 20
                                    radius: 3
                                    color: Qt.rgba(48, 209, 88, 0.2)
                                    Text {
                                        anchors.centerIn: parent
                                        text: model.status
                                        font.pixelSize: 9
                                        font.bold: true
                                        color: readyColor
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    component StatCard: Rectangle {
        property string title: ""
        property string value: ""
        property string change: ""
        property bool isPositive: true

        Layout.fillWidth: true
        Layout.preferredHeight: 100
        color: surfaceColor
        radius: 8
        border.color: borderColor

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 16
            spacing: 8

            Text {
                text: title
                font.pixelSize: 11
                color: textMuted
            }

            Text {
                text: value
                font.pixelSize: 28
                font.bold: true
                color: textPrimary
            }

            Text {
                text: (isPositive ? "↑ " : "↓ ") + change
                font.pixelSize: 11
                color: isPositive ? readyColor : "#ff453a"
            }
        }
    }
}
