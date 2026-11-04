pragma ComponentBehavior: Bound
import QtQuick

// HotkeyField: captures one key chord for a button's custom hotkey (docs/plans/ux-buttons.md 3.8, UXB-8). A click (or Space /
// Enter with the keyboard on it) starts a capture; the next key press with a key in it becomes the chord, checked with
// ButtonPrefs.hotkeyCheck(seq, surface, keyId): the field shows the normalised chord, or why it was refused ("not allowed:
// use F9-F12 ...", "reserved by the application", "taken by onair.deck/transport.stop") and keeps the old value. While
// capturing, Esc cancels and Backspace / Delete clear the chord (no binding). Every key press is taken by the field while
// it captures (ShortcutOverride accepted), so F9 or Ctrl+Shift+S reach it and not the window's shortcuts.
// - value: the chord, "" for none. message: the last refusal's reason (cleared by a success, a clear, the next capture).
//   ok: whether `value` passes the check (a refused attempt leaves it as it was). captured(seq) follows a successful
//   capture or a clear.
// - The chord is built from the event's modifiers and key code (not its text: Shift+S must read "Ctrl+Shift+S", not "Ctrl+s"),
//   keypad keys as Num+..., and normalised by ButtonPrefs, so what the field shows is what the prefs store.
// No supervisor / api / window access: Qt Quick Test loads it alone.
FocusScope {
    id: hf

    property string surface: ""
    property string keyId: ""
    property string value: ""
    property string placeholder: "Click, then press a key chord"
    readonly property bool capturing: hf._capturing
    property string message: ""
    property bool ok: true
    property var conflict: null
    property int fontSize: 12

    signal captured(string seq)            // the chord taken ("" after a clear)
    signal refused(string seq, string reason)

    property bool _capturing: false

    implicitWidth: 220
    implicitHeight: 30
    activeFocusOnTab: true
    Accessible.role: Accessible.EditableText
    Accessible.name: "Hotkey"
    Accessible.description: hf.capturing ? "Press a key chord; Esc cancels" : hf.value !== "" ? hf.value : "none"

    // The chord of a key event, "" for a modifier alone or a key the prefs cannot name.
    function chordOf(key, modifiers) {
        if (key === Qt.Key_Shift || key === Qt.Key_Control || key === Qt.Key_Meta || key === Qt.Key_Alt || key === Qt.Key_AltGr
            || key === Qt.Key_CapsLock || key === Qt.Key_NumLock || key === Qt.Key_ScrollLock || key === Qt.Key_unknown) return ""
        const parts = []
        if (modifiers & Qt.ControlModifier) parts.push("Ctrl")
        if (modifiers & Qt.AltModifier) parts.push("Alt")
        if (modifiers & Qt.ShiftModifier) parts.push("Shift")
        if (modifiers & Qt.MetaModifier) parts.push("Meta")
        if (modifiers & Qt.KeypadModifier) parts.push("Num")
        const name = hf.keyName(key)
        if (name === "") return ""
        parts.push(name)
        return ButtonPrefs.normalizeHotkey(parts.join("+"))
    }
    function keyName(key) {
        if (key >= Qt.Key_F1 && key <= Qt.Key_F35) return "F" + (key - Qt.Key_F1 + 1)
        if ((key >= Qt.Key_A && key <= Qt.Key_Z) || (key >= Qt.Key_0 && key <= Qt.Key_9)) return String.fromCharCode(key)
        if (key === Qt.Key_Asterisk || key === Qt.Key_Plus || key === Qt.Key_Minus || key === Qt.Key_Period || key === Qt.Key_Slash)
            return String.fromCharCode(key)
        const named = {}
        named[Qt.Key_Return] = "Return"; named[Qt.Key_Enter] = "Enter"; named[Qt.Key_Escape] = "Escape"; named[Qt.Key_Delete] = "Delete"
        named[Qt.Key_Insert] = "Insert"; named[Qt.Key_Space] = "Space"; named[Qt.Key_Tab] = "Tab"; named[Qt.Key_Backspace] = "Backspace"
        named[Qt.Key_Home] = "Home"; named[Qt.Key_End] = "End"; named[Qt.Key_PageUp] = "PgUp"; named[Qt.Key_PageDown] = "PgDown"
        named[Qt.Key_Left] = "Left"; named[Qt.Key_Right] = "Right"; named[Qt.Key_Up] = "Up"; named[Qt.Key_Down] = "Down"
        named[Qt.Key_Pause] = "Pause"; named[Qt.Key_Print] = "Print"; named[Qt.Key_Menu] = "Menu"
        return named[key] !== undefined ? named[key] : ""
    }
    // Take `seq` as the value when the prefs accept it for this key. A refusal shows its reason (message, conflict) but
    // keeps the old value, and `ok` keeps describing that value: one refused attempt must not block applying a valid draft.
    function take(seq) {
        const c = ButtonPrefs.hotkeyCheck(seq, hf.surface, hf.keyId)
        if (!c.ok) {
            hf.ok = hf.value === "" || ButtonPrefs.hotkeyCheck(hf.value, hf.surface, hf.keyId).ok
            hf.conflict = c.conflict
            hf.message = c.reason
            hf.refused(c.seq !== "" ? c.seq : String(seq), c.reason)
            return false
        }
        hf.ok = true
        hf.conflict = null
        hf.message = ""
        hf.value = c.seq
        hf.captured(c.seq)
        return true
    }
    function clear() {
        hf.value = ""
        hf.message = ""
        hf.ok = true
        hf.conflict = null
        hf.captured("")
    }
    function startCapture() {
        hf._capturing = true
        hf.message = ""
        hf.forceActiveFocus(Qt.MouseFocusReason)
    }
    function stopCapture() { hf._capturing = false }
    // Re-check the current value (the editor calls it when the surface or the stored prefs change under the field).
    function recheck() {
        if (hf.value === "") { hf.ok = true; hf.message = ""; hf.conflict = null; return true }
        const c = ButtonPrefs.hotkeyCheck(hf.value, hf.surface, hf.keyId)
        hf.ok = c.ok; hf.message = c.ok ? "" : c.reason; hf.conflict = c.conflict
        return c.ok
    }

    onActiveFocusChanged: if (!hf.activeFocus) hf._capturing = false

    Keys.onShortcutOverride: (e) => { if (hf._capturing) e.accepted = true }
    Keys.onPressed: (e) => {
        if (!hf._capturing) {
            if (e.key === Qt.Key_Space || e.key === Qt.Key_Return || e.key === Qt.Key_Enter) { e.accepted = true; hf.startCapture() }
            return
        }
        e.accepted = true
        if (e.isAutoRepeat) return
        if (e.key === Qt.Key_Escape) { hf.stopCapture(); return }
        if (e.key === Qt.Key_Backspace || e.key === Qt.Key_Delete) { hf.clear(); hf.stopCapture(); return }
        const seq = hf.chordOf(e.key, e.modifiers)
        if (seq === "") return                              // a modifier alone: wait for the key
        hf.take(seq)
        hf.stopCapture()
    }

    Rectangle {
        id: box
        anchors.fill: parent
        radius: 5
        color: hf.capturing ? BtnTokens.alpha(BtnTokens.hex("accent0"), 0.12) : BtnTokens.hex("bg3")
        border.width: hf.activeFocus || hf.capturing ? 2 : 1
        border.color: !hf.ok || hf.message !== "" ? BtnTokens.hex("danger") : hf.capturing ? BtnTokens.hex("accent0")
                    : hf.activeFocus ? BtnTokens.hex(BtnTokens.states.focusColor) : BtnTokens.hex("border1")
        Text {
            objectName: "hotkeyText"
            anchors.left: parent.left; anchors.right: clearKey.visible ? clearKey.left : parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin: 10; anchors.rightMargin: 6
            elide: Text.ElideRight
            text: hf.capturing ? "Press a key chord…" : hf.value !== "" ? hf.value : hf.placeholder
            color: hf.capturing ? BtnTokens.hex("accent0") : hf.value !== "" ? BtnTokens.hex("text0") : BtnTokens.hex("text2")
            font.family: hf.value !== "" && !hf.capturing ? "Cascadia Mono, Consolas" : "Segoe UI"
            font.pixelSize: hf.fontSize
            font.bold: hf.value !== "" && !hf.capturing
        }
        IconButton {
            id: clearKey
            objectName: "hotkeyClear"
            anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; anchors.rightMargin: 3
            size: "xs"; fill: "ghost"
            icon.name: "close"
            tip: "Clear the hotkey"
            visible: hf.value !== "" && !hf.capturing
            focusPolicy: Qt.NoFocus
            onFired: hf.clear()
        }
        TapHandler {
            objectName: "hotkeyTap"
            onTapped: { if (hf.capturing) hf.stopCapture(); else hf.startCapture() }
        }
    }
}
