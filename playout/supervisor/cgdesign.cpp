// cgdesign — the pure half of the CG Designer (docs/cg-designer.md): ids, the model prompt and reply parser, page
// normalisation and param derivation. No store, no network.
#include "cgdesign.h"

#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>

namespace cgdesign {

namespace {

using RX = QRegularExpression;
const RX::PatternOptions CI = RX::CaseInsensitiveOption;
const RX::PatternOptions CID = RX::CaseInsensitiveOption | RX::DotMatchesEverythingOption;

QString attr(const QString& attrs, const QString& name) {
    const RX re("(?:^|\\s)" + RX::escape(name) + "\\s*=\\s*(?:\"([^\"]*)\"|'([^']*)'|([^\\s>]+))", CI);
    const auto m = re.match(attrs);
    if (!m.hasMatch()) return QString();
    for (int i = 1; i <= 3; ++i) if (m.capturedStart(i) >= 0) return m.captured(i);
    return QString();
}
bool hasAttr(const QString& attrs, const QString& name) {
    return RX("(?:^|\\s)" + RX::escape(name) + "(?:\\s*=|\\s|$)", CI).match(attrs).hasMatch();
}
QString decodeEntities(QString s) {
    s.replace("&nbsp;", " ").replace("&lt;", "<").replace("&gt;", ">").replace("&quot;", "\"").replace("&#39;", "'").replace("&apos;", "'");
    static const RX num("&#(x[0-9a-fA-F]+|\\d+);");
    QString out; int last = 0;
    for (auto it = num.globalMatch(s); it.hasNext();) {
        const auto m = it.next();
        out += s.mid(last, m.capturedStart() - last);
        const QString v = m.captured(1);
        bool ok = false; const uint cp = v.startsWith('x') ? v.mid(1).toUInt(&ok, 16) : v.toUInt(&ok);
        out += ok && cp > 0 && cp < 0x110000 ? QString::fromUcs4(reinterpret_cast<const char32_t*>(&cp), 1) : m.captured(0);
        last = m.capturedEnd();
    }
    out += s.mid(last);
    return out.replace("&amp;", "&");
}
bool isColorValue(const QString& v) {
    static const RX c("^(#[0-9a-fA-F]{3,8}|(rgb|rgba|hsl|hsla|oklch|oklab|color-mix)\\(.*\\)|transparent|white|black|red|green|blue|yellow|orange|gold|navy|gray|grey|silver|crimson|teal|purple)$", CI);
    return c.match(v.trimmed()).hasMatch();
}
QJsonObject metaFor(const QJsonArray& meta, const QString& name) {
    for (const QJsonValue& v : meta) if (v.toObject().value("name").toString() == name) return v.toObject();
    return QJsonObject();
}
int bodyStart(const QString& html) {                         // index just after <body ...>, or -1
    const auto m = RX("<body\\b[^>]*>", CI).match(html);
    return m.hasMatch() ? m.capturedEnd() : -1;
}

} // namespace

// =====================================================================================================
// ids
// =====================================================================================================
QString slugify(const QString& name) {
    QString s = name.normalized(QString::NormalizationForm_KD).toLower(), out;
    bool us = false;
    for (const QChar c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) { out += c; us = false; }
        else if (c.category() == QChar::Mark_NonSpacing) continue;
        else if (!us && !out.isEmpty()) { out += '_'; us = true; }
    }
    while (out.endsWith('_')) out.chop(1);
    return out.left(40);
}

QString makeId(const QString& name, const QStringList& taken) {
    QString slug = slugify(name);
    if (slug.isEmpty()) slug = "graphic";
    while (slug.endsWith('_')) slug.chop(1);
    const QString base = "cgd_" + slug;
    if (!taken.contains(base)) return base;
    for (int n = 2; n < 10000; ++n) { const QString id = base.left(44) + "_" + QString::number(n); if (!taken.contains(id)) return id; }
    return base + "_x";
}

