import QtQuick
import ".."

/**
 * MixerState: the operator's audio mixer for the controlled channel.
 *
 * Strips come from the engine (supervisor.activeMeters.strips): "program" (the on-air item's audio, with the operator take of a live
 * input) and one strip per audio layer. Line-ins are audio layers of role "audio" (layers 61..68, source {type:"audio_in", device}),
 * so CLEAR ALL of graphics leaves them alone. Faders, mute and solo are engine commands: they act on air at once, and the strip shows
 * what the engine reports. The line-in list is saved per channel as the Supervisor setting `ui.mixer` and re-deployed when the mixer
 * opens on an engine that has lost them (engine restart).
 *
 *   afv  "audio follows video": ON = a taken input brings its own audio (default); OFF = a take mutes the taken input's audio for that
 *        item only (the take carries audioMute), so a line-in can be the voice-over; the next item has its audio again.
 *
 * Output mixes: besides "main" a channel can have extra mixes (a web stream with its own graphics and sound). `mix` is the mix the
 * faders act on. Main's faders are the engine's channel-wide ones; an extra mix's faders are its own ({action:"audio", mix, strips,
 * master, solo}) and are saved in the channel profile (PUT /channels/{id}/mixes) so an engine restart brings them back.
 */
QtObject {
    id: mx

    readonly property int baseLayer: 61
    readonly property int maxLines: 8
    property bool afv: true
    property var saved: ({})                  // channelId -> [{layer, device, name, loopback, gainDb, mute}]
    property var devices: []                  // GET /api/v1/audio/devices
    property bool loaded: false
    property string mix: "main"                // the mix the faders act on
    property var mixDefs: ({})                 // channelId -> [{id, name, strips:{id:{gainDb,mute}}, master:{gainDb}, solo, follow}]
    property Timer mixSaveTimer: Timer { interval: 700; property string ch: ""; onTriggered: { mx.saveMixes(ch) } }
    property Timer saveTimer: Timer { interval: 600; onTriggered: { mx.saveNow() } }

    function linesOf(ch) { return (saved[ch] || []).slice() }
    function refreshDevices() { api.get("/audio/devices", function(ok, d) { if (ok && d) mx.devices = d.devices || [] }) }
    function load() {
        api.get("/settings/ui.mixer", function(ok, d) {
            try {
                var o = ok && d && d.value ? JSON.parse(d.value) : null
                if (o) { mx.afv = o.afv !== false; mx.saved = o.saved || ({}) }
            } catch (e) { console.warn("[mixer] saved mixer unreadable: " + e) }
            mx.loaded = true
        })
    }
    function save() { saveTimer.restart() }
    function saveNow() { api.put("/settings/ui.mixer", { value: { afv: mx.afv, saved: mx.saved } }, function(ok) { if (!ok) window.toast("Could not save the mixer setup", "error") }) }
    function setAfv(on) { afv = on; save(); window.toast(on ? "Audio follows video: a taken input brings its audio" : "Audio follows video OFF: a take is silent, line-ins stay", "ok") }

    // ---- engine commands --------------------------------------------------------------------------------------------------------------------
    function cmd(ch, c) { supervisor.cg(c, ch) }
    function program(ch, o) { if (mx.mix !== "main") return mixStrip(ch, "program", o); cmd(ch, { action: "audio", program: o }) }
    function master(ch, db) {
        if (mx.mix !== "main") { cmd(ch, { action: "audio", mix: mx.mix, master: { gainDb: db } }); noteMix(ch, function(d) { d.master = { gainDb: db } }); return }
        cmd(ch, { action: "audio", master: { gainDb: db } })
    }
    function solo(ch, id) {
        if (mx.mix !== "main") { cmd(ch, { action: "audio", mix: mx.mix, solo: id }); return }      // solo is a monitoring aid: not saved
        cmd(ch, { action: "audio", solo: id })
    }
    function lineUpdate(ch, layer, o) {
        if (mx.mix !== "main" && (o.audioGainDb !== undefined || o.audioMute !== undefined)) {
            var so = {}; if (o.audioGainDb !== undefined) so.gainDb = o.audioGainDb; if (o.audioMute !== undefined) so.mute = o.audioMute
            return mixStrip(ch, "L" + layer, so)
        }
        var c = { action: "update", layer: layer }; for (var k in o) c[k] = o[k]; cmd(ch, c)
    }
    /// A fader / mute on an extra mix: live on the engine, then saved with the mix.
    function mixStrip(ch, strip, o) {
        var strips = {}; strips[strip] = o
        cmd(ch, { action: "audio", mix: mx.mix, strips: strips })
        noteMix(ch, function(d) { d.strips = d.strips || {}; var cur = d.strips[strip] || {}; for (var k in o) cur[k] = o[k]; d.strips[strip] = cur })
    }

    // ---- output mixes ------------------------------------------------------------------------------------------------------------------------
    function mixesOf(ch) { return (mixDefs[ch] || []).slice() }
    function loadMixes(ch) {
        if (!ch) return
        api.get("/channels/" + ch + "/mixes", function(ok, d) { if (ok && d) { var m = Object.assign({}, mx.mixDefs); m[ch] = d.mixes || []; mx.mixDefs = m } })
    }
    function noteMix(ch, fn) {
        var list = mixesOf(ch)
        for (var i = 0; i < list.length; ++i) if (list[i].id === mx.mix) { var d = JSON.parse(JSON.stringify(list[i])); fn(d); list[i] = d }
        var m = Object.assign({}, mixDefs); m[ch] = list; mixDefs = m
        mixSaveTimer.ch = ch; mixSaveTimer.restart()
    }
    function saveMixes(ch, done) {
        api.put("/channels/" + ch + "/mixes", { mixes: mixesOf(ch) }, function(ok, d) {
            if (!ok) { window.toast("Could not save the mixes", "error"); return }
            var m = Object.assign({}, mx.mixDefs); m[ch] = d.mixes || []; mx.mixDefs = m
            if (done) done()
        })
    }
    function addMix(ch, name) {
        var base = String(name || "mix").toLowerCase().replace(/[^a-z0-9_-]+/g, "-").replace(/^-+|-+$/g, "").slice(0, 24) || "mix"
        var list = mixesOf(ch), id = base, n = 2
        var taken = function(x) { if (x === "main") return true; for (var i = 0; i < list.length; ++i) if (list[i].id === x) return true; return false }
        while (taken(id)) id = base + "-" + (n++)
        list.push({ id: id, name: name || id, follow: true })
        var m = Object.assign({}, mixDefs); m[ch] = list; mixDefs = m
        saveMixes(ch, function() { mx.mix = id; window.toast("Mix added: " + (name || id) + ". Point an output at it in Outputs", "ok") })
    }
    function removeMix(ch, id) {
        var m = Object.assign({}, mixDefs); m[ch] = mixesOf(ch).filter(function(x) { return x.id !== id }); mixDefs = m
        if (mx.mix === id) mx.mix = "main"
        saveMixes(ch, function() { window.toast("Mix removed; its outputs carry main", "ok") })
    }

    // ---- line-ins --------------------------------------------------------------------------------------------------------------------------------
    function deployLine(ch, e) {
        cmd(ch, { action: "show", layer: e.layer, role: "audio", source: { type: "audio_in", device: e.device, label: e.name, loopback: !!e.loopback },
                  audioGainDb: e.gainDb, audioMute: !!e.mute, "in": { type: "cut" }, out: { type: "cut" } })
    }
    function addLine(ch, dev) {
        var list = linesOf(ch), used = {}
        for (var i = 0; i < list.length; ++i) used[list[i].layer] = true
        var layer = -1
        for (var l = baseLayer; l < baseLayer + maxLines; ++l) if (!used[l]) { layer = l; break }
        if (layer < 0) { window.toast("The mixer has " + maxLines + " line inputs already", "info"); return }
        var e = { layer: layer, device: dev.id, name: dev.name, loopback: dev.kind === "render", gainDb: 0, mute: false }
        list.push(e)
        var s = Object.assign({}, saved); s[ch] = list; saved = s
        save()
        deployLine(ch, e)
        window.toast("Line input added: " + dev.name, "ok")
    }
    function removeLine(ch, layer) {
        var s = Object.assign({}, saved); s[ch] = linesOf(ch).filter(function(e) { return e.layer !== layer }); saved = s
        save()
        cmd(ch, { action: "remove", layer: layer })
    }
    /// Remember a line-in's fader / mute so a restore brings it back as the operator left it.
    function note(ch, layer, o) {
        var list = linesOf(ch)
        for (var i = 0; i < list.length; ++i) if (list[i].layer === layer) { for (var k in o) list[i][k] = o[k] }
        var s = Object.assign({}, saved); s[ch] = list; saved = s
        save()
    }
    /// Re-deploy saved line-ins the engine does not report (it restarted); called when the mixer opens and when the engine reconnects.
    function restore(ch, strips) {
        var have = {}
        for (var i = 0; i < strips.length; ++i) if (strips[i].layer) have[strips[i].layer] = true
        var list = linesOf(ch), n = 0
        for (var j = 0; j < list.length; ++j) if (!have[list[j].layer]) { deployLine(ch, list[j]); ++n }
        if (n) console.info("[mixer] restored " + n + " line input(s) on " + ch)
    }

    Component.onCompleted: { load(); refreshDevices(); loadMixes(supervisor.activeChannelId) }
    property Connections _sup: Connections { target: supervisor; function onActiveChannelChanged() { mx.mix = "main"; mx.loadMixes(supervisor.activeChannelId) } }
}
