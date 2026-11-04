import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".."

CPanel {
    id: p
    title: "Automation channels / slave nodes"
    implicitHeight: content.implicitHeight + 58
    property var config: ({})
    property var nodes: []
    property string localId: ""
    property string mode: "local"
    property string nodeId: ""
    property string remoteId: "main"
    property string inputUrl: ""
    property int count: 1
    readonly property var channels: config.channels || []
    readonly property var selected: channels.filter(function(c) { return c.id === p.localId })[0] || ({})
    readonly property var state: selected.slaveStatus || ({})
    function load() {
        api.get("/automation/channels", function(ok,d) { if (ok) { p.config = d; if (!p.localId && p.channels.length) { p.count = d.channelCount; p.adopt(p.channels[0].id) } } })
        api.get("/remote/nodes", function(ok,d) { if (ok) p.nodes = d.nodes || [] })
    }
    function adopt(id) {
        localId = id; var d = selected.slave || {}
        mode = d.mode || "local"; nodeId = d.nodeId || ""; remoteId = d.channelId || "main"; inputUrl = d.inputUrl || ""
    }
    function save() {
        api.put("/automation/channels/" + encodeURIComponent(localId), {mode: mode, nodeId: nodeId, channelId: remoteId, inputUrl: inputUrl}, function(ok,d) {
            if (ok) p.config = d; window.toast(ok ? "Channel mode saved" : d.error || "Cannot save channel mode", ok ? "ok" : "error")
        })
    }
    Component.onCompleted: load()
    Timer { interval: 5000; running: p.visible; repeat: true; onTriggered: p.load() }
    ColumnLayout {
        id: content; anchors.left: parent.left; anchors.right: parent.right; anchors.margins: 12; spacing: 8
        RowLayout { Layout.fillWidth: true
            CSelect { label: "Automation channels"; model: [1,2,3,4,5,6,7,8]; currentIndex: p.count - 1; Layout.preferredWidth: 150; onActivated: function(i) { p.count = i + 1 } }
            CButton { text: "Apply count"; small: true; Layout.alignment: Qt.AlignBottom; onClicked: api.put("/automation/channels", {count: p.count}, function(ok,d) { if (ok) p.config = d; window.toast(ok ? "Channel count saved; schedules retained" : d.error || "Cannot save count", ok ? "ok" : "error") }) }
            Text { text: "Reducing the count stops those slots and keeps their schedules."; color: Theme.text2; font.pixelSize: Theme.fs11; Layout.fillWidth: true; wrapMode: Text.WordWrap }
        }
        RowLayout { Layout.fillWidth: true
            CSelect { label: "Local channel"; Layout.fillWidth: true; model: p.channels.map(function(c) { return c.name + " [" + c.id + "]" }); currentIndex: { for(var i=0;i<p.channels.length;i++) if(p.channels[i].id===p.localId) return i; return -1 }
                onActivated: function(i) { p.adopt(p.channels[i].id) } }
            CSelect { label: "Channel mode"; Layout.fillWidth: true; model: ["Local automation", "Slave: receive video + audio", "Slave: mirror schedule / local media"]; currentIndex: p.mode === "relay" ? 1 : p.mode === "mirror" ? 2 : 0; onActivated: function(i) { p.mode = ["local","relay","mirror"][i] } }
        }
        RowLayout { Layout.fillWidth: true; visible: p.mode !== "local"
            CSelect { label: "Upstream node"; Layout.fillWidth: true; model: ["Choose node…"].concat(p.nodes.map(function(n) { return n.name || n.id })); currentIndex: { for (var i=0;i<p.nodes.length;i++) if(p.nodes[i].id===p.nodeId) return i+1; return 0 }
                onActivated: function(i) { p.nodeId = i > 0 ? p.nodes[i-1].id : "" } }
            CField { label: "Remote channel ID"; text: p.remoteId; Layout.preferredWidth: 150; onEdited: function(value) { p.remoteId = value } }
            CField { label: "Receiver URL (blank: upstream NDI)"; visible: p.mode === "relay"; text: p.inputUrl; placeholder: "ndi://HOST (Program) or srt://…"; Layout.fillWidth: true; onEdited: function(value) { p.inputUrl = value } }
        }
        RowLayout { Layout.fillWidth: true
            CButton { text: "Save channel mode"; small: true; variant: "primary"; enabled: p.localId.length > 0; onClicked: p.save() }
            Text { Layout.fillWidth: true; wrapMode: Text.WordWrap; color: p.state.error ? Theme.danger : Theme.text2; font.pixelSize: Theme.fs11
                text: p.state.error || (p.mode === "local" ? "Local schedule automation" : p.state.lastSyncMs ? "Last sync: " + Qt.formatTime(new Date(p.state.lastSyncMs), "hh:mm:ss") : "Awaiting upstream configuration") }
        }
        Text { Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.text3; font.pixelSize: Theme.fs10
            text: "Relay carries the upstream picture, graphics and audio. Mirroring copies the current and next hours using matching local media. Returning to Local restores the original local hours." }
    }
}
