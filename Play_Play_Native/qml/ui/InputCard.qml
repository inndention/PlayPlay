import QtQuick
import QtQuick.Layouts
import ".."

/**
 * Picture area for a mapped input, best picture first:
 *   1. liveChannel   the input is on air on some channel: that channel's program picture (full quality, PGM tally)
 *   2. monitorId     the controlled engine's input monitor ("<ch>:in<N>", see InputMap.lease): a live low-rate picture
 *   3. source card   kind, title and address (SDI inputs, a monitor that has not delivered yet, or no engine)
 * monitor: the engine's status row for this input ({state, connected, warm, error}) adds a small line over the picture.
 */
Rectangle {
    id: card
    property var entry: null
    property int slot: 0
    property string liveChannel: ""           // channel currently playing this input, "" = none
    property string monitorId: ""             // "<ch>:in<N>" when the input is leased for monitoring
    property var monitor: null                // engine status row for this input
    property string skipped: ""               // why the input is not monitored (shown on big cards)
    property bool big: false
    readonly property bool showMonitor: liveChannel === "" && monitorId !== "" && mon.hasFrame
    readonly property color kindColor: !entry ? Theme.text3 : entry.kind === "ndi" ? Theme.accent2 : entry.kind === "camera" ? Theme.clsPreroll : entry.kind === "sdi" ? Theme.standby : Theme.clsEffects
    readonly property string kindGlyph: !entry ? "" : entry.kind === "ndi" ? "NDI" : entry.kind === "camera" ? "CAM" : entry.kind === "sdi" ? "SDI" : "URL"
    color: "#07070d"
    clip: true

    Rectangle { anchors.fill: parent; visible: !!card.entry; gradient: Gradient { GradientStop { position: 0; color: Qt.rgba(card.kindColor.r, card.kindColor.g, card.kindColor.b, 0.30) } GradientStop { position: 1; color: "#07070d" } } }
    ProgramImage { anchors.fill: parent; visible: card.liveChannel !== ""; channelId: card.liveChannel; live: card.liveChannel !== "" }
    ProgramImage { id: mon; anchors.fill: parent; visible: card.showMonitor; channelId: card.liveChannel === "" ? card.monitorId : ""; live: channelId !== "" }
    Rectangle {
        // monitor state: WARM = the full-quality receiver a take will use (instant cut); LOW = proxy picture
        visible: card.showMonitor && !!card.monitor
        anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 4
        width: monTxt.implicitWidth + 8; height: monTxt.implicitHeight + 2; radius: 2
        color: card.monitor && card.monitor.warm ? Theme.ready : "#000000a0"
        Text { id: monTxt; anchors.centerIn: parent; text: card.monitor && card.monitor.warm ? "WARM" : "LIVE"; color: card.monitor && card.monitor.warm ? "#05050a" : Theme.text1; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 0.8 }
    }

    Column {
        visible: card.liveChannel === "" && !card.showMonitor
        anchors.centerIn: parent; spacing: card.big ? 6 : 2; width: parent.width - 12
        Text { anchors.horizontalCenter: parent.horizontalCenter; text: card.entry ? card.kindGlyph : String(card.slot + 1); color: card.entry ? card.kindColor : Theme.text3; font.family: Theme.fontDisplay; font.pixelSize: card.big ? Theme.fs28 : Theme.fs16; font.bold: true; font.letterSpacing: 2 }
        Text { visible: !!card.entry; width: parent.width; horizontalAlignment: Text.AlignHCenter; text: card.entry ? card.entry.title : ""; color: Theme.text0; font.pixelSize: card.big ? Theme.fs14 : Theme.fs11; font.bold: true; elide: Text.ElideRight }
        Text { visible: !!card.entry && card.big; width: parent.width; horizontalAlignment: Text.AlignHCenter; text: card.entry ? card.entry.sub : ""; color: Theme.text2; font.family: Theme.fontMono; font.pixelSize: Theme.fs10; elide: Text.ElideMiddle }
        Text { visible: !card.entry; width: parent.width; horizontalAlignment: Text.AlignHCenter; text: "EMPTY"; color: Theme.text3; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1 }
        Text { visible: !card.entry && card.big; width: parent.width; horizontalAlignment: Text.AlignHCenter; text: "right-click to map an input"; color: Theme.text3; font.pixelSize: Theme.fs10 }
        Text {
            readonly property string why: card.skipped ? card.skipped : card.monitor ? (card.monitor.state === "opening" ? "connecting…" : card.monitor.state === "failed" ? (card.monitor.error || "no signal") : card.monitor.state === "open" && card.monitor.stale ? "no picture" : "") : ""
            visible: !!card.entry && why.length > 0 && (card.big || card.monitor && card.monitor.state !== "card")
            width: parent.width; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.WordWrap; maximumLineCount: 2; elide: Text.ElideRight
            text: why; color: card.monitor && card.monitor.state === "opening" ? Theme.text2 : Theme.standby; font.pixelSize: Theme.fs10
        }
    }
}
