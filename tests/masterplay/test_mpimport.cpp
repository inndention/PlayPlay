// MasterPlay import tests (the lane design notes §6). MP-06: the library resolver (playout/masterplay/mpresolver.h)
// against a temp ScheduleStore holding synthetic media: path rules (longest prefix, UNC), %substitutes%, the case-folded
// path index (case and separator variants, duplicate records of one file), search paths, the ANFS filename join key
// (unique hits only, the folder tie-break, live records over stale ones, title keys), the IngestManager quick hash
// (a 12-line copy, checked byte for byte against the algorithm), the missing report with its top-3 candidates and
// folder coverage (QueryBlocks::matchesFolder parity, masks, max age, MP019 with scan / watch actions).
// MP-07: the day timeline builder (playout/masterplay/mptimeline.h): candidates per weekday or date, priority ties
// (MP015), the cyclic week and dated carry-over, timeShift, the day-change marker, the ChangePlaylist cut (MP017) and
// follow, !InsertPlaylist expansion through the fixtures sub_*.in (nesting, the in-force rule, play mode after expansion,
// cycle MP023, missing MP024), the depth and fan-out bounds, and the file / memory readers.
// MP-08a: the import plan (playout/masterplay/mpimporter.h) of fixtures/import_times.in against a synthetic library:
// golden slot JSON (tests/masterplay/golden/, rewritten with PLAYPLAY_MP_UPDATE_GOLDEN=1) for exact and floating events
// at :07 and programmes crossing the hour, RunlistBuilder laying every planned slot the way the executor does (hard
// starts at the right ms, continuation items not truncated), the weekly seam, filters and joins, hourBoundary cut, a date
// range's first day, an event that places nothing (cuts nothing), the missing policies, options and determinism.
// MP-08b: fixtures/import_features.in, one weekday per mapping, with goldens (import_features_*.json): !Repeat with
// pre-items (unrolled copies, the isLooping hour RunlistBuilder replays to the hour end, the loop laid back to back up to
// the next hard start; repeat unroll; a carry; the guard), every !InsertDirectory loading mode (the plant's (3, 1) and
// (2, 1) and none, masks, MaxAge, SubDirs, search terms, empty folders) as folderBlocks RunlistBuilder expands, logo /
// crawl / text / clock spans as item cues or hooks (osd auto / hooks / off), acts with and without library segments
// (collapsed, or every part with MP027), a break pod at schedtime (asScheduled / live / skip), live sources through
// inputMap and the store's ui.inputMap, the unsupported instruction table, the whole fixture as a deterministic grid.
#include <QtTest>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTimeZone>
#include <algorithm>
#include <memory>
#include "../../playout/masterplay/mpimporter.h"
#include "../../playout/masterplay/mpresolver.h"
#include "../../playout/masterplay/mptimeline.h"
#include "../../playout/schedule/queryblocks.h"
#include "../../playout/schedule/schedulestore.h"

using namespace mp;
using sched::MediaRecord;

namespace {

QString S(const char* s) { return QString::fromUtf8(s); }
QString fx(const QString& name) { return QDir(QStringLiteral(MP_FIXTURES)).filePath(name); }
QByteArray readAll(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}
QString diagList(const QList<Diagnostic>& ds) {   // "line:code:col ..."
    QStringList o;
    for (const Diagnostic& d : ds) o << QStringLiteral("%1:%2:%3").arg(d.line).arg(d.code).arg(d.col);
    return o.join(' ');
}
QStringList candIds(const QList<res::Candidate>& cs) {
    QStringList o;
    for (const res::Candidate& c : cs) o << c.mediaId;
    return o;
}

const qint64 kNow = QDateTime(QDate(2026, 10, 26), QTime(0, 0), QTimeZone::UTC).toMSecsSinceEpoch();
const qint64 kDay = 86400000;

MediaRecord rec(const char* id, const QString& path, const QString& title, const char* type, double dur, qint64 added = kNow - 30 * kDay,
                const QJsonObject& extra = {}, bool archived = false) {
    MediaRecord m;
    m.id = QString::fromLatin1(id);
    m.path = path;
    m.title = title;
    m.type = QString::fromLatin1(type);
    m.durationSec = dur;
    m.addedAtMs = added;
    m.extra = extra;
    m.archived = archived;
    return m;
}

// The algorithm IngestManager::quickHash implements, written independently from its text: md5 over the first 4 MiB,
// then the last 4 MiB when the file is over 8 MiB or the bytes after the first 4 MiB when it is over 4 MiB, then the
// decimal size.
QString refQuick(const QByteArray& b) {
    const qint64 chunk = 4 * 1024 * 1024, size = b.size();
    QByteArray in = b.left(chunk);
    if (size > 2 * chunk) in += b.right(chunk);
    else if (size > chunk) in += b.mid(chunk);
    in += QByteArray::number(size);
    return QString::fromLatin1(QCryptographicHash::hash(in, QCryptographicHash::Md5).toHex());
}
QByteArray noise(qsizetype n, quint32 seed) {
    QByteArray b(n, Qt::Uninitialized);
    QRandomGenerator g(seed);
    for (qsizetype i = 0; i < n; ++i) b[i] = char(g.generate() & 0xff);
    return b;
}
bool writeFile(const QString& path, const QByteArray& b) {
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(b) == b.size();
}

// ---- MP-07 timeline helpers ----
QString hm(int sec) { return QStringLiteral("%1:%2").arg(sec / 3600, 2, 10, QLatin1Char('0')).arg(sec / 60 % 60, 2, 10, QLatin1Char('0')); }
/// "start-end:line" per window, "c<days>" after a carry window: "00:00-06:00:25c1 06:00-09:00:1 09:00-24:00:3".
QString winList(const tl::DayTimeline& t) {
    QStringList o;
    for (const tl::Window& w : t.windows)
        o << QStringLiteral("%1-%2:%3%4").arg(hm(w.startSec), hm(w.endSec)).arg(w.line).arg(w.carry ? QStringLiteral("c%1").arg(w.carryDays) : QString());
    return o.join(' ');
}
/// "file:line:code" per note (file name only).
QString noteList(const QList<tl::Note>& ns) {
    QStringList o;
    for (const tl::Note& n : ns) o << QStringLiteral("%1:%2:%3").arg(QFileInfo(n.file).fileName()).arg(n.diag.line).arg(n.diag.code);
    return o.join(' ');
}
/// "line/depth" per item, "+<children>" for an insert / followed change, then its status: "9/0 10/0+6:expanded 2/1 ...".
QString itemList(const tl::Window& w) {
    QStringList o;
    for (const tl::Item& it : w.items) {
        QString s = QStringLiteral("%1/%2").arg(it.entry.line).arg(it.depth);
        if (it.children > 0) s += QStringLiteral("+%1").arg(it.children);
        if (!it.status.isEmpty()) s += QLatin1Char(':') + it.status;
        o << s;
    }
    return o.join(' ');
}
QJsonArray itemsJson(const tl::Window& w) {
    QJsonArray a;
    for (const tl::Item& it : w.items) a.append(it.toJson());
    return a;
}
const tl::Window* windowAtLine(const tl::DayTimeline& t, int line, bool carry = false) {
    for (const tl::Window& w : t.windows)
        if (w.line == line && w.carry == carry) return &w;
    return nullptr;
}
/// The fixture's sub-playlists are named C:\Sync\NEWS HUB\...: one rule maps that folder onto the fixture folder.
QList<ini::PathRule> fixtureRules() {
    ini::PathRule r;
    r.from = S(R"(C:\Sync\NEWS HUB\)");
    r.to = QDir::toNativeSeparators(QStringLiteral(MP_FIXTURES)) + QLatin1Char('\\');
    r.origin = QStringLiteral("user");
    return {r};
}
QByteArray crlf(const QStringList& lines) { return (lines.join(QStringLiteral("\r\n")) + QStringLiteral("\r\n")).toUtf8(); }

res::Options baseOptions() {
    res::Options o;
    auto rule = [](const char* from, const char* to) { ini::PathRule r; r.from = S(from); r.to = S(to); r.origin = QStringLiteral("user"); return r; };
    o.pathMap = {rule(R"(S:\)", R"(\\nas\S\)"), rule(R"(S:\COMMERCIALS\)", R"(D:\Comm\)"), rule(R"(Z:\)", R"(E:\)"), rule(R"(P:\)", R"(D:\)")};
    o.substitutes = {ini::Substitute{QStringLiteral("MEDIA"), S(R"(D:\Media)")}};
    o.searchPaths = {S(R"(P:\Clips)")};
    o.nowMs = kNow;
    return o;
}

// ---- MP-08a importer helpers ----
const qint64 kImportNow = QDateTime(QDate(2025, 9, 1), QTime(12, 0), QTimeZone::UTC).toMSecsSinceEpoch();   // a Monday

/// The library import_times.in names (durations chosen so events overlap, cross hours and start late).
QList<MediaRecord> importLibrary() {
    return {
        rec("movie", S(R"(D:\Media\Prog\Movie.mpg)"), S("Movie"), "program", 5400),
        rec("news", S(R"(D:\Media\Prog\News Hour.mpg)"), S("News Hour"), "news", 2700),
        rec("morning", S(R"(D:\Media\Prog\Morning Show.mpg)"), S("Morning Show"), "program", 1800),
        rec("doc", S(R"(D:\Media\Prog\Doc.mpg)"), S("Doc"), "program", 1500),
        rec("talk", S(R"(D:\Media\Prog\Talk.mpg)"), S("Talk"), "program", 600),
        rec("weather", S(R"(D:\Media\Prog\Weather.mpg)"), S("Weather"), "weather", 300),
        rec("spotA", S(R"(D:\Media\Spots\Spot A.mpg)"), S("Spot A"), "ad", 30),
        rec("spotB", S(R"(D:\Media\Spots\Spot B.mpg)"), S("Spot B"), "ad", 60),
        rec("card", S(R"(D:\Media\Stills\Card.png)"), S("Card"), "image", 10),
    };
}
/// No file-system access: the library is all there is.
res::Options importResOptions() {
    res::Options o;
    o.useHash = false;
    o.probeFiles = false;
    o.nowMs = kImportNow;
    return o;
}
/// Dated Monday 2025-09-08, UTC, channel "mptest".
imp::Options mondayOptions() {
    imp::Options o;
    o.channelId = S("mptest");
    o.mode = S("dates");
    o.from = o.to = QDate(2025, 9, 8);
    o.timeZone = S("UTC");
    o.nowMs = kImportNow;
    return o;
}
/// "movie@420:finish_previous/300 spotA ..." per slot: mediaId (or the file of a kept path), the hard start and its policy,
/// "+in<sec>" for an in point, "~j" for a join; "cont(<root>,<carrySec>)" first for a continuation hour.
QString slotSummary(const sched::HourSlot& s) {
    QStringList o;
    if (s.isContinuation())
        o << QStringLiteral("cont(%1,%2)").arg(s.mergedEvent.value("rootSlotId").toString()).arg(s.mergedEvent.value("carrySec").toDouble());
    for (const sched::SlotItem& it : s.items) {
        QString x = it.mediaId.isEmpty() ? QStringLiteral("path:") + res::fileName(it.path) : it.mediaId;
        if (it.inPointSec > 0) x += QStringLiteral("+in%1").arg(it.inPointSec);
        if (it.isHardStart()) {
            x += QStringLiteral("@%1:%2").arg(it.fixedOffsetSec).arg(it.extra.value("conflictPolicy").toString());
            if (it.extra.contains("maxLateSec")) x += QStringLiteral("/%1").arg(it.extra.value("maxLateSec").toDouble());
        }
        if (it.id.endsWith(QLatin1String("~j"))) x += QStringLiteral("~j");
        o << x;
    }
    return o.join(' ');
}
QString clockOf(qint64 utcMs) {   // time of day (the import tests run in UTC)
    const qint64 s = ((utcMs % kDay) + kDay) % kDay / 1000;
    return QStringLiteral("%1:%2:%3").arg(s / 3600, 2, 10, QLatin1Char('0')).arg(s / 60 % 60, 2, 10, QLatin1Char('0')).arg(s % 60, 2, 10, QLatin1Char('0'));
}
/// "talk 10:03:30-10:12:00 T weather 10:12:00-10:17:00 ...": H hard (the engine cuts to it), T truncated.
QString runlistSummary(const sched::Runlist& rl) {
    QStringList o;
    for (const sched::RunlistItem& r : rl.items) {
        QString x = (r.item.mediaId.isEmpty() ? res::fileName(r.item.path) : r.item.mediaId) + QLatin1Char(' ') + clockOf(r.plannedStartMs) +
                    QLatin1Char('-') + clockOf(r.plannedEndMs);
        if (r.hard) x += QStringLiteral(" H");
        if (r.truncated) x += QStringLiteral(" T");
        o << x;
    }
    return o.join(QStringLiteral(" | "));
}
/// Lays a plan's consecutive slots the way ScheduleExecutor does (scheduleexecutor.cpp): a continuation hour starts where
/// the previous hour's runlist ends (its carrySec when that runlist has no items), and an hour followed by a continuation
/// keeps its last item whole.
QList<sched::Runlist> layChain(const imp::Plan& p, const sched::MediaLookup& lk) {
    QList<sched::Runlist> out;
    qint64 prevEnd = 0;
    for (int i = 0; i < p.hourSlots.size(); ++i) {
        sched::BuildOptions b = p.buildOptions(i);
        const qint64 h0 = p.hourStartMs[i];
        if (p.hourSlots[i].isContinuation() && prevEnd > h0) b.startOffsetSec = double(prevEnd - h0) / 1000.0;
        const sched::Runlist rl = sched::RunlistBuilder::build(p.hourSlots[i], h0, lk, b);
        prevEnd = 0;
        for (const sched::RunlistItem& r : rl.items) prevEnd = qMax(prevEnd, r.plannedEndMs);
        out << rl;
    }
    return out;
}
QString diagCodes(const QJsonArray& ds) {   // "file:line:code ..." (file name only)
    QStringList o;
    for (const QJsonValue& v : ds) {
        const QJsonObject d = v.toObject();
        o << QStringLiteral("%1:%2:%3").arg(QFileInfo(d.value("file").toString()).fileName()).arg(d.value("line").toInt()).arg(d.value("code").toString());
    }
    return o.join(' ');
}
/// The first difference between two JSON values ("" when equal), with its path.
QString jsonDiff(const QJsonValue& a, const QJsonValue& b, const QString& at) {
    if (a.type() != b.type()) return QStringLiteral("%1: type %2 vs %3").arg(at).arg(int(a.type())).arg(int(b.type()));
    if (a.isObject()) {
        const QJsonObject oa = a.toObject(), ob = b.toObject();
        QStringList keys = oa.keys();
        for (const QString& k : ob.keys()) if (!keys.contains(k)) keys << k;
        std::sort(keys.begin(), keys.end());
        for (const QString& k : keys) {
            if (!oa.contains(k)) return QStringLiteral("%1.%2: not in the golden file").arg(at, k);
            if (!ob.contains(k)) return QStringLiteral("%1.%2: missing from the plan").arg(at, k);
            const QString d = jsonDiff(oa.value(k), ob.value(k), at + QLatin1Char('.') + k);
            if (!d.isEmpty()) return d;
        }
        return {};
    }
    if (a.isArray()) {
        const QJsonArray xa = a.toArray(), xb = b.toArray();
        for (int i = 0; i < qMin(xa.size(), xb.size()); ++i) {
            const QString d = jsonDiff(xa[i], xb[i], QStringLiteral("%1[%2]").arg(at).arg(i));
            if (!d.isEmpty()) return d;
        }
        return xa.size() == xb.size() ? QString() : QStringLiteral("%1: %2 vs %3 elements").arg(at).arg(xa.size()).arg(xb.size());
    }
    if (a != b) return QStringLiteral("%1: golden %2, plan %3").arg(at, a.toVariant().toString(), b.toVariant().toString());
    return {};
}
/// Compares a plan's JSON with tests/masterplay/golden/<name>; PLAYPLAY_MP_UPDATE_GOLDEN=1 rewrites the file instead (review
/// the diff before committing it). On a difference the plan is written to the temp folder for inspection.
void checkGolden(const QString& name, const QJsonObject& plan) {
    const QJsonObject actual = QJsonDocument::fromJson(QJsonDocument(plan).toJson(QJsonDocument::Compact)).object();
    const QString path = QDir(QStringLiteral(MP_FIXTURES)).filePath(QStringLiteral("../golden/") + name);
    if (qEnvironmentVariableIsSet("PLAYPLAY_MP_UPDATE_GOLDEN")) {
        QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
        QVERIFY(writeFile(path, QJsonDocument(actual).toJson(QJsonDocument::Indented)));
        qInfo("golden written: %s", qPrintable(QFileInfo(path).absoluteFilePath()));
        return;
    }
    const QByteArray g = readAll(path);
    QVERIFY2(!g.isEmpty(), qPrintable(QStringLiteral("missing golden file %1 (write it with PLAYPLAY_MP_UPDATE_GOLDEN=1 and review it)").arg(path)));
    QJsonParseError err{};
    const QJsonDocument gd = QJsonDocument::fromJson(g, &err);
    QVERIFY2(gd.isObject(), qPrintable(path + QStringLiteral(": ") + err.errorString()));
    const QString diff = jsonDiff(gd.object(), actual, QStringLiteral("$"));
    if (!diff.isEmpty()) {
        const QString out = QDir::temp().filePath(QStringLiteral("mp08a_actual_") + name);
        writeFile(out, QJsonDocument(actual).toJson(QJsonDocument::Indented));
        QFAIL(qPrintable(QStringLiteral("%1 differs: %2 (the plan was written to %3)").arg(name, diff, out)));
    }
}

// ---- MP-08b importer helpers ----
/// importLibrary() plus the promos folder (three files, one more below it), a programme without segments and one with
/// three library segments (fixtures/import_features.in).
QList<MediaRecord> featureLibrary() {
    QList<MediaRecord> l = importLibrary();
    l << rec("promo1", S(R"(D:\Media\Promos\Promo One.mpg)"), S("Promo One"), "promo", 20, kImportNow - kDay)
      << rec("promo2", S(R"(D:\Media\Promos\Promo Two.mpg)"), S("Promo Two"), "promo", 40, kImportNow - 10 * kDay)
      << rec("spring", S(R"(D:\Media\Promos\Spring Spot.mpg)"), S("Spring Spot"), "promo", 30, kImportNow - kDay)
      << rec("promoOld", S(R"(D:\Media\Promos\Old\Promo Old.mpg)"), S("Promo Old"), "promo", 60, kImportNow - 100 * kDay)
      << rec("show", S(R"(D:\Media\Prog\Show Open.mpg)"), S("Show Open"), "program", 1500);
    MediaRecord seg = rec("segshow", S(R"(D:\Media\Prog\Seg Show.mpg)"), S("Seg Show"), "program", 1800);
    seg.segments = QJsonArray{QJsonObject{{"id", "seg-a"}, {"label", "Act 1"}, {"startTime", 0}, {"endTime", 600}},
                              QJsonObject{{"id", "seg-b"}, {"label", "Act 2"}, {"startTime", 600}, {"endTime", 1200}},
                              QJsonObject{{"id", "seg-c"}, {"label", "Act 3"}, {"startTime", 1200}, {"endTime", 1800}}};
    l << seg;
    return l;
}
/// One dated day of import_features.in (2025-09-07 Sunday .. 2025-09-13 Saturday), UTC, channel "mptest".
imp::Options featureOptions(const QDate& day) {
    imp::Options o = mondayOptions();
    o.from = o.to = day;
    return o;
}
/// The crawl file !ShowCrawlFromFile names, for the memory reader.
tl::ReadFn featureReader() { return tl::memoryReader({{S(R"(D:\Crawl\today.txt)"), QByteArray("Storm warning\r\nuntil 9 pm\r\n")}}); }
/// "folder(order,target,picks)" for a folder item, else slotSummary's form: what a feature slot holds.
QString featureSummary(const sched::HourSlot& s) {
    QStringList o;
    if (s.isContinuation()) o << QStringLiteral("cont(%1,%2)").arg(s.mergedEvent.value("rootSlotId").toString()).arg(s.mergedEvent.value("carrySec").toDouble());
    if (s.isLooping) o << QStringLiteral("LOOP");
    for (const sched::SlotItem& it : s.items) {
        QString x;
        if (!it.folderBlock.isEmpty())
            x = QStringLiteral("folder(%1,%2,%3)").arg(it.folderBlock.value("order").toString()).arg(it.folderBlock.value("targetSec").toDouble()).arg(it.durationSec);
        else x = it.mediaId.isEmpty() ? QStringLiteral("path:") + res::fileName(it.path) : it.mediaId;
        if (!it.segmentId.isEmpty()) x += QLatin1Char('/') + it.segmentId;
        if (it.isHardStart()) x += QStringLiteral("@%1:%2").arg(it.fixedOffsetSec).arg(it.extra.value("conflictPolicy").toString());
        const QJsonObject mp = it.extra.value("masterplay").toObject();
        if (mp.contains("break")) x += QStringLiteral("[b%1]").arg(mp.value("break").toInt());
        if (mp.value("copy").toInt() > 0) x += QStringLiteral("#%1").arg(mp.value("copy").toInt());
        o << x;
    }
    return o.join(' ');
}

} // namespace

class TestMpImport : public QObject {
    Q_OBJECT
    QTemporaryDir m_tmp;
    QString m_hashFile, m_hashValue, m_existsFile;
    QList<MediaRecord> m_lib;
    std::unique_ptr<sched::ScheduleStore> m_store;
    std::unique_ptr<res::Resolver> m_res;      // fromStore (the temp ScheduleStore)
    std::unique_ptr<res::Resolver> m_mem;      // the same records from memory

    QList<MediaRecord> library() const {
        const QJsonObject quick{{"hash", m_hashValue}, {"hashKind", "quick"}};
        const QJsonObject md5{{"hash", m_hashValue}, {"hashKind", "md5"}};   // same value, other kind: never a quick match
        return {
            rec("acme", S(R"(D:\Media\Commercials 2025\Acme_Spot-30.mpg)"), S("Acme Spot 30"), "ad", 30, kNow - 1 * kDay),
            rec("acme2", S(R"(D:\Media\Commercials 2025\Acme Spot 15.mpg)"), S("Acme Spot 15"), "ad", 15, kNow - 10 * kDay),
            rec("acmeSub", S(R"(D:\Media\Commercials 2025\Old\Acme old.mpg)"), S("Acme old"), "ad", 20),
            rec("acmeZero", S(R"(D:\Media\Commercials 2025\Broken.mpg)"), S("Broken"), "ad", 0),
            rec("top", S("d:/media/news/TOP STORY.MPG"), S("TOP STORY"), "news", 60, 1000),
            rec("top2", S(R"(D:\MEDIA\NEWS\Top Story.mpg)"), S("Top Story"), "news", 60, 2000),   // the same file, a second record
            rec("promo", S(R"(D:\Media\Promos\promo.mpg)"), S("Fall Promo"), "promo", 15),
            rec("dupA", S(R"(D:\A\dup.mpg)"), S("dup"), "program", 20, 100),
            rec("dupB", S(R"(E:\B\Sub\dup.mpg)"), S("dup"), "program", 20, 200),
            rec("out", S(R"(D:\New\OUT_bb_03.mpg)"), S("OUT_bb_03"), "program", 600),
            rec("wx", S(R"(D:\x\8f3a.mpg)"), S("Weather Open"), "weather", 10),
            rec("ids1", S(R"(D:\Ids\a.mpg)"), S("Station ID"), "bumper", 5),
            rec("ids2", S(R"(D:\Ids\b.mpg)"), S("Station ID"), "bumper", 5),
            rec("hashrec", S(R"(D:\Gone\renamed.mpg)"), S("Renamed"), "program", 45, kNow - 30 * kDay, quick),
            rec("md5rec", S(R"(D:\Gone\fullhash.mpg)"), S("Full"), "program", 45, kNow - 30 * kDay, md5),
            rec("stale", S(R"(D:\Old\Legacy Clip.mpg)"), S("Legacy Clip"), "program", 30, 50, {}, true),
            rec("fresh", S(R"(D:\Clips\Legacy_Clip.mpg)"), S("Legacy Clip"), "program", 30, 60),
            rec("lost", S(R"(D:\Lost\Lost Spot.mpg)"), S("Lost Spot"), "ad", 30, kNow - 30 * kDay, QJsonObject{{"missing", true}}),
            rec("clip2", S(R"(D:\Clips\clip2.mpg)"), S("clip2"), "program", 12),
            rec("deep", S(R"(D:\Clips\Deep\deep.mpg)"), S("Deep One"), "program", 12),
            rec("uncnews", S(R"(\\nas\S\NEWS A\Crime Report.mpg)"), S("Crime Report"), "news", 90),
            rec("comm", S(R"(D:\Comm\Spot1.mpg)"), S("Spot1"), "ad", 30),
            rec("slocal", S(R"(S:\Local\local.mpg)"), S("local"), "program", 30),
            rec("subst", S(R"(D:\Media\Sub\clip.mpg)"), S("Sub clip"), "program", 30),
        };
    }

private slots:
    void initTestCase() {
        QVERIFY(m_tmp.isValid());
        m_hashFile = m_tmp.filePath("original_name.mpg");
        const QByteArray body = noise(5 * 1024 * 1024 + 123, 7);
        QVERIFY(writeFile(m_hashFile, body));
        m_hashValue = refQuick(body);
        m_existsFile = m_tmp.filePath("exists_not_ingested.mpg");
        QVERIFY(writeFile(m_existsFile, noise(1000, 9)));
        m_lib = library();
        m_store = std::make_unique<sched::ScheduleStore>();
        QVERIFY(m_store->open(m_tmp.filePath("t.db")));
        for (const MediaRecord& m : m_lib) QVERIFY(m_store->saveMedia(m));
        QCOMPARE(m_store->allMedia(true).size(), m_lib.size());
        QCOMPARE(m_store->allMedia(false).size(), m_lib.size() - 1);   // "stale" is archived
        m_res = std::make_unique<res::Resolver>(res::Resolver::fromStore(*m_store, baseOptions()));
        m_mem = std::make_unique<res::Resolver>(m_lib, baseOptions());
        QCOMPARE(m_res->size(), m_lib.size());
    }
    void cleanupTestCase() {
        m_res.reset();
        m_mem.reset();
        m_store.reset();
    }

    // ---- keys ----------------------------------------------------------------------------------------------------
    void joinKeys_data() {
        QTest::addColumn<QString>("in");
        QTest::addColumn<QString>("key");
        QTest::newRow("underscores") << S("OUT_bb_03.mpg") << S("out bb 03");
        QTest::newRow("title form") << S("OUT bb 03") << S("out bb 03");
        QTest::newRow("runs") << S("A--B__C.mxf") << S("a b c");
        QTest::newRow("mixed run") << S("a_-_ b.mpg") << S("a b");
        QTest::newRow("blanks") << S("  Spaced\tName .mpg") << S("spaced name");
        QTest::newRow("dot file") << S(".hidden") << S("");
        QTest::newRow("trailing dot") << S("file.") << S("file.");
        QTest::newRow("dotted") << S("Sample.Show.S01E03.1080p.x265-GROUP.mpg") << S("sample.show.s01e03.1080p.x265 group");
        QTest::newRow("ext case") << S("Sample THREE (2).mpG") << S("sample three (2)");
        QTest::newRow("unicode") << QStringLiteral("Caf\u00e9_\u00dcn\u00efcode.MP4") << QStringLiteral("caf\u00e9 \u00fcn\u00efcode");
        QTest::newRow("nbsp") << QStringLiteral("nbsp\u00a0name.mpg") << S("nbsp name");
        QTest::newRow("em space") << QStringLiteral("a\u2003-\u2003b.mov") << S("a b");
        QTest::newRow("bom is js space") << QStringLiteral("\ufeffa\ufeffb.mpg") << S("a b");
        QTest::newRow("two exts") << S("tar.gz.bak") << S("tar.gz");
        QTest::newRow("ext with space") << S("x.y z") << S("x");
        QTest::newRow("empty") << S("") << S("");
        QTest::newRow("separators only") << S("-_-") << S("");
    }
    void joinKeys() {
        QFETCH(QString, in);
        QFETCH(QString, key);
        QCOMPARE(res::joinKey(in), key);
    }

    void keyHelpers() {
        QCOMPARE(res::titleKey(S("St. Louis News")), S("st. louis news"));
        QCOMPARE(res::joinKey(S("St. Louis News")), S("st"));   // why titles do not get the extension step
        QCOMPARE(res::titleKey(S("OUT_bb_03")), S("out bb 03"));
        QCOMPARE(res::pathKey(S("d:/media//news/TOP STORY.MPG ")), S(R"(d:\media\news\top story.mpg)"));
        QCOMPARE(res::pathKey(S("//nas/S/x.mpg")), S(R"(\\nas\s\x.mpg)"));
        QCOMPARE(res::fileName(S(R"(D:\a\b\)")), S("b"));
        QCOMPARE(res::fileName(S("C:/x/y.mpg")), S("y.mpg"));
        QCOMPARE(res::fileName(S("bare.mpg")), S("bare.mpg"));
        QCOMPARE(res::parentFolder(S(R"(D:\x.mpg)")), S(R"(D:\)"));
        QCOMPARE(res::parentFolder(S("D:/a/b/x.mpg")), S(R"(D:\a\b)"));
        QCOMPARE(res::parentFolder(S(R"(\\nas\S\x.mpg)")), S(R"(\\nas\S)"));
        QCOMPARE(res::parentFolder(S("x.mpg")), QString());
        QVERIFY(res::isAbsolute(S(R"(D:\x)")));
        QVERIFY(res::isAbsolute(S("d:/x")));
        QVERIFY(res::isAbsolute(S(R"(\\nas\S\x)")));
        QVERIFY(!res::isAbsolute(S("x.mpg")));
        QVERIFY(!res::isAbsolute(S(R"(Media\x.mpg)")));
        QVERIFY(!res::isAbsolute(S("D:x.mpg")));
        QCOMPARE(res::resolvedByName(res::ResolvedBy::None), S("none"));
        QCOMPARE(res::resolvedByName(res::ResolvedBy::Path), S("path"));
        QCOMPARE(res::resolvedByName(res::ResolvedBy::Name), S("name"));
        QCOMPARE(res::resolvedByName(res::ResolvedBy::Hash), S("hash"));
    }

    void similarityScores() {
        QCOMPARE(res::similarity(S("acme spot 30"), S("acme spot 30")), 1.0);
        QCOMPARE(res::similarity(S("acme"), S("zzzz")), 0.0);
        QCOMPARE(res::similarity(S(""), S("")), 0.0);
        const double near = res::similarity(S("acme spot 31"), S("acme spot 30"));
        const double far = res::similarity(S("acme spot 31"), S("acme old"));
        QVERIFY2(near > 0.7 && near < 1.0, qPrintable(QString::number(near)));
        QVERIFY(far > 0 && far < near);
        QCOMPARE(res::similarity(S("abc def"), S("def abc")), res::similarity(S("def abc"), S("abc def")));   // symmetric
        QCOMPARE(res::similarity(S("a.b.c"), S("a b c")), 1.0);   // punctuation reads as a space
    }

