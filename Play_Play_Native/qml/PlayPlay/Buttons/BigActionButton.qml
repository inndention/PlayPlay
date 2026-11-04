pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Shapes

// BigActionButton: the on-air key (docs/plans/ux-buttons.md 3.4): TAKE, CUT, AUTO, GO LIVE / END LIVE, the deck transport.
// Size l (default) or xl draws the icon above the label; it also works at row sizes (an emergency slate at m), without the
// dial below.
// - actionId: a station action id of buttons.json safetyDefaults (bus.cut, transport.stop, live.end ...). It gives the label,
//   icon, role, tip, hotkey hint and the native safety (design 3.5) as defaults; anything set on the key wins. actionOn:
//   false takes a stateful action's "off" entry (automation.arm disarmed, engine.toggle off). holdToFire forces "hold".
// - tally: "pgm" | "pvw" lights the LED (red / green; an explicit `led` wins) and frames the key in the tally colour, as
//   MonitorTile frames a monitor. Neither animates: a tally flips on the frame its state changes (3.2). On a solid key the
//   frame sits just outside it, 1 px clear, so a red tally still reads on a red key (inside, it would vanish); on outline,
//   tonal, ghost and lit keys it lines the edge as on a monitor. frameColor is that frame; PadButton also draws its
//   edit-mode selection with it. The frame steps aside during an error flash.
// - The arm countdown and the hold progress are drawn large on l / xl: a dial takes the icon's place. Armed, an amber ring
//   drains over armMs around the seconds left (3, 2, 1; `countdown`); held, a light ring fills over holdMs. A key without an
//   icon shows the seconds in its second line instead ("CONFIRM 3"). PButton's own marks stay (the pulsing border and
//   draining bar, the hold wash, the CONFIRM line). The dial exists only while armed or held.
PButton {
    id: bb

    property string actionId: ""           // a safetyDefaults id; "" = none (AbstractButton's own `action` is FINAL)
    property bool actionOn: true           // false: the action's "off" entry (stateful actions)
    property bool holdToFire: false        // safety "hold" whatever the action says
    property string tally: ""              // "" | pgm | pvw
    property color frameColor: bb.tallyColor
    readonly property var actionSpec: bb.actionId !== "" ? BtnTokens.actionDefaults(bb.actionId, bb.actionOn) : null
    readonly property string tallyLed: BtnTokens.leds.tally[bb.tally] !== undefined ? BtnTokens.leds.tally[bb.tally] : ""
    readonly property color tallyColor: bb.tallyLed !== "" ? BtnTokens.ledColor(bb.tallyLed) : "transparent"
    readonly property bool frameOutside: bb.effectiveFill === "solid"
    readonly property int countdown: bb.armed ? Math.max(1, Math.ceil(bb.armLeft * bb.armMs / 1000 - 1e-6)) : 0
    readonly property real dialSize: bb.tokens.icon
    readonly property bool dialShown: bb.stacked && bb._hasIcon && (bb.armed || bb.holdProgress > 0)

    size: "l"
    text: bb.actionSpec !== null ? bb.actionSpec.label : ""
    icon.name: bb.actionSpec !== null ? bb.actionSpec.icon : ""
    role: bb.actionSpec !== null ? bb.actionSpec.role : "neutral"
    tip: bb.actionSpec !== null ? bb.actionSpec.tip : ""
    hotkey: bb.actionSpec !== null ? bb.actionSpec.hotkey : ""
    safety: bb.holdToFire ? "hold" : bb.actionSpec !== null ? bb.actionSpec.safety : "none"
    led: bb.tallyLed
    iconFace: bb.dialShown ? dialC : null
    armLine: bb.stacked && !bb._hasIcon && bb.armed
             ? (bb.confirmText !== "" ? bb.confirmText : BtnTokens.states.armSublabel) + " " + bb.countdown : ""

    Accessible.description: bb.armed ? bb.armTip
        : [bb.tip, bb.tally === "pgm" ? "On air" : bb.tally === "pvw" ? "Preview" : ""].filter(s => s !== "").join(". ")

    function _checkAction() {
        if (bb.actionId !== "" && bb.actionSpec === null)
            BtnTokens.warnOnce("action:" + bb.actionId, "unknown action \"" + bb.actionId + "\" (assets/ui/buttons.json safetyDefaults)")
    }
    onActionIdChanged: bb._checkAction()
    Component.onCompleted: bb._checkAction()

    // The tally / selection frame. Inside: over the background (and its hover layer), under PButton's arm pulse and hold
    // wash (z -0.5) and the content. Outside (solid keys): a ring 1 px clear of the edge, inside the focus ring's span.
    Rectangle {
        id: frame
        readonly property real off: bb.frameOutside ? 1 + BtnTokens.states.litBorderW : 0
        objectName: "bigFrame"
        z: -0.75
        anchors.fill: parent
        anchors.margins: -frame.off
        visible: bb.frameColor.a > 0 && !bb._error
        color: "transparent"
        radius: bb.tokens.radius + frame.off
        topLeftRadius: bb._rL + frame.off; bottomLeftRadius: bb._rL + frame.off
        topRightRadius: bb._rR + frame.off; bottomRightRadius: bb._rR + frame.off
        border.width: BtnTokens.states.litBorderW
        border.color: bb.frameColor
    }

    Component {
        id: dialC
        Item {
            id: dial
            objectName: "bigDial"
            readonly property bool arming: bb.armed
            // armed: what is left of the window, drained clockwise from 12 o'clock; held: how far the hold has come
            readonly property real progress: dial.arming ? bb.armLeft : bb.holdProgress
            readonly property real sweep: 360 * Math.max(0, Math.min(1, dial.progress))
            readonly property color ink: dial.arming ? BtnTokens.hex(BtnTokens.states.armColor) : BtnTokens.hex("text0")
            readonly property real ring: Math.max(2, bb.dialSize / 8)
            readonly property real r: (bb.dialSize - dial.ring) / 2

            Rectangle {      // a dark disc, so the ring reads on any key colour
                anchors.fill: parent
                radius: width / 2
                color: BtnTokens.alpha(BtnTokens.hex("bg0"), 0.72)
            }
            Shape {
                anchors.fill: parent
                preferredRendererType: BtnTokens.iconRenderer === "geometry" ? Shape.GeometryRenderer : Shape.CurveRenderer
                ShapePath {  // the track
                    strokeColor: BtnTokens.alpha(dial.ink, 0.28); fillColor: "transparent"
                    strokeWidth: dial.ring; capStyle: ShapePath.FlatCap
                    PathAngleArc { centerX: bb.dialSize / 2; centerY: bb.dialSize / 2; radiusX: dial.r; radiusY: dial.r; startAngle: -90; sweepAngle: 360 }
                }
                ShapePath {  // the progress
                    strokeColor: dial.ink; fillColor: "transparent"
                    strokeWidth: dial.ring; capStyle: ShapePath.FlatCap
                    PathAngleArc {
                        centerX: bb.dialSize / 2; centerY: bb.dialSize / 2; radiusX: dial.r; radiusY: dial.r
                        startAngle: -90
                        sweepAngle: dial.sweep
                    }
                }
            }
            Text {
                objectName: "bigDialText"
                anchors.centerIn: parent
                visible: dial.arming
                text: bb.countdown
                color: dial.ink
                font.family: BtnTokens.fontDisplay
                font.pixelSize: Math.round(bb.dialSize * 0.55)
                font.bold: true
            }
        }
    }
}
