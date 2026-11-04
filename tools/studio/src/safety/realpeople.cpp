#include "safety/realpeople.h"

#include "core/datafiles.h"
#include "core/log.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <memory>
#include <mutex>

namespace studio::safety {
namespace {

// Everything the checks need, parsed once. `ok` false = fail closed (every check answers "real person").
struct Lists {
    bool ok = false;
    QString error;
    QStringList full, stage, ambiguous;
    QSet<QString> fullSet, given;
    QHash<char32_t, QString> confusables;
    QRegularExpression titleName, realWords, photoWords, stylized, notPersonWord;   // from the data file
    QList<QPair<QString, QRegularExpression>> capRe;   // ambiguous name → its Capitalised-anywhere matcher
    // hand-ported from realpeople.ts (code there, not data), compiled with their JS meaning through data::regex
    QRegularExpression dogs, upperStart, photoReal, ofTheAnd, spanielName, spanielAfter, weakName, ws;
};

QStringList strings(const QJsonValue& v) {
    QStringList out;
    for (const QJsonValue& x : v.toArray()) out << x.toString();
    return out;
}

// data::regex with the JS meaning; a pattern that does not translate or compile makes the whole snapshot fail closed.
bool compile(QRegularExpression& out, QStringView source, QStringView flags, QString* error) {
    QString why;
    out = data::regex(source, flags, &why);
    if (!out.isValid() || !why.isEmpty()) {
        if (error && error->isEmpty()) *error = QStringLiteral("pattern %1 does not compile: %2").arg(source.left(60), why);
        return false;
    }
    return true;
}
bool compile(QRegularExpression& out, const QJsonValue& pattern, QString* error) {
    QString why;
    out = data::regex(pattern, &why);
    if (!out.isValid() || !why.isEmpty()) {
        if (error && error->isEmpty()) *error = QStringLiteral("data pattern does not compile: %1").arg(why);
        return false;
    }
    return true;
}

QString escRe(const QString& s) { return QRegularExpression::escape(s); }

std::shared_ptr<const Lists> build(const QJsonObject& d, QString loadError) {
    auto l = std::make_shared<Lists>();
    QString& err = l->error;
    if (d.isEmpty()) {
        err = loadError.isEmpty() ? QStringLiteral("realpeople data did not load") : loadError;
        return l;
    }
    l->full = strings(d.value(u"publicFigures"));
    l->stage = strings(d.value(u"stageNames"));
    l->ambiguous = strings(d.value(u"ambiguousNames"));
    for (const QString& g : strings(d.value(u"givenNames"))) l->given.insert(g);
    for (const QString& f : l->full) l->fullSet.insert(f);
    const QJsonObject conf = d.value(u"confusables").toObject();
    for (auto it = conf.begin(); it != conf.end(); ++it) {
        const QList<uint> k = it.key().toUcs4();
        if (k.size() == 1) l->confusables.insert(char32_t(k.front()), it.value().toString());
    }
    // A list that came back empty is a broken export, not "nobody is famous".
    if (l->full.isEmpty() || l->stage.isEmpty() || l->ambiguous.isEmpty() || l->given.isEmpty() || l->confusables.isEmpty()) {
        err = QStringLiteral("realpeople data is missing a list");
        return l;
    }
    const QJsonObject p = d.value(u"patterns").toObject();
    bool ok = compile(l->titleName, p.value(u"titleName"), &err) && compile(l->realWords, p.value(u"realWords"), &err) &&
              compile(l->photoWords, p.value(u"photoWords"), &err) && compile(l->stylized, p.value(u"stylized"), &err) &&
              compile(l->notPersonWord, p.value(u"notPersonWord"), &err);
    // realpeople.ts: dogs(), capitalised(), the photo-real exception, "Mayor of …", the dog-breed rule, the weak pair
    ok = ok && compile(l->dogs, u" king charles (cavalier )?spaniels? ", u"g", &err) &&
         compile(l->upperStart, u"^\\p{Lu}", u"u", &err) && compile(l->photoReal, u"\\bphoto-?real(istic)?\\b", u"i", &err) &&
         compile(l->ofTheAnd, u"^(of|the|and)$", u"i", &err) && compile(l->spanielName, u"spaniel", u"i", &err) &&
         compile(l->spanielAfter, u"^\\s*(cavalier\\s+)?spaniels?\\b", u"i", &err) &&
         compile(l->weakName, u"\\b([A-Z][a-z'\u2019-]+)\\s+([A-Z][a-z'\u2019-]{1,})\\b", u"g", &err) &&
         compile(l->ws, u"\\s+", u"", &err);
    for (const QString& a : std::as_const(l->ambiguous)) {
        if (!ok) break;
        QStringList words = a.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        for (QString& w : words) w = escRe(w);
        QRegularExpression re;
        ok = compile(re, QStringLiteral("(?<![\\p{L}\\p{N}])%1(?![\\p{L}\\p{N}])").arg(words.join(QStringLiteral("\\s+"))), u"giu", &err);
        l->capRe.append({a, re});
    }
    l->ok = ok;
    return l;
}

std::mutex g_mutex;
std::shared_ptr<const Lists> g_lists;    // the current snapshot (lazily built)
bool g_logged = false;

std::shared_ptr<const Lists> lists() {
    std::lock_guard<std::mutex> g(g_mutex);
    if (!g_lists) {
        QString e;
        const QJsonObject d = data::load(u"realpeople", &e);
        g_lists = build(d, e);
    }
    if (!g_lists->ok && !g_logged) {
        g_logged = true;
        log::error(u"safety", QStringLiteral("real-people lists unavailable, every picture / voice check refuses: %1").arg(g_lists->error));
    }
    return g_lists;
}

// Code points removed by foldText besides \p{Cf}.
bool invisible(char32_t c) {
    return c == 0x034F || c == 0x115F || c == 0x1160 || c == 0x3164 || (c >= 0xFE00 && c <= 0xFE0F) ||
           (c >= 0xE0100 && c <= 0xE01EF) || QChar::category(c) == QChar::Other_Format;
}

QString fold(const Lists& l, QStringView s) {
    const QString n = s.toString().normalized(QString::NormalizationForm_KC);
    QString out;
    out.reserve(n.size());
    for (char32_t c : n.toUcs4()) {
        if (invisible(c)) continue;
        const auto it = l.confusables.constFind(c);
        if (it != l.confusables.constEnd()) out += *it;
        else out += QString::fromUcs4(&c, 1);
    }
    return out;
}

QString norm(const Lists& l, QStringView s) {
    const QString d = fold(l, s).normalized(QString::NormalizationForm_KD);
    QString lower;
    lower.reserve(d.size());
    for (QChar c : d)
        if (!(c.unicode() >= 0x0300 && c.unicode() <= 0x036F)) lower += c;
    lower = lower.toLower();
    QString out;
    out.reserve(lower.size());
    bool gap = false;
    for (QChar c : std::as_const(lower)) {
        const ushort u = c.unicode();
        if ((u >= 'a' && u <= 'z') || (u >= '0' && u <= '9')) {
            if (gap && !out.isEmpty()) out += QLatin1Char(' ');
            gap = false;
            out += c;
        } else {
            gap = true;
        }
    }
    return out;
}

// " <normName> " with the dog breed taken out.
QString paddedNoDogs(const Lists& l, QStringView s) {
    QString n = QLatin1Char(' ') + norm(l, s) + QLatin1Char(' ');
    n.replace(l.dogs, QStringLiteral(" spaniel "));
    return n;
}

bool containsWord(const QString& padded, const QString& name) {
    return padded.contains(QLatin1Char(' ') + name + QLatin1Char(' '));
}

void addUnique(QStringList& out, const QString& s) {
    if (!out.contains(s)) out << s;
}

QStringList figuresIn(const Lists& l, QStringView text, bool fullOnly) {
    const QString n = paddedNoDogs(l, text);
    QStringList out;
    for (const QString& f : l.full)
        if (containsWord(n, f)) addUnique(out, f);
    if (fullOnly) return out;
    for (const QString& s : l.stage)
        if (containsWord(n, s)) addUnique(out, s);
    const QString raw = fold(l, text);
    for (const auto& [name, re] : l.capRe) {
        for (auto it = re.globalMatch(raw); it.hasNext();) {
            const QString m = it.next().captured(0);
            bool cap = true;
            for (const QString& w : m.split(l.ws, Qt::SkipEmptyParts)) cap = cap && l.upperStart.match(w).hasMatch();
            if (cap) { addUnique(out, name); break; }
        }
    }
    return out;
}

QString joinFirst3(const QStringList& v) { return v.mid(0, 3).join(QStringLiteral(", ")); }

} // namespace

bool listsReady(QString* error) {
    const auto l = lists();
    if (error) *error = l->error;
    return l->ok;
}

QString foldText(QStringView s) { return fold(*lists(), s); }
QString normName(QStringView s) { return norm(*lists(), s); }

QStringList publicFiguresIn(QStringView text, bool fullNamesOnly) {
    const auto l = lists();
    if (!l->ok) return {kUnavailable.toString()};
    return figuresIn(*l, text, fullNamesOnly);
}

QString findPublicFigure(QStringView text, bool fullNamesOnly) {
    const QStringList v = publicFiguresIn(text, fullNamesOnly);
    return v.isEmpty() ? QString() : v.front();
}

QString publicFigureInName(QStringView name) {
    const auto l = lists();
    if (!l->ok) return kUnavailable.toString();
    const QString n = paddedNoDogs(*l, name);
    const QString t = n.trimmed();
    if (t.isEmpty()) return {};
    if (l->fullSet.contains(t)) return t;
    for (const QStringList* list : {&l->full, &l->stage, &l->ambiguous})
        for (const QString& f : *list)
            if (containsWord(n, f)) return f;
    return {};
}

RealPersonVerdict realPersonCheck(QStringView text, QStringView style) {
    const auto lp = lists();
    const Lists& l = *lp;
    RealPersonVerdict v;
    if (!l.ok) {
        v.refuse = true;
        v.names = {kUnavailable.toString()};
        v.reason = QStringLiteral("The real-people safety lists are unavailable (%1), so no picture or video of people can be checked or made. "
                                  "This is a Studio installation problem; nothing was sent.").arg(l.error);
        return v;
    }
    // read the text the way a model would: no invisible characters, no look-alike letters
    const QString all = fold(l, text.toString() + QLatin1Char(' ') + style.toString());
    v.stylized = l.stylized.match(all).hasMatch() && !l.photoReal.match(all).hasMatch();
    const bool photo = l.photoWords.match(all).hasMatch();
    QStringList strong = figuresIn(l, all, false), weak;

    for (auto it = l.titleName.globalMatch(all); it.hasNext();) {
        const auto m = it.next();
        const QString name = m.captured(1).trimmed();
        const QStringList words = name.split(l.ws);
        bool place = false;
        for (QString w : words) {
            w.remove(QLatin1Char('.')).remove(QLatin1Char(','));
            place = place || l.notPersonWord.match(w).hasMatch();
        }
        if (place) continue;                                        // "President Street", "Mayor of …"
        if (l.ofTheAnd.match(words.value(0)).hasMatch()) continue;
        if (name.isEmpty() || !(name.front() >= QLatin1Char('A') && name.front() <= QLatin1Char('Z'))) continue;
        if (l.spanielName.match(name).hasMatch() || l.spanielAfter.matchView(QStringView(all).mid(m.capturedEnd())).hasMatch()) continue;
        strong << m.captured(0).trimmed();
    }
    const auto rw = l.realWords.match(all);
    if (rw.hasMatch()) strong << rw.captured(0);

    for (auto it = l.weakName.globalMatch(all); it.hasNext();) {
        const auto m = it.next();
        const QString first = m.captured(1), last = m.captured(2);
        if (!l.given.contains(first.toLower())) continue;
        if (l.notPersonWord.match(last).hasMatch()) continue;
        weak << first + QLatin1Char(' ') + last;
    }

    for (const QString& s : std::as_const(strong)) addUnique(v.names, s);
    for (const QString& s : std::as_const(weak)) addUnique(v.names, s);
    QStringList strongU;
    for (const QString& s : std::as_const(strong)) addUnique(strongU, s);
    if (!strong.isEmpty() && !v.stylized) {
        v.refuse = true;
        v.reason = QStringLiteral("This looks like a request for a realistic picture of a real, identifiable person (%1). StarMedia does not generate "
                                  "photorealistic images or video of real people. Describe a fictional person by appearance instead (no real name), "
                                  "or ask for a clearly non-photographic look (illustration, cartoon, caricature) if that is appropriate.")
                       .arg(joinFirst3(strongU));
        return v;
    }
    if (!weak.isEmpty() && photo && !v.stylized) {
        v.refuse = true;
        v.reason = QStringLiteral("The prompt names someone (%1) and asks for a photographic look. StarMedia does not make photorealistic pictures of "
                                  "real, identifiable people. If this is a fictional character, drop the full name and describe them by appearance "
                                  "(e.g. \"a woman in her 40s with short grey hair\"), or choose an illustrated style.")
                       .arg(joinFirst3(weak));
    }
    return v;
}

namespace testing {
void setData(const QJsonObject& data) {
    auto l = build(data, QStringLiteral("test data"));
    std::lock_guard<std::mutex> g(g_mutex);
    g_lists = std::move(l);
}
void resetData() {
    std::lock_guard<std::mutex> g(g_mutex);
    g_lists.reset();
    g_logged = false;
}
} // namespace testing

} // namespace studio::safety
