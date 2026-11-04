// test_web_uibuttons: the web dashboard's pure button helpers, web/ui-buttons-core.js (docs/plans/ux-buttons.md 3.9), run
// as an ES module in QJSEngine (importModule resolves its `import * as Spec from "./ui-spec.js"`), so no Node is needed.
//
// - HTML output of btn / iconBtn / chip / padBtn: classes for size, role, fill and state, the stable DOM id, the safety
//   attributes, aria-label and title, the icon <use>.
// - Escaping: esc() on text and attributes (< > & " '), jsq() for a value inside an inline handler, including the
//   a');evil();// case from app.js:18-19, which must come out as a JSON string literal entity-escaped once.
// - glyphToIcon: whole-glyph, glyph + space, the two-character glyphs, and text that is not a glyph.
// - Hotkeys: normHotkey canonical form, the web allow-list (single keys allowed), reserved keys per surface page, conflicts
//   between bindings on one page and with the column visible on every page.
// - resolvePrefs / cleanButton / validatePrefs: overrides applied where valid for the surface, the safety floor, invalid
//   fields dropped with a note, order / hidden cleaned.
// - ui-buttons.js and ui-buttons.css exist, are referenced from index.html in the right order, and the runtime module
//   refers only to icons that are drawn.
#include <QtTest>
#include <QFile>
#include <QJSEngine>
#include <QJSValue>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#ifndef PLAYPLAY_SOURCE_DIR
#error "PLAYPLAY_SOURCE_DIR must name the source tree"
#endif

static const QString kSrc = QStringLiteral(PLAYPLAY_SOURCE_DIR);

class TestWebUiButtons : public QObject {
    Q_OBJECT
    QJSEngine js;
    QJSValue core;
    // Calls an exported function with JSON-literal arguments and returns the result; errors fail the caller.
    QJSValue callJs(const QString &expr) {
        js.globalObject().setProperty("core", core);
        const QJSValue v = js.evaluate(expr);
        if (v.isError()) qWarning() << "JS error in" << expr << ":" << v.toString();
        return v;
    }
    QString str(const QString &expr) { return callJs(expr).toString(); }
    QJsonValue json(const QString &expr) {
        const QJSValue v = callJs("JSON.stringify([" + expr + "])");
        return QJsonDocument::fromJson(v.toString().toUtf8()).array().at(0);
    }
    static QString readAll(const QString &rel) { QFile f(kSrc + "/" + rel); return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString(); }

private slots:
    void initTestCase() {
        core = js.importModule(kSrc + "/web/ui-buttons-core.js");
        QVERIFY2(!core.isError(), qPrintable(core.toString()));
        QVERIFY(core.property("btn").isCallable());
        QVERIFY(core.property("Spec").property("icons").property("play").isObject());
    }

    void escaping() {
        QCOMPARE(str("core.esc('<a href=\"x\">&\\'')"), QStringLiteral("&lt;a href=&quot;x&quot;&gt;&amp;&#39;"));
        QCOMPARE(str("core.esc(null) + core.esc(undefined) + core.esc(0)"), QStringLiteral("0"));
        // jsq: a JSON literal, then entity-escaped, so the attribute decodes back to a quoted JS string and nothing escapes it
        QCOMPARE(str("core.jsq(\"a');evil();//\")"), QStringLiteral("&quot;a&#39;);evil();//&quot;"));
        const QString html = str("core.btn({label:'Stop', action:'pp.stop(' + core.jsq(\"a');evil();//\") + ')'})");
        QVERIFY2(html.contains("onclick=\"pp.stop(&quot;a&#39;);evil();//&quot;)\""), qPrintable(html));
        QVERIFY(!html.contains("onclick=\"pp.stop(\"a"));
        // text and attributes
        const QString h2 = str("core.btn({label:'<b>&', tip:'say \"hi\"', badge:'<i>'})");
        QVERIFY2(h2.contains(">&lt;b&gt;&amp;</span>"), qPrintable(h2));
        QVERIFY2(h2.contains("title=\"say &quot;hi&quot;\""), qPrintable(h2));
        QVERIFY2(h2.contains("<span class=\"pb-badge\">&lt;i&gt;</span>"), qPrintable(h2));
        QVERIFY(!h2.contains("<b>") && !h2.contains("<i>"));
    }

