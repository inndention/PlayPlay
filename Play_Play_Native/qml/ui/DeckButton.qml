pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import PlayPlay.Buttons

/// Big tactile transport key: glyph over label. variant: default|success|danger|warn|info|primary; `active` lights it.
// UXB-5 (docs/plans/ux-buttons.md 3.6): a shim over PlayPlay.Buttons' BigActionButton, so the 12 On Air deck keys take the
// token look and the in-house icons with no edit. The API is today's: glyph, variant, active, text, ToolTip, clicked;
// 74 x 52 unless the call site sets its own size.
// - glyph -> icon through the glyph map ("⇥" cut, "⧉" auto, "‖" pause ...); a glyph the map does not know is drawn as text.
// - The key keeps today's meaning of variant: a neutral key whose icon is in the variant colour, lit (active) as a tonal key
//   in that colour with a 2 px border (BigActionButton's lit look is today's 30 % tint + 2 px). It is not a solid colour
//   key: UXB-9 moves the deck to actionId keys with their own roles and safety.
// - Kept from today on purpose: the key takes the focus on click (StrongFocus) and `clicked` fires on every click, the second
//   click of a double click included (the call sites use onClicked; see CButton for why PButton alone would drop it); no
//   actionId, so no safety, tip or hotkey of its own.
BigActionButton {
    id: b

    property string glyph: ""
    property string variant: "default"
    // `active` and `tint` are PButton's; tint is the variant colour while lit.

    readonly property var _vm: BtnTokens.legacyVariants[b.variant] !== undefined ? BtnTokens.legacyVariants[b.variant]
                                                                                    : BtnTokens.legacyVariants["default"]
    readonly property var _map: BtnTokens.glyph(b.glyph)
    readonly property color variantColor: b.variant === "default" || b.variant === "ghost" ? "transparent" : BtnTokens.roleColor(b._vm.role)

    implicitWidth: 74
    implicitHeight: 52
    role: "neutral"
    litRole: b.variant === "default" || b.variant === "ghost" ? "" : b._vm.role
    icon.name: b.glyph === "" ? "" : b._map !== null ? b._map.icon : "dot"     // unknown: the glyph face is drawn instead
    icon.color: b.variantColor
    iconFace: b.glyph !== "" && b._map === null ? glyphFace : null
    focusPolicy: Qt.StrongFocus
    onDoubleClicked: b.clicked()      // the second click of a double click still acts (CButton.qml explains)

    Component {
        id: glyphFace
        Item {
            objectName: "dbGlyphFace"
            Text {
                anchors.centerIn: parent
                text: b.glyph
                color: b.enabled && b.variantColor.a > 0 ? b.variantColor : b.look.icon
                font.pixelSize: 20
            }
        }
    }
}
