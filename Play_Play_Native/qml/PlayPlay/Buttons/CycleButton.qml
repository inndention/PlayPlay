import QtQuick

// CycleButton: shows values[index] (or labels[index]) plus `suffix` and steps to the next value on each fire, wrapping
// from the last to the first (docs/plans/ux-buttons.md 3.4; On Air's AUTO duration 0.25 / 0.5 / 1 / 2 s).
// - It steps when it fires, so safety, busy and the debounce gate the step (a double click is one step).
// - stepped(index, value) carries the new position. autoStep (default) moves `index` first; with autoStep: false the owner
//   keeps `index` bound to its state (index: d.indexOf(inputMap.transSec)) and applies the value in onStepped. An index
//   outside the list (a value the list lacks) shows no label and steps to the first value.
// - The key is as wide as its widest label, so cycling never reflows the row it sits in. That width leaves no room for a
//   row badge: put a status next to the key, not on it.
PButton {
    id: cy

    property var values: []
    property var labels: []                // optional display text per value
    property string suffix: ""             // " s"
    property int index: 0
    property bool autoStep: true
    readonly property int count: cy._n()
    readonly property var value: cy.index >= 0 && cy.index < cy.count ? cy.values[cy.index] : undefined

    signal stepped(int index, var value)

    function _n() { const v = cy.values; return v && v.length !== undefined ? v.length : 0 }
    function labelAt(i) {
        if (i < 0 || i >= cy._n()) return ""
        const l = cy.labels
        return (l && l.length !== undefined && i < l.length ? String(l[i]) : String(cy.values[i])) + cy.suffix
    }
    // The index one step away (dir +1 / -1), wrapping; -1 for an empty list.
    function nextIndex(dir) {
        const n = cy._n()
        if (n === 0) return -1
        const d = dir < 0 ? -1 : 1
        if (cy.index < 0 || cy.index >= n) return d > 0 ? 0 : n - 1
        return (cy.index + d + n) % n
    }

    text: cy.labelAt(cy.index)
    Accessible.description: cy.armed ? cy.armTip : (cy.tip !== "" ? cy.tip + ". " : "") + (cy.count > 0 ? "Next: " + cy.labelAt(cy.nextIndex(1)) : "")

    FontMetrics {
        id: fm
        font.family: BtnTokens.fontDisplay
        font.pixelSize: cy.tokens.font
        font.bold: true
        font.letterSpacing: cy.tokens.tracking
    }
    // The widest label. advanceWidth() is a call, which a binding does not track, so reading fm.height makes the font a
    // dependency (it changes with the size token).
    readonly property real _maxLabelW: {
        if (fm.height <= 0) return 0
        let w = 0
        for (let i = 0; i < cy._n(); ++i) w = Math.max(w, Math.ceil(fm.advanceWidth(cy.labelAt(i))) + 1)
        return w
    }
    implicitWidth: cy.stacked
        ? Math.max(cy.tokens.minW, 2 * cy.padX + Math.max(cy._hasIcon ? cy.tokens.icon : 0, cy._maxLabelW))
        : Math.max(cy.tokens.minW, 2 * cy.padX + (cy._hasIcon ? cy.tokens.icon + cy.tokens.gap : 0) + cy._maxLabelW
                                   + (cy._hasRight ? cy.tokens.icon + cy.tokens.gap : 0))

    onAboutToFire: {
        const i = cy.nextIndex(1)
        if (i < 0) return
        if (cy.autoStep) cy.index = i
        cy.stepped(i, cy.values[i])
    }
}
