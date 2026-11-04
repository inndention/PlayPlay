pragma ComponentBehavior: Bound
import QtQuick
import "spec.js" as Spec

// IconPicker: a searchable grid of the in-house icons (icons.json through spec.js) for a button's custom icon (docs/plans/
// ux-buttons.md 3.8, UXB-8). The search matches the icon's name and its tags ("halt" finds pause, "toh" finds alarm); the
// grid keeps the catalogue's group order (iconGroups). A "none" cell clears the custom icon (`value` ""). picked(name)
// follows a click; the matching names are `matches` (a test reads them).
// No supervisor / api / window access: Qt Quick Test loads it alone.
Item {
    id: ip

    property string value: ""
    property string query: ""
    property bool allowNone: true
    property int cell: 34
    property int iconSize: 18
    property int columns: 10
    property int rows: 4                   // visible rows; the grid scrolls past them
    readonly property var names: ip._orderedNames()
    readonly property var matches: ip._filter(ip.names, ip.query)
    readonly property int count: ip.matches.length

    property string hovered: ""          // the icon under the mouse (the name shown under the search)

    signal picked(string name)

    implicitWidth: ip.columns * (ip.cell + grid.spacing) - grid.spacing
    implicitHeight: search.height + 6 + ip.rows * (ip.cell + grid.spacing) - grid.spacing

    // Group order first (Transport, Switcher, ...), then whatever icons.json has that no group lists, alphabetically.
    function _orderedNames() {
        const out = [], seen = {}
        for (const g of Spec.iconGroups) for (const n of g.icons) if (Spec.icons[n] !== undefined && !seen[n]) { out.push(n); seen[n] = true }
        const rest = Object.keys(Spec.icons).filter(n => !seen[n]).sort()
        return out.concat(rest)
    }
    function _filter(names, query) {
        const q = String(query).trim().toLowerCase()
        if (q === "") return names
        const words = q.split(/\s+/)
        return names.filter(n => {
            const tags = Spec.icons[n].tags !== undefined ? Spec.icons[n].tags : []
            const hay = [n].concat(tags).join(" ").toLowerCase()
            return words.every(w => hay.indexOf(w) >= 0)
        })
    }
    function pick(name) {
        ip.value = name
        ip.picked(name)
    }
    function focusSearch() { searchInput.forceActiveFocus(Qt.OtherFocusReason) }

    component Cell: Rectangle {
        id: cellItem
        property string name: ""
        property bool none: false
        readonly property bool selected: cellItem.none ? ip.value === "" : ip.value === cellItem.name
        objectName: cellItem.none ? "icon:none" : "icon:" + cellItem.name
        width: ip.cell; height: ip.cell; radius: 5
        color: cellItem.selected ? BtnTokens.alpha(BtnTokens.hex("accent0"), BtnTokens.states.litA)
             : hh.hovered ? BtnTokens.hex(BtnTokens.states.hoverBg) : BtnTokens.hex("bg2")
        border.width: cellItem.selected ? 2 : 1
        border.color: cellItem.selected ? BtnTokens.hex("accent0") : BtnTokens.hex("border0")
        Accessible.role: Accessible.RadioButton
        Accessible.name: cellItem.none ? "No custom icon" : cellItem.name
        Accessible.checked: cellItem.selected
        Icon {
            visible: !cellItem.none
            anchors.centerIn: parent
            name: cellItem.name; size: ip.iconSize
            color: cellItem.selected ? BtnTokens.hex("accent0") : BtnTokens.hex("text0")
        }
        Text {
            visible: cellItem.none
            anchors.centerIn: parent
            text: "none"; color: BtnTokens.hex("text2")
            font.family: "Segoe UI"; font.pixelSize: 9
        }
        HoverHandler { id: hh; cursorShape: Qt.PointingHandCursor; onHoveredChanged: if (hh.hovered) ip.hovered = cellItem.none ? "" : cellItem.name }
        TapHandler { onTapped: ip.pick(cellItem.none ? "" : cellItem.name) }
    }


    Rectangle {
        id: search
        objectName: "iconSearch"
        width: parent.width; height: 26
        radius: 5
        color: BtnTokens.hex("bg3")
        border.width: searchInput.activeFocus ? 2 : 1
        border.color: searchInput.activeFocus ? BtnTokens.hex(BtnTokens.states.focusColor) : BtnTokens.hex("border1")
        Icon { id: searchIcon; name: "search"; size: 13; color: BtnTokens.hex("text2"); x: 8; anchors.verticalCenter: parent.verticalCenter }
        TextInput {
            id: searchInput
            objectName: "iconSearchInput"
            anchors.left: searchIcon.right; anchors.right: clearSearch.visible ? clearSearch.left : parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin: 6; anchors.rightMargin: 6
            clip: true
            color: BtnTokens.hex("text0")
            font.family: "Segoe UI"; font.pixelSize: 12
            selectByMouse: true
            text: ip.query
            onTextEdited: ip.query = searchInput.text
            Keys.onEscapePressed: { ip.query = ""; searchInput.text = "" }
            Text {
                anchors.fill: parent
                visible: searchInput.text === "" && !searchInput.activeFocus
                text: "Search icons by name or tag"
                color: BtnTokens.hex("text3"); font: searchInput.font
                verticalAlignment: Text.AlignVCenter
            }
        }
        Text {
            objectName: "iconHoverName"
            anchors.right: clearSearch.visible ? clearSearch.left : parent.right; anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            visible: ip.hovered !== "" && !searchInput.activeFocus
            text: ip.hovered; color: BtnTokens.hex("text2")
            font.family: "Cascadia Mono, Consolas"; font.pixelSize: 10
        }
        IconButton {
            id: clearSearch
            anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; anchors.rightMargin: 2
            size: "xs"; fill: "ghost"; icon.name: "close"; tip: "Clear the search"
            visible: ip.query !== ""
            focusPolicy: Qt.NoFocus
            onFired: { ip.query = ""; searchInput.text = "" }
        }
    }

    Flickable {
        id: flick
        objectName: "iconFlick"
        anchors.top: search.bottom; anchors.topMargin: 6
        anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
        contentWidth: width
        contentHeight: grid.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        Grid {
            id: grid
            objectName: "iconGrid"
            width: flick.width
            columns: Math.max(1, Math.floor((flick.width + grid.spacing) / (ip.cell + grid.spacing)))
            spacing: 4

            Cell { none: true; visible: ip.allowNone && ip.query === "" }
            Repeater {
                model: ip.matches
                delegate: Cell {
                    required property string modelData
                    name: modelData
                }
            }
        }
        Text {
            visible: ip.matches.length === 0
            anchors.centerIn: parent
            text: "No icon matches “" + ip.query + "”"
            color: BtnTokens.hex("text2"); font.family: "Segoe UI"; font.pixelSize: 11
        }
    }
}
