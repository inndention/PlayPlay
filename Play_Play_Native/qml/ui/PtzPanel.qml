import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".."

/**
 * PTZ camera control for the On Air page. PTZ1, PTZ2 ... are bound to ONVIF cameras (setting ui.ptzMap, kept in InputMap); every bus
 * input is set to PTZn, AUTO or none (right-click the input). AUTO here follows the input on preview, then the input on air, and moves
 * the camera that input resolves to (its PTZn, or AUTO detection: library camera, stream URI, or the camera's address in the NDI name /
 * URL). Picking PTZn controls that camera directly. Hold a pad key to move, release to stop; presets and home are one click.
 * Everything goes through POST /api/v1/cameras/{id}/ptz (the same calls as the Inputs page).
 */
CPanel {
    id: p
    property int pgmSlot: -1                  // bus slot that is on air (for "follow")
    property string sel: "auto"               // "auto" = follow PVW / PGM, else a PTZ slot id
    property real speed: 0.5
    readonly property var cams: inputMap.cameras
    readonly property var follow: {           // {input, on, r} the input AUTO follows and what it resolves to
        var a = inputMap.pvw >= 0 ? inputMap.ptzFor(inputMap.pvw) : null
        if (a && a.cam) return { input: inputMap.pvw, on: "PVW", r: a }
        var b = p.pgmSlot >= 0 ? inputMap.ptzFor(p.pgmSlot) : null
        if (b && b.cam) return { input: p.pgmSlot, on: "PGM", r: b }
        return null
    }
    readonly property var selSlot: p.sel === "auto" ? null : inputMap.ptzSlot(p.sel)
    readonly property var cam: {
        if (p.sel !== "auto") return p.selSlot ? inputMap.camById(p.selSlot.cameraId) : null
        if (p.follow) return p.follow.r.cam
        var only = cams.filter(function(c) { return c.hasPtz })
        return only.length === 1 ? only[0] : null          // one PTZ camera in the house: nothing to choose
    }
    readonly property string followText: {
        if (p.sel !== "auto") return p.selSlot ? (p.cam ? p.selSlot.name + " → " + inputMap.camLabel(p.cam) : p.selSlot.name + " has no camera: bind one below") : ""
        if (p.follow) return "AUTO  ·  input " + (p.follow.input + 1) + " (" + p.follow.on + ")  →  " + (p.follow.r.mode === "auto" ? "detected by " + p.follow.r.why : p.follow.r.slot.name) + "  ·  " + inputMap.camLabel(p.cam)
        if (p.cam) return "AUTO  ·  the only PTZ camera: " + inputMap.camLabel(p.cam)
        return "AUTO  ·  no camera input on preview or program"
    }
    readonly property var bindNames: { var a = ["(no camera)"]; for (var i = 0; i < cams.length; ++i) a.push(inputMap.camLabel(cams[i]) + (cams[i].host ? "  ·  " + cams[i].host : "") + (cams[i].hasPtz ? "" : "  (no PTZ)")); return a }
    CMenu { id: pmenu }
    function slotMenu(s) {
        var sub = [{ text: "(no camera)", checked: !s.cameraId, fn: function() { inputMap.bindPtz(s.id, "") } }]
        for (var i = 0; i < cams.length; ++i) (function(c) { sub.push({ text: inputMap.camLabel(c) + (c.host ? "   " + c.host : ""), checked: s.cameraId === c.id, fn: function() { inputMap.bindPtz(s.id, c.id) } }) })(cams[i])
        var used = []
        for (var k = 0; k < inputMap.count; ++k) if (inputMap.slots[k] && inputMap.slots[k].ptz === s.id) used.push(k + 1)
        pmenu.show([
            { header: s.name + (used.length ? "  ·  inputs " + used.join(", ") : "") },
            { text: "Control " + s.name, fn: function() { p.sel = s.id } },
            { text: "Bind to camera", sub: sub },
            { sep: true },
            { text: "Remove " + s.name, danger: true, enabled: inputMap.ptzSlots.length > 1, fn: function() { if (p.sel === s.id) p.sel = "auto"; inputMap.removePtz(s.id) } }
        ])
    }

    function ptz(c) {
        if (!cam) return
        api.post("/cameras/" + cam.id + "/ptz", c, function(ok, d) { if (!ok) window.toast("PTZ: " + (d && d.error ? d.error : "failed"), "error") })
    }
    function move(x, y, z) { ptz({ action: "move", x: x * speed, y: y * speed, z: z * speed }) }
    function stopMove() { ptz({ action: "stop" }) }

    title: "PTZ camera"
    padding: 8
    headerRight: [
        Rectangle { visible: !!p.cam; width: 8; height: 8; radius: 4; color: p.cam && p.cam.online ? Theme.ready : Theme.danger },
        Text { visible: !!p.cam; text: p.cam ? (p.cam.name || p.cam.host) : ""; color: Theme.text1; font.pixelSize: Theme.fs11; elide: Text.ElideRight; width: Math.min(160, implicitWidth) }
    ]

    ColumnLayout {
        anchors.fill: parent; spacing: 6
        // AUTO / PTZ1 / PTZ2 ... : click = control it, right-click = bind / remove
        Flow {
            Layout.fillWidth: true; spacing: 4
            Repeater {
                model: [{ id: "auto", name: "AUTO", cameraId: "" }].concat(inputMap.ptzSlots)
                Rectangle {
                    readonly property bool on: p.sel === modelData.id
                    readonly property bool bound: modelData.id === "auto" || !!inputMap.camById(modelData.cameraId)
                    readonly property bool live: !!(modelData.id !== "auto" && p.follow && p.follow.r.slot && p.follow.r.slot.id === modelData.id)
                    width: chipT.implicitWidth + 16; height: 22; radius: 3
                    color: on ? Qt.rgba(Theme.accent0.r, Theme.accent0.g, Theme.accent0.b, 0.25) : chipMa.containsMouse ? Theme.bg4 : Theme.bg3
                    border.color: on ? Theme.accent0 : live ? Theme.ready : Theme.border1
                    Text { id: chipT; anchors.centerIn: parent; text: modelData.name; color: parent.bound ? (parent.on ? Theme.text0 : Theme.text1) : Theme.text3; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 0.6 }
                    MouseArea { id: chipMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; acceptedButtons: Qt.LeftButton | Qt.RightButton
                        onClicked: function(m) { if (m.button === Qt.RightButton && modelData.id !== "auto") p.slotMenu(inputMap.ptzSlot(modelData.id)); else p.sel = modelData.id } }
                    ToolTip.visible: chipMa.containsMouse; ToolTip.delay: 500
                    ToolTip.text: modelData.id === "auto" ? "Follow the input on preview, then on air" : (inputMap.camLabel(inputMap.camById(modelData.cameraId)) || "no camera bound") + "  ·  right-click to bind"
                }
            }
            Rectangle {
                width: 22; height: 22; radius: 3; color: addMa.containsMouse ? Theme.bg4 : Theme.bg3; border.color: Theme.border1
                Text { anchors.centerIn: parent; text: "+"; color: Theme.text1; font.pixelSize: Theme.fs12; font.bold: true }
                MouseArea { id: addMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: { p.sel = inputMap.addPtz() } }
                ToolTip.visible: addMa.containsMouse; ToolTip.text: "Add a PTZ slot"
            }
        }
        Text { Layout.fillWidth: true; text: p.followText; color: p.cam ? Theme.text1 : Theme.text3; font.pixelSize: Theme.fs10; elide: Text.ElideRight }
        ComboBox {
            id: bindSel
            visible: p.sel !== "auto"
            Layout.fillWidth: true; implicitHeight: 26
            model: p.bindNames
            currentIndex: { if (!p.selSlot || !p.selSlot.cameraId) return 0; for (var i = 0; i < p.cams.length; ++i) if (p.cams[i].id === p.selSlot.cameraId) return i + 1; return 0 }
            onActivated: function(i) { if (p.selSlot) inputMap.bindPtz(p.selSlot.id, i === 0 ? "" : p.cams[i - 1].id) }
            font.pixelSize: Theme.fs11
            background: Rectangle { color: Theme.bg4; radius: Theme.r5; border.color: Theme.border1 }
            contentItem: Text { leftPadding: 8; text: (p.selSlot ? p.selSlot.name + " camera:  " : "") + bindSel.displayText; color: Theme.text0; font.pixelSize: Theme.fs11; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight }
            delegate: ItemDelegate { width: bindSel.width; contentItem: Text { text: modelData; color: Theme.text0; font.pixelSize: Theme.fs11; elide: Text.ElideRight } background: Rectangle { color: highlighted ? Theme.bg5 : Theme.bg3 } }
            popup.background: Rectangle { color: Theme.bg3; border.color: Theme.border1; radius: Theme.r5 }
        }
        RowLayout {
            Layout.fillWidth: true; Layout.fillHeight: true; spacing: 12
            enabled: !!p.cam && p.cam.hasPtz
            opacity: enabled ? 1 : 0.45
            ColumnLayout {
                Layout.alignment: Qt.AlignTop; spacing: 4
                GridLayout {
                    columns: 4; columnSpacing: 3; rowSpacing: 3
                    Repeater {
                        model: [ [-1, 1, "↖"], [0, 1, "↑"], [1, 1, "↗"], [0, 0, "+", 1], [-1, 0, "←"], [0, 0, "⌂"], [1, 0, "→"], [0, 0, "−", -1], [-1, -1, "↙"], [0, -1, "↓"], [1, -1, "↘"] ]
                        Rectangle {
                            width: 38; height: 30; radius: Theme.r3; color: pad.pressed ? Theme.accent0 : Theme.bg4; border.color: Theme.border1
                            readonly property bool isHome: modelData[2] === "⌂"
                            readonly property bool isZoom: modelData.length > 3
                            Text { anchors.centerIn: parent; text: modelData[2]; color: pad.pressed ? "#05050a" : isHome ? Theme.accent0 : Theme.text0; font.pixelSize: isZoom ? Theme.fs16 : Theme.fs14; font.bold: isZoom }
                            MouseArea { id: pad; anchors.fill: parent; cursorShape: Qt.PointingHandCursor
                                onPressed: { if (parent.isHome) p.ptz({ action: "home" }); else if (parent.isZoom) p.move(0, 0, modelData[3]); else p.move(modelData[0], modelData[1], 0) }
                                onReleased: { if (!parent.isHome) p.stopMove() }
                                onCanceled: { p.stopMove() } }
                        }
                    }
                }
                RowLayout {
                    spacing: 6
                    Text { text: "SPEED " + Math.round(p.speed * 100) + "%"; color: Theme.text2; font.pixelSize: Theme.fs10; font.bold: true; Layout.preferredWidth: 62 }
                    Slider { Layout.preferredWidth: 110; from: 0.1; to: 1; value: p.speed; onMoved: { p.speed = value } }
                }
            }
            ColumnLayout {
                Layout.fillWidth: true; Layout.fillHeight: true; spacing: 4
                Text { text: "PRESETS (" + (p.cam && p.cam.presets ? p.cam.presets.length : 0) + ")"; color: Theme.text2; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1 }
                Flickable {
                    Layout.fillWidth: true; Layout.fillHeight: true; clip: true
                    contentWidth: width; contentHeight: presetFlow.height; boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar { }
                    Flow {
                        id: presetFlow
                        width: parent.width - 8; spacing: 4
                        Repeater { model: p.cam && p.cam.presets ? p.cam.presets : []
                            CButton { text: modelData.name || modelData.token; small: true; onClicked: { p.ptz({ action: "preset", preset: modelData.token }) } ToolTip.text: "token " + modelData.token; ToolTip.visible: hovered } }
                        Text { visible: !(p.cam && p.cam.presets && p.cam.presets.length); text: "none stored"; color: Theme.text3; font.pixelSize: Theme.fs11 }
                    }
                }
                RowLayout {
                    Layout.fillWidth: true; spacing: 4
                    CField { id: presetName; Layout.fillWidth: true; placeholder: "New preset name" }
                    CButton { text: "Store"; small: true; Layout.alignment: Qt.AlignBottom; onClicked: { p.ptz({ action: "setPreset", name: presetName.text || "PlayPlay" }); presetName.text = "" } ToolTip.text: "Store the current position as a preset"; ToolTip.visible: hovered }
                    CButton { text: "Set home"; small: true; variant: "ghost"; Layout.alignment: Qt.AlignBottom; onClicked: { p.ptz({ action: "setHome" }) } }
                }
            }
        }
        Text { Layout.fillWidth: true; visible: !p.cam || !p.cam.hasPtz; wrapMode: Text.WordWrap; horizontalAlignment: Text.AlignHCenter; color: Theme.text3; font.pixelSize: Theme.fs10
            text: !p.cams.length ? "No ONVIF cameras found: discover them on the Inputs page." : !p.cam ? "No camera selected: put a camera input on preview, set its PTZ (right-click the input), or pick PTZ1…" : "This camera has no PTZ service." }
    }
}
