pragma ComponentBehavior: Bound
import QtQuick
import "spec.js" as Spec

// SwatchPicker: the 16 colour swatches of buttons.json (the Theme status and content-class colours plus 4 greys) as a row of
// cells, plus an optional "none" cell that clears the custom colour (docs/plans/ux-buttons.md 3.8, UXB-8). `value` is the
// chosen hex (#rrggbb, lower-cased) or "" for none; picked(hex) follows a click. A hex that is not a swatch (typed in the
// editor's field) shows no cell selected.
// No supervisor / api / window access: Qt Quick Test loads it alone.
Flow {
    id: sp

    property var swatches: Spec.swatches
    property string value: ""
    property bool allowNone: true
    property int cell: 22
    property string title: "Colour"
    readonly property int count: sp.swatches.length

    signal picked(string hex)

    spacing: 4
    Accessible.role: Accessible.Grouping
    Accessible.name: sp.title

    function norm(c) { return String(c === undefined || c === null ? "" : c).toLowerCase() }
    function pick(hex) {
        sp.value = sp.norm(hex)
        sp.picked(sp.value)
    }

    component Cell: Rectangle {
        id: cellItem
        property string hex: ""
        property bool none: false
        readonly property bool selected: cellItem.none ? sp.value === "" : sp.norm(cellItem.hex) === sp.norm(sp.value) && sp.value !== ""
        objectName: cellItem.none ? "swatch:none" : "swatch:" + sp.norm(cellItem.hex)
        width: sp.cell; height: sp.cell; radius: 4
        color: cellItem.none ? "transparent" : cellItem.hex
        border.width: cellItem.selected ? 2 : 1
        border.color: cellItem.selected ? BtnTokens.hex("text0") : cellItem.none ? BtnTokens.hex("border2") : BtnTokens.alpha("#000000", 0.35)
        Accessible.role: Accessible.RadioButton
        Accessible.name: cellItem.none ? "No custom colour" : cellItem.hex
        Accessible.checked: cellItem.selected
        // "none": a diagonal line, as a colour picker draws its clear cell
        Rectangle {
            visible: cellItem.none
            anchors.centerIn: parent
            width: parent.width * 1.1; height: 1.5
            rotation: -45
            color: BtnTokens.hex("danger")
        }
        Icon {
            visible: cellItem.selected && !cellItem.none
            anchors.centerIn: parent
            name: "check"; size: Math.round(sp.cell * 0.6)
            color: BtnTokens.inkOn(cellItem.hex !== "" ? cellItem.hex : "#000000")
        }
        HoverHandler { id: hh; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: sp.pick(cellItem.none ? "" : cellItem.hex) }
        scale: hh.hovered ? 1.12 : 1
        Behavior on scale { NumberAnimation { duration: BtnTokens.timing.animMs } }
    }

    Cell { none: true; visible: sp.allowNone }
    Repeater {
        model: sp.swatches
        delegate: Cell {
            required property string modelData
            hex: modelData
        }
    }
}
