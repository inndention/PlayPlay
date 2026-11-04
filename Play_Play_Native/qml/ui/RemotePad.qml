import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".."

/// Remote pad: mappable, themed buttons that call this station or any remote node (docs/remote-pad.md).
/// Data: GET/PUT /remote/pad (theme, buttons, themes, catalog, nodes, channels), POST /remote/pad/{id}/press, GET /remote/pad/log.
CPanel {
    id: p
    title: "Remote pad"
    implicitHeight: 34 + col.implicitHeight + 24
    property var d: ({ theme: "studio", buttons: [], themes: [], catalog: [], nodes: [], channels: [] })
    property var log: []
    property bool edit: false
    property bool dirty: false
    property string sel: ""
    property int catIdx: 0
    property string addNode: "local"
    property string addCh: ""
    property string armedKey: ""            // two-tap confirm: the button id waiting for its second tap
    readonly property var th: { for (var i = 0; i < (d.themes || []).length; i++) if (d.themes[i].id === d.theme) return d.themes[i]; return ({}) }
    readonly property var roles: ["neutral", "go", "stop", "warn", "info", "accent"]
    readonly property var selBtn: { for (var i = 0; i < (d.buttons || []).length; i++) if (d.buttons[i].id === sel) return d.buttons[i]; return null }

    function roleColor(r) { return th[r] || th.neutral || "#33445a" }
    function nodeName(id) { for (var i = 0; i < (d.nodes || []).length; i++) if (d.nodes[i].id === id) return d.nodes[i].name; return id }
    function groups() { var g = []; (d.buttons || []).forEach(function(b) { var n = b.group || "Main"; if (g.indexOf(n) < 0) g.push(n) }); return g }
    function inGroup(g) { return (d.buttons || []).filter(function(b) { return (b.group || "Main") === g }) }
    function load() {
        if (dirty) return
        api.get("/remote/pad", function(ok, r) { if (ok) { p.d = r; if (!p.addCh && (r.channels || []).length) p.addCh = r.channels[0].id } })
        api.get("/remote/pad/log", function(ok, r) { if (ok) p.log = (r.log || []).slice(-5).reverse() })
    }
    function press(b) {
        if (edit) { sel = b.id; return }
        if (b.confirm && armedKey !== b.id) { armedKey = b.id; armTimer.restart(); return }
        armedKey = ""
        api.post("/remote/pad/" + encodeURIComponent(b.id) + "/press", {}, function(ok, r) { window.toast(ok ? b.label + ": sent" : b.label + ": " + (r && r.error ? r.error : "failed"), ok ? "ok" : "error"); p.dirty = false; p.load() })
    }
    function patch(k, v) {
        var a = JSON.parse(JSON.stringify(d)); for (var i = 0; i < a.buttons.length; i++) if (a.buttons[i].id === sel) a.buttons[i][k] = v
        d = a; dirty = true
    }
    function add() {
        var a = JSON.parse(JSON.stringify(d)), c = catIdx < a.catalog.length ? a.catalog[catIdx] : { label: "Custom", group: "Custom", method: "GET", path: "/status", body: {}, role: "neutral", confirm: false }
        var b = { id: "b" + Math.random().toString(36).slice(2, 8), node: addNode, channel: c.needsChannel ? addCh : "", hotkey: "", label: c.label, group: c.group, method: c.method, path: c.path, body: c.body || {}, role: c.role, confirm: !!c.confirm }
        a.buttons.push(b); d = a; sel = b.id; dirty = true
    }
    function remove() { var a = JSON.parse(JSON.stringify(d)); a.buttons = a.buttons.filter(function(b) { return b.id !== sel }); d = a; sel = ""; dirty = true }
    function save() { api.put("/remote/pad", { theme: d.theme, buttons: d.buttons }, function(ok, r) { window.toast(ok ? "Pad saved" : (r && r.error ? r.error : "save failed"), ok ? "ok" : "error"); if (ok) { p.dirty = false; p.sel = ""; p.load() } }) }
    function cancel() { dirty = false; edit = false; sel = ""; load() }
    function hotkeyOk(h) { return /^(F(9|1[0-2])|(Ctrl|Alt|Shift)\+(?!(C|V|X|L|0|-|Right)$).+)$/i.test(h || "") }   // native shortcuts: F9-F12 or modified keys; F1-F8 are the page shortcuts and a bare letter would steal typing

    Component.onCompleted: load()
    Timer { interval: 5000; repeat: true; running: p.visible; onTriggered: p.load() }
    Timer { id: armTimer; interval: 3000; onTriggered: p.armedKey = "" }
    Repeater { model: p.edit ? [] : (p.d.buttons || []).filter(function(b) { return p.hotkeyOk(b.hotkey) })
        Shortcut { sequence: modelData.hotkey; enabled: p.visible; onActivated: p.press(modelData) } }

    ColumnLayout {
        id: col
        anchors.left: parent.left; anchors.right: parent.right; anchors.margins: 12
        spacing: 6
        Rectangle {
            Layout.fillWidth: true; radius: Theme.r8; color: p.th.bg || Theme.bg3; border.color: p.th.border || Theme.border1
            implicitHeight: inner.implicitHeight + 20
            ColumnLayout { id: inner; anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 10; spacing: 6
                RowLayout { Layout.fillWidth: true
                    Text { text: p.th.name || ""; color: p.th.text || Theme.text0; font.bold: true; font.pixelSize: Theme.fs12 }
                    Item { Layout.fillWidth: true }
                    CButton { text: p.edit ? "Done" : "Edit pad"; small: true; onClicked: { if (p.edit && p.dirty) p.cancel(); else { p.edit = !p.edit; p.sel = "" } } } }
                Repeater { model: p.groups()
                    ColumnLayout { Layout.fillWidth: true; spacing: 3
                        Text { text: String(modelData).toUpperCase(); color: p.th.muted || Theme.text2; font.pixelSize: Theme.fs10; font.letterSpacing: 1 }
                        Flow { Layout.fillWidth: true; spacing: 8
                            Repeater { model: p.inGroup(modelData)
                                Rectangle {
                                    property var b: modelData
                                    width: 112; height: 58; radius: Theme.r5
                                    color: pad.pressed ? Qt.darker(p.roleColor(b.role), 1.25) : pad.containsMouse ? Qt.lighter(p.roleColor(b.role), 1.1) : p.roleColor(b.role)
                                    border.color: p.edit && p.sel === b.id ? (p.th.accent || Theme.accent0) : (p.armedKey === b.id ? "#ffffff" : (p.th.border || Theme.border1)); border.width: (p.edit && p.sel === b.id) || p.armedKey === b.id ? 2 : 1
                                    Column { anchors.centerIn: parent; spacing: 2
                                        Text { anchors.horizontalCenter: parent.horizontalCenter; text: p.armedKey === b.id ? "CONFIRM?" : b.label; color: "#ffffff"; font.bold: true; font.pixelSize: Theme.fs12; elide: Text.ElideRight; width: 100; horizontalAlignment: Text.AlignHCenter }
                                        Text { anchors.horizontalCenter: parent.horizontalCenter; text: p.nodeName(b.node) + (b.channel ? " · " + b.channel : "") + (b.hotkey ? " · " + b.hotkey : ""); color: "#ffffff"; opacity: 0.8; font.pixelSize: Theme.fs10; elide: Text.ElideRight; width: 100; horizontalAlignment: Text.AlignHCenter } }
                                    MouseArea { id: pad; anchors.fill: parent; hoverEnabled: true; onClicked: p.press(b) } } } } } }
                Text { visible: (p.d.buttons || []).length === 0; text: "No buttons yet. Press Edit pad and add some (skip, arm, go live, or any call)."; color: p.th.muted || Theme.text2; font.pixelSize: Theme.fs11 }
                Flow { Layout.fillWidth: true; spacing: 12
                    Repeater { model: p.log
                        Text { text: (modelData.ok ? "✓ " : "✗ ") + modelData.label + " · " + modelData.ms + " ms" + (modelData.ok ? "" : " · " + (modelData.error || modelData.http)); color: modelData.ok ? (p.th.muted || Theme.text2) : Theme.danger; font.pixelSize: Theme.fs10 } } }
            }
        }
        ColumnLayout { visible: p.edit; Layout.fillWidth: true; spacing: 6
            RowLayout { Layout.fillWidth: true; spacing: 8
                CSelect { label: "Add"; Layout.preferredWidth: 230; model: (p.d.catalog || []).map(function(c) { return c.group + ": " + c.label }).concat(["Custom call…"]); currentIndex: p.catIdx; onActivated: function(i) { p.catIdx = i } }
                CSelect { label: "On"; Layout.preferredWidth: 150; model: (p.d.nodes || []).map(function(n) { return n.name }); currentIndex: { for (var i = 0; i < (p.d.nodes || []).length; i++) if (p.d.nodes[i].id === p.addNode) return i; return 0 } onActivated: function(i) { p.addNode = p.d.nodes[i].id } }
                CSelect { label: "Channel"; Layout.preferredWidth: 130; model: (p.d.channels || []).map(function(c) { return c.name || c.id }); currentIndex: { for (var i = 0; i < (p.d.channels || []).length; i++) if (p.d.channels[i].id === p.addCh) return i; return 0 } onActivated: function(i) { p.addCh = p.d.channels[i].id } }
                CButton { text: "+ Add button"; small: true; Layout.alignment: Qt.AlignBottom; onClicked: p.add() }
                Item { Layout.fillWidth: true }
                CSelect { label: "Theme"; Layout.preferredWidth: 130; model: (p.d.themes || []).map(function(t) { return t.name }); currentIndex: { for (var i = 0; i < (p.d.themes || []).length; i++) if (p.d.themes[i].id === p.d.theme) return i; return 0 } onActivated: function(i) { var a = JSON.parse(JSON.stringify(p.d)); a.theme = a.themes[i].id; p.d = a; p.dirty = true } }
                CButton { text: "Save pad"; variant: p.dirty ? "primary" : "default"; enabled: p.dirty; Layout.alignment: Qt.AlignBottom; onClicked: p.save() }
                CButton { text: "Cancel"; variant: "ghost"; Layout.alignment: Qt.AlignBottom; onClicked: p.cancel() } }
            RowLayout { visible: !!p.selBtn; Layout.fillWidth: true; spacing: 8
                CField { label: "Label"; Layout.preferredWidth: 130; text: p.selBtn ? p.selBtn.label : ""; onEdited: function(v) { p.patch("label", v) } }
                CField { label: "Group"; Layout.preferredWidth: 90; text: p.selBtn ? p.selBtn.group || "" : ""; onEdited: function(v) { p.patch("group", v) } }
                CSelect { label: "Colour"; Layout.preferredWidth: 100; model: p.roles; currentIndex: p.selBtn ? Math.max(0, p.roles.indexOf(p.selBtn.role)) : 0; onActivated: function(i) { p.patch("role", p.roles[i]) } }
                CSelect { label: "Node"; Layout.preferredWidth: 140; model: (p.d.nodes || []).map(function(n) { return n.name }); currentIndex: { if (!p.selBtn) return 0; for (var i = 0; i < (p.d.nodes || []).length; i++) if (p.d.nodes[i].id === p.selBtn.node) return i; return 0 } onActivated: function(i) { p.patch("node", p.d.nodes[i].id) } }
                CField { label: "Channel"; Layout.preferredWidth: 80; text: p.selBtn ? p.selBtn.channel || "" : ""; onEdited: function(v) { p.patch("channel", v) } }
                CField { label: "Key (F9, Ctrl+1)"; Layout.preferredWidth: 100; text: p.selBtn ? p.selBtn.hotkey || "" : ""; onEdited: function(v) { p.patch("hotkey", v) } }
                CField { label: "Path"; Layout.fillWidth: true; mono: true; text: p.selBtn ? p.selBtn.path : ""; onEdited: function(v) { p.patch("path", v) } }
                CToggle { label: "Confirm"; Layout.preferredWidth: 110; checked: p.selBtn ? !!p.selBtn.confirm : false; onToggled: function(v) { p.patch("confirm", v) } }
                CButton { text: "Delete"; small: true; variant: "danger"; Layout.alignment: Qt.AlignBottom; onClicked: p.remove() } }
        }
    }
}
