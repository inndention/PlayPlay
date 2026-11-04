import QtQuick
import QtQuick.Controls
import QtTest
import PlayPlay.Buttons

// UXB-4: HoldButton and PtzPad, imported from qrc:/qml as the app does.
// - HoldButton: started() on press and stopped() on release, lit while held; repeated(count) every repeatMs; every start is
//   paired with exactly one stop, also when the key is disabled, made busy, hidden, cancelled, stopped from outside or
//   destroyed while held; a disabled or busy key never starts; Space on a key with Tab focus holds, and a second press while
//   down does not start twice.
// - PtzPad: move(x, y, z) per key (directions round Home, zoom in / out), stopMove() on release, home() on a Home click
//   (debounced); two keys held together hand the camera back to the key still down and stop only after the last; disabling,
//   rebuilding or destroying the pad mid-move stops the camera; repeatMs re-sends the move; busy blocks; both layouts.
TestCase {
    id: tc
    name: "Hold"
    when: windowShown
    width: 640; height: 420

    property int stops: 0

    Component { id: holdC; HoldButton { } }
    Component { id: padC; PtzPad { } }
    Component { id: spyC; SignalSpy { } }

    function stage() { return tc.Window.contentItem }
    function make(props) {
        const o = createTemporaryObject(holdC, stage(), Object.assign({ x: 20, y: 20, text: "NUDGE" }, props || {}))
        verify(o !== null, "HoldButton did not create")
        return o
    }
    function makePad(props) {
        const o = createTemporaryObject(padC, stage(), Object.assign({ x: 20, y: 20 }, props || {}))
        verify(o !== null, "PtzPad did not create")
        return o
    }
    function spy(target, sig) { return createTemporaryObject(spyC, tc, { target: target, signalName: sig }) }
    function settle() { wait(BtnTokens.timing.minIntervalMs + 60) }
    function away() { mouseMove(stage(), stage().width - 2, stage().height - 2) }
    function same(a, b) {
        const x = Qt.tint(a, "transparent"), y = Qt.tint(b, "transparent"), e = 1 / 255 + 1e-6
        return Math.abs(x.r - y.r) <= e && Math.abs(x.g - y.g) <= e && Math.abs(x.b - y.b) <= e && Math.abs(x.a - y.a) <= e
    }
    function key(pad, id) {
        const k = findChild(pad, "ptz-" + id)
        verify(k !== null, "no PTZ key " + id)
        return k
    }
    // signalArguments entries are Arguments objects, not arrays
    function moveAt(s, i) { const a = s.signalArguments[i]; return [a[0], a[1], a[2]].join(",") }
    function lastMove(s) { return moveAt(s, s.count - 1) }

    function init() { tc.stops = 0 }
    function cleanup() { away() }

    function test_pressStartsReleaseStops() {
        const b = make({ "icon.name": "ptz-n", tip: "Up" })
        const st = spy(b, "started"), sp = spy(b, "stopped"), rp = spy(b, "repeated"), fired = spy(b, "fired")
        compare(b.safety, "none", "not the hold safety level")
        mousePress(b)
        compare(st.count, 1)
        verify(b.holding)
        verify(b.lit, "lit while held")
        verify(same(b.background.color, BtnTokens.alpha(BtnTokens.hex("accent0"), BtnTokens.states.litA)), "in the primary colour")
        wait(200)
        compare(rp.count, 0, "no repeats without repeatMs")
        compare(sp.count, 0)
        mouseRelease(b)
        compare(sp.count, 1)
        verify(!b.holding)
        verify(!b.lit)
        compare(st.count, 1)
        compare(fired.count, 1, "the tap still fires (debounced) for an owner that wants it")
    }

    function test_repeat() {
        const b = make({ repeatMs: 80 })
        const rp = spy(b, "repeated"), sp = spy(b, "stopped")
        mousePress(b)
        wait(330)
        verify(rp.count >= 2 && rp.count <= 5, "repeats every 80 ms: " + rp.count)
        compare(b.repeats, rp.count)
        compare(rp.signalArguments[rp.count - 1][0], rp.count, "repeated carries the count")
        mouseRelease(b)
        compare(sp.count, 1)
        const n = rp.count
        wait(250)
        compare(rp.count, n, "no repeats after the release")
        mousePress(b)
        tryVerify(() => rp.count > n, 1000)
        compare(b.repeats, 1, "the count restarts with each hold")
        mouseRelease(b)
    }

    function test_everyStartHasOneStop() {
        const b = make()
        const st = spy(b, "started"), sp = spy(b, "stopped")
        // disabled while held
        mousePress(b)
        b.enabled = false
        compare(sp.count, 1, "disabling stops it")
        mouseRelease(b)
        compare(sp.count, 1, "the late release adds nothing")
        b.enabled = true
        // busy while held
        mousePress(b)
        b.busy = true
        compare(sp.count, 2, "busy stops it")
        mouseRelease(b)
        b.busy = false
        // hidden while held
        mousePress(b)
        b.visible = false
        compare(sp.count, 3, "hiding stops it")
        mouseRelease(b)
        b.visible = true
        // a cancelled press (the grab was taken)
        mousePress(b)
        b.canceled()
        compare(sp.count, 4, "a cancel stops it")
        mouseRelease(b)
        compare(sp.count, 4)
        // stopped from outside
        mousePress(b)
        b.stop()
        compare(sp.count, 5)
        b.stop()
        mouseRelease(b)
        compare(sp.count, 5, "one stop per start")
        compare(st.count, 5)

        // a disabled or busy key never starts
        b.enabled = false
        mousePress(b); mouseRelease(b)
        b.enabled = true
        b.busy = true
        mousePress(b); mouseRelease(b)
        compare(st.count, 5)
        compare(sp.count, 5)
        b.busy = false

        // destroyed while held
        const d = holdC.createObject(stage(), { x: 200, y: 20, text: "GONE" })
        d.stopped.connect(() => tc.stops++)
        mousePress(d)
        verify(d.holding)
        d.destroy()
        wait(50)
        compare(tc.stops, 1, "destroying a held key stops it")
    }

    // Space on a key with Tab focus holds it; a second press while down (a keyboard auto-repeat) does not start it twice.
    function test_keyboardHold() {
        const b = make()
        const st = spy(b, "started"), sp = spy(b, "stopped")
        b.forceActiveFocus(Qt.TabFocusReason)
        keyPress(Qt.Key_Space)
        compare(st.count, 1)
        keyPress(Qt.Key_Space)
        compare(st.count, 1, "no second start while down")
        keyRelease(Qt.Key_Space)
        compare(sp.count, 1)
        b.focus = false
    }

    function test_padLayout() {
        const p = makePad()
        compare(p.keyHeight, 30)
        compare(p.keyWidth, 38, "38 x 30 at m, as PtzPanel today")
        const ids = ["nw", "n", "ne", "w", "e", "sw", "s", "se", "in", "out", "home"]
        const icons = ["ptz-nw", "ptz-n", "ptz-ne", "ptz-w", "ptz-e", "ptz-sw", "ptz-s", "ptz-se", "zoom-in", "zoom-out", "home"]
        for (let i = 0; i < ids.length; ++i) {
            const k = key(p, ids[i])
            compare(k.icon.name, icons[i])
            verify(k.tip !== "", ids[i] + " has a tip")
            compare(k.Accessible.name, k.tip, ids[i] + " is named by its tip")
            compare(k.height, 30)
        }
        compare(key(p, "n").x, 41)
        compare(key(p, "home").x, 41)
        compare(key(p, "home").y, 33)
        compare(key(p, "se").y, 66)
        compare(key(p, "in").x, 3 * 41, "zoom in: a fourth column")
        compare(key(p, "in").y, 0)
        compare(key(p, "out").y, 33)
        compare(p.implicitWidth, 3 * 38 + 2 * 3 + 3 + 38)
        compare(p.implicitHeight, 3 * 30 + 2 * 3)
        compare(p.Accessible.name, "PTZ")

        const r = makePad({ zoomLayout: "row", keyWidth: 46, keyHeight: 38, x: 260 })
        compare(r.implicitWidth, 3 * 46 + 2 * 3)
        compare(r.implicitHeight, 4 * 38 + 3 * 3)
        compare(key(r, "in").y, 3 * 41)
        compare(key(r, "in").x, 0)
        fuzzyCompare(key(r, "in").width, (144 - 3) / 2, 0.01)
        fuzzyCompare(key(r, "out").x, (144 - 3) / 2 + 3, 0.01)
        compare(key(r, "n").width, 46)

        const nz = makePad({ zoom: false, size: "s", y: 200 })
        compare(findChild(nz, "ptz-in"), null)
        compare(nz.implicitWidth, 3 * nz.keyWidth + 6)
        compare(nz.keyHeight, 24)
    }

    function test_padMoveStopHome() {
        const p = makePad()
        const mv = spy(p, "move"), stp = spy(p, "stopMove"), hm = spy(p, "home")
        const want = { nw: "-1,1,0", n: "0,1,0", ne: "1,1,0", w: "-1,0,0", e: "1,0,0", sw: "-1,-1,0", s: "0,-1,0", se: "1,-1,0",
                       "in": "0,0,1", out: "0,0,-1" }
        let n = 0
        for (const id of Object.keys(want)) {
            mousePress(key(p, id))
            compare(mv.count, ++n, id + " sends one move")
            compare(lastMove(mv), want[id], id)
            verify(p.moving)
            compare([p.vector.x, p.vector.y, p.vector.z].join(","), want[id])
            mouseRelease(key(p, id))
            compare(stp.count, n, id + " stops on release")
            verify(!p.moving)
            compare(p.vector, null)
        }
        compare(hm.count, 0)
        mouseClick(key(p, "home"))
        compare(hm.count, 1, "Home on a click")
        compare(mv.count, n, "Home sends no move")
        compare(stp.count, n, "and no stop")
        mouseDoubleClickSequence(key(p, "home"))
        compare(hm.count, 1, "a double click is debounced against the click before it")
        settle()
        mouseDoubleClickSequence(key(p, "home"))
        compare(hm.count, 2, "a double click is one Home")
    }

    function test_padTwoKeysHeld() {
        const p = makePad()
        const mv = spy(p, "move"), stp = spy(p, "stopMove")
        const e = key(p, "e"), n = key(p, "n")
        e.forceActiveFocus(Qt.TabFocusReason)
        keyPress(Qt.Key_Space)                 // right, by keyboard
        compare(lastMove(mv), "1,0,0")
        mousePress(n)                          // up, by mouse: the newest key drives
        compare(mv.count, 2)
        compare(lastMove(mv), "0,1,0")
        mouseRelease(n)
        compare(stp.count, 0, "no stop while a key is still down")
        compare(mv.count, 3)
        compare(lastMove(mv), "1,0,0", "the key still down drives again")
        keyRelease(Qt.Key_Space)
        compare(stp.count, 1, "the last key up stops the camera")
        compare(mv.count, 3)

        // an older key coming up sends nothing; stopAll ends every hold with one stop
        keyPress(Qt.Key_Space)
        mousePress(n)
        const c = mv.count
        keyRelease(Qt.Key_Space)
        compare(mv.count, c, "the older key's release sends nothing")
        compare(stp.count, 1)
        mouseRelease(n)
        compare(stp.count, 2)
        keyPress(Qt.Key_Space)
        mousePress(n)
        const c2 = mv.count
        p.stopAll()
        compare(stp.count, 3, "stopAll: one stop")
        compare(mv.count, c2, "and no move on the way")
        verify(!p.moving)
        mouseRelease(n)
        keyRelease(Qt.Key_Space)
        compare(stp.count, 3)
        e.focus = false
    }

    function test_padStopsWhenDisabledRebuiltOrGone() {
        const p = makePad()
        const mv = spy(p, "move"), stp = spy(p, "stopMove")
        mousePress(key(p, "n"))
        p.enabled = false                      // the camera went away (PtzPanel binds enabled to hasPtz)
        compare(stp.count, 1, "disabling the pad mid-move stops the camera")
        mouseRelease(key(p, "n"))
        compare(stp.count, 1)
        mousePress(key(p, "n"))
        compare(mv.count, 1, "a disabled pad sends nothing")
        mouseRelease(key(p, "n"))
        p.enabled = true

        mousePress(key(p, "in"))
        compare(lastMove(mv), "0,0,1")
        p.zoom = false                         // the keys are rebuilt: the held zoom key goes, and its stop with it
        tryCompare(stp, "count", 2, 1000, "rebuilding the keys mid-move stops the camera")
        verify(!p.moving)
        mouseRelease(stage(), 1, 1)

        const g = padC.createObject(stage(), { x: 300, y: 20 })
        g.stopMove.connect(() => tc.stops++)
        mousePress(key(g, "w"))
        verify(g.moving)
        g.destroy()
        wait(50)
        compare(tc.stops, 1, "destroying the pad mid-move stops the camera")
    }

    function test_padRepeatAndBusy() {
        const p = makePad({ repeatMs: 80 })
        const mv = spy(p, "move"), stp = spy(p, "stopMove"), hm = spy(p, "home")
        mousePress(key(p, "s"))
        wait(330)
        verify(mv.count >= 3, "the move is sent again every 80 ms: " + mv.count)
        for (let i = 0; i < mv.count; ++i) compare(moveAt(mv, i), "0,-1,0")
        mouseRelease(key(p, "s"))
        compare(stp.count, 1)
        const c0 = mv.count
        wait(200)
        compare(mv.count, c0, "no repeats after the release")

        // two keys held: only the driving (newest) key repeats
        const e = key(p, "e")
        e.forceActiveFocus(Qt.TabFocusReason)
        keyPress(Qt.Key_Space)
        mousePress(key(p, "n"))
        const from = mv.count
        wait(330)
        verify(mv.count >= from + 3, "the driving key repeats: " + (mv.count - from))
        for (let i = from; i < mv.count; ++i) compare(moveAt(mv, i), "0,1,0", "move " + i + " is the driving key's")
        mouseRelease(key(p, "n"))
        keyRelease(Qt.Key_Space)
        e.focus = false
        compare(stp.count, 2)
        const c = mv.count

        p.busy = true
        mousePress(key(p, "n"))
        mouseRelease(key(p, "n"))
        mouseClick(key(p, "home"))
        compare(mv.count, c, "busy: no move")
        compare(hm.count, 0, "busy: no home")
        p.busy = false
        mousePress(key(p, "n"))
        p.busy = true
        compare(stp.count, 3, "going busy mid-move stops the camera")
        mouseRelease(key(p, "n"))
    }
}
