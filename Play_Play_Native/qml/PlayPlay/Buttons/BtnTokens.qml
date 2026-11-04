pragma Singleton
import QtQuick
import "spec.js" as Spec

// BtnTokens: the button tokens of assets/ui/buttons.json (through the generated spec.js) plus the colour rules every component
// of PlayPlay.Buttons shares (docs/plans/ux-buttons.md section 3.2). Theme colours come from Spec.theme, whose values
// test_uispec pins to Theme.qml and web/style.css, so the module needs nothing outside qrc:/qml/PlayPlay/Buttons/ (Qt Quick
// Test loads it alone). No supervisor / api / window access here.
QtObject {
    id: tok

    readonly property var sizes: Spec.sizes
    readonly property var sizeOrder: Spec.sizeOrder
    readonly property var roles: Spec.roles
    readonly property var roleOrder: Spec.roleOrder
    readonly property var fills: Spec.fills
    readonly property var fillOrder: Spec.fillOrder
    readonly property var autoFill: Spec.autoFill
    readonly property var states: Spec.states
    readonly property var timing: Spec.timing
    readonly property var leds: Spec.leds
    readonly property var theme: Spec.theme
    readonly property var icons: Spec.icons
    readonly property var safetyLevels: Spec.safetyLevels
    readonly property var safetyDefaults: Spec.safetyDefaults
    readonly property var glyphs: Spec.glyphs
    readonly property var reservedKeys: Spec.reservedKeys
    readonly property var legacyVariants: Spec.legacyVariants

    readonly property color info: hex("info")          // the one new colour token (#0a84ff), replacing 3 hard-coded copies
    readonly property color onFill: hex(Spec.states.onFill)
    readonly property string fontDisplay: "Segoe UI Semibold"   // = Theme.fontDisplay (tst_pbutton compares them)

    // Icon renderer for every Icon that does not set its own: "curve" (Shape.CurveRenderer, sharp at every uiScale zoom)
    // or "geometry" (GeometryRenderer through a 4x multisampled layer), the fallback for a GPU where curves misbehave.
    property string iconRenderer: "curve"

    // One pending confirmation in the application, as the remote pad's armedKey: a key that arms emits this, and every other
    // armed key disarms itself; an open ConfirmPopup question is cancelled (ConfirmPopup.ask() emits it too).
    signal arming(Item item)
    // The key armed now, null when none (PButton sets and clears it; a destroyed key reads null). Another shortcut on Esc,
    // such as On Air's "clear preview" in main.qml, should add `&& !BtnTokens.armedItem && !BtnTokens.confirmOpen` to its
    // `enabled`: Qt gives a key that two enabled shortcuts share to one of them in turn (activatedAmbiguously), so the first
    // Esc would miss the armed key.
    property Item armedItem: null
    // The ConfirmPopup whose question is open, null when none (ConfirmPopup sets and clears it; one question in the
    // application, so one popup; a destroyed popup reads null). A modal popup does not block Qt.ApplicationShortcut shortcuts
    // (all of main.qml's On Air keys): ConfirmPopup takes Esc / Enter itself while the keyboard is on it, and an application
    // shortcut that should not act during a question (Esc, a user hotkey, TAKE / CUT) adds `&& !BtnTokens.confirmOpen`.
    property QtObject confirming: null
    readonly property bool confirmOpen: confirming !== null

    // Window -> ConfirmPopup, created lazily on the first `safety: "modal"` press in that window (UXB-3b ships ConfirmPopup).
    property var _popups: []
    property var _warned: ({})

    // A Theme colour by name (bg3, onAir, accent0 ...); "" and unknown names give transparent.
    function hex(name) {
        const t = Spec.theme[name]
        return t !== undefined ? t.hex : "transparent"
    }
    function col(c) { return Qt.tint(c, "transparent") }   // any colour value or string as an RGB color (exact, unlike lighter(c, 1))
    function alpha(c, a) { const k = col(c); return Qt.rgba(k.r, k.g, k.b, a) }
    function hover(c) { return Qt.lighter(c, Spec.states.hoverK) }
    function pressed(c) { return Qt.darker(c, Spec.states.pressK) }

    function size(name) { return Spec.sizes[name] !== undefined ? Spec.sizes[name] : Spec.sizes.m }
    function isStacked(name) { return size(name).layout === "stacked" }
    function roleColor(role) {
        const r = Spec.roles[role] !== undefined ? Spec.roles[role] : Spec.roles.neutral
        return hex(r.color)
    }
    // The tint of a button: the user's colour when one is set (alpha > 0), else its role colour.
    function tint(role, custom) {
        const c = col(custom)
        return c.a > 0 ? c : col(roleColor(role))
    }
    function ledColor(led) {
        const n = Spec.leds.colors[led]
        return n !== undefined ? hex(n) : "transparent"
    }

    // fill "auto" keeps today's CButton / DeckButton mapping: lit (active or checked) -> tonal, ghost -> ghost, neutral ->
    // outline (on bg3, see restBg), coloured roles -> solid. An explicit fill is kept, except that a lit button is tonal.
    function resolveFill(role, fill, lit) {
        if (lit) return Spec.autoFill.active
        if (fill === "solid" || fill === "tonal" || fill === "outline" || fill === "ghost") return fill
        return role === "neutral" || Spec.roles[role] === undefined ? Spec.autoFill.neutral : Spec.autoFill.coloured
    }
    // Resting background of an outline / ghost button: auto neutral sits on bg3 (today's default CButton), others are clear.
    function restBg(role, fill) {
        return fill === "auto" && (role === "neutral" || Spec.roles[role] === undefined) ? hex(Spec.autoFill.neutralBg) : "transparent"
    }

    // WCAG relative luminance (0 black .. 1 white) and contrast ratio (1 .. 21) of colours.
    function luminance(c) {
        const k = col(c), lin = v => v <= 0.04045 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4)
        return 0.2126 * lin(k.r) + 0.7152 * lin(k.g) + 0.0722 * lin(k.b)
    }
    function contrast(a, b) {
        const x = luminance(a), y = luminance(b)
        return (Math.max(x, y) + 0.05) / (Math.min(x, y) + 0.05)
    }
    // The ink for text on a fill of any colour (a user's or a pad theme's): the light text0 while it keeps a 3:1 contrast
    // (bold labels; white on saturated colours as the remote pad has always drawn), else the dark onFill, which then reads
    // better (amber, light greys, the daylight theme).
    function inkOn(c) {
        const light = col(hex("text0"))
        return contrast(c, light) >= 3 ? light : onFill
    }

    // The defaults of a station action (buttons.json safetyDefaults, design 3.5) for the native app: {id, label, icon, role,
    // tip, hotkey, safety, floor}; `on === false` takes a stateful action's "off" entry (automation.arm disarmed,
    // engine.toggle off, transport.playpause paused). null for an unknown id.
    function actionDefaults(id, on) {
        const a = Spec.safetyDefaults[id]
        if (a === undefined) return null
        const r = { id: id, label: a.label, icon: a.icon, role: a.role, tip: a.tip, hotkey: a.hotkey !== undefined ? a.hotkey : "",
                    safety: a.native, floor: a.floor }
        const off = on === false ? a.off : undefined
        if (off !== undefined)
            for (const k of ["label", "icon", "role", "tip", "hotkey", "floor"]) if (off[k] !== undefined) r[k] = off[k]
        if (off !== undefined && off.native !== undefined) r.safety = off.native
        if (Spec.safetyLevels.indexOf(r.safety) < 0) r.safety = "none"
        return r
    }

    // The colours of one button state. s: {role, fill, lit, tint, enabled, hovered, pressed, large, ink}. Returns {bg, border,
    // borderW, text, icon}: the section 3.2 state layers (disabled is never opacity: it would look muddy over pictures).
    // ink (optional) is the text / icon colour on a solid fill instead of onFill (a transparent ink means onFill).
    function look(s) {
        const st = Spec.states
        const f = resolveFill(s.role, s.fill, s.lit)
        const neutral = s.role === "neutral" || Spec.roles[s.role] === undefined
        const r = { bg: "transparent", border: "transparent", borderW: 0, text: hex(Spec.fills[f].text), icon: hex(Spec.fills[f].icon) }
        if (!s.enabled) {
            r.bg = hex(s.large ? st.disabledBgLarge : st.disabledBg)
            r.border = hex(st.disabledBorder); r.borderW = f === "ghost" ? 0 : 1
            r.text = r.icon = hex(st.disabledText)
            return r
        }
        if (s.lit) {    // tonal at litA with a litBorderW tint border; hover and press deepen the tint
            r.bg = alpha(s.tint, st.litA + (s.pressed ? 0.14 : s.hovered ? 0.07 : 0))
            r.border = s.tint; r.borderW = st.litBorderW
            r.text = r.icon = s.tint
            return r
        }
        switch (f) {
        case "solid":
            r.bg = s.pressed ? pressed(s.tint) : s.hovered ? hover(s.tint) : s.tint
            r.text = r.icon = s.ink !== undefined && col(s.ink).a > 0 ? col(s.ink) : onFill
            break
        case "tonal":
            r.bg = alpha(s.tint, Spec.fills.tonal.bgA + (s.pressed ? 0.14 : s.hovered ? 0.07 : 0))
            r.border = s.tint; r.borderW = 1
            r.text = r.icon = s.tint
            break
        default:        // outline, ghost
            r.bg = s.pressed ? hex(st.pressBg) : s.hovered ? hex(st.hoverBg) : restBg(s.role, s.fill)
            if (f === "outline") { r.border = hex(s.hovered ? st.hoverBorder : Spec.fills.outline.border); r.borderW = 1 }
            if (!neutral) r.icon = s.tint      // a coloured role still shows in the icon of an outline / ghost key
            break
        }
        return r
    }

    // --- the legacy glyphs (UXB-5 shims: CButton / DeckButton, design 3.6) -------------------------------------------------
    // The glyph map's entry {icon, tip} for a text glyph ("✕", "↻", "◀❘"; U+FE0F dropped), null when it has none.
    function glyph(s) {
        if (s === undefined || s === null || s === "") return null
        const g = Spec.glyphs[String(s).replace(/️/g, "")]
        return g !== undefined ? g : null
    }
    // Longest first, so "◀❘" wins over a one-character key that starts it.
    readonly property var _glyphKeys: Object.keys(Spec.glyphs).sort((a, x) => x.length - a.length)
    // A label that is only a known glyph ("✕") or starts with one and a space ("▶ Run now", "+ Add node", "★ " + name):
    // {glyph, icon, tip, rest} with rest the label after the glyph ("" for a glyph alone); null otherwise ("A+", "Bars + tone",
    // "Adherence ↗": a trailing glyph stays text). Plain ASCII labels other than "+" return at once.
    function splitGlyph(text) {
        if (text === undefined || text === null || text === "") return null
        const t = String(text).replace(/️/g, "")
        const c = t.charCodeAt(0)
        if (c < 0x80 && t[0] !== "+") return null
        for (let i = 0; i < _glyphKeys.length; ++i) {
            const k = _glyphKeys[i]
            if (t === k || t.startsWith(k + " ")) {
                const g = Spec.glyphs[k]
                return { glyph: k, icon: g.icon, tip: g.tip, rest: t === k ? "" : t.slice(k.length + 1) }
            }
        }
        return null
    }
    // The advance width of `str` in this font (family "" = the application font), as a one-line Text lays it out, so a shim
    // keeps a legacy key's width. One shared FontMetrics, read through its advanceWidth() method: a method call makes no
    // binding dependency (reading a hidden Text's implicitWidth would tie every caller's binding to the next measurement, a
    // binding loop). Cached per string and font in a plain JS object (its count in the same object, not in a property: a
    // property read and written here would loop the caller's binding too); cleared at 4000 entries.
    readonly property FontMetrics _fm: FontMetrics { }
    readonly property var _widths: ({ map: {}, n: 0 })
    function textWidth(str, px, bold, spacing, family) {
        if (str === undefined || str === null || str === "") return 0
        const fam = family !== undefined && family !== "" ? family : Qt.application.font.family
        const key = fam + "|" + px + "|" + (bold ? 1 : 0) + "|" + spacing + "|" + str
        const c = _widths
        const hit = c.map[key]
        if (hit !== undefined) return hit
        _fm.font = Qt.font({ family: fam, pixelSize: px, bold: !!bold, letterSpacing: spacing })
        const w = _fm.advanceWidth(str)
        if (c.n >= 4000) { c.map = {}; c.n = 0 }
        c.map[key] = w
        ++c.n
        return w
    }

    function warnOnce(key, msg) {
        if (_warned[key]) return
        _warned[key] = true
        console.warn("PlayPlay.Buttons: " + msg)
    }

    // The ConfirmPopup of the window whose overlay is `ov` (the caller's Overlay.overlay), created on first use and parented
    // to that overlay, so it goes away with its window; null when it cannot be made: a modal button then falls back to arm,
    // which still confirms. A destroyed popup's wrapper stays truthy in JS but its properties (objectName too) read undefined,
    // hence _live.
    function _live(e) { return !!e.popup && e.popup.objectName !== undefined }
    function confirmPopup(ov) {
        if (!ov) return null
        if (_popups.some(e => !tok._live(e))) _popups = _popups.filter(e => tok._live(e))    // windows that went away
        for (let i = 0; i < _popups.length; ++i)
            if (_popups[i].overlay === ov && _live(_popups[i])) return _popups[i].popup
        const c = Qt.createComponent(Qt.resolvedUrl("ConfirmPopup.qml"))
        if (c.status !== Component.Ready) { warnOnce("confirmPopup", "ConfirmPopup.qml: " + c.errorString()); return null }
        const p = c.createObject(ov)
        if (!p) return null
        _popups = _popups.concat([{ overlay: ov, popup: p }])
        return p
    }
}
