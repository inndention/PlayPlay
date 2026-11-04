import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".."

/**
 * Reporter feed (docs/reporter-feed.md): the field reporter's RTMP push from the getstarnews app as an input, with the
 * go-live countdown. Sessions come from the Supervisor (/api/v1/reporter/sessions, polled: 1 s while one counts down,
 * 5 s otherwise, since WS "reporter" events reach the web dashboard only). NEW creates a session for this channel and
 * shows the publish URL + stream key once (copy); GO starts the countdown (big digits), the Supervisor takes the feed to
 * program at zero; END returns the channel to its schedule. The feed is also on the input map as "Reporter: <name>".
 */
CPanel {
    id: rf
    property string channelId: supervisor.activeChannelId
    property var sessions: []
    property var fresh: null                    // the create reply: publishUrl + streamKey, shown until dismissed
    property bool creating: false
    property string host: ""
    readonly property var mine: { var a = []; for (var i = 0; i < sessions.length; ++i) if (sessions[i].status !== "ended") a.push(sessions[i]); return a }
    readonly property var counting: { for (var i = 0; i < mine.length; ++i) if (mine[i].status === "countdown") return mine[i]; return null }
    readonly property var onAir: { for (var i = 0; i < mine.length; ++i) if (mine[i].status === "live") return mine[i]; return null }
    title: "Reporter feed"
    implicitHeight: col.implicitHeight + 2 * padding + 34

    function load() {
        api.get("/reporter/sessions", function(ok, d) { if (ok && d) { rf.sessions = d.sessions || []; rf.host = d.host || "" } })
    }
    function create() {
        var name = nameField.text.trim()
        if (!name.length) { window.toast("Reporter name required", "err"); return }
        rf.creating = true
        api.post("/reporter/sessions", { reporter: name, title: titleField.text.trim() || "LIVE", channel: rf.channelId, countdownSec: parseInt(cdField.text) || 0 }, function(ok, d) {
            rf.creating = false
            if (!ok) { window.toast(d && d.error ? d.error : "Could not create the session", "err"); return }
            rf.fresh = d; newBox.visible = false
            window.toast("Session created: give the reporter the URL and key now (shown once)", "ok")
            rf.load()
        })
    }
    function act(id, what) { api.post("/reporter/sessions/" + id + "/" + what, {}, function(ok, d) { if (!ok) window.toast(d && d.error ? d.error : "Refused", "err"); rf.load() }) }
    function copy(text) { clip.text = text; clip.selectAll(); clip.copy(); window.toast("Copied", "ok") }
    function statusColor(s) { return s === "live" ? Theme.onAir : s === "countdown" ? Theme.standby : s === "idle" ? Theme.text2 : Theme.text3 }

    TextEdit { id: clip; visible: false }
    Timer { interval: rf.counting ? 1000 : 5000; running: rf.visible; repeat: true; onTriggered: rf.load() }
    Component.onCompleted: load()
    property Connections _sup: Connections { target: supervisor; function onActiveChannelChanged() { rf.load() } }

    headerRight: [
        Text { text: rf.host.length ? "publish host " + rf.host : ""; color: Theme.text3; font.pixelSize: Theme.fs10 },
        CButton { text: newBox.visible ? "CLOSE" : "NEW"; small: true; variant: newBox.visible ? "default" : "info"; onClicked: { newBox.visible = !newBox.visible } }
    ]

    ColumnLayout {
        id: col
        anchors.fill: parent
        spacing: 8

        // ---- new session ----------------------------------------------------------------------------------------------------
        ColumnLayout {
            id: newBox
            visible: false
            Layout.fillWidth: true
            spacing: 6
            RowLayout {
                spacing: 6
                CField { id: nameField; label: "Reporter"; placeholder: "Jane Doe"; Layout.fillWidth: true }
                CField { id: titleField; label: "Title"; text: "LIVE"; Layout.preferredWidth: 120 }
                CField { id: cdField; label: "Count"; text: "10"; mono: true; Layout.preferredWidth: 54; validator: IntValidator { bottom: 0; top: 600 } }
            }
            RowLayout {
                Text { text: "On " + rf.channelId + "  ·  the phone publishes as soon as the session exists; GO cuts it to program after the countdown"; color: Theme.text3; font.pixelSize: Theme.fs10; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                CButton { text: "CREATE"; variant: "primary"; small: true; enabled: !rf.creating; onClicked: { rf.create() } }
            }
        }

        // ---- shown once: publish URL + key ------------------------------------------------------------------------------------
        Rectangle {
            visible: rf.fresh !== null
            Layout.fillWidth: true
            implicitHeight: freshCol.implicitHeight + 16
            color: Qt.rgba(1, 0.84, 0.04, 0.08); radius: Theme.r5; border.color: Theme.warn
            ColumnLayout {
                id: freshCol
                anchors { fill: parent; margins: 8 }
                spacing: 4
                RowLayout {
                    Text { text: "SHOWN ONCE  ·  " + (rf.fresh ? rf.fresh.reporter : ""); color: Theme.warn; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1; Layout.fillWidth: true }
                    CButton { text: "DISMISS"; small: true; onClicked: { rf.fresh = null } }
                }
                Repeater {
                    model: rf.fresh ? [{ k: "Publish URL", v: rf.fresh.publishUrl }, { k: "Server", v: String(rf.fresh.publishUrl).replace(/\/[^/]+$/, "") }, { k: "Stream key", v: rf.fresh.streamKey }] : []
                    delegate: RowLayout {
                        spacing: 6
                        Text { text: modelData.k; color: Theme.text2; font.pixelSize: Theme.fs10; Layout.preferredWidth: 68 }
                        Text { text: modelData.v; color: Theme.text0; font.family: Theme.fontMono; font.pixelSize: Theme.fs11; elide: Text.ElideMiddle; Layout.fillWidth: true }
                        CButton { text: "COPY"; small: true; onClicked: { rf.copy(modelData.v) } }
                    }
                }
                Text { text: "The key is not stored and will not be shown again. In the app: Settings → target \"Phone → PlayPlay station\", or any RTMP app with server + key."; color: Theme.text3; font.pixelSize: Theme.fs10; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            }
        }

        // ---- countdown / on air digits ---------------------------------------------------------------------------------------
        Rectangle {
            visible: rf.counting !== null || rf.onAir !== null
            Layout.fillWidth: true
            implicitHeight: 86
            radius: Theme.r5
            color: rf.onAir ? Qt.rgba(1, 0.18, 0.33, 0.12) : Qt.rgba(1, 0.58, 0, 0.10)
            border.color: rf.onAir ? Theme.onAir : Theme.standby
            Column {
                anchors.centerIn: parent
                spacing: 2
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: rf.counting ? String(rf.counting.remainingSec).padStart(2, "0") : "TAKE SENT"
                    color: rf.counting ? Theme.standby : Theme.onAir
                    font.family: Theme.fontMono; font.pixelSize: rf.counting ? 54 : 32; font.bold: true
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: rf.counting ? "LIVE IN  ·  " + rf.counting.reporter : rf.onAir ? rf.onAir.reporter + "  ·  " + rf.onAir.title : ""
                    color: Theme.text1; font.pixelSize: Theme.fs11; font.bold: true
                }
            }
        }

        // ---- sessions --------------------------------------------------------------------------------------------------------
        Text { visible: rf.mine.length === 0 && !newBox.visible; text: "No reporter session. NEW creates one for " + rf.channelId + "."; color: Theme.text3; font.pixelSize: Theme.fs11; Layout.fillWidth: true; wrapMode: Text.WordWrap }
        Repeater {
            model: rf.mine
            delegate: Rectangle {
                required property var modelData
                Layout.fillWidth: true
                implicitHeight: row.implicitHeight + 12
                color: Theme.bg3; radius: Theme.r5; border.color: modelData.status === "live" ? Theme.onAir : Theme.border0
                RowLayout {
                    id: row
                    anchors { fill: parent; margins: 6 }
                    spacing: 6
                    Rectangle { width: 8; height: 8; radius: 4; color: modelData.feedUp ? Theme.ready : Theme.text3 }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 1
                        RowLayout {
                            spacing: 6
                            Text { text: modelData.reporter; color: Theme.text0; font.pixelSize: Theme.fs12; font.bold: true; elide: Text.ElideRight; Layout.fillWidth: true }
                            Text { text: String(modelData.status).toUpperCase(); color: rf.statusColor(modelData.status); font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1 }
                        }
                        Text { text: (modelData.channel || "no channel") + "  ·  " + (modelData.feedUp ? "phone publishing" : "waiting for the phone") + "  ·  key …" + modelData.keyHint + (modelData.lastError ? "  ·  " + modelData.lastError : ""); color: modelData.lastError ? Theme.danger : Theme.text3; font.pixelSize: Theme.fs10; elide: Text.ElideRight; Layout.fillWidth: true }
                    }
                    CButton { visible: modelData.status === "idle"; text: "GO " + modelData.countdownSec; small: true; variant: "success"; enabled: !!modelData.channel; onClicked: { rf.act(modelData.id, "go") } }
                    CButton { visible: modelData.status === "idle" || modelData.status === "countdown"; text: "TAKE"; small: true; variant: "warn"; onClicked: { rf.act(modelData.id, "take-now") } }
                    CButton { visible: modelData.status === "countdown"; text: "CANCEL"; small: true; onClicked: { rf.act(modelData.id, "cancel") } }
                    CButton { visible: modelData.status === "live"; text: "END"; small: true; variant: "danger"; onClicked: { rf.act(modelData.id, "end") } }
                    CButton { visible: modelData.status === "idle"; text: "✕"; small: true; onClicked: { api.del("/reporter/sessions/" + modelData.id, function() { rf.load() }) } }
                }
            }
        }
    }
}
