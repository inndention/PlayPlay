import QtQuick
import QtQuick.Controls
import QtTest
import PlayPlay.Buttons

// UXB-3a: ToggleButton toggles only when it fires, so disabled, busy, the debounce and the safety level gate the toggle too;
// `checked` shows lit; an exclusive ButtonGroup keeps one checked; autoToggle: false leaves `checked` (and its binding) to
// the owner.
TestCase {
    id: tc
    name: "ToggleButton"
    when: windowShown
    width: 480; height: 320

    Component { id: toggleC; ToggleButton { } }
    Component { id: spyC; SignalSpy { } }
    Component {
        id: groupC
        Item {
            property alias a: ta
            property alias b: tb
            property alias group: grp
            width: 400; height: 60
            ButtonGroup { id: grp }
            ToggleButton { id: ta; x: 20; y: 20; text: "MIX"; size: "xs"; ButtonGroup.group: grp }
            ToggleButton { id: tb; x: 120; y: 20; text: "WIPE"; size: "xs"; ButtonGroup.group: grp }
        }
    }
    Component {
        id: autoExC
        Item {
            property alias a: ta
            property alias b: tb
            width: 400; height: 60
            Row {
                x: 20; y: 20; spacing: 20
                ToggleButton { id: ta; text: "MIX"; size: "xs"; autoExclusive: true }
                ToggleButton { id: tb; text: "WIPE"; size: "xs"; autoExclusive: true }
            }
        }
    }
    // ButtonGroup { buttons: ... }: Qt does not tell the buttons about this group, so they say keepChecked themselves.
    Component {
        id: listGroupC
        Item {
            property alias a: ta
            property alias b: tb
            property alias group: grp
            width: 400; height: 60
            ButtonGroup { id: grp; buttons: row.children }
            Row {
                id: row
                x: 20; y: 20; spacing: 20
                ToggleButton { id: ta; text: "MIX"; size: "xs"; keepChecked: true }
                ToggleButton { id: tb; text: "WIPE"; size: "xs"; keepChecked: true }
            }
        }
    }
    Component {
        id: boundC
        Item {
            property bool onAir: false
            property alias key: tk
            width: 200; height: 60
            ToggleButton { id: tk; x: 20; y: 20; text: "CG1"; autoToggle: false; checked: parent.onAir }
        }
    }

    function stage() { return tc.Window.contentItem }
    function make(props) {
        const o = createTemporaryObject(toggleC, stage(), Object.assign({ x: 20, y: 20, text: "AFV" }, props || {}))
        verify(o !== null)
        return o
    }
    function spy(target, sig) { return createTemporaryObject(spyC, tc, { target: target, signalName: sig }) }
    function settle() { wait(BtnTokens.timing.minIntervalMs + 60) }

    function test_togglesWhenFired() {
        const t = make()
        const fired = spy(t, "fired"), toggled = spy(t, "toggled")
        verify(!t.checkable, "AbstractButton's own toggle stays off (it would bypass safety, busy and the debounce)")
        verify(!t.lit)
        mouseClick(t)
        verify(t.checked)
        verify(t.lit)
        compare(fired.count, 1)
        compare(toggled.count, 1)
        mouseMove(stage(), stage().width - 2, stage().height - 2)
        tryVerify(() => !t.hovered)
        const want = BtnTokens.alpha(BtnTokens.hex("text1"), BtnTokens.states.litA), got = t.background.color
        verify(Math.abs(got.r - want.r) < 0.005 && Math.abs(got.g - want.g) < 0.005 && Math.abs(got.a - want.a) < 0.005, "checked shows lit: " + got)
        compare(t.background.border.width, BtnTokens.states.litBorderW)
        settle()
        mouseClick(t)
        verify(!t.checked)
        compare(toggled.count, 2)
    }

    function test_checkedBeforeFired() {
        const t = make()
        let seen = null
        t.fired.connect(function() { seen = t.checked })
        mouseClick(t)
        compare(seen, true, "onFired already sees the new state")
    }

    function test_noToggleWhenDisabledOrBusy() {
        const d = make({ enabled: false })
        mouseClick(d)
        verify(!d.checked)
        const b = make({ busy: true, y: 60 })
        mouseClick(b)
        verify(!b.checked)
        b.busy = false
        mouseClick(b)
        verify(b.checked)
    }

    function test_debouncedToggle() {
        const t = make()
        const toggled = spy(t, "toggled")
        mouseClick(t); mouseClick(t)
        verify(t.checked, "two quick clicks are one toggle")
        compare(toggled.count, 1)
        const d = make({ y: 60 })
        mouseDoubleClickSequence(d)
        verify(d.checked, "a double click is one toggle")
    }

    function test_autoToggleOffKeepsBinding() {
        const o = createTemporaryObject(boundC, stage())
        const k = o.key
        const fired = spy(k, "fired")
        mouseClick(k)
        compare(fired.count, 1)
        verify(!k.checked, "the owner decides the state")
        o.onAir = true
        verify(k.checked, "the binding to the owner's state survives the click")
        o.onAir = false
        verify(!k.checked)
    }

    function test_exclusiveGroup() {
        const o = createTemporaryObject(groupC, stage())
        mouseClick(o.a)
        verify(o.a.checked)
        compare(o.group.checkedButton, o.a)
        mouseClick(o.b)
        verify(o.b.checked)
        verify(!o.a.checked, "the group unchecks the other button")
        compare(o.group.checkedButton, o.b)
        settle()
        mouseClick(o.b)
        verify(o.b.checked, "clicking the checked button of an exclusive group keeps it checked")
    }

    function test_autoExclusive() {
        const o = createTemporaryObject(autoExC, stage())
        const fired = spy(o.a, "fired")
        mouseClick(o.a)
        verify(o.a.checked)
        mouseClick(o.b)
        verify(o.b.checked)
        verify(!o.a.checked, "autoExclusive unchecks the sibling")
        settle()
        mouseClick(o.b)
        verify(o.b.checked, "clicking the checked autoExclusive button keeps it checked")
        settle()
        mouseClick(o.a)
        verify(o.a.checked && !o.b.checked)
        compare(fired.count, 2)
    }

    function test_buttonsListGroupWithKeepChecked() {
        const o = createTemporaryObject(listGroupC, stage())
        compare(o.a.ButtonGroup.group, null, "the buttons: form leaves the attached group empty")
        mouseClick(o.a)
        verify(o.a.checked)
        compare(o.group.checkedButton, o.a)
        mouseClick(o.b)
        verify(o.b.checked && !o.a.checked, "the group unchecks the other button")
        settle()
        mouseClick(o.b)
        verify(o.b.checked, "keepChecked: clicking the checked button keeps it checked")
        compare(o.group.checkedButton, o.b)
        const lone = make({ keepChecked: true, y: 100 })
        mouseClick(lone)
        settle()
        mouseClick(lone)
        verify(lone.checked, "keepChecked works without any group")
    }

    function test_armGatesToggle() {
        const t = make({ safety: "arm" })
        const fired = spy(t, "fired")
        mouseClick(t)
        verify(t.armed)
        verify(!t.checked, "arming does not toggle")
        settle()
        mouseClick(t)
        verify(t.checked)
        compare(fired.count, 1)
    }

    function test_accessibility() {
        const t = make({ tip: "Audio follows video" })
        verify(t.Accessible.checkable)
        verify(!t.Accessible.checked)
        mouseClick(t)
        verify(t.Accessible.checked)
        compare(t.Accessible.name, "AFV")
    }
}