    void btnHtml() {
        const QString h = str("core.btn({id:'ch1/transport.stop', label:'STOP', icon:'stop', role:'stop', size:'l', tip:'Stop and go to slate', hotkey:'ctrl+shift+s', safety:'arm', action:'pp.stop(' + core.jsq('ch1') + ')', led:'red', ledBlink:'slow', sublabel:'main'})");
        QVERIFY2(h.startsWith("<button type=\"button\""), qPrintable(h));
        QVERIFY2(h.contains("class=\"pb pb-l r-stop f-solid\""), qPrintable(h));
        QVERIFY2(h.contains("id=\"b-ch1-transport.stop\""), qPrintable(h));
        QVERIFY2(h.contains("data-pb=\"ch1/transport.stop\""), qPrintable(h));
        QVERIFY2(h.contains("data-safety=\"arm\""), qPrintable(h));
        QVERIFY2(h.contains("data-arm-label=\"CONFIRM\""), qPrintable(h));
        QVERIFY2(h.contains("data-action=\"pp.stop(&quot;ch1&quot;)\""), qPrintable(h));
        QVERIFY2(h.contains("onclick=\"ppui.fire(this,event)\""), qPrintable(h));
        QVERIFY2(h.contains("title=\"Stop and go to slate (Ctrl+Shift+S)\""), qPrintable(h));
        QVERIFY2(h.contains("aria-label=\"Stop and go to slate\""), qPrintable(h));
        QVERIFY2(h.contains("<svg class=\"ic\" aria-hidden=\"true\" focusable=\"false\"><use href=\"#i-stop\"/></svg>"), qPrintable(h));
        QVERIFY2(h.contains("<span class=\"led led-red blink-slow\""), qPrintable(h));
        QVERIFY2(h.contains("<span class=\"pb-label\">STOP</span><span class=\"pb-sub\">main</span>"), qPrintable(h));
        QVERIFY(h.endsWith("</button>"));
        // no id and no safety: today's inline onclick, no ppui involved
        const QString plain = str("core.btn({label:'Save', role:'primary', action:'pp.save()'})");
        QVERIFY2(plain.contains("onclick=\"pp.save()\"") && !plain.contains("data-") && !plain.contains("id="), qPrintable(plain));
        QVERIFY2(plain.contains("class=\"pb pb-m r-primary f-solid\""), qPrintable(plain));
        // auto fill: neutral -> outline, active -> tonal, ghost stays ghost; unknown role / size fall back
        QVERIFY(str("core.btn({label:'x'})").contains("r-neutral f-outline"));
        QVERIFY(str("core.btn({label:'x', active:true})").contains("f-tonal") && str("core.btn({label:'x', active:true})").contains("is-active"));
        QVERIFY(str("core.btn({label:'x', fill:'ghost', role:'go'})").contains("r-go f-ghost"));
        QVERIFY(str("core.btn({label:'x', role:'bogus', size:'huge'})").contains("pb pb-m r-neutral"));
        // disabled / busy / colour / hold / modal
        QVERIFY(str("core.btn({label:'x', disabled:true})").contains(" disabled"));
        const QString busy = str("core.btn({id:'a', label:'x', busy:true})");
        QVERIFY2(busy.contains("is-busy") && busy.contains("aria-busy=\"true\"") && busy.contains(" disabled"), qPrintable(busy));
        QVERIFY(str("core.btn({label:'x', color:'#FF2D55'})").contains("style=\"--pb-tint:#ff2d55\""));
        QVERIFY(!str("core.btn({label:'x', color:'red'})").contains("--pb-tint"));
        const QString hold = str("core.btn({id:'m/stop', label:'STOP', safety:'hold', action:'pp.muxStop()'})");
        QVERIFY2(hold.contains("data-safety=\"hold\"") && hold.contains("onclick=\"return false\"") && hold.contains("data-action=\"pp.muxStop()\""), qPrintable(hold));
        const QString modal = str("core.btn({id:'c/del', label:'Delete', role:'destructive', safety:'modal', confirm:'Delete it?', action:'pp.del()'})");
        QVERIFY2(modal.contains("data-safety=\"modal\"") && modal.contains("data-confirm=\"Delete it?\"") && modal.contains("ppui.fire"), qPrintable(modal));
        // an unknown safety level counts as none
        QVERIFY(str("core.btn({id:'q', label:'x', safety:'nuke', action:'f()'})").contains("data-safety=\"none\""));
    }

