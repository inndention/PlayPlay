import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".."

/// OBS Scenes: the libobs live-input layer on the active channel (docs/libobs.md). Saved scene sets (GET/POST/DELETE
/// /api/v1/libobs/scenes), a JSON spec editor with catalog-driven inserts (GET /api/v1/libobs/catalog), Show / Switch / Hide
/// through /api/v1/channels/{id}/libobs/* and the live status from the engine telemetry (program, frame age, libobs fps).
/// Same functions as web/libobs.js; the form editor lives there, here the spec is edited as JSON (the Supervisor validates).
CPanel {
    id: p
    title: "OBS scenes (libobs)"
    implicitHeight: 34 + col.implicitHeight + 24
    property var catalog: ({})
    property var scenes: []                   // [{id, name, layer, spec}]
    property var st: ({})                     // GET .../libobs/status
    property int sel: -1
    property bool dirty: false
    property string err: ""
    property string switchTo: ""
    property string switchVia: ""
    property int switchMs: 500
    property int keyScene: 0
    property int keyItem: 0
    readonly property var draftScenes: { try { return JSON.parse(specEdit.text).scenes || [] } catch (e) { return [] } }
    readonly property var keyItems: draftScenes[keyScene] ? draftScenes[keyScene].items || [] : []
    function readKey() {
        var it = keyItems[keyItem], fs = it ? it.filters || [] : [], f = fs.filter(function(x) { return x.id === "chroma_key_filter_v2" })[0]
        var s = f ? f.settings || {} : {}
        keyColor.text = s.key_color || "#00ff00"; keySimilarity.text = String(s.similarity === undefined ? 400 : s.similarity)
        keySmooth.text = String(s.smoothness === undefined ? 80 : s.smoothness); keySpill.text = String(s.spill === undefined ? 100 : s.spill)
        keyOpacity.text = String(s.opacity === undefined ? 1 : s.opacity)
    }
    function applyKey(remove) {
        var s = parsed(); if (!s || !s.scenes[keyScene] || !s.scenes[keyScene].items[keyItem]) return
        if (!remove && !/^#[0-9a-f]{6}$/i.test(keyColor.text)) { err = "Screen color must be #RRGGBB"; return }
        var it = s.scenes[keyScene].items[keyItem], existing = (it.filters || []).filter(function(f) { return f.id === "chroma_key_filter_v2" })[0]; it.filters = (it.filters || []).filter(function(f) { return f.id !== "chroma_key_filter_v2" })
        if (!remove) it.filters.push({id: "chroma_key_filter_v2", name: existing && existing.name ? existing.name : "chroma_key_filter_v2", settings: {key_color_type: "custom", key_color: keyColor.text,
            similarity: Math.max(1, Math.min(1000, parseInt(keySimilarity.text) || 400)), smoothness: Math.max(1, Math.min(1000, parseInt(keySmooth.text) || 80)), spill: Math.max(1, Math.min(1000, parseInt(keySpill.text) || 100)), opacity: isNaN(parseFloat(keyOpacity.text)) ? 1 : Math.max(0,Math.min(1,parseFloat(keyOpacity.text)))}})
        specEdit.text = JSON.stringify(s, null, 2); dirty = true
    }
    function liveKey() {
        applyKey(false); if (err) return
        var s = parsed(); if (!s || !s.scenes[keyScene]) return
        var scene = s.scenes[keyScene], item = scene.items[keyItem], filter = item.filters.filter(function(f) { return f.id === "chroma_key_filter_v2" })[0]
        api.post("/channels/" + channel + "/libobs/filter", {layer: parseInt(layerField.text) || 50, scene:scene.name, item:item.name, filter:filter}, function(ok,d) { window.toast(ok ? "Live key update sent; Save retains settings" : d.error || "Live key failed", ok ? "ok" : "error") })
    }
    readonly property string channel: supervisor.activeChannelId || ""
    readonly property bool onAir: st.onAir === true
    readonly property var obs: st.obs || ({})
    readonly property var rec: sel >= 0 && sel < scenes.length ? scenes[sel] : null
    readonly property var sceneNames: {
        if (obs.scenes && obs.scenes.length) return obs.scenes
        var r = rec; if (!r || !r.spec || !r.spec.scenes) return []
        return r.spec.scenes.map(function(s) { return s.name })
    }
    readonly property var transitionIds: (catalog.transitions || []).map(function(t) { return t.id })

    function ms(v) { return v === undefined ? "–" : (Math.round(v * 10) / 10) + " ms" }
    function load() {
        if (!catalog.items) api.get("/libobs/catalog", function(ok, d) { if (ok) p.catalog = d })
        api.get("/libobs/scenes", function(ok, d) { if (!ok) return; p.scenes = d.scenes || []; if (p.sel < 0 && p.scenes.length) { p.sel = 0; p.adoptSel() } })
    }
    function loadStatus() { if (!channel) return; api.get("/channels/" + channel + "/libobs/status", function(ok, d) { p.st = ok ? d : { engineState: "UNREACHABLE" } }) }
    function adoptSel() { var r = rec; nameField.text = r ? r.name : ""; idField.text = r ? r.id : ""; layerField.text = r ? String(r.layer || 50) : "50"; specEdit.text = r ? JSON.stringify(r.spec, null, 2) : ""; keyScene = 0; keyItem = 0; readKey(); dirty = false; err = "" }
    function fresh() {
        sel = -1; nameField.text = "New scene set"; idField.text = ""; layerField.text = String(catalog.layerDefault || 50)
        specEdit.text = JSON.stringify({ type: "libobs", audio: true, scenes: [{ name: "A", items: [{ id: "color_source_v3", name: "bg", settings: { color: "#2040c0" } }] }], program: "A", transition: { id: "fade_transition", durationMs: 500 } }, null, 2)
        dirty = true; err = ""
    }
    function parsed() { try { err = ""; return JSON.parse(specEdit.text) } catch (e) { err = "JSON: " + e.message; return null } }
    function record() { var s = parsed(); if (!s) return null; return { id: idField.text, name: nameField.text, layer: parseInt(layerField.text) || 50, spec: s } }
    function validate() { var s = parsed(); if (!s) return; api.post("/libobs/validate", { spec: s, layer: parseInt(layerField.text) || 50 }, function(ok, d) { p.err = d && d.ok ? "" : (d && d.error ? d.error : "invalid"); window.toast(d && d.ok ? "Valid: " + d.spec.scenes.length + " scene(s), program " + d.spec.program : p.err, d && d.ok ? "ok" : "error") }) }
    function save() { var r = record(); if (!r) return; api.post("/libobs/scenes", r, function(ok, d) { if (!ok) { p.err = d && d.error ? d.error : "save failed"; window.toast(p.err, "error"); return } window.toast("Saved " + d.scene.name, "ok"); api.get("/libobs/scenes", function(ok2, d2) { if (!ok2) return; p.scenes = d2.scenes || []; for (var i = 0; i < p.scenes.length; i++) if (p.scenes[i].id === d.scene.id) p.sel = i; p.adoptSel() }) }) }
    function remove() { if (!rec) return; api.del("/libobs/scenes/" + rec.id, function(ok) { if (ok) { p.sel = -1; p.load(); p.fresh() } }) }
    function show() {
        if (!channel) { window.toast("Pick a channel first", "error"); return }
        var body = dirty || !rec ? (function() { var r = record(); return r ? { spec: r.spec, layer: r.layer } : null })() : { sceneId: rec.id }
        if (!body) return
        api.post("/channels/" + channel + "/libobs/show", body, function(ok, d) { window.toast(ok ? "Show accepted on " + p.channel + " (layer " + d.layer + ")" : (d && d.error ? d.error : "show failed"), ok ? "ok" : "error"); statusTimer.restart() })
    }
    function doSwitch() { if (!channel || !switchTo) return; var b = { to: switchTo }; if (switchVia) { b.transition = switchVia; b.durationMs = switchMs } api.post("/channels/" + channel + "/libobs/switch", b, function(ok, d) { window.toast(ok ? "Switch to " + b.to : (d && d.error ? d.error : "switch failed"), ok ? "ok" : "error"); statusTimer.restart() }) }
    function hide() { if (!channel) return; api.post("/channels/" + channel + "/libobs/hide", {}, function(ok, d) { window.toast(ok ? "libobs layer removed from " + p.channel : (d && d.error ? d.error : "hide failed"), ok ? "ok" : "error"); statusTimer.restart() }) }
    function insertItem(id) { var s = parsed(); if (!s || !s.scenes || !s.scenes.length || !id) return; var k = (catalog.items || []).filter(function(x) { return x.id === id })[0]; var settings = {}; (k && k.params || []).forEach(function(q) { if (q["default"] !== undefined && q.type !== "path" && q.type !== "paths" && q.type !== "url") settings[q.key] = q["default"] }); var sc = s.scenes[s.scenes.length - 1]; sc.items = sc.items || []; sc.items.push({ id: id, name: id + "_" + (sc.items.length + 1), settings: settings }); specEdit.text = JSON.stringify(s, null, 2); dirty = true }

    onChannelChanged: { st = ({}); loadStatus() }
    Component.onCompleted: { load(); loadStatus() }
    Timer { id: statusTimer; interval: 2000; repeat: true; running: p.visible; onTriggered: p.loadStatus() }

    ColumnLayout {
        id: col
        anchors.left: parent.left; anchors.right: parent.right; anchors.margins: 12
        spacing: 8
        // ---- status + transport ------------------------------------------------------------
        RowLayout { Layout.fillWidth: true; spacing: 8
            CBadge { text: p.onAir ? "ON" : "OFF"; tint: p.onAir ? Theme.onAir : Theme.text3; solid: p.onAir }
            Text { color: Theme.text1; font.pixelSize: Theme.fs11; elide: Text.ElideRight; Layout.fillWidth: true
                text: p.onAir ? "layer " + p.st.layer + " · program " + (p.obs.program || "?") + (p.obs.transition ? " · " + p.obs.transition : "")
                              : (p.st.note || (p.st.engineState && p.st.engineState !== "RUNNING" && p.st.engineState !== "PLAYING" ? "engine " + p.st.engineState : "no libobs layer on " + (p.channel || "(no channel)"))) }
            CButton { text: "Hide"; small: true; variant: "danger"; enabled: p.onAir || !!p.st.shown; onClicked: p.hide() }
        }
        Text { visible: p.onAir; color: Theme.text2; font.family: Theme.fontMono; font.pixelSize: Theme.fs10; Layout.fillWidth: true; elide: Text.ElideRight
            text: "age p50 " + p.ms(p.obs.ageP50Ms) + " · p99 " + p.ms(p.obs.ageP99Ms) + " · obs " + (p.obs.obsFps ? Math.round(p.obs.obsFps * 100) / 100 + " fps" : "–") + " · render " + p.ms(p.obs.obsFrameTimeMs) + " · lagged " + (p.obs.obsLaggedFrames !== undefined ? p.obs.obsLaggedFrames : "–") + " · switches " + (p.obs.switches || 0) }
        RowLayout { Layout.fillWidth: true; spacing: 6
            CSelect { label: "Switch to"; Layout.preferredWidth: 140; model: p.sceneNames; onActivated: function(i) { p.switchTo = p.sceneNames[i] } Component.onCompleted: { if (p.sceneNames.length) p.switchTo = p.sceneNames[0] } }
            CSelect { label: "via"; Layout.preferredWidth: 170; model: ["(scene set's)"].concat(p.transitionIds); onActivated: function(i) { p.switchVia = i === 0 ? "" : p.transitionIds[i - 1] } }
            CField { label: "ms"; Layout.preferredWidth: 60; text: String(p.switchMs); onTextChanged: { p.switchMs = parseInt(text) || 0 } }
            CButton { text: "Switch"; small: true; variant: "primary"; Layout.alignment: Qt.AlignBottom; enabled: p.onAir && p.switchTo.length > 0; onClicked: { if (!p.switchTo && p.sceneNames.length) p.switchTo = p.sceneNames[0]; p.doSwitch() } }
        }
        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border0 }
        // ---- saved scene sets + editor ------------------------------------------------------
        RowLayout { Layout.fillWidth: true; spacing: 6
            CSelect { label: "Scene set (" + p.scenes.length + ")"; Layout.fillWidth: true; model: p.scenes.map(function(s) { return s.name + "  [" + s.id + "]" }); currentIndex: p.sel; onActivated: function(i) { p.sel = i; p.adoptSel() } }
            CButton { text: "New"; small: true; Layout.alignment: Qt.AlignBottom; onClicked: p.fresh() }
            CButton { text: "Show on " + (p.channel || "…"); small: true; variant: "success"; Layout.alignment: Qt.AlignBottom; enabled: p.channel.length > 0 && (p.rec !== null || p.dirty); onClicked: p.show() }
        }
        RowLayout { Layout.fillWidth: true; spacing: 6
            CField { id: nameField; label: "Name"; Layout.fillWidth: true; onTextChanged: { if (p.rec && text !== p.rec.name) p.dirty = true } }
            CField { id: idField; label: "Id"; Layout.preferredWidth: 120; mono: true; placeholder: "from name" }
            CField { id: layerField; label: "Layer"; Layout.preferredWidth: 50; text: "50"; onTextChanged: { p.dirty = true } }
            CSelect { label: "+ item"; Layout.preferredWidth: 150; model: ["insert…"].concat((p.catalog.items || []).map(function(k) { return k.name })); onActivated: function(i) { if (i > 0) p.insertItem(p.catalog.items[i - 1].id); currentIndex = 0 } }
        }
        Text { text: "CHROMA KEY"; color: Theme.text1; font.bold: true; font.pixelSize: Theme.fs11 }
        RowLayout { Layout.fillWidth: true
            CSelect { label: "Scene"; Layout.fillWidth: true; model: p.draftScenes.map(function(s) { return s.name }); currentIndex: p.keyScene; onActivated: function(i) { p.keyScene = i; p.keyItem = 0; p.readKey() } }
            CSelect { label: "Source"; Layout.fillWidth: true; model: p.keyItems.map(function(s) { return s.name || s.id }); currentIndex: p.keyItem; onActivated: function(i) { p.keyItem = i; p.readKey() } }
        }
        RowLayout { Layout.fillWidth: true
            CField { id: keyColor; label: "Screen color"; text: "#00ff00"; Layout.preferredWidth: 105 }
            CButton { text: "Eyedropper / color"; small: true; Layout.alignment: Qt.AlignBottom; onClicked: { var c = nativeTools.pickScreenColor(keyColor.text); if (c) keyColor.text = c } }
        }
        RowLayout { Layout.fillWidth: true
            CField { id: keySimilarity; label: "Similarity"; text: "400"; Layout.fillWidth: true }
            CField { id: keySmooth; label: "Smoothing"; text: "80"; Layout.fillWidth: true }
            CField { id: keySpill; label: "Spill"; text: "100"; Layout.fillWidth: true }
        }
        RowLayout { Layout.fillWidth: true
            CField { id: keyOpacity; label: "Opacity (0–1)"; text: "1"; Layout.preferredWidth: 105 }
            CButton { text: "Apply key"; small: true; Layout.alignment: Qt.AlignBottom; enabled: p.keyItems.length > p.keyItem; onClicked: p.applyKey(false) }
            CButton { text: "Apply live"; small: true; Layout.alignment: Qt.AlignBottom; enabled: p.onAir && p.keyItems.length > p.keyItem; onClicked: p.liveKey() }
            CButton { text: "Remove"; small: true; Layout.alignment: Qt.AlignBottom; enabled: p.keyItems.length > p.keyItem; onClicked: p.applyKey(true) }
        }
        Text { text: p.obs.filterError || "Apply live to the shown scene; Save retains settings. Range: 1–1000."; color: p.obs.filterError ? Theme.danger : Theme.text3; font.pixelSize: Theme.fs10; wrapMode: Text.WordWrap; Layout.fillWidth: true }
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 180; color: Theme.bg0; border.color: p.err ? Theme.danger : Theme.border0; radius: 4
            ScrollView { anchors.fill: parent; anchors.margins: 4; clip: true
                TextArea { id: specEdit; color: Theme.text0; font.family: Theme.fontMono; font.pixelSize: Theme.fs11; wrapMode: TextEdit.NoWrap; background: null; selectByMouse: true; placeholderText: "spec JSON: { scenes: [...], program, transition }"; onTextChanged: { p.dirty = true } }
            }
        }
        RowLayout { Layout.fillWidth: true; spacing: 6
            Text { Layout.fillWidth: true; elide: Text.ElideRight; color: p.err ? Theme.danger : Theme.text3; font.pixelSize: Theme.fs10
                text: p.err ? p.err : "one libobs layer per engine; Hide removes it so the next Show can open · a cut lands 2 frames after the command (docs/libobs.md)" }
            CButton { text: "Validate"; small: true; variant: "ghost"; onClicked: p.validate() }
            CButton { text: "Delete"; small: true; variant: "ghost"; enabled: p.rec !== null; onClicked: p.remove() }
            CButton { text: "Save"; small: true; variant: "primary"; enabled: p.dirty; onClicked: p.save() }
        }
    }
}
