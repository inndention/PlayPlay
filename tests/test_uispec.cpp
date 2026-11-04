// test_uispec: the button system's single source of truth (assets/ui/buttons.json, assets/ui/icons.json) and the four files
// tools/uispec/gen_uispec.cmake generates from it (internal plan ux-buttons.md section 3.3).
//
// - The JSON: parses, no key twice in one object (QJsonDocument would silently keep the last), names, fields and every cross
//   reference (roles, theme colours, icons, safety levels and floors, hotkeys); theme colours equal Theme.qml and web/style.css.
// - Icons: every path string tokenises as SVG path syntax that Qt's PathSvg and browsers both read the same way, stays in the
//   24x24 grid and has no zero-length segment or empty subpath (Qt draws nothing there, Chromium a round-cap dot). The catalogue
//   (iconGroups) is completely drawn (UXB-2); each icon's ink (path bounds plus half the stroke) keeps a margin to the edge and
//   sits near the centre; no arc is tighter than half the stroke (a tighter one folds the stroke's inner edge over, and Chromium
//   then draws a ring where Qt draws solid ink); and the filled parts of one icon never overlap (browsers fill nonzero, Qt's
//   ShapePath odd-even by default; disjoint parts look the same under both).
// - Outputs: byte-identical (line ends aside) to a fresh `cmake -P` generation, and they load: spec.js as a QML JS library,
//   ui-spec.js as an ES module whose values equal the JSON, icons.svg as XML, ui-tokens.css as custom properties.
// - Sources: icon names used in QML (icon: / icon.name: / iconRight: / Icon { name: }) and web modules (icon("x"), data-icon, icon: "x") are
//   drawn, and every IconButton has a tip.
//
// The regular expressions are plain string literals on purpose: moc loses track of raw strings holding quotes or braces and
// then finds no Q_OBJECT class in this file.
#include <QtTest>
#include <QDirIterator>
#include <QJSEngine>
#include <QJSValueIterator>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>
#include <QXmlStreamReader>
#include <QtMath>
#include <algorithm>
#include <array>
#include <cmath>

#ifndef PLAYPLAY_SOURCE_DIR
#error "PLAYPLAY_SOURCE_DIR must name the source tree"
#endif
#ifndef PP_CMAKE_COMMAND
#error "PP_CMAKE_COMMAND must name the cmake executable"
#endif

