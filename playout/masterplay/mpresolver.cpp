#include "mpresolver.h"

#include "mpasrun.h"
#include "queryblocks.h"
#include "schedulestore.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace mp::res {

namespace {

// JavaScript's \s (and String.prototype.trim): WhiteSpace + LineTerminator. QChar::isSpace differs on U+0085 / U+FEFF.
bool jsSpace(QChar c) {
    switch (c.unicode()) {
    case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x20: case 0xA0: case 0x1680:
    case 0x2028: case 0x2029: case 0x202F: case 0x205F: case 0x3000: case 0xFEFF:
        return true;
    default:
        return c.unicode() >= 0x2000 && c.unicode() <= 0x200A;
    }
}

// .replace(/[_\-]+/g, ' ').replace(/\s+/g, ' ').trim().toLowerCase() in one pass: every run of '_', '-' and whitespace
// becomes one space, none at either end.
QString keyBody(const QString& s) {
    QString out;
    out.reserve(s.size());
    bool gap = false;
    for (const QChar c : s) {
        if (c == QLatin1Char('_') || c == QLatin1Char('-') || jsSpace(c)) { gap = true; continue; }
        if (gap && !out.isEmpty()) out += QLatin1Char(' ');
        gap = false;
        out += c;
    }
    return out.toLower();
}

QString stripExtension(const QString& s) {   // /\.[^.]+$/ (the ANFS titleOf / joinKey rule)
    const qsizetype dot = s.lastIndexOf(QLatin1Char('.'));
    return dot >= 0 && dot < s.size() - 1 ? s.left(dot) : s;
}

QString norm(const QString& path) { return ini::normalizePath(path.trimmed()); }

// "D:\a\" -> "d:\a\" (case-folded, exactly one trailing separator): what a path under the folder starts with.
QString folderPrefix(const QString& folder) {
    QString k = pathKey(folder);
    while (k.endsWith(QLatin1Char('\\')) && k.size() > 1) k.chop(1);
    return k.isEmpty() ? QString() : k + QLatin1Char('\\');
}

// The root a path hangs off: "D:\", or "\\host\share\" (cached first, so a dead host costs one probe, not one per file).
QString rootOf(const QString& normalised) {
    if (normalised.startsWith(QLatin1String("\\\\"))) {
        const qsizetype host = normalised.indexOf(QLatin1Char('\\'), 2);
        if (host < 0) return QString();
        const qsizetype share = normalised.indexOf(QLatin1Char('\\'), host + 1);
        return share < 0 ? normalised + QLatin1Char('\\') : normalised.left(share + 1);
    }
    return normalised.size() >= 3 ? normalised.left(3) : QString();
}

// Windows wildcards (FindFirstFile): '*' any run, '?' one character, case-insensitive; '[' is literal.
bool wildMatch(const QString& name, const QString& mask) {
    const QString n = name.toCaseFolded(), m = mask.toCaseFolded();
    qsizetype i = 0, j = 0, star = -1, mark = 0;
    while (i < n.size()) {
        if (j < m.size() && (m.at(j) == QLatin1Char('?') || m.at(j) == n.at(i))) { ++i; ++j; }
        else if (j < m.size() && m.at(j) == QLatin1Char('*')) { star = j++; mark = i; }
        else if (star >= 0) { j = star + 1; i = ++mark; }
        else return false;
    }
    while (j < m.size() && m.at(j) == QLatin1Char('*')) ++j;
    return j == m.size();
}

// Trigrams of a join key with punctuation read as spaces, packed (3 UTF-16 units in 48 bits), sorted, unique.
QList<quint64> trigrams(const QString& key) {
    QString s(QLatin1Char(' '));
    bool gap = true;
    for (const QChar c : key) {
        if (c.isLetterOrNumber()) { s += c; gap = false; }
        else if (!gap) { s += QLatin1Char(' '); gap = true; }
    }
    if (!gap) s += QLatin1Char(' ');
    QList<quint64> out;
    if (s.size() < 3 || s.trimmed().isEmpty()) return out;
    out.reserve(s.size());
    for (qsizetype i = 0; i + 2 < s.size(); ++i)
        out << (quint64(s.at(i).unicode()) << 32 | quint64(s.at(i + 1).unicode()) << 16 | quint64(s.at(i + 2).unicode()));
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

double dice(const QList<quint64>& a, const QList<quint64>& b) {
    if (a.isEmpty() || b.isEmpty()) return 0;
    qsizetype i = 0, j = 0, both = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i] == b[j]) { ++both; ++i; ++j; }
        else if (a[i] < b[j]) ++i;
        else ++j;
    }
    return 2.0 * double(both) / double(a.size() + b.size());
}

const QRegularExpression& schemeRe() {
    static const QRegularExpression re(QStringLiteral("^([A-Za-z][A-Za-z0-9+.\\-]+)://"));
    return re;
}

Diagnostic diag(const QString& code, const Use& u, const QString& message) {
    Diagnostic d;
    d.line = u.line;
    d.col = u.col;
    d.code = code;
    d.severity = diagnosticInfo(code).severity;
    d.message = message;
    return d;
}

QString scanType(const QString& folder) {   // TYPE_RULES vocabulary -> a PlayPlay media type for POST /media/scan
    const QString t = logs::inferType(folder);
    return t == QLatin1String("unknown") ? QStringLiteral("program") : t;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Keys

QString joinKey(const QString& file) { return keyBody(stripExtension(file)); }
QString titleKey(const QString& title) { return keyBody(title); }
QString pathKey(const QString& path) { return norm(path).toCaseFolded(); }

QString fileName(const QString& path) {
    qsizetype end = path.size();
    while (end > 0 && (path.at(end - 1) == QLatin1Char('\\') || path.at(end - 1) == QLatin1Char('/'))) --end;
    qsizetype start = end;
    while (start > 0 && path.at(start - 1) != QLatin1Char('\\') && path.at(start - 1) != QLatin1Char('/')) --start;
    return path.mid(start, end - start);
}

QString parentFolder(const QString& path) {
    const QString p = norm(path);
    const qsizetype cut = p.lastIndexOf(QLatin1Char('\\'));
    if (cut < 0) return QString();
    if (p.startsWith(QLatin1String("\\\\")) && cut < 2) return QString();
    if (cut == 2 && p.at(1) == QLatin1Char(':')) return p.left(3);   // "D:\x" -> "D:\"
    return p.left(cut);
}

bool isAbsolute(const QString& path) {
    const QString p = norm(path);
    if (p.startsWith(QLatin1String("\\\\"))) return p.size() > 2;
    return p.size() >= 3 && p.at(0).isLetter() && p.at(1) == QLatin1Char(':') && p.at(2) == QLatin1Char('\\');
}

double similarity(const QString& keyA, const QString& keyB) {
    if (!keyA.isEmpty() && keyA == keyB) return 1.0;
    return dice(trigrams(keyA), trigrams(keyB));
}

// Copy of IngestManager::quickHash, playout/supervisor/ingestmanager.cpp:286 (12 lines). Keep it byte-for-byte the same
// algorithm: library records store this value as extra.hash with hashKind "quick".
QString quickHash(const QString& path, bool full) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    QCryptographicHash h(QCryptographicHash::Md5);
    if (full) { h.addData(&f); return QString::fromLatin1(h.result().toHex()); }
    const qint64 size = f.size(); const qint64 chunk = 4 * 1024 * 1024;
    h.addData(f.read(chunk));
    if (size > chunk * 2) { f.seek(size - chunk); h.addData(f.read(chunk)); }
    else if (size > chunk) { h.addData(f.read(size - chunk)); }
    h.addData(QByteArray::number(size));
    return QString::fromLatin1(h.result().toHex());
}