bool validId(const QString& id) { static const RX re("^cgd_[a-z0-9_]{1,44}$"); return re.match(id).hasMatch(); }

QString humanize(const QString& name) {
    QString s;
    for (int i = 0; i < name.size(); ++i) {
        const QChar c = name[i];
        if (c == '_' || c == '-') { s += ' '; continue; }
        if (c.isUpper() && i > 0 && !name[i - 1].isUpper() && name[i - 1] != '_' && name[i - 1] != '-') s += ' ';
        if (c.isDigit() && i > 0 && !name[i - 1].isDigit()) s += ' ';
        s += c.toLower();
    }
    s = s.simplified();
    if (!s.isEmpty()) s[0] = s[0].toUpper();
    return s;
}

QStringList categories() { return {"lower-thirds", "bugs", "tickers", "fullscreen", "news", "sports", "weather", "elections", "social", "promos", "advertising", "custom"}; }
QStringList roles() { return {"lower_third", "bug", "ticker", "secondary", "legal_id", "emergency"}; }

// =====================================================================================================
// the model's reply
// =====================================================================================================
Reply parseModelReply(const QString& contentIn) {
    Reply r;
    const QString content = QString(contentIn).replace("\r\n", "\n");
    if (content.trimmed().isEmpty()) { r.error = "the model returned an empty answer"; return r; }

    // the page: a ```html fence, else a bare document
    int hs = -1, he = -1;
    {
        static const RX fence("```\\s*html[^\\n]*\\n", CI);
        const auto m = fence.match(content);
        if (m.hasMatch()) {
            hs = m.capturedEnd();
            const int close = content.indexOf(RX("\\n\\s*```"), hs);
            he = close >= 0 ? close : content.size();
        } else {
            static const RX doc("<!DOCTYPE\\s+html|<html\\b", CI);
            const auto d = doc.match(content);
            if (d.hasMatch()) {
                hs = d.capturedStart();
                const int end = content.lastIndexOf(RX("</html\\s*>", CI));
                he = end >= 0 ? content.indexOf('>', end) + 1 : content.size();
            }
        }
    }
    if (hs < 0) { r.error = "the answer holds no HTML page"; return r; }
    r.html = content.mid(hs, he - hs).trimmed();
    if (!RX("</html\\s*>|</body\\s*>", CI).match(r.html).hasMatch()) r.truncated = true;
    if (r.truncated) { r.error = "the page was cut off before </body> (the model ran out of output tokens)"; }
    if (!RX("<[a-zA-Z]", CI).match(r.html).hasMatch()) { r.error = "the answer holds no HTML page"; return r; }

    // the metadata: a ```json fence, else the first {...} before the page
    {
        static const RX jf("```\\s*json[^\\n]*\\n(.*?)\\n\\s*```", CID);
        const auto m = jf.match(content);
        QString js;
        if (m.hasMatch()) js = m.captured(1);
        else {
            const QString before = content.left(hs);
            const int a = before.indexOf('{'), b = before.lastIndexOf('}');
            if (a >= 0 && b > a) js = before.mid(a, b - a + 1);
        }
        if (!js.isEmpty()) {
            QJsonParseError pe; const QJsonDocument d = QJsonDocument::fromJson(js.trimmed().toUtf8(), &pe);
            if (d.isObject()) r.meta = d.object();
        }
    }
    return r;
}

