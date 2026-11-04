import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".."
import "../ui"

/**
 * Star nodes (docs/star-nodes.md): the enrolled star-node registry with exec / watchdog / PlayPlay health,
 * enrol / edit / remove, probes, the allowlisted remote action runner (result shown as JSON), the
 * exec / poll config and the action audit. Talks to /api/v1/starnodes/*.
 */
Item {
    id: view
    property var nodes: []
    property var cfg: ({})
    property var allow: ({})
    property var audit: []
    property string editId: ""              // "" = enrol a new node
    property var editNode: null
    property string actNode: ""
    property string action: ""
    property string result: ""
    property bool resultOk: true
    property bool running: false
    readonly property var actions: allow.actions || ["fleet.status", "fleet.analytics", "fleet.query", "fleet.asPlayed", "sync.jobs", "sync.run", "swarm.index", "browse", "state", "service.restart", "station.status", "station.capabilities", "station.call", "exec"]
    readonly property var fleetCommands: allow.fleetCommands || ["on_air", "system_health", "as_played_today", "run_report", "adherence", "schedule_hour", "upcoming", "channel_list"]
    readonly property var services: ["playplay-server", "mediamtx", "obs-ch1", "obs-ch2", "obs-ch3", "obs-ch4"]

    component SecLabel: Text { color: Theme.text2; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1 }
    component PassField: ColumnLayout {
        id: pf
        property string label: ""
        property alias text: pfInput.text
        property alias placeholder: pfInput.placeholderText
        spacing: 4
        Text { visible: pf.label.length > 0; text: pf.label.toUpperCase(); color: Theme.text2; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1 }
        TextField {
            id: pfInput
            Layout.fillWidth: true; implicitHeight: 30
            echoMode: TextInput.Password
            color: Theme.text0; placeholderTextColor: Theme.text3; font.pixelSize: Theme.fs12
            leftPadding: 8; rightPadding: 8
            background: Rectangle { color: Theme.bg4; radius: Theme.r5; border.color: pfInput.activeFocus ? Theme.accent0 : Theme.border1 }
        }
    }
    component AreaField: ColumnLayout {
        id: af
        property string label: ""
        property alias text: afInput.text
        property alias placeholder: afInput.placeholderText
        property alias readOnly: afInput.readOnly
        property int areaHeight: 70
        property bool mono: false
        spacing: 4
        Text { visible: af.label.length > 0; text: af.label.toUpperCase(); color: Theme.text2; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1 }
        Rectangle {
            Layout.fillWidth: true; Layout.preferredHeight: af.areaHeight
            color: Theme.bg4; radius: Theme.r5; border.color: afInput.activeFocus ? Theme.accent0 : Theme.border1
            ScrollView {
                anchors.fill: parent; anchors.margins: 4; clip: true
                TextArea { id: afInput; color: Theme.text0; placeholderTextColor: Theme.text3; font.pixelSize: Theme.fs12; font.family: af.mono ? Theme.fontMono : Theme.fontSans; wrapMode: TextEdit.Wrap; background: null; selectByMouse: true }
            }
        }
    }
    component Dot: Rectangle {
        property var lit: undefined             // true | false | undefined (not configured)
        implicitWidth: 8; implicitHeight: 8; radius: 4
        color: lit === true ? Theme.ready : lit === false ? Theme.danger : Theme.text3
    }

    // Health readers: the node row carries health {exec, watchdog, playplay} (objects with ok, or booleans).
    function part(n, key) {
        var h = n.health || {}
        var v = h[key]
        if (v === undefined) v = n[key]
        if (v === undefined || v === null) return undefined
        if (typeof v === "object") return v.ok === undefined ? undefined : !!v.ok
        return !!v
    }
    function partText(n, key) { var h = n.health || {}; var v = h[key]; return v && typeof v === "object" ? (v.error || v.reason || v.version || (v.ok ? "ok" : "")) : "" }
    function ago(ms) { if (!ms) return "never"; var s = Math.max(0, Math.round((Date.now() - ms) / 1000)); return s < 60 ? s + " s ago" : s < 3600 ? Math.round(s / 60) + " min ago" : (s / 3600).toFixed(1) + " h ago" }
    function when(ms) { return ms ? new Date(ms).toLocaleString(Qt.locale(), "MM-dd HH:mm:ss") : "" }
    function nodeById(id) { for (var i = 0; i < nodes.length; ++i) if (nodes[i].id === id) return nodes[i]; return null }

    function reload() {
        api.get("/starnodes", function(ok, d) {
            if (!ok) return
            view.nodes = d.nodes || []
            view.cfg = d.config || {}
            view.allow = d.allow || {}
            if (!view.actNode && view.nodes.length) view.actNode = view.nodes[0].id
            if (!view.action) view.action = view.actions[0]
        })
        reloadAudit()
    }
    function reloadAudit() { api.get("/starnodes/audit", function(ok, d) { if (ok) view.audit = (d.audit || []).slice().sort(function(a, b) { return (b.ms || 0) - (a.ms || 0) }) }) }
    function startEdit(n) {
        view.editId = n ? n.id : ""
        view.editNode = n
        efName.text = n ? (n.name || "") : ""
        efExec.text = n ? (n.execUrl || "") : ""
        efHost.text = n ? (n.host || "") : ""
        efToken.text = ""
        efState.text = n ? (n.stateUrl || "") : ""
        efPp.text = n ? (n.playplayUrl || "") : ""
        efPpPass.text = ""
        efRoles.text = n ? (n.roles || []).join(", ") : ""
        efNotes.text = n ? (n.notes || "") : ""
        efEnabled.checked = n ? n.enabled !== false : true
        efExecAllow.checked = n ? !!n.allowExec : false
        efRender.checked = n ? !!n.render : false
        efForce.checked = false
    }
    function saveNode() {
        var b = { name: efName.text.trim(), notes: efNotes.text, roles: efRoles.text.split(",").map(function(s) { return s.trim() }).filter(function(s) { return s.length }) }
        if (efExec.text.trim().length) b.execUrl = efExec.text.trim()
        if (efHost.text.trim().length) b.host = efHost.text.trim()
        b.stateUrl = efState.text.trim()
        b.playplayUrl = efPp.text.trim()
        if (efToken.text.length) b.token = efToken.text
        if (efPpPass.text.length) b.playplayPass = efPpPass.text
        if (view.editId.length) {
            b.enabled = efEnabled.checked; b.allowExec = efExecAllow.checked; b.render = efRender.checked
            api.put("/starnodes/" + encodeURIComponent(view.editId), b, function(ok) { if (ok) { window.toast("Node saved", "ok"); view.startEdit(null); view.reload() } })
        } else {
            b.force = efForce.checked
            api.post("/starnodes/enrol", b, function(ok, d) { if (ok) { window.toast("Enrolled " + ((d && (d.name || d.id)) || b.name), "ok"); view.startEdit(null); view.reload() } })
        }
    }
    function buildArgs() {
        var a = {}
        switch (view.action) {
        case "fleet.query": a.stationId = aStation.text.trim(); a.command = aCommand.currentText; break
        case "fleet.asPlayed": if (aDate.text.trim().length) a.date = aDate.text.trim(); break
        case "station.call":
            a.method = aMethod.currentText; a.path = aPath.text.trim()
            if (aBody.text.trim().length) { try { a.body = JSON.parse(aBody.text) } catch (e) { window.toast("Body is not valid JSON: " + e.message, "error"); return null } }
            break
        case "browse": a.path = aPath.text.trim(); break
        case "service.restart": a.service = aService.currentText; break
        case "sync.run": a.id = aId.text.trim(); break
        case "exec": a.command = aCmd.text; if (aShell.text.trim().length) a.shell = aShell.text.trim(); break
        }
        return a
    }
    function runAction() {
        var args = buildArgs()
        if (args === null || !view.actNode) return
        view.running = true
        view.result = "running " + view.action + " on " + view.actNode + "\u2026"
        api.post("/starnodes/" + encodeURIComponent(view.actNode) + "/action", { action: view.action, args: args }, function(ok, d) {
            view.running = false
            view.resultOk = ok
            view.result = typeof d === "string" ? d : JSON.stringify(d, null, 2)
            view.reloadAudit()
        })
    }

    Component.onCompleted: { reload(); startEdit(null) }
    Connections { target: supervisor; function onUiEvent(t, p) { if (t === "starnodes") view.reload() } }

    Flickable {
        anchors.fill: parent; clip: true
        contentWidth: width; contentHeight: page.implicitHeight + 12
        ScrollBar.vertical: ScrollBar {}
        RowLayout {
            id: page
            width: parent.width; spacing: 10

            // ================================================================= left: registry
            ColumnLayout {
                Layout.preferredWidth: 640; Layout.minimumWidth: 520; Layout.maximumWidth: 700; Layout.alignment: Qt.AlignTop; spacing: 10
                CPanel {
                    Layout.fillWidth: true; implicitHeight: 34 + nodesCol.implicitHeight + 24
                    title: "Star nodes (" + view.nodes.length + ")"
                    headerRight: [
                        CButton { text: "Probe all"; small: true; onClicked: { api.post("/starnodes/probe", {}, function(ok) { if (ok) { window.toast("Probing star nodes", "ok"); view.reload() } }) } },
                        CButton { text: "\u21BB"; small: true; variant: "ghost"; onClicked: { view.reload() } }
                    ]
                    ColumnLayout {
                        id: nodesCol; anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; spacing: 4
                        Repeater {
                            model: view.nodes
                            Rectangle {
                                Layout.fillWidth: true; implicitHeight: nr.implicitHeight + 12; radius: Theme.r3
                                color: modelData.id === view.actNode ? Qt.rgba(0, 0.83, 1, 0.10) : (index % 2 ? Theme.bg2 : Theme.bg3)
                                MouseArea { anchors.fill: parent; onClicked: { view.actNode = modelData.id } }
                                ColumnLayout {
                                    id: nr; anchors.left: parent.left; anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; anchors.leftMargin: 10; anchors.rightMargin: 6; spacing: 3
                                    RowLayout { Layout.fillWidth: true; spacing: 6
                                        Dot { lit: modelData.online !== undefined ? !!modelData.online : view.part(modelData, "exec") }
                                        Text { text: modelData.name || modelData.id; color: Theme.text0; font.pixelSize: Theme.fs12; font.bold: true; elide: Text.ElideRight; Layout.fillWidth: true }
                                        CBadge { visible: modelData.enabled === false; text: "disabled"; tint: Theme.text3 }
                                        CBadge { visible: !!modelData.allowExec; text: "exec"; tint: Theme.standby }
                                        CBadge { visible: !!modelData.render; text: "render"; tint: Theme.accent0 }
                                        Repeater { model: modelData.roles || []; CBadge { text: modelData; tint: Theme.accent2 } }
                                        CButton { text: "probe"; small: true; variant: "ghost"; onClicked: { api.post("/starnodes/" + encodeURIComponent(modelData.id) + "/probe", {}, function(ok) { if (ok) view.reload() }) } }
                                        CButton { text: "\u270E"; small: true; variant: "ghost"; onClicked: { view.startEdit(modelData) } ToolTip.text: "Edit" }
                                        CButton { text: "\u2715"; small: true; variant: "ghost"; onClicked: { api.del("/starnodes/" + encodeURIComponent(modelData.id), function(ok) { if (ok) { window.toast("Node removed", "ok"); view.reload() } }) } ToolTip.text: "Remove" }
                                    }
                                    Text { Layout.fillWidth: true; text: (modelData.execUrl || modelData.host || "") + (modelData.playplayUrl ? "  \u00B7 pp " + modelData.playplayUrl : "") + (modelData.station && modelData.station.kind ? "  \u00B7 " + modelData.station.kind + (modelData.station.channel ? "/" + modelData.station.channel : "") : "")
                                        color: Theme.text3; font.pixelSize: Theme.fs10; font.family: Theme.fontMono; elide: Text.ElideMiddle }
                                    RowLayout { Layout.fillWidth: true; spacing: 12
                                        Row { spacing: 4; Dot { anchors.verticalCenter: parent.verticalCenter; lit: view.part(modelData, "exec") } Text { text: "exec " + view.partText(modelData, "exec"); color: Theme.text1; font.pixelSize: Theme.fs10 } }
                                        Row { spacing: 4; Dot { anchors.verticalCenter: parent.verticalCenter; lit: view.part(modelData, "watchdog") } Text { text: "watchdog " + (modelData.stateUrl ? view.partText(modelData, "watchdog") : "(not set)"); color: Theme.text1; font.pixelSize: Theme.fs10 } }
                                        Row { spacing: 4; Dot { anchors.verticalCenter: parent.verticalCenter; lit: view.part(modelData, "playplay") } Text { text: "playplay " + (modelData.playplayUrl ? view.partText(modelData, "playplay") : "(not set)"); color: Theme.text1; font.pixelSize: Theme.fs10 } }
                                        Item { Layout.fillWidth: true }
                                        Text { text: "seen " + view.ago(modelData.lastSeenMs || (modelData.health ? modelData.health.checkedMs : 0)); color: Theme.text3; font.pixelSize: Theme.fs10 }
                                    }
                                    Text { Layout.fillWidth: true; visible: !!modelData.error; text: modelData.error || ""; color: Theme.danger; font.pixelSize: Theme.fs10; wrapMode: Text.WordWrap }
                                }
                            }
                        }
                        Text { visible: !view.nodes.length; text: "No star nodes enrolled"; color: Theme.text3; font.pixelSize: Theme.fs11 }
                    }
                }
                CPanel {
                    Layout.fillWidth: true; implicitHeight: 34 + enrolCol.implicitHeight + 24
                    title: view.editId.length ? "Edit node " + view.editId : "Enrol a node"
                    headerRight: [ CButton { visible: view.editId.length > 0; text: "new enrolment"; small: true; variant: "ghost"; onClicked: { view.startEdit(null) } } ]
                    ColumnLayout {
                        id: enrolCol; anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; spacing: 6
                        GridLayout {
                            Layout.fillWidth: true; columns: 2; columnSpacing: 8; rowSpacing: 6
                            CField { id: efName; label: "Name"; Layout.fillWidth: true }
                            CField { id: efRoles; label: "Roles (comma list)"; Layout.fillWidth: true; placeholder: "playout, render" }
                            CField { id: efExec; label: "Exec URL"; Layout.fillWidth: true; mono: true; placeholder: "http://host:8898" }
                            CField { id: efHost; label: "or host"; Layout.fillWidth: true; mono: true; placeholder: "100.x.y.z" }
                            PassField { id: efToken; label: "Token (x-agent-auth)"; Layout.fillWidth: true; placeholder: view.editNode && view.editNode.hasToken ? "(set) \u2014 blank keeps it" : "" }
                            CField { id: efState; label: "Watchdog state URL"; Layout.fillWidth: true; mono: true; placeholder: "http://127.0.0.1:8125" }
                            CField { id: efPp; label: "PlayPlay URL"; Layout.fillWidth: true; mono: true; placeholder: "http://host:7700" }
                            PassField { id: efPpPass; label: "PlayPlay password"; Layout.fillWidth: true; placeholder: view.editNode && view.editNode.hasPlayplayPass ? "(set) \u2014 blank keeps it" : "" }
                            CField { id: efNotes; label: "Notes"; Layout.fillWidth: true; Layout.columnSpan: 2 }
                        }
                        RowLayout { Layout.fillWidth: true; spacing: 10
                            CToggle { id: efForce; visible: !view.editId.length; label: "Force (store offline)"; implicitWidth: 170 }
                            CToggle { id: efEnabled; visible: view.editId.length > 0; label: "Enabled"; implicitWidth: 100 }
                            CToggle { id: efExecAllow; visible: view.editId.length > 0; label: "Allow exec"; implicitWidth: 110 }
                            CToggle { id: efRender; visible: view.editId.length > 0; label: "Render"; implicitWidth: 90 }
                            Item { Layout.fillWidth: true }
                            CButton { text: view.editId.length ? "Save node" : "Enrol"; variant: "primary"; enabled: efExec.text.trim().length > 0 || efHost.text.trim().length > 0; onClicked: { view.saveNode() } }
                        }
                        Text { Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.text3; font.pixelSize: Theme.fs10
                            text: "Enrolment checks GET {execUrl}/api/fleet/status with the token first: 401 refuses it; an unreachable node is refused unless Force stores it offline." }
                    }
                }
                CPanel {
                    Layout.fillWidth: true; implicitHeight: 34 + cfgRow.implicitHeight + 24
                    title: "Config"
                    RowLayout {
                        id: cfgRow; anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; spacing: 10
                        CToggle { id: cfExec; label: "Allow exec (fleet-wide)"; hint: "also needs Allow exec on the node"; implicitWidth: 230; checked: !!view.cfg.allowExec }
                        CField { id: cfPoll; label: "Poll (s)"; Layout.preferredWidth: 90; mono: true; text: String(view.cfg.pollSec || 60) }
                        Item { Layout.fillWidth: true }
                        CButton { text: "Save"; variant: "primary"; Layout.alignment: Qt.AlignBottom
                            onClicked: { api.put("/starnodes/config", { allowExec: cfExec.checked, pollSec: parseInt(cfPoll.text) || 60 }, function(ok, d) { if (ok) { view.cfg = d.config || d; window.toast("Star node config saved", "ok") } }) } }
                    }
                }
            }

            // ================================================================= right: actions + audit
            ColumnLayout {
                Layout.fillWidth: true; Layout.alignment: Qt.AlignTop; spacing: 10
                CPanel {
                    Layout.fillWidth: true; implicitHeight: 34 + actCol.implicitHeight + 24
                    title: "Remote action"
                    ColumnLayout {
                        id: actCol; anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; spacing: 6
                        RowLayout { Layout.fillWidth: true; spacing: 8
                            CSelect { id: aNodeSel; label: "Node"; Layout.preferredWidth: 220; model: view.nodes.map(function(n) { return n.name || n.id })
                                currentIndex: { for (var i = 0; i < view.nodes.length; ++i) if (view.nodes[i].id === view.actNode) return i; return -1 }
                                onActivated: function(i) { view.actNode = view.nodes[i].id } }
                            CSelect { id: aActSel; label: "Action"; Layout.fillWidth: true; model: view.actions; currentIndex: Math.max(0, view.actions.indexOf(view.action))
                                onActivated: function(i) { view.action = view.actions[i] } }
                        }
                        // args per action
                        RowLayout { visible: view.action === "fleet.query"; Layout.fillWidth: true; spacing: 8
                            CField { id: aStation; label: "Station id"; Layout.fillWidth: true; mono: true; placeholder: "main" }
                            CSelect { id: aCommand; label: "Command"; Layout.preferredWidth: 200; model: view.fleetCommands } }
                        CField { id: aDate; visible: view.action === "fleet.asPlayed"; label: "Date (yyyy-MM-dd, blank = today)"; Layout.fillWidth: true; mono: true }
                        RowLayout { visible: view.action === "station.call" || view.action === "browse"; Layout.fillWidth: true; spacing: 8
                            CSelect { id: aMethod; visible: view.action === "station.call"; label: "Method"; Layout.preferredWidth: 100; model: ["GET", "POST", "PUT", "PATCH"] }
                            CField { id: aPath; label: "Path"; Layout.fillWidth: true; mono: true; placeholder: view.action === "browse" ? "D:\\Media" : "/api/schedule/hours" } }
                        Flow { visible: view.action === "station.call" && (view.allow.stationPaths || []).length > 0; Layout.fillWidth: true; spacing: 4
                            Repeater { model: view.allow.stationPaths || []
                                CButton { text: String(modelData); small: true; variant: "ghost"; onClicked: { aPath.text = String(modelData) } } } }
                        AreaField { id: aBody; visible: view.action === "station.call"; label: "Body (JSON)"; Layout.fillWidth: true; mono: true; areaHeight: 80; placeholder: "{ }" }
                        CSelect { id: aService; visible: view.action === "service.restart"; label: "Service"; Layout.preferredWidth: 220; model: view.services }
                        CField { id: aId; visible: view.action === "sync.run"; label: "Sync job id"; Layout.fillWidth: true; mono: true }
                        ColumnLayout { visible: view.action === "exec"; Layout.fillWidth: true; spacing: 6
                            Rectangle { Layout.fillWidth: true; implicitHeight: warnText.implicitHeight + 12; radius: Theme.r5; color: Qt.rgba(1, 0.58, 0, 0.12); border.color: Theme.standby
                                Text { id: warnText; anchors.fill: parent; anchors.margins: 6; wrapMode: Text.WordWrap; color: Theme.standby; font.pixelSize: Theme.fs11
                                    text: "exec runs a shell command on the node. It is refused unless Allow exec is on for this node AND in the fleet config (both default off)" + (view.cfg.allowExec ? "" : " \u2014 fleet config is OFF") + ((view.nodeById(view.actNode) || {}).allowExec ? "" : " \u2014 this node is OFF") + ". Every call is audited." } }
                            RowLayout { Layout.fillWidth: true; spacing: 8
                                CField { id: aCmd; label: "Command"; Layout.fillWidth: true; mono: true }
                                CField { id: aShell; label: "Shell"; Layout.preferredWidth: 120; mono: true; placeholder: "default" } }
                        }
                        RowLayout { Layout.fillWidth: true; spacing: 8
                            Item { Layout.fillWidth: true }
                            CButton { text: view.running ? "Running\u2026" : "Run action"; icon2: "\u25B6"; variant: view.action === "exec" || view.action === "service.restart" ? "warn" : "primary"
                                enabled: !view.running && view.actNode.length > 0 && view.action.length > 0; onClicked: { view.runAction() } } }
                        SecLabel { text: "RESULT" }
                        Rectangle {
                            Layout.fillWidth: true; Layout.preferredHeight: 300; color: Theme.bg1; radius: Theme.r5; border.color: view.resultOk ? Theme.border1 : Theme.danger
                            ScrollView {
                                anchors.fill: parent; anchors.margins: 4; clip: true
                                TextArea { readOnly: true; text: view.result; color: view.resultOk ? Theme.text0 : Theme.danger; font.family: Theme.fontMono; font.pixelSize: Theme.fs11; wrapMode: TextEdit.Wrap; background: null; selectByMouse: true; placeholderText: "Run an action to see its JSON result"; placeholderTextColor: Theme.text3 }
                            }
                        }
                    }
                }
                CPanel {
                    Layout.fillWidth: true; Layout.preferredHeight: 320; padding: 0
                    title: "Audit (" + view.audit.length + ")"
                    headerRight: [ CButton { text: "\u21BB"; small: true; variant: "ghost"; onClicked: { view.reloadAudit() } } ]
                    ListView {
                        id: auditList
                        anchors.fill: parent; clip: true; model: view.audit
                        ScrollBar.vertical: ScrollBar {}
                        delegate: Rectangle {
                            width: auditList.width; height: 30; color: index % 2 ? Theme.bg2 : Theme.bg3
                            RowLayout { anchors.fill: parent; anchors.leftMargin: 10; anchors.rightMargin: 8; spacing: 8
                                Text { text: view.when(modelData.ms || modelData.createdMs); color: Theme.text3; font.pixelSize: Theme.fs10; font.family: Theme.fontMono; Layout.preferredWidth: 100 }
                                Text { text: modelData.nodeName || modelData.node || modelData.nodeId || ""; color: Theme.text1; font.pixelSize: Theme.fs11; Layout.preferredWidth: 110; elide: Text.ElideRight }
                                Text { text: modelData.action || ""; color: Theme.text0; font.pixelSize: Theme.fs11; font.bold: true; Layout.preferredWidth: 130; elide: Text.ElideRight }
                                Text { Layout.fillWidth: true; text: modelData.error || (modelData.args ? JSON.stringify(modelData.args) : "") ; color: modelData.error ? Theme.danger : Theme.text3; font.pixelSize: Theme.fs10; font.family: Theme.fontMono; elide: Text.ElideRight }
                                Text { visible: !!modelData.actor; text: modelData.actor || ""; color: Theme.text2; font.pixelSize: Theme.fs10 }
                                CBadge { text: modelData.ok === false || modelData.error ? "failed" : modelData.ok ? "ok" : (modelData.status !== undefined ? String(modelData.status) : "\u2014"); tint: modelData.ok === false || modelData.error ? Theme.danger : modelData.ok ? Theme.ready : Theme.text2 }
                            }
                        }
                        Text { anchors.centerIn: parent; visible: !view.audit.length; text: "No actions yet"; color: Theme.text3; font.pixelSize: Theme.fs11 }
                    }
                }
            }
        }
    }
}
