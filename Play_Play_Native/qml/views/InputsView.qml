import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".."
import "../ui"

/**
 * Inputs: the live-input inventory. NDI sources found by every channel engine (merged, with
 * host/address and the scan settings for other subnets and groups) and ONVIF cameras with a
 * PTZ console (joystick pad, zoom, presets, home) plus "register as live source".
 * Everything goes through the Supervisor REST surface (/api/v1/inputs, /api/v1/cameras).
 */
Item {
    id: view
    property var inputs: ({ ndi: [], cameras: [], ndiConfig: {} })
    property string camSel: ""
    property real ptzSpeed: 0.5
    property var cam: {
        var cs = inputs.cameras || []
        for (var i = 0; i < cs.length; ++i) if (cs[i].id === camSel) return cs[i]
        return cs.length ? cs[0] : null
    }
    function reload() { api.get("/inputs", function(ok, d) { if (ok) { view.inputs = d; if (!view.camSel && d.cameras && d.cameras.length) view.camSel = d.cameras[0].id } }) }
    function ptz(cmd) { if (!view.cam) return; api.post("/cameras/" + view.cam.id + "/ptz", cmd, function(ok, d) { if (!ok) window.toast("PTZ: " + (d && d.error ? d.error : "failed"), "error"); else if (cmd.action === "setPreset") view.reload() }) }
    function move(x, y, z) { ptz({ action: "move", x: x * ptzSpeed, y: y * ptzSpeed, z: z * ptzSpeed }) }
    function stop() { ptz({ action: "stop" }) }
    function pip(src) { supervisor.cg({ action: "show", role: "secondary", source: src, placement: { x: 1280, y: 80, width: 560, height: 315 }, "in": { type: "fade", durationSec: 0.3 }, audioMute: true }); window.toast("PiP on " + supervisor.activeChannelId, "ok") }
    Component.onCompleted: { reload() }
    Timer { interval: 6000; running: true; repeat: true; onTriggered: { view.reload() } }

    RowLayout {
        anchors.fill: parent; spacing: 10
        // ---------------- NDI -------------------------------------------------------------
        CPanel {
            Layout.preferredWidth: 620; Layout.fillHeight: true
            title: "NDI sources (" + (view.inputs.ndi || []).length + ")"
            padding: 0
            headerRight: [
                Text { text: (view.inputs.engines || 0) + " engine(s), scanned " + (view.inputs.lastScanMs ? Math.max(0, Math.round((Date.now() - view.inputs.lastScanMs) / 1000)) + " s ago" : "never"); color: Theme.text3; font.pixelSize: Theme.fs10 },
                CButton { text: "Rescan"; small: true; onClicked: { api.post("/inputs/scan", {}, function(ok) { if (ok) { window.toast("Scan requested", "ok"); scanTimer.restart() } }) } }
            ]
            Timer { id: scanTimer; interval: 3000; onTriggered: { view.reload() } }
            ColumnLayout {
                anchors.fill: parent; spacing: 0
                ListView {
                    Layout.fillWidth: true; Layout.fillHeight: true; clip: true
                    model: view.inputs.ndi || []
                    delegate: Rectangle {
                        width: ListView.view.width; height: 40; color: index % 2 ? Theme.bg2 : Theme.bg3
                        RowLayout {
                            anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 8; spacing: 8
                            Rectangle { width: 8; height: 8; radius: 4; color: Theme.clsLive }
                            ColumnLayout { Layout.fillWidth: true; spacing: 0
                                Text { text: modelData.name || ""; color: Theme.text0; font.pixelSize: Theme.fs12; font.bold: true; elide: Text.ElideRight; Layout.fillWidth: true }
                                Text { text: (modelData.url || "") + ((modelData.seenBy || []).length ? "   seen by " + modelData.seenBy.join(", ") : ""); color: Theme.text3; font.pixelSize: Theme.fs10; font.family: Theme.fontMono; elide: Text.ElideRight; Layout.fillWidth: true }
                            }
                            CButton { text: "PiP"; small: true; onClicked: { view.pip({ type: "ndi", name: modelData.name }) } }
                            CButton { text: "+ Library"; small: true; variant: "ghost"; onClicked: { api.post("/media", { path: "live://ndi/" + modelData.name, title: modelData.name, type: "live", duration: 1800, liveSource: { type: "ndi", name: modelData.name } }, function(ok) { if (ok) window.toast("Registered in the library", "ok") }) } }
                        }
                    }
                    Text { anchors.centerIn: parent; visible: !(view.inputs.ndi && view.inputs.ndi.length); width: parent.width - 40; wrapMode: Text.WordWrap; horizontalAlignment: Text.AlignHCenter; color: Theme.text3; font.pixelSize: Theme.fs11
                        text: view.inputs.engines ? "No NDI sources on the network (each engine scans every 10 s)" : "No engine connected: NDI discovery runs inside each channel engine" }
                }
                Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border0 }
                ColumnLayout {
                    Layout.fillWidth: true; Layout.margins: 10; spacing: 6
                    RowLayout { Layout.fillWidth: true; spacing: 8
                        CField { id: ndiGroups; label: "NDI groups"; Layout.preferredWidth: 150; text: (view.inputs.ndiConfig || {}).groups || ""; placeholder: "public,studio" }
                        CField { id: ndiIps; label: "Extra IPs (other subnets)"; Layout.fillWidth: true; text: (view.inputs.ndiConfig || {}).extraIps || ""; placeholder: "192.168.1.66,192.168.1.67"; mono: true }
                        CButton { text: "Save & rescan"; small: true; variant: "primary"; Layout.alignment: Qt.AlignBottom; onClicked: { api.put("/inputs/ndi-config", { groups: ndiGroups.text, extraIps: ndiIps.text }, function(ok) { if (ok) { window.toast("NDI scan settings saved", "ok"); scanTimer.restart() } }) } }
                    }
                    Text { Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.text3; font.pixelSize: Theme.fs10
                        text: "The finder uses mDNS on the local subnet. Add the IPs of NDI hosts on other subnets (or a Discovery Server) and the names of private groups." }
                }
            }
        }
        // ---------------- Cameras -------------------------------------------------------------
        ColumnLayout {
            Layout.fillWidth: true; Layout.fillHeight: true; spacing: 10
            CPanel {
                Layout.fillWidth: true; Layout.fillHeight: true; padding: 0
                title: "Cameras — ONVIF (" + (view.inputs.cameras || []).length + ")"
                headerRight: [
                    Text { text: view.inputs.camerasDiscovering ? "discovering…" : (view.inputs.camerasLastMs ? "probed " + Math.max(0, Math.round((Date.now() - view.inputs.camerasLastMs) / 1000)) + " s ago" : ""); color: Theme.text3; font.pixelSize: Theme.fs10 },
                    CButton { text: "Discover"; small: true; onClicked: { api.post("/cameras/discover", { waitMs: 3000 }, function(ok) { if (ok) { window.toast("Discovering cameras…"); scanTimer.restart() } }) } }
                ]
                ColumnLayout {
                    anchors.fill: parent; spacing: 0
                    ListView {
                        Layout.fillWidth: true; Layout.fillHeight: true; clip: true
                        model: view.inputs.cameras || []
                        delegate: Rectangle {
                            width: ListView.view.width; height: 40; color: modelData.id === view.camSel ? Qt.rgba(0, 0.83, 1, 0.12) : (index % 2 ? Theme.bg2 : Theme.bg3)
                            MouseArea { anchors.fill: parent; onClicked: { view.camSel = modelData.id } }
                            RowLayout {
                                anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 8; spacing: 8
                                Rectangle { width: 8; height: 8; radius: 4; color: modelData.online ? Theme.ready : Theme.danger }
                                ColumnLayout { Layout.fillWidth: true; spacing: 0
                                    Text { text: (modelData.name || modelData.host) + (modelData.hardware ? "  · " + modelData.hardware : ""); color: Theme.text0; font.pixelSize: Theme.fs12; font.bold: true; elide: Text.ElideRight; Layout.fillWidth: true }
                                    Text { text: modelData.host + (modelData.error ? "   ! " + modelData.error : ""); color: modelData.error ? Theme.danger : Theme.text3; font.pixelSize: Theme.fs10; font.family: Theme.fontMono; elide: Text.ElideRight; Layout.fillWidth: true }
                                }
                                CBadge { visible: !!modelData.hasPtz; text: "PTZ"; tint: Theme.ready }
                                CBadge { text: modelData.configured ? "saved" : "found"; tint: modelData.configured ? Theme.accent0 : Theme.text2 }
                                CButton { text: "↻"; small: true; variant: "ghost"; onClicked: { api.post("/cameras/" + modelData.id + "/refresh", {}, function() { scanTimer.restart() }) } ToolTip.text: "Refresh capabilities" }
                                CButton { visible: !!modelData.configured; text: "✕"; small: true; variant: "ghost"; onClicked: { api.del("/cameras/" + modelData.id, function(ok) { if (ok) view.reload() }) } ToolTip.text: "Forget camera and credentials" }
                            }
                        }
                        Text { anchors.centerIn: parent; visible: !(view.inputs.cameras && view.inputs.cameras.length); color: Theme.text3; font.pixelSize: Theme.fs11; text: "No ONVIF cameras answered WS-Discovery. Add one by address below." }
                    }
                    Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border0 }
                    RowLayout {
                        Layout.fillWidth: true; Layout.margins: 10; spacing: 8
                        CField { id: camHost; label: "Host or XAddr"; Layout.fillWidth: true; placeholder: "192.168.1.64 or http://ip/onvif/device_service"; mono: true }
                        CField { id: camName; label: "Name"; Layout.preferredWidth: 120 }
                        CField { id: camUser; label: "User"; Layout.preferredWidth: 100 }
                        CField { id: camPass; label: "Password"; Layout.preferredWidth: 110; inputMethodHints: Qt.ImhHiddenText | Qt.ImhSensitiveData }
                        CButton { text: "Save"; small: true; variant: "primary"; Layout.alignment: Qt.AlignBottom
                            onClicked: {
                                var body = { name: camName.text, user: camUser.text, pass: camPass.text }
                                var h = camHost.text.trim()
                                if (h.length) { if (h.indexOf("http") === 0) body.xaddr = h; else body.host = h } else if (view.cam) body.id = view.cam.id; else { window.toast("Host required", "warn"); return }
                                api.post("/cameras", body, function(ok, d) { if (ok) { view.camSel = d.id; camHost.text = ""; camPass.text = ""; window.toast("Camera saved", "ok"); scanTimer.restart() } })
                            } }
                    }
                }
            }
            CPanel {
                Layout.fillWidth: true; Layout.preferredHeight: 250
                title: view.cam ? "PTZ — " + (view.cam.name || view.cam.host) : "PTZ"
                headerRight: [ Text { text: view.cam ? (view.cam.hasPtz ? view.cam.xaddr : "no PTZ service") : ""; color: Theme.text3; font.pixelSize: Theme.fs10 } ]
                RowLayout {
                    anchors.fill: parent; spacing: 16; enabled: !!view.cam
                    GridLayout {
                        columns: 3; columnSpacing: 4; rowSpacing: 4; Layout.alignment: Qt.AlignTop
                        Repeater {
                            model: [ [-1, 1, "↖"], [0, 1, "↑"], [1, 1, "↗"], [-1, 0, "←"], [0, 0, "⌂"], [1, 0, "→"], [-1, -1, "↙"], [0, -1, "↓"], [1, -1, "↘"] ]
                            Rectangle {
                                width: 46; height: 38; radius: Theme.r5; color: pad.pressed ? Theme.accent0 : Theme.bg4; border.color: Theme.border1
                                Text { anchors.centerIn: parent; text: modelData[2]; color: pad.pressed ? "#05050a" : (modelData[0] === 0 && modelData[1] === 0 ? Theme.accent0 : Theme.text0); font.pixelSize: 16 }
                                MouseArea { id: pad; anchors.fill: parent
                                    onPressed: { if (modelData[0] === 0 && modelData[1] === 0) view.ptz({ action: "home" }); else view.move(modelData[0], modelData[1], 0) }
                                    onReleased: { if (!(modelData[0] === 0 && modelData[1] === 0)) view.stop() }
                                    onCanceled: { view.stop() } }
                            }
                        }
                        Repeater {
                            model: [ [1, "Zoom +"], [-1, "Zoom −"] ]
                            Rectangle {
                                Layout.columnSpan: index === 0 ? 2 : 1; Layout.fillWidth: true; height: 30; radius: Theme.r5; color: zp.pressed ? Theme.accent0 : Theme.bg4; border.color: Theme.border1
                                Text { anchors.centerIn: parent; text: modelData[1]; color: zp.pressed ? "#05050a" : Theme.text0; font.pixelSize: Theme.fs11; font.bold: true }
                                MouseArea { id: zp; anchors.fill: parent; onPressed: { view.move(0, 0, modelData[0]) } onReleased: { view.stop() } onCanceled: { view.stop() } }
                            }
                        }
                        Text { Layout.columnSpan: 3; text: "SPEED " + Math.round(view.ptzSpeed * 100) + "%"; color: Theme.text2; font.pixelSize: Theme.fs10; font.bold: true }
                        Slider { Layout.columnSpan: 3; Layout.fillWidth: true; from: 0.1; to: 1; value: view.ptzSpeed; onMoved: { view.ptzSpeed = value } }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true; Layout.fillHeight: true; spacing: 6
                        Text { text: "PRESETS (" + (view.cam && view.cam.presets ? view.cam.presets.length : 0) + ")"; color: Theme.text2; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1 }
                        Flow { Layout.fillWidth: true; spacing: 4
                            Repeater { model: view.cam && view.cam.presets ? view.cam.presets : []
                                CButton { text: modelData.name || modelData.token; small: true; onClicked: { view.ptz({ action: "preset", preset: modelData.token }) } ToolTip.text: "token " + modelData.token } }
                            Text { visible: !(view.cam && view.cam.presets && view.cam.presets.length); text: "none"; color: Theme.text3; font.pixelSize: Theme.fs11 }
                        }
                        RowLayout { Layout.fillWidth: true; spacing: 6
                            CField { id: presetName; label: "New preset"; Layout.preferredWidth: 160; placeholder: "Wide shot" }
                            CButton { text: "Store position"; small: true; Layout.alignment: Qt.AlignBottom; onClicked: { view.ptz({ action: "setPreset", name: presetName.text || "PlayPlay" }) } }
                            CButton { text: "Set home"; small: true; variant: "ghost"; Layout.alignment: Qt.AlignBottom; onClicked: { view.ptz({ action: "setHome" }) } }
                        }
                        Text { text: "STREAM"; color: Theme.text2; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1 }
                        Text { Layout.fillWidth: true; elide: Text.ElideMiddle; font.family: Theme.fontMono; font.pixelSize: Theme.fs10; color: view.cam && view.cam.streamUri ? Theme.text1 : Theme.text3
                            text: view.cam && view.cam.streamUri ? view.cam.streamUri.replace(/:\/\/[^@]*@/, "://•••@") : "unknown — set credentials and refresh" }
                        RowLayout { Layout.fillWidth: true; spacing: 6
                            CButton { text: "Register as live source"; small: true; enabled: !!(view.cam && view.cam.streamUri); onClicked: { api.post("/cameras/" + view.cam.id + "/register", {}, function(ok, d) { if (ok) window.toast("Registered " + d.title, "ok") }) } }
                            CButton { text: "PiP"; small: true; enabled: !!(view.cam && view.cam.streamUri); onClicked: { view.pip({ type: "stream", url: view.cam.streamUri, realtime: true }) } }
                            Item { Layout.fillWidth: true }
                            Text { text: view.cam ? "schedule: ptz:" + view.cam.id + ":preset:<token>" : ""; color: Theme.text3; font.pixelSize: Theme.fs10; font.family: Theme.fontMono }
                        }
                    }
                }
            }
        }
        // ---------------- libobs scene layer (OBS Scenes; docs/libobs.md) -------------------
        LibObsPanel { Layout.preferredWidth: 420; Layout.fillHeight: true }

    }
}
