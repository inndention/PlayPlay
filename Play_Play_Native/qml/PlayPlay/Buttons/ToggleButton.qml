import QtQuick
import QtQuick.Controls

// ToggleButton: a PButton whose `checked` shows as lit. It toggles when it fires, so the safety level, busy and the debounce
// apply to the toggle too. That is why AbstractButton's own `checkable` stays false (it would flip `checked` on every release,
// before any of those checks); since Qt 6.2 `checked` works without it, and ButtonGroup exclusivity follows `checked`.
// In an exclusive set a click on the checked button keeps it checked (radio behaviour). The button sees that set by itself
// for `autoExclusive: true` and for an exclusive group attached as `ButtonGroup.group: g`; Qt does not tell a button about a
// `ButtonGroup { buttons: [...] }`, so with that form set `keepChecked: true` (or the click unchecks it and the group is left
// with nothing selected). autoToggle: false leaves `checked` to the owner (bind it to the server state and act in onFired),
// so a click never breaks that binding.
PButton {
    id: tb
    property bool autoToggle: true
    property bool keepChecked: false       // a click on the checked key leaves it checked (see above: the buttons: form)

    Accessible.checkable: true
    Accessible.checked: tb.checked

    function _exclusive() {
        if (tb.keepChecked || tb.autoExclusive) return true
        const g = tb.ButtonGroup.group
        return g !== null && g.exclusive
    }

    onAboutToFire: {
        if (!tb.autoToggle) return
        if (tb.checked && tb._exclusive()) return
        tb.checked = !tb.checked
        tb.toggled()
    }
}