// ---------------------------------------------------------------------------------------------------------------------
// Options

Options Options::fromIni(const ini::MplayIni& ini, const QList<ini::PathRule>& pathMap) {
    Options o;
    o.pathMap = pathMap;
    o.substitutes = ini.substitutes;
    o.searchPaths = ini.searchPaths;
    o.searchSubDirs = ini.searchIncludeSubDirs;
    return o;
}

Options Options::fromJson(const QJsonObject& j) {
    Options o;
    const QJsonValue pm = j.value(QStringLiteral("pathMap"));
    if (pm.isArray()) {
        for (const QJsonValue& v : pm.toArray()) if (v.isObject()) o.pathMap << ini::PathRule::fromJson(v.toObject());
    } else if (pm.isObject()) {   // {"S:\\": "\\\\nas\\S\\"}
        const QJsonObject m = pm.toObject();
        for (auto it = m.begin(); it != m.end(); ++it) {
            ini::PathRule r;
            r.from = it.key();
            r.to = it.value().toString();
            r.origin = QStringLiteral("user");
            o.pathMap << r;
        }
    }
    const QJsonValue su = j.value(QStringLiteral("substitutes"));
    if (su.isArray()) {
        for (const QJsonValue& v : su.toArray()) {
            const QJsonObject s = v.toObject();
            if (!s.value(QStringLiteral("name")).toString().isEmpty())
                o.substitutes << ini::Substitute{s.value(QStringLiteral("name")).toString(), s.value(QStringLiteral("value")).toString()};
        }
    } else if (su.isObject()) {
        const QJsonObject m = su.toObject();
        for (auto it = m.begin(); it != m.end(); ++it) o.substitutes << ini::Substitute{it.key(), it.value().toString()};
    }
    for (const QJsonValue& v : j.value(QStringLiteral("searchPaths")).toArray())
        if (!v.toString().trimmed().isEmpty()) o.searchPaths << v.toString().trimmed();
    o.searchSubDirs = j.value(QStringLiteral("searchSubDirs")).toBool(o.searchSubDirs);
    o.useHash = j.value(QStringLiteral("hash")).toBool(o.useHash);
    o.probeFiles = j.value(QStringLiteral("probe")).toBool(o.probeFiles);
    o.fileBudgetMs = qBound(0, j.value(QStringLiteral("fileBudgetMs")).toInt(o.fileBudgetMs), 600000);
    o.maxCandidates = qBound(0, j.value(QStringLiteral("candidates")).toInt(o.maxCandidates), 20);
    o.minScore = qBound(0.0, j.value(QStringLiteral("minScore")).toDouble(o.minScore), 1.0);
    o.channelId = j.value(QStringLiteral("channel")).toString();
    return o;
}

QJsonObject Options::toJson() const {
    QJsonArray rules, subs;
    for (const ini::PathRule& r : pathMap) rules.append(r.toJson());
    for (const ini::Substitute& s : substitutes) subs.append(QJsonObject{{"name", s.name}, {"value", s.value}});
    QJsonObject o{{"pathMap", rules}, {"substitutes", subs}, {"searchPaths", QJsonArray::fromStringList(searchPaths)},
                  {"searchSubDirs", searchSubDirs}, {"hash", useHash}, {"probe", probeFiles}, {"fileBudgetMs", fileBudgetMs},
                  {"candidates", maxCandidates}, {"minScore", minScore}};
    if (!channelId.isEmpty()) o["channel"] = channelId;
    return o;
}

// ---------------------------------------------------------------------------------------------------------------------
// Results

QString resolvedByName(ResolvedBy b) {
    switch (b) {
    case ResolvedBy::Path: return QStringLiteral("path");
    case ResolvedBy::Name: return QStringLiteral("name");
    case ResolvedBy::Hash: return QStringLiteral("hash");
    case ResolvedBy::None: break;
    }
    return QStringLiteral("none");
}

QJsonObject Candidate::toJson() const {
    QJsonObject o{{"mediaId", mediaId}, {"path", path}, {"title", title}, {"score", std::round(score * 1000.0) / 1000.0}, {"why", why}};
    if (underFolder) o["underFolder"] = true;
    if (!live) o["stale"] = true;
    return o;
}

QJsonObject Resolution::toJson() const {
    QJsonArray cands;
    for (const Candidate& c : candidates) cands.append(c.toJson());
    QJsonObject o{{"path", path.isEmpty() ? input : path}, {"mediaId", mediaId}, {"resolvedBy", resolvedByName(by)}, {"candidates", cands},
                  {"input", input}, {"kind", kind}, {"mapped", mapped}, {"type", type}};
    if (rule >= 0) o["rule"] = rule;
    if (!via.isEmpty()) o["via"] = via;
    if (resolved() && !media.title.isEmpty()) o["title"] = media.title;
    if (typeDerived) o["typeDerived"] = true;
    if (durationSec >= 0) o["durationSec"] = durationSec;
    if (forcedDurationSec >= 0) o["forcedDurationSec"] = forcedDurationSec;
    if (ambiguous) o["ambiguous"] = true;
    if (pickedByFolder) o["pickedByFolder"] = true;
    if (nameMatches > 1) o["nameMatches"] = nameMatches;
    if (duplicates > 0) o["duplicates"] = duplicates;
    if (hashMatches > 0) o["hashMatches"] = hashMatches;
    if (archived) o["archived"] = true;
    if (missingFile) o["missingFile"] = true;
    if (fileExists >= 0) o["fileExists"] = fileExists == 1;
    if (!hashSkipped.isEmpty()) o["hashSkipped"] = hashSkipped;
    if (!unknownSubstitutes.isEmpty()) o["unknownSubstitutes"] = QJsonArray::fromStringList(unknownSubstitutes);
    return o;
}

