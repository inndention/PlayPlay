import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".."
import "../ui"

/**
 * Output: the active channel's streaming outputs manager (DekTec ASI/SDI, NDI, UDP/SRT/RTMP/file).
 * Each output has a live state badge (running / stopped / error / disabled / pending), bitrate and
 * counters from the engine, Start / Stop / Restart that touch only that output, an autoStart policy
 * (start with the engine) and retry settings, plus Add / Remove. Edits of one output go through
 * PUT /channels/{id}/outputs/{name}; the engine diffs the output list by name, so untouched outputs
 * keep running. Data: GET /channels/{id}/outputs (definitions merged with telemetry).
 */
Item {
    id: view
    property string chId: supervisor.activeChannelId
    property var profile: { var c = supervisor.channels; return supervisor.channelOf(chId).profile || ({}) }
    property var merged: ({})
    property var outputs: []
    property int sel: -1
    property var edit: null                 // working copy of the selected output while editing
    property bool dirty: false
    property var presets: []
    readonly property var types: ["dektec_asi", "dektec_sdi", "decklink", "ndi", "udp", "srt", "rtmp", "file", "null"]
    function load() { api.get("/channels/" + encodeURIComponent(chId) + "/outputs", function(ok, d) { if (!ok) return; view.merged = d; view.outputs = d.outputs || []; if (view.sel >= view.outputs.length) view.sel = view.outputs.length - 1; if (view.sel < 0 && view.outputs.length) view.sel = 0; if (!view.dirty) view.edit = view.sel >= 0 ? JSON.parse(JSON.stringify(view.outputs[view.sel])) : null }) }
    onChIdChanged: { sel = -1; dirty = false; edit = null; load() }
    onSelChanged: { dirty = false; edit = sel >= 0 && sel < outputs.length ? JSON.parse(JSON.stringify(outputs[sel])) : null }
    Component.onCompleted: { load(); api.get("/mux/encoder-presets", function(ok, d) { if (ok) view.presets = (d.presets || []).map(function(p) { return p.id }) }) }
    Timer { interval: 3000; running: true; repeat: true; onTriggered: { view.load() } }
    Connections { target: supervisor; function onUiEvent(type, payload) { if (type === "mux") view.load() } }
    function set(k, v) { if (!edit) return; var e = JSON.parse(JSON.stringify(edit)); if (v === "" || v === undefined) delete e[k]; else e[k] = v; edit = e; dirty = true }
    function setOpt(k, v) { if (!edit) return; var o = Object.assign({}, edit.options || {}); if (v === "" || v === undefined) delete o[k]; else o[k] = v; set("options", o) }
    function setProf(k, v) { if (!edit) return; var p = typeof edit.profile === "string" ? { preset: edit.profile } : Object.assign({}, edit.profile || {}); if (v === "" || v === undefined) delete p[k]; else p[k] = v; set("profile", p) }
    function setRetry(k, v) { if (!edit) return; var r = Object.assign({ enabled: true, maxAttempts: 10, backoffSec: 5 }, edit.retry || {}); r[k] = v; set("retry", r) }
    function apply() {
        if (!edit || sel < 0) return; var name = (outputs[sel] || {}).name; var body = JSON.parse(JSON.stringify(edit)); delete body.live; delete body.state
        api.put("/channels/" + encodeURIComponent(chId) + "/outputs/" + encodeURIComponent(name), body, function(ok, d) { window.toast(ok ? "Output " + name + " updated" : (d && d.error ? d.error : "update failed"), ok ? "ok" : "error"); view.dirty = false; view.load() })
    }
    function op(name, what) { api.post("/channels/" + encodeURIComponent(chId) + "/outputs/" + encodeURIComponent(name) + "/" + what, {}, function(ok, d) { window.toast(ok ? name + ": " + what : (d && d.error ? d.error : what + " failed"), ok ? "ok" : "error"); view.load() }) }
    function add(type) {
        var o = { type: type, name: type, enabled: true, autoStart: true, retry: { enabled: true, maxAttempts: 10, backoffSec: 5 } }
        if (type === "dektec_asi") { o.options = { port: 2, muxrate: 19392658 }; o.profile = { preset: "atsc_mpeg2_ac3_hd" } }
        else if (type === "dektec_sdi") { o.options = { port: 2, videoStd: "auto", audioChannels: 2 } }
        else if (type === "decklink") { o.options = { deviceIndex: 0, videoMode: "auto", audioChannels: 2, keyer: "off", keyLevel: 255 } }
        else if (type === "ndi") { o.name = profile.name || "PLAY PLAY"; o.sourceName = profile.name || "" }
        else if (type === "udp") { o.url = "udp://239.1.1.1:1234?pkt_size=1316"; o.profile = { preset: "h264_aac", videoBitrateKbps: 6000 }; o.options = { muxrate: 8000000 } }
        else if (type === "srt") { o.url = "srt://0.0.0.0:9000?mode=listener"; o.profile = { preset: "stream_h264_aac_1080p" } }
        else if (type === "rtmp") { o.url = "rtmp://localhost/live/stream"; o.profile = { preset: "stream_h264_aac_720p" } }
        else if (type === "file") { o.url = "C:/PlayPlay/record/" + chId + ".ts"; o.profile = { preset: "h264_aac" } }
        api.post("/channels/" + encodeURIComponent(chId) + "/outputs/add", { output: o }, function(ok, d) { window.toast(ok ? "Added " + type : (d && d.error ? d.error : "add failed"), ok ? "ok" : "error"); if (ok) { view.sel = (d.outputs || []).length - 1 } view.load() })
    }
    function remove(i) { var name = (outputs[i] || {}).name; if (!name) return; api.del("/channels/" + encodeURIComponent(chId) + "/outputs/" + encodeURIComponent(name), function(ok, d) { window.toast(ok ? "Removed " + name : (d && d.error ? d.error : "remove failed"), ok ? "ok" : "error"); view.load() }) }
    function stateColor(st) { return st === "running" ? Theme.ready : st === "error" || st === "offline" ? Theme.danger : st === "starting" || st === "pending" ? Theme.warn : Theme.text3 }
    function kb(k) { return k >= 1000 ? (k / 1000).toFixed(2) + " Mb/s" : Math.round(k || 0) + " kb/s" }
    property var cur: edit
    property bool isTs: cur && (cur.type === "dektec_asi" || cur.type === "udp" || cur.type === "srt" || cur.type === "rtmp" || cur.type === "file" || cur.type === "null")
    property var curProf: cur ? (typeof cur.profile === "string" ? { preset: cur.profile } : (cur.profile || {})) : ({})
    property var curOpt: cur ? (cur.options || {}) : ({})
    property var curRetry: cur ? Object.assign({ enabled: true, maxAttempts: 10, backoffSec: 5 }, cur.retry || {}) : ({})
    property var curLive: sel >= 0 && sel < outputs.length ? (outputs[sel].live || {}) : ({})

    RowLayout {
        anchors.fill: parent
        spacing: 10
        CPanel {
            Layout.preferredWidth: 420; Layout.fillHeight: true
            title: "Outputs \u2014 " + (view.profile.name || view.chId) + (view.merged.engineConnected ? "  " + view.merged.running + "/" + view.merged.defined + " running" : "  engine offline")
            padding: 0
            ColumnLayout {
                anchors.fill: parent; spacing: 0
                ListView {
                    id: list
                    Layout.fillWidth: true; Layout.fillHeight: true; clip: true; model: view.outputs
                    delegate: Rectangle {
                        width: list.width; height: 62
                        property var o: modelData
                        property var l: modelData.live || ({})
                        property string st: l.state || "pending"
                        color: view.sel === index ? Theme.bg4 : index % 2 ? Theme.bg2 : Theme.bg3
                        border.color: view.sel === index ? Theme.accent0 : "transparent"
                        RowLayout { anchors.fill: parent; anchors.leftMargin: 10; anchors.rightMargin: 8; spacing: 8
                            CBadge { text: st; tint: view.stateColor(st); Layout.preferredWidth: 66 }
                            ColumnLayout { Layout.fillWidth: true; spacing: 1
                                Text { text: (o.name || o.type) + (o.autoStart === false ? "  (manual)" : "") + (o.enabled === false ? "  (disabled)" : ""); color: Theme.text0; font.pixelSize: Theme.fs12; font.bold: true; elide: Text.ElideRight; Layout.fillWidth: true }
                                Text { text: String(o.type || "").toUpperCase() + (o.url ? "  " + o.url : o.options && o.options.port ? "  port " + o.options.port : "") + (o.profile ? "  \u00B7 " + (typeof o.profile === "string" ? o.profile : o.profile.preset || o.profile.name || "h264_aac") : ""); color: Theme.text2; font.family: Theme.fontMono; font.pixelSize: Theme.fs10; elide: Text.ElideMiddle; Layout.fillWidth: true }
                                Text { text: (st === "running" ? view.kb(l.bitrateKbps) : "") + (l.bytesOut ? "  " + (l.bytesOut / 1048576).toFixed(1) + " MB" : "") + (l.dropped ? "  drop " + l.dropped : "") + (l.restarts ? "  " + l.restarts + " restarts" : "") + (l.retryInSec !== undefined ? "  retry in " + l.retryInSec + " s" : "") + (l.lastError ? "  " + l.lastError : ""); color: l.lastError ? Theme.danger : Theme.text3; font.family: Theme.fontMono; font.pixelSize: Theme.fs10; elide: Text.ElideRight; Layout.fillWidth: true } }
                            CButton { visible: st === "running" || st === "starting"; text: "Stop"; small: true; variant: "danger"; enabled: !!view.merged.engineConnected; onClicked: { view.op(o.name, "stop") } }
                            CButton { visible: !(st === "running" || st === "starting"); text: "Start"; small: true; variant: "success"; enabled: !!view.merged.engineConnected; onClicked: { view.op(o.name, "start") } }
                            CButton { icon2: "\u21bb"; small: true; variant: "ghost"; enabled: !!view.merged.engineConnected; onClicked: { view.op(o.name, "restart") } }
                            CButton { icon2: "\u2715"; small: true; variant: "ghost"; onClicked: { view.remove(index) } } }
                        MouseArea { anchors.fill: parent; z: -1; onClicked: { view.sel = index } }
                    }
                    Text { anchors.centerIn: parent; visible: view.outputs.length === 0; text: "No outputs. Add one below."; color: Theme.text3; font.pixelSize: Theme.fs11 }
                }
                Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border0 }
                Flow { Layout.fillWidth: true; Layout.margins: 8; spacing: 4
                    Repeater { model: view.types; CButton { text: "+ " + modelData.replace("dektec_", "").toUpperCase(); small: true; variant: "ghost"; onClicked: { view.add(modelData) } } } }
            }
        }
        ColumnLayout {
            Layout.fillWidth: true; Layout.fillHeight: true; spacing: 10
            FacebookLivePanel { Layout.fillWidth: true }
            CPanel {
                Layout.fillWidth: true
                implicitHeight: 34 + form.implicitHeight + 24
                title: view.cur ? "Output settings \u2014 " + (view.cur.name || "") : "Output settings"
                headerRight: [
                    Text { text: view.curLive.videoCodec ? "encoder " + view.curLive.videoCodec + (view.curLive.encodedWidth ? " " + view.curLive.encodedWidth + "x" + view.curLive.encodedHeight : "") : ""; color: Theme.text3; font.pixelSize: Theme.fs10; font.family: Theme.fontMono },
                    CButton { text: "Apply"; small: true; variant: view.dirty ? "primary" : "default"; enabled: view.dirty; onClicked: { view.apply() } }
                ]
                GridLayout {
                    id: form
                    anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
                    columns: 4; columnSpacing: 12; rowSpacing: 8
                    visible: view.cur !== null
                    CField { label: "Name"; Layout.fillWidth: true; text: view.cur ? view.cur.name || "" : ""; onEdited: function(v) { view.set("name", v) } }
                    CSelect { label: "Type"; Layout.fillWidth: true; model: view.types; currentIndex: view.cur ? Math.max(0, view.types.indexOf(view.cur.type)) : 0; onActivated: function(i) { view.set("type", view.types[i]) } }
                    CToggle { label: "Enabled"; hint: "off = defined but never started"; Layout.fillWidth: true; checked: view.cur ? view.cur.enabled !== false : true; onToggled: function(v) { view.set("enabled", v) } }
                    CToggle { label: "Auto-start"; hint: "start with the engine"; Layout.fillWidth: true; checked: view.cur ? view.cur.autoStart !== false : true; onToggled: function(v) { view.set("autoStart", v) } }
                    // Which output mix this output carries (MAIN, or an extra mix with its own graphics and sound: Audio mixer > + Mix).
                    CSelect {
                        label: "Mix"; Layout.fillWidth: true
                        readonly property var ids: { var a = ["main"]; var l = mixer.mixDefs[view.chId] || []; for (var i = 0; i < l.length; ++i) a.push(l[i].id); if (view.cur && view.cur.mix && a.indexOf(view.cur.mix) < 0) a.push(view.cur.mix); return a }
                        Component.onCompleted: { mixer.loadMixes(view.chId) }
                        Connections { target: view; function onChIdChanged() { mixer.loadMixes(view.chId) } }
                        model: ids.map(function(x) { return x.toUpperCase() })
                        currentIndex: Math.max(0, ids.indexOf(view.cur && view.cur.mix ? view.cur.mix : "main"))
                        onActivated: function(i) { view.set("mix", ids[i] === "main" ? undefined : ids[i]) }
                    }
                    CField { visible: view.cur && (view.cur.type === "udp" || view.cur.type === "srt" || view.cur.type === "rtmp" || view.cur.type === "file"); label: "URL / path"; Layout.columnSpan: 4; Layout.fillWidth: true; mono: true; text: view.cur ? view.cur.url || "" : ""; onEdited: function(v) { view.set("url", v) } }
                    CField { visible: view.cur && view.cur.type.indexOf("dektec") === 0; label: "Port (1-based)"; Layout.fillWidth: true; mono: true; text: String(view.curOpt.port !== undefined ? view.curOpt.port : 2); onEdited: function(v) { view.setOpt("port", parseInt(v) || 1) } }
                    CField { visible: view.cur && view.cur.type === "dektec_asi"; label: "TS rate (bps)"; Layout.fillWidth: true; mono: true; text: String(view.curOpt.muxrate || 19392658); onEdited: function(v) { view.setOpt("muxrate", parseInt(v) || 0) } }
                    CSelect { visible: view.cur && view.cur.type === "dektec_sdi"; label: "Video standard"; Layout.fillWidth: true; model: ["auto", "1080i59.94", "1080p29.97", "720p59.94", "1080i50", "1080p25", "720p50", "525i59.94", "625i50"]; currentIndex: Math.max(0, model.indexOf(view.curOpt.videoStd || "auto")); onActivated: function(i) { view.setOpt("videoStd", model[i]) } }
                    CField { visible: view.cur && view.cur.type === "dektec_sdi"; label: "Embedded audio ch"; Layout.fillWidth: true; mono: true; text: String(view.curOpt.audioChannels || 2); onEdited: function(v) { view.setOpt("audioChannels", parseInt(v) || 2) } }
                    CField { visible: view.cur && view.cur.type === "decklink"; label: "Device # (0-based)"; Layout.fillWidth: true; mono: true; text: String(view.curOpt.deviceIndex !== undefined ? view.curOpt.deviceIndex : 0); onEdited: function(v) { view.setOpt("deviceIndex", parseInt(v) || 0) } }
                    CField { visible: view.cur && view.cur.type === "decklink"; label: "Device name (overrides #)"; Layout.fillWidth: true; text: view.curOpt.deviceName || ""; onEdited: function(v) { view.setOpt("deviceName", v) } }
                    CSelect { visible: view.cur && view.cur.type === "decklink"; label: "Video mode"; Layout.fillWidth: true; model: ["auto", "1080i59.94", "1080p29.97", "720p59.94", "1080i50", "1080p25", "1080p23.98", "1080p30", "720p50", "2160p29.97"]; currentIndex: Math.max(0, model.indexOf(view.curOpt.videoMode || "auto")); onActivated: function(i) { view.setOpt("videoMode", model[i]) } }
                    CSelect { visible: view.cur && view.cur.type === "decklink"; label: "Embedded audio ch"; Layout.fillWidth: true; model: ["2", "8", "16"]; currentIndex: Math.max(0, model.indexOf(String(view.curOpt.audioChannels || 2))); onActivated: function(i) { view.setOpt("audioChannels", parseInt(model[i])) } }
                    CSelect { visible: view.cur && view.cur.type === "decklink"; label: "Keyer"; Layout.fillWidth: true; model: ["off", "internal", "external"]; currentIndex: Math.max(0, model.indexOf(view.curOpt.keyer || "off")); onActivated: function(i) { view.setOpt("keyer", model[i]) } }
                    CField { visible: view.cur && view.cur.type === "decklink" && (view.curOpt.keyer || "off") !== "off"; label: "Key level (0-255)"; Layout.fillWidth: true; mono: true; text: String(view.curOpt.keyLevel !== undefined ? view.curOpt.keyLevel : 255); onEdited: function(v) { view.setOpt("keyLevel", Math.max(0, Math.min(255, parseInt(v) || 0))) } }
                    CField { visible: view.cur && view.cur.type === "ndi"; label: "NDI source name"; Layout.fillWidth: true; text: view.cur ? view.cur.sourceName || "" : ""; onEdited: function(v) { view.set("sourceName", v) } }
                    CField { visible: view.cur && view.cur.type === "ndi"; label: "NDI groups"; Layout.fillWidth: true; text: view.curOpt.groups || ""; onEdited: function(v) { view.setOpt("groups", v) } }
                    CField { visible: view.isTs && view.cur.type !== "dektec_asi"; label: "Mux rate (bps, 0 = VBR)"; Layout.fillWidth: true; mono: true; text: String(view.curOpt.muxrate || 0); onEdited: function(v) { view.setOpt("muxrate", parseInt(v) || 0) } }
                    CField { visible: view.isTs; label: "Service name"; Layout.fillWidth: true; text: view.curOpt.serviceName || ""; onEdited: function(v) { view.setOpt("serviceName", v) } }
                    CField { visible: view.isTs; label: "Service id / video PID / audio PID"; Layout.fillWidth: true; mono: true; text: (view.curOpt.serviceId || 1) + " / " + (view.curOpt.videoPid || 256) + " / " + (view.curOpt.audioPid || 257); onEdited: function(v) { var p = v.split("/"); if (p.length === 3) { view.setOpt("serviceId", parseInt(p[0]) || 1); view.setOpt("videoPid", parseInt(p[1]) || 256); view.setOpt("audioPid", parseInt(p[2]) || 257) } } }
                    CSelect { visible: view.isTs; label: "Codec profile (preset)"; Layout.fillWidth: true; model: view.presets.length ? view.presets : ["h264_aac", "mpeg2_ac3"]; currentIndex: Math.max(0, model.indexOf(view.curProf.preset || "h264_aac")); onActivated: function(i) { view.setProf("preset", model[i]) } }
                    CField { visible: view.isTs; label: "Video kbps"; Layout.fillWidth: true; mono: true; text: view.curProf.videoBitrateKbps ? String(view.curProf.videoBitrateKbps) : ""; placeholder: "preset"; onEdited: function(v) { view.setProf("videoBitrateKbps", parseInt(v) || undefined) } }
                    CSelect { visible: view.isTs; label: "Rate control"; Layout.fillWidth: true; model: ["preset", "cbr", "vbr", "cqp"]; currentIndex: Math.max(0, model.indexOf(view.curProf.rateControl || "preset")); onActivated: function(i) { view.setProf("rateControl", i === 0 ? undefined : model[i]) } }
                    CField { visible: view.isTs; label: "Audio kbps"; Layout.fillWidth: true; mono: true; text: view.curProf.audioBitrateKbps ? String(view.curProf.audioBitrateKbps) : ""; placeholder: "preset"; onEdited: function(v) { view.setProf("audioBitrateKbps", parseInt(v) || undefined) } }
                    CField { visible: view.isTs; label: "GOP (frames)"; Layout.fillWidth: true; mono: true; text: view.curProf.gopFrames ? String(view.curProf.gopFrames) : (view.curProf.gopSize ? String(view.curProf.gopSize) : ""); placeholder: "preset"; onEdited: function(v) { view.setProf("gopFrames", parseInt(v) || undefined) } }
                    CField { visible: view.isTs; label: "Encode size WxH (0 = canvas)"; Layout.fillWidth: true; mono: true; text: view.curProf.width ? view.curProf.width + "x" + view.curProf.height : ""; placeholder: "canvas"; onEdited: function(v) { var p = v.toLowerCase().split("x"); if (p.length === 2 && parseInt(p[0])) { view.setProf("width", parseInt(p[0])); view.setProf("height", parseInt(p[1])) } else { view.setProf("width", undefined); view.setProf("height", undefined) } } }
                    CToggle { visible: view.isTs; label: "Interlaced (1080i)"; Layout.fillWidth: true; checked: !!view.curProf.interlaced; onToggled: function(v) { view.setProf("interlaced", v ? true : undefined) } }
                    CToggle { label: "Retry on failure"; hint: "re-open the sink after an error"; Layout.fillWidth: true; checked: view.curRetry.enabled !== false; onToggled: function(v) { view.setRetry("enabled", v) } }
                    CField { label: "Max attempts (0 = forever)"; Layout.fillWidth: true; mono: true; text: String(view.curRetry.maxAttempts !== undefined ? view.curRetry.maxAttempts : 10); onEdited: function(v) { view.setRetry("maxAttempts", parseInt(v) || 0) } }
                    CField { label: "Backoff (s)"; Layout.fillWidth: true; mono: true; text: String(view.curRetry.backoffSec || 5); onEdited: function(v) { view.setRetry("backoffSec", parseInt(v) || 5) } }
                }
                Text { anchors.centerIn: parent; visible: view.cur === null; text: "Select an output"; color: Theme.text3; font.pixelSize: Theme.fs11 }
            }
            CPanel {
                Layout.fillWidth: true; Layout.fillHeight: true; padding: 0
                title: "Live output statistics"
                ListView {
                    anchors.fill: parent; clip: true; model: (view.merged.encoders || []).concat(view.outputs)
                    delegate: Rectangle {
                        width: ListView.view.width; height: 34; color: index % 2 ? Theme.bg2 : Theme.bg3
                        property bool enc: modelData.kind === "encoder"
                        property var l: enc ? modelData : (modelData.live || {})
                        RowLayout { anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 12; spacing: 12
                            Rectangle { width: 8; height: 8; radius: 4; color: enc ? Theme.accent2 : view.stateColor(l.state || "pending") }
                            Text { text: enc ? "ENCODER " + modelData.profile + " (" + (modelData.videoCodec || "") + " " + modelData.width + "x" + modelData.height + ")" : (modelData.name || modelData.type); color: Theme.text0; font.pixelSize: Theme.fs11; font.bold: true; Layout.preferredWidth: 260; elide: Text.ElideRight }
                            Text { text: enc ? "encoder" : (l.kind || modelData.type || ""); color: Theme.text2; font.pixelSize: Theme.fs10; Layout.preferredWidth: 90 }
                            Text { Layout.fillWidth: true; elide: Text.ElideRight; color: Theme.text1; font.family: Theme.fontMono; font.pixelSize: Theme.fs10
                                text: enc ? "in " + modelData.framesIn + "  enc " + modelData.encoded + "  drop " + modelData.dropped + "  " + Number(modelData.avgEncodeMs || 0).toFixed(1) + " ms  q " + modelData.queue + "  sinks " + (modelData.sinks || 0)
                                    : (l.state || "pending") + (l.state === "running" ? "  " + view.kb(l.bitrateKbps) + "  up " + Math.floor((l.uptimeSec || 0) / 60) + "m" : "") + (l.framesOut !== undefined ? "  frames " + l.framesOut : l.packetsOut !== undefined ? "  pkts " + l.packetsOut : "") + (l.dropped ? "  drop " + l.dropped : "") + (l.bytesOut ? "  " + (l.bytesOut / 1048576).toFixed(1) + " MB" : "") + (l.fifoLoadPct !== undefined ? "  fifo " + Number(l.fifoLoadPct).toFixed(1) + "%" : "") + (l.underruns !== undefined ? "  underruns " + l.underruns : "") + (l.restarts ? "  restarts " + l.restarts : "") + (l.lastError || l.error ? "  " + (l.lastError || l.error) : "") } }
                    }
                    Text { anchors.centerIn: parent; visible: !(view.outputs.length || (view.merged.encoders || []).length); text: "Engine offline or no outputs"; color: Theme.text3; font.pixelSize: Theme.fs11 }
                }
            }
        }
    }
}
