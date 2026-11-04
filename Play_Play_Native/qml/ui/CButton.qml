pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import PlayPlay.Buttons

/// Pill button in the PlayPlayClean style. variant: default|primary|danger|success|warn|info|accent2|ghost
// UXB-5 (docs/plans/ux-buttons.md 3.6): a shim over PlayPlay.Buttons' PButton, so all 436 call sites take the token look and
// the in-house icons with no edit. The API is today's: variant, small, active, icon2, tint / filled (read-only, derived),
// text, ToolTip.text, clicked.
// - variant -> role / fill through buttons.json legacyVariants (default -> neutral outline on bg3, primary -> primary,
//   success -> go, danger -> stop, warn -> warn, info -> info, accent2 -> accent, ghost -> neutral ghost); small -> size s.
// - A glyph becomes an icon through the glyph map: icon2 ("✕"), a label that is only a glyph ("↻"), or a label that starts
//   with a glyph and a space ("▶ Run now" -> play + "Run now"; `text` keeps the whole string). A glyph the map does not
//   know is still drawn as text in the icon slot. An icon-only key with no ToolTip.text of its own shows the map's name
//   ("Remove", "Refresh") as its tooltip and accessible name.
// - Today's implicit sizes are kept: 24 / 30 high, and as wide as the old glyph and label laid out plus 16 / 22, so no layout
//   reflows (tst_shims holds every case to 2 px against a frozen copy of the old key). The side padding shrinks to fit the
//   icon where the old glyph was narrower.
// - Kept from today on purpose: the key takes the focus on click (StrongFocus; a text field next to it still finishes
//   editing when it is clicked), and `clicked` fires on every click, the second click of a double click included (call
//   sites use onClicked; frame stepping, A- / A+ and reorder keys are tapped faster than the double-click interval).
//   PButton's debounced `fired()` is there for the ones that move to it.
PButton {
    id: b

    property string variant: "default"
    property string icon2: ""
    property bool small: false
    // `active` and `tint` are PButton's: active lights the key (tonal with a 2 px border); tint is the role colour.
    readonly property bool filled: b.effectiveFill === "solid"

    readonly property var _vm: BtnTokens.legacyVariants[b.variant] !== undefined ? BtnTokens.legacyVariants[b.variant]
                                                                                    : BtnTokens.legacyVariants["default"]
    // The key's glyph in one object, so the icon, its face and the label always change together: {glyph, icon ("" when the map
    // does not know it), tip, rest (the label drawn), split}, or null when there is no glyph. icon2 wins; otherwise a label
    // that is a glyph or starts with one and a space (one icon slot: with icon2 the label stays as written).
    readonly property var _g: {
        if (b.icon2 !== "") {
            const m = BtnTokens.glyph(b.icon2)
            return { glyph: b.icon2, icon: m !== null ? m.icon : "", tip: m !== null ? m.tip : "", rest: b.text, split: false }
        }
        const s = BtnTokens.splitGlyph(b.text)
        return s === null ? null : { glyph: s.glyph, icon: s.icon, tip: s.tip, rest: s.rest, split: true }
    }
    readonly property string _glyph: b._g !== null ? b._g.glyph : ""
    readonly property bool _iconOnly: b._g !== null && b._g.rest === ""
    readonly property real _glyphPx: b.small ? 11 : 13          // today's glyph Text: Theme.fs11 / fs13, application font
    readonly property real _labelPx: b.small ? 11 : 12         // today's label: Theme.fs11 / fs12 bold, tracking 0.4
    // Today's width: Row { glyph Text, label Text; spacing 6 } + 16 / 22.
    readonly property real _legacyW: {
        const g = b.icon2 !== "" ? BtnTokens.textWidth(b.icon2, b._glyphPx, false, 0, "") : 0
        const l = b.text === "" ? 0 : b._g !== null && b._g.split ? BtnTokens.textWidth(b.text, b._labelPx, true, 0.4, BtnTokens.fontDisplay)
                                                                  : b.labelWidth
        return (b.small ? 16 : 22) + g + l + (g > 0 && l > 0 ? 6 : 0)
    }
    readonly property real _contentW: (b._g !== null ? b.tokens.icon : 0) + (b._g !== null && b._g.rest !== "" ? b.tokens.gap : 0)
                                      + (b.displayText !== "" ? b.labelWidth : 0)

    role: b._vm.role
    fill: b._vm.fill
    size: b.small ? "s" : "m"
    displayText: b._g !== null ? b._g.rest : b.text
    // An unknown glyph keeps a real icon name ("dot") so the slot exists; the glyph face is drawn in it instead of the icon.
    icon.name: b._g === null ? "" : b._g.icon !== "" ? b._g.icon : "dot"
    iconFace: b._g !== null && b._g.icon === "" ? glyphFace : null
    focusPolicy: Qt.StrongFocus
    implicitWidth: Math.max(b._legacyW, b._contentW + 4)
    // Never wider than the token padding; never so tight that the label elides (floor: the leftover is not split in halves).
    padX: Math.max(2, Math.min(b.tokens.padX, Math.floor((b.implicitWidth - b._contentW) / 2)))

    ToolTip.text: b._iconOnly ? b._g.tip : ""
    ToolTip.visible: b.hovered && b.ToolTip.text.length > 0
    ToolTip.delay: BtnTokens.timing.tipDelayMs
    Accessible.name: b.displayText !== "" ? b.displayText : b.ToolTip.text

    // The second click of a double click must still reach onClicked, as on today's key. AbstractButton drops that click's
    // `clicked` when it believes `doubleClicked` is connected, and that test (QQmlData::signalHasEndpoint) is a 64-bit mask
    // over signal index mod 64: a PButton has enough properties that a binding on one of them trips the bit doubleClicked
    // shares, so a plain PButton loses the click (one property out of 70 does it to a bare AbstractButton). Connecting
    // doubleClicked for real makes the drop certain, and re-emitting clicked here puts the click back; PButton's fired()
    // debounce still makes a double click one take for the keys that listen to fired().
    onDoubleClicked: b.clicked()

    Component {
        id: glyphFace
        Item {
            objectName: "cbGlyphFace"
            Text {
                anchors.centerIn: parent
                text: b._glyph
                color: b.look.icon
                font.pixelSize: b._glyphPx
            }
        }
    }
}