QJsonObject FolderCoverage::toJson() const {
    QJsonObject o{{"directory", directory}, {"folder", folder}, {"mapped", mapped}, {"recursive", recursive}, {"media", media},
                  {"playable", playable}, {"durationSec", std::round(durationSec * 1000.0) / 1000.0},
                  {"avgSec", std::round(avgSec * 1000.0) / 1000.0}, {"sample", QJsonArray::fromStringList(sample)}};
    if (!mask.isEmpty()) o["mask"] = mask;
    if (rule >= 0) o["rule"] = rule;
    if (searchTerm) o["searchTerm"] = true;
    if (maxAgeHours >= 0) o["maxAgeHours"] = maxAgeHours;
    if (fresh >= 0) o["fresh"] = fresh;
    if (unmappedMedia >= 0) o["unmappedMedia"] = unmappedMedia;
    if (exists >= 0) o["exists"] = exists == 1;
    QJsonArray actions;
    if (empty() && !mapped.isEmpty()) {
        const QString type = scanType(mapped);
        actions.append(QJsonObject{{"action", "scan"}, {"method", "POST"}, {"path", "/api/v1/media/scan"},
                                   {"body", QJsonObject{{"folder", mapped}, {"type", type}}}});
        actions.append(QJsonObject{{"action", "watch"}, {"method", "PUT"}, {"path", "/api/v1/ingest/watch"},
                                   {"folder", QJsonObject{{"path", mapped}, {"recursive", recursive}, {"type", type}, {"enabled", true}}},
                                   {"note", "add this folder to the list GET /api/v1/ingest/watch returns, then PUT the whole list"}});
    }
    o["actions"] = actions;
    return o;
}

// ---------------------------------------------------------------------------------------------------------------------
// Resolver

namespace {
struct Rec {
    QString pathKey, dirKey, fileKey;      // case-folded, normalised
    QString joinFile, joinFile2, joinTitle;
    QString qbPath;                        // QueryBlocks::matchesFolder's form of the path
    QString quick;                         // extra.hash when hashKind is "quick"
    bool live = true;
};
} // namespace

struct Resolver::Impl {
    Options o;
    QList<ini::PathRule> rules;
    QList<sched::MediaRecord> media;
    QList<Rec> recs;
    QHash<QString, QList<int>> byPath, byFile, byJoinFile, byJoinTitle, byHash;
    QHash<QString, QString> subs;          // case-folded name -> value
    QStringList searchFolders;             // mapped, normalised, no trailing separator (roots keep "D:\")
    qint64 nowMs = 0;

    // Similarity index, built on the first similar() call (only unresolved lines need it).
    mutable QMutex simMutex;
    mutable bool simBuilt = false;
    mutable QStringList simKeys;           // distinct filename / title keys
    mutable QList<QList<int>> simRecs;     // records per key
    mutable QHash<quint64, QList<int>> simPost;   // trigram -> key indexes
    mutable QList<QList<quint64>> simTri;         // trigrams per key (scored without rebuilding them)
    mutable std::vector<int> simCount;            // per-key scratch counts for similar()

    // File-system caches and budget.
    mutable QMutex fsMutex;
    mutable QHash<QString, bool> rootOk, dirOk;
    mutable QHash<QString, int> fileOk;
    mutable QHash<QString, QString> hashes;   // path key -> quick hash ("" unreadable)
    mutable qint64 fsSpentMs = 0;

    mutable QMutex folderMutex;
    mutable QHash<QString, FolderCoverage> folderCache;

    // Lower is better: live, then the earliest addedAtMs (0 = unknown, last), then the smallest id.
    bool better(int a, int b) const {
        if (recs[a].live != recs[b].live) return recs[a].live;
        const qint64 aa = media[a].addedAtMs > 0 ? media[a].addedAtMs : std::numeric_limits<qint64>::max();
        const qint64 ab = media[b].addedAtMs > 0 ? media[b].addedAtMs : std::numeric_limits<qint64>::max();
        if (aa != ab) return aa < ab;
        return media[a].id < media[b].id;
    }
    int best(const QList<int>& hits) const {
        int b = -1;
        for (int i : hits) if (b < 0 || better(i, b)) b = i;
        return b;
    }
    QList<int> preferLive(const QList<int>& hits) const {
        QList<int> live;
        for (int i : hits) if (recs[i].live) live << i;
        return live.isEmpty() ? hits : live;
    }
    struct File { int rec; int records; };
    // Hits grouped into files (one per path key), each with its preferred record, in path-key order.
    QList<File> files(const QList<int>& hits) const {
        QMap<QString, QList<int>> byKey;
        for (int i : hits) byKey[recs[i].pathKey] << i;
        QList<File> out;
        for (auto it = byKey.cbegin(); it != byKey.cend(); ++it) out << File{best(it.value()), int(it.value().size())};
        return out;
    }
    bool under(int rec, const QString& prefix) const { return !prefix.isEmpty() && recs[rec].pathKey.startsWith(prefix); }

    bool budgetLeft() const { return fsSpentMs < o.fileBudgetMs; }
    // 1 / 0, or -1 when not probed (relative path, file access off, budget spent). Locked by the caller.
    int probeDirLocked(const QString& normalisedDir) const {
        if (!isAbsolute(normalisedDir)) return -1;
        const QString root = rootOf(normalisedDir), rk = root.toCaseFolded(), dk = normalisedDir.toCaseFolded();
        if (auto it = dirOk.constFind(dk); it != dirOk.cend()) return it.value() ? 1 : 0;
        if (!root.isEmpty()) {
            auto it = rootOk.constFind(rk);
            if (it == rootOk.cend()) {
                if (!budgetLeft()) return -1;
                QElapsedTimer t; t.start();
                it = rootOk.insert(rk, QFileInfo(root).isDir());
                fsSpentMs += t.elapsed();
            }
            if (!it.value()) { dirOk.insert(dk, false); return 0; }
        }
        if (!budgetLeft()) return -1;
        QElapsedTimer t; t.start();
        const bool ok = QFileInfo(normalisedDir).isDir();
        fsSpentMs += t.elapsed();
        dirOk.insert(dk, ok);
        return ok ? 1 : 0;
    }
    int probeFileLocked(const QString& normalisedFile) const {
        if (!isAbsolute(normalisedFile)) return -1;
        const QString fk = normalisedFile.toCaseFolded();
        if (auto it = fileOk.constFind(fk); it != fileOk.cend()) return it.value();
        const int dir = probeDirLocked(parentFolder(normalisedFile));
        if (dir < 0) return -1;
        int ok = 0;
        if (dir == 1) {
            if (!budgetLeft()) return -1;
            QElapsedTimer t; t.start();
            ok = QFileInfo(normalisedFile).isFile() ? 1 : 0;
            fsSpentMs += t.elapsed();
        }
        fileOk.insert(fk, ok);
        return ok;
    }

    void buildSimilarity() const {
        if (simBuilt) return;
        QHash<QString, int> keyIndex;
        auto add = [&](const QString& key, int rec) {
            if (key.isEmpty()) return;
            auto it = keyIndex.constFind(key);
            int k;
            if (it == keyIndex.cend()) {
                k = int(simKeys.size());
                keyIndex.insert(key, k);
                simKeys << key;
                simRecs << QList<int>();
                simTri << trigrams(key);
                for (quint64 t : simTri.last()) simPost[t] << k;
            } else {
                k = it.value();
            }
            if (simRecs[k].isEmpty() || simRecs[k].last() != rec) simRecs[k] << rec;
        };
        for (int i = 0; i < recs.size(); ++i) {
            if (recs[i].pathKey.isEmpty()) continue;
            add(recs[i].joinFile, i);
            add(recs[i].joinFile2, i);
            add(recs[i].joinTitle, i);
        }
        simBuilt = true;
    }
};

