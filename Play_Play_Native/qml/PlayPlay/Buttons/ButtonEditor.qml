pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import "spec.js" as Spec

// ButtonEditor: the popup that customises one key of a surface (docs/plans/ux-buttons.md 3.8, UXB-8): label, icon
// (IconPicker), colour (role chips, SwatchPicker, a hex field), size (the sizes the surface offers), hotkey (HotkeyField,
// with the "taken by" message), safety (the levels below the key's floor disabled and the floor named), visibility on a
// deck, Reset key and Reset surface. A live preview draws the key as the deck will.
//   editor.edit("onair.deck", "transport.stop", acts.get("transport.stop"))   // defaults: {label, icon, role, color, size,
//                                                                            //   hotkey, safety, floor, tip} (Actions.get /
//                                                                            //   a bus key's title ...)
// The edit is a draft: nothing reaches ButtonPrefs until Apply (setButton / setHidden, then save() when a backend is
// attached and autoSave is on); Cancel / Esc drop it. Reset key and Reset surface write at once (resetButton /
// resetSurface + save) and the editor reloads its draft. applied(surface, id) / cancelled() tell the owner.
// A draft field holds an override; a missing field means "the default". The preview merges the draft over the defaults the
// way ButtonPrefs.resolve() does (safety never below the floor).
// No supervisor / api / window access of its own: Qt Quick Test loads it alone; the app passes `backend` (its api) or
// has already attached ButtonPrefs through ActionDeck.
Popup {
    id: ed
    objectName: "buttonEditor"

    property string surface: ""
    property string keyId: ""
    property var defaults: ({})
    property var draft: ({})
    property bool keyVisible: true
    property var backend: null             // {get, put}: ButtonPrefs.attach() when it has none yet
    property bool autoSave: true
    property string title: "Customize key"
    readonly property bool deck: { const s = ButtonPrefs.surfaceOf(ed.surface); return s !== null && s.deck === true }
    readonly property var sizes: ButtonPrefs.sizesOf(ed.surface)
    readonly property string floor: ed.defaults && ed.defaults.floor !== undefined && ButtonPrefs.safetyIndex(ed.defaults.floor) >= 0
                                    ? ed.defaults.floor : "none"
    readonly property int floorIndex: ButtonPrefs.safetyIndex(ed.floor)
    // The key as the deck will draw it: the defaults with the draft over them. `custom` names the overridden fields.
    readonly property var preview: ed._merge(ed.defaults, ed.draft)
    readonly property bool dirty: !ed._sameDraft(ed.draft, ed._stored) || ed.keyVisible !== ed._storedVisible
    readonly property string keyName: ed.surface + " / " + ed.keyId

    signal applied(string surface, string id)
    signal cancelled()

    property var _stored: ({})
    property bool _storedVisible: true
    property bool _justApplied: false

    // Field by field (not JSON text: set(f, "") then set(f, old) re-appends the key, and the key order is no change).
    function _sameDraft(a, b) {
        for (const f of ButtonPrefs.fields) if (a[f] !== b[f]) return false
        return true
    }
    // The pickers set their own `value` when clicked (which would drop a `value:` binding), so the editor pushes the
    // draft into them whenever it changes: after edit(), a reset, a role chip, the hex field, a reload under the editor.
    function _syncWidgets() {
        const p = ed._merge(ed.defaults, ed.draft)    // not ed.preview: its binding may run after this handler
        iconPicker.value = p.icon
        swatches.value = p.color
        hotkeyField.value = ed.has("hotkey") ? ed.draft.hotkey : ""
    }
    onDraftChanged: ed._syncWidgets()
    function _merge(d, o) {
        const r = {}
        const def = ButtonPrefs.isObj(d) ? d : {}
        for (const k in def) r[k] = def[k]
        if (r.label === undefined) r.label = ""
        if (r.icon === undefined) r.icon = ""
        if (r.role === undefined) r.role = "neutral"
        if (r.color === undefined) r.color = ""
        if (r.size === undefined || ed.sizes.indexOf(r.size) < 0) r.size = ed.sizes[0]
        if (r.hotkey === undefined) r.hotkey = ""
        r.safety = ButtonPrefs.clampSafety(r.safety === undefined ? "none" : r.safety, ed.floor)
        r.custom = []
        const ov = ButtonPrefs.isObj(o) ? o : {}
        if (typeof ov.label === "string" && ov.label !== "") { r.label = ov.label; r.custom.push("label") }
        if (typeof ov.icon === "string" && ov.icon !== "") { r.icon = ov.icon; r.custom.push("icon") }
        if (ButtonPrefs.colorOk(ov.color)) { r.color = ov.color; r.custom.push("color") }
        if (typeof ov.size === "string" && ed.sizes.indexOf(ov.size) >= 0) { r.size = ov.size; r.custom.push("size") }
        if (typeof ov.hotkey === "string" && ov.hotkey !== "") { r.hotkey = ov.hotkey; r.custom.push("hotkey") }
        if (typeof ov.safety === "string" && ButtonPrefs.safetyIndex(ov.safety) >= 0) {
            const s = ButtonPrefs.clampSafety(ov.safety, ed.floor)
            if (s !== r.safety) r.custom.push("safety")
            r.safety = s
        }
        return r
    }
    // Start editing surface/id with `defaults`; the draft is the stored overrides.
    function edit(surface, id, defaults) {
        ed.surface = String(surface || "")
        ed.keyId = String(id || "")
        ed.defaults = ButtonPrefs.isObj(defaults) ? defaults : {}
        if (ed.backend && !ButtonPrefs.backend) ButtonPrefs.attach(ed.backend)   // async GET: loadDone below reloads the draft
        ed._reloadAll()
        ed.open()
    }
    // reload() plus the widgets that keep their own text / value.
    function _reloadAll() {
        ed.reload()
        ed._syncWidgets()
        labelInput.text = ed.preview.label
        hexInput.text = ed.preview.color
        hotkeyField.recheck()
    }
    // The prefs answered a load (the attach above, or the reload timer): an untouched draft follows them; an edit wins.
    Connections {
        target: ButtonPrefs
        function onLoadDone(ok) { if (ok && ed.opened && !ed.dirty) ed._reloadAll() }
    }
    // The draft from the stored prefs (after a reset, or when the prefs reloaded under the editor).
    function reload() {
        const o = ButtonPrefs.overrides(ed.surface, ed.keyId)
        ed._stored = ButtonPrefs.clone(o)
        ed.draft = o
        ed._storedVisible = ButtonPrefs.hidden(ed.surface).indexOf(ed.keyId) < 0
        ed.keyVisible = ed._storedVisible
    }
    // Set one draft field; "" / null / undefined drops the override (back to the default).
    function set(field, value) {
        if (ButtonPrefs.fields.indexOf(field) < 0) return
        const d = ButtonPrefs.clone(ed.draft)
        if (value === undefined || value === null || value === "") delete d[field]
        else d[field] = value
        ed.draft = d
    }
    function has(field) { return ed.draft[field] !== undefined }
    // Write the draft: every field given (a missing one as "", which clears it), the visibility on a deck, then save.
    function apply() {
        const patch = {}
        for (const f of ButtonPrefs.fields) patch[f] = ed.draft[f] !== undefined ? ed.draft[f] : ""
        const r = ButtonPrefs.setButton(ed.surface, ed.keyId, patch)
        if (ed.deck && ed.keyVisible !== ed._storedVisible) ButtonPrefs.setHidden(ed.surface, ed.keyId, !ed.keyVisible)
        ed._save()
        ed.reload()
        ed._justApplied = true
        ed.applied(ed.surface, ed.keyId)
        ed.close()
        return r.ok
    }
    function cancel() { ed.close() }
    function resetKey() {
        ButtonPrefs.resetButton(ed.surface, ed.keyId)
        if (ed.deck && ButtonPrefs.hidden(ed.surface).indexOf(ed.keyId) >= 0) ButtonPrefs.setHidden(ed.surface, ed.keyId, false)
        ed._save()
        ed._reloadAll()
    }
    function resetSurface() {
        ButtonPrefs.resetSurface(ed.surface)
        ed._save()
        ed._reloadAll()
    }
    function _save() { if (ed.autoSave && ButtonPrefs.backend) ButtonPrefs.save() }
    function _roleHex(role) { return String(BtnTokens.roleColor(role)).toLowerCase() }

    // Esc, Cancel, the close key or a click outside: the draft is dropped (reload() on the next edit()).
    onClosed: { if (!ed._justApplied) ed.cancelled(); ed._justApplied = false }

    anchors.centerIn: parent
    width: Math.min(560, Math.max(320, (ed.parent ? ed.parent.width : 560) - 32))
    height: Math.min(body.implicitHeight + 2 * ed.padding, (ed.parent ? ed.parent.height : 800) - 24)
    modal: true
    dim: true
    focus: true
    closePolicy: Popup.CloseOnEscape
    padding: 16
    Overlay.modal: Rectangle { objectName: "editorDim"; color: "#000000a0" }
    background: Rectangle {
        color: BtnTokens.hex("bg2")
        border.color: BtnTokens.hex("border1")
        border.width: 1
        radius: 8
    }

    component FieldLabel: Text {
        color: BtnTokens.hex("text2")
        font.family: BtnTokens.fontDisplay; font.pixelSize: 10; font.bold: true; font.letterSpacing: 0.8
    }
    component Note: Text {
        width: parent.width
        wrapMode: Text.Wrap
        color: BtnTokens.hex("text2"); font.family: "Segoe UI"; font.pixelSize: 10
    }
    component Field: Rectangle {
        property alias input: inp
        property alias text: inp.text
        property string placeholder: ""
        signal edited()
        signal done()
        height: 28; radius: 5
        color: BtnTokens.hex("bg3")
        border.width: inp.activeFocus ? 2 : 1
        border.color: inp.activeFocus ? BtnTokens.hex(BtnTokens.states.focusColor) : BtnTokens.hex("border1")
        TextInput {
            id: inp
            anchors.fill: parent; anchors.leftMargin: 9; anchors.rightMargin: 9
            verticalAlignment: TextInput.AlignVCenter
            clip: true
            color: BtnTokens.hex("text0")
            font.family: "Segoe UI"; font.pixelSize: 12
            selectByMouse: true
            onTextEdited: parent.edited()
            onEditingFinished: parent.done()
            Text {
                anchors.fill: parent
                visible: inp.text === "" && !inp.activeFocus
                text: parent.parent.placeholder
                color: BtnTokens.hex("text3"); font: inp.font
                verticalAlignment: Text.AlignVCenter
            }
        }
    }

    contentItem: Flickable {
        id: scroller
        contentWidth: width
        contentHeight: body.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { policy: scroller.contentHeight > scroller.height ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff }

        Column {
            id: body
            width: scroller.width - 12          // clear of the scroll bar
            spacing: 10

            // --- header and preview --------------------------------------------------------------------------------------
            Row {
                width: parent.width
                spacing: 8
                Column {
                    width: parent.width - closeKey.width - 8
                    Text {
                        objectName: "editorTitle"
                        text: ed.title
                        color: BtnTokens.hex("text0"); font.family: BtnTokens.fontDisplay; font.pixelSize: 14; font.bold: true
                    }
                    Text {
                        objectName: "editorKeyName"
                        text: ed.keyName + (ed.preview.custom.length ? "  ·  custom: " + ed.preview.custom.join(", ") : "")
                        color: BtnTokens.hex("text2"); font.family: "Cascadia Mono, Consolas"; font.pixelSize: 10
                        elide: Text.ElideRight; width: parent.width
                    }
                }
                IconButton { id: closeKey; icon.name: "close"; tip: "Close (Esc)"; size: "s"; fill: "ghost"; onFired: ed.cancel() }
            }
            Rectangle {
                width: parent.width; height: 84; radius: 6
                color: BtnTokens.hex("bg1"); border.color: BtnTokens.hex("border0")
                BigActionButton {
                    id: previewKey
                    objectName: "editorPreview"
                    anchors.centerIn: parent
                    text: ed.preview.label
                    icon.name: ed.preview.icon
                    role: ed.preview.role
                    customColor: ed.preview.color !== "" ? ed.preview.color : "transparent"
                    size: ed.preview.size
                    hotkey: ed.preview.hotkey
                    safety: "none"                   // the preview never arms or asks
                    tip: ed.defaults && ed.defaults.tip !== undefined ? ed.defaults.tip : ""
                    enabled: ed.keyVisible
                    focusPolicy: Qt.NoFocus
                    width: Math.max(implicitWidth, ed.preview.size === "xl" ? 120 : ed.preview.size === "l" ? 84 : implicitWidth)
                }
                Text {
                    anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: 6
                    text: ed.preview.safety !== "none" ? "safety: " + ed.preview.safety : ""
                    color: BtnTokens.hex("text3"); font.family: "Segoe UI"; font.pixelSize: 9
                }
            }

            // --- label ---------------------------------------------------------------------------------------------------
            FieldLabel { text: "LABEL" }
            Row {
                width: parent.width; spacing: 6
                Field {
                    id: labelInput
                    objectName: "editorLabel"
                    width: parent.width - labelReset.width - 6
                    placeholder: ed.defaults.label !== undefined ? String(ed.defaults.label) : ""
                    input.maximumLength: ButtonPrefs.labelMax
                    onEdited: ed.set("label", labelInput.text.trim() === String(ed.defaults.label || "") ? "" : labelInput.text.trim())
                }
                IconButton {
                    id: labelReset
                    objectName: "editorLabelReset"
                    icon.name: "reload"; tip: "Default label"; size: "s"; fill: "ghost"
                    enabled: ed.has("label")
                    onFired: { ed.set("label", ""); labelInput.text = ed.preview.label }
                }
            }

            // --- icon ----------------------------------------------------------------------------------------------------
            FieldLabel { text: "ICON" + (ed.preview.icon !== "" ? "  ·  " + ed.preview.icon : "") }
            IconPicker {
                id: iconPicker
                objectName: "editorIconPicker"
                width: parent.width
                rows: 3
                onPicked: function(name) { ed.set("icon", name === String(ed.defaults.icon || "") ? "" : name) }
            }

            // --- colour --------------------------------------------------------------------------------------------------
            FieldLabel { text: "COLOUR" + (ed.preview.color !== "" ? "  ·  " + ed.preview.color : "  ·  the role's (" + ed.preview.role + ")") }
            ChipGroup {
                id: roleChips
                objectName: "editorRoleChips"
                width: parent.width
                size: "xs"; role: "primary"; title: "Role colour"
                autoSelect: false
                model: Spec.roleOrder.map(r => ({ id: r, label: r.toUpperCase(), tip: ed._roleHex(r) }))
                selected: Spec.roleOrder.find(r => ed._roleHex(r) === ed.preview.color) || ""
                onPicked: function(id) { ed.set("color", ed._roleHex(id)); hexInput.text = ed.preview.color }
            }
            Row {
                width: parent.width; spacing: 10
                SwatchPicker {
                    id: swatches
                    objectName: "editorSwatches"
                    width: parent.width - hexInput.width - 10
                    cell: 20
                    anchors.verticalCenter: parent.verticalCenter
                    onPicked: function(hex) { ed.set("color", hex); hexInput.text = hex }
                }
                Field {
                    id: hexInput
                    objectName: "editorHex"
                    width: 92
                    anchors.verticalCenter: parent.verticalCenter
                    placeholder: "#rrggbb"
                    input.maximumLength: 7
                    input.font.family: "Cascadia Mono, Consolas"
                    onDone: {
                        const t = hexInput.text.trim().toLowerCase()
                        if (t === "") ed.set("color", "")
                        else if (ButtonPrefs.colorOk(t)) ed.set("color", t)
                        hexInput.text = ed.preview.color
                    }
                }
            }

            // --- size ----------------------------------------------------------------------------------------------------
            FieldLabel { text: "SIZE" + (ed.sizes.length === 1 ? "  ·  fixed at " + ed.sizes[0].toUpperCase() + " on this surface" : "") }
            ChipGroup {
                id: sizeChips
                objectName: "editorSizeChips"
                width: parent.width
                size: "xs"; role: "primary"; title: "Size"
                autoSelect: false
                enabled: ed.sizes.length > 1
                model: ed.sizes.map(s => ({ id: s, label: s.toUpperCase() }))
                selected: ed.preview.size
                onPicked: function(id) { ed.set("size", id === ed.sizes[0] && ed.defaults.size === undefined ? "" : id) }
            }

            // --- hotkey --------------------------------------------------------------------------------------------------
            FieldLabel { text: "HOTKEY" + (ed.defaults.hotkey !== undefined && ed.defaults.hotkey !== "" ? "  ·  built in: " + ed.defaults.hotkey : "") }
            Row {
                width: parent.width; spacing: 8
                HotkeyField {
                    id: hotkeyField
                    objectName: "editorHotkey"
                    width: 220
                    surface: ed.surface; keyId: ed.keyId
                    placeholder: "Click, then press F9-F12 or Ctrl / Alt / Shift + a key"
                    onCaptured: function(seq) { ed.set("hotkey", seq) }
                }
                Text {
                    objectName: "editorHotkeyMessage"
                    width: parent.width - 228
                    anchors.verticalCenter: parent.verticalCenter
                    wrapMode: Text.Wrap
                    text: hotkeyField.message
                    color: BtnTokens.hex("danger"); font.family: "Segoe UI"; font.pixelSize: 11
                    visible: hotkeyField.message !== ""
                }
            }

            // --- safety --------------------------------------------------------------------------------------------------
            FieldLabel { text: "SAFETY" + (ed.floorIndex > 0 ? "  ·  floor: " + ed.floor.toUpperCase() + " (this key cannot go below it)" : "") }
            ChipGroup {
                id: safetyChips
                objectName: "editorSafetyChips"
                width: parent.width
                size: "xs"; role: ed.preview.safety === "none" ? "primary" : "warn"; title: "Safety"
                autoSelect: false
                model: Spec.safetyLevels.map((l, i) => ({ id: l, label: l.toUpperCase(), enabled: i >= ed.floorIndex,
                                                          tip: i < ed.floorIndex ? "Below this key's floor (" + ed.floor + ")" : "" }))
                selected: ed.preview.safety
                onPicked: function(id) { ed.set("safety", id === ButtonPrefs.clampSafety(ed.defaults.safety === undefined ? "none" : ed.defaults.safety, ed.floor) ? "" : id) }
            }
            Note { text: "none fires on click; arm asks for a second click within 3 s; hold fires after the key is held; modal asks in a dialog." }

            // --- visibility (decks) --------------------------------------------------------------------------------------
            Row {
                visible: ed.deck
                width: parent.width; spacing: 8
                ToggleButton {
                    objectName: "editorVisible"
                    text: ed.keyVisible ? "SHOWN ON THE DECK" : "HIDDEN FROM THE DECK"
                    icon.name: ed.keyVisible ? "eye" : "eye-off"
                    size: "s"; litRole: "go"
                    autoToggle: false
                    checked: ed.keyVisible
                    onFired: ed.keyVisible = !ed.keyVisible
                }
                Note { width: parent.width - 220; anchors.verticalCenter: parent.verticalCenter; text: "A hidden key keeps its customisation; its hotkey is not bound." }
            }

            // --- footer --------------------------------------------------------------------------------------------------
            Rectangle { width: parent.width; height: 1; color: BtnTokens.hex("border0") }
            Row {
                width: parent.width; spacing: 8
                PButton {
                    id: resetKeyBtn
                    objectName: "editorResetKey"
                    text: "RESET KEY"; icon.name: "reload"; size: "s"; role: "warn"; fill: "outline"
                    tip: "Drop every customisation of this key"
                    enabled: ButtonPrefs.isCustomised(ed.surface, ed.keyId) || ButtonPrefs.hidden(ed.surface).indexOf(ed.keyId) >= 0 || Object.keys(ed.draft).length > 0
                    onFired: ed.resetKey()
                }
                PButton {
                    id: resetSurfaceBtn
                    objectName: "editorResetSurface"
                    text: "RESET SURFACE"; icon.name: "eraser"; size: "s"; role: "destructive"; fill: "outline"
                    safety: "arm"
                    confirmText: "Every key of " + ed.surface + " back to default"
                    tip: "Every key of this surface back to its default"
                    onFired: ed.resetSurface()
                }
                Item { width: Math.max(0, parent.width - 4 * 8 - resetKeyBtn.width - resetSurfaceBtn.width - cancelKey.width - applyKey.width); height: 1 }
                PButton { id: cancelKey; objectName: "editorCancel"; text: "CANCEL"; size: "s"; tip: "Esc"; onFired: ed.cancel() }
                PButton {
                    id: applyKey
                    objectName: "editorApply"
                    text: "APPLY"; icon.name: "check"; size: "s"; role: "primary"
                    tip: "Save this key's customisation"
                    enabled: ed.dirty && hotkeyField.ok
                    onFired: ed.apply()
                }
            }
        }
    }
}
