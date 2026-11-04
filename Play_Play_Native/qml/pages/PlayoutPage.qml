import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: playoutPage

    // Sync ON AIR row from engine current asset
    Connections {
        target: controlService
        function onCurrentPlayingAssetIdChanged() {
            var row = playlistModel.rowForAssetId(controlService.currentPlayingAssetId)
            playlistModel.currentPlayingRow = row
        }
    }

    // Colors from parent
    property color bgColor: "#05050a"
    property color surfaceColor: "#0f0f18"
    property color surfaceElevated: "#141420"
    property color surfaceHighlight: "#1a1a28"
    property color borderColor: "#2a2a40"
    property color textPrimary: "#f0f0f8"
    property color textSecondary: "#a0a0b8"
    property color textMuted: "#505068"
    property color accentColor: "#00d4ff"
    property color onAirColor: "#ff2d55"
    property color readyColor: "#30d158"
    property color warningColor: "#ffd60a"

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // === LEFT DOCK - Library & Run of Show ===
        Rectangle {
            id: leftDock
            Layout.preferredWidth: 380
            Layout.fillHeight: true
            color: Qt.darker(surfaceColor, 1.1)

            ColumnLayout {
                anchors.fill: parent
                spacing: 0

                // Tab Bar
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 40
                    color: surfaceElevated

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 4
                        spacing: 4

                        TabButton {
                            id: libraryTabBtn
                            text: "📚 Library"
                            checked: true
                            Layout.fillWidth: true
                            
                            background: Rectangle {
                                color: libraryTabBtn.checked ? surfaceHighlight : "transparent"
                                radius: 4
                            }
                            contentItem: Text {
                                text: libraryTabBtn.text
                                font.pixelSize: 12
                                font.bold: libraryTabBtn.checked
                                color: libraryTabBtn.checked ? textPrimary : textSecondary
                                horizontalAlignment: Text.AlignHCenter
                            }
                        }

                        TabButton {
                            id: collectionsTabBtn
                            text: "⭐ Collections"
                            Layout.fillWidth: true
                            
                            background: Rectangle {
                                color: collectionsTabBtn.checked ? surfaceHighlight : "transparent"
                                radius: 4
                            }
                            contentItem: Text {
                                text: collectionsTabBtn.text
                                font.pixelSize: 12
                                font.bold: collectionsTabBtn.checked
                                color: collectionsTabBtn.checked ? textPrimary : textSecondary
                                horizontalAlignment: Text.AlignHCenter
                            }
                        }
                    }
                }

                // Search Bar
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 40
                    Layout.margins: 8
                    color: surfaceColor
                    radius: 6
                    border.color: searchField.activeFocus ? accentColor : borderColor

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 8
                        spacing: 8

                        Text {
                            text: "🔍"
                            font.pixelSize: 14
                            color: textMuted
                        }

                        TextField {
                            id: searchField
                            Layout.fillWidth: true
                            placeholderText: "Search media..."
                            color: textPrimary
                            placeholderTextColor: textMuted
                            background: Item {}
                            font.pixelSize: 12
                        }
                    }
                }

                // Media Library Grid
                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    color: "transparent"

                    GridView {
                        id: mediaGrid
                        anchors.fill: parent
                        anchors.margins: 8
                        cellWidth: 170
                        cellHeight: 130
                        clip: true

                        model: mediaLibraryModel

                        delegate: Rectangle {
                            width: 162
                            height: 122
                            radius: 6
                            color: mediaItemArea.containsMouse ? surfaceHighlight : surfaceColor
                            border.color: mediaItemArea.containsMouse ? accentColor : borderColor
                            border.width: mediaItemArea.containsMouse ? 2 : 1

                            Drag.active: mediaItemArea.drag.active
                            Drag.hotSpot.x: width / 2
                            Drag.hotSpot.y: height / 2
                            Drag.mimeData: { "text/plain": model.id }

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 8
                                spacing: 4

                                // Thumbnail
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 70
                                    color: "#000000"
                                    radius: 4

                                    Text {
                                        anchors.centerIn: parent
                                        text: model.type === "video" ? "🎬" : "🎵"
                                        font.pixelSize: 24
                                        color: textMuted
                                    }

                                    // Duration badge
                                    Rectangle {
                                        anchors.right: parent.right
                                        anchors.bottom: parent.bottom
                                        anchors.margins: 4
                                        color: Qt.rgba(0, 0, 0, 0.8)
                                        radius: 3

                                        Text {
                                            anchors.centerIn: parent
                                            text: formatDuration(model.duration || 0)
                                            font.pixelSize: 10
                                            font.family: "JetBrains Mono"
                                            color: "white"
                                        }
                                    }
                                }

                                // Title
                                Text {
                                    Layout.fillWidth: true
                                    text: model.title || "Untitled"
                                    font.pixelSize: 11
                                    font.bold: true
                                    color: textPrimary
                                    elide: Text.ElideRight
                                }

                                // Type
                                Text {
                                    text: (model.type || "video").toUpperCase()
                                    font.pixelSize: 9
                                    font.bold: true
                                    color: textMuted
                                }
                            }

                            MouseArea {
                                id: mediaItemArea
                                anchors.fill: parent
                                hoverEnabled: true
                                drag.target: parent
                                
                                onDoubleClicked: {
                                    // Add to playlist (full media for engine path)
                                    playlistModel.addItemFromMedia(mediaLibraryModel.getMediaItem(index))
                                }
                            }
                        }
                    }
                }
            }
        }

        // Resizer
        Rectangle {
            Layout.preferredWidth: 4
            Layout.fillHeight: true
            color: borderColor

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.SplitHCursor
                // TODO: Implement resize logic
            }
        }

        // === CENTER - Main Playout Area ===
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: bgColor

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 12
                spacing: 12

                // Schedule Header
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 48
                    color: surfaceColor
                    radius: 8
                    border.color: borderColor

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 16

                        ColumnLayout {
                            spacing: 0
                            Text {
                                text: "SCHEDULE START"
                                font.pixelSize: 9
                                font.bold: true
                                font.letterSpacing: 1
                                color: textMuted
                            }
                            Text {
                                text: Qt.formatDateTime(new Date(), "hh:mm:ss")
                                font.pixelSize: 14
                                font.family: "JetBrains Mono"
                                color: textPrimary
                            }
                        }

                        ColumnLayout {
                            spacing: 0
                            Text {
                                text: "SCHEDULE END"
                                font.pixelSize: 9
                                font.bold: true
                                font.letterSpacing: 1
                                color: textMuted
                            }
                            Text {
                                text: "23:59:59"
                                font.pixelSize: 14
                                font.family: "JetBrains Mono"
                                color: textPrimary
                            }
                        }

                        Item { Layout.fillWidth: true }

                        Button {
                            text: "📷 Snap to Now"
                            flat: true
                            background: Rectangle {
                                color: parent.hovered ? accentColor : surfaceHighlight
                                radius: 4
                            }
                            contentItem: Text {
                                text: parent.text
                                font.pixelSize: 11
                                font.bold: true
                                color: parent.hovered ? "white" : textPrimary
                                horizontalAlignment: Text.AlignHCenter
                            }
                            onClicked: {
                                var now = new Date()
                                var nowSecs = now.getHours() * 3600 + now.getMinutes() * 60 + now.getSeconds()
                                for (var i = 0; i < playlistModel.rowCount(); i++) {
                                    var start = playlistModel.getItem(i).startTime
                                    if (start && start.getHours) {
                                        var startSecs = start.getHours() * 3600 + start.getMinutes() * 60 + start.getSeconds()
                                        if (startSecs <= nowSecs) playlistListView.currentIndex = i
                                    }
                                }
                            }
                        }

                        Button {
                            text: "📥 Load Hour"
                            flat: true
                            background: Rectangle {
                                color: parent.hovered ? readyColor : surfaceHighlight
                                radius: 4
                            }
                            contentItem: Text {
                                text: parent.text
                                font.pixelSize: 11
                                font.bold: true
                                color: parent.hovered ? "white" : textPrimary
                                horizontalAlignment: Text.AlignHCenter
                            }
                            onClicked: {
                                var now = new Date()
                                playlistModel.loadSlot(now.getDay(), now.getHours())
                                window.isPlaying = false
                                window.nowPlayingTitle = ""
                            }
                        }
                    }
                }

                // Playlist Editor
                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    color: surfaceColor
                    radius: 8
                    border.color: borderColor

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 0

                        // Playlist Header
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 36
                            color: surfaceElevated
                            radius: 8

                            Rectangle {
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.bottom: parent.bottom
                                height: 8
                                color: surfaceElevated
                            }

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 12
                                anchors.rightMargin: 12

                                Text { text: "#"; font.pixelSize: 10; font.bold: true; color: textMuted; Layout.preferredWidth: 30 }
                                Text { text: "START"; font.pixelSize: 10; font.bold: true; color: textMuted; Layout.preferredWidth: 70 }
                                Text { text: "TITLE"; font.pixelSize: 10; font.bold: true; color: textMuted; Layout.fillWidth: true }
                                Text { text: "DURATION"; font.pixelSize: 10; font.bold: true; color: textMuted; Layout.preferredWidth: 70 }
                                Text { text: "STATUS"; font.pixelSize: 10; font.bold: true; color: textMuted; Layout.preferredWidth: 80 }
                            }
                        }

                        // Playlist Items
                        ListView {
                            id: playlistListView
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            model: playlistModel
                            spacing: 1

                            delegate: Rectangle {
                                width: playlistListView.width
                                height: 44
                                color: index === playlistListView.currentIndex ? Qt.rgba(0, 212, 255, 0.1) : 
                                       (index % 2 === 0 ? surfaceColor : surfaceElevated)
                                border.color: model.isPlaying ? onAirColor : "transparent"
                                border.width: model.isPlaying ? 2 : 0

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 12
                                    anchors.rightMargin: 12
                                    spacing: 8

                                    // Index
                                    Text {
                                        text: (index + 1).toString().padStart(2, '0')
                                        font.pixelSize: 11
                                        font.family: "JetBrains Mono"
                                        color: textMuted
                                        Layout.preferredWidth: 30
                                    }

                                    // Start Time
                                    Text {
                                        text: model.startTime || "--:--:--"
                                        font.pixelSize: 11
                                        font.family: "JetBrains Mono"
                                        color: textSecondary
                                        Layout.preferredWidth: 70
                                    }

                                    // Title
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 8

                                        Rectangle {
                                            width: 4
                                            height: 24
                                            radius: 2
                                            color: model.type === "video" ? "#5856d6" :
                                                   model.type === "audio" ? "#30d158" :
                                                   model.type === "graphics" ? "#ff9f0a" : "#64d2ff"
                                        }

                                        Text {
                                            Layout.fillWidth: true
                                            text: model.title || "Untitled"
                                            font.pixelSize: 12
                                            font.bold: model.isPlaying
                                            color: textPrimary
                                            elide: Text.ElideRight
                                        }
                                    }

                                    // Duration
                                    Text {
                                        text: formatDuration(model.duration || 0)
                                        font.pixelSize: 11
                                        font.family: "JetBrains Mono"
                                        color: textSecondary
                                        Layout.preferredWidth: 70
                                    }

                                    // Status
                                    Rectangle {
                                        Layout.preferredWidth: 70
                                        Layout.preferredHeight: 20
                                        radius: 3
                                        color: model.isPlaying ? onAirColor :
                                               model.status === "Ready" ? Qt.rgba(48, 209, 88, 0.2) :
                                               model.status === "Cued" ? Qt.rgba(94, 92, 230, 0.2) :
                                               surfaceHighlight

                                        Text {
                                            anchors.centerIn: parent
                                            text: model.isPlaying ? "ON AIR" : (model.status || "Ready")
                                            font.pixelSize: 9
                                            font.bold: true
                                            color: model.isPlaying ? "white" :
                                                   model.status === "Ready" ? readyColor :
                                                   model.status === "Cued" ? "#5e5ce6" :
                                                   textMuted
                                        }
                                    }
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    onClicked: playlistListView.currentIndex = index
                                    onDoubleClicked: {
                                        var path = model.assetPath || model.assetId || ""
                                        if (model.assetId && path) {
                                            controlService.playItem(model.assetId, path)
                                            playlistModel.currentPlayingRow = index
                                        }
                                        window.isPlaying = true
                                        window.nowPlayingTitle = model.title || "Untitled"
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        // Resizer
        Rectangle {
            Layout.preferredWidth: 4
            Layout.fillHeight: true
            color: borderColor

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.SplitHCursor
            }
        }

        // === RIGHT - Player Column ===
        Rectangle {
            id: playerColumn
            Layout.preferredWidth: 400
            Layout.fillHeight: true
            color: Qt.darker(surfaceColor, 1.1)

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 12
                spacing: 12

                // Studio Clock
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 80
                    color: surfaceColor
                    radius: 8
                    border.color: borderColor

                    ColumnLayout {
                        anchors.centerIn: parent
                        spacing: 4

                        Text {
                            id: studioClockTime
                            Layout.alignment: Qt.AlignHCenter
                            font.pixelSize: 36
                            font.bold: true
                            font.family: "JetBrains Mono"
                            color: textPrimary

                            Timer {
                                interval: 100
                                running: true
                                repeat: true
                                triggeredOnStart: true
                                onTriggered: {
                                    var now = new Date()
                                    studioClockTime.text = Qt.formatDateTime(now, "hh:mm:ss.zzz").slice(0, -2)
                                }
                            }
                        }

                        Text {
                            Layout.alignment: Qt.AlignHCenter
                            text: Qt.formatDateTime(new Date(), "dddd, MMMM d, yyyy")
                            font.pixelSize: 11
                            color: textSecondary
                        }
                    }
                }

                // Video Player Preview
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: width * 9 / 16
                    color: "#000000"
                    radius: 8

                    Rectangle {
                        anchors.fill: parent
                        radius: 8
                        color: "transparent"
                        border.color: window.isPlaying ? onAirColor : borderColor
                        border.width: window.isPlaying ? 2 : 1
                    }

                    Text {
                        anchors.centerIn: parent
                        text: window.isPlaying ? "▶ " + window.nowPlayingTitle : "No Media"
                        font.pixelSize: 14
                        color: textMuted
                    }

                    // On-Air Badge
                    Rectangle {
                        visible: window.isPlaying
                        anchors.top: parent.top
                        anchors.right: parent.right
                        anchors.margins: 12
                        width: 60
                        height: 24
                        radius: 4
                        color: onAirColor

                        Text {
                            anchors.centerIn: parent
                            text: "ON AIR"
                            font.pixelSize: 10
                            font.bold: true
                            color: "white"
                        }

                        SequentialAnimation on opacity {
                            running: window.isPlaying
                            loops: Animation.Infinite
                            NumberAnimation { to: 0.6; duration: 500 }
                            NumberAnimation { to: 1.0; duration: 500 }
                        }
                    }

                    // Progress Bar
                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: 4
                        color: surfaceHighlight
                        radius: 2

                        Rectangle {
                            width: parent.width * 0.35
                            height: parent.height
                            radius: 2
                            color: accentColor
                        }
                    }
                }

                // Transport Controls
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 60
                    color: surfaceColor
                    radius: 8
                    border.color: borderColor

                    RowLayout {
                        anchors.centerIn: parent
                        spacing: 8

                        // Previous
                        TransportButton { icon: "⏮"; onClicked: {} }

                        // Stop
                        TransportButton { 
                            icon: "⏹"
                            onClicked: {
                                controlService.stopPlayback()
                                window.isPlaying = false
                                window.nowPlayingTitle = ""
                            }
                        }

                        // Play/Pause
                        TransportButton {
                            icon: window.isPlaying ? "⏸" : "▶"
                            isPrimary: true
                            isPlaying: window.isPlaying
                            onClicked: {
                                if (window.isPlaying) controlService.pause()
                                else controlService.play()
                                window.isPlaying = !window.isPlaying
                            }
                        }

                        // Next
                        TransportButton { icon: "⏭"; onClicked: controlService.skipNext() }

                        Item { width: 8 }

                        // Live Button
                        Rectangle {
                            width: 80
                            height: 40
                            radius: 6
                            color: window.isPlaying ? onAirColor : surfaceHighlight

                            Text {
                                anchors.centerIn: parent
                                text: window.isPlaying ? "STOP" : "GO LIVE"
                                font.pixelSize: 11
                                font.bold: true
                                color: window.isPlaying ? "white" : textPrimary
                            }

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    if (window.isPlaying) {
                                        controlService.stopPlayback()
                                        window.isPlaying = false
                                        window.nowPlayingTitle = ""
                                    } else {
                                        controlService.play()
                                        window.isPlaying = true
                                    }
                                }
                            }
                        }
                    }
                }

                // Now Playing Info
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 80
                    color: surfaceColor
                    radius: 8
                    border.color: borderColor

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 4

                        Text {
                            text: "NOW PLAYING"
                            font.pixelSize: 9
                            font.bold: true
                            font.letterSpacing: 1
                            color: accentColor
                        }

                        Text {
                            Layout.fillWidth: true
                            text: window.nowPlayingTitle || "Nothing playing"
                            font.pixelSize: 14
                            font.bold: true
                            color: textPrimary
                            elide: Text.ElideRight
                        }

                        Text {
                            text: window.isPlaying ? "00:01:23 / 00:03:45" : "--:--:-- / --:--:--"
                            font.pixelSize: 12
                            font.family: "JetBrains Mono"
                            color: textSecondary
                        }
                    }
                }

                // Up Next
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 60
                    color: surfaceColor
                    radius: 8
                    border.color: borderColor

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 8

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2

                            Text {
                                text: "UP NEXT"
                                font.pixelSize: 9
                                font.bold: true
                                font.letterSpacing: 1
                                color: textMuted
                            }

                            Text {
                                Layout.fillWidth: true
                                text: "Commercial Break"
                                font.pixelSize: 12
                                font.bold: true
                                color: textPrimary
                                elide: Text.ElideRight
                            }
                        }

                        Text {
                            text: "⏱ 00:02:22"
                            font.pixelSize: 12
                            font.family: "JetBrains Mono"
                            color: accentColor
                        }
                    }
                }

                Item { Layout.fillHeight: true }

                // Side Panel Toggles
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 44
                    color: surfaceColor
                    radius: 8
                    border.color: borderColor

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 8
                        spacing: 8

                        SidePanelButton { text: "CG"; panel: "cg" }
                        SidePanelButton { text: "OBS"; panel: "obs" }
                        SidePanelButton { text: "DDR"; panel: "ddr" }
                        SidePanelButton { text: "Router"; panel: "router" }
                    }
                }
            }
        }
    }

    function formatDuration(seconds) {
        var mins = Math.floor(seconds / 60)
        var secs = Math.floor(seconds % 60)
        return mins.toString().padStart(2, '0') + ":" + secs.toString().padStart(2, '0')
    }
}
