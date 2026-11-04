pragma ComponentBehavior: Bound
import QtQuick

// ChipGroup: a Flow of small ToggleButtons, one per model entry, with one selection (`selected` an id string) or several
// (`multi: true`, `selected` an array of ids) (docs/plans/ux-buttons.md 3.4). It replaces the three chip copies: On Air's
// transition types, GraphicsDeck's mix targets and PtzPanel's slots.
// - model: strings (id = label) or objects {id, label | name, icon, tip, led, badge, role, litRole, enabled}. labelFn(id,
//   entry), when set, gives the label (On Air: inputMap.transLabel).
// - A chip fires through PButton (debounce, disabled, busy), then picked(id). With autoSelect (default) the group updates
//   `selected` itself: single selects the id (a click on the selected chip keeps it), multi adds or removes it. With
//   autoSelect: false it never writes `selected`, so a binding to the owner's state survives and the owner acts in onPicked
//   (GraphicsDeck's "All mixes" rule, a server round trip).
// - Chips are neutral outline keys at rest that light in `role` (go = green, as the transition chips) when selected.
// - Right-click on a chip: menuRequested(id, chip), for slot menus (bind / remove a PTZ slot).
// - fillWidth: the chips share the group's width equally on one row (On Air's chip row).
Flow {
    id: cg

    property var model: []
    property var selected: ""              // an id; an array of ids when multi
    property bool multi: false
    property bool autoSelect: true
    property string size: "xs"
    property string role: "go"             // the colour a selected chip lights in
    property bool fillWidth: false
    property var labelFn: null             // function(id, entry) -> label
    property string title: ""              // accessible name of the group
    readonly property int count: rep.count

    signal picked(string id)
    signal menuRequested(string id, Item anchor)

    spacing: 3
    Accessible.role: Accessible.Grouping
    Accessible.name: cg.title

    function entryOf(m) {
        return typeof m === "object" && m !== null ? m : { id: String(m), label: String(m) }
    }
    // `selected` as a list of id strings (multi). Lists from C++ arrive as array-like sequences, so test length, not
    // Array.isArray; a lone id is a list of one.
    function selectedIds() {
        const s = cg.selected
        if (s === undefined || s === null || s === "") return []
        if (typeof s === "object" && s.length !== undefined) {
            const out = []
            for (let i = 0; i < s.length; ++i) out.push(String(s[i]))
            return out
        }
        return [String(s)]
    }
    function isSelected(id) {
        if (cg.multi) return cg.selectedIds().indexOf(String(id)) >= 0
        const s = cg.selected
        return s !== undefined && s !== null && String(s) === String(id)
    }
    function labelOf(e) {
        const id = String(e.id), f = cg.labelFn
        if (typeof f === "function") return String(f(id, e))
        return e.label !== undefined ? String(e.label) : e.name !== undefined ? String(e.name) : id
    }
    function pick(id) {
        id = String(id)
        if (cg.multi) {
            const cur = cg.selectedIds(), i = cur.indexOf(id)
            if (i >= 0) cur.splice(i, 1); else cur.push(id)
            if (cg.autoSelect) cg.selected = cur
        } else if (cg.autoSelect) {
            cg.selected = id
        }
        cg.picked(id)
    }

    Repeater {
        id: rep
        model: cg.model
        delegate: ToggleButton {
            id: chip
            required property var modelData
            required property int index
            readonly property var entry: cg.entryOf(chip.modelData)
            readonly property string chipId: String(chip.entry.id)

            objectName: "chip:" + chip.chipId
            size: cg.size
            role: chip.entry.role !== undefined ? chip.entry.role : "neutral"
            litRole: chip.entry.litRole !== undefined ? chip.entry.litRole : cg.role
            text: cg.labelOf(chip.entry)
            icon.name: chip.entry.icon !== undefined ? chip.entry.icon : ""
            tip: chip.entry.tip !== undefined ? chip.entry.tip : ""
            led: chip.entry.led !== undefined ? chip.entry.led : ""
            badge: chip.entry.badge !== undefined ? chip.entry.badge : ""
            enabled: chip.entry.enabled !== false
            autoToggle: false                  // the group owns the selection; `checked` stays bound to it
            checked: cg.isSelected(chip.chipId)
            // fillWidth: equal shares, floored so the row never sums past the group's width and wraps
            width: cg.fillWidth && rep.count > 0 ? Math.max(chip.tokens.minW, Math.floor((cg.width - cg.spacing * (rep.count - 1)) / rep.count))
                                                 : chip.implicitWidth
            Accessible.role: cg.multi ? Accessible.CheckBox : Accessible.RadioButton
            onFired: cg.pick(chip.chipId)
            TapHandler {
                acceptedButtons: Qt.RightButton
                onTapped: cg.menuRequested(chip.chipId, chip)
            }
        }
    }
}
