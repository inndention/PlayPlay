import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qt.labs.qmlmodels
import PlayPlay.Models 1.0 // For enums if needed

Page {
    id: root
    title: "Scheduler"
    background: Rectangle { color: "transparent" }

    property var selectedDay: -1
    property var clipboardDay: -1

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 15

        // Header / Toolbar
        RowLayout {
            Layout.fillWidth: true
            spacing: 20

            Label {
                text: "Weekly Schedule"
                font.pixelSize: 24
                font.bold: true
                color: "white"
            }

            Item { Layout.fillWidth: true }

            Button {
                text: "Copy Day"
                enabled: selectedDay !== -1
                onClicked: {
                    clipboardDay = selectedDay
                    console.log("Copied day " + clipboardDay)
                }
            }

            Button {
                text: "Paste Day"
                enabled: clipboardDay !== -1 && selectedDay !== -1 && clipboardDay !== selectedDay
                onClicked: {
                    if (clipboardDay !== -1 && selectedDay !== -1) {
                        weeklyScheduleModel.copyDay(clipboardDay, selectedDay)
                        clipboardDay = -1
                    }
                }
            }
            
            Button {
                text: "Clear Day"
                enabled: selectedDay !== -1
                onClicked: weeklyScheduleModel.clearDay(selectedDay)
            }
            
            Button {
                text: "Refresh"
                onClicked: weeklyScheduleModel.loadSchedule()
            }
        }

        // Schedule Grid
        // Using TableView for 24x7 grid
        TableView {
            id: scheduleTable
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            
            model: weeklyScheduleModel
            
            columnWidthProvider: function (column) { return (root.width - 60) / 7; } // roughly valid
            rowHeightProvider: function (row) { return 60; }
            
            delegate: Rectangle {
                implicitWidth: (root.width - 60) / 7
                implicitHeight: 60
                
                color: model.color || "#2D2D30"
                border.width: 1
                border.color: "#3E3E42"
                
                property bool isSelected: root.selectedDay === model.column
                
                Rectangle {
                    anchors.fill: parent
                    color: "white"
                    opacity: 0.1
                    visible: parent.isSelected
                }

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 5
                    spacing: 2
                    
                    Label {
                        text: model.itemCount + " items"
                        color: "white"
                        font.bold: true
                        font.pixelSize: 12
                    }
                    
                    Label {
                        text: (model.duration / 60).toFixed(1) + " mins"
                        color: "#AAAAAA"
                        font.pixelSize: 10
                    }
                }
                
                MouseArea {
                    anchors.fill: parent
                    onClicked: {
                        console.log("Clicked Day: " + model.column + " Hour: " + model.row)
                        root.selectedDay = model.column
                        
                        // Load slot into PlaylistModel (Rundown)
                        playlistModel.loadSlot(model.column, model.row)
                        
                        // Navigate to Playlist Page
                        // Access stackView from root window context logic or signal
                        // For now, we assume stackView is globally accessible or we emit a signal
                        // Better approach: use a signal
                        
                        // Using globally registered object name or property if available. 
                        // In main.qml: stackView.replace(playlistPage)
                        // We can't access stackView directly here easily without an alias.
                        // Let's rely on `main.qml` handling.
                        
                        // Ideally: find parent "window" or "stackView"
                        var win = Window.window
                        if (win && win.navigateToPlaylist) {
                             win.navigateToPlaylist()
                        }
                    }
                }
            }
            
            // Header for Days
            // Note: TableView in QtQuick 2 doesn't have built-in headers like Widgets.
            // We usually implement them outside.
        }
    }
}
