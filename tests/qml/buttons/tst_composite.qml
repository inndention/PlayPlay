import QtQuick
import QtQuick.Controls
import QtTest
import PlayPlay.Buttons

// UXB-3b: the composite keys of PlayPlay.Buttons, imported from qrc:/qml as the app does.
// - ChipGroup: single and multi selection, autoSelect off keeps the owner's binding, string and object models, labelFn,
//   disabled entries, right-click menus, equal widths, neutral at rest and lit in its role.
// - CycleButton: steps and wraps, keeps one width, labels / suffix, autoStep off keeps the owner's binding, edge indexes.
// - SplitButton: fired from the main key, menuRequested(anchor) from the caret, joined segments, the icon alias, busy on the
//   main key only, Alt+Down.
// - DisclosureButton: chevrons, expanded, the status tag in warn, accessibility, an owner-held state.
// - RailButton: 36 x 36, the tooltip on the right, a click never breaks the owner's `checked`, the channel variant.
// - PButton's additions for these: litRole, segment, badgeColor.
TestCase {
    id: tc
    name: "Composite"
    when: windowShown
    width: 640; height: 400

    Component { id: btnC; PButton { } }
    Component { id: chipsC; ChipGroup { } }
    Component { id: cycleC; CycleButton { } }
    Component { id: splitC; SplitButton { } }
    Component { id: discC; DisclosureButton { } }
    Component { id: railC; RailButton { } }
    Component { id: spyC; SignalSpy { } }
    Component {
        id: boundChipsC
        Item {
            id: host
            property string cur: "dip"
            property alias group: grp
            width: 400; height: 40
            ChipGroup { id: grp; x: 10; y: 10; model: ["mix", "dip"]; autoSelect: false; selected: host.cur }
        }
    }
    Component {
        id: boundCycleC
        Item {
            id: host
            property real sec: 0.5
            property alias key: cyc
            width: 200; height: 40
            CycleButton {
                id: cyc; x: 10; y: 10
                values: [0.25, 0.5, 1, 2]; suffix: " s"; autoStep: false
                index: cyc.values.indexOf(host.sec)
                onStepped: function(i, v) { host.sec = v }
            }
        }
    }
    Component {
        id: boundDiscC
        Item {
            id: host
            property bool open: false
            property alias key: dk
            width: 300; height: 40
            DisclosureButton { id: dk; x: 10; y: 10; text: "PTZ"; autoToggle: false; checked: host.open; onFired: host.open = !host.open }
        }
    }
    Component {
        id: boundRailC
        Item {
            id: host
            property string page: "onair"
            property alias key: rk
            width: 100; height: 100
            RailButton { id: rk; x: 10; y: 10; icon.name: "broadcast"; tip: "On Air"; checked: host.page === "onair"; onFired: host.page = "onair" }
        }
    }

    function stage() { return tc.Window.contentItem }
    function create(c, props) {
        const o = createTemporaryObject(c, stage(), Object.assign({ x: 10, y: 10 }, props || {}))
        verify(o !== null, "did not create")
        return o
    }
    function spy(target, sig) { return createTemporaryObject(spyC, tc, { target: target, signalName: sig }) }
    function settle() { wait(BtnTokens.timing.minIntervalMs + 60) }
    function away() { mouseMove(stage(), stage().width - 2, stage().height - 2) }
    function same(a, b) {
        const x = Qt.tint(a, "transparent"), y = Qt.tint(b, "transparent"), e = 1 / 255 + 1e-6
        return Math.abs(x.r - y.r) <= e && Math.abs(x.g - y.g) <= e && Math.abs(x.b - y.b) <= e && Math.abs(x.a - y.a) <= e
    }
    function hex(n) { return BtnTokens.hex(n) }
    function chip(g, id) { const c = findChild(g, "chip:" + id); verify(c !== null, "chip " + id); return c }

    function init() {
        const win = tc.Window.window
        if (win && !win.active) { win.requestActivate(); tryVerify(() => win.active, 2000) }
    }

    // ---- PButton additions ----------------------------------------------------------------------------------------------
    function test_litRole() {
        away()
        const k = create(btnC, { text: "MIX", size: "xs", litRole: "go" })
        compare(k.effectiveFill, "outline", "neutral at rest")
        verify(same(k.background.color, hex("bg3")))
        verify(same(k.background.border.color, hex("border1")))
        k.active = true
        verify(same(k.background.color, BtnTokens.alpha(hex("ready"), BtnTokens.states.litA)), "lit in the litRole colour")
        verify(same(k.background.border.color, hex("ready")))
        compare(k.background.border.width, BtnTokens.states.litBorderW)
        verify(same(findChild(k, "pbLabel").color, hex("ready")))
        const r = create(btnC, { text: "R", role: "stop", y: 60 })
        verify(same(r.background.color, hex("onAir")), "no litRole: the role colour as before")
    }

    function test_segment() {
        const whole = create(btnC, { text: "W" }), bg = whole.background
        for (const c of ["topLeftRadius", "topRightRadius", "bottomLeftRadius", "bottomRightRadius"]) compare(bg[c], 5, c)
        const l = create(btnC, { text: "L", segment: "left", y: 50 }).background
        compare(l.topLeftRadius, 5); compare(l.bottomLeftRadius, 5)
        compare(l.topRightRadius, 0); compare(l.bottomRightRadius, 0)
        const r = create(btnC, { text: "R", segment: "right", y: 90 }).background
        compare(r.topLeftRadius, 0); compare(r.bottomLeftRadius, 0)
        compare(r.topRightRadius, 5); compare(r.bottomRightRadius, 5)
        const mk = create(btnC, { text: "M", segment: "middle", y: 130 })
        mouseMove(mk, mk.width / 2, mk.height / 2)      // the state layer is made on the first hover
        tryVerify(() => findChild(mk, "pbStateLayer") !== null, 1000)
        for (const c of ["topLeftRadius", "topRightRadius", "bottomLeftRadius", "bottomRightRadius"]) {
            compare(mk.background[c], 0, "middle: " + c)
            compare(findChild(mk, "pbStateLayer")[c], 0, "middle state layer: " + c)
        }
    }

    function test_padX() {
        const k = create(btnC, { text: "LOAD HOUR", size: "s" })
        compare(k.padX, BtnTokens.size("s").padX, "the size token by default")
        const lab = findChild(k, "pbLabel")
        const w0 = k.implicitWidth
        k.padX = 2
        fuzzyCompare(k.implicitWidth, w0 - 2 * (BtnTokens.size("s").padX - 2), 0.01, "the width follows padX")
        k.width = lab.implicitWidth + 4
        verify(lab.width >= lab.implicitWidth - 0.01, "and so does the room the label gets")
    }

    function test_badgeColor() {
        const k = create(btnC, { text: "AUDIO", size: "s", badge: "SOLO", badgeColor: hex("warn") })
        const t = findChild(k, "pbRowBadgeText")
        verify(same(t.color, hex("warn")), "the tag is drawn in badgeColor")
        k.badgeColor = "transparent"
        verify(same(t.color, hex("text1")), "transparent: the plain tag")
        k.badgeColor = hex("warn")
        k.enabled = false
        verify(same(t.color, hex("text1")), "a disabled key drops the colour")
    }

    // ---- ChipGroup ------------------------------------------------------------------------------------------------------
    function test_chipGroupSingle() {
        const g = create(chipsC, { model: ["mix", "dip", "wipe_left"], selected: "mix", title: "Transition",
                                   labelFn: function(id) { return id.replace("_left", "").toUpperCase() } })
        compare(g.count, 3)
        const mix = chip(g, "mix"), dip = chip(g, "dip"), wipe = chip(g, "wipe_left")
        compare(dip.text, "DIP")
        compare(wipe.text, "WIPE", "labelFn gives the label")
        compare(mix.size, "xs")
        compare(mix.implicitHeight, 20)
        verify(mix.checked && !dip.checked && !wipe.checked)
        compare(mix.Accessible.role, Accessible.RadioButton)
        compare(g.Accessible.name, "Transition")
        const picked = spy(g, "picked")
        mouseClick(dip)
        compare(g.selected, "dip")
        verify(dip.checked && !mix.checked, "one selection")
        compare(picked.count, 1)
        compare(picked.signalArguments[0][0], "dip")
        settle()
        mouseClick(dip)
        compare(g.selected, "dip", "a click on the selected chip keeps it")
        compare(picked.count, 2, "and still reports the pick")
        mouseClick(wipe); mouseClick(wipe)
        compare(picked.count, 3, "a double click on a chip is one pick")
    }

    function test_chipGroupMulti() {
        const g = create(chipsC, { multi: true, selected: [],
                                   model: [{ id: "", name: "All mixes" }, { id: "main", name: "MAIN" }, { id: "aux", label: "AUX", tip: "Aux mix", led: "red", icon: "speaker" }] })
        const all = chip(g, ""), main = chip(g, "main"), aux = chip(g, "aux")
        compare(all.text, "All mixes", "name is the label when there is no label")
        compare(aux.tip, "Aux mix")
        compare(aux.led, "red")
        compare(aux.icon.name, "speaker")
        compare(main.Accessible.role, Accessible.CheckBox)
        const picked = spy(g, "picked")
        mouseClick(main)
        mouseClick(aux)
        compare(JSON.stringify(g.selected), JSON.stringify(["main", "aux"]))
        verify(main.checked && aux.checked && !all.checked)
        settle()
        mouseClick(main)
        compare(JSON.stringify(g.selected), JSON.stringify(["aux"]), "a second click removes it")
        verify(!main.checked && aux.checked)
        compare(picked.count, 3)
        verify(g.isSelected("aux"))
        g.selected = "main"                       // a lone id counts as a list of one
        verify(main.checked && !aux.checked)
    }

    function test_chipGroupOwnerHoldsSelection() {
        const o = create(boundChipsC)
        const g = o.group
        const picked = spy(g, "picked")
        verify(chip(g, "dip").checked)
        mouseClick(chip(g, "mix"))
        compare(picked.count, 1)
        compare(picked.signalArguments[0][0], "mix")
        compare(g.selected, "dip", "autoSelect off: the group does not write the selection")
        verify(!chip(g, "mix").checked)
        o.cur = "mix"
        verify(chip(g, "mix").checked && !chip(g, "dip").checked, "the binding to the owner survives the click")
    }

    function test_chipGroupDisabledMenuWidthLook() {
        away()
        const g = create(chipsC, { width: 300, fillWidth: true, role: "primary", selected: "auto",
                                   model: [{ id: "auto", name: "AUTO" }, { id: "p1", name: "PTZ1", enabled: false }, { id: "p2", name: "PTZ2" }] })
        const a = chip(g, "auto"), p1 = chip(g, "p1"), p2 = chip(g, "p2")
        const share = Math.floor((300 - g.spacing * 2) / 3)
        compare(a.width, share); compare(p1.width, share); compare(p2.width, share)
        tryVerify(() => a.y === p2.y && p2.x > p1.x, 1000, "fillWidth: one row")
        verify(p2.x + p2.width <= 300)
        const picked = spy(g, "picked"), menu = spy(g, "menuRequested")
        verify(!p1.enabled)
        mouseClick(p1)
        compare(picked.count, 0, "a disabled entry does not pick")
        mouseClick(p2, p2.width / 2, p2.height / 2, Qt.RightButton)
        compare(menu.count, 1, "right-click asks for the chip's menu")
        compare(menu.signalArguments[0][0], "p2")
        compare(menu.signalArguments[0][1], p2)
        compare(picked.count, 0, "and does not pick")
        compare(g.selected, "auto")
        verify(same(p2.background.color, hex("bg3")), "an unselected chip is a neutral outline key, not a solid one")
        verify(same(p2.background.border.color, hex("border1")))
        verify(same(a.background.color, BtnTokens.alpha(hex("accent0"), BtnTokens.states.litA)), "the selected chip lights in the group role")
        verify(same(a.background.border.color, hex("accent0")))
    }

    // ---- CycleButton ----------------------------------------------------------------------------------------------------
    function test_cycleWraps() {
        const c = create(cycleC, { values: [0.25, 0.5, 1, 2], suffix: " s", index: 1, size: "xs" })
        compare(c.text, "0.5 s")
        compare(c.value, 0.5)
        compare(c.count, 4)
        const st = spy(c, "stepped")
        const w0 = c.implicitWidth
        mouseClick(c)
        compare(c.index, 2); compare(c.text, "1 s")
        compare(st.signalArguments[0][0], 2); compare(st.signalArguments[0][1], 1)
        settle(); mouseClick(c)
        compare(c.text, "2 s")
        settle(); mouseClick(c)
        compare(c.index, 0, "it wraps from the last value to the first")
        compare(c.text, "0.25 s")
        compare(st.count, 3)
        compare(c.implicitWidth, w0, "cycling never changes the width")
        for (let i = 0; i < c.count; ++i) {
            c.index = i
            const lab = findChild(c, "pbLabel")
            verify(lab.width >= lab.implicitWidth - 0.01, "label " + c.text + " is not elided")
        }
        c.index = 0
        settle()
        mouseDoubleClickSequence(c)
        compare(c.index, 1, "a double click is one step")
        compare(c.Accessible.description, "Next: 1 s")
    }

    function test_cycleLabelsAndEdges() {
        const c = create(cycleC, { values: [0, 1], labels: ["OFF", "ON"] })
        compare(c.text, "OFF")
        mouseClick(c)
        compare(c.text, "ON")
        const e = create(cycleC, { values: [], y: 60 })
        const st = spy(e, "stepped")
        compare(e.text, "")
        mouseClick(e)
        compare(st.count, 0, "an empty list does not step")
        compare(e.implicitWidth, BtnTokens.size("m").minW)
        const o = create(cycleC, { values: ["a", "b", "c"], index: -1, x: 200 })
        const so = spy(o, "stepped")
        compare(o.text, "", "an index outside the list shows nothing")
        mouseClick(o)
        compare(so.signalArguments[0][0], 0, "and steps to the first value")
        compare(o.text, "a")
        compare(o.nextIndex(-1), 2, "backwards wraps too")
    }

    function test_cycleOwnerHoldsIndex() {
        const o = create(boundCycleC)
        const k = o.key
        compare(k.index, 1)
        compare(k.text, "0.5 s")
        mouseClick(k)
        compare(o.sec, 1, "the owner applies the stepped value")
        compare(k.index, 2, "and the index follows its binding")
        o.sec = 0.25
        compare(k.index, 0, "the binding survives the click")
        o.sec = 0.75
        compare(k.text, "", "a value the list lacks shows no label")
        settle()
        mouseClick(k)
        compare(o.sec, 0.25)
    }

    // ---- SplitButton ----------------------------------------------------------------------------------------------------
    function test_splitButton() {
        const s = create(splitC, { text: "STORE IN CG", "icon.name": "layers", role: "info", size: "s", tip: "Store in the last CG" })
        const main = s.mainButton, caret = s.menuButton
        compare(main.text, "STORE IN CG")
        compare(main.icon.name, "layers", "icon.name rides through the alias")
        compare(caret.icon.name, "caret-down")
        compare(caret.tip, "More options")
        compare(caret.Accessible.role, Accessible.ButtonMenu)
        compare(caret.role, "info"); compare(caret.size, "s")
        compare(caret.width, Math.max(BtnTokens.size("s").icon + 6, Math.round(BtnTokens.size("s").h * 0.7)))
        compare(s.implicitWidth, main.implicitWidth + s.seam + caret.implicitWidth)
        compare(s.implicitHeight, 24)
        compare(caret.x, main.width + s.seam)
        compare(main.background.topRightRadius, 0, "the main key's inner corners are square")
        compare(main.background.topLeftRadius, 5)
        compare(caret.background.topLeftRadius, 0, "so are the caret's")
        compare(caret.background.bottomRightRadius, 5)
        const fired = spy(s, "fired"), menu = spy(s, "menuRequested")
        mouseClick(main)
        compare(fired.count, 1); compare(menu.count, 0)
        mouseClick(caret)
        compare(menu.count, 1); compare(fired.count, 1)
        compare(menu.signalArguments[0][0], caret, "the caret is the menu anchor")
        s.width = 300
        compare(main.width, 300 - s.seam - caret.width, "the main key takes the width the caret leaves")
        compare(caret.x + caret.width, 300)
        s.busy = true
        verify(main.busy && !caret.busy, "busy is the main key's")
        settle()
        mouseClick(main)
        compare(fired.count, 1)
        mouseClick(caret)
        compare(menu.count, 2, "the menu still opens while the main key is busy")
        s.busy = false
        main.forceActiveFocus(Qt.TabFocusReason)
        keyClick(Qt.Key_Down, Qt.AltModifier)
        compare(menu.count, 3, "Alt+Down on the main key opens the menu")
        s.enabled = false
        settle()
        mouseClick(caret)
        compare(menu.count, 3, "disabled: neither key acts")
    }

    // ---- DisclosureButton -----------------------------------------------------------------------------------------------
    function test_disclosure() {
        away()
        const d = create(discC, { text: "AUDIO MIXER", tip: "Show the mixer" })
        compare(d.size, "s")
        compare(d.implicitHeight, 24)
        compare(d.icon.name, "chevron-right")
        verify(!d.expanded)
        compare(d.Accessible.description, "Collapsed. Show the mixer")
        const toggled = spy(d, "toggled")
        mouseClick(d)
        verify(d.expanded && d.checked)
        compare(toggled.count, 1)
        compare(d.icon.name, "chevron-down")
        compare(d.Accessible.description, "Expanded. Show the mixer")
        away()
        tryVerify(() => same(d.background.color, BtnTokens.alpha(hex("accent0"), BtnTokens.states.litA)), 1000, "open: lit in the accent colour")
        const noTag = findChild(d, "pbRowBadge")         // the tag's Loader makes it only for a status
        verify(noTag === null || !noTag.visible, "no status, no tag")
        d.status = "SOLO · AFV OFF"
        const tag = findChild(d, "pbRowBadge")
        verify(tag.visible)
        compare(findChild(d, "pbRowBadgeText").text, "SOLO · AFV OFF")
        verify(same(findChild(d, "pbRowBadgeText").color, hex("warn")), "the status is in warn")
        compare(d.Accessible.name, "AUDIO MIXER, SOLO · AFV OFF")
        settle()
        mouseClick(d)
        verify(!d.expanded)
        compare(d.icon.name, "chevron-right")
        verify(findChild(d, "pbRowBadge").visible, "a closed drawer still shows its status")
    }

    function test_disclosureOwnerHoldsState() {
        const o = create(boundDiscC)
        const k = o.key
        mouseClick(k)
        verify(o.open && k.expanded)
        settle()
        mouseClick(k)
        verify(!o.open && !k.expanded)
        o.open = true
        verify(k.expanded, "the binding to the owner survives the clicks")
    }

    // ---- RailButton -----------------------------------------------------------------------------------------------------
    function test_railButton() {
        away()
        const r = create(railC, { "icon.name": "broadcast", tip: "On Air" })
        compare(r.implicitWidth, 36)
        compare(r.implicitHeight, 36)
        verify(r.stacked)
        compare(findChild(r, "pbIcon").width, BtnTokens.size("l").icon)
        verify(same(r.background.color, "transparent"), "ghost at rest")
        const fired = spy(r, "fired")
        mouseClick(r)
        compare(fired.count, 1)
        verify(!r.checked, "autoToggle is off: the owner decides `checked`")
        r.checked = true
        away()
        tryVerify(() => same(r.background.color, BtnTokens.alpha(hex("accent0"), BtnTokens.states.litA)), 1000, "checked: lit accent")
        compare(r.background.border.width, BtnTokens.states.litBorderW)

        mouseMove(r, r.width / 2, r.height / 2)
        tryVerify(() => { const t = findChild(r, "railTip"); return t !== null && t.visible }, 3000, "the rail tooltip opens on hover")
        const tip = findChild(r, "railTip")
        compare(tip.text, "On Air")
        compare(tip.x, r.width + 6, "on the right of the key")
        verify(!r.ToolTip.visible, "not PButton's tooltip above the key")
        away()
        tryVerify(() => findChild(r, "railTip") === null, 2000, "the tooltip goes when the pointer leaves")

        ignoreWarning(/RailButton "home" needs a tip/)
        create(railC, { "icon.name": "home", y: 60 })
    }

    function test_railOwnerAndChannelVariant() {
        const o = create(boundRailC)
        const k = o.key
        verify(k.checked)
        mouseClick(k)
        verify(k.checked)
        o.page = "schedule"
        verify(!k.checked, "the click did not break the owner's binding")

        const ch = create(railC, { text: "CH1", sublabel: "ON", led: "green", tip: "main", x: 200 })
        compare(ch.implicitWidth, 36)
        compare(ch.implicitHeight, 36)
        const lab = findChild(ch, "pbLabel"), sub = findChild(ch, "pbSublabel")
        verify(lab.visible && sub.visible, "the channel variant shows two lines")
        compare(lab.text, "CH1")
        compare(sub.text, "ON")
        verify(lab.width >= lab.implicitWidth - 0.01, "CH1 is not elided (" + lab.width + " of " + lab.implicitWidth + ")")
        compare(ch.padX, 2, "the rail key narrows its side padding")
        const ch10 = create(railC, { text: "CH10", sublabel: "OFF", tip: "ch10", x: 260 })
        const lab10 = findChild(ch10, "pbLabel")
        verify(lab10.width >= lab10.implicitWidth - 0.01, "CH10 is not elided either")
        compare(findChild(ch, "pbIcon"), null)
        const led = findChild(ch, "pbLed")
        verify(led !== null && same(led.color, hex("ready")))
        compare(led.height, BtnTokens.leds.barH, "the LED is the bar along the top")
        const subY = sub.mapToItem(lab.parent, 0, 0).y      // the second line sits in its Loader, in the label's Grid
        verify(lab.y + lab.height <= subY + 0.5 && subY + sub.height <= ch.height, "both lines fit in 36 px")
    }
}
