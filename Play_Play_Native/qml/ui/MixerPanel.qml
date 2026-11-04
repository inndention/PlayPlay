import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".."

/**
 * Audio mixer for the controlled channel: PROGRAM (the on-air item and operator takes), line-in strips (sound-card inputs, or a playback
 * device's loopback) and MASTER, each with a source meter (pre-fader: the source's own level), a mix meter (post-fader), fader, mute and
 * solo. Solo silences every other strip in that mix, so it is flagged in the header until cleared.
 *
 * Mix tabs: MAIN plus the channel's extra output mixes (a web stream with its own graphics and sound). The faders act on the selected
 * mix; an extra mix starts out following main and keeps its own setting for every strip it has been given. + Mix adds one (then point
 * an output at it in Outputs); right-click a tab to remove it. State and commands: ui/MixerState.qml (`mixer`).
 */
CPanel {
    id: p
    property string channelId: supervisor.activeChannelId
    property var meters: supervisor.activeMeters
    readonly property var mixList: {
        var a = meters && meters.mixes ? meters.mixes : []
        return a.length ? a : [{ id: "main", name: "Main" }]
    }
    readonly property var curMix: {
        for (var i = 0; i < mixList.length; ++i) if (mixList[i].id === mixer.mix) return mixList[i]
        return mixList[0]
    }
    readonly property bool onMain: !curMix || curMix.id === "main"
    readonly property var pre: meters && meters.sources ? meters.sources : ({})
    // Strips of the selected mix: main's come straight from the engine; an extra mix's faders / mutes / post meters are merged onto
    // main's strip descriptions (name, kind, layer). Strips whose layer is not in that mix are left out.
    readonly property var strips: {
        var base = meters && meters.strips ? meters.strips : []
        if (onMain) return base
        var byId = {}, out = []
        for (var i = 0; i < base.length; ++i) byId[base[i].id] = base[i]
        var ms = curMix.strips || []
        for (var j = 0; j < ms.length; ++j) {
            if (ms[j].inMix === false) continue
            var b = byId[ms[j].id] || { id: ms[j].id, name: ms[j].id, kind: "layer" }
            var o = Object.assign({}, b)
            o.gainDb = ms[j].gainDb; o.mute = ms[j].mute; o.peaks = ms[j].peaks; o.solo = (curMix.solo || "") === ms[j].id
            o.pre = p.pre[ms[j].id] || ({}); o.followsMain = !ms[j].explicit
            out.push(o)
        }
        return out
    }
    readonly property string solo: onMain ? (meters && meters.solo ? meters.solo : "") : (curMix.solo || "")
    readonly property var master: onMain
        ? { gainDb: meters && meters.masterGainDb !== undefined ? meters.masterGainDb : 0, peaks: meters && meters.channelPeaksDbfs ? meters.channelPeaksDbfs : [] }
        : { gainDb: curMix.masterGainDb !== undefined ? curMix.masterGainDb : 0, peaks: curMix.channelPeaksDbfs || [] }
    title: "Audio mixer · " + channelId.toUpperCase()
    padding: 8
    headerRight: [
        CBadge { visible: p.solo !== ""; text: "SOLO ON"; tint: Theme.warn; solid: true },
        CButton { visible: p.solo !== ""; text: "Clear solo"; small: true; onClicked: { mixer.solo(p.channelId, "") } },
        CButton { text: "+ Line in"; small: true; variant: "primary"; onClicked: { picker.openPicker() } }
    ]
    Component.onCompleted: { mixer.refreshDevices(); mixer.loadMixes(p.channelId) }
    // bring back saved line-ins an engine restart dropped
    Timer { interval: 1500; running: p.visible && mixer.loaded; repeat: false; onTriggered: { mixer.restore(p.channelId, meters && meters.strips ? meters.strips : []) } }

    DevicePicker { id: picker }
    CMenu { id: stripMenu }

    ColumnLayout {
        anchors.fill: parent; spacing: 6
        // ---- mix tabs
        RowLayout {
            Layout.fillWidth: true; spacing: 4
            Text { text: "MIX"; color: Theme.text3; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 1 }
            Repeater {
                model: p.mixList
                Rectangle {
                    readonly property bool on: (p.curMix && p.curMix.id === modelData.id)
                    implicitWidth: tabTxt.implicitWidth + 18; implicitHeight: 22; radius: 3
                    color: on ? Qt.rgba(Theme.accent0.r, Theme.accent0.g, Theme.accent0.b, 0.25) : tabMa.containsMouse ? Theme.bg4 : Theme.bg3
                    border.color: on ? Theme.accent0 : Theme.border1
                    Text { id: tabTxt; anchors.centerIn: parent; text: String(modelData.name || modelData.id).toUpperCase(); color: parent.on ? Theme.text0 : Theme.text2; font.pixelSize: Theme.fs10; font.bold: true; font.letterSpacing: 0.8 }
                    MouseArea {
                        id: tabMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; acceptedButtons: Qt.LeftButton | Qt.RightButton
                        onClicked: function(m) {
                            if (m.button === Qt.RightButton && modelData.id !== "main")
                                stripMenu.show([{ header: "Mix " + (modelData.name || modelData.id) },
                                                { text: "Remove this mix", danger: true, fn: function() { mixer.removeMix(p.channelId, modelData.id) } }])
                            else mixer.mix = modelData.id
                        }
                    }
                }
            }
            Rectangle {
                implicitWidth: addTxt.implicitWidth + 16; implicitHeight: 22; radius: 3; color: addMa.containsMouse ? Theme.bg4 : Theme.bg2; border.color: Theme.border1
                Text { id: addTxt; anchors.centerIn: parent; text: "+ Mix"; color: Theme.text1; font.pixelSize: Theme.fs10; font.bold: true }
                MouseArea {
                    id: addMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        stripMenu.show([{ header: "Add an output mix" },
                                        { text: "Web stream", fn: function() { mixer.addMix(p.channelId, "Web") } },
                                        { text: "Social", fn: function() { mixer.addMix(p.channelId, "Social") } },
                                        { text: "Clean feed (no graphics)", fn: function() { mixer.addMix(p.channelId, "Clean") } },
                                        { text: "Second channel feed", fn: function() { mixer.addMix(p.channelId, "Feed 2") } }])
                    }
                }
            }
            Item { Layout.fillWidth: true }
            Text {
                visible: !p.onMain; color: Theme.text3; font.pixelSize: Theme.fs10; elide: Text.ElideRight; Layout.maximumWidth: 380
                text: "Faders act on " + String(p.curMix.name || p.curMix.id).toUpperCase() + " only. Strips you have not touched follow MAIN."
            }
        }

        RowLayout {
            Layout.fillWidth: true; Layout.fillHeight: true; spacing: 8
            Repeater {
                model: p.strips
                MixStrip {
                    Layout.fillHeight: true; Layout.preferredWidth: 96
                    strip: modelData
                    afv: mixer.afv
                    showAfv: p.onMain
                    soloActive: p.solo !== ""
                    onGain: function(db) {
                        if (modelData.kind === "program") mixer.program(p.channelId, { gainDb: db })
                        else mixer.lineUpdate(p.channelId, modelData.layer, { audioGainDb: db })
                    }
                    onGainDone: function(db) { if (p.onMain && modelData.kind === "line") mixer.note(p.channelId, modelData.layer, { gainDb: db <= -59.5 ? -100 : db }) }
                    onMuteToggled: {
                        if (modelData.kind === "program") mixer.program(p.channelId, { mute: !modelData.mute })
                        else { mixer.lineUpdate(p.channelId, modelData.layer, { audioMute: !modelData.mute }); if (p.onMain && modelData.kind === "line") mixer.note(p.channelId, modelData.layer, { mute: !modelData.mute }) }
                    }
                    onSoloToggled: { mixer.solo(p.channelId, modelData.solo ? "" : modelData.id) }
                    onRemoveClicked: { mixer.removeLine(p.channelId, modelData.layer) }
                    onAfvToggled: { mixer.setAfv(!mixer.afv) }
                    onMenuClicked: {
                        var m = modelData
                        stripMenu.show([
                            { header: (m.name || m.id) + (p.onMain ? "" : "  ·  " + String(p.curMix.name || p.curMix.id).toUpperCase()) },
                            { text: m.mute ? "Unmute" : "Mute", fn: function() { if (m.kind === "program") mixer.program(p.channelId, { mute: !m.mute }); else mixer.lineUpdate(p.channelId, m.layer, { audioMute: !m.mute }) } },
                            { text: m.solo ? "Clear solo" : "Solo", fn: function() { mixer.solo(p.channelId, m.solo ? "" : m.id) } },
                            { text: "Fader to 0 dB", fn: function() { if (m.kind === "program") mixer.program(p.channelId, { gainDb: 0 }); else mixer.lineUpdate(p.channelId, m.layer, { audioGainDb: 0 }) } },
                            m.kind === "line" ? { sep: true } : null,
                            m.kind === "line" ? { text: "Remove line input", danger: true, fn: function() { mixer.removeLine(p.channelId, m.layer) } } : null
                        ])
                    }
                }
            }
            Item {
                Layout.fillWidth: true; Layout.fillHeight: true
                Text { anchors.centerIn: parent; visible: p.strips.length <= 1; width: parent.width - 10; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.WordWrap; color: Theme.text3; font.pixelSize: Theme.fs11
                    text: "Add a line input to mix a sound card's line in or microphone into the program" }
            }
            MixStrip {
                Layout.fillHeight: true; Layout.preferredWidth: 96
                master: true; strip: p.master
                onGain: function(db) { mixer.master(p.channelId, db) }
            }
        }
    }
}