    void iconBtnAndChip() {
        const QString h = str("core.iconBtn({icon:'close', action:'pp.x()'})");
        QVERIFY2(h.contains("class=\"pb pb-s r-neutral f-ghost pb-icon\""), qPrintable(h));
        QVERIFY2(h.contains("aria-label=\"Remove\"") && h.contains("title=\"Remove\""), qPrintable(h));   // the glyph map's verb for close
        QVERIFY2(!h.contains("pb-label"), qPrintable(h));
        const QString t = str("core.iconBtn({icon:'refresh', tip:'Reload the list', size:'m'})");
        QVERIFY2(t.contains("pb-m") && t.contains("aria-label=\"Reload the list\""), qPrintable(t));
        QVERIFY(str("core.iconBtn({icon:'wrench'})").contains("aria-label=\"wrench\""));              // no glyph verb: the name, never empty
        const QString c = str("core.chip({label:'Mix', active:true, action:'pp.t()'})");
        QVERIFY2(c.contains("pb-xs") && c.contains("f-tonal") && c.contains("aria-pressed=\"true\""), qPrintable(c));
        const QString c2 = str("core.chip({label:'Dip'})");
        QVERIFY2(c2.contains("f-outline") && c2.contains("aria-pressed=\"false\""), qPrintable(c2));
        // icon(): sprite use, size option, unknown name -> nothing
        QCOMPARE(str("core.icon('play')"), QStringLiteral("<svg class=\"ic\" aria-hidden=\"true\" focusable=\"false\"><use href=\"#i-play\"/></svg>"));
        QVERIFY(str("core.icon('play', {size:20, cls:'big'})").contains("class=\"ic big\" style=\"width:20px;height:20px\""));
        QCOMPARE(str("core.icon('no-such-icon')"), QString());
        QCOMPARE(str("core.icon('')"), QString());
    }

    void padBtn() {
        const QString h = str("core.padBtn({id:'k1', label:'GO LIVE', role:'go', hotkey:'f5', confirm:true, channel:'main', size:'l', span:2}, {go:'#30d158', stop:'#ff2d55'}, {node:'Studio', action:'pp.padClick(\"k1\")'})");
        QVERIFY2(h.contains("pb pb-l r-go f-solid pb-span2 pad-key"), qPrintable(h));
        QVERIFY2(h.contains("id=\"b-remote.pad-k1\"") && h.contains("data-pb=\"remote.pad/k1\""), qPrintable(h));
        QVERIFY2(h.contains("--pb-tint:#30d158"), qPrintable(h));
        QVERIFY2(h.contains("data-safety=\"arm\""), qPrintable(h));
        QVERIFY2(h.contains("<span class=\"pb-sub\">Studio · main · F5</span>"), qPrintable(h));
        // own colour beats the theme; an unknown role is neutral; the default size is xl; no confirm -> none
        const QString o = str("core.padBtn({id:'k2', label:'X', role:'weird', color:'#123456'}, {neutral:'#888888'}, {})");
        QVERIFY2(o.contains("r-neutral") && o.contains("--pb-tint:#123456") && o.contains("pb-xl") && o.contains("data-safety=\"none\""), qPrintable(o));
    }

