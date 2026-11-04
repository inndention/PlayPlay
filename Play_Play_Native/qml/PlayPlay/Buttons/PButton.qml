pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

// PButton: the base key of PlayPlay.Buttons (docs/plans/ux-buttons.md 3.4). A button declares what it is (role) and how hard it
// is to fire by accident (safety); the tokens pick the colours and the interaction.
// - Act on release, never twice: `fired()` follows a click only after the safety check and the minIntervalMs debounce (a
//   double click is one take), and never while disabled or busy. Use fired(), not clicked().
// - On-air keys do not take the focus on click (Qt.TabFocus): a clicked TAKE is never re-fired by Space or Enter, and the
//   window's Space shortcut keeps working. Tab still reaches them and shows the focus ring.
// - safety: "none" fires on click; "arm" arms on the first click (amber pulse, draining bar, CONFIRM) and fires on a second
//   click within armMs, Esc or the timeout disarm (the armed key is BtnTokens.armedItem, so a window's own Esc can yield);
//   "hold" fires after holdMs held down, an early release cancels; "modal" asks the window's ConfirmPopup (one per window,
//   created on the first modal press; Enter confirms, Esc cancels) and falls back to arm when it cannot have one (a key not
//   in a window). Timers and overlays for these live in a Loader that exists only while safety, busy or an error flash needs
//   it, so a plain key pays for no Timer.
// - The debounce and the double-click guard measure with the wall clock and treat a backward step as elapsed (_within).
// - The icon name rides in AbstractButton's own `icon.name` (its `icon` group is FINAL); `icon.color` overrides the colour.
AbstractButton {
    id: b

    property string role: "neutral"        // neutral|primary|go|stop|destructive|warn|info|accent|cued
    property string fill: "auto"           // auto|solid|tonal|outline|ghost
    property string size: "m"              // xs|s|m|l|xl
    property string iconRight: ""          // icon name drawn after the label (row sizes), e.g. caret-down
    property alias label: b.text
    property string sublabel: ""           // second line (l / xl)
    property string tip: ""                // tooltip; the hotkey is appended: "Hard cut (Ctrl+Enter)"
    property string hotkey: ""             // display only: the owner binds the shortcut
    property bool active: false            // lit
    property string led: ""                // ""|red|green|amber|blue
    property string ledBlink: "none"       // none|slow|fast
    property bool busy: false              // presses ignored, label dimmed, dots instead of the icon
    property string safety: "none"         // none|arm|hold|modal
    property string confirmText: ""        // modal text / armed sub-label
    property int armMs: BtnTokens.timing.armMs
    property int holdMs: BtnTokens.timing.holdMs
    property int minIntervalMs: BtnTokens.timing.minIntervalMs
    property color customColor: "transparent"   // user override of the role colour (transparent = none)
    property string badge: ""              // corner tag ("CG1·PTZ2")
    // UXB-3b, for the composite keys: litRole is the colour a key lights in when its role is neutral at rest (chips, rail,
    // drawers: an outline key that lights green / accent); segment squares the corners that join a neighbour ("left" |
    // "middle" | "right", SplitButton); badgeColor colours the badge tag (DisclosureButton's status in warn).
    property string litRole: ""            // "" = role
    property string segment: ""            // "" | left | middle | right
    property color badgeColor: "transparent"
    property real padX: b.tokens.padX      // side padding; the size token unless a fixed-width key needs less (RailButton)
    // UXB-4: inkColor is the ink (label, icon, hold wash) on a solid fill: the theme's onFill unless set (PadButton picks a
    // light or dark ink for its pad colour, BtnTokens.inkOn). armLeft is the share of the arm window left, 1 -> 0 while armed
    // and 1 otherwise; BigActionButton draws it as a countdown.
    property color inkColor: "transparent"
    readonly property real armLeft: b._armed ? b._armLeft : 1
    // iconFace, when set, is drawn in the icon slot instead of the icon (busy dots still win; a key without an icon has no
    // slot). armLine, when set, is the second line of an armed l / xl key instead of confirmText / CONFIRM. BigActionButton
    // puts its countdown dial and seconds there.
    property Component iconFace: null
    property string armLine: ""
    // UXB-5: displayText is the label drawn, the text unless a wrapper shows less of it (the CButton shim draws "▶ Run now" as
    // the play icon and "Run now", while `text` stays what its call site set). labelWidth is that label's implicit width.
    property string displayText: b.text
    readonly property real labelWidth: labelText.implicitWidth

    signal fired()
    signal aboutToFire()                   // just before fired(); ToggleButton toggles here

    readonly property bool armed: b._armed
    readonly property real holdProgress: b._holdProgress
    readonly property bool lit: b.active || b.checked
    readonly property var tokens: BtnTokens.size(b.size)
    readonly property bool stacked: b.tokens.layout === "stacked"
    readonly property color tint: BtnTokens.tint(b.lit && b.litRole !== "" ? b.litRole : b.role, b.customColor)
    readonly property string effectiveFill: BtnTokens.resolveFill(b.role, b.fill, b.lit)
    // look: the colours now (hover and press included); restLook: the same key at rest. The background paints restLook and
    // fades a state layer in look's colour over it on hover and press (animMs), so a lit / tally change flips on its frame.
    // Creation cost (UXB-6 --bench): a key at rest makes one look() call; look is restLook until it is hovered or pressed.
    readonly property var restLook: BtnTokens.look({ role: b.role, fill: b.fill, lit: b.lit, tint: b.tint, enabled: b.enabled,
                                                     hovered: false, pressed: false, large: b.stacked, ink: b.inkColor })
    readonly property var look: (b.hovered || b.down) && b.enabled
        ? BtnTokens.look({ role: b.role, fill: b.fill, lit: b.lit, tint: b.tint, enabled: b.enabled,
                           hovered: b.hovered, pressed: b.down, large: b.stacked, ink: b.inkColor })
        : b.restLook
    readonly property string tipText: b.tip !== "" ? (b.hotkey !== "" ? b.tip + " (" + b.hotkey + ")" : b.tip)
                                    : b.hotkey !== "" && b.text !== "" ? b.text + " (" + b.hotkey + ")"
                                    : b.safety === "hold" ? "Press and hold" : ""
    readonly property string armTip: b.confirmText !== "" ? b.confirmText : "Click again to confirm"

    property bool _armed: false
    property real _armLeft: 1
    property double _armedAt: 0
    property double _lastFire: 0
    property real _holdProgress: 0
    property bool _error: false
    readonly property bool _hasIcon: b.icon.name !== ""
    readonly property bool _hasLabel: b.displayText !== ""
    readonly property bool _hasRight: !b.stacked && b.iconRight !== ""
    readonly property bool _rowBadge: !b.stacked && b.badge !== ""        // l / xl show the badge in the corner instead
    readonly property string _sub: b._armed && b.stacked
        ? (b.armLine !== "" ? b.armLine : b.confirmText !== "" ? b.confirmText : BtnTokens.states.armSublabel) : b.sublabel
    readonly property real _rL: b.segment === "right" || b.segment === "middle" ? 0 : b.tokens.radius   // left corners
    readonly property real _rR: b.segment === "left" || b.segment === "middle" ? 0 : b.tokens.radius    // right corners
    readonly property bool _badgeTinted: b.badgeColor.a > 0 && b.enabled
    // Set on the first hover or press and kept: the state layer is made then, so a key nobody touches pays for none.
    property bool _touched: false

    focusPolicy: Qt.TabFocus
    hoverEnabled: true
    padding: 0
    implicitHeight: b.tokens.h
    implicitWidth: b.stacked
        ? Math.max(b.tokens.minW, 2 * b.padX + Math.max(b._hasIcon ? b.tokens.icon : 0, b._hasLabel ? labelText.implicitWidth : 0,
                                                               subSlot.active ? subSlot.implicitWidth : 0))
        : Math.max(b.tokens.minW, 2 * b.padX + (b._hasIcon ? b.tokens.icon : 0) + (b._hasLabel ? labelText.implicitWidth : 0)
                   + (b._hasRight ? b.tokens.icon : 0) + (b._rowBadge ? rowBadge.implicitWidth : 0)
                   + b.tokens.gap * Math.max(0, (b._hasIcon ? 1 : 0) + (b._hasLabel ? 1 : 0) + (b._hasRight ? 1 : 0) + (b._rowBadge ? 1 : 0) - 1))

    Accessible.role: Accessible.Button
    Accessible.name: b.text !== "" ? b.text : b.tip
    Accessible.description: b._armed ? b.armTip : b.tip

    ToolTip.visible: (b.hovered && b.tipText !== "") || b._armed
    ToolTip.text: b._armed ? b.armTip : b.tipText
    ToolTip.delay: b._armed ? 0 : BtnTokens.timing.tipDelayMs

    // --- firing ------------------------------------------------------------------------------------------------------------
    // True while less than `ms` has passed since `stamp`. Date.now() is the wall clock, which w32time / NTP or a manual time
    // set can step backwards: the stamp is then in the future, and "now - stamp < ms" would hold until the clock caught up,
    // leaving a key that just fired dead for as long as the step. A negative interval therefore counts as elapsed. (QML has
    // no monotonic clock, and a Timer per key would cost every plain key one.)
    function _within(stamp, ms) { const dt = Date.now() - stamp; return dt >= 0 && dt < ms }
    function _fire() {
        if (b._within(b._lastFire, b.minIntervalMs)) return false
        b._lastFire = Date.now()
        b.aboutToFire()
        b.fired()
        return true
    }
    function _arm() {
        BtnTokens.arming(b)                 // one pending confirmation in the application: others disarm
        b._armedAt = Date.now()
        b._armLeft = 1                      // before _armed: armLeft reads a full window from the arming frame on
        b._armed = true
        BtnTokens.armedItem = b
    }
    function disarm() {
        b._armed = false
        if (BtnTokens.armedItem === b) BtnTokens.armedItem = null
    }
    function _confirmByArm() {
        if (!b._armed) { b._arm(); return }
        if (b._within(b._armedAt, b.minIntervalMs)) return      // a double click never confirms
        b.disarm()
        b._fire()
    }
    // A click (mouse, touch or keyboard). Hold keys fire from their timer, never from a click.
    function _clickedNow() {
        if (!b.enabled || b.busy) return
        if (b.safety === "none" || b.safety === "") { b._fire(); return }
        if (b.safety === "arm") { b._confirmByArm(); return }
        if (b.safety === "modal") {
            const pop = BtnTokens.confirmPopup(b.Overlay.overlay)
            // The popup confirms later: by then the key may be disabled, busy, no longer modal or destroyed (a destroyed
            // object's properties read undefined), and then the confirmation fires nothing.
            const self = b
            // The key goes along as the popup's source: ConfirmPopup cancels itself when it stops being ready.
            if (pop) { pop.ask(b.text !== "" ? b.text : b.tip, b.confirmText !== "" ? b.confirmText : b.tip,
                               b.text !== "" ? b.text : "Confirm", b.role,
                               function() { if (self && self.enabled === true && self.busy === false && self.safety === "modal") self._fire() },
                               b)
                       return }
            b._confirmByArm()
        }
    }
    // Show the error state (a danger border for states.errorMs), e.g. when the call a key made fails.
    function flashError() { b._error = true }

    onClicked: b._clickedNow()
    onHoveredChanged: if (b.hovered) b._touched = true
    onDownChanged: if (b.down) b._touched = true
    onBusyChanged: if (b.busy) b.disarm()
    onEnabledChanged: if (!b.enabled) b.disarm()
    onSafetyChanged: b.disarm()

    // --- look -------------------------------------------------------------------------------------------------------------
    background: Rectangle {
        id: bgRect
        objectName: "pbBackground"
        radius: b.tokens.radius
        topLeftRadius: b._rL; bottomLeftRadius: b._rL
        topRightRadius: b._rR; bottomRightRadius: b._rR
        color: b.restLook.bg
        border.color: b._error ? BtnTokens.hex(BtnTokens.states.errorColor) : b.look.border
        border.width: b._error ? 2 : b.look.borderW
        Loader {     // the state layer, made on the first hover or press (_touched) and kept
            objectName: "pbStateSlot"
            anchors.fill: parent
            anchors.margins: bgRect.border.width
            active: b._touched
            sourceComponent: Rectangle {
                id: layer
                objectName: "pbStateLayer"
                readonly property bool on: b.enabled && (b.hovered || b.down)
                radius: Math.max(0, bgRect.radius - bgRect.border.width)
                topLeftRadius: Math.max(0, b._rL - bgRect.border.width); bottomLeftRadius: Math.max(0, b._rL - bgRect.border.width)
                topRightRadius: Math.max(0, b._rR - bgRect.border.width); bottomRightRadius: Math.max(0, b._rR - bgRect.border.width)
                color: b.look.bg
                opacity: 0           // bound on completion, so the first hover fades in like every later one
                visible: layer.opacity > 0
                Behavior on opacity { NumberAnimation { duration: BtnTokens.timing.animMs } }
                Component.onCompleted: layer.opacity = Qt.binding(() => layer.on ? 1 : 0)
            }
        }
    }

    contentItem: Item {
        Grid {
            anchors.centerIn: parent
            columns: b.stacked ? 1 : 4
            columnSpacing: b.tokens.gap
            rowSpacing: b.tokens.gap
            horizontalItemAlignment: Grid.AlignHCenter
            verticalItemAlignment: Grid.AlignVCenter
            Loader {
                id: iconSlot
                objectName: "pbIconSlot"
                active: b._hasIcon
                visible: iconSlot.active
                width: b.tokens.icon; height: b.tokens.icon
                sourceComponent: b.busy ? busyDots : b.iconFace !== null ? b.iconFace : mainIcon
            }
            Text {
                id: labelText
                objectName: "pbLabel"
                visible: b._hasLabel
                text: b.displayText
                width: Math.max(0, Math.min(labelText.implicitWidth, b.width - 2 * b.padX
                                - (b.stacked ? 0 : (b._hasIcon ? b.tokens.icon + b.tokens.gap : 0) + (b._hasRight ? b.tokens.icon + b.tokens.gap : 0)
                                                   + (b._rowBadge ? rowBadge.implicitWidth + b.tokens.gap : 0))))
                elide: Text.ElideRight
                horizontalAlignment: Text.AlignHCenter
                color: b.look.text
                opacity: b.busy ? BtnTokens.states.busyLabelA : 1
                font.family: BtnTokens.fontDisplay
                font.pixelSize: b.tokens.font
                font.bold: true
                font.letterSpacing: b.tokens.tracking
                lineHeightMode: b.stacked ? Text.FixedHeight : Text.ProportionalHeight
                lineHeight: b.stacked ? labelText.font.pixelSize + 2 : 1.0
            }
            // The second line and the row badge are Loaders: most keys have neither, and a Text costs its creation even hidden.
            Loader {         // second line (l / xl)
                id: subSlot
                objectName: "pbSublabelSlot"
                active: b.stacked && b._sub !== ""
                visible: subSlot.active
                sourceComponent: Text {
                    id: subText
                    objectName: "pbSublabel"
                    text: b._sub
                    width: Math.max(0, Math.min(subText.implicitWidth, b.width - 2 * b.padX))
                    elide: Text.ElideRight
                    horizontalAlignment: Text.AlignHCenter
                    color: b._armed && b.effectiveFill !== "solid" ? BtnTokens.hex(BtnTokens.states.armColor) : b.look.text
                    opacity: b._armed || !b.enabled ? 1 : 0.75
                    font.family: BtnTokens.fontDisplay
                    font.pixelSize: b.tokens.subFont !== undefined ? b.tokens.subFont : b.tokens.font
                    font.bold: true
                    lineHeightMode: Text.FixedHeight
                    lineHeight: subText.font.pixelSize + 2
                }
            }
            Loader {         // badge on row sizes: an inline tag after the label
                id: rowBadge
                objectName: "pbRowBadgeSlot"
                active: b._rowBadge
                visible: rowBadge.active
                sourceComponent: Rectangle {
                    objectName: "pbRowBadge"
                    implicitWidth: rowBadgeText.implicitWidth + 6
                    implicitHeight: rowBadgeText.implicitHeight + 2
                    radius: 2
                    color: b._badgeTinted ? BtnTokens.alpha(b.badgeColor, 0.16) : BtnTokens.alpha(BtnTokens.hex("bg0"), 0.55)
                    Text {
                        id: rowBadgeText
                        objectName: "pbRowBadgeText"
                        anchors.centerIn: parent
                        text: b.badge
                        color: b._badgeTinted ? b.badgeColor : BtnTokens.hex("text1")
                        font.family: BtnTokens.fontDisplay
                        font.pixelSize: Math.max(8, b.tokens.font - 3)
                        font.bold: true
                    }
                }
            }
            Loader {
                id: rightSlot
                objectName: "pbIconRight"
                active: b._hasRight
                visible: rightSlot.active
                width: b.tokens.icon; height: b.tokens.icon
                sourceComponent: Icon { name: b.iconRight; size: b.tokens.icon; color: b.look.icon }
            }
        }
    }

    Component {
        id: mainIcon
        Icon {
            objectName: "pbIcon"
            name: b.icon.name
            size: b.tokens.icon
            color: b.enabled && b.icon.color.a > 0 ? b.icon.color : b.look.icon
        }
    }
    Component {
        id: busyDots
        Item {
            objectName: "pbBusy"
            Row {
                anchors.centerIn: parent
                spacing: Math.max(1, b.tokens.icon / 8)
                Repeater {
                    model: 3
                    Rectangle {
                        id: dot
                        required property int index
                        width: Math.max(2, b.tokens.icon / 5); height: dot.width; radius: dot.width / 2
                        color: b.look.icon
                        SequentialAnimation on opacity {
                            loops: Animation.Infinite
                            PauseAnimation { duration: dot.index * 150 }
                            NumberAnimation { from: 0.25; to: 1; duration: 225 }
                            NumberAnimation { from: 1; to: 0.25; duration: 225 }
                            PauseAnimation { duration: (2 - dot.index) * 150 }
                        }
                    }
                }
            }
        }
    }

    // Safety, busy and error machinery, under the content (z between the background's -1 and the content's 0).
    Loader {
        id: safetyLayer
        z: -0.5
        anchors.fill: parent
        active: (b.safety !== "none" && b.safety !== "") || b.busy || b._error
        sourceComponent: Item {
            id: sl
            // The arm marks (pulse, draining bar) in the arm colour, except on a solid key whose tint is close to it (warn
            // keys such as mux.restart, a yellow or amber customColor), where amber on amber would hide the armed state: there
            // they take the fill's text colour (its ink: onFill unless inkColor is set).
            readonly property color armInk: {
                const arm = BtnTokens.col(BtnTokens.hex(BtnTokens.states.armColor)), t = b.tint
                const near = Math.abs(t.r - arm.r) + Math.abs(t.g - arm.g) + Math.abs(t.b - arm.b) < 0.35
                return b.effectiveFill === "solid" && near ? b.restLook.text : arm
            }

            Connections {
                target: BtnTokens
                function onArming(item) { if (item !== b) b.disarm() }
            }
            Timer { interval: b.armMs; running: b._armed; onTriggered: b.disarm() }
            NumberAnimation { target: b; property: "_armLeft"; running: b._armed; from: 1; to: 0; duration: b.armMs }
            // Qt hands a key that two enabled shortcuts share to one of them in turn, as activatedAmbiguously, so another Esc
            // shortcut (On Air's "clear preview", main.qml) must yield while BtnTokens.armedItem is set or the first Esc misses
            // this key. activatedAmbiguously still disarms for an Esc shortcut that does not yield (on the next press).
            Shortcut {
                sequence: "Escape"
                enabled: b._armed
                onActivated: b.disarm()
                onActivatedAmbiguously: b.disarm()
            }

            Connections {
                target: b
                function onPressedChanged() {
                    if (b.safety !== "hold") return
                    if (b.pressed && b.enabled && !b.busy) { holdTimer.restart(); holdAnim.restart() }
                    else { holdTimer.stop(); holdAnim.stop(); b._holdProgress = 0 }
                }
            }
            Timer { id: holdTimer; interval: b.holdMs; onTriggered: if (b.pressed && b.enabled && !b.busy && b.safety === "hold") b._fire() }
            NumberAnimation { id: holdAnim; target: b; property: "_holdProgress"; from: 0; to: 1; duration: b.holdMs }

            Timer { interval: BtnTokens.states.errorMs; running: b._error; onTriggered: b._error = false }

            Rectangle {      // hold progress: a fill growing left to right
                objectName: "pbHoldFill"
                visible: b._holdProgress > 0
                x: 0; y: 0; height: sl.height
                width: sl.width * b._holdProgress
                radius: b.tokens.radius
                topLeftRadius: b._rL; bottomLeftRadius: b._rL
                topRightRadius: b._rR; bottomRightRadius: b._rR
                color: BtnTokens.alpha(b.effectiveFill === "solid" ? b.restLook.text : b.tint, BtnTokens.states.holdA)
            }
            Rectangle {      // armed: the confirmation window draining along the bottom edge
                objectName: "pbArmBar"
                visible: b._armed
                x: b.tokens.radius / 2; y: sl.height - height
                height: BtnTokens.states.armBarH
                width: (sl.width - b.tokens.radius) * b._armLeft
                color: sl.armInk
            }
            Rectangle {      // armed: a pulsing amber border
                id: pulse
                objectName: "pbArmPulse"
                visible: b._armed
                anchors.fill: parent
                radius: b.tokens.radius
                topLeftRadius: b._rL; bottomLeftRadius: b._rL
                topRightRadius: b._rR; bottomRightRadius: b._rR
                color: "transparent"
                border.width: BtnTokens.states.armW
                border.color: sl.armInk
                SequentialAnimation on opacity {
                    running: b._armed
                    loops: Animation.Infinite
                    NumberAnimation { from: 1; to: BtnTokens.states.armPulseMin; duration: 500 / BtnTokens.states.armPulseHz }
                    NumberAnimation { from: BtnTokens.states.armPulseMin; to: 1; duration: 500 / BtnTokens.states.armPulseHz }
                }
            }
            Row {            // busy without an icon: the dots sit under the dimmed label
                visible: b.busy && !b._hasIcon
                anchors.horizontalCenter: parent.horizontalCenter
                y: sl.height - 4 - height
                spacing: 2
                Repeater {
                    model: 3
                    Rectangle {
                        id: bd
                        required property int index
                        width: 3; height: 3; radius: 1.5
                        color: b.look.text
                        SequentialAnimation on opacity {
                            running: b.busy
                            loops: Animation.Infinite
                            PauseAnimation { duration: bd.index * 150 }
                            NumberAnimation { from: 0.25; to: 1; duration: 225 }
                            NumberAnimation { from: 1; to: 0.25; duration: 225 }
                            PauseAnimation { duration: (2 - bd.index) * 150 }
                        }
                    }
                }
            }
        }
    }

    Loader {         // LED / tally: a bar along the top edge on l / xl, a dot in the corner on xs / s / m
        objectName: "pbLedSlot"
        z: 1
        anchors.fill: parent
        active: b.led !== ""
        sourceComponent: Item {
            Rectangle {
                id: ledShape
                objectName: "pbLed"
                color: BtnTokens.ledColor(b.led)
                x: b.stacked ? b.tokens.radius : parent.width - width - 3
                y: b.stacked ? 0 : 3
                width: b.stacked ? parent.width - 2 * b.tokens.radius : BtnTokens.leds.dotSize
                height: b.stacked ? BtnTokens.leds.barH : BtnTokens.leds.dotSize
                radius: b.stacked ? 1 : height / 2
                SequentialAnimation on opacity {
                    running: b.ledBlink === "slow" || b.ledBlink === "fast"
                    loops: Animation.Infinite
                    PropertyAction { value: 1 }
                    PauseAnimation { duration: 500 / (BtnTokens.leds.blinkHz[b.ledBlink] || 1) }
                    PropertyAction { value: 0.2 }
                    PauseAnimation { duration: 500 / (BtnTokens.leds.blinkHz[b.ledBlink] || 1) }
                    onRunningChanged: if (!running) ledShape.opacity = 1
                }
            }
        }
    }

    Loader {         // corner tag (l / xl; row sizes put it after the label)
        objectName: "pbBadgeSlot"
        z: 1
        active: b.stacked && b.badge !== ""
        x: 3; y: 3
        sourceComponent: Rectangle {
            objectName: "pbBadge"
            radius: 2
            color: b._badgeTinted ? BtnTokens.alpha(b.badgeColor, 0.2) : BtnTokens.alpha(BtnTokens.hex("bg0"), 0.7)
            width: badgeText.implicitWidth + 6; height: badgeText.implicitHeight + 2
            Text {
                id: badgeText
                anchors.centerIn: parent
                text: b.badge
                color: b._badgeTinted ? b.badgeColor : BtnTokens.hex("text1")
                font.family: BtnTokens.fontDisplay
                font.pixelSize: 8
                font.bold: true
            }
        }
    }

    Loader {         // keyboard focus: a ring outside the key, only for keyboard focus (visualFocus)
        objectName: "pbFocusSlot"
        z: 2
        active: b.visualFocus
        anchors.fill: parent
        anchors.margins: -(BtnTokens.states.focusOffset + BtnTokens.states.focusW)
        sourceComponent: Rectangle {
            objectName: "pbFocusRing"
            color: "transparent"
            radius: b.tokens.radius + BtnTokens.states.focusOffset + BtnTokens.states.focusW
            border.width: BtnTokens.states.focusW
            border.color: BtnTokens.hex(BtnTokens.states.focusColor)
        }
    }
}
