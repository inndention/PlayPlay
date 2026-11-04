// Headline News: story picking, page parsing, spoken-text normalisation, scene timing and the ffmpeg graph
// for the story-by-story render of the headlines recipe (see headlinenews.h, docs/spots.md).
#include "headlinenews.h"

#include <QDateTime>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <functional>

namespace headline {

namespace {
using RX = QRegularExpression;
constexpr RX::PatternOptions CI = RX::CaseInsensitiveOption;
constexpr RX::PatternOptions DOT = RX::DotMatchesEverythingOption | RX::CaseInsensitiveOption;

QString decodeEntitiesOnce(const QString& s) {
    static const QHash<QString, QString> named = {{"amp", "&"}, {"lt", "<"}, {"gt", ">"}, {"quot", "\""}, {"apos", "'"}, {"nbsp", " "},
        {"rsquo", "\u2019"}, {"lsquo", "\u2018"}, {"rdquo", "\u201d"}, {"ldquo", "\u201c"}, {"mdash", "\u2014"}, {"ndash", "\u2013"},
        {"hellip", "\u2026"}, {"deg", "\u00b0"}, {"copy", "\u00a9"}};
    static const RX rx("&(#x[0-9a-fA-F]+|#[0-9]+|[a-zA-Z]+);");
    QString out; int last = 0;
    auto it = rx.globalMatch(s);
    while (it.hasNext()) {
        const auto m = it.next(); out += s.mid(last, m.capturedStart() - last); last = m.capturedEnd();
        const QString e = m.captured(1);
        char32_t cp = 0;
        if (e.startsWith("#x", Qt::CaseInsensitive)) cp = char32_t(e.mid(2).toUInt(nullptr, 16));
        else if (e.startsWith('#')) cp = char32_t(e.mid(1).toUInt());
        if (cp > 0 && cp < 0x110000) out += QString::fromUcs4(&cp, 1);
        else if (named.contains(e.toLower())) out += named.value(e.toLower());
        else out += m.captured(0);
    }
    return out + s.mid(last);
}
QString decodeEntities(const QString& s) {
    const QString once = decodeEntitiesOnce(s);
    return once.contains('&') ? decodeEntitiesOnce(once) : once;      // feeds often double-encode ("&amp;#8217;")
}
/// Comparison key: lower-case letters and digits only.
QString key(const QString& s) { QString k; k.reserve(s.size()); for (const QChar c : s) if (c.isLetterOrNumber()) k += c.toLower(); return k; }
int words(const QString& s) { return s.split(RX("\\s+"), Qt::SkipEmptyParts).size(); }
QString n(int v) { return QString::number(v); }
QString sec(double s) { return QString::number(s, 'f', 6); }
} // namespace

// ---------------------------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------------------------
QString plainText(const QString& html) {
    static const RX cdata("<!\\[CDATA\\[(.*?)\\]\\]>", RX::DotMatchesEverythingOption);
    static const RX scripts("<(script|style)\\b[^>]*>.*?</\\1>", DOT);
    static const RX br("<br\\s*/?>", CI);
    static const RX tags("<[^>]+>");
    QString s = html;
    s.replace(cdata, "\\1"); s.remove(scripts); s.replace(br, " "); s.replace(tags, " ");
    return decodeEntities(s).simplified();
}

QString cleanTitle(const QString& in, const QString& siteName) {
    QString s = plainText(in);
    // leading emoji, bullets and dingbats: everything before the first letter, digit or opening quote
    int i = 0;
    while (i < s.size() && !(s[i].isLetterOrNumber() || s[i] == '"' || s[i] == '\'' || s[i] == '(' || s[i] == QChar(0x201C) || s[i] == QChar(0x2018))) ++i;
    s = s.mid(i);
    QString o; o.reserve(s.size());
    for (int k = 0; k < s.size(); ++k) {
        const QChar c = s[k];
        if (c.isHighSurrogate()) { ++k; continue; }                 // every non-BMP glyph (emoji)
        if (c.isLowSurrogate()) continue;
        const ushort u = c.unicode();
        if ((u >= 0x2600 && u <= 0x27BF) || (u >= 0x2B00 && u <= 0x2BFF) || u == 0xFE0F || u == 0x200D || u == 0x20E3) continue;
        o += c;
    }
    s = o;
    static const RX pipeLead("^([A-Z][A-Z ]{1,24}?)\\s*\\|\\s*");          // "UPDATE | A woman ..." -> "UPDATE: A woman ..."
    s.replace(pipeLead, "\\1: ");
    static const RX pipeMid("\\s+\\|\\s+");
    s.replace(pipeMid, " - ");
    if (!siteName.trimmed().isEmpty()) s.remove(RX("\\s*[-|\\x{2013}\\x{2014}]\\s*" + RX::escape(siteName.trimmed()) + "\\s*$", CI));
    return s.simplified();
}

bool isTruncated(const QString& text) {
    const QString t = text.trimmed();
    return t.endsWith("...") || t.endsWith(QChar(0x2026));
}

bool isJunk(const QString& title, const QString& text) {
    const QString t = cleanTitle(title);
    static const RX clock("^\\d{1,2}(:\\d{2})?\\s*([ap]\\.?\\s?m\\.?)?$", CI);
    if (t.isEmpty() || clock.match(t).hasMatch()) return true;
    const QString body = cleanTitle(plainText(text));
    const int extra = words(body) - (key(body).startsWith(key(t)) ? words(t) : 0);   // what the feed text adds beyond the title
    return words(t) < 5 && extra < 8;
}

bool isGenericImage(const QString& url) {
    if (url.trimmed().isEmpty()) return true;
    static const RX rx("(/branding/|/backdrop_system/|/logos?/|logo[_.-]|favicon|placeholder|apple-touch-icon|/og-default|default[_-]?(og|share|image|thumb))", CI);
    return rx.match(url).hasMatch();
}

QString placeFromUrl(const QString& url, const QStringList& tags) {
    static const RX rx("/(?:local|news|city|region)/([a-z0-9-]+)/(?:story|article|news|post)/", CI);
    const auto m = rx.match(QUrl(url).path());
    if (!m.hasMatch()) return QString();
    const QString slug = m.captured(1).toLower();
    const QString flat = key(slug);
    // a tag naming the place wins ("Martinsville"), unless it flattens a hyphenated name ("Winstonsalem" for winston-salem)
    for (const QString& t : tags) { const QString tt = t.trimmed(); if (key(tt) == flat && (!slug.contains('-') || tt.contains(' ') || tt.contains('-'))) return tt; }
    static const QSet<QString> hyphenated = {"winston-salem", "wilkes-barre", "sedro-woolley"};
    QStringList parts = slug.split('-', Qt::SkipEmptyParts);
    for (QString& p : parts) p = p.left(1).toUpper() + p.mid(1);
    return parts.join(hyphenated.contains(slug) ? "-" : " ");
}

QStringList sentences(const QString& text) {
    static const QSet<QString> abbr = {"jan", "feb", "mar", "apr", "jun", "jul", "aug", "sep", "sept", "oct", "nov", "dec",
        "mr", "mrs", "ms", "dr", "st", "ave", "rd", "blvd", "mt", "ft", "jr", "sr", "no", "co", "inc", "ltd", "corp",
        "gov", "sen", "rep", "lt", "sgt", "capt", "col", "gen", "det", "ofc", "cpl", "pvt", "prof", "rev", "hon", "vs",
        "etc", "approx", "dept", "est", "hwy", "pkwy", "univ", "atty", "supt", "cmdr"};
    static const QSet<QString> dayMonth = {"monday", "tuesday", "wednesday", "thursday", "friday", "saturday", "sunday",
        "january", "february", "march", "april", "may", "june", "july", "august", "september", "october", "november", "december",
        "jan", "feb", "mar", "apr", "jun", "jul", "aug", "sep", "sept", "oct", "nov", "dec", "today", "tonight", "tomorrow"};
    static const RX endRx("[.!?][\"'\\x{201D}\\x{2019})]*\\s+(?=[\"'\\x{201C}\\x{2018}(]?[A-Z0-9])");
    const QString s = text.simplified();
    QStringList out; int start = 0;
    auto it = endRx.globalMatch(s);
    while (it.hasNext()) {
        const auto m = it.next(); const int p = m.capturedStart();
        if (s[p] == '.') {
            int b = p; while (b > start && !s[b - 1].isSpace()) --b;
            QString w = s.mid(b, p - b).toLower();
            while (!w.isEmpty() && !w[0].isLetterOrNumber()) w.remove(0, 1);
            if (abbr.contains(w) || (w.size() == 1 && w[0].isLetter())) continue;           // "Oct. 23", "J. Smith"
            if (w.contains('.')) {                                                          // "a.m.", "p.m.", "U.S."
                QString next = s.mid(m.capturedEnd()).section(' ', 0, 0).toLower();
                while (!next.isEmpty() && !next.back().isLetter()) next.chop(1);
                if (w == "u.s" || w == "d.c" || dayMonth.contains(next)) continue;          // "3 p.m. Saturday", "U.S. Marshals"
            }
        }
        out << s.mid(start, m.capturedEnd() - start).trimmed();
        start = m.capturedEnd();
    }
    const QString rest = s.mid(start).trimmed();
    if (!rest.isEmpty()) out << rest;
    return out;
}

QString firstSentence(const QString& text, int maxWords) {
    const QStringList ss = sentences(text);
    if (ss.isEmpty()) return QString();
    QString f = ss.first();
    if (maxWords > 0 && words(f) > maxWords) {
        // a long sentence ends at the last clause break inside the limit (else at the limit), never mid-word
        QString cut = f.split(' ', Qt::SkipEmptyParts).mid(0, maxWords).join(' ');
        const int c = qMax(cut.lastIndexOf(", "), qMax(cut.lastIndexOf("; "), cut.lastIndexOf(": ")));
        if (c > cut.size() / 2) cut = cut.left(c);
        cut = cut.trimmed();
        while (!cut.isEmpty() && QString(",;:-\u2013\u2014").contains(cut.back())) cut.chop(1);
        f = cut.trimmed() + ".";
    }
    return f;
}

// ---------------------------------------------------------------------------------------------
// Story pages and picking
// ---------------------------------------------------------------------------------------------
QJsonObject parseStoryPage(const QString& html, const QString& pageUrl) {
    QJsonObject o; QJsonArray tags;
    static const RX metaRx("<meta\\b[^>]*>", CI);
    static const RX attrRx("\\b(property|name|itemprop|content)\\s*=\\s*(?:\"([^\"]*)\"|'([^']*)')", CI);
    auto it = metaRx.globalMatch(html);
    while (it.hasNext()) {
        const QString tag = it.next().captured(0); QString k, content; bool hasContent = false;
        auto at = attrRx.globalMatch(tag);
        while (at.hasNext()) {
            const auto a = at.next(); const QString name = a.captured(1).toLower(); const QString v = a.captured(2).isNull() ? a.captured(3) : a.captured(2);
            if (name == "content") { content = v; hasContent = true; } else if (k.isEmpty()) k = v.toLower();
        }
        if (k.isEmpty() || !hasContent) continue;
        content = decodeEntities(content).trimmed(); if (content.isEmpty()) continue;
        if ((k == "og:image" || k == "og:image:url" || k == "og:image:secure_url" || k == "twitter:image") && !o.contains("image")) o["image"] = QUrl(pageUrl).resolved(QUrl(content)).toString();
        else if (k == "og:title" && !o.contains("title")) o["title"] = content;
        else if ((k == "og:description" || k == "description" || k == "twitter:description") && !o.contains("description")) o["description"] = content;
        else if (k == "article:tag") tags.append(content);
        else if (k == "article:section" && !o.contains("section")) o["section"] = content;
        else if (k == "article:published_time" && !o.contains("publishedMs")) { const QDateTime t = QDateTime::fromString(content, Qt::ISODate); if (t.isValid()) o["publishedMs"] = double(t.toMSecsSinceEpoch()); }
    }
    o["tags"] = tags;
    // the post text: paragraphs of the first <article> (the story itself; related-story cards come after it)
    static const RX articleRx("<article\\b[^>]*>(.*?)</article>", DOT);
    static const RX noise("<(script|style|aside|figure|figcaption|form|nav|footer|ins|button)\\b[^>]*>.*?</\\1>", DOT);
    static const RX pRx("<p\\b[^>]*>(.*?)</p>", DOT);
    static const RX boiler("^(official sources?\\b|sources?:|the accompanying image|this image is|related\\b|share\\b|advertisement|sponsored|photo:|image:|credit:|copyright|\\x{00A9})", CI);
    QString body = articleRx.match(html).captured(1);
    body.remove(noise);
    QJsonArray paras;
    auto pit = pRx.globalMatch(body);
    while (pit.hasNext() && paras.size() < 12) {
        const QString t = plainText(pit.next().captured(1));
        if (words(t) < 4 || boiler.match(t).hasMatch()) continue;
        paras.append(t);
    }
    o["paragraphs"] = paras;
    return o;
}

QJsonObject mergeStory(const QJsonObject& item, const QJsonObject& page, const QString& siteName) {
    const QString link = item.value("link").toString().trimmed();
    QStringList tags; for (const QJsonValue& t : page.value("tags").toArray()) tags << t.toString();
    QStringList paras; for (const QJsonValue& p : page.value("paragraphs").toArray()) { const QString c = cleanTitle(p.toString()); if (!c.isEmpty()) paras << c; }
    const QString postText = paras.join(' ');
    QString title = cleanTitle(item.value("title").toString(), siteName);
    if (title.isEmpty()) title = cleanTitle(page.value("title").toString(), siteName);
    if (isTruncated(title) && !postText.isEmpty()) { const QString first = firstSentence(postText, 40); if (!first.isEmpty()) title = first; }   // a social post cut at ~100 characters
    // what the story says beyond the headline: og:description when complete, else the post text, else the feed text
    QString desc = cleanTitle(page.value("description").toString());
    if (desc.isEmpty() || isTruncated(desc)) desc = postText;
    if (desc.isEmpty()) desc = cleanTitle(plainText(item.value("text").toString()));
    const QString tk = key(title);
    QString summary;
    for (const QString& s : sentences(desc)) {
        const QString sk = key(s);
        if (sk.isEmpty() || sk.left(32) == tk.left(32) || tk.contains(sk) || sk.contains(tk)) continue;   // repeats the headline
        if (isTruncated(s) && words(s) < 6) continue;
        summary = s; break;
    }
    QString image = page.value("image").toString();
    if (isGenericImage(image)) image.clear();
    if (image.isEmpty() && !isGenericImage(item.value("image").toString())) image = item.value("image").toString();
    QString category = plainText(item.value("category").toString());
    if (category.isEmpty()) category = page.value("section").toString();
    double pub = item.value("pubMs").toDouble();
    if (pub <= 0) pub = page.value("publishedMs").toDouble();
    QJsonObject s;
    s["title"] = title; s["summary"] = summary; s["link"] = link; s["image"] = image;
    s["place"] = placeFromUrl(link, tags); s["category"] = category; s["pubMs"] = pub; s["source"] = item.value("source").toString();
    return s;
}

QJsonArray pickStories(const QJsonArray& stories, int n, bool preferImages) {
    QList<QJsonObject> uniq; QSet<QString> links, heads;
    for (const QJsonValue& v : stories) {
        const QJsonObject s = v.toObject(); const QString title = s.value("title").toString();
        if (title.trimmed().isEmpty()) continue;
        QString l = s.value("link").toString().trimmed().toLower(); while (l.endsWith('/')) l.chop(1);
        const QString h = key(title).left(48);
        if ((!l.isEmpty() && links.contains(l)) || heads.contains(h)) continue;
        if (!l.isEmpty()) links.insert(l);
        heads.insert(h); uniq << s;
    }
    const auto newestFirst = [](const QJsonObject& a, const QJsonObject& b) { return a.value("pubMs").toDouble() > b.value("pubMs").toDouble(); };
    std::stable_sort(uniq.begin(), uniq.end(), newestFirst);
    QList<QJsonObject> picked;
    if (preferImages) for (const QJsonObject& s : uniq) if (picked.size() < n && !s.value("image").toString().isEmpty()) picked << s;
    for (const QJsonObject& s : uniq) { if (picked.size() >= n) break; if (!picked.contains(s)) picked << s; }
    std::stable_sort(picked.begin(), picked.end(), newestFirst);
    QJsonArray out; for (const QJsonObject& s : picked) out.append(s);
    return out;
}

// ---------------------------------------------------------------------------------------------
// Speech
// ---------------------------------------------------------------------------------------------
QString speakable(const QString& in) {
    QString s = in;
    static const QList<QPair<RX, QString>> rules = {
        {RX("\\bJan\\.(?=\\s)"), "January"}, {RX("\\bFeb\\.(?=\\s)"), "February"}, {RX("\\bAug\\.(?=\\s)"), "August"},
        {RX("\\bSept?\\.(?=\\s)"), "September"}, {RX("\\bOct\\.(?=\\s)"), "October"}, {RX("\\bNov\\.(?=\\s)"), "November"},
        {RX("\\bDec\\.(?=\\s)"), "December"},
        {RX("\\bAve\\.(?=\\s*$)"), "Avenue."}, {RX("\\bAve\\b\\.?"), "Avenue"}, {RX("\\bBlvd\\b\\.?"), "Boulevard"},
        {RX("\\bHwy\\b\\.?"), "Highway"}, {RX("\\bPkwy\\b\\.?"), "Parkway"}, {RX("\\bMt\\.(?=\\s)"), "Mount"}, {RX("\\bFt\\.(?=\\s)"), "Fort"},
    };
    for (const auto& r : rules) s.replace(r.first, r.second);
    // street names: "231 W. First St." -> "231 West First Street." ("St." only after a capitalised word or an ordinal, so "St. Louis" stays)
    static const RX streetEnd("\\b([A-Z][a-z]+|\\d+(?:st|nd|rd|th))\\s+(St|Rd|Ln)\\.(?=\\s*$)");
    static const RX street("\\b([A-Z][a-z]+|\\d+(?:st|nd|rd|th))\\s+(St|Rd|Ln)\\b\\.?");
    static const QHash<QString, QString> streetWord = {{"St", "Street"}, {"Rd", "Road"}, {"Ln", "Lane"}};
    static const RX compass("\\b(\\d+)\\s+([NSEW])\\.?\\s+(?=[A-Z0-9])");
    static const QHash<QString, QString> compassWord = {{"N", "North"}, {"S", "South"}, {"E", "East"}, {"W", "West"}};
    auto expand = [](const QString& in, const RX& rx, const std::function<QString(const QRegularExpressionMatch&)>& f) {
        QString o; int last = 0; auto it = rx.globalMatch(in);
        while (it.hasNext()) { const auto m = it.next(); o += in.mid(last, m.capturedStart() - last) + f(m); last = m.capturedEnd(); }
        return o + in.mid(last);
    };
    s = expand(s, streetEnd, [&](const QRegularExpressionMatch& m) { return m.captured(1) + " " + streetWord.value(m.captured(2)) + "."; });
    s = expand(s, street, [&](const QRegularExpressionMatch& m) { return m.captured(1) + " " + streetWord.value(m.captured(2)); });
    s = expand(s, compass, [&](const QRegularExpressionMatch& m) { return m.captured(1) + " " + compassWord.value(m.captured(2)) + " "; });
    // street quadrants after a street word: "Luck Avenue SW" -> "Luck Avenue Southwest"
    static const RX quad("\\b(Avenue|Street|Road|Boulevard|Drive|Lane|Way|Place|Parkway|Highway|Circle|Court|Trail)\\s+(SW|NW|NE|SE)\\b");
    static const QHash<QString, QString> quadWord = {{"SW", "Southwest"}, {"NW", "Northwest"}, {"NE", "Northeast"}, {"SE", "Southeast"}};
    QString o; int last = 0; auto it = quad.globalMatch(s);
    while (it.hasNext()) { const auto m = it.next(); o += s.mid(last, m.capturedStart() - last) + m.captured(1) + " " + quadWord.value(m.captured(2)); last = m.capturedEnd(); }
    s = o + s.mid(last);
    // short number ranges: "Oct. 23-24", "2-3 p.m.", "21-14" (never phone numbers or longer runs)
    static const RX range("(?<![\\d:.-])(\\d{1,2})\\s?-\\s?(\\d{1,2})(?![\\d:-]|\\.\\d)");
    s.replace(range, "\\1 to \\2");
    return s.simplified();
}

QString spokenName(const QString& name) {
    QString o;
    for (int i = 0; i < name.size(); ++i) {
        const QChar c = name[i];
        if (i > 0 && c.isUpper() && name[i - 1].isLower()) o += ' ';
        o += c;
    }
    return o.simplified();
}

QString spokenSite(const QString& site, const QString& siteName) {
    QString host = site.trimmed();
    host.remove(RX("^[a-z]+://", CI)); host.remove(RX("^www\\.", CI)); host = host.section('/', 0, 0);
    QStringList parts = host.split('.', Qt::SkipEmptyParts);
    if (parts.isEmpty()) return spokenName(siteName);
    if (!siteName.isEmpty() && key(siteName) == key(parts.first())) parts[0] = spokenName(siteName);
    return parts.join(" dot ");
}

// ---------------------------------------------------------------------------------------------
// Scene timing + ffmpeg graph
// ---------------------------------------------------------------------------------------------
SceneTiming sceneTimings(const QVector<double>& wavSec, const QStringList& kinds, double fps, double xfadeSec, double leadSec, double tailSec, double introMinSec, double outroMinSec) {
    SceneTiming t; t.fps = fps > 0 ? fps : 30.0;
    const int K = wavSec.size();
    t.xfadeFrames = qMax(1, int(std::lround(xfadeSec * t.fps)));
    t.leadMs = int(std::lround(leadSec * 1000.0));
    for (int i = 0; i < K; ++i) {
        const QString kind = i < kinds.size() ? kinds[i] : QString("story");
        const double w = qMax(0.0, wavSec[i]);
        double s = leadSec + w + tailSec;
        if (kind == "intro") s = qMax(introMinSec, leadSec + w + 0.6);
        else if (kind == "outro") s = qMax(outroMinSec, leadSec + w + 1.4);
        t.slotFrames << qMax(int(std::ceil(s * t.fps - 1e-9)), t.xfadeFrames + 1);   // a scene always outlasts its transition
    }
    for (int i = 0; i < K; ++i) t.videoFrames << t.slotFrames[i] + (i + 1 < K ? t.xfadeFrames : 0);
    for (int f : t.slotFrames) t.totalFrames += f;
    return t;
}

QString storiesFilter(const SceneTiming& t, bool music, const QVector<bool>& animated) {
    const int K = t.videoFrames.size();
    if (K == 0) return QString();
    const double fps = t.fps;
    const QString fpsS = std::floor(fps) == fps ? n(int(fps)) : QString::number(fps, 'f', 3);
    QString fc;
    for (int i = 0; i < K; ++i) {
        // the photo plane: each scene moves differently (push in, pull out, drift right, drift left) so a run of stills breathes
        const int d = t.videoFrames[i];
        const QString last = n(qMax(1, d - 1)), zmax = "1.16", step = QString::number(0.16 / qMax(1, d - 1), 'f', 8);
        QString z = "min(1+" + step + "*on," + zmax + ")", x = "iw/2-(iw/zoom/2)";
        switch (i % 4) {
        case 1: z = "max(" + zmax + "-" + step + "*on,1)"; break;
        case 2: x = "(iw-iw/zoom)*on/" + last; break;
        case 3: x = "(iw-iw/zoom)*(1-on/" + last + ")"; break;
        default: break;
        }
        if (i < animated.size() && animated[i]) {         // a looping frame sequence: already moving, already 1080p
            fc += "[" + n(2 * i) + ":v]fps=" + fpsS + ",scale=1920:1080:force_original_aspect_ratio=increase:out_range=tv,crop=1920:1080,format=yuv420p,setsar=1[bg" + n(i) + "];";
        } else
        fc += "[" + n(2 * i) + ":v]scale=2880:1620:force_original_aspect_ratio=increase:out_range=tv,crop=2880:1620,zoompan=z='" + z + "':x='" + x
            + "':y='ih/2-(ih/zoom/2)':d=" + n(d) + ":s=1920x1080:fps=" + fpsS + ",format=yuv420p,setsar=1[bg" + n(i) + "];";
        // furniture over it; the looped PNG never ends on its own, so the scene is cut to its frame count
        fc += "[bg" + n(i) + "][" + n(2 * i + 1) + ":v]overlay=0:0:format=auto,format=yuv420p,trim=end_frame=" + n(d) + ",setpts=PTS-STARTPTS[s" + n(i) + "];";
    }
    QString chain = "s0"; int acc = t.videoFrames[0];
    for (int i = 1; i < K; ++i) {
        const int off = acc - t.xfadeFrames;
        const QString tr = (i == 1 || i == K - 1) ? "fade" : "smoothleft";
        fc += "[" + chain + "][s" + n(i) + "]xfade=transition=" + tr + ":duration=" + sec(double(t.xfadeFrames) / fps) + ":offset=" + sec(double(off) / fps) + "[x" + n(i) + "];";
        chain = "x" + n(i); acc = off + t.videoFrames[i];
    }
    const double total = double(t.totalFrames) / fps;
    fc += "[" + chain + "]fade=t=in:d=0.4,fade=t=out:st=" + sec(qMax(0.0, total - 0.6)) + ":d=0.6,format=yuv420p[v];";
    // narration: each scene's voice starts leadMs into its slot, padded with silence to the slot length
    for (int i = 0; i < K; ++i)
        fc += "[" + n(2 * K + i) + ":a]aformat=sample_rates=48000:channel_layouts=stereo,adelay=delays=" + n(t.leadMs) + ":all=1,apad,atrim=0:" + sec(double(t.slotFrames[i]) / fps) + ",asetpts=PTS-STARTPTS[n" + n(i) + "];";
    for (int i = 0; i < K; ++i) fc += "[n" + n(i) + "]";
    fc += "concat=n=" + n(K) + ":v=0:a=1[narr];";
    if (music) {
        fc += "[narr]asplit[nv1][nv2];[" + n(3 * K) + ":a]aformat=sample_rates=48000:channel_layouts=stereo,aloop=loop=-1:size=2000000000,volume=0.16,atrim=0:" + sec(total)
            + ",asetpts=PTS-STARTPTS,afade=t=in:d=0.8,afade=t=out:st=" + sec(qMax(0.0, total - 1.6)) + ":d=1.6[bed];"
              "[bed][nv1]sidechaincompress=threshold=0.03:ratio=8:attack=40:release=600:level_sc=1[duck];[duck][nv2]amix=inputs=2:duration=first:dropout_transition=0:normalize=0[a]";
    } else {
        fc += "[narr]anull[a]";
    }
    return fc;
}

} // namespace headline
