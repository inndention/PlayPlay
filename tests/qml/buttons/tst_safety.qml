import QtQuick
import QtQuick.Controls
import QtTest
import PlayPlay.Buttons

// UXB-3b: the safety model end to end (docs/plans/ux-buttons.md 3.5), imported from qrc:/qml as the app does.
// - arm: no fire on the first click, fire on the second within armMs; the timeout and Esc disarm; a SplitButton's main key
//   carries the safety and its caret neither arms nor fires.
// - modal: the window's ConfirmPopup opens (lazily, one per window, parented to that window's overlay) with the key's text;
//   Enter (Return or keypad Enter) and the confirm key fire, Esc and Cancel do not, Enter on a focused Cancel cancels, a
//   press outside does nothing, a confirmation right after opening is ignored, the question is cancelled when the asking
//   key stops being ready or is destroyed, a second question replaces the first, asking disarms an armed key, and the
//   window's own window-context shortcuts are blocked while it is open.
// - application shortcuts (main.qml's On Air keys are all Qt.ApplicationShortcut, which modal blocking skips): Esc, Return
//   and keypad Enter stay the dialog's even when the application binds them; other application shortcuts still act, and one
//   that adds `!BtnTokens.confirmOpen` yields. One question in the application: arming a key or asking in another window
//   cancels it, and BtnTokens.confirming names the open popup.
TestCase {
    id: tc
    name: "Safety"
    when: windowShown
    width: 640; height: 400

    Component { id: btnC; PButton { } }
    Component { id: splitC; SplitButton { } }
    Component { id: spyC; SignalSpy { } }
    // a window-level Space shortcut, as main.qml's play / pause
    Component {
        id: spaceHostC
        Item {
            id: host
            property int hits: 0
            Shortcut { sequence: "Space"; onActivated: host.hits++ }
        }
    }
    // Application-wide shortcuts, as main.qml's On Air keys: an Esc that does not yield (clear preview, main.qml:119 today),
    // Return / keypad Enter (a user's Num+Enter hotkey), Ctrl+Return (CUT), and an Alt+1 (TAKE) that yields to a question.
    Component {
        id: appHostC
        Item {
            id: ah
            property int esc: 0
            property int ret: 0
            property int cut: 0
            property int take: 0
            property int ambiguous: 0
            Shortcut { sequence: "Escape"; context: Qt.ApplicationShortcut; onActivated: ah.esc++; onActivatedAmbiguously: ah.ambiguous++ }
            Shortcut { sequences: ["Return", "Num+Enter"]; context: Qt.ApplicationShortcut; onActivated: ah.ret++; onActivatedAmbiguously: ah.ambiguous++ }
            Shortcut { sequence: "Ctrl+Return"; context: Qt.ApplicationShortcut; onActivated: ah.cut++ }
            Shortcut { sequence: "Alt+1"; context: Qt.ApplicationShortcut; enabled: !BtnTokens.confirmOpen; onActivated: ah.take++ }
        }
    }
    // a second window with two modal keys
    Component {
        id: winC
        Window {
            id: w
            property alias k1: key1
            property alias k2: key2
            width: 320; height: 200
            visible: true
            PButton { id: key1; x: 20; y: 20; text: "DELETE A"; safety: "modal" }
            PButton { id: key2; x: 160; y: 20; text: "DELETE B"; safety: "modal" }
        }
    }

    function stage() { return tc.Window.contentItem }
    function make(props) {
        const o = createTemporaryObject(btnC, stage(), Object.assign({ x: 20, y: 20 }, props || {}))
        verify(o !== null, "PButton did not create")
        return o
    }
    function spy(target, sig) { return createTemporaryObject(spyC, tc, { target: target, signalName: sig }) }
    function settle() { wait(BtnTokens.timing.minIntervalMs + 60) }
    function popupFor(item) { return BtnTokens.confirmPopup(item.Overlay.overlay) }
    function openedFor(item) {
        const p = popupFor(item)
        verify(p !== null, "no ConfirmPopup for this window")
        tryVerify(() => p.opened, 1000, "the ConfirmPopup did not open")
        return p
    }
    function key(p, name) { const k = findChild(p.contentItem, name); verify(k !== null, name); return k }

    // Each test starts with this window active (a second window may have taken the activation) and no question open.
    function init() {
        const win = tc.Window.window
        if (win && !win.active) { win.requestActivate(); tryVerify(() => win.active, 2000, "the test window is not active") }
    }
    function cleanup() {
        for (const e of BtnTokens._popups)
            if (BtnTokens._live(e) && e.popup.visible) { e.popup.cancel(); tryVerify(() => !e.popup.visible, 1000) }
    }

    // ---- arm ------------------------------------------------------------------------------------------------------------
    function test_armFirstClickArmsSecondFires() {
        const b = make({ text: "STOP", role: "stop", safety: "arm", armMs: 1500 })
        const fired = spy(b, "fired")
        mouseClick(b)
        compare(fired.count, 0, "the first click only arms")
        verify(b.armed)
        compare(BtnTokens.armedItem, b)
        settle()
        mouseClick(b)
        compare(fired.count, 1, "a second click within armMs fires")
        verify(!b.armed)
        compare(BtnTokens.armedItem, null)
    }

    function test_armDisarmsOnTimeout() {
        const b = make({ text: "STOP", safety: "arm", armMs: 400 })
        const fired = spy(b, "fired")
        mouseClick(b)
        verify(b.armed)
        tryCompare(b, "armed", false, 1500, "the arm times out after armMs")
        compare(fired.count, 0)
        mouseClick(b)
        compare(fired.count, 0, "after the timeout a click arms again; it does not fire")
        verify(b.armed)
    }

    function test_armDisarmsOnEsc() {
        const b = make({ text: "STOP", safety: "arm", armMs: 3000 })
        const fired = spy(b, "fired")
        mouseClick(b)
        verify(b.armed)
        keyClick(Qt.Key_Escape)
        verify(!b.armed, "Esc disarms")
        compare(BtnTokens.armedItem, null)
        settle()
        mouseClick(b)
        compare(fired.count, 0, "the click after Esc arms again")
        verify(b.armed)
    }

    // The armed state must show on every key: amber marks on a solid amber (warn) or yellow key would vanish, so those take
    // the fill's text colour; other keys keep the arm colour.
    function test_armMarksVisibleOnEveryKey() {
        const amber = BtnTokens.hex(BtnTokens.states.armColor), dark = BtnTokens.onFill
        const cases = [
            { props: { role: "warn" }, ink: dark, what: "solid warn (amber)" },
            { props: { role: "primary", customColor: "#ffd60a" }, ink: dark, what: "solid yellow customColor" },
            { props: { role: "stop" }, ink: amber, what: "solid stop (red)" },
            { props: { role: "go" }, ink: amber, what: "solid go (green)" },
            { props: { role: "warn", fill: "outline" }, ink: amber, what: "outline warn" },
            { props: {}, ink: amber, what: "neutral" } ]
        for (const c of cases) {
            const b = make(Object.assign({ text: "RESTART", safety: "arm" }, c.props))
            mouseClick(b)
            verify(b.armed, c.what)
            const pulse = findChild(b, "pbArmPulse"), bar = findChild(b, "pbArmBar")
            verify(Qt.colorEqual(BtnTokens.col(pulse.border.color), BtnTokens.col(c.ink)), c.what + ": pulse " + pulse.border.color)
            verify(Qt.colorEqual(BtnTokens.col(bar.color), BtnTokens.col(c.ink)), c.what + ": bar " + bar.color)
            b.disarm()
            b.destroy()
        }
    }

    function test_splitButtonArmsMainOnly() {
        const s = createTemporaryObject(splitC, stage(), { x: 20, y: 20, text: "PUSH", role: "warn", safety: "arm" })
        const fired = spy(s, "fired"), menu = spy(s, "menuRequested")
        compare(s.mainButton.safety, "arm")
        compare(s.menuButton.safety, "none", "the caret carries no safety: opening a menu is harmless")
        mouseClick(s.mainButton)
        verify(s.armed && s.mainButton.armed)
        compare(fired.count, 0)
        mouseClick(s.menuButton)
        compare(menu.count, 1)
        compare(fired.count, 0, "the caret neither fires nor confirms")
        verify(s.armed, "and leaves the main key armed")
        settle()
        mouseClick(s.mainButton)
        compare(fired.count, 1)
        verify(!s.armed)
    }

    function test_splitButtonHold() {
        const s = createTemporaryObject(splitC, stage(), { x: 20, y: 20, text: "MUX STOP", safety: "hold", holdMs: 300 })
        const fired = spy(s, "fired")
        mousePress(s.mainButton)
        wait(100)
        mouseRelease(s.mainButton)
        compare(fired.count, 0, "an early release cancels")
        settle()
        mousePress(s.mainButton)
        tryCompare(fired, "count", 1, 1500, "held for holdMs it fires")
        mouseRelease(s.mainButton)
    }

    // ---- modal ----------------------------------------------------------------------------------------------------------
    function test_modalOpensAndEnterFires() {
        const b = make({ text: "DELETE", role: "destructive", safety: "modal", confirmText: "Delete channel main? Its engine stops." })
        const fired = spy(b, "fired")
        mouseClick(b)
        const p = openedFor(b)
        verify(!b.armed, "a modal key asks; it does not arm")
        compare(fired.count, 0)
        compare(p.title, "DELETE")
        compare(p.text, "Delete channel main? Its engine stops.")
        compare(p.confirmLabel, "DELETE")
        compare(p.role, "destructive")
        compare(p.source, b)
        verify(p.modal && p.dim)
        compare(p.parent, b.Overlay.overlay, "parented to the window's overlay")
        const dimmer = findChild(p.parent, "confirmDim")
        verify(dimmer !== null && dimmer.visible, "the window is dimmed")
        verify(Qt.colorEqual(dimmer.color, "#000000a0"), "with the app's modal dim, not Fusion's faint one: " + dimmer.color)
        verify(findChild(p.contentItem, "confirmText").visible)
        const ok = key(p, "confirmOk")
        compare(ok.text, "DELETE")
        compare(ok.role, "destructive")
        verify(ok.Accessible.defaultButton)
        settle()
        keyClick(Qt.Key_Return)
        compare(fired.count, 1, "Enter confirms")
        tryVerify(() => !p.visible, 1000)
        compare(p.source, null, "the source is dropped on close")

        settle()
        b.forceActiveFocus(Qt.TabFocusReason)             // the keyboard path: Space on the key, then keypad Enter
        keyClick(Qt.Key_Space)
        tryVerify(() => p.opened, 1000, "Space on a focused modal key asks too")
        settle()
        keyClick(Qt.Key_Enter)
        compare(fired.count, 2, "keypad Enter confirms")
        tryVerify(() => !p.visible, 1000)
    }

    function test_modalEscCancelAndConfirmKeys() {
        const b = make({ text: "REMOVE", safety: "modal", tip: "Remove the output" })
        const fired = spy(b, "fired")
        mouseClick(b)
        let p = openedFor(b)
        compare(p.text, "Remove the output", "without confirmText the tip is the question")
        const cancelled = spy(p, "cancelled"), confirmed = spy(p, "confirmed")
        keyClick(Qt.Key_Escape)
        tryVerify(() => !p.visible, 1000, "Esc closes")
        compare(fired.count, 0, "Esc does not fire")
        compare(cancelled.count, 1)

        mouseClick(b)
        p = openedFor(b)
        settle()
        mouseClick(key(p, "confirmCancel"))
        tryVerify(() => !p.visible, 1000)
        compare(fired.count, 0, "Cancel does not fire")
        compare(cancelled.count, 2)

        mouseClick(b)
        p = openedFor(b)
        mouseClick(stage(), 4, stage().height - 4)        // on the dimmed window, outside the dialog
        verify(p.opened, "a press outside does not answer the question")
        settle()
        mouseClick(key(p, "confirmOk"))
        compare(fired.count, 1, "the confirm key fires")
        compare(confirmed.count, 1)
        compare(cancelled.count, 2)
        tryVerify(() => !p.visible, 1000)
    }

    function test_modalEnterOnFocusedCancelCancels() {
        const b = make({ text: "DELETE", safety: "modal" })
        const fired = spy(b, "fired")
        mouseClick(b)
        const p = openedFor(b)
        settle()
        const cancelKey = key(p, "confirmCancel")
        cancelKey.forceActiveFocus(Qt.TabFocusReason)
        verify(cancelKey.activeFocus)
        keyClick(Qt.Key_Return)
        tryVerify(() => !p.visible, 1000)
        compare(fired.count, 0, "with the keyboard on Cancel, Enter cancels")

        // The popup is a focus scope: the next question must not start on the Cancel key the last one tabbed to.
        mouseClick(b)
        openedFor(b)
        verify(!cancelKey.activeFocus, "a new question starts with the keyboard on the dialog")
        settle()
        keyClick(Qt.Key_Return)
        compare(fired.count, 1, "so Enter confirms it")
        tryVerify(() => !p.visible, 1000)
    }

    function test_modalIgnoresConfirmRightAfterOpening() {
        const b = make({ text: "DELETE", safety: "modal" })
        const fired = spy(b, "fired")
        mouseClick(b)
        const p = openedFor(b)
        const saved = p.minIntervalMs
        try {
            p.minIntervalMs = 5000                         // keep the window open for the whole check
            p._openedAt = Date.now()
            keyClick(Qt.Key_Return)
            verify(p.opened, "an Enter right after opening is ignored")
            mouseClick(key(p, "confirmOk"))
            verify(p.opened, "so is a click on the confirm key (a double click's second click)")
            compare(fired.count, 0)
            p._openedAt = Date.now() + 60000               // a backward clock step counts as elapsed
            keyClick(Qt.Key_Return)
            compare(fired.count, 1)
            tryVerify(() => !p.visible, 1000)
        } finally {
            p.minIntervalMs = saved
        }
    }

    function test_modalCancelsWhenSourceStopsBeingReady() {
        const muts = [
            { what: "disabled", run: k => k.enabled = false },
            { what: "busy", run: k => k.busy = true },
            { what: "no longer modal", run: k => k.safety = "none" },
            { what: "destroyed", run: k => k.destroy() } ]
        for (const m of muts) {
            const b = make({ text: "PURGE", safety: "modal" })
            const fired = spy(b, "fired")
            mouseClick(b)
            const p = openedFor(b)
            const cancelled = spy(p, "cancelled")
            m.run(b)
            tryVerify(() => !p.visible, 1000, m.what + ": the question closes")
            compare(cancelled.count, 1, m.what + ": cancelled")
            compare(fired.count, 0, m.what + ": nothing fires")
            settle()
        }
    }

    function test_secondAskReplacesFirst() {
        const p = BtnTokens.confirmPopup(tc.Overlay.overlay)
        verify(p !== null)
        let first = 0, second = 0
        const cancelled = spy(p, "cancelled")
        p.ask("First", "", "OK", "stop", function() { first++ })
        tryVerify(() => p.opened, 1000)
        p.ask("Second", "Body", "Go", "go", function() { second++ })
        compare(cancelled.count, 1, "the open question is cancelled first")
        verify(p.opened)
        compare(p.title, "Second")
        compare(key(p, "confirmOk").text, "Go")
        compare(p.source, null, "no source: nothing to watch")
        settle()
        keyClick(Qt.Key_Return)
        compare(second, 1)
        compare(first, 0, "the replaced question never confirms")
        tryVerify(() => !p.visible, 1000)
    }

    function test_askDisarmsArmedKey() {
        const a = make({ text: "STOP", safety: "arm" })
        const m = make({ text: "DELETE", safety: "modal", x: 200 })
        mouseClick(a)
        verify(a.armed)
        mouseClick(m)
        openedFor(m)
        verify(!a.armed, "one pending confirmation: the armed key disarmed")
        compare(BtnTokens.armedItem, null)
    }

    // Window-context shortcuts only: Qt's modal blocking does not cover Qt.ApplicationShortcut (next test).
    function test_modalBlocksWindowShortcuts() {
        const host = createTemporaryObject(spaceHostC, stage())
        keyClick(Qt.Key_Space)
        compare(host.hits, 1, "the window shortcut works with nothing open")
        const m = make({ text: "DELETE", safety: "modal" })
        mouseClick(m)
        const p = openedFor(m)
        keyClick(Qt.Key_Space)
        compare(host.hits, 1, "Space does not reach the window while the confirmation is open")
        keyClick(Qt.Key_Escape)
        tryVerify(() => !p.visible, 1000)
        keyClick(Qt.Key_Space)
        compare(host.hits, 2, "and works again once it is closed")
    }

    // main.qml's Esc (clear preview) is an application shortcut that does not yield: before this, every Esc cleared the
    // preview and left the question open. The dialog's keys are its own while the keyboard is on it.
    function test_modalOwnsItsKeysOverAppShortcuts() {
        const host = createTemporaryObject(appHostC, stage())
        keyClick(Qt.Key_Escape)
        keyClick(Qt.Key_Return)
        keyClick(Qt.Key_Enter, Qt.KeypadModifier)
        compare(host.esc, 1, "nothing open: the application's Esc runs")
        compare(host.ret, 2, "and its Return and keypad Enter")
        compare(BtnTokens.confirming, null)
        verify(!BtnTokens.confirmOpen)

        const b = make({ text: "DELETE", safety: "modal" })
        const fired = spy(b, "fired")
        mouseClick(b)
        let p = openedFor(b)
        compare(BtnTokens.confirming, p, "BtnTokens.confirming names the open popup")
        verify(BtnTokens.confirmOpen)
        keyClick(Qt.Key_Escape)
        tryVerify(() => !p.visible, 1000, "Esc cancels the question")
        compare(host.esc, 1, "and does not reach the application's Esc")
        compare(fired.count, 0)
        compare(BtnTokens.confirming, null, "closed: confirming is cleared")
        verify(!BtnTokens.confirmOpen)
        keyClick(Qt.Key_Escape)
        compare(host.esc, 2, "and Esc is the application's again")

        mouseClick(b)
        p = openedFor(b)
        settle()
        keyClick(Qt.Key_Return)
        compare(fired.count, 1, "Return confirms")
        tryVerify(() => !p.visible, 1000)
        settle()
        mouseClick(b)
        p = openedFor(b)
        settle()
        keyClick(Qt.Key_Enter, Qt.KeypadModifier)
        compare(fired.count, 2, "keypad Enter confirms")
        tryVerify(() => !p.visible, 1000)
        compare(host.ret, 2, "the application's Return / Num+Enter took neither")

        settle()
        mouseClick(b)
        p = openedFor(b)
        settle()
        key(p, "confirmCancel").forceActiveFocus(Qt.TabFocusReason)
        keyClick(Qt.Key_Return)
        tryVerify(() => !p.visible, 1000, "with the keyboard on Cancel, Enter still answers the dialog: it cancels")
        compare(fired.count, 2)
        compare(host.ret, 2)
        compare(host.ambiguous, 0)

        // Application shortcuts the dialog does not use still act during a question (UXB-9 / UXB-11 decide whether the On Air
        // keys should); one with `!BtnTokens.confirmOpen` in its enabled yields.
        settle()
        mouseClick(b)
        p = openedFor(b)
        keyClick(Qt.Key_Return, Qt.ControlModifier)
        compare(host.cut, 1, "Ctrl+Return (CUT) is not the dialog's: it still reaches the application")
        keyClick(Qt.Key_1, Qt.AltModifier)
        compare(host.take, 0, "a shortcut that adds !BtnTokens.confirmOpen yields")
        verify(p.opened, "the question stays open")
        compare(fired.count, 2)
        p.cancel()
        tryVerify(() => !p.visible, 1000)
        keyClick(Qt.Key_1, Qt.AltModifier)
        compare(host.take, 1, "and acts again once it is closed")
    }

    // One question in the application, as one armed key (BtnTokens.arming).
    function test_oneQuestionInTheApplication() {
        const a = make({ text: "STOP", safety: "arm" })
        const m = make({ text: "DELETE", safety: "modal", x: 200 })
        const fired = spy(m, "fired")
        mouseClick(m)
        const p = openedFor(m)
        const cancelled = spy(p, "cancelled")
        a._clickedNow()                                    // a key arming (another window's, or the remote pad's)
        verify(a.armed)
        tryVerify(() => !p.visible, 1000, "a key arming cancels the open question")
        compare(cancelled.count, 1)
        compare(BtnTokens.confirming, null)
        a.disarm()

        settle()
        mouseClick(m)
        openedFor(m)
        const w = createTemporaryObject(winC, tc)
        verify(w !== null)
        tryVerify(() => w.visible && w.k1.width > 0, 2000)
        mouseClick(w.k1)
        const p2 = BtnTokens.confirmPopup(w.k1.Overlay.overlay)
        verify(p2 !== null && p2 !== p)
        tryVerify(() => p2.opened, 1000)
        tryVerify(() => !p.visible, 1000, "asking in another window cancels this window's question")
        compare(cancelled.count, 2)
        compare(BtnTokens.confirming, p2)
        w.destroy()
        tryVerify(() => BtnTokens.confirming === null, 1000, "a popup that goes with its window while asking clears confirming")
        verify(!BtnTokens.confirmOpen)
        compare(fired.count, 0)
    }

    function test_lazyOnePerWindow() {
        const w = createTemporaryObject(winC, tc)
        verify(w !== null)
        tryVerify(() => w.visible && w.k1.width > 0, 2000)
        const ov2 = w.k1.Overlay.overlay
        verify(ov2 !== null && ov2 !== tc.Overlay.overlay)
        const mine = () => BtnTokens._popups.filter(e => e.overlay === ov2 && BtnTokens._live(e))
        compare(mine().length, 0, "no ConfirmPopup exists before the window's first modal press")
        const fired1 = spy(w.k1, "fired")
        mouseClick(w.k1)
        compare(mine().length, 1, "the first modal press creates one")
        const p2 = mine()[0].popup
        compare(p2.parent, ov2, "parented to that window's overlay")
        tryVerify(() => p2.opened, 1000)
        compare(p2.source, w.k1)
        p2.cancel()
        tryVerify(() => !p2.visible, 1000)
        mouseClick(w.k2)
        compare(mine().length, 1, "the window's second modal key reuses it")
        compare(BtnTokens.confirmPopup(ov2), p2)
        compare(p2.source, w.k2)
        p2.cancel()
        tryVerify(() => !p2.visible, 1000)
        compare(fired1.count, 0)

        const pMain = BtnTokens.confirmPopup(tc.Overlay.overlay)
        verify(pMain !== null && pMain !== p2, "each window has its own")
        w.destroy()
        tryVerify(() => !BtnTokens._live({ popup: p2 }), 1000, "the popup goes with its window")
        compare(BtnTokens.confirmPopup(tc.Overlay.overlay), pMain, "the other window keeps its popup")
        verify(!BtnTokens._popups.some(e => e.popup === p2), "the dead entry is dropped")
    }
}