namespace {

const QString kSrc = QStringLiteral(PLAYPLAY_SOURCE_DIR);
const QStringList kOutputs = {QStringLiteral("Play_Play_Native/qml/PlayPlay/Buttons/spec.js"), QStringLiteral("web/ui-spec.js"),
                              QStringLiteral("web/icons.svg"), QStringLiteral("web/ui-tokens.css")};

QByteArray readFile(const QString &path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}
QByteArray lf(QByteArray b) { return b.replace("\r\n", "\n"); }   // git autocrlf may check the outputs out with CRLF
QSet<QString> setOf(const QJsonArray &a) {
    QSet<QString> s;
    for (const QJsonValue &v : a) s.insert(v.toString());
    return s;
}
QSet<QString> keySet(const QJsonObject &o) {
    const QStringList k = o.keys();
    return QSet<QString>(k.begin(), k.end());
}

// Every key that appears twice in one object, as "/path/key".
QStringList duplicateKeys(const QByteArray &json) {
    struct Frame { bool obj; QSet<QString> keys; QString path; };
    QStringList dups;
    QList<Frame> st;
    QString lastKey;
    for (int i = 0; i < json.size(); ++i) {
        const char c = json[i];
        if (c == '"') {
            int j = i + 1;
            while (j < json.size() && json[j] != '"') j += json[j] == '\\' ? 2 : 1;
            int k = j + 1;
            while (k < json.size() && std::isspace(uchar(json[k]))) ++k;
            if (!st.isEmpty() && st.last().obj && k < json.size() && json[k] == ':') {
                const QString key = QJsonDocument::fromJson(QByteArray("[") + json.mid(i, j - i + 1) + "]").array().at(0).toString();
                if (st.last().keys.contains(key)) dups << st.last().path + "/" + key;
                st.last().keys.insert(key);
                lastKey = key;
            }
            i = j;
        } else if (c == '{' || c == '[') {
            const QString path = st.isEmpty() ? QString() : st.last().path + "/" + (st.last().obj ? lastKey : QStringLiteral("[]"));
            st.append({c == '{', {}, path});
        } else if ((c == '}' || c == ']') && !st.isEmpty()) {
            st.removeLast();
        }
    }
    return dups;
}

// One SVG path string, read the way Qt's PathSvg and browsers both read it but stricter: commands MmLlHhVvCcSsQqTtAaZz; plain
// decimal numbers (no exponents, and a number must not run into the next through a second '.', although a minus sign may start
// the next one); arc flags as separate 0 / 1 tokens (Qt's parser does not split "011"); the right count of numbers per command;
// a leading moveto; every end and control point inside the 0..24 grid; every segment gets at least 0.1 away from its start and
// every subpath draws at least one segment. The renderers disagree on degenerate pieces: an arc back to its own start point is
// "omitted" by SVG and by Qt's PathSvg, but Chromium turns it into a zero-length line and draws a round-cap dot (eye-off had two),
// and a lone moveto adds a point to Qt's bounding rect without ink. Returns an empty string when the path is fine.
QString checkPath(const QString &d) {
    struct Tok { bool cmd; QChar c; double v; QString text; };
    QList<Tok> toks;
    const int n = d.size();
    bool prevNum = false, sep = true, comma = false;
    for (int i = 0; i < n;) {
        const QChar ch = d[i];
        if (ch.isSpace()) { sep = true; ++i; continue; }
        if (ch == u',') {
            if (!prevNum || comma) return QStringLiteral("stray comma at %1").arg(i);
            comma = true; sep = true; ++i;
            continue;
        }
        if (QStringLiteral("MmLlHhVvCcSsQqTtAaZz").contains(ch)) {
            if (comma) return QStringLiteral("comma before command '%1' at %2").arg(ch).arg(i);
            toks.append({true, ch, 0, QString(ch)});
            prevNum = false; sep = true; ++i;
            continue;
        }
        if (ch.isDigit() || ch == u'.' || ch == u'-' || ch == u'+') {
            if (prevNum && !sep && ch != u'-' && ch != u'+')
                return QStringLiteral("number runs into the previous one at %1 (separate numbers with a space)").arg(i);
            int j = i;
            if (d[j] == u'-' || d[j] == u'+') ++j;
            int digits = 0;
            while (j < n && d[j].isDigit()) { ++j; ++digits; }
            if (j < n && d[j] == u'.') { ++j; while (j < n && d[j].isDigit()) { ++j; ++digits; } }
            if (!digits) return QStringLiteral("bad number at %1").arg(i);
            if (j < n && (d[j] == u'e' || d[j] == u'E')) return QStringLiteral("exponent at %1").arg(j);
            const QString text = d.mid(i, j - i);
            toks.append({false, QChar(), text.toDouble(), text});
            prevNum = true; sep = false; comma = false; i = j;
            continue;
        }
        return QStringLiteral("unexpected '%1' at %2").arg(ch).arg(i);
    }
    if (comma) return QStringLiteral("trailing comma");
    if (toks.isEmpty() || !toks.first().cmd || toks.first().c.toUpper() != u'M') return QStringLiteral("must start with M or m");
    double cx = 0, cy = 0, sx = 0, sy = 0;
    int segs = -1;   // segments drawn in the current subpath; -1 before the first moveto
    const auto outside = [](const QPointF &p) { return p.x() < -1e-9 || p.y() < -1e-9 || p.x() > 24 + 1e-9 || p.y() > 24 + 1e-9; };
    const auto empty = [&] { return QStringLiteral("the subpath at (%1, %2) draws nothing").arg(sx).arg(sy); };
    for (int t = 0; t < toks.size();) {
        const QChar c = toks[t].c, C = c.toUpper();
        const bool rel = c.isLower();
        ++t;
        QList<Tok> a;
        while (t < toks.size() && !toks[t].cmd) a.append(toks[t++]);
        const int arity = C == u'Z' ? 0 : (C == u'H' || C == u'V') ? 1 : (C == u'M' || C == u'L' || C == u'T') ? 2
                        : (C == u'S' || C == u'Q') ? 4 : C == u'C' ? 6 : 7;
        if (!arity) {
            if (!a.isEmpty()) return QStringLiteral("'%1' takes no numbers").arg(c);
            if (segs == 0) return empty();
            cx = sx; cy = sy;
            continue;
        }
        if (a.isEmpty() || a.size() % arity) return QStringLiteral("'%1' needs a multiple of %2 numbers, got %3").arg(c).arg(arity).arg(a.size());
        for (int k = 0; k < a.size(); k += arity) {
            const auto v = [&](int m) { return a[k + m].v; };
            const auto X = [&](double x) { return rel ? cx + x : x; };
            const auto Y = [&](double y) { return rel ? cy + y : y; };
            QList<QPointF> pts;
            double nx = cx, ny = cy;
            switch (C.unicode()) {
            case 'M': case 'L': case 'T': nx = X(v(0)); ny = Y(v(1)); break;
            case 'H': nx = X(v(0)); break;
            case 'V': ny = Y(v(0)); break;
            case 'C': pts << QPointF(X(v(0)), Y(v(1))) << QPointF(X(v(2)), Y(v(3))); nx = X(v(4)); ny = Y(v(5)); break;
            case 'S': case 'Q': pts << QPointF(X(v(0)), Y(v(1))); nx = X(v(2)); ny = Y(v(3)); break;
            case 'A':
                if (v(0) <= 0 || v(1) <= 0) return QStringLiteral("arc radius must be positive");
                for (int m : {3, 4})
                    if (a[k + m].text != u"0" && a[k + m].text != u"1") return QStringLiteral("arc flag '%1' must be 0 or 1").arg(a[k + m].text);
                nx = X(v(5)); ny = Y(v(6));
                break;
            }
            pts << QPointF(nx, ny);
            for (const QPointF &p : pts)
                if (outside(p)) return QStringLiteral("point (%1, %2) is outside the 24x24 grid").arg(p.x()).arg(p.y());
            if (C == u'M' && k == 0) {   // a moveto (further pairs are linetos)
                if (segs == 0) return empty();
                segs = 0; sx = nx; sy = ny;
            } else {
                double reach = 0;   // how far the segment gets from its start; an arc counts by its end point only (F.6.2)
                for (const QPointF &p : pts) reach = std::max(reach, std::hypot(p.x() - cx, p.y() - cy));
                if (reach < 0.1)
                    return QStringLiteral("'%1' to (%2, %3) has no length (Qt draws nothing there, Chromium a round-cap dot)").arg(c).arg(nx).arg(ny);
                ++segs;
            }
            cx = nx; cy = ny;
        }
    }
    if (segs == 0) return empty();
    return {};
}

// Points along one SVG path whose syntax checkPath accepted: line end points, 32 samples per curve and 48 per arc, one list per
// subpath. Bounds from these are within 0.01 of the exact curve bounds at icon sizes (the extremes of an arc are sampled to
// r * (1 - cos(sweep / 96))). Arcs follow SVG F.6.2: identical end points omit the arc, a zero radius makes it a line.
// minArcR (optional) receives the smallest radius any arc is drawn with, after a too-small radius is scaled up.
QList<QList<QPointF>> samplePath(const QString &d, double *minArcR = nullptr) {
    static const QRegularExpression tokRe(QStringLiteral("[MmLlHhVvCcSsQqTtAaZz]|[-+]?(?:[0-9]+[.]?[0-9]*|[.][0-9]+)"));
    QStringList t;
    for (auto it = tokRe.globalMatch(d); it.hasNext();) t << it.next().captured(0);
    QList<QList<QPointF>> subs;
    QPointF cur, start, ctrl;   // ctrl: the last control point, which S / T reflect
    QChar prev;
    int i = 0;
    const auto isCmd = [&](int k) { return t[k].size() == 1 && t[k][0].isLetter(); };
    const auto add = [&](const QPointF &p) { if (subs.isEmpty()) subs.append({cur}); subs.last() << p; };
    while (i < t.size()) {
        const QChar c = t[i++][0], C = c.toUpper();
        const bool rel = c.isLower();
        if (C == u'Z') { add(start); cur = ctrl = start; prev = C; continue; }
        bool first = true;
        while (i < t.size() && !isCmd(i)) {
            const auto num = [&]() { return t[i++].toDouble(); };
            const auto pt = [&]() { const double x = num(), y = num(); return rel ? cur + QPointF(x, y) : QPointF(x, y); };
            const QChar K = (C == u'M' && !first) ? QChar(u'L') : C;   // extra pairs after a moveto are linetos
            QPointF c1, c2, end;
            switch (K.unicode()) {
            case 'M': cur = start = ctrl = pt(); subs.append({cur}); break;
            case 'L': cur = ctrl = pt(); add(cur); break;
            case 'H': { const double x = num(); cur.setX(rel ? cur.x() + x : x); ctrl = cur; add(cur); break; }
            case 'V': { const double y = num(); cur.setY(rel ? cur.y() + y : y); ctrl = cur; add(cur); break; }
            case 'C': case 'S':
                c1 = K == u'C' ? pt() : ((prev == u'C' || prev == u'S') ? 2 * cur - ctrl : cur);
                c2 = pt(); end = pt();
                for (int k = 1; k <= 32; ++k) {
                    const double s = k / 32.0, u = 1 - s;
                    add(u * u * u * cur + 3 * u * u * s * c1 + 3 * u * s * s * c2 + s * s * s * end);
                }
                cur = end; ctrl = c2;
                break;
            case 'Q': case 'T':
                c1 = K == u'Q' ? pt() : ((prev == u'Q' || prev == u'T') ? 2 * cur - ctrl : cur);
                end = pt();
                for (int k = 1; k <= 32; ++k) { const double s = k / 32.0, u = 1 - s; add(u * u * cur + 2 * u * s * c1 + s * s * end); }
                cur = end; ctrl = c1;
                break;
            case 'A': {   // SVG 1.1 F.6.5: end points to centre parameterisation
                double rx = std::abs(num()), ry = std::abs(num());
                const double phi = qDegreesToRadians(num()), fa = num(), fs = num();
                end = pt();
                if (end.x() == cur.x() && end.y() == cur.y()) break;    // omitted entirely
                if (rx == 0 || ry == 0) { cur = ctrl = end; add(cur); break; }
                const double cp = std::cos(phi), sp = std::sin(phi), hx = (cur.x() - end.x()) / 2, hy = (cur.y() - end.y()) / 2;
                const double x1 = cp * hx + sp * hy, y1 = -sp * hx + cp * hy;
                const double lam = x1 * x1 / (rx * rx) + y1 * y1 / (ry * ry);
                if (lam > 1) { rx *= std::sqrt(lam); ry *= std::sqrt(lam); }
                if (minArcR) *minArcR = std::min({*minArcR, rx, ry});
                const double num2 = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1, den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
                const double k = (fa == fs ? -1 : 1) * std::sqrt(std::max(0.0, den > 0 ? num2 / den : 0.0));
                const double cxp = k * rx * y1 / ry, cyp = -k * ry * x1 / rx;
                const double cx = cp * cxp - sp * cyp + (cur.x() + end.x()) / 2, cy = sp * cxp + cp * cyp + (cur.y() + end.y()) / 2;
                const double th = std::atan2((y1 - cyp) / ry, (x1 - cxp) / rx);
                double dth = std::atan2((-y1 - cyp) / ry, (-x1 - cxp) / rx) - th;
                if (fs != 0 && dth < 0) dth += 2 * M_PI;
                if (fs == 0 && dth > 0) dth -= 2 * M_PI;
                for (int s = 1; s <= 48; ++s) {
                    const double a = th + dth * s / 48;
                    add(QPointF(cx + rx * std::cos(a) * cp - ry * std::sin(a) * sp, cy + rx * std::cos(a) * sp + ry * std::sin(a) * cp));
                }
                cur = ctrl = end;
                break;
            }
            }
            prev = K;
            first = false;
        }
    }
    return subs;
}

// Bounds of sampled subpaths as {x0, y0, x1, y1}; the rectangle may be a line or a point.
std::array<double, 4> boundsOf(const QList<QList<QPointF>> &subs) {
    std::array<double, 4> b = {1e9, 1e9, -1e9, -1e9};
    for (const auto &s : subs)
        for (const QPointF &p : s) { b[0] = std::min(b[0], p.x()); b[1] = std::min(b[1], p.y()); b[2] = std::max(b[2], p.x()); b[3] = std::max(b[3], p.y()); }
    return b;
}

int lineOf(const QString &text, qsizetype pos) { return int(QStringView(text).left(pos).count(u'\n')) + 1; }

// Problems in one QML source: icon names icons.json does not draw (icon: / icon.name: / iconRight: / Icon { name: }), and
// IconButtons without a tip. icon.name is PButton's icon: AbstractButton's `icon` group is FINAL, so the name rides in it.
QStringList scanQml(const QString &file, const QString &text, const QSet<QString> &drawn) {
    static const QRegularExpression iconProp(QStringLiteral("\\b(?:icon(?:\\.name)?|iconRight)\\s*:\\s*\"([a-z][a-z0-9-]*)\""));
    static const QRegularExpression iconItem(QStringLiteral("\\bIcon\\s*\\{[^{}]*?\\bname\\s*:\\s*\"([a-z][a-z0-9-]*)\""));
    static const QRegularExpression iconButton(QStringLiteral("\\bIconButton\\s*\\{"));
    static const QRegularExpression tip(QStringLiteral("\\btip\\s*:"));
    QStringList out;
    for (const QRegularExpression *re : {&iconProp, &iconItem}) {
        for (auto it = re->globalMatch(text); it.hasNext();) {
            const auto m = it.next();
            if (!drawn.contains(m.captured(1)))
                out << QStringLiteral("%1:%2: icon \"%3\" is not drawn in assets/ui/icons.json").arg(file).arg(lineOf(text, m.capturedStart(1))).arg(m.captured(1));
        }
    }
    for (auto it = iconButton.globalMatch(text); it.hasNext();) {
        const auto m = it.next();
        qsizetype end = text.size() - 1;
        int depth = 0;
        for (qsizetype i = m.capturedEnd() - 1; i < text.size(); ++i) {   // to the matching brace, skipping strings and comments
            const QChar c = text[i];
            if (c == u'"' || c == u'\'') {
                for (++i; i < text.size() && text[i] != c; ++i)
                    if (text[i] == u'\\') ++i;
            } else if (c == u'/' && i + 1 < text.size() && text[i + 1] == u'/') {
                while (i < text.size() && text[i] != u'\n') ++i;
            } else if (c == u'{') {
                ++depth;
            } else if (c == u'}' && --depth == 0) {
                end = i;
                break;
            }
        }
        if (!tip.match(text.mid(m.capturedStart(), end + 1 - m.capturedStart())).hasMatch())
            out << QStringLiteral("%1:%2: IconButton without tip").arg(file).arg(lineOf(text, m.capturedStart()));
    }
    return out;
}

// Problems in one web module: literal icon names (icon("x"), data-icon="x", icon: "x") icons.json does not draw. Names built
// at run time (data-icon="${b.icon}") are not literals and are skipped.
QStringList scanWeb(const QString &file, const QString &text, const QSet<QString> &drawn) {
    static const QRegularExpression call(QStringLiteral("\\bicon\\(\\s*[\"']([^\"'\\s]+)[\"']"));
    static const QRegularExpression data(QStringLiteral("data-icon\\s*=\\s*[\"']?([^\"'\\s>]+)"));
    static const QRegularExpression prop(QStringLiteral("\\bicon(?:Right)?\\s*:\\s*[\"']([a-z][a-z0-9-]*)[\"']"));
    static const QRegularExpression literal(QStringLiteral("^[a-z0-9-]+$"));
    QStringList out;
    for (const QRegularExpression *re : {&call, &data, &prop}) {
        for (auto it = re->globalMatch(text); it.hasNext();) {
            const auto m = it.next();
            const QString name = m.captured(1);
            if (literal.match(name).hasMatch() && !drawn.contains(name))
                out << QStringLiteral("%1:%2: icon \"%3\" is not drawn in assets/ui/icons.json").arg(file).arg(lineOf(text, m.capturedStart(1))).arg(name);
        }
    }
    return out;
}

// Hotkey "Ctrl+Shift+K" -> modifiers and key; false when it is not in the canonical form (modifiers in modifierOrder, once each,
// then one key: A-Z, 0-9, one punctuation character, F1-F35 or a named key).
bool canonicalHotkey(const QString &seq, const QStringList &modOrder, QString *why) {
    static const QSet<QString> named = {"Space", "Escape", "Return", "Enter", "Delete", "Insert", "Tab", "Backspace", "Home", "End",
                                        "PgUp", "PgDown", "Left", "Right", "Up", "Down"};
    static const QRegularExpression re(QStringLiteral("^((?:(?:Ctrl|Alt|Shift|Meta|Num)\\+)*)(.+)$"));
    static const QRegularExpression fkey(QStringLiteral("^F([1-9]|[12][0-9]|3[0-5])$"));
    static const QRegularExpression single(QStringLiteral("^[A-Z0-9=+\\-,./;'\\[\\]`\\\\*]$"));
    const auto m = re.match(seq);
    if (!m.hasMatch()) { *why = "empty"; return false; }
    const QStringList mods = m.captured(1).split(u'+', Qt::SkipEmptyParts);
    int last = -1;
    for (const QString &mod : mods) {
        const int at = int(modOrder.indexOf(mod));
        if (at <= last) { *why = "modifiers out of order or repeated"; return false; }
        last = at;
    }
    const QString key = m.captured(2);
    if (!named.contains(key) && !fkey.match(key).hasMatch() && !single.match(key).hasMatch()) { *why = "key '" + key + "' is not canonical"; return false; }
    return true;
}

} // namespace