    // ---- the quick hash copy ---------------------------------------------------------------------------------------
    void quickHashParity_data() {
        QTest::addColumn<qint64>("size");
        const qint64 M = 4 * 1024 * 1024;
        QTest::newRow("empty") << qint64(0);
        QTest::newRow("small") << qint64(100);
        QTest::newRow("one chunk") << M;
        QTest::newRow("chunk plus 10") << M + 10;
        QTest::newRow("two chunks") << 2 * M;
        QTest::newRow("two chunks plus 1") << 2 * M + 1;
        QTest::newRow("nine MiB") << qint64(9 * 1024 * 1024);
    }
    void quickHashParity() {
        QFETCH(qint64, size);
        const QByteArray b = noise(size, quint32(size) + 1);
        const QString path = m_tmp.filePath(QStringLiteral("q%1.bin").arg(size));
        QVERIFY(writeFile(path, b));
        QCOMPARE(res::quickHash(path), refQuick(b));
        QCOMPARE(res::quickHash(path, true), QString::fromLatin1(QCryptographicHash::hash(b, QCryptographicHash::Md5).toHex()));
        QFile::remove(path);
    }
    void quickHashUnreadable() {
        QCOMPARE(res::quickHash(m_tmp.filePath("no-such-file.mpg")), QString());
        QCOMPARE(res::quickHash(m_tmp.path()), QString());   // a folder
        QCOMPARE(res::quickHash(m_hashFile), m_hashValue);
    }

    // ---- resolution ------------------------------------------------------------------------------------------------
    void resolveTable_data() {
        QTest::addColumn<QString>("input");
        QTest::addColumn<QString>("by");
        QTest::addColumn<QString>("mediaId");
        QTest::addColumn<QString>("via");
        QTest::addColumn<QString>("mapped");     // "" = not checked
        QTest::addColumn<int>("rule");           // -2 = not checked
        QTest::addColumn<QString>("flags");
        auto row = [](const char* name, const QString& in, const char* by, const char* id, const char* via, const QString& mapped, int rule,
                      const char* flags) {
            QTest::newRow(name) << in << S(by) << S(id) << S(via) << mapped << rule << S(flags);
        };
        row("exact", S(R"(D:\Media\Commercials 2025\Acme_Spot-30.mpg)"), "path", "acme", "mapped", S(R"(D:\Media\Commercials 2025\Acme_Spot-30.mpg)"), -1, "type=ad dur=30");
        row("case variant, duplicate records", S(R"(D:\Media\News\top story.mpg)"), "path", "top", "mapped", {}, -1, "dup=1");
        row("separator variant", S("D:/Media//Promos/promo.mpg"), "path", "promo", "mapped", S(R"(D:\Media\Promos\promo.mpg)"), -1, "");
        row("longest rule", S(R"(S:\COMMERCIALS\Spot1.mpg)"), "path", "comm", "mapped", S(R"(D:\Comm\Spot1.mpg)"), 0, "");
        row("UNC rule, any case", S("s:/news a/crime report.mpg"), "path", "uncnews", "mapped", S(R"(\\nas\S\news a\crime report.mpg)"), 1, "");
        row("substitute", S(R"(%media%\Sub\clip.mpg)"), "path", "subst", "mapped", S(R"(D:\Media\Sub\clip.mpg)"), -1, "");
        row("search path", S("clip2.mpg"), "path", "clip2", "searchPath", S("clip2.mpg"), -1, "");
        row("playlist path kept", S(R"(S:\Local\local.mpg)"), "path", "slocal", "playlist", S(R"(\\nas\S\Local\local.mpg)"), 1, "");
        row("archived record by path", S(R"(D:\Old\Legacy Clip.mpg)"), "path", "stale", "mapped", {}, -1, "archived");
        row("missing record by path", S(R"(D:\Lost\Lost Spot.mpg)"), "path", "lost", "mapped", {}, -1, "missingFile");
        row("name, picked by folder", S(R"(Z:\B\dup.mpg)"), "name", "dupB", "filename", S(R"(E:\B\dup.mpg)"), 2,
            "ambiguous picked nameMatches=2 cands=dupB,dupA");
        row("name, ambiguous", S(R"(X:\nowhere\dup.mpg)"), "none", "", "", {}, -1, "ambiguous nameMatches=2 cands=dupA,dupB why=filename");
        row("name, separators", S(R"(S:\Old\OUT bb 03.mpg)"), "name", "out", "filename", S(R"(\\nas\S\Old\OUT bb 03.mpg)"), 1, "nameMatches=1");
        row("name, title key", S(R"(X:\MP\Weather-Open.mpg)"), "name", "wx", "title", {}, -1, "type=weather");
        row("title, ambiguous", S(R"(X:\Station-ID.mpg)"), "none", "", "", {}, -1, "ambiguous nameMatches=2 cands=ids1,ids2 why=title");
        row("name, live over archived", S(R"(X:\whatever\Legacy Clip.mpg)"), "name", "fresh", "filename", {}, -1, "nameMatches=1");
        row("name, deep without subdirs", S("deep.mpg"), "name", "deep", "filename", {}, -1, "");
        row("hash", m_hashFile, "hash", "hashrec", "quick", {}, -1, "hashMatches=1");
        row("none, candidates", S(R"(Q:\Commercials\Acme Spot 31.mpg)"), "none", "", "", {}, -1, "first=acme exists=0 hashSkipped=missing type=ad");
        row("none, type rules", S(R"(Q:\Missing\Acme Spot 31.mpg)"), "none", "", "", {}, -1, "first=acme type=unknown");
        row("none, exists", m_existsFile, "none", "", "", {}, -1, "exists=1 hash");
        row("none, bare", S("nothing_here_at_all.mpg"), "none", "", "", {}, -1, "hashSkipped=notAbsolute cands=");
        row("live source", S("mp://Camera 1"), "none", "", "", S("mp://Camera 1"), -1, "kind=live type=live");
        row("url", S("srt://10.0.0.1:9000"), "none", "", "", {}, -1, "kind=url");
        row("unknown substitute", S(R"(%NOPE%\zz_unique.mpg)"), "none", "", "", S(R"(%NOPE%\zz_unique.mpg)"), -1, "unknownSub=NOPE hashSkipped=notAbsolute");
    }
    void resolveTable() {
        QFETCH(QString, input);
        QFETCH(QString, by);
        QFETCH(QString, mediaId);
        QFETCH(QString, via);
        QFETCH(QString, mapped);
        QFETCH(int, rule);
        QFETCH(QString, flags);
        for (const res::Resolver* rv : {m_res.get(), m_mem.get()}) {
            const res::Resolution r = rv->resolve(input, -1);
            const QByteArray ctx = QJsonDocument(r.toJson()).toJson(QJsonDocument::Compact);
            QVERIFY2(res::resolvedByName(r.by) == by, ctx.constData());
            QVERIFY2(r.mediaId == mediaId, ctx.constData());
            QVERIFY2(r.via == via, ctx.constData());
            if (!mapped.isEmpty()) QVERIFY2(r.mapped == mapped, ctx.constData());
            if (rule != -2) QVERIFY2(r.rule == rule, ctx.constData());
            QCOMPARE(r.resolved(), by != QLatin1String("none"));
            if (r.resolved()) {
                QCOMPARE(r.media.id, mediaId);
                QVERIFY(!r.typeDerived);
                QCOMPARE(r.fileExists, -1);
            }
            bool sawAmbiguous = false, sawPicked = false, sawArchived = false, sawMissing = false, sawHash = false;
            for (const QString& f : flags.split(' ', Qt::SkipEmptyParts)) {
                const QString k = f.section('=', 0, 0), v = f.section('=', 1);
                if (k == "ambiguous") { sawAmbiguous = true; QVERIFY2(r.ambiguous, ctx.constData()); }
                else if (k == "picked") { sawPicked = true; QVERIFY2(r.pickedByFolder, ctx.constData()); }
                else if (k == "archived") { sawArchived = true; QVERIFY2(r.archived, ctx.constData()); }
                else if (k == "missingFile") { sawMissing = true; QVERIFY2(r.missingFile, ctx.constData()); }
                else if (k == "dup") QVERIFY2(r.duplicates == v.toInt(), ctx.constData());
                else if (k == "nameMatches") QVERIFY2(r.nameMatches == v.toInt(), ctx.constData());
                else if (k == "hashMatches") QVERIFY2(r.hashMatches == v.toInt(), ctx.constData());
                else if (k == "cands") QVERIFY2(candIds(r.candidates).join(',') == v, ctx.constData());
                else if (k == "first") QVERIFY2(!r.candidates.isEmpty() && r.candidates.first().mediaId == v, ctx.constData());
                else if (k == "why") { for (const auto& c : r.candidates) QVERIFY2(c.why == v && c.score == 1.0, ctx.constData()); }
                else if (k == "exists") QVERIFY2(r.fileExists == v.toInt(), ctx.constData());
                else if (k == "hashSkipped") QVERIFY2(r.hashSkipped == v, ctx.constData());
                else if (k == "hash") { sawHash = true; QVERIFY2(r.hash.size() == 32 && r.hashSkipped.isEmpty(), ctx.constData()); }
                else if (k == "kind") QVERIFY2(r.kind == v, ctx.constData());
                else if (k == "type") QVERIFY2(r.type == v, ctx.constData());
                else if (k == "dur") QVERIFY2(r.durationSec == v.toDouble(), ctx.constData());
                else if (k == "unknownSub") QVERIFY2(r.unknownSubstitutes == QStringList{v}, ctx.constData());
                else QFAIL(qPrintable("unknown flag " + f));
            }
            if (!sawAmbiguous) QVERIFY2(!r.ambiguous, ctx.constData());
            if (!sawPicked) QVERIFY(!r.pickedByFolder);
            if (!sawArchived) QVERIFY(!r.archived);
            if (!sawMissing) QVERIFY(!r.missingFile);
            if (!sawHash && by != QLatin1String("hash")) QVERIFY2(r.hash.isEmpty(), ctx.constData());
            // The REST shape (POST /masterplay/resolve).
            const QJsonObject j = r.toJson();
            for (const char* key : {"path", "mediaId", "resolvedBy", "candidates"}) QVERIFY2(j.contains(QLatin1String(key)), key);
            QCOMPARE(j.value("resolvedBy").toString(), by);
            QCOMPARE(j.value("candidates").toArray().size(), r.candidates.size());
        }
    }

    void storeAndMemoryAgree() {
        // fromStore (SQLite round trip, ORDER BY title) and the in-memory list, in reverse order too, give the same answers.
        QList<MediaRecord> reversed = m_lib;
        std::reverse(reversed.begin(), reversed.end());
        const res::Resolver rev(reversed, baseOptions());
        const QStringList inputs = {S(R"(D:\Media\News\top story.mpg)"), S(R"(Z:\B\dup.mpg)"), S(R"(X:\nowhere\dup.mpg)"), S(R"(X:\Station-ID.mpg)"),
                                    S(R"(Q:\Missing\Acme Spot 31.mpg)"), S(R"(X:\whatever\Legacy Clip.mpg)"), m_hashFile, S("deep.mpg"),
                                    S(R"(S:\Old\OUT bb 03.mpg)"), S(R"(Q:\Zed\totally unrelated name.mpg)")};
        for (const QString& in : inputs) {
            const QJsonObject a = m_res->resolve(in, -1).toJson(), b = m_mem->resolve(in, -1).toJson(), c = rev.resolve(in, -1).toJson();
            QVERIFY2(a == b, qPrintable(in));
            QVERIFY2(a == c, qPrintable(QJsonDocument(a).toJson(QJsonDocument::Compact) + " vs " + QJsonDocument(c).toJson(QJsonDocument::Compact)));
        }
    }

    void missingCandidates() {
        const res::Resolution r = m_res->resolve(S(R"(Q:\Missing\Acme Spot 31.mpg)"), -1);
        QCOMPARE(r.candidates.size(), 3);   // top 3
        QCOMPARE(r.candidates[0].mediaId, S("acme"));
        QCOMPARE(r.candidates[1].mediaId, S("acme2"));
        for (int i = 1; i < r.candidates.size(); ++i) QVERIFY(r.candidates[i - 1].score >= r.candidates[i].score);
        for (const auto& c : r.candidates) { QCOMPARE(c.why, S("similar")); QVERIFY(c.score >= 0.3 && c.score < 1.0); }
        // Fewer when asked, none for a name like nothing in the library.
        QCOMPARE(m_res->similar(S("Acme Spot 31.mpg"), 1).size(), 1);
        QCOMPARE(m_res->similar(S("Acme Spot 31.mpg"), 0).size(), 0);
        QVERIFY(m_res->similar(S("qqqq xxxx.mpg"), 3).isEmpty());
        // A duplicate pair of records is one candidate (the preferred record).
        const auto top = m_res->similar(S("Top Storyy.mpg"), 3);
        QVERIFY(!top.isEmpty());
        QCOMPARE(top[0].mediaId, S("top"));
        QCOMPARE(std::count_if(top.begin(), top.end(), [](const res::Candidate& c) { return c.mediaId.startsWith("top"); }), 1);
        // Equal scores: the one under the preferred folder first.
        const res::Resolver two({rec("x1", S(R"(D:\One\Same Name.mpg)"), S("t1"), "ad", 5), rec("x2", S(R"(D:\Two\Same Name.mpg)"), S("t2"), "ad", 5)});
        auto cs = two.similar(S("Same Namex.mpg"), 3, S(R"(D:\Two)"));
        QCOMPARE(candIds(cs), (QStringList{"x2", "x1"}));
        QVERIFY(cs[0].underFolder && !cs[1].underFolder);
        cs = two.similar(S("Same Namex.mpg"), 3);
        QCOMPARE(candIds(cs), (QStringList{"x1", "x2"}));
    }

    void searchSubDirs() {
        res::Options o = baseOptions();
        o.searchSubDirs = true;
        const res::Resolver rv(m_lib, o);
        res::Resolution r = rv.resolve(S("deep.mpg"), -1);
        QCOMPARE(res::resolvedByName(r.by), S("path"));
        QCOMPARE(r.via, S("searchPath"));
        QCOMPARE(r.mediaId, S("deep"));
        r = rv.resolve(S("clip2.mpg"), -1);   // the direct search-path hit still comes first
        QCOMPARE(r.mediaId, S("clip2"));
        QCOMPARE(r.via, S("searchPath"));
        // From Mplay.INI: SuchPfade, SearchDirsIncludeSubDirs, SubstituteTable.
        const ini::MplayIni ini = ini::readMplayIni(QByteArray("[MPEGPlay]\r\nSuchPfade=P:\\Clips\r\nSearchDirsIncludeSubDirs=1\r\n"
                                                               "SubstituteTable=MEDIA=D:\\Media\r\n"));
        const res::Options fi = res::Options::fromIni(ini, baseOptions().pathMap);
        QCOMPARE(fi.searchPaths, QStringList{S(R"(P:\Clips)")});
        QVERIFY(fi.searchSubDirs);
        QCOMPARE(fi.substitutes.size(), 1);
        const res::Resolver ri(m_lib, fi);
        QCOMPARE(ri.resolve(S("deep.mpg"), -1).via, S("searchPath"));
        QCOMPARE(ri.resolve(S(R"(%MEDIA%\Sub\clip.mpg)"), -1).mediaId, S("subst"));
    }

    void substitutesAndRules() {
        QStringList unknown;
        QCOMPARE(m_res->substitute(S(R"(%Media%/Sub/%x%/%%/50%)"), &unknown), S(R"(D:\Media\Sub\%x%\%%\50%)"));
        QCOMPARE(unknown, QStringList{S("x")});
        // One pass: a value holding a token is not expanded again.
        res::Options o;
        o.substitutes = {ini::Substitute{"A", "%B%"}, ini::Substitute{"%B%", "loop"}};
        const res::Resolver rv({}, o);
        QCOMPARE(rv.substitute(S("%a%")), S("%B%"));
        QCOMPARE(rv.substitute(S("%b%")), S("loop"));   // a name written with its '%' pair
        int rule = -9;
        QCOMPARE(m_res->mapPath(S(R"(S:\COMMERCIALS\x\y.mpg)"), &rule), S(R"(D:\Comm\x\y.mpg)"));
        QCOMPARE(rule, 0);
        QCOMPARE(m_res->mapPath(S(R"(S:\COMMERCIALSX\y.mpg)"), &rule), S(R"(\\nas\S\COMMERCIALSX\y.mpg)"));   // whole components
        QCOMPARE(rule, 1);
        QCOMPARE(m_res->mapPath(S(R"(C:\y.mpg)"), &rule), S(R"(C:\y.mpg)"));
        QCOMPARE(rule, -1);
        QCOMPARE(m_res->rules().size(), 4);
        QCOMPARE(m_res->rules().first().from, S(R"(S:\COMMERCIALS\)"));
    }

    void bracketAndLines() {
        // A raw line: the bracket duration and TAB fields are split off.
        res::Resolution r = m_res->resolve(S("Q:\\Docs\\notes.rtf[00:00:10;00]\tinfo=1"));
        QCOMPARE(r.input, S(R"(Q:\Docs\notes.rtf)"));
        QCOMPARE(r.forcedDurationSec, 10.0);
        QCOMPARE(r.durationSec, 10.0);
        QVERIFY(!r.resolved());
        r = m_res->resolve(S(R"(D:\Media\Promos\promo.mpg[00:00:20;00])"));
        QCOMPARE(r.mediaId, S("promo"));
        QCOMPARE(r.durationSec, 15.0);   // the library duration wins
        QCOMPARE(r.forcedDurationSec, 20.0);
        // A parsed entry.
        const Entry e = parseLine(S(R"(D:\Media\Promos\promo.mpg)"));
        QVERIFY(e.kind == LineKind::Media);
        QCOMPARE(m_res->resolve(e).mediaId, S("promo"));
        // Empty input.
        r = m_res->resolve(S("   "), -1);
        QCOMPARE(r.kind, S("empty"));
        QVERIFY(!r.reportable());
    }

    void hashCopiesAndBudget() {
        // Identical bytes at two library paths: the preferred record, no ambiguity.
        const QJsonObject q{{"hash", m_hashValue}, {"hashKind", "quick"}};
        QList<MediaRecord> lib = {rec("copyB", S(R"(D:\B\copy.mpg)"), S("Copy B"), "program", 45, 300, q),
                                  rec("copyA", S(R"(D:\A\copy.mpg)"), S("Copy A"), "program", 45, 200, q),
                                  rec("copyOld", S(R"(D:\C\copy.mpg)"), S("Copy C"), "program", 45, 100, q, true)};
        res::Resolution r = res::Resolver(lib, {}).resolve(m_hashFile, -1);
        QCOMPARE(res::resolvedByName(r.by), S("hash"));
        QCOMPARE(r.mediaId, S("copyA"));   // live before archived, then the earliest added
        QCOMPARE(r.hashMatches, 2);        // live files only
        QVERIFY(!r.ambiguous);
        // A hash value in other case still matches (IngestManager writes lower-case hex).
        lib = {rec("upper", S(R"(D:\U\u.mpg)"), S("U"), "program", 45, 1, QJsonObject{{"hash", m_hashValue.toUpper()}, {"hashKind", "quick"}})};
        QCOMPARE(res::Resolver(lib, {}).resolve(m_hashFile, -1).mediaId, S("upper"));
        // Off, and a spent budget.
        res::Options off;
        off.useHash = false;
        r = res::Resolver(lib, off).resolve(m_hashFile, -1);
        QVERIFY(!r.resolved());
        QCOMPARE(r.hashSkipped, S("off"));
        QCOMPARE(r.fileExists, 1);   // still probed for the missing report
        off.probeFiles = false;
        r = res::Resolver(lib, off).resolve(m_hashFile, -1);
        QCOMPARE(r.hashSkipped, S("off"));
        QCOMPARE(r.fileExists, -1);
        res::Options none;
        none.fileBudgetMs = 0;
        r = res::Resolver(lib, none).resolve(m_hashFile, -1);
        QVERIFY(!r.resolved());
        QCOMPARE(r.hashSkipped, S("budget"));
        QCOMPARE(r.fileExists, -1);
        // No record with a quick hash (a full-hash ingest, or records older than hashing): the file is not read, the
        // existence probe still runs, and with probes off nothing touches the file system.
        lib = {rec("full", S(R"(D:\F\f.mpg)"), S("F"), "program", 45, 1, QJsonObject{{"hash", m_hashValue}, {"hashKind", "md5"}})};
        r = res::Resolver(lib, {}).resolve(m_hashFile, -1);
        QVERIFY(!r.resolved());
        QCOMPARE(r.hashSkipped, S("noLibraryHashes"));
        QVERIFY(r.hash.isEmpty());
        QCOMPARE(r.fileExists, 1);
        res::Options noProbe;
        noProbe.probeFiles = false;
        r = res::Resolver(lib, noProbe).resolve(m_hashFile, -1);
        QCOMPARE(r.hashSkipped, S("noLibraryHashes"));
        QCOMPARE(r.fileExists, -1);
        QCOMPARE(res::Resolver({}, {}).resolve(S("bare_name.mpg"), -1).hashSkipped, S("noLibraryHashes"));
        // An ambiguous name settled by the hash.
        lib = {rec("n1", S(R"(D:\N1\original_name.mpg)"), S("x"), "program", 45, 1),
               rec("n2", S(R"(D:\N2\original_name.mpg)"), S("y"), "program", 45, 2, q)};
        r = res::Resolver(lib, {}).resolve(m_hashFile, -1);
        QCOMPARE(res::resolvedByName(r.by), S("hash"));
        QCOMPARE(r.mediaId, S("n2"));
        QVERIFY(!r.ambiguous);
        QVERIFY(r.candidates.isEmpty());
        QCOMPARE(r.nameMatches, 2);
    }

    void unreachableRootsAreCached() {
        // A missing root is probed once: every file under it is reported unreachable from the cache. (One quick-hashed
        // record, so step 4 is attempted and reports "missing" rather than "noLibraryHashes".)
        res::Options o;
        const res::Resolver rv({rec("h", S(R"(D:\H\h.mpg)"), S("h"), "program", 10, 1, QJsonObject{{"hash", m_hashValue}, {"hashKind", "quick"}})}, o);
        QElapsedTimer t;
        t.start();
        for (int i = 0; i < 200; ++i) {
            const res::Resolution r = rv.resolve(QStringLiteral("Q:\\Gone%1\\file%2.mpg").arg(i % 7).arg(i), -1);
            QCOMPARE(r.fileExists, 0);
            QCOMPARE(r.hashSkipped, S("missing"));
        }
        QVERIFY(t.elapsed() < 5000);
        // A relative path is never probed.
        QCOMPARE(rv.resolve(S(R"(Media\x.mpg)"), -1).fileExists, -1);
    }

    // ---- folder coverage -------------------------------------------------------------------------------------------
    void folderCoverage() {
        // Probes off: these folders map to real drives and to \\nas (an SMB name lookup). Existence is checked below
        // only on the missing drive Q: and on the temp folder.
        res::Options quiet = baseOptions();
        quiet.probeFiles = false;
        const res::Resolver rv = res::Resolver::fromStore(*m_store, quiet);
        const res::Resolver* m = &rv;
        res::FolderCoverage c = m->folder(S(R"(D:\Media\Commercials 2025)"), false);
        QCOMPARE(c.media, 3);          // acme, acme2, acmeZero (Old\ is a sub-folder)
        QCOMPARE(c.playable, 2);       // acmeZero has no duration
        QCOMPARE(c.durationSec, 45.0);
        QCOMPARE(c.avgSec, 22.5);
        QCOMPARE(c.sample, (QStringList{"Acme Spot 15", "Acme Spot 30"}));
        QVERIFY(!c.empty());
        QCOMPARE(c.fresh, -1);
        c = m->folder(S(R"(D:/Media/Commercials 2025/)"), true);
        QCOMPARE(c.media, 4);
        QCOMPARE(c.playable, 3);
        QCOMPARE(c.durationSec, 65.0);
        // Masks.
        c = m->folder(S(R"(D:\Media\Commercials 2025\Acme*.mpg)"), true);
        QCOMPARE(c.folder, S(R"(D:\Media\Commercials 2025)"));
        QCOMPARE(c.mask, S("Acme*.mpg"));
        QCOMPARE(c.media, 3);
        QCOMPARE(m->folder(S(R"(D:\Media\Commercials 2025\Acme*.mpg)"), false).media, 2);
        QCOMPARE(m->folder(S(R"(D:\Media\Commercials 2025\acme spot ??.MPG)"), true).media, 1);
        QCOMPARE(m->folder(S(R"(D:\Media\Commercials 2025\*)"), true).media, 4);
        // Max age (added date): acme 1 day old, acme2 10 days.
        c = m->folder(S(R"(D:\Media\Commercials 2025)"), false, 48);
        QCOMPARE(c.fresh, 1);
        QCOMPARE(c.maxAgeHours, 48.0);
        // Through a rule.
        c = m->folder(S(R"(S:\COMMERCIALS)"), true);
        QCOMPARE(c.mapped, S(R"(D:\Comm)"));
        QCOMPARE(c.rule, 0);
        QCOMPARE(c.media, 1);
        QCOMPARE(c.unmappedMedia, 0);
        // A wrong rule shows: media under the folder as written, none under the mapped one.
        c = m->folder(S(R"(S:\Local)"), true);
        QCOMPARE(c.mapped, S(R"(\\nas\S\Local)"));
        QCOMPARE(c.playable, 0);
        QCOMPARE(c.unmappedMedia, 1);
        QCOMPARE(c.exists, -1);   // not probed
        // Substitute and search term.
        c = m->folder(S(R"(%MEDIA%\Promos)"), true);
        QCOMPARE(c.mapped, S(R"(D:\Media\Promos)"));
        QCOMPARE(c.playable, 1);
        c = m->folder(S(R"(D:\Media\News\%m-dd-yy%.mpg)"), true);
        QVERIFY(c.searchTerm);
        QCOMPARE(c.folder, S(R"(D:\Media\News)"));
        QVERIFY(c.mask.isEmpty());
        QCOMPARE(c.media, 2);   // top and top2: one file, two records (the folderBlock sees records)
        // Empty: MP019 material with actions (probed: Q: is a drive letter that does not exist here).
        c = m_res->folder(S(R"(Q:\Nothing\Here)"), true);
        QVERIFY(c.empty());
        QCOMPARE(c.exists, 0);
        const QJsonObject j = c.toJson();
        const QJsonArray actions = j.value("actions").toArray();
        QCOMPARE(actions.size(), 2);
        QCOMPARE(actions[0].toObject().value("path").toString(), S("/api/v1/media/scan"));
        QCOMPARE(actions[0].toObject().value("body").toObject().value("folder").toString(), S(R"(Q:\Nothing\Here)"));
        QCOMPARE(actions[1].toObject().value("method").toString(), S("PUT"));
        QCOMPARE(actions[1].toObject().value("folder").toObject().value("recursive").toBool(), true);
        QVERIFY(m->folder(S(R"(D:\Media\Promos)"), true).toJson().value("actions").toArray().isEmpty());
        // An empty folder that exists here (the temp folder): exists 1, which MP019 turns into "scan or watch it".
        c = m_res->folder(m_tmp.path(), false);
        QVERIFY(c.empty());
        QCOMPARE(c.exists, 1);
        // A folder with playable media is never probed, even with probes on (no file-system time for covered folders).
        QCOMPARE(m_res->folder(S(R"(D:\Media\Promos)"), true).exists, -1);
        // A commercials folder suggests the ad type for the scan.
        QCOMPARE(m_res->folder(S(R"(Q:\COMMERCIALS 2026)"), true).toJson().value("actions").toArray()[0].toObject()
                     .value("body").toObject().value("type").toString(), S("ad"));
        // The InsertDirectory spec overload.
        const Entry e = parseLine(S("!InsertDirectory D:\\Media\\Commercials 2025\\\tLoadingMode=3\tToSelect=1\tSubDirs=1\tMaxAge="));
        const auto spec = insertDirectorySpec(e);
        QVERIFY(spec.has_value());
        c = m->folder(*spec);
        QVERIFY(c.recursive);
        QCOMPARE(c.playable, 3);
        QCOMPARE(c.maxAgeHours, -1.0);
    }

    void folderSearchTermIsPartOfTheKey() {
        // "D:\Media\News" and "D:\Media\News\%m-dd-yy%.mpg" share folder and mask; the cache must not hand one the
        // other's searchTerm (MP020 at import), whichever comes first.
        res::Options quiet = baseOptions();
        quiet.probeFiles = false;
        for (bool termFirst : {false, true}) {
            const res::Resolver rv(m_lib, quiet);
            const QString plain = S(R"(D:\Media\News)"), term = S(R"(D:\Media\News\%m-dd-yy%.mpg)");
            const res::FolderCoverage a = rv.folder(termFirst ? term : plain, true);
            const res::FolderCoverage b = rv.folder(termFirst ? plain : term, true);
            QCOMPARE(a.searchTerm, termFirst);
            QCOMPARE(b.searchTerm, !termFirst);
            QCOMPARE(a.media, b.media);
            QVERIFY(!rv.folder(plain, true).searchTerm);
            QVERIFY(rv.folder(term, true).searchTerm);
        }
    }

    void folderParityWithQueryBlocks() {
        // The precomputed matching equals QueryBlocks::matchesFolder (what the folderBlock will draw from). Probes off:
        // the list names real drives and \\nas.
        const QStringList folders = {S(R"(D:\Media\Commercials 2025)"), S(R"(D:\Media\Commercials 2025\)"), S("d:/media"), S(R"(D:\)"),
                                     S(R"(\\nas\S)"), S(R"(\\NAS\s\news a)"), S(R"(D:\Clips)"), S("Promos"), S("Deep"), S(R"(E:\B)"),
                                     S(R"(D:\Med)")};
        res::Options quiet;
        quiet.probeFiles = false;
        const res::Resolver rv(m_lib, quiet);
        for (const QString& f : folders) {
            for (bool recursive : {false, true}) {
                sched::FolderBlock fb;
                fb.folder = res::Resolver({}, {}).mapPath(f);
                fb.recursive = recursive;
                int expect = 0;
                for (const MediaRecord& m : m_lib) if (!m.archived && sched::QueryBlocks::matchesFolder(m, fb)) ++expect;
                QVERIFY2(rv.folder(f, recursive).media == expect, qPrintable(QStringLiteral("%1 recursive=%2: %3 vs %4").arg(f).arg(recursive)
                                                                               .arg(rv.folder(f, recursive).media).arg(expect)));
            }
        }
    }

    void folderCacheKeepsDirectory() {
        res::Options quiet = baseOptions();
        quiet.probeFiles = false;
        const res::Resolver rv(m_lib, quiet);
        const res::FolderCoverage a = rv.folder(S(R"(%MEDIA%\Promos)"), true);
        const res::FolderCoverage b = rv.folder(S(R"(D:\Media\Promos)"), true);
        QCOMPARE(a.mapped, b.mapped);
        QCOMPARE(a.playable, b.playable);
        QCOMPARE(b.directory, S(R"(D:\Media\Promos)"));
    }

