import QtQuick
import QtQuick.Window
import QtQuick.Controls
import QtQuick.Layouts
import QtQml
import "."
import "ui"
import "views"

/**
 * PLAY PLAY operator shell, laid out like PlayPlayClean:
 *   top menu bar (On Air, Schedule, Reports, Timeline, Remote | Output, CG, Tools | Config, Logs, Help)
 *   left icon rail (channel pills + view icons)
 *   persistent control column (clock, channels, transport, hour rundown)
 *   content: schedule ticker + the current view.
 * All data comes from the Supervisor (`supervisor` over IPC, `api` over REST).
 */
ApplicationWindow {
    id: window
    width: 1920
    height: 1080
    minimumWidth: 1280
    minimumHeight: 720
    visible: true
    title: "PLAY PLAY — " + (supervisor.connected ? "Supervisor connected" : "connecting")
    color: Theme.bg0
    font.family: Theme.fontSans
    font.pixelSize: Theme.fs12

    property double netToastAt: 0
    property string currentPage: "onair"
    property bool compactColumn: false
    readonly property bool columnShown: !compactColumn && currentPage !== "onair"      // On Air carries its own transport, rundown and channel wall
    property var onAirDrawers: Qt.application.arguments.indexOf("--drawers") > 0 ? ({ mixer: true, ptz: true }) : ({ mixer: false, ptz: false })      // collapsible mixer / PTZ panels of the On Air page
    property var cgDeckState: ({ tplId: "starnews_lower_third", t1: "", t2: "", t3: "", hold: "0" })     // the On Air graphics deck keeps its text across page changes
    property var pages: [
        { id: "onair",    label: "On Air",       icon: "▶", group: 0 },
        { id: "schedule", label: "Schedule",     icon: "▦", group: 0 },
        { id: "programming", label: "Programming", icon: "⚡", group: 0 },
        { id: "reports",  label: "Reports",      icon: "☰", group: 0 },
        { id: "timeline", label: "Timeline",     icon: "≡", group: 0 },
        { id: "remote",   label: "Remote",       icon: "☁", group: 0 },
        { id: "output",   label: "Output",       icon: "↗", group: 1 },
        { id: "cg",       label: "CG Templates", icon: "⌸", group: 1 },
        { id: "inputs",   label: "Inputs",       icon: "◉", group: 1 },
        { id: "mesh",     label: "Mesh",         icon: "⬡", group: 1 },
        { id: "mux",      label: "Mux",          icon: "⊞", group: 1 },
        { id: "spots",    label: "Spots",        icon: "✦", group: 1 },
        { id: "overlays", label: "Overlays",     icon: "◩", group: 1 },
        { id: "studio",   label: "Spot Studio",  icon: "✎", group: 1 },
        { id: "render",   label: "Render",       icon: "⧉", group: 1 },
        { id: "publish",  label: "Publish",      icon: "⇪", group: 1 },
        { id: "starnodes", label: "Star Nodes",  icon: "✶", group: 1 },
        { id: "agents",   label: "Agents",       icon: "⚇", group: 1 },
        { id: "tools",    label: "Tools",        icon: "⚒", group: 1 },
        { id: "config",   label: "Config",       icon: "⚙", group: 2 },
        { id: "logs",     label: "Logs",         icon: "☷", group: 2 },
        { id: "help",     label: "Help",         icon: "?",      group: 2 }
    ]

    function navTo(pageId) {
        if (pageId === currentPage) return
        currentPage = pageId
        switch (pageId) {
        case "onair": stack.replace(onAirView); break
        case "schedule": stack.replace(scheduleView); break
        case "programming": stack.replace(programmingView); break
        case "reports": stack.replace(reportsView); break
        case "timeline": stack.replace(timelineView); break
        case "remote": stack.replace(remoteView); break
        case "output": stack.replace(outputView); break
        case "cg": stack.replace(cgView); break
        case "inputs": stack.replace(inputsView); break
        case "mesh": stack.replace(meshView); break
        case "mux": stack.replace(muxView); break
        case "spots": stack.replace(spotsView); break
        case "overlays": stack.replace(overlaysView); break
        case "studio": stack.replace(spotStudioView); break
        case "render": stack.replace(renderView); break
        case "publish": stack.replace(publisherView); break
        case "starnodes": stack.replace(starNodesView); break
        case "agents": stack.replace(agentsView); break
        case "tools": stack.replace(toolsView); break
        case "config": stack.replace(configView); break
        case "logs": stack.replace(logsView); break
        case "help": stack.replace(helpView); break
        }
    }
    function openScheduleHour(channelId, slotId, hourStartMs) {
        if (channelId) supervisor.activeChannelId = channelId
        navTo("schedule")
        Qt.callLater(function() {
            if (stack.currentItem && stack.currentItem.openScheduleHour) stack.currentItem.openScheduleHour(channelId, slotId, hourStartMs)
            else window.toast("Could not open the schedule editor", "error")
        })
    }
    function toast(msg, level) { console.info("[toast] " + (level || "info") + ": " + msg); toastBar.show(msg, level || "info") }

    // --- text size (persisted) + shared clipboard for the context menus ---------------------------
    readonly property var textScales: [0.9, 1.0, 1.1, 1.2, 1.35, 1.5, 1.7]
    function setTextScale(v) { uiScale.zoom = v; toast("Interface size " + Math.round(uiScale.effectiveZoom * 100) + "%", "ok") }
    function stepTextScale(d) { uiScale.step(d); toast("Interface size " + Math.round(uiScale.effectiveZoom * 100) + "%", "ok") }
    Shortcut { sequences: ["Ctrl+=", "Ctrl++"]; onActivated: { window.stepTextScale(1) } }
    Shortcut { sequence: "Ctrl+-"; onActivated: { window.stepTextScale(-1) } }
    Shortcut { sequence: "Ctrl+0"; onActivated: { uiScale.reset() } }
    property var clip: null             // {kind: "items"|"hour"|"day", data, label}
    function setClip(kind, data, label) { clip = { kind: kind, data: JSON.parse(JSON.stringify(data)), label: label || kind }; toast("Copied " + (label || kind), "ok") }
    TextEdit { id: sysClip; visible: false }
    function copyText(s) { sysClip.text = String(s); sysClip.selectAll(); sysClip.copy(); toast("Copied " + (String(s).length > 60 ? String(s).substring(0, 60) + "…" : s), "ok") }

    Shortcut { sequence: "F1"; onActivated: { navTo("onair") } }
    Shortcut { sequence: "F2"; onActivated: { navTo("schedule") } }
    Shortcut { sequence: "F3"; onActivated: { navTo("reports") } }
    Shortcut { sequence: "F4"; onActivated: { navTo("timeline") } }
    Shortcut { sequence: "F5"; onActivated: { navTo("output") } }
    Shortcut { sequence: "F6"; onActivated: { navTo("cg") } }
    Shortcut { sequence: "F7"; onActivated: { navTo("config") } }
    Shortcut { sequence: "F8"; onActivated: { navTo("logs") } }
    // On Air switcher bus: 1..8 select the input on preview, Alt+1..8 take it to PGM, Ctrl+Enter cuts the preview, Esc clears it.
    // (F1..F8 stay page navigation.) Plain digits do not fire while a text field is being edited.
    InputMap { id: inputMap }
    MixerState { id: mixer }
    Instantiator { model: 8; delegate: Shortcut { sequence: String(index + 1); context: Qt.ApplicationShortcut; enabled: window.currentPage === "onair"; onActivated: { inputMap.select(index) } } }
    Instantiator { model: 8; delegate: Shortcut { sequence: "Alt+" + (index + 1); context: Qt.ApplicationShortcut; enabled: window.currentPage === "onair"; onActivated: { inputMap.take(index) } } }
    Shortcut { sequences: ["Ctrl+Return", "Ctrl+Enter"]; context: Qt.ApplicationShortcut; enabled: window.currentPage === "onair"; onActivated: { inputMap.cut() } }
    Shortcut { sequences: ["Ctrl+Shift+Return", "Ctrl+Shift+Enter"]; context: Qt.ApplicationShortcut; enabled: window.currentPage === "onair"; onActivated: { inputMap.autoTake() } }
    Shortcut { sequence: "Escape"; context: Qt.ApplicationShortcut; enabled: window.currentPage === "onair" && inputMap.pvw >= 0; onActivated: { inputMap.pvw = -1 } }
    // Space = play/pause unless a text input is being edited or a popup / dialog / menu holds the focus (there it keeps today's
    // "nothing happens"). Buttons have `text` too and take focus on click, so testing `text` left Space dead after any click (UXB-0).
    function spaceIsTransport(f) { if (!f) return true; if (f.hasOwnProperty("cursorPosition") || (f.hasOwnProperty("editText") && f.editable === true)) return false; var o = window.Overlay.overlay; for (var p = f; p; p = p.parent) if (p === o) return false; return true }
    Shortcut { sequence: "Space"; context: Qt.ApplicationShortcut; onActivated: { if (window.spaceIsTransport(activeFocusItem)) (supervisor.activeState.state === "ON_AIR" ? supervisor.pause() : supervisor.play()) } }
    Shortcut { sequence: "Ctrl+Right"; onActivated: { supervisor.skip() } }
    Shortcut { sequence: "Ctrl+L"; onActivated: { compactColumn = !compactColumn } }

    Connections {
        target: supervisor
        function onAlert(channelId, level, message) { if (level === "error" || level === "critical") toast(channelId + ": " + message, "error") }
        function onConnectedChanged() { if (supervisor.connected) toast("Supervisor connected", "ok") }
    }
    Connections {
        target: api
        function onFailed(method, path, status, message) {
            // no connection at all (status 0): one toast per 10 s, not one per failed call
            if (status === 0) { var now = Date.now(); if (now - window.netToastAt < 10000) return; window.netToastAt = now; toast("Supervisor not reachable (" + method + " " + path + ")", "error"); return }
            toast(message, "error")
        }
    }
    // perf-app PA-1: while the Supervisor is stalled (no IPC message for 3 s, or its pipe never accepts) the api.* GETs are held, one per
    // path, instead of piling into its backlog; they go out when it answers again. Writes still go out.
    Binding { target: api; property: "paused"; value: supervisor.supervisorStalled }

    // --- top menu bar ------------------------------------------------------------------------
    Rectangle {
        id: topBar
        anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
        height: Theme.topBarH
        color: Theme.bg1
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.border0 }
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 12; anchors.rightMargin: 12
            spacing: 2
            Row {
                spacing: 6
                Layout.rightMargin: 14
                Image { source: "qrc:/branding/logo.png"; height: 20; width: Math.round(20 * sourceSize.width / Math.max(1, sourceSize.height)); fillMode: Image.PreserveAspectFit; smooth: true; mipmap: true; anchors.verticalCenter: parent.verticalCenter }
            }
            // Page tabs. The strip takes the width the pinned right-hand cluster leaves. When every label does not fit, the inactive tabs fall back
            // to their icon (the tooltip names them); if even that is too wide the strip scrolls (wheel / drag) and keeps the current tab in view.
            // The tabs used to be fixed-width items of this layout, so at the 1280 px minimum window Logs, Help, A-/A+ and the SUPERVISOR indicator
            // were pushed off the right edge (and at 135%+ text on a 1920 px screen the indicator and address).
            Item {
                id: tabArea
                Layout.fillWidth: true; Layout.fillHeight: true; Layout.minimumWidth: 0
                clip: true
                // tabGhost always shows every label, so `compact` never depends on itself
                readonly property bool compact: tabGhost.implicitWidth > width
                function gapBefore(i) { return i > 0 && window.pages[i].group !== window.pages[i - 1].group }
                function reveal() {
                    var i = 0
                    for (; i < window.pages.length; ++i) if (window.pages[i].id === window.currentPage) break
                    var t = tabRep.itemAt(i); if (!t) return
                    var maxX = Math.max(0, tabFlick.contentWidth - tabFlick.width)
                    if (t.x < tabFlick.contentX) tabFlick.contentX = Math.max(0, t.x - 8)
                    else if (t.x + t.width > tabFlick.contentX + tabFlick.width) tabFlick.contentX = Math.min(maxX, t.x + t.width - tabFlick.width + 8)
                }
                onWidthChanged: { Qt.callLater(reveal) }
                onCompactChanged: { Qt.callLater(reveal) }
                Connections { target: window; function onCurrentPageChanged() { Qt.callLater(tabArea.reveal) } }
                // invisible measuring row: the width the strip needs with every label shown
                Row {
                    id: tabGhost
                    spacing: 2; opacity: 0; enabled: false
                    Repeater {
                        model: window.pages
                        Item {
                            width: ghostRow.implicitWidth + 22 + (tabArea.gapBefore(index) ? 18 : 0); height: 1
                            Row { id: ghostRow; spacing: 6
                                Text { text: modelData.icon; font.pixelSize: Theme.fs11 }
                                Text { text: modelData.label; font.pixelSize: Theme.fs12; font.bold: window.currentPage === modelData.id } }
                        }
                    }
                }
                Flickable {
                    id: tabFlick
                    anchors.fill: parent
                    contentWidth: tabStrip.width; contentHeight: height
                    flickableDirection: Flickable.HorizontalFlick; boundsBehavior: Flickable.StopAtBounds
                    interactive: contentWidth > width
                    WheelHandler {
                        target: null; enabled: tabFlick.contentWidth > tabFlick.width
                        onWheel: function(e) { tabFlick.contentX = Math.max(0, Math.min(tabFlick.contentWidth - tabFlick.width, tabFlick.contentX - (e.angleDelta.y !== 0 ? e.angleDelta.y : e.angleDelta.x))) }
                    }
                    Row {
                        id: tabStrip
                        height: tabFlick.height; spacing: 2
                        Repeater {
                            id: tabRep
                            model: window.pages
                            Item {
                                id: tab
                                property bool current: window.currentPage === modelData.id
                                property bool gap: tabArea.gapBefore(index)
                                property bool showLabel: !tabArea.compact || current
                                width: tabRow.implicitWidth + 22 + (gap ? 18 : 0); height: tabStrip.height
                                Rectangle { visible: tab.gap; x: 6; width: 1; height: 16; anchors.verticalCenter: parent.verticalCenter; color: Theme.border1 }
                                Rectangle {
                                    anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                                    width: tabRow.implicitWidth + 22; height: 28; radius: Theme.r5
                                    color: tab.current ? Theme.bg4 : tabMa.containsMouse ? Theme.bg3 : "transparent"
                                    border.color: tab.current ? Theme.border1 : "transparent"
                                    Row {
                                        id: tabRow; anchors.centerIn: parent; spacing: 6
                                        Text { text: modelData.icon; color: tab.current ? Theme.accent0 : Theme.text2; font.pixelSize: Theme.fs11; anchors.verticalCenter: parent.verticalCenter }
                                        Text { visible: tab.showLabel; text: modelData.label; color: tab.current ? Theme.text0 : Theme.text1; font.pixelSize: Theme.fs12; font.bold: tab.current; anchors.verticalCenter: parent.verticalCenter }
                                    }
                                    MouseArea { id: tabMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: { window.navTo(modelData.id) } }
                                    ToolTip.visible: tabMa.containsMouse && !tab.showLabel; ToolTip.text: modelData.label; ToolTip.delay: 300
                                }
                            }
                        }
                    }
                }
            }
            Row { spacing: 2; Layout.rightMargin: 10
                CButton { text: "A−"; small: true; variant: "ghost"; onClicked: { window.stepTextScale(-1) } ToolTip.text: "Smaller text (Ctrl+-)" }
                Text { text: Math.round(uiScale.effectiveZoom * 100) + "%"; color: Theme.text3; font.pixelSize: Theme.fs10; anchors.verticalCenter: parent.verticalCenter; width: 34; horizontalAlignment: Text.AlignHCenter
                    MouseArea { anchors.fill: parent; onClicked: { uiScale.reset() } cursorShape: Qt.PointingHandCursor } }
                CButton { text: "A+"; small: true; variant: "ghost"; onClicked: { window.stepTextScale(1) } ToolTip.text: "Bigger text (Ctrl+=)" } }
            Rectangle { width: 8; height: 8; radius: 4; color: supervisor.supervisorStalled ? Theme.standby : supervisor.connected ? Theme.ready : Theme.danger }
            Text { text: supervisor.supervisorStalled ? "SUPERVISOR NOT RESPONDING" : supervisor.connected ? "SUPERVISOR" + (supervisor.launchedByUs ? " (LOCAL)" : "") : "NO SUPERVISOR"; color: supervisor.supervisorStalled ? Theme.standby : Theme.text2; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1; Layout.leftMargin: 4; Layout.rightMargin: 12 }
            Text { text: api.baseUrl.replace("http://", ""); color: Theme.text3; font.family: Theme.fontMono; font.pixelSize: Theme.fs10; Layout.rightMargin: 12 }
            BusyIndicator { running: api.pending > 0; visible: running; implicitWidth: 18; implicitHeight: 18 }
        }
    }

    // --- Supervisor stalled banner (perf-app PA-1): over the top of the content while the Supervisor is hung ----------------------
    Rectangle {
        id: stallBanner
        readonly property var info: supervisor.stallInfo
        readonly property var health: info && info.health ? info.health : null
        anchors.left: parent.left; anchors.right: parent.right; anchors.top: topBar.bottom
        height: 34; visible: supervisor.supervisorStalled; clip: true
        z: 80
        color: Theme.standby
        Row {
            anchors.verticalCenter: parent.verticalCenter; anchors.left: parent.left; anchors.leftMargin: 14
            spacing: 14
            Text { text: "⚠"; color: "#05050a"; font.pixelSize: Theme.fs16; font.bold: true; anchors.verticalCenter: parent.verticalCenter }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                color: "#05050a"; font.pixelSize: Theme.fs12; font.bold: true
                text: (stallBanner.info.reason === "busy" ? "Supervisor not accepting connections"
                                                          : "Supervisor not responding (no message for " + Math.round((stallBanner.info.silentMs || 0) / 1000) + " s)")
                      + ": status and pictures here are frozen; refreshes resume when it answers."
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                visible: !!stallBanner.health
                color: "#05050a"; font.pixelSize: Theme.fs11; font.family: Theme.fontMono
                text: stallBanner.health ? "watchdog: " + stallBanner.health.state + (stallBanner.health.activity ? " · " + stallBanner.health.activity : "")
                                           + (stallBanner.health.lagMs !== undefined ? " · lag " + stallBanner.health.lagMs + " ms" : "") : ""
            }
        }
    }

    // --- icon rail --------------------------------------------------------------------------------
    Rectangle {
        id: rail
        anchors.left: parent.left; anchors.top: topBar.bottom; anchors.bottom: statusBar.top
        width: Theme.railW
        color: Theme.bg1
        Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.border0 }
        ColumnLayout {
            anchors.fill: parent; anchors.topMargin: 8; anchors.bottomMargin: 8
            spacing: 4
            Repeater {
                model: supervisor.channelModel   // perf-app PA-3: rows updated in place, not rebuilt every snapshot
                Rectangle {
                    Layout.alignment: Qt.AlignHCenter
                    width: 36; height: 30; radius: Theme.r5
                    property bool on: modelData.process && modelData.process.connected
                    property bool sel: modelData.profile.id === supervisor.activeChannelId
                    color: sel ? Theme.bg5 : "transparent"; border.color: sel ? Theme.accent0 : Theme.border1
                    Column { anchors.centerIn: parent; spacing: 0
                        Text { anchors.horizontalCenter: parent.horizontalCenter; text: "CH" + (index + 1); color: sel ? Theme.text0 : Theme.text1; font.pixelSize: 9; font.bold: true }
                        Text { anchors.horizontalCenter: parent.horizontalCenter; text: on ? "ON" : "OFF"; color: on ? Theme.ready : Theme.text3; font.pixelSize: 8; font.bold: true } }
                    MouseArea { anchors.fill: parent; onClicked: { supervisor.activeChannelId = modelData.profile.id } cursorShape: Qt.PointingHandCursor }
                    ToolTip.visible: ma2.containsMouse; ToolTip.text: modelData.profile.name
                    MouseArea { id: ma2; anchors.fill: parent; hoverEnabled: true; acceptedButtons: Qt.NoButton }
                }
            }
            Rectangle { Layout.alignment: Qt.AlignHCenter; width: 36; height: 24; radius: Theme.r5; color: "transparent"; border.color: Theme.border1
                Text { anchors.centerIn: parent; text: "+"; color: Theme.text2; font.pixelSize: Theme.fs14 }
                MouseArea { anchors.fill: parent; onClicked: { window.navTo("config"); } cursorShape: Qt.PointingHandCursor } }
            Rectangle { Layout.fillWidth: true; Layout.margins: 8; height: 1; color: Theme.border0 }
            Repeater {
                model: [ { id: "onair", icon: "▶", tip: "Playout" }, { id: "schedule", icon: "▦", tip: "Weekly Schedule" }, { id: "timeline", icon: "≡", tip: "Timeline" }, { id: "reports", icon: "☰", tip: "As-Played & Reports" }, { id: "remote", icon: "☁", tip: "Remote" } ]
                Rectangle {
                    Layout.alignment: Qt.AlignHCenter
                    width: 36; height: 36; radius: Theme.r5
                    color: window.currentPage === modelData.id ? Theme.accent0 : railMa.containsMouse ? Theme.bg3 : "transparent"
                    Text { anchors.centerIn: parent; text: modelData.icon; color: window.currentPage === modelData.id ? "#05050a" : Theme.text1; font.pixelSize: Theme.fs16 }
                    MouseArea { id: railMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: { window.navTo(modelData.id) } }
                    ToolTip.visible: railMa.containsMouse; ToolTip.text: modelData.tip; ToolTip.delay: 500
                }
            }
            Item { Layout.fillHeight: true }
            Rectangle { Layout.alignment: Qt.AlignHCenter; width: 36; height: 36; radius: Theme.r5; color: "transparent"
                Text { anchors.centerIn: parent; text: "↗"; color: Theme.text1; font.pixelSize: Theme.fs14 }
                MouseArea { anchors.fill: parent; onClicked: { window.navTo("output") } cursorShape: Qt.PointingHandCursor } }
            Rectangle { Layout.alignment: Qt.AlignHCenter; width: 36; height: 36; radius: Theme.r5; color: window.compactColumn ? Theme.bg4 : "transparent"
                Text { anchors.centerIn: parent; text: "⮜"; color: Theme.text1; font.pixelSize: Theme.fs14 }
                MouseArea { anchors.fill: parent; onClicked: { window.compactColumn = !window.compactColumn } cursorShape: Qt.PointingHandCursor } }
        }
    }

    // --- control column ---------------------------------------------------------------------------
    ControlColumn {
        id: column
        anchors.left: rail.right; anchors.top: topBar.bottom; anchors.bottom: statusBar.top
        width: window.columnShown ? Theme.columnW : 0
        visible: window.columnShown
        clip: true
        Behavior on width { NumberAnimation { duration: 140 } }
    }

    // --- content ----------------------------------------------------------------------------------
    Item {
        anchors.left: column.right; anchors.right: parent.right; anchors.top: topBar.bottom; anchors.bottom: statusBar.top
        ScheduleTicker {
            id: ticker
            anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
            height: Theme.tickerH
            runlist: supervisor.activeRunlist
            state: supervisor.activeState
        }
        StackView {
            id: stack
            anchors.left: parent.left; anchors.right: parent.right; anchors.top: ticker.bottom; anchors.bottom: parent.bottom
            anchors.margins: 10
            initialItem: onAirView
            replaceEnter: Transition { NumberAnimation { properties: "opacity"; from: 0; to: 1; duration: 120 } }
            replaceExit: Transition { NumberAnimation { properties: "opacity"; from: 1; to: 0; duration: 80 } }
        }
    }

    // --- status bar: always visible; frame rate, bitrate, CPU / memory, network ----------------------------------------
    SystemStatusBar {
        id: statusBar
        anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
        z: 5
    }

    // --- drop anywhere: files or folders from Explorer land in the media library --------------------
    function importPaths(paths, type) {
        var files = []
        for (var i = 0; i < paths.length; ++i) { var p = String(paths[i]); if (p.length) files.push(p) }
        if (!files.length) return
        api.post("/media/scan", { files: files, type: type || "program" }, function(ok, r) {
            if (ok) toast(r.queued ? "Importing " + r.queued + " file(s) into the library" : "Already in the library", "ok")
            else toast("Import failed: " + (r && r.error ? r.error : "see log"), "error")
        })
    }
    // Drop targets: panels (hour editor, On Air rundown, library) register here and get files dropped on them;
    // anywhere else a drop imports into the library. One window-wide DropArea routes by pointer position, because
    // it sits above every page and would otherwise shadow the panels' own drop areas.
    property var dropTargets: []
    function registerDropTarget(t) { if (dropTargets.indexOf(t) < 0) { var a = dropTargets.slice(); a.push(t); dropTargets = a } }
    function unregisterDropTarget(t) { dropTargets = dropTargets.filter(function(x) { return x !== t }) }
    function dropTargetAt(x, y) {
        for (var i = dropTargets.length - 1; i >= 0; --i) {
            var t = dropTargets[i]
            if (!t || !t.visible || t.width <= 0 || t.height <= 0) continue
            var p = dropAll.mapToItem(t, x, y)
            if (p.x >= 0 && p.y >= 0 && p.x < t.width && p.y < t.height) return t
        }
        return null
    }
    /// Expand dropped files/folders on the Supervisor into playlist-ready items [{path,title,type,duration,mediaId}].
    function resolvePaths(paths, cb) {
        api.post("/media/resolve", { paths: paths }, function(ok, r) {
            if (!ok) { toast("Could not read the dropped files: " + (r && r.error ? r.error : "see log"), "error"); cb(null); return }
            cb(r)
        })
    }
    function newPlaylistItem(m) {
        return { id: "i" + Date.now().toString(36) + Math.floor(Math.random() * 100000), mediaId: m.mediaId || "", title: m.title || "", path: m.path || "",
                 type: m.type || "program", duration: m.duration || 0, timeIn: 0, timeOut: 0, timingMode: "soft_start", transitionType: "cut", priority: 0 }
    }
    function routeDrop(urls, x, y) {
        var t = window.dropTargetAt(x, y)
        console.info("[drop] " + urls.length + " path(s) at " + Math.round(x) + "," + Math.round(y) + " -> " + (t ? (t.dropLabel || "panel") : "library import") + ": " + urls.join(" "))
        if (t) { var p = dropAll.mapToItem(t, x, y); t.dropPaths(urls, p.x, p.y) }
        else window.importPaths(urls)
    }
    // Test hook: --test-drop <path> [--drop-target <label prefix>] runs the drop code on a registered panel
    // (centre of the first target whose label starts with the prefix) without an OS drag.
    Timer {
        interval: 7000; running: Qt.application.arguments.indexOf("--test-drop") > 0
        onTriggered: {
            var a = Qt.application.arguments, i = a.indexOf("--test-drop"), j = a.indexOf("--drop-target")
            var path = a[i + 1], want = j > 0 ? a[j + 1] : ""
            console.info("[drop-test] targets: " + window.dropTargets.map(function(t) { return (t.dropLabel || "?") + (t.visible ? "" : " (hidden)") }).join(" | "))
            var t = null
            for (var k = 0; k < window.dropTargets.length; ++k) { var c = window.dropTargets[k]; if (c.visible && String(c.dropLabel || "").indexOf(want) === 0) { t = c; break } }
            if (!t) { console.info("[drop-test] no visible target matching '" + want + "'"); return }
            var p = t.mapToItem(dropAll, t.width / 2, t.height / 2)
            window.routeDrop([path.indexOf("file:") === 0 ? path : "file:///" + path.split("\\").join("/")], p.x, p.y)
        }
    }
    // Test hook: --test-select <n> puts input n (1..8) on preview, then --test-take <n> takes input n to PGM, through the same functions as the bus keys.
    Timer {
        interval: 3000; running: Qt.application.arguments.indexOf("--test-select") > 0 || Qt.application.arguments.indexOf("--test-take") > 0
        onTriggered: {
            var a = Qt.application.arguments, i = a.indexOf("--test-select"), j = a.indexOf("--test-take")

            if (i > 0) inputMap.select(parseInt(a[i + 1]) - 1)
            if (j > 0) { try { inputMap.take(parseInt(a[j + 1]) - 1) } catch (ex) { toast("take threw: " + ex, "error") } }
        }
    }
    // Test hook: --test-menu <n> opens the right-click menu of input n on the On Air page; --test-menu cg<n> opens the Graphics deck's CG n menu.
    property string testMenu: ""
    Timer {
        interval: 6000; running: Qt.application.arguments.indexOf("--test-menu") > 0
        onTriggered: { var a = Qt.application.arguments; window.testMenu = String(a[a.indexOf("--test-menu") + 1] || "") }
    }
    DropArea {
        id: dropAll
        anchors.fill: parent
        z: 90
        keys: ["text/uri-list"]
        property var hover: null
        onEntered: function(d) { hover = window.dropTargetAt(d.x, d.y) }
        onPositionChanged: function(d) { hover = window.dropTargetAt(d.x, d.y) }
        onExited: { hover = null }
        onDropped: function(d) {
            var urls = []; for (var i = 0; i < d.urls.length; ++i) urls.push(String(d.urls[i]))
            hover = null
            window.routeDrop(urls, d.x, d.y)
            d.accept()
        }
        // over a playlist / panel: outline it
        Rectangle {
            property rect r: dropAll.hover ? dropAll.hover.mapToItem(dropAll, 0, 0, dropAll.hover.width, dropAll.hover.height) : Qt.rect(0, 0, 0, 0)
            visible: dropAll.containsDrag && !!dropAll.hover
            x: r.x; y: r.y; width: r.width; height: r.height
            color: Qt.rgba(0, 0.83, 1, 0.14); border.color: Theme.accent0; border.width: 3; radius: Theme.r8
            Rectangle { anchors.centerIn: parent; width: dropHint.implicitWidth + 40; height: 56; radius: Theme.r8; color: Theme.bg2; border.color: Theme.accent0; border.width: 2
                Text { id: dropHint; anchors.centerIn: parent; text: "⤓  " + (dropAll.hover && dropAll.hover.dropLabel ? dropAll.hover.dropLabel : "Drop here"); color: Theme.text0; font.pixelSize: Theme.fs16; font.bold: true } }
        }
        // anywhere else: library import
        Rectangle {
            anchors.fill: parent; visible: dropAll.containsDrag && !dropAll.hover; color: Qt.rgba(0, 0, 0, 0.55)
            Rectangle { anchors.centerIn: parent; width: 620; height: 170; radius: Theme.r8; color: Theme.bg2; border.color: Theme.accent0; border.width: 2
                Column { anchors.centerIn: parent; spacing: 6
                    Text { text: "⤓"; color: Theme.accent0; font.pixelSize: 34; anchors.horizontalCenter: parent.horizontalCenter }
                    Text { text: "Drop to import into the media library"; color: Theme.text0; font.pixelSize: Theme.fs16; font.bold: true; anchors.horizontalCenter: parent.horizontalCenter }
                    Text { text: "Over the On Air rundown or the hour editor, files and folders go straight onto the playlist."; color: Theme.accent0; font.pixelSize: Theme.fs12; anchors.horizontalCenter: parent.horizontalCenter }
                    Text { text: "Files and folders are probed, hashed and thumbnailed on the Supervisor; folder names (ads, promo, ids) pick the type."; color: Theme.text2; font.pixelSize: Theme.fs11; anchors.horizontalCenter: parent.horizontalCenter } } }
        }
    }

    // --- toasts --------------------------------------------------------------------------------------
    Rectangle {
        id: toastBar
        property string level: "info"
        anchors.horizontalCenter: parent.horizontalCenter; anchors.bottom: statusBar.top; anchors.bottomMargin: 14
        width: toastText.implicitWidth + 32; height: 34; radius: Theme.r8
        color: level === "error" ? Theme.danger : level === "ok" ? Theme.ready : Theme.bg4
        border.color: Theme.border1
        opacity: 0; visible: opacity > 0
        z: 100
        function show(msg, lvl) { level = lvl; toastText.text = msg; opacity = 1; hide.restart() }
        Text { id: toastText; anchors.centerIn: parent; color: toastBar.level === "info" ? Theme.text0 : "#05050a"; font.pixelSize: Theme.fs12; font.bold: true }
        Timer { id: hide; interval: 3500; onTriggered: { toastBar.opacity = 0 } }
        Behavior on opacity { NumberAnimation { duration: 200 } }
    }

    Component { id: onAirView; OnAirView {} }
    Component { id: scheduleView; ScheduleView {} }
    Component { id: programmingView; ProgrammingView {} }
    Component { id: reportsView; ReportsView {} }
    Component { id: timelineView; TimelineView {} }
    Component { id: remoteView; RemoteView {} }
    Component { id: outputView; OutputView {} }
    Component { id: cgView; CGView {} }
    Component { id: inputsView; InputsView {} }
    Component { id: meshView; MeshView {} }
    Component { id: muxView; MuxView {} }
    Component { id: spotsView; SpotsView {} }
    Component { id: overlaysView; OverlaysView {} }
    Component { id: spotStudioView; SpotStudioView {} }
    Component { id: renderView; RenderView {} }
    Component { id: publisherView; PublisherView {} }
    Component { id: starNodesView; StarNodesView {} }
    Component { id: agentsView; AgentsView {} }
    MediaEditor { id: mediaEditor }
    function openMedia(id, tab) { mediaEditor.open(id, tab) }
    Component { id: toolsView; ToolsView {} }
    Component { id: configView; ConfigView {} }
    Component { id: logsView; LogsView {} }
    Component { id: helpView; HelpView {} }
}
