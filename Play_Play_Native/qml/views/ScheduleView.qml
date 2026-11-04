import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".."
import "../ui"

/// Schedule: media library | weekly grid, with the hour editor sliding in below the grid.
Item {
    id: view
    function openScheduleHour(channelId, slotId, hourStartMs) {
        var d = new Date(Number(hourStartMs)), parts = String(slotId).split("-")
        var hour = Number(parts[parts.length - 1])
        if (isNaN(d.getTime()) || hour < 0 || hour > 23) { window.toast("Invalid schedule hour", "error"); return }
        grid.weekStart = grid.startOfWeek(d)
        grid.selectedSlotId = slotId
        grid.reload()
        api.get("/schedule/" + encodeURIComponent(channelId) + "/slot/" + encodeURIComponent(slotId), function(ok, slot) {
            if (!ok || !slot || !slot.exists) { window.toast("The schedule hour is no longer available", "error"); return }
            if (supervisor.activeChannelId !== channelId) return
            editor.load(slotId, slot, d, hour)
            view.editorOpen = true
        })
    }
    RowLayout {
        anchors.fill: parent
        spacing: 10
        MediaLibraryPanel {
            id: library
            Layout.preferredWidth: 340
            Layout.fillHeight: true
            onPicked: function(m) { editor.addMedia(m); editorOpen = true }
            onPickedFolder: function(m) { editor.addFolderBlockFor(m); editorOpen = true }
            onTake: function(m) { supervisor.take(m.path, m.title, m.duration); window.toast("Take: " + m.title, "ok") }
        }
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 10
            Rectangle {
                Layout.fillWidth: true; implicitHeight: 36; color: Theme.bg2; radius: Theme.r8; border.color: Theme.border0
                RowLayout {
                    anchors.fill: parent; anchors.leftMargin: 10; anchors.rightMargin: 10; spacing: 8
                    // perf-app PA-3: the channel model updates its rows in place (a JS array rebuilt these buttons on every 2 s snapshot), and
                    // activeExecutor / activeProfile notify only when that part of the active channel changes.
                    Repeater {
                        model: supervisor.channelModel
                        CButton { text: modelData.profile.name; small: true; active: modelData.profile.id === supervisor.activeChannelId; variant: "ghost"; onClicked: { supervisor.activeChannelId = modelData.profile.id } }
                    }
                    Item { Layout.fillWidth: true }
                    property var ex: supervisor.activeExecutor
                    CBadge { text: parent.ex.armed ? "AUTO PUSH ON" : "AUTO PUSH OFF"; tint: parent.ex.armed ? Theme.ready : Theme.text3 }
                    CButton { text: "Rules"; small: true; icon2: "⚙"; variant: "ghost"; active: view.rulesOpen; onClicked: { view.rulesOpen = !view.rulesOpen } ToolTip.text: "Channel scheduling rules: gap fill, IDs, ads, strict timing" }
                    CButton { text: "Load hour now"; small: true; icon2: "⤓"; onClicked: { supervisor.loadHour() } }
                }
            }
            Rectangle {
                visible: view.rulesOpen
                Layout.fillWidth: true; implicitHeight: 86; color: Theme.bg2; radius: Theme.r8; border.color: Theme.border0
                property var prof: supervisor.activeProfile   // only on a profile change: MAX FILL no longer resets every 2 s while typed into
                property var fill: prof.fill || ({})
                function setRule(k, v) { var f = Object.assign({}, fill); f[k] = v; api.put("/channels/" + supervisor.activeChannelId, { fill: f }, function(ok) { if (ok) { window.toast("Rule saved", "ok"); supervisor.refresh() } }) }
                function setProf(k, v) { var o = {}; o[k] = v; api.put("/channels/" + supervisor.activeChannelId, o, function(ok) { if (ok) { window.toast("Rule saved", "ok"); supervisor.refresh() } }) }
                Flow {
                    id: rulesFlow
                    anchors.fill: parent; anchors.margins: 10; spacing: 14
                    // CToggle flips itself on a click, which kills a `checked:` binding: after the first click the switch would stop following the channel
                    // (switch to another channel and it still showed the old one's rule). A Binding element survives that, so each switch has one.
                    CToggle { id: tgIds; label: "Fill gaps with IDs"; implicitWidth: 170; onToggled: function(v) { rulesFlow.parent.setRule("fillGapsWithIds", v) } }
                    Binding { target: tgIds; property: "checked"; value: rulesFlow.parent.fill.fillGapsWithIds !== false }
                    CToggle { id: tgBumpers; label: "Fill with bumpers"; implicitWidth: 160; onToggled: function(v) { rulesFlow.parent.setRule("fillGapsWithBumpers", v) } }
                    Binding { target: tgBumpers; property: "checked"; value: rulesFlow.parent.fill.fillGapsWithBumpers !== false }
                    CToggle { id: tgFiller; label: "Fill with filler / PSA"; implicitWidth: 170; onToggled: function(v) { rulesFlow.parent.setRule("fillWithFiller", v) } }
                    Binding { target: tgFiller; property: "checked"; value: rulesFlow.parent.fill.fillWithFiller !== false }
                    CToggle { id: tgIdTop; label: "ID at top of hour"; implicitWidth: 160; onToggled: function(v) { rulesFlow.parent.setRule("idAtTopOfHour", v) } }
                    Binding { target: tgIdTop; property: "checked"; value: rulesFlow.parent.fill.idAtTopOfHour !== false }
                    CToggle { id: tgShuffle; label: "Shuffle ads in hour"; implicitWidth: 160; onToggled: function(v) { rulesFlow.parent.setRule("shuffleAdsInHour", v) } }
                    Binding { target: tgShuffle; property: "checked"; value: rulesFlow.parent.fill.shuffleAdsInHour !== false }
                    CToggle { id: tgLatest; label: "Latest episode for series"; implicitWidth: 190; onToggled: function(v) { rulesFlow.parent.setRule("alwaysPlayLatestEpisode", v) } }
                    Binding { target: tgLatest; property: "checked"; value: rulesFlow.parent.fill.alwaysPlayLatestEpisode !== false }
                    CToggle { id: tgStrict; label: "Strict wall-clock"; implicitWidth: 150; onToggled: function(v) { rulesFlow.parent.setProf("strict", v) } }
                    Binding { target: tgStrict; property: "checked"; value: !!rulesFlow.parent.prof.strict }
                    RowLayout { spacing: 6
                        Text { text: "MAX FILL (s)"; color: Theme.text2; font.pixelSize: Theme.fs10; font.bold: true }
                        TextField { implicitWidth: 70; implicitHeight: 26; text: String(rulesFlow.parent.fill.maxFillSec !== undefined ? rulesFlow.parent.fill.maxFillSec : 60); color: Theme.text0; font.pixelSize: Theme.fs12; background: Rectangle { color: Theme.bg4; radius: Theme.r5; border.color: Theme.border1 }
                            onEditingFinished: { rulesFlow.parent.setRule("maxFillSec", parseFloat(text) || 60) } } }
                }
            }
            WeeklyGrid {
                id: grid
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumHeight: 180      // the editor may ask for a lot (wrapping rows, live / block / resolved panels): it gives way, not the grid
                onSlotSelected: function(id, slot, date, hour) { editor.load(id, slot, date, hour); view.editorOpen = true }
            }
            SlotEditor {
                id: editor
                Layout.fillWidth: true
                // 360 px normally; more when the editor's own rows wrap (narrow window) or its live / block / resolved panels are open. A fixed
                // 360 left 88 px for the item list with the Resolved panel open and 40 px with the block row as well.
                Layout.preferredHeight: view.editorOpen ? Math.max(360, editor.wantHeight) : 0
                visible: view.editorOpen
                Behavior on Layout.preferredHeight { NumberAnimation { duration: 120 } }
                onSaved: { grid.reload() }
                onClosed: { view.editorOpen = false }
            }
        }
    }
    property bool editorOpen: false
    property bool rulesOpen: false
    Shortcut { sequence: "Esc"; onActivated: { view.editorOpen = false } }
}