    // ---- the report over a document --------------------------------------------------------------------------------
    void documentReport() {
        const QByteArray text = QByteArray("\xEF\xBB\xBF")
            + "0000-00-01T08:00:00\tPlayMode=1\r\n"                                          // 1
              "D:\\Media\\Commercials 2025\\Acme_Spot-30.mpg\r\n"                            // 2 path
              "D:\\Media\\News\\top story.mpg\r\n"                                           // 3 path
              "Q:\\Commercials\\Acme Spot 31.mpg\r\n"                                        // 4 none (MP005)
              "X:\\nowhere\\dup.mpg\r\n"                                                     // 5 ambiguous (MP018)
              "!InsertDirectory D:\\Media\\Commercials 2025\tLoadingMode=3\tToSelect=1\tSubDirs=1\r\n"   // 6 folder
              "!InsertDirectory Q:\\Nothing\\Here\tLoadingMode=3\tToSelect=1\r\n"          // 7 empty folder (MP019)
              "mp://Camera 1\r\n"                                                            // 8 live
              "0000-00-02T09:00:00\r\n"                                                      // 9
              "q:/commercials/acme spot 31.mpg\r\n"                                          // 10 none again (same file)
              "Z:\\B\\dup.mpg\r\n"                                                           // 11 picked by folder (MP018)
              "S:\\Old\\OUT bb 03.mpg\r\n"                                                   // 12 name
              "!InsertDirectory Q:\\Nothing\\Here\tLoadingMode=3\tToSelect=1\r\n";         // 13 empty folder again
        const Document doc = parse(text);
        QCOMPARE(doc.events.size(), 2);
        const res::Report rep = res::resolveDocument(doc, *m_res, S("test.in"));
        const QJsonObject st = rep.stats();
        QCOMPARE(st.value("items").toInt(), 8);
        QCOMPARE(st.value("live").toInt(), 1);
        QCOMPARE(st.value("resolved").toInt(), 4);
        const QJsonObject by = st.value("byResolvedBy").toObject();
        QCOMPARE(by.value("path").toInt(), 2);
        QCOMPARE(by.value("name").toInt(), 2);
        QCOMPARE(by.value("hash").toInt(), 0);
        QCOMPARE(by.value("none").toInt(), 3);
        QCOMPARE(st.value("files").toInt(), 6);
        QCOMPARE(st.value("missingFiles").toInt(), 2);
        QCOMPARE(st.value("ambiguousFiles").toInt(), 2);
        QCOMPARE(st.value("pickedByFolder").toInt(), 1);
        QCOMPARE(st.value("folders").toInt(), 2);
        QCOMPARE(st.value("emptyFolders").toInt(), 1);
        QCOMPARE(st.value("folderUses").toInt(), 3);
        // Diagnostics: one per use, lines and columns exact (the folder points at its head).
        QCOMPARE(diagList(rep.diagnostics()), S("4:MP005:1 5:MP018:1 7:MP019:18 10:MP005:1 11:MP018:1 13:MP019:18"));
        for (const Diagnostic& d : rep.diagnostics()) {
            QCOMPARE(d.severity, diagnosticInfo(d.code).severity);
            QCOMPARE(diagnosticInfo(d.code).stage, S("resolver"));
        }
        QVERIFY(rep.diagnostics()[0].message.contains(S("closest: D:\\Media\\Commercials 2025\\Acme_Spot-30.mpg")));
        QVERIFY(rep.diagnostics()[0].message.contains(S("not reachable")));
        QVERIFY(rep.diagnostics()[4].message.contains(S("took E:\\B\\Sub\\dup.mpg")));
        // Missing report: grouped by file, first-use order, lines and events.
        const QJsonArray missing = rep.missing();
        QCOMPARE(missing.size(), 2);
        const QJsonObject m0 = missing[0].toObject();
        QCOMPARE(m0.value("mapped").toString(), S(R"(Q:\Commercials\Acme Spot 31.mpg)"));
        QCOMPARE(m0.value("file").toString(), S("test.in"));
        QCOMPARE(m0.value("uses").toInt(), 2);
        QCOMPARE(m0.value("lines").toArray(), (QJsonArray{4, 10}));
        const QJsonArray ev = m0.value("events").toArray();
        QCOMPARE(ev.size(), 2);
        QCOMPARE(ev[0].toObject().value("line").toInt(), 1);
        QCOMPARE(ev[0].toObject().value("stamp").toString(), S("0000-00-01T08:00:00"));
        QCOMPARE(ev[1].toObject().value("line").toInt(), 9);
        QCOMPARE(m0.value("exists").toBool(true), false);
        QCOMPARE(m0.value("type").toString(), S("ad"));
        QCOMPARE(m0.value("candidates").toArray().size(), 3);
        QCOMPARE(m0.value("candidates").toArray()[0].toObject().value("mediaId").toString(), S("acme"));
        const QJsonObject m1 = missing[1].toObject();
        QCOMPARE(m1.value("mapped").toString(), S(R"(X:\nowhere\dup.mpg)"));
        QVERIFY(m1.value("ambiguous").toBool());
        QCOMPARE(m1.value("candidates").toArray().size(), 2);
        // Ambiguous report.
        const QJsonArray amb = rep.ambiguous();
        QCOMPARE(amb.size(), 2);
        QCOMPARE(amb[0].toObject().value("picked").toString(), S(""));
        QCOMPARE(amb[1].toObject().value("picked").toString(), S("dupB"));
        QCOMPARE(amb[1].toObject().value("lines").toArray(), QJsonArray{11});
        // Folder report.
        const QJsonArray folders = rep.folders();
        QCOMPARE(folders.size(), 2);
        QCOMPARE(folders[0].toObject().value("playable").toInt(), 3);
        QCOMPARE(folders[0].toObject().value("lines").toArray(), QJsonArray{6});
        QVERIFY(folders[0].toObject().value("actions").toArray().isEmpty());
        QCOMPARE(folders[1].toObject().value("uses").toInt(), 2);
        QCOMPARE(folders[1].toObject().value("lines").toArray(), (QJsonArray{7, 13}));
        QCOMPARE(folders[1].toObject().value("events").toArray().size(), 2);
        QCOMPARE(folders[1].toObject().value("actions").toArray().size(), 2);
        // Whole JSON.
        const QJsonObject j = rep.toJson();
        for (const char* k : {"stats", "missing", "ambiguous", "folders", "diagnostics"}) QVERIFY(j.contains(QLatin1String(k)));
        QCOMPARE(j.value("diagnostics").toArray().size(), 6);
    }

    void reportRowsKeepTheirFields() {
        // A missing file used with different brackets stays ONE row (durationSec of the first use, the distinct brackets
        // listed); a search-term use of a folder is its own folder row, while a trailing separator or other case is not.
        const QByteArray text = QByteArray("0000-00-01T08:00:00\r\n")                                        // 1
            + "Q:\\Commercials\\Acme Spot 31.mpg[00:00:20;00]\r\n"                                          // 2
              "Q:\\Commercials\\Acme Spot 31.mpg\r\n"                                                      // 3
              "q:/commercials/acme spot 31.mpg[00:00:20;00]\r\n"                                           // 4
              "Q:\\Other\\Single.mpg[00:00:07;00]\r\n"                                                     // 5
              "!InsertDirectory D:\\Media\\News\tLoadingMode=3\tToSelect=1\tSubDirs=1\r\n"                  // 6
              "!InsertDirectory D:\\Media\\News\\%m-dd-yy%.mpg\tLoadingMode=3\tToSelect=1\tSubDirs=1\r\n"   // 7
              "!InsertDirectory d:\\media\\news\\\tLoadingMode=3\tToSelect=1\tSubDirs=1\r\n";              // 8 (row of 6)
        res::Options o = baseOptions();
        o.useHash = false;
        o.probeFiles = false;
        const res::Resolver rv(m_lib, o);
        const res::Report rep = res::resolveDocument(parse(text), rv, S("t.in"));
        const QJsonArray missing = rep.missing();
        QCOMPARE(missing.size(), 2);
        const QJsonObject m0 = missing[0].toObject();
        QCOMPARE(m0.value("uses").toInt(), 3);
        QCOMPARE(m0.value("lines").toArray(), (QJsonArray{2, 3, 4}));
        QCOMPARE(m0.value("durationSec").toDouble(), 20.0);
        QCOMPARE(m0.value("forcedDurationsSec").toArray(), (QJsonArray{20.0, QJsonValue(QJsonValue::Null)}));
        const QJsonObject m1 = missing[1].toObject();
        QCOMPARE(m1.value("durationSec").toDouble(), 7.0);
        QVERIFY(!m1.contains("forcedDurationsSec"));   // one bracket for every use
        QCOMPARE(rep.stats().value("missingFiles").toInt(), 2);
        const QJsonArray folders = rep.folders();
        QCOMPARE(folders.size(), 2);
        const QJsonObject plain = folders[0].toObject(), term = folders[1].toObject();
        QVERIFY(!plain.contains("searchTerm"));
        QCOMPARE(plain.value("lines").toArray(), (QJsonArray{6, 8}));
        QVERIFY(term.value("searchTerm").toBool());
        QCOMPARE(term.value("lines").toArray(), QJsonArray{7});
        QCOMPARE(term.value("media").toInt(), plain.value("media").toInt());
        // The other order: the search term first.
        const QByteArray swapped = QByteArray("0000-00-01T08:00:00\r\n")
            + "!InsertDirectory D:\\Media\\News\\%m-dd-yy%.mpg\tLoadingMode=3\tToSelect=1\tSubDirs=1\r\n"
              "!InsertDirectory D:\\Media\\News\tLoadingMode=3\tToSelect=1\tSubDirs=1\r\n";
        const QJsonArray f2 = res::resolveDocument(parse(swapped), rv).folders();
        QCOMPARE(f2.size(), 2);
        QVERIFY(f2[0].toObject().value("searchTerm").toBool());
        QVERIFY(!f2[1].toObject().contains("searchTerm"));
    }

    void fixturesThroughTheResolver() {
        // Every .in fixture against an empty library (no file access: the sample paths may exist on this PC): every
        // media line is one item, each diagnostic sits on a media or !InsertDirectory line, nothing resolves.
        res::Options o;
        o.useHash = false;
        o.probeFiles = false;
        const res::Resolver empty({}, o);
        const QStringList files = QDir(QStringLiteral(MP_FIXTURES)).entryList({"*.in"}, QDir::Files, QDir::Name);
        QVERIFY(files.size() >= 11);
        for (const QString& f : files) {
            const Document doc = parse(readAll(fx(f)));
            int media = 0, live = 0, folders = 0;
            QSet<int> itemLines;
            for (const Entry& e : doc.entries) {
                if (e.kind == LineKind::Media) {
                    if (!e.path.trimmed().isEmpty()) ++media;
                    itemLines << e.line;
                    if (e.path.startsWith(QLatin1String("mp://"), Qt::CaseInsensitive)) ++live;
                } else if (insertDirectorySpec(e)) {
                    ++folders;
                    itemLines << e.line;
                }
            }
            const res::Report rep = res::resolveDocument(doc, empty, f);
            const QJsonObject st = rep.stats();
            QVERIFY2(st.value("items").toInt() == media, qPrintable(f));
            QVERIFY2(st.value("resolved").toInt() == 0, qPrintable(f));
            QVERIFY2(st.value("live").toInt() + st.value("urls").toInt() >= live, qPrintable(f));
            QVERIFY2(st.value("folderUses").toInt() == folders, qPrintable(f));
            QVERIFY2(st.value("emptyFolders").toInt() == st.value("folders").toInt(), qPrintable(f));
            for (const Diagnostic& d : rep.diagnostics()) {
                QVERIFY2(itemLines.contains(d.line), qPrintable(f + ": line " + QString::number(d.line)));
                QVERIFY2(QStringList({"MP005", "MP018", "MP019"}).contains(d.code), qPrintable(d.code));
            }
        }
    }

    void plantCorpus() {
        // Opt-in (the plant playlist is business data, never committed): PLAYPLAY_MP_SAMPLES names a folder holding a
        // scratch copy of the station playlist saved as plant_playlist.in. A synthetic library built from the plant's own file list: every second
        // distinct file at its own path in other case and separators (path), a quarter moved to another folder (name),
        // a quarter absent (none).
        const QString dir = qEnvironmentVariable("PLAYPLAY_MP_SAMPLES");
        if (dir.isEmpty()) QSKIP("PLAYPLAY_MP_SAMPLES is not set (point it at a folder holding plant_playlist.in)");
        const QByteArray bytes = readAll(QDir(dir).filePath("plant_playlist.in"));
        if (bytes.isEmpty()) QSKIP("plant_playlist.in not found under PLAYPLAY_MP_SAMPLES");
        const Document doc = parse(bytes);
        QStringList order;                    // distinct files (path key), first-use order
        QHash<QString, QString> pathOf;
        QHash<QString, int> uses;
        int folderLines = 0;
        for (const Entry& e : doc.entries) {
            if (e.kind == LineKind::Media) {
                const QString k = res::pathKey(e.path);
                if (!uses.contains(k)) { order << k; pathOf.insert(k, e.path); }
                ++uses[k];
            } else if (insertDirectorySpec(e)) {
                ++folderLines;
            }
        }
        QList<MediaRecord> lib;
        int expectPath = 0, moved = 0, absent = 0;
        for (int i = 0; i < order.size(); ++i) {
            const QString& k = order[i];
            const QString p = pathOf.value(k);
            if (i % 2 == 0) {
                lib << rec(qPrintable(QStringLiteral("p%1").arg(i)), p.toUpper().replace('\\', '/'), res::fileName(p), "program", 30);
                expectPath += uses.value(k);
            } else if (i % 4 == 1) {
                lib << rec(qPrintable(QStringLiteral("m%1").arg(i)), QStringLiteral("D:\\Moved\\") + res::fileName(p), res::fileName(p), "program", 30);
                moved += uses.value(k);
            } else {
                absent += uses.value(k);
            }
        }
        res::Options o;
        o.useHash = false;
        o.probeFiles = false;
        QElapsedTimer t;
        t.start();
        const res::Resolver rv(lib, o);
        const res::Report rep = res::resolveDocument(doc, rv, S("plant_playlist.in"));
        const qint64 ms = t.elapsed();
        const QJsonObject st = rep.stats(), by = st.value("byResolvedBy").toObject();
        QCOMPARE(st.value("items").toInt(), 2068);
        QCOMPARE(folderLines, 211);
        QCOMPARE(st.value("folderUses").toInt(), 211);
        QCOMPARE(by.value("path").toInt(), expectPath);
        QCOMPARE(by.value("hash").toInt(), 0);
        QCOMPARE(by.value("name").toInt() + by.value("none").toInt(), moved + absent);
        QVERIFY(by.value("none").toInt() >= absent);
        QVERIFY(by.value("name").toInt() > 0);
        for (const Diagnostic& d : rep.diagnostics()) QVERIFY(QStringList({"MP005", "MP018", "MP019"}).contains(d.code));
        qInfo("plant: %d distinct files, %d items: path %d, name %d (moved %d), none %d (absent %d); %d folders (%d empty); %lld ms",
              int(order.size()), st.value("items").toInt(), by.value("path").toInt(), by.value("name").toInt(), moved, by.value("none").toInt(),
              absent, st.value("folders").toInt(), st.value("emptyFolders").toInt(), ms);
        QVERIFY2(ms < 10000, qPrintable(QString::number(ms)));
    }

    void optionsJson() {
        QJsonObject j{{"pathMap", QJsonArray{QJsonObject{{"from", "S:\\"}, {"to", "\\\\nas\\S\\"}, {"origin", "ExchangeList"}, {"guess", true}}}},
                      {"substitutes", QJsonObject{{"MEDIA", "D:\\Media"}}},
                      {"searchPaths", QJsonArray{"P:\\Clips", " ", ""}},
                      {"searchSubDirs", true}, {"hash", false}, {"probe", false}, {"fileBudgetMs", 9999999},
                      {"candidates", 99}, {"minScore", 2.5}, {"channel", "ch2"}};
        res::Options o = res::Options::fromJson(j);
        QCOMPARE(o.pathMap.size(), 1);
        QCOMPARE(o.pathMap[0].origin, S("ExchangeList"));
        QVERIFY(o.pathMap[0].guess);
        QCOMPARE(o.substitutes.size(), 1);
        QCOMPARE(o.substitutes[0].name, S("MEDIA"));
        QCOMPARE(o.searchPaths, QStringList{S(R"(P:\Clips)")});
        QVERIFY(o.searchSubDirs);
        QVERIFY(!o.useHash);
        QVERIFY(!o.probeFiles);
        QCOMPARE(o.fileBudgetMs, 600000);
        QCOMPARE(o.maxCandidates, 20);
        QCOMPARE(o.minScore, 1.0);
        QCOMPARE(o.channelId, S("ch2"));
        // Round trip, and the object forms.
        const res::Options back = res::Options::fromJson(o.toJson());
        QCOMPARE(back.toJson(), o.toJson());
        o = res::Options::fromJson(QJsonObject{{"pathMap", QJsonObject{{"Z:\\", "E:\\"}}},
                                               {"substitutes", QJsonArray{QJsonObject{{"name", "A"}, {"value", "B"}}, QJsonObject{{"value", "x"}}}}});
        QCOMPARE(o.pathMap.size(), 1);
        QCOMPARE(o.pathMap[0].to, S(R"(E:\)"));
        QCOMPARE(o.pathMap[0].origin, S("user"));
        QCOMPARE(o.substitutes.size(), 1);
        QVERIFY(o.useHash && o.probeFiles);
        QCOMPARE(o.maxCandidates, 3);
        // Defaults.
        o = res::Options::fromJson({});
        QCOMPARE(o.toJson().value("candidates").toInt(), 3);
        QVERIFY(o.toJson().value("hash").toBool());
    }

    void largeLibrary() {
        // 20,000 records, 2,000 lookups (exact, name, unresolved with candidates) and 100 folders: bounded work.
        QList<MediaRecord> lib;
        lib.reserve(20000);
        for (int i = 0; i < 20000; ++i)
            lib << rec(qPrintable(QStringLiteral("m%1").arg(i)), QStringLiteral("D:\\Lib\\F%1\\Clip_%2 Story %3.mpg").arg(i % 100).arg(i).arg(i % 37),
                       QStringLiteral("Clip %1 Story").arg(i), "program", 30 + i % 50);
        res::Options o;
        o.probeFiles = false;
        o.useHash = false;
        QElapsedTimer t;
        t.start();
        const res::Resolver rv(lib, o);
        const qint64 buildMs = t.elapsed();
        QElapsedTimer first;
        first.start();
        QVERIFY(rv.similar(S("warm up.mpg"), 3).size() <= 3);   // builds the similarity index
        const qint64 indexMs = first.elapsed();
        int path = 0, name = 0, none = 0;
        for (int i = 0; i < 2000; ++i) {
            const int k = (i * 7919) % 20000;
            QString in;
            if (i % 4 == 0) in = QStringLiteral("d:/lib/f%1/clip_%2 story %3.MPG").arg(k % 100).arg(k).arg(k % 37);
            else if (i % 4 == 1) in = QStringLiteral("Y:\\Elsewhere\\Clip-%1-Story-%2.mpg").arg(k).arg(k % 37);
            else in = QStringLiteral("Y:\\Elsewhere\\Clip %1 Storie %2.mpg").arg(k + 100000).arg(k % 37);
            const res::Resolution r = rv.resolve(in, -1);
            if (r.by == res::ResolvedBy::Path) ++path;
            else if (r.by == res::ResolvedBy::Name) ++name;
            else { ++none; QVERIFY(r.candidates.size() <= 3); }
        }
        const qint64 lookupMs = t.elapsed() - buildMs;
        for (int f = 0; f < 100; ++f) QCOMPARE(rv.folder(QStringLiteral("D:\\Lib\\F%1").arg(f), true).media, 200);
        const qint64 ms = t.elapsed();
        QCOMPARE(path, 500);
        QCOMPARE(name, 500);
        QCOMPARE(none, 1000);
        QVERIFY2(ms < 20000, qPrintable(QString::number(ms)));
        qInfo("large library (20000 records): build %lld ms, similarity index %lld ms, 2000 lookups %lld ms (1000 with candidates), "
              "100 folders %lld ms", buildMs, indexMs, lookupMs - indexMs, ms - buildMs - lookupMs);
    }

    // ---- MP-07: day timelines --------------------------------------------------------------------------------------
    void timelineGridWeek() {
        // sub_week.in through the file reader, with one rule mapping C:\Sync\NEWS HUB\ onto the fixture folder.
        const QString root = fx("sub_week.in");
        const Document doc = parse(readAll(root));
        QCOMPARE(doc.events.size(), 11);
        QVERIFY2(doc.diagnostics.isEmpty(), qPrintable(diagList(doc.diagnostics)));
        tl::Builder b(doc, root, {}, tl::fileReader(fixtureRules()));
        const QList<tl::DayTimeline> week = b.week();
        QCOMPARE(week.size(), 7);
        const QStringList expect = {
            S("00:00-06:00:25c1 06:00-09:00:1 09:00-24:00:3"),                                   // Sun: Saturday's movie carries
            S("00:00-06:00:3c1 06:00-09:00:6 09:00-24:00:3"),                                    // Mon: weekly beats every day
            S("00:00-06:00:3c1 06:00-09:00:1 09:00-24:00:3"),
            S("00:00-06:00:3c1 06:00-09:00:1 09:00-12:00:3 12:00-19:00:8 19:00-24:00:12"),
            S("00:00-06:00:12c1 06:00-09:00:1 09:00-12:00:3 12:00-20:00:14 20:00-24:00:19"),
            S("00:00-06:00:19c1 06:00-09:00:1 09:00-12:00:3 12:00-24:00:21"),
            S("00:00-06:00:21c1 06:00-09:00:1 09:00-23:00:3 23:00-24:00:25"),
        };
        for (int d = 0; d < 7; ++d) {
            QCOMPARE(week[d].day.dow, d);
            QCOMPARE(week[d].day.key(), QString::number(d));
            QCOMPARE(winList(week[d]), expect[d]);
            QCOMPARE(week[d].datedSkipped, 2);   // the two dated events belong to date imports
        }
        // Sunday 00:00 continues Saturday 23:00 (an hour in).
        const tl::Window& sun0 = week[0].windows[0];
        QVERIFY(sun0.carry);
        QCOMPARE(sun0.carryDays, 1);
        QCOMPARE(sun0.eventSec, 23 * 3600);
        QCOMPARE(sun0.offsetSec, qint64(3600));
        QCOMPARE(sun0.eventType, 2);
        QCOMPARE(sun0.name, S("Late Movie"));
        QCOMPARE(sun0.priority, 2);
        QCOMPARE(sun0.stamp.weekday, 7);
        QCOMPARE(sun0.eventDurationSec, 9000.0);
        QCOMPARE(itemList(sun0), S("26/0"));
        // Every day 09:00: header PlayMode=2 and no !Repeat, so the whole event loops.
        const tl::Window* sun9 = windowAtLine(week[0], 3);
        QVERIFY(sun9);
        QCOMPARE(sun9->playMode, 2);
        QCOMPARE(sun9->playModeFrom, S("header"));
        QCOMPARE(sun9->repeatAt, -1);
        QCOMPARE(sun9->loopFrom, 0);
        QCOMPARE(windowAtLine(week[0], 1)->playModeFrom, S("header"));
        QCOMPARE(windowAtLine(week[0], 1)->loopFrom, -1);
        // Monday drops the every-day 06:00 event for the weekly one.
        QCOMPARE(week[1].dropped.size(), 1);
        QCOMPARE(week[1].dropped[0].line, 1);
        QCOMPARE(week[1].dropped[0].keptLine, 6);
        QCOMPARE(week[1].dropped[0].priority, 1);
        QCOMPARE(week[1].dropped[0].keptPriority, 2);
        QCOMPARE(week[1].dropped[0].sec, 6 * 3600);
        QCOMPARE(windowAtLine(week[1], 3, true)->offsetSec, qint64(86400 - 9 * 3600));
        for (int d : {0, 2, 3, 4, 5, 6}) QVERIFY(week[d].dropped.isEmpty());

        // Wednesday noon: two levels of sub-playlists; the inner list's Wednesday 00:00 event is the one in force at 12:00.
        const tl::Window* noon = windowAtLine(week[3], 8);
        QVERIFY(noon);
        QCOMPARE(itemList(*noon), S("9/0 10/0+6:expanded 2/1 3/1+3:expanded 2/2 3/2 4/2 4/1 11/0"));
        QCOMPARE(noon->items[0].file, root);
        QVERIFY(noon->items[2].file.endsWith(S("sub_news.in"), Qt::CaseInsensitive));
        QVERIFY(noon->items[4].file.endsWith(S("sub_news_inner.in"), Qt::CaseInsensitive));
        QCOMPARE(noon->items[4].entry.path, S(R"(S:\News\Weather Early.mpg)"));
        QCOMPARE(noon->items[0].parent, -1);
        QCOMPARE(noon->items[2].parent, 1);
        QCOMPARE(noon->items[3].parent, 1);
        QCOMPARE(noon->items[4].parent, 3);
        QCOMPARE(noon->items[7].parent, 1);
        QCOMPARE(noon->items[8].parent, -1);
        QCOMPARE(noon->items[1].target, S(R"(C:\Sync\NEWS HUB\sub_news.in)"));
        QCOMPARE(noon->items[1].targetPath.toCaseFolded(), QDir::toNativeSeparators(fx("sub_news.in")).toCaseFolded());
        QCOMPARE(noon->items[1].subEventLine, 1);
        QCOMPARE(noon->items[1].subPlayMode, 1);
        QCOMPARE(noon->items[3].target, S("sub_news_inner.in"));
        QCOMPARE(noon->items[3].subEventLine, 1);
        QCOMPARE(noon->items[3].subPlayMode, 2);
        QVERIFY(noon->items[0].group.isEmpty() && noon->items[1].group.isEmpty());
        QVERIFY(!noon->items[2].group.isEmpty());
        QCOMPARE(noon->items[7].group, noon->items[2].group);
        QVERIFY(!noon->items[4].group.isEmpty() && noon->items[4].group != noon->items[2].group);
        // The first marker is the !InsertPlaylist, so the sub-playlists decide: the inner list's !Repeat makes it repeat.
        QCOMPARE(noon->playMode, 2);
        QCOMPARE(noon->playModeFrom, S("sub"));
        QCOMPARE(noon->repeatAt, 5);
        QCOMPARE(noon->loopFrom, 6);
        QCOMPARE(noon->eventType, 1);
        QCOMPARE(noon->headerPlayMode, 1);
        QCOMPARE(doc.events.at(noon->eventIndex).effectivePlayMode, 1);   // what MP-03 saw before expansion
        // Wednesday 19:00: the inner list's 18:00 event; no marker there, so its header (normal) decides.
        const tl::Window* eve = windowAtLine(week[3], 12);
        QVERIFY(eve);
        QCOMPARE(itemList(*eve), S("13/0+1:expanded 6/1"));
        QCOMPARE(eve->items[0].subEventLine, 5);
        QCOMPARE(eve->playMode, 1);
        QCOMPARE(eve->playModeFrom, S("sub"));
        QCOMPARE(eve->loopFrom, -1);
        // Thursday's carry window expands exactly like the event's own window on Wednesday.
        const tl::Window* thu0 = windowAtLine(week[4], 12, true);
        QVERIFY(thu0);
        QCOMPARE(thu0->offsetSec, qint64(5 * 3600));
        QCOMPARE(itemsJson(*thu0), itemsJson(*eve));
        // Thursday noon: the !ChangePlaylist cut (not followed by default).
        const tl::Window* chg = windowAtLine(week[4], 14);
        QVERIFY(chg);
        QCOMPARE(itemList(*chg), S("15/0 16/0:notFollowed"));
        QCOMPARE(chg->cutAt, 1);
        QCOMPARE(chg->cutItems, 2);
        QCOMPARE(chg->items[1].why, S("followChangePlaylist is off"));
        QCOMPARE(chg->items[1].target, S(R"(C:\Sync\NEWS HUB\sub_evening.in)"));
        QVERIFY(chg->items[1].targetPath.isEmpty());
        // Friday noon: a -> b -> A (case and a relative name) is a cycle; a missing sub-playlist.
        const tl::Window* fri = windowAtLine(week[5], 21);
        QVERIFY(fri);
        QCOMPARE(itemList(*fri), S("22/0+5:expanded 2/1 3/1+3:expanded 2/2 3/2:cycle 4/2 23/0:missing 24/0"));
        QCOMPARE(fri->items[4].target, S(R"(C:\Sync\NEWS HUB\SUB_CYCLE_A.IN)"));
        QCOMPARE(fri->items[6].why, S("not found"));
        QCOMPARE(itemsJson(*windowAtLine(week[6], 21, true)), itemsJson(*fri));

        // Notes: de-duplicated across days, each in the file its line belongs to.
        QCOMPARE(noteList(b.notes()), S("sub_week.in:1:MP015 sub_week.in:16:MP017 sub_cycle_b.in:3:MP023 sub_week.in:23:MP024"));
        const tl::Note& n15 = b.notes()[0];
        QCOMPARE(n15.diag.severity, S("warning"));
        QCOMPARE(n15.diag.col, 1);
        QCOMPARE(n15.days, QStringList{S("Mon")});
        QVERIFY2(n15.diag.message.contains(S("(line 6)")) && n15.diag.message.contains(S("weekly over everyDay")), qPrintable(n15.diag.message));
        const tl::Note& n17 = b.notes()[1];
        QCOMPARE(n17.diag.col, 17);
        QCOMPARE(n17.days, QStringList{S("Thu")});
        QVERIFY2(n17.diag.message.contains(S("followChangePlaylist is off")) && n17.diag.message.contains(S("2 line(s) after it never run")),
                 qPrintable(n17.diag.message));
        const tl::Note& n23 = b.notes()[2];
        QCOMPARE(n23.diag.severity, S("error"));
        QCOMPARE(n23.diag.col, 17);
        QCOMPARE(n23.days, (QStringList{S("Fri"), S("Sat")}));
        QVERIFY2(n23.diag.message.contains(S("sub_week.in -> sub_cycle_a.in -> sub_cycle_b.in -> sub_cycle_a.in")), qPrintable(n23.diag.message));
        const tl::Note& n24 = b.notes()[3];
        QCOMPARE(n24.diag.col, 17);
        QCOMPARE(n24.days, (QStringList{S("Fri"), S("Sat")}));
        QVERIFY2(n24.diag.message.contains(S("(not found)")), qPrintable(n24.diag.message));
        // Each (target, including file) is read once, one document per file: sub_news, sub_news_inner from sub_news and
        // from the root, sub_cycle_a, sub_cycle_b, SUB_CYCLE_A.IN (the same document as sub_cycle_a), sub_missing.
        QCOMPARE(b.reads(), 7);
        const QJsonArray src = b.sourcesJson();
        QCOMPARE(src.size(), 6);
        QVERIFY(src[0].toObject().value("root").toBool());
        QCOMPARE(src[0].toObject().value("path").toString(), root);
        QVERIFY(src[1].toObject().value("path").toString().endsWith(S("sub_news.in"), Qt::CaseInsensitive));
        QVERIFY(!src[5].toObject().value("ok").toBool());
        QCOMPARE(src[5].toObject().value("why").toString(), S("not found"));
        const QJsonObject st = b.stats();
        QCOMPARE(st.value("days").toInt(), 7);
        QCOMPARE(st.value("windows").toInt(), 27);
        QCOMPARE(st.value("carry").toInt(), 7);
        QCOMPARE(st.value("dropped").toInt(), 1);
        QCOMPARE(st.value("maxDepth").toInt(), 2);
        QCOMPARE(st.value("inserts").toObject().value("expanded").toInt(), 8);
        QCOMPARE(st.value("inserts").toObject().value("cycle").toInt(), 2);
        QCOMPARE(st.value("inserts").toObject().value("missing").toInt(), 2);
        QCOMPARE(st.value("changes").toObject().value("notFollowed").toInt(), 1);
        QCOMPARE(st.value("cutItems").toInt(), 2);
        QCOMPARE(st.value("sources").toInt(), 6);
        QCOMPARE(st.value("notes").toInt(), 4);
        // A second week comes from the caches: no read, no new note.
        const QList<tl::DayTimeline> again = b.week();
        QCOMPARE(b.reads(), 7);
        QCOMPARE(b.notes().size(), 4);
        for (int d = 0; d < 7; ++d) QCOMPARE(again[d].toJson(), week[d].toJson());
    }

