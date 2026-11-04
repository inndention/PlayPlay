pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

// ConfirmPopup: the one confirmation dialog of a window, for `safety: "modal"` keys (docs/plans/ux-buttons.md 3.4 / 3.5).
// BtnTokens.confirmPopup() creates it lazily on the first modal press in a window, parented to that window's
// Overlay.overlay; every later modal key of the window reuses it. ask(title, text, confirmLabel, role, onConfirm, source)
// shows one question:
// - Enter confirms and Esc / Cancel cancel. When Cancel has the keyboard focus, Enter cancels (the focused key wins).
//   A press outside does nothing: the second click of a double click on the modal key must not answer the question.
// - A confirmation within minIntervalMs of opening is ignored (the double click's second click on the confirm key, a held
//   Enter), with the same backward-clock rule as PButton's debounce.
// - It is modal, but Qt's modal blocking only stops window-context shortcuts (an armed key's Esc, a default Shortcut).
//   Qt.ApplicationShortcut shortcuts still act: main.qml's On Air keys all are (Esc clear preview, 1-8, Alt+1-8 TAKE,
//   Ctrl+Enter CUT, Space; Space does nothing over a popup only through spaceIsTransport's overlay test). So the dialog
//   takes its own keys first: while the keyboard is on it (ask() puts it there) it accepts the ShortcutOverride of Esc,
//   Return and Enter (keypad too), and those reach its Keys handler, not a shortcut. Other application shortcuts still act
//   during a question; BtnTokens.confirmOpen is set while one is open, for those that should yield.
// - source: the key that asked. When it stops being ready (disabled, busy, no longer modal) or is destroyed (its view was
//   replaced) while the question is open, the question is cancelled. PButton's callback re-checks the same as a backstop.
// - One question in the application, as BtnTokens.arming says: a new ask() while a question is open cancels that question
//   first, asking disarms any armed key and cancels another window's question, and a key arming cancels the question.
//   BtnTokens.confirming names the popup whose question is open.
// No supervisor / api / window access: Qt Quick Test loads it alone.
Popup {
    id: pop
    objectName: "confirmPopup"

    property string title: ""
    property string text: ""
    property string confirmLabel: "Confirm"
    property string role: "destructive"
    property Item source: null
    property int minIntervalMs: BtnTokens.timing.minIntervalMs
    readonly property bool pending: pop._pending !== null

    signal confirmed()
    signal cancelled()

    property var _pending: null            // {fn} of the open question
    property bool _hadSource: false
    property double _openedAt: 0
    // The asking key is still ready to fire. busy / safety are read by name (_get): a source may be any Item, and PButton's
    // are what matter when it is one. The binding still tracks them, since the reads happen while it evaluates.
    readonly property bool _sourceOk: {
        const s = pop.source
        if (s === null) return !pop._hadSource          // destroyed while asking
        const safety = pop._get(s, "safety")
        return s.enabled === true && pop._get(s, "busy") !== true && (safety === undefined || safety === "modal")
    }

    function _get(o, key) { return o[key] }
    function _within(stamp, ms) { const dt = Date.now() - stamp; return dt >= 0 && dt < ms }

    function ask(title, text, confirmLabel, role, onConfirm, source) {
        if (pop._pending !== null) { pop._pending = null; pop.cancelled() }
        // Armed keys disarm and another window's question is cancelled (onArming below): one pending confirmation. This
        // popup's own handler sees no question, as the line above cleared it.
        BtnTokens.arming(source instanceof Item ? source : null)
        pop.title = title || ""
        pop.text = text || ""
        pop.confirmLabel = confirmLabel || "Confirm"
        pop.role = role || "destructive"
        pop._hadSource = false
        pop.source = source instanceof Item ? source : null
        pop._hadSource = pop.source !== null
        pop._openedAt = Date.now()
        pop._pending = { fn: onConfirm }
        // Every question starts with the keyboard on the dialog, not on a key: the popup is a focus scope and would give
        // the focus back to the Cancel key a previous question tabbed to, and then Enter would cancel.
        body.focus = true
        pop.open()
        body.forceActiveFocus(Qt.PopupFocusReason)
    }
    function confirm() {
        if (pop._pending === null || pop._within(pop._openedAt, pop.minIntervalMs)) return false
        const p = pop._pending
        pop._pending = null
        pop.close()
        pop.confirmed()
        if (typeof p.fn === "function") p.fn()
        return true
    }
    function cancel() {
        const had = pop._pending !== null
        pop._pending = null
        pop.close()
        if (had) pop.cancelled()
    }
    // Enter: confirm, unless the keyboard is on Cancel (the focused key wins).
    function _enter() { if (cancelKey.activeFocus) pop.cancel(); else pop.confirm() }

    onPendingChanged: {
        if (pop.pending) BtnTokens.confirming = pop
        else if (BtnTokens.confirming === pop) BtnTokens.confirming = null
    }
    Component.onDestruction: if (BtnTokens.confirming === pop) BtnTokens.confirming = null
    Connections {
        target: BtnTokens
        function onArming(item) { if (pop._pending !== null) pop.cancel() }
    }

    // Deferred: cancelling here would reset source / _hadSource while _sourceOk is still notifying (a binding loop).
    // PButton's callback re-checks the key, so nothing can fire in between.
    function _cancelIfSourceGone() { if (!pop._sourceOk && pop._pending !== null) pop.cancel() }
    on_SourceOkChanged: if (!pop._sourceOk && pop._pending !== null) Qt.callLater(pop._cancelIfSourceGone)
    onClosed: {
        if (pop._pending !== null) { pop._pending = null; pop.cancelled() }     // Esc
        pop._hadSource = false
        pop.source = null
    }

    anchors.centerIn: parent
    width: Math.min(440, Math.max(260, (pop.parent ? pop.parent.width : 440) - 32))
    modal: true
    dim: true
    focus: true
    closePolicy: Popup.CloseOnEscape
    padding: 18
    // The app's modal dim (InputPicker, DevicePicker): Fusion's own is a faint shadow that does not show the window is blocked.
    Overlay.modal: Rectangle { objectName: "confirmDim"; color: "#000000a0" }

    background: Rectangle {
        color: BtnTokens.hex("bg2")
        border.color: BtnTokens.hex("border1")
        border.width: 1
        radius: 8
    }

    contentItem: Column {
        id: body
        spacing: 12
        // The dialog's own keys, while the keyboard is on it (the body or one of its keys): accepting the ShortcutOverride
        // keeps Qt from offering Esc / Return / Enter to any shortcut, so an application shortcut on the same key (main.qml's
        // Esc, a user's Num+Enter hotkey) cannot take them, and they arrive here as key presses. A held key's repeats are
        // swallowed.
        function _ownKey(e) {
            const k = e.key
            return (k === Qt.Key_Escape || k === Qt.Key_Return || k === Qt.Key_Enter) && (e.modifiers & ~Qt.KeypadModifier) === Qt.NoModifier
        }
        Keys.onShortcutOverride: (e) => { if (pop.opened && body._ownKey(e)) e.accepted = true }
        Keys.onPressed: (e) => {
            if (!body._ownKey(e)) return
            e.accepted = true
            if (e.isAutoRepeat) return
            if (e.key === Qt.Key_Escape) pop.cancel(); else pop._enter()
        }
        Text {
            objectName: "confirmTitle"
            width: parent.width
            text: pop.title
            visible: pop.title !== ""
            wrapMode: Text.Wrap
            color: BtnTokens.hex("text0")
            font.family: BtnTokens.fontDisplay
            font.pixelSize: 14
            font.bold: true
        }
        Text {
            objectName: "confirmText"
            width: parent.width
            text: pop.text
            visible: pop.text !== "" && pop.text !== pop.title
            wrapMode: Text.Wrap
            color: BtnTokens.hex("text1")
            font.pixelSize: 12
        }
        Row {
            anchors.right: parent.right
            spacing: 8
            PButton {
                id: cancelKey
                objectName: "confirmCancel"
                text: "Cancel"
                tip: "Esc"
                onFired: pop.cancel()
            }
            PButton {
                id: okKey
                objectName: "confirmOk"
                text: pop.confirmLabel
                role: pop.role
                tip: "Enter"
                minIntervalMs: 0                        // the popup's own open guard applies (confirm())
                Accessible.defaultButton: true
                onFired: pop.confirm()
            }
        }
        // Enter / Return when the keyboard is somewhere else in the window (the Keys handler above has them otherwise).
        // Inside the popup, so Qt's modal blocking lets it through while the window's own window-context shortcuts are
        // blocked.
        Shortcut {
            sequences: ["Return", "Enter"]
            enabled: pop.opened
            autoRepeat: false
            onActivated: pop._enter()
        }
    }
}
