pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

// RailButton: a 36 x 36 key of the left icon rail (docs/plans/ux-buttons.md 3.4): an icon, lit in the accent colour while
// `checked` (the current page), with its tooltip on the right where the rail leaves room, not above the key.
// - The channel variant ("RailChannelButton") is the same key with two lines instead of an icon: text "CH1", sublabel
//   "ON" / "OFF", and led: "green" while the engine is up (the l / xl LED bar along the top).
// - autoToggle is off: the owner binds `checked` (window.currentPage === id) and navigates in onFired, so a click never
//   breaks that binding. Set autoToggle: true for a plain toggle (the column collapse key).
// - Ghost at rest (the rail is a bare strip), tonal accent with a 2 px border while lit.
// - Like IconButton, a key with neither text nor tip warns: the tip is its tooltip and accessible name.
ToggleButton {
    id: rb

    size: "l"
    fill: "ghost"
    litRole: "primary"
    autoToggle: false
    implicitWidth: 36
    implicitHeight: 36
    padX: 2                                // the l token's 8 px a side would leave 20 px and elide "CH1"

    ToolTip.visible: false                 // PButton's shared tooltip opens above; the rail's own opens on the right

    Loader {
        objectName: "railTipSlot"
        active: rb.hovered || rb.armed
        sourceComponent: Item {
            ToolTip {
                objectName: "railTip"
                parent: rb
                x: rb.width + 6
                y: (rb.height - height) / 2
                visible: (rb.hovered && rb.tipText !== "") || rb.armed
                delay: rb.armed ? 0 : BtnTokens.timing.tipDelayMs
                text: rb.armed ? rb.armTip : rb.tipText
            }
        }
    }

    Component.onCompleted: if (rb.tip === "" && rb.text === "")
        console.warn("PlayPlay.Buttons: RailButton \"" + rb.icon.name + "\" needs a tip (its tooltip and accessible name)")
}
