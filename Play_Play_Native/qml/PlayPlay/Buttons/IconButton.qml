import QtQuick

// IconButton: a square PButton with an icon and no label. `tip` is required: it is the tooltip and the accessible name, and an
// icon-only key without one warns here (and fails test_uispec's source scan).
PButton {
    id: ib
    implicitWidth: ib.implicitHeight
    Component.onCompleted: if (ib.tip === "")
        console.warn("PlayPlay.Buttons: IconButton \"" + ib.icon.name + "\" needs a tip (its tooltip and accessible name)")
}
