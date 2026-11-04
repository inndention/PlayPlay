import QtQuick
import QtQuick.Controls
import QtTest

// UXB-0: Space play/pause (main.qml) and the Overlays preset hotkeys (OverlaysView.qml) keep working after a button click and
// stay quiet while text is being edited. The scene is an ApplicationWindow shaped like main.qml with the app's own CButton,
// DeckButton, CField and CSelect (imported from Play_Play_Native/qml/ui), and the guard lines are the shipped ones
// (ppSpaceGuard, ppTypingProperty and ppPresetEnabled, read from the sources by test_buttons_qml.cpp).
TestCase {
    id: tc
    name: "TypingGuard"
    when: windowShown
    property var win: null

    function sceneSource() {
        return [
            'import QtQuick', 'import QtQuick.Controls', 'import "' + ppQmlDir + '/ui"',
            'ApplicationWindow {',
            '    id: window; width: 640; height: 480; visible: false',
            '    QtObject { id: supervisor; property var activeState: ({ state: "READY" }); property int plays: 0; property int pauses: 0',
            '        function play() { plays++ } function pause() { pauses++ } }',
            '    property int taken: 0',
            '    readonly property QtObject sup: supervisor',
            '    readonly property Item ov: view',
            '    readonly property Item cb: cbutton',
            '    readonly property Item deck: deckKey',
            '    readonly property Item cfield: field',
            '    readonly property Item cselect: select',
            '    readonly property Item area: textArea',
            '    readonly property Item ecombo: editCombo',
            '    readonly property Item spin: spinBox',
            '    readonly property Item check: checkBox',
            '    readonly property Item popBtn: popupButton',
            '    readonly property var pop: popup',
            '    Item {',
            '        id: view; anchors.fill: parent',
            '        ' + ppTypingProperty.split('\n').join('\n        '),
            '        property int fired: 0',
            '        Instantiator {',
            '            model: [{ id: "p1", seq: "Ctrl+K" }, { id: "p2", seq: "G" }]',
            '            delegate: Shortcut {',
            '                ' + ppPresetEnabled,
            '                onActivated: view.fired++',
            '            }',
            '        }',
            '        Column {',
            '            x: 10; y: 10; spacing: 6',
            '            CButton { id: cbutton; text: "TAKE"; onClicked: window.taken++ }',
            '            DeckButton { id: deckKey; glyph: "▶"; text: "PLAY" }',
            '            CField { id: field; label: "Name"; width: 200 }',
            '            CSelect { id: select; label: "Channel"; model: ["main", "ch2"]; width: 200 }',
            '            TextArea { id: textArea; width: 200; height: 40 }',
            '            ComboBox { id: editCombo; editable: true; model: ["one", "two"] }',
            '            SpinBox { id: spinBox; editable: true }',
            '            CheckBox { id: checkBox; text: "Loop" }',
            '        }',
            '    }',
            '    Popup {',
            '        id: popup; x: 380; y: 20; width: 160; height: 80; focus: true; closePolicy: Popup.NoAutoClose',
            '        Button { id: popupButton; text: "OK"; anchors.centerIn: parent }',
            '    }',
            '    ' + ppSpaceGuard.split('\n').join('\n    '),
            '}'
        ].join('\n')
    }

    // First descendant (depth first) that has `prop`: the TextField inside CField, the ComboBox inside CSelect.
    function childWith(item, prop) {
        for (var i = 0; i < item.children.length; ++i) {
            var c = item.children[i]
            if (c.hasOwnProperty(prop)) return c
            var d = childWith(c, prop)
            if (d) return d
        }
        return null
    }

    function initTestCase() {
        verify(ppSpaceGuard.indexOf('Shortcut { sequence: "Space"') >= 0, "main.qml Space shortcut not found")
        verify(ppSpaceGuard.indexOf("function spaceIsTransport(") >= 0, "main.qml spaceIsTransport() not found")
        verify(ppTypingProperty.indexOf("readonly property bool typing:") === 0, "OverlaysView typing property not found")
        verify(ppPresetEnabled.indexOf("!view.typing") >= 0, "OverlaysView preset Shortcut enabled line not found")
    }

    function init() {
        win = createTemporaryQmlObject(sceneSource(), tc, "typingguard_scene.qml")
        verify(win !== null, "scene failed to load")
        win.show()
        win.requestActivate()
        // The window turns active first and its root item gets the focus a little later (the focus-in is queued).
        tryVerify(function() { return win.active && win.activeFocusItem !== null }, 5000, "scene window never got the focus")
        tryVerify(function() { return win.check.y > 0 && win.cb.width > 0 }, 5000, "scene never laid out")
    }

    // The old guards tested `text`, which every button has: Space and the presets died after any button click.
    function test_guardsNoLongerTestText() {
        verify(ppSpaceGuard.indexOf('hasOwnProperty("text")') < 0, ppSpaceGuard)
        verify(ppTypingProperty.indexOf('hasOwnProperty("text")') < 0, ppTypingProperty)
        verify(ppSpaceGuard.indexOf('hasOwnProperty("cursorPosition")') >= 0)
        verify(ppTypingProperty.indexOf('hasOwnProperty("cursorPosition")') >= 0)
    }

    function test_spaceAfterButtonClick() {
        mouseClick(win.cb)
        compare(win.taken, 1)
        verify(win.cb.activeFocus, "a clicked CButton holds the focus (the bug's premise)")
        keyClick(Qt.Key_Space)
        compare(win.sup.plays, 1)
        compare(win.taken, 1, "Space must not re-fire the focused button")
        win.sup.activeState = { state: "ON_AIR" }
        keyClick(Qt.Key_Space)
        compare(win.sup.pauses, 1)
        mouseClick(win.deck)
        verify(win.deck.activeFocus)
        keyClick(Qt.Key_Space)
        compare(win.sup.pauses, 2)
        mouseClick(win.check)
        verify(win.check.checked)
        keyClick(Qt.Key_Space)
        compare(win.sup.pauses, 3)
        verify(win.check.checked, "Space goes to transport, not to the focused check box")
    }

    function test_spaceWithNoFocusItemOrPlainCombo() {
        win.contentItem.forceActiveFocus()
        keyClick(Qt.Key_Space)
        compare(win.sup.plays, 1)
        var combo = childWith(win.cselect, "editText")      // non-editable: not a text input, today's behaviour kept
        verify(combo !== null)
        combo.forceActiveFocus()
        verify(combo.activeFocus)
        keyClick(Qt.Key_Space)
        compare(win.sup.plays, 2)
    }

    function test_noTransportWhileTyping() {
        var input = childWith(win.cfield, "cursorPosition")
        verify(input !== null)
        mouseClick(input)
        verify(input.activeFocus)
        keyClick(Qt.Key_Space)
        compare(win.sup.plays, 0)
        compare(input.text, " ", "the space reaches the field")
        mouseClick(win.area)
        verify(win.area.activeFocus)
        keyClick(Qt.Key_Space)
        compare(win.sup.plays, 0)
        compare(win.area.text, " ")
        win.ecombo.forceActiveFocus()
        keyClick(Qt.Key_Space)
        compare(win.sup.plays, 0, "editable combo")
        win.spin.forceActiveFocus()
        keyClick(Qt.Key_Space)
        compare(win.sup.plays, 0, "editable spin box (focus item " + win.activeFocusItem + ")")
        compare(win.sup.pauses, 0)
    }

    // A popup / dialog / menu keeps today's "Space does nothing": there Space belongs to the popup's own buttons.
    function test_noTransportInPopup() {
        win.pop.open()
        tryVerify(function() { return win.pop.opened })
        mouseClick(win.popBtn)
        verify(win.popBtn.activeFocus)
        keyClick(Qt.Key_Space)
        compare(win.sup.plays, 0)
        win.pop.close()
        tryVerify(function() { return !win.pop.visible })
        mouseClick(win.cb)
        keyClick(Qt.Key_Space)
        compare(win.sup.plays, 1, "Space works again once the popup is closed")
    }

    function test_presetHotkeysAfterClickNotWhileTyping() {
        mouseClick(win.cb)
        verify(!win.ov.typing)
        keyClick(Qt.Key_K, Qt.ControlModifier)
        compare(win.ov.fired, 1, "Ctrl+K preset fires after a button click")
        keyClick(Qt.Key_G)
        compare(win.ov.fired, 2, "single-key preset fires after a button click")
        var input = childWith(win.cfield, "cursorPosition")
        mouseClick(input)
        verify(win.ov.typing)
        keyClick(Qt.Key_K, Qt.ControlModifier)
        keyClick(Qt.Key_G)
        compare(win.ov.fired, 2, "no preset while typing in a TextField")
        compare(input.text, "g")
        win.ecombo.forceActiveFocus()
        verify(win.ov.typing, "an editable combo is a text input")
        keyClick(Qt.Key_K, Qt.ControlModifier)
        compare(win.ov.fired, 2)
        var combo = childWith(win.cselect, "editText")
        combo.forceActiveFocus()
        verify(!win.ov.typing, "a plain combo is not a text input")
        keyClick(Qt.Key_K, Qt.ControlModifier)
        compare(win.ov.fired, 3)
    }
}
