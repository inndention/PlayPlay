import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ".."

/**
 * CMenu: a themed context menu built from a plain JS list, plus the clipboard and Properties helpers every menu needs.
 *
 *   CMenu { id: ctx }
 *   MouseArea { acceptedButtons: Qt.RightButton; onClicked: ctx.show([
 *       { header: "Row" },
 *       { text: "Copy title", hint: "Ctrl+C", fn: function() { ctx.copyText(title) } },
 *       { text: "Delete", danger: true, enabled: canDelete, fn: function() { ... } },
 *       { sep: true },
 *       { text: "Move to", sub: [ { text: "Top", fn: ... }, { text: "Bottom", fn: ... } ] },
 *       { text: "Properties…", fn: function() { ctx.showProps("Item", modelData) } } ]) }
 *
 * Spec keys: text, fn, hint, danger, enabled (default true), checked (shows a check), sep, header, sub.
 * The menu opens at the mouse cursor, scales with the UI zoom (it lives in the window overlay, a child of the scaled root),
 * and is rebuilt on every show(), so specs may close over the current row.
 */
Menu {
    id: m
    property var _made: []          // [{obj, isMenu}]
    background: Rectangle { color: Theme.bg3; border.color: Theme.border1; radius: Theme.r5; implicitWidth: 220 }
    padding: 4

    Component { id: itemC; CMenuItem {} }
    Component { id: sepC; MenuSeparator { padding: 0; topPadding: 3; bottomPadding: 3; contentItem: Rectangle { implicitHeight: 1; color: Theme.border1 } } }
    Component { id: subC; Menu { padding: 4; background: Rectangle { color: Theme.bg3; border.color: Theme.border1; radius: Theme.r5; implicitWidth: 200 } } }
    TextEdit { id: clipper; visible: false }

    // ---- clipboard / properties helpers -------------------------------------------------------------------------
    function copyText(s, what) {
        s = String(s === undefined || s === null ? "" : s)
        clipper.text = s; clipper.selectAll(); clipper.copy(); clipper.deselect()
        if (typeof window !== "undefined" && window.toast) window.toast("Copied " + (what || (s.length > 48 ? s.substring(0, 48) + "…" : s)), "ok")
    }
    function copyJson(o, what) { copyText(JSON.stringify(o, null, 2), what || "JSON") }
    function pasteText() { clipper.text = ""; clipper.paste(); var t = clipper.text; clipper.text = ""; return t }
    function showProps(title, obj, actions) { propsLoader.open(title, obj, actions) }

    // ---- build + show ------------------------------------------------------------------------------------------------
    function clearAll() {
        for (var i = m._made.length - 1; i >= 0; --i) {
            var e = m._made[i]
            try { if (e.isMenu) m.removeMenu(e.obj); else m.removeItem(e.obj) } catch (ex) { }
            try { e.obj.destroy() } catch (ex2) { }
        }
        m._made = []
    }
    function fill(target, specs, top) {
        var made = []
        var list = (specs || []).filter(function(s) { return s })
        // collapse leading/duplicate/trailing separators
        var clean = []
        for (var i = 0; i < list.length; ++i) { if (list[i].sep) { if (clean.length && !clean[clean.length - 1].sep) clean.push(list[i]) } else clean.push(list[i]) }
        while (clean.length && clean[clean.length - 1].sep) clean.pop()
        for (var k = 0; k < clean.length; ++k) {
            var s = clean[k], obj
            if (s.sep) { obj = sepC.createObject(null); target.addItem(obj); made.push({ obj: obj, isMenu: false }); continue }
            if (s.sub) {
                obj = subC.createObject(null, { title: s.text || "" })
                fill(obj, s.sub, false)
                target.addMenu(obj); made.push({ obj: obj, isMenu: true })
                continue
            }
            obj = itemC.createObject(null, { text: s.header !== undefined && typeof s.header === "string" ? s.header : (s.text || ""), header: s.header !== undefined, hint: s.hint || "", danger: !!s.danger, enabled2: s.enabled !== false, checkable: s.checked !== undefined, checked: !!s.checked })
            if (s.fn) obj.triggered.connect(s.fn)
            target.addItem(obj); made.push({ obj: obj, isMenu: false })
        }
        if (top) m._made = made
        else return made
    }
    function show(specs) {
        clearAll()
        fill(m, specs, true)
        if (m.count > 0) m.popup()
    }

    // ---- Properties dialog (created on first use, lives in the window overlay) -------------------------------------------
    Loader { id: propsLoader; active: false
        function open(title, obj, actions) { active = true; item.show(title, obj, actions) }
        sourceComponent: PropsDialog { menuHelper: m }
    }
}
