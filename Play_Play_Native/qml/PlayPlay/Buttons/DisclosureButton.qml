import QtQuick

// DisclosureButton: a ToggleButton that opens and closes a drawer, with chevron-right (closed) / chevron-down (open) and a
// `status` tag after the label ("SOLO", "AFV OFF"), drawn in statusColor (Theme warn) so a closed drawer still tells the
// operator what is unusual inside it (docs/plans/ux-buttons.md 3.4; On Air's mixer / PTZ drawers).
// - `expanded` is `checked`. It toggles when it fires (safety, busy and the debounce apply). To keep the open state in
//   the owner (window.onAirDrawers), bind `checked` and set autoToggle: false, then toggle in onFired.
// - Neutral outline at rest, lit in the accent colour while open (litRole primary), as today's drawer toggles.
ToggleButton {
    id: db

    property string status: ""
    property color statusColor: BtnTokens.hex("warn")
    property alias expanded: db.checked

    size: "s"
    litRole: "primary"
    icon.name: db.checked ? "chevron-down" : "chevron-right"
    badge: db.status
    badgeColor: db.statusColor

    Accessible.name: db.text + (db.status !== "" ? ", " + db.status : "")
    Accessible.description: db.armed ? db.armTip : (db.checked ? "Expanded" : "Collapsed") + (db.tip !== "" ? ". " + db.tip : "")
}