class TestUiSpec : public QObject {
    Q_OBJECT
    QByteArray rawB, rawI;
    QJsonObject B, I;
    QSet<QString> drawn, catalogue, theme, roles, levels;

    int rank(const QString &level) const { return int(B.value("safetyLevels").toArray().toVariantList().indexOf(level)); }

private slots:
    void initTestCase() {
        rawB = readFile(kSrc + "/assets/ui/buttons.json");
        rawI = readFile(kSrc + "/assets/ui/icons.json");
        QVERIFY2(!rawB.isEmpty() && !rawI.isEmpty(), "assets/ui/buttons.json or icons.json is missing");
        QJsonParseError e;
        const QJsonDocument db = QJsonDocument::fromJson(rawB, &e);
        QVERIFY2(db.isObject(), qPrintable("buttons.json: " + e.errorString() + " at offset " + QString::number(e.offset)));
        const QJsonDocument di = QJsonDocument::fromJson(rawI, &e);
        QVERIFY2(di.isObject(), qPrintable("icons.json: " + e.errorString() + " at offset " + QString::number(e.offset)));
        B = db.object();
        I = di.object();
        drawn = keySet(I);
        theme = keySet(B.value("theme").toObject());
        roles = keySet(B.value("roles").toObject());
        levels = setOf(B.value("safetyLevels").toArray());
        for (const QJsonValue &g : B.value("iconGroups").toArray())
            for (const QJsonValue &n : g.toObject().value("icons").toArray()) catalogue.insert(n.toString());
    }

    void jsonHasNoDuplicateKeys() {
        QVERIFY(duplicateKeys(QByteArray("{\"a\":1,\"b\":{\"a\":2,\"c\":[{\"a\":1,\"a\":2}]},\"b\":3}")).size() == 2);   // the scanner itself
        const QStringList db = duplicateKeys(rawB), di = duplicateKeys(rawI);
        QVERIFY2(db.isEmpty(), qPrintable("buttons.json has duplicate keys: " + db.join(", ")));
        QVERIFY2(di.isEmpty(), qPrintable("icons.json has duplicate keys: " + di.join(", ")));
        QVERIFY2(!rawB.startsWith("\xEF\xBB\xBF") && !rawI.startsWith("\xEF\xBB\xBF"), "the JSON files must not start with a BOM");
    }

    void pathCheckerSelfTest() {
        for (const char *good : {"M8 5.5 L18.5 12 L8 18.5 Z", "m3 4 1-1", "M1 1 A2 2 0 1 0 3 3 Z", "M1,1 2,2",
                                 "M2 2 C3 3 4 4 5 5 S7 7 8 8 Q9 9 10 10 T11 11 H12 V13 Z", "M0 0 L24 24 m-1 -1 h-2 v-2 z",
                                 "M12 16 V16.25", "M5 5 C8 8 8 2 5 5", "M1 1 L2 2 L1 1 Z"})
            QVERIFY2(checkPath(QString::fromLatin1(good)).isEmpty(), qPrintable(QString::fromLatin1(good) + ": " + checkPath(QString::fromLatin1(good))));
        for (const char *bad : {"", "L1 1", "M1 1 L2", "M1 1.5.5", "M1 1 A1 1 0 011 1 1", "M1 1 A1 1 0 2 1 3 3", "M1 1e2 3",
                                "M30 1", "M-1 1", "M1 1 X2 2", "M1 1,,2", "M1 1 Z 2", "M1 1 A0 1 0 1 1 3 3", "M1 1 C2 2 30 3 4 4", ", M1 1",
                                // degenerate pieces (eye-off's leftovers were the first two kinds)
                                "M10.06 13.94 A2.75 2.75 0 0 1 10.06 13.94", "M5 5 C8 5 3 12 3 12 L3 12 C3 10 4 9 5 9", "M1 1 H1.05",
                                "M1 1 C1 1 1 1 1 1", "M1 1 L2 2 l0 0", "M1 1 2 2 2 2", "M1 1", "M1 1 Z", "M1 1 M2 2 L3 3", "M2 2 L3 3 M4 4",
                                "M2 2 L3 3 Z m1 1"})
            QVERIFY2(!checkPath(QString::fromLatin1(bad)).isEmpty(), qPrintable(QStringLiteral("accepted bad path \"%1\"").arg(QString::fromLatin1(bad))));
    }