Resolver::Resolver(const QList<sched::MediaRecord>& media, const Options& o) : d(std::make_unique<Impl>()) {
    d->o = o;
    d->rules = ini::normalizeRules(o.pathMap);
    d->media = media;
    d->nowMs = o.nowMs > 0 ? o.nowMs : QDateTime::currentMSecsSinceEpoch();
    for (const ini::Substitute& s : o.substitutes) {
        QString name = s.name.trimmed();
        if (name.size() >= 2 && name.startsWith(QLatin1Char('%')) && name.endsWith(QLatin1Char('%'))) name = name.mid(1, name.size() - 2);
        if (!name.isEmpty() && !d->subs.contains(name.toCaseFolded())) d->subs.insert(name.toCaseFolded(), s.value);
    }
    d->recs.reserve(media.size());
    for (int i = 0; i < media.size(); ++i) {
        const sched::MediaRecord& m = media[i];
        Rec r;
        const QString n = norm(m.path);
        r.live = !m.archived && !m.extra.value(QStringLiteral("missing")).toBool(false);
        if (!n.isEmpty()) {
            r.pathKey = n.toCaseFolded();
            r.dirKey = parentFolder(n).toCaseFolded();
            r.fileKey = fileName(n).toCaseFolded();
            r.joinFile = joinKey(fileName(n));
            const QString stored = m.extra.value(QStringLiteral("filename")).toString();   // ingest keeps fi.fileName()
            if (!stored.isEmpty() && joinKey(stored) != r.joinFile) r.joinFile2 = joinKey(stored);
            r.joinTitle = titleKey(m.title);
            r.qbPath = sched::QueryBlocks::norm(QDir::fromNativeSeparators(m.path));
            if (m.extra.value(QStringLiteral("hashKind")).toString() == QLatin1String("quick"))
                r.quick = m.extra.value(QStringLiteral("hash")).toString().toLower();
        }
        d->recs << r;
        if (r.pathKey.isEmpty()) continue;   // a record without a path can never be the file a playlist names
        d->byPath[r.pathKey] << i;
        if (!r.fileKey.isEmpty()) d->byFile[r.fileKey] << i;
        if (!r.joinFile.isEmpty()) d->byJoinFile[r.joinFile] << i;
        if (!r.joinFile2.isEmpty()) d->byJoinFile[r.joinFile2] << i;
        if (!r.joinTitle.isEmpty()) d->byJoinTitle[r.joinTitle] << i;
        if (!r.quick.isEmpty()) d->byHash[r.quick] << i;
    }
    QSet<QString> seen;
    for (const QString& sp : o.searchPaths) {
        QString m = norm(ini::mapPath(substitute(sp), d->rules));
        while (m.endsWith(QLatin1Char('\\')) && !(m.size() == 3 && m.at(1) == QLatin1Char(':')) && m.size() > 2) m.chop(1);
        if (m.isEmpty() || seen.contains(m.toCaseFolded())) continue;
        seen.insert(m.toCaseFolded());
        d->searchFolders << m;
    }
}

Resolver Resolver::fromStore(const sched::ScheduleStore& store, const Options& o) { return Resolver(store.allMedia(true), o); }
Resolver::Resolver(Resolver&&) noexcept = default;
Resolver& Resolver::operator=(Resolver&&) noexcept = default;
Resolver::~Resolver() = default;

const Options& Resolver::options() const { return d->o; }
const QList<ini::PathRule>& Resolver::rules() const { return d->rules; }
const QList<sched::MediaRecord>& Resolver::media() const { return d->media; }
int Resolver::size() const { return int(d->media.size()); }

QString Resolver::substitute(const QString& path, QStringList* unknown) const {
    // One pass, left to right: a value is never searched again, so a table cannot loop. A token is %name% with a name
    // holding no separator ("%%", "50%\x\" and "%a\b%" are text); unknown names stay as written (a %m-dd-yy% search
    // term is the importer's MP020).
    auto tokenName = [](QStringView n) {
        if (n.isEmpty()) return false;
        for (const QChar c : n)
            if (c == QLatin1Char('\\') || c == QLatin1Char('/') || c == QLatin1Char(':') || c == QLatin1Char('\t') || c == QLatin1Char('\r') || c == QLatin1Char('\n'))
                return false;
        return true;
    };
    QString out;
    out.reserve(path.size());
    qsizetype i = 0;
    while (i < path.size()) {
        const qsizetype a = path.indexOf(QLatin1Char('%'), i);
        if (a < 0) { out += path.mid(i); break; }
        const qsizetype b = path.indexOf(QLatin1Char('%'), a + 1);
        if (b < 0) { out += path.mid(i); break; }
        out += path.mid(i, a - i);
        const QString name = path.mid(a + 1, b - a - 1);
        if (!tokenName(name)) { out += QLatin1Char('%'); i = a + 1; continue; }   // a lone '%': the next one may open a token
        const auto it = d->subs.constFind(name.toCaseFolded());
        if (it != d->subs.cend()) out += it.value();
        else {
            if (unknown && !unknown->contains(name)) *unknown << name;
            out += path.mid(a, b - a + 1);
        }
        i = b + 1;
    }
    return norm(out);
}

QString Resolver::mapPath(const QString& playlistPath, int* rule) const {
    return norm(ini::mapPath(substitute(playlistPath), d->rules, rule));
}

Resolution Resolver::resolve(const Entry& e) const {
    Resolution r = resolve(e.path, e.forcedDurationSec);
    return r;
}

Resolution Resolver::resolve(const QString& line) const {
    const Entry e = parseLine(line);
    if (e.kind == LineKind::Media) return resolve(e.path, e.forcedDurationSec);
    return resolve(line.trimmed(), -1);
}