    void glyphs() {
        QCOMPARE(json("core.glyphToIcon('✕')").toObject().value("icon").toString(), QStringLiteral("close"));
        QCOMPARE(json("core.glyphToIcon('✕')").toObject().value("rest").toString(), QString());
        QCOMPARE(json("core.glyphToIcon('✕')").toObject().value("tip").toString(), QStringLiteral("Remove"));
        const QJsonObject add = json("core.glyphToIcon('+ Add node')").toObject();
        QCOMPARE(add.value("icon").toString(), QStringLiteral("plus"));
        QCOMPARE(add.value("rest").toString(), QStringLiteral("Add node"));
        QCOMPARE(json("core.glyphToIcon('◀❘')").toObject().value("icon").toString(), QStringLiteral("frame-back"));
        QCOMPARE(json("core.glyphToIcon('❘▶')").toObject().value("icon").toString(), QStringLiteral("frame-fwd"));
        QCOMPARE(json("core.glyphToIcon('  ↻  ')").toObject().value("icon").toString(), QStringLiteral("refresh"));
        QVERIFY(json("core.glyphToIcon('Save')").isNull());
        QVERIFY(json("core.glyphToIcon('A+')").isNull());
        QVERIFY(json("core.glyphToIcon('+1')").isNull());
        QVERIFY(json("core.glyphToIcon('')").isNull());
        // every glyph maps to a drawn icon
        QCOMPARE(json("Object.values(core.Spec.glyphs).filter(g => !core.Spec.icons[g.icon]).length").toInt(), 0);
    }

    void hotkeys() {
        QCOMPARE(str("core.normHotkey(' ctrl+shift+s ')"), QStringLiteral("Ctrl+Shift+S"));
        QCOMPARE(str("core.normHotkey('Shift+Control+a')"), QStringLiteral("Ctrl+Shift+A"));
        QCOMPARE(str("core.normHotkey('Esc')"), QStringLiteral("Escape"));
        QCOMPARE(str("core.normHotkey('Ctrl++')"), QStringLiteral("Ctrl++"));
        QCOMPARE(str("core.normHotkey('f10')"), QStringLiteral("F10"));
        QCOMPARE(str("core.normHotkey('Ctrl+')"), QString());
        QCOMPARE(str("core.normHotkey('A+B')"), QString());
        QCOMPARE(str("core.normHotkey('')"), QString());
        QCOMPARE(str("core.normHotkey(null)"), QString());
        // the web allow-list: single keys and F9-F12 allowed, F1-F8 are pages (reserved globally), Space reserved
        QVERIFY(json("core.hotkeyOk('ctrl+shift+s', 'onair.deck')").toBool());
        QVERIFY(json("core.hotkeyOk('K', 'remote.pad')").toBool());
        QVERIFY(json("core.hotkeyOk('F9', 'remote.pad')").toBool());
        QVERIFY(json("core.hotkeyOk('', 'remote.pad')").toBool());                                   // no binding is fine
        QVERIFY(!json("core.hotkeyOk('F1', 'remote.pad')").toBool());
        QVERIFY(!json("core.hotkeyOk('Space', 'remote.pad')").toBool());
        QVERIFY(!json("core.hotkeyOk('Ctrl+', 'remote.pad')").toBool());
        QVERIFY(!json("core.hotkeyOk('Ctrl+Alt+Delete', 'remote.pad')").toBool());
        // page reservations: 1-8 and Escape only on the On Air page; the column ("" page) sees every page's list
        QVERIFY(!json("core.hotkeyOk('1', 'onair.bus')").toBool());
        QVERIFY(json("core.hotkeyOk('1', 'remote.pad')").toBool());
        QVERIFY(!json("core.hotkeyOk('1', 'column.transport')").toBool());
        QCOMPARE(json("core.hotkeyCheck('Ctrl+C', 'onair.deck').reason").toString(), QStringLiteral("reserved by the application"));
        QVERIFY(json("core.hotkeyCheck('F2', 'onair.deck').reason").toString().startsWith("not allowed"));
        QCOMPARE(json("core.hotkeyCheck('A+B', 'onair.deck').reason").toString(), QStringLiteral("not a key chord"));
        // conflicts: same page clashes, another page does not, the column clashes with everything, the key itself is skipped
        const QString b = "{'onair.deck/transport.stop': {hotkey: 'ctrl+shift+s'}, 'remote.pad/k1': {hotkey: 'F9'}, 'column.transport/transport.next': {hotkey: 'Ctrl+N'}}";
        QVERIFY(!json("core.hotkeyOk('Ctrl+Shift+S', 'onair.bus', '1', " + b + ")").toBool());
        QCOMPARE(json("core.hotkeyCheck('Ctrl+Shift+S', 'onair.bus', '1', " + b + ").conflict.id").toString(), QStringLiteral("transport.stop"));
        QVERIFY(json("core.hotkeyOk('Ctrl+Shift+S', 'onair.deck', 'transport.stop', " + b + ")").toBool());
        QVERIFY(json("core.hotkeyOk('Ctrl+Shift+S', 'remote.pad', 'k2', " + b + ")").toBool());
        QVERIFY(!json("core.hotkeyOk('F9', 'remote.pad', 'k2', " + b + ")").toBool());
        QVERIFY(!json("core.hotkeyOk('Ctrl+N', 'remote.pad', 'k2', " + b + ")").toBool());
        QVERIFY(!json("core.hotkeyOk('F9', 'column.transport', 'x', " + b + ")").toBool());
        // eventHotkey: a KeyboardEvent-like object
        QCOMPARE(str("core.eventHotkey({key:'!', code:'Digit1', shiftKey:true})"), QStringLiteral("Shift+1"));
        QCOMPARE(str("core.eventHotkey({key:'s', code:'KeyS', ctrlKey:true, shiftKey:true})"), QStringLiteral("Ctrl+Shift+S"));
        QCOMPARE(str("core.eventHotkey({key:'Control', ctrlKey:true})"), QString());
        QCOMPARE(str("core.eventHotkey({key:'Enter', location:3})"), QStringLiteral("Num+Enter"));
        QCOMPARE(str("core.eventHotkey({key:' '})"), QStringLiteral("Space"));
    }