    void iconsWellFormed() {
        static const QRegularExpression nameRe(QStringLiteral("^[a-z0-9-]{1,32}$"));
        static const QSet<QString> fields = {"d", "fd", "fill", "sw", "tags", "origin"};
        QVERIFY(!I.isEmpty());
        for (auto it = I.begin(); it != I.end(); ++it) {
            const QString n = it.key();
            QVERIFY2(nameRe.match(n).hasMatch(), qPrintable("icon name \"" + n + "\" must match [a-z0-9-]{1,32}"));
            QVERIFY2(it.value().isObject(), qPrintable(n + ": not an object"));
            const QJsonObject o = it.value().toObject();
            for (const QString &k : o.keys()) QVERIFY2(fields.contains(k), qPrintable(n + ": unknown field " + k));
            QVERIFY2(o.value("d").isArray() && !o.value("d").toArray().isEmpty(), qPrintable(n + ": d must be a non-empty array"));
            QVERIFY2(!o.contains("fd") || o.value("fd").isArray(), qPrintable(n + ": fd must be an array"));
            for (const char *member : {"d", "fd"})
                for (const QJsonValue &p : o.value(QLatin1String(member)).toArray()) {
                    QVERIFY2(p.isString(), qPrintable(n + ": path is not a string"));
                    const QString err = checkPath(p.toString());
                    QVERIFY2(err.isEmpty(), qPrintable(QStringLiteral("%1.%2: %3 in \"%4\"").arg(n, QLatin1String(member), err, p.toString())));
                }
            QVERIFY2(!o.contains("fill") || o.value("fill").isBool(), qPrintable(n + ": fill must be true or false"));
            const double sw = o.value("sw").toDouble(1.75);   // stroke widths in quarter steps keep the set consistent
            QVERIFY2(sw >= 1.0 && sw <= 2.5 && std::fmod(sw * 4, 1.0) == 0, qPrintable(n + ": sw must be 1..2.5 in steps of 0.25"));
            const QJsonArray tags = o.value("tags").toArray();
            QVERIFY2(!tags.isEmpty(), qPrintable(n + ": needs tags (the icon picker searches them)"));
            for (const QJsonValue &t : tags) QVERIFY2(t.isString() && !t.toString().isEmpty() && t.toString() == t.toString().toLower(), qPrintable(n + ": tags are lower-case strings"));
            QVERIFY2(o.value("origin").toString() == u"original", qPrintable(n + ": origin must be \"original\" (drawn in-house, not copied from an icon set)"));
        }
    }

    void iconCatalogue() {
        static const QRegularExpression nameRe(QStringLiteral("^[a-z0-9-]{1,32}$"));
        QSet<QString> ids, seen;
        for (const QJsonValue &g : B.value("iconGroups").toArray()) {
            const QJsonObject o = g.toObject();
            QVERIFY2(!o.value("id").toString().isEmpty() && !ids.contains(o.value("id").toString()), "iconGroups need unique ids");
            ids.insert(o.value("id").toString());
            QVERIFY(!o.value("label").toString().isEmpty());
            for (const QJsonValue &n : o.value("icons").toArray()) {
                QVERIFY2(nameRe.match(n.toString()).hasMatch(), qPrintable("catalogue name " + n.toString()));
                QVERIFY2(!seen.contains(n.toString()), qPrintable(n.toString() + " is in two icon groups"));
                seen.insert(n.toString());
            }
        }
        for (const QString &n : drawn) QVERIFY2(catalogue.contains(n), qPrintable("icon " + n + " is drawn but not listed in iconGroups"));
        // UXB-2 completed the set: every catalogue name is drawn, so glyph-map and safety targets (which must be catalogue names)
        // always resolve. A new name goes into iconGroups and icons.json together.
        QStringList todo;
        for (const QString &n : catalogue) if (!drawn.contains(n)) todo << n;
        todo.sort();
        QVERIFY2(todo.isEmpty(), qPrintable("listed in iconGroups but not drawn in icons.json: " + todo.join(' ')));
        // tags start with the icon's group id, so the icon picker can show the groups from the tags alone
        for (const QJsonValue &g : B.value("iconGroups").toArray()) {
            const QString id = g.toObject().value("id").toString();
            for (const QJsonValue &n : g.toObject().value("icons").toArray()) {
                const QJsonArray tags = I.value(n.toString()).toObject().value("tags").toArray();
                QVERIFY2(!tags.isEmpty() && tags.at(0).toString() == id, qPrintable(n.toString() + ": the first tag must be its group id \"" + id + "\""));
            }
        }
    }

    void pathSamplerSelfTest() {
        const auto near = [](const std::array<double, 4> &b, std::array<double, 4> want) {
            for (int k = 0; k < 4; ++k) if (std::abs(b[k] - want[k]) > 0.01) return false;
            return true;
        };
        const QList<QPair<const char *, std::array<double, 4>>> cases = {
            {"M18 12 A6 6 0 1 1 6 12 A6 6 0 1 1 18 12 Z", {6, 6, 18, 18}},     // a circle as two half arcs
            {"M6 12 A6 6 0 0 1 18 12", {6, 6, 18, 12}},                         // sweep 1: clockwise on screen, over the top
            {"M6 12 A6 6 0 0 0 18 12", {6, 12, 18, 18}},                        // sweep 0: under
            {"M10 5 A5 2.5 90 0 1 10 15", {10, 5, 12.5, 15}},                   // rotated ellipse
            {"M12 12 A2 2 0 0 1 22 12", {12, 7, 22, 12}},                        // radius too small: scaled up to 5
            {"M2 2 C2 10 10 10 10 2", {2, 2, 10, 8}},                           // cubic: extreme between the end points
            {"M2 2 C2 6 6 6 6 2 S10 -2 10 2", {2, -1, 10, 5}},                  // S reflects the last control point
            {"M4 4 Q12 20 20 4", {4, 4, 20, 12}},
            {"M2 2 Q4 6 6 2 T10 2", {2, 0, 10, 4}},                             // T reflects too
            {"m4 4 h10 v10 z m2 2 l1 1", {4, 4, 14, 14}},                       // relative; z returns to the subpath start
            {"M1 1 2 3 4 1", {1, 1, 4, 3}},                                     // implicit linetos after M
            {"M5 5 L7 5 A2 2 0 0 1 7 5", {5, 5, 7, 5}},                         // an arc back to its start is omitted (F.6.2)
            {"M5 5 A0 2 0 0 1 7 6", {5, 5, 7, 6}}};                             // a zero radius makes the arc a line
        for (const auto &c : cases) {
            const auto b = boundsOf(samplePath(QString::fromLatin1(c.first)));
            QVERIFY2(near(b, c.second), qPrintable(QStringLiteral("%1: bounds %2 %3 %4 %5").arg(QLatin1String(c.first)).arg(b[0]).arg(b[1]).arg(b[2]).arg(b[3])));
        }
        QCOMPARE(samplePath(QStringLiteral("M1 1 L2 2 M5 5 H6 M8 8 V9")).size(), qsizetype(3));
        double r = 1e9;   // the drawn arc radius: as written, or scaled up when too small for the end points
        samplePath(QStringLiteral("M1 1 A0.5 0.75 0 0 1 2 1 L4 4"), &r);
        QCOMPARE(r, 0.5);
        r = 1e9;
        samplePath(QStringLiteral("M12 12 A2 2 0 0 1 22 12"), &r);
        QVERIFY2(std::abs(r - 5) < 1e-9, qPrintable(QString::number(r)));
    }

