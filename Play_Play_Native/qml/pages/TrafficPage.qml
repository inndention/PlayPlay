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
        spacing: 20

        RowLayout {
            Layout.fillWidth: true
            Label {
                text: "Traffic & Ad Management"
                color: Theme.textPrimary
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontSizeHeader
                font.bold: true
                Layout.fillWidth: true
            }
        }

        GridLayout {
            columns: 4
            Layout.fillWidth: true
            columnSpacing: 20

            StatusCard {
                title: "Active Campaigns"
                value: "12"
                detail: "Delivery on track"
            }
            StatusCard {
                title: "Weighted Slots"
                value: "85%"
                detail: "Optimization active"
            }
            StatusCard {
                title: "Today's Imps"
                value: "1,240"
                detail: "+12% from yesterday"
            }
            StatusCard {
                title: "Engine Priority"
                value: "BALANCED"
                detail: "Rule: Delivery Focus"
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.surface
            radius: Theme.radius
            border.color: Theme.border
            
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 20
                
                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        text: "Current Ad Inventory"
                        color: Theme.textPrimary
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontSizeLarge
                        font.bold: true
                        Layout.fillWidth: true
                    }
                    Button {
                        text: "Refresh Logic"
                        onClicked: console.log("Re-calculating traffic weights...")
                    }
                }

                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: 10 // Mock for now, will connect to TrafficEngine via Model
                    spacing: 10
                    delegate: Rectangle {
                        width: parent.width
                        height: 60
                        color: Theme.background
                        radius: 8
                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: 10
                            ColumnLayout {
                                Label { text: "Campaign " + (index + 1); color: Theme.textPrimary; font.bold: true }
                                Label { text: "Client: Acme Corp"; color: Theme.textSecondary; font.pixelSize: 11 }
                            }
                            Item { Layout.fillWidth: true }
                            ColumnLayout {
                                Layout.alignment: Qt.AlignRight
                                Label { text: "Weight: 100"; color: Theme.accent }
                                Label { text: "Probability: " + (10 - index) + "%"; color: Theme.textSecondary; font.pixelSize: 10 }
                            }
                            Rectangle {
                                width: 100
                                height: 8
                                radius: 4
                                color: Theme.surfaceHighlight
                                Rectangle {
                                    width: 80 - (index * 5)
                                    height: parent.height
                                    radius: 4
                                    color: Theme.success
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
