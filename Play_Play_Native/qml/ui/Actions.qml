import QtQuick
import PlayPlay.Buttons

/// Actions: the station action registry (docs/plans/ux-buttons.md 3.5). One entry per action id of buttons.json
/// safetyDefaults: label, icon, role, tip, hotkey hint, native safety and floor for the current state, and run(), which
/// calls exactly what today's key calls (OnAirView's deck, the control column, the bus keys, the Remote / Config / Mux /
/// Output / Facebook Live / CG keys). ActionDeck and HotkeyHost build keys and shortcuts from it; UXB-9..12 migrate the views.
///
/// The owner injects what the actions act on, so a test can pass fakes:
///   Actions { client: supervisor; rest: api; inputs: inputMap; notify: window.toast
///             channel: view.ch; engineState: view.engineState; engineUp: view.engineUp; armed: view.armed; onTake: view.onTake
///             pvw: inputMap.pvw }
/// (the right-hand names resolve in the owner's scope, so the properties here are not called supervisor / api / inputMap).
///
/// run(id, arg, cb): arg is what the key's own call site has at hand (a channel id for channel.delete, an output name for
/// output.stop, a media id, a session id, {id, nodes} for node.delete, the slate source for cg.emergency); cb(ok, data)
/// follows a REST answer (a command sent over IPC answers cb(true) at once). run() returns false and does nothing for an
/// unknown or disabled action, as a disabled key cannot be clicked.
QtObject {
    id: acts

    property var client: null          // SupervisorClient: play / pause / stop / skip / hold / snap / arm / loadHour / refresh / cg
    property var rest: null            // SupervisorApi: post / put / del
    property var inputs: null          // InputMap: cut / autoTake
    property var notify: null          // function(message, level), window.toast

    // the state the labels, safety, enabled and lit follow (OnAirView's view.*, ControlColumn's col.*)
    property string channel: ""        // the active channel id (view.ch / col.activeId)
    property string engineState: "OFFLINE"
    property bool engineUp: false
    property bool armed: false
    property bool onTake: false        // a live take is on air (SCHEDULE returns to the schedule)
    property int pvw: -1               // the input on preview (CUT / AUTO act on it)

    signal ran(string id)
    signal failed(string id, string message)

    // Today's key order (design 3.8 default sources): the On Air deck (OnAirView.qml:480-491), the switch row (281-283) and
    // the control column's transport rows (ControlColumn.qml:94-107), "|" a separator.
    readonly property var deckDefault: ["transport.playpause", "transport.stop", "transport.next", "transport.hold", "automation.snap", "|",
                                        "automation.arm", "automation.loadhour", "automation.toh", "|", "engine.toggle"]
    readonly property var switchDefault: ["bus.cut", "bus.auto", "schedule.return"]
    readonly property var columnDefault: ["automation.arm", "automation.snap", "transport.stop", "transport.playpause", "transport.next", "|",
                                          "engine.toggle", "automation.loadhour", "automation.toh"]
    readonly property var ids: Object.keys(BtnTokens.safetyDefaults)

    function defaultOrder(surface) {
        switch (surface) {
        case "onair.deck": return acts.deckDefault.slice()
        case "onair.switch": return acts.switchDefault.slice()
        case "column.transport": return acts.columnDefault.slice()
        default: return []
        }
    }
    function known(id) { return BtnTokens.safetyDefaults[id] !== undefined }

    // A stateful action's state: playpause shows PAUSE while on air, arm shows ARMED while armed, engine.toggle ENGINE ON
    // while the engine is up (the "off" entry of safetyDefaults otherwise).
    function on(id) {
        switch (id) {
        case "transport.playpause": return acts.engineState === "ON_AIR"
        case "automation.arm": return acts.armed
        case "engine.toggle": return acts.engineUp
        default: return true
        }
    }
    // Today's `enabled` of the key; the actions without a native key (output, live, failover ...) are enabled, their call
    // sites gate them.
    function enabled(id) {
        switch (id) {
        case "transport.playpause": case "transport.stop": case "transport.next": case "transport.hold":
        case "automation.snap": case "automation.loadhour": case "automation.toh":
            return acts.engineUp
        case "bus.cut": case "bus.auto": return acts.pvw >= 0 && acts.engineUp
        case "schedule.return": return acts.onTake
        default: return acts.known(id)
        }
    }
    // Today's `active` (lit) of the key.
    function active(id) {
        switch (id) {
        case "transport.playpause": return acts.engineState === "ON_AIR"
        case "transport.hold": return acts.engineState === "HOLD"
        case "automation.arm": return acts.armed
        case "engine.toggle": return acts.engineUp
        case "bus.cut": case "bus.auto": return acts.pvw >= 0
        case "schedule.return": return acts.onTake
        default: return false
        }
    }
    // An action that needs care: its floor is above none, or its role is stop / destructive.
    function critical(id) {
        const s = acts.spec(id)
        return s !== null && (s.floor !== "none" || s.role === "stop" || s.role === "destructive")
    }
    // The defaults of the action in its current state: {id, label, icon, role, tip, hotkey, safety, floor}; null if unknown.
    function spec(id) { return BtnTokens.actionDefaults(id, acts.on(id)) }
    // Everything a key needs: the spec plus on / enabled / active / critical and a bound run().
    function get(id) {
        const s = acts.spec(id)
        if (s === null) return null
        s.on = acts.on(id); s.enabled = acts.enabled(id); s.active = acts.active(id); s.critical = acts.critical(id)
        s.run = function(arg, cb) { return acts.run(id, arg, cb) }
        return s
    }

    function _toast(msg, level) { if (typeof acts.notify === "function") acts.notify(msg, level) }
    function _done(id, cb, ok, d) {
        if (ok) acts.ran(id)
        else acts.failed(id, d && d.error ? String(d.error) : "failed")
        if (typeof cb === "function") cb(ok, d)
    }
    function _post(id, path, body, cb) { acts.rest.post(path, body, function(ok, d) { acts._done(id, cb, ok, d) }) }
    function _put(id, path, body, cb) { acts.rest.put(path, body, function(ok, d) { acts._done(id, cb, ok, d) }) }
    function _del(id, path, cb) { acts.rest.del(path, function(ok, d) { acts._done(id, cb, ok, d) }) }
    function _enc(s) { return encodeURIComponent(String(s === undefined || s === null ? "" : s)) }

    // Run an action: the same calls as today's keys, in the same order and with the same arguments.
    function run(id, arg, cb) {
        if (!acts.known(id) || !acts.enabled(id)) return false
        const c = acts.client, r = acts.rest, ch = acts.channel
        const chOf = x => typeof x === "string" && x !== "" ? x : ch
        const needs = (what, ok) => { if (!ok) { console.warn("Actions: " + id + " needs " + what); return false } return true }
        switch (id) {
        // --- transport (OnAirView.qml:480-485, ControlColumn.qml:94-98) ---
        case "transport.playpause": if (!needs("client", c)) return false; if (acts.engineState === "ON_AIR") c.pause(); else c.play(); break
        case "transport.stop": if (!needs("client", c)) return false; c.stop(ch); break           // supervisor.stop(view.ch)
        case "transport.next": if (!needs("client", c)) return false; c.skip(); break
        case "transport.hold": if (!needs("client", c)) return false; c.hold(); break
        // --- automation (OnAirView.qml:485-489) ---
        case "automation.snap": if (!needs("client", c)) return false; c.snap(); break
        case "automation.arm": if (!needs("client", c)) return false; c.arm(!acts.armed); break
        case "automation.loadhour": if (!needs("client", c)) return false; c.loadHour(); break
        case "automation.toh": if (!needs("client", c)) return false; c.arm(true); c.loadHour(); break
        // --- the engine process (OnAirView.qml:89 toggleEngine, RemoteView.qml:152) ---
        case "engine.toggle":
            if (!needs("rest", r)) return false
            r.post("/channels/" + ch + (acts.engineUp ? "/stop" : "/start"), {}, function(ok, d) { if (c) c.refresh(); acts._done(id, cb, ok, d) })
            return true
        case "engine.restart": if (!needs("rest", r)) return false; acts._post(id, "/channels/" + chOf(arg) + "/restart", {}, cb); return true
        // --- the switcher bus (OnAirView.qml:281-283) ---
        case "bus.cut": if (!needs("inputs", acts.inputs)) return false; acts.inputs.cut(); break
        case "bus.auto": if (!needs("inputs", acts.inputs)) return false; acts.inputs.autoTake(); break
        case "bus.take": if (!needs("inputs", acts.inputs)) return false; acts.inputs.cut(); break      // no native TAKE key today: a cut
        case "schedule.return": if (!needs("client", c)) return false; c.skip(); break
        // --- graphics (CGView.qml:114, ConfigView.qml:156) ---
        case "cg.clearAll": if (!needs("client", c)) return false; c.cg({ action: "clear" }); break
        case "cg.emergency":
            if (!needs("client", c)) return false
            c.cg({ action: "show", role: "emergency", source: arg !== undefined && arg !== null ? arg : { type: "bars", captions: ["PLEASE STAND BY"] }, "in": { type: "cut" } })
            break
        // --- the multiplexer (MuxView.qml:101, web/mux.js:355) ---
        case "mux.stop": if (!needs("rest", r)) return false; acts._post(id, "/mux/stop", {}, cb); return true
        case "mux.restart": if (!needs("rest", r)) return false; acts._post(id, "/mux/restart", {}, cb); return true
        // --- outputs (OutputView.qml:40, 53): arg is the output name ---
        case "output.stop":
            if (!needs("rest", r) || !needs("an output name", typeof arg === "string" && arg !== "")) return false
            acts._post(id, "/channels/" + acts._enc(ch) + "/outputs/" + acts._enc(arg) + "/stop", {}, cb); return true
        case "output.remove":
            if (!needs("rest", r) || !needs("an output name", typeof arg === "string" && arg !== "")) return false
            acts._del(id, "/channels/" + acts._enc(ch) + "/outputs/" + acts._enc(arg), cb); return true
        // --- Facebook Live (FacebookLivePanel.qml:43) ---
        case "live.start": if (!needs("rest", r)) return false; acts._post(id, "/live/facebook/start", {}, cb); return true
        case "live.end": if (!needs("rest", r)) return false; acts._post(id, "/live/facebook/stop", {}, cb); return true
        // --- failover (RemoteView.qml:165-168) ---
        case "failover.promote": if (!needs("rest", r)) return false; acts._post(id, "/failover/promote", { reason: "operator (app)" }, cb); return true
        case "failover.demote": if (!needs("rest", r)) return false; acts._post(id, "/failover/demote", { reason: "operator (app)" }, cb); return true
        // --- deletes (ConfigView.qml:114, MediaEditor.qml:117, RemoteView.qml:322, 245): arg is the id ---
        case "channel.delete":
            if (!needs("rest", r) || !needs("a channel id", typeof arg === "string" && arg !== "")) return false
            r.del("/channels/" + arg, function(ok, d) { if (ok && c) c.refresh(); acts._done(id, cb, ok, d) }); return true
        case "media.remove":
            if (!needs("rest", r) || !needs("a media id", arg !== undefined && arg !== null && arg !== "")) return false
            acts._post(id, "/media/delete", { ids: Array.isArray(arg) ? arg : [arg] }, cb); return true
        case "session.delete":
            if (!needs("rest", r) || !needs("a session id", typeof arg === "string" && arg !== "")) return false
            acts._del(id, "/sessions/" + arg, cb); return true
        case "node.delete": {
            const nid = arg && typeof arg === "object" ? arg.id : arg
            const nodes = arg && typeof arg === "object" && Array.isArray(arg.nodes) ? arg.nodes : null
            if (!needs("rest", r) || !needs("{id, nodes}", typeof nid === "string" && nid !== "" && nodes !== null)) return false
            acts._put(id, "/remote/nodes", { nodes: nodes.filter(function(x) { return x.id !== nid }) }, cb); return true
        }
        default:
            console.warn("Actions: no run() for " + id)
            return false
        }
        acts.ran(id)
        if (typeof cb === "function") cb(true)
        return true
    }
}
