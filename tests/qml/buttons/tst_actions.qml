import QtQuick
import QtQuick.Controls
import QtTest
import PlayPlay.Buttons
import "../../../Play_Play_Native/qml/ui" as Ui

// UXB-7: the action registry (ui/Actions.qml), the deck built from it (ui/ActionDeck.qml) and the custom hotkeys
// (ui/HotkeyHost.qml), with fake supervisor / api / inputMap objects that record every call. It checks:
// - every safetyDefaults id is known, the default orders are today's deck / switch row / column;
// - the stateful actions read their state (PAUSE / PLAY, ARMED / DISARMED with the arm floor only when disarming, ENGINE
//   ON / OFF with hold only when stopping), enabled and lit follow today's keys;
// - run() calls exactly what today's key calls: supervisor.pause / play / stop(ch) / skip / hold / snap / arm(!armed) /
//   loadHour, TOH = arm(true) then loadHour(), engine.toggle = POST /channels/{ch}/stop|start then refresh(), inputMap.cut /
//   autoTake, and the REST paths of the Remote / Mux / Output / Facebook Live / CG keys; an unknown or disabled action runs
//   nothing;
// - ActionDeck: the keys in order with separators, label / icon / size / enabled / lit from the registry, prefs applied
//   (label, colour, size, hidden, order, hotkey, a safety never below the floor), STOP arms then fires, NEXT fires at once,
//   a right click asks for the editor;
// - HotkeyHost: a custom chord presses the deck's key (so STOP's F9 arms first), a reserved or disallowed chord is never
//   bound, nothing fires while typing, while a question is open or while inactive, and without a deck only a safety-none
//   action runs.
TestCase {
    id: tc
    name: "Actions"
    when: windowShown
    width: 760; height: 300

    // --- fakes ---------------------------------------------------------------------------------------------------------
    Component {
        id: clientC
        QtObject {
            property var calls: []
            function play(ch) { calls.push(["play", ch]) }
            function pause(ch) { calls.push(["pause", ch]) }
            function stop(ch) { calls.push(["stop", ch]) }
            function skip(ch) { calls.push(["skip", ch]) }
            function hold(ch) { calls.push(["hold", ch]) }
            function snap(ch) { calls.push(["snap", ch]) }
            function arm(on, ch) { calls.push(["arm", on, ch]) }
            function loadHour(ch) { calls.push(["loadHour", ch]) }
            function refresh() { calls.push(["refresh"]) }
            function cg(cmd, ch) { calls.push(["cg", JSON.parse(JSON.stringify(cmd)), ch]) }
            function names() { return calls.map(c => c[0]) }
        }
    }
    Component {
        id: restC
        QtObject {
            property var calls: []
            property bool ok: true
            property var answer: ({})
            function post(path, body, cb) { calls.push(["post", path, JSON.parse(JSON.stringify(body))]); if (cb) cb(ok, ok ? answer : { error: "nope" }) }
            function put(path, body, cb) { calls.push(["put", path, JSON.parse(JSON.stringify(body))]); if (cb) cb(ok, ok ? answer : { error: "nope" }) }
            function del(path, cb) { calls.push(["del", path]); if (cb) cb(ok, ok ? answer : { error: "nope" }) }
            property var getAnswer: ({ value: null })
            function get(path, cb) { calls.push(["get", path]); if (cb) cb(true, getAnswer) }
            function last() { return calls[calls.length - 1] }
        }
    }
    Component {
        id: inputsC
        QtObject {
            property var calls: []
            function cut(ch) { calls.push(["cut", ch]) }
            function autoTake(ch) { calls.push(["autoTake", ch]) }
        }
    }
    Component { id: actsC; Ui.Actions { } }
    Component { id: deckC; Ui.ActionDeck { } }
    Component { id: hostC; Ui.HotkeyHost { } }
    Component { id: spyC; SignalSpy { } }
    Component { id: fieldC; TextInput { width: 100; height: 20 } }

    property var client: null
    property var rest: null
    property var inputs: null
    property var toasts: []

    function stage() { return tc.Window.contentItem }
    function spy(target, sig) { return createTemporaryObject(spyC, tc, { target: target, signalName: sig }) }
    function settle() { wait(BtnTokens.timing.minIntervalMs + 60) }
    function away() { mouseMove(stage(), stage().width - 2, stage().height - 2) }
    // a registry on an On Air channel "ch2", engine up and playing, disarmed, nothing on preview
    function acts(props) {
        const a = createTemporaryObject(actsC, tc, Object.assign({
            client: tc.client, rest: tc.rest, inputs: tc.inputs, notify: function(m, l) { tc.toasts.push([m, l]) },
            channel: "ch2", engineState: "ON_AIR", engineUp: true, armed: false, onTake: false, pvw: -1 }, props || {}))
        verify(a !== null, "Actions did not create")
        return a
    }
    function deck(a, props) {
        const d = createTemporaryObject(deckC, stage(), Object.assign({ actions: a, x: 10, y: 10, width: 740 }, props || {}))
        verify(d !== null, "ActionDeck did not create")
        waitForRendering(d)
        return d
    }

    function init() {
        tc.client = createTemporaryObject(clientC, tc); tc.rest = createTemporaryObject(restC, tc); tc.inputs = createTemporaryObject(inputsC, tc)
        tc.toasts = []
        ButtonPrefs.backend = null; ButtonPrefs.data = ButtonPrefs.empty(); ButtonPrefs.dirty = false; ButtonPrefs.autoRefresh = false
        ButtonPrefs.revision++
        if (BtnTokens.armedItem) BtnTokens.armedItem.disarm()
    }
    function cleanup() { away(); ButtonPrefs.autoRefresh = true }

    // --- the registry ----------------------------------------------------------------------------------------------------
    function test_registry() {
        const a = acts()
        const ids = Object.keys(BtnTokens.safetyDefaults)
        verify(ids.length >= 28, "the design's 28 action ids are in buttons.json")
        compare(a.ids, ids)
        for (const id of ids) {
            verify(a.known(id), id + " is known")
            const g = a.get(id)
            verify(g !== null, id + " resolves")
            compare(g.id, id); verify(g.label !== ""); verify(BtnTokens.icons[g.icon] !== undefined, id + " icon " + g.icon + " is drawn")
            verify(BtnTokens.roles[g.role] !== undefined, id + " role"); verify(BtnTokens.safetyLevels.indexOf(g.safety) >= 0)
            verify(BtnTokens.safetyLevels.indexOf(g.floor) >= 0); verify(typeof g.run === "function")
            verify(BtnTokens.safetyLevels.indexOf(g.safety) >= BtnTokens.safetyLevels.indexOf(g.floor), id + ": the default is never below its floor")
        }
        compare(a.get("no.such"), null); verify(!a.known("no.such")); compare(a.spec("no.such"), null)
        verify(!a.run("no.such")); compare(tc.client.calls, []); compare(tc.rest.calls, [])
        // today's orders (OnAirView.qml:480-491, 281-283; ControlColumn.qml:94-107)
        compare(a.defaultOrder("onair.deck"), ["transport.playpause", "transport.stop", "transport.next", "transport.hold", "automation.snap", "|",
                                               "automation.arm", "automation.loadhour", "automation.toh", "|", "engine.toggle"])
        compare(a.defaultOrder("onair.switch"), ["bus.cut", "bus.auto", "schedule.return"])
        compare(a.defaultOrder("column.transport"), ["automation.arm", "automation.snap", "transport.stop", "transport.playpause", "transport.next", "|",
                                                     "engine.toggle", "automation.loadhour", "automation.toh"])
        compare(a.defaultOrder("onair.bus"), [])
        const d1 = a.defaultOrder("onair.deck"); d1.push("x"); compare(a.defaultOrder("onair.deck").length, 11, "a copy each time")
        // critical: a floor above none or a stop / destructive role
        verify(a.critical("transport.stop")); verify(a.critical("channel.delete")); verify(a.critical("bus.cut"), "CUT is role stop")
        verify(!a.critical("transport.next")); verify(!a.critical("automation.snap")); verify(!a.critical("no.such"))
    }

    function test_state() {
        const a = acts()
        // PLAY / PAUSE (OnAirView.qml:480): PAUSE while on air, lit; PLAY otherwise
        let g = a.get("transport.playpause")
        compare(g.label, "PAUSE"); compare(g.icon, "pause"); verify(g.on); verify(g.active); verify(g.enabled); compare(g.safety, "none"); compare(g.hotkey, "Space")
        a.engineState = "READY"
        g = a.get("transport.playpause"); compare(g.label, "PLAY"); compare(g.icon, "play"); verify(!g.on); verify(!g.active); verify(g.enabled)
        a.engineUp = false
        g = a.get("transport.playpause"); verify(!g.enabled, "enabled: view.engineUp")
        for (const id of ["transport.stop", "transport.next", "transport.hold", "automation.snap", "automation.loadhour", "automation.toh"])
            verify(!a.enabled(id), id + " is disabled while the engine is down")
        verify(a.enabled("automation.arm"), "ARMED has no enabled gate today")
        verify(a.enabled("engine.toggle"))
        a.engineUp = true
        // HOLD lit on HOLD
        a.engineState = "HOLD"; verify(a.active("transport.hold")); verify(!a.active("transport.playpause"))
        a.engineState = "ON_AIR"; verify(!a.active("transport.hold"))
        verify(!a.active("transport.stop")); verify(!a.active("transport.next")); verify(!a.active("automation.snap"))
        // ARMED / DISARMED (OnAirView.qml:487): the arm safety only when disarming
        g = a.get("automation.arm")
        compare(g.label, "DISARMED"); compare(g.icon, "bolt-off"); compare(g.safety, "none"); compare(g.floor, "none"); verify(!g.on); verify(!g.active)
        a.armed = true
        g = a.get("automation.arm"); compare(g.label, "ARMED"); compare(g.icon, "bolt"); compare(g.safety, "arm"); compare(g.floor, "arm"); verify(g.on); verify(g.active)
        // ENGINE ON / OFF (OnAirView.qml:491): hold only when stopping the process
        g = a.get("engine.toggle"); compare(g.label, "ENGINE ON"); compare(g.icon, "power"); compare(g.safety, "hold"); compare(g.floor, "hold"); verify(g.on); verify(g.active)
        compare(g.tip, "Stop the channel engine process")
        a.engineUp = false
        g = a.get("engine.toggle"); compare(g.label, "ENGINE OFF"); compare(g.icon, "power"); compare(g.safety, "none"); compare(g.floor, "none"); verify(!g.on); verify(!g.active)
        compare(g.tip, "Start the channel engine process")
        a.engineUp = true
        // the switch row (OnAirView.qml:281-283)
        verify(!a.enabled("bus.cut")); verify(!a.active("bus.cut")); verify(!a.enabled("bus.auto"))
        a.pvw = 2; verify(a.enabled("bus.cut")); verify(a.active("bus.cut")); verify(a.enabled("bus.auto")); verify(a.active("bus.auto"))
        a.engineUp = false; verify(!a.enabled("bus.cut"), "CUT needs the engine"); verify(a.active("bus.cut"), "but is lit by the preview"); a.engineUp = true
        verify(!a.enabled("schedule.return")); verify(!a.active("schedule.return"))
        a.onTake = true; verify(a.enabled("schedule.return")); verify(a.active("schedule.return"))
        compare(a.get("bus.cut").hotkey, "Ctrl+Return"); compare(a.get("transport.next").hotkey, "Ctrl+Right")
        // the rest: enabled, never lit
        for (const id of ["engine.restart", "cg.clearAll", "mux.stop", "output.stop", "live.end", "failover.promote", "channel.delete", "media.remove"]) {
            verify(a.enabled(id), id); verify(!a.active(id), id)
        }
    }

    function test_runToday() {
        const a = acts()
        const ran = spy(a, "ran")
        const c = tc.client, r = tc.rest, i = tc.inputs
        function last() { return c.calls[c.calls.length - 1] }
        // deck
        verify(a.run("transport.playpause")); compare(last(), ["pause", undefined], "on air: supervisor.pause()")
        a.engineState = "READY"; verify(a.run("transport.playpause")); compare(last(), ["play", undefined], "else supervisor.play()")
        a.engineState = "ON_AIR"
        verify(a.run("transport.stop")); compare(last(), ["stop", "ch2"], "supervisor.stop(view.ch)")
        verify(a.run("transport.next")); compare(last(), ["skip", undefined])
        verify(a.run("transport.hold")); compare(last(), ["hold", undefined])
        verify(a.run("automation.snap")); compare(last(), ["snap", undefined])
        verify(a.run("automation.arm")); compare(last(), ["arm", true, undefined], "disarmed: arm(true)")
        a.armed = true; verify(a.run("automation.arm")); compare(last(), ["arm", false, undefined], "armed: arm(false)"); a.armed = false
        verify(a.run("automation.loadhour")); compare(last(), ["loadHour", undefined])
        const n = c.calls.length
        verify(a.run("automation.toh")); compare(c.calls.slice(n), [["arm", true, undefined], ["loadHour", undefined]], "TOH: arm(true) then loadHour()")
        compare(r.calls, [], "none of these touch REST")
        compare(ran.count, 10)
        // the engine process: POST then refresh() in the answer
        let got = null
        verify(a.run("engine.toggle", undefined, function(ok, d) { got = [ok, d] }))
        compare(r.last(), ["post", "/channels/ch2/stop", {}]); compare(last(), ["refresh"]); compare(got, [true, {}])
        a.engineUp = false; verify(a.run("engine.toggle")); compare(r.last(), ["post", "/channels/ch2/start", {}]); compare(last(), ["refresh"]); a.engineUp = true
        r.ok = false; const failed = spy(a, "failed"); const nc = c.calls.length
        verify(a.run("engine.toggle")); compare(last(), ["refresh"], "refresh() whatever the answer, as today"); compare(c.calls.length, nc + 1)
        compare(failed.count, 1); compare(failed.signalArguments[0][0], "engine.toggle"); compare(failed.signalArguments[0][1], "nope"); r.ok = true
        verify(a.run("engine.restart")); compare(r.last(), ["post", "/channels/ch2/restart", {}])
        verify(a.run("engine.restart", "main")); compare(r.last(), ["post", "/channels/main/restart", {}], "a channel argument")
        // the switch row
        verify(!a.run("bus.cut"), "nothing on preview: disabled, as the key"); compare(i.calls, [])
        a.pvw = 1
        verify(a.run("bus.cut")); compare(i.calls, [["cut", undefined]])
        verify(a.run("bus.auto")); compare(i.calls[1], ["autoTake", undefined])
        verify(a.run("bus.take")); compare(i.calls[2], ["cut", undefined], "no native TAKE key: a cut")
        verify(!a.run("schedule.return")); a.onTake = true; verify(a.run("schedule.return")); compare(last(), ["skip", undefined], "SCHEDULE = supervisor.skip()")
        // graphics
        verify(a.run("cg.clearAll")); compare(last(), ["cg", { action: "clear" }, undefined])
        verify(a.run("cg.emergency", { type: "file", path: "D:/slate.mp4", loop: true }))
        compare(last(), ["cg", { action: "show", role: "emergency", source: { type: "file", path: "D:/slate.mp4", loop: true }, "in": { type: "cut" } }, undefined])
        verify(a.run("cg.emergency")); compare(last()[1].source, { type: "bars", captions: ["PLEASE STAND BY"] }, "ConfigView's fallback")
        // mux, outputs, live, failover, deletes
        verify(a.run("mux.stop")); compare(r.last(), ["post", "/mux/stop", {}])
        verify(a.run("mux.restart")); compare(r.last(), ["post", "/mux/restart", {}])
        verify(a.run("output.stop", "ndi 1")); compare(r.last(), ["post", "/channels/ch2/outputs/ndi%201/stop", {}], "OutputView.op: encoded names")
        verify(a.run("output.remove", "ndi1")); compare(r.last(), ["del", "/channels/ch2/outputs/ndi1"])
        verify(!a.run("output.stop"), "an output needs its name"); verify(!a.run("output.remove", ""))
        verify(a.run("live.start")); compare(r.last(), ["post", "/live/facebook/start", {}])
        verify(a.run("live.end")); compare(r.last(), ["post", "/live/facebook/stop", {}])
        verify(a.run("failover.promote")); compare(r.last(), ["post", "/failover/promote", { reason: "operator (app)" }])
        verify(a.run("failover.demote")); compare(r.last(), ["post", "/failover/demote", { reason: "operator (app)" }])
        const nr = c.calls.length
        verify(a.run("channel.delete", "old")); compare(r.last(), ["del", "/channels/old"]); compare(c.calls.slice(nr), [["refresh"]], "ConfigView: refresh() after the delete")
        verify(!a.run("channel.delete"), "needs the id")
        verify(a.run("media.remove", "m1")); compare(r.last(), ["post", "/media/delete", { ids: ["m1"] }])
        verify(a.run("media.remove", ["m1", "m2"])); compare(r.last(), ["post", "/media/delete", { ids: ["m1", "m2"] }])
        verify(a.run("session.delete", "s1")); compare(r.last(), ["del", "/sessions/s1"])
        verify(a.run("node.delete", { id: "n2", nodes: [{ id: "n1" }, { id: "n2" }, { id: "n3" }] }))
        compare(r.last(), ["put", "/remote/nodes", { nodes: [{ id: "n1" }, { id: "n3" }] }])
        verify(!a.run("node.delete", "n2"), "needs the list too")
        // the bound run() of get()
        const nn = c.calls.length
        verify(a.get("transport.next").run()); compare(c.calls.length, nn + 1); compare(last(), ["skip", undefined])
    }

    function test_runGuards() {
        // nothing injected: a warning, false, nothing thrown
        const a = acts({ client: null, rest: null, inputs: null, pvw: 3, onTake: true })
        ignoreWarning(/Actions: transport.stop needs client/)
        verify(!a.run("transport.stop"))
        ignoreWarning(/Actions: engine.toggle needs rest/)
        verify(!a.run("engine.toggle"))
        ignoreWarning(/Actions: bus.cut needs inputs/)
        verify(!a.run("bus.cut"))
        // disabled: nothing runs
        const b = acts({ engineUp: false, engineState: "OFFLINE" })
        for (const id of ["transport.playpause", "transport.stop", "transport.next", "transport.hold", "automation.snap", "automation.loadhour", "automation.toh", "bus.cut", "schedule.return"])
            verify(!b.run(id), id + " is disabled while the engine is down")
        compare(tc.client.calls, []); compare(tc.inputs.calls, [])
        verify(b.run("automation.arm")); compare(tc.client.calls, [["arm", true, undefined]])
        verify(b.run("engine.toggle")); compare(tc.rest.last(), ["post", "/channels/ch2/start", {}])
    }

    // --- the deck --------------------------------------------------------------------------------------------------------
    function test_deckBuild() {
        const a = acts()
        const d = deck(a)
        compare(d.keys, a.defaultOrder("onair.deck")); compare(d.keyCount, 9)
        const labels = ["PAUSE", "STOP", "NEXT", "HOLD", "SNAP", "DISARMED", "LOAD HOUR", "TOH", "ENGINE ON"]
        const icons = ["pause", "stop", "skip-next", "hold", "snap", "bolt-off", "load-hour", "alarm", "power"]
        const ids = d.keys.filter(k => k !== "|")
        for (let i = 0; i < ids.length; ++i) {
            const k = d.keyFor(ids[i])
            verify(k !== null, ids[i] + " has a key")
            compare(k.objectName, "deck-" + ids[i]); compare(k.actionId, ids[i])
            compare(k.text, labels[i], ids[i]); compare(k.icon.name, icons[i], ids[i])
            compare(k.size, "l", "the surface's size"); compare(k.height, 46); verify(k.enabled, ids[i] + " enabled")
            compare(k.hotkey, a.get(ids[i]).hotkey, "no custom hotkey: the design's hint (Space, Ctrl+Right) shows in the tooltip")
            compare(k.role, a.get(ids[i]).role)
        }
        verify(d.keyFor("|") === null); verify(d.keyFor("no.such") === null)
        // separators where today's dividers are, and the keys in x order
        const dx = id => d.keyFor(id).mapToItem(d, 0, 0).x        // a key's x is relative to its Loader
        let xs = []
        for (let i = 0; i < ids.length; ++i) xs.push(dx(ids[i]))
        for (let i = 1; i < xs.length; ++i) verify(xs[i] > xs[i - 1], "keys left to right")
        verify(dx("automation.arm") - (dx("automation.snap") + d.keyFor("automation.snap").width) > d.spacing * 2 - 1, "a separator between SNAP and ARMED")
        verify(dx("transport.next") - (dx("transport.stop") + d.keyFor("transport.stop").width) < d.spacing + 1, "none between STOP and NEXT")
        // lit and safety follow the state
        compare(d.keyFor("transport.playpause").active, true); compare(d.keyFor("transport.hold").active, false)
        compare(d.keyFor("transport.stop").safety, "arm"); compare(d.keyFor("engine.toggle").safety, "hold"); compare(d.keyFor("automation.arm").safety, "none")
        a.engineState = "HOLD"
        compare(d.keyFor("transport.hold").active, true); compare(d.keyFor("transport.playpause").active, false); compare(d.keyFor("transport.playpause").text, "PLAY")
        a.armed = true
        compare(d.keyFor("automation.arm").text, "ARMED"); compare(d.keyFor("automation.arm").active, true); compare(d.keyFor("automation.arm").safety, "arm")
        a.engineUp = false; a.engineState = "OFFLINE"
        verify(!d.keyFor("transport.stop").enabled); verify(d.keyFor("automation.arm").enabled)
        compare(d.keyFor("engine.toggle").text, "ENGINE OFF"); compare(d.keyFor("engine.toggle").safety, "none"); verify(!d.keyFor("engine.toggle").active)
        // other surfaces
        const s = deck(a, { surface: "onair.switch", y: 80 })
        compare(s.keys, ["bus.cut", "bus.auto", "schedule.return"]); compare(s.keyFor("bus.cut").text, "CUT"); verify(!s.keyFor("bus.cut").enabled)
        compare(s.keyFor("bus.cut").hotkey, "Ctrl+Return", "the hint; the shortcut itself stays main.qml's")
        const col = deck(a, { surface: "column.transport", y: 150, size: "" })
        compare(col.keyFor("automation.arm").size, "s", "the column's first size"); tryCompare(col.keyFor("automation.arm"), "height", 24)
        const col2 = deck(a, { surface: "column.transport", y: 190, size: "m" })
        compare(col2.keyFor("automation.arm").size, "m")
        const none = deck(a, { surface: "onair.bus", y: 240 }); compare(none.keys, []); compare(none.keyCount, 0)
        const empty = deck(null, { y: 240 }); compare(empty.keys, []); verify(empty.keyFor("transport.stop") === null); verify(!empty.trigger("transport.stop"))
    }

    function test_deckPrefs() {
        const a = acts()
        const d = deck(a)
        ButtonPrefs.setButton("onair.deck", "transport.stop", { label: "HALT", color: "#123456", size: "xl", hotkey: "Ctrl+Shift+S", safety: "modal" })
        let k = d.keyFor("transport.stop")
        tryCompare(k, "text", "HALT"); compare(k.size, "xl"); tryCompare(k, "height", 58); compare(k.hotkey, "Ctrl+Shift+S"); compare(k.safety, "modal")
        verify(Qt.colorEqual(k.customColor, "#123456")); verify(Qt.colorEqual(k.tint, "#123456"), "the key is tinted by the colour")
        verify(k.tipText.indexOf("(Ctrl+Shift+S)") > 0, "the chord shows in the tooltip")
        ButtonPrefs.setButton("onair.deck", "transport.stop", { safety: "none" })
        compare(k.safety, "arm", "never below the floor")
        ButtonPrefs.setButton("onair.deck", "transport.next", { icon: "play", safety: "arm" })
        compare(d.keyFor("transport.next").icon.name, "play"); compare(d.keyFor("transport.next").safety, "arm", "raised above a none floor")
        // the arm key's floor follows its state
        ButtonPrefs.setButton("onair.deck", "automation.arm", { safety: "none" })
        compare(d.keyFor("automation.arm").safety, "none"); a.armed = true; compare(d.keyFor("automation.arm").safety, "arm", "disarming keeps the arm floor"); a.armed = false
        // hidden and order
        ButtonPrefs.setHidden("onair.deck", "transport.next", true)
        tryCompare(d, "keyCount", 8); verify(d.keyFor("transport.next") === null)
        ButtonPrefs.setOrder("onair.deck", ["engine.toggle", "|", "transport.stop"])
        tryVerify(() => d.keys[0] === "engine.toggle", 1000)
        compare(d.keys, ["engine.toggle", "|", "transport.stop", "transport.playpause", "transport.hold", "automation.snap", "automation.arm", "automation.loadhour", "automation.toh"])
        compare(d.keyCount, 8)
        tryVerify(() => d.keyFor("engine.toggle").mapToItem(d, 0, 0).x < d.keyFor("transport.stop").mapToItem(d, 0, 0).x, 1000, "laid out in the new order")
        compare(d.keyFor("transport.stop").text, "HALT", "the customised key survives the reorder")
        ButtonPrefs.resetSurface("onair.deck")
        tryCompare(d, "keyCount", 9); compare(d.keys, a.defaultOrder("onair.deck")); compare(d.keyFor("transport.stop").text, "STOP"); compare(d.keyFor("transport.stop").size, "l")
        // a reload from the backend reaches the deck
        const rest2 = tc.rest
        rest2.getAnswer = { value: JSON.stringify({ version: 1, buttons: { "onair.deck/transport.hold": { label: "WAIT" } } }) }
        ButtonPrefs.dirty = false                         // the edits above would otherwise keep a reload out, as designed
        ButtonPrefs.attach(rest2)
        tryCompare(d.keyFor("transport.hold"), "text", "WAIT"); compare(rest2.calls[rest2.calls.length - 1], ["get", "/settings/ui.buttons"])
        // the deck attaches a backend it is given (OnAirView passes api) and asks for a reload when shown
        ButtonPrefs.backend = null
        const d2 = deck(a, { backend: rest2, y: 100 })
        compare(ButtonPrefs.backend, rest2)
        ButtonPrefs._touched = 0; const n = rest2.calls.length
        d2.visible = false; d2.visible = true; compare(rest2.calls.length, n + 1, "touch() on show")
        ButtonPrefs.backend = null
    }

    function test_deckFires() {
        const a = acts()
        const d = deck(a)
        const trig = spy(d, "triggered")
        const next = d.keyFor("transport.next"), stop = d.keyFor("transport.stop"), arm = d.keyFor("automation.arm")
        mouseClick(next)
        compare(tc.client.calls, [["skip", undefined]], "NEXT fires at once (safety none)"); compare(trig.count, 1); compare(trig.signalArguments[0][0], "transport.next")
        settle()
        mouseClick(stop)
        verify(stop.armed, "STOP arms"); compare(tc.client.calls.length, 1, "no stop yet")
        settle()
        mouseClick(stop)
        verify(!stop.armed); compare(tc.client.calls[1], ["stop", "ch2"], "the second click stops the channel"); compare(trig.count, 2)
        settle()
        // a disabled key never fires
        a.engineUp = false
        verify(!next.enabled); mouseClick(next); compare(tc.client.calls.length, 2)
        verify(!d.trigger("transport.next"), "trigger() refuses a disabled key")
        a.engineUp = true; settle()
        // trigger(): through the key, so STOP arms first
        verify(d.trigger("transport.stop")); verify(stop.armed); compare(tc.client.calls.length, 2)
        settle(); verify(d.trigger("transport.stop")); verify(!stop.armed); compare(tc.client.calls[2], ["stop", "ch2"])
        settle()
        // the arm key toggles the state the owner feeds back
        mouseClick(arm); compare(tc.client.calls[3], ["arm", true, undefined]); a.armed = true; settle()
        compare(arm.text, "ARMED"); compare(arm.safety, "arm")
        mouseClick(arm); verify(arm.armed, "disarming needs a confirmation"); compare(tc.client.calls.length, 4)
        settle(); mouseClick(arm); compare(tc.client.calls[4], ["arm", false, undefined])
        settle()
        // the engine key: hold to stop
        const eng = d.keyFor("engine.toggle")
        mouseClick(eng); compare(tc.rest.calls, [], "a click never fires a hold key")
        mousePress(eng); wait(eng.holdMs + 150); mouseRelease(eng)
        compare(tc.rest.calls, [["post", "/channels/ch2/stop", {}]]); compare(tc.client.calls[5], ["refresh"])
        // right click asks for the editor
        const cust = spy(d, "customize")
        mouseClick(stop, stop.width / 2, stop.height / 2, Qt.RightButton)
        compare(cust.count, 1); compare(cust.signalArguments[0][0], "onair.deck"); compare(cust.signalArguments[0][1], "transport.stop")
        verify(!stop.armed, "a right click does not press the key")
    }

    // --- hotkeys ---------------------------------------------------------------------------------------------------------
    function test_hotkeyHost() {
        const a = acts()
        const d = deck(a)
        const h = createTemporaryObject(hostC, stage(), { actions: a, deck: d, surface: "onair.deck" })
        verify(h !== null)
        compare(h.count, 0, "no custom chords yet")
        ButtonPrefs.setButton("onair.deck", "transport.next", { hotkey: "Ctrl+Shift+N" })
        ButtonPrefs.setButton("onair.deck", "transport.stop", { hotkey: "F9" })
        ButtonPrefs.setButton("onair.deck", "transport.hold", { hotkey: "Shift+H" })
        ButtonPrefs.data.buttons["onair.deck/automation.snap"] = { hotkey: "F1" }       // reserved, as if an old client stored it
        ButtonPrefs.data.buttons["onair.deck/automation.loadhour"] = { hotkey: "Q" }    // a bare letter
        ButtonPrefs.revision++
        tryCompare(h, "count", 3)
        compare(h.bindings.map(b => b.id + "=" + b.seq), ["transport.stop=F9", "transport.next=Ctrl+Shift+N", "transport.hold=Shift+H"], "deck order; reserved and disallowed never bound")
        compare(h.sequenceFor("transport.next"), "Ctrl+Shift+N"); compare(h.sequenceFor("automation.snap"), "")
        const trig = spy(h, "triggered"), refused = spy(h, "refused")
        keySequence("Ctrl+Shift+N")
        compare(tc.client.calls, [["skip", undefined]], "the chord presses NEXT"); compare(trig.count, 1); compare(trig.signalArguments[0][0], "transport.next")
        settle()
        keySequence("F9")
        verify(d.keyFor("transport.stop").armed, "STOP's chord arms the key, it cannot skip the confirmation"); compare(tc.client.calls.length, 1)
        settle(); keySequence("F9")
        compare(tc.client.calls[1], ["stop", "ch2"]); compare(trig.count, 3)
        settle()
        keySequence("F1"); keySequence("Q")
        compare(tc.client.calls.length, 2, "reserved and bare chords do nothing here")
        // typing: Shift+H types an H
        const field = createTemporaryObject(fieldC, stage(), { x: 400, y: 200 })
        field.forceActiveFocus(); tryCompare(field, "activeFocus", true); verify(h.typing)
        keySequence("Shift+H")
        compare(tc.client.calls.length, 2, "no hold while typing"); compare(field.text.toLowerCase(), "h", "the key reached the field")
        verify(!h.press("transport.hold"), "press() itself refuses while typing (a chord a field does not override, F9, would reach it)")
        compare(tc.client.calls.length, 2)
        tc.forceActiveFocus(); tryCompare(h, "typing", false)
        keySequence("Shift+H"); compare(tc.client.calls[2], ["hold", undefined])
        settle()
        // inactive (the page is not shown)
        h.active = false; keySequence("Ctrl+Shift+N"); compare(tc.client.calls.length, 3); h.active = true
        // a disabled key: refused
        a.engineUp = false
        keySequence("Ctrl+Shift+N"); compare(tc.client.calls.length, 3); compare(refused.count, 1); compare(refused.signalArguments[0][1], "key disabled")
        a.engineUp = true
        // a hidden key has no chord
        ButtonPrefs.setHidden("onair.deck", "transport.next", true)
        tryCompare(h, "count", 2); keySequence("Ctrl+Shift+N"); compare(tc.client.calls.length, 3)
        ButtonPrefs.setHidden("onair.deck", "transport.next", false); tryCompare(h, "count", 3)
        // a question open: nothing fires
        ButtonPrefs.setButton("onair.deck", "automation.snap", { safety: "modal", hotkey: "F10" })
        tryCompare(h, "count", 4)
        keySequence("F10"); tryVerify(() => BtnTokens.confirmOpen, 1000, "SNAP asks")
        keySequence("Ctrl+Shift+N"); compare(tc.client.calls.length, 3, "no NEXT while the question is open")
        keyClick(Qt.Key_Escape); tryVerify(() => !BtnTokens.confirmOpen, 1000)
        settle(); keySequence("Ctrl+Shift+N"); compare(tc.client.calls[3], ["skip", undefined])
    }

    function test_hotkeyHostWithoutDeck() {
        const a = acts()
        const h = createTemporaryObject(hostC, stage(), { actions: a, surface: "column.transport" })
        compare(h.ids, a.defaultOrder("column.transport"))
        ButtonPrefs.setButton("column.transport", "automation.snap", { hotkey: "F11" })
        ButtonPrefs.setButton("column.transport", "transport.stop", { hotkey: "F12" })
        tryCompare(h, "count", 2)
        const refused = spy(h, "refused")
        keySequence("F11"); compare(tc.client.calls, [["snap", undefined]], "a safety-none action runs directly")
        keySequence("F12"); compare(tc.client.calls.length, 1, "an arm action has no key to confirm on: refused, never run")
        compare(refused.count, 1); compare(refused.signalArguments[0][1], "needs a key for arm")
        // the same chord on an On Air surface is a conflict with the column (visible everywhere): it is not bound twice
        verify(!ButtonPrefs.setButton("onair.deck", "transport.next", { hotkey: "F11" }).ok)
        // no actions: refused
        const h2 = createTemporaryObject(hostC, stage(), { surface: "column.transport" })
        compare(h2.ids, []); compare(h2.count, 0)
    }
}