    void resolvePrefs() {
        const QString d = "{label:'STOP', icon:'stop', role:'stop', safety:'none', floor:'arm', tip:'Stop'}";
        // no overrides: defaults, with the safety raised to the floor
        QJsonObject r = json("core.resolvePrefs(" + d + ", null, 'onair.deck')").toObject();
        QCOMPARE(r.value("safety").toString(), QStringLiteral("arm"));
        QCOMPARE(r.value("size").toString(), QStringLiteral("l"));
        QCOMPARE(r.value("label").toString(), QStringLiteral("STOP"));
        QCOMPARE(r.value("custom").toArray().size(), 0);
        // valid overrides apply and are listed; safety below the floor is clamped and not counted as custom
        r = json("core.resolvePrefs(" + d + ", {label:'HALT', icon:'power', color:'#FF9500', size:'xl', hotkey:'ctrl+h', safety:'none'}, 'onair.deck')").toObject();
        QCOMPARE(r.value("label").toString(), QStringLiteral("HALT"));
        QCOMPARE(r.value("icon").toString(), QStringLiteral("power"));
        QCOMPARE(r.value("color").toString(), QStringLiteral("#ff9500"));
        QCOMPARE(r.value("size").toString(), QStringLiteral("xl"));
        QCOMPARE(r.value("hotkey").toString(), QStringLiteral("Ctrl+H"));
        QCOMPARE(r.value("safety").toString(), QStringLiteral("arm"));
        QCOMPARE(r.value("tip").toString(), QStringLiteral("Stop"));
        QStringList custom; for (const QJsonValue &v : r.value("custom").toArray()) custom << v.toString();
        QCOMPARE(custom, QStringList({ "label", "icon", "color", "size", "hotkey" }));
        // raising is allowed
        QCOMPARE(json("core.resolvePrefs(" + d + ", {safety:'modal'}, 'onair.deck').safety").toString(), QStringLiteral("modal"));
        QVERIFY(json("core.resolvePrefs(" + d + ", {safety:'modal'}, 'onair.deck').custom").toArray().contains(QJsonValue("safety")));
        // invalid overrides are ignored: unknown icon, bad colour, a size the surface does not offer, a reserved / bare hotkey, a 29-char label
        r = json("core.resolvePrefs(" + d + ", {label:'" + QString(29, 'x') + "', icon:'nope', color:'red', size:'xs', hotkey:'F1'}, 'onair.deck')").toObject();
        QCOMPARE(r.value("label").toString(), QStringLiteral("STOP"));
        QCOMPARE(r.value("icon").toString(), QStringLiteral("stop"));
        QCOMPARE(r.value("color").toString(), QString());
        QCOMPARE(r.value("size").toString(), QStringLiteral("l"));
        QCOMPARE(r.value("hotkey").toString(), QString());
        QCOMPARE(r.value("custom").toArray().size(), 0);
        // no surface: any size goes, default m-first order
        QCOMPARE(json("core.resolvePrefs({}, {size:'xs'}).size").toString(), QStringLiteral("xs"));
        QCOMPARE(json("core.resolvePrefs({}, null).size").toString(), QStringLiteral("xs"));
        // actionDefaults from the spec, with the stateful "off" variant
        QCOMPARE(json("core.actionDefaults('transport.stop').safety").toString(), QStringLiteral("modal"));
        QCOMPARE(json("core.actionDefaults('transport.stop').floor").toString(), QStringLiteral("arm"));
        QCOMPARE(json("core.actionDefaults('automation.arm').label").toString(), QStringLiteral("ARMED"));
        QCOMPARE(json("core.actionDefaults('automation.arm', 'off').label").toString(), QStringLiteral("DISARMED"));
        QCOMPARE(json("core.actionDefaults('automation.arm', 'off').icon").toString(), QStringLiteral("bolt-off"));
        QCOMPARE(json("core.actionDefaults('bus.take').safety").toString(), QStringLiteral("none"));
        QVERIFY(json("core.actionDefaults('no.such')").isNull());
    }

