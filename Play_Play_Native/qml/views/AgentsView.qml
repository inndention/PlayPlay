import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".."
import "../ui"

/**
 * Agents (docs/agents.md): one task runner over the station operator commands, StarAgent daemons, Star
 * nodes and the LLM assistant. Agents list, run-task panel (args generated from the command's params),
 * the persistent task log with results / tool trail, the command reference and the agents config.
 * Talks to /api/v1/agents/* and upserts the task carried by the "agents" uiEvent.
 */
Item {
    id: view
    property var agents: []
    property var cfg: ({})
    property var commands: []
    property var tasks: []
    property string agentId: ""
    property string command: ""
    property string selTask: ""
    property bool showRef: false
    readonly property var agent: { for (var i = 0; i < agents.length; ++i) if (agents[i].id === agentId) return agents[i]; return null }
    readonly property string agentKind: agent ? (agent.kind || "") : ""
    readonly property var commandList: {
        if (!agent) return []
        if (agentKind === "operator") return (agent.commands && agent.commands.length ? agent.commands : commands.map(function(c) { return c.name }))
        if (agentKind === "staragent") return agent.commands && agent.commands.length ? agent.commands : ["health", "status", "nowplaying", "list", "search", "exec"]
        if (agentKind === "starnode") return agent.actions || []
        return []
    }
    readonly property var cmdInfo: { for (var i = 0; i < commands.length; ++i) if (commands[i].name === command) return commands[i]; return null }
    readonly property var paramNames: agentKind === "operator" && cmdInfo && cmdInfo.params ? Object.keys(cmdInfo.params) : []
    readonly property var task: { for (var i = 0; i < tasks.length; ++i) if (tasks[i].id === selTask) return tasks[i]; return null }
    readonly property bool active: { for (var i = 0; i < tasks.length; ++i) if (tasks[i].state === "running" || tasks[i].state === "queued") return true; return false }

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

    function stColor(s) { return s === "done" ? Theme.ready : s === "failed" ? Theme.danger : s === "running" ? Theme.accent0 : s === "queued" ? Theme.standby : Theme.text3 }
    function riskColor(r) { return r === "action" ? Theme.standby : r === "exec" ? Theme.danger : Theme.text2 }
    function kindColor(k) { return k === "operator" ? Theme.accent0 : k === "staragent" ? Theme.standby : k === "starnode" ? Theme.accent2 : k === "llm" ? Theme.ready : Theme.text1 }
    function when(ms) { return ms ? new Date(ms).toLocaleString(Qt.locale(), "MM-dd HH:mm:ss") : "" }
    function dur(t) { if (!t.startedMs) return ""; var ms = (t.finishedMs || Date.now()) - t.startedMs; return ms < 1000 ? ms + " ms" : (ms / 1000).toFixed(1) + " s" }
    function pretty(v) { if (v === undefined || v === null) return ""; if (typeof v === "string") return v; return JSON.stringify(v, null, 2) }
    function resultText(t) {
        if (!t) return ""
        var r = t.result
        if (t.agentKind === "llm" && r && typeof r === "object" && (r.answer || r.text)) return r.answer || r.text
        return view.pretty(r)
    }
    function sortTasks(l) { return l.slice().sort(function(a, b) { return (b.createdMs || 0) - (a.createdMs || 0) }) }

    function reload() {
        api.get("/agents", function(ok, d) {
            if (!ok) return
            view.agents = d.agents || []
            if (d.config) view.cfg = d.config
            if (!view.agentId && view.agents.length) view.pickAgent(view.agents[0].id)
        })
        api.get("/agents/commands", function(ok, d) { if (ok) view.commands = d.commands || [] })
        api.get("/agents/config", function(ok, d) { if (ok) view.cfg = d.config || d })
        reloadTasks()
    }
    function reloadTasks() { api.get("/agents/tasks?limit=100", function(ok, d) { if (ok) view.tasks = view.sortTasks(d.tasks || []) }) }
    function upsert(t) {
        if (!t || !t.id) return
        var l = view.tasks.slice(), found = false
        for (var i = 0; i < l.length; ++i) if (l[i].id === t.id) { l[i] = t; found = true; break }
        if (!found) l.unshift(t)
        view.tasks = view.sortTasks(l)
    }
    function pickAgent(id) { view.agentId = id; var l = view.commandList; view.command = l.length ? String(l[0]) : "" }
    function runTask() {
        var args = {}
        if (argsJson.text.trim().length) {
            try { args = JSON.parse(argsJson.text) } catch (e) { window.toast("Args JSON: " + e.message, "error"); return }
        }
        for (var i = 0; i < paramRep.count; ++i) {
            var it = paramRep.itemAt(i)
            if (it && it.text.length) { var v = it.text.trim(); args[it.name] = /^-?[0-9]+(\.[0-9]+)?$/.test(v) ? Number(v) : it.text }
        }
        var b = { agent: view.agentId, command: view.agentKind === "llm" ? "prompt" : view.command, args: args }
        if (view.agentKind === "llm") b.prompt = promptText.text
        api.post("/agents/tasks", b, function(ok, d) {
            if (!ok) return
            var t = d.task || d
            view.upsert(t)
            if (t && t.id) view.selTask = t.id
            window.toast("Task queued: " + (b.command || "prompt"), "ok")
        })
    }
    function saveCfg() {
        var b = { allowExec: cfExec.checked, allowActions: cfActions.checked, model: cfModel.value }
        if (cfKey.text.length) b.apiKey = cfKey.text
        api.put("/agents/config", b, function(ok, d) { if (ok) { view.cfg = d.config || d; cfKey.text = ""; window.toast("Agents config saved", "ok"); view.reload() } })
    }

    Component.onCompleted: { reload() }
    Connections { target: supervisor; function onUiEvent(t, p) { if (t === "agents") view.upsert(p) } }
    Timer { interval: 5000; repeat: true; running: view.active; onTriggered: { view.reloadTasks() } }

    RowLayout {
        anchors.fill: parent; spacing: 10

        // ======================================================================= left: agents, run, config, reference
        Flickable {
            Layout.preferredWidth: 440; Layout.minimumWidth: 380; Layout.maximumWidth: 460; Layout.fillHeight: true
            clip: true; contentWidth: width; contentHeight: leftCol.implicitHeight + 12
            ScrollBar.vertical: ScrollBar {}
            ColumnLayout {
                id: leftCol; width: parent.width; spacing: 10
                CPanel {
                    Layout.fillWidth: true; implicitHeight: 34 + agCol.implicitHeight + 24
                    title: "Agents (" + view.agents.length + ")"
                    headerRight: [ CButton { text: "↻"; small: true; variant: "ghost"; onClicked: { view.reload() } } ]
                    ColumnLayout {
                        id: agCol; anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; spacing: 3
                        Repeater {
                            model: view.agents
                            Rectangle {
                                Layout.fillWidth: true; implicitHeight: 32; radius: Theme.r3
                                color: modelData.id === view.agentId ? Qt.rgba(0, 0.83, 1, 0.12) : (index % 2 ? Theme.bg2 : Theme.bg3)
                                MouseArea { anchors.fill: parent; onClicked: { view.pickAgent(modelData.id) } }
                                RowLayout { anchors.fill: parent; anchors.leftMargin: 8; anchors.rightMargin: 8; spacing: 6
                                    Rectangle { implicitWidth: 8; implicitHeight: 8; radius: 4; color: modelData.online === false || modelData.available === false ? Theme.danger : modelData.online ? Theme.ready : Theme.text3 }
                                    Text { Layout.fillWidth: true; text: (modelData.name || modelData.id) + (modelData.model ? "  · " + modelData.model : ""); color: Theme.text0; font.pixelSize: Theme.fs12; elide: Text.ElideRight }
                                    CBadge { text: modelData.kind || "?"; tint: view.kindColor(modelData.kind) } }
                            }
                        }
                        Text { visible: !view.agents.length; text: "No agents"; color: Theme.text3; font.pixelSize: Theme.fs11 }
                    }
                }
                CPanel {
                    Layout.fillWidth: true; implicitHeight: 34 + runCol.implicitHeight + 24
                    title: "Run task"
                    ColumnLayout {
                        id: runCol; anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; spacing: 6
                        CSelect { label: "Agent"; Layout.fillWidth: true; model: view.agents.map(function(a) { return (a.name || a.id) + "  (" + a.kind + ")" })
                            currentIndex: { for (var i = 0; i < view.agents.length; ++i) if (view.agents[i].id === view.agentId) return i; return -1 }
                            onActivated: function(i) { view.pickAgent(view.agents[i].id) } }
                        CSelect { visible: view.agentKind !== "llm"; label: "Command"; Layout.fillWidth: true; model: view.commandList.map(function(c) { return String(c) })
                            currentIndex: Math.max(0, model.indexOf(view.command)); onActivated: function(i) { view.command = String(view.commandList[i]) } }
                        RowLayout { Layout.fillWidth: true; spacing: 6; visible: !!view.cmdInfo && view.agentKind === "operator"
                            CBadge { text: view.cmdInfo ? (view.cmdInfo.risk || "read") : ""; tint: view.riskColor(view.cmdInfo ? view.cmdInfo.risk : "") }
                            Text { Layout.fillWidth: true; text: view.cmdInfo ? (view.cmdInfo.desc || "") : ""; color: Theme.text2; font.pixelSize: Theme.fs11; wrapMode: Text.WordWrap } }
                        Rectangle { visible: view.command === "exec"; Layout.fillWidth: true; implicitHeight: ew.implicitHeight + 10; radius: Theme.r5; color: Qt.rgba(1, 0.58, 0, 0.12); border.color: Theme.standby
                            Text { id: ew; anchors.fill: parent; anchors.margins: 5; wrapMode: Text.WordWrap; color: Theme.standby; font.pixelSize: Theme.fs11
                                text: "exec runs a shell command on the agent's machine; refused unless Allow exec is on" + (view.cfg.allowExec ? "." : " (it is OFF).") } }
                        Repeater {
                            id: paramRep
                            model: view.paramNames
                            CField {
                                property string name: modelData
                                Layout.fillWidth: true; label: modelData
                                placeholder: view.cmdInfo && view.cmdInfo.params ? String(view.cmdInfo.params[modelData] || "") : ""
                            }
                        }
                        AreaField { id: promptText; visible: view.agentKind === "llm"; label: "Prompt"; Layout.fillWidth: true; areaHeight: 110; placeholder: "What is on air on every channel, and is anything expiring this week?" }
                        Text { visible: view.agentKind === "llm"; Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.text3; font.pixelSize: Theme.fs10
                            text: "Answers with a tool loop over the read commands" + (view.cfg.allowActions ? " and the action commands (allowed)." : "; action commands are off.") }
                        AreaField { id: argsJson; label: view.agentKind === "operator" ? "Extra args (JSON, optional)" : "Args (JSON)"; Layout.fillWidth: true; areaHeight: 56; mono: true; placeholder: "{ }" }
                        CButton { text: "Run"; icon2: "▶"; Layout.alignment: Qt.AlignRight
                            variant: view.cmdInfo && view.cmdInfo.risk === "action" || view.command === "exec" ? "warn" : "primary"
                            enabled: view.agentId.length > 0 && (view.agentKind === "llm" ? promptText.text.trim().length > 0 : view.command.length > 0)
                            onClicked: { view.runTask() } }
                    }
                }
                CPanel {
                    Layout.fillWidth: true; implicitHeight: 34 + cfCol.implicitHeight + 24
                    title: "Config"
                    ColumnLayout {
                        id: cfCol; anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; spacing: 6
                        CToggle { id: cfExec; Layout.fillWidth: true; label: "Allow exec on StarAgents"; checked: !!view.cfg.allowExec }
                        CToggle { id: cfActions; Layout.fillWidth: true; label: "Allow the assistant to run action commands"; checked: !!view.cfg.allowActions }
                        ColumnLayout { Layout.fillWidth: true; spacing: 6   // picker and key on separate rows: side by side the key field was pushed off the panel
                            ModelPicker { id: cfModel; label: "Assistant model"; Layout.fillWidth: true; value: view.cfg.model || "" }
                            PassField { id: cfKey; label: "OpenRouter key (shared)"; Layout.fillWidth: true; placeholder: view.cfg.keySource === "env" ? "from environment (OPENROUTER_API_KEY) — type to override" : view.cfg.keySource === "settings" ? "saved override — blank keeps it" : "(not set) — or set OPENROUTER_API_KEY" } }
                        CButton { text: "Save config"; variant: "primary"; Layout.alignment: Qt.AlignRight; onClicked: { view.saveCfg() } }
                    }
                }
                CPanel {
                    Layout.fillWidth: true; implicitHeight: 34 + (view.showRef ? refCol.implicitHeight + 24 : 0)
                    title: "Command reference (" + view.commands.length + ")"
                    headerRight: [ CButton { text: view.showRef ? "hide" : "show"; small: true; variant: "ghost"; onClicked: { view.showRef = !view.showRef } } ]
                    ColumnLayout {
                        id: refCol; visible: view.showRef; anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; spacing: 6
                        Repeater {
                            model: view.commands
                            ColumnLayout { Layout.fillWidth: true; spacing: 1
                                RowLayout { Layout.fillWidth: true; spacing: 6
                                    Text { text: modelData.name; color: Theme.text0; font.pixelSize: Theme.fs12; font.bold: true; font.family: Theme.fontMono }
                                    CBadge { text: modelData.risk || "read"; tint: view.riskColor(modelData.risk) }
                                    Item { Layout.fillWidth: true }
                                    CButton { text: "use"; small: true; variant: "ghost"
                                        onClicked: { for (var i = 0; i < view.agents.length; ++i) if (view.agents[i].kind === "operator") { view.pickAgent(view.agents[i].id); break } view.command = modelData.name } } }
                                Text { Layout.fillWidth: true; text: modelData.desc || ""; color: Theme.text2; font.pixelSize: Theme.fs11; wrapMode: Text.WordWrap }
                                Text { Layout.fillWidth: true; visible: !!modelData.params && Object.keys(modelData.params).length > 0
                                    text: modelData.params ? Object.keys(modelData.params).map(function(k) { return k + ": " + modelData.params[k] }).join("\n") : ""
                                    color: Theme.text3; font.pixelSize: Theme.fs10; font.family: Theme.fontMono; wrapMode: Text.WordWrap }
                            }
                        }
                    }
                }
            }
        }

        // ======================================================================= middle: tasks
        CPanel {
            Layout.fillWidth: true; Layout.fillHeight: true; Layout.minimumWidth: 320; padding: 0
            title: "Tasks (" + view.tasks.length + ")"
            headerRight: [
                CButton { text: "Clear finished"; small: true; variant: "ghost"; onClicked: { api.post("/agents/tasks/clear", {}, function(ok) { if (ok) view.reloadTasks() }) } },
                CButton { text: "↻"; small: true; variant: "ghost"; onClicked: { view.reloadTasks() } }
            ]
            ListView {
                id: taskList
                anchors.fill: parent; clip: true; model: view.tasks
                ScrollBar.vertical: ScrollBar {}
                delegate: Rectangle {
                    width: taskList.width; height: 46; color: modelData.id === view.selTask ? Qt.rgba(0, 0.83, 1, 0.12) : (index % 2 ? Theme.bg2 : Theme.bg3)
                    MouseArea { anchors.fill: parent; onClicked: { view.selTask = modelData.id } }
                    ColumnLayout { anchors.fill: parent; anchors.leftMargin: 10; anchors.rightMargin: 6; anchors.topMargin: 4; anchors.bottomMargin: 4; spacing: 2
                        RowLayout { Layout.fillWidth: true; spacing: 6
                            CBadge { text: modelData.state || "?"; tint: view.stColor(modelData.state) }
                            CBadge { visible: !!modelData.risk; text: modelData.risk || ""; tint: view.riskColor(modelData.risk) }
                            Text { Layout.fillWidth: true; text: modelData.agentKind === "llm" ? (modelData.prompt || "prompt") : (modelData.command || ""); color: Theme.text0; font.pixelSize: Theme.fs12; font.bold: true; elide: Text.ElideRight }
                            CButton { visible: modelData.state === "running" || modelData.state === "queued"; text: "cancel"; small: true; variant: "warn"
                                onClicked: { api.del("/agents/tasks/" + encodeURIComponent(modelData.id), function(ok) { if (ok) view.reloadTasks() }) } ToolTip.text: "Cancel (DELETE)" }
                            CButton { visible: modelData.state !== "running" && modelData.state !== "queued"; text: "✕"; small: true; variant: "ghost"
                                onClicked: { api.del("/agents/tasks/" + encodeURIComponent(modelData.id), function(ok) { if (ok) view.reloadTasks() }) } ToolTip.text: "Delete" } }
                        RowLayout { Layout.fillWidth: true; spacing: 6
                            Text { text: modelData.agent || ""; color: view.kindColor(modelData.agentKind); font.pixelSize: Theme.fs10 }
                            Text { Layout.fillWidth: true; text: view.when(modelData.createdMs); color: Theme.text3; font.pixelSize: Theme.fs10; font.family: Theme.fontMono }
                            Text { text: view.dur(modelData); color: Theme.text2; font.pixelSize: Theme.fs10; font.family: Theme.fontMono } }
                    }
                }
                Text { anchors.centerIn: parent; visible: !view.tasks.length; text: "No tasks yet"; color: Theme.text3; font.pixelSize: Theme.fs11 }
            }
        }

        // ======================================================================= right: task detail
        CPanel {
            Layout.preferredWidth: 500; Layout.maximumWidth: 560; Layout.fillHeight: true; padding: 0
            title: view.task ? "Task " + view.task.id : "Task"
            Text { anchors.centerIn: parent; visible: !view.task; text: "Select a task"; color: Theme.text3; font.pixelSize: Theme.fs11 }
            ColumnLayout {
                anchors.fill: parent; anchors.margins: 10; spacing: 6; visible: !!view.task
                id: td
                property var t: view.task || ({})
                RowLayout { Layout.fillWidth: true; spacing: 6
                    CBadge { text: td.t.state || ""; tint: view.stColor(td.t.state); solid: true }
                    CBadge { visible: !!td.t.risk; text: td.t.risk || ""; tint: view.riskColor(td.t.risk) }
                    Text { Layout.fillWidth: true; text: (td.t.agent || "") + " · " + (td.t.command || ""); color: Theme.text0; font.pixelSize: Theme.fs12; font.bold: true; elide: Text.ElideRight }
                    Text { text: view.dur(td.t); color: Theme.text2; font.pixelSize: Theme.fs11; font.family: Theme.fontMono } }
                Text { Layout.fillWidth: true; visible: !!td.t.prompt; text: td.t.prompt || ""; color: Theme.text1; font.pixelSize: Theme.fs11; wrapMode: Text.WordWrap; maximumLineCount: 4; elide: Text.ElideRight }
                Text { Layout.fillWidth: true; visible: !!td.t.args && Object.keys(td.t.args || {}).length > 0; text: "args " + JSON.stringify(td.t.args || {}); color: Theme.text3; font.pixelSize: Theme.fs10; font.family: Theme.fontMono; elide: Text.ElideRight }
                Text { Layout.fillWidth: true; visible: !!td.t.error; text: td.t.error || ""; color: Theme.danger; font.pixelSize: Theme.fs11; wrapMode: Text.WordWrap }
                SecLabel { text: td.t.agentKind === "llm" ? "ANSWER" : "RESULT" }
                Rectangle {
                    Layout.fillWidth: true; Layout.fillHeight: true; Layout.minimumHeight: 160; color: Theme.bg1; radius: Theme.r5; border.color: Theme.border1
                    ScrollView {
                        anchors.fill: parent; anchors.margins: 4; clip: true
                        TextArea { readOnly: true; text: view.resultText(view.task); color: Theme.text0; font.family: view.task && view.task.agentKind === "llm" ? Theme.fontSans : Theme.fontMono; font.pixelSize: Theme.fs11; wrapMode: TextEdit.Wrap; background: null; selectByMouse: true }
                    }
                }
                SecLabel { visible: (td.t.steps || []).length > 0; text: "TOOL TRAIL (" + (td.t.steps || []).length + ")" }
                Rectangle {
                    visible: (td.t.steps || []).length > 0
                    Layout.fillWidth: true; Layout.preferredHeight: 200; color: Theme.bg1; radius: Theme.r5; border.color: Theme.border1
                    ListView {
                        id: stepList
                        anchors.fill: parent; anchors.margins: 4; clip: true; spacing: 4
                        model: view.task ? (view.task.steps || []) : []
                        ScrollBar.vertical: ScrollBar {}
                        delegate: ColumnLayout {
                            width: stepList.width - 8; spacing: 1
                            Text { Layout.fillWidth: true; text: (index + 1) + ". " + (modelData.tool || modelData.command || modelData.name || "step") + (modelData.args ? " " + JSON.stringify(modelData.args) : ""); color: Theme.accent0; font.pixelSize: Theme.fs10; font.family: Theme.fontMono; elide: Text.ElideRight }
                            Text { Layout.fillWidth: true; text: { var r = modelData.result !== undefined ? modelData.result : modelData.error; var s = typeof r === "string" ? r : JSON.stringify(r); return s ? (s.length > 400 ? s.substring(0, 400) + "…" : s) : "" }
                                color: modelData.error ? Theme.danger : Theme.text2; font.pixelSize: Theme.fs10; font.family: Theme.fontMono; wrapMode: Text.WrapAnywhere; maximumLineCount: 4; elide: Text.ElideRight }
                        }
                    }
                }
                RowLayout { Layout.fillWidth: true; spacing: 6
                    Text { Layout.fillWidth: true; text: "created " + view.when(td.t.createdMs) + (td.t.finishedMs ? "  · finished " + view.when(td.t.finishedMs) : ""); color: Theme.text3; font.pixelSize: Theme.fs10 }
                    CButton { text: "Copy result"; small: true; variant: "ghost"; onClicked: { window.copyText(view.resultText(view.task)) } } }
            }
        }
    }
}