    void timelineDatesAndCarry() {
        // Dated mode: the dated events take part, the day opens with the event in force at the end of the date before.
        const QString root = fx("sub_week.in");
        tl::Builder b(parse(readAll(root)), root, {}, tl::fileReader(fixtureRules()));
        const QList<tl::DayTimeline> ds = b.dates(QDate(2025, 9, 9), QDate(2025, 9, 11));
        QCOMPARE(ds.size(), 3);
        QCOMPARE(ds[1].day.dow, 3);   // 2025-09-10 is a Wednesday
        QCOMPARE(ds[1].day.key(), S("2025-09-10"));
        QCOMPARE(ds[1].day.label(), S("2025-09-10"));
        QCOMPARE(winList(ds[0]), S("00:00-06:00:3c1 06:00-09:00:1 09:00-24:00:3"));
        QCOMPARE(winList(ds[1]), S("00:00-06:00:3c1 06:00-09:00:27 09:00-12:00:3 12:00-19:00:8 19:00-22:00:12 22:00-24:00:29"));
        QCOMPARE(winList(ds[2]), S("00:00-06:00:29c1 06:00-09:00:1 09:00-12:00:3 12:00-20:00:14 20:00-24:00:19"));
        QCOMPARE(ds[1].datedSkipped, 0);
        QCOMPARE(ds[1].dropped.size(), 1);
        QCOMPARE(ds[1].dropped[0].line, 1);
        QCOMPARE(ds[1].dropped[0].keptLine, 27);
        QCOMPARE(ds[1].dropped[0].keptPriority, 3);
        // 2025-09-11 opens with the dated 22:00 special, two hours in, looping after its !Repeat.
        const tl::Window& carry = ds[2].windows[0];
        QCOMPARE(carry.priority, 3);
        QCOMPARE(carry.stamp.kind, DayKind::Dated);
        QCOMPARE(carry.offsetSec, qint64(7200));
        QCOMPARE(carry.name, S("Special Night"));
        QCOMPARE(carry.eventDurationSec, 14400.0);
        QCOMPARE(itemList(carry), S("30/0 31/0"));
        QCOMPARE(carry.playMode, 2);
        QCOMPARE(carry.playModeFrom, S("inBand"));
        QCOMPARE(carry.repeatAt, 0);
        QCOMPARE(carry.loopFrom, 1);
        QCOMPARE(noteList(b.notes()), S("sub_week.in:1:MP015 sub_week.in:16:MP017"));
        QVERIFY2(b.notes()[0].diag.message.contains(S("(line 27)")) && b.notes()[0].diag.message.contains(S("dated over everyDay")),
                 qPrintable(b.notes()[0].diag.message));
        QCOMPARE(b.notes()[0].days, QStringList{S("2025-09-10")});
        QCOMPARE(b.notes()[1].days, QStringList{S("2025-09-11")});

        // A lone dated event carries for as long as nothing follows it; before it nothing is in force; the grid skips it.
        const Document only = parse(crlf({S("2025-01-01T08:00:00\tPlayMode=1"), S(R"(S:\x.mpg)")}));
        tl::Builder o(only);
        QCOMPARE(winList(o.build(tl::Day::of(QDate(2024, 12, 31)))), QString());
        QCOMPARE(winList(o.build(tl::Day::of(QDate(2025, 1, 1)))), S("08:00-24:00:1"));
        QCOMPARE(winList(o.build(tl::Day::of(QDate(2025, 1, 5)))), S("00:00-24:00:1c4"));
        const tl::DayTimeline feb = o.build(tl::Day::of(QDate(2025, 2, 1)));
        QCOMPARE(winList(feb), S("00:00-24:00:1c31"));
        QCOMPARE(feb.windows[0].offsetSec, qint64(31) * 86400 - 8 * 3600);
        for (const tl::DayTimeline& t : o.week()) {
            QVERIFY(t.windows.isEmpty());
            QCOMPARE(t.datedSkipped, 1);
        }
        // Of two dated events long before, the later one carries.
        const Document two = parse(crlf({S("2025-01-01T08:00:00\tPlayMode=1"), S("2025-01-03T07:00:00\tPlayMode=1")}));
        QCOMPARE(winList(tl::Builder(two).build(tl::Day::of(QDate(2025, 3, 1)))), S("00:00-24:00:2c57"));
        // Ranges: at most 366 days; none when the range is reversed.
        QCOMPARE(o.dates(QDate(2025, 1, 1), QDate(2026, 12, 31)).size(), 366);
        QVERIFY(o.dates(QDate(2025, 1, 2), QDate(2025, 1, 1)).isEmpty());
        QVERIFY(o.dates(QDate(), QDate(2025, 1, 1)).isEmpty());
    }

    void timelinePriorityTies_data() {
        // L1 every day, L3 weekly Tuesday, L5 dated Tuesday 2026-10-27, L7 the day-change marker, L9 a second weekly
        // Tuesday event (MP003): all at 10:00 when the day change is at 10:00.
        QTest::addColumn<int>("dow");             // grid weekday, or -1
        QTest::addColumn<QDate>("date");
        QTest::addColumn<bool>("same");
        QTest::addColumn<int>("dayChangeSec");
        QTest::addColumn<QString>("windows");
        QTest::addColumn<QString>("dropped");     // "dropped>kept" in tie order
        QTest::newRow("grid Tue") << 2 << QDate() << false << 36000 << S("00:00-10:00:1c1 10:00-24:00:3") << S("9>3 1>3 7>3");
        QTest::newRow("grid Wed") << 3 << QDate() << false << 36000 << S("00:00-10:00:3c1 10:00-24:00:1") << S("7>1");
        QTest::newRow("date Tue") << -1 << QDate(2026, 10, 27) << false << 36000 << S("00:00-10:00:1c1 10:00-24:00:5") << S("3>5 9>5 1>5 7>5");
        QTest::newRow("date Wed") << -1 << QDate(2026, 10, 28) << false << 36000 << S("00:00-10:00:5c1 10:00-24:00:1") << S("7>1");
        QTest::newRow("same grid Tue") << 2 << QDate() << true << 36000 << S("00:00-10:00:1c1 10:00-24:00:1") << S("3>1 7>1 9>1");
        QTest::newRow("same date Tue") << -1 << QDate(2026, 10, 27) << true << 36000 << S("00:00-10:00:1c1 10:00-24:00:1") << S("3>1 5>1 7>1 9>1");
        QTest::newRow("day change at midnight") << 2 << QDate() << false << 0 << S("00:00-10:00:7 10:00-24:00:3") << S("9>3 1>3");
    }
    void timelinePriorityTies() {
        QFETCH(int, dow);
        QFETCH(QDate, date);
        QFETCH(bool, same);
        QFETCH(int, dayChangeSec);
        QFETCH(QString, windows);
        QFETCH(QString, dropped);
        QCOMPARE(QDate(2026, 10, 27).dayOfWeek(), 2);   // a Tuesday (Qt: 1 Monday)
        const Document doc = parse(crlf({S("0000-00-00T10:00:00\tPlayMode=1"), S(R"(S:\every.mpg)"), S("0000-00-03T10:00:00\tPlayMode=1"),
                                         S(R"(S:\tuesday.mpg)"), S("2026-10-27T10:00:00\tPlayMode=1"), S(R"(S:\dated.mpg)"),
                                         S("0000-00-80T04:00:00\tPlayMode=1"), S(R"(S:\daychange.mpg)"), S("0000-00-03T10:00:00\tPlayMode=1"),
                                         S(R"(S:\tuesday2.mpg)")}));
        tl::Options o;
        o.samePriority = same;
        o.dayChangeSec = dayChangeSec;
        tl::Builder b(doc, S("ties.in"), o);
        const tl::DayTimeline t = b.build(dow >= 0 ? tl::Day::weekday(dow) : tl::Day::of(date));
        QCOMPARE(winList(t), windows);
        QStringList got;
        for (const tl::Dropped& x : t.dropped) got << QStringLiteral("%1>%2").arg(x.line).arg(x.keptLine);
        QCOMPARE(got.join(' '), dropped);
        // One MP015 per dropped event, at its header, naming the kept one and why.
        QCOMPARE(b.notes().size(), t.dropped.size());
        for (int i = 0; i < t.dropped.size(); ++i) {
            const tl::Note& n = b.notes()[i];
            QCOMPARE(n.diag.code, S("MP015"));
            QCOMPARE(n.diag.line, t.dropped[i].line);
            QCOMPARE(n.file, S("ties.in"));
            QVERIFY2(n.diag.message.contains(QStringLiteral("(line %1)").arg(t.dropped[i].keptLine)), qPrintable(n.diag.message));
            const bool equal = same || t.dropped[i].priority == t.dropped[i].keptPriority;
            QVERIFY2(n.diag.message.contains(S("equal priority")) == equal, qPrintable(n.diag.message));
        }
    }

    void timelineWeekWrap() {
        // One weekly event (Wednesday 10:00) runs the whole week: Wednesday morning continues last week's.
        const Document wed = parse(crlf({S("0000-00-04T10:00:00\tPlayMode=1"), S(R"(S:\wed.mpg)")}));
        tl::Builder b(wed);
        const QList<tl::DayTimeline> w = b.week();
        QCOMPARE(winList(w[3]), S("00:00-10:00:1c7 10:00-24:00:1"));
        QCOMPARE(w[3].windows[0].offsetSec, qint64(7) * 86400 - 36000);
        QCOMPARE(winList(w[4]), S("00:00-24:00:1c1"));
        QCOMPARE(winList(w[0]), S("00:00-24:00:1c4"));
        QCOMPARE(winList(w[2]), S("00:00-24:00:1c6"));
        // Nothing to carry: an empty playlist, only the end-of-list sentinel, only a bad stamp, only a preamble.
        const QList<QByteArray> nothing = {QByteArray(), crlf({S("9999-99-99T00:00:00")}), crlf({S("2025-13-01T00:00:00\tPlayMode=1"), S(R"(S:\x.mpg)")}),
                                           crlf({S(R"(S:\pre.mpg)")})};
        for (const QByteArray& text : nothing) {
            tl::Builder e(parse(text));
            for (const tl::DayTimeline& t : e.week()) QVERIFY(t.windows.isEmpty());
            QVERIFY(e.notes().isEmpty());
            QCOMPARE(e.stats().value("windows").toInt(), 0);
        }
        QCOMPARE(tl::Builder(parse(nothing.last())).stats().value("preambleItems").toInt(), 1);

        // timeShiftMin: +60 moves Saturday 23:30 to Sunday 00:30 and every day 00:30 to 01:30; -60 moves them to Saturday
        // 22:30 and the evening before (23:30).
        const Document sh = parse(crlf({S("0000-00-00T00:30:00\tPlayMode=1"), S(R"(S:\every.mpg)"), S("0000-00-07T23:30:00\tPlayMode=1"), S(R"(S:\sat.mpg)")}));
        tl::Options plus;
        plus.timeShiftMin = 60;
        tl::Builder p(sh, {}, plus);
        const tl::DayTimeline psun = p.build(tl::Day::weekday(0));
        QCOMPARE(winList(psun), S("00:00-00:30:1c1 00:30-01:30:3 01:30-24:00:1"));
        QCOMPARE(psun.windows[1].eventSec, 1800);
        QCOMPARE(winList(p.build(tl::Day::weekday(6))), S("00:00-01:30:1c1 01:30-24:00:1"));
        tl::Options minus;
        minus.timeShiftMin = -60;
        tl::Builder m(sh, {}, minus);
        QCOMPARE(winList(m.build(tl::Day::weekday(6))), S("00:00-22:30:1c1 22:30-23:30:3 23:30-24:00:1"));
        QCOMPARE(winList(m.build(tl::Day::weekday(0))), S("00:00-23:30:1c1 23:30-24:00:1"));
        // A dated event shifted over midnight moves to the next date.
        const Document dsh = parse(crlf({S("2025-09-10T23:30:00\tPlayMode=1"), S(R"(S:\d.mpg)")}));
        tl::Builder dp(dsh, {}, plus);
        QVERIFY(dp.build(tl::Day::of(QDate(2025, 9, 10))).windows.isEmpty());
        QCOMPARE(winList(dp.build(tl::Day::of(QDate(2025, 9, 11)))), S("00:30-24:00:1"));
        // The shift is clamped to a day.
        tl::Options huge;
        huge.timeShiftMin = 5000;
        QCOMPARE(tl::Builder(sh, {}, huge).options().timeShiftMin, 1440);
    }

    void timelineDayChange() {
        const Document dc = parse(crlf({S("0000-00-80T04:00:00\tPlayMode=1"), S(R"(S:\dc.mpg)")}));
        // By default the day change is at midnight, whatever time the stamp holds.
        tl::DayTimeline t = tl::Builder(dc).build(tl::Day::weekday(1));
        QCOMPARE(winList(t), S("00:00-24:00:1"));
        QCOMPARE(t.windows[0].priority, 0);
        QCOMPARE(t.windows[0].eventSec, 0);
        // TagesWechsel and EventsDoesHaveSamePriority from Mplay.INI.
        const ini::MplayIni ini = ini::readMplayIni(QByteArray("[MPEGPlay]\r\nEventsDoesHaveSamePriority=1\r\n[PlayListEditor]\r\nTagesWechsel=06:00\r\nVideoformat=25\r\n"));
        const tl::Options o = tl::Options::fromIni(ini);
        QCOMPARE(o.dayChangeSec, 21600);
        QVERIFY(o.samePriority);
        QCOMPARE(o.parse.fps, 25);
        t = tl::Builder(dc, {}, o).build(tl::Day::weekday(1));
        QCOMPARE(winList(t), S("00:00-06:00:1c1 06:00-24:00:1"));
        QCOMPARE(t.windows[1].eventSec, 21600);
        // The sample INI: TagesWechsel=00:00, no equal priorities, Videoformat 23.98 -> 24 fps.
        const tl::Options s = tl::Options::fromIni(ini::readMplayIni(readAll(fx("mplay_sample.ini"))));
        QCOMPARE(s.dayChangeSec, 0);
        QVERIFY(!s.samePriority);
        QCOMPARE(s.parse.fps, 24);
    }

    void timelineChangePlaylist() {
        // sample.in ends with a bare !ChangePlaylist: the cut is the last line, there is nothing to follow (MP017).
        {
            tl::Builder b(parse(readAll(fx("sample.in"))), fx("sample.in"), {}, tl::memoryReader({}));
            const tl::DayTimeline t = b.build(tl::Day::weekday(0));
            QCOMPARE(winList(t), S("00:00-24:00:1"));
            const tl::Window& w = t.windows[0];
            QCOMPARE(w.items.size(), 13);
            QCOMPARE(w.cutAt, 12);
            QCOMPARE(w.cutItems, 0);
            QCOMPARE(w.items[12].status, S("notFollowed"));
            QCOMPARE(w.items[12].why, S("no playlist named"));
            QCOMPARE(w.playMode, 2);
            QCOMPARE(w.playModeFrom, S("inBand"));
            QCOMPARE(w.repeatAt, 0);
            QCOMPARE(w.loopFrom, 1);
            QCOMPARE(noteList(b.notes()), S("sample.in:14:MP017"));
            QCOMPARE(b.notes()[0].diag.col, 2);
            QVERIFY(b.notes()[0].diag.message.contains(S("names no playlist")));
        }
        // followChangePlaylist on the fixture's Thursday: the evening list's event in force at 12:00 (its 10:00 repeat
        // block) plays after the change line, and its 18:00 event replaces the root's 20:00 one. That event changes back
        // to sub_week.in: a change cycle, not followed.
        {
            const QString root = fx("sub_week.in");
            tl::Options o;
            o.followChangePlaylist = true;
            tl::Builder b(parse(readAll(root)), root, o, tl::fileReader(fixtureRules()));
            const tl::DayTimeline thu = b.build(tl::Day::weekday(4));
            QCOMPARE(winList(thu), S("00:00-06:00:12c1 06:00-09:00:1 09:00-12:00:3 12:00-18:00:14 18:00-24:00:6"));
            const tl::Window* noon = windowAtLine(thu, 14);
            QVERIFY(noon);
            QCOMPARE(itemList(*noon), S("15/0 16/0+2:followed 4/1 5/1"));
            const tl::Item& ch = noon->items[1];
            QCOMPARE(ch.targetPlayMode, 2);
            QCOMPARE(ch.targetLoopFrom, 3);
            QCOMPARE(ch.replacedWindows, 1);
            QCOMPARE(ch.targetPath.toCaseFolded(), QDir::toNativeSeparators(fx("sub_evening.in")).toCaseFolded());
            QVERIFY(ch.why.isEmpty());
            QVERIFY(noon->items[2].viaChange && noon->items[3].viaChange);
            QCOMPARE(noon->items[2].parent, 1);
            QCOMPARE(noon->items[3].parent, 1);
            QVERIFY(noon->items[2].file.endsWith(S("sub_evening.in"), Qt::CaseInsensitive));
            QVERIFY(!noon->items[2].group.isEmpty());
            QCOMPARE(noon->playMode, 1);      // the window's own event: the target's !Repeat plays after the change
            QCOMPARE(noon->playModeFrom, S("header"));
            QCOMPARE(noon->repeatAt, -1);
            QCOMPARE(noon->loopFrom, -1);
            QCOMPARE(noon->cutAt, 1);
            QCOMPARE(noon->cutItems, 2);
            const tl::Window& last = thu.windows.last();
            QVERIFY(last.file.endsWith(S("sub_evening.in"), Qt::CaseInsensitive));
            QCOMPARE(last.line, 6);
            QCOMPARE(last.changeDepth, 1);
            QCOMPARE(last.via, root + S("#16"));
            QCOMPARE(itemList(last), S("7/0 8/0:notFollowed"));
            QCOMPARE(last.items[1].why, S("cycle"));
            QCOMPARE(last.cutItems, 1);
            QCOMPARE(noteList(b.notes()), S("sub_evening.in:8:MP017"));
            QCOMPARE(b.reads(), 3);   // sub_news_inner (the carry window), sub_evening, sub_week through the evening change
            QCOMPARE(b.stats().value("changes").toObject().value("followed").toInt(), 1);
            QCOMPARE(b.stats().value("changes").toObject().value("notFollowed").toInt(), 1);
            QCOMPARE(thu.toJson().value("windows").toArray().last().toObject().value("via").toString(), root + S("#16"));
        }
        // A change inside a header-less sub-playlist ends the parent's content too.
        const QHash<QString, QByteArray> files{
            {S(R"(M:\sub.in)"), crlf({S(R"(S:\s1.mpg)"), S(R"(!ChangePlaylist M:\other.in)"), S(R"(S:\s2.mpg)")})},
            {S(R"(M:\other.in)"), crlf({S("0000-00-00T00:00:00\tPlayMode=1"), S(R"(S:\o1.mpg)"), S("0000-00-00T12:00:00\tPlayMode=2"), S(R"(S:\o2.mpg)")})}};
        const Document root = parse(crlf({S("0000-00-00T00:00:00\tPlayMode=1"), S(R"(S:\a.mpg)"), S(R"(!InsertPlaylist M:\sub.in)"), S(R"(S:\never.mpg)")}));
        {
            tl::Builder b(root, S(R"(M:\root.in)"), {}, tl::memoryReader(files));
            const tl::DayTimeline t = b.build(tl::Day::weekday(2));
            QCOMPARE(winList(t), S("00:00-24:00:1"));
            QCOMPARE(itemList(t.windows[0]), S("2/0 3/0+2:expanded 1/1 2/1:notFollowed"));
            QCOMPARE(t.windows[0].cutAt, 3);
            QCOMPARE(t.windows[0].cutItems, 2);
            QCOMPARE(t.windows[0].items[1].subEventLine, 0);   // the preamble was inlined
            QCOMPARE(noteList(b.notes()), S("sub.in:2:MP017"));
        }
        tl::Options o;
        o.followChangePlaylist = true;
        {
            tl::Builder b(root, S(R"(M:\root.in)"), o, tl::memoryReader(files));
            const tl::DayTimeline t = b.build(tl::Day::weekday(2));
            QCOMPARE(winList(t), S("00:00-12:00:1 12:00-24:00:3"));
            QCOMPARE(itemList(t.windows[0]), S("2/0 3/0+2:expanded 1/1 2/1+1:followed 2/2"));
            QVERIFY(t.windows[0].items[4].viaChange);
            QCOMPARE(t.windows[0].items[4].parent, 3);
            QCOMPARE(t.windows[0].items[3].targetPlayMode, 1);
            QCOMPARE(t.windows[0].items[3].replacedWindows, 0);
            QCOMPARE(t.windows[1].file, S(R"(M:\other.in)"));
            QCOMPARE(t.windows[1].changeDepth, 1);
            QCOMPARE(t.windows[1].via, S(R"(M:\sub.in#2)"));
            QCOMPARE(t.windows[1].playMode, 2);
            QCOMPARE(t.windows[1].loopFrom, 0);
            QVERIFY(b.notes().isEmpty());
        }
        // Follow refused: the target cannot be read; more hops than maxDepth.
        {
            const Document miss = parse(crlf({S("0000-00-00T00:00:00\tPlayMode=1"), S(R"(!ChangePlaylist M:\nope.in)")}));
            tl::Builder b(miss, S(R"(M:\root.in)"), o, tl::memoryReader({}));
            const tl::DayTimeline t = b.build(tl::Day::weekday(0));
            QCOMPARE(t.windows[0].items[0].why, S("missing: not found"));
            QCOMPARE(noteList(b.notes()), S("root.in:2:MP017"));
            QVERIFY(b.notes()[0].diag.message.contains(S("could not be read (not found)")));
        }
        {
            QHash<QString, QByteArray> chain;
            for (int i = 1; i <= 4; ++i)
                chain.insert(QStringLiteral("M:\\c%1.in").arg(i), crlf({S("0000-00-00T00:00:00\tPlayMode=1"), QStringLiteral("!ChangePlaylist M:\\c%1.in").arg(i + 1)}));
            const Document c0 = parse(crlf({S("0000-00-00T00:00:00\tPlayMode=1"), S(R"(!ChangePlaylist M:\c1.in)")}));
            tl::Options h = o;
            h.maxDepth = 2;
            tl::Builder b(c0, S(R"(M:\c0.in)"), h, tl::memoryReader(chain));
            const tl::DayTimeline t = b.build(tl::Day::weekday(0));
            QCOMPARE(itemList(t.windows[0]), S("2/0+2:followed 2/1+1:followed 2/2:notFollowed"));
            QCOMPARE(t.windows[0].items[2].why, S("more than 2 ChangePlaylist hops"));
            QCOMPARE(noteList(b.notes()), S("c2.in:2:MP017"));
        }
    }

    void timelineSubPlaylistLimits() {
        // A chain d1 -> d2 -> ... -> d6 (header-less, relative names): maxDepth 4 (the default) expands to depth 4 and
        // refuses the next !InsertPlaylist (MP023 at its line in d4.in).
        QHash<QString, QByteArray> files;
        for (int i = 1; i <= 6; ++i)
            files.insert(QStringLiteral("M:\\d%1.in").arg(i), crlf({QStringLiteral("S:\\d%1.mpg").arg(i), QStringLiteral("!InsertPlaylist d%1.in").arg(i + 1)}));
        const Document root = parse(crlf({S("0000-00-00T00:00:00\tPlayMode=1"), S(R"(S:\d0.mpg)"), S(R"(!InsertPlaylist M:\d1.in)")}));
        auto run = [&](int maxDepth, QString* notes) {
            tl::Options o;
            o.maxDepth = maxDepth;
            tl::Builder b(root, S(R"(M:\d0.in)"), o, tl::memoryReader(files));
            const tl::DayTimeline t = b.build(tl::Day::weekday(0));
            *notes = noteList(b.notes());
            return itemList(t.windows[0]);
        };
        QString notes;
        QCOMPARE(run(4, &notes), S("2/0 3/0+8:expanded 1/1 2/1+6:expanded 1/2 2/2+4:expanded 1/3 2/3+2:expanded 1/4 2/4:depth"));
        QCOMPARE(notes, S("d4.in:2:MP023"));
        QCOMPARE(run(1, &notes), S("2/0 3/0+2:expanded 1/1 2/1:depth"));
        QCOMPARE(notes, S("d1.in:2:MP023"));
        QCOMPARE(run(0, &notes), S("2/0 3/0:depth"));
        QCOMPARE(notes, S("d0.in:3:MP023"));
        // Expansion off: nothing is read.
        tl::Options off;
        off.expandSubPlaylists = false;
        tl::Builder ob(root, S(R"(M:\d0.in)"), off, tl::memoryReader(files));
        QCOMPARE(itemList(ob.build(tl::Day::weekday(0)).windows[0]), S("2/0 3/0:off"));
        QCOMPARE(ob.reads(), 0);
        QVERIFY(ob.notes().isEmpty());
        // Pasted text cannot resolve a relative name; without a reader nothing is read at all.
        const Document rel = parse(crlf({S("0000-00-00T00:00:00\tPlayMode=1"), S("!InsertPlaylist news.in")}));
        tl::Builder pb(rel, QString(), {}, tl::fileReader());
        const tl::DayTimeline pt = pb.build(tl::Day::weekday(0));
        QCOMPARE(itemList(pt.windows[0]), S("2/0:missing"));
        QVERIFY2(pt.windows[0].items[0].why.contains(S("relative name")), qPrintable(pt.windows[0].items[0].why));
        QCOMPARE(noteList(pb.notes()), S(":2:MP024"));
        tl::Builder nb(rel);
        QVERIFY(nb.build(tl::Day::weekday(0)).windows[0].items[0].why.startsWith(S("no reader")));
        QCOMPARE(nb.reads(), 0);
        // An empty target and one whose only event is on another weekday (it is still the one in force): the first
        // marker is the Sunday list's insert, and its !Break decides the play mode.
        const QHash<QString, QByteArray> subs{{S(R"(M:\empty.in)"), crlf({S("0000-00-00T00:00:00\tPlayMode=1")})},
                                              {S(R"(M:\sunday.in)"), crlf({S("0000-00-01T05:00:00\tPlayMode=4"), S("!Break\t#=1"), S(R"(S:\spot.mpg)")})}};
        const Document es = parse(crlf({S("0000-00-04T10:00:00\tPlayMode=1"), S(R"(!InsertPlaylist M:\sunday.in)"), S(R"(!InsertPlaylist M:\empty.in)")}));
        tl::Builder eb(es, S(R"(M:\r.in)"), {}, tl::memoryReader(subs));
        const tl::Window ew = eb.build(tl::Day::weekday(3)).windows.last();
        QCOMPARE(itemList(ew), S("2/0+2:expanded 2/1 3/1 3/0:empty"));
        QCOMPARE(ew.items[0].subEventLine, 1);
        QCOMPARE(ew.items[0].subPlayMode, 4);
        QCOMPARE(ew.playMode, 4);
        QCOMPARE(ew.playModeFrom, S("sub"));
        // An empty target first: its header mode (normal) decides.
        const Document es2 = parse(crlf({S("0000-00-04T10:00:00\tPlayMode=2"), S(R"(!InsertPlaylist M:\empty.in)"), S(R"(S:\after.mpg)")}));
        const tl::Window ew2 = tl::Builder(es2, S(R"(M:\r.in)"), {}, tl::memoryReader(subs)).build(tl::Day::weekday(3)).windows.last();
        QCOMPARE(ew2.playMode, 1);
        QCOMPARE(ew2.playModeFrom, S("sub"));
        QCOMPARE(ew2.loopFrom, -1);

        // Fan-out: 30 inserts of a list of 30 inserts of a list of 30 clips (27,930 items) stop at maxItems (MP023).
        QHash<QString, QByteArray> fan;
        QStringList f0{S("0000-00-00T00:00:00\tPlayMode=1")}, f1, f2;
        for (int i = 0; i < 30; ++i) {
            f0 << S(R"(!InsertPlaylist M:\f1.in)");
            f1 << S(R"(!InsertPlaylist M:\f2.in)");
            f2 << QStringLiteral("S:\\clip%1.mpg").arg(i);
        }
        fan.insert(S(R"(M:\f1.in)"), crlf(f1));
        fan.insert(S(R"(M:\f2.in)"), crlf(f2));
        const Document fd = parse(crlf(f0));
        QElapsedTimer timer;
        timer.start();
        tl::Builder fb(fd, S(R"(M:\f0.in)"), {}, tl::memoryReader(fan));
        const tl::Window fw = fb.build(tl::Day::weekday(0)).windows[0];
        QVERIFY(fw.limited);
        QCOMPARE(fw.items.size(), 20000);
        QCOMPARE(fb.reads(), 2);
        QCOMPARE(fb.notes().size(), 1);
        QCOMPARE(fb.notes()[0].diag.code, S("MP023"));
        QVERIFY(fb.notes()[0].diag.message.contains(S("maxItems")));
        QCOMPARE(fb.stats().value("limited").toInt(), 1);
        tl::Options big;
        big.maxItems = 100000;
        const tl::Window bw = tl::Builder(fd, S(R"(M:\f0.in)"), big, tl::memoryReader(fan)).build(tl::Day::weekday(0)).windows[0];
        QVERIFY(!bw.limited);
        QCOMPARE(bw.items.size(), 30 + 30 * 30 + 30 * 30 * 30);
        const qint64 ms = timer.elapsed();
        qInfo("fan-out: 20,000 items (capped) and 27,930 items in %lld ms", ms);
        QVERIFY2(ms < 10000, qPrintable(QString::number(ms)));
    }

