import QtQuick

// PadButton: one key of the remote pad (docs/plans/ux-buttons.md 3.4, 3.8 "Remote pad extension"): a BigActionButton filled
// in the pad's colour, with a second line, an edit-mode selection frame and the arm confirmation built in. It replaces
// RemotePad's two-tap armedKey / "CONFIRM?" code.
// - fillColor: the pad theme's role colour or the button's own colour (solid; transparent = the role's). The ink is light
//   or dark by contrast (BtnTokens.inkOn), so a light theme colour keeps a readable label; textColor overrides it.
// - sub: the second line (node · channel · hotkey) on l / xl.
// - confirm: arm before firing (padSafety "arm"; set padSafety for hold / modal). Pressed in editMode a key never arms or
//   acts: it emits picked() for the editor and shows `selected` with a frame in selectColor.
// - triggered() is the press to act on (outside edit mode, after the safety and the debounce); fired() follows every
//   press in both modes, so act on triggered().
// - size m | l | xl (buttons.json limits.padSizes; xl by default, today's 58 px pad key) and span 1 | 2: a span-2 key is two
//   units plus the gap, so a pad grid stays aligned (unitWidth 112, the pad key today; spanGap the pad's Flow spacing).
BigActionButton {
    id: pk

    property color fillColor: "transparent"
    property color textColor: BtnTokens.inkOn(pk.tint)
    property alias sub: pk.sublabel
    property bool confirm: false
    property string padSafety: pk.confirm ? "arm" : "none"
    property bool editMode: false
    property bool selected: false
    property color selectColor: BtnTokens.hex("accent0")
    property int span: 1
    property real unitWidth: 112
    property real spanGap: 8
    readonly property int spanUnits: pk.span >= 2 ? 2 : 1

    signal triggered()
    signal picked()

    size: "xl"
    fill: "solid"
    customColor: pk.fillColor
    inkColor: pk.textColor
    safety: pk.editMode ? "none" : pk.padSafety
    frameColor: pk.editMode && pk.selected ? pk.selectColor : pk.tallyColor
    implicitWidth: Math.max(pk.tokens.minW, pk.spanUnits * pk.unitWidth + (pk.spanUnits - 1) * pk.spanGap)

    onFired: if (pk.editMode) pk.picked(); else pk.triggered()
}