Resolution Resolver::resolve(const QString& input, double forcedDurationSec) const {
    Resolution r;
    r.input = input;
    r.forcedDurationSec = forcedDurationSec >= 0 ? forcedDurationSec : -1;
    const QString t = input.trimmed();
    if (t.isEmpty()) {
        r.kind = QStringLiteral("empty");
        r.type = QStringLiteral("unknown");
        r.typeDerived = true;
        return r;
    }
    if (const auto m = schemeRe().match(t); m.hasMatch()) {   // mp://Camera 1, http://..., srt://...
        r.kind = m.captured(1).compare(QLatin1String("mp"), Qt::CaseInsensitive) == 0 ? QStringLiteral("live") : QStringLiteral("url");
        r.path = r.mapped = t;
        r.type = r.kind == QLatin1String("live") ? QStringLiteral("live") : QStringLiteral("unknown");
        r.typeDerived = true;
        r.durationSec = r.forcedDurationSec;
        return r;
    }

    r.path = substitute(t, &r.unknownSubstitutes);
    r.mapped = norm(ini::mapPath(r.path, d->rules, &r.rule));
    const QString mk = r.mapped.toCaseFolded(), pk = r.path.toCaseFolded();
    const QString name = fileName(r.mapped);
    const QString mappedFolder = parentFolder(r.mapped), playlistFolder = parentFolder(r.path);

    auto take = [&](int rec, ResolvedBy by, const QString& via, int sameFile) {
        r.by = by;
        r.via = via;
        r.mediaId = d->media[rec].id;
        r.media = d->media[rec];
        r.archived = d->media[rec].archived;
        r.missingFile = d->media[rec].extra.value(QStringLiteral("missing")).toBool(false);
        r.duplicates = qMax(0, sameFile - 1);
    };
    auto exact = [&](const QString& key, const QString& via) {
        const auto it = d->byPath.constFind(key);
        if (it == d->byPath.cend() || it.value().isEmpty()) return false;
        take(d->best(it.value()), ResolvedBy::Path, via, int(it.value().size()));
        return true;
    };

    // 2. exact library path: mapped, then as written, then the search paths
    bool done = exact(mk, QStringLiteral("mapped")) || (pk != mk && exact(pk, QStringLiteral("playlist")));
    if (!done && !name.isEmpty()) {
        for (const QString& sp : d->searchFolders) {
            const QString cand = sp.endsWith(QLatin1Char('\\')) ? sp + name : sp + QLatin1Char('\\') + name;
            if (exact(cand.toCaseFolded(), QStringLiteral("searchPath"))) { done = true; break; }
        }
        if (!done && d->o.searchSubDirs && !d->searchFolders.isEmpty()) {
            QList<int> hits;
            for (int i : d->byFile.value(name.toCaseFolded()))
                for (const QString& sp : d->searchFolders)
                    if (d->under(i, folderPrefix(sp))) { hits << i; break; }
            const auto fs = d->files(d->preferLive(hits));
            if (fs.size() == 1) { take(fs[0].rec, ResolvedBy::Path, QStringLiteral("searchPath"), fs[0].records); done = true; }
        }
    }

    // 3. filename join key, then the title key; unique files only
    QStringList preferFolders;
    for (const QString& f : {mappedFolder, playlistFolder}) if (!f.isEmpty() && !preferFolders.contains(f, Qt::CaseInsensitive)) preferFolders << f;
    for (const QString& sp : d->searchFolders) if (!preferFolders.contains(sp, Qt::CaseInsensitive)) preferFolders << sp;
    auto underAny = [&](int rec) {
        for (const QString& f : preferFolders) if (d->under(rec, folderPrefix(f))) return true;
        return false;
    };
    const QString jk = joinKey(name);
    if (!done && !jk.isEmpty()) {
        const std::pair<const QHash<QString, QList<int>>*, QString> tiers[] = {{&d->byJoinFile, QStringLiteral("filename")},
                                                                               {&d->byJoinTitle, QStringLiteral("title")}};
        for (const auto& [index, via] : tiers) {
            const QList<int> hits = index->value(jk);
            if (hits.isEmpty()) continue;
            const auto fs = d->files(d->preferLive(hits));
            r.nameMatches = int(fs.size());
            if (fs.size() == 1) { take(fs[0].rec, ResolvedBy::Name, via, fs[0].records); done = true; break; }
            // Several files: the one under the mapped folder, then the playlist folder, then a search path.
            r.ambiguous = true;
            for (const QString& f : preferFolders) {
                QList<Impl::File> in;
                for (const auto& file : fs) if (d->under(file.rec, folderPrefix(f))) in << file;
                if (in.size() == 1) { take(in[0].rec, ResolvedBy::Name, via, in[0].records); r.pickedByFolder = true; done = true; break; }
                if (in.size() > 1) break;   // still ambiguous inside the closest folder: a wider one cannot settle it
            }
            QList<Candidate> cands;
            for (const auto& file : fs) {
                const sched::MediaRecord& m = d->media[file.rec];
                cands << Candidate{m.id, m.path, m.title, 1.0, via, underAny(file.rec), d->recs[file.rec].live};
            }
            std::stable_sort(cands.begin(), cands.end(), [](const Candidate& a, const Candidate& b) {
                if (a.underFolder != b.underFolder) return a.underFolder;
                if (a.live != b.live) return a.live;
                return a.path.compare(b.path, Qt::CaseInsensitive) < 0;
            });
            r.candidates = cands.mid(0, 20);
            break;   // a filename-key ambiguity does not fall through to the titles
        }
    }

    // 4. quick hash of the mapped file, when it is readable here
    if (!done) {
        // No record carries a quick hash (ingest set to fullHash, or records older than hashing): nothing can match, so
        // the file is not read (up to 8 MiB per unresolved line off the playout NAS). The existence probe still runs.
        const QString noHash = !d->o.useHash ? QStringLiteral("off") : d->byHash.isEmpty() ? QStringLiteral("noLibraryHashes") : QString();
        const bool access = noHash.isEmpty() || d->o.probeFiles;
        if (access && isAbsolute(r.mapped)) {
            QMutexLocker lock(&d->fsMutex);
            r.fileExists = d->probeFileLocked(r.mapped);
            if (!noHash.isEmpty()) r.hashSkipped = noHash;
            else if (r.fileExists < 0) r.hashSkipped = QStringLiteral("budget");
            else if (r.fileExists == 0) r.hashSkipped = QStringLiteral("missing");
            else {
                auto it = d->hashes.constFind(mk);
                if (it == d->hashes.cend()) {
                    if (!d->budgetLeft()) r.hashSkipped = QStringLiteral("budget");
                    else {
                        QElapsedTimer tm; tm.start();
                        it = d->hashes.insert(mk, quickHash(r.mapped).toLower());
                        d->fsSpentMs += tm.elapsed();
                    }
                }
                if (it != d->hashes.cend()) {
                    r.hash = it.value();
                    if (r.hash.isEmpty()) r.hashSkipped = QStringLiteral("unreadable");
                }
            }
        } else {
            r.hashSkipped = !noHash.isEmpty() ? noHash : QStringLiteral("notAbsolute");
        }
        if (!r.hash.isEmpty()) {
            const auto fs = d->files(d->preferLive(d->byHash.value(r.hash)));
            if (!fs.isEmpty()) {
                // Same bytes: any of them plays the same programme. Prefer one under the folders the playlist names.
                const Impl::File* pick = &fs[0];
                for (const auto& f : fs) {
                    const bool a = underAny(f.rec), b = underAny(pick->rec);
                    if (a != b ? a : d->better(f.rec, pick->rec)) pick = &f;
                }
                take(pick->rec, ResolvedBy::Hash, QStringLiteral("quick"), pick->records);
                r.hashMatches = int(fs.size());
                r.ambiguous = false;
                r.candidates.clear();
                done = true;
            }
        }
    }

    // 5. nothing: the closest names (an ambiguous line keeps its hits)
    if (!done && !r.ambiguous) r.candidates = similar(name, d->o.maxCandidates, mappedFolder);

    if (r.resolved()) {
        r.type = r.media.type;
        r.durationSec = r.media.durationSec > 0 ? r.media.durationSec : r.forcedDurationSec;
        r.fileExists = -1;
        r.hashSkipped.clear();
    } else {
        r.type = logs::inferType(r.mapped);
        r.typeDerived = true;
        r.durationSec = r.forcedDurationSec;
    }
    return r;
}