    void timelineFixtures() {
        // Every .in fixture, the week and one date, nothing readable: windows tile the day from the first one to 24:00,
        // depth-0 items are lines of their event, every note is a timeline code with its catalog severity.
        const QStringList files = QDir(QStringLiteral(MP_FIXTURES)).entryList({"*.in"}, QDir::Files, QDir::Name);
        QVERIFY(files.size() >= 18);
        const QStringList codes{S("MP015"), S("MP017"), S("MP023"), S("MP024")};
        for (const QString& f : files) {
            const Document doc = parse(readAll(fx(f)));
            tl::Builder b(doc, fx(f), {}, tl::memoryReader({}));
            QList<tl::DayTimeline> days = b.week();
            days << b.build(tl::Day::of(QDate(2025, 9, 10)));
            for (const tl::DayTimeline& t : days) {
                for (int i = 0; i < t.windows.size(); ++i) {
                    const tl::Window& w = t.windows[i];
                    const QString where = f + QLatin1Char(' ') + t.day.label() + QLatin1Char(' ') + hm(w.startSec);
                    QVERIFY2(w.startSec < w.endSec, qPrintable(where));
                    QVERIFY2(w.endSec == (i + 1 < t.windows.size() ? t.windows[i + 1].startSec : tl::kDaySec), qPrintable(where));
                    QVERIFY2(!w.carry || (i == 0 && w.startSec == 0), qPrintable(where));
                    const Event& ev = doc.events.at(w.eventIndex);
                    QCOMPARE(doc.header(ev).line, w.line);
                    QSet<int> lines;
                    for (const int k : ev.items) lines << doc.entries.at(k).line;
                    for (const tl::Item& it : w.items)
                        if (it.depth == 0) QVERIFY2(lines.contains(it.entry.line), qPrintable(where));
                    QVERIFY2(w.playMode >= 1 && w.playMode <= 4, qPrintable(where));
                }
            }
            for (const tl::Note& n : b.notes()) {
                QVERIFY2(codes.contains(n.diag.code), qPrintable(f + ": " + n.diag.code));
                QCOMPARE(n.diag.severity, diagnosticInfo(n.diag.code).severity);
                QVERIFY(!n.days.isEmpty());
            }
        }
        // grammar_basic: the every-day 00:00 repeat block opens each day (no carry needed), the day-change marker at
        // midnight loses to it (MP015 on every day), the Monday show's sub-playlist is not here (MP024).
        const Document g = parse(readAll(fx("grammar_basic.in")));
        tl::Builder gb(g, fx("grammar_basic.in"), {}, tl::memoryReader({}));
        const QList<tl::DayTimeline> gw = gb.week();
        QCOMPARE(winList(gw[0]), S("00:00-05:00:3 05:00-06:00:36 06:00-14:00:7 14:00-24:00:48"));
        QCOMPARE(winList(gw[1]), S("00:00-06:00:3 06:00-14:00:10 14:00-24:00:48"));
        QCOMPARE(winList(gw[2]), S("00:00-14:00:3 14:00-16:00:48 16:00-24:00:14"));
        QCOMPARE(winList(gw[6]), S("00:00-05:00:3 05:00-14:00:37 14:00-23:00:48 23:00-24:00:32"));
        QCOMPARE(winList(gb.build(tl::Day::of(QDate(2025, 9, 10)))), S("00:00-09:07:3 09:07-12:00:39 12:00-14:00:20 14:00-24:00:48"));
        QCOMPARE(noteList(gb.notes()), S("grammar_basic.in:38:MP015 grammar_basic.in:11:MP024"));
        QCOMPARE(gb.notes()[0].days.size(), 8);
        QCOMPARE(windowAtLine(gw[0], 3)->loopFrom, 1);
    }

    void timelineJsonAndOptions() {
        // Options: JSON round trip, clamps, names.
        tl::Options o = tl::Options::fromJson(QJsonObject{{"maxDepth", 99}, {"maxItems", 5}, {"subPlaylists", "off"}, {"followChangePlaylist", true},
                                                          {"samePriority", true}, {"dayChange", "06:30"}, {"timeShiftMin", -5000}, {"fps", 25},
                                                          {"legacyDateOrder", "MDY"}});
        QCOMPARE(o.maxDepth, 8);
        QCOMPARE(o.maxItems, 100);
        QVERIFY(!o.expandSubPlaylists);
        QVERIFY(o.followChangePlaylist);
        QVERIFY(o.samePriority);
        QCOMPARE(o.dayChangeSec, 23400);
        QCOMPARE(o.timeShiftMin, -1440);
        QCOMPARE(o.parse.fps, 25);
        QCOMPARE(o.parse.legacyDateOrder, S("mdy"));
        QCOMPARE(tl::Options::fromJson(o.toJson()).toJson(), o.toJson());
        QVERIFY(tl::Options::fromJson(QJsonObject{{"subPlaylists", "link"}}).expandSubPlaylists);
        QCOMPARE(tl::Options::fromJson(QJsonObject{{"dayChangeSec", 3600}}).dayChangeSec, 3600);
        QCOMPARE(tl::Options::fromJson(QJsonObject{{"dayChange", "25:00"}}).dayChangeSec, 0);   // not a time of day: ignored
        QCOMPARE(tl::Options::fromJson({}).toJson(), tl::Options().toJson());
        // Days and priorities.
        QCOMPARE(tl::Day::weekday(-1).dow, 6);
        QCOMPARE(tl::Day::weekday(0).addDays(-1).label(), S("Sat"));
        QCOMPARE(tl::Day::weekday(9).weekdayCode(), 3);
        QCOMPARE(tl::Day::of(QDate(2025, 9, 10)).addDays(1).key(), S("2025-09-11"));
        QCOMPARE(tl::Day::of(QDate(2025, 9, 7)).weekdayCode(), 1);   // a Sunday
        QVERIFY(!tl::Day().isValid());
        QVERIFY(tl::Builder(Document()).build(tl::Day()).windows.isEmpty());
        QCOMPARE(tl::priorityName(tl::priorityOf(DayKind::Dated)), S("dated"));
        QCOMPARE(tl::priorityName(tl::priorityOf(DayKind::DayChange)), S("dayChange"));
        QCOMPARE(tl::priorityOf(DayKind::EndOfList), -1);
        QCOMPARE(tl::priorityOf(DayKind::Invalid), -1);
        // The JSON views of the fixture's Wednesday.
        const QString root = fx("sub_week.in");
        tl::Builder b(parse(readAll(root)), root, {}, tl::fileReader(fixtureRules()));
        const tl::DayTimeline wed = b.build(tl::Day::weekday(3));
        const QJsonObject j = wed.toJson();
        QCOMPARE(j.value("day").toString(), S("Wed"));
        QCOMPARE(j.value("key").toString(), S("3"));
        QCOMPARE(j.value("dow").toInt(), 3);
        QVERIFY(!j.contains("date"));
        QCOMPARE(j.value("datedSkipped").toInt(), 2);
        QCOMPARE(j.value("windows").toArray().size(), 5);
        const QJsonObject noon = j.value("windows").toArray().at(3).toObject();
        QCOMPARE(noon.value("start").toString(), S("12:00:00"));
        QCOMPARE(noon.value("end").toString(), S("19:00:00"));
        QCOMPARE(noon.value("startSec").toInt(), 43200);
        QCOMPARE(noon.value("line").toInt(), 8);
        QCOMPARE(noon.value("stamp").toString(), S("0000-00-04T12:00:00"));
        QCOMPARE(noon.value("dayKind").toString(), S("weekly"));
        QCOMPARE(noon.value("priority").toString(), S("weekly"));
        QCOMPARE(noon.value("eventTime").toString(), S("12:00:00"));
        QCOMPARE(noon.value("eventType").toInt(), 1);
        QCOMPARE(noon.value("playMode").toInt(), 2);
        QCOMPARE(noon.value("playModeFrom").toString(), S("sub"));
        QCOMPARE(noon.value("headerPlayMode").toInt(), 1);
        QCOMPARE(noon.value("name").toString(), S("Noon News"));
        QCOMPARE(noon.value("eventDurationSec").toDouble(), 1800.0);
        QCOMPARE(noon.value("repeatAt").toInt(), 5);
        QCOMPARE(noon.value("loopFrom").toInt(), 6);
        QCOMPARE(noon.value("itemCount").toInt(), 9);
        QCOMPARE(noon.value("items").toArray().size(), 9);
        QVERIFY(!noon.contains("carry") && !noon.contains("cutAt") && !noon.contains("via") && !noon.contains("changeDepth"));
        const QJsonObject ins = noon.value("items").toArray().at(1).toObject();
        QCOMPARE(ins.value("instruction").toString(), S("InsertPlaylist"));
        QCOMPARE(ins.value("target").toString(), S(R"(C:\Sync\NEWS HUB\sub_news.in)"));
        QCOMPARE(ins.value("status").toString(), S("expanded"));
        QCOMPARE(ins.value("children").toInt(), 6);
        QCOMPARE(ins.value("subEventLine").toInt(), 1);
        QCOMPARE(ins.value("subPlayMode").toInt(), 1);
        QVERIFY(!ins.contains("depth") && !ins.contains("file") && !ins.contains("parent"));
        const QJsonObject inner = noon.value("items").toArray().at(4).toObject();
        QCOMPARE(inner.value("depth").toInt(), 2);
        QCOMPARE(inner.value("parent").toInt(), 3);
        QVERIFY(inner.value("file").toString().endsWith(S("sub_news_inner.in"), Qt::CaseInsensitive));
        QVERIFY(!inner.value("group").toString().isEmpty());
        QCOMPARE(inner.value("kind").toString(), S("media"));
        QCOMPARE(inner.value("text").toString(), S(R"(S:\News\Weather Early.mpg)"));
        const QJsonObject carry = j.value("windows").toArray().at(0).toObject();
        QVERIFY(carry.value("carry").toBool());
        QCOMPARE(carry.value("carryDays").toInt(), 1);
        QCOMPARE(carry.value("offsetSec").toDouble(), 54000.0);
        QCOMPARE(carry.value("end").toString(), S("06:00:00"));
        QVERIFY(!wed.toJson(false).value("windows").toArray().at(3).toObject().contains("items"));
        // Sources and stats.
        const QJsonArray src = b.sourcesJson();
        QCOMPARE(src.size(), 3);
        QVERIFY(src[0].toObject().value("root").toBool());
        QCOMPARE(src[0].toObject().value("events").toInt(), 11);
        QVERIFY(!src[0].toObject().contains("bytes"));
        const QJsonObject s1 = src[1].toObject();
        QCOMPARE(s1.value("bytes").toInt(), int(readAll(fx("sub_news.in")).size()));
        QCOMPARE(s1.value("sha256").toString(), QString::fromLatin1(QCryptographicHash::hash(readAll(fx("sub_news.in")), QCryptographicHash::Sha256).toHex()));
        QCOMPARE(s1.value("encoding").toString(), S("utf8bom"));
        QCOMPARE(s1.value("events").toInt(), 1);
        QCOMPARE(s1.value("items").toInt(), 3);
        QCOMPARE(s1.value("diagnostics").toObject().value("warning").toInt(), 0);
        QCOMPARE(s1.value("target").toString(), S(R"(C:\Sync\NEWS HUB\sub_news.in)"));
        QCOMPARE(b.stats().value("inserts").toObject().value("expanded").toInt(), 3);
        QCOMPARE(b.stats().value("days").toInt(), 1);
        // Dropped and Note JSON from Monday.
        const tl::DayTimeline mon = b.build(tl::Day::weekday(1));
        const QJsonObject dj = mon.toJson().value("dropped").toArray().at(0).toObject();
        QCOMPARE(dj.value("line").toInt(), 1);
        QCOMPARE(dj.value("time").toString(), S("06:00:00"));
        QCOMPARE(dj.value("priority").toString(), S("everyDay"));
        QCOMPARE(dj.value("stamp").toString(), S("0000-00-00T06:00:00"));
        QCOMPARE(dj.value("kept").toObject().value("line").toInt(), 6);
        QCOMPARE(dj.value("kept").toObject().value("priority").toString(), S("weekly"));
        QCOMPARE(dj.value("kept").toObject().value("stamp").toString(), S("0000-00-02T06:00:00"));
        const QJsonObject nj = b.notesJson().at(0).toObject();
        QCOMPARE(nj.value("code").toString(), S("MP015"));
        QCOMPARE(nj.value("file").toString(), root);
        QCOMPARE(nj.value("days").toArray(), QJsonArray{"Mon"});
        QCOMPARE(nj.value("line").toInt(), 1);
        QCOMPARE(nj.value("col").toInt(), 1);
        QCOMPARE(nj.value("severity").toString(), S("warning"));
    }

    void timelineFileReader() {
        const tl::ReadFn rd = tl::fileReader(fixtureRules());
        const QString nativeDir = QDir::toNativeSeparators(QStringLiteral(MP_FIXTURES));
        tl::ReadResult r = rd(S(R"(C:\Sync\NEWS HUB\sub_news.in)"), QString());
        QVERIFY2(r.ok, qPrintable(r.why));
        QCOMPARE(r.path.toCaseFolded(), (nativeDir + S("\\sub_news.in")).toCaseFolded());
        QCOMPARE(r.bytes, readAll(fx("sub_news.in")));
        QVERIFY(rd(S("c:/sync/news hub/SUB_NEWS.IN"), QString()).ok);           // forward slashes, case
        QVERIFY(rd(S("sub_news_inner.in"), fx("sub_news.in")).ok);              // relative to the including file
        r = rd(S(R"(..\fixtures\sub_news_inner.in)"), fx("sub_news.in"));
        QVERIFY2(r.ok, qPrintable(r.why));
        QVERIFY(!r.path.contains(S("..")));
        r = rd(S(R"(C:\Sync\NEWS HUB\nope.in)"), QString());
        QVERIFY(!r.ok);
        QCOMPARE(r.why, S("not found"));
        QCOMPARE(rd(S(R"(C:\Sync\NEWS HUB\)"), QString()).why, S("is a folder"));
        QCOMPARE(rd(S(" "), QString()).why, S("no playlist named"));
        QVERIFY(rd(S("x.in"), QString()).why.contains(S("relative name")));
        r = tl::fileReader({}, 10)(fx("sub_news.in"), QString());
        QVERIFY(!r.ok);
        QVERIFY2(r.why.startsWith(S("too large")), qPrintable(r.why));
        r = rd(S(R"(Q:\Lists\x.in)"), QString());   // no Q: drive here (the resolver tests rely on that too)
        QVERIFY(!r.ok);
        QCOMPARE(r.why, S(R"(root unreachable: Q:\)"));
        QCOMPARE(rd(S(R"(Q:\Other\y.in)"), QString()).why, S(R"(root unreachable: Q:\)"));   // served from the root cache
        // The memory reader compares names as paths and tries relative names against the including file's folder.
        const tl::ReadFn mem = tl::memoryReader({{S(R"(M:\Lists\a.in)"), QByteArray("x")}});
        QVERIFY(mem(S("m:/lists/A.IN"), QString()).ok);
        QCOMPARE(mem(S("m:/lists/A.IN"), QString()).path, S(R"(M:\Lists\a.in)"));
        QVERIFY(mem(S("a.in"), S(R"(M:\Lists\root.in)")).ok);
        QVERIFY(!mem(S("a.in"), QString()).ok);
        QCOMPARE(mem(S("b.in"), S(R"(M:\Lists\root.in)")).why, S("not found"));
    }

    void timelinePlantCorpus() {
        // Opt-in (business data, never committed): every weekday of the plant playlist. The sub-playlists are not in the
        // samples folder, so the memory reader keeps this hermetic: each !InsertPlaylist line is MP024 once.
        const QString dir = qEnvironmentVariable("PLAYPLAY_MP_SAMPLES");
        if (dir.isEmpty()) QSKIP("PLAYPLAY_MP_SAMPLES is not set (point it at a folder holding plant_playlist.in)");
        const QByteArray bytes = readAll(QDir(dir).filePath("plant_playlist.in"));
        if (bytes.isEmpty()) QSKIP("plant_playlist.in not found under PLAYPLAY_MP_SAMPLES");
        const Document doc = parse(bytes);
        int insertLines = 0;
        for (const Entry& e : doc.entries)
            if (insertPlaylistSpec(e)) ++insertLines;
        QCOMPARE(insertLines, 272);
        QElapsedTimer t;
        t.start();
        tl::Builder b(doc, S("plant_playlist.in"), {}, tl::memoryReader({}));
        const QList<tl::DayTimeline> week = b.week();
        const qint64 ms = t.elapsed();
        const int perCode[7] = {58, 54, 52, 52, 52, 56, 50};
        int carry = 0;
        for (int d = 0; d < 7; ++d) {
            int own = 0;
            for (const tl::Window& w : week[d].windows) {
                if (w.carry) ++carry;
                else ++own;
                QCOMPARE(w.priority, 2);
            }
            QCOMPARE(own, perCode[d]);
            QVERIFY(week[d].dropped.isEmpty());
            QCOMPARE(week[d].windows.last().endSec, tl::kDaySec);
        }
        int mp024 = 0;
        for (const tl::Note& n : b.notes()) {
            QCOMPARE(n.diag.code, S("MP024"));
            ++mp024;
        }
        QCOMPARE(mp024, 272);
        const QJsonObject st = b.stats();
        qInfo("plant week: %d windows (%d carry), %d items, %d inserts missing, %d reads, %lld ms", st.value("windows").toInt(), carry,
              st.value("items").toInt(), st.value("inserts").toObject().value("missing").toInt(), b.reads(), ms);
        QVERIFY2(ms < 10000, qPrintable(QString::number(ms)));
    }

    // ---- MP-08a: the import plan ----------------------------------------------------------------------------------
    void importDatedMonday() {
        // import_times.in as a dated override of Monday 2025-09-08 (UTC): exact events at 08:30 / 09:07 / 13:20 / 16:00,
        // floating skip at 10:07 (capped at 300 s) and 11:07 (90 s late), floating play all at 00:00 and 12:00, programmes
        // crossing 01:00, 09:00, 10:00, 11:00, 12:00 and a movie covering 14:00 whole.
        const QByteArray bytes = readAll(fx("import_times.in"));
        QCOMPARE(bytes.size(), 1460);
        QVERIFY(bytes.startsWith("\xef\xbb\xbf"));
        QCOMPARE(bytes.count("\r\n"), 38);
        const Document doc = parse(bytes);
        QVERIFY2(doc.diagnostics.isEmpty(), qPrintable(diagList(doc.diagnostics)));
        QCOMPARE(doc.events.size(), 9);
        const res::Resolver rv(importLibrary(), importResOptions());
        const imp::Importer im(nullptr, &rv, tl::memoryReader({}));
        const imp::Plan p = im.plan(doc, S("import_times.in"), mondayOptions());
        QVERIFY2(p.ok(), qPrintable(p.errors.join(S("; "))));
        QCOMPARE(p.hourSlots.size(), 24);
        const QStringList expect = {
            S("movie@0:finish_previous"),                                                       // 00 play all, on time
            S("cont(2025-09-08-0,1800) spotA talk"),                                             // 01 the movie runs on
            S(""), S(""), S(""), S(""), S(""), S(""),                                            // 02-07 the overnight window
            S("news@1800:truncate"),                                                             // 08
            S("cont(2025-09-08-8,420) morning@420:truncate spotB spotA doc"),                   // 09 exact at :07 cuts the news
            S("cont(2025-09-08-9,210) talk weather@420:finish_previous/300 spotA news"),       // 10 skip at :07, capped (native)
            S("cont(2025-09-08-10,150) spotB weather morning@420:finish_previous/300 doc"),    // 11 skip at :07, 90 s late (native)
            S("cont(2025-09-08-11,210) movie"),                                                  // 12 play all waits for the doc (soft)
            S("cont(2025-09-08-12,1200) talk@1200:truncate spotB card doc movie"),              // 13 exact cuts the movie
            S("cont(2025-09-08-13,3600)"),                                                       // 14 pure continuation
            S("cont(2025-09-08-13,1580) spotA"),                                                 // 15
            S("weather@0:truncate"),                                                             // 16
            S(""), S(""), S(""), S(""), S(""), S(""), S(""),                                     // 17-23
        };
        for (int h = 0; h < 24; ++h) {
            QCOMPARE(p.hourSlots[h].id, QStringLiteral("2025-09-08-%1").arg(h));
            QCOMPARE(p.hourSlots[h].date, QDate(2025, 9, 8));
            QCOMPARE(p.hourSlots[h].channelId, S("mptest"));
            QVERIFY(!p.hourSlots[h].randomizeAds && p.hourSlots[h].keepProgramOrder && !p.hourSlots[h].isLooping);
            QCOMPARE(slotSummary(p.hourSlots[h]), expect[h]);
            QCOMPARE(p.hourStartMs[h], QDateTime(QDate(2025, 9, 8), QTime(h, 0), QTimeZone::UTC).toMSecsSinceEpoch());
            QCOMPARE(p.nextIndex[h], h < 23 ? h + 1 : -1);
            if (p.hourSlots[h].isContinuation()) QCOMPARE(p.hourSlots[h].mergedEvent.value("source").toString(), S("masterplay"));
        }
        // Provenance and the start rules.
        auto mpOf = [&](int h, int i) { return p.hourSlots[h].items[i].extra.value("masterplay").toObject(); };
        QCOMPARE(mpOf(0, 0).value("src").toString(), S("import_times.in#2"));
        QCOMPARE(mpOf(0, 0).value("eventSrc").toString(), S("import_times.in#1"));
        QCOMPARE(mpOf(0, 0).value("event").toString(), S("0000-00-00T00:00:00"));
        QCOMPARE(mpOf(0, 0).value("day").toString(), S("2025-09-08"));
        QCOMPARE(mpOf(0, 0).value("resolvedBy").toString(), S("path"));
        QCOMPARE(mpOf(0, 0).value("anchor").toObject(), (QJsonObject{{"time", "00:00:00"}, {"eventType", 2}, {"start", "onTime"}}));
        QCOMPARE(mpOf(10, 1).value("anchor").toObject(), (QJsonObject{{"time", "10:07:00"}, {"eventType", 1}, {"start", "native"}, {"lateSec", 300}}));
        QCOMPARE(mpOf(11, 2).value("anchor").toObject(), (QJsonObject{{"time", "11:07:00"}, {"eventType", 1}, {"start", "native"}, {"lateSec", 90}}));
        QCOMPARE(mpOf(12, 0).value("anchor").toObject(), (QJsonObject{{"time", "12:00:00"}, {"eventType", 2}, {"start", "soft"}, {"lateSec", 210}}));
        QCOMPARE(p.hourSlots[12].items[0].timingMode, S("soft_start"));
        QCOMPARE(p.hourSlots[12].items[0].fixedOffsetSec, -1.0);
        QVERIFY(!p.hourSlots[12].items[0].extra.contains("conflictPolicy"));
        // The !ShowLogo the overnight event holds rides on the item after it; the !Info and the missing file on the card.
        const QJsonArray logo = mpOf(1, 1).value("instructions").toArray();
        QCOMPARE(logo.size(), 1);
        QCOMPARE(logo[0].toObject().value("line").toInt(), 4);
        QCOMPARE(logo[0].toObject().value("name").toString(), S("ShowLogo"));
        QCOMPARE(logo[0].toObject().value("state").toString(), S("mapped"));
        QCOMPARE(logo[0].toObject().value("raw").toString(), S("!ShowLogo\tf=D:\\Media\\Logos\\bug.png\tLayer=1"));
        const QJsonArray before = mpOf(13, 2).value("instructions").toArray();
        QCOMPARE(before.size(), 2);
        QCOMPARE(before[0].toObject().value("name").toString(), S("Info"));
        QCOMPARE(before[1].toObject().value("line").toInt(), 30);
        QCOMPARE(before[1].toObject().value("state").toString(), S("dropped"));
        QCOMPARE(before[1].toObject().value("reason").toString(), S("missing"));
        const sched::SlotItem& card = p.hourSlots[13].items[2];
        QCOMPARE(card.mediaId, S("card"));
        QCOMPARE(card.outPointSec, 20.0);   // the playlist's forced length
        QCOMPARE(card.durationSec, 10.0);
        QCOMPARE(p.hourSlots[13].items[4].title, S("Matinee Movie"));   // Name= on the media line
        QCOMPARE(mpOf(13, 4).value("params").toArray(), (QJsonArray{"Name=Matinee Movie"}));
        // Show titles from Name=, provenance of the events in force.
        QCOMPARE(p.hourSlots[2].blockType, S("show"));
        QCOMPARE(p.hourSlots[2].showBlock.value("title").toString(), S("Overnight"));
        QCOMPARE(p.hourSlots[9].showBlock.value("title").toString(), S("Morning Edition"));   // 53 of its minutes
        QCOMPARE(p.hourSlots[8].showBlock.value("title").toString(), S("Breakfast"));
        QCOMPARE(p.hourSlots[11].blockType, S("general"));   // the 11:07 event has no Name= and holds most of the hour
        QCOMPARE(p.hourSlots[17].blockType, S("general"));
        const QJsonObject prov = p.hourSlots[9].showBlock.value("masterplay").toObject();
        QCOMPARE(prov.value("file").toString(), S("import_times.in"));
        QCOMPARE(prov.value("sha256").toString(), QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()));
        QCOMPARE(prov.value("events").toArray().size(), 2);
        QCOMPARE(prov.value("events").toArray()[1].toObject().value("start").toString(), S("09:07:00"));
        // Reports.
        QCOMPARE(p.dropped.size(), 2);
        QCOMPARE(p.dropped[0].toObject().value("line").toInt(), 25);
        QCOMPARE(p.dropped[0].toObject().value("reason").toString(), S("cutByEvent"));
        QCOMPARE(p.dropped[0].toObject().value("at").toString(), S("13:33:30"));
        QCOMPARE(p.dropped[1].toObject().value("reason").toString(), S("missing"));
        QCOMPARE(p.missing.size(), 1);
        QCOMPARE(p.missing[0].toObject().value("path").toString(), S(R"(D:\Media\Missing\Gone.mpg)"));
        QVERIFY(p.unsupported.isEmpty());   // MP-08b: the !ShowLogo is an overlay, the !Info a note
        QCOMPARE(card.note, S("Card follows"));
        QCOMPARE(p.overlays.size(), 1);
        const QJsonObject ov = p.overlays[0].toObject();
        QCOMPARE(ov.value("kind").toString(), S("logo"));
        QCOMPARE(ov.value("role").toString(), S("bug"));
        QCOMPARE(ov.value("mode").toString(), S("cue"));
        QCOMPARE(ov.value("line").toInt(), 4);
        QCOMPARE(ov.value("items").toInt(), 22);   // Talk at 01:30:30 and everything after it (no !OSD off)
        QCOMPARE(ov.value("end").toString(), S("end"));
        QCOMPARE(ov.value("firstSlot").toString(), S("2025-09-08-1"));
        QCOMPARE(ov.value("lastSlot").toString(), S("2025-09-08-16"));
        QVERIFY(p.hourSlots[0].items[0].cgOverlays.isEmpty() && p.hourSlots[1].items[0].cgOverlays.isEmpty());
        const QJsonObject cue = p.hourSlots[1].items[1].cgOverlays[0].toObject();
        QCOMPARE(cue.value("filePath").toString(), S(R"(D:\Media\Logos\bug.png)"));
        QCOMPARE(cue.value("role").toString(), S("bug"));
        QCOMPARE(cue.value("id").toString(), ov.value("id").toString());
        QCOMPARE(p.hourSlots[16].items[0].cgOverlays, p.hourSlots[1].items[1].cgOverlays);
        QCOMPARE(diagCodes(p.diagnostics), S("import_times.in:30:MP005 import_times.in:1:MP016 import_times.in:6:MP016 import_times.in:8:MP016 "
                                             "import_times.in:20:MP016 import_times.in:23:MP016"));
        for (const QJsonValue& v : p.diagnostics) QCOMPARE(v.toObject().value("severity").toString(), S("warning"));
        QVERIFY(p.notes.isEmpty());
        QVERIFY(p.onAirSlots.isEmpty());
        const QJsonObject st = p.stats;
        QCOMPARE(st.value("placed").toInt(), 24);
        QCOMPARE(st.value("dropped").toObject(), (QJsonObject{{"cutByEvent", 1}, {"missing", 1}}));
        QCOMPARE(st.value("starts").toObject(), (QJsonObject{{"onTime", 5}, {"native", 2}, {"soft", 1}}));
        QCOMPARE(st.value("hardStarts").toInt(), 7);
        QCOMPARE(st.value("continuations").toInt(), 8);
        QCOMPARE(st.value("joins").toInt(), 0);
        QCOMPARE(st.value("events").toInt(), 8);
        QCOMPARE(st.value("items").toInt(), 24);
        QCOMPARE(st.value("slots").toObject().value("empty").toInt(), 14);
        QCOMPARE(st.value("placements").toObject(), (QJsonObject{{"media", 26}, {"folder", 0}, {"live", 0}, {"structural", 0}, {"break", 0},
                                                                   {"osd", 1}, {"info", 1}, {"unsupported", 0}}));
        // Validation per slot: every item resolves, nothing is an error (truncations are warnings).
        QCOMPARE(p.validation.size(), 24);
        for (int h = 0; h < 24; ++h) {
            QCOMPARE(p.validation[h].toObject().value("slotId").toString(), p.hourSlots[h].id);
            QVERIFY2(p.validation[h].toObject().value("errors").toInt() == 0, qPrintable(QJsonDocument(p.validation[h].toObject()).toJson()));
        }
        QCOMPARE(st.value("validation").toObject().value("errors").toInt(), 0);

        // RunlistBuilder lays the slots the way the executor would: hard starts at their ms, continuation items whole.
        const QList<sched::Runlist> rls = layChain(p, im.mediaLookup());
        QCOMPARE(runlistSummary(rls[0]), S("movie 00:00:00-01:30:00 H"));
        QCOMPARE(runlistSummary(rls[1]), S("spotA 01:30:00-01:30:30 | talk 01:30:30-01:40:30"));
        QCOMPARE(runlistSummary(rls[8]), S("news 08:30:00-09:15:00 H"));
        QCOMPARE(runlistSummary(rls[9]), S("morning 09:07:00-09:37:00 H | spotB 09:37:00-09:38:00 | spotA 09:38:00-09:38:30 | doc 09:38:30-10:03:30"));
        QCOMPARE(runlistSummary(rls[10]), S("talk 10:03:30-10:12:00 T | weather 10:12:00-10:17:00 | spotA 10:17:00-10:17:30 | news 10:17:30-11:02:30"));
        QCOMPARE(runlistSummary(rls[11]), S("spotB 11:02:30-11:03:30 | weather 11:03:30-11:08:30 | morning 11:08:30-11:38:30 | doc 11:38:30-12:03:30"));
        QCOMPARE(runlistSummary(rls[12]), S("movie 12:03:30-13:33:30"));
        QCOMPARE(runlistSummary(rls[13]), S("talk 13:20:00-13:30:00 H | spotB 13:30:00-13:31:00 | card 13:31:00-13:31:20 | doc 13:31:20-13:56:20 | "
                                             "movie 13:56:20-15:26:20"));
        QVERIFY(rls[14].items.isEmpty());
        QCOMPARE(runlistSummary(rls[15]), S("spotA 15:26:20-15:26:50"));
        QCOMPARE(runlistSummary(rls[16]), S("weather 16:00:00-16:05:00 H"));
        const qint64 h9 = p.hourStartMs[9];
        QCOMPARE(rls[9].items[0].plannedStartMs, h9 + 420000);                      // the hard start at 09:07:00.000
        QCOMPARE(rls[9].items[0].toEngineJson().value("hardStartUtcMs").toDouble(), double(h9 + 420000));
        QCOMPARE(rls[10].items[1].plannedStartMs, p.hourStartMs[10] + 720000);      // 10:07 + maxLateSec
        QVERIFY(!rls[12].items[0].truncated);                                       // the continuation item is whole
        QCOMPARE(rls[12].items[0].plannedEndMs - rls[12].items[0].plannedStartMs, qint64(5400000));

