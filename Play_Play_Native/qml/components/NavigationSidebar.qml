import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".."

Rectangle {
    id: root
    implicitWidth: 260
    color: Theme.surface

    property string currentPage: "dashboardPage"
    property bool compact: false
    signal pageRequested(string pageId)

    Rectangle {
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: 1
        color: Theme.border
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 80
            color: "transparent"

            RowLayout {
                anchors.centerIn: parent
                spacing: 15

                Rectangle {
                    width: 40
                    height: 40
                    radius: 8
                    color: Theme.accent
                    Text {
                        anchors.centerIn: parent
                        text: "P"
                        color: "white"
                        font.bold: true
                        font.pixelSize: 24
                    }
                }

                Label {
                    text: "PLAY PLAY"
                    color: Theme.textPrimary
                    font.family: Theme.fontFamily
                    font.bold: true
                    font.pixelSize: 20
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            height: 1
            color: Theme.border
        }

        ListView {
            id: navList
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.topMargin: 20
            Layout.bottomMargin: 8
            clip: true
            interactive: true
            model: ListModel {
                    ListElement { name: "Dashboard"; icon: "📊"; page: "dashboardPage" }
                    ListElement { name: "Playout"; icon: "▶"; page: "playoutPage" }
                    ListElement { name: "Media Library"; icon: "📚"; page: "mediaLibraryPage" }
                    ListElement { name: "Playlist"; icon: "📋"; page: "playlistPage" }
                    ListElement { name: "Scheduler"; icon: "📅"; page: "schedulerPage" }
                    ListElement { name: "CG Control"; icon: "🖼"; page: "cgTemplatesPage" }
                    ListElement { name: "Analytics"; icon: "📈"; page: "analyticsPage" }
                    ListElement { name: "Master Control"; icon: "🖥"; page: "masterControlPage" }
                    ListElement { name: "Broadcast"; icon: "📡"; page: "broadcastPage" }
                    ListElement { name: "Multiview"; icon: "📺"; page: "multiviewPage" }
                    ListElement { name: "Traffic"; icon: "📑"; page: "trafficPage" }
                    ListElement { name: "Output"; icon: "🖵"; page: "outputPage" }
                    ListElement { name: "Settings"; icon: "⚙"; page: "settingsPage" }
                }

                delegate: ItemDelegate {
                    id: navDelegate
                    width: navList.width
                    height: 50

                    property bool isSelected: root.currentPage === model.page

                    background: Rectangle {
                        color: isSelected ? Theme.surfaceSelected : (navDelegate.hovered ? Theme.surfaceHighlight : "transparent")

                        Rectangle {
                            visible: isSelected
                            width: 4
                            height: 30
                            anchors.left: parent.left
                            anchors.verticalCenter: parent.verticalCenter
                            color: Theme.accent
                            radius: 2
                        }
                    }

                    contentItem: RowLayout {
                        spacing: 15
                        anchors.left: parent.left
                        anchors.leftMargin: 25
                        anchors.verticalCenter: parent.verticalCenter

                        Text {
                            text: model.icon
                            color: isSelected ? Theme.accent : Theme.textSecondary
                            font.pixelSize: 16
                        }

                        Label {
                            text: model.name
                            color: isSelected ? Theme.textPrimary : Theme.textSecondary
                            font.family: Theme.fontFamily
                            font.pixelSize: 15
                            font.weight: isSelected ? Font.DemiBold : Font.Normal
                        }
                    }

                    onClicked: {
                        root.currentPage = model.page
                        root.pageRequested(model.page)
                    }
                }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 60
            color: "transparent"

            Rectangle {
                anchors.top: parent.top
                width: parent.width
                height: 1
                color: Theme.border
            }

            RowLayout {
                anchors.centerIn: parent
                spacing: 10

                Rectangle {
                    width: 10
                    height: 10
                    radius: 5
                    color: Theme.success
                }
                Label {
                    text: "System Online"
                    color: Theme.textSecondary
                    font.pixelSize: 12
                }
            }
        }
    }
}

