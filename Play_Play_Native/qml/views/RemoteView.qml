import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".."
import "../ui"

/// Remote: how to reach this Supervisor (REST, WebSocket, preview streams, web dashboard), who is connected,
/// other Supervisors driven from here (nodes), station state sessions and the primary/backup failover pair.
/// Everything below the endpoints talks to /api/v1/remote/*, /api/v1/sessions/*, /api/v1/failover/*.
Item {
    id: view
    property var snap: supervisor.snapshot
    property var status: ({})
    // --- redundancy state
    property var fo: ({})
    property var cfg: ({})                   // what the form shows: the node's config + the operator's edits
    property var dirty: ({})                 // keys the operator edited: ONLY these are saved (a stale role/peer must never revert the node)
    property bool cfgDirty: false
    property var hist: []
    property var nodes: []
    property var discovery: ({})
    property var overview: []
    property var sessions: []
    property var selfNode: ({})
    property string confirmKey: ""           // two-click confirm: "promote" | "demote" | "del:<id>" | "apply:<id>"
    property string applyReport: ""
    property var pull: null                  // {nodeId, name, sessions}
    function reload() {
        api.get("/status", function(ok, d) { if (ok) view.status = d })
        api.get("/failover/status", function(ok, d) { if (ok) { view.fo = d; view.mergeCfg(d) } })
        api.get("/failover/history", function(ok, d) { if (ok) view.hist = (d.history || []).slice(0, 10) })
        api.get("/sessions", function(ok, d) { if (ok) { view.sessions = d.sessions || []; view.selfNode = d.node || {} } })
        api.get("/remote/discovery", function(ok, d) { if (ok) view.discovery = d })
        api.get("/remote/nodes", function(ok, d) { if (ok) { view.nodes = d.nodes || []; if (view.nodes.length) api.get("/remote/overview", function(ok2, o) { if (ok2) view.overview = o.nodes || [] }); else view.overview = [] } })
    }
    function confirmOr(key, fn) { if (view.confirmKey === key) { view.confirmKey = ""; fn() } else { view.confirmKey = key; confirmReset.restart(); window.toast("Click again to confirm", "warn") } }
    Timer { id: confirmReset; interval: 5000; onTriggered: { view.confirmKey = "" } }
    // Refresh the form from the node except the keys being edited.
    function mergeCfg(d) { var c = Object.assign({}, d.config || {}); for (var k in view.dirty) c[k] = view.cfg[k]; view.cfg = c }
    function setCfg(k, v) { var c = Object.assign({}, view.cfg); c[k] = v; view.cfg = c; var dd = Object.assign({}, view.dirty); dd[k] = true; view.dirty = dd; view.cfgDirty = true }
    // Saves only the edited keys. A role change is two-click confirmed and carries the role it was chosen against (expectedRole):
    // the node answers 409 when its role has changed since (auto-promote, a newer epoch) instead of silently reverting it.
    function saveCfg() {
        var patch = {}
        for (var k in view.dirty) patch[k] = view.cfg[k]
        var cur = (view.fo && view.fo.role) || "standalone"
        if (patch.role === cur) delete patch.role
        if (patch.role === undefined) { view.sendCfg(patch); return }
        patch.expectedRole = cur
        view.confirmOr("role", function() { view.sendCfg(patch) })
    }
    function sendCfg(patch) {
        api.put("/failover/config", patch, function(ok, d) {
            if (ok) { view.fo = d; view.cfg = Object.assign({}, d.config || {}); view.dirty = ({}); view.cfgDirty = false; window.toast("Failover config saved — role " + d.role, "ok") }
            else {
                var dd = Object.assign({}, view.dirty); delete dd.role; view.dirty = dd; view.cfgDirty = Object.keys(dd).length > 0   // e.g. 409: show the node's real role again
                window.toast(d && d.error ? d.error : "save failed", "error"); view.reload()
            }
        })
    }
    function proxy(nodeId, method, path, okMsg) { api.post("/remote/nodes/" + nodeId + "/proxy", { method: method, path: path, body: {} }, function(ok, d) { window.toast(ok ? okMsg : (d && d.error ? d.error : "failed"), ok ? "ok" : "error"); if (ok) view.reload() }) }
    function roleColor(r) { return r === "primary" ? Theme.accent2 : r === "backup" ? Theme.standby : Theme.text2 }
    function ago(ms) { if (!ms) return "never"; var s = Math.max(0, Math.round((Date.now() - ms) / 1000)); return s < 60 ? s + " s ago" : s < 3600 ? Math.round(s / 60) + " min ago" : (s / 3600).toFixed(1) + " h ago" }
    function when(ms) { return ms ? new Date(ms).toLocaleString(Qt.locale(), "MM-dd HH:mm") : "—" }
    function summary(s) { var sm = s.summary || {}; return (sm.channels || 0) + " ch · " + (sm.schedule || 0) + " hours · " + (sm.media || 0) + " media · " + ((sm.settings || 0) + (sm.cg || 0) + (sm.spots || 0) + (sm.programming || 0)) + " settings" }
    Component.onCompleted: { reload() }
    Timer { interval: 5000; running: true; repeat: true; onTriggered: { view.reload() } }
    Connections { target: supervisor; function onUiEvent(type, payload) { if (type === "redundancy") view.reload() } }
    TextEdit { id: clip; visible: false }
    function copy(s) { clip.text = s; clip.selectAll(); clip.copy(); window.toast("Copied " + s, "ok") }

    Flickable {
        anchors.fill: parent; clip: true
        contentWidth: width; contentHeight: col.implicitHeight + 12
        ScrollBar.vertical: ScrollBar {}
        ColumnLayout {
            id: col
            width: parent.width; spacing: 10

            RemotePad { Layout.fillWidth: true }
            AutomationPanel { Layout.fillWidth: true }

            // ------------------------------------------------------------ endpoints + supervisor (existing)
            GridLayout {
                Layout.fillWidth: true; columns: 2; columnSpacing: 10; rowSpacing: 10
                CPanel {
                    Layout.fillWidth: true; Layout.preferredHeight: 250
                    title: "Endpoints"
                    ColumnLayout {
                        anchors.fill: parent; spacing: 8
                        Repeater {
                            model: [
                                ["Web dashboard", api.rawUrl("/")],
                                ["REST API", api.baseUrl + "/api/v1/status"],
                                ["WebSocket telemetry", "ws://127.0.0.1:" + (view.snap.wsPort || 7701)],
                                ["Preview MJPEG (" + supervisor.activeChannelId + ")", api.rawUrl("/preview/" + supervisor.activeChannelId + ".mjpeg")],
                                ["IPC pipe", view.snap.ipcName || "PlayPlaySupervisor"],
                                ["Data directory", view.snap.dataDir || ""]
                            ]
                            RowLayout {
                                Layout.fillWidth: true
                                Text { text: modelData[0]; color: Theme.text2; font.pixelSize: Theme.fs11; Layout.preferredWidth: 180 }
                                Text { text: modelData[1]; color: Theme.text0; font.family: Theme.fontMono; font.pixelSize: Theme.fs11; elide: Text.ElideMiddle; Layout.fillWidth: true }
                                CButton { icon2: "⎘"; small: true; variant: "ghost"; onClicked: { view.copy(modelData[1]) } }
                                CButton { icon2: "↗"; small: true; variant: "ghost"; visible: modelData[1].indexOf("http") === 0; onClicked: { Qt.openUrlExternally(modelData[1]) } }
                            }
                        }
                        Item { Layout.fillHeight: true }
                        Text { Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.text3; font.pixelSize: Theme.fs11
                            text: "Remote operators use the web dashboard (same REST surface as this app). Bind address and token are set in the Supervisor's settings table (settings.rest.bind, settings.rest.token)." }
                    }
                }
                CPanel {
                    Layout.fillWidth: true; Layout.preferredHeight: 250
                    title: "Supervisor"
                    ColumnLayout {
                        anchors.fill: parent; spacing: 6
                        Repeater {
                            model: [
                                ["Connected", supervisor.connected ? "yes" + (supervisor.launchedByUs ? " (launched by this app)" : "") : "no"],
                                ["Uptime", Theme.fmtDur((view.status.uptimeMs || view.snap.uptimeMs || 0) / 1000)],
                                ["NTP offset", ((view.snap.ntpOffsetMs !== undefined ? view.snap.ntpOffsetMs : 0)).toFixed(1) + " ms"],
                                ["Channels", String((view.snap.channels || []).length)],
                                ["Health", view.snap.health ? (view.snap.health.status + " (" + view.snap.health.score + ")") : "—"],
                                ["REST port", String(view.snap.restPort || 7700)],
                                ["Engines", (view.snap.channels || []).filter(function(c) { return c.process && c.process.connected }).length + " connected"],
                                ["Node", (view.selfNode.name || "") + "  " + String(view.selfNode.id || "").slice(0, 8)]
                            ]
                            RowLayout {
                                Layout.fillWidth: true; spacing: 12
                                Text { text: modelData[0]; color: Theme.text2; font.pixelSize: Theme.fs11; Layout.preferredWidth: 110 }
                                Text { text: modelData[1]; color: Theme.text0; font.family: Theme.fontMono; font.pixelSize: Theme.fs11; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                            }
                        }
                        Item { Layout.fillHeight: true }
                        CButton { text: "Query status"; small: true; onClicked: { view.reload(); supervisor.refresh() } }
                    }
                }
            }
            CPanel {
                Layout.fillWidth: true; Layout.preferredHeight: 44 + Math.max(1, supervisor.channels.length) * 34
                title: "Channel engines"
                padding: 0
                ListView {
                    anchors.fill: parent; clip: true; model: supervisor.channelModel   // perf-app PA-3: rows updated in place
                    delegate: Rectangle {
                        width: ListView.view.width; height: 34; color: index % 2 ? Theme.bg2 : Theme.bg3
                        property var t: supervisor.telemetryOf(modelData.profile.id)
                        RowLayout { anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 12; spacing: 12
                            Text { text: modelData.profile.name; color: Theme.text0; font.pixelSize: Theme.fs12; font.bold: true; Layout.preferredWidth: 140 }
                            CBadge { text: modelData.process.connected ? "CONNECTED" : modelData.process.running ? "STARTING" : "STOPPED"; tint: modelData.process.connected ? Theme.ready : modelData.process.running ? Theme.standby : Theme.text3 }
                            Text { text: "pid " + modelData.process.pid + "  restarts " + modelData.process.restarts; color: Theme.text2; font.family: Theme.fontMono; font.pixelSize: Theme.fs10 }
                            Text { text: t.state || modelData.engineState; color: Theme.stateColor(t.state || modelData.engineState); font.pixelSize: Theme.fs11; font.bold: true }
                            Text { text: t.telemetry ? "tick " + t.telemetry.tickMsLast.toFixed(1) + " ms, late " + t.telemetry.lateTicks : ""; color: Theme.text2; font.family: Theme.fontMono; font.pixelSize: Theme.fs10; Layout.fillWidth: true }
                            CButton { text: "Restart"; small: true; onClicked: { api.post("/channels/" + modelData.profile.id + "/restart", {}) } }
                            CButton { text: modelData.process.running ? "Stop" : "Start"; small: true; variant: modelData.process.running ? "danger" : "success"; onClicked: { api.post("/channels/" + modelData.profile.id + (modelData.process.running ? "/stop" : "/start"), {}, function() { supervisor.refresh() }) } } }
                    }
                }
            }

            // ------------------------------------------------------------ failover pair
            CPanel {
                Layout.fillWidth: true; Layout.preferredHeight: 330
                title: "Failover pair"
                headerRight: [
                    CBadge { text: view.fo.role || "standalone"; tint: view.roleColor(view.fo.role) },
                    Text { text: (view.fo.nodeName || "") + " · epoch " + (view.fo.epoch || 0); color: Theme.text3; font.pixelSize: Theme.fs10; font.family: Theme.fontMono },
                    CButton { visible: view.fo.role === "backup"; text: view.confirmKey === "promote" ? "CONFIRM PROMOTE" : "⚡ Promote"; small: true; variant: "success"
                        onClicked: { view.confirmOr("promote", function() { api.post("/failover/promote", { reason: "operator (app)" }, function(ok, d) { window.toast(ok ? "Promoted to " + d.role : (d && d.error) || "failed", ok ? "ok" : "error"); view.reload() }) }) } },
                    CButton { visible: view.fo.role === "primary"; text: view.confirmKey === "demote" ? "CONFIRM DEMOTE" : "⬇ Demote"; small: true; variant: "danger"
                        onClicked: { view.confirmOr("demote", function() { api.post("/failover/demote", { reason: "operator (app)" }, function(ok, d) { window.toast(ok ? "Demoted to " + d.role : (d && d.error) || "failed", ok ? "ok" : "error"); view.reload() }) }) } }
                ]
                ColumnLayout {
                    anchors.fill: parent; spacing: 8
                    RowLayout {
                        Layout.fillWidth: true; spacing: 8
                        property var peer: view.fo.peer || ({})
                        Rectangle { width: 8; height: 8; radius: 4; color: parent.peer.reachable ? Theme.ready : parent.peer.url ? Theme.danger : Theme.text3 }
                        Text { text: "peer " + (parent.peer.url || "— not paired —"); color: Theme.text0; font.family: Theme.fontMono; font.pixelSize: Theme.fs11 }
                        CBadge { visible: !!parent.peer.role; text: parent.peer.role || ""; tint: view.roleColor(parent.peer.role) }
                        Text { Layout.fillWidth: true; elide: Text.ElideRight; color: Theme.text2; font.pixelSize: Theme.fs11
                            text: parent.peer.reachable ? "heartbeat " + view.ago(parent.peer.lastOkMs) + (parent.peer.state && parent.peer.state.armedChannels ? " · armed: " + (parent.peer.state.armedChannels.join(", ") || "none") : "")
                                : (parent.peer.error || (parent.peer.url ? "waiting for first heartbeat" : "")) + (parent.peer.silentSec > 0 && !parent.peer.reachable ? " · silent " + Math.round(parent.peer.silentSec) + " s / grace " + (view.cfg.graceSec || 30) + " s" : "") }
                    }
                    RowLayout {
                        Layout.fillWidth: true; spacing: 8
                        CSelect { id: roleSel; label: "Role"; Layout.preferredWidth: 120; model: ["standalone", "primary", "backup"]
                            currentIndex: Math.max(0, ["standalone", "primary", "backup"].indexOf(view.cfg.role || "standalone"))
                            onActivated: function(i) { view.setCfg("role", roleSel.model[i]) } }
                        CField { label: "Peer URL"; Layout.preferredWidth: 220; text: view.cfg.peerUrl || ""; placeholder: "http://192.168.1.50:7700"; mono: true; onEdited: function(v) { view.setCfg("peerUrl", v) } }
                        CField { label: "Peer token"; Layout.preferredWidth: 160; text: view.cfg.peerToken || ""; mono: true; onEdited: function(v) { view.setCfg("peerToken", v) } }
                        CField { label: "Heartbeat s"; Layout.preferredWidth: 70; text: String(view.cfg.intervalSec || 5); onEdited: function(v) { view.setCfg("intervalSec", parseInt(v) || 5) } }
                        CField { label: "Grace s"; Layout.preferredWidth: 70; text: String(view.cfg.graceSec || 30); onEdited: function(v) { view.setCfg("graceSec", parseInt(v) || 30) } }
                        CField { label: "Replicate s"; Layout.preferredWidth: 80; text: String(view.cfg.replicateIntervalSec || 60); onEdited: function(v) { view.setCfg("replicateIntervalSec", parseInt(v) || 60) } }
                        ColumnLayout { spacing: 2
                            CToggle { label: "auto-promote"; checked: !!view.cfg.auto; implicitWidth: 170; implicitHeight: 22; onToggled: function(v) { view.setCfg("auto", v) } }
                            CToggle { label: "replicate state"; checked: !!view.cfg.replicate; implicitWidth: 170; implicitHeight: 22; onToggled: function(v) { view.setCfg("replicate", v) } }
                            CToggle { label: "pull missing media"; checked: !!view.cfg.pullMedia; implicitWidth: 170; implicitHeight: 22; onToggled: function(v) { view.setCfg("pullMedia", v) } }
                        }
                        Item { Layout.fillWidth: true }
                        CButton { text: view.confirmKey === "role" ? "CONFIRM ROLE → " + String(view.cfg.role || "").toUpperCase() : "Save"; small: true; variant: view.confirmKey === "role" ? "danger" : "primary"; enabled: view.cfgDirty; onClicked: { view.saveCfg() } }
                    }
                    Text { visible: view.fo.role === "backup"; Layout.fillWidth: true; elide: Text.ElideRight; color: Theme.text2; font.pixelSize: Theme.fs11
                        property var repl: view.fo.replication || ({})
                        text: "Replication " + (repl.enabled ? "on" : "off") + " · last " + view.ago(repl.lastOkMs) + ((repl.lastGroups || []).length ? " (" + repl.lastGroups.join(", ") + ")" : "") + " · " + (repl.unresolvedMedia || []).length + " unresolved media" + (repl.pulling ? " · pulling " + repl.pulling : "") + (repl.pulled ? " · pulled " + repl.pulled : "") }
                    Text { text: "HISTORY"; color: Theme.text2; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1 }
                    ListView {
                        Layout.fillWidth: true; Layout.fillHeight: true; clip: true; model: view.hist
                        delegate: RowLayout { width: ListView.view.width; height: 20; spacing: 8
                            Text { text: view.when(modelData.timeMs); color: Theme.text3; font.family: Theme.fontMono; font.pixelSize: Theme.fs10; Layout.preferredWidth: 90 }
                            CBadge { text: modelData.event; tint: modelData.level === "critical" ? Theme.danger : modelData.level === "warning" || modelData.level === "error" ? Theme.warn : Theme.text2 }
                            Text { text: modelData.detail; color: Theme.text1; font.pixelSize: Theme.fs11; elide: Text.ElideRight; Layout.fillWidth: true } }
                        Text { anchors.centerIn: parent; visible: view.hist.length === 0; text: "no events yet"; color: Theme.text3; font.pixelSize: Theme.fs11 }
                    }
                }
            }

            // ------------------------------------------------------------ remote nodes
            CPanel {
                Layout.fillWidth: true; Layout.preferredHeight: 170 + nodeList.contentHeight + (view.pull ? 36 + Math.max(1, (view.pull.sessions || []).length) * 24 : 0)
                title: "Nodes"
                padding: 0
                headerRight: [
                    Text { text: view.nodes.length + " configured"; color: Theme.text3; font.pixelSize: Theme.fs10 },
                    CButton { text: "refresh"; small: true; variant: "ghost"; onClicked: { view.reload() } }
                ]
                ColumnLayout {
                    anchors.fill: parent; spacing: 0
                    ListView {
                        id: nodeList
                        Layout.fillWidth: true; Layout.preferredHeight: contentHeight; clip: true; interactive: false
                        model: view.overview.length ? view.overview : view.nodes
                        delegate: Rectangle {
                            width: ListView.view.width; height: 40 + Math.max(0, (modelData.channels || []).length) * 24; color: index % 2 ? Theme.bg2 : Theme.bg3
                            property var n: modelData
                            ColumnLayout { anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 12; spacing: 2
                                RowLayout { Layout.fillWidth: true; Layout.preferredHeight: 36; spacing: 8
                                    Text { text: n.name || ""; color: Theme.text0; font.pixelSize: Theme.fs12; font.bold: true; Layout.preferredWidth: 130; elide: Text.ElideRight }
                                    Text { text: n.url || ""; color: Theme.accent0; font.family: Theme.fontMono; font.pixelSize: Theme.fs11; Layout.preferredWidth: 200; elide: Text.ElideMiddle
                                        MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: { Qt.openUrlExternally(n.url) } } }
                                    CBadge { text: n.reachable === undefined ? "…" : n.reachable ? "online " + (n.latencyMs >= 0 ? n.latencyMs + " ms" : "") : "offline"; tint: n.reachable ? Theme.ready : n.reachable === false ? Theme.danger : Theme.text3 }
                                    CBadge { visible: !!n.role; text: n.role || ""; tint: view.roleColor(n.role) }
                                    Text { text: n.error && !n.reachable ? n.error : ""; color: Theme.text3; font.pixelSize: Theme.fs10; Layout.fillWidth: true; elide: Text.ElideRight }
                                    CButton { text: "Open"; small: true; onClicked: { Qt.openUrlExternally(n.url) } }
                                    CSelect { id: pushSel; Layout.preferredWidth: 150; visible: view.sessions.length > 0; model: view.sessions; textRole: "name" }
                                    CButton { text: "Push"; small: true; visible: view.sessions.length > 0; onClicked: { var s = view.sessions[pushSel.currentIndex]; if (!s) return; api.post("/remote/nodes/" + n.id + "/push-session/" + s.id, {}, function(ok, d) { window.toast(ok ? "Push: " + (d.action || "ok") + (d.reason ? " — " + d.reason : "") : (d && d.error) || "push failed", ok && d.action !== "conflict" ? "ok" : "error") }) } }
                                    CButton { text: "Pull…"; small: true; onClicked: { view.pull = { nodeId: n.id, name: n.name, sessions: [] }; api.post("/remote/nodes/" + n.id + "/proxy", { method: "GET", path: "/sessions", body: {} }, function(ok, d) { if (ok) view.pull = { nodeId: n.id, name: n.name, sessions: d.sessions || [] }; else window.toast((d && d.error) || "cannot list sessions", "error") }) } }
                                    CButton { icon2: "✕"; small: true; variant: "ghost"; text: view.confirmKey === "del:" + n.id ? "confirm" : ""
                                        onClicked: { view.confirmOr("del:" + n.id, function() { var l = view.nodes.filter(function(x) { return x.id !== n.id }); api.put("/remote/nodes", { nodes: l }, function(ok, d) { if (ok) { view.nodes = d.nodes || []; view.reload() } }) }) } }
                                }
                                Repeater { model: n.channels || []
                                    RowLayout { Layout.fillWidth: true; Layout.preferredHeight: 22; spacing: 6
                                        Item { width: 12 }
                                        Text { text: modelData.name || modelData.id; color: Theme.text1; font.pixelSize: Theme.fs11; font.bold: true; Layout.preferredWidth: 118; elide: Text.ElideRight }
                                        CBadge { text: modelData.state || "offline"; tint: modelData.state === "PLAYING" ? Theme.onAir : modelData.state ? Theme.text2 : Theme.text3 }
                                        CBadge { text: modelData.armed ? "armed" : "disarmed"; tint: modelData.armed ? Theme.ready : Theme.text3 }
                                        Text { text: modelData.title || ""; color: Theme.text1; font.pixelSize: Theme.fs11; Layout.fillWidth: true; elide: Text.ElideRight }
                                        CButton { text: modelData.armed ? "disarm" : "arm"; small: true; variant: "ghost"; onClicked: { view.proxy(n.id, "POST", "/channels/" + modelData.id + (modelData.armed ? "/disarm" : "/arm"), (modelData.armed ? "Disarmed " : "Armed ") + modelData.id + " on " + n.name) } }
                                        CButton { text: "load hour"; small: true; variant: "ghost"; onClicked: { view.proxy(n.id, "POST", "/channels/" + modelData.id + "/load-hour", "Hour pushed on " + n.name) } }
                                        CButton { text: "snap"; small: true; variant: "ghost"; onClicked: { view.proxy(n.id, "POST", "/channels/" + modelData.id + "/snap", "Snapped " + modelData.id + " on " + n.name) } }
                                    }
                                }
                            }
                        }
                    }
                    Text { visible: view.nodes.length === 0; Layout.fillWidth: true; Layout.margins: 12; wrapMode: Text.WordWrap; color: Theme.text3; font.pixelSize: Theme.fs11
                        text: "No remote nodes. Add another Supervisor (URL + its rest.token) to drive it from here, push sessions to it, or pair it as a failover peer." }
                    // pull list
                    Rectangle { visible: !!view.pull; Layout.fillWidth: true; Layout.preferredHeight: visible ? 36 + Math.max(1, ((view.pull || {}).sessions || []).length) * 24 : 0; color: Theme.bg1
                        ColumnLayout { anchors.fill: parent; anchors.margins: 8; spacing: 2
                            RowLayout { Layout.fillWidth: true
                                Text { text: "Sessions on " + ((view.pull || {}).name || ""); color: Theme.text0; font.pixelSize: Theme.fs11; font.bold: true; Layout.fillWidth: true }
                                CButton { text: "close"; small: true; variant: "ghost"; onClicked: { view.pull = null } } }
                            Repeater { model: (view.pull || {}).sessions || []
                                RowLayout { Layout.fillWidth: true; Layout.preferredHeight: 22; spacing: 8
                                    Text { text: modelData.name; color: Theme.text1; font.pixelSize: Theme.fs11; Layout.preferredWidth: 220; elide: Text.ElideRight }
                                    Text { text: "v" + (modelData.version || 1) + " · " + view.when(modelData.updatedAt); color: Theme.text3; font.family: Theme.fontMono; font.pixelSize: Theme.fs10; Layout.fillWidth: true }
                                    CButton { text: "← pull"; small: true; onClicked: { api.post("/remote/nodes/" + view.pull.nodeId + "/pull-session/" + modelData.id, {}, function(ok, d) { window.toast(ok ? "Pulled: " + (d.action || "ok") + (d.reason ? " — " + d.reason : "") : (d && d.error) || "pull failed", ok && d.action !== "conflict" ? "ok" : "error"); view.reload() }) } } } }
                            Text { visible: ((view.pull || {}).sessions || []).length === 0; text: "no sessions there (or still loading)"; color: Theme.text3; font.pixelSize: Theme.fs11 }
                        }
                    }
                    Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border0 }
                    RowLayout {
                        Layout.fillWidth: true; Layout.margins: 8; spacing: 8
                        CField { id: lanPass; label: "Shared LAN password"; Layout.preferredWidth: 175; placeholder: view.discovery.hasPassword ? "saved" : "enter on each node"; echoMode: TextInput.Password }
                        CButton { text: view.discovery.enabled ? "Update LAN password" : "Enable auto-connect"; small: true; onClicked: { var body = { enabled: true }; if (lanPass.text) body.password = lanPass.text; api.put("/remote/discovery", body, function(ok, d) { if (ok) { view.discovery = d; lanPass.text = ""; view.reload(); window.toast("LAN auto-connect enabled", "ok") } else window.toast((d && d.error) || "Unable to enable LAN discovery", "error") }) } }
                        CButton { visible: view.discovery.enabled; text: "Disable auto-connect"; small: true; onClicked: api.put("/remote/discovery", { enabled: false }, function(ok, d) { if (ok) view.discovery = d }) }
                        Item { Layout.fillWidth: true }
                    }
                    RowLayout {
                        Layout.fillWidth: true; Layout.margins: 8; spacing: 8
                        CField { id: nnName; label: "Name"; Layout.preferredWidth: 140; placeholder: "Backup" }
                        CField { id: nnUrl; label: "IP address"; Layout.preferredWidth: 240; placeholder: "192.168.1.50"; mono: true }
                        CField { id: nnTok; label: "Password"; Layout.preferredWidth: 220; echoMode: TextInput.Password }
                        CButton { text: "+ Add node"; small: true; variant: "primary"; Layout.alignment: Qt.AlignBottom; onClicked: { if (!nnUrl.text.trim()) { window.toast("IP address required", "error"); return }
                            var l = view.nodes.slice(); l.push({ name: nnName.text.trim(), url: nnUrl.text.trim(), password: nnTok.text })
                            api.put("/remote/nodes", { nodes: l }, function(ok, d) { if (ok) { view.nodes = d.nodes || []; nnName.text = ""; nnUrl.text = ""; nnTok.text = ""; window.toast("Node added", "ok"); view.reload() } }) } }
                        Item { Layout.fillWidth: true }
                    }
                }
            }

            // ------------------------------------------------------------ sessions
            CPanel {
                Layout.fillWidth: true; Layout.preferredHeight: 110 + Math.max(1, view.sessions.length) * 36 + (view.applyReport.length ? 160 : 0)
                title: "Sessions"
                padding: 0
                headerRight: [
                    Text { text: view.sessions.length + " stored"; color: Theme.text3; font.pixelSize: Theme.fs10 },
                    CField { id: capName; width: 220; placeholder: "snapshot name" },
                    CButton { text: "＋ Capture current state"; small: true; variant: "primary"; onClicked: { api.post("/sessions/capture", { name: capName.text.trim() || ("Station " + new Date().toISOString().slice(0, 16).replace("T", " ")), notes: "captured from the operator app" }, function(ok, d) { if (ok) { window.toast("Captured \"" + d.name + "\"", "ok"); capName.text = ""; view.reload() } }) } }
                ]
                ColumnLayout {
                    anchors.fill: parent; spacing: 0
                    ListView {
                        Layout.fillWidth: true; Layout.fillHeight: true; clip: true; model: view.sessions
                        delegate: Rectangle {
                            width: ListView.view.width; height: 36; color: index % 2 ? Theme.bg2 : Theme.bg3
                            property var s: modelData
                            RowLayout { anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 12; spacing: 8
                                Text { text: s.name; color: Theme.text0; font.pixelSize: Theme.fs12; font.bold: true; Layout.preferredWidth: 200; elide: Text.ElideRight; ToolTip.text: (s.notes || "") + "\n" + s.id; ToolTip.visible: hov.containsMouse; MouseArea { id: hov; anchors.fill: parent; hoverEnabled: true } }
                                Text { text: view.when(s.createdAt) + "  v" + (s.version || 1); color: Theme.text2; font.family: Theme.fontMono; font.pixelSize: Theme.fs10; Layout.preferredWidth: 120 }
                                Text { text: (s.node || {}).name || ""; color: Theme.text2; font.pixelSize: Theme.fs11; Layout.preferredWidth: 110; elide: Text.ElideRight }
                                Text { text: view.summary(s); color: Theme.text1; font.pixelSize: Theme.fs11; Layout.fillWidth: true; elide: Text.ElideRight }
                                CButton { text: view.confirmKey === "apply:" + s.id ? "CONFIRM APPLY" : "Apply"; small: true; variant: "success"
                                    onClicked: { var body = { groups: ["channels", "schedule", "media", "settings", "cg", "spots", "programming"], dryRun: true }
                                        if (view.confirmKey !== "apply:" + s.id) { api.post("/sessions/" + s.id + "/apply", body, function(ok, d) { view.applyReport = ok ? "DRY RUN " + s.name + "\n" + JSON.stringify({ applied: d.applied, remapped: d.remapped, unresolvedMedia: d.unresolvedMedia, warnings: d.warnings, errors: d.errors }, null, 1) : (d && d.error) || "dry run failed" }) }
                                        view.confirmOr("apply:" + s.id, function() { body.dryRun = false; api.post("/sessions/" + s.id + "/apply", body, function(ok, d) { view.applyReport = ok ? "APPLIED " + s.name + "\n" + JSON.stringify({ applied: d.applied, unresolvedMedia: d.unresolvedMedia, warnings: d.warnings, errors: d.errors }, null, 1) : (d && d.error) || "apply failed"; window.toast(ok ? "Session applied (outputs and arming kept local)" : "apply failed", ok ? "ok" : "error"); view.reload() }) }) } }
                                CButton { text: "Clone"; small: true; onClicked: { api.post("/sessions/" + s.id + "/clone", { name: s.name + " (copy)" }, function(ok) { if (ok) view.reload() }) } }
                                CButton { text: "Recapture"; small: true; onClicked: { api.post("/sessions/" + s.id + "/recapture", {}, function(ok) { if (ok) { window.toast("Re-captured", "ok"); view.reload() } }) } }
                                CButton { text: "Export"; small: true; onClicked: { Qt.openUrlExternally(api.rawUrl("/api/v1/sessions/export/" + s.id)) } }
                                CButton { icon2: "✕"; small: true; variant: "ghost"; text: view.confirmKey === "sdel:" + s.id ? "confirm" : ""; onClicked: { view.confirmOr("sdel:" + s.id, function() { api.del("/sessions/" + s.id, function(ok) { if (ok) view.reload() }) }) } }
                            }
                        }
                        Text { anchors.centerIn: parent; visible: view.sessions.length === 0; width: parent.width - 40; wrapMode: Text.WordWrap; horizontalAlignment: Text.AlignHCenter; color: Theme.text3; font.pixelSize: Theme.fs11
                            text: "No sessions yet. Capture bundles channels, every hour of the schedule, media records (without transcripts), CG, spots and programming settings into a portable file you can clone, export, push to another node or restore here." }
                    }
                    Rectangle { visible: view.applyReport.length > 0; Layout.fillWidth: true; Layout.preferredHeight: visible ? 160 : 0; color: Theme.bg1
                        Flickable { anchors.fill: parent; anchors.margins: 8; clip: true; contentHeight: rep.implicitHeight; ScrollBar.vertical: ScrollBar {}
                            TextEdit { id: rep; width: parent.width; readOnly: true; selectByMouse: true; wrapMode: Text.Wrap; text: view.applyReport; color: Theme.text1; font.family: Theme.fontMono; font.pixelSize: Theme.fs10 } }
                        CButton { anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 6; text: "close"; small: true; variant: "ghost"; onClicked: { view.applyReport = "" } }
                    }
                    Text { Layout.fillWidth: true; Layout.margins: 8; wrapMode: Text.WordWrap; color: Theme.text3; font.pixelSize: Theme.fs10
                        text: "Apply here uses every group except outputs and keeps this node's armed flags; the first click shows a dry run, the second applies. Group selection, replace mode and the full report are on the web dashboard's Remote page." }
                }
            }
        }
    }
}