    void cleanAndValidate() {
        QJsonObject c = json("core.cleanButton('onair.deck', 'transport.stop', {label:' HALT ', icon:'nope', color:'#ABCDEF', size:'s', hotkey:'ctrl+h', safety:'arm', extra:1, bogus:null})").toObject();
        QJsonObject clean = c.value("clean").toObject();
        QCOMPARE(clean.value("label").toString(), QStringLiteral("HALT"));
        QCOMPARE(clean.value("color").toString(), QStringLiteral("#abcdef"));
        QCOMPARE(clean.value("hotkey").toString(), QStringLiteral("Ctrl+H"));
        QCOMPARE(clean.value("safety").toString(), QStringLiteral("arm"));
        QVERIFY(!clean.contains("icon") && !clean.contains("size") && !clean.contains("extra"));
        QCOMPARE(c.value("errors").toArray().size(), 2);
        QVERIFY(c.value("errors").toArray().at(0).toString().contains("unknown icon"));
        QVERIFY(c.value("errors").toArray().at(1).toString().contains("not offered"));
        QVERIFY(json("core.cleanButton('onair.deck', 'x', 'nope').errors[0]").toString().contains("not an object"));
        // the whole setting
        const QString obj = "{version:1, surfaces:{'onair.deck':{order:['a','|','b','a',3,''], hidden:['b','|']}, 'nope':{}}, buttons:{"
                            "'onair.deck/transport.stop':{hotkey:'Ctrl+Shift+S'}, 'onair.bus/1':{hotkey:'ctrl+shift+s', color:'#112233'}, 'bad':{}, 'zz/1':{} }}";
        QJsonObject v = json("core.validatePrefs(" + obj + ")").toObject();
        QVERIFY(v.value("ok").toBool());
        const QJsonObject cl = v.value("clean").toObject();
        QCOMPARE(cl.value("surfaces").toObject().value("onair.deck").toObject().value("order").toArray(), QJsonArray({ "a", "|", "b" }));
        QCOMPARE(cl.value("surfaces").toObject().value("onair.deck").toObject().value("hidden").toArray(), QJsonArray({ "b" }));
        QVERIFY(!cl.value("surfaces").toObject().contains("nope"));
        QCOMPARE(cl.value("buttons").toObject().value("onair.deck/transport.stop").toObject().value("hotkey").toString(), QStringLiteral("Ctrl+Shift+S"));
        const QJsonObject bus = cl.value("buttons").toObject().value("onair.bus/1").toObject();
        QCOMPARE(bus.value("color").toString(), QStringLiteral("#112233"));
        QVERIFY2(!bus.contains("hotkey"), "the second binding of Ctrl+Shift+S on the On Air page must be dropped");
        QVERIFY(!cl.value("buttons").toObject().contains("bad") && !cl.value("buttons").toObject().contains("zz/1"));
        QStringList errs; for (const QJsonValue &e : v.value("errors").toArray()) errs << e.toString();
        QVERIFY2(errs.size() == 5, qPrintable(errs.join(" | ")));   // bad order entries, unknown surface, taken hotkey, bad key, unknown surface in key
        QVERIFY(!json("core.validatePrefs('not json').ok").toBool());
        QVERIFY(!json("core.validatePrefs([1]).ok").toBool());
        QVERIFY(json("core.validatePrefs('{\"version\":1}').ok").toBool());
        QCOMPARE(json("core.validatePrefs({version:2}).errors[0]").toString(), QStringLiteral("version 2 (this is 1)"));
        // the surfaces table matches ButtonPrefs.qml
        const QString qml = readAll("Play_Play_Native/qml/PlayPlay/Buttons/ButtonPrefs.qml");
        QVERIFY(!qml.isEmpty());
        for (const QString &s : { "onair.deck", "onair.switch", "onair.bus", "column.transport", "graphics.cg", "graphics.quick", "remote.pad" }) {
            QVERIFY2(qml.contains("\"" + s + "\":"), qPrintable(s));
            QVERIFY2(json("core.surfaces['" + s + "']").isObject(), qPrintable(s));
        }
        QCOMPARE(json("Object.keys(core.surfaces).length").toInt(), 7);
    }

