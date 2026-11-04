#include "queryblocks.h"
#include <QJsonArray>
#include <QRandomGenerator>
#include <QDir>
#include <QFileInfo>
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <QDateTime>

namespace sched {

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------

QStringList QueryBlocks::listOf(const QJsonObject& o, const char* a, const char* b) {
    QJsonValue v = o.value(a);
    if (v.isUndefined() && b) v = o.value(b);
    QStringList out;
    if (v.isString()) { if (!v.toString().trimmed().isEmpty()) out << v.toString().trimmed(); }
    else for (const QJsonValue& x : v.toArray()) if (!x.toString().trimmed().isEmpty()) out << x.toString().trimmed();
    return out;
}

static QJsonArray arr(const QStringList& l) { QJsonArray a; for (const auto& s : l) a << s; return a; }
static double numOf(const QJsonObject& o, const char* a, const char* b, double def) {
    if (o.contains(a)) return o.value(a).toDouble(def);
    if (b && o.contains(b)) return o.value(b).toDouble(def);
    return def;
}

// ---------------------------------------------------------------------------
// Conditions (age limits + stacked rules)
// ---------------------------------------------------------------------------

namespace {
const QStringList kTextFields = {"type", "category", "subcategory", "series", "client", "title", "filename", "path", "tag", "codec"};
const QStringList kNumFields = {"duration", "durationmin", "agedays", "episodeagedays", "lastplayeddays", "playcount", "height", "width", "priority"};
const QStringList kOps = {"is", "is_not", "contains", "not_contains", "starts_with", "ends_with", "in", "not_in", "lt", "lte", "gt", "gte", "between", "exists", "empty"};

QString canonField(QString f) { f = f.trimmed().toLower(); f.remove('_'); f.remove(' '); if (f == "tags") f = "tag"; if (f == "age") f = "agedays"; return f; }
QString canonOp(QString o) {
    o = o.trimmed().toLower();
    static const QHash<QString, QString> alias = {{"=", "is"}, {"==", "is"}, {"equals", "is"}, {"!=", "is_not"}, {"isnot", "is_not"}, {"not", "is_not"},
        {"<", "lt"}, {"<=", "lte"}, {">", "gt"}, {">=", "gte"}, {"has", "contains"}, {"notcontains", "not_contains"}, {"startswith", "starts_with"},
        {"endswith", "ends_with"}, {"notin", "not_in"}, {"newer_than", "lt"}, {"older_than", "gt"}};
    return alias.value(o, o);
}
QStringList valueList(const QJsonValue& v) {
    QStringList out;
    if (v.isArray()) { for (const auto& x : v.toArray()) { const QString s = x.isString() ? x.toString() : QString::number(x.toDouble()); if (!s.trimmed().isEmpty()) out << s.trimmed(); } }
    else if (v.isString()) { for (const QString& s : v.toString().split(',', Qt::SkipEmptyParts)) if (!s.trimmed().isEmpty()) out << s.trimmed(); }
    else if (v.isDouble()) out << QString::number(v.toDouble());
    return out;
}
double daysSince(qint64 fromMs, qint64 nowMs) { return fromMs > 0 ? double(nowMs - fromMs) / 86400000.0 : -1.0; }

// Text values of a field (a file has several tags; every other field has one value).
QStringList textValues(const MediaRecord& m, const QString& f) {
    if (f == "type") return {m.type};
    if (f == "category") return {QueryBlocks::category(m)};
    if (f == "subcategory") return {QueryBlocks::subcategory(m)};
    if (f == "series") return {m.series};
    if (f == "client") return {m.client};
    if (f == "title") return {m.title};
    if (f == "filename") return {QFileInfo(m.path).fileName()};
    if (f == "path") return {QDir::fromNativeSeparators(m.path)};
    if (f == "tag") return QueryBlocks::tags(m);
    if (f == "codec") return {m.videoCodec, m.audioCodec};
    return {};
}
// Numeric value of a field; false when the file has none. A never-aired file counts as aired infinitely long ago.
bool numValue(const MediaRecord& m, const QString& f, qint64 nowMs, double* out) {
    if (f == "duration") { *out = m.durationSec; return true; }
    if (f == "durationmin") { *out = m.durationSec / 60.0; return true; }
    if (f == "agedays") { if (m.addedAtMs <= 0) return false; *out = daysSince(m.addedAtMs, nowMs); return true; }
    if (f == "episodeagedays") { if (!m.episodeDate.isValid()) return false; *out = double(m.episodeDate.daysTo(QDateTime::fromMSecsSinceEpoch(nowMs).date())); return true; }
    if (f == "lastplayeddays") { *out = m.lastPlayedMs > 0 ? daysSince(m.lastPlayedMs, nowMs) : 1e9; return true; }
    if (f == "playcount") { *out = m.playCount; return true; }
    if (f == "height") { *out = m.height; return true; }
    if (f == "width") { *out = m.width; return true; }
    if (f == "priority") { *out = m.priority; return true; }
    return false;
}
} // namespace

bool Conditions::ruleMatches(const MediaRecord& m, const QJsonObject& rule, qint64 nowMs) {
    const bool neg = rule.value("not").toBool(false);
    if (rule.contains("rules")) {                 // nested group
        Conditions g; g.rules = rule.value("rules").toArray(); g.match = rule.value("match").toString("all").toLower();
        return g.matches(m, nowMs) != neg;
    }
    const QString f = canonField(rule.value("field").toString());
    const QString op = canonOp(rule.value("op").toString("is"));
    const QJsonValue val = rule.value("value");
    bool r = true;
    if (kNumFields.contains(f)) {
        double x = 0.0; const bool has = numValue(m, f, nowMs, &x);
        if (op == "exists") r = has;
        else if (op == "empty") r = !has;
        else if (!has) r = false;                 // no value: a numeric comparison cannot hold
        else {
            const QStringList vl = valueList(val);
            const double a = vl.value(0).toDouble(), b = vl.value(1).toDouble();
            if (op == "lt") r = x < a; else if (op == "lte") r = x <= a; else if (op == "gt") r = x > a; else if (op == "gte") r = x >= a;
            else if (op == "between") r = x >= qMin(a, b) && x <= qMax(a, b);
            else if (op == "is") r = qAbs(x - a) < 1e-6; else if (op == "is_not") r = qAbs(x - a) >= 1e-6;
            else if (op == "in") { r = false; for (const auto& s2 : vl) if (qAbs(x - s2.toDouble()) < 1e-6) r = true; }
            else if (op == "not_in") { r = true; for (const auto& s2 : vl) if (qAbs(x - s2.toDouble()) < 1e-6) r = false; }
            else r = false;
        }
    } else if (kTextFields.contains(f)) {
        QStringList have; for (const QString& s2 : textValues(m, f)) if (!s2.trimmed().isEmpty()) have << s2.trimmed().toLower();
        QStringList want; for (const auto& s2 : valueList(val)) want << s2.toLower();
        auto any = [&](auto pred) { for (const QString& h : have) for (const QString& w : want) if (pred(h, w)) return true; return false; };
        if (op == "exists") r = !have.isEmpty();
        else if (op == "empty") r = have.isEmpty();
        else if (op == "is" || op == "in") r = any([](const QString& h, const QString& w) { return h == w; });
        else if (op == "is_not" || op == "not_in") r = !any([](const QString& h, const QString& w) { return h == w; });
        else if (op == "contains") r = any([](const QString& h, const QString& w) { return h.contains(w); });
        else if (op == "not_contains") r = !any([](const QString& h, const QString& w) { return h.contains(w); });
        else if (op == "starts_with") r = any([](const QString& h, const QString& w) { return h.startsWith(w); });
        else if (op == "ends_with") r = any([](const QString& h, const QString& w) { return h.endsWith(w); });
        else r = false;
    } else {
        r = false;                                // unknown field never matches (the editors and the validator flag it)
    }
    return r != neg;
}

bool Conditions::matches(const MediaRecord& m, qint64 nowMs) const {
    if (nowMs <= 0) nowMs = QDateTime::currentMSecsSinceEpoch();
    if (maxAgeDays > 0.0 || minAgeDays > 0.0) {
        double age = -1.0;
        if (ageField == "episode") { if (m.episodeDate.isValid()) age = double(m.episodeDate.daysTo(QDateTime::fromMSecsSinceEpoch(nowMs).date())); }
        else age = daysSince(m.addedAtMs, nowMs);
        if (age < 0.0) return false;              // age unknown: an age limit cannot be satisfied
        if (maxAgeDays > 0.0 && age > maxAgeDays) return false;
        if (minAgeDays > 0.0 && age < minAgeDays) return false;
    }
    if (rules.isEmpty()) return true;
    const bool anyMode = match == "any";
    for (const QJsonValue& v : rules) {
        const bool ok = ruleMatches(m, v.toObject(), nowMs);
        if (anyMode && ok) return true;
        if (!anyMode && !ok) return false;
    }
    return !anyMode;
}

Conditions Conditions::fromJson(const QJsonObject& o) {
    Conditions c;
    c.maxAgeDays = qMax(0.0, o.value("maxAgeDays").toDouble(o.value("newerThanDays").toDouble(0.0)));
    c.minAgeDays = qMax(0.0, o.value("minAgeDays").toDouble(o.value("olderThanDays").toDouble(0.0)));
    c.ageField = o.value("ageField").toString("added").toLower() == "episode" ? "episode" : "added";
    c.rules = o.value("rules").toArray();
    c.match = o.value("match").toString("all").toLower() == "any" ? "any" : "all";
    return c;
}

void Conditions::writeJson(QJsonObject& o) const {
    if (maxAgeDays > 0.0) o["maxAgeDays"] = maxAgeDays;
    if (minAgeDays > 0.0) o["minAgeDays"] = minAgeDays;
    if ((maxAgeDays > 0.0 || minAgeDays > 0.0) && ageField != "added") o["ageField"] = ageField;
    if (!rules.isEmpty()) { o["rules"] = rules; if (match != "all") o["match"] = match; }
}

QString Conditions::label() const {
    QStringList p;
    const QString unit = ageField == "episode" ? "d ep" : "d";
    if (maxAgeDays > 0.0 && minAgeDays > 0.0) p << QString("%1-%2%3").arg(minAgeDays).arg(maxAgeDays).arg(unit);
    else if (maxAgeDays > 0.0) p << QString::fromUtf8("\u2264") + QString::number(maxAgeDays) + unit;
    else if (minAgeDays > 0.0) p << QString::fromUtf8("\u2265") + QString::number(minAgeDays) + unit;
    if (!rules.isEmpty()) p << QString("%1 rule%2%3").arg(rules.size()).arg(rules.size() == 1 ? "" : "s").arg(match == "any" ? " (any)" : "");
    return p.join(", ");
}

QStringList Conditions::errors() const {
    QStringList e;
    std::function<void(const QJsonArray&)> walk = [&](const QJsonArray& a) {
        for (const QJsonValue& v : a) {
            const QJsonObject r = v.toObject();
            if (r.contains("rules")) { walk(r.value("rules").toArray()); continue; }
            const QString f = canonField(r.value("field").toString()), op = canonOp(r.value("op").toString("is"));
            if (!kTextFields.contains(f) && !kNumFields.contains(f)) e << "unknown rule field '" + r.value("field").toString() + "'";
            if (!kOps.contains(op)) e << "unknown rule op '" + r.value("op").toString() + "'";
            else if (kTextFields.contains(f) && QStringList{"lt", "lte", "gt", "gte", "between"}.contains(op)) e << "'" + op + "' needs a number field, not '" + f + "'";
        }
    };
    walk(rules);
    return e;
}

QueryBlock QueryBlock::fromJson(const QJsonObject& o) {
    QueryBlock b;
    b.types = QueryBlocks::listOf(o, "types", "type");
    b.categories = QueryBlocks::listOf(o, "categories", "category");
    b.subcategories = QueryBlocks::listOf(o, "subcategories", "subcategory");
    b.series = QueryBlocks::listOf(o, "series", "seriesName");
    b.tags = QueryBlocks::listOf(o, "tags", "tag");
    b.clients = QueryBlocks::listOf(o, "clients", "client");
    b.targetSec = qMax(0.0, numOf(o, "targetSec", "target_sec", 0.0));
    if (b.targetSec <= 0.0 && o.contains("minutes")) b.targetSec = qMax(0.0, o.value("minutes").toDouble()) * 60.0;
    const QString p = o.value("prefer").toString("least_played").trimmed().toLower();
    static const QStringList prefers = {"even", "newest", "oldest", "most_played", "least_played", "random", "longest", "shortest"};
    b.prefer = (p == "rotate" || p == "rotation" || p == "evenly") ? "even" : prefers.contains(p) ? p : "least_played";
    b.nthLatest = qMax(0, int(numOf(o, "nthLatest", "nth_latest", 0)));
    b.noRepeatMin = qMax(0.0, numOf(o, "noRepeatMin", "no_repeat_min", 30.0));
    b.minSec = qMax(0.0, numOf(o, "minSec", "min_sec", 0.0));
    b.maxSec = qMax(0.0, numOf(o, "maxSec", "max_sec", 0.0));
    b.fitToTarget = o.value("fitToTarget").toBool(o.value("fit_to_target").toBool(true));
    b.cond = Conditions::fromJson(o);
    return b;
}

QJsonObject QueryBlock::toJson() const {
    QJsonObject o;
    o["types"] = arr(types);
    if (!categories.isEmpty()) o["categories"] = arr(categories);
    if (!subcategories.isEmpty()) o["subcategories"] = arr(subcategories);
    if (!series.isEmpty()) o["series"] = arr(series);
    if (!tags.isEmpty()) o["tags"] = arr(tags);
    if (!clients.isEmpty()) o["clients"] = arr(clients);
    o["targetSec"] = targetSec; o["prefer"] = prefer;
    if (nthLatest) o["nthLatest"] = nthLatest;
    o["noRepeatMin"] = noRepeatMin;
    if (minSec > 0) o["minSec"] = minSec;
    if (maxSec > 0) o["maxSec"] = maxSec;
    if (!fitToTarget) o["fitToTarget"] = false;
    cond.writeJson(o);
    return o;
}

QString QueryBlock::label() const {
    QString what = !series.isEmpty() ? series.join("/") : !subcategories.isEmpty() ? subcategories.join("/") : !categories.isEmpty() ? categories.join("/") : types.join("/");
    if (what.isEmpty()) what = "any";
    if (!cond.isEmpty()) what += " [" + cond.label() + "]";
    QString pref = prefer; pref.replace('_', ' ');
    if (!series.isEmpty()) pref = nthLatest == 0 ? "latest" : nthLatest == 1 ? "previous" : QString("%1 back").arg(nthLatest);
    const QString len = targetSec > 0 ? QString(" ~%1m").arg(QString::number(targetSec / 60.0, 'g', 3)) : QString();
    return QString("%1%2 (%3)").arg(what, len, pref);
}

FolderBlock FolderBlock::fromJson(const QJsonObject& o) {
    FolderBlock f;
    f.folder = o.value("folder").toString(o.value("path").toString()).trimmed();
    f.targetSec = qMax(0.0, numOf(o, "targetSec", "target_sec", 0.0));
    const QString w = o.value("weight").toString("freshness").toLower();
    f.weight = (w == "uniform") ? "uniform" : "freshness";
    const QString ord = o.value("order").toString(o.value("sortOrder").toString("random")).toLower();
    static const QStringList orders = {"random", "even", "name", "date", "size", "added"};
    f.order = orders.contains(ord) ? ord : "random";
    f.recursive = o.value("recursive").toBool(true);
    f.cond = Conditions::fromJson(o);
    return f;
}

QJsonObject FolderBlock::toJson() const {
    QJsonObject o; o["folder"] = folder; o["targetSec"] = targetSec; o["weight"] = weight; o["order"] = order; o["recursive"] = recursive; cond.writeJson(o); return o;
}

QString FolderBlock::label() const {
    const QString name = QFileInfo(QDir::fromNativeSeparators(folder)).fileName().isEmpty() ? folder : QFileInfo(QDir::fromNativeSeparators(folder)).fileName();
    const QString how = order == "random" ? (weight == "uniform" ? "random" : "random, fresh first") : order == "even" ? "even rotation" : "by " + order;
    return QString("\xF0\x9F\x93\x81 %1 (%2)").arg(name, cond.isEmpty() ? how : how + ", " + cond.label());
}

// ---------------------------------------------------------------------------
// Matching
// ---------------------------------------------------------------------------

bool QueryBlocks::inList(const QString& v, const QStringList& l) {
    if (l.isEmpty()) return true;
    const QString n = norm(v);
    for (const auto& s : l) if (norm(s) == n) return true;
    return false;
}

QStringList QueryBlocks::tags(const MediaRecord& m) {
    QStringList t;
    const QJsonValue v = m.extra.value("tags");
    if (v.isString()) t = v.toString().split(',', Qt::SkipEmptyParts);
    else for (const QJsonValue& x : v.toArray()) t << x.toString();
    for (auto& s : t) s = s.trimmed();
    return t;
}

bool QueryBlocks::eligible(const MediaRecord& m, const QueryContext& ctx) {
    if (m.path.isEmpty() || m.durationSec <= 0.5) return false;
    if (!m.isAvailableAt(ctx.nowMs)) return false;
    if (!m.allowedChannels.isEmpty() && !ctx.channelId.isEmpty() && !m.allowedChannels.contains(ctx.channelId)) return false;
    const QString st = m.extra.value("playoutStatus").toString(m.extra.value("status").toString());
    if (st == "missing" || st == "failed" || st == "blocked" || m.extra.value("missing").toBool(false)) return false;
    return true;
}

bool QueryBlocks::matches(const MediaRecord& m, const QueryBlock& b, const QueryContext& ctx) {
    if (!eligible(m, ctx)) return false;
    if (!inList(m.type, b.types)) return false;
    if (!inList(category(m), b.categories)) return false;
    if (!inList(subcategory(m), b.subcategories)) return false;
    if (!inList(m.series, b.series)) return false;
    if (!inList(m.client, b.clients)) return false;
    if (!b.tags.isEmpty()) { bool any = false; for (const auto& t : tags(m)) if (inList(t, b.tags)) { any = true; break; } if (!any) return false; }
    if (b.minSec > 0 && m.durationSec < b.minSec) return false;
    if (b.maxSec > 0 && m.durationSec > b.maxSec) return false;
    if (!b.cond.isEmpty() && !b.cond.matches(m, ctx.nowMs)) return false;
    return true;
}

bool QueryBlocks::matchesFolder(const MediaRecord& m, const FolderBlock& f) {
    const QString want = norm(QDir::fromNativeSeparators(f.folder));
    if (want.isEmpty()) return false;
    const QString path = norm(QDir::fromNativeSeparators(m.path));
    if (want.contains('/') || want.contains(':')) {
        QString prefix = want; if (!prefix.endsWith('/')) prefix += '/';
        if (path.startsWith(prefix)) {
            if (f.recursive) return true;
            return !path.mid(prefix.size()).contains('/');
        }
        return false;
    }
    // A bare name: the folder the file sits in, or a category / subcategory / tag / series of that name.
    const QString dirName = norm(QFileInfo(path).dir().dirName());
    if (dirName == want) return true;
    if (norm(category(m)) == want || norm(subcategory(m)) == want || norm(m.series) == want) return true;
    for (const auto& t : tags(m)) if (norm(t) == want) return true;
    return false;
}

// ---------------------------------------------------------------------------
// Ranking
// ---------------------------------------------------------------------------

namespace {
// Ranking keys compare lexicographically (never folded into one double: a play count, an epoch-ms play time and an
// epoch-ms added date live on scales that swamp each other).
using Key = std::array<double, 3>;
struct Ranked { const MediaRecord* m; Key key; double bonus; bool repeat; bool avoid; double jitter; };

// Effective last airing for rotation: a pick already planned for an upcoming hour counts as airing "now" (it will air
// before this pick does), so consecutive hours do not take the same least-recently-aired file.
double rotationTime(const MediaRecord& m, int planned, qint64 nowMs) { return planned > 0 ? double(qMax<qint64>(nowMs, m.lastPlayedMs)) + planned : double(m.lastPlayedMs); }

Key preferKey(const MediaRecord& m, const QString& prefer, int planned, qint64 nowMs) {
    // Lower sorts first.
    // newest / oldest follow when the asset entered the library (PlayPlay OBS: created_at). Episode dates only order
    // series blocks, which are positional and handled separately; mixing them in here let a Julian-day term swamp addedAtMs.
    const double added = double(m.addedAtMs > 0 ? m.addedAtMs : 0);
    if (prefer == "newest") return {-added, 0.0, 0.0};
    if (prefer == "oldest") return {added, 0.0, 0.0};
    if (prefer == "most_played") return {-double(m.playCount), -double(m.lastPlayedMs), 0.0};
    if (prefer == "longest") return {-m.durationSec, 0.0, 0.0};
    if (prefer == "shortest") return {m.durationSec, 0.0, 0.0};
    if (prefer == "random") return {0.0, 0.0, 0.0};
    // even: strict rotation, longest since aired (or planned) first; never-aired files lead; play count breaks ties.
    if (prefer == "even") return {rotationTime(m, planned, nowMs), double(m.playCount + planned), added};
    // least_played: fewest plays (counting picks planned for upcoming hours), then longest since aired, then newest.
    return {double(m.playCount + planned), rotationTime(m, planned, nowMs), -added};
}
} // namespace

QList<QueryPick> QueryBlocks::candidates(const QList<MediaRecord>& pool, const QueryBlock& b, const QueryContext& ctx, int n) {
    QList<QueryPick> out;
    if (!b.isValid()) return out;
    const bool seriesBlock = !b.series.isEmpty();
    QRandomGenerator rng(ctx.seed ? ctx.seed : QRandomGenerator::global()->generate());
    QList<Ranked> ranked;
    for (const MediaRecord& m : pool) {
        if (!matches(m, b, ctx)) continue;
        if (!seriesBlock && ctx.exclude.contains(m.id)) continue;
        Ranked r; r.m = &m; r.key = preferKey(m, b.prefer, ctx.planned.value(m.id, 0), ctx.nowMs); r.jitter = rng.generateDouble();
        r.bonus = ctx.contractBonus ? ctx.contractBonus(m.id) : 0.0;
        r.repeat = (m.lastPlayedMs > 0 && ctx.nowMs > 0 && b.noRepeatMin > 0 && (ctx.nowMs - m.lastPlayedMs) < qint64(b.noRepeatMin * 60000.0))
                   || (ctx.planned.value(m.id, 0) > 0 && b.noRepeatMin > 0);   // already lined up for an upcoming hour
        r.avoid = ctx.avoid.contains(m.id);
        ranked.append(r);
    }
    if (seriesBlock) {
        // Episode order (newest first), positional pick; exclusions and repeats ignored on purpose.
        std::sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& c) {
            const qint64 da = a.m->episodeDate.isValid() ? a.m->episodeDate.toJulianDay() : 0, dc = c.m->episodeDate.isValid() ? c.m->episodeDate.toJulianDay() : 0;
            if (da != dc) return da > dc;
            return a.m->addedAtMs > c.m->addedAtMs;
        });
        for (int i = b.nthLatest; i < ranked.size() && out.size() < n; ++i) { QueryPick p; p.media = *ranked[i].m; out.append(p); }
        return out;
    }
    std::sort(ranked.begin(), ranked.end(), [&](const Ranked& a, const Ranked& c) {
        if (a.repeat != c.repeat) return !a.repeat;                 // fresh before repeats
        if (a.avoid != c.avoid) return !a.avoid;                    // a re-roll moves on from its previous picks
        if (std::fabs(a.bonus - c.bonus) > 1e-9) return a.bonus > c.bonus;   // behind-pace contracts first
        if (b.prefer == "random") return a.jitter < c.jitter;
        if (a.key != c.key) return a.key < c.key;
        return a.jitter < c.jitter;
    });
    for (const Ranked& r : ranked) { if (out.size() >= n) break; QueryPick p; p.media = *r.m; p.repeat = r.repeat; out.append(p); }
    return out;
}