    // Every icon's ink stays inside 1.5..22.5 (a margin to the edge, so round caps never clip at any zoom) and is centred within
    // 2.25 of (12, 12) (play's triangle sits right of centre on purpose, A- shares the A+ baseline); the filled parts of one icon
    // must not overlap (bounding boxes apart), so the web sprite (nonzero fill) and a Qt ShapePath (odd-even by default) agree.
    // No arc may be tighter than half the stroke: the stroke's inner edge would fold over, and Chromium (Skia) then leaves a hole
    // that Qt fills; filled dots of r 0.25 came out as rings on the web (UXB-2 review). Small dots are short stroked segments.
    void iconGeometry() {
        for (auto it = I.begin(); it != I.end(); ++it) {
            const QString n = it.key();
            const QJsonObject o = it.value().toObject();
            const double half = o.value("sw").toDouble(1.75) / 2;
            QList<QList<QPointF>> all, filled;
            double minArcR = 1e9;
            const bool fill = o.value("fill").toBool();
            for (const QJsonValue &p : o.value("d").toArray()) { const auto s = samplePath(p.toString(), &minArcR); all << s; if (fill) filled << s; }
            for (const QJsonValue &p : o.value("fd").toArray()) { const auto s = samplePath(p.toString(), &minArcR); all << s; filled << s; }
            QVERIFY2(minArcR >= half - 1e-9,
                     qPrintable(QStringLiteral("%1: an arc of radius %2 is tighter than half the stroke (%3); Chromium draws it hollow. "
                                               "Use a radius of at least %3, or draw a small dot as a short stroked segment (\"M12 16 V16.25\")")
                                    .arg(n).arg(minArcR).arg(half)));
            const auto b = boundsOf(all);
            const double x0 = b[0] - half, y0 = b[1] - half, x1 = b[2] + half, y1 = b[3] + half;
            QVERIFY2(x0 >= 1.5 && y0 >= 1.5 && x1 <= 22.5 && y1 <= 22.5,
                     qPrintable(QStringLiteral("%1: ink spans (%2, %3)..(%4, %5); keep it inside 1.5..22.5").arg(n).arg(x0).arg(y0).arg(x1).arg(y1)));
            const double cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
            QVERIFY2(std::abs(cx - 12) <= 2.25 && std::abs(cy - 12) <= 2.25,
                     qPrintable(QStringLiteral("%1: ink centre (%2, %3) is off the grid centre").arg(n).arg(cx).arg(cy)));
            QVERIFY2(std::max(x1 - x0, y1 - y0) >= 6, qPrintable(n + ": too small to read at 12 px"));
            for (int a = 0; a < filled.size(); ++a)
                for (int c = a + 1; c < filled.size(); ++c) {
                    const auto ba = boundsOf({filled[a]}), bc = boundsOf({filled[c]});
                    const bool overlap = ba[0] < bc[2] && bc[0] < ba[2] && ba[1] < bc[3] && bc[1] < ba[3];
                    QVERIFY2(!overlap, qPrintable(QStringLiteral("%1: filled parts %2 and %3 overlap (nonzero and odd-even fills would differ)").arg(n).arg(a).arg(c)));
                }
        }
    }

    void themeMatchesQmlAndCss() {
        const QString qml = QString::fromUtf8(readFile(kSrc + "/Play_Play_Native/qml/Theme.qml"));
        const QString css = QString::fromUtf8(readFile(kSrc + "/web/style.css"));
        QVERIFY2(!qml.isEmpty() && !css.isEmpty(), "Theme.qml or web/style.css is missing");
        QHash<QString, QString> qmlHex, cssHex;
        static const QRegularExpression qre(QStringLiteral("readonly\\s+property\\s+color\\s+(\\w+)\\s*:\\s*\"(#[0-9a-fA-F]{6})\""));
        for (auto it = qre.globalMatch(qml); it.hasNext();) { const auto m = it.next(); qmlHex.insert(m.captured(1), m.captured(2).toLower()); }
        const qsizetype a = css.indexOf(":root"), z = css.indexOf(u'}', a);
        QVERIFY2(a >= 0 && z > a, "web/style.css has no :root block");
        static const QRegularExpression cre(QStringLiteral("--([A-Za-z0-9-]+)\\s*:\\s*(#[0-9a-fA-F]{6})"));
        for (auto it = cre.globalMatch(css.mid(a, z - a)); it.hasNext();) { const auto m = it.next(); cssHex.insert(m.captured(1), m.captured(2).toLower()); }
        static const QRegularExpression hexRe(QStringLiteral("^#[0-9a-f]{6}$"));
        const QJsonObject th = B.value("theme").toObject();
        for (auto it = th.begin(); it != th.end(); ++it) {
            const QJsonObject o = it.value().toObject();
            const QString hex = o.value("hex").toString(), cssName = o.value("css").toString();
            QVERIFY2(hexRe.match(hex).hasMatch(), qPrintable(it.key() + ": hex must be #rrggbb in lower case"));
            QVERIFY2(!cssName.isEmpty(), qPrintable(it.key() + ": css name missing"));
            if (o.value("new").toBool()) {   // defined by ui-tokens.css; must not contradict an existing definition
                QVERIFY2(!qmlHex.contains(it.key()) || qmlHex.value(it.key()) == hex, qPrintable(it.key() + " differs from Theme.qml"));
                QVERIFY2(!cssHex.contains(cssName) || cssHex.value(cssName) == hex, qPrintable(cssName + " differs from style.css"));
            } else {
                QVERIFY2(qmlHex.value(it.key()) == hex, qPrintable(QStringLiteral("theme.%1 is %2 but Theme.qml has %3").arg(it.key(), hex, qmlHex.value(it.key(), "nothing"))));
                QVERIFY2(cssHex.value(cssName) == hex, qPrintable(QStringLiteral("theme.%1 is %2 but style.css --%3 is %4").arg(it.key(), hex, cssName, cssHex.value(cssName, "missing"))));
            }
        }
    }

    void tokens() {
        // sizes: the design's table (internal plan ux-buttons.md section 3.2); s and m equal CButton, l the deck keys, xl the remote pad
        const QJsonObject sizes = B.value("sizes").toObject();
        const QStringList order = {"xs", "s", "m", "l", "xl"};
        QCOMPARE(B.value("sizeOrder").toArray().toVariantList(), QVariant(order).toList());
        QCOMPARE(keySet(sizes), QSet<QString>(order.begin(), order.end()));
        const QList<int> h = {20, 24, 30, 46, 58}, minW = {20, 24, 30, 64, 104}, padX = {6, 8, 11, 8, 10}, font = {10, 11, 12, 10, 12},
                         icon = {12, 13, 15, 20, 22}, radius = {3, 5, 5, 5, 8};
        for (int i = 0; i < order.size(); ++i) {
            const QJsonObject s = sizes.value(order[i]).toObject();
            QCOMPARE(s.value("h").toInt(), h[i]);
            QCOMPARE(s.value("minW").toInt(), minW[i]);
            QCOMPARE(s.value("padX").toInt(), padX[i]);
            QCOMPARE(s.value("font").toInt(), font[i]);
            QCOMPARE(s.value("icon").toInt(), icon[i]);
            QCOMPARE(s.value("radius").toInt(), radius[i]);
            QVERIFY(s.value("tracking").isDouble() && s.value("gap").isDouble());
            const QString layout = s.value("layout").toString();
            QVERIFY2(layout == u"row" || layout == u"stacked", qPrintable(order[i] + ": layout"));
            QVERIFY2(layout == u"row" || s.value("subFont").toInt() > 0, qPrintable(order[i] + ": a stacked size needs subFont"));
        }
        // roles: colours are theme names; roleOrder lists each once
        const QJsonObject r = B.value("roles").toObject();
        QCOMPARE(setOf(B.value("roleOrder").toArray()), roles);
        QCOMPARE(B.value("roleOrder").toArray().size(), r.size());
        for (const QString &role : {"neutral", "primary", "go", "stop", "destructive", "warn", "info", "accent", "cued"}) QVERIFY2(roles.contains(role), qPrintable("role " + role));
        for (auto it = r.begin(); it != r.end(); ++it) {
            QVERIFY2(theme.contains(it.value().toObject().value("color").toString()), qPrintable("role " + it.key() + ": colour is not a theme name"));
            QVERIFY(!it.value().toObject().value("label").toString().isEmpty());
        }
        QCOMPARE(r.value("info").toObject().value("color").toString(), QStringLiteral("info"));
        // fills and the legacy CButton / DeckButton variants
        const QJsonObject fills = B.value("fills").toObject();
        QCOMPARE(setOf(B.value("fillOrder").toArray()), keySet(fills));
        for (auto it = fills.begin(); it != fills.end(); ++it)
            for (const char *f : {"bg", "border", "text", "icon"}) {
                const QString c = it.value().toObject().value(QLatin1String(f)).toString();
                QVERIFY2(c.isEmpty() || c == u"tint" || c == u"onFill" || theme.contains(c), qPrintable(QStringLiteral("fills.%1.%2: %3").arg(it.key(), QLatin1String(f), c)));
            }
        QVERIFY(fills.value("tonal").toObject().value("bgA").toDouble() > 0);
        const QJsonObject autoFill = B.value("autoFill").toObject();
        for (const char *k : {"neutral", "coloured", "ghost", "active"}) QVERIFY2(fills.contains(autoFill.value(QLatin1String(k)).toString()), k);
        QVERIFY(theme.contains(autoFill.value("neutralBg").toString()));
        const QJsonObject legacy = B.value("legacyVariants").toObject();
        QCOMPARE(keySet(legacy), (QSet<QString>{"default", "primary", "success", "danger", "warn", "info", "accent2", "ghost"}));
        for (auto it = legacy.begin(); it != legacy.end(); ++it) {
            const QJsonObject o = it.value().toObject();
            QVERIFY2(roles.contains(o.value("role").toString()), qPrintable("legacyVariants." + it.key()));
            QVERIFY2(o.value("fill").toString() == u"auto" || fills.contains(o.value("fill").toString()), qPrintable("legacyVariants." + it.key()));
        }
        // states: strings are theme colours (except the sub-label text); timing in whole milliseconds
        const QJsonObject st = B.value("states").toObject();
        for (auto it = st.begin(); it != st.end(); ++it) {
            if (it.value().isString()) QVERIFY2(it.key().endsWith(u"Sublabel") || theme.contains(it.value().toString()), qPrintable("states." + it.key() + " is not a theme colour"));
            else QVERIFY2(it.value().isDouble() && it.value().toDouble() >= 0, qPrintable("states." + it.key()));
        }
        QCOMPARE(st.value("armSublabel").toString(), QStringLiteral("CONFIRM"));
        const QJsonObject timing = B.value("timing").toObject();
        QCOMPARE(timing.value("armMs").toInt(), 3000);
        QCOMPARE(timing.value("holdMs").toInt(), 800);
        QCOMPARE(timing.value("minIntervalMs").toInt(), 250);
        for (auto it = timing.begin(); it != timing.end(); ++it)
            QVERIFY2(it.key().endsWith(u"Ms") && it.value().toDouble() > 0 && std::fmod(it.value().toDouble(), 1.0) == 0, qPrintable("timing." + it.key()));
        // LEDs
        const QJsonObject leds = B.value("leds").toObject(), lc = leds.value("colors").toObject();
        QCOMPARE(keySet(lc), (QSet<QString>{"red", "green", "amber", "blue"}));
        for (auto it = lc.begin(); it != lc.end(); ++it) QVERIFY2(theme.contains(it.value().toString()), qPrintable("leds.colors." + it.key()));
        for (const QJsonValue &v : leds.value("tally").toObject()) QVERIFY(lc.contains(v.toString()));
        for (const QJsonValue &v : leds.value("blinkHz").toObject()) QVERIFY(v.toDouble() >= 0 && std::fmod(v.toDouble(), 1.0) == 0);
        // swatches and limits
        static const QRegularExpression hexRe(QStringLiteral("^#[0-9a-f]{6}$"));
        const QJsonArray sw = B.value("swatches").toArray();
        QCOMPARE(sw.size(), 16);
        QCOMPARE(setOf(sw).size(), 16);
        for (const QJsonValue &v : sw) QVERIFY2(hexRe.match(v.toString()).hasMatch(), qPrintable("swatch " + v.toString()));
        const QJsonObject lim = B.value("limits").toObject();
        QCOMPARE(lim.value("labelMax").toInt(), 28);
        const QRegularExpression colorRe(lim.value("colorPattern").toString());
        QVERIFY(colorRe.isValid() && colorRe.match("#00d4FF").hasMatch() && !colorRe.match("#00d4f").hasMatch() && !colorRe.match("red").hasMatch());
        for (const QJsonValue &v : lim.value("padSizes").toArray()) QVERIFY(sizes.contains(v.toString()));
        for (const QJsonValue &v : lim.value("padSpans").toArray()) QVERIFY(v.toInt() >= 1);
    }