// =====================================================================================================
// the page
// =====================================================================================================
QString normalizeHtml(const QString& in, QStringList* warnings) {
    QStringList w;
    QString h = in;
    h.remove(QChar(0xFEFF));

    // editor leftovers
    h.remove(RX("<base\\b[^>]*data-cge-base[^>]*>\\s*", CI));
    h.remove(RX("\\s+data-cge(?:-[a-z]+)?(?:\\s*=\\s*(?:\"[^\"]*\"|'[^']*'|[^\\s>]+))?", CI));
    h.remove(RX("\\s+contenteditable(?:\\s*=\\s*(?:\"[^\"]*\"|'[^']*'|[^\\s>]+))?", CI));
    h.remove(RX("\\s+spellcheck\\s*=\\s*\"false\"", CI));

    // nothing loaded off the machine
    {
        static const RX ext("<script\\b[^>]*\\bsrc\\s*=\\s*[\"']?\\s*(?:https?:)?//([^\"'\\s>]*)[^>]*>\\s*</script\\s*>\\s*", CI);
        for (auto it = ext.globalMatch(h); it.hasNext();) w << "removed an external script (" + it.next().captured(1).left(80) + "): the playout machine may be offline";
        h.remove(ext);
        static const RX link("<link\\b[^>]*\\bhref\\s*=\\s*[\"']?\\s*(?:https?:)?//([^\"'\\s>]*)[^>]*>\\s*", CI);
        for (auto it = link.globalMatch(h); it.hasNext();) w << "removed an external stylesheet / font link (" + it.next().captured(1).left(80) + "): system fonts are used instead";
        h.remove(link);
        static const RX imp("@import\\s+(?:url\\()?\\s*[\"']?\\s*(?:https?:)?//([^\"')\\s;]*)[^;\\n]*;?", CI);
        for (auto it = imp.globalMatch(h); it.hasNext();) w << "removed a CSS @import of " + it.next().captured(1).left(80);
        h.remove(imp);
        static const RX remoteImg("(?:src\\s*=\\s*[\"']|url\\(\\s*[\"']?)(https?://[^\"')\\s]+)", CI);
        QSet<QString> seen;
        for (auto it = remoteImg.globalMatch(h); it.hasNext();) { const QString u = it.next().captured(1).left(80); if (!seen.contains(u)) { seen.insert(u); w << "references a remote file (" + u + "): it will be missing when the playout machine is offline"; } }
    }

    // document skeleton
    if (!RX("<body\\b", CI).match(h).hasMatch()) {
        if (RX("<html\\b", CI).match(h).hasMatch()) {
            const auto he = RX("</head\\s*>", CI).match(h);
            int at = he.hasMatch() ? he.capturedEnd() : RX("<html\\b[^>]*>", CI).match(h).capturedEnd();
            h.insert(at, "\n<body>\n");
            const auto ce = RX("</html\\s*>", CI).match(h);
            if (ce.hasMatch()) h.insert(ce.capturedStart(), "\n</body>\n"); else h += "\n</body>\n</html>\n";
        } else {
            // a fragment: keep <style> blocks in the head, the rest in the body
            QString styles;
            static const RX st("<style\\b[^>]*>.*?</style\\s*>", CID);
            for (auto it = st.globalMatch(h); it.hasNext();) styles += it.next().captured(0) + "\n";
            QString bodyPart = h; bodyPart.remove(st);
            bodyPart.remove(RX("<!DOCTYPE[^>]*>", CI));
            bodyPart.remove(RX("</?head\\b[^>]*>", CI));
            h = "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n<meta charset=\"UTF-8\">\n" + styles + "</head>\n<body>\n" + bodyPart.trimmed() + "\n</body>\n</html>\n";
        }
    }
    if (!RX("^\\s*<!DOCTYPE", CI).match(h).hasMatch()) { h.remove(RX("<!DOCTYPE[^>]*>\\s*", CI)); h.prepend("<!DOCTYPE html>\n"); }
    if (!RX("<head\\b", CI).match(h).hasMatch()) {
        const auto m = RX("<html\\b[^>]*>", CI).match(h);
        if (m.hasMatch()) h.insert(m.capturedEnd(), "\n<head>\n<meta charset=\"UTF-8\">\n</head>");
    }
    if (!RX("<meta\\b[^>]*charset", CI).match(h).hasMatch()) {
        const auto m = RX("<head\\b[^>]*>", CI).match(h);
        if (m.hasMatch()) h.insert(m.capturedEnd(), "\n<meta charset=\"UTF-8\">");
    }
    // the 1920x1080 transparent frame (first in the head, so the page's own CSS still wins)
    if (!h.contains("data-cg-frame") && !RX("width\\s*:\\s*1920px", CI).match(h).hasMatch()) {
        const auto m = RX("<head\\b[^>]*>", CI).match(h);
        if (m.hasMatch()) h.insert(m.capturedEnd(), "\n<style data-cg-frame>html,body{margin:0;padding:0;width:1920px;height:1080px;overflow:hidden;background:transparent}</style>");
    }

    // the root the runtime animates
    const bool ownInit = h.contains("CGAnimations.init") || RX("\\bdata-cg-auto\\b", CI).match(h).hasMatch();
    if (!RX("\\bdata-cg-root\\b", CI).match(h).hasMatch() && !ownInit) {
        const int bs = bodyStart(h);
        if (bs >= 0) {
            static const RX tag("<([a-zA-Z][a-zA-Z0-9-]*)\\b");
            int pos = bs;
            while (true) {
                const auto m = tag.match(h, pos);
                if (!m.hasMatch()) break;
                const QString t = m.captured(1).toLower();
                if (t == "script" || t == "style" || t == "template" || t == "noscript") {
                    const int close = h.indexOf(RX("</" + t + "\\s*>", CI), m.capturedEnd());
                    if (close < 0) break;
                    pos = close + 2;
                    continue;
                }
                if (t == "link" || t == "meta" || t == "br") { const int gt = h.indexOf('>', m.capturedEnd()); pos = gt >= 0 ? gt + 1 : m.capturedEnd(); continue; }
                h.insert(m.capturedEnd(), " data-cg-root");
                w << "no element had data-cg-root: the first element in the body (<" + t + ">) now carries it";
                break;
            }
        }
    }

    // the runtime scripts
    const bool hasAnim = h.contains("js/cg-animations.js"), hasFields = h.contains("js/cg-fields.js");
    if (!hasAnim || !hasFields) {
        QString add;
        if (!hasAnim) add += "<script src=\"js/cg-animations.js\"></script>\n";
        if (!hasFields) add += "<script src=\"js/cg-fields.js\"></script>\n";
        int at = -1;
        if (hasAnim) {                                                      // cg-fields right after cg-animations
            const auto m = RX("<script\\b[^>]*js/cg-animations\\.js[^>]*>\\s*</script\\s*>", CI).match(h);
            if (m.hasMatch()) { at = m.capturedEnd(); add.prepend("\n"); }
        }
        if (at < 0) {                                                       // before the first script in the body, else before </body>
            const int bs = bodyStart(h);
            const auto sm = RX("<script\\b", CI).match(h, qMax(0, bs));
            const auto be = RX("</body\\s*>", CI).match(h);
            at = bs >= 0 && sm.hasMatch() && (!be.hasMatch() || sm.capturedStart() < be.capturedStart()) ? sm.capturedStart() : (be.hasMatch() ? be.capturedStart() : h.size());
        }
        h.insert(at, add);
    }
    if (warnings) *warnings << w;
    return h;
}

