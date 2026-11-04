import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".."
import "../ui"

/**
 * Mesh: the multi-node media library. Two transports behind one console:
 *   - the AN-FS bridge (nodes, shares, mesh-wide search, download/upload)
 *   - StarAgent daemons on Star mesh nodes (file system browse/search, resumable fetch, chunked push)
 * Browse a node, search, fetch into the library (probed on arrival), push library media, watch transfers.
 */
Item {
    id: view
    property var status: ({})
    property var nodes: []          // [{id, name, kind: "anfs"|"star", online, ...}]
    property string node: ""
    property string nodeKind: "anfs"
    property string path: ""
    property var listing: null
    property var hits: []
    property var jobs: []
    property var media: []
    property string fetchType: "program"
    property var cur: { for (var i = 0; i < nodes.length; ++i) if (nodes[i].id === node) return nodes[i]; return {} }

    function reload() {
        api.get("/mesh/status", function(ok, d) { if (ok) view.status = d })
        api.get("/transfers", function(ok, d) { if (ok) view.jobs = d.jobs || [] })
        var merged = []
        api.get("/mesh/nodes", function(ok, d) {
            if (!ok) { view.status = Object.assign({}, view.status, { lastError: d && d.error ? d.error : "bridge unreachable" }) }
            else for (var i = 0; i < (d.nodes || []).length; ++i) { var n = d.nodes[i]; n.kind = "anfs"; n.label = n.machineName || n.hostname; merged.push(n) }
            api.get("/star/agents", function(ok2, d2) {
                if (ok2) for (var j = 0; j < (d2.agents || []).length; ++j) { var a = d2.agents[j]; merged.push({ id: "star:" + a.node, agentNode: a.node, kind: "star", label: a.name + " (agent)", ip: a.url, online: a.online, hasToken: a.hasToken, error: a.error, drives: [], sharedFolders: [] }) }
                view.nodes = merged
                if (!view.node && merged.length) { var f = null; for (var k = 0; k < merged.length; ++k) if (merged[k].online) { f = merged[k]; break } f = f || merged[0]; view.browse(f.id, f.sharedFolders && f.sharedFolders.length ? f.sharedFolders[0].path : "") }
            })
        })
        if (!view.media.length) api.get("/media", function(ok, d) { if (ok) view.media = (d.media || []).filter(function(m) { return m.type !== "live" }) })
    }
    function browse(n, p) {
        view.node = n; view.path = p || ""; view.hits = []; view.listing = null
        var c = null; for (var i = 0; i < nodes.length; ++i) if (nodes[i].id === n) c = nodes[i]
        view.nodeKind = c && c.kind === "star" ? "star" : "anfs"
        var url = view.nodeKind === "star" ? "/star/" + encodeURIComponent(c.agentNode) + "/list?path=" + encodeURIComponent(view.path) : "/mesh/browse?node=" + encodeURIComponent(n) + "&path=" + encodeURIComponent(view.path)
        api.get(url, function(ok, d) { view.listing = ok ? d : { error: d && d.error ? d.error : "browse failed", items: [] }; if (ok && d.currentPath !== undefined) view.path = d.currentPath })
    }
    function up() { var p = view.path.replace(/[\\/]+$/, ""); var i = Math.max(p.lastIndexOf("\\"), p.lastIndexOf("/")); browse(view.node, i > 1 ? p.slice(0, i + 1) : (p.length > 2 ? p.slice(0, 3) : "")) }
    function search(q) {
        if (!q.length) return
        var url = view.nodeKind === "star" ? "/star/" + encodeURIComponent(view.cur.agentNode) + "/search?path=" + encodeURIComponent(view.path || "C:\\") + "&q=" + encodeURIComponent(q) : "/mesh/search?q=" + encodeURIComponent(q)
        api.get(url, function(ok, d) { if (ok) { view.hits = d.hits || []; view.listing = { search: true, items: [] } } else window.toast(d && d.error ? d.error : "search failed", "error") })
    }
    function fetch(n, p) {
        var star = view.nodeKind === "star"
        api.post(star ? "/star/fetch" : "/mesh/fetch", star ? { node: view.cur.agentNode, path: p, type: view.fetchType } : { node: n, path: p, type: view.fetchType }, function(ok, d) { if (ok) { window.toast("Fetching " + d.name + (d.state === "done" ? " (already here)" : ""), "ok"); jobTimer.restart() } else window.toast(d && d.error ? d.error : "fetch failed", "error") })
    }
    function push(mediaId) {
        var target = pushNode.model[pushNode.currentIndex] || {}
        if (target.kind === "star") api.post("/star/push", { mediaId: mediaId, node: target.agentNode, remotePath: pushFolder.text }, function(ok, d) { if (ok) { window.toast("Pushing " + d.name, "ok"); jobTimer.restart() } else window.toast(d && d.error ? d.error : "push failed", "error") })
        else api.post("/mesh/push", { mediaId: mediaId, node: target.id || view.node, folder: pushFolder.text }, function(ok, d) { if (ok) { window.toast("Pushing " + d.name, "ok"); jobTimer.restart() } })
    }
    function fmtB(b) { return b > 1e9 ? (b / 1e9).toFixed(2) + " GB" : b > 1e6 ? (b / 1e6).toFixed(1) + " MB" : b > 1e3 ? Math.round(b / 1e3) + " kB" : (b || 0) + " B" }
    Component.onCompleted: { reload() }
    Timer { id: jobTimer; interval: 1200; onTriggered: { api.get("/transfers", function(ok, d) { if (ok) view.jobs = d.jobs || [] }) } }
    Timer { interval: 2500; running: true; repeat: true; onTriggered: { var active = false; for (var i = 0; i < view.jobs.length; ++i) if (view.jobs[i].state === "running" || view.jobs[i].state === "queued") active = true; if (active) jobTimer.restart() } }

    RowLayout {
        anchors.fill: parent; spacing: 10
        // ------------- nodes -------------
        CPanel {
            Layout.preferredWidth: 280; Layout.fillHeight: true; padding: 0
            title: "Mesh nodes (" + view.nodes.length + ")"
            headerRight: [
                CButton { text: "probe agents"; small: true; variant: "ghost"; onClicked: { api.post("/star/probe", {}, function() { view.nodes = []; view.reload() }) } ToolTip.text: "Health-check every StarAgent" },
                CButton { text: "\u21BB"; small: true; variant: "ghost"; onClicked: { view.nodes = []; view.listing = null; view.reload() } }
            ]
            ColumnLayout {
                anchors.fill: parent; spacing: 0
                ListView {
                    Layout.fillWidth: true; Layout.fillHeight: true; clip: true
                    model: view.nodes
                    delegate: Rectangle {
                        width: ListView.view.width; height: modelData.kind === "star" ? 44 : 56; color: modelData.id === view.node ? Qt.rgba(0, 0.83, 1, 0.12) : (index % 2 ? Theme.bg2 : Theme.bg3)
                        MouseArea { anchors.fill: parent; onClicked: { view.browse(modelData.id, modelData.sharedFolders && modelData.sharedFolders.length ? modelData.sharedFolders[0].path : "") } }
                        RowLayout {
                            anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 8; spacing: 8
                            Rectangle { width: 8; height: 8; radius: 4; color: modelData.online ? Theme.ready : Theme.danger; Layout.alignment: Qt.AlignTop; Layout.topMargin: 10 }
                            ColumnLayout { Layout.fillWidth: true; spacing: 1
                                RowLayout { spacing: 6
                                    Text { text: modelData.label; color: Theme.text0; font.pixelSize: Theme.fs12; font.bold: true; elide: Text.ElideRight; Layout.fillWidth: true }
                                    CBadge { text: modelData.kind === "star" ? "star" : "an-fs"; tint: modelData.kind === "star" ? Theme.accent2 : Theme.accent0 } }
                                Text { text: modelData.ip + (modelData.isLocal ? " \u00B7 bridge" : "") + (modelData.relay ? " \u00B7 relay" : "") + (modelData.kind === "star" && !modelData.hasToken ? " \u00B7 no token" : "") + (modelData.error ? "  " + modelData.error : ""); color: modelData.error ? Theme.danger : Theme.text3; font.pixelSize: Theme.fs10; font.family: Theme.fontMono; elide: Text.ElideRight; Layout.fillWidth: true }
                                Flow { visible: modelData.kind !== "star"; Layout.fillWidth: true; spacing: 4
                                    Repeater { model: modelData.drives || []
                                        Rectangle { width: 54; height: 14; radius: 3; color: Theme.bg1
                                            Text { anchors.left: parent.left; anchors.leftMargin: 4; anchors.verticalCenter: parent.verticalCenter; text: modelData.letter; color: Theme.text2; font.pixelSize: 9 }
                                            Rectangle { anchors.right: parent.right; anchors.rightMargin: 4; anchors.verticalCenter: parent.verticalCenter; height: 4; radius: 2; color: Theme.accent0; width: modelData.sizeGb ? Math.round(26 * (1 - modelData.freeGb / modelData.sizeGb)) : 0 } } }
                                }
                            }
                        }
                    }
                    Text { anchors.centerIn: parent; visible: !view.nodes.length; width: parent.width - 30; wrapMode: Text.WordWrap; horizontalAlignment: Text.AlignHCenter; color: Theme.text3; font.pixelSize: Theme.fs11; text: view.status.lastError || "loading\u2026" }
                }
                Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border0 }
                ColumnLayout {
                    Layout.fillWidth: true; Layout.margins: 10; spacing: 6
                    CField { id: meshUrl; label: "AN-FS bridge URL"; Layout.fillWidth: true; text: view.status.url || ""; mono: true }
                    RowLayout { Layout.fillWidth: true; spacing: 6
                        CField { id: meshToken; label: view.status.hasToken ? "Mesh token (set)" : "Mesh token (uploads)"; Layout.fillWidth: true; inputMethodHints: Qt.ImhHiddenText | Qt.ImhSensitiveData }
                        CButton { text: "Save"; small: true; variant: "primary"; Layout.alignment: Qt.AlignBottom; onClicked: { var b = { url: meshUrl.text }; if (meshToken.text.length) b.token = meshToken.text; api.put("/mesh/config", b, function(ok, d) { if (ok) { view.status = d; window.toast("Mesh settings saved", "ok"); view.nodes = []; view.listing = null; view.reload() } }) } }
                    }
                    RowLayout { Layout.fillWidth: true; spacing: 6
                        CField { id: agentNode; label: "StarAgent node id"; Layout.preferredWidth: 90; placeholder: "dev" }
                        CField { id: agentUrl; label: "URL"; Layout.fillWidth: true; placeholder: "http://100.x.y.z:8780"; mono: true }
                        CField { id: agentToken; label: "Token"; Layout.preferredWidth: 90; inputMethodHints: Qt.ImhHiddenText | Qt.ImhSensitiveData }
                        CButton { text: "Add"; small: true; Layout.alignment: Qt.AlignBottom; enabled: agentNode.text.length > 0 && agentUrl.text.length > 0
                            onClicked: { api.get("/star/agents", function(ok, d) { var list = ok ? (d.agents || []) : []; var out = []; for (var i = 0; i < list.length; ++i) if (list[i].configured) out.push({ node: list[i].node, name: list[i].name, url: list[i].url }); out.push({ node: agentNode.text, name: agentNode.text, url: agentUrl.text, token: agentToken.text }); api.put("/star/agents", { agents: out }, function(ok2) { if (ok2) { window.toast("Agent saved", "ok"); agentToken.text = ""; view.nodes = []; view.reload() } }) }) } }
                    }
                    Text { Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.text3; font.pixelSize: Theme.fs10; text: "Fetched files land in " + (view.status.downloadRoot || "") + " (bridge) or its star\\ sibling (agents) and are probed into the library." }
                }
            }
        }
        // ------------- browser + transfers -------------
        ColumnLayout {
            Layout.fillWidth: true; Layout.fillHeight: true; spacing: 10
            CPanel {
                Layout.fillWidth: true; Layout.fillHeight: true; Layout.minimumHeight: 260; padding: 0
                title: (view.cur.label || "Browse") + (view.nodeKind === "star" ? "  \u2014 StarAgent file system" : "")
                headerRight: [
                    CField { id: meshQ; implicitWidth: 220; placeholder: view.nodeKind === "star" ? "search under this folder" : "search the whole mesh"; Keys.onReturnPressed: { view.search(meshQ.text) } },
                    CButton { text: "Search"; small: true; onClicked: { view.search(meshQ.text) } }
                ]
                ColumnLayout {
                    anchors.fill: parent; spacing: 0
                    RowLayout {
                        Layout.fillWidth: true; Layout.margins: 8; spacing: 6
                        CButton { text: "\u2191"; small: true; variant: "ghost"; onClicked: { view.up() } }
                        Repeater { model: view.cur.sharedFolders || []; CButton { text: modelData.name; small: true; variant: "ghost"; onClicked: { view.browse(view.cur.id, modelData.path) } } }
                        CField { id: pathField; Layout.fillWidth: true; text: view.path; mono: true; Keys.onReturnPressed: { view.browse(view.node, pathField.text) } }
                        CButton { text: "Go"; small: true; onClicked: { view.browse(view.node, pathField.text) } }
                        CSelect { label: "Import as"; Layout.preferredWidth: 120; model: ["program", "ad", "promo", "station_id", "bumper", "filler", "psa", "image"]; onActivated: function(i) { view.fetchType = model[i] } }
                    }
                    Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border0 }
                    ListView {
                        id: fileList
                        Layout.fillWidth: true; Layout.fillHeight: true; clip: true
                        model: view.listing ? (view.listing.search ? view.hits : (view.listing.items || [])) : []
                        delegate: Rectangle {
                            width: ListView.view.width; height: 30; color: index % 2 ? Theme.bg2 : Theme.bg3
                            MouseArea { anchors.fill: parent; enabled: !!modelData.isDirectory; onClicked: { view.browse(view.node, modelData.path) } cursorShape: modelData.isDirectory ? Qt.PointingHandCursor : Qt.ArrowCursor }
                            RowLayout {
                                anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 8; spacing: 10
                                Text { text: modelData.isDirectory ? "\u25B8" : "\u2022"; color: modelData.isDirectory ? Theme.accent0 : Theme.text3; font.pixelSize: Theme.fs12; Layout.preferredWidth: 12 }
                                Text { text: modelData.name; color: Theme.text0; font.pixelSize: Theme.fs12; font.bold: !!modelData.isDirectory; elide: Text.ElideMiddle; Layout.fillWidth: true }
                                Text { visible: !!(view.listing && view.listing.search); text: modelData.nodeName || modelData.nodeId || ""; color: Theme.text2; font.pixelSize: Theme.fs11; Layout.preferredWidth: 110; elide: Text.ElideRight }
                                CBadge { visible: !!(modelData.swarmNodes && modelData.swarmNodes.length); text: (modelData.swarmNodes ? modelData.swarmNodes.length : 0) + " copies"; tint: Theme.accent2 }
                                Text { text: modelData.isDirectory ? (modelData.drive && modelData.free ? view.fmtB(modelData.free) + " free" : "") : view.fmtB(modelData.size); color: Theme.text2; font.pixelSize: Theme.fs11; font.family: Theme.fontMono; Layout.preferredWidth: 80; horizontalAlignment: Text.AlignRight }
                                Text { text: modelData.mtime ? new Date(modelData.mtime).toLocaleDateString(Qt.locale(), Locale.ShortFormat) : ""; color: Theme.text3; font.pixelSize: Theme.fs10; Layout.preferredWidth: 70 }
                                CButton { visible: !modelData.isDirectory; text: "\u2913 Fetch"; small: true; onClicked: { view.fetch(modelData.nodeId || view.node, modelData.path) } }
                            }
                        }
                        Text { anchors.centerIn: parent; visible: fileList.count === 0; width: parent.width - 40; wrapMode: Text.WordWrap; horizontalAlignment: Text.AlignHCenter; color: view.listing && view.listing.error ? Theme.danger : Theme.text3; font.pixelSize: Theme.fs11
                            text: !view.listing ? "loading\u2026" : view.listing.error ? view.listing.error : view.listing.search ? "No matches" : "Empty folder" }
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true; Layout.preferredHeight: 230; Layout.maximumHeight: 230; spacing: 10
                CPanel {
                    Layout.fillWidth: true; Layout.fillHeight: true; padding: 0
                    title: "Transfers (" + view.jobs.length + ")"
                    headerRight: [ CButton { text: "clear finished"; small: true; variant: "ghost"; onClicked: { api.post("/transfers/clear", {}, function() { jobTimer.restart() }) } } ]
                    ListView {
                        anchors.fill: parent; clip: true
                        model: view.jobs.slice().reverse()
                        delegate: Rectangle {
                            width: ListView.view.width; height: 44; color: index % 2 ? Theme.bg2 : Theme.bg3
                            property var j: modelData
                            RowLayout {
                                anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 8; spacing: 10
                                Text { text: j.kind === "fetch" ? "\u2913" : "\u2912"; color: Theme.text2; font.pixelSize: 14 }
                                ColumnLayout { Layout.preferredWidth: 220; spacing: 1
                                    Text { text: j.name; color: Theme.text0; font.pixelSize: Theme.fs12; font.bold: true; elide: Text.ElideMiddle; Layout.fillWidth: true }
                                    Text { text: (j.via === "star" ? "agent " : "") + (j.nodeName || j.node || "server") + (j.kind === "fetch" ? ": " + j.remotePath : " \u2190 " + j.localPath); color: Theme.text3; font.pixelSize: Theme.fs10; elide: Text.ElideMiddle; Layout.fillWidth: true } }
                                Rectangle { Layout.fillWidth: true; height: 8; radius: 2; color: Theme.bg4
                                    Rectangle { height: parent.height; radius: 2; width: parent.width * (j.total ? Math.min(1, j.bytes / j.total) : (j.state === "done" ? 1 : 0)); color: j.state === "failed" ? Theme.danger : j.state === "done" ? Theme.ready : j.state === "cancelled" ? Theme.text3 : Theme.accent0 } }
                                Text { text: view.fmtB(j.bytes) + (j.total ? " / " + view.fmtB(j.total) : "") + (j.state === "running" && j.rateBps ? "  " + view.fmtB(j.rateBps) + "/s" : ""); color: Theme.text2; font.pixelSize: Theme.fs10; font.family: Theme.fontMono; Layout.preferredWidth: 170 }
                                CBadge { text: j.state; tint: j.state === "done" ? Theme.ready : j.state === "failed" ? Theme.danger : j.state === "running" ? Theme.accent0 : Theme.text2; ToolTip.text: j.error || "" }
                                CButton { text: "\u2715"; small: true; variant: "ghost"; onClicked: { api.del("/transfers/" + j.id, function() { jobTimer.restart() }) } }
                            }
                        }
                        Text { anchors.centerIn: parent; visible: !view.jobs.length; color: Theme.text3; font.pixelSize: Theme.fs11; text: "No transfers yet" }
                    }
                }
                CPanel {
                    Layout.preferredWidth: 520; Layout.fillHeight: true; padding: 0
                    title: "Push library media"
                    headerRight: [
                        CSelect { id: pushNode; implicitWidth: 170; model: view.nodes; textRole: "label" },
                        CField { id: pushFolder; implicitWidth: 170; placeholder: "folder (default: drop folder)" }
                    ]
                    ListView {
                        anchors.fill: parent; clip: true; model: view.media
                        delegate: Rectangle {
                            width: ListView.view.width; height: 30; color: index % 2 ? Theme.bg2 : Theme.bg3
                            RowLayout { anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 8; spacing: 8
                                Text { text: modelData.title; color: Theme.text0; font.pixelSize: Theme.fs12; elide: Text.ElideRight; Layout.fillWidth: true }
                                Text { text: modelData.fileSizeBytes ? view.fmtB(modelData.fileSizeBytes) : ""; color: Theme.text3; font.pixelSize: Theme.fs10; font.family: Theme.fontMono }
                                CButton { text: "\u2912 Push"; small: true; onClicked: { view.push(modelData.id) } } }
                        }
                        Text { anchors.centerIn: parent; visible: !view.media.length; color: Theme.text3; font.pixelSize: Theme.fs11; text: "Library is empty" }
                    }
                }
            }
        }
    }
}