    void glyphMap() {
        const QJsonObject g = B.value("glyphs").toObject();
        static const QRegularExpression plain(QStringLiteral("[A-Za-z0-9\\s]"));
        for (auto it = g.begin(); it != g.end(); ++it) {
            const QString k = it.key();
            QVERIFY2(!k.isEmpty() && k.size() <= 4 && !plain.match(k).hasMatch() && !k.contains(QChar(0xFE0F)),
                     qPrintable("glyph key \"" + k + "\": one or two symbols, no letters, digits, spaces or U+FE0F"));
            const QJsonObject o = it.value().toObject();
            QVERIFY2(catalogue.contains(o.value("icon").toString()), qPrintable("glyph \"" + k + "\" maps to " + o.value("icon").toString() + ", which is not in iconGroups"));
            QVERIFY2(!o.value("tip").toString().isEmpty(), qPrintable("glyph \"" + k + "\" needs a tip (the shim's tooltip for icon-only buttons)"));
        }
        // the glyphs today's QML and web buttons use most (internal plan ux-buttons.md sections 1.3, 2 and 3.6)
        const QList<QPair<QString, QString>> must = {
            {"\u25B6", "play"}, {"\u2016", "pause"}, {"\u23F8", "pause"}, {"\u25A0", "stop"}, {"\u23ED", "skip-next"}, {"\u23EE", "skip-prev"},
            {"\u25C0\u2758", "frame-back"}, {"\u2758\u25B6", "frame-fwd"}, {"\u21BB", "refresh"}, {"\u27F3", "refresh"}, {"\u2715", "close"},
            {"\u270E", "edit"}, {"\u2398", "copy"}, {"\u2197", "external"}, {"\u26A1", "bolt"}, {"\u2913", "load-hour"}, {"+", "plus"},
            {"\uFF0B", "plus"}, {"\u23F0", "alarm"}, {"\u21E5", "cut"}, {"\u29C9", "auto"}, {"\u21A9", "return"}, {"\u2192", "chevron-right"},
            {"\u2713", "check"}, {QString::fromUtf8("\xF0\x9F\x94\x91"), "key"}, {QString::fromUtf8("\xF0\x9F\x94\x92"), "lock"}};
        for (const auto &p : must)
            QVERIFY2(g.value(p.first).toObject().value("icon").toString() == p.second, qPrintable("glyph \"" + p.first + "\" should map to " + p.second));
    }

    void hotkeys() {
        const QJsonObject rules = B.value("hotkeyRules").toObject();
        QStringList modOrder;
        for (const QJsonValue &v : rules.value("modifierOrder").toArray()) modOrder << v.toString();
        QCOMPARE(modOrder, (QStringList{"Ctrl", "Alt", "Shift", "Meta", "Num"}));
        const QJsonObject reserved = B.value("reservedKeys").toObject();
        for (const char *s : {"global", "onair", "schedule"}) QVERIFY2(reserved.contains(QLatin1String(s)), s);
        const QJsonObject aliases = rules.value("aliases").toObject();
        QSet<QString> global;
        for (auto it = reserved.begin(); it != reserved.end(); ++it) {
            QSet<QString> seen;
            for (const QJsonValue &v : it.value().toArray()) {
                const QString k = v.toString();
                QString why;
                QVERIFY2(canonicalHotkey(k, modOrder, &why), qPrintable(QStringLiteral("reservedKeys.%1: \"%2\" %3").arg(it.key(), k, why)));
                for (const QString &part : k.split(u'+')) QVERIFY2(!aliases.contains(part), qPrintable(k + " uses the alias " + part));
                QVERIFY2(!seen.contains(k), qPrintable(QStringLiteral("reservedKeys.%1 lists %2 twice").arg(it.key(), k)));
                seen.insert(k);
            }
            if (it.key() == u"global") global = seen;
        }
        for (const QJsonValue &v : reserved.value("onair").toArray()) QVERIFY2(!global.contains(v.toString()), qPrintable(v.toString() + " is reserved globally already"));
        for (const QString &k : {"F1", "F8", "Space", "Ctrl+Right", "Ctrl+L", "Ctrl+0", "Ctrl+C", "Delete"}) QVERIFY2(global.contains(k), qPrintable(k));
        const QSet<QString> onair = setOf(reserved.value("onair").toArray());
        for (const QString &k : {"1", "8", "Alt+1", "Alt+8", "Ctrl+Return", "Ctrl+Enter", "Ctrl+Shift+Return", "Escape"}) QVERIFY2(onair.contains(k), qPrintable(k));
        for (auto it = aliases.begin(); it != aliases.end(); ++it) {
            QString why;
            QVERIFY2(modOrder.contains(it.value().toString()) || canonicalHotkey(it.value().toString(), modOrder, &why), qPrintable("alias " + it.key()));
        }
        const QRegularExpression native(rules.value("allowedNative").toString()), web(rules.value("allowedWeb").toString());
        QVERIFY2(native.isValid() && web.isValid(), "allowedNative / allowedWeb must be valid regular expressions");
        for (const char *k : {"F9", "F12", "Ctrl+K", "Alt+7", "Shift+Q", "Ctrl+Shift+K", "Num+5", "Num+Enter"}) {
            QVERIFY2(native.match(QLatin1String(k)).hasMatch(), k);
            QVERIFY2(web.match(QLatin1String(k)).hasMatch(), k);
        }
        for (const char *k : {"K", "7", "F1", "F8", "Ctrl+Alt+K", "Ctrl+F1", "Space", "Ctrl+k"}) QVERIFY2(!native.match(QLatin1String(k)).hasMatch(), k);
        for (const char *k : {"K", "7"}) QVERIFY2(web.match(QLatin1String(k)).hasMatch(), k);
    }

