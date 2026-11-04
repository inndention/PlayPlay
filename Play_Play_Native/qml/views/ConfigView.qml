import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".."
import "../ui"

/**
 * Config (PlayPlayClean's Automation / Channels tabs): channel profiles with
 * identity, raster, time zone, automation and gap-fill policy; add/remove
 * channels. Saved with PUT /channels/{id}; raster changes restart the engine.
 */
Item {
    id: view
    property string tab: "automation"
    property string chId: supervisor.activeChannelId
    // perf-app PA-3: changes only with the active channel's profile (chId is the active channel), not on every 2 s snapshot
    property var channel: ({ profile: supervisor.activeProfile })
    property var p: ({})
    property bool dirty: false
    function load() { p = channel && channel.profile ? JSON.parse(JSON.stringify(channel.profile)) : {}; dirty = false }
    onChannelChanged: { if (!dirty) load() }
    Component.onCompleted: { load() }
    function set(k, v) { var o = Object.assign({}, p); o[k] = v; p = o; dirty = true }
    function setSub(sub, k, v) { var o = Object.assign({}, p); var s = Object.assign({}, o[sub] || {}); s[k] = v; o[sub] = s; p = o; dirty = true }
    function save() { api.put("/channels/" + chId, p, function(ok) { if (ok) { dirty = false; window.toast("Saved channel " + chId, "ok"); supervisor.refresh() } }) }
    function addChannel() {
        var n = supervisor.channels.length + 1
        api.post("/channels", { id: "ch" + n, name: "Channel " + n, enabled: true, frameRate: "30000/1001", width: 1920, height: 1080, audioChannels: 2, timezone: p.timezone || "America/New_York", outputs: [] },
                 function(ok, d) { if (ok) { supervisor.refresh(); supervisor.activeChannelId = "ch" + n; window.toast("Added ch" + n, "ok") } })
    }
    property var fill: p.fill || ({})
    property var identity: p.identity || ({})
    property var extra: p

    ColumnLayout {
        anchors.fill: parent
        spacing: 10
        Rectangle {
            Layout.fillWidth: true; implicitHeight: 40; color: Theme.bg2; radius: Theme.r8; border.color: Theme.border0
            RowLayout {
                anchors.fill: parent; anchors.leftMargin: 8; anchors.rightMargin: 8; spacing: 2
                Repeater {
                    model: [ ["automation", "⚙", "Automation"], ["channels", "▣", "Channels"], ["identity", "☆", "Identity"], ["media", "▦", "Media"], ["notes", "✎", "Notes"] ]
                    CButton { text: modelData[2]; icon2: modelData[1]; variant: "ghost"; active: view.tab === modelData[0]; onClicked: { view.tab = modelData[0] } }
                }
                Item { Layout.fillWidth: true }
                Text { visible: view.dirty; text: "unsaved changes"; color: Theme.standby; font.pixelSize: Theme.fs10; font.bold: true }
                CButton { text: "Reset"; small: true; variant: "ghost"; onClicked: { view.load() } }
                CButton { text: "Save"; small: true; variant: view.dirty ? "primary" : "default"; enabled: view.dirty; onClicked: { view.save() } }
            }
        }

        AutomationPanel { visible: view.tab === "channels"; Layout.fillWidth: true }
        // --- AUTOMATION -------------------------------------------------------------------------
        CPanel {
            visible: view.tab === "automation"
            Layout.fillWidth: true; Layout.fillHeight: true
            title: "Automation — " + (view.p.name || view.chId)
            Flickable {
                anchors.fill: parent; contentHeight: autoCol.implicitHeight; clip: true
                ColumnLayout {
                    id: autoCol; width: parent.width; spacing: 18
                    ColumnLayout { spacing: 8
                        Text { text: "▣  PROGRAM AUTOMATION"; color: Theme.accent0; font.pixelSize: Theme.fs11; font.bold: true; font.letterSpacing: 1 }
                        CToggle { label: "Armed: execute the schedule (auto-push every hour, hard starts at :00)"; checked: view.p.armed !== false; onToggled: function(v) { view.set("armed", v); supervisor.arm(v) }; implicitWidth: 600 }
                        CToggle { label: "Strict: always snap to the wall clock when an item ends"; hint: "Relax lets a late item run to its end and catches up at the next anchor"; checked: !!view.p.strict; onToggled: function(v) { view.set("strict", v) }; implicitWidth: 600 }
                        CToggle { label: "Always play latest episode for series slots"; checked: view.fill.alwaysPlayLatestEpisode !== false; onToggled: function(v) { view.setSub("fill", "alwaysPlayLatestEpisode", v) }; implicitWidth: 600 }
                        CToggle { label: "Shuffle ads within each hour"; checked: view.fill.shuffleAdsInHour !== false; onToggled: function(v) { view.setSub("fill", "shuffleAdsInHour", v) }; implicitWidth: 600 }
                    }
                    ColumnLayout { spacing: 8
                        Text { text: "▣  GAP FILL SETTINGS"; color: Theme.accent0; font.pixelSize: Theme.fs11; font.bold: true; font.letterSpacing: 1 }
                        CToggle { label: "Fill gaps with station IDs"; checked: view.fill.fillGapsWithIds !== false; onToggled: function(v) { view.setSub("fill", "fillGapsWithIds", v) }; implicitWidth: 600 }
                        CToggle { label: "Fill gaps with bumpers"; checked: view.fill.fillGapsWithBumpers !== false; onToggled: function(v) { view.setSub("fill", "fillGapsWithBumpers", v) }; implicitWidth: 600 }
                        CToggle { label: "Fill gaps with filler / PSA"; checked: view.fill.fillWithFiller !== false; onToggled: function(v) { view.setSub("fill", "fillWithFiller", v) }; implicitWidth: 600 }
                        CToggle { label: "Station ID first at the top of the hour"; checked: view.fill.idAtTopOfHour !== false; onToggled: function(v) { view.setSub("fill", "idAtTopOfHour", v) }; implicitWidth: 600 }
                        RowLayout { spacing: 12
                            CField { label: "Max fill duration (seconds)"; Layout.preferredWidth: 220; mono: true; text: String(view.fill.maxFillSec !== undefined ? view.fill.maxFillSec : 60); onEdited: function(v) { view.setSub("fill", "maxFillSec", parseFloat(v) || 0) } }
                            CField { label: "Emergency loop (type)"; Layout.preferredWidth: 220; text: view.fill.emergencyType || "station_id"; onEdited: function(v) { view.setSub("fill", "emergencyType", v) } } }
                    }
                    ColumnLayout { spacing: 8
                        Text { text: "▣  PLAYBACK"; color: Theme.accent0; font.pixelSize: Theme.fs11; font.bold: true; font.letterSpacing: 1 }
                        CToggle { label: "Auto-advance to next item"; checked: view.fill.autoAdvance !== false; onToggled: function(v) { view.setSub("fill", "autoAdvance", v) }; implicitWidth: 600 }
                        CToggle { label: "Loop at end of schedule"; checked: !!view.fill.loopAtEndOfSchedule; onToggled: function(v) { view.setSub("fill", "loopAtEndOfSchedule", v) }; implicitWidth: 600 }
                        RowLayout { spacing: 12
                            CField { label: "Pre-roll (s, item preparation lead)"; Layout.preferredWidth: 220; mono: true; text: String(view.p.prerollSec !== undefined ? view.p.prerollSec : 3); onEdited: function(v) { view.set("prerollSec", parseFloat(v) || 3) } }
                            CField { label: "Default transition"; Layout.preferredWidth: 220; text: view.fill.defaultTransition || "cut"; onEdited: function(v) { view.setSub("fill", "defaultTransition", v) } } }
                    }
                    ColumnLayout { spacing: 8
                        Text { text: "▣  PLAYOUT ENGINE"; color: Theme.accent0; font.pixelSize: Theme.fs11; font.bold: true; font.letterSpacing: 1 }
                        Text { Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.text2; font.pixelSize: Theme.fs11; text: "PLAY PLAY runs its own compositor in one engine process per channel. No external playout server is used." }
                        CToggle { label: "Direct3D 11 compositing (CPU fallback automatic)"; checked: !!view.extra.gpu; onToggled: function(v) { view.set("gpu", v) }; implicitWidth: 600 }
                        CToggle { label: "Hardware decode (d3d11va)"; checked: view.extra.hwDecode !== false; onToggled: function(v) { view.set("hwDecode", v) }; implicitWidth: 600 }
                    }
                }
            }
        }

        // --- CHANNELS ---------------------------------------------------------------------------------
        RowLayout {
            visible: view.tab === "channels"
            Layout.fillWidth: true; Layout.fillHeight: true; spacing: 10
            CPanel {
                Layout.preferredWidth: 300; Layout.fillHeight: true; padding: 0
                title: "Channels"
                headerRight: [ CButton { text: "+ Add"; small: true; onClicked: { view.addChannel() } } ]
                ListView {
                    anchors.fill: parent; clip: true; model: supervisor.channelModel
                    delegate: Rectangle {
                        width: ListView.view.width; height: 48; color: modelData.profile.id === view.chId ? Theme.bg4 : index % 2 ? Theme.bg2 : Theme.bg3
                        RowLayout { anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 8; spacing: 8
                            Rectangle { width: 8; height: 8; radius: 4; color: modelData.process.connected ? Theme.ready : modelData.profile.enabled ? Theme.standby : Theme.text3 }
                            ColumnLayout { Layout.fillWidth: true; spacing: 0
                                Text { text: modelData.profile.name + "  (" + modelData.profile.id + ")"; color: Theme.text0; font.pixelSize: Theme.fs12; font.bold: true }
                                Text { text: modelData.profile.width + "x" + modelData.profile.height + " @" + modelData.profile.frameRate + "  ·  " + modelData.profile.timezone; color: Theme.text2; font.family: Theme.fontMono; font.pixelSize: Theme.fs10 } }
                            CButton { icon2: "✕"; small: true; variant: "ghost"; onClicked: { api.del("/channels/" + modelData.profile.id, function(ok) { if (ok) supervisor.refresh() }) } } }
                        MouseArea { anchors.fill: parent; z: -1; onClicked: { supervisor.activeChannelId = modelData.profile.id } }
                    }
                }
            }
            CPanel {
                Layout.fillWidth: true; Layout.fillHeight: true
                title: "Channel profile — " + view.chId
                GridLayout {
                    anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
                    columns: 3; columnSpacing: 12; rowSpacing: 10
                    CField { label: "Name"; Layout.fillWidth: true; text: view.p.name || ""; onEdited: function(v) { view.set("name", v) } }
                    CField { label: "Call letters"; Layout.fillWidth: true; text: view.p.callLetters || ""; onEdited: function(v) { view.set("callLetters", v) } }
                    CField { label: "Brand name"; Layout.fillWidth: true; text: view.p.brandName || ""; onEdited: function(v) { view.set("brandName", v) } }
                    CSelect { label: "Frame rate"; Layout.fillWidth: true; model: ["30000/1001", "60000/1001", "25/1", "50/1", "30/1", "24000/1001"]; currentIndex: Math.max(0, model.indexOf(view.p.frameRate || "30000/1001")); onActivated: function(i) { view.set("frameRate", model[i]) } }
                    CSelect { label: "Raster"; Layout.fillWidth: true; model: ["1920x1080", "1280x720", "720x480", "720x576", "3840x2160"]; currentIndex: Math.max(0, model.indexOf((view.p.width || 1920) + "x" + (view.p.height || 1080))); onActivated: function(i) { var wh = model[i].split("x"); view.set("width", parseInt(wh[0])); view.set("height", parseInt(wh[1])) } }
                    CSelect { label: "Audio channels"; Layout.fillWidth: true; model: ["2", "6", "8"]; currentIndex: Math.max(0, model.indexOf(String(view.p.audioChannels || 2))); onActivated: function(i) { view.set("audioChannels", parseInt(model[i])) } }
                    CField { label: "Time zone (IANA)"; Layout.fillWidth: true; text: view.p.timezone || ""; onEdited: function(v) { view.set("timezone", v) } }
                    CToggle { label: "Enabled (engine spawned)"; Layout.fillWidth: true; Layout.alignment: Qt.AlignBottom; checked: view.p.enabled !== false; onToggled: function(v) { view.set("enabled", v) } }
                    Text { Layout.fillWidth: true; Layout.columnSpan: 3; wrapMode: Text.WordWrap; color: Theme.text3; font.pixelSize: Theme.fs10; text: "Changing frame rate, raster or audio channels restarts the channel engine. Outputs are edited on the Output page." }
                }
            }
        }

        // --- IDENTITY ------------------------------------------------------------------------------------
        CPanel {
            visible: view.tab === "identity"
            Layout.fillWidth: true; Layout.fillHeight: true
            title: "Identity assets — " + view.chId
            GridLayout {
                anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
                columns: 2; columnSpacing: 12; rowSpacing: 10
                CField { label: "Slate captions (comma separated)"; Layout.fillWidth: true; Layout.columnSpan: 2; text: (view.identity.slateCaptions || []).join(", "); onEdited: function(v) { view.setSub("identity", "slateCaptions", v.split(",").map(function(s) { return s.trim() }).filter(function(s) { return s.length })) } }
                CField { label: "Channel bug image (PNG with alpha)"; Layout.fillWidth: true; mono: true; text: view.identity.bugAsset || ""; onEdited: function(v) { view.setSub("identity", "bugAsset", v) } }
                CField { label: "Bug text (when no image)"; Layout.fillWidth: true; text: view.identity.bugText || ""; onEdited: function(v) { view.setSub("identity", "bugText", v) } }
                CField { label: "Legal ID asset (media id or path)"; Layout.fillWidth: true; mono: true; text: view.identity.legalIdAsset || ""; onEdited: function(v) { view.setSub("identity", "legalIdAsset", v) } }
                CField { label: "Emergency slate (media id or path)"; Layout.fillWidth: true; mono: true; text: view.identity.emergencySlate || ""; onEdited: function(v) { view.setSub("identity", "emergencySlate", v) } }
                CToggle { label: "Show bug while on air"; checked: !!view.identity.bugAlwaysOn; onToggled: function(v) { view.setSub("identity", "bugAlwaysOn", v) }; implicitWidth: 300 }
                CToggle { label: "Legal ID at the top of every hour"; checked: view.identity.legalIdHourly !== false; onToggled: function(v) { view.setSub("identity", "legalIdHourly", v) }; implicitWidth: 300 }
                RowLayout { Layout.columnSpan: 2; spacing: 8
                    CButton { text: "Show bug now"; small: true; onClicked: { supervisor.cg({ action: "show", role: "bug", source: view.identity.bugAsset ? { type: "image", path: view.identity.bugAsset } : { type: "template", kind: "bug", text: view.identity.bugText || view.p.callLetters || view.p.name }, placement: { anchor: "top_right", margin: 60 }, "in": { type: "fade", durationSec: 0.5 } }) } }
                    CButton { text: "Hide bug"; small: true; onClicked: { supervisor.cg({ action: "hide", role: "bug", transition: { type: "fade", durationSec: 0.5 } }) } }
                    CButton { text: "Emergency slate"; small: true; variant: "danger"; onClicked: { supervisor.cg({ action: "show", role: "emergency", source: view.identity.emergencySlate ? { type: "file", path: view.identity.emergencySlate, loop: true } : { type: "bars", captions: view.identity.slateCaptions || ["PLEASE STAND BY"] }, "in": { type: "cut" } }) } }
                    CButton { text: "Clear emergency"; small: true; onClicked: { supervisor.cg({ action: "clear", role: "emergency" }) } } }
            }
        }

        // --- MEDIA / NOTES --------------------------------------------------------------------------------
        CPanel {
            visible: view.tab === "media"
            Layout.fillWidth: true; Layout.fillHeight: true
            title: "Media settings"
            GridLayout {
                anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
                columns: 2; columnSpacing: 12; rowSpacing: 10
                CField { label: "Media root (default scan folder)"; Layout.fillWidth: true; Layout.columnSpan: 2; mono: true; text: view.extra.mediaRoot || ""; onEdited: function(v) { view.set("mediaRoot", v) } }
                CField { label: "Watch folder for show hours"; Layout.fillWidth: true; Layout.columnSpan: 2; mono: true; text: view.extra.watchFolder || ""; onEdited: function(v) { view.set("watchFolder", v) } }
                CToggle { label: "Generate thumbnails on scan"; checked: view.extra.thumbnails !== false; onToggled: function(v) { view.set("thumbnails", v) }; implicitWidth: 300 }
                CToggle { label: "Archive media missing on disk"; checked: !!view.extra.archiveMissing; onToggled: function(v) { view.set("archiveMissing", v) }; implicitWidth: 300 }
            }
        }
        CPanel {
            visible: view.tab === "notes"
            Layout.fillWidth: true; Layout.fillHeight: true
            title: "Operator notes"
            TextArea {
                anchors.fill: parent; text: view.extra.notes || ""; color: Theme.text0; font.pixelSize: Theme.fs12; wrapMode: TextEdit.Wrap
                background: Rectangle { color: Theme.bg4; radius: Theme.r5; border.color: Theme.border1 }
                onTextChanged: { if (text !== (view.extra.notes || "")) view.set("notes", text) }
            }
        }
    }
}