QJsonArray deriveParams(const QString& html, const QJsonArray& metaParams) {
    QJsonArray out;
    QSet<QString> seen;
    // data-field elements, document order
    static const RX el("<([a-zA-Z][a-zA-Z0-9-]*)((?:\\s+[^\\s>=]+(?:\\s*=\\s*(?:\"[^\"]*\"|'[^']*'|[^\\s>]+))?)*)\\s*/?>", CI);
    // only look inside the body (the head has no fields, and <script> contents must not be read as markup)
    QString scan = html;
    scan.replace(RX("<script\\b[^>]*>.*?</script\\s*>", CID), "<script></script>");
    scan.replace(RX("<style\\b[^>]*>.*?</style\\s*>", CID), "<style></style>");
    for (auto it = el.globalMatch(scan); it.hasNext();) {
        const auto m = it.next();
        const QString attrs = m.captured(2);
        if (!attrs.contains("data-field", Qt::CaseInsensitive)) continue;
        const QString name = attr(attrs, "data-field").trimmed();
        if (name.isEmpty() || seen.contains(name)) continue;
        seen.insert(name);
        const QString tag = m.captured(1).toLower();
        QString type = attr(attrs, "data-type").toLower();
        if (tag == "img") type = "image";
        if (type != "textarea" && type != "image" && type != "number" && type != "color") type = "text";
        QString def;
        if (type == "image") def = attr(attrs, "src");
        else {
            const int start = m.capturedEnd();
            // the element's text: up to its closing tag, markup stripped (<br> -> newline)
            const int close = scan.indexOf(RX("</" + RX::escape(tag) + "\\s*>", CI), start);
            QString inner = scan.mid(start, close >= 0 ? qMin(close - start, 4000) : 0);
            inner.replace(RX("<br\\s*/?>", CI), "\n");
            inner.remove(RX("<[^>]*>"));
            def = decodeEntities(inner);
            if (type == "textarea") { QStringList lines = def.split('\n'); for (QString& l : lines) l = l.simplified(); while (!lines.isEmpty() && lines.first().isEmpty()) lines.removeFirst(); while (!lines.isEmpty() && lines.last().isEmpty()) lines.removeLast(); def = lines.join('\n'); }
            else def = def.simplified();
        }
        const QJsonObject mp = metaFor(metaParams, name);
        QJsonObject p;
        p["name"] = name;
        p["label"] = !attr(attrs, "data-label").trimmed().isEmpty() ? decodeEntities(attr(attrs, "data-label").trimmed()) : mp.value("label").toString(humanize(name));
        p["type"] = type;
        p["default"] = def.isEmpty() ? mp.value("default").toString() : def;
        if (mp.value("required").toBool()) p["required"] = true;
        p["source"] = "field";
        out.append(p);
    }
    // :root custom properties
    static const RX rootBlock(":root\\s*\\{([^}]*)\\}", CI);
    static const RX decl("--([A-Za-z_][A-Za-z0-9_-]*)\\s*:\\s*([^;]+);?");
    QString styles;
    for (auto it = RX("<style\\b[^>]*>(.*?)</style\\s*>", CID).globalMatch(html); it.hasNext();) styles += it.next().captured(1) + "\n";
    styles.remove(RX("/\\*.*?\\*/", RX::DotMatchesEverythingOption));
    for (auto it = rootBlock.globalMatch(styles); it.hasNext();) {
        const QString body = it.next().captured(1);
        for (auto d = decl.globalMatch(body); d.hasNext();) {
            const auto dm = d.next();
            const QString name = dm.captured(1), value = dm.captured(2).trimmed();
            if (seen.contains(name)) continue;
            seen.insert(name);
            const QJsonObject mp = metaFor(metaParams, name);
            QJsonObject p;
            p["name"] = name;
            p["label"] = mp.value("label").toString(humanize(name));
            p["type"] = isColorValue(value) ? "color" : "text";
            p["default"] = value;
            p["source"] = "var";
            out.append(p);
        }
    }
    // model params only the page's own script reads
    for (const QJsonValue& v : metaParams) {
        QJsonObject p = v.toObject();
        const QString name = p.value("name").toString();
        if (name.isEmpty() || seen.contains(name)) continue;
        if (!RX("CGFields\\.get\\(\\s*['\"]" + RX::escape(name) + "['\"]").match(html).hasMatch()) continue;
        seen.insert(name);
        if (p.value("label").toString().isEmpty()) p["label"] = humanize(name);
        QString t = p.value("type").toString("text");
        if (t != "textarea" && t != "image" && t != "number" && t != "color") t = "text";
        p["type"] = t;
        p["source"] = "script";
        out.append(p);
    }
    return out;
}