    void safetyDefaults() {
        QCOMPARE(B.value("safetyLevels").toArray().toVariantList(), QVariant(QStringList{"none", "arm", "hold", "modal"}).toList());
        static const QRegularExpression idRe(QStringLiteral("^[a-z]+(\\.[a-zA-Z]+)+$"));
        static const QSet<QString> fields = {"label", "icon", "role", "native", "web", "floor", "tip", "hotkey", "off"};
        const int labelMax = B.value("limits").toObject().value("labelMax").toInt();
        QSet<QString> reservedAll;
        const QJsonObject reserved = B.value("reservedKeys").toObject();
        for (const QJsonValue &a : reserved) reservedAll.unite(setOf(a.toArray()));
        const QJsonObject sd = B.value("safetyDefaults").toObject();
        for (auto it = sd.begin(); it != sd.end(); ++it) {
            const QString id = it.key();
            QVERIFY2(idRe.match(id).hasMatch(), qPrintable("action id " + id));
            const QJsonObject on = it.value().toObject();
            for (const QString &k : on.keys()) QVERIFY2(fields.contains(k), qPrintable(id + ": unknown field " + k));
            QList<QJsonObject> states = {on};
            if (on.contains("off")) {
                QJsonObject off = on;
                const QJsonObject o = on.value("off").toObject();
                QVERIFY2(!o.isEmpty() && !o.contains("off") && !o.contains("role") && !o.contains("hotkey"), qPrintable(id + ".off overrides label / icon / tip / levels only"));
                for (auto f = o.begin(); f != o.end(); ++f) off.insert(f.key(), f.value());
                states << off;
            }
            for (const QJsonObject &s : states) {
                const QString label = s.value("label").toString(), native = s.value("native").toString(), web = s.value("web").toString(), floor = s.value("floor").toString();
                QVERIFY2(!label.isEmpty() && label.size() <= labelMax, qPrintable(id + ": label"));
                QVERIFY2(catalogue.contains(s.value("icon").toString()), qPrintable(id + ": icon " + s.value("icon").toString() + " is not in iconGroups"));
                QVERIFY2(roles.contains(s.value("role").toString()), qPrintable(id + ": role"));
                QVERIFY2(!s.value("tip").toString().isEmpty(), qPrintable(id + ": tip"));
                QVERIFY2(levels.contains(native) && levels.contains(floor) && (web.isEmpty() || levels.contains(web)), qPrintable(id + ": safety level"));
                QVERIFY2(rank(floor) <= rank(native), qPrintable(id + ": native default " + native + " is below the floor " + floor));
                QVERIFY2(web.isEmpty() || rank(floor) <= rank(web), qPrintable(id + ": web default " + web + " is below the floor " + floor));
                if (s.contains("hotkey")) QVERIFY2(reservedAll.contains(s.value("hotkey").toString()), qPrintable(id + ": a built-in hotkey must be a reserved key"));
            }
        }
        // the design's safety table (section 3.5): what must hold, whatever else changes
        const auto lv = [&](const char *id, const char *field, bool off = false) {
            const QJsonObject o = sd.value(QLatin1String(id)).toObject();
            return (off && o.value("off").toObject().contains(QLatin1String(field)) ? o.value("off").toObject() : o).value(QLatin1String(field)).toString();
        };
        QCOMPARE(lv("transport.stop", "native"), QStringLiteral("arm"));
        QCOMPARE(lv("transport.stop", "floor"), QStringLiteral("arm"));
        QCOMPARE(lv("transport.next", "web"), QStringLiteral("modal"));
        QCOMPARE(lv("automation.arm", "native"), QStringLiteral("arm"));        // armed: pressing disarms
        QCOMPARE(lv("automation.arm", "native", true), QStringLiteral("none"));  // disarmed: pressing arms
        QCOMPARE(lv("engine.toggle", "native"), QStringLiteral("hold"));
        QCOMPARE(lv("engine.toggle", "floor"), QStringLiteral("hold"));
        QCOMPARE(lv("engine.toggle", "native", true), QStringLiteral("none"));
        QCOMPARE(lv("mux.stop", "native"), QStringLiteral("hold"));
        QCOMPARE(lv("live.end", "native"), QStringLiteral("hold"));
        QCOMPARE(lv("live.start", "native"), QStringLiteral("arm"));
        QCOMPARE(lv("channel.delete", "native"), QStringLiteral("modal"));
        QCOMPARE(lv("channel.delete", "floor"), QStringLiteral("modal"));
        QCOMPARE(lv("cg.clearAll", "floor"), QStringLiteral("none"));
        for (const char *id : {"bus.take", "bus.cut", "bus.auto", "schedule.return"}) QCOMPARE(lv(id, "native"), QStringLiteral("none"));
    }

    void generatedFilesInSync() {
        QVERIFY2(QFileInfo::exists(QStringLiteral(PP_CMAKE_COMMAND)), "the cmake that configured this build is gone; cannot regenerate");
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const auto run = [&](QByteArray *log) {
            QProcess p;
            p.setProcessChannelMode(QProcess::MergedChannels);
            p.start(QStringLiteral(PP_CMAKE_COMMAND), {"-DOUT_DIR=" + tmp.path(), "-P", kSrc + "/tools/uispec/gen_uispec.cmake"});
            const bool done = p.waitForFinished(90000);
            *log = p.readAll();
            return done && p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
        };
        QByteArray log;
        QVERIFY2(run(&log), log.constData());
        for (const QString &rel : kOutputs) {
            const QByteArray freshRaw = readFile(tmp.path() + "/" + rel), committed = lf(readFile(kSrc + "/" + rel));
            QVERIFY2(!freshRaw.isEmpty(), qPrintable("the generator wrote no " + rel));
            QVERIFY2(!freshRaw.contains('\r'), qPrintable(rel + ": the generator must write LF line ends"));
            if (freshRaw != committed) {
                const QList<QByteArray> a = freshRaw.split('\n'), b = committed.split('\n');
                int line = 0;
                while (line < a.size() && line < b.size() && a[line] == b[line]) ++line;
                QFAIL(qPrintable(QStringLiteral("%1 differs from a fresh generation at line %2 (committed: \"%3\", fresh: \"%4\"). "
                                                "Edit assets/ui/*.json, never the output, then run: cmake -P tools/uispec/gen_uispec.cmake")
                                     .arg(rel).arg(line + 1).arg(QString::fromUtf8(b.value(line).left(120)), QString::fromUtf8(a.value(line).left(120)))));
            }
        }
        // a second run over its own output changes nothing (deterministic, and it skips unchanged files)
        QVERIFY2(run(&log), log.constData());
        QCOMPARE(log.count("gen_uispec: unchanged"), qsizetype(kOutputs.size()));
    }