QList<Candidate> Resolver::similar(const QString& file, int n, const QString& preferFolder) const {
    QList<Candidate> out;
    const QString q = joinKey(fileName(file));
    if (n <= 0 || q.isEmpty()) return out;
    const QList<quint64> qt = trigrams(q);
    if (qt.isEmpty()) return out;

    QHash<int, double> scoreByRec;   // best score per record
    {
        QMutexLocker lock(&d->simMutex);
        d->buildSimilarity();
        // Shared-trigram counts per key. Trigrams held by a large share of the keys are skipped (they say little and
        // would make every query walk the library); the shortlist is then scored exactly.
        const qsizetype cap = qMax<qsizetype>(2000, d->simKeys.size() / 4);
        std::vector<int>& count = d->simCount;   // scratch, all zero between calls (reset through `touched`)
        count.resize(size_t(d->simKeys.size()), 0);
        QList<int> touched;
        bool any = false;
        for (int pass = 0; pass < 2 && !any; ++pass) {
            for (quint64 t : qt) {
                const auto it = d->simPost.constFind(t);
                if (it == d->simPost.cend() || (pass == 0 && it.value().size() > cap)) continue;
                any = true;
                for (int k : it.value()) if (count[size_t(k)]++ == 0) touched << k;
            }
        }
        QList<std::pair<int, int>> shortlist;   // (count, key)
        shortlist.reserve(touched.size());
        for (int k : touched) { shortlist.append({count[size_t(k)], k}); count[size_t(k)] = 0; }
        const qsizetype keep = qMin<qsizetype>(shortlist.size(), 256);
        std::partial_sort(shortlist.begin(), shortlist.begin() + keep, shortlist.end(), [](const auto& a, const auto& b) {
            return a.first != b.first ? a.first > b.first : a.second < b.second;
        });
        for (qsizetype i = 0; i < keep; ++i) {
            const int k = shortlist[i].second;
            const double s = d->simKeys[k] == q ? 1.0 : dice(qt, d->simTri[k]);
            if (s < d->o.minScore || s <= 0) continue;
            for (int rec : d->simRecs[k]) {
                auto it = scoreByRec.find(rec);
                if (it == scoreByRec.end()) scoreByRec.insert(rec, s);
                else if (s > it.value()) it.value() = s;
            }
        }
    }
    // One candidate per file (path key): its preferred record, carrying the best score of the file's records. Only the
    // top n become Candidates (a shortlist of 256 similar keys can name hundreds of files).
    struct Pick { double score; int rec; bool under, live; };
    QHash<QString, QList<int>> byFile;
    for (auto it = scoreByRec.cbegin(); it != scoreByRec.cend(); ++it) byFile[d->recs[it.key()].pathKey] << it.key();
    const QString prefix = folderPrefix(preferFolder);
    QList<Pick> picks;
    picks.reserve(byFile.size());
    for (auto it = byFile.cbegin(); it != byFile.cend(); ++it) {
        const int rec = d->best(it.value());
        double s = 0;
        for (int i : it.value()) s = qMax(s, scoreByRec.value(i));
        picks.append(Pick{s, rec, d->under(rec, prefix), d->recs[rec].live});
    }
    const qsizetype top = qMin<qsizetype>(picks.size(), n);
    std::partial_sort(picks.begin(), picks.begin() + top, picks.end(), [this](const Pick& a, const Pick& b) {
        if (std::fabs(a.score - b.score) > 1e-9) return a.score > b.score;
        if (a.under != b.under) return a.under;
        if (a.live != b.live) return a.live;
        const int c = d->media[a.rec].path.compare(d->media[b.rec].path, Qt::CaseInsensitive);
        return c != 0 ? c < 0 : d->media[a.rec].id < d->media[b.rec].id;
    });
    for (qsizetype i = 0; i < top; ++i) {
        const sched::MediaRecord& m = d->media[picks[i].rec];
        out << Candidate{m.id, m.path, m.title, picks[i].score, QStringLiteral("similar"), picks[i].under, picks[i].live};
    }
    return out;
}

FolderCoverage Resolver::folder(const InsertDirectorySpec& spec) const {
    return folder(spec.directory, spec.recursive(), spec.maxAgeHours);
}