QJsonArray textFields(const QJsonArray& params) {
    QJsonArray a;
    for (const QJsonValue& v : params) {
        const QJsonObject p = v.toObject();
        const QString t = p.value("type").toString();
        if ((t == "text" || t == "textarea") && p.value("source").toString() != "var") a.append(p.value("name").toString());
        if (a.size() == 3) break;
    }
    return a;
}

// =====================================================================================================
// the prompt
// =====================================================================================================
QString systemPrompt(const QString& houseStyle) {
    QString s = QStringLiteral(
R"P(You are a senior broadcast motion-graphics designer building on-air CG (character generator) graphics for a local TV station's playout system, PLAY PLAY. Each graphic is ONE self-contained HTML page. A headless Chromium renders it at exactly 1920x1080 over live video and the station keys it by its alpha channel: every pixel you do not paint is see-through video.

THE TEMPLATE CONTRACT (the playout system depends on every rule)
1. Frame: start with <!DOCTYPE html>. html and body: width 1920px, height 1080px, margin 0, overflow hidden, background transparent. Never fill the whole frame unless the brief asks for a full-screen graphic (slate, title card, full-screen panel).
2. Root: put everything that appears and disappears inside ONE wrapper element with the attribute data-cg-root (usually a full-frame position:absolute; inset:0 div). The runtime plays the operator's chosen in/out animation on that wrapper, so do not give the wrapper an opacity:0 start state or an entrance animation of its own. Lay elements out with position:absolute and px units inside the 1920x1080 frame.
3. Operator text: every piece of text an operator may change carries data-field="<camelCaseName>" and data-label="<Readable label>", and holds realistic sample text as its content (that content is the default). Use specific names (anchorName, anchorTitle, headline, location, homeScore, teamA), never the reserved names text, text1, text2 or text3 (the playout system uses those for its generic Text 1/2/3 inputs). Fixed decoration (a "LIVE" tag, a station slogan the brief gives) needs no data-field unless an operator would plausibly change it.
4. Multi-line text: add data-type="textarea" and style it with white-space: pre-line. Optional lines that may be blank (a subtitle, a second name): add data-hide-empty, and make the layout still look right when they are hidden.
5. Images (logo, headshot, sponsor): <img data-field="logo" data-label="Logo" data-type="image" src="">, sized with explicit width/height or max-height plus object-fit: contain. Leave src empty or use a small inline SVG data URI placeholder; an empty image is hidden, so the design must still look complete without it.
6. Theme: put the colours (and any size an operator may want to tune) in CSS custom properties on :root, e.g. --accent: #d7262d; --plate: #0b1e3f; --ink: #ffffff; and use them through var(). Each variable becomes an operator setting named after it (accent, plate, ink), so name them in camelCase, never the same as a data-field and never text / text1-3, and give 2 to 5 of them. Derived shades: use color-mix() with the variables so a colour change re-themes everything.
7. Scripts: end the body with exactly
   <script src="js/cg-animations.js"></script>
   <script src="js/cg-fields.js"></script>
   They fill the fields and variables from the URL, apply live updates and run the in/out animation. Do NOT parse the URL yourself and do not define window.update, window.CGAnim or window.CGFields. Only when the graphic needs behaviour (a running clock, a countdown, a crawling ticker, rotating items) add ONE inline <script> after those two: read values with CGFields.get('name', fallback) and react to operator edits with CGFields.onChange(function (values) { ... }). Keep it small: requestAnimationFrame or timers no faster than 30 per second, no network calls, no console noise.
8. Self-contained: no external scripts, stylesheets, web fonts or remote images (the playout machine may be offline). Fonts: system stacks only, e.g. "Segoe UI", "Helvetica Neue", Arial, sans-serif; display faces "Bahnschrift", "Segoe UI Black", "Arial Black", Impact, Georgia. Shapes and icons: CSS or inline SVG.

BROADCAST DESIGN RULES
- Safe areas: all text inside title-safe (96px from the left and right edges, 54px from top and bottom); all plates inside action-safe (48px / 27px). Lower thirds sit with their main line around y = 840-960 and start at x = 96-160; corner bugs sit inside title-safe; tickers run along the bottom inside action-safe.
- Legibility at a glance on a TV across the room: main lines 44-64px, secondary lines 28-36px, nothing under 24px; weight 600+ for names and headlines; tight but not cramped letter-spacing. Put text on a solid or gradient plate (or at least a strong shadow): the video behind it can be anything. Text-to-plate contrast at least 4.5:1.
- Broadcast-legal colour: no pure #FFFFFF over large filled areas (use #F2F4F7 or similar for big fills; white text is fine), no large fully saturated pure red or blue fills.
- Restraint and craft: one strong idea, 2-3 colours plus neutrals, aligned edges, consistent corner radii, spacing on an 8px grid, subtle depth (a soft shadow under plates, a thin highlight line). It must look like a network-quality news/sports package, not a web page: no web buttons, no scrollbars, no card grids.
- Motion: ambient motion inside the root (a shine sweep, a slow gradient drift, a pulsing LIVE dot) via CSS @keyframes is welcome; staggered build-ins on child elements are fine if they end visible (animation-fill-mode: both). Nothing may flash more than 3 times a second. Every frame must look complete as a still.
- Text must fit: size plates for the sample text plus about 30% longer real values. Prefer plates that grow with their text (display:inline-flex / width:max-content) or single lines with white-space:nowrap; overflow:hidden; text-overflow:ellipsis inside a fixed width. Never let text clip mid-glyph or wrap into a plate that does not grow.

OUTPUT FORMAT (exactly these two fenced blocks, nothing before, between or after them)
```json
{"name": "Short Title-Case Name", "description": "One sentence: what it is and when to use it.", "category": "%CATEGORIES%", "role": "%ROLES%", "params": [{"name": "anchorName", "label": "Anchor name", "type": "text", "default": "Jane Smith"}]}
```
```html
<!DOCTYPE html>
<html lang="en">
... the complete page ...
</html>
```
category: pick one of the listed values. role: lower_third (name straps, captions, info bars in the lower third), bug (corner logo, clock, LIVE tag), ticker (crawls, bottom bars), secondary (full-screen and anything else), legal_id (station identification), emergency (alerts). params lists every data-field and every :root variable with its type (text, textarea, image or color) and default, plus any value only your inline script reads.)P");
    s.replace("%CATEGORIES%", categories().join(" | "));
    s.replace("%ROLES%", roles().join(" | "));
    const QString hs = houseStyle.trimmed().left(4000);
    if (!hs.isEmpty())
        s += "\n\nStation house style (the station's own rules: follow them unless the brief says otherwise; the contract above always wins)\n" + hs;
    return s;
}

