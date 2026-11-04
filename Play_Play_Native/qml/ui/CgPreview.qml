import QtQuick
import ".."

/**
 * Approximate preview of a graphic over the channel's program picture: the typed Text 1 / 2 / 3 laid out the way that kind of template
 * (by role) lays them out. The real render happens on the channel (HTML template or native twin); this shows where the words land.
 */
Rectangle {
    id: pv
    property string channelId: ""
    property string role: "lower_third"
    property string t1: ""
    property string t2: ""
    property string t3: ""
    property color accent: Theme.accent0
    readonly property string a1: t1.length ? t1 : "Text 1"
    readonly property string a2: t2
    readonly property string a3: t3
    color: "#000"
    clip: true
    radius: Theme.r3
    border.color: Theme.border1

    ProgramImage { anchors.fill: parent; channelId: pv.channelId; opacity: 0.75 }
    Rectangle { anchors.fill: parent; color: "#00000040" }

    // lower third: accent stripe, big line, small line
    Item {
        visible: pv.role === "lower_third"
        x: parent.width * 0.05; y: parent.height * 0.62; width: parent.width * 0.62; height: parent.height * 0.26
        Rectangle { anchors.fill: parent; color: "#d0101018"; radius: 2 }
        Rectangle { width: Math.max(3, pv.width * 0.012); height: parent.height; color: pv.accent }
        Column {
            anchors.verticalCenter: parent.verticalCenter; x: pv.width * 0.03; width: parent.width - pv.width * 0.045; spacing: 1
            Text { width: parent.width; text: pv.a1; color: "#fff"; font.pixelSize: Math.max(9, pv.height * 0.085); font.bold: true; elide: Text.ElideRight }
            Text { visible: pv.a2.length > 0; width: parent.width; text: pv.a2; color: pv.accent; font.pixelSize: Math.max(8, pv.height * 0.058); elide: Text.ElideRight }
            Text { visible: pv.a3.length > 0; width: parent.width; text: pv.a3; color: "#c0c0d0"; font.pixelSize: Math.max(7, pv.height * 0.045); elide: Text.ElideRight }
        }
    }
    // bug: pill, top right
    Rectangle {
        visible: pv.role === "bug" || pv.role === "legal_id"
        anchors.right: parent.right; anchors.rightMargin: parent.width * 0.04; y: parent.height * 0.07
        width: Math.min(parent.width * 0.5, bugText.implicitWidth + 18); height: Math.max(16, pv.height * 0.14); radius: height / 2; color: pv.accent
        Text { id: bugText; anchors.centerIn: parent; width: parent.width - 14; horizontalAlignment: Text.AlignHCenter; text: pv.a1 + (pv.a2.length ? "  ·  " + pv.a2 : ""); color: "#05050a"; font.pixelSize: Math.max(8, pv.height * 0.07); font.bold: true; elide: Text.ElideRight }
    }
    // ticker: strip along the bottom
    Rectangle {
        visible: pv.role === "ticker"
        anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: Math.max(16, pv.height * 0.14); color: "#e0101018"
        Rectangle { width: Math.max(34, pv.width * 0.16); height: parent.height; color: pv.accent
            Text { anchors.centerIn: parent; text: pv.a2.length ? pv.a2 : "NEWS"; color: "#05050a"; font.pixelSize: Math.max(8, pv.height * 0.06); font.bold: true } }
        Text { x: Math.max(34, pv.width * 0.16) + 8; anchors.verticalCenter: parent.verticalCenter; width: parent.width - x - 6; text: pv.a1; color: "#fff"; font.pixelSize: Math.max(8, pv.height * 0.065); elide: Text.ElideRight }
    }
    // full screen / secondary / emergency: centred card
    Rectangle {
        visible: pv.role !== "lower_third" && pv.role !== "bug" && pv.role !== "legal_id" && pv.role !== "ticker"
        anchors.centerIn: parent; width: parent.width * 0.82; height: parent.height * 0.5; radius: 3
        color: pv.role === "emergency" ? "#e0a01020" : "#e0101018"; border.color: pv.accent; border.width: 2
        Column {
            anchors.centerIn: parent; width: parent.width - 16; spacing: 2
            Text { width: parent.width; horizontalAlignment: Text.AlignHCenter; text: pv.a1; color: "#fff"; font.pixelSize: Math.max(10, pv.height * 0.1); font.bold: true; elide: Text.ElideRight }
            Text { visible: pv.a2.length > 0; width: parent.width; horizontalAlignment: Text.AlignHCenter; text: pv.a2; color: pv.accent; font.pixelSize: Math.max(8, pv.height * 0.065); elide: Text.ElideRight }
            Text { visible: pv.a3.length > 0; width: parent.width; horizontalAlignment: Text.AlignHCenter; text: pv.a3; color: "#c0c0d0"; font.pixelSize: Math.max(7, pv.height * 0.05); elide: Text.ElideRight }
        }
    }
    Text { anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: 3; text: "preview"; color: "#ffffff70"; font.pixelSize: 8; visible: pv.role !== "ticker" }
}