QList<QueryPick> QueryBlocks::resolve(const QList<MediaRecord>& pool, const QueryBlock& b, const QueryContext& ctx, QString* why) {
    QList<QueryPick> out;
    QueryContext c = ctx;
    QList<QueryPick> cands = candidates(pool, b, c, 400);
    if (cands.isEmpty() && !c.exclude.isEmpty() && b.series.isEmpty()) {
        // Everything that matches is already placed in this hour (or the next): a small library must still fill the block,
        // so repeat (in the block's own order, flagged) rather than leave dead air for the gap fill.
        QueryContext relaxed = c; relaxed.exclude.clear();
        cands = candidates(pool, b, relaxed, 400);
        for (QueryPick& p : cands) p.repeat = true;
        c = relaxed;
    }
    if (cands.isEmpty()) { if (why) *why = "no media matches " + b.label(); return out; }
    if (b.targetSec <= 0.0 || !b.series.isEmpty()) { out.append(cands.first()); return out; }

    // Cover the target: best-ranked item that fits the remaining room; tolerance 10 % of the target (>= 15 s).
    const double tol = tolerance(b.targetSec);
    double remaining = b.targetSec;
    QSet<QString> used = c.exclude;
    int guard = 0;
    while (remaining > 1.0 && guard++ < 200) {
        int chosen = -1;
        if (b.fitToTarget) for (int i = 0; i < cands.size(); ++i) { if (used.contains(cands[i].media.id)) continue; if (cands[i].media.durationSec <= remaining + tol) { chosen = i; break; } }
        if (chosen < 0) {
            if (!out.isEmpty()) break;                       // the rest of the target stays open for the next item / fill
            for (int i = 0; i < cands.size(); ++i) if (!used.contains(cands[i].media.id)) { chosen = i; break; }
            if (chosen < 0) break;
            cands[chosen].overrun = cands[chosen].media.durationSec > remaining + tol;
        }
        QueryPick p = cands[chosen];
        used.insert(p.media.id);
        out.append(p);
        remaining -= p.media.durationSec;
        // Repeats are only acceptable once every fresh candidate is in use.
        if (out.size() >= 60) break;
    }
    if (out.isEmpty() && why) *why = "nothing fits " + b.label();
    return out;
}

