import QtQuick
import QtQuick.Controls
import QtQuick.Shapes
import QtTest
import PlayPlay.Buttons

// UXB-3a: the PlayPlay.Buttons module core, imported from qrc:/qml exactly as the app imports it (the runner links
// ui-buttons.qrc and adds only that import path). PButton fires on click and never while disabled or busy, debounces a
// double click, takes no focus on click, sizes itself from the tokens (docs/plans/ux-buttons.md 3.2, pinned here as literals
// too), paints the section 3.2 state layers, and runs the arm / hold safety it owns; IconButton is square and needs a tip;
// Icon draws the generated path data at any size; BtnTokens' theme copy equals Theme.qml.
TestCase {
    id: tc
    name: "PButton"
    when: windowShown
    width: 480; height: 320

    Component { id: btnC; PButton { } }
    Component { id: iconBtnC; IconButton { } }
    Component { id: iconC; Icon { } }
    Component { id: spyC; SignalSpy { } }
    Component { id: plate; Rectangle { width: 96; height: 96; color: "black" } }
    // Stands in for UXB-3b's ConfirmPopup: records the question and keeps the confirm callback for the test to call.
    Component {
        id: fakePopupC
        QtObject {
            property int asked: 0
            property var confirm: null
            function ask(title, text, confirmLabel, role, onConfirm) { asked++; confirm = onConfirm }
        }
    }
    // A window shortcut on Esc next to the armed key, as On Air's "clear preview" (main.qml), yielding while a key is armed.
    Component {
        id: escHostC
        Item {
            id: host
            property int cleared: 0
            property int ambiguous: 0
            width: 400; height: 100
            Shortcut {
                sequence: "Escape"
                context: Qt.ApplicationShortcut
                enabled: !BtnTokens.armedItem
                onActivated: host.cleared++
                onActivatedAmbiguously: host.ambiguous++
            }
        }
    }

    readonly property var designSizes: ({   // docs/plans/ux-buttons.md 3.2
        xs: { h: 20, minW: 20, padX: 6, font: 10, tracking: 0.6, icon: 12, radius: 3 },
        s:  { h: 24, minW: 24, padX: 8, font: 11, tracking: 0.4, icon: 13, radius: 5 },
        m:  { h: 30, minW: 30, padX: 11, font: 12, tracking: 0.4, icon: 15, radius: 5 },
        l:  { h: 46, minW: 64, padX: 8, font: 10, tracking: 0.8, icon: 20, radius: 5 },
        xl: { h: 58, minW: 104, padX: 10, font: 12, tracking: 0.4, icon: 22, radius: 8 } })

    function stage() { return tc.Window.contentItem }
    function make(props) {
        const p = Object.assign({ x: 20, y: 20 }, props || {})
        const o = createTemporaryObject(btnC, stage(), p)
        verify(o !== null, "PButton did not create")
        return o
    }
    function spy(target, sig) { return createTemporaryObject(spyC, tc, { target: target, signalName: sig }) }
    function away() { mouseMove(stage(), stage().width - 2, stage().height - 2) }
    // colours by channel (to 1/255): Qt.colorEqual also compares the colour spec, and lighter() / darker() answer in HSV
    function same(a, b) {
        const x = Qt.tint(a, "transparent"), y = Qt.tint(b, "transparent"), e = 1 / 255 + 1e-6
        return Math.abs(x.r - y.r) <= e && Math.abs(x.g - y.g) <= e && Math.abs(x.b - y.b) <= e && Math.abs(x.a - y.a) <= e
    }
    function hex(n) { return BtnTokens.hex(n) }

    function test_moduleFromQrc() {
        compare(Qt.createComponent("qrc:/qml/PlayPlay/Buttons/PButton.qml").status, Component.Ready, "PButton.qml is not in qrc:/qml")
        compare(Spec.version, 1, "the qmldir's Spec (spec.js) is not reachable")
        verify(BtnTokens.icons.play !== undefined && BtnTokens.icons.play.fill !== "", "BtnTokens has no icons")
        compare(BtnTokens.safetyLevels.join(","), "none,arm,hold,modal")
    }

    function test_tokensMatchDesignAndSpec() {
        compare(BtnTokens.sizeOrder.join(","), "xs,s,m,l,xl")
        for (const s of BtnTokens.sizeOrder) {
            const want = designSizes[s], got = BtnTokens.size(s)
            for (const k of Object.keys(want)) {
                compare(got[k], want[k], s + "." + k + " differs from the design table")
                compare(got[k], Spec.sizes[s][k], s + "." + k + " differs from spec.js")
            }
        }
        compare(BtnTokens.size("nope").h, 30, "an unknown size falls back to m")
        compare(BtnTokens.timing.minIntervalMs, 250)
        compare(BtnTokens.timing.armMs, 3000)
        compare(BtnTokens.timing.holdMs, 800)
        verify(same(BtnTokens.info, "#0a84ff"))
    }

    // BtnTokens keeps its own copy of the Theme colours (spec.js) and font so the module loads alone; they must not drift.
    function test_tokensMatchTheme() {
        const keys = Object.keys(BtnTokens.theme).filter(k => !BtnTokens.theme[k]["new"])
        const t = Qt.createQmlObject('import QtQuick; import "' + ppQmlDir + '"\nQtObject { property var th: Theme }', tc, "theme")
        verify(t !== null && t.th !== null, "Theme.qml did not load from " + ppQmlDir)
        for (const k of keys) verify(same(hex(k), t.th[k]), "BtnTokens " + k + " " + hex(k) + " != Theme." + k + " " + t.th[k])
        compare(BtnTokens.fontDisplay, t.th.fontDisplay)
        verify(keys.length >= 20, "too few theme keys compared: " + keys.length)
        t.destroy()
    }

    function test_firedOnClick() {
        const b = make({ text: "TAKE" })
        const fired = spy(b, "fired"), clicked = spy(b, "clicked")
        mouseClick(b)
        compare(fired.count, 1)
        compare(clicked.count, 1)
        b.clicked()                          // a programmatic click is still debounced like any other
        compare(fired.count, 1)
    }

    function test_keyboardFires() {
        const b = make({ text: "TAKE" })
        const fired = spy(b, "fired")
        b.forceActiveFocus(Qt.TabFocusReason)
        verify(b.activeFocus)
        keyClick(Qt.Key_Space)
        compare(fired.count, 1)
    }

    function test_noFireWhileDisabled() {
        const b = make({ text: "TAKE", enabled: false })
        const fired = spy(b, "fired")
        mouseClick(b)
        b.clicked()
        compare(fired.count, 0)
    }

    function test_noFireWhileBusy() {
        const b = make({ text: "TAKE", busy: true, "icon.name": "play" })
        const fired = spy(b, "fired")
        mouseClick(b)
        compare(fired.count, 0, "a busy key ignores presses")
        verify(findChild(b, "pbBusy") !== null, "busy shows the dots in place of the icon")
        compare(findChild(b, "pbIcon"), null)
        compare(findChild(b, "pbLabel").opacity, BtnTokens.states.busyLabelA)
        b.busy = false
        compare(findChild(b, "pbLabel").opacity, 1)
        tryVerify(() => findChild(b, "pbIcon") !== null && findChild(b, "pbBusy") === null, 1000)
        mouseClick(b)
        compare(fired.count, 1)
    }

    function test_debounce() {
        const b = make({ text: "TAKE" })
        const fired = spy(b, "fired")
        mouseClick(b); mouseClick(b); mouseClick(b)
        compare(fired.count, 1, "clicks inside minIntervalMs are one take")
        wait(BtnTokens.timing.minIntervalMs + 60)
        mouseClick(b)
        compare(fired.count, 2)

        const d = make({ text: "AUTO", x: 160 })
        const dFired = spy(d, "fired")
        mouseDoubleClickSequence(d)
        compare(dFired.count, 1, "a double click is one take")

        const z = make({ text: "FAST", x: 300, minIntervalMs: 0 })
        const zFired = spy(z, "fired")
        mouseClick(z); mouseClick(z)
        compare(zFired.count, 2, "minIntervalMs 0 turns the debounce off")
    }

    // The wall clock can step backwards (w32time / NTP, a manual time set): a stamp left in the future must not keep a key dead
    // until the clock catches up. A negative interval counts as elapsed; the debounce then restarts from the new stamp.
    function test_clockStepBack() {
        const b = make({ text: "TAKE" })
        const fired = spy(b, "fired")
        mouseClick(b)
        compare(fired.count, 1)
        b._lastFire = Date.now() + 60000          // as if the clock stepped back 60 s right after that take
        mouseClick(b)
        compare(fired.count, 2, "a backward clock step leaves the key live")
        mouseClick(b)
        compare(fired.count, 2, "the debounce works again from the new stamp")

        const a = make({ text: "STOP", safety: "arm", x: 200 })
        const aFired = spy(a, "fired")
        mouseClick(a)
        verify(a.armed)
        a._armedAt = Date.now() + 60000           // the step while armed: the next click still confirms
        mouseClick(a)
        compare(aFired.count, 1, "an armed key still confirms after a backward clock step")
        verify(!a.armed)
    }

    function test_noFocusOnClick() {
        const b = make({ text: "TAKE" })
        compare(b.focusPolicy, Qt.TabFocus)
        mouseClick(b)
        verify(!b.activeFocus, "a click must not move the focus to an on-air key")
        compare(findChild(b, "pbFocusRing"), null)
        b.forceActiveFocus(Qt.TabFocusReason)
        verify(b.visualFocus)
        const ring = findChild(b, "pbFocusRing")
        verify(ring !== null && ring.visible, "keyboard focus shows the ring")
        const out = BtnTokens.states.focusOffset + BtnTokens.states.focusW
        compare(ring.width, b.width + 2 * out)
        verify(same(ring.border.color, hex("accent0")))
        b.focus = false
        verify(!b.activeFocus)
        b.forceActiveFocus(Qt.MouseFocusReason)
        verify(b.activeFocus && !b.visualFocus)
        tryVerify(() => findChild(b, "pbFocusRing") === null, 1000, "no ring for mouse focus")
    }

    function test_sizes() {
        for (const s of BtnTokens.sizeOrder) {
            const t = designSizes[s]
            const b = make({ text: "TAKE", size: s, "icon.name": "play" })
            const lab = findChild(b, "pbLabel"), slot = findChild(b, "pbIconSlot")
            compare(b.implicitHeight, t.h, s + " height")
            compare(b.background.radius, t.radius, s + " radius")
            compare(lab.font.pixelSize, t.font, s + " label size")
            fuzzyCompare(lab.font.letterSpacing, t.tracking, 1 / 32, s + " tracking")      // QFont keeps 1/64 px
            verify(lab.font.bold, s + " label is bold")
            compare(lab.font.family, BtnTokens.fontDisplay)
            compare(slot.width, t.icon, s + " icon size")
            compare(findChild(b, "pbIcon").width, t.icon)
            const want = b.stacked ? Math.max(t.minW, 2 * t.padX + Math.max(t.icon, lab.implicitWidth))
                                   : Math.max(t.minW, 2 * t.padX + t.icon + b.tokens.gap + lab.implicitWidth)
            fuzzyCompare(b.implicitWidth, want, 0.01, s + " width")
            verify(b.implicitWidth >= t.minW)
            compare(b.stacked, s === "l" || s === "xl")
            const e = make({ size: s, x: 300 })
            compare(e.implicitWidth, t.minW, s + " empty key is minW wide")
            compare(e.implicitHeight, t.h)
            e.destroy(); b.destroy()
        }
        // CButton parity: a text key is the label plus 16 (s) / 22 (m)
        const ts = make({ text: "LOAD HOUR", size: "s" }), tm = make({ text: "LOAD HOUR", size: "m", y: 60 })
        fuzzyCompare(ts.implicitWidth, findChild(ts, "pbLabel").implicitWidth + 16, 0.01)
        fuzzyCompare(tm.implicitWidth, findChild(tm, "pbLabel").implicitWidth + 22, 0.01)
        // a key laid out narrower than its label elides instead of overflowing
        const n = make({ text: "A VERY LONG LABEL FOR A KEY", size: "s", y: 100, width: 60 })
        verify(findChild(n, "pbLabel").width <= 60 - 16 + 0.01)
    }

    function test_iconButton() {
        for (const s of BtnTokens.sizeOrder) {
            const ib = createTemporaryObject(iconBtnC, stage(), { x: 20, y: 20, size: s, "icon.name": "close", tip: "Remove" })
            compare(ib.implicitWidth, designSizes[s].h, s + " IconButton is square")
            compare(ib.implicitHeight, designSizes[s].h)
            compare(ib.Accessible.name, "Remove", "the tip is the accessible name")
            ib.destroy()
        }
        ignoreWarning(/IconButton "trash" needs a tip/)
        const bare = createTemporaryObject(iconBtnC, stage(), { "icon.name": "trash" })
        verify(bare !== null)
    }

    function test_tipAndAccessibility() {
        const b = make({ text: "CUT", tip: "Hard cut", hotkey: "Ctrl+Enter" })
        compare(b.tipText, "Hard cut (Ctrl+Enter)")
        compare(b.Accessible.name, "CUT")
        compare(b.Accessible.description, "Hard cut")
        compare(b.Accessible.role, Accessible.Button)
        b.tip = ""
        compare(b.tipText, "CUT (Ctrl+Enter)")
        b.hotkey = ""
        compare(b.tipText, "")
        compare(b.label, "CUT", "label aliases text")
        b.label = "TAKE"
        compare(b.text, "TAKE")
        const h = make({ text: "STOP", safety: "hold", x: 200 })
        compare(h.tipText, "Press and hold")
    }

    function test_restLooks() {
        away()
        const n = make({ text: "N", "icon.name": "play" })          // auto neutral: outline on bg3
        verify(same(n.background.color, hex("bg3")))
        verify(same(n.background.border.color, hex("border1")))
        compare(n.background.border.width, 1)
        verify(same(findChild(n, "pbLabel").color, hex("text0")))
        verify(same(findChild(n, "pbIcon").color, hex("text1")))
        compare(n.effectiveFill, "outline")

        const p = make({ text: "P", role: "primary", "icon.name": "play", y: 60 })   // auto coloured: solid
        compare(p.effectiveFill, "solid")
        verify(same(p.background.color, hex("accent0")))
        compare(p.background.border.width, 0)
        verify(same(findChild(p, "pbLabel").color, hex("bg0")))
        verify(same(findChild(p, "pbIcon").color, hex("bg0")))

        const roles = { primary: "accent0", go: "ready", stop: "onAir", destructive: "danger", warn: "standby", accent: "accent2", cued: "cued" }
        for (const r of Object.keys(roles)) {
            const k = make({ text: r, role: r, x: 200 })
            verify(same(k.background.color, hex(roles[r])), r + " paints " + roles[r])
            k.destroy()
        }
        const inf = make({ text: "I", role: "info", x: 200 })
        verify(same(inf.background.color, "#0a84ff"))

        const g = make({ text: "G", fill: "ghost", "icon.name": "close", y: 100 })
        verify(same(g.background.color, "transparent"))
        compare(g.background.border.width, 0)
        verify(same(findChild(g, "pbLabel").color, hex("text1")))
        const gs = make({ text: "GS", fill: "ghost", role: "stop", "icon.name": "close", y: 100, x: 200 })
        verify(same(findChild(gs, "pbIcon").color, hex("onAir")), "a coloured ghost key keeps its colour in the icon")

        const o = make({ text: "O", fill: "outline", y: 140 })      // explicit outline: no bg3
        verify(same(o.background.color, "transparent"))
        compare(o.background.border.width, 1)

        const t = make({ text: "T", fill: "tonal", role: "go", y: 180 })
        verify(same(t.background.color, BtnTokens.alpha(hex("ready"), 0.18)))
        verify(same(t.background.border.color, hex("ready")))
        verify(same(findChild(t, "pbLabel").color, hex("ready")))

        const a = make({ text: "A", active: true, role: "warn", y: 220 })   // lit: tonal at litA with a 2 px border
        compare(a.effectiveFill, "tonal")
        verify(same(a.background.color, BtnTokens.alpha(hex("standby"), 0.30)))
        compare(a.background.border.width, 2)
        verify(same(findChild(a, "pbLabel").color, hex("standby")))

        const c = make({ text: "C", role: "primary", customColor: "#5e5ce6", y: 260 })
        verify(same(c.background.color, "#5e5ce6"), "customColor overrides the role colour")
        const ic = make({ text: "IC", "icon.name": "play", "icon.color": "#ff9500", y: 260, x: 200 })
        verify(same(findChild(ic, "pbIcon").color, "#ff9500"), "icon.color overrides the icon colour")
    }

    function test_disabledLook() {
        away()
        const s = make({ text: "S", role: "go", "icon.name": "play", enabled: false })
        verify(same(s.background.color, hex("bg3")))
        verify(same(s.background.border.color, hex("border0")))
        verify(same(findChild(s, "pbLabel").color, hex("text3")))
        verify(same(findChild(s, "pbIcon").color, hex("text3")))
        compare(s.opacity, 1, "disabled is never opacity")
        const l = make({ text: "L", size: "l", enabled: false, y: 60 })
        verify(same(l.background.color, hex("bg2")), "large keys go to bg2 when disabled")
    }

    // Hover and press fade a state layer in over the resting colour (animMs); the resting colour itself never animates.
    function test_hoverAndPress() {
        away()
        const b = make({ text: "GO", role: "go" })
        const tint = hex("ready")
        verify(same(b.background.color, tint))
        compare(findChild(b, "pbStateLayer"), null, "an untouched key has no state layer (creation cost)")
        mouseMove(b, b.width / 2, b.height / 2)
        tryVerify(() => findChild(b, "pbStateLayer") !== null, 1000, "the first hover makes it")
        const layer = findChild(b, "pbStateLayer")
        tryVerify(() => b.hovered && layer.opacity === 1 && same(layer.color, BtnTokens.hover(tint)), 1000, "hover lightens a solid key")
        verify(same(b.background.color, tint), "the resting colour stays put under the state layer")
        mousePress(b)
        tryVerify(() => same(layer.color, BtnTokens.pressed(tint)), 1000, "press darkens a solid key")
        mouseRelease(b)
        const n = make({ text: "N", y: 60 })
        mouseMove(n, n.width / 2, n.height / 2)
        tryVerify(() => findChild(n, "pbStateLayer") !== null, 1000)
        const nl = findChild(n, "pbStateLayer")
        tryVerify(() => nl.opacity === 1 && same(nl.color, hex("bg4")) && same(n.background.border.color, hex("border2")), 1000, "hover on outline")
        mousePress(n)
        tryVerify(() => same(nl.color, hex("bg5")), 1000)
        mouseRelease(n)
        away()
        tryCompare(nl, "opacity", 0, 1000)
        n.active = true        // a lit change flips on its frame, with no animation
        verify(same(n.background.color, BtnTokens.alpha(hex("text1"), BtnTokens.states.litA)))
    }

    function test_ledBadgeSublabel() {
        const l = make({ text: "PGM", size: "l", led: "red", badge: "CG1", sublabel: "MIX 0.5 s" })
        const bar = findChild(l, "pbLed")
        verify(bar !== null)
        compare(bar.height, BtnTokens.leds.barH)
        compare(bar.y, 0)
        compare(bar.width, l.width - 2 * BtnTokens.size("l").radius)
        verify(same(bar.color, hex("onAir")))
        compare(findChild(l, "pbBadge").children[0].text, "CG1")
        const sub = findChild(l, "pbSublabel")
        verify(sub.visible)
        compare(sub.text, "MIX 0.5 s")
        compare(sub.font.pixelSize, BtnTokens.size("l").subFont)

        const s = make({ text: "PVW", size: "s", led: "green", sublabel: "ignored", y: 80 })
        const dot = findChild(s, "pbLed")
        compare(dot.width, BtnTokens.leds.dotSize)
        compare(dot.height, BtnTokens.leds.dotSize)
        verify(same(dot.color, hex("ready")))
        const noSub = findChild(s, "pbSublabel")
        verify(noSub === null || !noSub.visible, "row sizes have no second line")
        s.led = ""
        tryVerify(() => findChild(s, "pbLed") === null, 1000)
        const w0 = s.implicitWidth
        s.badge = "PTZ2"                     // row sizes show the badge inline after the label, not over it
        const rb = findChild(s, "pbRowBadge")
        verify(rb.visible)
        fuzzyCompare(s.implicitWidth, w0 + rb.implicitWidth + BtnTokens.size("s").gap, 0.01)
        const sl = findChild(s, "pbLabel")
        tryVerify(() => rb.mapToItem(sl.parent, 0, 0).x >= sl.x + sl.width, 1000, "the badge sits after the label")      // positioners lay out on polish
        compare(findChild(s, "pbBadgeSlot").item, null)
        const noRowBadge = findChild(l, "pbRowBadge")
        verify(noRowBadge === null || !noRowBadge.visible, "l / xl keep the corner tag")
        compare(findChild(make({ text: "X", y: 120 }), "pbLedSlot").item, null, "no LED item unless led is set")
    }

    function test_plainKeyHasNoSafetyMachinery() {
        const b = make({ text: "PLAIN" })
        compare(findChild(b, "pbArmPulse"), null, "a key with safety none and not busy creates no timers or overlays")
        b.safety = "arm"
        verify(findChild(b, "pbArmPulse") !== null)
        b.safety = "none"
        tryVerify(() => findChild(b, "pbArmPulse") === null, 1000, "the machinery goes away with the safety")
    }

    function test_armSafety() {
        const b = make({ text: "STOP", role: "stop", safety: "arm", armMs: 600 })
        const fired = spy(b, "fired")
        mouseClick(b)
        compare(fired.count, 0, "the first click only arms")
        verify(b.armed)
        verify(findChild(b, "pbArmPulse").visible)
        verify(findChild(b, "pbArmBar").visible)
        compare(b.Accessible.description, "Click again to confirm")
        wait(BtnTokens.timing.minIntervalMs + 60)
        mouseClick(b)
        compare(fired.count, 1, "the second click within armMs fires")
        verify(!b.armed)

        wait(BtnTokens.timing.minIntervalMs + 60)
        mouseClick(b)
        verify(b.armed)
        tryCompare(b, "armed", false, 1500, "the arm times out")
        compare(fired.count, 1)

        const d = make({ text: "STOP", safety: "arm", x: 200 })
        const dFired = spy(d, "fired")
        mouseDoubleClickSequence(d)
        compare(dFired.count, 0, "a double click never confirms")
        verify(d.armed)

        const e = make({ text: "OFF", size: "l", safety: "arm", confirmText: "ENGINE OFF?", y: 80 })
        mouseClick(e)
        verify(e.armed)
        verify(!d.armed, "arming another key disarms the first")
        compare(findChild(e, "pbSublabel").text, "ENGINE OFF?", "a stacked key shows the confirm text as its second line")
        e.busy = true
        verify(!e.armed, "busy disarms")
    }

    function test_holdSafety() {
        const b = make({ text: "MUX STOP", safety: "hold", holdMs: 300 })
        const fired = spy(b, "fired")
        mousePress(b)
        wait(120)
        verify(b.holdProgress > 0 && b.holdProgress < 1)
        verify(findChild(b, "pbHoldFill").visible)
        mouseRelease(b)
        compare(b.holdProgress, 0, "an early release cancels")
        compare(fired.count, 0)
        mouseClick(b)
        compare(fired.count, 0, "a click never fires a hold key")
        wait(BtnTokens.timing.minIntervalMs)
        mousePress(b)
        tryCompare(fired, "count", 1, 1500, "held for holdMs it fires")
        mouseRelease(b)
        compare(fired.count, 1, "the release after a hold fires nothing more")
    }

    // BtnTokens.armedItem names the armed key, so a window's own Esc shortcut can yield to it: then one Esc disarms (two
    // enabled shortcuts on Esc would share it in turn, and the first press would miss the key).
    function test_armedItemAndEsc() {
        const host = createTemporaryObject(escHostC, stage())
        const b = createTemporaryObject(btnC, host, { x: 20, y: 20, text: "STOP", safety: "arm" })
        const c = createTemporaryObject(btnC, host, { x: 160, y: 20, text: "OFF", safety: "arm" })
        compare(BtnTokens.armedItem, null)
        keyClick(Qt.Key_Escape)
        compare(host.cleared, 1, "nothing armed: the window's Esc runs")
        mouseClick(b)
        verify(b.armed)
        compare(BtnTokens.armedItem, b)
        keyClick(Qt.Key_Escape)
        verify(!b.armed, "one Esc disarms the armed key")
        compare(BtnTokens.armedItem, null)
        compare(host.cleared, 1, "the window's Esc yielded")
        compare(host.ambiguous, 0)
        keyClick(Qt.Key_Escape)
        compare(host.cleared, 2, "disarmed: Esc is the window's again")

        wait(BtnTokens.timing.minIntervalMs + 60)
        mouseClick(b)
        mouseClick(c)
        verify(!b.armed && c.armed)
        compare(BtnTokens.armedItem, c, "arming another key moves armedItem")
        c.enabled = false
        compare(BtnTokens.armedItem, null, "disabling the armed key clears it")
        c.enabled = true
        mouseClick(c)
        compare(BtnTokens.armedItem, c)
        c.destroy()
        tryVerify(() => BtnTokens.armedItem === null, 1000, "a destroyed armed key leaves armedItem null")
        keyClick(Qt.Key_Escape)
        compare(host.cleared, 3)

        const t = createTemporaryObject(btnC, host, { x: 300, y: 20, text: "T", safety: "arm", armMs: 300 })
        mouseClick(t)
        compare(BtnTokens.armedItem, t)
        tryCompare(BtnTokens, "armedItem", null, 1500, "the timeout clears it")
    }

    // With a ConfirmPopup the confirmation comes later: the key may be disabled, busy, no longer modal or gone by then.
    function test_modalConfirmRechecks() {
        const b = make({ text: "DELETE", role: "destructive", safety: "modal" })
        const ov = b.Overlay.overlay
        verify(ov !== null)
        const pop = createTemporaryObject(fakePopupC, tc)
        const saved = BtnTokens._popups
        BtnTokens._popups = [{ overlay: ov, popup: pop }]
        try {
            const fired = spy(b, "fired")
            mouseClick(b)
            compare(pop.asked, 1, "a modal key asks the window's ConfirmPopup")
            verify(!b.armed, "with a popup it does not arm")
            const confirm = pop.confirm
            b.enabled = false
            confirm()
            compare(fired.count, 0, "disabled while the popup was open: no fire")
            b.enabled = true
            b.busy = true
            confirm()
            compare(fired.count, 0, "busy: no fire")
            b.busy = false
            b.safety = "none"
            confirm()
            compare(fired.count, 0, "no longer modal: no fire")
            b.safety = "modal"
            confirm()
            compare(fired.count, 1, "the confirmation fires a key that is still ready")

            const g = make({ text: "PURGE", safety: "modal", y: 60 })
            mouseClick(g)
            compare(pop.asked, 2)
            const late = pop.confirm
            g.destroy()
            wait(50)
            late()                                // must neither fire nor throw
        } finally {
            BtnTokens._popups = saved
        }
    }

    // A modal key that cannot have a ConfirmPopup still confirms, by arming: BtnTokens.confirmPopup answers null when the key
    // is in no window (no overlay), as when ConfirmPopup.qml fails to load. tst_safety covers modal with the popup.
    function test_modalWithoutPopupArms() {
        const b = createTemporaryObject(btnC, null, { text: "DELETE", role: "destructive", safety: "modal" })
        verify(b !== null)
        verify(!b.Overlay.overlay, "a key in no window has no overlay")
        compare(BtnTokens.confirmPopup(b.Overlay.overlay), null, "so no ConfirmPopup")
        const fired = spy(b, "fired")
        b._clickedNow()                         // a click: no window to click in
        compare(fired.count, 0, "the first click arms")
        verify(b.armed)
        compare(BtnTokens.armedItem, b)
        compare(BtnTokens.confirming, null, "and asks nothing")
        wait(BtnTokens.timing.minIntervalMs + 60)
        b._clickedNow()
        compare(fired.count, 1, "the second fires")
        verify(!b.armed)
        compare(BtnTokens.armedItem, null)
    }

    function test_errorFlash() {
        const b = make({ text: "TAKE" })
        b.flashError()
        verify(same(b.background.border.color, hex("danger")))
        compare(b.background.border.width, 2)
        tryVerify(() => !same(b.background.border.color, hex("danger")), 2000, "the error flash ends after errorMs")
    }

    function test_icon() {
        const i = createTemporaryObject(iconC, stage(), { name: "play", size: 15, x: 20, y: 20 })
        compare(i.width, 15)
        compare(i.height, 15)
        verify(i.known)
        compare(findChild(i, "iconFill").preferredRendererType, Shape.CurveRenderer, "CurveRenderer by default")
        verify(findChild(i, "iconFill").visible)
        verify(!findChild(i, "iconStroke").visible, "play has no stroke path, so only its fill Shape is shown")
        i.renderer = "geometry"
        compare(findChild(i, "iconFill").preferredRendererType, Shape.GeometryRenderer)
        verify(i.layer.enabled)
        compare(i.layer.samples, 4)
        i.renderer = "curve"
        verify(!i.layer.enabled)

        ignoreWarning(/unknown icon "nope-x"/)
        const u = createTemporaryObject(iconC, stage(), { name: "nope-x" })
        verify(!u.known)
        verify(!findChild(u, "iconFill").visible && !findChild(u, "iconStroke").visible, "an unknown icon draws nothing")

        // drawn at 4x: the stop square is inked at its centre and blank at the corners, in the requested colour
        const bg = createTemporaryObject(plate, stage(), { x: 0, y: 0 })
        createTemporaryObject(iconC, bg, { name: "stop", size: 96, color: "white" })
        waitForRendering(bg)
        const img = grabImage(bg)
        verify(img.red(48, 48) > 200, "the stop icon is filled at its centre")
        verify(img.red(2, 2) < 30 && img.red(93, 93) < 30, "nothing is drawn outside the icon")
    }
}
