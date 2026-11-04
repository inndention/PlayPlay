import QtQuick
import QtQuick.Controls
import ".."

/**
 * TextMenu: right-click edit menu for a TextField / TextArea (Undo, Redo, Cut, Copy, Paste, Delete, Select all, Clear).
 * Drop it inside the text control:   TextField { ...; TextMenu {} }   (it targets its parent; set `target` otherwise).
 * Only the right button is claimed, so left-click caret placement, drag selection and double-click word selection are untouched.
 */
Item {
    id: tm
    property Item target: parent
    anchors.fill: parent
    z: 10
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.RightButton
        cursorShape: Qt.IBeamCursor
        onPressed: function(mouse) { if (tm.target && !tm.target.activeFocus) tm.target.forceActiveFocus() }
        onClicked: function(mouse) {
            var t = tm.target; if (!t) return
            var ro = !!t.readOnly
            var hasSel = (t.selectedText || "").length > 0
            var hasText = (t.text || "").length > 0
            var pw = t.echoMode !== undefined && t.echoMode !== TextInput.Normal
            menu.show([
                { header: "Text" },
                { text: "Undo", hint: "Ctrl+Z", enabled: !ro && !!t.canUndo, fn: function() { t.undo() } },
                { text: "Redo", hint: "Ctrl+Y", enabled: !ro && !!t.canRedo, fn: function() { t.redo() } },
                { sep: true },
                { text: "Cut", hint: "Ctrl+X", enabled: !ro && hasSel && !pw, fn: function() { t.cut() } },
                { text: "Copy", hint: "Ctrl+C", enabled: hasSel && !pw, fn: function() { t.copy() } },
                { text: "Paste", hint: "Ctrl+V", enabled: !ro && !!t.canPaste, fn: function() { t.paste() } },
                { text: "Paste as one line", enabled: !ro, fn: function() { var s = menu.pasteText().replace(/\s*[\r\n]+\s*/g, " ").trim(); if (s.length) t.insert(t.selectionStart, s) } },
                { text: "Delete", enabled: !ro && hasSel, fn: function() { t.remove(t.selectionStart, t.selectionEnd) } },
                { sep: true },
                { text: "Select all", hint: "Ctrl+A", enabled: hasText, fn: function() { t.selectAll() } },
                { text: "Copy all", enabled: hasText && !pw, fn: function() { menu.copyText(t.text, "field text") } },
                { text: "Clear field", danger: true, enabled: !ro && hasText, fn: function() { t.selectAll(); t.remove(0, t.length) } }
            ])
        }
    }
    CMenu { id: menu }
}