FolderCoverage Resolver::folder(const QString& directory, bool recursive, double maxAgeHours) const {
    FolderCoverage c;
    QStringList unknown;
    c.directory = substitute(directory, &unknown);
    c.recursive = recursive;
    c.maxAgeHours = maxAgeHours > 0 ? maxAgeHours : -1;
    // A mask (or a %search term%) in the last component: Dir\Name*.ext, Dir\%m-dd-yy%.
    const QString last = fileName(c.directory);
    const bool wild = last.contains(QLatin1Char('*')) || last.contains(QLatin1Char('?'));
    if (wild || last.contains(QLatin1Char('%'))) {
        c.folder = parentFolder(c.directory);
        if (!last.contains(QLatin1Char('%'))) c.mask = last;
    } else {
        c.folder = c.directory;
    }
    c.searchTerm = c.directory.contains(QLatin1Char('%'));
    c.mapped = norm(ini::mapPath(c.folder, d->rules, &c.rule));
    if (c.mapped.isEmpty()) return c;

    // Everything the result depends on: "D:\News" and "D:\News\%m-dd-yy%.mpg" share folder and mask but not searchTerm
    // (MP020 at import), and the folder as written decides rule / unmappedMedia.
    const QString cacheKey = c.mapped.toCaseFolded() + QLatin1Char('|') + c.mask.toCaseFolded() + QLatin1Char('|')
                             + QString::number(int(recursive)) + QLatin1Char('|') + QString::number(c.maxAgeHours) + QLatin1Char('|')
                             + QString::number(int(c.searchTerm)) + QLatin1Char('|') + c.folder.toCaseFolded();
    {
        QMutexLocker lock(&d->folderMutex);
        if (auto it = d->folderCache.constFind(cacheKey); it != d->folderCache.cend()) {
            FolderCoverage hit = it.value();
            hit.directory = c.directory;
            return hit;
        }
    }

    // QueryBlocks::matchesFolder, with the record side precomputed (a folder per !InsertDirectory times the library).
    auto matcher = [&](const QString& folder) {
        sched::FolderBlock fb;
        fb.folder = folder;
        fb.recursive = recursive;
        const QString want = sched::QueryBlocks::norm(QDir::fromNativeSeparators(folder));
        const bool absolute = want.contains(QLatin1Char('/')) || want.contains(QLatin1Char(':'));
        const QString prefix = want.endsWith(QLatin1Char('/')) ? want : want + QLatin1Char('/');
        return [this, fb, want, absolute, prefix, recursive](int i) {
            if (want.isEmpty()) return false;
            if (!absolute) return sched::QueryBlocks::matchesFolder(d->media[i], fb);
            const QString& p = d->recs[i].qbPath;
            if (!p.startsWith(prefix)) return false;
            return recursive || !QStringView(p).mid(prefix.size()).contains(QLatin1Char('/'));
        };
    };
    const auto inMapped = matcher(c.mapped);
    sched::QueryContext ctx;
    ctx.channelId = d->o.channelId;
    ctx.nowMs = d->nowMs;
    sched::Conditions age;
    if (c.maxAgeHours > 0) { age.maxAgeDays = c.maxAgeHours / 24.0; age.ageField = QStringLiteral("added"); c.fresh = 0; }
    QStringList titles;
    for (int i = 0; i < d->media.size(); ++i) {
        const sched::MediaRecord& m = d->media[i];
        if (m.archived || d->recs[i].pathKey.isEmpty() || !inMapped(i)) continue;
        if (!c.mask.isEmpty() && !wildMatch(fileName(m.path), c.mask)) continue;
        ++c.media;
        if (!sched::QueryBlocks::eligible(m, ctx)) continue;
        ++c.playable;
        c.durationSec += m.durationSec;
        titles << (m.title.isEmpty() ? fileName(m.path) : m.title);
        if (c.maxAgeHours > 0 && age.matches(m, d->nowMs)) ++c.fresh;
    }
    c.avgSec = c.playable > 0 ? c.durationSec / c.playable : 0;
    std::sort(titles.begin(), titles.end(), [](const QString& a, const QString& b) { return a.compare(b, Qt::CaseInsensitive) < 0; });
    c.sample = titles.mid(0, 5);
    if (c.rule >= 0) {   // the folder as written: media there and none under the mapped folder means a wrong rule
        const QString written = norm(c.folder);
        const auto inWritten = matcher(written);
        c.unmappedMedia = 0;
        for (int i = 0; i < d->media.size(); ++i)
            if (!d->media[i].archived && !d->recs[i].pathKey.isEmpty() && inWritten(i)
                && (c.mask.isEmpty() || wildMatch(fileName(d->media[i].path), c.mask)))
                ++c.unmappedMedia;
    }
    // Only a folder with nothing playable is probed: exists feeds MP019 ("scan or watch it"), and a covered folder on a
    // slow NAS share should cost no file-system time.
    if (d->o.probeFiles && c.empty() && isAbsolute(c.mapped)) {
        QMutexLocker lock(&d->fsMutex);
        c.exists = d->probeDirLocked(c.mapped);
    }
    QMutexLocker lock(&d->folderMutex);
    d->folderCache.insert(cacheKey, c);
    return c;
}

// ---------------------------------------------------------------------------------------------------------------------
// Report

namespace {
QJsonArray linesOf(const QList<Use>& uses, int cap = 50) {
    QJsonArray a;
    for (const Use& u : uses) { if (a.size() >= cap) break; a.append(u.line); }
    return a;
}
QJsonArray eventsOf(const QList<Use>& uses, int cap = 50) {
    QJsonArray a;
    QSet<QString> seen;
    for (const Use& u : uses) {
        if (a.size() >= cap) break;
        const QString k = u.file + QLatin1Char('#') + QString::number(u.eventLine);
        if (seen.contains(k)) continue;
        seen.insert(k);
        QJsonObject e{{"line", u.eventLine}, {"stamp", u.event}};
        if (!u.file.isEmpty()) e["file"] = u.file;
        a.append(e);
    }
    return a;
}
QJsonArray filesOf(const QList<Use>& uses) {
    QStringList files;
    for (const Use& u : uses) if (!u.file.isEmpty() && !files.contains(u.file)) files << u.file;
    return QJsonArray::fromStringList(files);
}
QJsonArray candidatesOf(const QList<Candidate>& cs) {
    QJsonArray a;
    for (const Candidate& c : cs) a.append(c.toJson());
    return a;
}
} // namespace

void Report::add(const Resolution& r, const Use& u) {
    if (r.kind == QLatin1String("empty")) return;
    ++m_items;
    if (r.kind == QLatin1String("live")) { ++m_live; return; }
    if (r.kind == QLatin1String("url")) { ++m_urls; return; }
    ++m_by[int(r.by)];
    const QString key = r.mapped.toCaseFolded();
    auto it = m_fileIndex.constFind(key);
    if (it == m_fileIndex.cend()) {
        it = m_fileIndex.insert(key, int(m_files.size()));
        m_files << FileRow{r, {}, {}};
    }
    FileRow& row = m_files[it.value()];
    row.uses << u;
    if (!row.forced.contains(r.forcedDurationSec)) row.forced << r.forcedDurationSec;
}

void Report::addFolder(const FolderCoverage& f, const Use& u) {
    // The same fields as Resolver::folder()'s cache (a row's searchTerm / rule / unmappedMedia hold for all its uses);
    // folders are compared as prefixes, so "D:\X\" and "d:\x" stay one row.
    const QString key = folderPrefix(f.mapped) + QLatin1Char('|') + f.mask.toCaseFolded() + QLatin1Char('|') + QString::number(int(f.recursive))
                        + QLatin1Char('|') + QString::number(f.maxAgeHours) + QLatin1Char('|') + QString::number(int(f.searchTerm))
                        + QLatin1Char('|') + folderPrefix(f.folder);
    auto it = m_folderIndex.constFind(key);
    if (it == m_folderIndex.cend()) {
        it = m_folderIndex.insert(key, int(m_folders.size()));
        m_folders << FolderRow{f, {}};
    }
    m_folders[it.value()].uses << u;
}

QJsonArray Report::missing() const {
    QJsonArray out;
    for (const FileRow& row : m_files) {
        const Resolution& r = row.r;
        if (r.resolved()) continue;
        QJsonObject o{{"path", r.path}, {"mapped", r.mapped}, {"uses", int(row.uses.size())}, {"lines", linesOf(row.uses)},
                      {"events", eventsOf(row.uses)}, {"type", r.type}, {"candidates", candidatesOf(r.candidates)}};
        const QJsonArray files = filesOf(row.uses);
        if (files.size() == 1) o["file"] = files.first();
        else if (!files.isEmpty()) o["files"] = files;
        if (r.rule >= 0) o["rule"] = r.rule;
        if (r.ambiguous) o["ambiguous"] = true;
        if (r.fileExists >= 0) o["exists"] = r.fileExists == 1;
        if (!r.hashSkipped.isEmpty()) o["hashSkipped"] = r.hashSkipped;
        if (r.durationSec >= 0) o["durationSec"] = r.durationSec;
        if (row.forced.size() > 1) {   // uses with different brackets: durationSec is the first use's
            QJsonArray fs;
            for (double f : row.forced) fs.append(f >= 0 ? QJsonValue(f) : QJsonValue(QJsonValue::Null));
            o["forcedDurationsSec"] = fs;
        }
        if (!r.unknownSubstitutes.isEmpty()) o["unknownSubstitutes"] = QJsonArray::fromStringList(r.unknownSubstitutes);
        out.append(o);
    }
    return out;
}