QJsonArray userContent(const Request& r) {
    QString t;
    const bool refine = r.mode == "refine" && !r.baseHtml.trimmed().isEmpty();
    if (refine) {
        t += "REFINE the current graphic below. Apply the instruction and return the WHOLE page in the output format. Change only what the "
             "instruction asks; keep the layout, data-field names, data-labels, :root variable names and everything else as they are unless "
             "the instruction says otherwise.\n\nINSTRUCTION:\n" + r.prompt.trimmed() + "\n\nCURRENT GRAPHIC:\n```html\n" + r.baseHtml.left(60000) + "\n```\n";
    } else {
        t += "BRIEF:\n" + r.prompt.trimmed() + "\n";
    }
    if (!r.kind.trimmed().isEmpty()) t += "\nKind of graphic: " + r.kind.trimmed() + "\n";
    if (!r.brand.isEmpty()) {
        QStringList b;
        const QJsonValue c = r.brand.value("colors");
        if (c.isArray()) { QStringList cs; for (const QJsonValue& v : c.toArray()) cs << v.toString(); if (!cs.isEmpty()) b << "colours " + cs.join(", "); }
        else if (!c.toString().trimmed().isEmpty()) b << "colours " + c.toString().trimmed();
        if (!r.brand.value("font").toString().trimmed().isEmpty()) b << "font " + r.brand.value("font").toString().trimmed() + " (fall back to a system stack)";
        if (!r.brand.value("notes").toString().trimmed().isEmpty()) b << r.brand.value("notes").toString().trimmed();
        if (!b.isEmpty()) t += "\nStation brand: " + b.join("; ") + "\n";
    }
    int n = 0, img = 0;
    for (const Example& e : r.examples) {
        if (e.kind == "image") continue;
        if (e.html.trimmed().isEmpty()) continue;
        ++n;
        t += QString("\nREFERENCE %1%2: match its visual language (palette, typography, shapes, proportions, motion), not necessarily its content or layout%3. "
                     "It may predate the contract (parse the URL itself, lack data-field); your page must follow the contract regardless.\n```html\n%4\n```\n")
                 .arg(QString::number(n), e.name.isEmpty() ? QString() : " (" + e.name + ")", e.note.trimmed().isEmpty() ? QString() : ". Note: " + e.note.trimmed(),
                      e.html.left(14000));                      // one pass: a "%4" inside the example is not a placeholder
    }
    for (const Example& e : r.examples) {
        if (e.kind != "image" || e.dataUrl.isEmpty()) continue;
        ++img;
        t += QString("\nREFERENCE IMAGE %1 is attached%2. Match its look (palette, typography, shapes, layout feel). Do not trace third-party logos or "
                     "trademarks into the artwork (give logos an image field instead) and never reproduce a real person's likeness.\n")
                 .arg(QString::number(img), e.note.trimmed().isEmpty() ? QString() : ": " + e.note.trimmed());
    }
    if (r.variations > 1)
        t += QString("\nThis is variation %1 of %2: take a clearly different design direction from the other variations (layout, shape language, "
                     "typography, motion) while meeting the same brief.\n").arg(r.variation).arg(r.variations);
    t += "\nReply in the output format only.";
    QJsonArray parts;
    parts.append(QJsonObject{{"type", "text"}, {"text", t}});
    for (const Example& e : r.examples)
        if (e.kind == "image" && !e.dataUrl.isEmpty()) parts.append(QJsonObject{{"type", "image_url"}, {"image_url", QJsonObject{{"url", e.dataUrl}}}});
    return parts;
}

