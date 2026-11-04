import QtQuick
import QtQuick.Controls
import PlayPlay.Buttons
import ".."

/// Switch row: cyan pill like the Clean config pages.
// UXB-5 (docs/plans/ux-buttons.md 3.6): the same switch row and API (label, hint, checked, enabled2, toggled(bool)), now a
// Control with hover (pill border and thumb brighten), the real `enabled` (enabled2 is an alias of it; disabled is drawn in
// the disabled tokens, not as opacity), a focus ring on keyboard focus, Space / Enter toggling while it has the focus, and
// Accessible.CheckBox. Like today it takes no focus on click (Qt.TabFocus), so a click never ends a text field's editing.
// A toggle that has the keyboard focus (Tab put it there; the ring shows) claims Space in Keys.onShortcutOverride, so
// main.qml's application-wide Space (play / pause) does not take it: Space toggles, as the design says. With the focus
// anywhere else Space is play / pause as before.
Control {
    id: t
    property string label: ""
    property string hint: ""
    property bool checked: false
    property alias enabled2: t.enabled
    signal toggled(bool value)

    implicitHeight: hint.length > 0 ? 44 : 30
    implicitWidth: 300
    padding: 0
    hoverEnabled: true
    focusPolicy: Qt.TabFocus

    Accessible.role: Accessible.CheckBox
    Accessible.name: t.label
    Accessible.description: t.hint
    Accessible.checkable: true
    Accessible.checked: t.checked
    Accessible.onToggleAction: t.toggle()
    Accessible.onPressAction: t.toggle()

    function toggle() {
        if (!t.enabled) return
        t.checked = !t.checked
        t.toggled(t.checked)
    }

    // ShortcutOverride reaches the focus item before any Shortcut (QShortcutMap asks it first); accepting it turns the key
    // into an ordinary key press for Keys.onPressed below. Only Space: the application has no plain Enter shortcut.
    Keys.onShortcutOverride: (e) => { if (t.enabled && e.key === Qt.Key_Space) e.accepted = true }
    Keys.onPressed: (e) => {
        if (e.isAutoRepeat) return
        if (e.key === Qt.Key_Space || e.key === Qt.Key_Return || e.key === Qt.Key_Enter) { t.toggle(); e.accepted = true }
    }

    Rectangle {
        id: pill
        objectName: "ctPill"
        width: 36; height: 18; radius: 9
        anchors.left: parent.left; anchors.verticalCenter: parent.verticalCenter
        color: !t.enabled ? BtnTokens.hex(BtnTokens.states.disabledBg)
             : t.checked ? (t.hovered ? BtnTokens.hover(Theme.accent0) : Theme.accent0)
             : (t.hovered ? Theme.bg4 : Theme.bg5)
        border.color: !t.enabled ? BtnTokens.hex(BtnTokens.states.disabledBorder)
                    : t.checked ? Theme.accent0 : t.hovered ? Theme.text2 : Theme.border2
        Rectangle {
            objectName: "ctThumb"
            width: 14; height: 14; radius: 7; y: 2; x: t.checked ? 20 : 2
            color: !t.enabled ? BtnTokens.hex(BtnTokens.states.disabledText) : t.checked ? "#05050a" : t.hovered ? Theme.text0 : Theme.text1
            Behavior on x { NumberAnimation { duration: 120 } }
        }
        Rectangle {      // keyboard focus: the ring the buttons draw, around the pill
            objectName: "ctFocusRing"
            visible: t.visualFocus
            anchors.fill: parent
            anchors.margins: -(BtnTokens.states.focusOffset + BtnTokens.states.focusW)
            radius: pill.radius + BtnTokens.states.focusOffset + BtnTokens.states.focusW
            color: "transparent"
            border.width: BtnTokens.states.focusW
            border.color: BtnTokens.hex(BtnTokens.states.focusColor)
        }
    }
    Column {
        anchors.left: pill.right; anchors.leftMargin: 10; anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
        spacing: 1
        Text { objectName: "ctLabel"; text: t.label; color: t.enabled ? Theme.text0 : Theme.text3; font.pixelSize: Theme.fs12; elide: Text.ElideRight; width: parent.width }
        Text { visible: t.hint.length > 0; text: t.hint; color: t.enabled ? Theme.text2 : Theme.text3; font.pixelSize: Theme.fs10; elide: Text.ElideRight; width: parent.width }
    }
    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: t.toggle() }
}
