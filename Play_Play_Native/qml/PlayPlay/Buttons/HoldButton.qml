import QtQuick

// HoldButton: a continuous-press key (docs/plans/ux-buttons.md 3.4) for PTZ moves, jog and nudge: started() when it goes
// down, stopped() when it comes up, and with repeatMs > 0 repeated(count) every repeatMs while it stays down.
// - Every started() is paired with exactly one stopped(): on release, on a cancelled press (the pointer grab was taken), and
//   when the key becomes disabled, busy or hidden, or is destroyed while down. A camera must never keep moving because its
//   stop got lost. stop() ends a hold from outside.
// - It follows the `pressed` property, not the pressed() signal, so a keyboard auto-repeat (Space on a key with Tab focus)
//   never starts it twice. A disabled or busy key does not start.
// - This is not the "hold" safety level, which fires once after holdMs: a HoldButton's own safety stays "none", and
//   fired() still follows a click (debounced) for an owner that wants the tap.
// - Lit while held (litRole primary), as the PTZ pad has always lit its pressed key.
PButton {
    id: hb

    property int repeatMs: 0               // > 0: repeated(count) every repeatMs while held
    readonly property bool holding: hb._holding
    readonly property int repeats: hb._repeats
    property bool _holding: false
    property int _repeats: 0

    signal started()
    signal stopped()
    signal repeated(int count)

    active: hb._holding
    litRole: "primary"

    function _start() {
        if (hb._holding || !hb.enabled || hb.busy || !hb.visible) return
        hb._repeats = 0
        hb._holding = true
        hb.started()
    }
    function stop() {
        if (!hb._holding) return
        hb._holding = false
        hb.stopped()
    }

    onPressedChanged: if (hb.pressed) hb._start(); else hb.stop()
    onCanceled: hb.stop()
    onEnabledChanged: if (!hb.enabled) hb.stop()
    onBusyChanged: if (hb.busy) hb.stop()
    onVisibleChanged: if (!hb.visible) hb.stop()
    Component.onDestruction: hb.stop()

    Timer {
        interval: Math.max(1, hb.repeatMs)
        repeat: true
        running: hb._holding && hb.repeatMs > 0
        onTriggered: { hb._repeats++; hb.repeated(hb._repeats) }
    }
}
