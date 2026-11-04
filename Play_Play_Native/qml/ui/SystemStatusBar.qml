import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".."

/**
 * Bottom status bar, always visible: per-channel measured frame rate and output bitrate / drops, CPU and memory (machine, Supervisor,
 * engines) and network in / out. Everything comes from GET /api/v1/system/stats, which the Supervisor samples once a second from the
 * engines' telemetry, GetSystemTimes / GlobalMemoryStatusEx / GetProcessTimes and GetIfTable2. Polled with XMLHttpRequest on purpose: a
 * 1.5 s poll through `api` would keep its busy indicator and its error toasts running.
 */
Rectangle {
    id: bar
    property var stats: ({})
    property bool ok: false
    property int failures: 0
    readonly property var channels: stats.channels || []
    readonly property var cpu: stats.cpu || ({})
    readonly property var mem: stats.memory || ({})
    readonly property var net: stats.net || ({})
    readonly property var totals: stats.totals || ({})
    // every configured channel, in the app's order; a channel whose engine has not reported shows as offline
    readonly property var rows: {
        var out = [], byId = {}, sc = supervisor.channels
        for (var i = 0; i < channels.length; ++i) byId[channels[i].id] = channels[i]
        for (var k = 0; k < sc.length; ++k) { var id = sc[k].profile.id; out.push(byId[id] || { id: id, stale: true, state: "OFFLINE" }); delete byId[id] }
        for (var rest in byId) out.push(byId[rest])
        return out
    }

    implicitHeight: 28
    color: Theme.bg1
    Rectangle { anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right; height: 1; color: Theme.border1 }

    function bitsText(bps) {                           // network: bytes/s in, "12.3 Mb/s" out
        var b = Math.max(0, bps * 8)
        if (b >= 1e9) return (b / 1e9).toFixed(2) + " Gb/s"
        if (b >= 1e6) return (b / 1e6).toFixed(b >= 1e8 ? 0 : 1) + " Mb/s"
        if (b >= 1e3) return (b / 1e3).toFixed(0) + " kb/s"
        return b.toFixed(0) + " b/s"
    }
    function kbpsText(kbps) {
        if (!(kbps > 0)) return "—"
        return kbps >= 1000 ? (kbps / 1000).toFixed(kbps >= 10000 ? 1 : 2) + " Mb/s" : Math.round(kbps) + " kb/s"
    }
    function gbText(mb) { return (mb / 1024).toFixed(1) }
    function pctColor(p) { return p >= 90 ? Theme.danger : p >= 75 ? Theme.standby : Theme.text0 }
    function poll() {
        var x = new XMLHttpRequest()
        x.onreadystatechange = function() {
            if (x.readyState !== XMLHttpRequest.DONE) return
            if (x.status === 200) {
                try { bar.stats = JSON.parse(x.responseText); bar.ok = true; bar.failures = 0; return } catch (e) { }
            }
            if (++bar.failures >= 2) bar.ok = false
        }
        x.open("GET", api.rawUrl("/api/v1/system/stats"))
        if (api.token) x.setRequestHeader("Authorization", "Bearer " + api.token)
        x.send()
    }
    Timer { interval: 1500; running: true; repeat: true; triggeredOnStart: true; onTriggered: { bar.poll() } }
    Connections { target: api; function onBaseUrlChanged() { bar.poll() } }

    component Sep: Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 14; color: Theme.border1 }
    component Key: Text { color: Theme.text3; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1 }
    component Val: Text { color: Theme.text0; font.family: Theme.fontMono; font.pixelSize: Theme.fs11 }
    component Meter: Rectangle {
        property real frac: 0
        property color tint: Theme.accent0
        Layout.preferredWidth: 34; Layout.preferredHeight: 6; radius: 3; color: Theme.bg4
        Rectangle { width: parent.width * Math.max(0, Math.min(1, parent.frac)); height: parent.height; radius: 3; color: parent.tint }
    }

    RowLayout {
        anchors.fill: parent; anchors.leftMargin: 10; anchors.rightMargin: 10; spacing: 10

        // supervisor link
        Rectangle { width: 8; height: 8; radius: 4; color: supervisor.connected && bar.ok ? Theme.ready : supervisor.connected ? Theme.standby : Theme.danger }
        Key { text: supervisor.connected ? (bar.ok ? "SUPERVISOR" : "NO STATS") : "NO SUPERVISOR" }
        Sep {}

        // channels: fps · bitrate · drops
        Repeater {
            model: bar.rows
            RowLayout {
                spacing: 6
                readonly property bool stale: !!modelData.stale
                readonly property real fps: modelData.fps || 0
                readonly property real nominal: modelData.nominalFps || 0
                readonly property bool slow: !stale && nominal > 0 && fps < nominal * 0.97
                Rectangle { width: 6; height: 6; radius: 3; color: stale ? Theme.text3 : Theme.stateColor(modelData.state) }
                Key { text: String(modelData.id).toUpperCase() }
                Val { text: stale ? "offline" : fps.toFixed(2) + " fps"; color: stale ? Theme.text3 : slow ? Theme.standby : Theme.text0 }
                Key { visible: !stale; text: "OUT" }
                Val { visible: !stale; text: bar.kbpsText(modelData.bitrateKbps); color: Theme.text0 }
                Text { visible: (modelData.outputCount || 0) > 0 && (modelData.outputsUp || 0) < modelData.outputCount; text: "▲" + modelData.outputsUp + "/" + modelData.outputCount; color: Theme.standby; font.pixelSize: Theme.fs10; font.bold: true }
                Text { visible: (modelData.drops || 0) > 0; text: "DROP " + Math.round(modelData.drops); color: Theme.danger; font.pixelSize: Theme.fs10; font.bold: true }
                Sep {}
                HoverHandler { id: chHover }
                ToolTip.visible: chHover.hovered
                ToolTip.text: String(modelData.id).toUpperCase() + "  " + modelData.width + "x" + modelData.height + " @ " + nominal.toFixed(2) + " nominal"
                              + "\ntick " + (modelData.tickMs || 0).toFixed(1) + " ms (max " + (modelData.tickMsMax || 0).toFixed(0) + ")  late " + (modelData.lateTicks || 0)
                              + (modelData.cpu !== undefined ? "\nengine CPU " + modelData.cpu + "%  " + modelData.memMB + " MB" : "")
                              + (modelData.outputs && modelData.outputs.length ? "\n" + modelData.outputs.map(function(o) { return o.name + " (" + o.kind + ") " + bar.kbpsText(o.bitrateKbps) + (o.dropped ? " drops " + o.dropped : "") + (o.connected === false ? " DOWN" : "") }).join("\n") : "")
            }
        }
        Text { visible: bar.rows.length === 0; text: supervisor.connected ? "no channels" : ""; color: Theme.text3; font.pixelSize: Theme.fs10 }

        Item { Layout.fillWidth: true }

        // CPU
        Key { text: "CPU" }
        Val { text: bar.cpu.systemPct !== undefined ? Math.round(bar.cpu.systemPct) + "%" : "—"; color: bar.pctColor(bar.cpu.systemPct || 0) }
        Meter { frac: (bar.cpu.systemPct || 0) / 100; tint: bar.pctColor(bar.cpu.systemPct || 0) === Theme.text0 ? Theme.accent0 : bar.pctColor(bar.cpu.systemPct || 0) }
        Text { text: "SUP " + (bar.cpu.supervisorPct !== undefined ? bar.cpu.supervisorPct.toFixed(1) : "—") + "  ENG " + (bar.cpu.enginesPct !== undefined ? bar.cpu.enginesPct.toFixed(1) : "—") + " %"; color: Theme.text2; font.family: Theme.fontMono; font.pixelSize: Theme.fs10 }
        Sep {}
        // memory
        Key { text: "MEM" }
        Val { text: bar.mem.totalMB ? bar.gbText(bar.mem.usedMB) + " / " + bar.gbText(bar.mem.totalMB) + " GB" : "—"; color: bar.pctColor(bar.mem.usedPct || 0) }
        Meter { frac: (bar.mem.usedPct || 0) / 100; tint: bar.pctColor(bar.mem.usedPct || 0) === Theme.text0 ? Theme.accent2 : bar.pctColor(bar.mem.usedPct || 0) }
        Text { text: "SUP " + Math.round(bar.mem.supervisorMB || 0) + "  ENG " + Math.round(bar.mem.enginesMB || 0) + " MB"; color: Theme.text2; font.family: Theme.fontMono; font.pixelSize: Theme.fs10 }
        Sep {}
        // network
        Key { text: "NET" }
        Val { text: "↓ " + bar.bitsText(bar.net.inBps || 0); color: Theme.ready }
        Val { text: "↑ " + bar.bitsText(bar.net.outBps || 0); color: Theme.accent0 }
    }
}