    void generatedFilesLoad() {
        // spec.js through the QML engine, as the PlayPlay.Buttons module will import it
        {
            QQmlEngine engine;
            QQmlComponent c(&engine);
            c.setData("import QtQml\nimport \"spec.js\" as Spec\nQtObject {\n"
                      "  property int version: Spec.version\n  property int mh: Spec.sizes.m.h\n"
                      "  property int icons: Object.keys(Spec.icons).length\n  property string playFill: Spec.icons.play.fill\n"
                      "  property string stopGlyph: Spec.glyphs[\"\\u25A0\"].icon\n"
                      "  property string stopSafety: Spec.safetyDefaults[\"transport.stop\"].native\n"
                      "  property int onairKeys: Spec.reservedKeys.onair.length\n}\n",
                      QUrl::fromLocalFile(kSrc + "/Play_Play_Native/qml/PlayPlay/Buttons/probe_uispec.qml"));
            QScopedPointer<QObject> o(c.create());
            QVERIFY2(o, qPrintable(c.errorString()));
            QCOMPARE(o->property("version").toInt(), B.value("version").toInt());
            QCOMPARE(o->property("mh").toInt(), 30);
            QCOMPARE(o->property("icons").toInt(), int(I.size()));
            QVERIFY(!o->property("playFill").toString().isEmpty());
            QCOMPARE(o->property("stopGlyph").toString(), QStringLiteral("stop"));
            QCOMPARE(o->property("stopSafety").toString(), QStringLiteral("arm"));
            QCOMPARE(o->property("onairKeys").toInt(), int(B.value("reservedKeys").toObject().value("onair").toArray().size()));
        }
        // ui-spec.js as an ES module: every export equals its JSON source (icons after the stroke / fill transform)
        {
            QJSEngine js;
            const QJSValue ns = js.importModule(kSrc + "/web/ui-spec.js");
            QVERIFY2(!ns.isError(), qPrintable(ns.toString()));
            const QJSValue stringify = js.globalObject().property("JSON").property("stringify");
            const auto asJson = [&](const QJSValue &v) { return QJsonDocument::fromJson("[" + stringify.call({v}).toString().toUtf8() + "]").array().at(0); };
            QSet<QString> exported;
            for (QJSValueIterator it(ns); it.hasNext();) { it.next(); exported.insert(it.name()); }
            QSet<QString> expected = keySet(B);
            expected.remove("about");
            expected.insert("icons");
            QCOMPARE(exported, expected);
            for (auto it = B.begin(); it != B.end(); ++it) {
                if (it.key() == u"about") continue;
                QVERIFY2(asJson(ns.property(it.key())) == it.value(), qPrintable("ui-spec.js export " + it.key() + " differs from buttons.json"));
            }
            const QJsonObject icons = asJson(ns.property("icons")).toObject();
            QCOMPARE(keySet(icons), drawn);
            const auto join = [](const QJsonValue &a) {
                QStringList s;
                for (const QJsonValue &v : a.toArray()) s << v.toString().trimmed();
                return s.join(u' ');
            };
            for (auto it = I.begin(); it != I.end(); ++it) {
                const QJsonObject src = it.value().toObject(), out = icons.value(it.key()).toObject();
                const bool filled = src.value("fill").toBool();
                const QString d = join(src.value("d")), fd = join(src.value("fd"));
                QCOMPARE(out.value("stroke").toString(), filled ? QString() : d);
                QCOMPARE(out.value("fill").toString(), filled ? (fd.isEmpty() ? d : d + u' ' + fd) : fd);
                QCOMPARE(out.value("sw").toDouble(), src.value("sw").toDouble(1.75));
                QCOMPARE(out.value("tags"), src.value("tags"));
            }
        }
        // icons.svg: well-formed XML, one <symbol id="i-NAME" viewBox="0 0 24 24"> per drawn icon
        {
            QXmlStreamReader x(readFile(kSrc + "/web/icons.svg"));
            QSet<QString> ids;
            int paths = 0;
            while (!x.atEnd()) {
                if (x.readNext() != QXmlStreamReader::StartElement) continue;
                if (x.name() == u"svg") QCOMPARE(x.namespaceUri().toString(), QStringLiteral("http://www.w3.org/2000/svg"));
                if (x.name() == u"symbol") {
                    QCOMPARE(x.attributes().value("viewBox").toString(), QStringLiteral("0 0 24 24"));
                    ids.insert(x.attributes().value("id").toString());
                }
                if (x.name() == u"path") { QVERIFY(!x.attributes().value("d").isEmpty()); ++paths; }
            }
            QVERIFY2(!x.hasError(), qPrintable("icons.svg: " + x.errorString()));
            QSet<QString> expected;
            for (const QString &n : drawn) expected.insert("i-" + n);
            QCOMPARE(ids, expected);
            QVERIFY(paths >= int(drawn.size()));
        }
        // ui-tokens.css: one :root block of custom properties, each var() defined here or in style.css
        {
            const QString css = QString::fromUtf8(lf(readFile(kSrc + "/web/ui-tokens.css")));   // CRLF checkouts: the line regex below anchors on LF
            const qsizetype a = css.indexOf(":root {"), z = css.lastIndexOf(u'}');
            QVERIFY2(a >= 0 && z > a && css.count(u'{') == 1 && css.count(u'}') == 1, "ui-tokens.css must be one :root block");
            QSet<QString> defined;
            static const QRegularExpression decl(QStringLiteral("^  --([a-z0-9-]+): ([^;]+);$"));
            for (const QString &line : css.mid(a + 7, z - a - 7).split(u'\n', Qt::SkipEmptyParts)) {
                const auto m = decl.match(line);
                QVERIFY2(m.hasMatch(), qPrintable("ui-tokens.css: \"" + line + "\""));
                defined.insert(m.captured(1));
            }
            static const QRegularExpression styleVar(QStringLiteral("--([A-Za-z0-9-]+)\\s*:"));
            const QString style = QString::fromUtf8(readFile(kSrc + "/web/style.css"));
            for (auto it = styleVar.globalMatch(style); it.hasNext();) defined.insert(it.next().captured(1));
            static const QRegularExpression use(QStringLiteral("var\\(--([a-z0-9-]+)\\)"));
            for (auto it = use.globalMatch(css); it.hasNext();) { const QString v = it.next().captured(1); QVERIFY2(defined.contains(v), qPrintable("ui-tokens.css uses undefined --" + v)); }
            QVERIFY(css.contains("  --info: #0a84ff;\n"));
            QVERIFY(css.contains("  --role-stop: var(--onair);\n"));
            QVERIFY(css.contains("  --role-info: var(--info);\n"));
            const QJsonObject sizes = B.value("sizes").toObject();
            for (auto it = sizes.begin(); it != sizes.end(); ++it)
                QVERIFY2(css.contains(QStringLiteral("  --btn-h-%1: %2px;\n").arg(it.key()).arg(it.value().toObject().value("h").toInt())), qPrintable("size " + it.key()));
            QVERIFY(css.contains("  --btn-tracking-xs: 0.6px;\n"));
            QVERIFY(css.contains("  --btn-arm-ms: 3000ms;\n"));
            QVERIFY(css.contains("  --btn-focus-color: var(--accent);\n"));
            QVERIFY(css.contains("  --btn-lit-a: 0.3;\n"));
            QVERIFY(css.contains("  --led-red: var(--onair);\n"));
            QVERIFY(css.contains("  --led-blink-fast: 250ms;\n"));
            QVERIFY(!css.contains("CONFIRM"));
        }
    }

    void sourceScannerSelfTest() {
        const QSet<QString> known = {"close", "play"};
        const QString qml = QStringLiteral("Item {\n  IconButton { icon: \"close\"; tip: \"Remove\" }\n  IconButton { icon: \"nope\"\n"
                                           "    ToolTip.text: \"x }\" }\n  PButton { icon: \"play\"; text: \"tip: no\" }\n"
                                           "  Icon { name: \"missing-one\"; size: 13 }\n  Text { text: \"\\u25B6\"; property string icon: \"\\u25B6\" }\n"
                                           "  PButton { icon.name: \"play\" }\n  IconButton { icon.name: \"gone-two\"; tip: \"x\" }\n}\n");
        const QStringList q = scanQml("t.qml", qml, known);
        QVERIFY2(q.size() == 4, qPrintable(q.join(" | ")));
        QVERIFY(q.join('|').contains("t.qml:9: icon \"gone-two\""));
        QVERIFY(q.join('|').contains("t.qml:3: icon \"nope\""));
        QVERIFY(q.join('|').contains("t.qml:3: IconButton without tip"));
        QVERIFY(q.join('|').contains("t.qml:6: icon \"missing-one\""));
        const QString js = QStringLiteral("x = icon(\"play\") + icon('gone') + `<button data-icon=\"key\">` + `<b data-icon=\"${b.icon}\">`;\n"
                                          "btn({ icon: \"close\", iconRight: \"caret-down\" });\n");
        const QStringList w = scanWeb("t.js", js, known);
        QVERIFY2(w.size() == 3, qPrintable(w.join(" | ")));
        QVERIFY(w.join('|').contains("\"gone\"") && w.join('|').contains("\"key\"") && w.join('|').contains("t.js:2: icon \"caret-down\""));
    }

    void sourcesUseKnownIcons() {
        QStringList problems;
        int files = 0;
        const QString qmlRoot = kSrc + "/Play_Play_Native/qml";
        for (QDirIterator it(qmlRoot, {"*.qml", "*.js"}, QDir::Files, QDirIterator::Subdirectories); it.hasNext();) {
            const QString path = it.next(), rel = path.mid(qmlRoot.size() + 1);
            if (rel.startsWith(u"pages/") || rel.startsWith(u"components/") || rel == u"PlayPlay/Buttons/spec.js") continue;   // dead legacy QML; generated
            problems << scanQml("qml/" + rel, QString::fromUtf8(readFile(path)), drawn);
            ++files;
        }
        const QDir web(kSrc + "/web");
        for (const QString &f : web.entryList({"*.js", "*.html"}, QDir::Files)) {
            if (f == u"ui-spec.js") continue;
            problems << scanWeb("web/" + f, QString::fromUtf8(readFile(web.filePath(f))), drawn);
            ++files;
        }
        QVERIFY2(files > 50, "the source scan found too few files");
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join('\n')));
    }
};

QTEST_GUILESS_MAIN(TestUiSpec)
#include "test_uispec.moc"
