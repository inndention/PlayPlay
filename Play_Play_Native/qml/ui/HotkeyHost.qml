pragma ComponentBehavior: Bound
import QtQuick
import PlayPlay.Buttons

/// HotkeyHost: the user's custom hotkeys of one surface (docs/plans/ux-buttons.md 3.8), one Shortcut per binding in
/// ButtonPrefs that is allowed natively and not reserved (the reserved chords, F1-F8, Space, Ctrl+Enter ..., stay
/// main.qml's). A chord presses the deck's key when the deck has one (so an arm key arms on the first press and fires on
/// the second, and a hold key stays a hold: a hotkey cannot skip the safety), else runs the action directly when its safety
/// is none. Nothing fires while a text field is being edited (Shift+letter is an allowed chord and would steal typing), while
/// a ConfirmPopup question is open, or while `active` is false (the owner binds the page's visibility).
///   HotkeyHost { actions: acts; deck: theDeck; surface: "onair.deck"; active: view.visible }
/// It is an Item (0 x 0, not drawn) because a Shortcut finds its window through its Item ancestors.
Item {
    id: host

    property var actions: null               // an Actions registry
    property var deck: null                  // an ActionDeck, optional: chords go through its keys
    property string surface: "onair.deck"
    property bool active: true
    // The ids the host binds: the deck's visible keys, else the surface's default order (hidden keys have no hotkey).
    readonly property var ids: host.deck ? host.deck.keys : (host.actions ? host.actions.defaultOrder(host.surface) : [])
    // a string signature, so the Shortcuts are rebuilt only when a binding really changes
    readonly property string bindSig: ButtonPrefs.bindings(host.surface, host.ids).map(b => b.id + "=" + b.seq).join("
")
    readonly property var bindings: host.bindSig === "" ? [] : host.bindSig.split("
").map(l => { const at = l.indexOf("="); return { id: l.slice(0, at), seq: l.slice(at + 1) } })
    readonly property int count: host.bindings.length
    // Text input has the focus (cursorPosition, or an editable combo's editText): the same test as main.qml's Space guard.
    readonly property bool typing: {
        const w = host.Window.window, f = w ? w.activeFocusItem : null
        return !!(f && (f.hasOwnProperty("cursorPosition") || (f.hasOwnProperty("editText") && f.editable === true)))
    }

    signal triggered(string id)              // a chord pressed a key or ran the action
    signal refused(string id, string why)    // a chord arrived but nothing could run (disabled key, unknown action, no deck for a guarded action)

    width: 0; height: 0
    visible: false

    function sequenceFor(id) { const b = host.bindings.find(x => x.id === id); return b ? b.seq : "" }
    // The chord for `id` arrived.
    function press(id) {
        if (!host.active || host.typing || BtnTokens.confirmOpen) return false
        if (host.deck) {
            const k = host.deck.keyFor(id)
            if (k) { if (host.deck.trigger(id)) { host.triggered(id); return true } host.refused(id, "key disabled"); return false }
        }
        if (!host.actions) { host.refused(id, "no actions"); return false }
        const a = host.actions.get(id)
        if (a === null) { host.refused(id, "unknown action"); return false }
        const safety = ButtonPrefs.resolve(host.surface, id, a).safety
        if (safety !== "none") { host.refused(id, "needs a key for " + safety); return false }   // never skip a confirmation
        if (!host.actions.run(id)) { host.refused(id, "disabled"); return false }
        host.triggered(id)
        return true
    }

    Instantiator {
        id: inst
        model: host.bindings
        delegate: Shortcut {
            required property var modelData
            sequence: modelData.seq
            context: Qt.WindowShortcut
            enabled: host.active && !host.typing && !BtnTokens.confirmOpen
            onActivated: host.press(modelData.id)
        }
    }
}
