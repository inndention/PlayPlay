import QtQuick

// SplitButton: a main PButton joined to a caret-down menu key (docs/plans/ux-buttons.md 3.4). fired() comes from the main
// key (its safety, busy and debounce apply); menuRequested(anchor) from the caret, with the caret key as the anchor, and the
// caller opens its own menu there (menu.popup(anchor, 0, anchor.height)), so the module stays free of ui/ code. Uses:
// GraphicsDeck "Store in CG ▾ pick a CG", RemoteView "Push ▾ session", "Load hour ▾ (now / TOH / snap)".
// - The two keys are segments of one shape (their inner corners squared, a 1 px seam) and take the same role, fill and
//   size; the caret is narrower than square. In a layout the main key takes the width the caret leaves.
// - safety / busy / sublabel / led / badge / hotkey apply to the main key only: opening the menu is harmless, and what a
//   menu entry does is the caller's to guard. Alt+Down (or the Menu key) on the focused main key opens the menu too.
// - The icon rides in `icon.name` as on PButton (an alias of the main key's icon group).
Item {
    id: sb

    property string role: "neutral"
    property string fill: "auto"
    property string size: "m"
    property alias text: mainKey.text
    property alias label: mainKey.text
    property alias icon: mainKey.icon
    property string sublabel: ""
    property string tip: ""
    property string hotkey: ""
    property string menuTip: "More options"
    property bool active: false
    property string led: ""
    property bool busy: false
    property string safety: "none"
    property string confirmText: ""
    property int armMs: BtnTokens.timing.armMs
    property int holdMs: BtnTokens.timing.holdMs
    property int minIntervalMs: BtnTokens.timing.minIntervalMs
    property color customColor: "transparent"
    property string badge: ""
    readonly property bool armed: mainKey.armed
    readonly property PButton mainButton: mainKey
    readonly property PButton menuButton: menuKey
    readonly property real seam: 1

    signal fired()
    signal menuRequested(Item anchor)

    implicitWidth: mainKey.implicitWidth + sb.seam + menuKey.implicitWidth
    implicitHeight: mainKey.implicitHeight

    function openMenu() { sb.menuRequested(menuKey) }
    function disarm() { mainKey.disarm() }

    PButton {
        id: mainKey
        objectName: "splitMain"
        segment: "left"
        width: Math.max(0, sb.width - sb.seam - menuKey.width)
        height: sb.height
        role: sb.role
        fill: sb.fill
        size: sb.size
        sublabel: sb.sublabel
        tip: sb.tip
        hotkey: sb.hotkey
        active: sb.active
        led: sb.led
        busy: sb.busy
        safety: sb.safety
        confirmText: sb.confirmText
        armMs: sb.armMs
        holdMs: sb.holdMs
        minIntervalMs: sb.minIntervalMs
        customColor: sb.customColor
        badge: sb.badge
        onFired: sb.fired()
        Keys.onPressed: function(e) {
            if ((e.key === Qt.Key_Down && (e.modifiers & Qt.AltModifier)) || e.key === Qt.Key_Menu) {
                e.accepted = true
                sb.openMenu()
            }
        }
    }
    IconButton {
        id: menuKey
        objectName: "splitMenu"
        segment: "right"
        x: sb.width - menuKey.width
        height: sb.height
        implicitWidth: Math.max(menuKey.tokens.icon + 6, Math.round(menuKey.tokens.h * 0.7))
        role: sb.role
        fill: sb.fill
        size: sb.size
        active: sb.active
        customColor: sb.customColor
        icon.name: "caret-down"
        tip: sb.menuTip
        Accessible.role: Accessible.ButtonMenu
        onFired: sb.menuRequested(menuKey)
    }
}