    void filesAndIndex() {
        const QString runtime = readAll("web/ui-buttons.js"), css = readAll("web/ui-buttons.css"), index = readAll("web/index.html");
        QVERIFY(!runtime.isEmpty() && !css.isEmpty() && !index.isEmpty());
        QVERIFY(runtime.contains("import * as Core from \"./ui-buttons-core.js\""));
        QVERIFY(runtime.contains("window.ppui ="));
        // index.html: tokens and button styles after style.css, the runtime module before app.js, the token button by icon name
        const int style = index.indexOf("href=\"style.css\""), tokens = index.indexOf("href=\"ui-tokens.css\""), bcss = index.indexOf("href=\"ui-buttons.css\"");
        const int rt = index.indexOf("src=\"ui-buttons.js\""), app = index.indexOf("src=\"app.js\"");
        QVERIFY(style >= 0 && tokens > style && bcss > tokens);
        QVERIFY(rt >= 0 && app > rt);
        QVERIFY(index.contains("type=\"module\" src=\"ui-buttons.js\""));
        QVERIFY(index.contains("id=\"tokenBtn\"") && index.contains("data-icon=\"key\"") && !index.contains("&#128273;"));
        // the CSS defines every class the builders and the runtime emit, and the legacy fixes
        for (const QString &cls : { ".pb", ".pb-xs", ".pb-s", ".pb-l", ".pb-xl", ".pb-icon", ".r-primary", ".r-go", ".r-stop", ".r-destructive", ".r-warn", ".r-info", ".r-accent", ".r-cued",
                                    ".f-solid", ".f-tonal", ".f-outline", ".f-ghost", ".is-active", ".is-armed", ".is-holding", ".is-busy", ".is-error", ".led-red", ".led-green", ".led-amber", ".led-blue",
                                    ".blink-slow", ".blink-fast", ".ic", "button:focus-visible", "button.primary:hover", "button.primary:active", ".ppui-editor" })
            QVERIFY2(css.contains(cls), qPrintable(cls));
        // literal icon names in the runtime are drawn (the generic scan in test_uispec covers the builders' callers)
        static const QRegularExpression lit(QStringLiteral("icon\\(\"([a-z0-9-]+)\"\\)"));
        for (auto it = lit.globalMatch(runtime); it.hasNext();) { const QString n = it.next().captured(1); QVERIFY2(core.property("Spec").property("icons").property(n).isObject(), qPrintable(n)); }
    }
};

QTEST_GUILESS_MAIN(TestWebUiButtons)
#include "test_web_uibuttons.moc"
