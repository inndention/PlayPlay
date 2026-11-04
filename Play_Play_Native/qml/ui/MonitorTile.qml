import QtQuick
import QtQuick.Layouts
import ".."

/**
 * One tile of the monitor wall: picture area (children), caption bar, tally lamp and a red / green tally border.
 *   tally "pgm" = on air (red), "pvw" = preview bus (green), "" = none; `selected` outlines the tile in the accent colour.
 * Signals: clicked (select), doubleClicked (take), rightClicked (context menu).
 */
Rectangle {
    id: t
    property string label: ""
    property string sub: ""
    property string tally: ""
    property string badge: ""
    property color badgeTint: Theme.text1
    property bool selected: false
    property bool showCaption: true
    property int captionH: 20
    readonly property bool narrow: width < 150      // small tiles keep the name only; the tally border carries the state
    default property alias content: area.data
    signal clicked()
    signal doubleClicked()
    signal rightClicked()

    color: "#000"
    radius: Theme.r5
    clip: true
    border.width: tally !== "" ? 3 : 1
    border.color: tally === "pgm" ? Theme.onAir : tally === "pvw" ? Theme.ready : Theme.border1

    Item { id: area; anchors.fill: parent; anchors.margins: t.border.width }

    Rectangle { visible: t.selected; anchors.fill: parent; z: 2; color: "transparent"; radius: t.radius; border.width: 2; border.color: Theme.accent0; anchors.margins: t.border.width }
    Rectangle {
        visible: t.showCaption
        anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
        anchors.margins: t.border.width
        height: t.captionH; color: "#000000c0"
        RowLayout {
            anchors.fill: parent; anchors.leftMargin: 6; anchors.rightMargin: 6; spacing: 6
            Text { text: t.label; color: "#fff"; font.pixelSize: Theme.fs11; font.bold: true; elide: Text.ElideRight; Layout.fillWidth: true; Layout.maximumWidth: t.width * 0.62 }
            Text { visible: t.sub.length > 0 && !t.narrow; text: t.sub; color: Theme.accent0; font.family: Theme.fontMono; font.pixelSize: Theme.fs10; elide: Text.ElideRight; Layout.fillWidth: true; horizontalAlignment: Text.AlignRight }
            Item { visible: t.sub.length === 0 || t.narrow; Layout.fillWidth: true }
            Text { visible: t.badge.length > 0 && !t.narrow; text: t.badge; color: t.badgeTint; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 0.8 }
        }
    }
    Rectangle {
        visible: t.tally !== ""
        x: 6; y: 6; width: tallyText.implicitWidth + 12; height: 16; radius: 3
        color: t.tally === "pgm" ? Theme.onAir : Theme.ready
        Text { id: tallyText; anchors.centerIn: parent; text: t.tally === "pgm" ? "PGM" : "PVW"; color: "#05050a"; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1 }
    }
    MouseArea {
        anchors.fill: parent; acceptedButtons: Qt.LeftButton | Qt.RightButton; cursorShape: Qt.PointingHandCursor
        onClicked: function(m) { if (m.button === Qt.RightButton) t.rightClicked(); else t.clicked() }
        onDoubleClicked: function(m) { if (m.button === Qt.LeftButton) t.doubleClicked() }
    }
}