QJsonObject chatBody(const Request& r, const QString& model, double temperature, int maxTokens) {
    QJsonObject b;
    b["model"] = model;
    b["temperature"] = temperature;
    b["max_tokens"] = maxTokens;
    b["messages"] = QJsonArray{QJsonObject{{"role", "system"}, {"content", systemPrompt(r.houseStyle)}}, QJsonObject{{"role", "user"}, {"content", userContent(r)}}};
    return b;
}

QJsonObject redactImages(const QJsonObject& body) {
    QJsonObject b = body;
    QJsonArray msgs = b.value("messages").toArray();
    int n = 0;
    for (int i = 0; i < msgs.size(); ++i) {
        QJsonObject m = msgs[i].toObject();
        if (!m.value("content").isArray()) continue;
        QJsonArray parts = m.value("content").toArray();
        for (int k = 0; k < parts.size(); ++k) {
            QJsonObject p = parts[k].toObject();
            if (p.value("type").toString() != "image_url") continue;
            QJsonObject iu = p.value("image_url").toObject();
            iu["url"] = QString("<image %1: %2 bytes>").arg(++n).arg(iu.value("url").toString().size());
            p["image_url"] = iu; parts[k] = p;
        }
        m["content"] = parts; msgs[i] = m;
    }
    b["messages"] = msgs;
    return b;
}

} // namespace cgdesign
