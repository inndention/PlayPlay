import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".."
import "../ui"

/// Programming: hour recipes (time patterns laid as query blocks), the ad-break recipe builder,
/// the format clock (weighted fill buckets), the autofill policy, ad contracts and a query tester.
Item {
    id: view
    property string ch: supervisor.activeChannelId
    property var recipes: []
    property var def: null
    property var builtins: []          // read-only recipe library from the Supervisor ("builtin-..." ids)
    // The list on the left: the default, the built-ins, then the station's own recipes (_ui = index into `recipes`, -1 = read-only).
    property var recipeRows: {
        var out = []
        if (def) out.push(Object.assign({}, def, { builtin: true, _ui: -1 }))
        for (var i = 0; i < builtins.length; ++i) out.push(Object.assign({}, builtins[i], { builtin: true, _ui: -1 }))
        for (var k = 0; k < recipes.length; ++k) out.push(Object.assign({}, recipes[k], { builtin: false, _ui: k }))
        return out
    }
    // Rows of the preview list: one per hour, each with its blocks and the files every block resolved to.
    property var previewRows: []
    function rowsFromHourRecipe(r) {
        return Object.keys(r.slots || {}).map(function(k) {
            var sl = r.slots[k], pv = r.previews ? r.previews[k] : null
            return { key: k, placedSec: sl.placedSec || 0, laid: (sl.items || []).map(function(it) { return (it.isLocked ? "\uD83D\uDD12 " : "") + it.title }), blocks: pv ? pv.blocks : null, picks: pv ? pv.picks : -1, unresolved: pv ? pv.unresolved : 0 }
        })
    }
    function rowsFromRecipePreview(r) {
        return (r.hours || []).map(function(h) {
            return { key: h.date + " " + pad2(h.hour) + ":00", placedSec: h.scheduledSec || 0, laid: [], blocks: h.blocks || [], picks: h.picks, unresolved: h.unresolved }
        })
    }
    /// The files a recipe would play in the next hour(s) on this channel, block by block (nothing is written).
    function previewRecipeFiles(id, name, hours) {
        api.post("/programming/recipes/preview", { recipeId: id, channel: ch, hours: hours || 1 }, function(ok, r) {
            if (!ok) { window.toast("Preview failed: " + (r && r.error ? r.error : "see log"), "error"); return }
            view.preview = r; view.previewRows = rowsFromRecipePreview(r)
            window.toast(name + ": " + r.summary.picks + " file(s) picked" + (r.summary.unresolvedBlocks ? ", " + r.summary.unresolvedBlocks + " block(s) found nothing" : ""), r.summary.unresolvedBlocks ? "error" : "ok")
        })
    }
    property var facets: ({})
    property var weights: []
    property bool weightsDefault: true
    property var autofill: ({})
    property var contracts: []
    property var preview: null
    property var tester: null
    property int tab: 0
    // editor state
    property var edit: null            // {index, r:{name, idAtTop, parts:[...]}}
    property string applyScope: "day"
    property int applyDow: new Date().getDay()
    property int applyHour: 0
    property string applyHours: ""
    property bool applyDated: false
    property bool applyEmptyOnly: false
    property bool applyKeepLocks: true
    property string applyRecipe: "default"
    // Bumped by the *Typed() functions below: the read-only displays that depend on a typed value (cycle length, share total and bar) list it
    // as a dependency, because typed edits change a row in place instead of republishing the array its ListView is built on.
    property int rev: 0

    function listStr(v) { return Array.isArray(v) ? v.join(", ") : String(v || "") }
    function toList(s) { return String(s || "").split(",").map(function(x) { return x.trim() }).filter(function(x) { return x.length }) }
    function fmtM(s) { s = Math.round(Number(s) || 0); return Math.floor(s / 60) + ":" + (s % 60 < 10 ? "0" : "") + (s % 60) }
    function parseRange(s, lo, hi) { var out = []; var parts = String(s || "").split(","); for (var i = 0; i < parts.length; ++i) { var p = parts[i].trim(); if (!p.length) continue; var m = p.match(/^(\d+)\s*-\s*(\d+)$/); if (m) { for (var k = parseInt(m[1]); k <= parseInt(m[2]); ++k) if (k >= lo && k <= hi) out.push(k) } else if (!isNaN(parseInt(p)) && parseInt(p) >= lo && parseInt(p) <= hi) out.push(parseInt(p)) } return out }

    function reload() {
        api.get("/programming/recipes", function(ok, d) { if (ok) { view.recipes = d.recipes || []; view.def = d["default"] || null; view.builtins = d.builtin || [] } })
        api.get("/programming/facets", function(ok, d) { if (ok) view.facets = d })
        if (!ch.length) return
        api.get("/channels/" + ch + "/weights", function(ok, d) { if (ok) { view.weights = d.buckets || []; view.weightsDefault = !!d.isDefault } })
        api.get("/channels/" + ch + "/autofill", function(ok, d) { if (ok) view.autofill = d })
        api.get("/traffic/contracts?channel=" + ch, function(ok, d) { if (ok) view.contracts = d.contracts || [] })
    }
    onChChanged: reload()
    Component.onCompleted: reload()

    function applyBody(extra) {
        var b = Object.assign({ scope: applyScope, dated: applyDated, emptyOnly: applyEmptyOnly, keepLocks: applyKeepLocks }, extra || {})
        if (applyScope === "day" || applyScope === "hour") b.dow = applyDow
        if (applyScope === "hour") b.hour = applyHour
        var hrs = parseRange(applyHours, 0, 23); if (hrs.length && applyScope !== "hour") b.hours = hrs
        return b
    }
    function applyRecipeNow(apply) {
        api.post("/schedule/" + ch + "/hour-recipe", applyBody({ recipeId: applyRecipe, apply: apply }), function(ok, r) {
            if (!ok) { window.toast("Recipe failed: " + (r && r.error ? r.error : ""), "error"); return }
            view.preview = r; view.previewRows = rowsFromHourRecipe(r); if (apply) window.toast((r.summary.written || 0) + " hour(s) written", "ok")
        })
    }
    function weightedNow(apply) {
        api.post("/schedule/" + ch + "/weighted-fill", applyBody({ apply: apply, oneProgramPerHour: true }), function(ok, r) {
            if (!ok) { window.toast("Weighted fill failed", "error"); return }
            view.preview = r; view.previewRows = rowsFromHourRecipe(r); if (apply) window.toast((r.summary.written || 0) + " hour(s) written, " + (r.summary.placed || 0) + " items", "ok")
        })
    }
    function clearScope() { api.post("/schedule/" + ch + "/clear", applyBody({ keepLocks: true }), function(ok, r) { if (ok) window.toast(r.cleared + " hour(s) cleared (locks kept)", "ok") }) }
    function saveRecipes(list, msg) { api.put("/programming/recipes", { recipes: list }, function(ok, d) { if (ok) { view.recipes = d.recipes || []; view.edit = null; window.toast(msg || "Recipes saved", "ok") } }) }
    function startEdit(i) {
        if (i >= 0) view.edit = { index: i, r: JSON.parse(JSON.stringify(recipes[i])) }
        else view.edit = { index: -1, r: { name: "", idAtTop: false, parts: [ { label: "Programs", types: ["program"], minutes: 10, prefer: "least_played" }, { label: "Ads", types: ["ad"], minutes: 5, prefer: "least_played" } ] } }
    }
    function editSet(f) { var e = Object.assign({}, view.edit); e.r = Object.assign({}, e.r); f(e.r); view.edit = e }
    function partSet(i, k, v) { editSet(function(r) { var parts = r.parts.slice(); parts[i] = Object.assign({}, parts[i]); parts[i][k] = v; r.parts = parts }) }
    // Typed (per keystroke) edits of a row field: written into the row IN PLACE, never republished. The row lists use the row COUNT as their
    // model and every row reads its data by index, so a keystroke neither rebuilds the rows nor rewrites the field being typed in. (Before,
    // every keystroke assigned a new array to the ListView model: the delegates were destroyed, focus was lost and "125" became "1".)
    function partTyped(i, k, v) { if (!edit || !edit.r.parts[i]) return; edit.r.parts[i][k] = v; rev++ }
    function bucketTyped(i, k, v) { if (!weights[i]) return; weights[i][k] = v; rev++ }
    function contractTyped(i, k, v) { if (!contracts[i]) return; contracts[i][k] = v; rev++ }
    function afTyped(k, v) { autofill[k] = v }
    function saveWeights() { api.put("/channels/" + ch + "/weights", { buckets: weights }, function(ok, d) { if (ok) { view.weights = d.buckets || []; view.weightsDefault = false; window.toast("Program mix saved", "ok") } }) }
    function bucketSet(i, k, v) { var w = weights.slice(); w[i] = Object.assign({}, w[i]); w[i][k] = v; view.weights = w }
    function saveAutofill() { api.put("/channels/" + ch + "/autofill", autofill, function(ok, d) { if (ok) { view.autofill = d; window.toast("Autofill policy saved", "ok") } }) }
    function afSet(k, v) { var a = Object.assign({}, autofill); a[k] = v; view.autofill = a }
    function saveContracts() { api.put("/traffic/contracts", { contracts: contracts }, function(ok) { if (ok) { window.toast("Contracts saved", "ok"); reload() } }) }
    function contractSet(i, k, v) { var c = contracts.slice(); c[i] = Object.assign({}, c[i]); c[i][k] = v; view.contracts = c }
    function runTester() {
        var body = { channel: ch, n: 40 }
        if (tFolder.text.trim().length) body.folder = { folder: tFolder.text.trim(), targetSec: (parseFloat(tMin.text) || 0) * 60 }
        else body.block = { types: toList(tTypes.text), targetSec: (parseFloat(tMin.text) || 0) * 60, prefer: tPrefer.currentText, categories: toList(tCats.text), subcategories: toList(tSubs.text), series: toList(tSeries.text) }
        api.post("/programming/query/preview", body, function(ok, d) { if (ok) view.tester = d; else window.toast("Tester: " + (d && d.error ? d.error : "failed"), "error") })
    }
    readonly property var prefers: ["least_played", "newest", "oldest", "most_played", "random", "longest", "shortest"]
    readonly property var dows: ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"]

    ColumnLayout {
        anchors.fill: parent
        spacing: 10
        RowLayout {
            Layout.fillWidth: true; spacing: 6
            Repeater {
                model: ["Hour recipes", "Format clock", "Autofill", "Ad contracts", "Query tester"]
                CButton { text: modelData; small: true; variant: view.tab === index ? "primary" : "ghost"; onClicked: { view.tab = index } }
            }
            Item { Layout.fillWidth: true }
            Text { text: "channel " + view.ch; color: Theme.text3; font.pixelSize: Theme.fs10 }
            CButton { icon2: "↻"; small: true; variant: "ghost"; onClicked: { view.reload() } }
        }
        StackLayout {
            Layout.fillWidth: true; Layout.fillHeight: true
            currentIndex: view.tab

            // ---------------------------------------------------------------- recipes
            RowLayout {
                spacing: 10
                ColumnLayout {
                    // A layout inside a layout fills the width by default: with only a preferredWidth this column took the whole row and left the
                    // editor / apply column 0-2 px wide. Fixed share for the list, the rest (never under 380 px) for the editor.
                    Layout.fillWidth: false; Layout.preferredWidth: 420; Layout.minimumWidth: 320; Layout.fillHeight: true; spacing: 10
                    CPanel {
                        Layout.fillWidth: true; Layout.fillHeight: true; title: "Hour recipes"; padding: 0
                        headerRight: [ CButton { text: "+ New"; small: true; onClicked: { view.startEdit(-1) } } ]
                        ListView {
                            anchors.fill: parent; clip: true
                            model: view.recipeRows
                            ScrollBar.vertical: ScrollBar {}
                            delegate: Rectangle {
                                width: ListView.view.width; height: 48; color: index % 2 ? Theme.bg2 : Theme.bg3
                                Rectangle { width: 3; height: parent.height; color: modelData.builtin ? Theme.text3 : Theme.accent0 }
                                RowLayout {
                                    anchors.fill: parent; anchors.leftMargin: 10; anchors.rightMargin: 8; spacing: 8
                                    ColumnLayout { Layout.fillWidth: true; spacing: 1
                                        RowLayout { spacing: 6
                                            Text { text: modelData.name; color: Theme.text0; font.pixelSize: Theme.fs12; font.bold: true; elide: Text.ElideRight; Layout.maximumWidth: 300 }
                                            CBadge { visible: !!modelData.builtin; text: "built in"; tint: Theme.text3 }
                                            Text { text: "cycle " + view.fmtM(modelData.cycleSec || 0) + (modelData.idAtTop ? " · ID at :00" : ""); color: Theme.text3; font.pixelSize: Theme.fs10 } }
                                        Text { text: (modelData.parts || []).map(function(p) { return p.label + " " + p.minutes + "m" }).join(" · "); color: Theme.text2; font.pixelSize: Theme.fs10; elide: Text.ElideRight; Layout.fillWidth: true }
                                    }
                                    CButton { text: "Files"; small: true; onClicked: { view.previewRecipeFiles(modelData.id, modelData.name, 1) } ToolTip.text: "Which files this recipe would play in the next hour on " + view.ch + " (nothing is written)" }
                                    CButton { text: "Apply…"; small: true; onClicked: { view.applyRecipe = modelData.id || "default"; window.toast("Recipe selected — set the scope on the right and press Apply", "ok") } }
                                    CButton { icon2: "✎"; small: true; variant: "ghost"; onClicked: { if (modelData.builtin) { var c = JSON.parse(JSON.stringify(modelData)); delete c.builtin; delete c._ui; c.id = undefined; c.name = (modelData.id === "default" ? "My default" : modelData.name + " (copy)"); view.edit = { index: -1, r: c } } else view.startEdit(modelData._ui) } ToolTip.text: modelData.builtin ? "Copy into your own recipes to edit" : "Edit" }
                                    CButton { icon2: "✕"; small: true; variant: "ghost"; visible: !modelData.builtin; onClicked: { var l = view.recipes.slice(); l.splice(modelData._ui, 1); view.saveRecipes(l, "Recipe deleted") } }
                                }
                            }
                        }
                    }
                    CPanel {
                        Layout.fillWidth: true; Layout.preferredHeight: 280; title: "Ad-break recipe builder"
                        headerRight: [ Text { text: "programs with a break every N minutes"; color: Theme.text3; font.pixelSize: Theme.fs10 } ]
                        GridLayout {
                            anchors.fill: parent; columns: 3; columnSpacing: 8; rowSpacing: 6
                            CField { id: abProg; label: "Program min"; Layout.preferredWidth: 80; mono: true; text: "12" }
                            CField { id: abAd; label: "Break min"; Layout.preferredWidth: 80; mono: true; text: "3" }
                            CField { id: abGen; label: "Autogen min"; Layout.preferredWidth: 80; mono: true; text: "0" }
                            CField { id: abTypes; label: "Program types"; Layout.preferredWidth: 120; text: "program" }
                            CField { id: abAdTypes; label: "Break types"; Layout.preferredWidth: 130; text: "ad, promo, psa" }
                            CSelect { id: abPrefer; label: "Play"; Layout.preferredWidth: 120; model: view.prefers }
                            CField { id: abCats; label: "Program categories"; Layout.preferredWidth: 150; placeholder: "any" }
                            CField { id: abSeries; label: "Series"; Layout.preferredWidth: 150; placeholder: "any" }
                            CToggle { id: abIdBreak; label: "ID before break"; implicitWidth: 130 }
                            CToggle { id: abIdTop; label: "ID at :00"; implicitWidth: 100; checked: true }
                            CField { id: abName; label: "Name"; Layout.preferredWidth: 150; placeholder: "optional" }
                            CButton { text: "Build & save"; small: true; variant: "primary"; onClicked: {
                                api.post("/programming/recipes/ad-break", { programMin: parseFloat(abProg.text) || 12, adMin: parseFloat(abAd.text) || 3, autogenMin: parseFloat(abGen.text) || 0, programTypes: view.toList(abTypes.text), adTypes: view.toList(abAdTypes.text), prefer: abPrefer.currentText, idEveryBreak: abIdBreak.checked, idAtTop: abIdTop.checked, programCategories: view.toList(abCats.text), programSeries: view.toList(abSeries.text), name: abName.text.trim().length ? abName.text.trim() : undefined, save: true },
                                    function(ok, r) { if (ok) { window.toast("Recipe saved: " + r.name, "ok"); view.applyRecipe = r.id; view.reload() } }) } }
                        }
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true; Layout.minimumWidth: 380; Layout.fillHeight: true; spacing: 10
                    // recipe editor
                    CPanel {
                        visible: view.edit !== null
                        Layout.fillWidth: true; Layout.preferredHeight: 300; title: view.edit && view.edit.index >= 0 ? "Edit recipe" : "New recipe"; padding: 0
                        headerRight: [
                            CButton { text: "Cancel"; small: true; variant: "ghost"; onClicked: { view.edit = null } },
                            CButton { text: "Save recipe"; small: true; variant: "primary"; onClicked: { if (!view.edit.r.name.trim().length) { window.toast("Name the recipe", "error"); return } var l = view.recipes.slice(); if (view.edit.index >= 0) l[view.edit.index] = view.edit.r; else l.push(view.edit.r); view.saveRecipes(l) } }
                        ]
                        ColumnLayout {
                            anchors.fill: parent; spacing: 0
                            // a Flow: this column is only ~380 px wide at the 1280 px window, so the header and every part row wrap
                            Flow {
                                Layout.fillWidth: true; Layout.margins: 8; spacing: 10
                                CField { label: "Name"; width: 200; text: view.edit ? view.edit.r.name : ""; onEdited: function(v) { view.editSet(function(r) { r.name = v }) } }
                                // CToggle flips itself on a click, which kills a `checked:` binding (opening another recipe then showed the old state): use a Binding
                                CToggle { id: idTopT; label: "Station ID at :00"; implicitWidth: 150; onToggled: function(v) { view.editSet(function(r) { r.idAtTop = v }) } }
                                Binding { target: idTopT; property: "checked"; value: view.edit ? !!view.edit.r.idAtTop : false }
                                Text { height: 30; verticalAlignment: Text.AlignBottom; text: view.rev < 0 || !view.edit ? "" : "cycle " + view.fmtM(view.edit.r.parts.reduce(function(a, p) { return a + (parseFloat(p.minutes) || 0) * 60 }, 0)) + " — repeats until the hour is full"; color: Theme.text3; font.pixelSize: Theme.fs10 }
                                CButton { height: 30; text: "+ part"; small: true; onClicked: { view.editSet(function(r) { r.parts = r.parts.concat([{ label: "Ads", types: ["ad"], minutes: 3, prefer: "least_played" }]) }) } }
                            }
                            ListView {
                                Layout.fillWidth: true; Layout.fillHeight: true; clip: true
                                // the model is the part COUNT and a row reads view.edit.r.parts[index]: see partTyped()
                                model: view.edit ? view.edit.r.parts.length : 0
                                ScrollBar.vertical: ScrollBar {}
                                delegate: Rectangle {
                                    property var row: view.edit && view.edit.r.parts[index] ? view.edit.r.parts[index] : ({})
                                    width: ListView.view.width; height: partFlow.implicitHeight + 10; color: index % 2 ? Theme.bg2 : Theme.bg3
                                    Flow {
                                        id: partFlow
                                        anchors.left: parent.left; anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; anchors.leftMargin: 8; anchors.rightMargin: 8; spacing: 6
                                        CField { width: 100; text: row.label || ""; placeholder: "label"; onEdited: function(v) { view.partTyped(index, "label", v) } }
                                        CField { width: 130; text: view.listStr(row.types); placeholder: "types"; onEdited: function(v) { view.partTyped(index, "types", view.toList(v)) } }
                                        CField { width: 56; mono: true; text: String(row.minutes); onEdited: function(v) { view.partTyped(index, "minutes", parseFloat(v) || 0) } }
                                        CSelect { width: 120; model: view.prefers; currentIndex: Math.max(0, view.prefers.indexOf(row.prefer || "least_played")); onActivated: function(i) { view.partSet(index, "prefer", view.prefers[i]) } }
                                        CField { width: 130; text: view.listStr(row.categories); placeholder: "categories"; onEdited: function(v) { view.partTyped(index, "categories", view.toList(v)) } }
                                        CField { width: 130; text: view.listStr(row.series); placeholder: "series"; onEdited: function(v) { view.partTyped(index, "series", view.toList(v)) } }
                                        CButton { height: 30; icon2: "▲"; small: true; variant: "ghost"; onClicked: { if (index > 0) view.editSet(function(r) { var p = r.parts.slice(); var t = p[index]; p[index] = p[index - 1]; p[index - 1] = t; r.parts = p }) } }
                                        CButton { height: 30; icon2: "▼"; small: true; variant: "ghost"; onClicked: { view.editSet(function(r) { if (index < r.parts.length - 1) { var p = r.parts.slice(); var t = p[index]; p[index] = p[index + 1]; p[index + 1] = t; r.parts = p } }) } }
                                        CButton { height: 30; icon2: "✕"; small: true; variant: "ghost"; onClicked: { view.editSet(function(r) { var p = r.parts.slice(); p.splice(index, 1); r.parts = p }) } }
                                    }
                                }
                            }
                        }
                    }
                    // apply panel
                    CPanel {
                        Layout.fillWidth: true; Layout.fillHeight: true; title: "Lay recipe → " + view.ch
                        headerRight: [ Text { text: "late-bound query blocks; locked items survive"; color: Theme.text3; font.pixelSize: Theme.fs10 } ]
                        ColumnLayout {
                            anchors.fill: parent; spacing: 8
                            Flow {
                                Layout.fillWidth: true; spacing: 8
                                CSelect { label: "Recipe"; width: 240; model: view.recipeRows.map(function(r) { return (r.builtin ? "\u2605 " : "") + r.name }); currentIndex: Math.max(0, view.recipeRows.map(function(r) { return r.id || "default" }).indexOf(view.applyRecipe)); onActivated: function(i) { view.applyRecipe = view.recipeRows[i].id || "default" } }
                                CSelect { label: "Scope"; width: 90; model: ["hour", "day", "week", "hours"]; currentIndex: Math.max(0, model.indexOf(view.applyScope)); onActivated: function(i) { view.applyScope = model[i] } }
                                CSelect { label: "Day"; visible: view.applyScope === "day" || view.applyScope === "hour"; width: 80; model: view.dows; currentIndex: view.applyDow; onActivated: function(i) { view.applyDow = i } }
                                CField { label: "Hour"; visible: view.applyScope === "hour"; width: 60; mono: true; text: String(view.applyHour); onEdited: function(v) { view.applyHour = parseInt(v) || 0 } }
                                CField { label: "Hours"; visible: view.applyScope !== "hour"; width: 140; placeholder: "all, e.g. 6-23"; text: view.applyHours; onEdited: function(v) { view.applyHours = v } }
                            }
                            Flow {
                                Layout.fillWidth: true; spacing: 14
                                CToggle { label: "Dated overrides (this week)"; implicitWidth: 200; checked: view.applyDated; onToggled: function(v) { view.applyDated = v } }
                                CToggle { label: "Empty hours only"; implicitWidth: 150; checked: view.applyEmptyOnly; onToggled: function(v) { view.applyEmptyOnly = v } }
                                CToggle { label: "Keep locked items"; implicitWidth: 150; checked: view.applyKeepLocks; onToggled: function(v) { view.applyKeepLocks = v } }
                            }
                            Flow {
                                Layout.fillWidth: true; spacing: 8
                                CButton { height: 30; text: "Preview"; small: true; onClicked: { view.applyRecipeNow(false) } }
                                CButton { height: 30; text: "Apply recipe"; small: true; variant: "primary"; onClicked: { view.applyRecipeNow(true) } }
                                CButton { height: 30; text: "Weighted fill (format clock)"; small: true; onClicked: { view.weightedNow(true) } }
                                CButton { height: 30; text: "Clear scope (keep locks)"; small: true; variant: "ghost"; onClicked: { view.clearScope() } }
                            }
                            ListView {
                                Layout.fillWidth: true; Layout.fillHeight: true; clip: true; spacing: 2
                                model: view.previewRows
                                ScrollBar.vertical: ScrollBar {}
                                delegate: Rectangle {
                                    width: ListView.view.width; height: hourCol.implicitHeight + 12; color: index % 2 ? Theme.bg2 : Theme.bg3
                                    ColumnLayout { id: hourCol; anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; anchors.margins: 6; spacing: 2
                                        RowLayout { spacing: 8
                                            Text { text: modelData.key; color: Theme.text0; font.family: Theme.fontMono; font.pixelSize: Theme.fs11; font.bold: true }
                                            Text { text: view.fmtM(modelData.placedSec) + (modelData.picks >= 0 ? " · " + modelData.picks + " file(s)" : " · " + modelData.laid.length + " block(s)"); color: Theme.text3; font.pixelSize: Theme.fs10 }
                                            CBadge { visible: modelData.unresolved > 0; text: modelData.unresolved + " found nothing"; tint: Theme.danger } }
                                        // without a resolve (applied, weighted fill): the laid blocks only
                                        Text { visible: !modelData.blocks; text: modelData.laid.join(" · "); color: Theme.text2; font.pixelSize: Theme.fs10; elide: Text.ElideRight; Layout.fillWidth: true }
                                        Repeater {
                                            model: modelData.blocks || []
                                            delegate: RowLayout { Layout.fillWidth: true; spacing: 6
                                                Text { text: modelData.label; color: modelData.kind === "fill" ? Theme.text3 : Theme.accent0; font.pixelSize: Theme.fs10; font.bold: true; elide: Text.ElideRight; Layout.preferredWidth: 150 }
                                                Text { Layout.fillWidth: true; font.pixelSize: Theme.fs10; elide: Text.ElideRight
                                                    color: (modelData.picks || []).length ? Theme.text1 : Theme.danger
                                                    text: (modelData.picks || []).length
                                                          ? modelData.picks.map(function(p) { return p.title + " " + view.fmtM(p.duration) + (p.overrun ? " (cut)" : "") + (p.repeat ? " (repeat)" : "") }).join(" \u00B7 ")
                                                          : "nothing: " + (modelData.note || "no match") } }
                                        }
                                    }
                                }
                                Text { anchors.centerIn: parent; visible: !view.previewRows.length; text: "Preview, or press Files on a recipe, to see what each block will play"; color: Theme.text3; font.pixelSize: Theme.fs11 }
                            }
                        }
                    }
                }
            }

            // ---------------------------------------------------------------- format clock
            CPanel {
                title: "Format clock — program mix for " + view.ch; padding: 0
                headerRight: [
                    CBadge { visible: view.weightsDefault; text: "default"; tint: Theme.text3 },
                    Text { property real tot: view.rev < 0 ? 0 : view.weights.reduce(function(a, b) { return a + (parseFloat(b.pct) || 0) }, 0); text: tot.toFixed(0) + "%"; color: Math.abs(tot - 100) < 0.5 ? Theme.ready : Theme.warn; font.family: Theme.fontMono; font.pixelSize: Theme.fs12; font.bold: true },
                    CButton { text: "+ bucket"; small: true; onClicked: { view.weights = view.weights.concat([{ label: "New", types: ["promo"], categories: [], subcategories: [], series: "", pct: 5 }]) } },
                    CButton { text: "Weighted fill…"; small: true; onClicked: { view.tab = 0; window.toast("Set the scope in the recipe panel, then press Weighted fill", "ok") } },
                    CButton { text: "Save"; small: true; variant: "primary"; onClicked: { view.saveWeights() } }
                ]
                ColumnLayout {
                    anchors.fill: parent; spacing: 0
                    Text { Layout.fillWidth: true; Layout.margins: 10; wrapMode: Text.WordWrap; color: Theme.text2; font.pixelSize: Theme.fs11
                        text: "Shares are airtime of the hour. The first matching bucket wins, so specific buckets (a series, a category) go before broad ones. Rotation inside a bucket: least played → longest since aired → newest in library; one program per hour, long programs are segmented and carried into the next hour." }
                    Rectangle { Layout.fillWidth: true; Layout.leftMargin: 10; Layout.rightMargin: 10; height: 12; radius: 3; color: Theme.bg4; clip: true
                        Row { anchors.fill: parent
                            Repeater { model: view.weights.length
                                Rectangle { property var row: view.weights[index] || ({}); property real tot: view.rev < 0 ? 1 : Math.max(1, view.weights.reduce(function(a, b) { return a + (parseFloat(b.pct) || 0) }, 0)); width: view.rev < 0 ? 0 : parent.width * Math.min(1, (parseFloat(row.pct) || 0) / tot); height: parent.height; color: Qt.hsla(((index * 67) % 360) / 360, 0.6, 0.45, 1) } } } }
                    ListView {
                        Layout.fillWidth: true; Layout.fillHeight: true; Layout.topMargin: 8; clip: true
                        // the model is the bucket COUNT and a row reads view.weights[index]: see bucketTyped()
                        model: view.weights.length
                        ScrollBar.vertical: ScrollBar {}
                        delegate: Rectangle {
                            property var row: view.weights[index] || ({})
                            width: ListView.view.width; height: bucketFlow.implicitHeight + 10; color: index % 2 ? Theme.bg2 : Theme.bg3
                            Flow {
                                id: bucketFlow
                                anchors.left: parent.left; anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; anchors.leftMargin: 8; anchors.rightMargin: 8; spacing: 6
                                Item { width: 12; height: 30; Rectangle { anchors.centerIn: parent; width: 10; height: 10; radius: 5; color: Qt.hsla(((index * 67) % 360) / 360, 0.6, 0.45, 1) } }
                                CField { width: 120; text: row.label || ""; onEdited: function(v) { view.bucketTyped(index, "label", v) } }
                                CField { width: 160; text: view.listStr(row.types); placeholder: "types (any)"; onEdited: function(v) { view.bucketTyped(index, "types", view.toList(v)) } }
                                CField { width: 140; text: view.listStr(row.categories); placeholder: "categories (any)"; onEdited: function(v) { view.bucketTyped(index, "categories", view.toList(v)) } }
                                CField { width: 130; text: view.listStr(row.subcategories); placeholder: "subcategories"; onEdited: function(v) { view.bucketTyped(index, "subcategories", view.toList(v)) } }
                                CField { width: 120; text: row.series || ""; placeholder: "series"; onEdited: function(v) { view.bucketTyped(index, "series", v) } }
                                CField { width: 56; mono: true; text: String(row.pct); onEdited: function(v) { view.bucketTyped(index, "pct", parseFloat(v) || 0) } }
                                Text { height: 30; verticalAlignment: Text.AlignVCenter; text: "%"; color: Theme.text2; font.pixelSize: Theme.fs11 }
                                CField { width: 56; mono: true; text: String(row.maxPerHour || 0); placeholder: "max/h"; onEdited: function(v) { view.bucketTyped(index, "maxPerHour", parseInt(v) || 0) } }
                                CButton { height: 30; icon2: "▲"; small: true; variant: "ghost"; onClicked: { if (index > 0) { var w = view.weights.slice(); var t = w[index]; w[index] = w[index - 1]; w[index - 1] = t; view.weights = w } } }
                                CButton { height: 30; icon2: "▼"; small: true; variant: "ghost"; onClicked: { if (index < view.weights.length - 1) { var w = view.weights.slice(); var t = w[index]; w[index] = w[index + 1]; w[index + 1] = t; view.weights = w } } }
                                CButton { height: 30; icon2: "✕"; small: true; variant: "ghost"; onClicked: { var w = view.weights.slice(); w.splice(index, 1); view.weights = w } }
                            }
                        }
                    }
                }
            }

            // ---------------------------------------------------------------- autofill
            CPanel {
                title: "Autofill policy — " + view.ch
                headerRight: [ CButton { text: "Save"; small: true; variant: "primary"; onClicked: { view.saveAutofill() } } ]
                ColumnLayout {
                    anchors.fill: parent; spacing: 12
                    // CToggle flips itself on a click, which kills a `checked:` binding (the switches then ignored the policy of the next channel): use Bindings
                    CToggle { id: afEnabledT; label: "Fill empty hours automatically when the executor builds them"; implicitWidth: 460; onToggled: function(v) { view.afSet("enabled", v) } }
                    Binding { target: afEnabledT; property: "checked"; value: !!view.autofill.enabled }
                    Text { Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Theme.text2; font.pixelSize: Theme.fs11; text: "Nothing is written to the schedule: an empty hour is shaped at air from the recipe (late-bound blocks) or the format clock (seeded by the hour, so it stays stable while it airs)." }
                    RowLayout {
                        spacing: 10
                        CSelect { label: "Mode"; Layout.preferredWidth: 120; model: ["recipe", "weighted"]; currentIndex: view.autofill.mode === "weighted" ? 1 : 0; onActivated: function(i) { view.afSet("mode", model[i]) } }
                        CSelect { label: "Recipe"; Layout.preferredWidth: 220; model: ["Default (built in)"].concat(view.recipes.map(function(r) { return r.name })); currentIndex: Math.max(0, [""].concat(view.recipes.map(function(r) { return r.id })).indexOf(view.autofill.recipeId || "")); onActivated: function(i) { view.afSet("recipeId", i === 0 ? "" : view.recipes[i - 1].id) } }
                        // typed fields: in place (afTyped), so "0.5" / "id, promo, ad" can be typed (the text used to be rewritten to the parsed value after every key)
                        CField { label: "No-repeat (min)"; Layout.preferredWidth: 90; mono: true; text: String(view.autofill.noRepeatMin !== undefined ? view.autofill.noRepeatMin : 30); onEdited: function(v) { view.afTyped("noRepeatMin", parseFloat(v) || 0) } }
                        CToggle { id: afPacingT; label: "Ad-contract pacing"; implicitWidth: 160; onToggled: function(v) { view.afSet("adPacing", v) } }
                        Binding { target: afPacingT; property: "checked"; value: view.autofill.adPacing !== false }
                    }
                    CField { label: "Interstitial order at air"; Layout.preferredWidth: 420; text: view.listStr(view.autofill.order); onEdited: function(v) { view.afTyped("order", view.toList(v)) } }
                    RowLayout { spacing: 8
                        CButton { text: "Re-roll this channel's blocks now"; small: true; onClicked: { api.post("/channels/" + view.ch + "/reroll", {}, function(ok) { if (ok) window.toast("Blocks re-rolled", "ok") }) } }
                        Text { text: "forgets the current hour's query-block picks and rebuilds"; color: Theme.text3; font.pixelSize: Theme.fs10 } }
                    Item { Layout.fillHeight: true }
                }
            }

            // ---------------------------------------------------------------- contracts
            CPanel {
                title: "Ad contracts"; padding: 0
                headerRight: [
                    CButton { text: "+ contract"; small: true; onClicked: { view.contracts = view.contracts.concat([{ id: "c" + Date.now().toString(36), client: "", title: "", mediaIds: [], spotsTotal: 0, spotsPerDay: 0, startDate: Qt.formatDate(new Date(), "yyyy-MM-dd"), endDate: "", channels: [], status: "active" }]) } },
                    CButton { text: "Report"; small: true; variant: "ghost"; onClicked: { Qt.openUrlExternally(api.rawUrl("/reports/contracts.html")) } },
                    CButton { text: "Save"; small: true; variant: "primary"; onClicked: { view.saveContracts() } }
                ]
                ListView {
                    anchors.fill: parent; clip: true
                    // the model is the contract COUNT and a row reads view.contracts[index]: see contractTyped()
                    model: view.contracts.length
                    ScrollBar.vertical: ScrollBar {}
                    delegate: Rectangle {
                        property var row: view.contracts[index] || ({})
                        width: ListView.view.width; height: contractCol.implicitHeight + 12; color: index % 2 ? Theme.bg2 : Theme.bg3
                        ColumnLayout {
                            id: contractCol
                            anchors.left: parent.left; anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; anchors.leftMargin: 6; anchors.rightMargin: 6; spacing: 4
                            Flow {
                                Layout.fillWidth: true; spacing: 6
                                CField { width: 120; text: row.client || ""; placeholder: "client"; onEdited: function(v) { view.contractTyped(index, "client", v) } }
                                CField { width: 140; text: row.title || ""; placeholder: "title"; onEdited: function(v) { view.contractTyped(index, "title", v) } }
                                CField { width: 240; text: view.listStr(row.mediaIds); placeholder: "media ids (comma separated)"; mono: true; onEdited: function(v) { view.contractTyped(index, "mediaIds", view.toList(v)) } }
                                CField { width: 60; mono: true; text: String(row.spotsTotal || 0); placeholder: "spots"; onEdited: function(v) { view.contractTyped(index, "spotsTotal", parseInt(v) || 0) } }
                                CField { width: 56; mono: true; text: String(row.spotsPerDay || 0); placeholder: "/day"; onEdited: function(v) { view.contractTyped(index, "spotsPerDay", parseInt(v) || 0) } }
                                CField { width: 96; mono: true; text: row.startDate || ""; placeholder: "start"; onEdited: function(v) { view.contractTyped(index, "startDate", v) } }
                                CField { width: 96; mono: true; text: row.endDate || ""; placeholder: "end"; onEdited: function(v) { view.contractTyped(index, "endDate", v) } }
                                CSelect { width: 90; model: ["active", "paused", "done"]; currentIndex: Math.max(0, model.indexOf(row.status || "active")); onActivated: function(i) { view.contractSet(index, "status", model[i]) } }
                                CButton { height: 30; icon2: "✕"; small: true; variant: "ghost"; onClicked: { var c = view.contracts.slice(); c.splice(index, 1); view.contracts = c } }
                            }
                            RowLayout {
                                spacing: 8
                                Rectangle { Layout.preferredWidth: 220; height: 6; radius: 3; color: Theme.bg4
                                    Rectangle { width: parent.width * Math.min(1, (row.pacingPct || 0) / 100); height: parent.height; radius: 3; color: (row.pacingPct || 0) < 90 ? Theme.warn : Theme.ready; visible: row.target !== undefined } }
                                Text { text: row.target !== undefined ? (row.aired + "/" + row.spotsTotal + " aired · target " + Math.round(row.target) + " · " + row.daysLeft + " d left" + (row.projectedShortfall > 0 ? " · short " + Math.round(row.projectedShortfall) : "")) : (row.spotsPerDay ? "today " + (row.airedToday || 0) + "/" + row.spotsPerDay : (row.aired || 0) + " aired"); color: Theme.text2; font.pixelSize: Theme.fs10 }
                            }
                        }
                    }
                    Text { anchors.centerIn: parent; visible: view.contracts.length === 0; text: "No contracts. Behind-pace campaigns float to the top when ad blocks resolve."; color: Theme.text3; font.pixelSize: Theme.fs11 }
                }
            }

            // ---------------------------------------------------------------- query tester
            CPanel {
                title: "Query block tester — what resolves right now on " + view.ch; padding: 0
                ColumnLayout {
                    anchors.fill: parent; spacing: 0
                    GridLayout {
                        Layout.fillWidth: true; Layout.margins: 8; columns: 7; columnSpacing: 8; rowSpacing: 6
                        CField { id: tTypes; label: "Types"; Layout.preferredWidth: 150; text: "program" }
                        CField { id: tMin; label: "Target min"; Layout.preferredWidth: 70; mono: true; text: "10" }
                        CSelect { id: tPrefer; label: "Prefer"; Layout.preferredWidth: 130; model: view.prefers }
                        CField { id: tCats; label: "Categories"; Layout.preferredWidth: 140; placeholder: "any" }
                        CField { id: tSubs; label: "Subcategories"; Layout.preferredWidth: 130; placeholder: "any" }
                        CField { id: tSeries; label: "Series"; Layout.preferredWidth: 140; placeholder: "any" }
                        CButton { text: "Resolve"; small: true; variant: "primary"; Layout.alignment: Qt.AlignBottom; onClicked: { view.runTester() } }
                        CField { id: tFolder; label: "…or folder block"; Layout.columnSpan: 4; Layout.preferredWidth: 400; placeholder: "folder path, category or tag (overrides the query above)" }
                        Text { Layout.columnSpan: 3; text: view.tester ? (view.tester.label + (view.tester.why ? "  —  " + view.tester.why : "") + "\npicks: " + (view.tester.picks || []).map(function(p) { return p.title + " (" + view.fmtM(p.duration) + (p.overrun ? ", overrun" : "") + (p.repeat ? ", repeat" : "") + ")" }).join(" · ")) : "Facets: " + ["types", "categories", "subcategories", "series"].map(function(k) { return k + " " + ((view.facets[k] || []).length) }).join(", "); color: Theme.text2; font.pixelSize: Theme.fs10; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    }
                    ListView {
                        Layout.fillWidth: true; Layout.fillHeight: true; clip: true
                        model: view.tester ? (view.tester.candidates || []) : []
                        ScrollBar.vertical: ScrollBar {}
                        delegate: Rectangle {
                            width: ListView.view.width; height: 30
                            property bool picked: view.tester && (view.tester.picks || []).some(function(p) { return p.id === modelData.id })
                            color: picked ? Qt.rgba(0, 0.83, 1, 0.12) : index % 2 ? Theme.bg2 : Theme.bg3
                            RowLayout {
                                anchors.fill: parent; anchors.leftMargin: 10; anchors.rightMargin: 10; spacing: 8
                                Text { text: index + 1; color: Theme.text3; font.family: Theme.fontMono; font.pixelSize: Theme.fs10; Layout.preferredWidth: 22 }
                                Text { text: modelData.title; color: Theme.text0; font.pixelSize: Theme.fs11; elide: Text.ElideRight; Layout.fillWidth: true }
                                CBadge { visible: !!modelData.repeat; text: "repeat"; tint: Theme.warn }
                                CBadge { text: modelData.type || ""; tint: Theme.typeColor(modelData.type) }
                                Text { text: view.fmtM(modelData.duration); color: Theme.text1; font.family: Theme.fontMono; font.pixelSize: Theme.fs10; Layout.preferredWidth: 46 }
                                Text { text: (modelData.playCount || 0) + " plays"; color: Theme.text2; font.pixelSize: Theme.fs10; Layout.preferredWidth: 60 }
                                Text { text: modelData.lastPlayedMs ? Qt.formatDateTime(new Date(modelData.lastPlayedMs), "MM-dd hh:mm") : "never"; color: Theme.text3; font.family: Theme.fontMono; font.pixelSize: Theme.fs10; Layout.preferredWidth: 80 }
                                Text { text: [modelData.category, modelData.subcategory, modelData.series].filter(function(x) { return x }).join(" / "); color: Theme.text3; font.pixelSize: Theme.fs10; elide: Text.ElideRight; Layout.preferredWidth: 200 }
                            }
                        }
                    }
                }
            }
        }
    }
}
