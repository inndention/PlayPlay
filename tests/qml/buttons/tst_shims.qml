import QtQuick
import QtQuick.Controls
import QtTest
import PlayPlay.Buttons
import "legacy" as Legacy
import "../../../Play_Play_Native/qml/ui" as Ui

// UXB-5: the compatibility shims (docs/plans/ux-buttons.md 3.6). Play_Play_Native/qml/ui's CButton, DeckButton and CToggle
// keep their API and their implicit sizes, now drawn by PlayPlay.Buttons. Each shim is measured against a frozen copy of the
// key it replaces (tests/qml/buttons/legacy/, the baseline files with only the Theme import changed): implicit widths within
// 2 px, heights equal, for the variants and for every glyph the call sites use today. Glyphs map to icons (icon2, a label that
// is only a glyph, a leading glyph and a space); an icon-only key falls back to the glyph map's name for its tooltip.
TestCase {
    id: tc
    name: "Shims"
    when: windowShown
    width: 900; height: 400

    Component { id: legC; Legacy.CButton { } }
    Component { id: shimC; Ui.CButton { } }
    Component { id: shimTipC; Ui.CButton { ToolTip.text: "Delete preset" } }
    Component { id: legDeckC; Legacy.DeckButton { } }
    Component { id: deckC; Ui.DeckButton { } }
    Component { id: deckTipC; Ui.DeckButton { ToolTip.text: "Hard cut"; ToolTip.visible: hovered } }
    Component { id: legToggleC; Legacy.CToggle { } }
    Component { id: toggleC; Ui.CToggle { } }
    Component { id: spyC; SignalSpy { } }
    Component {   // a toggle with today's call-site pattern: checked bound to state, onToggled writes it back
        id: boundToggleC
        Item {
            property bool setting: false
            property alias tg: tg
            width: 320; height: 40
            Ui.CToggle { id: tg; label: "Loop"; checked: parent.setting; onToggled: (v) => parent.setting = v }
        }
    }

    // The glyphs on CButton / DeckButton call sites today (icon2 values, glyph-only labels, leading glyphs), as written.
    readonly property var siteIcon2: ["✕", "✕", "↻", "↻", "↻", "⟳", "▶", "▶", "■", "■", "‖", "↗", "↗",
                                      "⎘", "▲", "▼", "⏭", "⤓", "✎", "✎", "●", "●", "✓", "‹", "›", "⚡", "⚡", "⇧",
                                      "⛶", "⬇", "+", "⚙", "✦", "⌕", "↩"]
    readonly property var siteGlyphText: ["✕", "✕", "↻", "↻", "⟳", "⏮", "⏭", "◀❘", "❘▶", "▶", "⏸", "↑", "↓", "↑",
                                          "✎", "✎", "⤙", "↩"]
    readonly property var siteLeading: ["+ Add node", "+ Add", "+ Segment (Enter)", "+ bucket", "▶ Run now", "▶ from start",
                                        "★ Prime time", "← pull", "→ CG", "＋ Capture current state", "⤓ Fetch", "⤒ Push",
                                        "⬇ Demote", "⚡ Promote"]
    readonly property var sitePlain: ["LOAD", "Save", "J", "A+", "A−", "Bars + tone", "Adherence ↗", "Fill hour ▾",
                                      "Output page ↗", "End = here ]", "Slate captions…", "CONFIRM ROLE → MAIN", ""]
    // Glyphs no map entry covers (ConfigView's tabs, ToolsView's archive key): drawn as text in the icon slot, as today.
    readonly property var siteUnmapped: ["▣", "☆", "▦", "☒"]
    readonly property var deckGlyphs: ({ "⇥": "cut", "⧉": "auto", "↩": "return", "‖": "pause", "▶": "play", "■": "stop",
                                         "⏭": "skip-next", "⏸": "pause", "↻": "refresh", "⚡": "bolt", "⤓": "load-hour",
                                         "⏰": "alarm", "●": "dot" })

    function stage() { return tc.Window.contentItem }
    function make(c, props) {
        const o = createTemporaryObject(c, stage(), Object.assign({ x: 20, y: 20 }, props || {}))
        verify(o !== null, "did not create")
        return o
    }
    function spy(target, sig) { return createTemporaryObject(spyC, tc, { target: target, signalName: sig }) }
    function same(a, b) {
        const x = Qt.tint(a, "transparent"), y = Qt.tint(b, "transparent"), e = 1 / 255 + 1e-6
        return Math.abs(x.r - y.r) <= e && Math.abs(x.g - y.g) <= e && Math.abs(x.b - y.b) <= e && Math.abs(x.a - y.a) <= e
    }
    function away() { mouseMove(stage(), stage().width - 2, stage().height - 2) }
    // Today's key and the shim with the same properties: same height, width within 2 px.
    function sizeMatches(props, what) {
        const l = make(legC, props), s = make(shimC, Object.assign({ y: 60 }, props))
        compare(s.implicitHeight, l.implicitHeight, what + ": height")
        verify(Math.abs(s.implicitWidth - l.implicitWidth) <= 2,
               what + ": shim " + s.implicitWidth.toFixed(1) + " px vs today's " + l.implicitWidth.toFixed(1) + " px")
        // the label never elides at the implicit width
        const lab = findChild(s, "pbLabel")
        if (lab !== null && lab.visible) verify(lab.width >= lab.implicitWidth - 0.01, what + ": the label elides")
        l.destroy(); s.destroy()
    }

    function init() { away() }

    // --- CButton ----------------------------------------------------------------------------------------------------------
    function test_cbuttonApi() {
        const b = make(shimC, { text: "LOAD", variant: "primary", small: true, active: false, icon2: "" })
        for (const p of ["variant", "small", "active", "icon2", "tint", "filled", "text"])
            verify(b[p] !== undefined, "CButton." + p)
        verify(b instanceof AbstractButton)
        compare(b.filled, true)
        verify(same(b.tint, BtnTokens.hex("accent0")), "primary's tint is accent0, as today")
        const c = spy(b, "clicked")
        mouseClick(b)
        compare(c.count, 1, "clicked on a click")
        // A double click: today's key gave two clicked, and so does the shim (frame stepping, A- / A+ and reorder keys are
        // tapped faster than the double-click interval). A plain PButton gives one: AbstractButton drops the second click's
        // clicked when it believes doubleClicked is connected, which a PButton's property count makes true by accident (the
        // impl log has the mechanism), so the shim connects doubleClicked for real and re-emits clicked. Separate clicks
        // each act too.
        const l = make(legC, { text: "LOAD", y: 60 }), b2 = make(shimC, { text: "LOAD", y: 100 })
        const lc = spy(l, "clicked"), c2 = spy(b2, "clicked")
        wait(Qt.styleHints.mouseDoubleClickInterval + 50)
        mouseDoubleClickSequence(l)
        wait(Qt.styleHints.mouseDoubleClickInterval + 50)
        mouseDoubleClickSequence(b2)
        compare(lc.count, 2, "today's key: two clicked on a double click")
        compare(c2.count, 2, "the shim: two clicked on a double click, as today")
        wait(Qt.styleHints.mouseDoubleClickInterval + 50)
        mouseClick(b2)
        wait(Qt.styleHints.mouseDoubleClickInterval + 50)
        mouseClick(b2)
        compare(c2.count, 4, "separate clicks each act")
        // PButton's own debounce is untouched: a double click is still one fired() for the keys that listen to it.
        const f = spy(b2, "fired")
        wait(Qt.styleHints.mouseDoubleClickInterval + 50)
        mouseDoubleClickSequence(b2)
        compare(c2.count, 6)
        compare(f.count, 1, "fired() once for a double click (the minIntervalMs debounce)")
        mouseClick(b)
        verify(b.activeFocus, "the key takes the focus on click, as today (StrongFocus)")
        const n = c.count
        b.enabled = false
        mouseClick(b)
        compare(c.count, n, "no click while disabled")
    }

    function test_cbuttonVariants() {
        const want = { "default": ["neutral", "outline", false], primary: ["primary", "solid", true], success: ["go", "solid", true],
                       danger: ["stop", "solid", true], warn: ["warn", "solid", true], info: ["info", "solid", true],
                       accent2: ["accent", "solid", true], ghost: ["neutral", "ghost", false], bogus: ["neutral", "outline", false] }
        for (const v in want) {
            const b = make(shimC, { text: "KEY", variant: v })
            compare(b.role, want[v][0], v + " role")
            compare(b.effectiveFill, want[v][1], v + " fill")
            compare(b.filled, want[v][2], v + " filled")
            b.active = true
            compare(b.effectiveFill, "tonal", v + " active is lit (tonal)")
            b.destroy()
        }
        // today's colours where they were tokens: danger is onAir, success ready, warn standby, info #0a84ff, the ghost key clear
        const d = make(shimC, { text: "X", variant: "danger" })
        verify(same(d.restLook.bg, BtnTokens.hex("onAir")))
        const g = make(shimC, { text: "X", variant: "ghost", y: 60 })
        compare(BtnTokens.col(g.restLook.bg).a, 0)
        compare(g.restLook.borderW, 0)
        const o = make(shimC, { text: "X", y: 100 })
        verify(same(o.restLook.bg, BtnTokens.hex("bg3")), "the default key sits on bg3")
        o.enabled = false
        verify(same(o.restLook.text, BtnTokens.hex("text3")), "disabled text3")
        compare(make(shimC, { text: "X", small: true }).size, "s")
        compare(make(shimC, { text: "X" }).size, "m")
    }

    function test_cbuttonSizesPlain() {
        for (const v of ["default", "primary", "ghost", "danger"])
            for (const small of [true, false])
                for (const t of sitePlain)
                    sizeMatches({ text: t, variant: v, small: small }, v + (small ? " small " : " ") + JSON.stringify(t))
        sizeMatches({ text: "ON", active: true, variant: "ghost", small: true }, "active")
        sizeMatches({ text: "OFF", enabled: false }, "disabled")
    }

    function test_cbuttonSizesIcon2() {
        for (const small of [true, false])
            for (const g of siteIcon2.concat(siteUnmapped)) {
                sizeMatches({ icon2: g, small: small, variant: "ghost" }, "icon2 " + g + (small ? " small" : ""))
                sizeMatches({ icon2: g, text: "Run now", small: small }, "icon2 " + g + " + text" + (small ? " small" : ""))
            }
    }

    function test_cbuttonSizesGlyphText() {
        for (const small of [true, false]) {
            for (const g of siteGlyphText) sizeMatches({ text: g, small: small, variant: "ghost" }, "text " + g + (small ? " small" : ""))
            for (const t of siteLeading) sizeMatches({ text: t, small: small, variant: "primary" }, "text " + t + (small ? " small" : ""))
        }
    }

    function test_cbuttonGlyphMapping() {
        failOnWarning(/.+/)
        // icon2 through the glyph map, escapes and literals alike
        const cases = { "✕": "close", "✕": "close", "↻": "refresh", "↻": "refresh", "⟳": "refresh", "▶": "play",
                        "▶": "play", "■": "stop", "‖": "pause", "↗": "external", "⎘": "copy", "▲": "chevron-up",
                        "⏭": "skip-next", "⤓": "load-hour", "✎": "edit", "●": "dot", "✓": "check", "‹": "chevron-left",
                        "⚡": "bolt", "⚡": "bolt", "⇧": "upload", "⛶": "fullscreen", "⬇": "download", "+": "plus",
                        "⚙": "settings", "✦": "spark", "⌕": "search", "↩": "return" }
        for (const g in cases) {
            const b = make(shimC, { icon2: g, text: "Label" })
            compare(b.icon.name, cases[g], "icon2 " + g)
            compare(b.displayText, "Label")
            verify(findChild(b, "pbIcon") !== null, g + ": the icon is drawn")
            compare(findChild(b, "cbGlyphFace"), null)
            b.destroy()
        }
        for (const g of siteIcon2) verify(BtnTokens.glyph(g) !== null, "icon2 " + g + " has a map entry")
        for (const g of siteGlyphText) verify(BtnTokens.glyph(g) !== null, "glyph label " + g + " has a map entry")
        // a glyph the map does not know is still drawn, as text in the icon slot, with no warning
        for (const g of siteUnmapped) {
            verify(BtnTokens.glyph(g) === null, g + " is unmapped")
            const u = make(shimC, { icon2: g, text: "Channels" })
            const face = findChild(u, "cbGlyphFace")
            verify(face !== null, g + ": the glyph face")
            compare(face.children[0].text, g)
            compare(findChild(u, "pbIcon"), null)
            u.destroy()
        }
        // the icon follows a changing glyph (col.engineState === "ON_AIR" ? "‖" : "▶")
        const p = make(shimC, { icon2: "▶", text: "PLAY" })
        compare(p.icon.name, "play")
        p.icon2 = "‖"
        compare(p.icon.name, "pause")
        p.icon2 = "☒"
        verify(findChild(p, "cbGlyphFace") !== null)
        p.icon2 = ""
        compare(p.icon.name, "")
        compare(findChild(p, "pbIconSlot").active, false)
    }

    function test_cbuttonLeadingGlyphSplit() {
        failOnWarning(/.+/)
        const want = [["▶ Run now", "play", "Run now"], ["+ Add node", "plus", "Add node"], ["★ Prime time", "star", "Prime time"],
                      ["← pull", "chevron-left", "pull"], ["→ CG", "chevron-right", "CG"], ["＋ Capture current state", "plus", "Capture current state"],
                      ["⤓ Fetch", "load-hour", "Fetch"], ["◀❘ back", "frame-back", "back"], ["⚡️ Promote", "bolt", "Promote"]]
        for (const w of want) {
            const b = make(shimC, { text: w[0], small: true })
            compare(b.icon.name, w[1], w[0])
            compare(b.displayText, w[2], w[0] + " label")
            compare(b.text, w[0], "text keeps what the call site set")
            compare(findChild(b, "pbLabel").text, w[2])
            compare(b.Accessible.name, w[2])
            compare(b.ToolTip.text, "", "a key with a label needs no fallback tip")
            b.destroy()
        }
        // not split: no space after the glyph, a glyph inside or at the end, ASCII
        for (const t of ["A+", "+Add", "Bars + tone", "Adherence ↗", "Fill hour ▾", "LOAD", "End = here ]"]) {
            const b = make(shimC, { text: t })
            compare(b.icon.name, "", t)
            compare(b.displayText, t, t)
            b.destroy()
        }
        // with icon2 the label stays as written ("+ Live" next to a ● icon2)
        const both = make(shimC, { text: "+ Live", icon2: "●", small: true })
        compare(both.icon.name, "dot")
        compare(both.displayText, "+ Live")
        // a label built at run time is split when it starts with a known glyph ("+ " + model name)
        const dyn = make(shimC, { text: "", small: true })
        dyn.text = "+ " + "DTA2154"
        compare(dyn.icon.name, "plus")
        compare(dyn.displayText, "DTA2154")
        dyn.text = "Stop"
        compare(dyn.icon.name, "")
        compare(dyn.displayText, "Stop")
    }

    function test_cbuttonIconOnlyTooltip() {
        const x = make(shimC, { text: "✕", small: true, variant: "ghost" })
        compare(x.icon.name, "close")
        compare(x.displayText, "")
        compare(findChild(x, "pbLabel").visible, false)
        compare(x.ToolTip.text, "Remove", "the glyph map's name when the call site has no tooltip")
        compare(x.Accessible.name, "Remove")
        compare(x.ToolTip.visible, false)
        mouseMove(x, x.width / 2, x.height / 2)
        tryCompare(x, "hovered", true)
        tryCompare(x.ToolTip, "visible", true)      // hovering shows the fallback tip
        away()
        const r = make(shimC, { icon2: "↻", small: true, y: 60 })
        compare(r.ToolTip.text, "Refresh")
        // the call site's own tooltip wins
        const own = make(shimTipC, { icon2: "✕", small: true, y: 100 })
        compare(own.ToolTip.text, "Delete preset")
        compare(own.Accessible.name, "Delete preset")
        // a key with a label has no fallback; an unmapped glyph has none either
        compare(make(shimC, { icon2: "✕", text: "Remove all", y: 140 }).ToolTip.text, "")
        compare(make(shimC, { icon2: "☒", small: true, y: 180 }).ToolTip.text, "")
        // a plain key shows no tooltip at all, like today
        const plain = make(shimC, { text: "LOAD", y: 220 })
        mouseMove(plain, 5, 5)
        tryCompare(plain, "hovered", true)
        compare(plain.ToolTip.visible, false)
    }

    function test_cbuttonCallSiteLayouts() {
        // call sites that size the key themselves keep working: height 30 on a small key, a fixed implicitWidth, fillWidth
        const h = make(shimC, { text: "+ Live", small: true, height: 30 })
        compare(h.height, 30)
        const w = make(shimC, { text: "◀❘", small: true, implicitWidth: 40, y: 60 })
        compare(w.width, 40)
        const n = make(shimC, { text: "LONG LABEL HERE", small: true, width: 50, y: 100 })
        verify(findChild(n, "pbLabel").width <= 50, "narrower than its label: elides inside the key")
    }

    // --- DeckButton -------------------------------------------------------------------------------------------------------
    function test_deckApiAndSize() {
        const d = make(deckC, { glyph: "⇥", text: "CUT", variant: "danger" })
        for (const p of ["glyph", "variant", "active", "text", "tint"]) verify(d[p] !== undefined, "DeckButton." + p)
        compare(d.implicitWidth, 74)
        compare(d.implicitHeight, 52)
        const l = make(legDeckC, { glyph: "⇥", text: "CUT", variant: "danger", x: 120 })
        compare(d.implicitWidth, l.implicitWidth)
        compare(d.implicitHeight, l.implicitHeight)
        const s = make(deckC, { glyph: "⧉", text: "AUTO MIX", implicitHeight: 46, y: 100 })
        compare(s.height, 46, "a call site's own height (CUT / AUTO / SCHEDULE are 46)")
        compare(d.size, "l")
        compare(d.stacked, true)
        const c = spy(d, "clicked")
        mouseClick(d)
        compare(c.count, 1)
        verify(d.activeFocus, "takes the focus on click, as today")
        wait(Qt.styleHints.mouseDoubleClickInterval + 50)
        mouseDoubleClickSequence(d)
        compare(c.count, 3, "a double tap (NEXT, SNAP) acts twice, as today")
        d.enabled = false
        mouseClick(d)
        compare(c.count, 3)
    }

    function test_deckGlyphsAndColours() {
        failOnWarning(/.+/)
        for (const g in deckGlyphs) {
            const d = make(deckC, { glyph: g, text: "KEY" })
            compare(d.icon.name, deckGlyphs[g], "deck glyph " + g)
            verify(findChild(d, "pbIcon") !== null)
            d.destroy()
        }
        const roles = { success: "ready", danger: "onAir", warn: "standby", info: "info", primary: "accent0" }
        for (const v in roles) {
            const d = make(deckC, { glyph: "▶", text: "PLAY", variant: v })
            compare(d.role, "neutral", v + ": a neutral key, as today")
            verify(same(d.icon.color, BtnTokens.hex(roles[v])), v + ": the icon in the variant colour")
            verify(same(findChild(d, "pbIcon").color, BtnTokens.hex(roles[v])))
            compare(d.effectiveFill, "outline")
            d.active = true
            compare(d.effectiveFill, "tonal", v + " lit")
            verify(same(d.tint, BtnTokens.hex(roles[v])), v + ": lit in the variant colour")
            verify(same(d.restLook.border, BtnTokens.hex(roles[v])))
            compare(d.restLook.borderW, 2, "lit: a 2 px border, as today")
            fuzzyCompare(d.restLook.bg.a, 0.30, 0.01, "lit: a 30 % tint, as today")
            d.destroy()
        }
        const n = make(deckC, { glyph: "■", text: "STOP" })
        compare(n.icon.color.a, 0, "default: the icon in the key's own ink")
        n.enabled = false
        verify(same(findChild(n, "pbIcon").color, BtnTokens.hex("text3")), "disabled icon text3")
        verify(same(n.restLook.bg, BtnTokens.hex("bg2")), "disabled bg2, as today")
        // a changing glyph, and one the map does not know
        const p = make(deckC, { glyph: "▶", text: "PLAY", y: 100 })
        p.glyph = "‖"
        compare(p.icon.name, "pause")
        p.glyph = "▣"
        verify(findChild(p, "dbGlyphFace") !== null)
    }

    function test_deckTooltip() {
        const d = make(deckTipC, { glyph: "⇥", text: "CUT" })
        compare(d.ToolTip.text, "Hard cut")
        compare(d.safety, "none")
        compare(d.Accessible.name, "CUT")
        mouseMove(d, 10, 10)
        tryCompare(d, "hovered", true)
        tryCompare(d.ToolTip, "visible", true)
    }

    // --- CToggle ----------------------------------------------------------------------------------------------------------
    function test_toggleApiAndSize() {
        const t = make(toggleC, { label: "Loop playlist" })
        for (const p of ["label", "hint", "checked", "enabled2"]) verify(t[p] !== undefined, "CToggle." + p)
        const l = make(legToggleC, { label: "Loop playlist", y: 60 })
        compare(t.implicitWidth, l.implicitWidth)
        compare(t.implicitHeight, l.implicitHeight)
        t.hint = "Repeat at the end"; l.hint = "Repeat at the end"
        compare(t.implicitHeight, 44)
        compare(t.implicitHeight, l.implicitHeight)
        compare(t.Accessible.role, Accessible.CheckBox)
        compare(t.Accessible.name, "Loop playlist")
        compare(t.Accessible.checked, false)
    }

    function test_toggleClickAndEnabled() {
        const t = make(toggleC, { label: "Auto-advance" })
        const s = spy(t, "toggled")
        mouseClick(t, 18, t.height / 2)
        compare(t.checked, true)
        compare(s.count, 1)
        compare(s.signalArguments[0][0], true, "toggled(bool) carries the new state")
        compare(t.Accessible.checked, true)
        verify(!t.activeFocus, "a click takes no focus, as today")
        mouseClick(t, 150, t.height / 2)
        compare(t.checked, false, "the label toggles too")
        // the real enabled, and enabled2 as its alias
        t.enabled = false
        compare(t.enabled2, false)
        mouseClick(t, 18, t.height / 2)
        compare(t.checked, false, "no toggle while disabled")
        compare(s.count, 2)
        verify(same(findChild(t, "ctLabel").color, BtnTokens.hex("text3")), "disabled label text3")
        compare(findChild(t, "ctPill").opacity, 1, "disabled is drawn in tokens, not opacity")
        t.enabled2 = true
        compare(t.enabled, true)
        mouseClick(t, 18, t.height / 2)
        compare(t.checked, true)
        t.enabled2 = false
        compare(t.enabled, false)
        // today's call-site pattern keeps working
        const host = make(boundToggleC, { y: 100 })
        mouseClick(host.tg, 18, host.tg.height / 2)
        compare(host.setting, true, "onToggled writes the owner's state")
        compare(host.tg.checked, true)
    }

    function test_toggleHoverFocusKeys() {
        const t = make(toggleC, { label: "Loop" })
        const pill = findChild(t, "ctPill")
        const rest = Qt.tint(pill.border.color, "transparent")
        mouseMove(t, 18, t.height / 2)
        tryCompare(t, "hovered", true)
        verify(!same(pill.border.color, rest), "hover brightens the pill")
        away()
        tryCompare(t, "hovered", false)
        // keyboard: Tab reaches it, the ring shows, Space and Enter toggle
        compare(t.focusPolicy, Qt.TabFocus, "Tab reaches it; a click does not take the focus")
        t.forceActiveFocus(Qt.TabFocusReason)
        verify(t.activeFocus)
        verify(t.visualFocus)
        verify(findChild(t, "ctFocusRing").visible, "the focus ring on keyboard focus")
        const s = spy(t, "toggled")
        keyClick(Qt.Key_Space)
        compare(t.checked, true, "Space toggles")
        keyClick(Qt.Key_Return)
        compare(t.checked, false, "Enter toggles")
        keyClick(Qt.Key_Enter)
        compare(t.checked, true)
        compare(s.count, 3)
        t.enabled = false
        compare(findChild(t, "ctFocusRing").visible, false)
    }

    Component { id: spaceShortcutC; Shortcut { property int hits: 0; sequence: "Space"; context: Qt.ApplicationShortcut; onActivated: hits++ } }

    function test_toggleSpaceBeatsAppShortcut() {
        // main.qml binds Space application-wide to play / pause. A focused toggle claims Space in Keys.onShortcutOverride, so
        // it toggles and the shortcut stays quiet; a disabled toggle, or the focus anywhere else, leaves Space to the shortcut.
        const sc = createTemporaryObject(spaceShortcutC, tc)
        const t = make(toggleC, { label: "Loop" })
        t.forceActiveFocus(Qt.TabFocusReason)
        verify(t.activeFocus)
        keyClick(Qt.Key_Space)
        compare(t.checked, true, "Space toggles the focused toggle")
        compare(sc.hits, 0, "the application-wide Space shortcut did not fire")
        keyClick(Qt.Key_Space)
        compare(t.checked, false)
        compare(sc.hits, 0)
        t.enabled = false
        keyClick(Qt.Key_Space)
        compare(t.checked, false, "a disabled toggle does not toggle")
        compare(sc.hits, 1, "... and leaves Space to the shortcut")
        t.enabled = true
        t.focus = false
        verify(!t.activeFocus)
        keyClick(Qt.Key_Space)
        compare(t.checked, false, "with the focus elsewhere the toggle is not touched")
        compare(sc.hits, 2, "... and the shortcut fires as before")
    }
}