QList<QueryPick> QueryBlocks::resolveFolder(const QList<MediaRecord>& pool, const FolderBlock& f, const QueryContext& ctx, QString* why) {
    QList<QueryPick> out = resolveFolderOnce(pool, f, ctx, why);
    if (out.isEmpty() && !ctx.exclude.isEmpty()) {
        // Every file of the folder is already placed nearby: repeat (flagged) rather than leave the block empty.
        QueryContext relaxed = ctx; relaxed.exclude.clear();
        QString why2;
        out = resolveFolderOnce(pool, f, relaxed, &why2);
        for (QueryPick& p : out) p.repeat = true;
        if (!out.isEmpty() && why) why->clear();
    }
    return out;
}

QList<QueryPick> QueryBlocks::resolveFolderOnce(const QList<MediaRecord>& pool, const FolderBlock& f, const QueryContext& ctx, QString* why) {
    QList<QueryPick> out;
    if (!f.isValid()) { if (why) *why = "folder block has no folder"; return out; }
    QList<const MediaRecord*> cands;
    for (const MediaRecord& m : pool) if (eligible(m, ctx) && matchesFolder(m, f) && (f.cond.isEmpty() || f.cond.matches(m, ctx.nowMs))) cands.append(&m);
    if (cands.isEmpty()) { if (why) *why = "folder '" + f.folder + "' has no playable media"; return out; }

    const double target = f.targetSec;
    double remaining = target > 0 ? target : 1e9;      // no target = a single pick (the loops break after the first)
    const double tol = tolerance(target);
    QSet<QString> used = ctx.exclude;
    QRandomGenerator rng(ctx.seed ? ctx.seed : QRandomGenerator::global()->generate());

    if (f.order == "even") {
        // Even rotation: longest since aired (or planned for an upcoming hour) first, so every file in the folder plays
        // once before any plays twice. A re-roll's previous picks go last.
        std::stable_sort(cands.begin(), cands.end(), [&](const MediaRecord* a, const MediaRecord* c) {
            const bool aa = ctx.avoid.contains(a->id), ca = ctx.avoid.contains(c->id);
            if (aa != ca) return !aa;
            const double ta = rotationTime(*a, ctx.planned.value(a->id, 0), ctx.nowMs), tc = rotationTime(*c, ctx.planned.value(c->id, 0), ctx.nowMs);
            if (ta != tc) return ta < tc;
            if (a->playCount != c->playCount) return a->playCount < c->playCount;
            return QFileInfo(a->path).fileName().compare(QFileInfo(c->path).fileName(), Qt::CaseInsensitive) < 0;
        });
        for (const MediaRecord* m : cands) {
            if (remaining <= 1.0) break;
            if (used.contains(m->id)) continue;
            if (target > 0 && !out.isEmpty() && m->durationSec > remaining + tol) continue;
            QueryPick p; p.media = *m; p.overrun = target > 0 && out.isEmpty() && m->durationSec > remaining + tol;
            out.append(p); used.insert(m->id); remaining -= m->durationSec;
            if (target <= 0) break;
        }
        if (out.isEmpty() && why) *why = "nothing fits folder '" + f.folder + "'";
        return out;
    }

    if (f.order != "random") {
        // Ordered round robin: the sort order defines the loop; the item after the most recently aired one is next.
        std::stable_sort(cands.begin(), cands.end(), [&](const MediaRecord* a, const MediaRecord* c) {
            if (f.order == "name") return QFileInfo(a->path).fileName().compare(QFileInfo(c->path).fileName(), Qt::CaseInsensitive) < 0;
            if (f.order == "size") return a->fileSizeBytes < c->fileSizeBytes;
            if (f.order == "added") return a->addedAtMs < c->addedAtMs;
            const qint64 da = a->episodeDate.isValid() ? a->episodeDate.toJulianDay() : 0, dc = c->episodeDate.isValid() ? c->episodeDate.toJulianDay() : 0;
            if (da != dc) return da < dc;
            return a->addedAtMs < c->addedAtMs;
        });
        // Never-aired items carry lastPlayedMs 0: only a real airing moves the cursor, so an untouched folder starts at its first item.
        int start = 0; qint64 best = 0;
        for (int i = 0; i < cands.size(); ++i) if (cands[i]->lastPlayedMs > best) { best = cands[i]->lastPlayedMs; start = (i + 1) % cands.size(); }
        // A re-roll's previous picks (ctx.avoid) are skipped on the first pass and only used when nothing else is left.
        for (int pass = 0; pass < 2 && out.isEmpty(); ++pass) {
            for (int k = 0; k < cands.size() && remaining > 1.0; ++k) {
                const MediaRecord* m = cands[(start + k) % cands.size()];
                if (used.contains(m->id)) continue;
                if (pass == 0 && ctx.avoid.contains(m->id)) continue;
                if (target > 0 && !out.isEmpty() && m->durationSec > remaining + tol) continue;
                QueryPick p; p.media = *m; p.overrun = target > 0 && out.isEmpty() && m->durationSec > remaining + tol;
                out.append(p); used.insert(m->id); remaining -= m->durationSec;
                if (target <= 0) break;
            }
        }
        if (out.isEmpty() && why) *why = "nothing fits folder '" + f.folder + "'";
        return out;
    }

    // Random modes: weighted by freshness (1/(1+plays), x0.15 when aired in the last 30 min) or uniform.
    int guard = 0;
    while (remaining > 1.0 && guard++ < 200) {
        QList<double> w; double total = 0.0; QList<int> idx;
        for (int pass = 0; pass < 2 && idx.isEmpty(); ++pass) {      // pass 0 leaves out a re-roll's previous picks (ctx.avoid)
            for (int i = 0; i < cands.size(); ++i) {
                const MediaRecord* m = cands[i];
                if (used.contains(m->id)) continue;
                if (pass == 0 && ctx.avoid.contains(m->id)) continue;
                if (target > 0 && !out.isEmpty() && m->durationSec > remaining + tol) continue;
                double x = 1.0;
                if (f.weight == "freshness") {
                    x = 1.0 / (1.0 + m->playCount);
                    if (m->lastPlayedMs > 0 && ctx.nowMs - m->lastPlayedMs < 1800000LL) x *= 0.15;
                    if (ctx.planned.value(m->id, 0) > 0) x *= 0.15;     // already lined up for an upcoming hour
                    if (m->addedAtMs > 0 && ctx.nowMs - m->addedAtMs < 7LL * 86400000LL) x *= 1.5;   // new arrivals get a nudge
                }
                x = qMax(x, 0.01);
                w.append(x); idx.append(i); total += x;
            }
        }
        if (idx.isEmpty()) break;
        double r = rng.generateDouble() * total; int pick = idx.last();
        for (int k = 0; k < idx.size(); ++k) { r -= w[k]; if (r <= 0) { pick = idx[k]; break; } }
        QueryPick p; p.media = *cands[pick]; p.overrun = target > 0 && out.isEmpty() && p.media.durationSec > remaining + tol;
        out.append(p); used.insert(p.media.id); remaining -= p.media.durationSec;
        if (target <= 0) break;
    }
    if (out.isEmpty() && why) *why = "nothing fits folder '" + f.folder + "'";
    return out;
}

} // namespace sched
