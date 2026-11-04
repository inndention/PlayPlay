pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import PlayPlay.Buttons

/// ActionDeck: a row of action keys built from the registry and the station's button prefs (docs/plans/ux-buttons.md 3.8).
///   ActionDeck { actions: acts; surface: "onair.deck" }
/// Each key is a BigActionButton for one action id of `ButtonPrefs.order(surface, actions.defaultOrder(surface))` ("|" draws
/// a separator): its label, icon, colour, size, hotkey and safety come from ButtonPrefs.resolve() over the action's spec
/// for its current state, so a customisation or a state change redraws the key; enabled and lit follow the registry;
/// fired() (after the key's safety and debounce) runs the action. A right click asks for the editor (customize(), which
/// UXB-8 wires), and trigger(id) presses a key from a hotkey, so a hotkey confirms like a click would.
/// The deck attaches ButtonPrefs to `backend` (the app's `api`) once, and asks for a reload each time it is shown.
RowLayout {
    id: deck

    property var actions: null                 // an Actions registry
    property string surface: "onair.deck"
    property var backend: null                 // {get, put}: ButtonPrefs.attach() on completion when it has none yet
    property string size: ""                   // "" = the surface's first allowed size (l on onair.deck)
    property bool fillKeys: true               // keys share the row's width (OnAirView's Layout.fillWidth)
    property int separatorWidth: 1
    // keySig is a string, so `keys` (and the Repeater) change only when the order really does, not on every prefs edit.
    readonly property string keySig: deck.actions ? ButtonPrefs.order(deck.surface, deck.actions.defaultOrder(deck.surface)).join("
") : ""
    readonly property var keys: deck.keySig === "" ? [] : deck.keySig.split("
")
    readonly property int keyCount: deck.keys.filter(k => k !== "|").length

    signal customize(string surface, string id)   // right click on a key: open the button editor (UXB-8)
    signal triggered(string id)                   // an action ran from this deck (click or hotkey)

    spacing: 6

    Component.onCompleted: { if (deck.backend && !ButtonPrefs.backend) ButtonPrefs.attach(deck.backend) }
    onVisibleChanged: { if (deck.visible && ButtonPrefs.backend) ButtonPrefs.touch() }

    // The key for an action id, null when the deck has none (hidden, or not on this surface).
    function keyFor(id) {
        for (let i = 0; i < rep.count; ++i) {
            const it = rep.itemAt(i)
            if (it && it.item && !it.separator && it.actionKey === id) return it.item
        }
        return null
    }
    // Press the key for `id` as a click would (safety applies: an arm key arms first); false when there is no such key or
    // it is disabled. Used by HotkeyHost.
    function trigger(id) {
        const k = deck.keyFor(id)
        if (!k || !k.enabled || k.busy) return false
        k.clicked()
        return true
    }

    Repeater {
        id: rep
        model: deck.keys
        delegate: Loader {
            id: slot
            required property string modelData
            readonly property string actionKey: slot.modelData
            readonly property bool separator: slot.modelData === "|"
            Layout.fillWidth: !slot.separator && deck.fillKeys
            Layout.fillHeight: slot.separator
            sourceComponent: slot.separator ? sepC : keyC
        }
    }

    Component {
        id: sepC
        Rectangle { implicitWidth: deck.separatorWidth; implicitHeight: 1; color: BtnTokens.hex("border1") }
    }
    Component {
        id: keyC
        BigActionButton {
            id: key
            readonly property string actionKey: parent && parent.actionKey !== undefined ? parent.actionKey : ""
            // (`action` is AbstractButton's own, FINAL: this is `act`)
            readonly property var act: deck.actions && key.actionKey !== "" ? deck.actions.get(key.actionKey) : null
            readonly property var pref: ButtonPrefs.resolve(deck.surface, key.actionKey,
                                                            key.act !== null ? Object.assign({ size: deck.size !== "" ? deck.size : undefined }, key.act) : null)
            objectName: "deck-" + key.actionKey
            actionId: key.actionKey
            actionOn: key.act !== null ? key.act.on : true
            text: key.pref.label
            icon.name: key.pref.icon
            customColor: key.pref.color !== "" ? key.pref.color : "transparent"
            size: key.pref.size
            safety: key.pref.safety
            hotkey: key.pref.hotkey
            tip: key.act !== null ? key.act.tip : ""
            enabled: key.act !== null && key.act.enabled
            active: key.act !== null && key.act.active
            onFired: { if (deck.actions && deck.actions.run(key.actionKey)) deck.triggered(key.actionKey) }
            TapHandler {
                acceptedButtons: Qt.RightButton
                onTapped: deck.customize(deck.surface, key.actionKey)
            }
        }
    }
}
