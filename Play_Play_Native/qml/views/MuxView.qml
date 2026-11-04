import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".."
import "../ui"

/**
 * Mux: the ATSC multiplexer console (TSDuck, ported from the Starfile broadcast_gui). Tabs mirror the web
 * console: Services (channel / NDI / ffmpeg input / UDP merge, encoder preset + advanced editor, statmux,
 * PIDs), Encoders (presets + ffmpeg/TSDuck/DekTec capabilities), Tables (PSIP summary, XML, overrides,
 * compile, live analysis), Output (ASI/UDP/SRT/file, DekTec devices, dry run), Streams (per-channel
 * streaming outputs: start/stop/restart/autoStart/add/remove), Guide, Log. All through /api/v1/mux/* and
 * /api/v1/channels/{id}/outputs*.
 */
Item {
    id: view
    property var status: ({})
    property var cfg: ({})
    property var services: []
    property var alloc: ({})
    property var guide: null
    property var logLines: []
    property var presets: []
    property var caps: null
    property var tables: null
    property var ovr: ({})
    property bool ovrDirty: false
    property var dektec: []
    property var dry: null
    property var streams: []
    property var ndiInputs: []
    property string analysis: ""
    property string tab: "services"
    property bool dirty: false
    property int advIndex: -1
    property int xmlIndex: 0
    property bool showHidden: false
    property var met: status.metrics || ({})
    readonly property var codecs: ["libx264", "libx265", "mpeg2video", "h264_qsv", "hevc_qsv", "h264_nvenc", "hevc_nvenc"]
    readonly property var acodecs: ["ac3", "aac", "mp2", "eac3"]
    readonly property var tabs: ["services", "encoders", "tables", "output", "streams", "guide", "log"]

    function reload() {
        api.get("/mux/status", function(ok, d) { if (ok) view.status = d })
        api.get("/mux/config", function(ok, d) { if (ok) view.cfg = d })
        if (!view.dirty) api.get("/mux/services", function(ok, d) { if (ok) { view.services = d.services || []; view.alloc = d.allocation || {} } })
        if (!view.presets.length) api.get("/mux/encoder-presets", function(ok, d) { if (ok) view.presets = d.presets || [] })
        if (view.tab === "services") api.get("/inputs", function(ok, d) { if (ok) view.ndiInputs = (d.ndi || []).map(function(n) { return n.name }) })
        if (view.tab === "guide") api.get("/mux/guide", function(ok, d) { if (ok) view.guide = d })
        if (view.tab === "log") api.get("/mux/log?n=200", function(ok, d) { if (ok) view.logLines = d.lines || [] })
        if (view.tab === "encoders" && !view.caps) loadCaps(false)
        if (view.tab === "tables") loadTables()
        if (view.tab === "output") api.get("/mux/dektec?refresh=0", function(ok, d) { if (ok) view.dektec = d.devices || [] })
        if (view.tab === "streams") loadStreams()
    }
    function loadCaps(refresh) { api.get("/mux/capabilities" + (refresh ? "?refresh=1" : ""), function(ok, d) { if (ok) view.caps = d }) }
    function loadTables() { api.get("/mux/tables", function(ok, d) { if (ok) { view.tables = d; if (!view.ovrDirty) view.ovr = d.overrides || {} } }) }
    function loadStreams() { api.get("/outputs", function(ok, d) { if (ok) view.streams = (d.channels || []).filter(function(c) { return view.showHidden || !c.hidden }) }) }
    function setPath(obj, path, v) { var parts = path.split("."); var cur = obj; for (var k = 0; k < parts.length - 1; ++k) { cur[parts[k]] = cur[parts[k]] || {}; cur = cur[parts[k]] } cur[parts[parts.length - 1]] = v }
    function set(i, path, v) { var s = view.services.slice(); setPath(s[i], path, v); if (path === "source.kind" && v === "input" && !s[i].source.input) s[i].source.input = { type: "test" }; if (path.indexOf("encoder.") === 0 && s[i].encoder) delete s[i].encoder.encoderPreset; view.services = s; view.dirty = true }
    function enc(s, k, d) { var e = (s && s.encoder) || {}; return e[k] !== undefined ? e[k] : d }
    function aud(s, k, d) { var a = ((s && s.encoder) || {}).audio || {}; if (a[k] !== undefined) return a[k]; var e = (s && s.encoder) || {}; var legacy = { codec: e.audCodec, bitrateKbps: e.audBitrateKbps, channels: e.audChannels }; return legacy[k] !== undefined ? legacy[k] : d }
    function tsv(s, k, d) { var t = ((s && s.encoder) || {}).ts || {}; return t[k] !== undefined ? t[k] : d }
    function applyPreset(i, id) { var p = null; for (var k = 0; k < view.presets.length; ++k) if (view.presets[k].id === id) p = view.presets[k]; if (!p || !view.services[i]) return; var s = view.services.slice(); var e = Object.assign({}, s[i].encoder || {}); for (var key in p) if (["id", "label", "note"].indexOf(key) < 0) e[key] = JSON.parse(JSON.stringify(p[key])); e.encoderPreset = id; delete e.audCodec; delete e.audBitrateKbps; delete e.audChannels; s[i].encoder = e; view.services = s; view.dirty = true; window.toast("Preset " + id + " applied (save to persist)", "ok") }
    function save() { api.put("/mux/services", { services: view.services }, function(ok, d) { if (ok) { view.dirty = false; view.services = d.services; view.alloc = d.allocation; window.toast("Services saved", "ok"); view.reload() } else window.toast(d && d.error ? d.error : "save failed", "error") }) }
    function cfgSet(obj) { api.put("/mux/config", obj, function(ok) { if (ok) view.reload() }) }
    function outSet(k, v) { var o = {}; o[k] = v; cfgSet({ output: o }) }
    function ovrSet(path, v) { var o = JSON.parse(JSON.stringify(view.ovr || {})); setPath(o, path, v); view.ovr = o; view.ovrDirty = true }
    function ovrSave() { api.put("/mux/tables/overrides", view.ovr || {}, function(ok, d) { if (ok) { view.ovr = d; view.ovrDirty = false; window.toast("Overrides saved, tables rewritten", "ok"); loadTables() } }) }
    function strOp(ch, name, op) { api.post("/channels/" + encodeURIComponent(ch) + "/outputs/" + encodeURIComponent(name) + "/" + op, {}, function(ok, d) { window.toast(ok ? name + ": " + op : (d && d.error ? d.error : op + " failed"), ok ? "ok" : "error"); loadStreams() }) }
    function strSet(ch, name, k, v) { var b = {}; b[k] = v; api.put("/channels/" + encodeURIComponent(ch) + "/outputs/" + encodeURIComponent(name), b, function(ok) { loadStreams() }) }
    function strRemove(ch, name) { api.del("/channels/" + encodeURIComponent(ch) + "/outputs/" + encodeURIComponent(name), function(ok, d) { window.toast(ok ? "Removed " + name : (d && d.error ? d.error : "remove failed"), ok ? "ok" : "error"); loadStreams() }) }
    function mb(b) { return (Number(b || 0) / 1e6).toFixed(2) }
    function kb(k) { return k >= 1000 ? (k / 1000).toFixed(2) + " Mb/s" : Math.round(k || 0) + " kb/s" }
    function stateColor(st) { return st === "running" ? Theme.ready : st === "error" || st === "offline" ? Theme.danger : st === "starting" || st === "pending" ? Theme.warn : Theme.text3 }
    function indColor(v) { return v === "OK" || v === "ok" ? Theme.ready : v === "ERROR" || v === "underrun" ? Theme.danger : v === "preloading" ? Theme.warn : Theme.text2 }
    function idx(list, v, d) { var i = list.indexOf(v); return i >= 0 ? i : (d || 0) }
    Component.onCompleted: { reload() }
    Timer { interval: 3000; running: true; repeat: true; onTriggered: { api.get("/mux/status", function(ok, d) { if (ok) view.status = d }); if (view.tab === "streams") view.loadStreams(); if (view.tab === "log") api.get("/mux/log?n=200", function(ok, d) { if (ok) view.logLines = d.lines || [] }) } }

    ColumnLayout {
        anchors.fill: parent; spacing: 10
        RowLayout {
            Layout.fillWidth: true; spacing: 10
            Repeater {
                model: [ ["MUX" + (view.status.autoStart ? " · AUTO" : ""), String(view.status.state || "stopped").toUpperCase(), view.status.state === "running" ? Theme.ready : view.status.state === "error" ? Theme.danger : view.status.state === "starting" ? Theme.warn : Theme.text3],
                         ["TRANSPORT", view.met.muxBitrate ? view.mb(view.met.muxBitrate) + " Mb/s" : "—", Theme.text0], ["NULL PADDING", view.met.stuffing !== undefined ? view.mb(view.met.stuffing) + " Mb/s" : "—", Theme.text0],
                         ["ASI FIFO", view.met.fifo || "—", view.indColor(view.met.fifo)], ["CONTINUITY", view.met.continuity || "—", view.indColor(view.met.continuity)], ["PCR", view.met.pcr || "—", view.indColor(view.met.pcr)], ["PAT/PMT", view.met.patPmt || "—", view.indColor(view.met.patPmt)] ]
                Rectangle { Layout.fillWidth: true; height: 56; radius: Theme.r8; color: Theme.bg2; border.color: Theme.border0
                    ColumnLayout { anchors.centerIn: parent; spacing: 1
                        Text { text: modelData[1]; color: modelData[2]; font.pixelSize: Theme.fs16; font.bold: true; font.family: Theme.fontMono; Layout.alignment: Qt.AlignHCenter }
                        Text { text: modelData[0]; color: Theme.text3; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1; Layout.alignment: Qt.AlignHCenter } } }
            }
        }
        CPanel {
            Layout.fillWidth: true; Layout.fillHeight: true; padding: 0
            title: "ATSC multiplexer — " + view.services.length + " service(s) on " + ((view.cfg.output || {}).kind || "dektec")
            headerRight: [
                Text { text: (view.status.restarts ? view.status.restarts + " restarts  " : "") + (view.status.error || ""); color: view.status.error ? Theme.danger : Theme.text3; font.pixelSize: Theme.fs10 },
                CButton { visible: !(view.status.running); text: "START MUX"; small: true; variant: "success"; onClicked: { api.post("/mux/start", {}, function(ok, d) { if (ok) { window.toast("Mux starting", "ok"); view.reload() } else window.toast(d && d.error ? d.error : "start failed", "error") }) } },
                CButton { visible: !!view.status.running; text: "STOP"; small: true; variant: "danger"; onClicked: { api.post("/mux/stop", {}, function() { view.reload() }) } },
                CButton { text: "Validate PSIP"; small: true; variant: "ghost"; onClicked: { api.post("/mux/validate", {}, function(ok, r) { window.toast(r && r.compiled ? "PSIP tables compile" : "Tables: " + (r ? r.error : "?"), r && r.compiled ? "ok" : "error") }) } }
            ]
            ColumnLayout {
                anchors.fill: parent; spacing: 0
                RowLayout {
                    Layout.fillWidth: true; Layout.margins: 8; spacing: 6
                    Repeater { model: view.tabs; CButton { text: modelData; small: true; variant: "ghost"; active: view.tab === modelData; onClicked: { view.tab = modelData; view.reload() } } }
                    Item { Layout.fillWidth: true }
                    Text { text: "allocated video " + view.mb(view.status.allocatedVideoBps) + " Mb/s   packets " + (view.met.packets ? Number(view.met.packets).toLocaleString() : "—"); color: Theme.text3; font.pixelSize: Theme.fs10; font.family: Theme.fontMono }
                    CSelect { id: preset; visible: view.tab === "services"; Layout.preferredWidth: 70; Layout.maximumWidth: 90; model: [2, 4, 6, 8, 10, 12] }
                    CButton { visible: view.tab === "services"; text: "Apply preset"; small: true; onClicked: { api.post("/mux/preset", { count: preset.model[preset.currentIndex] }, function(ok, r) { if (ok) { window.toast("Preset: " + r.kept + " kept, " + r.added + " added, " + r.dropped + " dropped", "ok"); view.dirty = false; view.reload() } }) } }
                    CButton { visible: view.tab === "services"; text: "+ service"; small: true; onClicked: { var s = view.services.slice(); var n = s.length + 1; var ch = supervisor.channels.length ? supervisor.channels[0].profile.id : ""; s.push({ id: "svc" + n, major: 25, minor: n, shortName: "25-" + n, longName: "Service " + n, source: { kind: ch ? "channel" : "input", channelId: ch, input: { type: "test" } } }); view.services = s; view.dirty = true } }
                    CButton { visible: view.dirty; text: "SAVE"; small: true; variant: "primary"; onClicked: { view.save() } }
                    CButton { visible: view.dirty; text: "discard"; small: true; variant: "ghost"; onClicked: { view.dirty = false; view.reload() } }
                }
                Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border0 }

                // ---------------- services ----------------
                RowLayout {
                    visible: view.tab === "services"; Layout.fillWidth: true; Layout.fillHeight: true; spacing: 0
                    ListView {
                        Layout.fillWidth: true; Layout.fillHeight: true; clip: true; model: view.services
                        delegate: Rectangle {
                            width: ListView.view.width; height: 60; color: view.advIndex === index ? Theme.bg4 : index % 2 ? Theme.bg2 : Theme.bg3
                            property var s: modelData
                            property var per: (view.met.perService || {})[s.id] || ({})
                            property string kind: (s.source || {}).kind || "input"
                            RowLayout {
                                anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 8; spacing: 8
                                Text { text: s.major + "." + s.minor; color: Theme.text0; font.pixelSize: Theme.fs14; font.bold: true; font.family: Theme.fontMono; Layout.preferredWidth: 44 }
                                ColumnLayout { Layout.preferredWidth: 170; spacing: 2
                                    CField { Layout.fillWidth: true; text: s.longName || ""; onEdited: function(v) { view.set(index, "longName", v) } }
                                    Text { text: "short " + (s.shortName || "") + "   SID " + s.serviceId + "   PIDs " + s.vidPid + "/" + s.audPid; color: Theme.text3; font.pixelSize: Theme.fs10; font.family: Theme.fontMono } }
                                ColumnLayout { Layout.preferredWidth: 300; spacing: 2
                                    RowLayout { spacing: 4
                                        CSelect { Layout.preferredWidth: 84; model: ["channel", "ndi", "input", "udp"]; currentIndex: view.idx(model, kind, 2); onActivated: function(i) { view.set(index, "source.kind", model[i]) } }
                                        CSelect { visible: kind === "channel"; Layout.preferredWidth: 110; model: supervisor.channels.filter(function(c) { return view.showHidden || !(c.profile.extra || {}).hidden }).map(function(c) { return c.profile.id }); currentIndex: Math.max(0, model.indexOf((s.source || {}).channelId)); onActivated: function(i) { view.set(index, "source.channelId", model[i]) } }
                                        CSelect { visible: kind === "ndi"; Layout.preferredWidth: 110; model: ["— NDI —"].concat(view.ndiInputs); currentIndex: Math.max(0, model.indexOf((s.source || {}).ndiName)); onActivated: function(i) { if (i > 0) view.set(index, "source.ndiName", model[i]) } }
                                        CSelect { visible: kind === "input"; Layout.preferredWidth: 72; model: ["test", "file", "srt", "rtmp", "rtsp", "udp", "http", "dshow"]; currentIndex: Math.max(0, model.indexOf(((s.source || {}).input || {}).type || "test")); onActivated: function(i) { view.set(index, "source.input.type", model[i]) } }
                                        CField { visible: kind !== "channel"; Layout.fillWidth: true; mono: true; placeholder: kind === "ndi" ? "MACHINE (Source)" : "url / path"; text: kind === "udp" ? ((s.source || {}).url || "") : kind === "ndi" ? ((s.source || {}).ndiName || "") : (((s.source || {}).input || {}).location || ""); onEdited: function(v) { view.set(index, kind === "udp" ? "source.url" : kind === "ndi" ? "source.ndiName" : "source.input.location", v) } } }
                                    Text { text: (kind === "ndi" ? "→ hidden channel mux-" + s.id + "   " : "") + view.enc(s, "codec", "libx264") + "  " + view.enc(s, "width", "") + "x" + view.enc(s, "height", "") + " @" + view.enc(s, "fps", "") + "  " + view.enc(s, "vidBitrateKbps", "") + "k " + view.enc(s, "rateControl", "cbr") + (view.enc(s, "interlaced", false) ? " i" : "") + " / " + view.aud(s, "bitrateKbps", 192) + "k " + view.aud(s, "codec", "ac3") + (view.enc(s, "encoderPreset", "") ? "   [" + view.enc(s, "encoderPreset", "") + "]" : ""); color: Theme.text3; font.pixelSize: Theme.fs10; elide: Text.ElideRight; Layout.fillWidth: true } }
                                ColumnLayout { Layout.preferredWidth: 150; spacing: 2
                                    RowLayout { spacing: 4
                                        CToggle { label: "statmux"; checked: (s.statmux || {}).managed !== false; implicitWidth: 80; onToggled: function(v) { view.set(index, "statmux.managed", v) } }
                                        CField { Layout.preferredWidth: 56; mono: true; text: String((s.statmux || {}).minKbps || ""); onEdited: function(v) { view.set(index, "statmux.minKbps", parseInt(v) || 0) } }
                                        CField { Layout.preferredWidth: 56; mono: true; text: String((s.statmux || {}).maxKbps || ""); onEdited: function(v) { view.set(index, "statmux.maxKbps", parseInt(v) || 0) } } }
                                    Text { text: "alloc " + (view.alloc[s.id] ? view.mb(view.alloc[s.id]) + " Mb/s" : "—") + "   " + ((s.statmux || {}).priority || "normal"); color: Theme.text3; font.pixelSize: Theme.fs10 } }
                                ColumnLayout { Layout.fillWidth: true; spacing: 3
                                    Rectangle { Layout.fillWidth: true; height: 8; radius: 2; color: Theme.bg4
                                        Rectangle { height: parent.height; radius: 2; width: parent.width * (view.alloc[s.id] ? Math.min(1, (per.bitrate || 0) / (view.alloc[s.id] + view.aud(s, "bitrateKbps", 192) * 1000)) : 0); color: per.bitrate ? Theme.ready : Theme.text3 } }
                                    Text { text: (per.bitrate ? view.mb(per.bitrate) + " Mb/s live" : "no bitrate") ; color: per.bitrate ? Theme.text1 : Theme.text3; font.pixelSize: Theme.fs10; font.family: Theme.fontMono } }
                                CButton { text: view.advIndex === index ? "Adv ▴" : "Adv…"; small: true; variant: "ghost"; onClicked: { view.advIndex = view.advIndex === index ? -1 : index; if (!view.caps) view.loadCaps(false) } }
                                CButton { text: "✕"; small: true; variant: "ghost"; onClicked: { var a = view.services.slice(); a.splice(index, 1); view.services = a; view.advIndex = -1; view.dirty = true } }
                            }
                        }
                        Text { anchors.centerIn: parent; visible: !view.services.length; color: Theme.text3; font.pixelSize: Theme.fs11; text: "No services: add one or apply a channel-count preset" }
                    }
                    // advanced encoder editor for the selected service
                    Rectangle { visible: view.advIndex >= 0 && view.advIndex < view.services.length; Layout.preferredWidth: 1; Layout.fillHeight: true; color: Theme.border0 }
                    Flickable {
                        id: advFlick
                        visible: view.advIndex >= 0 && view.advIndex < view.services.length; Layout.preferredWidth: 420; Layout.fillHeight: true; clip: true; contentHeight: advCol.implicitHeight + 16
                        property var s: view.advIndex >= 0 && view.advIndex < view.services.length ? view.services[view.advIndex] : ({})
                        property int i: view.advIndex
                        ColumnLayout {
                            id: advCol; x: 10; y: 8; width: advFlick.width - 20; spacing: 6
                            Text { text: "ADVANCED ENCODER — " + (advFlick.s.longName || ""); color: Theme.text1; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1 }
                            CSelect { label: "Preset"; Layout.fillWidth: true; model: ["— apply preset —"].concat(view.presets.map(function(p) { return p.id })); currentIndex: Math.max(0, model.indexOf(view.enc(advFlick.s, "encoderPreset", ""))); onActivated: function(k) { if (k > 0) view.applyPreset(advFlick.i, model[k]) } }
                            Text { text: "VIDEO"; color: Theme.text2; font.pixelSize: Theme.fs10; font.letterSpacing: 1 }
                            GridLayout { columns: 2; columnSpacing: 8; rowSpacing: 4; Layout.fillWidth: true
                                CSelect { label: "Codec"; Layout.fillWidth: true; model: view.codecs.filter(function(c) { var has = ((view.caps || {}).ffmpeg || {}).has; return !has || has[c] !== false || c === view.enc(advFlick.s, "codec", "libx264") }); currentIndex: Math.max(0, model.indexOf(view.enc(advFlick.s, "codec", "libx264"))); onActivated: function(k) { view.set(advFlick.i, "encoder.codec", model[k]) } }
                                CSelect { label: "Rate control"; Layout.fillWidth: true; model: ["cbr", "vbr", "cqp"]; currentIndex: view.idx(model, view.enc(advFlick.s, "rateControl", "cbr")); onActivated: function(k) { view.set(advFlick.i, "encoder.rateControl", model[k]) } }
                                CField { label: "Width"; Layout.fillWidth: true; mono: true; text: String(view.enc(advFlick.s, "width", 1280)); onEdited: function(v) { view.set(advFlick.i, "encoder.width", parseInt(v) || 1280) } }
                                CField { label: "Height"; Layout.fillWidth: true; mono: true; text: String(view.enc(advFlick.s, "height", 720)); onEdited: function(v) { view.set(advFlick.i, "encoder.height", parseInt(v) || 720) } }
                                CField { label: "FPS"; Layout.fillWidth: true; mono: true; text: String(view.enc(advFlick.s, "fps", "30000/1001")); onEdited: function(v) { view.set(advFlick.i, "encoder.fps", v) } }
                                CField { label: "Video kb/s"; Layout.fillWidth: true; mono: true; text: String(view.enc(advFlick.s, "vidBitrateKbps", 3000)); onEdited: function(v) { view.set(advFlick.i, "encoder.vidBitrateKbps", parseInt(v) || 0) } }
                                CField { label: "Maxrate kb/s (vbr)"; Layout.fillWidth: true; mono: true; text: String(view.enc(advFlick.s, "maxrateKbps", 0)); onEdited: function(v) { view.set(advFlick.i, "encoder.maxrateKbps", parseInt(v) || 0) } }
                                CField { label: "VBV buffer kb (0 = auto)"; Layout.fillWidth: true; mono: true; text: String(view.enc(advFlick.s, "bufsizeKbps", 0)); onEdited: function(v) { view.set(advFlick.i, "encoder.bufsizeKbps", parseInt(v) || 0) } }
                                CField { label: "QP (cqp)"; Layout.fillWidth: true; mono: true; text: String(view.enc(advFlick.s, "qp", 23)); onEdited: function(v) { view.set(advFlick.i, "encoder.qp", parseInt(v) || 23) } }
                                CField { label: "GOP frames"; Layout.fillWidth: true; mono: true; text: String(view.enc(advFlick.s, "gop", 30)); onEdited: function(v) { view.set(advFlick.i, "encoder.gop", parseInt(v) || 30) } }
                                CField { label: "Keyint min (0 = gop)"; Layout.fillWidth: true; mono: true; text: String(view.enc(advFlick.s, "keyintMin", 0)); onEdited: function(v) { view.set(advFlick.i, "encoder.keyintMin", parseInt(v) || 0) } }
                                CField { label: "B-frames"; Layout.fillWidth: true; mono: true; text: String(view.enc(advFlick.s, "bframes", 2)); onEdited: function(v) { view.set(advFlick.i, "encoder.bframes", parseInt(v) || 0) } }
                                CSelect { label: "Speed preset"; Layout.fillWidth: true; model: ["ultrafast", "superfast", "veryfast", "faster", "fast", "medium", "slow", "p1", "p2", "p4", "p6"]; currentIndex: view.idx(model, view.enc(advFlick.s, "preset", "veryfast"), 2); onActivated: function(k) { view.set(advFlick.i, "encoder.preset", model[k]) } }
                                CSelect { label: "Profile"; Layout.fillWidth: true; model: ["high", "main", "baseline"]; currentIndex: view.idx(model, view.enc(advFlick.s, "profile", "high")); onActivated: function(k) { view.set(advFlick.i, "encoder.profile", model[k]) } }
                                CSelect { label: "Level"; Layout.fillWidth: true; model: ["3", "3.1", "3.2", "4", "4.1", "4.2", "5", "5.1"]; currentIndex: view.idx(model, String(view.enc(advFlick.s, "level", "4.1")), 4); onActivated: function(k) { view.set(advFlick.i, "encoder.level", model[k]) } }
                                CField { label: "AFD (-1 off)"; Layout.fillWidth: true; mono: true; text: String(view.enc(advFlick.s, "afd", -1)); onEdited: function(v) { view.set(advFlick.i, "encoder.afd", parseInt(v)) } }
                                CToggle { label: "Interlaced (1080i/480i)"; Layout.fillWidth: true; checked: !!view.enc(advFlick.s, "interlaced", false); onToggled: function(v) { view.set(advFlick.i, "encoder.interlaced", v) } }
                                CToggle { label: "Closed GOP"; Layout.fillWidth: true; checked: view.enc(advFlick.s, "closedGop", true) !== false; onToggled: function(v) { view.set(advFlick.i, "encoder.closedGop", v) } }
                                CToggle { label: "Scene-cut"; Layout.fillWidth: true; checked: !!view.enc(advFlick.s, "scenecut", false); onToggled: function(v) { view.set(advFlick.i, "encoder.scenecut", v) } }
                                CToggle { label: "CEA-708 pass-through"; Layout.fillWidth: true; checked: view.enc(advFlick.s, "ccPassthrough", true) !== false; onToggled: function(v) { view.set(advFlick.i, "encoder.ccPassthrough", v) } }
                                CField { label: "Extra x264/x265 params"; Layout.columnSpan: 2; Layout.fillWidth: true; mono: true; placeholder: "key=val:key=val"; text: String(view.enc(advFlick.s, "extra", "") || ""); onEdited: function(v) { view.set(advFlick.i, "encoder.extra", v) } }
                            }
                            Text { text: "AUDIO  (A/85 target −24 LKFS; dialnorm signals the dialogue level)"; color: Theme.text2; font.pixelSize: Theme.fs10; font.letterSpacing: 1 }
                            GridLayout { columns: 2; columnSpacing: 8; rowSpacing: 4; Layout.fillWidth: true
                                CSelect { label: "Codec"; Layout.fillWidth: true; model: view.acodecs; currentIndex: view.idx(model, view.aud(advFlick.s, "codec", "ac3")); onActivated: function(k) { view.set(advFlick.i, "encoder.audio.codec", model[k]) } }
                                CField { label: "kb/s"; Layout.fillWidth: true; mono: true; text: String(view.aud(advFlick.s, "bitrateKbps", 192)); onEdited: function(v) { view.set(advFlick.i, "encoder.audio.bitrateKbps", parseInt(v) || 192) } }
                                CSelect { label: "Channels"; Layout.fillWidth: true; model: ["1", "2", "6"]; currentIndex: view.idx(model, String(view.aud(advFlick.s, "channels", 2)), 1); onActivated: function(k) { view.set(advFlick.i, "encoder.audio.channels", parseInt(model[k])) } }
                                CField { label: "Dialnorm (ac3)"; Layout.fillWidth: true; mono: true; text: String(view.aud(advFlick.s, "dialnorm", -27)); onEdited: function(v) { view.set(advFlick.i, "encoder.audio.dialnorm", parseInt(v) || -27) } }
                                CField { label: "Mixing level (-1 off)"; Layout.fillWidth: true; mono: true; text: String(view.aud(advFlick.s, "mixingLevel", -1)); onEdited: function(v) { view.set(advFlick.i, "encoder.audio.mixingLevel", parseInt(v)) } }
                                CSelect { label: "AAC profile"; Layout.fillWidth: true; model: ["aac_low", "aac_he", "aac_he_v2", "aac_ld"]; currentIndex: view.idx(model, view.aud(advFlick.s, "aacProfile", "aac_low")); onActivated: function(k) { view.set(advFlick.i, "encoder.audio.aacProfile", model[k]) } }
                                CToggle { label: "loudnorm −24 (ffmpeg inputs)"; Layout.columnSpan: 2; Layout.fillWidth: true; checked: !!view.aud(advFlick.s, "normalize", false); onToggled: function(v) { view.set(advFlick.i, "encoder.audio.normalize", v) } }
                            }
                            Text { text: "TRANSPORT / PIDs"; color: Theme.text2; font.pixelSize: Theme.fs10; font.letterSpacing: 1 }
                            GridLayout { columns: 3; columnSpacing: 8; rowSpacing: 4; Layout.fillWidth: true
                                CField { label: "Major"; Layout.fillWidth: true; mono: true; text: String(advFlick.s.major || 25); onEdited: function(v) { view.set(advFlick.i, "major", parseInt(v) || 25) } }
                                CField { label: "Minor"; Layout.fillWidth: true; mono: true; text: String(advFlick.s.minor || 1); onEdited: function(v) { view.set(advFlick.i, "minor", parseInt(v) || 1) } }
                                CField { label: "Service id"; Layout.fillWidth: true; mono: true; text: String(advFlick.s.serviceId || ""); onEdited: function(v) { view.set(advFlick.i, "serviceId", parseInt(v) || 0) } }
                                CField { label: "Video PID"; Layout.fillWidth: true; mono: true; text: String(advFlick.s.vidPid || ""); onEdited: function(v) { view.set(advFlick.i, "vidPid", parseInt(v) || 0) } }
                                CField { label: "Audio PID"; Layout.fillWidth: true; mono: true; text: String(advFlick.s.audPid || ""); onEdited: function(v) { view.set(advFlick.i, "audPid", parseInt(v) || 0) } }
                                CField { label: "PMT PID"; Layout.fillWidth: true; mono: true; text: String(view.tsv(advFlick.s, "pmtPid", 4096)); onEdited: function(v) { view.set(advFlick.i, "encoder.ts.pmtPid", parseInt(v) || 4096) } }
                                CField { label: "PCR period ms"; Layout.fillWidth: true; mono: true; text: String(view.tsv(advFlick.s, "pcrPeriodMs", 20)); onEdited: function(v) { view.set(advFlick.i, "encoder.ts.pcrPeriodMs", parseInt(v) || 20) } }
                                CField { label: "PAT period s"; Layout.fillWidth: true; mono: true; text: String(view.tsv(advFlick.s, "patPeriodSec", 0.1)); onEdited: function(v) { view.set(advFlick.i, "encoder.ts.patPeriodSec", parseFloat(v) || 0.1) } }
                                CField { label: "UDP feed port"; Layout.fillWidth: true; mono: true; text: String(advFlick.s.udpPort || ""); onEdited: function(v) { view.set(advFlick.i, "udpPort", parseInt(v) || 0) } }
                                CField { label: "Provider"; Layout.fillWidth: true; text: String(view.tsv(advFlick.s, "provider", "PlayPlay")); onEdited: function(v) { view.set(advFlick.i, "encoder.ts.provider", v) } }
                                CField { label: "Feed muxrate (0 = 1.15×)"; Layout.columnSpan: 2; Layout.fillWidth: true; mono: true; text: String(view.tsv(advFlick.s, "muxrate", 0)); onEdited: function(v) { view.set(advFlick.i, "encoder.ts.muxrate", parseInt(v) || 0) } }
                            }
                            Text { text: "STATMUX"; color: Theme.text2; font.pixelSize: Theme.fs10; font.letterSpacing: 1 }
                            GridLayout { columns: 3; columnSpacing: 8; rowSpacing: 4; Layout.fillWidth: true
                                CField { label: "Weight"; Layout.fillWidth: true; mono: true; text: String((advFlick.s.statmux || {}).weight || 100); onEdited: function(v) { view.set(advFlick.i, "statmux.weight", parseInt(v) || 100) } }
                                CSelect { label: "Priority"; Layout.fillWidth: true; model: ["high", "normal", "low"]; currentIndex: view.idx(model, (advFlick.s.statmux || {}).priority || "normal", 1); onActivated: function(k) { view.set(advFlick.i, "statmux.priority", model[k]) } }
                                CButton { text: "close"; small: true; variant: "ghost"; Layout.alignment: Qt.AlignBottom; onClicked: { view.advIndex = -1 } }
                            }
                        }
                    }
                }

                // ---------------- encoders ----------------
                ColumnLayout {
                    visible: view.tab === "encoders"; Layout.fillWidth: true; Layout.fillHeight: true; spacing: 0
                    ListView {
                        Layout.fillWidth: true; Layout.preferredHeight: Math.min(count * 46 + 4, 320); clip: true; model: view.presets
                        delegate: Rectangle { width: ListView.view.width; height: 46; color: index % 2 ? Theme.bg2 : Theme.bg3
                            property var p: modelData
                            property var has: ((view.caps || {}).ffmpeg || {}).has || ({})
                            RowLayout { anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 8; spacing: 10
                                ColumnLayout { Layout.preferredWidth: 220; spacing: 1
                                    Text { text: p.id; color: Theme.text0; font.pixelSize: Theme.fs12; font.bold: true }
                                    Text { text: p.label || ""; color: Theme.text3; font.pixelSize: Theme.fs10; elide: Text.ElideRight; Layout.fillWidth: true } }
                                Text { text: p.codec + "  " + p.width + "x" + p.height + (p.interlaced ? "i" : "p") + "  " + p.vidBitrateKbps + "k " + p.rateControl + (p.maxrateKbps && p.rateControl === "vbr" ? " max " + p.maxrateKbps + "k" : "") + "  " + p.profile + "@" + p.level + "  gop " + p.gop + "/" + p.bframes + "B  " + (p.audio || {}).codec + " " + (p.audio || {}).bitrateKbps + "k dn " + (p.audio || {}).dialnorm; color: has[p.codec] === false ? Theme.warn : Theme.text1; font.pixelSize: Theme.fs10; font.family: Theme.fontMono; Layout.fillWidth: true; elide: Text.ElideRight }
                                CBadge { visible: has[p.codec] === false; text: "codec missing"; tint: Theme.warn }
                                CSelect { Layout.preferredWidth: 150; model: ["apply to…"].concat(view.services.map(function(s) { return s.major + "." + s.minor + " " + (s.longName || s.id) })); onActivated: function(k) { if (k > 0) view.applyPreset(k - 1, p.id); currentIndex = 0 } } } }
                    }
                    Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border0 }
                    Flickable { Layout.fillWidth: true; Layout.fillHeight: true; clip: true; contentHeight: capsText.implicitHeight + 20
                        TextEdit { id: capsText; width: parent.width; readOnly: true; selectByMouse: true; padding: 10; color: Theme.text1; font.pixelSize: Theme.fs10; font.family: Theme.fontMono; wrapMode: TextEdit.Wrap
                            text: view.caps ? (function(c) { var ff = c.ffmpeg || {}, has = ff.has || {}, b = ff.build || {}, td = c.tsduck || {}, dk = c.dektec || {}; var yes = function(v) { return v === undefined ? "?" : v ? "yes" : "NO" }
                                var lines = ["FFMPEG  " + (ff.found ? "found" : "NOT FOUND") + "  " + ff.path, "  " + (ff.version || ""), "  encoders: " + ["libx264", "libx265", "mpeg2video", "h264_qsv", "hevc_qsv", "h264_nvenc", "hevc_nvenc", "ac3", "eac3", "aac", "mp2"].map(function(n) { return n + "=" + yes(has[n]) }).join("  "), "  build: srt=" + yes(b.srt) + " rist=" + yes(b.rist) + " libndi=" + yes(b.ndi) + " qsv=" + yes(b.qsv) + " nvenc=" + yes(b.nvenc), "  " + (ff.video || []).length + " video / " + (ff.audio || []).length + " audio encoders (missing codecs fall back to libx264 with a warning)", "",
                                             "TSDUCK  " + (td.found ? td.version : "NOT FOUND") + "  " + td.dir, "  tools: " + Object.keys(td.tools || {}).map(function(n) { return n + "=" + yes(td.tools[n].found) }).join("  "), "",
                                             "DEKTEC  DTAPI " + (dk.dtapi || "?") + "  Dta " + (dk.dta || "?")]
                                for (var i = 0; i < (dk.devices || []).length; ++i) { var d = dk.devices[i]; lines.push("  " + d.index + ": " + d.model + "  " + d.description + "  ports " + d.ports + "  in " + d.inputs + " / out " + d.outputs + "  s/n " + d.serial + "  fw " + d.firmware); for (var j = 0; j < (d.channels || []).length; ++j) { var ch = d.channels[j]; lines.push("      " + ch.kind + " " + ch.index + " = port " + ch.port + (ch.label ? " (" + ch.label + ")" : "") + "  [" + ch.standards + "]") } }
                                if (!(dk.devices || []).length) lines.push("  no device found" + (dk.raw ? ": " + String(dk.raw).slice(0, 160) : ""))
                                lines.push("", "probed " + (c.probedMs ? new Date(c.probedMs).toLocaleTimeString() : "—") + " (cached 10 min)"); return lines.join("\n") })(view.caps) : "probing ffmpeg / TSDuck / DekTec…" } }
                    RowLayout { Layout.fillWidth: true; Layout.margins: 8; CButton { text: "Re-probe"; small: true; onClicked: { view.caps = null; view.loadCaps(true) } } Item { Layout.fillWidth: true } }
                }

                // ---------------- tables ----------------
                RowLayout {
                    visible: view.tab === "tables"; Layout.fillWidth: true; Layout.fillHeight: true; spacing: 0
                    Flickable { Layout.preferredWidth: 520; Layout.fillHeight: true; clip: true; contentHeight: tblCol.implicitHeight + 16
                        property var ps: (view.tables || {}).psip || ({})
                        id: tblFlick
                        ColumnLayout { id: tblCol; x: 10; y: 8; width: tblFlick.width - 20; spacing: 6
                            Text { text: "TVCT — TSID " + (tblFlick.ps.tsid || "") + "  " + (tblFlick.ps.modulation || "8-VSB") + "  v" + (tblFlick.ps.version || 0) + (tblFlick.ps.generatedMs ? "  generated " + new Date(tblFlick.ps.generatedMs).toLocaleTimeString() : ""); color: Theme.text1; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1 }
                            Repeater { model: tblFlick.ps.tvct || []
                                Rectangle { Layout.fillWidth: true; height: 54; radius: 4; color: index % 2 ? Theme.bg2 : Theme.bg3
                                    property var r: modelData
                                    property var so: ((view.ovr || {}).services || {})[r.id] || ({})
                                    ColumnLayout { anchors.fill: parent; anchors.margins: 6; spacing: 2
                                        Text { text: r.major + "." + r.minor + "  " + r.shortName + "  " + r.longName + "   prog " + r.serviceId + "  src " + r.sourceId + "  PMT 0x" + Number(r.pmtPid || 0).toString(16) + "  " + r.serviceType + (r.hidden ? "  HIDDEN" : "") + (r.hideGuide ? "  HIDE-GUIDE" : "") + (r.accessControlled ? "  CA" : ""); color: Theme.text0; font.pixelSize: Theme.fs11; font.family: Theme.fontMono; elide: Text.ElideRight; Layout.fillWidth: true }
                                        RowLayout { spacing: 6
                                            CField { Layout.preferredWidth: 70; mono: true; placeholder: "source id"; text: so.sourceId !== undefined ? String(so.sourceId) : ""; onEdited: function(v) { view.ovrSet("services." + r.id + ".sourceId", parseInt(v) || 0) } }
                                            CSelect { Layout.preferredWidth: 76; model: ["dtv", "audio", "data", "analog"]; currentIndex: view.idx(model, so.serviceType || "dtv"); onActivated: function(k) { view.ovrSet("services." + r.id + ".serviceType", model[k]) } }
                                            CToggle { label: "hidden"; implicitWidth: 76; checked: !!so.hidden; onToggled: function(v) { view.ovrSet("services." + r.id + ".hidden", v) } }
                                            CToggle { label: "hide guide"; implicitWidth: 92; checked: !!so.hideGuide; onToggled: function(v) { view.ovrSet("services." + r.id + ".hideGuide", v) } }
                                            CToggle { label: "CA"; implicitWidth: 50; checked: !!so.accessControlled; onToggled: function(v) { view.ovrSet("services." + r.id + ".accessControlled", v) } }
                                            CField { Layout.preferredWidth: 44; mono: true; placeholder: "ETM"; text: String(so.etm !== undefined ? so.etm : 0); onEdited: function(v) { view.ovrSet("services." + r.id + ".etm", parseInt(v) || 0) } } } } } }
                            GridLayout { columns: 3; columnSpacing: 8; rowSpacing: 4; Layout.fillWidth: true
                                CSelect { label: "Modulation"; Layout.fillWidth: true; model: ["8-VSB", "16-VSB", "64-QAM", "256-QAM", "analog"]; currentIndex: view.idx(model, ((view.ovr || {}).tvct || {}).modulation || "8-VSB"); onActivated: function(k) { view.ovrSet("tvct.modulation", model[k]) } }
                                CField { label: "Channel TSID (empty = TSID)"; Layout.fillWidth: true; mono: true; text: ((view.ovr || {}).tvct || {}).channelTsid !== undefined ? String(((view.ovr || {}).tvct || {}).channelTsid) : ""; onEdited: function(v) { view.ovrSet("tvct.channelTsid", parseInt(v) || 0) } }
                                CField { label: "EIT depth (days)"; Layout.fillWidth: true; mono: true; text: String((view.ovr || {}).eitDays !== undefined ? (view.ovr || {}).eitDays : 1); onEdited: function(v) { view.ovrSet("eitDays", parseFloat(v) || 1) } }
                                CField { label: "EIT-0 interval s (0 = default)"; Layout.fillWidth: true; mono: true; text: String((view.ovr || {}).eitIntervalSec || 0); onEdited: function(v) { view.ovrSet("eitIntervalSec", parseFloat(v) || 0) } }
                                CField { label: "STT interval s"; Layout.fillWidth: true; mono: true; text: String((view.ovr || {}).sttIntervalSec || 5); onEdited: function(v) { view.ovrSet("sttIntervalSec", parseInt(v) || 5) } }
                                CField { label: "MGT/TVCT interval s (0 = default)"; Layout.fillWidth: true; mono: true; text: String((view.ovr || {}).mgtIntervalSec || 0); onEdited: function(v) { view.ovrSet("mgtIntervalSec", parseFloat(v) || 0) } }
                                CToggle { label: "Transmit US RRT (region 1)"; Layout.columnSpan: 3; Layout.fillWidth: true; checked: !!(((view.ovr || {}).rrt || {}).enabled); onToggled: function(v) { view.ovrSet("rrt.enabled", v) } }
                                CField { label: "TVCT extra descriptors (TSDuck XML)"; Layout.columnSpan: 3; Layout.fillWidth: true; mono: true; text: ((view.ovr || {}).tvct || {}).extraXml || ""; onEdited: function(v) { view.ovrSet("tvct.extraXml", v) } }
                                CField { label: "Custom tables XML (injected on " + ((view.ovr || {}).customPid || "0x1FFA") + ")"; Layout.columnSpan: 3; Layout.fillWidth: true; mono: true; text: (view.ovr || {}).customXml || ""; onEdited: function(v) { view.ovrSet("customXml", v) } }
                            }
                            RowLayout { spacing: 6
                                CButton { text: "Save overrides"; small: true; variant: view.ovrDirty ? "primary" : "default"; enabled: view.ovrDirty; onClicked: { view.ovrSave() } }
                                CButton { text: "Compile"; small: true; onClicked: { api.post("/mux/tables/compile", {}, function(ok, r) { if (!r) return; var out = (r.compiled ? "ALL TABLES COMPILE" : "COMPILE FAILED: " + r.error) + "\n"; for (var i = 0; i < (r.files || []).length; ++i) out += (r.files[i].ok ? "  ok   " : "  FAIL ") + r.files[i].file + (r.files[i].output ? "  " + r.files[i].output : "") + "\n"; view.analysis = out; window.toast(r.compiled ? "All tables compile" : "Compile failed", r.compiled ? "ok" : "error"); view.loadTables() }) } }
                                CSelect { id: anMode; Layout.preferredWidth: 110; model: ["tables", "analyze"] }
                                CSelect { id: anSvc; Layout.preferredWidth: 150; model: ["mux output / first svc"].concat(view.services.map(function(s) { return s.id })) }
                                CButton { text: "Analyze live"; small: true; onClicked: { view.analysis = "analyzing…"; api.get("/mux/tables/analyze?mode=" + anMode.model[anMode.currentIndex] + (anSvc.currentIndex > 0 ? "&service=" + anSvc.model[anSvc.currentIndex] : ""), function(ok, r) { view.analysis = r ? ((r.error ? "ERROR " + r.error + "\n" : "") + (r.command || "") + "\nrc=" + r.exitCode + "  " + (r.note || "") + "\n\n" + (r.output || "(no output)")) : "request failed" }) } }
                                Text { text: "STT GPS " + ((tblFlick.ps.stt || {}).systemTimeGps || "") + "  EIT-0.." + (((view.tables || {}).eitTables || 8) - 1); color: Theme.text3; font.pixelSize: Theme.fs10; font.family: Theme.fontMono } }
                            Text { text: "MGT: " + (tblFlick.ps.mgt || []).map(function(m) { return m.type + "@" + m.pid }).join("  "); color: Theme.text3; font.pixelSize: Theme.fs10; font.family: Theme.fontMono; wrapMode: Text.Wrap; Layout.fillWidth: true }
                            TextEdit { visible: view.analysis.length > 0; Layout.fillWidth: true; readOnly: true; selectByMouse: true; text: view.analysis; color: Theme.text1; font.pixelSize: Theme.fs10; font.family: Theme.fontMono; wrapMode: TextEdit.Wrap }
                        }
                    }
                    Rectangle { Layout.preferredWidth: 1; Layout.fillHeight: true; color: Theme.border0 }
                    ColumnLayout { Layout.fillWidth: true; Layout.fillHeight: true; spacing: 0
                        RowLayout { Layout.fillWidth: true; Layout.margins: 6
                            CSelect { Layout.preferredWidth: 160; model: ((view.tables || {}).files || []).map(function(f) { return f.name }); currentIndex: view.xmlIndex; onActivated: function(k) { view.xmlIndex = k } }
                            Text { property var fl: (((view.tables || {}).files || [])[view.xmlIndex]) || ({}); text: (fl.size ? fl.size + " B" : "") + (fl.modifiedMs ? "  written " + new Date(fl.modifiedMs).toLocaleTimeString() : "") + "  · " + ((view.tables || {}).dir || ""); color: Theme.text3; font.pixelSize: Theme.fs10; elide: Text.ElideMiddle; Layout.fillWidth: true } }
                        Flickable { Layout.fillWidth: true; Layout.fillHeight: true; clip: true; contentHeight: xmlText.implicitHeight + 16
                            TextEdit { id: xmlText; width: parent.width; readOnly: true; selectByMouse: true; padding: 8; color: Theme.text1; font.pixelSize: Theme.fs10; font.family: Theme.fontMono; wrapMode: TextEdit.NoWrap; text: ((((view.tables || {}).files || [])[view.xmlIndex]) || {}).xml || "" } }
                    }
                }

                // ---------------- output ----------------
                Flickable {
                    visible: view.tab === "output"; Layout.fillWidth: true; Layout.fillHeight: true; clip: true; contentHeight: outCol.implicitHeight + 20
                    id: outFlick
                    property var out: view.cfg.output || ({})
                    ColumnLayout { id: outCol; x: 12; y: 8; width: outFlick.width - 24; spacing: 8
                        GridLayout { columns: 4; columnSpacing: 12; rowSpacing: 6; Layout.fillWidth: true
                            CSelect { label: "Output"; Layout.fillWidth: true; model: ["dektec", "udp", "srt", "file"]; currentIndex: view.idx(model, outFlick.out.kind || "dektec"); onActivated: function(k) { view.outSet("kind", model[k]) } }
                            CSelect { visible: outFlick.out.kind === "dektec" || !outFlick.out.kind; label: "DekTec device"; Layout.fillWidth: true; model: view.dektec.length ? view.dektec.map(function(d) { return d.index + ": " + d.model + " s/n " + d.serial }) : ["device " + (outFlick.out.device || 0)]; currentIndex: Math.max(0, Math.min(model.length - 1, outFlick.out.device || 0)); onActivated: function(k) { if (view.dektec.length) view.outSet("device", view.dektec[k].index) } }
                            CField { visible: outFlick.out.kind === "dektec" || !outFlick.out.kind; label: "Output channel (tsp --channel)"; Layout.fillWidth: true; mono: true; text: String(outFlick.out.channel || 0); onEdited: function(v) { view.outSet("channel", parseInt(v) || 0) } }
                            CField { visible: outFlick.out.kind === "dektec" || !outFlick.out.kind; label: "ASI port (tsdektec --output)"; Layout.fillWidth: true; mono: true; text: String(outFlick.out.port || 2); onEdited: function(v) { view.outSet("port", parseInt(v) || 2) } }
                            CField { visible: outFlick.out.kind === "udp" || outFlick.out.kind === "srt" || outFlick.out.kind === "file"; label: outFlick.out.kind === "file" ? "File path" : outFlick.out.kind === "srt" ? "SRT URL (srt://host:port?mode=caller|listener)" : "UDP URL"; Layout.columnSpan: 3; Layout.fillWidth: true; mono: true; text: outFlick.out.url || ""; onEdited: function(v) { view.outSet("url", v) } }
                            CField { label: "Transport bitrate (b/s)"; Layout.fillWidth: true; mono: true; text: String(outFlick.out.bitrate || 19392658); onEdited: function(v) { view.outSet("bitrate", parseInt(v) || 19392658) } }
                            CField { visible: outFlick.out.kind === "dektec" || !outFlick.out.kind; label: "FIFO preload ms (0 = off)"; Layout.fillWidth: true; mono: true; text: String(outFlick.out.preloadMs !== undefined ? outFlick.out.preloadMs : 4000); onEdited: function(v) { view.outSet("preloadMs", parseInt(v) || 0) } }
                            CToggle { visible: outFlick.out.kind === "dektec" || !outFlick.out.kind; label: "Maintain preload"; Layout.fillWidth: true; checked: outFlick.out.maintainPreload !== false; onToggled: function(v) { view.outSet("maintainPreload", v) } }
                            CToggle { visible: outFlick.out.kind === "dektec" || !outFlick.out.kind; label: "Null stuffing"; Layout.fillWidth: true; checked: outFlick.out.stuffing !== false; onToggled: function(v) { view.outSet("stuffing", v) } }
                            CField { visible: outFlick.out.kind === "srt"; label: "SRT latency ms"; Layout.fillWidth: true; mono: true; text: String(outFlick.out.latencyMs || 200); onEdited: function(v) { view.outSet("latencyMs", parseInt(v) || 200) } }
                            CField { label: "TSID"; Layout.fillWidth: true; mono: true; text: String(view.cfg.tsid || 25); onEdited: function(v) { view.cfgSet({ tsid: parseInt(v) || 25 }) } }
                            CField { label: "Timezone (EPG)"; Layout.fillWidth: true; text: view.cfg.timezone || ""; onEdited: function(v) { view.cfgSet({ timezone: v }) } }
                            CField { label: "Multicast base"; Layout.fillWidth: true; mono: true; text: view.cfg.mcastBase || ""; onEdited: function(v) { view.cfgSet({ mcastBase: v }) } }
                            CToggle { label: "Statmux"; Layout.fillWidth: true; checked: (view.cfg.statmux || {}).enabled !== false; onToggled: function(v) { view.cfgSet({ statmux: { enabled: v } }) } }
                            CField { label: "PSIP reserve (b/s)"; Layout.fillWidth: true; mono: true; text: String((view.cfg.statmux || {}).reservedBps || 900000); onEdited: function(v) { view.cfgSet({ statmux: { reservedBps: parseInt(v) || 900000 } }) } }
                            CToggle { label: "Auto-start (3 s, 5 retries)"; Layout.fillWidth: true; checked: !!view.cfg.autoStart; onToggled: function(v) { view.cfgSet({ autoStart: v, enabled: true }) } }
                            CField { label: "TSDuck bin dir"; Layout.fillWidth: true; mono: true; text: view.cfg.tsduckDir || ""; onEdited: function(v) { view.cfgSet({ tsduckDir: v }) } }
                            CField { label: "FFmpeg (optional)"; Layout.fillWidth: true; mono: true; placeholder: "on PATH"; text: view.cfg.ffmpegPath || ""; onEdited: function(v) { view.cfgSet({ ffmpegPath: v }) } }
                        }
                        Text { text: "tsp " + (view.status.tspFound ? "found" : "NOT FOUND") + ": " + (view.status.tsp || "") + "   ffmpeg " + (view.status.ffmpeg || ""); color: view.status.tspFound ? Theme.text3 : Theme.danger; font.pixelSize: Theme.fs10; font.family: Theme.fontMono; elide: Text.ElideMiddle; Layout.fillWidth: true }
                        Text { text: "DEKTEC DEVICES (tsdektec -a -v)"; color: Theme.text2; font.pixelSize: Theme.fs10; font.letterSpacing: 1 }
                        Repeater { model: view.dektec
                            Text { text: modelData.index + ": " + modelData.model + "  " + modelData.description + "  ports " + modelData.ports + "  s/n " + modelData.serial + "  fw " + modelData.firmware + "\n      " + (modelData.channels || []).map(function(c) { return c.kind + " " + c.index + " → port " + c.port + (c.label ? " (" + c.label + ")" : "") }).join("   "); color: Theme.text1; font.pixelSize: Theme.fs10; font.family: Theme.fontMono; Layout.fillWidth: true; wrapMode: Text.Wrap } }
                        Text { visible: !view.dektec.length; text: "no DekTec device found"; color: Theme.text3; font.pixelSize: Theme.fs10 }
                        RowLayout { spacing: 6
                            CButton { text: "Refresh devices"; small: true; variant: "ghost"; onClicked: { api.get("/mux/dektec?refresh=1", function(ok, d) { if (ok) view.dektec = d.devices || [] }) } }
                            CButton { text: "Dry run (command lines)"; small: true; onClicked: { api.post("/mux/dryrun", {}, function(ok, r) { if (ok) { view.dry = r; window.toast((r.warnings || []).length ? r.warnings.length + " warning(s)" : "Dry run ready", (r.warnings || []).length ? "error" : "ok") } }) } }
                            Text { text: view.dry ? "transport " + view.mb(view.dry.transportBps) + " Mb/s   " + ((view.dry.warnings || []).join(" · ") || "no warnings") : ""; color: view.dry && (view.dry.warnings || []).length ? Theme.warn : Theme.text3; font.pixelSize: Theme.fs10; elide: Text.ElideRight; Layout.fillWidth: true } }
                        TextEdit { visible: !!view.dry; Layout.fillWidth: true; readOnly: true; selectByMouse: true; color: Theme.text1; font.pixelSize: Theme.fs10; font.family: Theme.fontMono; wrapMode: TextEdit.Wrap
                            text: view.dry ? (function(d) { var out = []; if (d.dektecPrep) out.push("# port setup", d.dektecPrep, ""); out.push("# tsp master (" + ((d.tsp || {}).args || []).length + " args)", (d.tsp || {}).commandLine || "", ""); for (var i = 0; i < (d.encoders || []).length; ++i) { var e = d.encoders[i]; out.push("# ffmpeg encoder · service " + e.serviceId + (e.error ? "   ! " + e.error : ""), e.commandLine || "(not runnable)", "") } for (var j = 0; j < (d.channels || []).length; ++j) { var c = d.channels[j]; out.push("# " + (c.channelId ? "channel " + c.channelId + (c.managed ? " (mux-managed NDI " + ((c.fixedSource || {}).name || "") + ")" : "") : "merge " + c.merge) + " · service " + c.serviceId, c.outputs ? JSON.stringify(c.outputs[0] || {}) : "", "") } return out.join("\n") })(view.dry) : "" }
                    }
                }

                // ---------------- streams ----------------
                ColumnLayout {
                    visible: view.tab === "streams"; Layout.fillWidth: true; Layout.fillHeight: true; spacing: 0
                    ListView {
                        Layout.fillWidth: true; Layout.fillHeight: true; clip: true
                        model: (function(chs) { var rows = []; for (var i = 0; i < chs.length; ++i) { rows.push({ header: true, ch: chs[i] }); for (var j = 0; j < (chs[i].outputs || []).length; ++j) rows.push({ header: false, ch: chs[i], o: chs[i].outputs[j] }) } return rows })(view.streams)
                        delegate: Rectangle {
                            width: ListView.view.width; height: modelData.header ? 30 : 48; color: modelData.header ? Theme.bg1 : index % 2 ? Theme.bg2 : Theme.bg3
                            property var ch: modelData.ch
                            property var o: modelData.o || ({})
                            property var l: o.live || ({})
                            property string st: l.state || "pending"
                            RowLayout { visible: modelData.header; anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 8; spacing: 8
                                Text { text: ch.channelName || ch.channelId; color: Theme.text0; font.pixelSize: Theme.fs12; font.bold: true }
                                CBadge { visible: !ch.engineConnected; text: "engine offline"; tint: Theme.danger }
                                CBadge { visible: !!ch.hidden; text: "mux-managed"; tint: Theme.accent0 }
                                Text { text: ch.engineConnected ? ch.running + " / " + ch.defined + " running   " + (ch.encoders || []).map(function(e) { return "enc " + e.profile + " " + (e.videoCodec || "") + " " + e.width + "x" + e.height + " q" + e.queue + " " + Number(e.avgEncodeMs || 0).toFixed(1) + " ms" }).join("   ") : ""; color: Theme.text3; font.pixelSize: Theme.fs10; font.family: Theme.fontMono; elide: Text.ElideRight; Layout.fillWidth: true } }
                            RowLayout { visible: !modelData.header; anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 8; spacing: 8
                                CBadge { text: st; tint: view.stateColor(st); Layout.preferredWidth: 70 }
                                ColumnLayout { Layout.preferredWidth: 200; spacing: 1
                                    Text { text: o.name || o.type; color: Theme.text0; font.pixelSize: Theme.fs12; font.bold: true; elide: Text.ElideRight; Layout.fillWidth: true }
                                    Text { text: String(o.type || "").toUpperCase() + "  " + (o.url || ((o.options || {}).port !== undefined ? "port " + (o.options || {}).port : "")) + (o.profile ? "  · " + (typeof o.profile === "string" ? o.profile : (o.profile.preset || o.profile.name || "h264_aac")) : "") + (l.videoCodec ? " " + l.videoCodec : ""); color: Theme.text2; font.family: Theme.fontMono; font.pixelSize: Theme.fs10; elide: Text.ElideMiddle; Layout.fillWidth: true } }
                                Text { text: (st === "running" ? view.kb(l.bitrateKbps) : "—") + (l.bytesOut ? "  " + (l.bytesOut / 1048576).toFixed(1) + " MB" : "") + (l.dropped ? "  drop " + l.dropped : "") + (l.uptimeSec && st === "running" ? "  up " + Math.floor(l.uptimeSec / 60) + "m" : "") + (l.restarts ? "  " + l.restarts + " restarts" : "") + (l.retryInSec !== undefined ? "  retry in " + l.retryInSec + " s" : "") + (l.lastError ? "  " + l.lastError : ""); color: l.lastError ? Theme.danger : Theme.text1; font.pixelSize: Theme.fs10; font.family: Theme.fontMono; elide: Text.ElideRight; Layout.fillWidth: true }
                                CToggle { label: "auto"; implicitWidth: 64; checked: o.autoStart !== false; onToggled: function(v) { view.strSet(ch.channelId, o.name, "autoStart", v) } }
                                CToggle { label: "on"; implicitWidth: 56; checked: o.enabled !== false; onToggled: function(v) { view.strSet(ch.channelId, o.name, "enabled", v) } }
                                CButton { visible: st === "running" || st === "starting"; text: "Stop"; small: true; variant: "danger"; enabled: !!ch.engineConnected; onClicked: { view.strOp(ch.channelId, o.name, "stop") } }
                                CButton { visible: st === "running" || st === "starting"; text: "Restart"; small: true; enabled: !!ch.engineConnected; onClicked: { view.strOp(ch.channelId, o.name, "restart") } }
                                CButton { visible: !(st === "running" || st === "starting"); text: "Start"; small: true; variant: "success"; enabled: !!ch.engineConnected; onClicked: { view.strOp(ch.channelId, o.name, "start") } }
                                CButton { icon2: "\u2715"; small: true; variant: "ghost"; onClicked: { view.strRemove(ch.channelId, o.name) } } }
                        }
                        Text { anchors.centerIn: parent; visible: !view.streams.length; color: Theme.text3; font.pixelSize: Theme.fs11; text: "No channels" }
                    }
                    Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border0 }
                    RowLayout { Layout.fillWidth: true; Layout.margins: 8; spacing: 6
                        property var addCh: view.streams.length ? view.streams[Math.min(addChSel.currentIndex, view.streams.length - 1)].channelId : ""
                        id: addRow
                        CSelect { id: addChSel; label: "Channel"; Layout.preferredWidth: 130; model: view.streams.map(function(c) { return c.channelName || c.channelId }) }
                        CSelect { id: addType; label: "Type"; Layout.preferredWidth: 100; model: ["rtmp", "srt", "udp", "file", "ndi", "dektec_asi", "dektec_sdi", "decklink"]; onActivated: function(k) { var d = { rtmp: "rtmp://localhost/live/stream", srt: "srt://0.0.0.0:9000?mode=listener", udp: "udp://239.1.1.1:1234?pkt_size=1316", file: "C:/PlayPlay/record/" + addRow.addCh + ".ts" }; if (d[model[k]]) addUrl.text = d[model[k]] } }
                        CField { id: addName; label: "Name"; Layout.preferredWidth: 110; placeholder: addType.currentText }
                        CField { id: addUrl; label: "URL / NDI name / port"; Layout.fillWidth: true; mono: true; text: "rtmp://localhost/live/stream" }
                        CSelect { id: addPreset; label: "Profile"; Layout.preferredWidth: 170; model: view.presets.map(function(p) { return p.id }); currentIndex: Math.max(0, model.indexOf("stream_h264_aac_720p")) }
                        CField { id: addKbps; label: "Video kb/s"; Layout.preferredWidth: 70; mono: true; placeholder: "preset" }
                        CToggle { id: addEnabled; label: "enabled"; implicitWidth: 80; checked: true }
                        CToggle { id: addAuto; label: "auto-start"; implicitWidth: 90; checked: true }
                        CButton { text: "+ Add output"; small: true; variant: "primary"; onClicked: {
                            var t = addType.currentText; var o = { type: t, name: addName.text || t, enabled: addEnabled.checked, autoStart: addAuto.checked }
                            if (["rtmp", "srt", "udp", "file"].indexOf(t) >= 0) o.url = addUrl.text
                            if (["rtmp", "srt", "udp", "file", "dektec_asi"].indexOf(t) >= 0) { o.profile = { preset: addPreset.currentText || "h264_aac" }; if (parseInt(addKbps.text)) o.profile.videoBitrateKbps = parseInt(addKbps.text) }
                            if (t.indexOf("dektec") === 0) o.options = t === "dektec_asi" ? { port: parseInt(addUrl.text) || 2, muxrate: 19392658 } : { port: parseInt(addUrl.text) || 2, videoStd: "auto", audioChannels: 2 }
                            if (t === "decklink") o.options = { deviceIndex: parseInt(addUrl.text) || 0, videoMode: "auto", audioChannels: 2, keyer: "off" }
                            if (t === "ndi") o.sourceName = addUrl.text
                            api.post("/channels/" + encodeURIComponent(addRow.addCh) + "/outputs/add", { output: o }, function(ok, d) { window.toast(ok ? "Output " + o.name + " added" : (d && d.error ? d.error : "add failed"), ok ? "ok" : "error"); view.loadStreams() }) } }
                        CToggle { label: "show mux channels"; implicitWidth: 130; checked: view.showHidden; onToggled: function(v) { view.showHidden = v; view.loadStreams() } }
                    }
                }

                // ---------------- guide / log ----------------
                ListView {
                    visible: view.tab === "guide"; Layout.fillWidth: true; Layout.fillHeight: true; clip: true
                    model: view.guide ? 24 : 0
                    delegate: Rectangle { width: ListView.view.width; height: 24; color: index % 2 ? Theme.bg2 : Theme.bg3
                        RowLayout { anchors.fill: parent; anchors.leftMargin: 12; spacing: 12
                            Text { text: new Date(view.guide.from + index * 3600000).toLocaleTimeString(Qt.locale(), "h AP"); color: Theme.text2; font.pixelSize: Theme.fs11; font.family: Theme.fontMono; Layout.preferredWidth: 60 }
                            Repeater { model: view.guide.services; Text { Layout.fillWidth: true; text: modelData.channel + "  " + ((modelData.events[index] || {}).title || ""); color: Theme.text0; font.pixelSize: Theme.fs11; elide: Text.ElideRight } } } }
                }
                Flickable { visible: view.tab === "log"; Layout.fillWidth: true; Layout.fillHeight: true; clip: true; contentHeight: logText.implicitHeight
                    TextEdit { id: logText; width: parent.width; readOnly: true; text: view.logLines.join("\n"); color: Theme.text1; font.pixelSize: Theme.fs10; font.family: Theme.fontMono; wrapMode: TextEdit.Wrap; padding: 10; selectByMouse: true } }
            }
        }
    }
}
