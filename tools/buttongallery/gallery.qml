import QtQuick
import QtQuick.Window
import QtQuick.Controls
import PlayPlay.Buttons
import "../../Play_Play_Native/qml/ui" as Shim
import "../../tests/qml/buttons/legacy" as Legacy      // frozen pre-UXB-5 CButton / DeckButton / CToggle

// PlayPlayButtonGallery (UXB-6, docs/plans/ux-buttons.md 5.4): every PlayPlay.Buttons component x role x size x state, the
// icon grid, the legacy-vs-module shim panel and the confirm popup, on pages. main.cpp loads this from qrc (or from the source
// tree with --source) and drives it: --screenshot sets `shooting` (no toolbar, tool tips off), walks `pages`, calls pose() and
// grabs tiles scrolled through scrollX / scrollY; --bench calls buildBench() / destroyBench(); report() describes the page.
//
// States a key cannot be put in through its public API are posed: hover through galleryHost.hover() (a HoverEnter sent to the
// key itself, so the real mouse is never moved), pressed through `down`, armed / hold / error through PButton's private
// _armed / _holdProgress / _error, the way the UXB-3b and UXB-4 review sheets forced them. pose() runs again before each tile,
// so the 600 ms error flash is still lit in every tile.
Window {
    id: win
    visible: false                         // main.cpp shows it (on screen, or off screen and input-transparent when shooting)
    width: 1480; height: 940
    title: "PlayPlay Button Gallery"
    color: BtnTokens.hex("bg1")

    property string page: "matrix"
    property real zoom: 1
    readonly property bool shooting: galleryHost.shooting
    property real scrollX: 0
    property real scrollY: 0
    readonly property var pages: [
        { id: "matrix", name: "ROLE × SIZE × STATE" },
        { id: "fills", name: "FILLS" },
        { id: "composites", name: "COMPOSITES" },
        { id: "keys", name: "ON-AIR KEYS" },
        { id: "icons", name: "ICONS" },
        { id: "shim", name: "SHIM" },
        { id: "popup", name: "CONFIRM" },
        { id: "editor", name: "EDITOR" }
    ]
    readonly property var pageComponents: ({ matrix: matrixPage, fills: fillsPage, composites: compositesPage, keys: keysPage,
                                             icons: iconsPage, shim: shimPage, popup: popupPage,
                                             editor: editorPage })
    // The page's size at the current zoom, for main.cpp's window size and tiles.
    readonly property size contentSize: Qt.size(Math.ceil(pageLoader.width * win.zoom), Math.ceil(pageLoader.height * win.zoom))
    readonly property bool pageReady: win.page === "" || pageLoader.status === Loader.Ready

    // A sample label and icon per role, so a row of the matrix reads as a real key.
    readonly property var sample: ({
        neutral: ["LOAD", "folder"], primary: ["PLAY", "play"], go: ["GO LIVE", "broadcast"], stop: ["CUT", "cut"],
        destructive: ["DELETE", "trash"], warn: ["RESTART", "reload"], info: ["INFO", "info"], accent: ["CG", "cg"],
        cued: ["CUE", "preview"]
    })
    readonly property var stateList: ["rest", "hover", "pressed", "lit", "led", "disabled", "busy", "armed", "hold", "error"]
    readonly property var benchIcons: Object.keys(BtnTokens.icons)
    readonly property var benchGlyphs: Object.keys(BtnTokens.glyphs).filter(g => g.length === 1)

    // --- driven by main.cpp ------------------------------------------------------------------------------------------------
    function findNamed(root, name) {
        let hit = null
        win.walk(root, function(it) { if (!hit && it.objectName === name) hit = it })
        if (!hit && root && root.contentItem) win.walk(root.contentItem, function(it) { if (!hit && it.objectName === name) hit = it })
        return hit
    }
    function walk(it, fn) {
        if (!it) return
        fn(it)
        const kids = it.children
        if (kids) for (let i = 0; i < kids.length; ++i) win.walk(kids[i], fn)
    }
    function pose() {
        win.walk(pageLoader.item, function(it) { if (it.galleryPose === true && typeof it.pose === "function") it.pose() })
    }
    function report() {
        const r = { page: win.page, ready: win.pageReady, keys: 0, icons: 0, iconsKnown: 0, unknownIcons: [], shim: [] }
        win.walk(pageLoader.item, function(it) {
            if (it.effectiveFill !== undefined && it.tokens !== undefined) ++r.keys   // a PButton (or one built on it)
            if (it.galleryIconName !== undefined) {
                ++r.icons
                if (BtnTokens.icons[it.galleryIconName] !== undefined) ++r.iconsKnown
                else r.unknownIcons.push(it.galleryIconName)
            }
            if (it.galleryDelta !== undefined) r.shim.push({ name: it.galleryName, dw: it.galleryDelta, dh: it.galleryDeltaH })
        })
        r.iconsTotal = Object.keys(BtnTokens.icons).length
        r.pageSize = [Math.ceil(pageLoader.width), Math.ceil(pageLoader.height)]
        return r
    }
    function buildBench(kind, count) {
        return (kind === "icon" ? benchIconGrid : kind === "legacy" ? benchLegacyGrid : kind === "shim" ? benchShimGrid
                : benchGlyphGrid).createObject(benchHost, { count: count })
    }
    function destroyBench(o) { if (o) o.destroy() }

    // --- toolbar (interactive only) ----------------------------------------------------------------------------------------
    Rectangle {
        id: bar
        visible: !win.shooting
        width: win.width; height: win.shooting ? 0 : 44
        color: BtnTokens.hex("bg0")
        Row {
            x: 12; anchors.verticalCenter: parent.verticalCenter; spacing: 10
            ChipGroup {
                anchors.verticalCenter: parent.verticalCenter
                size: "s"; role: "primary"; title: "Page"
                model: win.pages
                selected: win.page
                onPicked: function(id) { win.page = id }
            }
            CycleButton {
                anchors.verticalCenter: parent.verticalCenter
                size: "s"; values: [1, 1.25, 1.5, 2]; labels: ["100 %", "125 %", "150 %", "200 %"]
                index: Math.max(0, [1, 1.25, 1.5, 2].indexOf(win.zoom))
                tip: "Zoom (the app's UI scale)"
                onStepped: function(i, v) { win.zoom = v }
            }
            ToggleButton {
                anchors.verticalCenter: parent.verticalCenter
                size: "s"; text: "GEOMETRY ICONS"; icon.name: "layers"
                tip: "Icon renderer: off = CurveRenderer (default), on = the GeometryRenderer fallback in a 4x MSAA layer"
                checked: BtnTokens.iconRenderer === "geometry"
                onFired: BtnTokens.iconRenderer = checked ? "geometry" : "curve"
            }
            PButton {
                anchors.verticalCenter: parent.verticalCenter
                size: "s"; text: "POSE"; icon.name: "refresh"
                tip: "Put the posed keys back into hover / pressed / armed / hold / error"
                onFired: win.pose()
            }
        }
    }

    Flickable {
        id: flick
        y: bar.height
        width: win.width; height: win.height - bar.height
        clip: true
        interactive: !win.shooting
        contentWidth: holder.width; contentHeight: holder.height
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { visible: !win.shooting }
        ScrollBar.horizontal: ScrollBar { visible: !win.shooting }
        Item {
            id: holder
            width: win.contentSize.width; height: win.contentSize.height
            Rectangle { anchors.fill: parent; color: BtnTokens.hex("bg1") }
            Loader {
                id: pageLoader
                scale: win.zoom; transformOrigin: Item.TopLeft
                sourceComponent: win.page !== "" ? win.pageComponents[win.page] : null
                onLoaded: if (!win.shooting) Qt.callLater(win.pose)
            }
        }
    }
    Binding { when: win.shooting; target: flick; property: "contentX"; value: win.scrollX }
    Binding { when: win.shooting; target: flick; property: "contentY"; value: win.scrollY }

    // --bench grids are created here, over everything.
    Item { id: benchHost; anchors.fill: parent; z: 10 }

    onPageChanged: { flick.contentX = 0; flick.contentY = 0; if (BtnTokens.confirming) BtnTokens.confirming.cancel() }
    // The editor page's popup lives in the window overlay: close it with the page (it is created per page).

    // --- building blocks ---------------------------------------------------------------------------------------------------
    component Caption: Text { color: BtnTokens.hex("text2"); font.family: "Segoe UI"; font.pixelSize: 10 }
    component Heading: Text { color: BtnTokens.hex("text0"); font.family: BtnTokens.fontDisplay; font.pixelSize: 15; font.bold: true }
    component Section: Text { color: BtnTokens.hex("text1"); font.family: BtnTokens.fontDisplay; font.pixelSize: 12; font.bold: true; topPadding: 8 }

    // A PButton put into one state of the matrix. `st` is one of win.stateList.
    component StateKey: PButton {
        id: sk
        property string st: "rest"
        readonly property bool galleryPose: true
        active: sk.st === "lit"
        enabled: sk.st !== "disabled"
        busy: sk.st === "busy"
        led: sk.st === "led" ? (({ stop: "red", destructive: "red", go: "green", warn: "amber", info: "blue", primary: "blue",
                                    cued: "amber" })[sk.role] || "green") : ""
        safety: sk.st === "armed" ? "arm" : sk.st === "hold" ? "hold" : "none"
        armMs: 600000
        ToolTip.visible: !galleryHost.shooting && sk.hovered && sk.tipText !== ""
        function pose() {
            if (sk.st === "hover") galleryHost.hover(sk)
            else if (sk.st === "pressed") sk.down = true
            else if (sk.st === "armed") { sk._armLeft = 0.6; sk._armed = true }
            else if (sk.st === "hold") sk._holdProgress = 0.55
            else if (sk.st === "error") { sk._error = false; sk._error = true }
        }
    }

    // Keys posed armed through _armed. PButton shows the shared ToolTip for an armed key; it lives in the window overlay, which
    // does not scroll with the tiles, so in a shot it would land as a stray box at a tile seam: no tips while shooting.
    component PosedBig: BigActionButton {
        id: pbk
        readonly property bool galleryPose: true
        ToolTip.visible: !galleryHost.shooting && ((pbk.hovered && pbk.tipText !== "") || pbk.armed)
    }
    component PosedPad: PadButton {
        id: ppk
        readonly property bool galleryPose: true
        ToolTip.visible: !galleryHost.shooting && ((ppk.hovered && ppk.tipText !== "") || ppk.armed)
    }

    // One icon at the four token sizes (xs 12, m 15, l 20, xl 22) and 24, its name under it.
    component IconCell: Rectangle {
        id: cell
        property string name: ""
        readonly property string galleryIconName: cell.name
        width: 132; height: 58; radius: 4
        color: BtnTokens.hex("bg2"); border.color: BtnTokens.hex("border0")
        Row {
            x: 8; y: 6; spacing: 7
            Icon { name: cell.name; size: 12; color: BtnTokens.hex("text0"); anchors.bottom: parent.bottom }
            Icon { name: cell.name; size: 15; color: BtnTokens.hex("text0"); anchors.bottom: parent.bottom }
            Icon { name: cell.name; size: 20; color: BtnTokens.hex("text0"); anchors.bottom: parent.bottom }
            Icon { name: cell.name; size: 24; color: BtnTokens.hex("accent0"); anchors.bottom: parent.bottom }
        }
        Text {
            x: 8; y: 40; width: cell.width - 12; elide: Text.ElideRight
            text: cell.name; color: BtnTokens.icons[cell.name] !== undefined ? BtnTokens.hex("text2") : BtnTokens.hex("danger")
            font.family: "Cascadia Mono, Consolas"; font.pixelSize: 9
        }
    }

    // --- pages -------------------------------------------------------------------------------------------------------------
    Component {
        id: matrixPage
        Column {
            id: mp
            padding: 16; spacing: 6
            Heading { text: "PButton · every role × size × state" }
            Caption { text: "hover, pressed, armed, hold and error are posed (see gallery.qml); lit = active, led = the role's LED; armed keys show 60 % of the window left, hold 55 %." }
            Repeater {
                model: BtnTokens.sizeOrder
                Column {
                    id: sizeBlock
                    readonly property string sz: modelData
                    readonly property var tok: BtnTokens.size(sizeBlock.sz)
                    spacing: 4
                    Section { text: "size " + sizeBlock.sz + " · " + sizeBlock.tok.h + " px · " + sizeBlock.tok.layout + " · icon " + sizeBlock.tok.icon }
                    Grid {
                        readonly property int cols: win.stateList.length + 1
                        columns: cols; rowSpacing: 6; columnSpacing: 8
                        verticalItemAlignment: Grid.AlignVCenter
                        Repeater {
                            model: (BtnTokens.roleOrder.length + 1) * parent.cols
                            Loader {
                                id: cell
                                readonly property int rr: Math.floor(index / (win.stateList.length + 1))
                                readonly property int cc: index % (win.stateList.length + 1)
                                readonly property string role: cell.rr > 0 ? BtnTokens.roleOrder[cell.rr - 1] : ""
                                sourceComponent: cell.rr === 0 ? head : cell.cc === 0 ? roleCap : key
                                Component { id: head; Caption { text: cell.cc === 0 ? "role \\ state" : win.stateList[cell.cc - 1]; font.bold: true } }
                                Component { id: roleCap; Caption { text: cell.role; width: 70 } }
                                Component {
                                    id: key
                                    StateKey {
                                        role: cell.role; size: sizeBlock.sz; st: win.stateList[cell.cc - 1]
                                        text: win.sample[cell.role][0]; icon.name: win.sample[cell.role][1]
                                        sublabel: sizeBlock.tok.layout === "stacked" ? "main" : ""
                                        tip: BtnTokens.roles[cell.role].label
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    Component {
        id: fillsPage
        Column {
            id: fp
            padding: 16; spacing: 6
            readonly property var cols: [
                { f: "auto", st: "rest" }, { f: "solid", st: "rest" }, { f: "solid", st: "hover" }, { f: "solid", st: "pressed" },
                { f: "tonal", st: "rest" }, { f: "tonal", st: "hover" }, { f: "tonal", st: "pressed" },
                { f: "outline", st: "rest" }, { f: "outline", st: "hover" }, { f: "outline", st: "pressed" },
                { f: "ghost", st: "rest" }, { f: "ghost", st: "hover" }, { f: "ghost", st: "pressed" },
                { f: "auto", st: "lit" }, { f: "ghost", st: "disabled" }, { f: "outline", st: "disabled" }
            ]
            Heading { text: "Fills · every role × fill (size s), with hover and press" }
            Caption { text: "auto resolves to " + JSON.stringify(BtnTokens.autoFill) + "; lit is tonal at litA with a 2 px border whatever the fill." }
            Repeater {
                model: ["label", "icon"]
                Column {
                    id: fblock
                    readonly property bool iconOnly: modelData === "icon"
                    spacing: 4
                    Section { text: fblock.iconOnly ? "icon only (IconButton, m)" : "label + icon (s)" }
                    Grid {
                        columns: fp.cols.length + 1; rowSpacing: 6; columnSpacing: 6
                        verticalItemAlignment: Grid.AlignVCenter
                        Repeater {
                            model: (BtnTokens.roleOrder.length + 1) * (fp.cols.length + 1)
                            Loader {
                                id: fcell
                                readonly property int rr: Math.floor(index / (fp.cols.length + 1))
                                readonly property int cc: index % (fp.cols.length + 1)
                                readonly property string role: fcell.rr > 0 ? BtnTokens.roleOrder[fcell.rr - 1] : ""
                                readonly property var col: fcell.cc > 0 ? fp.cols[fcell.cc - 1] : null
                                sourceComponent: fcell.rr === 0 ? fhead : fcell.cc === 0 ? frole : fblock.iconOnly ? fikey : fkey
                                Component { id: fhead; Caption { text: fcell.cc === 0 ? "role \\ fill" : fcell.col.f + (fcell.col.st !== "rest" ? "\n" + fcell.col.st : ""); font.bold: true } }
                                Component { id: frole; Caption { text: fcell.role; width: 70 } }
                                Component {
                                    id: fkey
                                    StateKey { role: fcell.role; fill: fcell.col.f; st: fcell.col.st; size: "s"
                                               text: win.sample[fcell.role][0]; icon.name: win.sample[fcell.role][1] }
                                }
                                Component {
                                    id: fikey
                                    StateKey { role: fcell.role; fill: fcell.col.f; st: fcell.col.st; size: "m"
                                               implicitWidth: implicitHeight
                                               icon.name: win.sample[fcell.role][1]; tip: win.sample[fcell.role][0] }
                                }
                            }
                        }
                    }
                }
            }
            Section { text: "customColor (the user swatches, solid and lit)" }
            Flow {
                width: 1300; spacing: 6
                Repeater {
                    model: Spec.swatches
                    PButton { size: "s"; text: modelData; customColor: modelData; role: "primary"; fill: "solid" }
                }
                Repeater {
                    model: Spec.swatches.slice(0, 6)
                    PButton { size: "s"; text: "LIT"; customColor: modelData; active: true; icon.name: "star" }
                }
            }
        }
    }

    Component {
        id: compositesPage
        Column {
            padding: 16; spacing: 6
            Heading { text: "Composite keys" }
            Section { text: "IconButton · every size × role (tip = accessible name)" }
            Grid {
                columns: BtnTokens.roleOrder.length + 1; rowSpacing: 6; columnSpacing: 8
                verticalItemAlignment: Grid.AlignVCenter
                Repeater {
                    model: BtnTokens.sizeOrder.length * (BtnTokens.roleOrder.length + 1)
                    Loader {
                        id: icell
                        readonly property int rr: Math.floor(index / (BtnTokens.roleOrder.length + 1))
                        readonly property int cc: index % (BtnTokens.roleOrder.length + 1)
                        readonly property string sz: BtnTokens.sizeOrder[icell.rr]
                        readonly property string role: icell.cc > 0 ? BtnTokens.roleOrder[icell.cc - 1] : ""
                        sourceComponent: icell.cc === 0 ? icap : ikey
                        Component { id: icap; Caption { text: icell.sz; width: 30 } }
                        Component { id: ikey; IconButton { size: icell.sz; role: icell.role; icon.name: win.sample[icell.role][1]; tip: icell.role } }
                    }
                }
            }
            Section { text: "PButton extras: hotkey tip, badge, iconRight, sublabel, LED blink, segments, keyboard focus ring (posed)" }
            Row {
                spacing: 8
                PButton { text: "CUT"; icon.name: "cut"; role: "stop"; hotkey: "Ctrl+Enter"; tip: "Hard cut" }
                PButton { text: "CAM 2"; icon.name: "camera"; badge: "CG1·PTZ2" }
                PButton { text: "LOAD HOUR"; icon.name: "load-hour"; iconRight: "caret-down" }
                PButton { text: "STATUS"; badge: "WARN"; badgeColor: BtnTokens.hex("warn") }
                PButton { size: "l"; text: "SCHEDULE"; sublabel: "AUTO"; icon.name: "calendar-grid"; led: "green"; ledBlink: "slow" }
                PButton { size: "l"; text: "PREVIEW"; sublabel: "CAM 3"; icon.name: "preview"; led: "amber"; ledBlink: "fast"; badge: "PTZ1" }
                Row {
                    PButton { segment: "left"; text: "A"; size: "s" }
                    PButton { segment: "middle"; text: "B"; size: "s"; active: true }
                    PButton { segment: "right"; text: "C"; size: "s" }
                }
                PButton {
                    id: focusKey
                    readonly property bool galleryPose: true
                    text: "FOCUSED"; icon.name: "key"; tip: "Keyboard focus ring (Tab)"
                    function pose() { focusKey.forceActiveFocus(Qt.TabFocusReason) }
                }
            }
            Section { text: "ToggleButton (off / on, litRole) · DisclosureButton · CycleButton" }
            Row {
                spacing: 8
                ToggleButton { text: "AFV"; icon.name: "afv" }
                ToggleButton { text: "AFV"; icon.name: "afv"; checked: true }
                ToggleButton { text: "SOLO"; checked: true; role: "warn" }
                ToggleButton { text: "MUTE"; icon.name: "speaker-off"; checked: true; role: "stop" }
                ToggleButton { size: "s"; text: "LOOP"; icon.name: "loop"; checked: true; litRole: "go" }
                ToggleButton { size: "s"; text: "LOCK"; icon.name: "lock"; enabled: false }
                DisclosureButton { text: "AUDIO MIXER"; status: "SOLO · AFV OFF" }
                DisclosureButton { text: "PTZ"; checked: true }
                DisclosureButton { text: "PTZ"; size: "m"; checked: true; status: "SOLO" }
                CycleButton { size: "xs"; values: [0.25, 0.5, 1, 2]; suffix: " s"; index: 1; tip: "Transition time" }
                CycleButton { size: "s"; values: ["mix", "dip", "wipe"]; labels: ["MIX", "DIP", "WIPE"]; tip: "Transition" }
            }
            Section { text: "ChipGroup: single (fillWidth), multi, LEDs and a disabled chip, sizes xs / s / m" }
            Row {
                spacing: 16
                ChipGroup {
                    width: 300; fillWidth: true
                    model: ["mix", "dip", "wipe_left", "push_left", "squeeze"]
                    selected: "dip"
                    labelFn: function(id) { return ({ mix: "MIX", dip: "DIP", wipe_left: "WIPE", push_left: "PUSH", squeeze: "SQZ" })[id] }
                }
                ChipGroup { multi: true; size: "s"; selected: ["main", "aux"]; model: [{ id: "", name: "ALL MIXES" }, { id: "main", name: "MAIN" }, { id: "aux", name: "AUX" }, { id: "web", name: "WEB" }] }
                ChipGroup { role: "primary"; size: "m"; selected: "p1"; model: [{ id: "auto", name: "AUTO" }, { id: "p1", name: "PTZ1", led: "blue" }, { id: "p2", name: "PTZ2", led: "green" }, { id: "p3", name: "PTZ3", enabled: false }] }
            }
            Section { text: "SplitButton s / m / l, armed main (posed), busy" }
            Row {
                spacing: 8
                SplitButton { size: "s"; text: "STORE IN CG"; icon.name: "layers"; role: "info" }
                SplitButton { size: "m"; text: "LOAD HOUR"; icon.name: "load-hour" }
                SplitButton {
                    id: armedSplit
                    readonly property bool galleryPose: true
                    size: "m"; text: "PUSH"; role: "warn"; safety: "arm"; armMs: 600000
                    function pose() { armedSplit.mainButton._armLeft = 0.6; armedSplit.mainButton._armed = true }
                }
                SplitButton { size: "m"; text: "RENDER"; icon.name: "render"; busy: true }
                SplitButton { size: "l"; text: "PUSH"; icon.name: "send"; role: "go" }
            }
            Section { text: "RailButton: channel keys (LED, ON / OFF) and page keys, current page lit" }
            Rectangle {
                width: 52 * 9 + 8; height: 52; color: BtnTokens.hex("bg0"); radius: 4
                Row {
                    x: 8; anchors.verticalCenter: parent.verticalCenter; spacing: 8
                    RailButton { text: "CH1"; sublabel: "ON"; led: "green"; checked: true; tip: "main" }
                    RailButton { text: "CH2"; sublabel: "OFF"; tip: "ch2" }
                    RailButton { icon.name: "broadcast"; tip: "On Air"; checked: true }
                    RailButton { icon.name: "calendar-grid"; tip: "Schedule" }
                    RailButton { icon.name: "timeline"; tip: "Timeline" }
                    RailButton { icon.name: "report"; tip: "Reports" }
                    RailButton { icon.name: "remote"; tip: "Remote" }
                    RailButton { icon.name: "output"; tip: "Output" }
                    RailButton { icon.name: "sidebar-collapse"; tip: "Collapse"; autoToggle: true }
                }
            }
        }
    }

    Component {
        id: keysPage
        Column {
            padding: 16; spacing: 6
            Heading { text: "On-air keys" }
            Section { text: "BigActionButton l: tally pgm / pvw, sub, LED, armed (dial), held (dial), disabled, busy, stateful off" }
            Row {
                spacing: 8
                BigActionButton { actionId: "bus.cut"; tally: "pgm"; width: 74 }
                BigActionButton { text: "PGM"; icon.name: "program"; tally: "pgm"; width: 74 }
                BigActionButton { actionId: "bus.auto"; active: true; tally: "pvw"; width: 74 }
                BigActionButton { actionId: "bus.auto"; sublabel: "MIX 0.5 s"; width: 74 }
                BigActionButton { actionId: "schedule.return"; led: "amber"; width: 74 }
                BigActionButton { actionId: "bus.take"; tally: "pvw"; width: 74 }
                PosedBig { id: armedL; actionId: "transport.stop"; width: 74; armMs: 5000
                                  function pose() { armedL._armLeft = 0.6; armedL._armed = true } }
                BigActionButton { id: heldL; readonly property bool galleryPose: true; actionId: "live.end"; width: 84
                                  function pose() { heldL._holdProgress = 0.45 } }
                BigActionButton { actionId: "transport.hold"; enabled: false; width: 74 }
                BigActionButton { actionId: "automation.snap"; busy: true; width: 74 }
                BigActionButton { actionId: "automation.arm"; actionOn: false; width: 84 }
            }
            Section { text: "BigActionButton xl: TAKE pgm, GO LIVE armed (amber key), END LIVE held; m row size armed; an icon-less armed key counts in its line" }
            Row {
                spacing: 8
                BigActionButton { actionId: "bus.take"; size: "xl"; tally: "pgm" }
                PosedBig { id: armedX; actionId: "live.start"; size: "xl"; customColor: "#ff9500"; armMs: 5000
                                  function pose() { armedX._armLeft = 0.6; armedX._armed = true } }
                BigActionButton { id: heldX; readonly property bool galleryPose: true; actionId: "live.end"; size: "xl"
                                  function pose() { heldX._holdProgress = 0.7 } }
                PosedBig { id: armedM; actionId: "cg.emergency"; size: "m"; armMs: 5000
                                  anchors.verticalCenter: parent.verticalCenter
                                  function pose() { armedM._armLeft = 0.6; armedM._armed = true } }
                PosedBig { id: armedNoIcon; text: "END STREAM"; role: "stop"; safety: "arm"; size: "xl"; armMs: 5000
                                  function pose() { armedNoIcon._armLeft = 0.6; armedNoIcon._armed = true } }
            }
            Section { text: "PadButton: studio roles, daylight neutral (dark ink), span 2, edit selected, armed confirm, busy, m" }
            Flow {
                width: 1240; spacing: 8
                PadButton { text: "SKIP"; sub: "local · main · F9"; fillColor: "#33445a"; icon.name: "skip-next" }
                PadButton { text: "GO LIVE"; sub: "local · main"; fillColor: "#1f9d55"; icon.name: "broadcast" }
                PadButton { text: "STOP"; sub: "studio-b · ch2"; fillColor: "#d63b3b"; icon.name: "stop" }
                PadButton { text: "HOLD SLATE"; sub: "local"; fillColor: "#d9922b"; icon.name: "hold" }
                PadButton { text: "STATUS"; sub: "daylight"; fillColor: "#d5dce6" }
                PadButton { text: "PUSH THIS HOUR NOW"; sub: "local · main · Ctrl+Shift+P"; fillColor: "#2f7fd1"; span: 2; icon.name: "load-hour" }
                PadButton { text: "SKIP"; sub: "edit mode"; fillColor: "#33445a"; editMode: true; selected: true; icon.name: "skip-next" }
                PosedPad { id: armedPad; text: "END STREAM"; sub: "local"; fillColor: "#d63b3b"; confirm: true; armMs: 5000
                            function pose() { armedPad._armLeft = 0.6; armedPad._armed = true } }
                PadButton { text: "SKIP"; sub: "busy"; fillColor: "#33445a"; busy: true; icon.name: "skip-next" }
                PadButton { text: "NEXT"; size: "m"; fillColor: "#2f7fd1"; icon.name: "skip-next" }
            }
            Section { text: "PtzPad m (zoom column, Up held), 46 × 38 keys (zoom row), s disabled; HoldButton s / m (s held)" }
            Row {
                spacing: 24
                PtzPad {
                    id: ptz1
                    readonly property bool galleryPose: true
                    function pose() {
                        win.walk(ptz1, function(it) { if (it.objectName === "ptz-n") it._holding = true })
                    }
                }
                PtzPad { zoomLayout: "row"; keyWidth: 46; keyHeight: 38 }
                PtzPad { size: "s"; zoom: false; enabled: false }
                Column {
                    spacing: 6
                    HoldButton { id: held1; readonly property bool galleryPose: true; text: "NUDGE"; icon.name: "chevron-right"; size: "s"
                                 function pose() { held1._holding = true } }
                    HoldButton { text: "JOG"; icon.name: "frame-fwd" }
                    HoldButton { text: "REPEAT"; icon.name: "refresh"; repeatMs: 250; size: "l" }
                }
            }
        }
    }

    Component {
        id: iconsPage
        Column {
            id: ip
            padding: 16; spacing: 6
            readonly property var grouped: {
                const s = {}
                for (const g of Spec.iconGroups) for (const n of g.icons) s[n] = true
                return s
            }
            readonly property var ungrouped: Object.keys(BtnTokens.icons).filter(n => !ip.grouped[n])
            Heading { text: "Icons · " + Object.keys(BtnTokens.icons).length + " in-house (assets/ui/icons.json), at 12 / 15 / 20 / 24 px" }
            Caption { text: "24-unit grid, round caps and joins; renderer " + BtnTokens.iconRenderer + ". A red name is listed in a group but missing from the set." }
            Repeater {
                model: Spec.iconGroups
                Column {
                    spacing: 4
                    Section { text: modelData.label + " · " + modelData.icons.length }
                    Flow {
                        width: 1360; spacing: 6
                        Repeater { model: modelData.icons; IconCell { name: modelData } }
                    }
                }
            }
            Section { visible: ip.ungrouped.length > 0; text: "Not in a group · " + ip.ungrouped.length }
            Flow {
                width: 1360; spacing: 6; visible: ip.ungrouped.length > 0
                Repeater { model: ip.ungrouped; IconCell { name: modelData } }
            }
            Section { text: "Renderer check: transport and switcher at 15 px, CurveRenderer (top) and the GeometryRenderer fallback (bottom)" }
            Repeater {
                model: ["curve", "geometry"]
                Row {
                    id: rrow
                    readonly property string rend: modelData
                    spacing: 10
                    Caption { text: rrow.rend; width: 60; anchors.verticalCenter: parent.verticalCenter }
                    Repeater {
                        model: Spec.iconGroups[0].icons.concat(Spec.iconGroups[1].icons)
                        Icon { name: modelData; size: 15; renderer: rrow.rend; color: BtnTokens.hex("text0") }
                    }
                }
            }
        }
    }

    Component {
        id: shimPage
        Column {
            id: sp
            padding: 16; spacing: 6
            readonly property var variants: Object.keys(Spec.legacyVariants)
            readonly property var cases: [
                { t: "LOAD", g: "", small: false }, { t: "PLAY", g: "▶", small: false }, { t: "", g: "✕", small: false },
                { t: "LOAD", g: "", small: true }, { t: "PLAY", g: "▶", small: true }, { t: "", g: "✕", small: true },
                { t: "+ Add node", g: "", small: true }, { t: "↻", g: "", small: true }
            ]
            Heading { text: "Shim panel · today's CButton / DeckButton / CToggle (frozen copies, tests/qml/buttons/legacy) next to the UXB-5 shims in Play_Play_Native/qml/ui" }
            Caption { text: "Each cell: the legacy key, the shim with the same properties, and Δw = legacy − shim implicit width (red when |Δ| > 2 px, the UXB-5 budget)." }
            Grid {
                columns: sp.cases.length + 1; rowSpacing: 6; columnSpacing: 14
                verticalItemAlignment: Grid.AlignVCenter
                Repeater {
                    model: (sp.variants.length + 3) * (sp.cases.length + 1)
                    Loader {
                        id: scell
                        readonly property int rr: Math.floor(index / (sp.cases.length + 1))
                        readonly property int cc: index % (sp.cases.length + 1)
                        // rows: header, the variants, then "default active" and "primary disabled"
                        readonly property string variant: scell.rr === 0 ? "" : scell.rr <= sp.variants.length ? sp.variants[scell.rr - 1]
                                                         : scell.rr === sp.variants.length + 1 ? "default" : "primary"
                        readonly property bool isActive: scell.rr === sp.variants.length + 1
                        readonly property bool isDisabled: scell.rr === sp.variants.length + 2
                        readonly property var cs: scell.cc > 0 ? sp.cases[scell.cc - 1] : null
                        sourceComponent: scell.rr === 0 ? shead : scell.cc === 0 ? srow : spair
                        Component {
                            id: shead
                            Caption { font.bold: true
                                      text: scell.cc === 0 ? "variant" : (scell.cs.g !== "" ? scell.cs.g + " " : "") + (scell.cs.t !== "" ? scell.cs.t : "(glyph only)") + (scell.cs.small ? " · small" : "") }
                        }
                        Component { id: srow; Caption { width: 96; text: scell.variant + (scell.isActive ? " active" : scell.isDisabled ? " disabled" : "") } }
                        Component {
                            id: spair
                            Row {
                                spacing: 4
                                Legacy.CButton {
                                    id: lg
                                    variant: scell.variant; small: scell.cs.small; text: scell.cs.t; icon2: scell.cs.g
                                    active: scell.isActive; enabled: !scell.isDisabled
                                }
                                Shim.CButton {
                                    id: mk
                                    variant: scell.variant; small: scell.cs.small; text: scell.cs.t; icon2: scell.cs.g
                                    active: scell.isActive; enabled: !scell.isDisabled
                                }
                                Text {
                                    readonly property string galleryName: "CButton " + scell.variant + (scell.isActive ? " active" : scell.isDisabled ? " disabled" : "")
                                                                          + " [" + scell.cs.g + scell.cs.t + (scell.cs.small ? " small" : "") + "]"
                                    readonly property real galleryDelta: Math.round(lg.implicitWidth - mk.implicitWidth)
                                    readonly property real galleryDeltaH: Math.round(lg.implicitHeight - mk.implicitHeight)
                                    width: 34; anchors.verticalCenter: parent.verticalCenter
                                    text: (galleryDelta > 0 ? "+" : "") + galleryDelta + (galleryDeltaH !== 0 ? "\nh" + galleryDeltaH : "")
                                    color: Math.abs(galleryDelta) > 2 || galleryDeltaH !== 0 ? BtnTokens.hex("danger") : BtnTokens.hex("text2")
                                    font.family: "Cascadia Mono, Consolas"; font.pixelSize: 10
                                }
                            }
                        }
                    }
                }
            }
            Section { text: "DeckButton (74 × 52) next to its shim over BigActionButton l" }
            Row {
                spacing: 18
                Repeater {
                    model: [["▶", "PLAY", "success", false], ["⏸", "PAUSE", "warn", false], ["■", "STOP", "danger", false],
                            ["⏭", "NEXT", "default", false], ["⚡", "ARMED", "primary", true], ["⏰", "TOH", "info", false]]
                    Row {
                        id: drow
                        spacing: 4
                        Legacy.DeckButton { id: dk; glyph: modelData[0]; text: modelData[1]; variant: modelData[2]; active: modelData[3] }
                        Shim.DeckButton { id: bk; glyph: modelData[0]; text: modelData[1]; variant: modelData[2]; active: modelData[3] }
                        Text {
                            readonly property string galleryName: "DeckButton " + modelData[1]
                            readonly property real galleryDelta: Math.round(dk.implicitWidth - bk.implicitWidth)
                            readonly property real galleryDeltaH: Math.round(dk.implicitHeight - bk.implicitHeight)
                            anchors.verticalCenter: parent.verticalCenter
                            text: "Δw " + galleryDelta + "\nΔh " + galleryDeltaH
                            color: BtnTokens.hex("text2"); font.family: "Cascadia Mono, Consolas"; font.pixelSize: 10
                        }
                    }
                }
            }
            Section { text: "CToggle (switch row): today's, the shim (rest, disabled, keyboard focus), and ToggleButton" }
            Row {
                spacing: 18
                Legacy.CToggle { label: "Auto-advance"; checked: true; width: 200 }
                Legacy.CToggle { label: "Loop playlist"; hint: "Repeat at the end"; checked: false; width: 200 }
                Shim.CToggle { label: "Auto-advance"; checked: true; width: 200 }
                Shim.CToggle { label: "Loop playlist"; hint: "Repeat at the end"; checked: false; width: 200 }
                Shim.CToggle { label: "Disabled"; checked: true; enabled: false; width: 160 }
                Shim.CToggle {
                    id: focusedToggle
                    readonly property bool galleryPose: true
                    function pose() { focusedToggle.forceActiveFocus(Qt.TabFocusReason) }
                    label: "Tab focus"; width: 160
                }
                ToggleButton { text: "AUTO-ADVANCE"; checked: true; anchors.verticalCenter: parent.verticalCenter }
                ToggleButton { text: "LOOP"; icon.name: "loop"; anchors.verticalCenter: parent.verticalCenter }
            }
        }
    }

    Component {
        id: popupPage
        Item {
            id: pp
            readonly property bool galleryPose: true
            implicitWidth: 820; implicitHeight: 420
            Column {
                x: 16; y: 16; spacing: 8
                Heading { text: "ConfirmPopup · a modal key's question (Enter confirms, Esc cancels), one per window" }
                Caption { text: "Click a key to ask again." }
                Row {
                    spacing: 8
                    PButton { id: delKey; text: "DELETE CHANNEL"; icon.name: "trash"; role: "destructive"; safety: "modal"
                              confirmText: "Delete channel main? Its engine stops." }
                    PButton { text: "RESTART MUX"; icon.name: "reload"; role: "warn"; safety: "modal"; confirmText: "Restart the mux? Output drops for a few seconds." }
                }
            }
            function pose() {
                if (BtnTokens.confirming) return
                const p = BtnTokens.confirmPopup(delKey.Overlay.overlay)
                if (!p) return
                p.scale = win.zoom
                p.ask("Delete channel", "Delete channel main? Its engine stops.", "DELETE", "destructive", function() {}, delKey)
            }
        }
    }

    // UXB-8: the button editor open on a deck key, with a customised draft (label, colour, hotkey) so every field shows a
    // value, and the hotkey message posed to the "taken by" conflict. ButtonPrefs is local here (no backend): the Apply
    // writes to the singleton and is never saved.
    Component {
        id: editorPage
        Item {
            id: ep
            readonly property bool galleryPose: true
            implicitWidth: 820; implicitHeight: 780
            Column {
                x: 16; y: 16; spacing: 8
                Heading { text: "ButtonEditor · label, icon, colour, size, hotkey, safety, visibility, reset (UXB-8)" }
                Caption { text: "Right-click a deck key in the app opens it; here the STOP key with a draft. The hotkey field shows a conflict with the switch row's CUT." }
                Row {
                    spacing: 8
                    BigActionButton { id: editKey; actionId: "transport.stop"; width: 74; safety: "none"; onFired: ep.pose() }
                    BigActionButton { actionId: "transport.next"; width: 74; safety: "none"; onFired: ep.openFor("transport.next") }
                }
            }
            ButtonEditor {
                id: galleryEditor
                parent: editKey.Overlay.overlay
                autoSave: false
                closePolicy: Popup.CloseOnEscape
            }
            function openFor(id) {
                const d = BtnTokens.actionDefaults(id, true)
                galleryEditor.scale = win.zoom
                galleryEditor.edit("onair.deck", id, d)
            }
            function pose() {
                if (galleryEditor.opened) return
                ButtonPrefs.setButton("onair.switch", "bus.cut", { hotkey: "Ctrl+Shift+S" })
                ButtonPrefs.setButton("onair.deck", "transport.stop", { label: "HALT", color: "#ff9500", hotkey: "F9", size: "xl" })
                ep.openFor("transport.stop")
                const hk = win.findNamed(galleryEditor, "editorHotkey")
                if (hk) hk.take("Ctrl+Shift+S")        // refused: "taken by onair.switch/bus.cut"
            }
            Component.onDestruction: if (galleryEditor.opened) galleryEditor.close()
        }
    }

    // --- bench grids (--bench): 500 icon keys against the same grid of text-glyph keys, and legacy CButtons ----------------
    Component {
        id: benchIconGrid
        Grid {
            property int count: 500
            x: 8; y: 8; columns: 25; spacing: 4
            Repeater {
                model: parent.count
                IconButton { icon.name: win.benchIcons[index % win.benchIcons.length]; tip: icon.name }
            }
        }
    }
    Component {
        id: benchGlyphGrid
        Grid {
            property int count: 500
            x: 8; y: 8; columns: 25; spacing: 4
            Repeater {
                model: parent.count
                PButton { text: win.benchGlyphs[index % win.benchGlyphs.length]; tip: "glyph"; implicitWidth: implicitHeight }
            }
        }
    }
    // The reference: today's glyph key (a CButton with a text glyph), the one UXB-5 replaces. Reported, not gated.
    Component {
        id: benchLegacyGrid
        Grid {
            property int count: 500
            x: 8; y: 8; columns: 25; spacing: 4
            Repeater {
                model: parent.count
                Legacy.CButton { icon2: win.benchGlyphs[index % win.benchGlyphs.length]; implicitWidth: implicitHeight }
            }
        }
    }
    // The UXB-5 shim of the same key (CButton over PButton, the glyph mapped to an icon). Reported, not gated.
    Component {
        id: benchShimGrid
        Grid {
            property int count: 500
            x: 8; y: 8; columns: 25; spacing: 4
            Repeater {
                model: parent.count
                Shim.CButton { icon2: win.benchGlyphs[index % win.benchGlyphs.length]; implicitWidth: implicitHeight }
            }
        }
    }
}
