pragma ComponentBehavior: Bound
import QtQuick

// PtzPad: the camera pad (docs/plans/ux-buttons.md 3.4), one component for On Air's PTZ drawer (PtzPanel) and the Inputs
// page: a 3 x 3 grid of HoldButtons (eight directions round a Home key) plus zoom in / out.
// - move(x, y, z) when a key goes down (x, y, z in -1 / 0 / 1; up and right are +1, zoom in is z +1); stopMove() when no key
//   is down any more; home() when Home is clicked (it fires on release, debounced, like any key). The owner scales by its
//   speed and makes the calls: PtzPad { onMove: (x, y, z) => p.move(x, y, z); onStopMove: p.stopMove(); onHome: ... }.
// - Keys held together (touch, or Space on a focused key while the mouse holds another): the newest one drives the camera;
//   when it comes up while another is still down, that one's move is sent again, and stopMove() follows only the last
//   key. Every key's stop is guaranteed by HoldButton (release, cancel, disable, busy, hide, destroy), so disabling the
//   pad mid-move stops the camera.
// - repeatMs > 0 sends the driving key's move again every repeatMs while it is held (cameras whose continuous move times out).
// - Layout: zoomLayout "column" puts zoom in / out in a fourth column beside the top two rows (PtzPanel today); "row" puts
//   them as two equal keys under the grid (InputsView). size picks the key's token (icon size; the height unless keyHeight
//   is set); keyWidth defaults to 1.27 x the height (38 x 30 at m, as today).
Item {
    id: pad

    property string size: "m"              // s | m | l
    property real keyHeight: BtnTokens.size(pad.size).h
    property real keyWidth: Math.round(pad.keyHeight * 1.27)
    property real spacing: 3
    property bool zoom: true
    property string zoomLayout: "column"   // column | row
    property int repeatMs: 0
    property bool busy: false
    readonly property bool moving: pad._held.length > 0
    readonly property var vector: pad._held.length > 0 ? pad._vec(pad._held[pad._held.length - 1]) : null
    property var _held: []                 // the HoldButtons down now, oldest first

    signal move(real x, real y, real z)
    signal stopMove()
    signal home()

    // Eight directions round Home, then the zoom keys; col / row are grid cells (the zoom row lays out on its own).
    readonly property var keys: {
        const k = [
            { id: "nw", x: -1, y: 1, z: 0, icon: "ptz-nw", tip: "Up left", col: 0, row: 0 },
            { id: "n", x: 0, y: 1, z: 0, icon: "ptz-n", tip: "Up", col: 1, row: 0 },
            { id: "ne", x: 1, y: 1, z: 0, icon: "ptz-ne", tip: "Up right", col: 2, row: 0 },
            { id: "w", x: -1, y: 0, z: 0, icon: "ptz-w", tip: "Left", col: 0, row: 1 },
            { id: "e", x: 1, y: 0, z: 0, icon: "ptz-e", tip: "Right", col: 2, row: 1 },
            { id: "sw", x: -1, y: -1, z: 0, icon: "ptz-sw", tip: "Down left", col: 0, row: 2 },
            { id: "s", x: 0, y: -1, z: 0, icon: "ptz-s", tip: "Down", col: 1, row: 2 },
            { id: "se", x: 1, y: -1, z: 0, icon: "ptz-se", tip: "Down right", col: 2, row: 2 }
        ]
        if (pad.zoom) {
            const row = pad.zoomLayout === "row"
            k.push({ id: "in", x: 0, y: 0, z: 1, icon: "zoom-in", tip: "Zoom in", col: row ? 0 : 3, row: row ? 3 : 0 })
            k.push({ id: "out", x: 0, y: 0, z: -1, icon: "zoom-out", tip: "Zoom out", col: row ? 1 : 3, row: row ? 3 : 1 })
        }
        return k
    }
    readonly property real gridWidth: 3 * pad.keyWidth + 2 * pad.spacing
    readonly property real zoomRowKeyWidth: (pad.gridWidth - pad.spacing) / 2

    implicitWidth: pad.zoom && pad.zoomLayout !== "row" ? pad.gridWidth + pad.spacing + pad.keyWidth : pad.gridWidth
    implicitHeight: 3 * pad.keyHeight + 2 * pad.spacing + (pad.zoom && pad.zoomLayout === "row" ? pad.spacing + pad.keyHeight : 0)

    Accessible.role: Accessible.Grouping
    Accessible.name: "PTZ"

    function _vec(k) { return { x: k.modelData.x, y: k.modelData.y, z: k.modelData.z } }
    function _send(k) { const v = pad._vec(k); pad.move(v.x, v.y, v.z) }
    function _begin(k) {
        pad._held = pad._held.filter(h => h !== k).concat([k])
        pad._send(k)
    }
    function _end(k) {
        if (pad._held.indexOf(k) < 0) return                            // already ended (the pad's own destruction)
        const was = pad._held[pad._held.length - 1] === k
        pad._held = pad._held.filter(h => h !== k)
        if (pad._held.length === 0) pad.stopMove()
        else if (was) pad._send(pad._held[pad._held.length - 1])     // the key still down drives the camera again
    }
    function _repeat(k) {
        if (pad._held.length > 0 && pad._held[pad._held.length - 1] === k) pad._send(k)
    }
    // Ends every hold, oldest first so the driving key goes last: one stopMove() and no move() on the way.
    function stopAll() {
        for (const k of pad._held.slice()) k.stop()
    }
    // Going away mid-move (its page was replaced): stop here, as the keys' own stops may come after the pad is gone.
    Component.onDestruction: if (pad._held.length > 0) { pad._held = []; pad.stopMove() }

    Repeater {
        model: pad.keys
        HoldButton {
            id: k
            required property var modelData
            readonly property bool inZoomRow: k.modelData.row === 3
            objectName: "ptz-" + k.modelData.id
            x: k.inZoomRow ? k.modelData.col * (pad.zoomRowKeyWidth + pad.spacing) : k.modelData.col * (pad.keyWidth + pad.spacing)
            y: k.modelData.row * (pad.keyHeight + pad.spacing)
            width: k.inZoomRow ? pad.zoomRowKeyWidth : pad.keyWidth
            height: pad.keyHeight
            size: pad.size
            padX: 2
            icon.name: k.modelData.icon
            tip: k.modelData.tip
            busy: pad.busy
            repeatMs: pad.repeatMs
            onStarted: pad._begin(k)
            onStopped: pad._end(k)
            onRepeated: pad._repeat(k)
        }
    }
    PButton {
        id: homeKey
        objectName: "ptz-home"
        x: pad.keyWidth + pad.spacing
        y: pad.keyHeight + pad.spacing
        width: pad.keyWidth
        height: pad.keyHeight
        size: pad.size
        padX: 2
        role: "primary"
        fill: "outline"
        icon.name: "home"
        tip: "Home position"
        busy: pad.busy
        onFired: pad.home()
    }
}
