import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".."

/// OpenRouter model picker fed by GET /ai/models: a search box filters a dropdown whose rows show the price per million
/// tokens and the context size. Models older than ~12 months are hidden unless "Older" is on; legacy families
/// (gpt-4o, gpt-4, gpt-3.5, claude-3/2, gemini-1.x/2.0) are never listed. `picked(id)` fires on a choice.
ColumnLayout {
    id: root
    property string label: "Model"
    property string value: ""
    property var models: []
    property bool showAll: false
    property string source: ""
    property string defaultModel: "anthropic/claude-sonnet-5.5"
    property string keySource: "none"
    signal picked(string id)
    spacing: 4

    function load(refresh) {
        api.get("/ai/models?all=" + (root.showAll ? "1" : "0") + (refresh ? "&refresh=1" : ""), function(ok, d) {
            if (!ok) return
            root.models = d.models || []; root.source = d.source || ""; root.defaultModel = d.defaultModel || root.defaultModel; root.keySource = d.keySource || "none"
        })
    }
    function price(v) { v = Number(v || 0); return v === 0 ? "free" : "$" + (v >= 10 ? v.toFixed(0) : v >= 1 ? String(+v.toFixed(2)) : String(+v.toFixed(3))) }
    function ctx(n) { n = Number(n || 0); return !n ? "" : n >= 1e6 ? (n / 1e6).toFixed(n % 1e6 ? 1 : 0) + "M" : Math.round(n / 1000) + "k" }
    function labelOf(m) { return (m.name || m.id) + "  ·  " + price(m.promptPerMtok) + " in / " + price(m.completionPerMtok) + " out per Mtok" + (m.context ? "  ·  " + ctx(m.context) + " ctx" : "") }
    readonly property var filtered: {
        var q = search.text.trim().toLowerCase(), out = []
        for (var i = 0; i < root.models.length; ++i) { var m = root.models[i]; if (!q || m.id.toLowerCase().indexOf(q) >= 0 || String(m.name || "").toLowerCase().indexOf(q) >= 0) out.push(m) }
        return out.slice(0, 250)
    }
    readonly property var current: { for (var i = 0; i < root.models.length; ++i) if (root.models[i].id === root.value) return root.models[i]; return null }
    Component.onCompleted: load(false)

    RowLayout {
        Layout.fillWidth: true; spacing: 6
        CField { id: search; Layout.fillWidth: true; label: root.label + " (search)"; placeholder: "type part of a name or id" }
        CToggle { label: "Older"; implicitWidth: 90; checked: root.showAll; Layout.alignment: Qt.AlignBottom; onToggled: function(v) { root.showAll = v; root.load(false) } }
        CButton { text: "↻"; Layout.alignment: Qt.AlignBottom; onClicked: root.load(true) }
    }
    CSelect {
        Layout.fillWidth: true; label: root.filtered.length + " of " + root.models.length + " models (" + (root.source || "…") + ")"
        model: root.filtered.map(function(m) { return root.labelOf(m) })
        currentIndex: { for (var i = 0; i < root.filtered.length; ++i) if (root.filtered[i].id === root.value) return i; return -1 }
        onActivated: function(i) { if (i >= 0 && i < root.filtered.length) { root.value = root.filtered[i].id; root.picked(root.value) } }
    }
    Text {
        Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.text3; font.pixelSize: Theme.fs10
        text: (root.current ? root.current.id : root.value ? root.value + " (not in the current list)" : "default: " + root.defaultModel)
              + "   ·   " + (root.keySource === "env" ? "key: from environment (OPENROUTER_API_KEY)" : root.keySource === "settings" ? "key: saved override" : "no key: set OPENROUTER_API_KEY")
    }
}
