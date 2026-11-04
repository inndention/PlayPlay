import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".."

/// Facebook Live: stream key box + channel + quality, GO LIVE / END LIVE. Talks to /api/v1/live/facebook (docs/mux.md): the Supervisor keeps the
/// key (write-only: it answers "key saved …abcd" and never the key itself) and publishes one RTMPS output on the chosen channel.
CPanel {
    id: p
    title: "Facebook Live"
    implicitHeight: 34 + col.implicitHeight + 24
    property var st: ({})
    property var chans: []                   // [{id, name}]
    property var presets: []
    property bool dirty: false
    property string channelSel: ""
    property string presetSel: ""
    property string serverSel: ""
    property bool showKey: false
    readonly property bool live: st.live === true
    readonly property string state: st.state || "idle"

    function stateColor(s) { return s === "running" ? Theme.ready : s === "error" || s === "offline" ? Theme.danger : s === "starting" || s === "pending" ? Theme.standby : Theme.text2 }
    function fmtUp(s) { s = Math.floor(s || 0); return s >= 3600 ? Math.floor(s / 3600) + "h " + Math.floor(s % 3600 / 60) + "m" : s >= 60 ? Math.floor(s / 60) + "m " + (s % 60) + "s" : s + "s" }
    function adopt(d) {
        st = d
        if (!dirty) { channelSel = d.channel || ""; presetSel = d.preset || ""; serverSel = d.serverUrl || "" }
    }
    function load() {
        api.get("/live/facebook", function(ok, d) { if (ok) p.adopt(d) })
        api.get("/outputs", function(ok, d) { if (!ok) return; var a = []; (d.channels || []).forEach(function(c) { if (!c.hidden) a.push({ id: c.channelId, name: c.channelName || c.channelId }) }); p.chans = a })
        if (presets.length === 0) api.get("/mux/encoder-presets", function(ok, d) { if (ok) p.presets = (d.presets || []).map(function(x) { return x.id }).filter(function(id) { return /^stream_|h264/.test(id) }) })
    }
    function save() {
        var b = { channel: channelSel, serverUrl: serverSel, preset: presetSel }
        if (keyField.text.length > 0) b.streamKey = keyField.text
        api.put("/live/facebook", b, function(ok, d) {
            window.toast(ok ? "Facebook Live saved" + (d.hasKey ? " (key " + d.keyHint + ")" : "") : (d && d.error ? d.error : "save failed"), ok ? "ok" : "error")
            if (ok) { p.dirty = false; keyField.text = ""; p.adopt(d) }
        })
    }
    function forgetKey() { api.put("/live/facebook", { clearKey: true }, function(ok, d) { window.toast(ok ? "Key forgotten" : (d && d.error ? d.error : "failed"), ok ? "ok" : "error"); if (ok) p.adopt(d) }) }
    function op(what) { api.post("/live/facebook/" + what, {}, function(ok, d) { window.toast(ok ? (what === "start" ? "Connecting to Facebook…" : "Facebook stream stopped") : (d && d.error ? d.error : what + " failed"), ok ? "ok" : "error"); Qt.callLater(function() { p.load() }) }) }
    function idxOf(list, v) { for (var i = 0; i < list.length; i++) if (list[i] === v) return i; return -1 }

    Component.onCompleted: load()
    Timer { interval: 3000; repeat: true; running: p.visible; onTriggered: p.load() }

    ColumnLayout {
        id: col
        anchors.left: parent.left; anchors.right: parent.right; anchors.margins: 12
        spacing: 8
        RowLayout { Layout.fillWidth: true; spacing: 8
            CBadge { text: p.live ? "LIVE" : p.state; tint: p.live ? Theme.onAir : p.stateColor(p.state); solid: p.live }
            Text { visible: p.live; color: Theme.text1; font.family: Theme.fontMono; font.pixelSize: Theme.fs11
                text: p.fmtUp(p.st.uptimeSec) + "  ·  " + Math.round(p.st.bitrateKbps || 0) + " kb/s" + (p.st.width ? "  ·  " + p.st.width + "×" + p.st.height : "") }
            Text { visible: p.st.retryInSec !== undefined; color: Theme.standby; font.pixelSize: Theme.fs11; text: "retrying in " + p.st.retryInSec + " s (attempt " + p.st.attempts + ")" }
            Item { Layout.fillWidth: true }
            CButton { visible: p.live; text: "END LIVE"; variant: "danger"; onClicked: p.op("stop") }
            CButton { visible: !p.live; text: "GO LIVE"; variant: "success"; enabled: !!p.st.hasKey && p.channelSel.length > 0 && !!p.st.engineConnected && !p.dirty; onClicked: p.op("start") }
        }
        Text { visible: !!p.st.lastError; Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.danger; font.pixelSize: Theme.fs11; text: p.st.lastError || "" }
        RowLayout { Layout.fillWidth: true; spacing: 8
            CSelect { label: "Channel"; Layout.preferredWidth: 160
                model: p.chans.map(function(c) { return c.name }); currentIndex: { for (var i = 0; i < p.chans.length; i++) if (p.chans[i].id === p.channelSel) return i; return -1 }
                onActivated: function(i) { p.channelSel = p.chans[i].id; p.dirty = true } }
            CField { id: keyField; label: "Stream key"; Layout.fillWidth: true; mono: true; echoMode: p.showKey ? TextInput.Normal : TextInput.Password
                placeholder: p.st.hasKey ? "saved " + p.st.keyHint + " — type to replace" : "paste the key from Facebook Live Producer"
                onEdited: function(v) { p.dirty = true } }
            CButton { text: p.showKey ? "Hide" : "Show"; small: true; variant: "ghost"; Layout.alignment: Qt.AlignBottom; onClicked: { p.showKey = !p.showKey } }
        }
        RowLayout { Layout.fillWidth: true; spacing: 8
            CSelect { label: "Quality"; Layout.preferredWidth: 220
                model: p.presets.length ? p.presets : [p.presetSel || "stream_h264_aac_720p"]; currentIndex: Math.max(0, p.idxOf(p.presets, p.presetSel))
                onActivated: function(i) { p.presetSel = (p.presets.length ? p.presets : [p.presetSel])[i]; p.dirty = true } }
            CField { label: "Server"; Layout.fillWidth: true; mono: true; text: p.serverSel; onEdited: function(v) { p.serverSel = v; p.dirty = true } }
            CButton { text: "Save"; variant: p.dirty ? "primary" : "default"; enabled: p.dirty; Layout.alignment: Qt.AlignBottom; onClicked: p.save() }
            CButton { visible: !!p.st.hasKey; text: "Forget key"; variant: "ghost"; Layout.alignment: Qt.AlignBottom; onClicked: p.forgetKey() }
        }
        Text { Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.text3; font.pixelSize: Theme.fs10
            text: "Create the live video in Facebook (Live Producer → Streaming software), paste its stream key, Save, then GO LIVE here and Go live on Facebook once its preview shows. The key is kept by the Supervisor and never shown again." }
    }
}
