import QtQuick
import QtQuick.Controls
import QtTest
import PlayPlay.Buttons

// UXB-8: the native button editor (ButtonEditor, IconPicker, SwatchPicker, HotkeyField) over ButtonPrefs with a fake
// backend. It checks:
// - HotkeyField: a click starts the capture, the next chord is taken (Ctrl+Shift+S, F9, Num+5), a modifier alone waits,
//   Esc cancels, Backspace clears, a bare letter / a reserved chord is refused with the prefs' reason, the "taken by"
//   conflict message names the other key, and every key press is kept from the window's shortcuts while capturing;
// - IconPicker: the catalogue in group order, the search by name and tag, "none", picked();
// - SwatchPicker: the 16 swatches, none, picked(), a typed hex selects no cell;
// - ButtonEditor: edit() loads the stored overrides as the draft, the preview merges the draft over the defaults, the safety
//   chips below the floor are disabled and the floor named, the size chips are the surface's, Apply writes setButton /
//   setHidden and saves, Cancel drops the draft, Reset key and Reset surface clear the prefs and save.
TestCase {
    id: tc
    name: "Editor"
    when: windowShown
    width: 720; height: 1000

    Component {
        id: fakeC
        QtObject {
            property var calls: []
            property var stored: null
            function get(path, cb) { calls.push({ m: "get", path: path }); cb(true, { key: "ui.buttons", value: stored }) }
            function put(path, body, cb) { calls.push({ m: "put", path: path, body: JSON.parse(JSON.stringify(body)) }); stored = JSON.stringify(body.value); cb(true, { ok: true }) }
            function count(m) { return calls.filter(c => c.m === m).length }
        }
    }
    Component { id: fieldC; HotkeyField { width: 220; height: 30 } }
    Component { id: iconsC; IconPicker { width: 400 } }
    Component { id: swatchC; SwatchPicker { width: 400 } }
    Component { id: editorC; ButtonEditor { } }
    Component { id: spyC; SignalSpy { } }
    Component { id: shortcutC; Shortcut { property int hits: 0; context: Qt.ApplicationShortcut; onActivated: hits++ } }

    function stage() { return tc.Window.contentItem }
    function spy(target, sig) { return createTemporaryObject(spyC, tc, { target: target, signalName: sig }) }
    function fake() { return createTemporaryObject(fakeC, tc) }
    function field(props) {
        const f = createTemporaryObject(fieldC, stage(), Object.assign({ x: 10, y: 10, surface: "onair.deck", keyId: "transport.stop" }, props || {}))
        verify(f !== null, "HotkeyField did not create")
        waitForRendering(f)
        return f
    }
    function editor(props) {
        const e = createTemporaryObject(editorC, stage(), props || {})
        verify(e !== null, "ButtonEditor did not create")
        return e
    }
    function child(root, name) { return findChild(root, name) }
    // Open the editor and let it settle: the popup's enter transition, and the Repeaters whose old delegates are deleted on
    // the next event loop turn (a click meanwhile could land on one of them).
    function openEditor(e, surface, id, defaults) {
        e.edit(surface, id, defaults)
        tryVerify(() => e.opened, 1000)
        wait(80)
    }
    function settle() { wait(60) }
    function stopDefaults() {       // what Actions.get("transport.stop") gives: an arm floor
        return { id: "transport.stop", label: "STOP", icon: "stop", role: "stop", tip: "Stop the channel", hotkey: "", safety: "arm", floor: "arm", size: "l" }
    }
    function nextDefaults() { return { id: "transport.next", label: "NEXT", icon: "skip-next", role: "primary", tip: "Next", hotkey: "", safety: "none", floor: "none" } }

    function init() {
        ButtonPrefs.backend = null; ButtonPrefs.data = ButtonPrefs.empty(); ButtonPrefs.dirty = false; ButtonPrefs.autoRefresh = false
        ButtonPrefs.revision++
        if (BtnTokens.armedItem) BtnTokens.armedItem.disarm()
    }
    function cleanup() { ButtonPrefs.autoRefresh = true; ButtonPrefs.backend = null }

    // --- HotkeyField -----------------------------------------------------------------------------------------------------
    function test_capture() {
        const f = field()
        const got = spy(f, "captured")
        verify(!f.capturing)
        mouseClick(f, 20, 15)
        verify(f.capturing, "a click starts the capture")
        verify(f.activeFocus)
        keyClick(Qt.Key_Control, Qt.ControlModifier)
        verify(f.capturing, "a modifier alone waits for the key")
        keyClick(Qt.Key_S, Qt.ControlModifier | Qt.ShiftModifier)
        compare(f.value, "Ctrl+Shift+S")
        verify(!f.capturing, "one chord ends the capture")
        verify(f.ok); compare(f.message, "")
        compare(got.count, 1); compare(got.signalArguments[0][0], "Ctrl+Shift+S")
        // F9, a keypad key, a Shift+letter
        mouseClick(f, 20, 15); keyClick(Qt.Key_F9); compare(f.value, "F9")
        mouseClick(f, 20, 15); keyClick(Qt.Key_5, Qt.KeypadModifier); compare(f.value, "Num+5")
        mouseClick(f, 20, 15); keyClick(Qt.Key_N, Qt.ShiftModifier); compare(f.value, "Shift+N")
        mouseClick(f, 20, 15); keyClick(Qt.Key_Enter, Qt.KeypadModifier); compare(f.value, "Num+Enter")
        // Esc cancels and keeps the value; Backspace clears
        mouseClick(f, 20, 15); verify(f.capturing); keyClick(Qt.Key_Escape); verify(!f.capturing); compare(f.value, "Num+Enter")
        mouseClick(f, 20, 15); keyClick(Qt.Key_Backspace); compare(f.value, ""); verify(!f.capturing)
        compare(got.signalArguments[got.count - 1][0], "")
        // the keyboard starts a capture too (Space / Enter on the focused field)
        f.forceActiveFocus(); verify(!f.capturing); keyClick(Qt.Key_Space); verify(f.capturing); keyClick(Qt.Key_F11); compare(f.value, "F11")
        // a second click while capturing stops it
        mouseClick(f, 20, 15); verify(f.capturing); mouseClick(f, 20, 15); verify(!f.capturing); compare(f.value, "F11")
        // the clear key
        const clearKey = child(f, "hotkeyClear")
        verify(clearKey.visible)
        mouseClick(clearKey); wait(BtnTokens.timing.minIntervalMs + 20)
        compare(f.value, "")
        verify(!clearKey.visible)
    }

    function test_refused() {
        const f = field()
        const no = spy(f, "refused")
        mouseClick(f, 20, 15); keyClick(Qt.Key_F9); compare(f.value, "F9")
        // a bare letter would steal typing
        mouseClick(f, 20, 15); keyClick(Qt.Key_Q)
        verify(f.message.indexOf("not allowed") === 0, f.message); compare(f.value, "F9", "the old value stays")
        verify(f.ok, "ok describes the kept value (F9 is fine), not the refused attempt")
        verify(!f.capturing, "a refused chord ends the capture too")
        compare(no.count, 1); compare(no.signalArguments[0][0], "Q")
        // reserved on the On Air page (Alt+1 is a TAKE)
        mouseClick(f, 20, 15); keyClick(Qt.Key_1, Qt.AltModifier)
        verify(f.ok); compare(f.message, "reserved by the application"); compare(f.value, "F9")
        // a page key
        mouseClick(f, 20, 15); keyClick(Qt.Key_F2)
        verify(f.ok); verify(f.message.indexOf("not allowed") === 0)
        // a value that became bad meanwhile is reported through ok once anything re-checks it
        ButtonPrefs.setButton("onair.switch", "bus.cut", { hotkey: "F9" })
        mouseClick(f, 20, 15); keyClick(Qt.Key_Q)
        verify(!f.ok, "F9 is now taken, so the kept value is not ok"); compare(f.value, "F9")
        ButtonPrefs.resetButton("onair.switch", "bus.cut")
        verify(f.recheck())
        // free on the remote pad
        f.surface = "remote.pad"; f.keyId = "b1"
        mouseClick(f, 20, 15); keyClick(Qt.Key_1, Qt.AltModifier)
        verify(f.ok); compare(f.value, "Alt+1")
        // a success clears the message
        compare(f.message, "")
    }

    function test_conflict() {
        ButtonPrefs.setButton("onair.deck", "transport.stop", { hotkey: "Ctrl+Shift+S" })
        const f = field({ surface: "onair.switch", keyId: "bus.cut" })
        mouseClick(f, 20, 15); keyClick(Qt.Key_S, Qt.ControlModifier | Qt.ShiftModifier)
        verify(f.ok, "no value yet, so nothing is wrong with the field itself")
        compare(f.message, "taken by onair.deck/transport.stop")
        verify(f.conflict !== null); compare(f.conflict.surface, "onair.deck"); compare(f.conflict.id, "transport.stop")
        compare(f.value, "")
        // the key's own chord is no conflict
        const own = field({ x: 10, y: 60, surface: "onair.deck", keyId: "transport.stop" })
        mouseClick(own, 20, 15); keyClick(Qt.Key_S, Qt.ControlModifier | Qt.ShiftModifier)
        verify(own.ok); compare(own.value, "Ctrl+Shift+S")
        // recheck() after the other binding went away
        ButtonPrefs.resetButton("onair.deck", "transport.stop")
        f.value = "Ctrl+Shift+S"
        verify(f.recheck()); compare(f.message, "")
        ButtonPrefs.setButton("onair.deck", "transport.stop", { hotkey: "Ctrl+Shift+S" })
        verify(!f.recheck()); verify(f.message.indexOf("taken by") === 0)
    }

    function test_captureBeatsShortcuts() {
        // an application shortcut on F9 (as main.qml's keys are) must not fire while the field captures
        const sc = createTemporaryObject(shortcutC, tc, { sequence: "F9" })
        const f = field()
        f.forceActiveFocus()
        keyClick(Qt.Key_F9)
        compare(sc.hits, 1, "F9 reaches the shortcut while the field is idle")
        mouseClick(f, 20, 15); verify(f.capturing)
        keyClick(Qt.Key_F9)
        compare(sc.hits, 1, "the capture takes the key"); compare(f.value, "F9")
        // and losing the focus ends a capture
        mouseClick(f, 20, 15); verify(f.capturing)
        const other = createTemporaryObject(fieldC, stage(), { x: 300, y: 10, surface: "remote.pad", keyId: "b2" })
        other.forceActiveFocus()
        verify(!f.capturing)
    }

    // --- IconPicker ------------------------------------------------------------------------------------------------------
    function test_iconPicker() {
        const p = createTemporaryObject(iconsC, stage(), { x: 10, y: 10 })
        waitForRendering(p)
        const all = Object.keys(BtnTokens.icons)
        compare(p.names.length, all.length, "every icon of the catalogue")
        compare(p.names[0], "play", "the Transport group leads")
        compare(p.count, all.length)
        for (const n of all) verify(p.names.indexOf(n) >= 0, n + " is offered")
        // by name and by tag
        p.query = "stop"; verify(p.matches.indexOf("stop") >= 0); verify(p.matches.length < all.length)
        p.query = "toh"; verify(p.matches.indexOf("alarm") >= 0, "a tag matches (toh -> alarm)")
        p.query = "halt"; verify(p.matches.indexOf("pause") >= 0)
        p.query = "   "; compare(p.count, all.length, "blanks are no query")
        p.query = "nothing-like-this"; compare(p.count, 0)
        p.query = "skip"; settle()
        const got = spy(p, "picked")
        const cell = child(p, "icon:skip-next")
        verify(cell !== null && cell.visible)
        mouseClick(cell)
        compare(p.value, "skip-next"); compare(got.count, 1, JSON.stringify(got.signalArguments)); compare(got.signalArguments[0][0], "skip-next")
        verify(cell.selected)
        // none (shown without a query)
        p.query = ""; settle()
        const none = child(p, "icon:none")
        verify(none.visible)
        mouseClick(none)
        compare(p.value, ""); compare(got.signalArguments[1][0], ""); verify(none.selected)
        // the grid scrolls inside the picker: nothing is drawn past its bottom (a cell of row 6 would sit at y 10 + 32 + 5 * 38)
        const flick = child(p, "iconFlick")
        verify(flick.clip); verify(flick.height < p.height); verify(flick.contentHeight > flick.height, "more rows than fit")
        const img = grabImage(stage())
        const below = img.pixel(10 + 17, 10 + p.height + 12)
        const inside = img.pixel(10 + 17 + 38, 10 + 32 + 17)
        verify(Qt.colorEqual(below, stage().Window.window.color) || !Qt.colorEqual(below, inside), "no cell is drawn below the picker: " + below + " vs " + inside)
        // the search clears with Esc
        p.focusSearch(); keyClick(Qt.Key_S); verify(p.query !== "" || child(p, "iconSearchInput").text === "s")
        keyClick(Qt.Key_Escape); compare(p.query, "")
    }

    // --- SwatchPicker ----------------------------------------------------------------------------------------------------
    function test_swatchPicker() {
        const p = createTemporaryObject(swatchC, stage(), { x: 10, y: 10 })
        waitForRendering(p)
        compare(p.count, 16, "16 swatches (buttons.json)")
        const got = spy(p, "picked")
        const red = child(p, "swatch:#ff2d55")
        verify(red !== null, "the On Air red is a swatch")
        mouseClick(red)
        compare(p.value, "#ff2d55"); compare(got.count, 1); verify(red.selected)
        p.value = "#FF9500"
        verify(child(p, "swatch:#ff9500").selected, "a value is matched whatever its case")
        p.value = "#123456"
        verify(!child(p, "swatch:#ff9500").selected); verify(!child(p, "swatch:none").selected, "a typed hex is no cell")
        mouseClick(child(p, "swatch:none"))
        compare(p.value, ""); verify(child(p, "swatch:none").selected)
        p.allowNone = false
        verify(!child(p, "swatch:none").visible)
    }

    // --- ButtonEditor ----------------------------------------------------------------------------------------------------
    function test_editorOpensWithDraft() {
        ButtonPrefs.setButton("onair.deck", "transport.stop", { label: "HALT", color: "#ff9500", hotkey: "F9", size: "xl" })
        const e = editor()
        const cancelled = spy(e, "cancelled")
        openEditor(e, "onair.deck", "transport.stop", stopDefaults())
        compare(e.draft.label, "HALT"); compare(e.draft.hotkey, "F9"); compare(e.draft.size, "xl")
        verify(!e.dirty, "nothing changed yet")
        compare(e.preview.label, "HALT"); compare(e.preview.color, "#ff9500"); compare(e.preview.size, "xl"); compare(e.preview.safety, "arm")
        compare(e.preview.custom, ["label", "color", "size", "hotkey"])
        const pv = child(e, "editorPreview")
        compare(pv.text, "HALT"); compare(pv.size, "xl"); compare(String(pv.customColor), "#ff9500"); compare(pv.hotkey, "F9")
        compare(child(e, "editorLabel").text, "HALT")
        compare(child(e, "editorHex").text, "#ff9500")
        compare(child(e, "editorHotkey").value, "F9")
        verify(child(e, "editorKeyName").text.indexOf("onair.deck / transport.stop") === 0)
        verify(e.deck, "onair.deck is a deck"); verify(child(e, "editorVisible").visible)
        // Esc drops the draft
        e.set("label", "OTHER"); verify(e.dirty)
        keyClick(Qt.Key_Escape)
        tryVerify(() => !e.opened, 1000)
        compare(cancelled.count, 1)
        compare(ButtonPrefs.overrides("onair.deck", "transport.stop").label, "HALT", "nothing written")
        e.edit("onair.deck", "transport.stop", stopDefaults())
        compare(e.draft.label, "HALT", "the next edit starts from the stored prefs again")
        e.cancel()
    }

    function test_safetyFloor() {
        const e = editor()
        openEditor(e, "onair.deck", "transport.stop", stopDefaults())
        compare(e.floor, "arm")
        const chips = child(e, "editorSafetyChips")
        const none = child(chips, "chip:none"), arm = child(chips, "chip:arm"), hold = child(chips, "chip:hold"), modal = child(chips, "chip:modal")
        verify(!none.enabled, "none is below the floor"); verify(arm.enabled); verify(hold.enabled); verify(modal.enabled)
        verify(arm.checked, "the default level is selected")
        verify(e.preview.safety === "arm")
        // raising is fine
        e.set("safety", "modal"); compare(e.preview.safety, "modal"); verify(modal.checked)
        // a stored level below the floor shows as the floor and is not "custom"
        e.set("safety", "none"); compare(e.preview.safety, "arm"); compare(e.preview.custom.indexOf("safety"), -1)
        // a key without a floor offers every level; picking the default clears the override
        e.cancel()
        openEditor(e, "onair.deck", "transport.next", nextDefaults())
        compare(e.floor, "none"); verify(child(child(e, "editorSafetyChips"), "chip:none").enabled)
        mouseClick(child(child(e, "editorSafetyChips"), "chip:hold")); wait(BtnTokens.timing.minIntervalMs + 20)
        compare(e.preview.safety, "hold"); verify(e.has("safety"))
        mouseClick(child(child(e, "editorSafetyChips"), "chip:none")); wait(BtnTokens.timing.minIntervalMs + 20)
        compare(e.preview.safety, "none"); verify(!e.has("safety"), "the default level is no override")
        e.cancel()
    }

    function test_sizesOfSurface() {
        const e = editor()
        openEditor(e, "onair.deck", "transport.stop", stopDefaults())
        compare(e.sizes, ["l", "xl"])
        const chips = child(e, "editorSizeChips")
        compare(chips.count, 2); verify(chips.enabled)
        verify(child(chips, "chip:l").checked)
        mouseClick(child(chips, "chip:xl")); wait(BtnTokens.timing.minIntervalMs + 20)
        compare(e.preview.size, "xl"); compare(child(e, "editorPreview").size, "xl")
        e.cancel()
        // the bus is fixed at l
        openEditor(e, "onair.bus", "3", { label: "CAM 3", role: "neutral" })
        compare(e.sizes, ["l"]); verify(!child(e, "editorSizeChips").enabled)
        verify(!e.deck); verify(!child(e, "editorVisible").visible, "no visibility on a bus key")
        e.cancel()
    }

    function test_applyWritesAndSaves() {
        const api = fake()
        ButtonPrefs.attach(api)
        const e = editor()
        const applied = spy(e, "applied")
        openEditor(e, "onair.deck", "transport.stop", stopDefaults())
        verify(!child(e, "editorApply").enabled, "nothing to apply yet")
        // label through the field
        const label = child(e, "editorLabel")
        label.input.forceActiveFocus(); label.input.selectAll(); keyClick(Qt.Key_Delete); for (const ch of "HALT") keyClick(ch)
        compare(e.draft.label, "HALT"); compare(e.preview.label, "HALT")
        // colour through a swatch, then the hex field, then a role chip
        mouseClick(child(child(e, "editorSwatches"), "swatch:#30d158"))
        compare(e.draft.color, "#30d158"); compare(child(e, "editorHex").text, "#30d158")
        const hex = child(e, "editorHex")
        hex.input.forceActiveFocus(); hex.input.selectAll(); keyClick(Qt.Key_Delete)
        for (const ch of "#ABCDEF") keyClick(ch)
        keyClick(Qt.Key_Return)
        compare(e.draft.color, "#abcdef"); compare(hex.text, "#abcdef")
        hex.input.forceActiveFocus(); hex.input.selectAll(); keyClick(Qt.Key_Delete); for (const ch of "zz") keyClick(ch); keyClick(Qt.Key_Return)
        compare(e.draft.color, "#abcdef", "a bad hex is ignored"); compare(hex.text, "#abcdef", "and the field shows the colour kept")
        mouseClick(child(child(e, "editorRoleChips"), "chip:go")); wait(BtnTokens.timing.minIntervalMs + 20)
        compare(e.draft.color, "#30d158", "a role chip sets the role's colour"); verify(child(child(e, "editorRoleChips"), "chip:go").checked)
        // icon through the picker, hotkey through the field, hidden through the toggle
        const picker = child(e, "editorIconPicker")
        picker.query = "hold"; settle()
        mouseClick(child(picker, "icon:hold"))
        compare(e.draft.icon, "hold"); compare(e.preview.icon, "hold")
        const hk = child(e, "editorHotkey")
        mouseClick(hk, 20, 15); keyClick(Qt.Key_F10)
        compare(e.draft.hotkey, "F10"); compare(child(e, "editorPreview").hotkey, "F10")
        mouseClick(child(e, "editorVisible")); wait(BtnTokens.timing.minIntervalMs + 20)
        verify(!e.keyVisible)
        verify(e.dirty); verify(child(e, "editorApply").enabled)
        const puts = api.count("put")
        mouseClick(child(e, "editorApply"))
        tryVerify(() => !e.opened, 1000)
        compare(applied.count, 1); compare(applied.signalArguments[0][0], "onair.deck"); compare(applied.signalArguments[0][1], "transport.stop")
        const o = ButtonPrefs.overrides("onair.deck", "transport.stop")
        compare(o.label, "HALT"); compare(o.color, "#30d158"); compare(o.icon, "hold"); compare(o.hotkey, "F10")
        verify(o.size === undefined, "the default size is no override"); verify(o.safety === undefined)
        compare(ButtonPrefs.hidden("onair.deck"), ["transport.stop"])
        compare(api.count("put"), puts + 1, "saved once")
        verify(!ButtonPrefs.dirty)
        const saved = JSON.parse(api.stored)
        compare(saved.buttons["onair.deck/transport.stop"].label, "HALT")
        // a conflicting hotkey in the draft blocks Apply
        ButtonPrefs.setButton("onair.switch", "bus.cut", { hotkey: "F11" })
        openEditor(e, "onair.deck", "transport.stop", stopDefaults())
        const hk2 = child(e, "editorHotkey")
        mouseClick(hk2, 20, 15); keyClick(Qt.Key_F11)
        compare(child(e, "editorHotkeyMessage").text, "taken by onair.switch/bus.cut")
        verify(child(e, "editorHotkeyMessage").visible)
        compare(e.draft.hotkey, "F10", "the refused chord never reaches the draft")
        verify(hk2.ok, "the kept chord is still fine"); verify(!e.dirty)
        e.set("label", "HALT2")
        verify(child(e, "editorApply").enabled, "one refused attempt does not block applying a valid draft")
        // clearing the label field clears the override (back to STOP)
        const lbl = child(e, "editorLabel")
        lbl.input.forceActiveFocus(); lbl.input.selectAll(); keyClick(Qt.Key_Delete)
        verify(!e.has("label")); compare(e.preview.label, "STOP")
        mouseClick(child(e, "editorLabelReset")); wait(BtnTokens.timing.minIntervalMs + 20)
        verify(!e.has("label"))
        e.cancel()
    }

    function test_resetKeyAndSurface() {
        const api = fake()
        ButtonPrefs.attach(api)
        ButtonPrefs.setButton("onair.deck", "transport.stop", { label: "HALT", hotkey: "F9" })
        ButtonPrefs.setButton("onair.deck", "transport.next", { label: "GO ON" })
        ButtonPrefs.setHidden("onair.deck", "transport.stop", true)
        ButtonPrefs.setButton("onair.switch", "bus.cut", { label: "HARD" })
        const e = editor()
        openEditor(e, "onair.deck", "transport.stop", stopDefaults())
        verify(child(e, "editorResetKey").enabled)
        compare(e.preview.label, "HALT"); verify(!e.keyVisible)
        let puts = api.count("put")
        mouseClick(child(e, "editorResetKey"))
        tryVerify(() => api.count("put") === puts + 1, 1000, "reset saved")
        compare(ButtonPrefs.overrides("onair.deck", "transport.stop"), {})
        compare(ButtonPrefs.hidden("onair.deck"), [], "a reset key is shown again")
        compare(e.preview.label, "STOP"); compare(e.draft, {}); verify(e.keyVisible)
        compare(child(e, "editorLabel").text, "STOP"); compare(child(e, "editorHotkey").value, "")
        verify(e.opened, "the editor stays open after a reset")
        verify(!child(e, "editorResetKey").enabled)
        compare(ButtonPrefs.overrides("onair.deck", "transport.next").label, "GO ON", "the other key is untouched")
        // reset surface: arm, then confirm
        puts = api.count("put")
        const rs = child(e, "editorResetSurface")
        mouseClick(rs); verify(rs.armed, "reset surface arms first")
        compare(ButtonPrefs.overrides("onair.deck", "transport.next").label, "GO ON", "nothing yet")
        wait(BtnTokens.timing.minIntervalMs + 20)
        mouseClick(rs)
        tryVerify(() => api.count("put") === puts + 1, 1000)
        compare(ButtonPrefs.overrides("onair.deck", "transport.next"), {})
        compare(ButtonPrefs.overrides("onair.switch", "bus.cut").label, "HARD", "another surface is untouched")
        e.cancel()
        // no backend: nothing to save, the edit still applies locally
        ButtonPrefs.backend = null
        openEditor(e, "onair.switch", "bus.cut", { label: "CUT", icon: "cut", role: "stop", safety: "none", floor: "none" })
        e.set("label", "SLAM")
        puts = api.count("put")
        verify(e.apply())
        compare(ButtonPrefs.overrides("onair.switch", "bus.cut").label, "SLAM")
        compare(api.count("put"), puts); verify(ButtonPrefs.dirty, "unsaved without a backend")
    }

    // The pickers set their own value on a click; the editor must still refresh them after a reset, a role chip, the hex
    // field and the next edit() of another key (a reused editor showed the first key's chord / colour / icon otherwise).
    function test_pickersFollowDraft() {
        const api = fake()
        ButtonPrefs.attach(api)
        const e = editor()
        openEditor(e, "onair.deck", "transport.stop", stopDefaults())
        const hk = child(e, "editorHotkey"), sw = child(e, "editorSwatches"), ip = child(e, "editorIconPicker")
        mouseClick(hk, 20, 15); keyClick(Qt.Key_F10); compare(hk.value, "F10")
        mouseClick(child(sw, "swatch:#30d158")); compare(sw.value, "#30d158")
        ip.query = "hold"; settle(); mouseClick(child(ip, "icon:hold")); compare(ip.value, "hold")
        // a role chip and the hex field reach the swatch row
        mouseClick(child(child(e, "editorRoleChips"), "chip:stop")); wait(BtnTokens.timing.minIntervalMs + 20)
        compare(sw.value, e.preview.color); verify(child(sw, "swatch:#30d158").selected === (e.preview.color === "#30d158"))
        const hex = child(e, "editorHex")
        hex.input.forceActiveFocus(); hex.input.selectAll(); keyClick(Qt.Key_Delete); for (const ch of "#ff9500") keyClick(ch); keyClick(Qt.Key_Return)
        compare(sw.value, "#ff9500"); verify(child(sw, "swatch:#ff9500").selected)
        // reset key: every widget back to the default
        e.resetKey()
        compare(e.draft, {})
        compare(hk.value, ""); verify(hk.ok); compare(hk.message, "")
        compare(sw.value, ""); verify(child(sw, "swatch:none").selected)
        compare(ip.value, "stop", "the default icon"); verify(child(e, "editorApply").enabled === false)
        // capture again, cancel, edit another key: nothing of STOP's draft remains in the widgets
        mouseClick(hk, 20, 15); keyClick(Qt.Key_F10); compare(hk.value, "F10")
        mouseClick(child(sw, "swatch:#30d158"))
        e.cancel(); tryVerify(() => !e.opened, 1000)
        openEditor(e, "onair.deck", "transport.next", nextDefaults())
        compare(e.draft, {})
        compare(hk.value, ""); verify(hk.ok); compare(hk.message, "")
        compare(sw.value, ""); compare(ip.value, "skip-next")
        verify(!e.dirty); verify(!child(e, "editorApply").enabled)
        // a stored chord of the new key shows, and a stale one is not re-checked against it
        ButtonPrefs.setButton("onair.deck", "transport.next", { hotkey: "F12", color: "#ff2d55", icon: "alarm" })
        openEditor(e, "onair.deck", "transport.next", nextDefaults())
        compare(hk.value, "F12"); compare(sw.value, "#ff2d55"); compare(ip.value, "alarm")
        // dirty is a field comparison, not the JSON text: dropping and restoring a field is no change
        e.set("color", ""); verify(e.dirty)
        e.set("color", "#ff2d55"); verify(!e.dirty, "the same fields in another order are no change")
        // a load that lands under an untouched editor refreshes the draft; an edit wins over it
        api.stored = JSON.stringify({ buttons: { "onair.deck/transport.next": { hotkey: "F11" } } })
        ButtonPrefs.dirty = false; ButtonPrefs.load()
        compare(hk.value, "F11"); compare(e.draft.hotkey, "F11"); compare(sw.value, "")
        e.set("label", "GO"); verify(e.dirty)
        api.stored = JSON.stringify({ buttons: { "onair.deck/transport.next": { hotkey: "F12" } } })
        ButtonPrefs.dirty = false; ButtonPrefs.load()
        compare(e.draft.label, "GO", "the operator's draft is kept"); compare(hk.value, "F11")
        e.cancel()
    }
}
