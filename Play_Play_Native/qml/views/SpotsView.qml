import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".."
import "../ui"

/**
 * Spots: auto-generated spots (recipes → script → voice → card → MP4 → library), the recurring jobs
 * that pin GEN markers into the weekly grid, generate-now with a script preview, the news scatter and
 * the writer / voice / render configuration. Talks to /api/v1/spots/* and follows the "spots" uiEvent.
 * The web dashboard's Spots page has the full per-placement editor; this view edits the first placement.
 */
Item {
    id: view
    property var jobs: []
    property var markers: ({})
    property var recipes: []
    property var status: ({})
    property var cfg: ({})
    property var scatter: ({})
    property string tab: "jobs"
    property var edit: null
    property string genRecipe: "weather"
    property string genChannel: supervisor.activeChannelId || ""
    property string genVoice: ""
    property string genMarket: ""                   // "" = the station default market, "none" = generic, else a market key
    property var preview: null
    property bool previewBusy: false
    property string previewId: ""                   // a script preview being polled (preview-script is queued server-side)
    property int previewTries: 0
    property bool genBusy: false
    property string genBusyId: ""                   // the generation behind the Generate button
    property var pendingGens: ({})                  // genId -> {kind, since}: renders queued from here, settled from the "spots" status (recent[])
    property var current: status.current || null
    readonly property var dows: ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"]

    function reload() {
        api.get("/spots/jobs", function(ok, d) { if (ok) { view.jobs = d.jobs || []; view.markers = d.markers || {} } })
        api.get("/spots/recipes", function(ok, d) { if (ok) view.recipes = d.recipes || [] })
        api.get("/spots/status", function(ok, d) { if (ok) { view.status = d; view.scatter = d.scatter || {} } })
        api.get("/spots/config", function(ok, d) { if (ok) view.cfg = d })
    }
    function channels() { return supervisor.channels.map(function(c) { return c.profile.id }) }
    function recipeKeys() { return view.recipes.map(function(r) { return r.key }) }
    function recipeLabels() { return view.recipes.map(function(r) { return r.label + " (" + r.key + ")" }) }
    // Area markets (Martinsville / Reidsville / Danville): the pickers list what GET /spots/markets returns inside the config
    function marketsAll() { return (view.cfg.market && view.cfg.market.markets) || [] }
    function marketByKey(k) { var a = view.marketsAll(); for (var i = 0; i < a.length; ++i) if (a[i].key === k) return a[i]; return null }
    function stationMarketKey() { return (view.cfg.market && view.cfg.market.effectiveDefault) || "" }
    function marketKeys() { return ["", "none"].concat(view.marketsAll().map(function(m) { return m.key })) }               // per job / one-off: inherit, generic, then each market
    function marketLabels() { var d = view.marketByKey(view.stationMarketKey()); return ["Station default (" + (d ? d.label : "generic") + ")", "Generic (no market)"].concat(view.marketsAll().map(function(m) { return m.label })) }
    function stationKeys() { return [""].concat(view.marketsAll().map(function(m) { return m.key })) }                       // the station default: none, then each market
    function stationLabels() { return ["Generic (no market)"].concat(view.marketsAll().map(function(m) { return m.label })) }
    function marketBlurb(m) {
        if (!m) return "No market: recipes keep their generic Martinsville-flavoured defaults and the Weather place / coordinates below."
        var s = m.sources || {}
        return m.label + " — " + m.area + " · " + (m.region || "") + "\nTowns: " + (m.towns || []).join(", ") + "\nVoice: " + (m.voiceStyle || "") + "\nSources: " + ((s.feeds || []).length) + " news feed(s) · schools " + (s.schools || []).join(", ") + " · traffic " + (s.traffic || "?") + (s.trafficConfigured === false ? " (not set up)" : "") + " · cinema " + (s.cinema === "" ? "none" : "set") + " · realty " + (s.realty === "" ? "none" : "set")
    }
    function markColor(s) { return s === "fresh" ? Theme.ready : s === "due" ? Theme.warn : s === "stale" ? Theme.danger : Theme.text3 }
    function when(ms) { if (!ms) return "—"; var d = new Date(ms); var t = d.toLocaleTimeString(Qt.locale(), "h:mm AP"); return d.toDateString() === new Date().toDateString() ? t : view.dows[d.getDay()] + " " + t }
    function ago(ms) { if (!ms) return "never"; var m = Math.round((Date.now() - ms) / 60000); return m < 1 ? "just now" : m < 60 ? m + " min ago" : m < 1440 ? Math.round(m / 60) + " h ago" : Math.round(m / 1440) + " d ago" }
    function cadence(j) { var e = j.every || {}; return e.kind === "clock" ? "at " + (e.times || []).join(", ") : "every " + (e.minutes || 60) + " min" }
    function dur(s) { s = Math.round(s || 0); return Math.floor(s / 60) + ":" + (s % 60 < 10 ? "0" : "") + (s % 60) }
    function cfgSet(patch) { api.put("/spots/config", patch, function(ok, d) { if (ok) view.cfg = d; else window.toast("Config not saved", "error") }) }
    function openEditor(j) {
        var ch = view.channels().length ? view.channels()[0] : ""
        view.edit = j ? JSON.parse(JSON.stringify(j)) : { recipe: view.recipes.length ? view.recipes[0].key : "weather", label: "", channel: ch, enabled: false, every: { kind: "interval", minutes: 60, times: [] }, dows: [], voice: "", data: "", feeds: [], targetSec: 0, placements: [{ channel: ch, hours: "all", atMin: 5, dows: [] }], market: "" }
        if (!view.edit.every) view.edit.every = { kind: "interval", minutes: 60, times: [] }
        if (!view.edit.placements || !view.edit.placements.length) view.edit.placements = [{ channel: view.edit.channel || ch, hours: "all", atMin: 5, dows: [] }]
    }
    function saveEditor() {
        var j = view.edit; var body = { recipe: j.recipe, label: j.label, channel: j.channel, enabled: j.enabled, every: j.every, dows: j.dows, voice: j.voice, data: j.data, feeds: j.feeds, targetSec: j.targetSec, placements: j.placements, market: j.market || "" }
        var cb = function(ok, d) { if (ok && !(d && d.error)) { view.edit = null; window.toast("Job saved; markers refreshed", "ok"); view.reload() } else window.toast(d && d.error ? d.error : "save failed", "error") }
        if (j.id) api.put("/spots/jobs/" + j.id, body, cb); else api.post("/spots/jobs", body, cb)
    }
    // A render takes minutes and the app's REST client aborts a request after 8 s without data, so renders are queued with {wait: false}
    // (202 + genId) and settled from the "spots" status instead of holding one request open for the whole render.
    function trackGen(genId, kind) { var p = view.pendingGens; p[genId] = { kind: kind, since: Date.now() }; view.pendingGens = p; genPoll.restart() }
    function settleGens(st) {
        var p = view.pendingGens; var ids = Object.keys(p); if (!ids.length || !st) return
        var recent = st.recent || []; var live = ({}); var q = st.queue || []
        if (st.current) live[st.current.id] = true
        for (var k = 0; k < q.length; ++k) live[q[k].id] = true
        var settled = false
        for (var n = 0; n < ids.length; ++n) {
            var id = ids[n]; var hit = null
            for (var i = 0; i < recent.length; ++i) { if (recent[i].genId === id || recent[i].id === id) { hit = recent[i]; break } }
            if (!hit && (live[id] || Date.now() - p[id].since < 5000)) continue
            var kind = p[id].kind; delete p[id]; settled = true
            if (id === view.genBusyId) { view.genBusyId = ""; view.genBusy = false }
            if (hit && hit.ok) window.toast("Spot ready: " + hit.headline + (kind === "generate" ? " (" + view.dur(hit.durationSec) + ")" : ""), "ok")
            else if (hit && hit.step === "skipped") window.toast("Spot skipped: " + (hit.error || "nothing to report"), "ok")
            else window.toast("Spot failed: " + (hit && hit.error ? hit.error : "cancelled or lost"), "error")
        }
        view.pendingGens = p
        if (settled) view.reload()
    }
    function runJob(id) {
        api.post("/spots/jobs/" + id + "/run", { wait: false }, function(ok, r) {
            if (!ok || !r || !r.genId) { window.toast("Spot failed: " + (r && r.error ? r.error : "could not queue"), "error"); return }
            window.toast(r.already ? "Already generating this job…" : "Generating… (a minute or two)", "ok"); view.trackGen(r.genId, "job"); view.reload()
        })
    }
    function previewScript() {
        view.previewBusy = true; view.preview = null; view.previewId = ""
        api.post("/spots/preview-script", { recipe: view.genRecipe, channel: view.genChannel, data: genData.text, market: view.genMarket, wait: false }, function(ok, p) {
            if (!ok || !p || !p.previewId) { view.previewBusy = false; view.preview = { error: p && p.error ? p.error : "preview failed" }; return }
            view.previewTries = 0; view.previewId = p.previewId; previewPoll.restart()
        })
    }
    function generateNow() {
        view.genBusy = true
        api.post("/spots/generate", { recipe: view.genRecipe, channel: view.genChannel, data: genData.text, voice: view.genVoice, market: view.genMarket, wait: false }, function(ok, r) {
            if (!ok || !r || !r.genId) { view.genBusy = false; window.toast("Spot failed: " + (r && r.error ? r.error : "could not queue"), "error"); return }
            view.genBusyId = r.genId; window.toast("Generating… (a minute or two)", "ok"); view.trackGen(r.genId, "generate"); view.reload()
        })
    }
    Component.onCompleted: reload()
    Connections {
        target: supervisor
        function onUiEvent(type, payload) { if (type === "spots") { view.status = payload; view.markers = payload.markers || view.markers; view.scatter = payload.scatter || view.scatter; view.settleGens(payload); if (!view.edit) api.get("/spots/jobs", function(ok, d) { if (ok) view.jobs = d.jobs || [] }) } }
    }
    // safety net for a missed WS event: while renders are pending, read the status every 3 s
    Timer {
        id: genPoll; interval: 3000; repeat: true
        onTriggered: {
            if (!Object.keys(view.pendingGens).length) { stop(); return }
            api.get("/spots/status", function(ok, d) { if (ok) { view.status = d; view.scatter = d.scatter || view.scatter; view.settleGens(d) } })
        }
    }
    Timer {
        id: previewPoll; interval: 1200; repeat: true
        onTriggered: {
            if (!view.previewId) { stop(); return }
            var id = view.previewId; view.previewTries += 1
            if (view.previewTries > 600) { view.previewId = ""; view.previewBusy = false; view.preview = { error: "preview timed out" }; stop(); return }
            api.get("/spots/preview-script/" + id, function(ok, p) {
                if (view.previewId !== id) return
                if (ok && p && p.pending) return
                view.previewId = ""; view.previewBusy = false; previewPoll.stop()
                view.preview = ok && p ? p : { error: p && p.error ? p.error : "preview failed" }
            })
        }
    }

    ColumnLayout {
        anchors.fill: parent; spacing: 10
        CPanel {
            Layout.fillWidth: true; Layout.fillHeight: true; padding: 0
            title: "Generated spots — " + view.jobs.length + " job(s)"
            headerRight: [
                CBadge { text: view.current ? "rendering " + view.current.recipe + " · " + view.current.step : "idle"; tint: view.current ? Theme.accent0 : Theme.text3 },
                CBadge { visible: (view.status.queue || []).length > 0; text: (view.status.queue || []).length + " queued"; tint: Theme.warn },
                CButton { text: "⟳"; small: true; variant: "ghost"; onClicked: view.reload() }
            ]
            ColumnLayout {
                anchors.fill: parent; spacing: 0
                RowLayout {
                    Layout.fillWidth: true; Layout.margins: 8; spacing: 6
                    Repeater { model: ["jobs", "generate", "scatter", "config"]; CButton { text: modelData; small: true; variant: "ghost"; active: view.tab === modelData; onClicked: { view.tab = modelData } } }
                    Item { Layout.fillWidth: true }
                    Rectangle { visible: !!view.current; Layout.preferredWidth: 160; height: 8; radius: 2; color: Theme.bg4
                        Rectangle { height: parent.height; radius: 2; width: parent.width * ((view.current ? view.current.pct : 0) / 100); color: Theme.accent0 } }
                    Text { visible: !!view.current; text: view.current ? (view.current.headline || view.current.recipe) + " · " + view.current.step : ""; color: Theme.text2; font.pixelSize: Theme.fs10; elide: Text.ElideRight; Layout.maximumWidth: 320 }
                    CButton { visible: !!view.current; text: "✕"; small: true; variant: "ghost"; onClicked: { api.del("/spots/generation/" + view.current.id, function() { view.reload() }) } }
                }
                Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border0 }

                // ---- jobs ------------------------------------------------------------------------------------
                ColumnLayout {
                    visible: view.tab === "jobs"; Layout.fillWidth: true; Layout.fillHeight: true; spacing: 0
                    ListView {
                        Layout.fillWidth: true; Layout.fillHeight: true; clip: true; model: view.jobs
                        delegate: Rectangle {
                            width: ListView.view.width; height: 50; color: index % 2 ? Theme.bg2 : Theme.bg3
                            property var j: modelData
                            property var mk: view.markers[j.id] || ({})
                            RowLayout {
                                anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 8; spacing: 10
                                CToggle { label: ""; checked: !!j.enabled; implicitWidth: 40; onToggled: function(v) { api.put("/spots/jobs/" + j.id, { enabled: v }, function() { view.reload() }) } }
                                Text { text: "✦"; color: view.markColor(mk.state); font.pixelSize: Theme.fs16; Layout.preferredWidth: 18; ToolTip.visible: hov.containsMouse; ToolTip.text: (mk.state || "?") + (mk.reason ? " — " + mk.reason : ""); MouseArea { id: hov; anchors.fill: parent; hoverEnabled: true } }
                                Text { text: "GEN " + (j.genNum || "?"); color: Theme.accent0; font.bold: true; font.family: Theme.fontMono; font.pixelSize: Theme.fs11; Layout.preferredWidth: 54 }
                                ColumnLayout { Layout.preferredWidth: 220; spacing: 1
                                    Text { text: j.label || j.recipe; color: Theme.text0; font.pixelSize: Theme.fs12; font.bold: true; elide: Text.ElideRight; Layout.fillWidth: true }
                                    Text { text: j.recipe + " · " + (j.channel || "any channel") + " · " + view.cadence(j) + " · " + (mk.slotsPerWeek || 0) + " slots/wk"; color: Theme.text3; font.pixelSize: Theme.fs10; elide: Text.ElideRight; Layout.fillWidth: true } }
                                ColumnLayout { Layout.preferredWidth: 110; spacing: 1
                                    Text { text: j.enabled ? (j.due ? "due now" : view.when(j.nextRunMs)) : "—"; color: j.due ? Theme.warn : Theme.text2; font.pixelSize: Theme.fs11; font.family: Theme.fontMono }
                                    Text { text: "last " + view.ago(j.lastRunMs); color: Theme.text3; font.pixelSize: Theme.fs10 } }
                                ColumnLayout { Layout.fillWidth: true; spacing: 1
                                    Text { text: j.lastMediaId ? (j.lastHeadline || "open") : "no spot yet"; color: j.lastMediaId ? Theme.accent0 : Theme.text3; font.pixelSize: Theme.fs11; elide: Text.ElideRight; Layout.fillWidth: true
                                        MouseArea { anchors.fill: parent; cursorShape: j.lastMediaId ? Qt.PointingHandCursor : Qt.ArrowCursor; onClicked: { if (j.lastMediaId) window.openMedia(j.lastMediaId) } } }
                                    Text { visible: !!j.lastError; text: "⚠ " + (j.lastError || ""); color: Theme.danger; font.pixelSize: Theme.fs10; elide: Text.ElideRight; Layout.fillWidth: true } }
                                CButton { text: "▶ Run now"; small: true; onClicked: view.runJob(j.id) }
                                CButton { text: "✎"; small: true; variant: "ghost"; onClicked: view.openEditor(j) }
                                CButton { text: "✕"; small: true; variant: "ghost"; onClicked: { api.del("/spots/jobs/" + j.id, function() { view.reload() }) } }
                            }
                        }
                        Text { anchors.centerIn: parent; visible: !view.jobs.length; color: Theme.text3; font.pixelSize: Theme.fs11; text: "No jobs: add one (a recipe, a cadence, and the hours its GEN marker pins into the grid)" }
                    }
                    Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border0 }
                    RowLayout {
                        Layout.fillWidth: true; Layout.margins: 8; spacing: 6
                        CButton { text: "+ job"; small: true; onClicked: view.openEditor(null) }
                        CButton { text: "Enforce markers"; small: true; variant: "ghost"; onClicked: { api.post("/spots/markers/enforce", {}, function(ok, r) { if (ok) { window.toast("Markers: " + r.placed + " placed, " + r.updated + " updated, " + r.removed + " removed", "ok"); view.reload() } }) } }
                        CButton { text: "Sweep expired"; small: true; variant: "ghost"; onClicked: { api.post("/spots/sweep", {}, function(ok, r) { if (ok) window.toast(r.changed + " spot(s) swept", "ok") }) } }
                        Item { Layout.fillWidth: true }
                        Text { text: "✦ fresh · due · stale · off"; color: Theme.text3; font.pixelSize: Theme.fs10 }
                    }
                    // editor (first placement only; the web page edits every placement)
                    Rectangle {
                        visible: !!view.edit; Layout.fillWidth: true; Layout.preferredHeight: 352; color: Theme.bg2; border.color: Theme.border0
                        GridLayout {
                            anchors.fill: parent; anchors.margins: 10; columns: 4; columnSpacing: 10; rowSpacing: 6
                            CSelect { label: "Recipe"; Layout.preferredWidth: 200; model: view.recipeLabels(); currentIndex: Math.max(0, view.recipeKeys().indexOf(view.edit ? view.edit.recipe : "")); onActivated: function(i) { if (view.edit) view.edit.recipe = view.recipeKeys()[i] } }
                            CField { label: "Label"; Layout.preferredWidth: 200; text: view.edit ? (view.edit.label || "") : ""; onEdited: function(v) { if (view.edit) view.edit.label = v } }
                            CSelect { label: "Channel"; Layout.preferredWidth: 140; model: view.channels(); currentIndex: Math.max(0, view.channels().indexOf(view.edit ? view.edit.channel : "")); onActivated: function(i) { if (view.edit) { view.edit.channel = view.channels()[i]; view.edit.placements[0].channel = view.channels()[i] } } }
                            CField { label: "Voice"; Layout.preferredWidth: 160; placeholder: "recipe default"; text: view.edit ? (view.edit.voice || "") : ""; onEdited: function(v) { if (view.edit) view.edit.voice = v } }
                            CSelect { label: "Made"; Layout.preferredWidth: 200; model: ["every N minutes", "at clock times"]; currentIndex: view.edit && view.edit.every.kind === "clock" ? 1 : 0; onActivated: function(i) { if (view.edit) view.edit.every.kind = i === 1 ? "clock" : "interval" } }
                            CField { label: "Minutes / times (06:05, 12:05)"; Layout.preferredWidth: 200; mono: true; text: view.edit ? (view.edit.every.kind === "clock" ? (view.edit.every.times || []).join(", ") : String(view.edit.every.minutes || 60)) : ""; onEdited: function(v) { if (!view.edit) return; if (view.edit.every.kind === "clock") view.edit.every.times = v.split(",").map(function(s) { return s.trim() }).filter(function(s) { return s.length }); else view.edit.every.minutes = parseInt(v) || 60 } }
                            CField { label: "Marker hours (all or 5,9,13)"; Layout.preferredWidth: 140; mono: true; text: view.edit ? (Array.isArray(view.edit.placements[0].hours) ? view.edit.placements[0].hours.join(",") : "all") : ""; onEdited: function(v) { if (!view.edit) return; var t = v.trim(); view.edit.placements[0].hours = (t === "all" || !t) ? "all" : t.split(",").map(function(x) { return parseInt(x) }).filter(function(x) { return x >= 0 && x <= 23 }) } }
                            CField { label: "At minute"; Layout.preferredWidth: 160; mono: true; text: view.edit ? String(view.edit.placements[0].atMin || 0) : ""; onEdited: function(v) { if (view.edit) view.edit.placements[0].atMin = parseInt(v) || 0 } }
                            CSelect { label: "Market"; Layout.preferredWidth: 200; model: view.marketLabels(); currentIndex: Math.max(0, view.marketKeys().indexOf(view.edit ? (view.edit.market || "") : "")); onActivated: function(i) { if (view.edit) view.edit.market = view.marketKeys()[i] } }
                            Text { Layout.columnSpan: 3; Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.text3; font.pixelSize: Theme.fs10
                                text: { var k = view.edit ? (view.edit.market || "") : ""; var eff = k === "none" ? "" : (k || view.stationMarketKey()); var m = view.marketByKey(eff); return m ? "Spots are localized to " + m.label + ": place names, feeds, schools, traffic centre, weather point and wording." : "Generic: the recipe's own defaults." } }
                            ColumnLayout { Layout.columnSpan: 2; Layout.fillWidth: true; spacing: 4
                                Text { text: "TOPIC / MATERIAL (free text; replaces the feed)"; color: Theme.text2; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1 }
                                Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 86; color: Theme.bg4; radius: Theme.r5; border.color: Theme.border1
                                    Flickable { anchors.fill: parent; anchors.margins: 4; clip: true; contentHeight: editData.implicitHeight
                                        TextEdit { id: editData; width: parent.width; color: Theme.text0; font.pixelSize: Theme.fs12; wrapMode: TextEdit.Wrap; selectByMouse: true; text: view.edit ? (view.edit.data || "") : ""; onTextChanged: { if (view.edit) view.edit.data = text } } } } }
                            ColumnLayout { Layout.columnSpan: 2; Layout.fillWidth: true; spacing: 4
                                Text { text: "FEEDS (one URL per line; empty = recipe defaults)"; color: Theme.text2; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1 }
                                Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 86; color: Theme.bg4; radius: Theme.r5; border.color: Theme.border1
                                    Flickable { anchors.fill: parent; anchors.margins: 4; clip: true; contentHeight: editFeeds.implicitHeight
                                        TextEdit { id: editFeeds; width: parent.width; color: Theme.text0; font.pixelSize: Theme.fs11; font.family: Theme.fontMono; wrapMode: TextEdit.Wrap; selectByMouse: true; text: view.edit ? (view.edit.feeds || []).join("\n") : ""; onTextChanged: { if (view.edit) view.edit.feeds = text.split("\n").map(function(s) { return s.trim() }).filter(function(s) { return s.indexOf("http") === 0 }) } } } } }
                            RowLayout { Layout.columnSpan: 4; spacing: 6
                                CToggle { label: "enabled"; checked: view.edit ? !!view.edit.enabled : false; implicitWidth: 120; onToggled: function(v) { if (view.edit) view.edit.enabled = v } }
                                Item { Layout.fillWidth: true }
                                CButton { text: "SAVE"; variant: "primary"; onClicked: view.saveEditor() }
                                CButton { text: "cancel"; variant: "ghost"; onClicked: { view.edit = null } } }
                        }
                    }
                }

                // ---- generate now ------------------------------------------------------------------------------
                RowLayout {
                    visible: view.tab === "generate"; Layout.fillWidth: true; Layout.fillHeight: true; Layout.margins: 10; spacing: 12
                    ColumnLayout {
                        Layout.preferredWidth: 420; Layout.fillHeight: true; spacing: 8
                        CSelect { label: "Recipe"; Layout.fillWidth: true; model: view.recipeLabels(); currentIndex: Math.max(0, view.recipeKeys().indexOf(view.genRecipe)); onActivated: function(i) { view.genRecipe = view.recipeKeys()[i]; view.preview = null } }
                        CSelect { label: "Channel"; Layout.fillWidth: true; model: view.channels(); currentIndex: Math.max(0, view.channels().indexOf(view.genChannel)); onActivated: function(i) { view.genChannel = view.channels()[i] } }
                        CSelect { label: "Market"; Layout.fillWidth: true; model: view.marketLabels(); currentIndex: Math.max(0, view.marketKeys().indexOf(view.genMarket)); onActivated: function(i) { view.genMarket = view.marketKeys()[i]; view.preview = null } }
                        CField { label: "Voice"; Layout.fillWidth: true; placeholder: "recipe default (af_heart, am_michael…)"; text: view.genVoice; onEdited: function(v) { view.genVoice = v } }
                        Text { text: "TOPIC / MATERIAL (optional)"; color: Theme.text2; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1 }
                        Rectangle { Layout.fillWidth: true; Layout.fillHeight: true; color: Theme.bg4; radius: Theme.r5; border.color: Theme.border1
                            Flickable { anchors.fill: parent; anchors.margins: 6; clip: true; contentHeight: genData.implicitHeight
                                TextEdit { id: genData; width: parent.width; color: Theme.text0; font.pixelSize: Theme.fs12; wrapMode: TextEdit.Wrap; selectByMouse: true } } }
                        RowLayout { spacing: 6
                            CButton { text: view.previewBusy ? "writing…" : "Preview script"; small: true; enabled: !view.previewBusy; onClicked: view.previewScript() }
                            CButton { text: view.genBusy ? "Generating…" : "Generate spot"; small: true; variant: "primary"; enabled: !view.genBusy; onClicked: view.generateNow() } }
                    }
                    CPanel {
                        Layout.fillWidth: true; Layout.fillHeight: true; title: "Script preview"
                        Flickable { anchors.fill: parent; clip: true; contentHeight: previewCol.implicitHeight
                            ColumnLayout { id: previewCol; width: parent.width; spacing: 6
                                // Headline News (render "stories"): the scenes in air order, each story with its photo and the words the voice reads
                                readonly property bool hn: !!(view.preview && !view.preview.error && view.preview.stories && view.preview.stories.length)
                                readonly property var storyScenes: hn ? (view.preview.scenes || []).filter(function(s) { return s.kind === "story" }) : []
                                function sceneOf(kind) { var a = (view.preview && view.preview.scenes) || []; for (var i = 0; i < a.length; ++i) if (a[i].kind === kind) return a[i]; return null }
                                Text { visible: !view.preview; text: "Press Preview to see what the writer would say (no render)."; color: Theme.text3; font.pixelSize: Theme.fs11 }
                                Text { visible: !!(view.preview && view.preview.error); text: view.preview && view.preview.error ? view.preview.error : ""; color: Theme.danger; font.pixelSize: Theme.fs11; wrapMode: Text.Wrap; Layout.fillWidth: true }
                                Text { visible: !!(view.preview && !view.preview.error) && !previewCol.hn; text: view.preview ? (view.preview.headline || "") : ""; color: Theme.text0; font.pixelSize: Theme.fs16; font.bold: true; wrapMode: Text.Wrap; Layout.fillWidth: true }
                                Text { visible: !!(view.preview && !view.preview.error) && !previewCol.hn; text: view.preview ? (view.preview.lines || []).map(function(l) { return "▪ " + l }).join("\n") : ""; color: Theme.text2; font.pixelSize: Theme.fs11; wrapMode: Text.Wrap; Layout.fillWidth: true }
                                Text { visible: !!(view.preview && !view.preview.error) && !previewCol.hn; text: view.preview ? (view.preview.narration || "") : ""; color: Theme.text1; font.pixelSize: Theme.fs12; wrapMode: Text.Wrap; lineHeight: 1.3; Layout.fillWidth: true }
                                Text { visible: !!(view.preview && !view.preview.error) && !previewCol.hn; text: view.preview ? (view.preview.words || 0) + " words · ~" + Math.round(view.preview.estimatedSec || 0) + " s · writer " + (view.preview.writer || "") + " · " + ((view.preview.sources || []).length) + " source(s)" : ""; color: Theme.text3; font.pixelSize: Theme.fs10 }
                                Text { visible: !!(view.preview && !view.preview.error && view.preview.market); text: view.preview && view.preview.market ? "Market: " + view.preview.market.label + " · " + view.preview.market.area : ""; color: Theme.accent0; font.pixelSize: Theme.fs10; font.bold: true }
                                Text { visible: previewCol.hn; Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.text0; font.pixelSize: Theme.fs16; font.bold: true
                                    text: { if (!previewCol.hn) return ""; var st = view.preview.stories, pics = st.filter(function(s) { return !!s.image }).length; var pkg = view.preview.package; return (pkg || "Headline News") + " · " + st.length + (pkg ? (st.length === 1 ? " scene, " : " scenes, ") : (st.length === 1 ? " story, " : " stories, ")) + pics + " with " + (pics === 1 ? "its photo" : "their photos") } }
                                Text { visible: previewCol.hn && !!previewCol.sceneOf("intro"); text: previewCol.hn && previewCol.sceneOf("intro") ? "▶ " + previewCol.sceneOf("intro").narration : ""; color: Theme.text2; font.pixelSize: Theme.fs11; wrapMode: Text.Wrap; Layout.fillWidth: true }
                                Repeater {
                                    model: previewCol.hn ? view.preview.stories : []
                                    RowLayout {
                                        Layout.fillWidth: true; spacing: 10
                                        Rectangle { Layout.preferredWidth: 128; Layout.preferredHeight: 72; Layout.alignment: Qt.AlignTop; color: "#000"; radius: 4; clip: true
                                            Image { anchors.fill: parent; source: modelData.image || ""; fillMode: Image.PreserveAspectCrop; asynchronous: true; cache: true; visible: status === Image.Ready }
                                            Text { anchors.centerIn: parent; visible: !modelData.image; text: "no photo"; color: Theme.text3; font.pixelSize: Theme.fs10 } }
                                        ColumnLayout { Layout.fillWidth: true; spacing: 2
                                            Text { Layout.fillWidth: true; elide: Text.ElideRight; color: Theme.text3; font.pixelSize: Theme.fs10; font.capitalization: Font.AllUppercase
                                                text: (index + 1) + " / " + view.preview.stories.length + (modelData.place ? " · " + modelData.place : "") + (modelData.category ? " · " + modelData.category : "") }
                                            Text { Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.text0; font.pixelSize: Theme.fs12; font.bold: true; text: modelData.title || "" }
                                            Text { visible: !!modelData.detail; Layout.fillWidth: true; wrapMode: Text.Wrap; color: "#e0b040"; font.pixelSize: Theme.fs11; font.bold: true; text: modelData.detail || "" }
                                            Text { Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.text1; font.pixelSize: Theme.fs11; lineHeight: 1.25
                                                text: (previewCol.storyScenes[index] && previewCol.storyScenes[index].narration) || modelData.summary || "" }
                                        }
                                    }
                                }
                                Text { visible: previewCol.hn && !!previewCol.sceneOf("outro"); text: previewCol.hn && previewCol.sceneOf("outro") ? "■ " + previewCol.sceneOf("outro").narration : ""; color: Theme.text2; font.pixelSize: Theme.fs11; wrapMode: Text.Wrap; Layout.fillWidth: true }
                                Text { visible: previewCol.hn; text: previewCol.hn ? (view.preview.words || 0) + " words · ~" + Math.round(view.preview.estimatedSec || 0) + " s" + (view.preview.package ? "" : " · " + (view.preview.items || 0) + " posts checked · newest first") : ""; color: Theme.text3; font.pixelSize: Theme.fs10 }
                            } }
                    }
                }

                // ---- scatter -------------------------------------------------------------------------------------
                ColumnLayout {
                    visible: view.tab === "scatter"; Layout.fillWidth: true; Layout.fillHeight: true; Layout.margins: 10; spacing: 8
                    property var sc: view.cfg.scatter || ({})
                    RowLayout { spacing: 12
                        CToggle { label: "News scatter enabled"; hint: "fetch unseen stories and scatter them every " + (parent.parent.sc.everyMin || 45) + " min"; checked: !!parent.parent.sc.enabled; implicitWidth: 320; onToggled: function(v) { view.cfgSet({ scatter: { enabled: v } }) } }
                        CSelect { label: "Channel"; Layout.preferredWidth: 140; model: view.channels(); currentIndex: Math.max(0, view.channels().indexOf(parent.parent.sc.channel || "")); onActivated: function(i) { view.cfgSet({ scatter: { channel: view.channels()[i] } }) } }
                        CField { label: "Per hour"; Layout.preferredWidth: 80; mono: true; text: String(parent.parent.sc.perHour || 3); onEdited: function(v) { if (parseInt(v) >= 0) view.cfgSet({ scatter: { perHour: parseInt(v) } }) } }
                        CField { label: "Every (min)"; Layout.preferredWidth: 90; mono: true; text: String(parent.parent.sc.everyMin || 45); onEdited: function(v) { if (parseInt(v) >= 5) view.cfgSet({ scatter: { everyMin: parseInt(v) } }) } }
                        CField { label: "TTL (min)"; Layout.preferredWidth: 90; mono: true; text: String(parent.parent.sc.ttlMin || 1440); onEdited: function(v) { if (parseInt(v) >= 30) view.cfgSet({ scatter: { ttlMin: parseInt(v) } }) } }
                        CField { label: "Max new / run"; Layout.preferredWidth: 90; mono: true; text: String(parent.parent.sc.maxNewPerRun || 4); onEdited: function(v) { if (parseInt(v) >= 1) view.cfgSet({ scatter: { maxNewPerRun: parseInt(v) } }) } }
                        CField { label: "Voice"; Layout.preferredWidth: 140; text: parent.parent.sc.voice || ""; onEdited: function(v) { view.cfgSet({ scatter: { voice: v } }) } }
                    }
                    Text { text: "FEEDS (one URL per line)"; color: Theme.text2; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1 }
                    Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 80; color: Theme.bg4; radius: Theme.r5; border.color: Theme.border1
                        Flickable { anchors.fill: parent; anchors.margins: 6; clip: true; contentHeight: scFeeds.implicitHeight
                            TextEdit { id: scFeeds; width: parent.width; color: Theme.text0; font.pixelSize: Theme.fs11; font.family: Theme.fontMono; wrapMode: TextEdit.Wrap; selectByMouse: true; text: ((view.cfg.scatter || {}).feeds || []).join("\n")
                                onEditingFinished: { view.cfgSet({ scatter: { feeds: text.split("\n").map(function(s) { return s.trim() }).filter(function(s) { return s.indexOf("http") === 0 }) } }) } } } }
                    RowLayout { spacing: 6
                        CButton { text: "Generate stories"; small: true; enabled: !view.scatter.busy; onClicked: { api.post("/spots/scatter/generate", {}, function(ok, r) { window.toast(ok && r.ok ? r.queued + " story spot(s) queued (" + r.fresh + " fresh of " + r.considered + ")" : (r && r.error ? r.error : "failed"), ok && r.ok ? "ok" : "error"); view.reload() }) } }
                        CButton { text: "Apply scatter now"; small: true; variant: "primary"; onClicked: { api.post("/spots/scatter/apply", { channel: (view.cfg.scatter || {}).channel || "", perHour: (view.cfg.scatter || {}).perHour || 0 }, function(ok, r) { if (ok) { window.toast("Scattered " + r.placed + " placement(s) over " + r.slots + " hour(s) from a pool of " + r.pool, "ok"); view.reload() } }) } }
                        Item { Layout.fillWidth: true }
                        Text { text: "pool " + (view.scatter.pool || 0) + " airable · " + (view.scatter.pending || 0) + " rendering · " + (view.scatter.seenCount || 0) + " seen · last fetch " + view.ago(view.scatter.lastGenMs) + " · last apply " + view.ago(view.scatter.lastApplyMs) + ((view.scatter.last || {}).placed !== undefined ? " (" + (view.scatter.last || {}).placed + " placed)" : ""); color: Theme.text3; font.pixelSize: Theme.fs10; elide: Text.ElideRight; Layout.fillWidth: true }
                    }
                    Text { text: "Stories are verbatim reads of unseen feed items over their photo (min " + ((view.cfg.scatter || {}).minSec || 30) + " s with a music bed); the apply pass fills today's remaining hours at banded random minutes, skipping the hour on air, replacing the previous scatter."; color: Theme.text3; font.pixelSize: Theme.fs10; wrapMode: Text.Wrap; Layout.fillWidth: true }
                    Item { Layout.fillHeight: true }
                }

                // ---- config ---------------------------------------------------------------------------------------
                Flickable {
                    visible: view.tab === "config"; Layout.fillWidth: true; Layout.fillHeight: true; clip: true; contentHeight: cfgGrid.implicitHeight + 20
                    GridLayout {
                        id: cfgGrid; width: parent.width - 20; x: 10; y: 10; columns: 4; columnSpacing: 10; rowSpacing: 8
                        property var w: view.cfg.weather || ({})
                        property var l: view.cfg.llm || ({})
                        property var t: view.cfg.tts || ({})
                        property var b: view.cfg.brand || ({})
                        Text { Layout.columnSpan: 4; text: "AREA MARKET (station default: localizes language, place names, sources and weather point of every spot that does not name its own market)"; color: Theme.text2; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1 }
                        CSelect { label: "Station default market"; Layout.preferredWidth: 260; model: view.stationLabels(); currentIndex: Math.max(0, view.stationKeys().indexOf((view.cfg.market && view.cfg.market.default) || "")); onActivated: function(i) { view.cfgSet({ market: { default: view.stationKeys()[i] } }) } }
                        Text { Layout.columnSpan: 3; Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.text3; font.pixelSize: Theme.fs10; text: view.marketBlurb(view.marketByKey(view.stationMarketKey())) }
                        Text { Layout.columnSpan: 4; text: "WEATHER (Open-Meteo)"; color: Theme.text2; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1 }
                        CField { label: "Place"; Layout.preferredWidth: 200; text: cfgGrid.w.place || ""; onEdited: function(v) { view.cfgSet({ weather: { place: v } }) } }
                        CField { label: "Latitude"; Layout.preferredWidth: 120; mono: true; text: String(cfgGrid.w.lat !== undefined ? cfgGrid.w.lat : ""); onEdited: function(v) { if (!isNaN(parseFloat(v))) view.cfgSet({ weather: { lat: parseFloat(v) } }) } }
                        CField { label: "Longitude"; Layout.preferredWidth: 120; mono: true; text: String(cfgGrid.w.lon !== undefined ? cfgGrid.w.lon : ""); onEdited: function(v) { if (!isNaN(parseFloat(v))) view.cfgSet({ weather: { lon: parseFloat(v) } }) } }
                        CSelect { label: "Units"; Layout.preferredWidth: 120; model: ["us", "metric"]; currentIndex: cfgGrid.w.units === "metric" ? 1 : 0; onActivated: function(i) { view.cfgSet({ weather: { units: i === 1 ? "metric" : "us" } }) } }
                        Text { Layout.columnSpan: 4; text: "LLM WRITER (optional; templates when off or on any failure)"; color: Theme.text2; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1 }
                        CToggle { label: "Use the model"; hint: cfgGrid.l.keySource === "env" ? "key from environment" : cfgGrid.l.hasKey ? "key saved" : "no key (set OPENROUTER_API_KEY)"; checked: !!cfgGrid.l.enabled; implicitWidth: 200; onToggled: function(v) { view.cfgSet({ llm: { enabled: v } }) } }
                        CField { label: "OpenRouter key (shared)"; Layout.preferredWidth: 200; placeholder: cfgGrid.l.keySource === "env" ? "from environment (OPENROUTER_API_KEY)" : cfgGrid.l.hasKey ? "•••••• (blank keeps it)" : "sk-or-…"; onAccepted: { if (text.length) { view.cfgSet({ llm: { apiKey: text } }); text = "" } } }
                        ModelPicker { label: "Writer model"; Layout.columnSpan: 2; Layout.preferredWidth: 420; value: cfgGrid.l.model || ""; onPicked: function(id) { view.cfgSet({ llm: { model: id } }) } }
                        CField { label: "Base URL"; Layout.preferredWidth: 220; mono: true; text: cfgGrid.l.baseUrl || ""; onEdited: function(v) { view.cfgSet({ llm: { baseUrl: v } }) } }
                        Text { Layout.columnSpan: 4; text: "VOICE (Kokoro when installed, else Windows SAPI)"; color: Theme.text2; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1 }
                        CSelect { label: "Engine"; Layout.preferredWidth: 120; model: ["auto", "kokoro", "sapi"]; currentIndex: Math.max(0, model.indexOf(cfgGrid.t.engine || "auto")); onActivated: function(i) { view.cfgSet({ tts: { engine: model[i] } }) } }
                        CField { label: "Kokoro dir"; Layout.preferredWidth: 220; mono: true; text: cfgGrid.t.kokoroDir || ""; onEdited: function(v) { view.cfgSet({ tts: { kokoroDir: v } }) } }
                        CField { label: "Default voice"; Layout.preferredWidth: 140; text: cfgGrid.t.defaultVoice || ""; onEdited: function(v) { view.cfgSet({ tts: { defaultVoice: v } }) } }
                        CField { label: "Speed"; Layout.preferredWidth: 80; mono: true; text: String(cfgGrid.t.speed !== undefined ? cfgGrid.t.speed : 1); onEdited: function(v) { if (parseFloat(v) > 0) view.cfgSet({ tts: { speed: parseFloat(v) } }) } }
                        Text { Layout.columnSpan: 4; text: "RENDER + RETENTION"; color: Theme.text2; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1 }
                        CField { label: "FFmpeg (blank = auto)"; Layout.preferredWidth: 260; mono: true; text: view.cfg.ffmpeg || ""; onEdited: function(v) { view.cfgSet({ ffmpeg: v }) } }
                        CField { label: "Keep spots (days)"; Layout.preferredWidth: 100; mono: true; text: String(view.cfg.keepDays !== undefined ? view.cfg.keepDays : 3); onEdited: function(v) { if (parseFloat(v) > 0) view.cfgSet({ keepDays: parseFloat(v) }) } }
                        CField { label: "Accent colour"; Layout.preferredWidth: 110; mono: true; text: cfgGrid.b.accent || ""; onEdited: function(v) { view.cfgSet({ brand: { accent: v } }) } }
                        CField { label: "Ink colour"; Layout.preferredWidth: 110; mono: true; text: cfgGrid.b.ink || ""; onEdited: function(v) { view.cfgSet({ brand: { ink: v } }) } }
                        CField { label: "Music beds folder"; Layout.preferredWidth: 260; mono: true; text: view.cfg.musicDir || ""; onEdited: function(v) { view.cfgSet({ musicDir: v }) } }
                        CField { label: "Backgrounds folder"; Layout.preferredWidth: 260; mono: true; text: view.cfg.backgroundsDir || ""; onEdited: function(v) { view.cfgSet({ backgroundsDir: v }) } }
                        Text { Layout.columnSpan: 2; text: "python " + ((view.cfg.tts || {}).pythonResolved || "?") + "\nffmpeg " + (view.cfg.ffmpegResolved || "?") + "\noutput " + (view.cfg.outputDir || ""); color: Theme.text3; font.pixelSize: Theme.fs10; font.family: Theme.fontMono; wrapMode: Text.Wrap; Layout.fillWidth: true }
                    }
                }
            }
        }
    }
}
