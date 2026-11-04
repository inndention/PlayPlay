import QtQuick
import QtQuick.Controls
import QtTest
import PlayPlay.Buttons

// UXB-4: the on-air keys BigActionButton and PadButton, imported from qrc:/qml as the app does.
// - BigActionButton: l by default (icon above the label), xl, and row sizes; an actionId gives the design 3.5 defaults
//   (label, icon, role, tip, hotkey, native safety, the "off" entry of a stateful action) and anything set on the key wins;
//   tally lights the LED and frames the key, and both flip on the frame the tally changes (a grab right after the change);
//   on a solid key the frame sits outside, 1 px clear; the arm countdown (3, 2, 1 around a draining ring) and the hold
//   progress drawn as a dial in the icon's place ("CONFIRM 3" on a key without an icon); a hold key fires only after
//   holdMs and an early release cancels; a double click while busy fires once.
// - PadButton: the pad colour with a contrast ink, sub line, span widths, confirm arms before triggered(), edit mode picks
//   without arming or acting and frames the selected key.
// - BtnTokens' UXB-4 helpers: actionDefaults, luminance / contrast / inkOn, and look()'s ink.
TestCase {
    id: tc
    name: "BigAction"
    when: windowShown
    width: 640; height: 420

    Component { id: bigC; BigActionButton { } }
    Component { id: padC; PadButton { } }
    Component { id: spyC; SignalSpy { } }
    Component { id: hostC; Rectangle { width: 120; height: 70; color: BtnTokens.hex("bg2") } }

    function stage() { return tc.Window.contentItem }
    function make(props) {
        const o = createTemporaryObject(bigC, stage(), Object.assign({ x: 20, y: 20 }, props || {}))
        verify(o !== null, "BigActionButton did not create")
        return o
    }
    function makePad(props) {
        const o = createTemporaryObject(padC, stage(), Object.assign({ x: 20, y: 20 }, props || {}))
        verify(o !== null, "PadButton did not create")
        return o
    }
    function spy(target, sig) { return createTemporaryObject(spyC, tc, { target: target, signalName: sig }) }
    function settle() { wait(BtnTokens.timing.minIntervalMs + 60) }
    function away() { mouseMove(stage(), stage().width - 2, stage().height - 2) }
    function hex(n) { return BtnTokens.hex(n) }
    // colours by channel, within `tol` / 255 (default 1)
    function same(a, b, tol) {
        const x = Qt.tint(a, "transparent"), y = Qt.tint(b, "transparent"), e = (tol === undefined ? 1 : tol) / 255 + 1e-6
        return Math.abs(x.r - y.r) <= e && Math.abs(x.g - y.g) <= e && Math.abs(x.b - y.b) <= e && Math.abs(x.a - y.a) <= e
    }
    function dialOf(b) { return findChild(b, "bigDial") }

    function cleanup() { away() }

    function test_layoutAndSizes() {
        const b = make({ text: "CUT", "icon.name": "cut" })
        compare(b.size, "l", "a BigActionButton is l by default")
        verify(b.stacked)
        compare(b.height, 46)
        verify(b.width >= 64)
        const icon = findChild(b, "pbIconSlot"), label = findChild(b, "pbLabel")
        tryVerify(() => icon.mapToItem(b, 0, 0).y + icon.height <= label.mapToItem(b, 0, 0).y + 1, 1000, "the icon sits above the label")
        compare(b.safety, "none")
        compare(b.countdown, 0)
        verify(!findChild(b, "bigFrame").visible, "no frame without a tally")
        compare(dialOf(b), null, "no dial at rest")
        compare(findChild(b, "pbLedSlot").item, null, "no LED without a tally or led")

        const x = make({ text: "GO LIVE", size: "xl", y: 90 })
        compare(x.height, 58)
        verify(x.width >= 104)
        compare(x.dialSize, 22, "the dial is icon-sized")
        compare(b.dialSize, 20)
    }

    function test_actionDefaults() {
        const stop = make({ actionId: "transport.stop" })
        compare(stop.text, "STOP")
        compare(stop.icon.name, "stop")
        compare(stop.role, "stop")
        compare(stop.safety, "arm", "transport.stop arms natively (design 3.5)")
        compare(stop.tip, "Stop to slate")

        const cut = make({ actionId: "bus.cut", x: 120 })
        compare(cut.safety, "none")
        compare(cut.hotkey, "Ctrl+Return")
        compare(cut.tipText, "Hard cut the preview to program (Ctrl+Return)")
        compare(make({ actionId: "mux.stop", x: 220 }).safety, "hold")
        compare(make({ actionId: "live.end", x: 320 }).safety, "hold")
        compare(make({ actionId: "channel.delete", x: 420 }).safety, "modal")

        // stateful actions: actionOn false takes the "off" entry (and keeps what it does not override)
        const arm = make({ actionId: "automation.arm", y: 90 })
        compare(arm.text, "ARMED")
        compare(arm.icon.name, "bolt")
        compare(arm.safety, "arm", "disarming arms first")
        arm.actionOn = false
        compare(arm.text, "DISARMED")
        compare(arm.icon.name, "bolt-off")
        compare(arm.safety, "none", "arming does not")
        compare(arm.tip, "Arm the schedule")
        compare(arm.role, "warn")
        const eng = make({ actionId: "engine.toggle", x: 120, y: 90 })
        compare(eng.safety, "hold", "turning the engine off is a hold")
        eng.actionOn = false
        compare(eng.text, "ENGINE OFF")
        compare(eng.icon.name, "power", "the off entry keeps the icon it does not override")
        compare(eng.safety, "none")

        // holdToFire forces hold; anything set on the key wins over the action
        compare(make({ actionId: "bus.take", holdToFire: true, x: 220, y: 90 }).safety, "hold")
        const own = make({ actionId: "bus.cut", text: "MY CUT", safety: "modal", role: "warn", x: 320, y: 90 })
        compare(own.text, "MY CUT")
        compare(own.safety, "modal")
        compare(own.role, "warn")
        compare(own.icon.name, "cut")

        // BtnTokens.actionDefaults itself
        const d = BtnTokens.actionDefaults("engine.toggle", false)
        compare(d.label, "ENGINE OFF")
        compare(d.floor, "none")
        compare(BtnTokens.actionDefaults("engine.toggle", true).floor, "hold")
        compare(BtnTokens.actionDefaults("nope", true), null)

        ignoreWarning(/unknown action "uxb4-no-such-action"/)
        const u = make({ actionId: "uxb4-no-such-action", x: 420, y: 90 })
        compare(u.actionSpec, null)
        compare(u.text, "")
        compare(u.safety, "none")
    }

    // A tally sets the LED and a frame in the tally colour, and neither animates: the window grabbed right after the change
    // already shows the new colour (3.2: a tally flips on the frame its state changes).
    function test_tallyFlipsOnTheSameFrame() {
        const b = make({ text: "CUT", "icon.name": "cut", width: 90 })
        waitForRendering(b)
        const left = Qt.point(1, Math.round(b.height / 2)), top = Qt.point(Math.round(b.width / 2), 1)
        let img = grabImage(b)
        verify(!same(img.pixel(left.x, left.y), hex("onAir"), 40), "no red edge before the tally")

        b.tally = "pgm"
        img = grabImage(b)
        verify(same(img.pixel(left.x, left.y), hex("onAir"), 3), "the pgm frame is drawn on the next frame: " + img.pixel(left.x, left.y))
        verify(same(img.pixel(top.x, top.y), hex("onAir"), 3), "the red LED bar is drawn on the next frame")
        compare(b.led, "red")
        const frame = findChild(b, "bigFrame")
        verify(frame.visible)
        compare(frame.border.width, BtnTokens.states.litBorderW)
        verify(same(frame.border.color, hex("onAir")))

        b.tally = "pvw"
        img = grabImage(b)
        verify(same(img.pixel(left.x, left.y), hex("ready"), 3), "pgm -> pvw flips at once: " + img.pixel(left.x, left.y))
        verify(same(img.pixel(top.x, top.y), hex("ready"), 3))
        compare(b.led, "green")

        b.tally = ""
        img = grabImage(b)
        verify(!same(img.pixel(left.x, left.y), hex("ready"), 40), "the frame goes on the next frame")
        verify(!frame.visible)
        compare(b.led, "")

        // an explicit led wins for the LED; the frame still follows the tally
        const a = make({ text: "SCHEDULE", tally: "pgm", led: "amber", x: 140 })
        verify(same(findChild(a, "pbLed").color, hex("standby")))
        verify(same(findChild(a, "bigFrame").border.color, hex("onAir")))
        compare(a.Accessible.description, "On air")
        a.tip = "Back to the schedule"
        compare(a.Accessible.description, "Back to the schedule. On air")

        // on a solid key of the tally's own colour (a red CUT key, pgm) the frame sits outside, 1 px clear, so it still shows
        const host = createTemporaryObject(hostC, stage(), { x: 300, y: 120 })
        const s = createTemporaryObject(bigC, host, { x: 8, y: 8, text: "CUT", "icon.name": "cut", role: "stop", width: 80 })
        compare(s.effectiveFill, "solid")
        waitForRendering(host)
        s.tally = "pgm"
        img = grabImage(host)
        const sy = 8 + Math.round(s.height / 2)
        verify(same(img.pixel(8 - 3, sy), hex("onAir"), 3) && same(img.pixel(8 - 2, sy), hex("onAir"), 3), "a red ring outside the red key: " + img.pixel(8 - 3, sy))
        verify(same(img.pixel(8 - 1, sy), hex("bg2"), 3), "1 px clear of it: " + img.pixel(8 - 1, sy))
        verify(same(img.pixel(8 - 4, sy), hex("bg2"), 3), "and 2 px wide")
        verify(findChild(s, "bigFrame").x < 0)
        s.active = true                         // lit, the key is tonal: the frame lines its edge instead
        compare(findChild(s, "bigFrame").x, 0)

        // the error flash steps over the frame for errorMs
        b.tally = "pvw"
        b.flashError()
        verify(!frame.visible, "the error border is not hidden under the tally frame")
        verify(same(b.background.border.color, hex("danger")))
        tryVerify(() => frame.visible, BtnTokens.states.errorMs + 1000, "the frame comes back after the flash")
    }

    function test_armCountdown() {
        const b = make({ text: "STOP", "icon.name": "stop", role: "stop", safety: "arm", armMs: 2000 })
        const fired = spy(b, "fired")
        compare(b.armLeft, 1)
        mouseClick(b)
        compare(fired.count, 0, "the first click arms")
        verify(b.armed)
        compare(b.countdown, 2, "the countdown starts at the whole window")
        const dial = dialOf(b)
        verify(dial !== null, "armed, the dial is shown")
        tryVerify(() => findChild(b, "pbIcon") === null, 1000, "the dial takes the icon's place")   // the Loader drops the icon later
        compare(dial.width, 20)
        verify(findChild(dial, "bigDialText").visible)
        compare(findChild(dial, "bigDialText").text, "2")
        verify(same(findChild(dial, "bigDialText").color, hex("standby")))
        compare(findChild(b, "pbSublabel").text, BtnTokens.states.armSublabel, "PButton's CONFIRM line stays")
        verify(findChild(b, "pbArmBar").visible, "and its draining bar")
        const s0 = dial.sweep
        verify(s0 > 300, "the ring starts full: " + s0)
        wait(400)
        compare(b.countdown, 2, "still 2 s left during the first second")
        tryCompare(b, "countdown", 1, 1200, "2 -> 1 after a second")
        verify(b.armed)
        verify(dial.sweep < s0 && dial.sweep <= 185, "the ring drains: " + dial.sweep)
        verify(b.armLeft < 0.55 && b.armLeft > 0)
        compare(findChild(dial, "bigDialText").text, "1")
        tryCompare(b, "armed", false, 1600, "the arm times out")
        compare(b.countdown, 0)
        compare(b.armLeft, 1)
        tryVerify(() => dialOf(b) === null, 1000, "the dial goes with the arm")
        verify(findChild(b, "pbIcon") !== null, "and the icon comes back")
        compare(fired.count, 0)

        // confirmed inside the window: one fire, the dial goes; the default window counts 3, 2, 1
        const c = make({ text: "GO LIVE", "icon.name": "broadcast", size: "xl", safety: "arm", y: 90 })
        const cf = spy(c, "fired")
        mouseClick(c)
        compare(c.countdown, 3)
        compare(dialOf(c).width, 22)
        compare(findChild(c, "pbSublabel").text, BtnTokens.states.armSublabel)
        settle()
        mouseClick(c)
        compare(cf.count, 1)
        verify(!c.armed)
        tryVerify(() => dialOf(c) === null, 1000)

        // a double click never confirms; row sizes keep the bar but draw no dial
        const d = make({ text: "SLATE", size: "m", safety: "arm", x: 200 })
        const df = spy(d, "fired")
        mouseDoubleClickSequence(d)
        compare(df.count, 0)
        verify(d.armed)
        compare(d.countdown, 3)
        compare(dialOf(d), null, "no dial at row sizes")
        verify(findChild(d, "pbArmBar").visible)
        d.disarm()

        // without an icon the seconds go in the CONFIRM line (no dial over the label)
        const n = make({ text: "END STREAM", size: "xl", safety: "arm", armMs: 2000, confirmText: "SURE?", x: 300, y: 90 })
        mouseClick(n)
        compare(dialOf(n), null)
        compare(findChild(n, "pbSublabel").text, "SURE? 2")
        tryCompare(findChild(n, "pbSublabel"), "text", "SURE? 1", 1600)
        n.disarm()
        // the line's Loader drops it (the Text is deleted later, so wait for it)
        tryVerify(() => { const g = findChild(n, "pbSublabel"); return g === null || g.text === "" }, 1000, "the line goes with the arm")
    }

    function test_holdFiresOnlyAfterHoldMs() {
        const b = make({ text: "END LIVE", "icon.name": "stop", role: "stop", holdToFire: true, holdMs: 500 })
        const fired = spy(b, "fired")
        mousePress(b)
        wait(200)
        verify(b.holdProgress > 0 && b.holdProgress < 1, "progress " + b.holdProgress)
        const dial = dialOf(b)
        verify(dial !== null, "held, the dial is shown")
        tryVerify(() => findChild(b, "pbIcon") === null, 1000, "in the icon's place")
        verify(!findChild(dial, "bigDialText").visible, "no digits while held")
        verify(same(findChild(dial, "bigDialText").color, hex("text0")))
        verify(dial.sweep > 0 && dial.sweep < 360)
        mouseRelease(b)
        compare(fired.count, 0, "an early release cancels")
        compare(b.holdProgress, 0)
        tryVerify(() => dialOf(b) === null, 1000)
        mouseClick(b)
        compare(fired.count, 0, "a click never fires a hold key")

        settle()
        const t0 = Date.now()
        mousePress(b)
        wait(300)
        compare(fired.count, 0, "not before holdMs")
        tryCompare(fired, "count", 1, 1500, "held for holdMs it fires")
        verify(Date.now() - t0 >= 480, "fired after " + (Date.now() - t0) + " ms")
        mouseRelease(b)
        wait(100)
        compare(fired.count, 1, "the release adds nothing")
    }

    function test_doubleClickWhileBusyFiresOnce() {
        const b = make({ text: "TAKE", "icon.name": "take", role: "stop" })
        let n = 0
        b.fired.connect(() => { n++; b.busy = true })     // the owner goes busy until its call answers
        mouseDoubleClickSequence(b)
        compare(n, 1, "the second click of a double click lands on a busy key")
        verify(findChild(b, "pbBusy") !== null, "busy dots replace the icon")
        mouseClick(b)
        compare(n, 1, "nothing while busy")
        b.busy = false
        settle()
        mouseDoubleClickSequence(b)
        compare(n, 2, "after the answer a double click is one take again")

        const c = make({ text: "CUT", busy: true, x: 120 })
        const cf = spy(c, "fired")
        mouseDoubleClickSequence(c)
        compare(cf.count, 0, "a busy key fires nothing")
    }

    function test_tokensHelpers() {
        fuzzyCompare(BtnTokens.luminance("#ffffff"), 1, 1e-6)
        fuzzyCompare(BtnTokens.luminance("#000000"), 0, 1e-6)
        fuzzyCompare(BtnTokens.contrast("#000000", "#ffffff"), 21, 1e-6)
        verify(same(BtnTokens.inkOn("#33445a"), hex("text0")), "light ink on the studio neutral")
        verify(same(BtnTokens.inkOn("#d63b3b"), hex("text0")), "light ink on red")
        verify(same(BtnTokens.inkOn("#d5dce6"), BtnTokens.onFill), "dark ink on the daylight neutral")
        verify(same(BtnTokens.inkOn("#ff9500"), BtnTokens.onFill), "dark ink on amber")
        const base = { role: "go", fill: "auto", lit: false, tint: hex("ready"), enabled: true, hovered: false, pressed: false, large: false }
        verify(same(BtnTokens.look(base).text, BtnTokens.onFill), "solid keys keep onFill by default")
        verify(same(BtnTokens.look(Object.assign({}, base, { ink: "transparent" })).text, BtnTokens.onFill))
        verify(same(BtnTokens.look(Object.assign({}, base, { ink: "#ffffff" })).text, "#ffffff"))
        verify(same(BtnTokens.look(Object.assign({}, base, { ink: "#ffffff", fill: "outline" })).text, hex("text0")), "the ink is for solid fills only")
    }

    function test_padLookAndSize() {
        const p = makePad({ text: "SKIP", sub: "local · ch1 · F9", fillColor: "#33445a" })
        compare(p.size, "xl")
        compare(p.height, 58)
        compare(p.width, 112, "a span-1 pad key is today's 112 px")
        compare(p.effectiveFill, "solid")
        verify(same(p.background.color, "#33445a"))
        verify(same(findChild(p, "pbLabel").color, hex("text0")), "light ink on a dark pad colour")
        const sub = findChild(p, "pbSublabel")
        verify(sub.visible)
        compare(sub.text, "local · ch1 · F9")
        verify(same(sub.color, hex("text0")))
        compare(p.safety, "none")

        p.fillColor = "#d5dce6"
        verify(same(findChild(p, "pbLabel").color, BtnTokens.onFill), "dark ink on a light pad colour")
        p.textColor = "#ffffff"
        verify(same(findChild(p, "pbLabel").color, "#ffffff"), "textColor overrides the ink")

        const w = makePad({ text: "GO LIVE", span: 2, fillColor: "#1f9d55", y: 90 })
        compare(w.width, 2 * 112 + 8, "span 2 is two units and the gap")
        w.span = 5
        compare(w.width, 232, "spans above 2 clamp")
        w.span = 0
        compare(w.width, 112)
        const m = makePad({ text: "NEXT", size: "m", fillColor: "#2f7fd1", y: 170 })
        compare(m.height, 30)
        compare(m.width, 112)

        // hover / press on the pad colour
        const h = makePad({ text: "HOVER", fillColor: "#2f7fd1", x: 260, y: 90 })
        mouseMove(h, h.width / 2, h.height / 2)         // the state layer is made on the first hover
        tryVerify(() => findChild(h, "pbStateLayer") !== null, 1000)
        const layer = findChild(h, "pbStateLayer")
        tryVerify(() => layer.opacity === 1 && same(layer.color, BtnTokens.hover("#2f7fd1")), 1000, "hover lightens the pad colour")
        mousePress(h)
        tryVerify(() => same(layer.color, BtnTokens.pressed("#2f7fd1")), 1000, "press darkens it")
        mouseRelease(h)
    }

    // confirm arms before acting: the CONFIRM line, the pulse and the countdown replace RemotePad's "CONFIRM?" label swap.
    function test_padConfirmArms() {
        const p = makePad({ text: "GO LIVE", sub: "local", fillColor: "#1f9d55", confirm: true })
        const trig = spy(p, "triggered"), picked = spy(p, "picked")
        compare(p.safety, "arm")
        mouseClick(p)
        compare(trig.count, 0, "the first click arms")
        verify(p.armed)
        compare(findChild(p, "pbLabel").text, "GO LIVE", "the label stays")
        compare(findChild(p, "pbSublabel").text, BtnTokens.states.armSublabel + " 3", "an icon-less pad key counts in its CONFIRM line")
        compare(p.countdown, 3)
        compare(dialOf(p), null)
        verify(findChild(p, "pbArmPulse").visible)
        settle()
        mouseClick(p)
        compare(trig.count, 1, "the second click acts")
        compare(picked.count, 0)
        verify(!p.armed)

        // an amber pad colour takes the dark ink, and the arm marks follow the ink there
        const a = makePad({ text: "HOLD", fillColor: "#ff9500", confirm: true, x: 160 })
        mouseClick(a)
        verify(a.armed)
        verify(same(findChild(a, "pbArmPulse").border.color, BtnTokens.onFill), "arm marks on amber use the ink")
        a.disarm()

        // padSafety: a pad key may hold instead
        const hk = makePad({ text: "END", fillColor: "#d63b3b", padSafety: "hold", holdMs: 300, x: 300 })
        const ht = spy(hk, "triggered")
        mouseClick(hk)
        compare(ht.count, 0)
        mousePress(hk)
        tryCompare(ht, "count", 1, 1500)
        mouseRelease(hk)

        // busy: a double click does nothing
        const b = makePad({ text: "SKIP", fillColor: "#33445a", busy: true, y: 90 })
        const bt = spy(b, "triggered")
        mouseDoubleClickSequence(b)
        compare(bt.count, 0)
    }

    function test_padEditMode() {
        const p = makePad({ text: "SKIP", fillColor: "#33445a", confirm: true, editMode: true })
        const trig = spy(p, "triggered"), picked = spy(p, "picked")
        compare(p.safety, "none", "edit mode never arms")
        mouseClick(p)
        compare(picked.count, 1, "an edit-mode click picks the key")
        compare(trig.count, 0, "and never acts")
        verify(!p.armed)
        const frame = findChild(p, "bigFrame")
        verify(!frame.visible, "not selected yet")
        p.selected = true
        verify(frame.visible)
        verify(same(frame.border.color, hex("accent0")), "the selection frame is the accent")
        p.selectColor = "#4aa3ff"
        verify(same(frame.border.color, "#4aa3ff"), "or the pad theme's accent")
        p.editMode = false
        verify(!frame.visible, "the selection frame is an edit-mode thing")
        compare(p.safety, "arm")

        settle()
        mouseClick(p)
        verify(p.armed)
        p.editMode = true                       // entering edit mode disarms (the safety changes)
        verify(!p.armed)
        settle()
        mouseClick(p)
        compare(picked.count, 2)
        compare(trig.count, 0)
    }
}