        // The golden plan (slots, validation, reports, stats) and determinism.
        checkGolden(S("import_times_monday.json"), p.toJson());
        if (QTest::currentTestFailed()) return;
        QCOMPARE(im.plan(doc, S("import_times.in"), mondayOptions()).toJson(), p.toJson());
    }

    void importGridWeek() {
        const Document doc = parse(readAll(fx("import_times.in")));
        const res::Resolver rv(importLibrary(), importResOptions());
        const imp::Importer im(nullptr, &rv, tl::memoryReader({}));
        imp::Options o = mondayOptions();
        o.mode = S("grid");
        o.from = o.to = QDate();
        const imp::Plan g = im.plan(doc, S("import_times.in"), o);
        QVERIFY2(g.ok(), qPrintable(g.errors.join(S("; "))));
        QCOMPARE(g.hourSlots.size(), 168);   // the cyclic week covers every hour
        QCOMPARE(g.hourSlots.first().id, S("0-0"));
        QCOMPARE(g.hourSlots.last().id, S("6-23"));
        // Monday is the dated Monday (ids and day keys aside).
        const imp::Plan d = im.plan(doc, S("import_times.in"), mondayOptions());
        for (int h = 0; h < 24; ++h) {
            const sched::HourSlot* s = g.slot(QStringLiteral("1-%1").arg(h));
            QVERIFY(s);
            QCOMPARE(s->dayOfWeek, 1);
            QVERIFY(!s->date.isValid());
            QCOMPARE(slotSummary(*s).replace(S("cont(1-"), S("cont(2025-09-08-")), slotSummary(d.hourSlots[h]));
        }
        // The seam: Saturday's 23:30 movie runs into Sunday, whose every-day play-all event waits for it.
        QCOMPARE(slotSummary(*g.slot(S("6-23"))), S("movie@1800:truncate"));
        QCOMPARE(slotSummary(*g.slot(S("0-0"))), S("cont(6-23,3600)"));
        QCOMPARE(slotSummary(*g.slot(S("0-1"))), S("movie"));
        QCOMPARE(slotSummary(*g.slot(S("0-2"))), S("cont(0-1,1800) spotA talk"));
        QCOMPARE(slotSummary(*g.slot(S("6-0"))), S("movie@0:finish_previous"));
        QCOMPARE(slotSummary(*g.slot(S("2-0"))), S("movie@0:finish_previous"));
        const QJsonObject an = g.slot(S("0-1"))->items[0].extra.value("masterplay").toObject().value("anchor").toObject();
        QCOMPARE(an.value("start").toString(), S("soft"));
        QCOMPARE(an.value("lateSec").toDouble(), 3600.0);
        QCOMPARE(g.slot(S("6-23"))->items[0].extra.value("masterplay").toObject().value("day").toString(), S("Sat"));
        QCOMPARE(g.slot(S("6-23"))->showBlock.value("title").toString(), S("Late Movie"));
        QVERIFY2(g.notes.isEmpty(), qPrintable(g.notes.join(S("; "))));
        // Grid hours sit at their next occurrence on or after nowMs (Monday 2025-09-01); Saturday 23 continues into Sunday 0.
        const int i623 = g.indexOf(S("6-23")), i00 = g.indexOf(S("0-0")), i01 = g.indexOf(S("0-1"));
        QCOMPARE(g.hourStartMs[g.indexOf(S("1-0"))], QDateTime(QDate(2025, 9, 1), QTime(0, 0), QTimeZone::UTC).toMSecsSinceEpoch());
        QCOMPARE(g.hourStartMs[i623], QDateTime(QDate(2025, 9, 6), QTime(23, 0), QTimeZone::UTC).toMSecsSinceEpoch());
        QCOMPARE(g.hourStartMs[i00], QDateTime(QDate(2025, 9, 7), QTime(0, 0), QTimeZone::UTC).toMSecsSinceEpoch());
        QCOMPARE(g.nextIndex[i623], i00);
        QVERIFY(g.buildOptions(i623).continuesIntoNextHour);
        QVERIFY(g.buildOptions(i00).continuesFromPreviousHour);
        const sched::MediaLookup lk = im.mediaLookup();
        const sched::Runlist sat = sched::RunlistBuilder::build(g.hourSlots[i623], g.hourStartMs[i623], lk, g.buildOptions(i623));
        QCOMPARE(runlistSummary(sat), S("movie 23:30:00-01:00:00 H"));
        QVERIFY(sched::RunlistBuilder::build(g.hourSlots[i00], g.hourStartMs[i00], lk, g.buildOptions(i00)).items.isEmpty());
        const sched::Runlist sun1 = sched::RunlistBuilder::build(g.hourSlots[i01], g.hourStartMs[i01], lk, g.buildOptions(i01));
        QCOMPARE(runlistSummary(sun1), S("movie 01:00:00-02:30:00 H"));
        QCOMPARE(g.stats.value("events").toInt(), 15);   // the every-day event on 7 days, 7 Monday events, Saturday's
        QCOMPARE(g.validation.size(), 168);
        QCOMPARE(g.stats.value("validation").toObject().value("errors").toInt(), 0);

        // Golden: the seam hours only (weekdays Sunday and Saturday, hours 0-2 and 23).
        o.weekdays = {0, 6};
        o.hours = {0, 1, 2, 23};
        const imp::Plan s = im.plan(doc, S("import_times.in"), o);
        QStringList ids;
        for (const sched::HourSlot& h : s.hourSlots) ids << h.id;
        QCOMPARE(ids, (QStringList{S("0-0"), S("0-1"), S("0-2"), S("0-23"), S("6-0"), S("6-1"), S("6-2"), S("6-23")}));
        QCOMPARE(slotSummary(*s.slot(S("0-0"))), S("cont(6-23,3600)"));
        QCOMPARE(slotSummary(*s.slot(S("6-1"))), S("cont(6-0,1800) spotA talk"));
        QCOMPARE(s.stats.value("slots").toObject().value("filtered").toInt(), 160);
        checkGolden(S("import_times_seam.json"), s.toJson());
    }

    void importFiltersAndJoins() {
        const Document doc = parse(readAll(fx("import_times.in")));
        const res::Resolver rv(importLibrary(), importResOptions());
        const imp::Importer im(nullptr, &rv, tl::memoryReader({}));
        const sched::MediaLookup lk = im.mediaLookup();
        // Sunday only: Saturday's 23:30 slot is not written, so Sunday 0 joins the movie 30 minutes in.
        imp::Options o = mondayOptions();
        o.mode = S("grid");
        o.from = o.to = QDate();
        o.weekdays = {0};
        const imp::Plan sun = im.plan(doc, S("import_times.in"), o);
        QCOMPARE(sun.hourSlots.size(), 24);
        QCOMPARE(slotSummary(sun.hourSlots[0]), S("movie+in1800~j"));
        QVERIFY(!sun.hourSlots[0].isContinuation());
        QCOMPARE(sun.hourSlots[0].items[0].timingMode, S("soft_start"));
        QCOMPARE(sun.hourSlots[0].items[0].extra.value("masterplay").toObject().value("joinSec").toDouble(), 1800.0);
        QVERIFY(!sun.hourSlots[0].items[0].extra.value("masterplay").toObject().contains("anchor"));
        QCOMPARE(slotSummary(sun.hourSlots[1]), S("movie"));
        QCOMPARE(sun.stats.value("joins").toInt(), 1);
        QCOMPARE(runlistSummary(sched::RunlistBuilder::build(sun.hourSlots[0], sun.hourStartMs[0], lk, sun.buildOptions(0))),
                 S("movie 00:00:00-01:00:00 H"));
        // One dated hour: 09:00 joins the 08:30 news, whose hour is not written; the 09:07 hard start still cuts it.
        imp::Options h9 = mondayOptions();
        h9.hours = {9};
        const imp::Plan one = im.plan(doc, S("import_times.in"), h9);
        QCOMPARE(one.hourSlots.size(), 1);
        QCOMPARE(slotSummary(one.hourSlots[0]), S("news+in1800~j morning@420:truncate spotB spotA doc"));
        QCOMPARE(one.nextIndex[0], -1);
        QCOMPARE(runlistSummary(sched::RunlistBuilder::build(one.hourSlots[0], one.hourStartMs[0], lk, one.buildOptions(0))),
                 S("news 09:00:00-09:07:00 H T | morning 09:07:00-09:37:00 H | spotB 09:37:00-09:38:00 | spotA 09:38:00-09:38:30 | doc 09:38:30-10:00:00 T"));
        int filteredItems = 0;
        for (const imp::Placement& pl : one.placements) if (pl.output && pl.state == S("filtered")) ++filteredItems;
        QCOMPARE(filteredItems, 20);   // placed in hours this import does not write (24 placed, 4 in hour 9)
        QCOMPARE(one.stats.value("slots").toObject().value("filtered").toInt(), 23);
        // Uncovered hours: a lone dated event leaves the morning to the existing schedule, unless clearUncovered.
        const Document lone = parse(crlf({S("2025-09-08T12:00:00\tPlayMode=1"), S(R"(D:\Media\Prog\Talk.mpg)")}));
        const imp::Plan l1 = im.plan(lone, {}, mondayOptions());
        QCOMPARE(l1.hourSlots.size(), 12);
        QCOMPARE(l1.hourSlots[0].id, S("2025-09-08-12"));
        QCOMPARE(l1.stats.value("slots").toObject().value("uncovered").toInt(), 12);
        imp::Options cu = mondayOptions();
        cu.clearUncovered = true;
        const imp::Plan l2 = im.plan(lone, {}, cu);
        QCOMPARE(l2.hourSlots.size(), 24);
        QVERIFY(l2.hourSlots[3].items.isEmpty());
        QVERIFY(l2.hourSlots[3].showBlock.value("masterplay").toObject().value("uncovered").toBool());
        QCOMPARE(slotSummary(l2.hourSlots[12]), S("talk@0:truncate"));
    }

    void importCutMode() {
        // hourBoundary cut: an item crossing the hour ends there and the content moves up; no continuation anywhere.
        const Document doc = parse(readAll(fx("import_times.in")));
        const res::Resolver rv(importLibrary(), importResOptions());
        const imp::Importer im(nullptr, &rv, tl::memoryReader({}));
        imp::Options o = mondayOptions();
        o.hourBoundary = S("cut");
        const imp::Plan p = im.plan(doc, S("import_times.in"), o);
        QVERIFY(p.ok());
        for (const sched::HourSlot& s : p.hourSlots) QVERIFY2(!s.isContinuation(), qPrintable(s.id));
        QCOMPARE(slotSummary(p.hourSlots[0]), S("movie@0:finish_previous"));
        QVERIFY(p.hourSlots[0].items[0].extra.value("masterplay").toObject().value("cutAtHour").toBool());
        QCOMPARE(slotSummary(p.hourSlots[1]), S("spotA talk"));
        QVERIFY(!p.hourSlots[1].items[0].extra.value("masterplay").toObject().contains("cutAtHour"));
        QCOMPARE(slotSummary(p.hourSlots[9]), S("morning@420:truncate spotB spotA doc"));
        QCOMPARE(slotSummary(p.hourSlots[10]), S("talk weather@420:finish_previous/300 spotA news"));
        QCOMPARE(slotSummary(p.hourSlots[11]), S("spotB weather morning@420:finish_previous/300 doc"));
        QCOMPARE(slotSummary(p.hourSlots[12]), S("movie@0:finish_previous"));
        QCOMPARE(slotSummary(p.hourSlots[13]), S("spotA talk@1200:truncate spotB card doc movie"));
        QCOMPARE(slotSummary(p.hourSlots[14]), S("spotA"));
        QCOMPARE(p.stats.value("continuations").toInt(), 0);
        QCOMPARE(p.stats.value("dropped").toObject(), (QJsonObject{{"missing", 1}}));   // line 25 now airs at 13:00
        const QList<sched::Runlist> rls = layChain(p, im.mediaLookup());
        QCOMPARE(runlistSummary(rls[0]), S("movie 00:00:00-01:00:00 H T"));
        QCOMPARE(runlistSummary(rls[1]), S("spotA 01:00:00-01:00:30 H | talk 01:00:30-01:10:30"));
        QCOMPARE(runlistSummary(rls[10]), S("talk 10:00:00-10:10:00 H | weather 10:10:00-10:15:00 | spotA 10:15:00-10:15:30 | news 10:15:30-11:00:00 T"));
        int cutAtHour = 0;
        for (const sched::RunlistItem& r : rls[13].items) if (r.truncated) ++cutAtHour;
        QCOMPARE(cutAtHour, 1);   // the 13:56:20 movie ends at 14:00
    }

    void importDateRangeStart() {
        // A range's first day opens with the event in force from the day before: the item playing at midnight joins.
        const res::Resolver rv(importLibrary(), importResOptions());
        const imp::Importer im(nullptr, &rv, tl::memoryReader({}));
        const Document doc = parse(crlf({S("0000-00-07T23:30:00\tName=Late Movie\tPlayMode=1"), S(R"(D:\Media\Prog\Movie.mpg)")}));
        imp::Options o = mondayOptions();
        o.from = o.to = QDate(2025, 9, 7);   // Sunday
        o.nowMs = QDateTime(QDate(2025, 9, 7), QTime(0, 30), QTimeZone::UTC).toMSecsSinceEpoch();
        const imp::Plan p = im.plan(doc, {}, o);
        QCOMPARE(p.hourSlots.size(), 24);
        QCOMPARE(slotSummary(p.hourSlots[0]), S("movie+in1800"));
        QCOMPARE(p.hourSlots[0].items[0].extra.value("masterplay").toObject().value("joinSec").toDouble(), 1800.0);
        for (int h = 1; h < 24; ++h) QCOMPARE(slotSummary(p.hourSlots[h]), S(""));   // the carry window holds the day
        QCOMPARE(p.stats.value("carries").toInt(), 1);
        QCOMPARE(p.stats.value("events").toInt(), 0);
        QVERIFY(p.notes.isEmpty());
        QCOMPARE(p.onAirSlots, (QStringList{S("2025-09-07-0"), S("2025-09-07-1"), S("2025-09-07-2")}));
        QCOMPARE(runlistSummary(sched::RunlistBuilder::build(p.hourSlots[0], p.hourStartMs[0], im.mediaLookup(), p.buildOptions(0))),
                 S("movie 00:00:00-01:00:00 H"));
        // With Saturday in the range the movie is one item: 23:30 Saturday, continued by Sunday 0 (no join).
        o.from = QDate(2025, 9, 6);
        const imp::Plan two = im.plan(doc, {}, o);
        QCOMPARE(two.hourSlots.size(), 48);
        QCOMPARE(slotSummary(two.hourSlots[23]), S("movie@1800:truncate"));
        QCOMPARE(slotSummary(two.hourSlots[24]), S("cont(2025-09-06-23,3600)"));
        QCOMPARE(slotSummary(two.hourSlots[25]), S(""));
        QCOMPARE(two.stats.value("joins").toInt(), 0);
        QCOMPARE(two.notes.size(), 1);   // Sunday 0 and 1 hold no item: nothing is queued behind the movie
        QVERIFY2(two.notes[0].startsWith(S("2025-09-07-0 is covered by 'Movie' carried in from 2025-09-06-23")), qPrintable(two.notes[0]));
        // Saturday alone: the movie runs past the range, and the plan says so.
        o.to = QDate(2025, 9, 6);
        const imp::Plan sat = im.plan(doc, {}, o);
        QCOMPARE(sat.hourSlots.size(), 24);
        QCOMPARE(sat.notes.size(), 1);
        QVERIFY2(sat.notes[0].contains(S("runs 3600 s past the end of the range (into 2025-09-07-0")), qPrintable(sat.notes[0]));
    }

    void importEmptyContinuations() {
        // A 90-minute movie at 12:45 with nothing after it: 13 is covered whole, 14 for 15 minutes. ScheduleValidator lays an
        // item-less slot as a whole empty hour; the plan rewords that, and notes the executor's emergency-loop risk.
        const res::Resolver rv(importLibrary(), importResOptions());
        const imp::Importer im(nullptr, &rv, tl::memoryReader({}));
        const Document doc = parse(crlf({S("2025-09-08T12:45:00\tPlayMode=1"), S(R"(D:\Media\Prog\Movie.mpg)")}));
        const imp::Plan p = im.plan(doc, {}, mondayOptions());
        const int i13 = p.indexOf(S("2025-09-08-13")), i14 = p.indexOf(S("2025-09-08-14"));
        QCOMPARE(slotSummary(p.hourSlots[i13]), S("cont(2025-09-08-12,3600)"));
        QCOMPARE(slotSummary(p.hourSlots[i14]), S("cont(2025-09-08-12,900)"));
        auto issues = [&](int i) {
            QStringList o;
            for (const QJsonValue& v : p.validation[i].toObject().value("issues").toArray())
                o << v.toObject().value("code").toString() + QLatin1Char('/') + v.toObject().value("severity").toString() + QLatin1Char('/') +
                         v.toObject().value("message").toString();
            return o;
        };
        QCOMPARE(issues(i13), QStringList{S("gap/info/the hour is covered by 'Movie' carried in from 2025-09-08-12")});
        QCOMPARE(issues(i14), (QStringList{S("underfill/warning/2700.0 s unfilled after 'Movie' carried in from 2025-09-08-12 ends (fill policy will cover it at playout)")}));
        QCOMPARE(p.validation[i14].toObject().value("gapSeconds").toDouble(), 2700.0);
        QCOMPARE(p.validation[i14].toObject().value("warnings").toInt(), 1);
        QCOMPARE(p.notes.size(), 2);   // 13 and 14: nothing queued behind the movie for two hours
        QVERIFY2(p.notes[0].startsWith(S("2025-09-08-13 is covered by 'Movie'")) && p.notes[0].contains(S("emergency loop")), qPrintable(p.notes[0]));
        QVERIFY(p.notes[1].startsWith(S("2025-09-08-14 ")));
        // With an item at 14:30 the movie has something queued behind it: no note.
        const Document doc2 = parse(crlf({S("2025-09-08T12:45:00\tPlayMode=1"), S(R"(D:\Media\Prog\Movie.mpg)"), S("2025-09-08T14:30:00\tPlayMode=1"),
                                          S(R"(D:\Media\Spots\Spot A.mpg)")}));
        const imp::Plan p2 = im.plan(doc2, {}, mondayOptions());
        QCOMPARE(slotSummary(*p2.slot(S("2025-09-08-14"))), S("cont(2025-09-08-12,900) spotA@1800:truncate"));
        QVERIFY2(p2.notes.isEmpty(), qPrintable(p2.notes.join(S("; "))));
    }

    void importMissingPolicies() {
        // MP-08b: the mp:// line is a live source with no inputMap entry (dropped, MP006); the URL line airs as a live item
        // for its LengthStr.
        const res::Resolver rv(importLibrary(), importResOptions());
        const imp::Importer im(nullptr, &rv, tl::memoryReader({}));
        const Document doc = parse(crlf({S("0000-00-02T10:00:00\tPlayMode=1"), S("D:\\Media\\Missing\\Gone.mpg\tLengthStr=00:00:45"),
                                         S(R"(D:\Media\Missing\NoLen.mpg)"), S("mp://Studio A"), S("http://example.invalid/live.m3u8\tLengthStr=00:01:00"),
                                         S(R"(D:\Media\Spots\Spot A.mpg)")}));
        imp::Options o = mondayOptions();
        const imp::Plan drop = im.plan(doc, S("m.in"), o);
        QCOMPARE(slotSummary(*drop.slot(S("2025-09-08-10"))), S("path:live.m3u8@0:truncate spotA"));
        QCOMPARE(drop.dropped.size(), 3);
        QCOMPARE(drop.dropped[2].toObject().value("reason").toString(), S("liveUnmapped"));
        QCOMPARE(drop.missing.size(), 2);
        QVERIFY(drop.unsupported.isEmpty());
        QVERIFY(drop.ingest.isEmpty());
        QVERIFY2(diagCodes(drop.diagnostics).contains(S("m.in:4:MP006")), qPrintable(diagCodes(drop.diagnostics)));
        const sched::SlotItem& live = drop.slot(S("2025-09-08-10"))->items[0];
        QCOMPARE(live.type, S("live"));
        QCOMPARE(live.path, S("http://example.invalid/live.m3u8"));
        QCOMPARE(live.durationSec, 60.0);
        o.missing = S("keepPath");
        const imp::Plan keep = im.plan(doc, S("m.in"), o);
        const sched::HourSlot* s10 = keep.slot(S("2025-09-08-10"));
        QCOMPARE(slotSummary(*s10), S("path:Gone.mpg@0:truncate path:live.m3u8 spotA"));
        QCOMPARE(s10->items[0].path, S(R"(D:\Media\Missing\Gone.mpg)"));
        QCOMPARE(s10->items[0].durationSec, 45.0);
        QCOMPARE(s10->items[0].type, S("video"));
        QCOMPARE(s10->items[0].title, S("Gone"));
        QVERIFY(s10->items[0].extra.value("masterplay").toObject().value("keepPath").toBool());
        QCOMPARE(keep.dropped.size(), 2);
        QCOMPARE(keep.dropped[0].toObject().value("reason").toString(), S("noDuration"));
        const int i10 = keep.indexOf(S("2025-09-08-10"));
        QCOMPARE(runlistSummary(sched::RunlistBuilder::build(*s10, keep.hourStartMs[i10], im.mediaLookup(), keep.buildOptions(i10))),
                 S("Gone.mpg 10:00:00-10:00:45 H | live.m3u8 10:00:45-10:01:45 | spotA 10:01:45-10:02:15"));
        o.missing = S("ingest");
        const imp::Plan ing = im.plan(doc, S("m.in"), o);
        QCOMPARE(slotSummary(*ing.slot(S("2025-09-08-10"))), S("path:Gone.mpg@0:truncate path:live.m3u8 spotA"));
        QVERIFY(ing.slot(S("2025-09-08-10"))->items[0].extra.value("masterplay").toObject().value("ingest").toBool());
        QCOMPARE(ing.ingest.size(), 2);
        QCOMPARE(ing.ingest[0].toObject().value("mapped").toString(), S(R"(D:\Media\Missing\Gone.mpg)"));
        QCOMPARE(ing.ingest[1].toObject().value("mapped").toString(), S(R"(D:\Media\Missing\NoLen.mpg)"));
        // With an inputMap entry the mp:// source airs too, until the next event when it has no length of its own.
        o.missing = S("drop");
        o.inputMap = QJsonObject{{"Studio A", "ndi://STUDIO (A)"}};
        const imp::Plan mapped = im.plan(doc, S("m.in"), o);
        const sched::HourSlot* m10 = mapped.slot(S("2025-09-08-10"));
        QCOMPARE(slotSummary(*m10), S("path:STUDIO (A)@0:truncate"));
        QCOMPARE(m10->items[0].liveSourceId, S("Studio A"));
        QCOMPARE(m10->items[0].durationSec, 14.0 * 3600);   // 10:00 to midnight: the event runs to the end of the day
        QVERIFY(m10->items[0].extra.value("masterplay").toObject().value("live").toObject().value("untilNextEvent").toBool());
    }

    void importEmptyEventCutsNothing() {
        // An exact event at 10:30 holding only a missing file places nothing, so it writes no hard start: on air the 10:00
        // movie plays on whole, and the plan must lay it that way (not cut at 10:30). A laid item a later start drops
        // (Spot B, cut by 12:00:20) hands its deferred !ShowLogo to the next placed item.
        const res::Resolver rv(importLibrary(), importResOptions());
        const imp::Importer im(nullptr, &rv, tl::memoryReader({}));
        const QString logo = S("!ShowLogo\tf=D:\\Media\\Logos\\bug.png\tLayer=1");
        const Document doc = parse(crlf({S("2025-09-08T10:00:00\tPlayMode=1"), S(R"(D:\Media\Prog\Movie.mpg)"),
                                         S("2025-09-08T10:30:00\tName=Ghost\tPlayMode=1"), logo, S(R"(D:\Media\Missing\Gone.mpg)"),
                                         S("2025-09-08T12:00:00\tPlayMode=1"), S(R"(D:\Media\Spots\Spot A.mpg)"), logo,
                                         S(R"(D:\Media\Spots\Spot B.mpg)"), S("2025-09-08T12:00:20\tPlayMode=1"), S(R"(D:\Media\Prog\Talk.mpg)")}));
        const imp::Plan p = im.plan(doc, S("m.in"), mondayOptions());
        QVERIFY2(p.ok(), qPrintable(p.errors.join(S("; "))));
        const int i10 = p.indexOf(S("2025-09-08-10")), i11 = p.indexOf(S("2025-09-08-11")), i12 = p.indexOf(S("2025-09-08-12"));
        QVERIFY(i10 >= 0 && i11 >= 0 && i12 >= 0);
        QCOMPARE(slotSummary(p.hourSlots[i10]), S("movie@0:truncate"));
        QCOMPARE(slotSummary(p.hourSlots[i11]), S("cont(2025-09-08-10,1800)"));
        QCOMPARE(slotSummary(p.hourSlots[i12]), S("spotA@0:truncate talk@20:truncate"));
        QCOMPARE(runlistSummary(sched::RunlistBuilder::build(p.hourSlots[i10], p.hourStartMs[i10], im.mediaLookup(), p.buildOptions(i10))),
                 S("movie 10:00:00-11:30:00 H"));
        bool sawMovie = false;
        for (const imp::Placement& pl : p.placements)
            if (pl.line == 2 && pl.output) { sawMovie = true; QCOMPARE(pl.state, S("placed")); QCOMPARE(pl.endMs - pl.startMs, qint64(5400000)); }
        QVERIFY(sawMovie);
        // The reports: only the missing file and Spot B are dropped; the empty event is noted, never the movie's "runs past".
        QCOMPARE(p.dropped.size(), 2);
        QCOMPARE(p.stats.value("dropped").toObject(), (QJsonObject{{"cutByEvent", 1}, {"missing", 1}}));
        const QString codes = diagCodes(p.diagnostics);
        QVERIFY2(codes.contains(S("m.in:3:MP016")) && !codes.contains(S("m.in:1:MP016")), qPrintable(codes));
        bool sawNote = false;
        for (const QJsonValue& v : p.diagnostics)
            if (v.toObject().value("line").toInt() == 3 && v.toObject().value("code").toString() == S("MP016")) {
                sawNote = true;
                const QString m = v.toObject().value("message").toString();
                QVERIFY2(m.contains(S("nothing of the event is placed")) && m.contains(S("10:30:00")) && m.contains(S("goes on")), qPrintable(m));
            }
        QVERIFY(sawNote);
        // The empty event is listed (with its unplaced lines) but gives no title.
        const QJsonArray evs = p.hourSlots[i10].showBlock.value("masterplay").toObject().value("events").toArray();
        QCOMPARE(evs.size(), 2);
        QVERIFY(evs[1].toObject().value("empty").toBool());
        QCOMPARE(evs[1].toObject().value("unplaced").toArray().size(), 2);
        QVERIFY(!p.hourSlots[i10].showBlock.contains("title"));
        QVERIFY(!p.hourSlots[i11].showBlock.contains("title"));
        // Spot B's !ShowLogo rides on Talk, marked with the event it came from.
        const QJsonArray ins = p.hourSlots[i12].items[1].extra.value("masterplay").toObject().value("instructions").toArray();
        QCOMPARE(ins.size(), 1);
        QCOMPARE(ins[0].toObject().value("line").toInt(), 8);
        QCOMPARE(ins[0].toObject().value("name").toString(), S("ShowLogo"));
        QCOMPARE(ins[0].toObject().value("carriedFrom").toString(), S("m.in#6"));
        QCOMPARE(ins[0].toObject().value("file").toString(), S("m.in"));
        QCOMPARE(p.stats.value("starts").toObject(), (QJsonObject{{"onTime", 3}}));
    }

    void importOptionsAndErrors() {
        QStringList errs;
        const imp::Options o = imp::Options::fromJson(
            QJsonObject{{"channel", "ch2"}, {"mode", "week"}, {"from", "2025-09-08"}, {"weekdays", QJsonArray{1, 1, 0}}, {"hours", QJsonArray{23, "x", 24}},
                        {"missing", "KEEPPATH"}, {"hourBoundary", "cut"}, {"floatingSkipMaxLateSec", 120}, {"timeZone", "Mars/Base"}, {"nowMs", 5.0},
                        {"subPlaylists", "off"}, {"maxDepth", 2}, {"clearUncovered", true}, {"validateFiles", true}},
            &errs);
        QCOMPARE(o.channelId, S("ch2"));
        QCOMPARE(o.mode, S("grid"));
        QCOMPARE(o.from, QDate(2025, 9, 8));
        QCOMPARE(o.weekdays, (QList<int>{0, 1}));
        QCOMPARE(o.hours, QList<int>{23});
        QCOMPARE(o.missing, S("keepPath"));
        QCOMPARE(o.hourBoundary, S("cut"));
        QCOMPARE(o.floatingSkipMaxLateSec, 120.0);
        QVERIFY(o.timeZone.isEmpty());
        QCOMPARE(o.nowMs, qint64(5));
        QVERIFY(o.clearUncovered && o.validateFiles);
        QVERIFY(!o.timeline.expandSubPlaylists);
        QCOMPARE(o.timeline.maxDepth, 2);
        QCOMPARE(errs.size(), 3);   // "x", 24, Mars/Base
        QVERIFY2(errs.join(' ').contains(S("Mars/Base")), qPrintable(errs.join(S("; "))));
        QStringList none;
        QCOMPARE(imp::Options::fromJson(o.toJson(), &none).toJson(), o.toJson());
        QVERIFY(none.isEmpty());
        errs.clear();
        const imp::Options bad = imp::Options::fromJson(QJsonObject{{"mode", "monthly"}, {"missing", "sometimes"}, {"floatingSkipMaxLateSec", "x"}}, &errs);
        QCOMPARE(bad.mode, S("grid"));
        QCOMPARE(bad.missing, S("drop"));
        QCOMPARE(errs.size(), 3);
        // check(): a date range is needed, ordered and at most 366 days.
        imp::Options d;
        d.mode = S("dates");
        QCOMPARE(d.check().size(), 1);
        d.from = QDate(2025, 9, 8);
        d.to = QDate(2025, 9, 7);
        QVERIFY(d.check().join(' ').contains(S("before")));
        d.to = d.from.addDays(366);
        QVERIFY(d.check().join(' ').contains(S("366")));
        d.to = d.from.addDays(365);
        QVERIFY(d.check().isEmpty());
        // A plan with bad options plans nothing; so does an importer without a resolver.
        const res::Resolver rv(importLibrary(), importResOptions());
        const Document doc = parse(readAll(fx("import_times.in")));
        imp::Options nr = mondayOptions();
        nr.to = QDate();
        const imp::Plan p = imp::Importer(nullptr, &rv).plan(doc, {}, nr);
        QVERIFY(!p.ok());
        QVERIFY(p.hourSlots.isEmpty());
        QCOMPARE(p.toJson().value("errors").toArray().size(), 1);
        QVERIFY(!imp::Importer(nullptr, nullptr).plan(doc, {}, mondayOptions()).ok());
    }

    void importDeterministic() {
        // The same file, options and library give the same plan, whatever order the library comes in; ids are mp-<16 hex>,
        // unique within each slot; hour starts follow the channel's zone.
        const Document doc = parse(readAll(fx("import_times.in")));
        QList<MediaRecord> lib = importLibrary();
        const res::Resolver a(lib, importResOptions());
        std::reverse(lib.begin(), lib.end());
        const res::Resolver b(lib, importResOptions());
        imp::Options o = mondayOptions();
        o.mode = S("grid");
        o.from = o.to = QDate();
        const imp::Plan pa = imp::Importer(nullptr, &a, tl::memoryReader({})).plan(doc, S("import_times.in"), o);
        const imp::Plan pb = imp::Importer(nullptr, &b, tl::memoryReader({})).plan(doc, S("import_times.in"), o);
        QCOMPARE(pb.toJson(true, true), pa.toJson(true, true));
        const QRegularExpression idRx(S("^mp-[0-9a-f]{16}(~j)?$"));
        int items = 0;
        for (const sched::HourSlot& s : pa.hourSlots) {
            QSet<QString> ids;
            for (const sched::SlotItem& it : s.items) {
                QVERIFY2(idRx.match(it.id).hasMatch(), qPrintable(it.id));
                QVERIFY2(!ids.contains(it.id), qPrintable(s.id + S(": ") + it.id));
                ids.insert(it.id);
                ++items;
            }
        }
        QCOMPARE(items, pa.stats.value("items").toInt());
        QCOMPARE(imp::Importer::itemId(S("f.in"), S("f.in"), 1, S("1"), 0, 0, 2), imp::Importer::itemId(S("f.in"), S("f.in"), 1, S("1"), 0, 0, 2));
        QVERIFY(imp::Importer::itemId(S("f.in"), S("f.in"), 1, S("1"), 0, 0, 2) != imp::Importer::itemId(S("f.in"), S("f.in"), 1, S("1"), 0, 0, 2, 1));
        QVERIFY(imp::Importer::itemId(S("f.in"), S("f.in"), 1, S("1"), 0, 0, 2) != imp::Importer::itemId(S("f.in"), S("f.in"), 1, S("2"), 0, 0, 2));
        // The placements of the output, in JSON.
        const QJsonArray pls = pa.toJson(false, true).value("placements").toArray();
        QVERIFY(!pls.isEmpty());
        QVERIFY(!pa.toJson(false).contains("slots"));
        // America/New_York: Monday 2025-09-08 00:00 local is 04:00 UTC (EDT).
        imp::Options ny = mondayOptions();
        ny.timeZone = S("America/New_York");
        const imp::Plan pn = imp::Importer(nullptr, &a, tl::memoryReader({})).plan(doc, S("import_times.in"), ny);
        QCOMPARE(pn.hourStartMs[0], QDateTime(QDate(2025, 9, 8), QTime(4, 0), QTimeZone::UTC).toMSecsSinceEpoch());
    }

    // ---- MP-08b: repeat, folders, OSD, acts, breaks, live sources, unsupported instructions ----------------------------

    void importRepeat() {
        // Monday of import_features.in: Talk once, then a 390 s loop (Spot A, Weather, Spot B) until the exact 08:20 event.
        // Hour 6 holds the pre-item and unrolled copies, hour 7 lies wholly inside the loop (isLooping, its one pass after the
        // carried Weather), hour 8 holds copies up to the anchor.
        const Document doc = parse(readAll(fx("import_features.in")));
        QVERIFY2(diagList(doc.diagnostics) == S("69:MP011:2"), qPrintable(diagList(doc.diagnostics)));   // !FooBar, the unknown name
        const res::Resolver rv(featureLibrary(), importResOptions());
        const imp::Importer im(nullptr, &rv, featureReader());
        const imp::Plan p = im.plan(doc, S("import_features.in"), featureOptions(QDate(2025, 9, 8)));
        QVERIFY2(p.ok(), qPrintable(p.errors.join(S("; "))));
        const int i6 = p.indexOf(S("2025-09-08-6")), i7 = i6 + 1, i8 = i6 + 2;
        QString copies;
        for (int c = 1; c <= 6; ++c) copies += QStringLiteral(" spotA#%1 weather#%1 spotB#%1").arg(c);
        QCOMPARE(featureSummary(p.hourSlots[i6]), S("talk@0:truncate spotA weather spotB") + copies + S(" spotA#7 weather#7"));
        QCOMPARE(featureSummary(p.hourSlots[i7]), S("cont(2025-09-08-6,60) LOOP spotA#8 weather#8 spotB#8"));
        QCOMPARE(featureSummary(p.hourSlots[i8]), S("spotA#9 weather#9 spotB#9 spotA#10 weather#10 spotB#10 spotA#11 weather#11 spotB#11 spotA#12 "
                                                    "news@1200:truncate"));
        QVERIFY(!p.hourSlots[i6].isLooping && p.hourSlots[i7].isLooping && !p.hourSlots[i8].isLooping);
        // Copies have their own ids (the copy number is part of the id) and say which pass they are.
        QSet<QString> ids;
        for (const sched::SlotItem& it : p.hourSlots[i6].items) { QVERIFY2(!ids.contains(it.id), qPrintable(it.id)); ids.insert(it.id); }
        QCOMPARE(p.hourSlots[i7].items[0].extra.value("masterplay").toObject().value("copy").toInt(), 8);
        QCOMPARE(p.hours[i7].toObject().value("loop").toBool(), true);
        QCOMPARE(p.hours[i7].toObject().value("plannedSec").toDouble(), 3600.0);   // the carried 60 s, one pass, the replays
        QCOMPARE(p.stats.value("loops").toObject(), (QJsonObject{{"events", 1}, {"unrolledLines", 33}}));
        QCOMPARE(p.stats.value("slots").toObject().value("looping").toInt(), 1);
        QVERIFY(p.hourSlots[i6].showBlock.value("masterplay").toObject().value("events").toArray().last().toObject().value("loops").toBool());
        // RunlistBuilder lays the unrolled loop to the right length: back to back from 06:00 to the 08:20 hard start, the hour
        // loop replaying its pass to 08:00 exactly.
        const QList<sched::Runlist> rls = layChain(p, im.mediaLookup());
        QCOMPARE(rls[i6].items.last().item.mediaId, S("weather"));
        QCOMPARE(clockOf(rls[i6].items.last().plannedEndMs), S("07:01:00"));   // whole: the next hour continues it
        QVERIFY(!rls[i6].items.last().truncated);
        QCOMPARE(rls[i7].items.size(), 28);                                     // 9 passes and one Spot A
        QCOMPARE(clockOf(rls[i7].items.first().plannedStartMs), S("07:01:00"));
        QCOMPARE(clockOf(rls[i7].items.last().plannedEndMs), S("08:00:00"));
        QVERIFY2(runlistSummary(rls[i8]).endsWith(S("spotA 08:19:30-08:20:00 | news 08:20:00-09:05:00 H")), qPrintable(runlistSummary(rls[i8])));
        qint64 at = p.hourStartMs[i6], total = 0;
        for (int i = i6; i <= i8; ++i)
            for (const sched::RunlistItem& r : rls[i].items) {
                QCOMPARE(r.plannedStartMs, at);   // no gap, no overlap
                at = r.plannedEndMs;
                total += r.plannedEndMs - r.plannedStartMs;
                if (r.item.mediaId == S("news")) break;
            }
        QCOMPARE(total, qint64(8400000 + 2700000));   // 06:00-08:20 and the news
        checkGolden(S("import_features_repeat.json"), p.toJson());
        if (QTest::currentTestFailed()) return;
        QCOMPARE(im.plan(doc, S("import_features.in"), featureOptions(QDate(2025, 9, 8))).toJson(), p.toJson());

        // repeat "unroll": copies everywhere, the phase runs on across the hours.
        imp::Options un = featureOptions(QDate(2025, 9, 8));
        un.repeat = S("unroll");
        const imp::Plan u = im.plan(doc, S("import_features.in"), un);
        for (const sched::HourSlot& s : u.hourSlots) QVERIFY2(!s.isLooping, qPrintable(s.id));
        QCOMPARE(featureSummary(u.hourSlots[i6]), featureSummary(p.hourSlots[i6]));
        QVERIFY2(featureSummary(u.hourSlots[i7]).startsWith(S("cont(2025-09-08-6,60) spotB#7 spotA#8 weather#8 spotB#8 spotA#9")),
                 qPrintable(featureSummary(u.hourSlots[i7])));
        QVERIFY2(featureSummary(u.hourSlots[i8]).endsWith(S("news@1200:truncate")), qPrintable(featureSummary(u.hourSlots[i8])));
        const QList<sched::Runlist> url = layChain(u, im.mediaLookup());
        at = u.hourStartMs[i6];
        for (int i = i6; i <= i8; ++i)
            for (const sched::RunlistItem& r : url[i].items) {
                QCOMPARE(r.plannedStartMs, at);
                at = r.plannedEndMs;
            }
        QCOMPARE(clockOf(url[i8].items.last().plannedStartMs), S("08:20:00"));
        QVERIFY(url[i8].items.last().hard);
        QCOMPARE(u.stats.value("loops").toObject().value("unrolledLines").toInt(), 57);   // 06:16:30 to 08:20 is 19 passes of 3 lines

        // A carry: Monday 22:00's loop seen from a Tuesday-only import restarts at midnight; 00, 01 and 02 loop.
        const Document night = parse(crlf({S("0000-00-02T22:00:00\tPlayMode=2"), S("!Repeat"), S(R"(D:\Media\Spots\Spot A.mpg)"),
                                           S(R"(D:\Media\Prog\Weather.mpg)"), S("0000-00-03T03:00:00\tPlayMode=1"), S(R"(D:\Media\Prog\Talk.mpg)")}));
        const imp::Plan c = im.plan(night, S("n.in"), featureOptions(QDate(2025, 9, 9)));
        QCOMPARE(featureSummary(c.hourSlots[0]), S("LOOP spotA#1 weather#1"));
        QCOMPARE(featureSummary(c.hourSlots[1]), S("LOOP spotA#2 weather#2"));
        QCOMPARE(featureSummary(c.hourSlots[2]), S("LOOP spotA#3 weather#3"));
        QCOMPARE(featureSummary(c.hourSlots[3]), S("talk@0:truncate"));
        const sched::Runlist c0 = sched::RunlistBuilder::build(c.hourSlots[0], c.hourStartMs[0], im.mediaLookup(), c.buildOptions(0));
        QCOMPARE(clockOf(c0.items.first().plannedStartMs), S("00:00:00"));
        QCOMPARE(clockOf(c0.items.last().plannedEndMs), S("01:00:00"));
        QCOMPARE(c0.items.size(), 22);   // 3600 / 330: 10 passes, Spot A and a cut Weather

        // The guard, and a loop with nothing that airs.
        const Document spots = parse(crlf({S("2025-09-08T10:00:00\tPlayMode=2"), S("!Repeat"), S(R"(D:\Media\Spots\Spot A.mpg)")}));
        imp::Options g = featureOptions(QDate(2025, 9, 8));
        g.repeat = S("unroll");
        g.maxUnrollItems = 100;
        const imp::Plan gp = im.plan(spots, S("s.in"), g);
        QCOMPARE(gp.notes.size(), 1);
        QVERIFY2(gp.notes[0].contains(S("stops after 100 unrolled lines")) && gp.notes[0].contains(S("maxUnrollItems")), qPrintable(gp.notes[0]));
        const Document idle = parse(crlf({S("2025-09-08T10:00:00\tPlayMode=2"), S(R"(D:\Media\Prog\Talk.mpg)"), S("!Repeat"),
                                          S("!ShowLogo\tf=D:\\Media\\Logos\\bug.png")}));
        const imp::Plan ip = im.plan(idle, S("i.in"), featureOptions(QDate(2025, 9, 8)));
        QCOMPARE(featureSummary(*ip.slot(S("2025-09-08-10"))), S("talk@0:truncate"));
        QVERIFY2(diagCodes(ip.diagnostics).contains(S("i.in:1:MP016")), qPrintable(diagCodes(ip.diagnostics)));
        // Saturday_0200.in: PlayMode=2 without a !Repeat loops its whole content.
        const imp::Plan sat = imp::Importer(nullptr, &rv).plan(parse(readAll(fx("Saturday_0200.in"))), S("Saturday_0200.in"), featureOptions(QDate(2025, 9, 8)));
        QCOMPARE(sat.stats.value("loops").toObject().value("events").toInt(), 1);
    }

    void importFolders() {
        // Tuesday: every LoadingMode (the plant's (3, 1) and (2, 1) and none first), SelectRandom=N, SubDirs, MaxAge, a mask,
        // a search term, an empty folder, LoadingMode=8, and LoadingMode=7 until the 07:00 event.
        const Document doc = parse(readAll(fx("import_features.in")));
        const res::Resolver rv(featureLibrary(), importResOptions());
        const imp::Importer im(nullptr, &rv, featureReader());
        const imp::Plan p = im.plan(doc, S("import_features.in"), featureOptions(QDate(2025, 9, 9)));
        const sched::HourSlot* s6 = p.slot(S("2025-09-09-6"));
        QVERIFY(s6);
        QCOMPARE(featureSummary(*s6), S("folder(random,0,30)@0:truncate folder(name,0,30) folder(name,90,90) folder(random,90,90) folder(name,0,30) "
                                        "folder(name,0,30) folder(random,0,30) folder(random,0,30) folder(random,0,37.5) folder(name,0,30) "
                                        "folder(name,0,30) folder(name,90,90) folder(random,0,30) folder(random,0,37.5) folder(random,2985,2985)"));
        QCOMPARE(featureSummary(*p.slot(S("2025-09-09-7"))), S("talk@0:truncate"));
        auto fb = [&](int i) { return s6->items[i].folderBlock; };
        auto fbObj = [](const char* order, double target, const char* weight, bool recursive) {
            return QJsonObject{{"folder", S(R"(D:\Media\Promos)")}, {"targetSec", target}, {"weight", weight}, {"order", order}, {"recursive", recursive}};
        };
        QCOMPARE(fb(0), fbObj("random", 0, "freshness", false));            // LoadingMode=3 ToSelect=1
        QCOMPARE(fb(1), fbObj("name", 0, "freshness", false));              // LoadingMode=2 ToSelect=1
        QCOMPARE(fb(2), fbObj("name", 90, "freshness", false));             // none: Normal, the whole folder by name
        QCOMPARE(fb(3), fbObj("random", 90, "uniform", false));             // LoadingMode=1
        QCOMPARE(fb(4), fb(5));                                             // LoadingMode=2 ToSelect=2: two picks
        QCOMPARE(fb(6), fb(0));                                             // SelectRandom=2 (older files): two random picks
        QJsonObject aged = fbObj("random", 0, "freshness", true);
        aged["maxAgeDays"] = 2.0;
        QCOMPARE(fb(8), aged);                                              // SubDirs=1 MaxAge=48
        QJsonObject masked = fbObj("random", 0, "freshness", false);
        masked["rules"] = QJsonArray{QJsonObject{{"field", "filename"}, {"op", "starts_with"}, {"value", "Promo"}},
                                     QJsonObject{{"field", "filename"}, {"op", "ends_with"}, {"value", ".mpg"}}};
        QCOMPARE(fb(12), masked);                                           // Promo*.mpg
        QCOMPARE(fb(14).value("targetSec").toDouble(), 2985.0);             // LoadingMode=7: until 07:00
        QCOMPARE(s6->items[0].title, QString::fromUtf8("\xF0\x9F\x93\x81 Promos"));
        QCOMPARE(s6->items[4].title, QString::fromUtf8("\xF0\x9F\x93\x81 Promos \xC3\x97" "2"));
        QCOMPARE(s6->items[5].id, s6->items[4].id + S("~p1"));
        QCOMPARE(s6->items[5].extra.value("masterplay").toObject().value("folder").toObject().value("pick").toInt(), 1);
        QCOMPARE(s6->items[0].extra.value("masterplay").toObject().value("folder").toObject().value("loadingMode").toString(), S("selectRandom"));
        // Reports: three lines do not air, the folder report has the coverage and the scan / watch actions of the empty one.
        QCOMPARE(p.dropped.size(), 3);
        QCOMPARE(p.stats.value("dropped").toObject(), (QJsonObject{{"emptyFolder", 1}, {"searchTerm", 1}, {"unsupported", 1}}));
        QCOMPARE(p.stats.value("folderPicks").toInt(), 15);
        QCOMPARE(p.folders.size(), 5);   // by (folder, mask, recursive, max age): plain, SubDirs=1 MaxAge=48, the mask, SubDirs=2, the empty one
        bool sawEmpty = false;
        for (const QJsonValue& v : p.folders) {
            const QJsonObject f = v.toObject();
            QVERIFY2(f.value("mapped").toString() != S(R"(D:\Media\Daily)"), "a search term is MP020, not a folder row");
            if (f.value("mapped").toString() == S(R"(D:\Media\Nothing)")) {
                sawEmpty = true;
                QCOMPARE(f.value("playable").toInt(), 0);
                QCOMPARE(f.value("actions").toArray().size(), 2);
            }
        }
        QVERIFY(sawEmpty);
        QCOMPARE(diagCodes(p.diagnostics), S("import_features.in:69:MP011 import_features.in:24:MP019 import_features.in:17:MP021 import_features.in:18:MP021 "
                                             "import_features.in:19:MP021 import_features.in:20:MP022 import_features.in:22:MP021 import_features.in:23:MP020"));
        // RunlistBuilder expands every block into library picks from 06:00 to at most 07:00.
        const sched::Runlist rl = sched::RunlistBuilder::build(*s6, p.hourStartMs[p.indexOf(S("2025-09-09-6"))], im.mediaLookup(),
                                                               p.buildOptions(p.indexOf(S("2025-09-09-6"))));
        QVERIFY(rl.items.size() > 15);
        QVERIFY(rl.items.first().hard);
        for (const sched::RunlistItem& r : rl.items) {
            QCOMPARE(r.origin, S("query"));
            QVERIFY2(r.item.path.startsWith(S(R"(D:\Media\Promos\)")), qPrintable(r.item.path));
        }
        QVERIFY(rl.items.last().plannedEndMs <= p.hourStartMs[p.indexOf(S("2025-09-09-6"))] + 3600000);
        checkGolden(S("import_features_folders.json"), p.toJson());
        if (QTest::currentTestFailed()) return;
        // selectRandomOrder even: LoadingMode 3 picks rotate strictly.
        imp::Options ev = featureOptions(QDate(2025, 9, 9));
        ev.selectRandomOrder = S("even");
        const imp::Plan e = im.plan(doc, S("import_features.in"), ev);
        QCOMPARE(e.slot(S("2025-09-09-6"))->items[0].folderBlock.value("order").toString(), S("even"));
        QCOMPARE(e.slot(S("2025-09-09-6"))->items[1].folderBlock.value("order").toString(), S("name"));
    }

    void importOsd() {
        // Wednesday: a logo over Talk, Weather and Spot A (a crawl joins it on Spot A), !OSD off, a single-item logo on
        // layer 2, a clock over two items, a crawl read from a file, a text file that cannot be read.
        const Document doc = parse(readAll(fx("import_features.in")));
        const res::Resolver rv(featureLibrary(), importResOptions());
        const imp::Importer im(nullptr, &rv, featureReader());
        const imp::Plan p = im.plan(doc, S("import_features.in"), featureOptions(QDate(2025, 9, 10)));
        const sched::HourSlot* s6 = p.slot(S("2025-09-10-6"));
        QCOMPARE(featureSummary(*s6), S("talk@0:truncate weather spotA spotB doc weather spotA spotB"));
        auto cueIds = [&](int i) {
            QStringList o;
            for (const QJsonValue& v : s6->items[i].cgOverlays) o << v.toObject().value("id").toString().section(QLatin1Char('-'), 0, 1);
            return o.join(QLatin1Char(','));
        };
        const QStringList expect{S("mp-logo"), S("mp-logo"), S("mp-logo,mp-crawl"), S(""), S("mp-logo"), S(""), S("mp-crawl"), S("")};
        for (int i = 0; i < expect.size(); ++i) QCOMPARE(cueIds(i), expect[i]);
        const QJsonObject logo = s6->items[0].cgOverlays[0].toObject();
        QCOMPARE(logo.value("filePath").toString(), S(R"(D:\Media\Logos\bug.png)"));
        QCOMPARE(logo.value("role").toString(), S("bug"));
        QCOMPARE(logo.value("params").toObject().value("placement").toObject(), (QJsonObject{{"anchor", "top_right"}, {"marginX", 60}, {"marginY", 60}}));
        QCOMPARE(s6->items[1].cgOverlays, s6->items[0].cgOverlays);                         // one span: the same cue on every item
        QCOMPARE(s6->items[4].cgOverlays[0].toObject().value("role").toString(), S("secondary"));   // Layer=2
        QCOMPARE(s6->items[2].cgOverlays[1].toObject().value("template").toString(), S("ticker"));
        QCOMPARE(s6->items[2].cgOverlays[1].toObject().value("params").toObject().value("text").toString(), S("Breaking: Main St closed Use Route 58"));
        QCOMPARE(s6->items[6].cgOverlays[0].toObject().value("params").toObject().value("text").toString(), S("Storm warning until 9 pm"));
        // The clock is the identity hook: shown with Weather, hidden at the end of Spot A.
        QCOMPARE(s6->items[5].cgOnStart, QStringList{S("clock")});
        QCOMPARE(s6->items[6].cgOnEnd, QStringList{S("clock")});
        QVERIFY(s6->items[6].cgOnStart.isEmpty() && s6->items[7].cgOnStart.isEmpty() && s6->items[7].cgOverlays.isEmpty());
        QCOMPARE(p.overlays.size(), 5);
        QStringList spans;
        for (const QJsonValue& v : p.overlays) {
            const QJsonObject r = v.toObject();
            spans << QStringLiteral("%1/%2/%3/%4/%5").arg(r.value("kind").toString(), r.value("role").toString(), r.value("mode").toString())
                         .arg(r.value("items").toInt()).arg(r.value("end").toString());
        }
        QCOMPARE(spans, (QStringList{S("logo/bug/cue/3/osdOff"), S("crawl/ticker/cue/1/osdOff"), S("logo/secondary/cue/1/osdOff"),
                                     S("clock/clock/identity/2/osdOff"), S("crawl/ticker/cue/1/osdOff")}));
        QCOMPARE(p.unsupported.size(), 1);   // the unreadable text file
        QCOMPARE(p.unsupported[0].toObject().value("name").toString(), S("ShowTextFromFile"));
        QCOMPARE(p.unsupported[0].toObject().value("code").toString(), S("MP006"));
        QCOMPARE(diagCodes(p.diagnostics), S("import_features.in:69:MP011 import_features.in:39:MP006 import_features.in:41:MP028 import_features.in:43:MP006"));
        checkGolden(S("import_features_osd.json"), p.toJson());
        if (QTest::currentTestFailed()) return;

        // osd hooks: the design's show / hide commands as cgOnStart / cgOnEnd strings (noted: the engine does not run them).
        imp::Options h = featureOptions(QDate(2025, 9, 10));
        h.osd = S("hooks");
        const imp::Plan hp = im.plan(doc, S("import_features.in"), h);
        const sched::HourSlot* h6 = hp.slot(S("2025-09-10-6"));
        for (const sched::SlotItem& it : h6->items) QVERIFY(it.cgOverlays.isEmpty());
        QCOMPARE(h6->items[0].cgOnStart.size(), 1);
        const QJsonObject show = QJsonDocument::fromJson(h6->items[0].cgOnStart[0].toUtf8()).object();
        QCOMPARE(show.value("action").toString(), S("show"));
        QCOMPARE(show.value("role").toString(), S("bug"));
        QCOMPARE(show.value("source").toObject(), (QJsonObject{{"type", "image"}, {"path", S(R"(D:\Media\Logos\bug.png)")}}));
        QVERIFY(h6->items[1].cgOnStart.isEmpty());   // the same hour: the layer is already up
        QCOMPARE(h6->items[2].cgOnEnd.size(), 2);    // the logo and the crawl end with Spot A
        QCOMPARE(QJsonDocument::fromJson(h6->items[2].cgOnEnd[0].toUtf8()).object(), (QJsonObject{{"action", "hide"}, {"role", "bug"}}));
        QCOMPARE(h6->items[5].cgOnStart, QStringList{S("clock")});
        QVERIFY(hp.notes.join(' ').contains(S("osd hooks")));
        // A span crossing the hour re-opens the layer on its first item of the next hour.
        const Document two = parse(crlf({S("2025-09-08T10:00:00\tPlayMode=1"), S("!ShowLogo\tf=D:\\Media\\Logos\\bug.png"), S(R"(D:\Media\Prog\Movie.mpg)"),
                                         S(R"(D:\Media\Prog\Talk.mpg)"), S("!OSD off"), S(R"(D:\Media\Spots\Spot A.mpg)")}));
        h.from = h.to = QDate(2025, 9, 8);
        const imp::Plan tp = im.plan(two, S("t.in"), h);
        QCOMPARE(tp.slot(S("2025-09-08-10"))->items[0].cgOnStart.size(), 1);
        QCOMPARE(tp.slot(S("2025-09-08-11"))->items[0].cgOnStart, tp.slot(S("2025-09-08-10"))->items[0].cgOnStart);
        QCOMPARE(tp.slot(S("2025-09-08-11"))->items[0].cgOnEnd.size(), 1);
        QVERIFY(tp.slot(S("2025-09-08-11"))->items[1].cgOnStart.isEmpty());
        // osd off: the lines stay provenance and are not listed as unsupported.
        imp::Options off = featureOptions(QDate(2025, 9, 10));
        off.osd = S("off");
        const imp::Plan op = im.plan(doc, S("import_features.in"), off);
        for (const sched::SlotItem& it : op.slot(S("2025-09-10-6"))->items) QVERIFY(it.cgOverlays.isEmpty() && it.cgOnStart.isEmpty() && it.cgOnEnd.isEmpty());
        QVERIFY(op.overlays.isEmpty() && op.unsupported.isEmpty());
        QCOMPARE(op.slot(S("2025-09-10-6"))->items[0].extra.value("masterplay").toObject().value("instructions").toArray()[0].toObject().value("reason").toString(),
                 S("osdOff"));
    }

    void importActs() {
        // Thursday: Show Open in three acts without library segments (one whole item, MP026) and Seg Show with segments.
        const Document doc = parse(readAll(fx("import_features.in")));
        const res::Resolver rv(featureLibrary(), importResOptions());
        const imp::Importer im(nullptr, &rv, featureReader());
        const imp::Plan p = im.plan(doc, S("import_features.in"), featureOptions(QDate(2025, 9, 11)));
        const int i6 = p.indexOf(S("2025-09-11-6"));
        QCOMPARE(featureSummary(p.hourSlots[i6]), S("show@0:truncate spotA spotB segshow/seg-a spotA segshow/seg-b"));
        QCOMPARE(p.hourSlots[i6].items[3].segmentLabel, S("Act 1"));
        QCOMPARE(p.hourSlots[i6].items[0].extra.value("masterplay").toObject().value("act").toObject(), (QJsonObject{{"index", 0}, {"collapsed", true}}));
        QCOMPARE(p.hourSlots[i6].items[5].extra.value("masterplay").toObject().value("act").toObject(), (QJsonObject{{"index", 1}, {"segmentId", "seg-b"}}));
        QCOMPARE(runlistSummary(sched::RunlistBuilder::build(p.hourSlots[i6], p.hourStartMs[i6], im.mediaLookup(), p.buildOptions(i6))),
                 S("show 06:00:00-06:25:00 H | spotA 06:25:00-06:25:30 | spotB 06:25:30-06:26:30 | segshow 06:26:30-06:36:30 | spotA 06:36:30-06:37:00 | "
                   "segshow 06:37:00-06:47:00"));
        QCOMPARE(p.stats.value("dropped").toObject(), (QJsonObject{{"actCollapsed", 2}}));
        QCOMPARE(p.acts.size(), 2);
        QCOMPARE(p.acts[1].toObject().value("mode").toString(), S("collapsed"));
        QCOMPARE(p.acts[1].toObject().value("lines").toArray().size(), 3);
        QCOMPARE(p.acts[0].toObject().value("mode").toString(), S("segments"));
        QCOMPARE(diagCodes(p.diagnostics), S("import_features.in:69:MP011 import_features.in:49:MP026 import_features.in:51:MP026 import_features.in:53:MP026"));
        checkGolden(S("import_features_acts.json"), p.toJson());
        if (QTest::currentTestFailed()) return;
        // Without the collapse every part airs the whole programme: MP027 errors, segmentLabel "part N+1".
        imp::Options o = featureOptions(QDate(2025, 9, 11));
        o.collapseUnsegmentedActs = false;
        const imp::Plan w = im.plan(doc, S("import_features.in"), o);
        const sched::HourSlot* w6 = w.slot(S("2025-09-11-6"));
        QCOMPARE(featureSummary(*w6), S("show@0:truncate spotA show spotB show"));   // the third part runs into 07:00: the rest is cut
        QCOMPARE(w6->items[2].segmentLabel, S("part 2"));
        QCOMPARE(w.stats.value("diagnostics").toObject().value("error").toInt(), 3);
        QVERIFY2(diagCodes(w.diagnostics).contains(S("import_features.in:53:MP027")), qPrintable(diagCodes(w.diagnostics)));
    }

    void importBreaks() {
        // Friday: a break-mode event with pod 1 at schedtime=06:10 and pod 2 after it.
        const Document doc = parse(readAll(fx("import_features.in")));
        const res::Resolver rv(featureLibrary(), importResOptions());
        const imp::Importer im(nullptr, &rv, featureReader());
        const imp::Plan p = im.plan(doc, S("import_features.in"), featureOptions(QDate(2025, 9, 12)));
        const int i6 = p.indexOf(S("2025-09-12-6"));
        QCOMPARE(featureSummary(p.hourSlots[i6]), S("spotA@600:truncate[b1] spotB[b1] spotA[b2]"));
        QCOMPARE(p.hourSlots[i6].items[0].extra.value("masterplay").toObject().value("anchor").toObject(),
                 (QJsonObject{{"time", "06:10:00"}, {"eventType", 0}, {"start", "onTime"}, {"pod", 1}}));
        QCOMPARE(runlistSummary(sched::RunlistBuilder::build(p.hourSlots[i6], p.hourStartMs[i6], im.mediaLookup(), p.buildOptions(i6))),
                 S("spotA 06:10:00-06:10:30 H | spotB 06:10:30-06:11:30 | spotA 06:11:30-06:12:00"));
        QCOMPARE(p.pods.size(), 2);
        QCOMPARE(p.pods[0].toObject(), (QJsonObject{{"line", 60}, {"pod", 1}, {"mode", "asScheduled"}, {"items", 2}, {"day", "2025-09-12"},
                                                    {"file", "import_features.in"}, {"schedTime", "06:10:00"}, {"at", "06:10:00"}, {"slotId", "2025-09-12-6"}}));
        QCOMPARE(p.pods[1].toObject().value("items").toInt(), 1);
        QCOMPARE(diagCodes(p.diagnostics), S("import_features.in:69:MP011 import_features.in:59:MP025"));
        checkGolden(S("import_features_breaks.json"), p.toJson());
        if (QTest::currentTestFailed()) return;
        // breaks live: the event airs the network input for its window; the pods are listed, not placed.
        imp::Options l = featureOptions(QDate(2025, 9, 12));
        l.breaks = S("live");
        QVERIFY(l.check().join(' ').contains(S("networkInput")));
        l.networkInput = S("Network");
        l.inputMap = QJsonObject{{"network", "ndi://NETWORK FEED"}};
        const imp::Plan lp = im.plan(doc, S("import_features.in"), l);
        QVERIFY2(lp.ok(), qPrintable(lp.errors.join(S("; "))));
        const sched::HourSlot* l6 = lp.slot(S("2025-09-12-6"));
        QCOMPARE(featureSummary(*l6), S("path:NETWORK FEED@0:truncate"));
        QCOMPARE(l6->items[0].type, S("live"));
        QCOMPARE(l6->items[0].durationSec, 3600.0);
        QCOMPARE(l6->items[0].extra.value("masterplay").toObject().value("instructions").toArray().size(), 5);   // every line, as provenance
        QCOMPARE(lp.pods.size(), 2);
        QCOMPARE(lp.pods[0].toObject().value("mode").toString(), S("live"));
        QCOMPARE(lp.pods[0].toObject().value("items").toInt(), 0);
        QCOMPARE(runlistSummary(sched::RunlistBuilder::build(*l6, lp.hourStartMs[lp.indexOf(S("2025-09-12-6"))], im.mediaLookup(),
                                                             lp.buildOptions(lp.indexOf(S("2025-09-12-6"))))),
                 S("NETWORK FEED 06:00:00-07:00:00 H"));
        // breaks skip: the pods do not air; the event places nothing.
        imp::Options sk = featureOptions(QDate(2025, 9, 12));
        sk.breaks = S("skip");
        const imp::Plan sp = im.plan(doc, S("import_features.in"), sk);
        QVERIFY(sp.slot(S("2025-09-12-6"))->items.isEmpty());
        QCOMPARE(sp.stats.value("dropped").toObject(), (QJsonObject{{"breakSkipped", 3}}));
        QVERIFY2(diagCodes(sp.diagnostics).contains(S("import_features.in:59:MP016")), qPrintable(diagCodes(sp.diagnostics)));
        // A schedtime before the event's start is ignored (noted): the pod follows the content.
        const Document early = parse(crlf({S("2025-09-08T10:00:00\tPlayMode=4"), S(R"(D:\Media\Prog\Talk.mpg)"), S("!Break\t#=7\tschedtime=09:00:00"),
                                           S(R"(D:\Media\Spots\Spot A.mpg)")}));
        const imp::Plan ep = im.plan(early, S("e.in"), featureOptions(QDate(2025, 9, 8)));
        QCOMPARE(featureSummary(*ep.slot(S("2025-09-08-10"))), S("talk@0:truncate spotA[b7]"));
        QVERIFY2(diagCodes(ep.diagnostics).contains(S("e.in:3:MP025")), qPrintable(diagCodes(ep.diagnostics)));
    }

    void importLiveSources() {
        // !Video <source> through inputMap, !LiveStream Path=, a bare !Video taking networkInput "in2" from the store's
        // ui.inputMap slots, and the lengths: the line's own, else until the next event.
        QVERIFY(m_store->setSettingJson(S("ui.inputMap"), QJsonObject{{"slots", QJsonArray{QJsonObject{}, QJsonObject{{"enginePath", "ndi://SWITCHER (IN2)"}}}}}));
        const res::Resolver rv(featureLibrary(), importResOptions());
        const imp::Importer im(m_store.get(), &rv, tl::memoryReader({}));
        const Document doc = parse(crlf({S("2025-09-08T10:00:00\tPlayMode=1"), S("!Video Studio B\tForceLength=1"), S("2025-09-08T10:30:00\tPlayMode=1"),
                                         S("!LiveStream\tPath=srt://192.0.2.10:9000\tParam=latency=200"), S("2025-09-08T11:00:00\tPlayMode=1"), S("!Video "),
                                         S("mp://Weather Cam\tLengthStr=00:02:00"), S("2025-09-08T12:00:00\tPlayMode=1"), S(R"(D:\Media\Prog\Talk.mpg)")}));
        imp::Options o = featureOptions(QDate(2025, 9, 8));
        o.inputMap = QJsonObject{{"studio b", "ndi://STUDIO B"}, {"Weather Cam", QJsonObject{{"url", "rtsp://192.0.2.20/cam"}}}};
        o.networkInput = S("in2");
        const imp::Plan p = im.plan(doc, S("l.in"), o);
        QCOMPARE(featureSummary(*p.slot(S("2025-09-08-10"))), S("path:STUDIO B@0:truncate path:192.0.2.10:9000@1800:truncate"));
        QCOMPARE(featureSummary(*p.slot(S("2025-09-08-11"))), S("path:SWITCHER (IN2)@0:truncate"));
        const sched::HourSlot* s10 = p.slot(S("2025-09-08-10"));
        QCOMPARE(s10->items[0].durationSec, 1800.0);
        QCOMPARE(s10->items[0].liveSourceId, S("Studio B"));
        QCOMPARE(s10->items[1].liveSource, (QJsonObject{{"url", "srt://192.0.2.10:9000"}}));
        // The bare !Video runs until noon, so the Weather Cam line after it never airs (cut by the 12:00 event).
        QCOMPARE(p.slot(S("2025-09-08-11"))->items[0].durationSec, 3600.0);
        QCOMPARE(p.dropped.size(), 1);
        QCOMPARE(p.dropped[0].toObject().value("reason").toString(), S("cutByEvent"));
        QCOMPARE(p.stats.value("liveItems").toInt(), 3);
        QCOMPARE(runlistSummary(sched::RunlistBuilder::build(*s10, p.hourStartMs[p.indexOf(S("2025-09-08-10"))], im.mediaLookup(),
                                                             p.buildOptions(p.indexOf(S("2025-09-08-10"))))),
                 S("STUDIO B 10:00:00-10:30:00 H | 192.0.2.10:9000 10:30:00-11:00:00 H"));
        for (const QJsonValue& v : p.validation) QCOMPARE(v.toObject().value("errors").toInt(), 0);
        // A liveSource object resolves at build time (RunlistBuilder reads its url).
        const Document cam = parse(crlf({S("2025-09-08T10:00:00\tPlayMode=1"), S("mp://Weather Cam\tLengthStr=00:02:00"), S(R"(D:\Media\Spots\Spot A.mpg)")}));
        const imp::Plan cp = im.plan(cam, S("c.in"), o);
        const sched::HourSlot* c10 = cp.slot(S("2025-09-08-10"));
        QCOMPARE(c10->items[0].liveSource, (QJsonObject{{"url", "rtsp://192.0.2.20/cam"}}));
        QCOMPARE(runlistSummary(sched::RunlistBuilder::build(*c10, cp.hourStartMs[0], im.mediaLookup(), cp.buildOptions(0))),
                 S("cam 10:00:00-10:02:00 H | spotA 10:02:00-10:02:30"));
        QVERIFY(m_store->setSettingJson(S("ui.inputMap"), QJsonObject{}));
    }

    void importUnsupported() {
        // Saturday: device, legacy and unknown instructions in front of Spot A: provenance on it, one table row each.
        const Document doc = parse(readAll(fx("import_features.in")));
        const res::Resolver rv(featureLibrary(), importResOptions());
        const imp::Importer im(nullptr, &rv, featureReader());
        const imp::Plan p = im.plan(doc, S("import_features.in"), featureOptions(QDate(2025, 9, 13)));
        const sched::HourSlot* s6 = p.slot(S("2025-09-13-6"));
        QCOMPARE(featureSummary(*s6), S("spotA@0:truncate"));
        const QJsonArray ins = s6->items[0].extra.value("masterplay").toObject().value("instructions").toArray();
        QCOMPARE(ins.size(), 7);
        for (const QJsonValue& v : ins) QCOMPARE(v.toObject().value("state").toString(), S("kept"));
        QStringList rows;
        for (const QJsonValue& v : p.unsupported) {
            const QJsonObject r = v.toObject();
            rows << QStringLiteral("%1:%2:%3:%4").arg(r.value("name").toString(), r.value("catalog").toString(), r.value("code").toString()).arg(r.value("count").toInt());
        }
        QCOMPARE(rows, (QStringList{S("CountDown To:mapped:MP022:1"), S("FooBar:unknown:MP011:1"), S("InsertTextDirectory:unsupported:MP022:1"),
                                    S("SendTo:unsupported:MP022:1"), S("Set Config:unsupported:MP022:1"), S("TTS:unsupported:MP022:1"),
                                    S("Video:mapped:MP022:1")}));
        QVERIFY(p.unsupported[4].toObject().value("why").toString().contains(S("SCTE-35")));
        QCOMPARE(p.stats.value("unsupported").toInt(), 7);
        QCOMPARE(diagCodes(p.diagnostics), S("import_features.in:69:MP011 import_features.in:68:MP022 import_features.in:70:MP022 import_features.in:71:MP022 "
                                             "import_features.in:72:MP022 import_features.in:73:MP022 import_features.in:74:MP022"));
        checkGolden(S("import_features_unsupported.json"), p.toJson());
    }

    void importFeaturesGrid() {
        // The whole fixture as a weekly grid: deterministic, ids unique per slot, the Monday loop hour in the template.
        const Document doc = parse(readAll(fx("import_features.in")));
        QList<MediaRecord> lib = featureLibrary();
        const res::Resolver a(lib, importResOptions());
        std::reverse(lib.begin(), lib.end());
        const res::Resolver b(lib, importResOptions());
        imp::Options o = mondayOptions();
        o.mode = S("grid");
        o.from = o.to = QDate();
        const imp::Plan pa = imp::Importer(nullptr, &a, featureReader()).plan(doc, S("import_features.in"), o);
        const imp::Plan pb = imp::Importer(nullptr, &b, featureReader()).plan(doc, S("import_features.in"), o);
        QVERIFY2(pa.ok(), qPrintable(pa.errors.join(S("; "))));
        QCOMPARE(pb.toJson(true, true), pa.toJson(true, true));
        QCOMPARE(pa.hourSlots.size(), 168);
        QVERIFY(pa.slot(S("1-7"))->isLooping);
        QCOMPARE(featureSummary(*pa.slot(S("1-7"))), S("cont(1-6,60) LOOP spotA#8 weather#8 spotB#8"));
        const QRegularExpression idRx(S("^mp-[0-9a-f]{16}(~p\\d+)?(~j)?$"));
        for (const sched::HourSlot& s : pa.hourSlots) {
            QSet<QString> ids;
            for (const sched::SlotItem& it : s.items) {
                QVERIFY2(idRx.match(it.id).hasMatch(), qPrintable(it.id));
                QVERIFY2(!ids.contains(it.id), qPrintable(s.id + S(": ") + it.id));
                ids.insert(it.id);
            }
        }
        QCOMPARE(pa.stats.value("validation").toObject().value("errors").toInt(), 0);
    }

    void importOptionsMp08b() {
        QStringList errs;
        const imp::Options o = imp::Options::fromJson(QJsonObject{{"repeat", "UNROLL"}, {"osd", "hooks"}, {"breaks", "live"}, {"networkInput", " in1 "},
                                                                  {"inputMap", QJsonObject{{"Studio", "ndi://S"}}}, {"collapseUnsegmentedActs", false},
                                                                  {"selectRandomOrder", "even"}, {"maxUnrollItems", 50}},
                                                      &errs);
        QVERIFY2(errs.isEmpty(), qPrintable(errs.join(S("; "))));
        QCOMPARE(o.repeat, S("unroll"));
        QCOMPARE(o.osd, S("hooks"));
        QCOMPARE(o.breaks, S("live"));
        QCOMPARE(o.networkInput, S("in1"));
        QCOMPARE(o.inputMap.value("Studio").toString(), S("ndi://S"));
        QVERIFY(!o.collapseUnsegmentedActs);
        QCOMPARE(o.selectRandomOrder, S("even"));
        QCOMPARE(o.maxUnrollItems, 100);   // clamped
        QCOMPARE(imp::Options::fromJson(o.toJson()).toJson(), o.toJson());
        errs.clear();
        const imp::Options bad = imp::Options::fromJson(QJsonObject{{"repeat", "forever"}, {"osd", "loud"}, {"breaks", "gpi"}, {"inputMap", "x"},
                                                                    {"selectRandomOrder", "chaos"}},
                                                        &errs);
        QCOMPARE(errs.size(), 5);
        QCOMPARE(bad.repeat, S("loop"));
        QCOMPARE(bad.osd, S("auto"));
        QCOMPARE(bad.breaks, S("asScheduled"));
        QVERIFY(bad.check().isEmpty());
    }

    // ---- MP-09: apply / undo / history ---------------------------------------------------------------------------------

    void importApplyUndo() {
        // The dated Monday into a temp store holding an override at 09:00 and a template slot: refused on air, applied with a
        // backup and a history entry, undo refused on air, then undone byte for byte.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        sched::ScheduleStore st;
        QVERIFY(st.open(dir.filePath(S("apply.db"))));
        sched::ChannelProfile cp;
        cp.id = S("mptest");
        cp.name = S("MP test");
        cp.timezone = S("UTC");
        QVERIFY(st.saveChannel(cp));
        auto stamped = [](const QJsonObject& j) {   // compact JSON without the save time
            QJsonObject c = j;
            c.remove(S("updatedAt"));
            return QJsonDocument(c).toJson(QJsonDocument::Compact);
        };
        sched::HourSlot old;
        old.channelId = S("mptest");
        old.date = QDate(2025, 9, 8);
        old.hour = 9;
        old.id = sched::HourSlot::dateId(old.date, 9);
        sched::SlotItem keep;
        keep.id = S("keep-1");
        keep.mediaId = S("talk");
        keep.durationSec = 600;
        keep.timingMode = S("hard_start");
        keep.fixedOffsetSec = 120;
        keep.cgOnStart = {S("bug")};
        keep.extra[S("custom")] = QJsonObject{{"x", 1}, {"y", QJsonArray{1, "two"}}};
        old.items << keep;
        old.blockType = S("news");
        old.showBlock = QJsonObject{{"title", "Old News"}, {"locked", false}};
        old.deviceCommands = QJsonArray{QJsonObject{{"id", "d1"}, {"commandId", "ptz:1"}, {"offsetSec", 5}}};
        QVERIFY(st.saveSlot(old));
        sched::HourSlot tmpl;
        tmpl.channelId = S("mptest");
        tmpl.dayOfWeek = 1;
        tmpl.hour = 9;
        tmpl.id = sched::HourSlot::templateId(1, 9);
        tmpl.items << keep;
        QVERIFY(st.saveSlot(tmpl));
        const QByteArray oldJson = stamped(st.slot(S("mptest"), old.id).toJson());
        const QByteArray tmplJson = stamped(st.slot(S("mptest"), tmpl.id).toJson());

        const Document doc = parse(readAll(fx("import_times.in")));
        const res::Resolver rv(importLibrary(), importResOptions());
        const imp::Importer im(&st, &rv, tl::memoryReader({}));
        const imp::Plan p = im.plan(doc, S("import_times.in"), mondayOptions());
        QVERIFY2(p.ok(), qPrintable(p.errors.join(S("; "))));
        QCOMPARE(p.hourSlots.size(), 24);
        QVERIFY(p.onAirSlots.isEmpty());   // planned a week before

        // On air: [10:30, 12:30] on the Monday touches 10, 11 and 12. Nothing is written.
        int saves = 0;
        imp::ApplyOptions onAir;
        onAir.nowMs = QDateTime(QDate(2025, 9, 8), QTime(10, 30), QTimeZone::UTC).toMSecsSinceEpoch();
        onAir.afterSave = [&saves]() { ++saves; };
        const quint64 rev0 = st.scheduleRevision();
        imp::ApplyResult r = im.apply(p, onAir);
        QCOMPARE(r.status, 409);
        QCOMPARE(r.onAirSlots, (QStringList{S("2025-09-08-10"), S("2025-09-08-11"), S("2025-09-08-12")}));
        QVERIFY(r.error.contains(S("allowOnAir")));
        QCOMPARE(st.scheduleRevision(), rev0);
        QCOMPARE(saves, 0);
        QVERIFY(im.imports().isEmpty());
        QCOMPARE(r.toJson().value("ok").toBool(), false);

        // Applied.
        imp::ApplyOptions ao;
        ao.nowMs = kImportNow;
        ao.by = S("test");
        ao.afterSave = [&saves]() { ++saves; };
        r = im.apply(p, ao);
        QVERIFY2(r.ok(), qPrintable(r.error));
        QVERIFY2(QRegularExpression(S("^mp20250901T120000000-[0-9a-f]{6}$")).match(r.importId).hasMatch(), qPrintable(r.importId));
        const QString id = r.importId;
        QCOMPARE(r.written.size(), 24);
        QCOMPARE(r.backedUp, QStringList{old.id});
        QCOMPARE(r.created.size(), 23);
        QVERIFY(r.failed.isEmpty());
        QCOMPARE(saves, 1);   // enforceMarkers once, not per slot
        for (const sched::HourSlot& s : p.hourSlots) {
            bool found = false;
            const sched::HourSlot got = st.slot(S("mptest"), s.id, &found);
            QVERIFY2(found, qPrintable(s.id));
            QCOMPARE(got.items.size(), s.items.size());
            for (int i = 0; i < s.items.size(); ++i) QCOMPARE(got.items[i].id, s.items[i].id);
            QCOMPARE(got.isContinuation(), s.isContinuation());
            const QJsonObject prov = got.showBlock.value("masterplay").toObject();
            QCOMPARE(prov.value("importId").toString(), id);
            QCOMPARE(prov.value("sha256").toString(), p.sha256);
            QCOMPARE(got.showBlock.value("title"), s.showBlock.value("title"));
        }
        QCOMPARE(stamped(st.slot(S("mptest"), tmpl.id).toJson()), tmplJson);   // a dated import leaves the template alone
        // History and backup.
        const QJsonArray hist = im.imports();
        QCOMPARE(hist.size(), 1);
        const QJsonObject e = hist.first().toObject();
        QCOMPARE(e.value("id").toString(), id);
        QCOMPARE(e.value("state").toString(), S("applied"));
        QCOMPARE(e.value("by").toString(), S("test"));
        QCOMPARE(e.value("channel").toString(), S("mptest"));
        QCOMPARE(e.value("mode").toString(), S("dates"));
        QCOMPARE(e.value("from").toString(), S("2025-09-08"));
        QCOMPARE(e.value("file").toString(), S("import_times.in"));
        QCOMPARE(e.value("sha256").toString(), p.sha256);
        QCOMPARE(e.value("slots").toInt(), 24);
        QCOMPARE(e.value("written").toInt(), 24);
        QCOMPARE(e.value("replaced").toArray(), QJsonArray{old.id});
        QCOMPARE(e.value("created").toArray().size(), 23);
        QVERIFY(e.value("backup").toBool());
        QVERIFY(!e.contains("onAir"));
        QCOMPARE(e.value("options").toObject(), p.options.toJson());
        QCOMPARE(r.entry, e);
        const QJsonObject bk = im.backup(id);
        QCOMPARE(bk.value("importId").toString(), id);
        QCOMPARE(bk.value("order").toArray().size(), 24);
        QCOMPARE(stamped(bk.value("slots").toObject().value(old.id).toObject()), oldJson);
        QVERIFY(bk.value("slots").toObject().value(S("2025-09-08-10")).isNull());
        for (const sched::HourSlot& s : p.hourSlots)
            QCOMPARE(bk.value("written").toObject().value(s.id).toString(), imp::Importer::slotDigest(st.slot(S("mptest"), s.id).toJson()));
        QVERIFY(QJsonDocument::fromJson(st.setting(S("masterplay.imports")).toUtf8()).isObject());

        // Undo refused on air; nothing changes.
        imp::ApplyOptions uo = onAir;
        r = im.undo(id, uo);
        QCOMPARE(r.status, 409);
        QCOMPARE(r.onAirSlots, (QStringList{S("2025-09-08-10"), S("2025-09-08-11"), S("2025-09-08-12")}));
        QCOMPARE(st.slot(S("mptest"), S("2025-09-08-10")).showBlock.value("masterplay").toObject().value("importId").toString(), id);
        QCOMPARE(im.imports().first().toObject().value("state").toString(), S("applied"));

        // Undone: the override back byte for byte, the 23 created slots gone, the template untouched.
        saves = 0;
        uo.nowMs = kImportNow;
        uo.by = S("tester");
        r = im.undo(id, uo);
        QVERIFY2(r.ok(), qPrintable(r.error));
        QCOMPARE(r.restored, QStringList{old.id});
        QCOMPARE(r.deleted.size(), 23);
        QVERIFY(r.conflicts.isEmpty());
        QVERIFY(r.unchanged.isEmpty());
        QCOMPARE(saves, 1);
        QCOMPARE(stamped(st.slot(S("mptest"), old.id).toJson()), oldJson);
        QCOMPARE(stamped(st.slot(S("mptest"), tmpl.id).toJson()), tmplJson);
        for (const sched::HourSlot& s : p.hourSlots) {
            if (s.id == old.id) continue;
            bool found = true;
            st.slot(S("mptest"), s.id, &found);
            QVERIFY2(!found, qPrintable(s.id));
        }
        const QJsonObject u = im.imports().first().toObject();
        QCOMPARE(u.value("state").toString(), S("undone"));
        QCOMPARE(u.value("undoneBy").toString(), S("tester"));
        QCOMPARE(u.value("undo").toObject().value("deleted").toInt(), 23);
        QCOMPARE(im.undo(id, uo).status, 409);          // already undone
        QCOMPARE(im.undo(S("nope"), uo).status, 404);
        QCOMPARE(im.undo(QString(), uo).status, 404);

        // The same plan again (another id) gives the same slots; applying on air with allowOnAir is recorded.
        imp::ApplyOptions forced = onAir;
        forced.allowOnAir = true;
        r = im.apply(p, forced);
        QVERIFY2(r.ok(), qPrintable(r.error));
        QVERIFY(r.importId != id);
        QCOMPARE(r.onAirSlots.size(), 3);
        QCOMPARE(r.entry.value("onAir").toArray().size(), 3);
        QCOMPARE(im.imports().size(), 2);
        QCOMPARE(im.imports(1).first().toObject().value("id").toString(), r.importId);   // newest first
    }

    void importApplyConflictsAndHistory() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        sched::ScheduleStore st;
        QVERIFY(st.open(dir.filePath(S("history.db"))));
        sched::ChannelProfile cp;
        cp.id = S("mptest");
        cp.timezone = S("UTC");
        QVERIFY(st.saveChannel(cp));
        const Document doc = parse(readAll(fx("import_times.in")));
        const res::Resolver rv(importLibrary(), importResOptions());
        const imp::Importer im(&st, &rv, tl::memoryReader({}));
        imp::Options o = mondayOptions();
        o.hours = {9, 10};
        const imp::Plan p = im.plan(doc, S("import_times.in"), o);
        QCOMPARE(p.hourSlots.size(), 2);
        imp::ApplyOptions ao;
        ao.nowMs = kImportNow;

        // A slot edited after the import is a conflict: left alone, the entry stays undoable; force restores it.
        imp::ApplyResult r = im.apply(p, ao);
        QVERIFY2(r.ok(), qPrintable(r.error));
        sched::HourSlot edited = st.slot(S("mptest"), S("2025-09-08-10"));
        edited.items.removeLast();
        QVERIFY(st.saveSlot(edited));
        const QString editedDigest = imp::Importer::slotDigest(st.slot(S("mptest"), S("2025-09-08-10")).toJson());
        imp::ApplyResult u = im.undo(r.importId, ao);
        QVERIFY2(u.ok(), qPrintable(u.error));
        QCOMPARE(u.deleted, QStringList{S("2025-09-08-9")});
        QCOMPARE(u.conflicts, QStringList{S("2025-09-08-10")});
        QCOMPARE(imp::Importer::slotDigest(st.slot(S("mptest"), S("2025-09-08-10")).toJson()), editedDigest);
        QCOMPARE(im.imports().first().toObject().value("state").toString(), S("undoPartial"));
        u = im.undo(r.importId, ao);   // again: slot 9 is now unchanged, 10 still a conflict
        QCOMPARE(u.unchanged, QStringList{S("2025-09-08-9")});
        QCOMPARE(u.conflicts, QStringList{S("2025-09-08-10")});
        imp::ApplyOptions force = ao;
        force.force = true;
        u = im.undo(r.importId, force);
        QVERIFY2(u.ok(), qPrintable(u.error));
        QCOMPARE(u.deleted, QStringList{S("2025-09-08-10")});
        QCOMPARE(u.conflicts, QStringList{S("2025-09-08-10")});
        QCOMPARE(im.imports().first().toObject().value("state").toString(), S("undone"));
        bool found = true;
        st.slot(S("mptest"), S("2025-09-08-10"), &found);
        QVERIFY(!found);

        // Bad requests write nothing.
        const quint64 rev = st.scheduleRevision();
        imp::Options noCh = o;
        noCh.channelId.clear();
        QCOMPARE(im.apply(im.plan(doc, {}, noCh), ao).status, 400);
        imp::Options other = o;
        other.channelId = S("nope");
        QCOMPARE(im.apply(im.plan(doc, {}, other), ao).status, 400);
        imp::Options bad = o;
        bad.mode = S("bogus");
        r = im.apply(im.plan(doc, {}, bad), ao);
        QCOMPARE(r.status, 400);
        QVERIFY(r.error.startsWith(S("the plan has errors")));
        imp::ApplyOptions badId = ao;
        badId.importId = S("a b");
        QCOMPARE(im.apply(p, badId).status, 400);
        QCOMPARE(imp::Importer(nullptr, &rv).apply(p, ao).status, 500);
        QCOMPARE(st.scheduleRevision(), rev);
        imp::ApplyOptions named = ao;
        named.importId = S("my-import.1");
        QVERIFY(im.apply(p, named).ok());
        QCOMPARE(im.apply(p, named).status, 409);   // the id is taken

        // History: newest 100 entries, backups for the newest 20; a pruned import cannot be undone.
        for (int i = 0; i < 105; ++i) {
            imp::ApplyOptions a = ao;
            a.nowMs = kImportNow + i;
            QVERIFY2(im.apply(p, a).ok(), qPrintable(QString::number(i)));
        }
        const QJsonArray hist = im.imports();
        QCOMPARE(hist.size(), imp::Importer::kMaxImports);
        QCOMPARE(im.imports(5).size(), 5);
        for (int i = 0; i < hist.size(); ++i) {
            const QJsonObject e = hist.at(i).toObject();
            QCOMPARE(e.value("backup").toBool(), i < imp::Importer::kMaxBackups);
            QCOMPARE(im.backup(e.value("id").toString()).isEmpty(), i >= imp::Importer::kMaxBackups);
        }
        QCOMPARE(im.backup(S("my-import.1")), QJsonObject());   // dropped from the history: its backup is cleared too
        u = im.undo(hist.at(imp::Importer::kMaxBackups).toObject().value("id").toString(), ao);
        QCOMPARE(u.status, 409);
        QVERIFY(u.error.contains(S("backup")));
        u = im.undo(hist.first().toObject().value("id").toString(), ao);
        QVERIFY2(u.ok(), qPrintable(u.error));
        QCOMPARE(u.restored.size(), 2);   // back to what the previous import wrote

        // ApplyOptions from JSON.
        const imp::ApplyOptions j = imp::ApplyOptions::fromJson(QJsonObject{{"importId", " x1 "}, {"allowOnAir", true}, {"force", true}, {"nowMs", 5.0}, {"by", "web"}});
        QCOMPARE(j.importId, S("x1"));
        QVERIFY(j.allowOnAir && j.force);
        QCOMPARE(j.nowMs, qint64(5));
        QCOMPARE(j.by, S("web"));
        QVERIFY(!imp::ApplyOptions::fromJson({}).allowOnAir);

        // onAirAmong: a grid id by weekday / hour, a dated id by date, across midnight.
        const QTimeZone utc = QTimeZone::utc();
        const qint64 sat2330 = QDateTime(QDate(2025, 9, 13), QTime(23, 30), QTimeZone::UTC).toMSecsSinceEpoch();
        QCOMPARE(imp::Importer::onAirAmong({S("6-23"), S("0-0"), S("0-1"), S("0-2"), S("2025-09-14-1"), S("2025-09-13-22")}, utc, sat2330),
                 (QStringList{S("6-23"), S("0-0"), S("0-1"), S("2025-09-14-1")}));
    }

    void importPlantCorpus() {
        // Opt-in: the plant week through the importer with every file in a synthetic library (30 s each). Sub-playlists are
        // not readable here (MP024), so this checks scale and determinism, not the content.
        const QString dir = qEnvironmentVariable("PLAYPLAY_MP_SAMPLES");
        if (dir.isEmpty()) QSKIP("PLAYPLAY_MP_SAMPLES is not set (point it at a folder holding plant_playlist.in)");
        const QByteArray bytes = readAll(QDir(dir).filePath("plant_playlist.in"));
        if (bytes.isEmpty()) QSKIP("plant_playlist.in not found under PLAYPLAY_MP_SAMPLES");
        const Document doc = parse(bytes);
        QList<MediaRecord> lib;
        QSet<QString> seen;
        for (const Entry& e : doc.entries) {
            if (e.kind != LineKind::Media || seen.contains(e.path.toCaseFolded())) continue;
            seen.insert(e.path.toCaseFolded());
            lib << rec(qPrintable(QStringLiteral("p%1").arg(lib.size())), e.path, res::fileName(e.path), "program", 30);
        }
        const res::Resolver rv(lib, importResOptions());
        imp::Options o;
        o.channelId = S("plant");
        o.timeZone = S("UTC");
        o.nowMs = kImportNow;
        QElapsedTimer t;
        t.start();
        const imp::Plan p = imp::Importer(nullptr, &rv, tl::memoryReader({})).plan(doc, S("plant_playlist.in"), o);
        const qint64 ms = t.elapsed();
        QVERIFY(p.ok());
        QCOMPARE(p.hourSlots.size(), 168);
        QCOMPARE(p.stats.value("events").toInt(), 374);
        QCOMPARE(p.stats.value("resolver").toObject().value("missingFiles").toInt(), 0);
        QCOMPARE(imp::Importer(nullptr, &rv, tl::memoryReader({})).plan(doc, S("plant_playlist.in"), o).toJson(), p.toJson());
        int eventsPlaced = 0;   // events with at least one placed item
        for (const QJsonValue& v : p.stats.value("starts").toObject()) eventsPlaced += v.toInt();
        qInfo("plant grid plan: %d slots, %d items, %d hard starts, starts %s (%d of 374 events place an item), %d continuations, "
              "%d unsupported lines, %d folder picks, %d overlays, %d pods, dropped %s, %lld ms", int(p.hourSlots.size()), p.stats.value("items").toInt(),
              p.stats.value("hardStarts").toInt(), QJsonDocument(p.stats.value("starts").toObject()).toJson(QJsonDocument::Compact).constData(), eventsPlaced,
              p.stats.value("continuations").toInt(), p.stats.value("unsupported").toInt(), p.stats.value("folderPicks").toInt(),
              p.stats.value("overlays").toInt(), p.stats.value("pods").toInt(), QJsonDocument(p.stats.value("dropped").toObject()).toJson(QJsonDocument::Compact).constData(), ms);
        QCOMPARE(p.stats.value("hardStarts").toInt(), p.stats.value("starts").toObject().value("onTime").toInt() +
                                                          p.stats.value("starts").toObject().value("native").toInt() +
                                                          p.stats.value("starts").toObject().value("capped").toInt());
        QVERIFY2(ms < 20000, qPrintable(QString::number(ms)));
    }
};

QTEST_GUILESS_MAIN(TestMpImport)
#include "test_mpimport.moc"
