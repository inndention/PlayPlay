import QtQuick
import QtTest
import PlayPlay.Buttons

// UXB-7: ButtonPrefs, the station's button customisations in the `ui.buttons` setting (docs/plans/ux-buttons.md 3.8),
// against a fake backend ({get, put} as the app's api). It checks:
// - hotkey normalisation (modifier order, aliases, one key), the native allow-list, the reserved chords per page, and
//   conflicts (same page, or anywhere when one side is visible on every page);
// - validate(): every field cleaned (icon in the catalogue, #rrggbb, allowed size, label cut at 28, known safety, hotkey
//   allowed / not reserved / not bound twice), unknown surfaces and malformed entries dropped, the problems listed;
// - resolve(): overrides over the owner's defaults, the safety never below the floor (raising is fine), custom fields named;
// - order(): stored order filtered to known ids, new ids appended, hidden removed, separators tidied;
// - editing: setButton / resetButton / setOrder / setHidden / moveKey / resetSurface bump `revision`, set `dirty`, emit changed;
// - load / save through the backend: the Supervisor's string value and an object value, an absent or unreadable value, a
//   failing GET, the whole object PUT to /settings/ui.buttons, a dirty copy never overwritten, revert, touch() throttled to
//   one reload per 5 s, and the periodic reload.
TestCase {
    id: tc
    name: "Prefs"
    when: windowShown
    width: 200; height: 100

    Component {
        id: fakeC
        QtObject {
            property var calls: []
            property var stored: null          // what GET answers as `value` (an object, or the JSON string the Supervisor keeps)
            property bool fail: false
            property bool hold: false          // keep the answers for release()
            property var pending: []
            function get(path, cb) {
                calls.push({ m: "get", path: path })
                const answer = () => cb(!fail, fail ? { error: "boom" } : { key: "ui.buttons", value: stored })
                if (hold) pending.push(answer); else answer()
            }
            function put(path, body, cb) {
                calls.push({ m: "put", path: path, body: JSON.parse(JSON.stringify(body)) })
                if (!fail) stored = JSON.stringify(body.value)
                const answer = () => cb(!fail, fail ? { error: "boom" } : { key: "ui.buttons", ok: true })
                if (hold) pending.push(answer); else answer()
            }
            function release() { const p = pending.slice(); pending = []; for (const f of p) f() }
            function count(m) { return calls.filter(c => c.m === m).length }
        }
    }
    Component { id: spyC; SignalSpy { } }
    function spy(target, sig) { return createTemporaryObject(spyC, tc, { target: target, signalName: sig }) }
    function fake(props) { return createTemporaryObject(fakeC, tc, props || {}) }
    function has(list, x) { return list.indexOf(x) >= 0 }

    // the singleton is shared by every test file in the runner: start each test from nothing
    function init() {
        ButtonPrefs.backend = null
        ButtonPrefs.data = ButtonPrefs.empty()
        ButtonPrefs.dirty = false
        ButtonPrefs.loaded = false
        ButtonPrefs.loading = false
        ButtonPrefs.errors = []
        ButtonPrefs.lastError = ""
        ButtonPrefs.refreshMs = 30000
        ButtonPrefs.autoRefresh = true
        ButtonPrefs._touched = 0
        ButtonPrefs.revision++
    }
    function cleanup() { init() }

    function test_normalizeHotkey_data() {
        return [
            { tag: "plain", seq: "ctrl+shift+s", want: "Ctrl+Shift+S" },
            { tag: "order", seq: "Shift+Ctrl+a", want: "Ctrl+Shift+A" },
            { tag: "alt digit", seq: "alt+3", want: "Alt+3" },
            { tag: "alias esc", seq: "Esc", want: "Escape" },
            { tag: "alias control", seq: "control+Return", want: "Ctrl+Return" },
            { tag: "alias cmd", seq: "Cmd+K", want: "Meta+K" },
            { tag: "fkey", seq: "f10", want: "F10" },
            { tag: "fkey 1", seq: "F1", want: "F1" },
            { tag: "plus key", seq: "Ctrl++", want: "Ctrl++" },
            { tag: "spaces", seq: "  Ctrl + Shift + N ", want: "Ctrl+Shift+N" },
            { tag: "numpad", seq: "num+5", want: "Num+5" },
            { tag: "numpad enter", seq: "Num+enter", want: "Num+Enter" },
            { tag: "named", seq: "ctrl+right", want: "Ctrl+Right" },
            { tag: "pgup alias", seq: "Page Up", want: "PgUp" },
            { tag: "empty", seq: "", want: "" },
            { tag: "null", seq: null, want: "" },
            { tag: "no key", seq: "Ctrl+", want: "" },
            { tag: "two keys", seq: "Ctrl+A+B", want: "" },
            { tag: "unknown name", seq: "Ctrl+Banana", want: "" },
            { tag: "only modifier", seq: "Shift", want: "" }
        ]
    }
    function test_normalizeHotkey(d) { compare(ButtonPrefs.normalizeHotkey(d.seq), d.want) }

    function test_allowedReservedConflict() {
        // the native allow-list (buttons.json hotkeyRules.allowedNative)
        verify(ButtonPrefs.hotkeyAllowed("F9")); verify(ButtonPrefs.hotkeyAllowed("f12"))
        verify(ButtonPrefs.hotkeyAllowed("Ctrl+S")); verify(ButtonPrefs.hotkeyAllowed("Shift+Ctrl+9"))
        verify(ButtonPrefs.hotkeyAllowed("Alt+Q")); verify(ButtonPrefs.hotkeyAllowed("Shift+N")); verify(ButtonPrefs.hotkeyAllowed("Num+5"))
        verify(!ButtonPrefs.hotkeyAllowed("A"), "a bare letter would steal typing")
        verify(!ButtonPrefs.hotkeyAllowed("F8"), "F1-F8 are the pages")
        verify(!ButtonPrefs.hotkeyAllowed("Ctrl+Alt+S"), "three modifiers are not offered")
        verify(!ButtonPrefs.hotkeyAllowed("Meta+K"))
        verify(!ButtonPrefs.hotkeyAllowed("Ctrl+Return"))
        verify(!ButtonPrefs.hotkeyAllowed(""))
        // reserved per page: global everywhere, onair on the On Air surfaces, everything for the column
        verify(has(ButtonPrefs.reservedFor("onair"), "Alt+1")); verify(has(ButtonPrefs.reservedFor("onair"), "Ctrl+Return"))
        verify(has(ButtonPrefs.reservedFor("onair"), "Ctrl+L")); verify(!has(ButtonPrefs.reservedFor("remote"), "Alt+1"))
        verify(has(ButtonPrefs.reservedFor(""), "Alt+1")); verify(has(ButtonPrefs.reservedFor(""), "Escape"))
        verify(ButtonPrefs.hotkeyReserved("alt+1", "onair.deck")); verify(ButtonPrefs.hotkeyReserved("Ctrl+Enter", "onair.switch"))
        verify(ButtonPrefs.hotkeyReserved("Esc", "graphics.cg")); verify(ButtonPrefs.hotkeyReserved("Ctrl+L", "remote.pad"))
        verify(!ButtonPrefs.hotkeyReserved("Alt+1", "remote.pad"), "the pad is not on the On Air page")
        verify(ButtonPrefs.hotkeyReserved("Alt+1", "column.transport"), "the column is on every page")
        verify(!ButtonPrefs.hotkeyReserved("Ctrl+Shift+S", "onair.deck"))
        // hotkeyCheck: ok / reason
        let c = ButtonPrefs.hotkeyCheck("ctrl+shift+s", "onair.deck", "transport.stop")
        verify(c.ok); compare(c.seq, "Ctrl+Shift+S"); compare(c.conflict, null)
        c = ButtonPrefs.hotkeyCheck("", "onair.deck", "transport.stop"); verify(c.ok); compare(c.seq, "")
        c = ButtonPrefs.hotkeyCheck("Ctrl+", "onair.deck", "transport.stop"); verify(!c.ok); compare(c.reason, "not a key chord")
        c = ButtonPrefs.hotkeyCheck("Q", "onair.deck", "transport.stop"); verify(!c.ok); verify(c.reason.indexOf("not allowed") === 0)
        c = ButtonPrefs.hotkeyCheck("Alt+2", "onair.deck", "transport.stop"); verify(!c.ok); compare(c.reason, "reserved by the application")
        verify(ButtonPrefs.hotkeyOk("F11", "onair.deck", "transport.stop")); verify(!ButtonPrefs.hotkeyOk("F2", "onair.deck", "transport.stop"))
        // conflicts
        ButtonPrefs.setButton("onair.deck", "transport.stop", { hotkey: "Ctrl+Shift+S" })
        c = ButtonPrefs.hotkeyCheck("ctrl+shift+s", "onair.switch", "bus.cut")
        verify(!c.ok, "the same chord on another On Air surface"); compare(c.conflict.surface, "onair.deck"); compare(c.conflict.id, "transport.stop")
        verify(c.reason.indexOf("taken by onair.deck/transport.stop") === 0)
        verify(ButtonPrefs.hotkeyCheck("Ctrl+Shift+S", "onair.deck", "transport.stop").ok, "its own binding is no conflict")
        verify(ButtonPrefs.hotkeyCheck("Ctrl+Shift+S", "remote.pad", "b1").ok, "another page is free")
        verify(!ButtonPrefs.hotkeyCheck("Ctrl+Shift+S", "column.transport", "transport.stop").ok, "the column clashes with every page")
        ButtonPrefs.setButton("column.transport", "automation.snap", { hotkey: "F9" })
        verify(!ButtonPrefs.hotkeyCheck("F9", "remote.pad", "b2").ok, "and every page clashes with the column")
        compare(ButtonPrefs.conflict("F9", "remote.pad", "b2").id, "automation.snap")
        compare(ButtonPrefs.conflict("F10", "remote.pad", "b2"), null)
    }

    function test_validate() {
        let v = ButtonPrefs.validate("not json {")
        verify(!v.ok); compare(Object.keys(v.clean.buttons).length, 0); verify(v.errors[0].indexOf("not JSON") === 0)
        v = ButtonPrefs.validate([1, 2]); verify(!v.ok); compare(v.errors, ["not an object"])
        v = ButtonPrefs.validate(null); verify(!v.ok)
        v = ButtonPrefs.validate({}); verify(v.ok); compare(v.errors.length, 0); compare(v.clean.version, 1)
        v = ButtonPrefs.validate('{"version":1,"surfaces":{},"buttons":{}}'); verify(v.ok); compare(v.errors.length, 0)
        const longLabel = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
        v = ButtonPrefs.validate({
            version: 7,
            surfaces: {
                "onair.deck": { order: ["transport.stop", 5, "|", "transport.next", "transport.stop"], hidden: ["automation.snap", 3, "|"] },
                "nowhere": { order: [] },
                "onair.switch": "x",
                "column.transport": { order: "bad", hidden: "bad" }
            },
            buttons: {
                "onair.deck/transport.stop": { label: longLabel, icon: "stop", color: "#FF2D55", size: "xl", hotkey: "ctrl+shift+s", safety: "hold", extra: 1 },
                "onair.deck/transport.next": { label: 12, icon: "no-such-icon", color: "red", size: "xs", hotkey: "Alt+2", safety: "later" },
                "onair.deck/transport.hold": { hotkey: "Ctrl+Shift+S" },
                "remote.pad/b1": { hotkey: "Ctrl+Shift+S", size: "xl" },
                "column.transport/automation.snap": { hotkey: "Shift+Q" },
                "onair.switch/bus.cut": { hotkey: "shift+q" },
                "nowhere/x": { label: "X" },
                "bad": { label: "X" },
                "onair.deck/": { label: "X" },
                "onair.deck/automation.arm": "text",
                "onair.deck/automation.toh": { label: "  " }
            }
        })
        verify(v.ok, "a half-valid object still applies")
        const c = v.clean, e = v.errors
        compare(c.version, 1)
        compare(c.surfaces["onair.deck"].order, ["transport.stop", "|", "transport.next"])
        compare(c.surfaces["onair.deck"].hidden, ["automation.snap"])
        verify(c.surfaces["nowhere"] === undefined); verify(c.surfaces["onair.switch"] === undefined); verify(c.surfaces["column.transport"] === undefined)
        const stop = c.buttons["onair.deck/transport.stop"]
        compare(stop.label, longLabel.slice(0, 28)); compare(stop.icon, "stop"); compare(stop.color, "#ff2d55"); compare(stop.size, "xl")
        compare(stop.hotkey, "Ctrl+Shift+S"); compare(stop.safety, "hold"); verify(stop.extra === undefined, "unknown fields are dropped")
        verify(c.buttons["onair.deck/transport.next"] === undefined, "every field of NEXT was invalid")
        verify(c.buttons["onair.deck/transport.hold"] === undefined, "the chord is already STOP's on this page")
        compare(c.buttons["remote.pad/b1"].hotkey, "Ctrl+Shift+S", "the same chord on another page is fine")
        compare(c.buttons["remote.pad/b1"].size, "xl")
        compare(c.buttons["column.transport/automation.snap"].hotkey, "Shift+Q")
        verify(c.buttons["onair.switch/bus.cut"] === undefined, "the column's chord is taken on every page")
        verify(c.buttons["nowhere/x"] === undefined); verify(c.buttons["bad"] === undefined); verify(c.buttons["onair.deck/"] === undefined)
        verify(c.buttons["onair.deck/automation.arm"] === undefined); verify(c.buttons["onair.deck/automation.toh"] === undefined)
        const text = e.join("\n")
        verify(text.indexOf("version 7") >= 0, "version noted")
        verify(text.indexOf("unknown surface \"nowhere\"") >= 0)
        verify(text.indexOf("onair.switch: not an object") >= 0)
        verify(text.indexOf("column.transport: order is not a list") >= 0)
        verify(text.indexOf("transport.stop: label cut to 28") >= 0)
        verify(text.indexOf("transport.next: label is not a string") >= 0)
        verify(text.indexOf("unknown icon \"no-such-icon\"") >= 0)
        verify(text.indexOf("colour \"red\" is not #rrggbb") >= 0)
        verify(text.indexOf("size \"xs\" is not offered on onair.deck") >= 0)
        verify(text.indexOf("hotkey \"Alt+2\" reserved by the application") >= 0)
        verify(text.indexOf("unknown safety \"later\"") >= 0)
        verify(text.indexOf("transport.hold: hotkey \"Ctrl+Shift+S\" taken by onair.deck/transport.stop") >= 0)
        verify(text.indexOf("button key \"bad\" is not surface/id") >= 0)
        verify(text.indexOf("automation.arm: not an object") >= 0)
        // per-surface sizes
        compare(ButtonPrefs.sizesOf("onair.deck"), ["l", "xl"]); compare(ButtonPrefs.sizesOf("onair.bus"), ["l"])
        compare(ButtonPrefs.sizesOf("column.transport"), ["s", "m"]); compare(ButtonPrefs.sizesOf("remote.pad"), ["m", "l", "xl"])
        compare(ButtonPrefs.sizesOf("nowhere"), BtnTokens.sizeOrder)
    }

    function test_resolveAndFloors() {
        const def = { id: "transport.stop", label: "STOP", icon: "stop", role: "stop", tip: "Stop to slate", hotkey: "", safety: "arm", floor: "arm" }
        let r = ButtonPrefs.resolve("onair.deck", "transport.stop", def)
        compare(r.label, "STOP"); compare(r.icon, "stop"); compare(r.role, "stop"); compare(r.color, ""); compare(r.size, "l", "the surface's first size")
        compare(r.hotkey, ""); compare(r.safety, "arm"); compare(r.floor, "arm"); compare(r.custom, [])
        r = ButtonPrefs.resolve("column.transport", "transport.stop", def); compare(r.size, "s")
        r = ButtonPrefs.resolve("onair.deck", "transport.stop", null); compare(r.label, ""); compare(r.safety, "none"); compare(r.floor, "none")
        // overrides
        ButtonPrefs.setButton("onair.deck", "transport.stop", { label: "HALT", icon: "warning", color: "#123456", size: "xl", hotkey: "Ctrl+Shift+S", safety: "modal" })
        r = ButtonPrefs.resolve("onair.deck", "transport.stop", def)
        compare(r.label, "HALT"); compare(r.icon, "warning"); compare(r.color, "#123456"); compare(r.size, "xl"); compare(r.hotkey, "Ctrl+Shift+S")
        compare(r.safety, "modal", "raising above the floor is allowed"); compare(r.tip, "Stop to slate", "untouched fields pass through")
        compare(r.custom, ["label", "icon", "color", "size", "hotkey", "safety"])
        // the floor
        ButtonPrefs.setButton("onair.deck", "transport.stop", { safety: "none" })
        r = ButtonPrefs.resolve("onair.deck", "transport.stop", def)
        compare(r.safety, "arm", "never below the floor"); verify(!has(r.custom, "safety"), "a floored override changes nothing")
        compare(ButtonPrefs.clampSafety("none", "hold"), "hold"); compare(ButtonPrefs.clampSafety("modal", "arm"), "modal")
        compare(ButtonPrefs.clampSafety("bogus", "arm"), "arm"); compare(ButtonPrefs.clampSafety("hold", "bogus"), "hold")
        // a stored value that this surface does not allow is ignored at resolve time too (size l on the bus, a reserved key)
        ButtonPrefs.data = { version: 1, surfaces: {}, buttons: { "onair.bus/1": { size: "xl", hotkey: "Alt+1", icon: "camera" } } }
        ButtonPrefs.revision++
        r = ButtonPrefs.resolve("onair.bus", "1", { label: "CAM 1", size: "l" })
        compare(r.size, "l"); compare(r.hotkey, ""); compare(r.icon, "camera"); compare(r.custom, ["icon"])
        compare(ButtonPrefs.overrides("onair.bus", "1").size, "xl", "the raw override is still there for the editor")
        verify(ButtonPrefs.isCustomised("onair.bus", "1")); verify(!ButtonPrefs.isCustomised("onair.bus", "2"))
    }

    function test_order() {
        const def = ["a", "b", "c", "|", "d", "e"]
        compare(ButtonPrefs.order("onair.deck", def), def, "nothing stored: the default")
        compare(ButtonPrefs.storedOrder("onair.deck"), [])
        ButtonPrefs.setOrder("onair.deck", ["e", "|", "zz", "b", "b"])
        compare(ButtonPrefs.order("onair.deck", def), ["e", "|", "b", "a", "c", "d"], "unknown dropped, duplicates once, new ids appended")
        ButtonPrefs.setHidden("onair.deck", "a", true); ButtonPrefs.setHidden("onair.deck", "e", true)
        compare(ButtonPrefs.hidden("onair.deck"), ["a", "e"])
        compare(ButtonPrefs.order("onair.deck", def), ["b", "c", "d"], "hidden removed and the leading separator tidied")
        ButtonPrefs.setHidden("onair.deck", "a", false)
        compare(ButtonPrefs.order("onair.deck", def), ["b", "a", "c", "d"])
        ButtonPrefs.setOrder("onair.deck", ["|", "a", "|", "|", "b", "|"])
        compare(ButtonPrefs.order("onair.deck", def), ["a", "|", "b", "|", "c", "d"], "no double separator, none at the ends")
        compare(ButtonPrefs.order("onair.deck", []), [], "no defaults: nothing")
        compare(ButtonPrefs.order("onair.deck", undefined), [])
        // moveKey keeps the separators in place and the keys change places
        ButtonPrefs.setOrder("onair.deck", []); ButtonPrefs.setHidden("onair.deck", "e", false)
        verify(ButtonPrefs.moveKey("onair.deck", "c", 1, def))
        compare(ButtonPrefs.order("onair.deck", def), ["a", "b", "d", "|", "c", "e"])
        verify(ButtonPrefs.moveKey("onair.deck", "a", -1, def) === false, "already first")
        verify(ButtonPrefs.moveKey("onair.deck", "nope", 1, def) === false)
        verify(ButtonPrefs.moveKey("onair.deck", "e", 5, def) === false, "already last")
        verify(ButtonPrefs.moveKey("onair.deck", "e", -4, def))
        compare(ButtonPrefs.order("onair.deck", def), ["e", "a", "b", "|", "d", "c"])
    }

    function test_editing() {
        const ch = spy(ButtonPrefs, "changed")
        const r0 = ButtonPrefs.revision
        verify(!ButtonPrefs.dirty)
        let res = ButtonPrefs.setButton("onair.deck", "transport.stop", { label: "HALT", icon: "warning" })
        verify(res.ok); compare(res.errors, [])
        compare(ButtonPrefs.revision, r0 + 1); verify(ButtonPrefs.dirty); compare(ch.count, 1)
        compare(ButtonPrefs.overrides("onair.deck", "transport.stop"), { label: "HALT", icon: "warning" })
        res = ButtonPrefs.setButton("onair.deck", "transport.stop", { color: "#0000ff", icon: "bogus", hotkey: "F1", size: "xs" })
        verify(!res.ok, "the invalid fields are refused"); compare(res.errors.length, 3)
        compare(ButtonPrefs.overrides("onair.deck", "transport.stop"), { label: "HALT", icon: "warning", color: "#0000ff" }, "the valid one is merged")
        res = ButtonPrefs.setButton("onair.deck", "transport.stop", { label: "", icon: null })
        verify(res.ok); compare(ButtonPrefs.overrides("onair.deck", "transport.stop"), { color: "#0000ff" }, "\"\" and null clear a field")
        ButtonPrefs.setButton("onair.deck", "transport.stop", { color: "" })
        compare(ButtonPrefs.overrides("onair.deck", "transport.stop"), {}); verify(ButtonPrefs.data.buttons["onair.deck/transport.stop"] === undefined, "an empty entry goes")
        res = ButtonPrefs.setButton("nowhere", "x", { label: "X" }); verify(!res.ok)
        // a hotkey conflict is checked against the other keys, not against the key's own old chord
        ButtonPrefs.setButton("onair.deck", "transport.next", { hotkey: "F9" })
        verify(ButtonPrefs.setButton("onair.deck", "transport.next", { hotkey: "F9", label: "GO" }).ok, "re-setting its own chord")
        verify(!ButtonPrefs.setButton("onair.deck", "transport.hold", { hotkey: "F9" }).ok, "another key cannot take it")
        const r1 = ButtonPrefs.revision
        ButtonPrefs.resetButton("onair.deck", "nothing-here"); compare(ButtonPrefs.revision, r1, "resetting nothing changes nothing")
        ButtonPrefs.resetButton("onair.deck", "transport.next"); compare(ButtonPrefs.revision, r1 + 1)
        compare(ButtonPrefs.overrides("onair.deck", "transport.next"), {})
        ButtonPrefs.setButton("onair.deck", "transport.next", { label: "GO" }); ButtonPrefs.setButton("onair.switch", "bus.cut", { label: "KUT" })
        ButtonPrefs.setOrder("onair.deck", ["transport.next"]); ButtonPrefs.setHidden("onair.deck", "automation.snap", true)
        ButtonPrefs.resetSurface("onair.deck")
        verify(ButtonPrefs.data.surfaces["onair.deck"] === undefined); compare(ButtonPrefs.overrides("onair.deck", "transport.next"), {})
        compare(ButtonPrefs.overrides("onair.switch", "bus.cut"), { label: "KUT" }, "other surfaces stay")
        ButtonPrefs.resetAll(); compare(ButtonPrefs.data, ButtonPrefs.empty()); verify(ButtonPrefs.dirty)
    }

    function test_loadSave() {
        const stored = { version: 1, surfaces: { "onair.deck": { order: ["transport.stop", "transport.playpause"], hidden: [] } },
                         buttons: { "onair.deck/transport.stop": { label: "HALT", hotkey: "Ctrl+Shift+S" }, "onair.deck/transport.next": { icon: "bogus" } } }
        const f = fake({ stored: JSON.stringify(stored) })
        const done = spy(ButtonPrefs, "loadDone"), ch = spy(ButtonPrefs, "changed")
        ButtonPrefs.attach(f)
        compare(f.count("get"), 1, "attach loads at once"); compare(f.calls[0].path, "/settings/ui.buttons")
        verify(ButtonPrefs.loaded); compare(done.count, 1); compare(done.signalArguments[0][0], true)
        compare(ButtonPrefs.storedOrder("onair.deck"), ["transport.stop", "transport.playpause"])
        compare(ButtonPrefs.overrides("onair.deck", "transport.stop"), { label: "HALT", hotkey: "Ctrl+Shift+S" }, "the Supervisor's string value is parsed")
        compare(ButtonPrefs.overrides("onair.deck", "transport.next"), {}, "the bad icon was dropped on load")
        compare(ButtonPrefs.errors.length, 1); verify(ButtonPrefs.errors[0].indexOf("unknown icon") >= 0)
        verify(!ButtonPrefs.dirty); compare(ch.count, 1)
        // an object value (another client may store one)
        f.stored = { version: 1, buttons: { "onair.switch/bus.cut": { label: "KUT" } } }
        ButtonPrefs.load(); compare(ButtonPrefs.overrides("onair.switch", "bus.cut"), { label: "KUT" }); compare(ButtonPrefs.overrides("onair.deck", "transport.stop"), {})
        // absent: empty prefs, fine
        f.stored = null; ButtonPrefs.load(); compare(ButtonPrefs.data, ButtonPrefs.empty()); compare(done.signalArguments[done.count - 1][0], true)
        f.stored = ""; ButtonPrefs.load(); compare(ButtonPrefs.data, ButtonPrefs.empty())
        // unreadable: kept empty, reported, loadDone(false)
        ButtonPrefs.setButton("onair.deck", "transport.stop", { label: "KEEP" }); ButtonPrefs.dirty = false
        f.stored = "{ not json"; ButtonPrefs.load()
        compare(done.signalArguments[done.count - 1][0], false); verify(ButtonPrefs.lastError.indexOf("ui.buttons") === 0)
        compare(ButtonPrefs.overrides("onair.deck", "transport.stop"), { label: "KEEP" }, "an unreadable value changes nothing")
        f.stored = "[1,2]"; ButtonPrefs.load(); compare(done.signalArguments[done.count - 1][0], false)
        compare(ButtonPrefs.overrides("onair.deck", "transport.stop"), { label: "KEEP" })
        // a failing GET
        f.fail = true; ButtonPrefs.lastError = ""; ButtonPrefs.load()
        compare(done.signalArguments[done.count - 1][0], false); verify(ButtonPrefs.lastError.indexOf("could not load") > 0); verify(ButtonPrefs.lastError.indexOf("boom") > 0)
        f.fail = false
        // save: the whole object, as an object, to the setting
        const saved = spy(ButtonPrefs, "saveDone")
        ButtonPrefs.setButton("onair.deck", "transport.stop", { label: "HALT", color: "#ff2d55" }); ButtonPrefs.setOrder("onair.deck", ["transport.stop", "|", "transport.next"])
        verify(ButtonPrefs.dirty)
        let cbOk = null
        ButtonPrefs.save(function(ok) { cbOk = ok })
        compare(f.count("put"), 1); const put = f.calls[f.calls.length - 1]
        compare(put.path, "/settings/ui.buttons")
        compare(typeof put.body.value, "object"); verify(!Array.isArray(put.body.value), "the route stores objects only")
        compare(put.body.value, { version: 1, surfaces: { "onair.deck": { order: ["transport.stop", "|", "transport.next"] } },
                                  buttons: { "onair.deck/transport.stop": { label: "HALT", color: "#ff2d55" } } })
        verify(!ButtonPrefs.dirty); compare(saved.count, 1); compare(saved.signalArguments[0][0], true); compare(cbOk, true)
        // what was saved loads back the same
        ButtonPrefs.data = ButtonPrefs.empty(); ButtonPrefs.revision++
        ButtonPrefs.load(); compare(ButtonPrefs.data, put.body.value)
        // a dirty copy is never overwritten by a reload; revert() drops it
        ButtonPrefs.setButton("onair.deck", "transport.stop", { label: "LOCAL" })
        const gets = f.count("get"); ButtonPrefs.load(); compare(f.count("get"), gets, "no GET while dirty")
        compare(ButtonPrefs.overrides("onair.deck", "transport.stop").label, "LOCAL")
        ButtonPrefs.revert(); compare(f.count("get"), gets + 1); compare(ButtonPrefs.overrides("onair.deck", "transport.stop").label, "HALT"); verify(!ButtonPrefs.dirty)
        // an answer that arrives after a local edit began does not clobber it
        f.hold = true; ButtonPrefs.load(); verify(ButtonPrefs.loading)
        ButtonPrefs.setButton("onair.deck", "transport.stop", { label: "LATER" })
        f.release(); verify(!ButtonPrefs.loading); compare(ButtonPrefs.overrides("onair.deck", "transport.stop").label, "LATER"); f.hold = false
        // a failing PUT keeps dirty and reports
        f.fail = true; ButtonPrefs.save(); verify(ButtonPrefs.dirty); compare(saved.signalArguments[saved.count - 1][0], false); verify(ButtonPrefs.lastError.indexOf("could not save") > 0)
        f.fail = false
        // no backend
        ButtonPrefs.attach(null); compare(ButtonPrefs.backend, null)
        let called = null; ButtonPrefs.load(function(ok) { called = ok }); compare(called, false)
        ButtonPrefs.save(function(ok) { called = ok }); compare(called, false); compare(saved.signalArguments[saved.count - 1][0], false)
    }

    function test_touchAndRefresh() {
        const f = fake({ stored: null })
        ButtonPrefs.autoRefresh = false
        ButtonPrefs.attach(f); compare(f.count("get"), 1)
        verify(ButtonPrefs.touch()); compare(f.count("get"), 2, "the first touch reloads")
        verify(!ButtonPrefs.touch()); compare(f.count("get"), 2, "a second touch within 5 s does not")
        ButtonPrefs._touched = Date.now() - ButtonPrefs.touchMs - 1
        verify(ButtonPrefs.touch()); compare(f.count("get"), 3, "after touchMs it reloads again")
        // the periodic reload, at a test interval
        ButtonPrefs.refreshMs = 60; ButtonPrefs.autoRefresh = true
        const n = f.count("get")
        tryVerify(() => f.count("get") >= n + 2, 2000, "the timer reloads")
        ButtonPrefs.autoRefresh = false
        const m = f.count("get"); wait(200); compare(f.count("get"), m, "off: no more loads")
        ButtonPrefs.refreshMs = 30000
    }
}
