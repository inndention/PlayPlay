import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".."

/**
 * Weekly schedule grid: 7 day columns x 24 hour rows, each cell showing the
 * fill percentage and block type colour. Template slots ("D-H") apply every
 * week; dated overrides ("YYYY-MM-DD-H") win for that date.
 */
Item {
    id: grid
    property string channelId: supervisor.activeChannelId
    property var weekStart: startOfWeek(new Date())
    property var slots: ({})            // slotId -> HourSlot
    property string selectedSlotId: ""
    property bool loading: false
    property var mediaDur: ({})
    signal slotSelected(string slotId, var slot, var date, int hour)
    readonly property int rowH: 32
    readonly property int hourColW: 52
    readonly property var dayNames: ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"]

    function startOfWeek(d) { var s = new Date(d.getFullYear(), d.getMonth(), d.getDate()); s.setDate(s.getDate() - s.getDay()); return s }
    function dayDate(i) { var d = new Date(weekStart); d.setDate(d.getDate() + i); return d }
    function iso(d) { return d.getFullYear() + "-" + Theme.pad2(d.getMonth() + 1) + "-" + Theme.pad2(d.getDate()) }
    function slotFor(day, hour) {
        var dated = iso(dayDate(day)) + "-" + hour
        if (slots[dated]) return { id: dated, slot: slots[dated], dated: true }
        var tpl = day + "-" + hour
        if (slots[tpl]) return { id: tpl, slot: slots[tpl], dated: false }
        return { id: tpl, slot: null, dated: false }
    }
    function fillSec(slot) { if (!slot || !slot.items) return 0; var t = 0; for (var i = 0; i < slot.items.length; ++i) { var it = slot.items[i]; var full = it.duration || mediaDur[it.mediaId] || 0; var d = it.timeOut > 0 ? it.timeOut - (it.timeIn || 0) : full - (it.timeIn || 0); t += Math.max(0, d) } return t }
    function loadMedia() { api.get("/media", function(ok, d) { if (!ok) return; var m = {}; var list = d.media || []; for (var i = 0; i < list.length; ++i) m[list[i].id] = list[i].duration || 0; grid.mediaDur = m }) }
    function reload() {
        loading = true
        api.get("/schedule/" + channelId + "/week?start=" + iso(weekStart), function(ok, data) { loading = false; if (ok) grid.slots = data.slots || {} })
    }
    function shiftWeek(n) { var d = new Date(weekStart); d.setDate(d.getDate() + 7 * n); weekStart = d; reload() }
    onChannelIdChanged: { reload() }
    // One Component.onCompleted per object: a second handler on the same item is "Property value set multiple times" and the whole page fails to compile.
    Component.onCompleted: { loadMedia(); reload(); loadHourRecipes() }
    Connections { target: supervisor; function onScheduleChanged(ch, slotId) { if (ch === grid.channelId) grid.reload() } function onMediaScanDone() { grid.loadMedia() } }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        // Header: week navigation
        Rectangle {
            Layout.fillWidth: true; implicitHeight: 40; color: Theme.bg2; radius: Theme.r8
            RowLayout {
                anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 12; spacing: 8
                Text { text: "▦"; color: Theme.accent0; font.pixelSize: Theme.fs14 }
                Text { text: "Weekly Schedule"; color: Theme.text0; font.family: Theme.fontDisplay; font.pixelSize: Theme.fs14; font.bold: true }
                Item { Layout.fillWidth: true }
                CButton { icon2: "‹"; small: true; onClicked: { grid.shiftWeek(-1) } }
                CButton { text: "Today"; small: true; onClicked: { grid.weekStart = grid.startOfWeek(new Date()); grid.reload() } }
                CButton { icon2: "›"; small: true; onClicked: { grid.shiftWeek(1) } }
                Text { text: Qt.formatDate(grid.weekStart, "ddd M/d") + " – " + Qt.formatDate(grid.dayDate(6), "ddd M/d"); color: Theme.accent0; font.pixelSize: Theme.fs12; font.bold: true; Layout.leftMargin: 6 }
                Item { Layout.fillWidth: true }
                CButton { text: "Validate week"; small: true; icon2: "✓"; onClicked: { grid.validateAll() } }
                CButton { text: "Overview"; small: true; variant: "ghost"; active: grid.overview; onClicked: { grid.overview = !grid.overview } }
            }
        }
        // Column headers
        Rectangle {
            Layout.fillWidth: true; implicitHeight: 30; color: Theme.bg1
            Row {
                anchors.fill: parent
                Item { width: grid.hourColW; height: parent.height }
                Repeater {
                    model: 7
                    Item { width: (parent.width - grid.hourColW) / 7; height: parent.height
                        property bool today: grid.iso(grid.dayDate(index)) === grid.iso(new Date())
                        Text { anchors.centerIn: parent; text: grid.dayNames[index] + " " + Qt.formatDate(grid.dayDate(index), "M/d"); color: today ? Theme.accent0 : Theme.text1; font.pixelSize: Theme.fs11; font.bold: true } }
                }
            }
        }
        Flickable {
            Layout.fillWidth: true; Layout.fillHeight: true
            contentHeight: 24 * grid.rowH; clip: true
            ScrollBar.vertical: ScrollBar {}
            Column {
                width: parent.width
                Repeater {
                    model: 24
                    Row {
                        property int hour: index
                        width: parent.width; height: grid.rowH
                        Rectangle { width: grid.hourColW; height: grid.rowH; color: Theme.bg1
                            Text { anchors.centerIn: parent; text: Theme.pad2(hour) + ":00"; color: Theme.text2; font.family: Theme.fontMono; font.pixelSize: Theme.fs10 } }
                        Repeater {
                            model: 7
                            Rectangle {
                                property int day: index
                                property var info: grid.slotFor(day, hour)
                                property real fill: grid.fillSec(info.slot)
                                property real pct: Math.min(1.2, fill / 3600)
                                property bool sel: grid.selectedSlotId === info.id
                                property bool nowCell: grid.iso(grid.dayDate(day)) === grid.iso(new Date()) && new Date().getHours() === hour
                                width: (parent.width - grid.hourColW) / 7; height: grid.rowH
                                color: sel ? Theme.bg5 : cellMa.containsMouse ? Theme.bg4 : (day % 2 ? Theme.bg2 : Theme.bg1)
                                border.color: sel ? Theme.accent0 : nowCell ? Theme.onAir : Theme.border0; border.width: 1
                                Rectangle {
                                    anchors.left: parent.left; anchors.bottom: parent.bottom; anchors.margins: 2
                                    width: (parent.width - 4) * Math.min(1, pct); height: 3; radius: 1
                                    color: pct > 1.01 ? Theme.danger : pct > 0.95 ? Theme.ready : pct > 0 ? Theme.accent0 : "transparent"
                                }
                                RowLayout {
                                    anchors.fill: parent; anchors.margins: 4; spacing: 4
                                    Text { text: info.slot && info.slot.showBlock && info.slot.showBlock.title ? info.slot.showBlock.title : (info.slot && info.slot.items && info.slot.items.length ? info.slot.items[0].title : ""); color: Theme.text1; font.pixelSize: Theme.fs10; elide: Text.ElideRight; Layout.fillWidth: true }
                                    Text { text: info.slot ? Math.round(pct * 100) + "%" : "0%"; color: pct > 1.01 ? Theme.danger : pct > 0 ? Theme.text0 : Theme.text3; font.family: Theme.fontMono; font.pixelSize: Theme.fs10; font.bold: pct > 0 }
                                    Text { visible: info.dated; text: "●"; color: Theme.accent2; font.pixelSize: 8; ToolTip.text: "Dated override" }
                                    Text { visible: !!(info.slot && info.slot.isLooping); text: "↻"; color: Theme.standby; font.pixelSize: Theme.fs10 }
                                }
                                MouseArea { id: cellMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; acceptedButtons: Qt.LeftButton | Qt.RightButton
                                    onClicked: function(m) { grid.selectedSlotId = info.id; grid.slotSelected(info.id, info.slot, grid.dayDate(day), hour); if (m.button === Qt.RightButton) { grid.menuDay = day; grid.menuHour = hour; grid.menuInfo = info; cellMenu.popup() } } }
                            }
                        }
                    }
                }
            }
        }
    }
    property bool overview: false
    property int menuDay: 0
    property int menuHour: 0
    property var menuInfo: ({})
    property var hourRecipes: []
    function loadHourRecipes() { api.get("/programming/recipes", function(ok, d) { if (!ok) return; var l = [{ id: "default", name: (d["default"] && d["default"].name) || "Default" }]; grid.hourRecipes = l.concat(d.recipes || []) }) }
    function tplId() { return menuDay + "-" + menuHour }
    function datedId() { return iso(dayDate(menuDay)) + "-" + menuHour }
    function saveSlotJson(id, body, msg) { api.put("/schedule/" + channelId + "/slot/" + id, body, function(ok) { if (ok) { if (msg) window.toast(msg, "ok"); reload() } }) }
    function copyHourFromMenu() { var sl = menuInfo.slot || { items: [] }; window.setClip("hour", { items: sl.items || [], isLooping: !!sl.isLooping, randomizeAds: sl.randomizeAds !== false, keepProgramOrder: sl.keepProgramOrder !== false, blockType: sl.blockType || "general", showBlock: sl.showBlock || {} }, "hour " + Theme.pad2(menuHour) + ":00") }
    function pasteHourInto(dated) {
        if (!window.clip || window.clip.kind !== "hour") { window.toast("Clipboard has no hour"); return }
        var h = JSON.parse(JSON.stringify(window.clip.data)); var id = dated ? datedId() : tplId()
        var body = { channelId: channelId, hour: menuHour, items: (h.items || []).map(function(it) { if (!it.isLocked) it.id = "p" + Date.now().toString(36) + Math.floor(Math.random() * 10000); return it }), isLooping: !!h.isLooping, randomizeAds: h.randomizeAds !== false, keepProgramOrder: h.keepProgramOrder !== false, blockType: h.blockType || "general" }
        if (dated) { body.date = iso(dayDate(menuDay)); body.dayOfWeek = -1 } else body.dayOfWeek = menuDay
        saveSlotJson(id, body, "Pasted into " + id)
    }
    function copyDayFromMenu() { var hours = {}; for (var h = 0; h < 24; ++h) { var inf = slotFor(menuDay, h); if (inf.slot && inf.slot.items && inf.slot.items.length) hours[h] = inf.slot } window.setClip("day", hours, dayNames[menuDay] + " (" + Object.keys(hours).length + " hours)") }
    function pasteDay(dated) {
        if (!window.clip || window.clip.kind !== "day") { window.toast("Clipboard has no day"); return }
        var d = window.clip.data; var keys = Object.keys(d); var n = 0
        for (var k = 0; k < keys.length; ++k) { var h = parseInt(keys[k]); var sl = JSON.parse(JSON.stringify(d[keys[k]])); var id = dated ? iso(dayDate(menuDay)) + "-" + h : menuDay + "-" + h
            var body = Object.assign({}, sl, { channelId: channelId, hour: h, items: (sl.items || []).map(function(it) { if (!it.isLocked) it.id = "p" + Date.now().toString(36) + Math.floor(Math.random() * 10000); return it }) }); delete body.id
            if (dated) { body.date = iso(dayDate(menuDay)); body.dayOfWeek = -1 } else { body.dayOfWeek = menuDay; delete body.date }
            api.put("/schedule/" + channelId + "/slot/" + id, body, function(ok) { if (ok && ++n === keys.length) { window.toast("Pasted " + n + " hour(s)", "ok"); reload() } }) }
    }
    /// The hour the right-clicked cell shows: its dated override when it has one, else its weekly template. Copy and fill actions work from
    /// (and write to) THIS id; the template id would copy a stale or empty row and leave the visible hour unchanged.
    function srcId() { return menuInfo && menuInfo.id ? menuInfo.id : tplId() }
    function srcDated() { return !!(menuInfo && menuInfo.dated) }
    /// Run a batch of /copy requests and report what really happened: hours written (the route returns `written`), failures, and reload either way.
    function runCopies(bodies, what) {
        var left = bodies.length, written = 0, failed = 0
        if (!left) return
        for (var i = 0; i < bodies.length; ++i) {
            api.post("/schedule/" + channelId + "/copy", bodies[i], function(ok, r) {
                if (ok) written += (r && r.written) || 0; else failed++
                if (--left > 0) return
                if (failed) window.toast(what + ": " + failed + " of " + bodies.length + " copies failed, " + written + " hour(s) written", "error")
                else if (written) window.toast(what + ": " + written + " hour(s) written", "ok")
                else window.toast(what + ": nothing written (the destinations already match)", "info")
                reload()
            })
        }
    }
    function copyHourToAllHours() {
        var bodies = []
        for (var h = 0; h < 24; ++h) {
            if (h === menuHour) continue
            // a dated cell fills the other hours of that date (dated overrides); a template cell fills the weekly template
            bodies.push({ from: { scope: "hour", slotId: srcId() }, to: srcDated() ? { date: iso(dayDate(menuDay)), hour: h } : { dow: menuDay, hour: h } })
        }
        runCopies(bodies, "Copy to every hour of " + dayNames[menuDay] + (srcDated() ? " (this date)" : ""))
    }
    function copyHourToAllDays() {
        var bodies = []
        for (var d = 0; d < 7; ++d) {
            if (d === menuDay && !srcDated()) continue      // a template is already its own weekday's template; a dated hour is promoted to it
            bodies.push({ from: { scope: "hour", slotId: srcId() }, to: { dow: d } })
        }
        runCopies(bodies, "Copy to every day (template)")
    }
    function layRecipe(recipeId, dated) { api.post("/schedule/" + channelId + "/hour-recipe", { recipeId: recipeId, scope: "hour", slotId: dated ? datedId() : tplId(), keepLocks: true, apply: true }, function(ok) { if (ok) { window.toast("Recipe laid into " + (dated ? datedId() : tplId()), "ok"); reload() } }) }
    Menu {
        id: cellMenu
        MenuItem { text: "Edit hour"; onTriggered: { grid.slotSelected(grid.menuInfo.id, grid.menuInfo.slot, grid.dayDate(grid.menuDay), grid.menuHour) } }
        MenuSeparator {}
        Menu { id: recipeTplMenu; title: "Fill with recipe (template)"
            Instantiator { model: grid.hourRecipes; delegate: MenuItem { text: modelData.name; onTriggered: { grid.layRecipe(modelData.id, false) } } onObjectAdded: function(i, o) { recipeTplMenu.insertItem(i, o) } onObjectRemoved: function(i, o) { recipeTplMenu.removeItem(o) } } }
        Menu { id: recipeDatedMenu; title: "Fill with recipe (this date only)"
            Instantiator { model: grid.hourRecipes; delegate: MenuItem { text: modelData.name; onTriggered: { grid.layRecipe(modelData.id, true) } } onObjectAdded: function(i, o) { recipeDatedMenu.insertItem(i, o) } onObjectRemoved: function(i, o) { recipeDatedMenu.removeItem(o) } } }
        MenuItem { text: "Weighted fill (format clock)"; onTriggered: { api.post("/schedule/" + grid.channelId + "/weighted-fill", { scope: "hour", slotId: grid.srcId(), keepLocks: true, apply: true }, function(ok, r) { if (ok) { window.toast(((r.summary && r.summary.placed) || 0) + " items placed into " + grid.srcId(), "ok"); grid.reload() } }) } }
        MenuItem { text: "Shuffle ads"; enabled: !!(grid.menuInfo.slot && grid.menuInfo.slot.items && grid.menuInfo.slot.items.length); onTriggered: { api.post("/schedule/" + grid.channelId + "/slot/" + grid.menuInfo.id + "/shuffle", {}, function(ok) { if (ok) { window.toast("Shuffled", "ok"); grid.reload() } }) } }
        MenuItem { text: "Validate"; onTriggered: { api.post("/schedule/" + grid.channelId + "/slot/" + grid.menuInfo.id + "/validate", {}, function(ok, d) { if (ok) window.toast(d.issues && d.issues.length ? d.issues.length + " issue(s): " + d.issues.map(function(i) { return i.code }).join(", ") : "Hour validates clean", d.issues && d.issues.length ? "error" : "ok") }) } }
        MenuSeparator {}
        MenuItem { text: "Copy hour"; onTriggered: { grid.copyHourFromMenu() } }
        MenuItem { text: "Paste hour (template)"; enabled: !!(window.clip && window.clip.kind === "hour"); onTriggered: { grid.pasteHourInto(false) } }
        MenuItem { text: "Paste hour (this date only)"; enabled: !!(window.clip && window.clip.kind === "hour"); onTriggered: { grid.pasteHourInto(true) } }
        MenuItem { text: "Copy day"; onTriggered: { grid.copyDayFromMenu() } }
        MenuItem { text: "Paste day (template)"; enabled: !!(window.clip && window.clip.kind === "day"); onTriggered: { grid.pasteDay(false) } }
        MenuItem { text: "Paste day (dated)"; enabled: !!(window.clip && window.clip.kind === "day"); onTriggered: { grid.pasteDay(true) } }
        MenuSeparator {}
        MenuItem { text: "Copy hour to all days (template)"; onTriggered: { grid.copyHourToAllDays() } }
        MenuItem { text: grid.srcDated() ? "Copy hour to every hour of this date" : "Copy hour to every hour of this day"; onTriggered: { grid.copyHourToAllHours() } }
        MenuItem { text: "Copy this day to next week (dated)"; onTriggered: { var nd = grid.dayDate(grid.menuDay); nd.setDate(nd.getDate() + 7); api.post("/schedule/" + grid.channelId + "/copy", { from: { scope: "day", dow: grid.menuDay }, to: { date: grid.iso(nd), dated: true } }, function(ok, r) { if (ok) window.toast(r.written + " hour(s) written", "ok") }) } }
        MenuSeparator {}
        MenuItem { text: "Clear hour (keep locks)"; onTriggered: { api.post("/schedule/" + grid.channelId + "/clear", { scope: "hour", slotId: grid.menuInfo.id, keepLocks: true }, function(ok) { if (ok) grid.reload() }) } }
        MenuItem { text: "Delete hour"; onTriggered: { api.del("/schedule/" + grid.channelId + "/slot/" + grid.menuInfo.id, function(ok) { if (ok) { window.toast("Deleted " + grid.menuInfo.id, "ok"); grid.reload() } }) } }
        MenuSeparator {}
        MenuItem { text: "Properties (hour flags, block type)"; onTriggered: { grid.slotSelected(grid.menuInfo.id, grid.menuInfo.slot, grid.dayDate(grid.menuDay), grid.menuHour) } }
    }
    function validateAll() {
        var ids = Object.keys(slots); var bad = 0; var done = 0
        if (!ids.length) { window.toast("Nothing scheduled this week"); return }
        for (var i = 0; i < ids.length; ++i) {
            api.post("/schedule/" + channelId + "/slot/" + ids[i] + "/validate", {}, function(ok, data) {
                done++; if (ok && data.issues && data.issues.length) bad++
                if (done === ids.length) window.toast(bad ? bad + " slot(s) with issues (open a slot for details)" : "All " + ids.length + " slots validate clean", bad ? "error" : "ok")
            })
        }
    }
}