QJsonArray Report::ambiguous() const {
    QJsonArray out;
    for (const FileRow& row : m_files) {
        const Resolution& r = row.r;
        if (!r.ambiguous) continue;
        out.append(QJsonObject{{"path", r.path}, {"mapped", r.mapped}, {"picked", r.mediaId}, {"nameMatches", r.nameMatches},
                               {"uses", int(row.uses.size())}, {"lines", linesOf(row.uses)}, {"candidates", candidatesOf(r.candidates)}});
    }
    return out;
}

QJsonArray Report::folders() const {
    QJsonArray out;
    for (const FolderRow& row : m_folders) {
        QJsonObject o = row.f.toJson();
        o["uses"] = int(row.uses.size());
        o["lines"] = linesOf(row.uses);
        o["events"] = eventsOf(row.uses);
        const QJsonArray files = filesOf(row.uses);
        if (files.size() == 1) o["file"] = files.first();
        else if (!files.isEmpty()) o["files"] = files;
        out.append(o);
    }
    return out;
}

QJsonObject Report::stats() const {
    int missingFiles = 0, ambiguousFiles = 0, picked = 0, emptyFolders = 0, folderUses = 0;
    for (const FileRow& row : m_files) {
        if (!row.r.resolved()) ++missingFiles;
        if (row.r.ambiguous) ++ambiguousFiles;
        if (row.r.pickedByFolder) ++picked;
    }
    for (const FolderRow& row : m_folders) { if (row.f.empty()) ++emptyFolders; folderUses += int(row.uses.size()); }
    return QJsonObject{{"items", m_items}, {"resolved", m_by[1] + m_by[2] + m_by[3]},
                       {"byResolvedBy", QJsonObject{{"path", m_by[int(ResolvedBy::Path)]}, {"name", m_by[int(ResolvedBy::Name)]},
                                                    {"hash", m_by[int(ResolvedBy::Hash)]}, {"none", m_by[int(ResolvedBy::None)]}}},
                       {"live", m_live}, {"urls", m_urls}, {"files", int(m_files.size())}, {"missingFiles", missingFiles},
                       {"ambiguousFiles", ambiguousFiles}, {"pickedByFolder", picked}, {"folders", int(m_folders.size())},
                       {"emptyFolders", emptyFolders}, {"folderUses", folderUses}};
}

QList<Diagnostic> Report::diagnostics() const {
    QList<Diagnostic> out;
    for (const FileRow& row : m_files) {
        const Resolution& r = row.r;
        QString msg;
        QString code;
        if (r.ambiguous) {
            code = QStringLiteral("MP018");
            msg = r.pickedByFolder
                      ? QStringLiteral("ambiguous filename match for %1: %2 library files share the name; took %3 (under the playlist's folder)")
                            .arg(fileName(r.mapped), QString::number(r.nameMatches), r.media.path)
                      : QStringLiteral("ambiguous filename match for %1: %2 library files share the name; not resolved")
                            .arg(fileName(r.mapped), QString::number(r.nameMatches));
        } else if (!r.resolved()) {
            code = QStringLiteral("MP005");
            msg = QStringLiteral("not in the library: %1").arg(r.mapped);
            if (r.fileExists == 1) msg += QStringLiteral(" (the file exists here: ingest it)");
            else if (r.fileExists == 0) msg += QStringLiteral(" (the file is not reachable from this station)");
            if (!r.candidates.isEmpty()) msg += QStringLiteral("; closest: %1").arg(r.candidates.first().path);
        } else {
            continue;
        }
        for (const Use& u : row.uses) out << diag(code, u, msg);
    }
    for (const FolderRow& row : m_folders) {
        if (!row.f.empty()) continue;
        QString msg = QStringLiteral("no playable library media under %1").arg(row.f.mapped);
        if (!row.f.mask.isEmpty()) msg += QStringLiteral(" matching %1").arg(row.f.mask);
        if (row.f.media > 0) msg += QStringLiteral(" (%1 records, none playable)").arg(row.f.media);
        if (row.f.exists == 1) msg += QStringLiteral("; the folder exists here: scan or watch it");
        else if (row.f.unmappedMedia > 0) msg += QStringLiteral("; %1 records sit under the folder as written: check the path rule").arg(row.f.unmappedMedia);
        for (const Use& u : row.uses) out << diag(QStringLiteral("MP019"), u, msg);
    }
    std::stable_sort(out.begin(), out.end(), [](const Diagnostic& a, const Diagnostic& b) {
        return a.line != b.line ? a.line < b.line : a.col < b.col;
    });
    return out;
}

QJsonObject Report::toJson() const {
    QJsonArray ds;
    for (const Diagnostic& d : diagnostics()) ds.append(d.toJson());
    return QJsonObject{{"stats", stats()}, {"missing", missing()}, {"ambiguous", ambiguous()}, {"folders", folders()}, {"diagnostics", ds}};
}

Report resolveDocument(const Document& doc, const Resolver& r, const QString& file) {
    Report rep;
    QHash<int, int> eventOf;   // entry index -> event index
    for (int e = 0; e < doc.events.size(); ++e)
        for (int it : doc.events[e].items) eventOf.insert(it, e);
    QHash<QString, Resolution> cache;   // a file named on many lines resolves once
    for (int i = 0; i < doc.entries.size(); ++i) {
        const Entry& en = doc.entries[i];
        if (en.kind != LineKind::Media && en.kind != LineKind::Instruction) continue;
        Use u;
        u.file = file;
        u.line = en.line;
        u.col = 1;
        if (const auto ev = eventOf.constFind(i); ev != eventOf.cend()) {
            const Entry& h = doc.entries.at(doc.events.at(ev.value()).entry);
            u.eventLine = h.line;
            u.event = h.stamp.raw.isEmpty() ? formatStamp(h.stamp, !h.stamp.legacy) : h.stamp.raw;
        }
        if (en.kind == LineKind::Media) {
            const QString key = en.path + QLatin1Char('\x1f') + QString::number(en.forcedDurationSec);
            auto it = cache.constFind(key);
            if (it == cache.cend()) it = cache.insert(key, r.resolve(en));
            rep.add(it.value(), u);
        } else if (const auto spec = insertDirectorySpec(en)) {
            const Instruction ins = identify(en);
            if (ins.headCol > 0) u.col = ins.headCol;
            rep.addFolder(r.folder(*spec), u);
        }
    }
    return rep;
}

} // namespace mp::res
