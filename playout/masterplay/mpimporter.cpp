#include "mpimporter.h"

#include "mpasrun.h"
#include "mposd.h"
#include "schedulestore.h"
#include "schedulevalidator.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QTimeZone>
#include <algorithm>
#include <climits>
#include <cmath>
#include <memory>

namespace mp::imp {

namespace {

constexpr qint64 kHourMs = 3600000;
constexpr qint64 kDayMs = 86400000;
constexpr int kMaxListed = 50;   // lines listed per report row

const QStringList kModes{QStringLiteral("grid"), QStringLiteral("dates")};
const QStringList kMissing{QStringLiteral("drop"), QStringLiteral("keepPath"), QStringLiteral("ingest")};
const QStringList kBoundaries{QStringLiteral("continue"), QStringLiteral("cut")};
const QStringList kRepeat{QStringLiteral("loop"), QStringLiteral("unroll")};
const QStringList kOsd{QStringLiteral("auto"), QStringLiteral("cues"), QStringLiteral("hooks"), QStringLiteral("off")};
const QStringList kBreaks{QStringLiteral("asScheduled"), QStringLiteral("live"), QStringLiteral("skip")};
const QStringList kSelectRandom{QStringLiteral("random"), QStringLiteral("even")};
const QStringList kKinds{QStringLiteral("media"), QStringLiteral("folder"), QStringLiteral("live"), QStringLiteral("structural"),
                         QStringLiteral("break"), QStringLiteral("osd"), QStringLiteral("info"), QStringLiteral("unsupported")};
/// Drop reasons that mean "no media / no mapping", not layout: MP016 is not raised for a window holding one.
const QStringList kContentDrops{QStringLiteral("missing"), QStringLiteral("noDuration"), QStringLiteral("emptyFolder"), QStringLiteral("searchTerm"),
                                QStringLiteral("unsupported"), QStringLiteral("liveUnmapped"), QStringLiteral("noTime"), QStringLiteral("breakSkipped")};

qint64 floorDiv(qint64 a, qint64 b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
qint64 secToMs(double s) { return qint64(std::llround(s * 1000.0)); }
double msToSec(qint64 ms) { return double(ms) / 1000.0; }

/// "HH:MM:SS" (hours past 24 for a spill; "-" before midnight).
QString clock(qint64 ms) {
    const bool neg = ms < 0;
    const qint64 s = (neg ? -ms : ms) / 1000;
    return QStringLiteral("%1%2:%3:%4")
        .arg(neg ? QStringLiteral("-") : QString())
        .arg(s / 3600, 2, 10, QLatin1Char('0'))
        .arg(s / 60 % 60, 2, 10, QLatin1Char('0'))
        .arg(s % 60, 2, 10, QLatin1Char('0'));
}
QString mmss(qint64 ms) {   // "m:ss", or "h:mm:ss" from an hour on, for durations in messages
    const qint64 s = qAbs(ms) / 1000;
    if (s >= 3600) return QStringLiteral("%1:%2:%3").arg(s / 3600).arg(s / 60 % 60, 2, 10, QLatin1Char('0')).arg(s % 60, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QLatin1Char('0'));
}

QString canon(const QString& v, const QStringList& allowed) {
    for (const QString& a : allowed) if (a.compare(v.trimmed(), Qt::CaseInsensitive) == 0) return a;
    return {};
}
QJsonArray intArray(const QList<int>& l) {
    QJsonArray a;
    for (int v : l) a.append(v);
    return a;
}
QString compact(const QJsonObject& o) { return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)); }
QString shortHash(const QString& s) {
    return QString::fromLatin1(QCryptographicHash::hash(s.toUtf8(), QCryptographicHash::Sha1).toHex().left(10));
}

bool isStructural(Instr id) {
    switch (id) {
    case Instr::Normal: case Instr::Repeat: case Instr::Fillup: case Instr::WaitForGPI: case Instr::Package:
    case Instr::InsertPlaylist: case Instr::ChangePlaylist:
        return true;
    default:
        return false;
    }
}
bool isOsd(Instr id) {
    switch (id) {
    case Instr::ShowLogo: case Instr::ShowCrawl: case Instr::ShowCrawlFromFile: case Instr::ShowText: case Instr::ShowTextFromFile:
    case Instr::ShowClock: case Instr::RealTimeClock: case Instr::OsdOff:
        return true;
    default:
        return false;
    }
}
/// A line kept as provenance that would take air time in MasterPlay (an unknown instruction, a content or live
/// instruction with no PlayPlay form): the content around it is short, so MP016 is not raised for its window.
bool isTimed(const Instruction& ins) {
    if (!ins.known()) return true;
    return ins.info->group == InstrGroup::Content || ins.info->group == InstrGroup::Live;
}
/// Why an instruction stays provenance (design §6.9, the unsupported table).
QString unsupportedWhy(const Instruction& ins) {
    switch (ins.id) {
    case Instr::SendTo:
        return QStringLiteral("no PlayPlay device command sends it yet (http: / udp: device commands are MP-17c)");
    case Instr::SetConfig:
        if (ins.head.contains(QLatin1String("network"), Qt::CaseInsensitive) || ins.head.contains(QLatin1String("SCTE"), Qt::CaseInsensitive))
            return QStringLiteral("out-of-network / SCTE-35 settings map to the mux's SCTE-35 later (muxmanager), not here");
        return QStringLiteral("runtime configuration has no PlayPlay form");
    case Instr::CountDownTo: case Instr::CountUpFrom:
        return QStringLiteral("counters need command-object CG hooks, which the engine does not run yet (MP-17)");
    case Instr::Overlay: case Instr::ShowCGC:
        return QStringLiteral("no PlayPlay CG template is matched to it");
    case Instr::Video:
        return QStringLiteral("\"Set only\" switches the router without playing anything");
    case Instr::LiveStream:
        return QStringLiteral("it names no Path=");
    case Instr::InsertTextDirectory: case Instr::InsertFile: case Instr::ReadPlaylist: case Instr::Switcher:
        return QStringLiteral("no PlayPlay form plays it");
    default:
        return ins.known() ? QStringLiteral("no PlayPlay form plays it") : QStringLiteral("unknown instruction");
    }
}

/// The planned on-air length of a media line: the playlist's forced bracket, else the library's duration, else (an
/// unresolved line kept by path) LengthStr / ExpectedLengthStr. -1 when nothing says. ExpectedLength= alone is not
/// used: its unit is not known [UNVERIFIED].
double lengthOf(const Entry& e, int fps) {
    if (e.forcedDurationSec > 0) return e.forcedDurationSec;
    for (const char* key : {"LengthStr", "ExpectedLengthStr"}) {
        bool ok = false;
        const double d = parseDuration(e.value(QString::fromLatin1(key)).trimmed(), fps, &ok);
        if (ok && d > 0) return d;
    }
    return -1;
}
double plannedDuration(const res::Resolution& r, const Entry& e, int fps) {
    if (r.forcedDurationSec > 0) return r.forcedDurationSec;
    if (r.resolved()) return r.media.durationSec;
    return lengthOf(e, fps);
}

/// A text file read at import (crawl / text snapshots): UTF-8 (a BOM dropped) unless it does not decode, then Latin-1;
/// whitespace runs (line breaks included) become one space; at most 1,000 characters.
QString snapshotText(const QByteArray& b) {
    QString s = QString::fromUtf8(b);
    if (s.contains(QChar(0xFFFD))) s = QString::fromLatin1(b);
    if (s.startsWith(QChar(0xFEFF))) s.remove(0, 1);
    return s.simplified().left(1000);
}

bool sameEvent(const tl::Window& a, const tl::Window& b) {
    return a.eventIndex == b.eventIndex && a.line == b.line && a.file.compare(b.file, Qt::CaseInsensitive) == 0;
}

/// A diagnostic raised while laying a line, reported once its placement is known to be in the output.
struct PendingNote {
    QString code;
    int col = 1;
    QString message;
};

/// What an OSD line does (design §6.8): open a layer of a role, or clear them all.
struct Osd {
    int op = 0;                    // 1 open, 2 off; 0 nothing (unread file, no image)
    QString kind, role, id;        // logo | crawl | text | clock; bug | secondary | ticker | lower_third | clock
    QJsonObject cue;               // the item cue (cgOverlays entry)
    QString hookShow, hookHide;    // cgOnStart / cgOnEnd strings (hooks mode; clocks: the identity name)
    QString text, filePath;        // what it shows
    QString keptReason;            // op 0: osdUnread
    QList<PendingNote> diags;
};

/// One placement plus what the layout needs.
struct Pl {
    Placement pub;
    qint64 atMs = 0;               // the cursor when the line was reached (where it would air)
    QString eventSrc;              // "<window file>#<line>"
    int winIdx = -1;               // its window in Run::wins
    int copy = 0;                  // loop pass
    res::Resolution r;
    bool hasRes = false;
    res::FolderCoverage fc;        // folder lines
    bool hasFolder = false, folderLine = false;   // folderLine: the line's first pick (the folder report's use)
    bool noProvenance = false;     // a later pick of a folder line: its line is already provenance
    sched::SlotItem item;
    QJsonObject mp;                // extra.masterplay without the provenance lists and the anchor
    qint64 inOffsetMs = 0;         // joined in progress at its window's start
    qint64 contentMs = 0;          // planned length before any trim
    bool cutAtHour = false;        // hourBoundary cut ended it at the top of the hour
    bool loopCut = false;          // a loop hour ended it at the top of the hour
    bool eventFirst = false;       // the first placed item of an event (not of a carry), or of a timed pod
    bool podStart = false;         // ... of a timed pod (schedtime=)
    qint64 anchorMs = 0;
    int eventType = 0;
    bool capped = false;
    QString rule;                  // onTime | native | capped | soft
    bool timed = false;            // a kept line that would take air time
    int pod = -1;                  // break pods: the pod (break lines and pod items)
    int act = -1;                  // actinout
    QString actMode;               // segments | collapsed | parts
    QString infoText;              // !Info
    QString why;                   // unsupported lines: why (the unsupported table)
    Osd osd;                       // OSD lines
    QJsonArray cues;               // the OSD pass: item cues of the open layers
    QStringList onStart, onEnd;    // ... and hooks
    QJsonArray before, after;      // provenance: lines not placed before it / after it (window end)
    QList<int> beforeIdx, afterIdx;// ... as placement indexes (OSD and !Info lines apply through them)
    QList<PendingNote> diags;
};

/// One laid window (windows of the sequence in order; merged carry windows too).
struct Win {
    int day = 0, win = 0;
    bool merged = false, virt = false;
    qint64 anchorMs = -1;          // the event start (-1: a carry)
    qint64 startMs = 0;            // where its content starts airing
    qint64 endMs = 0;              // where it stops: the next event's start (loops, "until the next event" lengths)
    QList<int> pls;
    QJsonArray unplaced;           // its lines when nothing of it was placed
    QList<int> unplacedIdx;
    bool started = false;          // an event that placed something (only then does its start trim what plays)
    bool looped = false, breakLive = false, hasPods = false;
    int unrolled = 0;              // lines laid after the first pass
    QList<PendingNote> diags;
};

/// The layout state of one window.
struct Lay {
    int k = 0, wi = 0, winIdx = -1;
    bool virt = false;
    qint64 anchorMs = -1, cursor = 0, fromMs = 0, endMs = 0;
    qint64 evStartMs = 0, evDayMs = 0;   // the event's start and its day's midnight (schedtime=)
    int eventType = 0;
    QString evSrc;
    QJsonArray pending;
    QList<int> pendingIdx;
    int lastPlaced = -1;
    bool first = true, capped = false;
    int copy = 0;
    bool inPod = false;
    int pod = -1, podOrdinal = 0;
    qint64 podAnchor = -1;
    QHash<QString, int> actFirst;  // acts of this pass: file key -> line of the part that airs whole
    qint64 curHour = LLONG_MIN;
    bool loopHour = false;
};

struct LiveTarget {
    bool ok = false;
    QString id, path, why;
    QJsonObject liveSource;
};

struct Run {
    const Options& o;
    const res::Resolver& rv;
    Plan& p;
    tl::ReadFn read;
    sched::ScheduleStore* store = nullptr;
    bool grid = true;
    QList<tl::DayTimeline> seq;
    int outFirst = 0;              // first output day in seq
    qint64 outStart = 0, seqEnd = 0;
    QList<Pl> pls;
    QList<Win> wins;
    QList<QList<int>> winIndex;    // [day][window] -> wins index
    QList<int> laid;               // placed items, in time order
    QHash<QString, res::Resolution> cache;
    QHash<QString, Osd> osdCache;
    QHash<QString, tl::ReadResult> fileCache;
    QSet<qint64> loopHours;        // absolute hours (ms / kHourMs) written as isLooping
    QHash<qint64, qint64> loopFill;// ... and the ms the loop replays after its first pass
    qint64 minItemMs = 500, maxLateMs = 300000;
    bool cut = false;
    QList<tl::Note> notes;
    QHash<QString, int> noteIndex;
    QJsonArray orphans;            // provenance of laid items a later start dropped: goes onto the next placed item
    QList<int> orphanIdx;

    Run(const Options& opt, const res::Resolver& r, Plan& plan) : o(opt), rv(r), p(plan) {}

    void note(const QString& code, const QString& file, int line, int col, const QString& message, const QString& day) {
        const QString k = file.toCaseFolded() + QLatin1Char('|') + QString::number(line) + QLatin1Char('|') + QString::number(col) + QLatin1Char('|') +
                          code + QLatin1Char('|') + message;
        const auto it = noteIndex.constFind(k);
        if (it != noteIndex.cend()) {
            if (!day.isEmpty() && !notes[*it].days.contains(day)) notes[*it].days << day;
            return;
        }
        tl::Note n;
        n.diag = Diagnostic{line, col, diagnosticInfo(code).severity, code, message};
        n.file = file;
        if (!day.isEmpty()) n.days << day;
        noteIndex.insert(k, int(notes.size()));
        notes << n;
    }

    int add(const Pl& x) {
        pls << x;
        return int(pls.size()) - 1;
    }

    QJsonObject provenance(const Pl& x, const tl::Item& it, const QString& windowFile) const {
        QJsonObject l{{"line", it.entry.line}, {"raw", it.entry.raw}, {"state", x.pub.state}};
        if (it.file.compare(windowFile, Qt::CaseInsensitive) != 0) l["file"] = it.file;
        if (!x.pub.name.isEmpty()) l["name"] = x.pub.name;
        if (!x.pub.reason.isEmpty()) l["reason"] = x.pub.reason;
        if (x.copy > 0) l["copy"] = x.copy;
        return l;
    }

    void drop(Pl& x, const QString& reason) {
        x.pub.state = QStringLiteral("dropped");
        x.pub.reason = reason;
    }

    /// A line that places nothing: its provenance waits for the next placed item.
    int keep(Lay& s, Win& W, Pl& x, const tl::Item& it, const tl::Window& w) {
        if (!x.noProvenance) s.pending.append(provenance(x, it, w.file));
        const int idx = add(x);
        if (!x.noProvenance) s.pendingIdx << idx;
        W.pls << idx;
        return idx;
    }

    /// A laid item the trim drops: its provenance lines would reach no written item, so they move to the next placed
    /// one, marked with the event they came from.
    void dropLaid(Pl& q, const QString& reason) {
        drop(q, reason);
        const QString evFile = q.eventSrc.section(QLatin1Char('#'), 0, -2);
        for (const QJsonArray* a : {&q.before, &q.after})
            for (const QJsonValue& v : *a) {
                QJsonObject l = v.toObject();
                if (!l.contains(QStringLiteral("file")) && !evFile.isEmpty()) l["file"] = evFile;
                l["carriedFrom"] = q.eventSrc;
                orphans.append(l);
            }
        orphanIdx << q.beforeIdx << q.afterIdx;
        q.before = QJsonArray();
        q.after = QJsonArray();
        q.beforeIdx.clear();
        q.afterIdx.clear();
    }

    /// The next event's start trims what is still playing (the header comment's three rules). Returns where the event
    /// starts; capped is set when a floating skip had to cut the item it waited for.
    qint64 trimAt(qint64 anchor, int eventType, bool* capped) {
        *capped = false;
        const int et = (eventType == 1 && maxLateMs <= 0) ? 0 : eventType;
        if (et != 2) {
            for (int j = int(laid.size()) - 1; j >= 0; --j) {
                Pl& q = pls[laid[j]];
                if (q.pub.endMs <= anchor) break;
                if (q.pub.startMs >= anchor) {
                    dropLaid(q, et == 0 ? QStringLiteral("cutByEvent") : QStringLiteral("skippedByFloating"));
                    laid.removeAt(j);
                    continue;
                }
                if (et == 0) q.pub.endMs = anchor;
                else if (q.pub.endMs - anchor > maxLateMs) { q.pub.endMs = anchor + maxLateMs; *capped = true; }
                if (q.pub.endMs - q.pub.startMs < minItemMs) { dropLaid(q, QStringLiteral("tooShort")); laid.removeAt(j); *capped = false; }
                break;
            }
        }
        qint64 start = anchor;
        if (et != 0 && !laid.isEmpty()) start = qMax(start, pls[laid.last()].pub.endMs);
        return start;
    }

    /// Where the content of window (k, wi) stops: the next window's start that is not the same event going on past
    /// midnight, else the end of the laid days.
    qint64 nextStartMs(int k, int wi) const {
        int j = wi + 1;
        for (int kk = k; kk < seq.size(); ++kk, j = 0) {
            const QList<tl::Window>& ws = seq[kk].windows;
            for (; j < ws.size(); ++j) {
                const tl::Window& w = ws[j];
                if (w.carry && kk > 0 && !seq[kk - 1].windows.isEmpty() && sameEvent(seq[kk - 1].windows.last(), w)) continue;
                return kk * kDayMs + qint64(w.startSec) * 1000;
            }
        }
        return seqEnd;
    }

    /// The common provenance of a line's item (design §6.10).
    QJsonObject mpBase(const Pl& x, const tl::Item& ti, const tl::Window& w, const tl::DayTimeline& t) const {
        QJsonObject mp{{"src", ti.file + QLatin1Char('#') + QString::number(ti.entry.line)}, {"line", ti.entry.line}, {"raw", ti.entry.raw},
                       {"event", w.stamp.raw}, {"eventSrc", x.eventSrc}, {"day", t.day.label()}};
        if (!ti.entry.params.isEmpty()) {
            QJsonArray ps;
            for (const Param& pa : ti.entry.params) ps.append(pa.text());
            mp["params"] = ps;
        }
        if (ti.depth > 0) { mp["from"] = ti.file; mp["subGroup"] = ti.group; mp["depth"] = ti.depth; }
        if (ti.viaChange) mp["viaChange"] = true;
        if (x.copy > 0) mp["copy"] = x.copy;
        return mp;
    }
    QString idOf(const Pl& x, const tl::Item& ti, const tl::Window& w, const tl::DayTimeline& t) const {
        return Importer::itemId(ti.file, w.file, w.line, t.day.key(), w.startSec, x.pub.item, ti.entry.line, x.copy);
    }

    sched::SlotItem makeMediaItem(const Pl& x, const tl::Item& ti, const tl::Window& w, const tl::DayTimeline& t, QJsonObject* mpOut) const {
        sched::SlotItem it;
        it.id = idOf(x, ti, w, t);
        const res::Resolution& r = x.r;
        if (r.resolved()) {
            it.mediaId = r.mediaId;
            it.path = r.media.path;
            it.title = r.media.title;
            it.type = !r.media.type.isEmpty() ? r.media.type : QStringLiteral("video");
            it.durationSec = r.media.durationSec;
            if (r.forcedDurationSec > 0) it.outPointSec = r.forcedDurationSec;   // the playlist's forced length (a still's time)
        } else {
            it.path = r.mapped;
            it.title = logs::titleOf(logs::fileOf(r.mapped));
            it.type = (r.type.isEmpty() || r.type == QLatin1String("unknown")) ? QStringLiteral("video") : r.type;
            it.durationSec = x.pub.durationSec;
        }
        const QString name = ti.entry.value(QStringLiteral("Name")).trimmed();
        if (!name.isEmpty()) it.title = name;                                   // Name= is the item label (design §2.4)
        QJsonObject mp = mpBase(x, ti, w, t);
        mp["resolvedBy"] = res::resolvedByName(r.by);
        if (!r.via.isEmpty()) mp["via"] = r.via;
        if (!r.resolved()) {
            mp["keepPath"] = true;
            if (o.missing == QLatin1String("ingest")) mp["ingest"] = true;
        }
        *mpOut = mp;
        return it;
    }

    // ---- laying lines -------------------------------------------------------------------------------------------------

    /// Lays a timed placement (media, a folder pick, a live item) at the cursor; returns its index when it airs, else -1
    /// (kept as provenance). x.item / x.mp come from the caller; the in point, the cuts, the pod and the event start are
    /// added here. The event's first placeable line is where its start trims what plays (an event that places nothing
    /// writes no hard start, so on air the earlier content goes on, and the plan must say the same).
    int placeTimed(Lay& s, Win& W, Pl x, const tl::Item& it, const tl::Window& w, double dur) {
        x.pub.durationSec = dur;
        qint64 podAt = -1;
        if (!s.virt && !W.started) {
            W.started = true;
            qint64 a = s.anchorMs;
            int et = s.eventType;
            if (s.podAnchor >= a) { a = s.podAnchor; et = 0; podAt = s.podAnchor; s.podAnchor = -1; }   // a timed pod opens the event
            s.cursor = s.fromMs = W.startMs = trimAt(a, et, &s.capped);
        } else if (s.podAnchor >= 0) {   // a timed pod: an exact start at schedtime (a gap before it, or a cut)
            podAt = s.podAnchor;
            s.podAnchor = -1;
            if (podAt > s.cursor) s.cursor = podAt;
            else if (podAt < s.cursor) { bool c = false; s.cursor = trimAt(podAt, 0, &c); }
        }
        x.atMs = s.cursor;
        qint64 start = s.cursor, end = start + secToMs(dur);
        x.contentMs = end - start;
        if (cut) {   // hourBoundary cut: the item ends at the top of the hour it crosses
            const qint64 b = (floorDiv(start, kHourMs) + 1) * kHourMs;
            if (end > b) { end = b; x.cutAtHour = true; }
        }
        if (s.loopHour) {   // a loop hour's runlist cuts its last copy at the hour end
            const qint64 b = (s.curHour + 1) * kHourMs;
            if (end > b) { end = b; x.loopCut = true; }
        }
        s.cursor = end;
        x.pub.startMs = start;
        x.pub.endMs = end;
        if (end <= s.fromMs) drop(x, QStringLiteral("beforeWindow"));
        else {
            if (start < s.fromMs) { x.inOffsetMs = s.fromMs - start; start = s.fromMs; x.pub.startMs = start; }
            if (end - start < minItemMs) drop(x, QStringLiteral("tooShort"));
        }
        if (x.pub.state == QLatin1String("dropped")) {
            keep(s, W, x, it, w);
            return -1;
        }
        x.pub.state = QStringLiteral("placed");
        if (x.inOffsetMs > 0) {
            x.item.inPointSec += msToSec(x.inOffsetMs);
            x.mp["joinSec"] = msToSec(x.inOffsetMs);
        }
        if (x.cutAtHour) x.mp["cutAtHour"] = true;
        if (x.loopCut) x.mp["loopHourEnd"] = true;
        if (s.inPod) { x.pod = s.pod; x.mp["break"] = s.pod; }
        x.before = orphans;
        x.beforeIdx = orphanIdx;
        for (const QJsonValue& v : std::as_const(s.pending)) x.before.append(v);
        x.beforeIdx << s.pendingIdx;
        orphans = QJsonArray();
        orphanIdx.clear();
        s.pending = QJsonArray();
        s.pendingIdx.clear();
        if (podAt >= 0) {
            x.eventFirst = true;
            x.podStart = true;
            x.anchorMs = podAt;
            x.eventType = 0;
        } else if (s.first && !s.virt) {
            x.eventFirst = true;
            x.anchorMs = s.anchorMs;
            x.eventType = s.eventType;
            x.capped = s.capped;
        }
        s.first = false;
        const int idx = add(x);
        W.pls << idx;
        laid << idx;
        s.lastPlaced = idx;
        return idx;
    }

    /// actinout=N (design §6.7): the library segment, else one whole item for the parts of a file in this pass, else every
    /// part whole. false when the line is dropped (it was kept as provenance).
    bool actLine(Lay& s, Win& W, Pl& x, const tl::Item& it, const tl::Window& w, int act, double* dur) {
        x.act = act;
        QJsonObject a{{"index", act}};
        const QJsonArray segs = x.r.resolved() ? x.r.media.segments : QJsonArray();
        const QJsonObject seg = act < segs.size() ? segs[act].toObject() : QJsonObject();
        if (!seg.value("id").toString().isEmpty()) {
            x.actMode = QStringLiteral("segments");
            x.item.segmentId = seg.value("id").toString();
            x.item.segmentLabel = seg.value("label").toString();
            const double st = seg.value("startTime").toDouble(seg.value("time_in").toDouble(0.0));
            const double en = seg.value("endTime").toDouble(seg.value("time_out").toDouble(0.0));
            if (en > st && x.r.forcedDurationSec <= 0) *dur = en - st;
            a["segmentId"] = x.item.segmentId;
        } else if (o.collapseUnsegmentedActs) {
            x.actMode = QStringLiteral("collapsed");
            const QString key = x.r.resolved() ? x.r.mediaId : x.r.mapped.toCaseFolded();
            const auto f = s.actFirst.constFind(key);
            if (f != s.actFirst.cend()) {
                drop(x, QStringLiteral("actCollapsed"));
                x.diags << PendingNote{QStringLiteral("MP026"), 1,
                                       QStringLiteral("act %1 of '%2' is part of the whole programme placed at line %3 (the library has no segments for it): "
                                                      "define segments and re-import")
                                           .arg(act)
                                           .arg(x.item.title)
                                           .arg(*f)};
                keep(s, W, x, it, w);
                return false;
            }
            s.actFirst.insert(key, it.entry.line);
            a["collapsed"] = true;
            x.diags << PendingNote{QStringLiteral("MP026"), 1,
                                   QStringLiteral("act %1 of '%2' airs as the whole programme (the library has no segments for it); later acts of it in this "
                                                  "event are dropped, so only where the breaks fall moves")
                                       .arg(act)
                                       .arg(x.item.title)};
        } else {
            x.actMode = QStringLiteral("parts");
            x.item.segmentLabel = QStringLiteral("part %1").arg(act + 1);
            a["whole"] = true;
            x.diags << PendingNote{QStringLiteral("MP027"), 1,
                                   QStringLiteral("act %1 of '%2' has no segment in the library: this part airs the whole programme").arg(act).arg(x.item.title)};
        }
        x.mp["act"] = a;
        return true;
    }

    void mediaLine(Lay& s, Win& W, Pl& x, const tl::Item& it, const tl::Window& w, const tl::DayTimeline& t) {
        const QString key = it.entry.path + QLatin1Char('\x1f') + QString::number(it.entry.forcedDurationSec);
        auto c = cache.constFind(key);
        if (c == cache.cend()) c = cache.insert(key, rv.resolve(it.entry));
        x.r = c.value();
        x.hasRes = true;
        x.pub.kind = QStringLiteral("media");
        if (x.r.kind != QLatin1String("file")) {   // mp://<source> and URL lines are live sources (design §6.9)
            const QString as = it.entry.path.trimmed();
            if (x.r.kind == QLatin1String("live")) liveLine(s, W, x, it, w, t, as.mid(as.indexOf(QLatin1String("://")) + 3).trimmed(), {});
            else liveLine(s, W, x, it, w, t, {}, as);
            return;
        }
        double dur = plannedDuration(x.r, it.entry, o.timeline.parse.fps);
        if (s.inPod && o.breaks == QLatin1String("skip")) drop(x, QStringLiteral("breakSkipped"));
        else if (!x.r.resolved() && o.missing == QLatin1String("drop")) drop(x, QStringLiteral("missing"));
        else if (dur <= 0) drop(x, QStringLiteral("noDuration"));
        if (x.pub.state == QLatin1String("dropped")) {
            keep(s, W, x, it, w);
            return;
        }
        x.pub.durationSec = dur;
        x.item = makeMediaItem(x, it, w, t, &x.mp);
        const QString actRaw = it.entry.value(QStringLiteral("actinout")).trimmed();
        if (!actRaw.isEmpty()) {
            bool ok = false;
            const int act = actRaw.toInt(&ok);
            if (ok && act >= 0 && !actLine(s, W, x, it, w, act, &dur)) return;
        }
        placeTimed(s, W, x, it, w, dur);
    }

    /// The live source a name stands for (design §6.9): an inputMap entry (a path / URL string, "inN" from the store's
    /// ui.inputMap slots, or a liveSource object), else the name itself when it is a URL / ndi:// path or "inN".
    LiveTarget liveTarget(const QString& name0, const QString& url) const {
        LiveTarget t;
        if (!url.isEmpty()) {
            t.ok = true;
            t.path = url;
            t.liveSource = QJsonObject{{"url", url}};
            return t;
        }
        const QString name = name0.trimmed();
        t.id = name;
        if (name.isEmpty()) { t.why = QStringLiteral("no source is named (set networkInput)"); return t; }
        QJsonValue v;
        bool mapped = false;
        for (auto it = o.inputMap.constBegin(); it != o.inputMap.constEnd(); ++it)
            if (it.key().trimmed().compare(name, Qt::CaseInsensitive) == 0) { v = it.value(); mapped = true; break; }
        if (!mapped && (name.contains(QLatin1String("://")) || QRegularExpression(QStringLiteral("^in\\d+$"), QRegularExpression::CaseInsensitiveOption).match(name).hasMatch()))
            v = name;
        if (v.isObject()) {
            t.liveSource = v.toObject();
            t.path = t.liveSource.value("url").toString();
            t.ok = !t.liveSource.isEmpty();
            if (!t.ok) t.why = QStringLiteral("its inputMap entry is empty");
            return t;
        }
        const QString s = v.toString().trimmed();
        if (s.isEmpty()) { t.why = QStringLiteral("it has no inputMap entry"); return t; }
        if (s.contains(QLatin1String("://"))) {
            t.ok = true;
            t.path = s;
            if (!s.startsWith(QLatin1String("ndi://"), Qt::CaseInsensitive)) t.liveSource = QJsonObject{{"url", s}};
            return t;
        }
        const QRegularExpressionMatch m = QRegularExpression(QStringLiteral("^in(\\d+)$"), QRegularExpression::CaseInsensitiveOption).match(s);
        if (m.hasMatch()) {
            const int n = m.captured(1).toInt();
            const QJsonArray inputSlots = store ? store->settingJson(QStringLiteral("ui.inputMap")).value("slots").toArray() : QJsonArray();
            const QJsonObject slot = n >= 1 && n <= inputSlots.size() ? inputSlots[n - 1].toObject() : QJsonObject();
            t.path = slot.value("enginePath").toString(slot.value("path").toString());
            t.ok = !t.path.isEmpty();
            if (!t.ok) t.why = QStringLiteral("%1 is not a configured input (ui.inputMap)").arg(s);
            return t;
        }
        t.why = QStringLiteral("its inputMap entry \"%1\" is not a URL, an ndi:// path, inN or a liveSource object").arg(s);
        return t;
    }

    /// A live item: an mp:// or URL media line, !Video, !LiveStream. Its length is the line's own (bracket, LengthStr),
    /// else the time left until the next event.
    void liveLine(Lay& s, Win& W, Pl& x, const tl::Item& it, const tl::Window& w, const tl::DayTimeline& t, const QString& source, const QString& url) {
        x.pub.kind = QStringLiteral("live");
        if (s.inPod && o.breaks == QLatin1String("skip")) {
            drop(x, QStringLiteral("breakSkipped"));
            keep(s, W, x, it, w);
            return;
        }
        const LiveTarget lt = liveTarget(source, url);
        if (!lt.ok) {
            drop(x, QStringLiteral("liveUnmapped"));
            x.diags << PendingNote{QStringLiteral("MP006"), 1, QStringLiteral("live source '%1' is not mapped: %2").arg(source, lt.why)};
            keep(s, W, x, it, w);
            return;
        }
        if (s.virt && s.cursor < s.fromMs) s.cursor = s.fromMs;   // a live source has no in point: it starts with the window
        double dur = it.entry.kind == LineKind::Media ? lengthOf(it.entry, o.timeline.parse.fps) : -1;
        const bool untilNext = dur <= 0;
        if (untilNext) dur = msToSec(s.endMs - s.cursor);
        if (dur <= 0) {
            drop(x, QStringLiteral("noTime"));
            keep(s, W, x, it, w);
            return;
        }
        x.item.id = idOf(x, it, w, t);
        x.item.type = QStringLiteral("live");
        x.item.path = lt.path;
        x.item.liveSource = lt.liveSource;
        x.item.liveSourceId = lt.id;
        x.item.durationSec = dur;
        const QString name = it.entry.value(QStringLiteral("Name")).trimmed();
        x.item.title = !name.isEmpty() ? name : !source.isEmpty() ? source : url;
        x.mp = mpBase(x, it, w, t);
        QJsonObject live{{"path", lt.path}};
        if (!source.isEmpty()) live["source"] = source;
        if (untilNext) live["untilNextEvent"] = true;
        x.mp["live"] = live;
        placeTimed(s, W, x, it, w, dur);
    }

    /// !InsertDirectory / !InsertTopinDirectory as folderBlock picks (design §6.4; the header comment's table).
    void folderLine(Lay& s, Win& W, Pl& x, const tl::Item& it, const tl::Window& w, const tl::DayTimeline& t) {
        x.pub.kind = QStringLiteral("folder");
        const std::optional<InsertDirectorySpec> spec = insertDirectorySpec(it.entry, o.timeline.parse);
        if (!spec) {   // (identify() said it is one; never expected)
            drop(x, QStringLiteral("unsupported"));
            keep(s, W, x, it, w);
            return;
        }
        x.fc = rv.folder(*spec);
        x.hasFolder = x.folderLine = true;
        const LoadingMode m = spec->loadingMode;
        if (s.inPod && o.breaks == QLatin1String("skip")) drop(x, QStringLiteral("breakSkipped"));
        else if (spec->searchTerm || x.fc.searchTerm) {
            drop(x, QStringLiteral("searchTerm"));
            x.diags << PendingNote{QStringLiteral("MP020"), 1,
                                   QStringLiteral("%1 holds a %search term%, which PlayPlay folder blocks cannot match: the folder is not imported").arg(spec->directory)};
        } else if (m == LoadingMode::SelectEventNo) {
            drop(x, QStringLiteral("unsupported"));
            x.diags << PendingNote{QStringLiteral("MP022"), 1, QStringLiteral("LoadingMode=8 (select by event number) has no PlayPlay form: the folder is not imported")};
        } else if (x.fc.empty()) {
            drop(x, QStringLiteral("emptyFolder"));   // MP019 and the scan / watch actions come from the folder report
        }
        if (x.pub.state == QLatin1String("dropped")) {
            keep(s, W, x, it, w);
            return;
        }
        QStringList approx;
        if (spec->subDirs == 2) approx << QStringLiteral("SubDirs=2 (one file per subfolder) draws from the whole tree");
        if (m == LoadingMode::Episode || m == LoadingMode::EpisodeWeekly) approx << QStringLiteral("LoadingMode=%1 (episodes) picks by name").arg(int(m));
        if (m == LoadingMode::PictureSequence)
            approx << QStringLiteral("LoadingMode=6 (picture sequence) plays the folder by name%1")
                          .arg(spec->stillSec > 0 ? QStringLiteral(" at each still's own length, not DefPTime") : QString());
        if (spec->invertOrder) approx << QStringLiteral("InvertOrder=1 is not applied");
        for (const Param& pa : spec->unmodelled) approx << QStringLiteral("%1 is not applied").arg(pa.text());
        QJsonArray rules;
        const QString mask = x.fc.mask;
        if (!mask.isEmpty()) {   // Name*.ext: the parts before the first and after the last '*' (a '?' ends them)
            const int a = int(mask.indexOf(QLatin1Char('*'))), b = int(mask.lastIndexOf(QLatin1Char('*')));
            QString prefix = a >= 0 ? mask.left(a) : mask, suffix = b >= 0 ? mask.mid(b + 1) : QString();
            if (prefix.contains(QLatin1Char('?'))) prefix = prefix.left(prefix.indexOf(QLatin1Char('?')));
            if (suffix.contains(QLatin1Char('?'))) suffix = suffix.mid(suffix.lastIndexOf(QLatin1Char('?')) + 1);
            if (a < 0 && !mask.contains(QLatin1Char('?'))) suffix.clear();   // (an exact name: starts_with alone)
            if (!prefix.isEmpty()) rules.append(QJsonObject{{"field", "filename"}, {"op", "starts_with"}, {"value", prefix}});
            if (!suffix.isEmpty()) rules.append(QJsonObject{{"field", "filename"}, {"op", "ends_with"}, {"value", suffix}});
            if (mask.contains(QLatin1Char('?')) || a != b) approx << QStringLiteral("mask %1 is matched by its start and end only").arg(mask);
        }
        if (x.fc.maxAgeHours > 0 && x.fc.fresh == 0)
            approx << QStringLiteral("nothing in it is newer than MaxAge=%1 h today: the block finds nothing until newer files arrive").arg(x.fc.maxAgeHours);
        if (!approx.isEmpty())
            x.diags << PendingNote{QStringLiteral("MP021"), 1, QStringLiteral("%1 approximated: %2").arg(spec->directory, approx.join(QStringLiteral("; ")))};
        int n = 1;
        double target = 0, each = x.fc.avgSec;
        QString order = QStringLiteral("random"), weight = QStringLiteral("freshness");
        switch (m) {
        case LoadingMode::Normal: target = each = x.fc.durationSec; order = QStringLiteral("name"); break;
        case LoadingMode::Random: target = each = x.fc.durationSec; weight = QStringLiteral("uniform"); break;
        case LoadingMode::SelectNormal: n = qMax(1, spec->toSelect); order = QStringLiteral("name"); break;
        case LoadingMode::SelectRandom: n = qMax(1, spec->toSelect); order = o.selectRandomOrder == QLatin1String("even") ? QStringLiteral("even") : QStringLiteral("random"); break;
        case LoadingMode::Episode: case LoadingMode::EpisodeWeekly: order = QStringLiteral("name"); break;
        case LoadingMode::PictureSequence: target = each = x.fc.durationSec; order = QStringLiteral("name"); break;
        case LoadingMode::UntilNextEvent:
            if (s.virt && s.cursor < s.fromMs) s.cursor = s.fromMs;
            target = each = msToSec(s.endMs - s.cursor);
            break;
        case LoadingMode::SelectEventNo: break;
        }
        if (each <= 0) {
            drop(x, QStringLiteral("noTime"));
            keep(s, W, x, it, w);
            return;
        }
        QJsonObject fb{{"folder", x.fc.mapped}, {"targetSec", target}, {"weight", weight}, {"order", order}, {"recursive", spec->recursive()}};
        if (x.fc.maxAgeHours > 0) fb["maxAgeDays"] = x.fc.maxAgeHours / 24.0;
        if (!rules.isEmpty()) fb["rules"] = rules;
        const QString folderName = res::fileName(x.fc.folder.isEmpty() ? x.fc.mapped : x.fc.folder);
        const QString title = QString::fromUtf8("\xF0\x9F\x93\x81 ") + folderName + (n > 1 ? QString::fromUtf8(" \xC3\x97%1").arg(n) : QString());
        const QString id0 = idOf(x, it, w, t);
        for (int k = 0; k < n; ++k) {
            Pl y = x;
            y.pub.pick = k;
            if (k > 0) {
                y.diags.clear();
                y.folderLine = false;
                y.noProvenance = true;
            }
            y.item = sched::SlotItem();
            y.item.id = k == 0 ? id0 : id0 + QStringLiteral("~p%1").arg(k);
            y.item.title = title;
            y.item.folderBlock = fb;
            y.item.durationSec = each;
            y.mp = mpBase(y, it, w, t);
            y.mp["folder"] = QJsonObject{{"loadingMode", loadingModeName(m)}, {"picks", n}, {"pick", k}, {"playable", x.fc.playable},
                                         {"avgSec", x.fc.avgSec}, {"folderSec", x.fc.durationSec}};
            placeTimed(s, W, y, it, w, each);
        }
    }

    /// !Break opens a pod (design §6.6).
    void breakLine(Lay& s, Win& W, Pl& x, const tl::Item& it, const tl::Window& w) {
        x.pub.kind = QStringLiteral("break");
        x.pub.state = QStringLiteral("mapped");
        const std::optional<BreakSpec> b = breakSpec(it.entry, o.timeline.parse);
        W.hasPods = true;
        ++s.podOrdinal;
        s.pod = b && b->number >= 0 ? b->number : s.podOrdinal;
        s.inPod = true;
        s.podAnchor = -1;
        x.pod = s.pod;
        if (o.breaks == QLatin1String("asScheduled") && b && b->schedSec >= 0) {
            if (s.copy > 0) {
                // A loop repeats the pod: its schedtime is the first pass's (the copies follow the content).
            } else {
                qint64 a = s.evDayMs + qint64(b->schedSec) * 1000;
                if (a < s.evStartMs - 12 * kHourMs) a += kDayMs;   // far before a late event's start: the next day's (00:10 after 23:30)
                const qint64 floor = s.virt ? s.fromMs : (W.started ? W.startMs : s.anchorMs);
                if (a < floor)
                    x.diags << PendingNote{QStringLiteral("MP025"), 1,
                                           QStringLiteral("schedtime=%1 is before the event's content starts: pod %2 follows the content").arg(b->schedTime).arg(s.pod)};
                else s.podAnchor = a;
            }
        }
        keep(s, W, x, it, w);
    }

    /// What an OSD line does (design §6.8; the header comment). Placement is the kind's default
    /// (osd::OsdConfig::placement: the .osd XPos / YPos units are not settled, design open question 4), so the cfg= file
    /// is recorded, not read.
    Osd osdOf(const Instruction& ins, const tl::Item& it) {
        Osd r;
        const Entry& e = it.entry;
        const ParseOptions& po = o.timeline.parse;
        if (ins.id == Instr::OsdOff) { r.op = 2; return r; }
        auto placement = [](int type) { osd::OsdConfig c; c.type = type; c.kind = osd::kindOfType(type); QJsonObject p = c.placement(); p.remove("fromOsd"); return p; };
        auto readText = [&](const QString& file, QString* text) {
            const QString k = file.toCaseFolded() + QLatin1Char('|') + it.file.toCaseFolded();
            auto f = fileCache.constFind(k);
            if (f == fileCache.cend()) {
                tl::ReadResult rr;
                if (read) rr = read(file, it.file);
                else rr.why = QStringLiteral("no reader");
                f = fileCache.insert(k, rr);
            }
            if (!f->ok) {
                r.diags << PendingNote{QStringLiteral("MP006"), 1, QStringLiteral("%1 was not read (%2): the layer is not shown").arg(file, f->why)};
                r.keptReason = QStringLiteral("osdUnread");
                return false;
            }
            *text = snapshotText(f->bytes);
            r.diags << PendingNote{QStringLiteral("MP028"), 1, QStringLiteral("the text of %1 is a snapshot read at import (re-import to update it)").arg(file)};
            return true;
        };
        OsdCommon common;
        switch (ins.id) {
        case Instr::ShowLogo: {
            const ShowLogoSpec sp = showLogoSpec(e, po).value_or(ShowLogoSpec());
            common = sp;
            r.kind = QStringLiteral("logo");
            r.role = sp.layer <= 1 ? QStringLiteral("bug") : QStringLiteral("secondary");
            if (sp.file.trimmed().isEmpty()) {
                r.diags << PendingNote{QStringLiteral("MP006"), 1, QStringLiteral("!ShowLogo names no image (f=): the layer is not shown")};
                r.keptReason = QStringLiteral("osdUnread");
                return r;
            }
            r.filePath = rv.mapPath(sp.file);
            const QJsonObject pl = placement(1);
            r.cue = QJsonObject{{"filePath", r.filePath}, {"role", r.role}, {"params", QJsonObject{{"placement", pl}}}, {"startOffset", 0}};
            r.hookShow = compact(QJsonObject{{"action", "show"}, {"role", r.role}, {"source", QJsonObject{{"type", "image"}, {"path", r.filePath}}},
                                             {"placement", pl}, {"in", QJsonObject{{"type", "fade"}, {"durationSec", 0.3}}}});
            break;
        }
        case Instr::ShowCrawl: case Instr::ShowCrawlFromFile: {
            const ShowCrawlSpec sp = showCrawlSpec(e, po).value_or(ShowCrawlSpec());
            common = sp;
            r.kind = QStringLiteral("crawl");
            r.role = QStringLiteral("ticker");
            if (sp.fromFile) { if (!readText(sp.file, &r.text)) return r; }
            else r.text = sp.lines().join(QStringLiteral("   ")).simplified();
            r.cue = QJsonObject{{"template", "ticker"}, {"role", r.role}, {"params", QJsonObject{{"text", r.text}}}, {"startOffset", 0}};
            r.hookShow = compact(QJsonObject{{"action", "show"}, {"role", r.role}, {"source", QJsonObject{{"type", "template"}, {"kind", "ticker"}, {"text", r.text}}}});
            break;
        }
        case Instr::ShowText: case Instr::ShowTextFromFile: {
            const ShowTextSpec sp = showTextSpec(e, po).value_or(ShowTextSpec());
            common = sp;
            r.kind = QStringLiteral("text");
            r.role = QStringLiteral("lower_third");
            if (sp.fromFile) { if (!readText(sp.file, &r.text)) return r; }
            else r.text = sp.text.simplified();
            r.cue = QJsonObject{{"template", "lower_third"}, {"role", r.role}, {"params", QJsonObject{{"title", r.text}}}, {"startOffset", 0}};
            r.hookShow = compact(QJsonObject{{"action", "show"}, {"role", r.role}, {"source", QJsonObject{{"type", "template"}, {"kind", "lower_third"}, {"title", r.text}}}});
            break;
        }
        case Instr::ShowClock: case Instr::RealTimeClock: {
            const ClockSpec sp = clockSpec(e, po).value_or(ClockSpec());
            common = sp;
            r.kind = QStringLiteral("clock");
            r.role = QStringLiteral("clock");
            r.hookShow = r.hookHide = QStringLiteral("clock");   // the engine's identity clock (identityCg "clock", role legal_id)
            r.diags << PendingNote{QStringLiteral("MP006"), 1,
                                   QStringLiteral("the clock is the channel's identity clock (cgOnStart \"clock\"): the format and position of %1 are not carried")
                                       .arg(sp.cfg.isEmpty() ? QStringLiteral("the line") : sp.cfg)};
            break;
        }
        default:
            return r;
        }
        r.op = 1;
        r.id = QStringLiteral("mp-%1-%2").arg(r.kind, shortHash(it.file.toCaseFolded() + QLatin1Char('#') + QString::number(e.line) + QLatin1Char('|') + e.raw));
        if (!r.cue.isEmpty()) r.cue["id"] = r.id;
        if (r.hookHide.isEmpty()) r.hookHide = compact(QJsonObject{{"action", "hide"}, {"role", r.role}});
        if (!common.timing.show.isEmpty() || !common.timing.hide.isEmpty() || common.timing.hasShowValue || common.timing.hasHideValue)
            r.diags << PendingNote{QStringLiteral("MP006"), 1, QStringLiteral("show= / hide= timing is not mapped: the layer is up for its whole span")};
        return r;
    }

    void osdLine(Lay& s, Win& W, Pl& x, const tl::Item& it, const tl::Window& w, const Instruction& ins) {
        x.pub.kind = QStringLiteral("osd");
        if (o.osd == QLatin1String("off")) {
            x.pub.state = QStringLiteral("kept");
            x.pub.reason = QStringLiteral("osdOff");
            keep(s, W, x, it, w);
            return;
        }
        const QString key = it.file.toCaseFolded() + QLatin1Char('#') + QString::number(it.entry.line);
        auto c = osdCache.constFind(key);
        if (c == osdCache.cend()) c = osdCache.insert(key, osdOf(ins, it));
        x.osd = c.value();
        x.diags << x.osd.diags;
        if (x.osd.op == 0) {
            x.pub.state = QStringLiteral("kept");
            x.pub.reason = x.osd.keptReason.isEmpty() ? QStringLiteral("unsupported") : x.osd.keptReason;
        } else {
            x.pub.state = QStringLiteral("mapped");
        }
        keep(s, W, x, it, w);
    }

    void unsupportedLine(Lay& s, Win& W, Pl& x, const tl::Item& it, const tl::Window& w, const Instruction& ins) {
        x.pub.kind = QStringLiteral("unsupported");
        x.pub.state = QStringLiteral("kept");
        x.pub.reason = QStringLiteral("unsupported");
        x.timed = isTimed(ins);
        x.why = unsupportedWhy(ins);
        if (ins.known())   // (an unknown name is the parser's MP011)
            x.diags << PendingNote{QStringLiteral("MP022"), 1, QStringLiteral("!%1: %2; kept in extra.masterplay.instructions").arg(ins.name, x.why)};
        keep(s, W, x, it, w);
    }

    void instructionLine(Lay& s, Win& W, Pl& x, const tl::Item& it, const tl::Window& w, const tl::DayTimeline& t) {
        const Instruction ins = identify(it.entry);
        x.pub.name = ins.name;
        if (isStructural(it.instr)) {
            x.pub.kind = QStringLiteral("structural");
            x.pub.state = QStringLiteral("consumed");
            keep(s, W, x, it, w);
            return;
        }
        if (isOsd(ins.id)) { osdLine(s, W, x, it, w, ins); return; }
        switch (ins.id) {
        case Instr::InsertDirectory: case Instr::InsertTopinDirectory:
            folderLine(s, W, x, it, w, t);
            return;
        case Instr::Break:
            breakLine(s, W, x, it, w);
            return;
        case Instr::Video: {
            const std::optional<VideoSpec> v = videoSpec(it.entry);
            if (v && v->setOnly) break;
            liveLine(s, W, x, it, w, t, v && !v->source.trimmed().isEmpty() ? v->source.trimmed() : o.networkInput, {});
            return;
        }
        case Instr::LiveStream: {
            const std::optional<LiveStreamSpec> l = liveStreamSpec(it.entry);
            if (!l || l->path.trimmed().isEmpty()) break;
            liveLine(s, W, x, it, w, t, {}, l->path.trimmed());
            return;
        }
        case Instr::Info: {
            const std::optional<InfoSpec> in = infoSpec(it.entry, o.timeline.parse);
            x.pub.kind = QStringLiteral("info");
            x.pub.state = QStringLiteral("mapped");
            x.infoText = in ? (!in->text.trimmed().isEmpty() ? in->text.trimmed() : in->name.trimmed()) : QString();
            keep(s, W, x, it, w);
            return;
        }
        default:
            break;
        }
        unsupportedLine(s, W, x, it, w, ins);
    }

    void line(Lay& s, Win& W, const tl::DayTimeline& t, const tl::Window& w, int i) {
        const tl::Item& it = w.items[i];
        Pl x;
        x.pub.day = s.k;
        x.pub.window = s.wi;
        x.pub.item = i;
        x.pub.file = it.file;
        x.pub.line = it.entry.line;
        x.pub.copy = s.copy;
        x.copy = s.copy;
        x.atMs = s.cursor;
        x.eventSrc = s.evSrc;
        x.winIdx = s.winIdx;
        if (it.entry.kind != LineKind::Media) instructionLine(s, W, x, it, w, t);
        else mediaLine(s, W, x, it, w, t);
    }

    /// The loop's first item (the header comment): the window's own loop, or after a followed !ChangePlaylist the
    /// target's; a loop the change line sits in never repeats (the change happens on its first pass).
    static int loopFromOf(const tl::Window& w) {
        if (w.cutAt >= 0) {
            const tl::Item& ch = w.items[w.cutAt];
            return ch.status == QLatin1String("followed") && ch.targetLoopFrom > w.cutAt ? ch.targetLoopFrom : -1;
        }
        return w.loopFrom;
    }

    static void newPass(Lay& s) {
        s.inPod = false;
        s.pod = -1;
        s.podOrdinal = 0;
        s.podAnchor = -1;
        s.actFirst.clear();
    }

    /// Unrolls a window's loop until the next event (the header comment's Repeat rules).
    void unroll(Lay& s, Win& W, const tl::DayTimeline& t, const tl::Window& w, int L, const QList<bool>& timed, qint64 passMs) {
        const int n = int(w.items.size());
        W.looped = true;
        if (passMs <= 0) {
            W.diags << PendingNote{QStringLiteral("MP016"), 1, QStringLiteral("the event loops (!Repeat), but nothing in its loop takes air time: it is placed once")};
            return;
        }
        const bool hours = o.repeat == QLatin1String("loop") && passMs <= kHourMs;
        if (s.virt && s.cursor < s.fromMs) s.cursor = s.fromMs;   // a carry restarts the loop at its start
        s.curHour = floorDiv(s.cursor - 1, kHourMs);
        s.loopHour = false;
        s.copy = 1;
        newPass(s);
        int i = L, lines = 0;
        bool advanced = false;
        while (s.cursor < s.endMs) {
            if (i >= n) {   // a pass ended
                if (!advanced) break;
                if (s.loopHour) {   // the hour's runlist replays the pass to its end
                    const qint64 he = (s.curHour + 1) * kHourMs;
                    if (s.cursor < he) { loopFill[s.curHour] += he - s.cursor; s.cursor = he; }
                }
                i = L;
                ++s.copy;
                newPass(s);
                advanced = false;
                continue;
            }
            if (lines >= o.maxUnrollItems) {
                const qint64 d0 = floorDiv(s.cursor, kDayMs) * kDayMs;
                p.notes << QStringLiteral("the loop of %1 line %2 (%3) stops after %4 unrolled lines at %5 (maxUnrollItems): the rest of its window is left "
                                          "to the fill policy")
                               .arg(w.file.isEmpty() ? QStringLiteral("the playlist") : w.file)
                               .arg(w.line)
                               .arg(t.day.label())
                               .arg(lines)
                               .arg(clock(s.cursor - d0));
                break;
            }
            if (hours && timed[i]) {
                const qint64 H = floorDiv(s.cursor, kHourMs);
                if (H != s.curHour) {   // the first line that starts in a new hour
                    s.curHour = H;
                    s.loopHour = (H + 1) * kHourMs <= s.endMs;
                    if (s.loopHour) {
                        loopHours.insert(H);
                        if (i != L) {   // its runlist starts the loop over
                            i = L;
                            ++s.copy;
                            newPass(s);
                            advanced = false;
                            continue;
                        }
                    }
                }
            }
            const qint64 c0 = s.cursor;
            line(s, W, t, w, i);
            ++lines;
            ++i;
            if (s.cursor > c0) advanced = true;
        }
        s.loopHour = false;
        W.unrolled = lines;
    }

    /// breaks "live": a break-mode event airs the network input for its whole window; its lines are listed, not placed.
    void layBreakLive(Lay& s, Win& W, const tl::DayTimeline& t, const tl::Window& w) {
        W.breakLive = true;
        for (int i = 0; i < w.items.size(); ++i) {
            const tl::Item& it = w.items[i];
            Pl x;
            x.pub.day = s.k;
            x.pub.window = s.wi;
            x.pub.item = i;
            x.pub.file = it.file;
            x.pub.line = it.entry.line;
            x.atMs = s.cursor;
            x.eventSrc = s.evSrc;
            x.winIdx = s.winIdx;
            if (it.entry.kind == LineKind::Media) x.pub.kind = QStringLiteral("media");
            else {
                const Instruction ins = identify(it.entry);
                x.pub.name = ins.name;
                x.pub.kind = ins.id == Instr::Break ? QStringLiteral("break") : isStructural(it.instr) ? QStringLiteral("structural") : QStringLiteral("unsupported");
            }
            x.pub.state = QStringLiteral("kept");
            x.pub.reason = QStringLiteral("breakLive");
            if (x.pub.kind == QLatin1String("break")) {   // listed in the pod report, not placed
                W.hasPods = true;
                const std::optional<BreakSpec> b = breakSpec(it.entry, o.timeline.parse);
                ++s.podOrdinal;
                x.pod = b && b->number >= 0 ? b->number : s.podOrdinal;
            }
            keep(s, W, x, it, w);
        }
        tl::Item h;   // the event header stands for the live item
        h.file = w.file;
        h.entry.line = w.line;
        h.entry.raw = w.stamp.raw;
        h.entry.kind = LineKind::Header;
        Pl x;
        x.pub.day = s.k;
        x.pub.window = s.wi;
        x.pub.item = -1;
        x.pub.file = w.file;
        x.pub.line = w.line;
        x.pub.name = QStringLiteral("breakLive");
        x.atMs = s.cursor;
        x.eventSrc = s.evSrc;
        x.winIdx = s.winIdx;
        x.noProvenance = true;
        liveLine(s, W, x, h, w, t, o.networkInput, {});
    }

    /// Lays one window's content from baseMs; nothing airs before fromMs (a carry window laid from its event's start joins
    /// the item playing at fromMs in progress). anchorMs / eventType describe the event start (not for a carry).
    void layWindow(int k, int wi, qint64 baseMs, qint64 fromMs, bool virt, qint64 anchorMs, int eventType) {
        const tl::DayTimeline& t = seq[k];
        const tl::Window& w = t.windows[wi];
        Win W;
        W.day = k;
        W.win = wi;
        W.virt = virt;
        W.anchorMs = virt ? -1 : anchorMs;
        W.startMs = fromMs;
        W.endMs = nextStartMs(k, wi);
        Lay s;
        s.k = k;
        s.wi = wi;
        s.winIdx = int(wins.size());
        s.virt = virt;
        s.anchorMs = anchorMs;
        s.eventType = eventType;
        s.cursor = baseMs;
        s.fromMs = fromMs;
        s.endMs = W.endMs;
        s.evStartMs = baseMs;
        s.evDayMs = baseMs - qint64(w.eventSec) * 1000;
        s.evSrc = w.file + QLatin1Char('#') + QString::number(w.line);
        const int n = int(w.items.size());
        if (w.playMode == 4 && o.breaks == QLatin1String("live")) {
            layBreakLive(s, W, t, w);
        } else {
            const int L = loopFromOf(w);
            QList<bool> timed(n, false);
            qint64 passMs = 0;
            for (int i = 0; i < n; ++i) {
                const int before = int(pls.size());
                line(s, W, t, w, i);
                for (int j = before; j < pls.size(); ++j) {
                    if (pls[j].contentMs <= 0) continue;
                    timed[i] = true;
                    if (L >= 0 && i >= L) passMs += pls[j].contentMs;
                }
            }
            if (L >= 0 && L < n) unroll(s, W, t, w, L, timed, passMs);
        }
        if (W.hasPods || w.playMode == 4) {
            const QString how = o.breaks == QLatin1String("live")
                                    ? (w.playMode == 4 ? QStringLiteral("the event airs networkInput '%1' for its whole window; its pods are listed, not placed").arg(o.networkInput)
                                                       : QStringLiteral("this event is not in break mode, so its pods are imported as scheduled items"))
                                : o.breaks == QLatin1String("skip") ? QStringLiteral("its pods are skipped")
                                                                    : QStringLiteral("its pods are imported as scheduled items (at schedtime= when given)");
            W.diags << PendingNote{QStringLiteral("MP025"), 1, QStringLiteral("break mode needs GPI, which PlayPlay has no input for: %1").arg(how)};
        }
        if (!s.pending.isEmpty()) {
            if (s.lastPlaced >= 0) {
                pls[s.lastPlaced].after = s.pending;
                pls[s.lastPlaced].afterIdx = s.pendingIdx;
            } else {
                W.unplaced = s.pending;
                W.unplacedIdx = s.pendingIdx;
            }
        }
        winIndex[k][wi] = int(wins.size());
        wins << W;
    }

    void layAll() {
        winIndex.clear();
        for (const tl::DayTimeline& t : seq) winIndex << QList<int>(t.windows.size(), -1);
        for (int k = 0; k < seq.size(); ++k) {
            const qint64 day0 = k * kDayMs;
            const QList<tl::Window>& ws = seq[k].windows;
            for (int wi = 0; wi < ws.size(); ++wi) {
                const tl::Window& w = ws[wi];
                if (w.carry) {
                    if (k > 0 && !seq[k - 1].windows.isEmpty() && sameEvent(seq[k - 1].windows.last(), w)) {
                        Win W;   // the previous day's last window goes on: nothing starts
                        W.day = k;
                        W.win = wi;
                        W.merged = true;
                        W.startMs = day0;
                        W.endMs = nextStartMs(k, wi);
                        winIndex[k][wi] = int(wins.size());
                        wins << W;
                        continue;
                    }
                    if (k > 0 && !laid.isEmpty() && pls[laid.last()].pub.endMs > day0) {
                        bool capped = false;
                        trimAt(day0, 0, &capped);
                        if (k >= outFirst)   // (the grid's first week only warms the layout up)
                            p.notes << QStringLiteral("%1 opens with the event of %2 line %3 (%4 day(s) earlier), not the day before's last event: "
                                                      "what was still playing is cut at midnight")
                                           .arg(seq[k].day.label(), w.file.isEmpty() ? QStringLiteral("the playlist") : w.file)
                                           .arg(w.line)
                                           .arg(w.carryDays);
                    }
                    layWindow(k, wi, day0 - w.offsetSec * 1000, day0, true, -1, 0);
                } else {
                    const qint64 anchor = day0 + qint64(w.startSec) * 1000;
                    layWindow(k, wi, anchor, anchor, false, anchor, w.eventType);
                    if (!wins.last().started && !wins.last().breakLive && anchor >= outStart && anchor < seqEnd) {
                        const qint64 at = laid.isEmpty() ? -1 : pls[laid.last()].pub.endMs;
                        note(QStringLiteral("MP016"), w.file, w.line, 1,
                             QStringLiteral("nothing of the event is placed (every line dropped, kept or consumed): it writes no start at "
                                            "%1, so %2")
                                 .arg(clock(anchor - day0),
                                      at > anchor ? QStringLiteral("what is playing then goes on (nothing cuts it)")
                                                  : QStringLiteral("the time until the next event is left to the fill policy")),
                             seq[k].day.label());
                    }
                }
            }
        }
        // Past the last day: the grid's second week wraps onto its own start (the first laid week stands for it), a date
        // range ends.
        for (int j = int(laid.size()) - 1; j >= 0; --j) {
            Pl& q = pls[laid[j]];
            if (q.pub.startMs < seqEnd) break;
            drop(q, grid ? QStringLiteral("wrap") : QStringLiteral("afterRange"));
            laid.removeAt(j);
        }
    }

    // ---- OSD spans ----------------------------------------------------------------------------------------------------

    struct Span {
        int pl = -1;                   // the OSD line that opened it
        QList<int> items;              // laid items it covers, in time order
        QString end;                   // osdOff | replaced | end
    };

    /// Walks the laid items in time order with the OSD lines at the times they run (before an item: its start; after a
    /// window's last item: its end; an event with nothing placed: its start) and puts every open layer on the items of its
    /// span: item cues, or hooks on the span's first item (and its first item in every later hour) and its last.
    QList<Span> applyOsd() {
        struct Ev { qint64 at; int pl; };
        QList<Ev> evs;
        auto addEvs = [&](const QList<int>& idx, qint64 at) {
            for (int j : idx)
                if (pls[j].pub.kind == QLatin1String("osd") && pls[j].pub.state == QLatin1String("mapped") && pls[j].osd.op != 0) evs << Ev{at, j};
        };
        for (int idx : std::as_const(laid)) {
            addEvs(pls[idx].beforeIdx, pls[idx].pub.startMs);
            addEvs(pls[idx].afterIdx, pls[idx].pub.endMs);
        }
        for (const Win& W : std::as_const(wins))
            if (!W.unplacedIdx.isEmpty()) addEvs(W.unplacedIdx, W.anchorMs >= 0 ? W.anchorMs : W.startMs);
        std::stable_sort(evs.begin(), evs.end(), [](const Ev& a, const Ev& b) { return a.at != b.at ? a.at < b.at : a.pl < b.pl; });
        QMap<QString, Span> open;
        QList<Span> done;
        auto close = [&](const QString& role, const QString& why) {
            auto it = open.find(role);
            if (it == open.end()) return;
            it->end = why;
            done << *it;
            open.erase(it);
        };
        auto apply = [&](int pl) {
            const Osd& d = pls[pl].osd;
            if (d.op == 2) {
                for (const QString& role : open.keys()) close(role, QStringLiteral("osdOff"));
                return;
            }
            close(d.role, QStringLiteral("replaced"));
            Span sp;
            sp.pl = pl;
            open.insert(d.role, sp);
        };
        int e = 0;
        for (int idx : std::as_const(laid)) {
            while (e < evs.size() && evs[e].at <= pls[idx].pub.startMs) apply(evs[e++].pl);
            for (auto it = open.begin(); it != open.end(); ++it) it->items << idx;
        }
        while (e < evs.size()) apply(evs[e++].pl);
        for (const QString& role : open.keys()) close(role, QStringLiteral("end"));
        const bool hookMode = o.osd == QLatin1String("hooks");
        for (const Span& sp : std::as_const(done)) {
            if (sp.items.isEmpty()) continue;
            const Osd& d = pls[sp.pl].osd;
            const bool hook = d.kind == QLatin1String("clock") || hookMode;
            qint64 lastHour = LLONG_MIN;
            for (int idx : sp.items) {
                Pl& q = pls[idx];
                if (!hook) { q.cues.append(d.cue); continue; }
                const qint64 h = floorDiv(q.pub.startMs, kHourMs);
                if (h != lastHour) q.onStart << d.hookShow;   // the span's first item, and its first in every later hour
                lastHour = h;
            }
            if (hook) pls[sp.items.last()].onEnd << d.hookHide;
        }
        return done;
    }
};

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Options

Options Options::fromJson(const QJsonObject& o, QStringList* errors) {
    Options r;
    auto bad = [&](const QString& m) { if (errors) *errors << m; };
    r.channelId = o.value(QStringLiteral("channel")).toString(o.value(QStringLiteral("channelId")).toString()).trimmed();
    if (o.contains(QStringLiteral("mode"))) {
        QString m = o.value(QStringLiteral("mode")).toString().trimmed();
        for (const char* a : {"week", "weekly", "template"}) if (m.compare(QLatin1String(a), Qt::CaseInsensitive) == 0) m = QStringLiteral("grid");
        for (const char* a : {"date", "override", "overrides"}) if (m.compare(QLatin1String(a), Qt::CaseInsensitive) == 0) m = QStringLiteral("dates");
        const QString c = canon(m, kModes);
        if (c.isEmpty()) bad(QStringLiteral("mode \"%1\" is not grid or dates").arg(m));
        else r.mode = c;
    }
    auto date = [&](const char* key, QDate* out) {
        const QString k = QString::fromLatin1(key);
        if (!o.contains(k) || o.value(k).isNull()) return;
        const QString s = o.value(k).toString().trimmed();
        const QDate d = QDate::fromString(s.left(10), Qt::ISODate);
        if (!d.isValid()) bad(QStringLiteral("%1 \"%2\" is not a date (YYYY-MM-DD)").arg(k, s));
        else *out = d;
    };
    date("from", &r.from);
    date("to", &r.to);
    auto ints = [&](const char* key, int lo, int hi, QList<int>* out) {
        const QString k = QString::fromLatin1(key);
        if (!o.contains(k) || o.value(k).isNull()) return;
        if (!o.value(k).isArray()) { bad(QStringLiteral("%1 must be a list of numbers").arg(k)); return; }
        QList<int> l;
        for (const QJsonValue& v : o.value(k).toArray()) {
            const double d = v.toDouble(-1);
            if (!v.isDouble() || d != std::floor(d) || d < lo || d > hi) { bad(QStringLiteral("%1: %2 is not a whole number from %3 to %4").arg(k, v.toVariant().toString()).arg(lo).arg(hi)); continue; }
            if (!l.contains(int(d))) l << int(d);
        }
        std::sort(l.begin(), l.end());
        *out = l;
    };
    ints("weekdays", 0, 6, &r.weekdays);
    ints("hours", 0, 23, &r.hours);
    auto word = [&](const char* key, const QStringList& allowed, QString* out) {
        const QString k = QString::fromLatin1(key);
        if (!o.contains(k) || o.value(k).isNull()) return;
        const QString c = canon(o.value(k).toString(), allowed);
        if (c.isEmpty()) bad(QStringLiteral("%1 \"%2\" is not one of %3").arg(k, o.value(k).toString(), allowed.join(QStringLiteral(", "))));
        else *out = c;
    };
    word("missing", kMissing, &r.missing);
    word("hourBoundary", kBoundaries, &r.hourBoundary);
    word("repeat", kRepeat, &r.repeat);
    word("osd", kOsd, &r.osd);
    word("breaks", kBreaks, &r.breaks);
    word("selectRandomOrder", kSelectRandom, &r.selectRandomOrder);
    auto num = [&](const char* key, double lo, double hi, double* out) {
        const QString k = QString::fromLatin1(key);
        if (!o.contains(k) || o.value(k).isNull()) return;
        if (!o.value(k).isDouble()) { bad(QStringLiteral("%1 must be a number").arg(k)); return; }
        *out = std::clamp(o.value(k).toDouble(), lo, hi);
    };
    num("floatingSkipMaxLateSec", -1, 86400, &r.floatingSkipMaxLateSec);
    num("minItemSec", 0.04, 60, &r.minItemSec);
    double unrollMax = r.maxUnrollItems;
    num("maxUnrollItems", 100, 500000, &unrollMax);
    r.maxUnrollItems = int(unrollMax);
    auto flag = [&](const char* key, bool* out) {
        const QString k = QString::fromLatin1(key);
        if (o.contains(k) && !o.value(k).isNull()) *out = o.value(k).toBool(*out);
    };
    flag("clearUncovered", &r.clearUncovered);
    flag("validate", &r.validate);
    flag("validateFiles", &r.validateFiles);
    flag("collapseUnsegmentedActs", &r.collapseUnsegmentedActs);
    if (o.contains(QStringLiteral("networkInput")) && !o.value(QStringLiteral("networkInput")).isNull())
        r.networkInput = o.value(QStringLiteral("networkInput")).toString().trimmed();
    if (o.contains(QStringLiteral("inputMap")) && !o.value(QStringLiteral("inputMap")).isNull()) {
        if (!o.value(QStringLiteral("inputMap")).isObject()) bad(QStringLiteral("inputMap must be an object (MasterPlay source -> path, inN or liveSource)"));
        else r.inputMap = o.value(QStringLiteral("inputMap")).toObject();
    }
    if (o.contains(QStringLiteral("timeZone"))) {
        const QString tz = o.value(QStringLiteral("timeZone")).toString().trimmed();
        if (!tz.isEmpty() && !QTimeZone(tz.toUtf8()).isValid()) bad(QStringLiteral("timeZone \"%1\" is not a known time zone").arg(tz));
        else r.timeZone = tz;
    }
    if (o.value(QStringLiteral("nowMs")).isDouble()) r.nowMs = qint64(o.value(QStringLiteral("nowMs")).toDouble());
    r.timeline = tl::Options::fromJson(o);
    return r;
}

QJsonObject Options::toJson() const {
    QJsonObject o = timeline.toJson();
    o["channel"] = channelId;
    o["mode"] = mode;
    if (from.isValid()) o["from"] = from.toString(Qt::ISODate);
    if (to.isValid()) o["to"] = to.toString(Qt::ISODate);
    o["weekdays"] = intArray(weekdays);
    o["hours"] = intArray(hours);
    o["missing"] = missing;
    o["hourBoundary"] = hourBoundary;
    o["floatingSkipMaxLateSec"] = floatingSkipMaxLateSec;
    o["clearUncovered"] = clearUncovered;
    o["minItemSec"] = minItemSec;
    o["timeZone"] = timeZone;
    o["nowMs"] = double(nowMs);
    o["validate"] = validate;
    o["validateFiles"] = validateFiles;
    o["repeat"] = repeat;
    o["maxUnrollItems"] = maxUnrollItems;
    o["osd"] = osd;
    o["breaks"] = breaks;
    o["networkInput"] = networkInput;
    o["inputMap"] = inputMap;
    o["collapseUnsegmentedActs"] = collapseUnsegmentedActs;
    o["selectRandomOrder"] = selectRandomOrder;
    return o;
}

QStringList Options::check() const {
    QStringList e;
    if (!kModes.contains(mode)) e << QStringLiteral("mode \"%1\" is not grid or dates").arg(mode);
    if (mode == QLatin1String("dates")) {
        if (!from.isValid() || !to.isValid()) e << QStringLiteral("dates mode needs from and to (YYYY-MM-DD)");
        else if (to < from) e << QStringLiteral("to (%1) is before from (%2)").arg(to.toString(Qt::ISODate), from.toString(Qt::ISODate));
        else if (from.daysTo(to) >= 366) e << QStringLiteral("a date range holds at most 366 days (%1 asked)").arg(from.daysTo(to) + 1);
    }
    if (!kMissing.contains(missing)) e << QStringLiteral("missing \"%1\" is not drop, keepPath or ingest").arg(missing);
    if (!kBoundaries.contains(hourBoundary)) e << QStringLiteral("hourBoundary \"%1\" is not continue or cut").arg(hourBoundary);
    if (!kRepeat.contains(repeat)) e << QStringLiteral("repeat \"%1\" is not loop or unroll").arg(repeat);
    if (!kOsd.contains(osd)) e << QStringLiteral("osd \"%1\" is not auto, cues, hooks or off").arg(osd);
    if (!kBreaks.contains(breaks)) e << QStringLiteral("breaks \"%1\" is not asScheduled, live or skip").arg(breaks);
    if (!kSelectRandom.contains(selectRandomOrder)) e << QStringLiteral("selectRandomOrder \"%1\" is not random or even").arg(selectRandomOrder);
    if (breaks == QLatin1String("live") && networkInput.trimmed().isEmpty()) e << QStringLiteral("breaks live needs networkInput (an inputMap key, inN or a URL)");
    if (maxUnrollItems < 100) e << QStringLiteral("maxUnrollItems %1 is below 100").arg(maxUnrollItems);
    for (int d : weekdays) if (d < 0 || d > 6) e << QStringLiteral("weekday %1 is not 0..6").arg(d);
    for (int h : hours) if (h < 0 || h > 23) e << QStringLiteral("hour %1 is not 0..23").arg(h);
    if (!timeZone.isEmpty() && !QTimeZone(timeZone.toUtf8()).isValid()) e << QStringLiteral("timeZone \"%1\" is not a known time zone").arg(timeZone);
    return e;
}

QJsonObject Placement::toJson() const {
    QJsonObject o{{"kind", kind}, {"state", state}, {"line", line}};
    if (!reason.isEmpty()) o["reason"] = reason;
    if (!file.isEmpty()) o["file"] = file;
    if (!name.isEmpty()) o["name"] = name;
    if (durationSec >= 0) o["durationSec"] = durationSec;
    if (startMs >= 0 || endMs >= 0) { o["startMs"] = double(startMs); o["endMs"] = double(endMs); }
    if (!slotId.isEmpty()) o["slotId"] = slotId;
    if (copy > 0) o["copy"] = copy;
    if (pick > 0) o["pick"] = pick;
    return o;
}

// ---------------------------------------------------------------------------------------------------------------------
// Plan

int Plan::indexOf(const QString& slotId) const {
    for (int i = 0; i < hourSlots.size(); ++i) if (hourSlots[i].id == slotId) return i;
    return -1;
}

const sched::HourSlot* Plan::slot(const QString& slotId) const {
    const int i = indexOf(slotId);
    return i >= 0 ? &hourSlots[i] : nullptr;
}

sched::BuildOptions Plan::buildOptions(int i) const {
    sched::BuildOptions b;
    b.fillGaps = false;
    b.nowMs = options.nowMs;
    if (i < 0 || i >= hourSlots.size()) return b;
    const sched::HourSlot& s = hourSlots[i];
    if (s.isContinuation()) {
        b.continuesFromPreviousHour = true;
        b.startOffsetSec = s.mergedEvent.value(QStringLiteral("carrySec")).toDouble(0.0);
    }
    const int n = i < nextIndex.size() ? nextIndex[i] : -1;
    b.continuesIntoNextHour = n >= 0 && hourSlots[n].isContinuation();
    return b;
}

QJsonObject Plan::toJson(bool withSlots, bool withPlacements) const {
    QJsonObject o{{"file", file}, {"sha256", sha256}, {"mode", options.mode}, {"channel", options.channelId}, {"options", options.toJson()},
                  {"hours", hours}, {"validation", validation}, {"missing", missing}, {"ambiguous", ambiguous}, {"ingest", ingest},
                  {"folders", folders}, {"unsupported", unsupported}, {"overlays", overlays}, {"pods", pods}, {"acts", acts},
                  {"dropped", dropped}, {"diagnostics", diagnostics}, {"onAirSlots", QJsonArray::fromStringList(onAirSlots)},
                  {"notes", QJsonArray::fromStringList(notes)}, {"errors", QJsonArray::fromStringList(errors)}, {"sources", sources}, {"stats", stats}};
    if (withSlots) {
        QJsonObject s;
        for (const sched::HourSlot& h : hourSlots) s.insert(h.id, h.toJson());
        o["slots"] = s;
        QJsonArray order;
        for (const sched::HourSlot& h : hourSlots) order.append(h.id);
        o["slotOrder"] = order;
    }
    if (withPlacements) {
        QJsonArray a;
        for (const Placement& pl : placements) if (pl.output) a.append(pl.toJson());
        o["placements"] = a;
    }
    return o;
}

// ---------------------------------------------------------------------------------------------------------------------
// Importer

Importer::Importer(sched::ScheduleStore* store, const res::Resolver* resolver, tl::ReadFn read)
    : m_store(store), m_res(resolver), m_read(std::move(read)) {}

QString Importer::itemId(const QString& itemFile, const QString& windowFile, int windowLine, const QString& dayKey, int windowStartSec, int itemIndex,
                         int itemLine, int copy) {
    const QString key = itemFile + QLatin1Char('|') + windowFile + QLatin1Char('#') + QString::number(windowLine) + QLatin1Char('|') + dayKey +
                        QLatin1Char('|') + QString::number(windowStartSec) + QLatin1Char('|') + QString::number(itemIndex) + QLatin1Char('|') +
                        QString::number(itemLine) + QLatin1Char('|') + QString::number(copy);
    return QStringLiteral("mp-") + QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex().left(16));
}

sched::MediaLookup Importer::mediaLookup() const {
    auto list = std::make_shared<QList<sched::MediaRecord>>(m_res ? m_res->media() : QList<sched::MediaRecord>());
    // In id order: folder / query blocks draw from all() / byTypes(), and the plan's validation must not depend on the
    // order the library came in.
    std::stable_sort(list->begin(), list->end(), [](const sched::MediaRecord& a, const sched::MediaRecord& b) { return a.id < b.id; });
    auto byId = std::make_shared<QHash<QString, int>>();
    for (int i = 0; i < list->size(); ++i) if (!byId->contains(list->at(i).id)) byId->insert(list->at(i).id, i);
    sched::MediaLookup m;
    m.byId = [list, byId](const QString& id, bool* found) {
        const auto it = byId->constFind(id);
        if (found) *found = it != byId->cend();
        return it != byId->cend() ? list->at(*it) : sched::MediaRecord();
    };
    m.bySeries = [list](const QString& series) {
        QList<sched::MediaRecord> out;
        for (const sched::MediaRecord& r : *list) if (!r.archived && r.series.compare(series, Qt::CaseInsensitive) == 0) out << r;
        std::stable_sort(out.begin(), out.end(), [](const sched::MediaRecord& a, const sched::MediaRecord& b) {
            return a.episodeDate != b.episodeDate ? a.episodeDate > b.episodeDate : a.addedAtMs > b.addedAtMs;
        });
        return out;
    };
    m.byTypes = [list](const QStringList& types) {
        QList<sched::MediaRecord> out;
        for (const sched::MediaRecord& r : *list) {
            if (r.archived) continue;
            for (const QString& t : types) if (r.type.compare(t, Qt::CaseInsensitive) == 0) { out << r; break; }
        }
        return out;
    };
    m.all = [list]() {
        QList<sched::MediaRecord> out;
        for (const sched::MediaRecord& r : *list) if (!r.archived) out << r;
        return out;
    };
    return m;
}

Plan Importer::plan(const Document& doc, const QString& docPath, const Options& o0) const {
    Plan p;
    p.options = o0;
    if (p.options.nowMs <= 0) p.options.nowMs = QDateTime::currentMSecsSinceEpoch();
    const Options& o = p.options;
    p.file = docPath;
    p.sha256 = QString::fromLatin1(QCryptographicHash::hash(serialize(doc), QCryptographicHash::Sha256).toHex());
    p.errors = o.check();
    if (!m_res) p.errors << QStringLiteral("no library resolver");
    if (!p.errors.isEmpty()) return p;

    // Hour starts: the option, else the channel's zone, else UTC.
    QTimeZone tz = QTimeZone::utc();
    if (!o.timeZone.isEmpty()) tz = QTimeZone(o.timeZone.toUtf8());
    else if (m_store && !o.channelId.isEmpty()) {
        bool found = false;
        const sched::ChannelProfile ch = m_store->channel(o.channelId, &found);
        if (found) tz = ch.tz();
    }

    Run run(o, *m_res, p);
    run.read = m_read;
    run.store = m_store;
    run.grid = o.mode == QLatin1String("grid");
    run.minItemMs = qMax<qint64>(1, secToMs(o.minItemSec));
    run.maxLateMs = secToMs(o.floatingSkipMaxLateSec);
    run.cut = o.hourBoundary == QLatin1String("cut");

    tl::Builder b(doc, docPath, o.timeline, m_read);
    if (run.grid) {
        const QList<tl::DayTimeline> week = b.week();
        run.seq = week + week;   // the second week continues the first, as the cyclic week does
        run.outFirst = 7;
    } else {
        run.seq = b.dates(o.from, o.to);
        run.outFirst = 0;
    }
    const int nDays = int(run.seq.size());
    const int nH = nDays * 24;
    const int outH = run.outFirst * 24;
    run.outStart = run.outFirst * kDayMs;
    run.seqEnd = nDays * kDayMs;
    run.layAll();
    const QList<Run::Span> spans = run.applyOsd();
    QList<Pl>& pls = run.pls;

    // Output membership: placed items by their start, other lines by where they were reached.
    for (Pl& x : pls) {
        const qint64 at = x.pub.state == QLatin1String("placed") || x.pub.startMs >= 0 ? x.pub.startMs : x.atMs;
        x.pub.output = run.grid ? (at >= run.outStart && at < run.seqEnd && x.pub.reason != QLatin1String("wrap")) : at >= 0;
    }
    auto winOutput = [&](const Win& W) {
        const qint64 at = W.anchorMs >= 0 ? W.anchorMs : qint64(W.day) * kDayMs;
        return at >= run.outStart && at < run.seqEnd;
    };
    // The diagnostics raised while laying, for the output only.
    for (const Pl& x : std::as_const(pls)) {
        if (!x.pub.output || x.diags.isEmpty()) continue;
        const qint64 at = x.pub.startMs >= 0 ? x.pub.startMs : x.atMs;
        const QString day = run.seq[int(std::clamp<qint64>(floorDiv(at, kDayMs), 0, qint64(nDays) - 1))].day.label();
        for (const PendingNote& n : x.diags) run.note(n.code, x.pub.file, x.pub.line, n.col, n.message, day);
    }
    for (const Win& W : std::as_const(run.wins)) {
        if (W.diags.isEmpty() || !winOutput(W)) continue;
        const tl::Window& w = run.seq[W.day].windows[W.win];
        for (const PendingNote& n : W.diags) run.note(n.code, w.file, w.line, n.col, n.message, run.seq[W.day].day.label());
    }

    // ---- hours: coverage, filters, which are written ----------------------------------------------------------------
    QList<bool> covered(nH, false), written(nH, false);
    QList<QList<QPair<int, int>>> winsAt(nH);
    for (int k = 0; k < nDays; ++k) {
        const QList<tl::Window>& ws = run.seq[k].windows;
        for (int wi = 0; wi < ws.size(); ++wi) {
            const qint64 a = k * kDayMs + qint64(ws[wi].startSec) * 1000, e = k * kDayMs + qint64(ws[wi].endSec) * 1000;
            for (qint64 g = floorDiv(a, kHourMs); g * kHourMs < e && g < nH; ++g) {
                covered[int(g)] = true;
                winsAt[int(g)] << qMakePair(k, wi);
            }
        }
    }
    for (int g = outH; g < nH; ++g) {
        const tl::Day& day = run.seq[g / 24].day;
        const bool keep = (o.weekdays.isEmpty() || o.weekdays.contains(day.dow)) && (o.hours.isEmpty() || o.hours.contains(g % 24));
        written[g] = keep && (covered[g] || o.clearUncovered);
    }
    auto slotIdOf = [&](int g) { return run.seq[g / 24].day.key() + QLatin1Char('-') + QString::number(g % 24); };
    /// The output hour standing for hour g: the grid's first laid week stands for the second.
    auto outHour = [&](qint64 g) -> qint64 { return (run.grid && g < outH && g >= 0) ? g + 7 * 24 : g; };

    // ---- which item covers the start of each hour (items never overlap: each window starts where the last ended) ----
    QList<int> coverAt(nH, -1);
    QList<double> plannedSec(nH, 0.0);
    for (int idx : run.laid) {
        const Pl& q = pls[idx];
        for (qint64 g = floorDiv(q.pub.startMs, kHourMs); g * kHourMs < q.pub.endMs; ++g) {
            if (g < 0 || g >= nH) continue;
            const qint64 a = qMax(q.pub.startMs, g * kHourMs), e = qMin(q.pub.endMs, (g + 1) * kHourMs);
            plannedSec[int(g)] += msToSec(e - a);
            if (q.pub.startMs < g * kHourMs) coverAt[int(g)] = idx;
        }
    }
    for (auto it = run.loopFill.cbegin(); it != run.loopFill.cend(); ++it)
        if (it.key() >= 0 && it.key() < nH) plannedSec[int(it.key())] += msToSec(it.value());

    // ---- continuations and joins (in hour order) --------------------------------------------------------------------
    QHash<int, int> effRoot;              // placement -> the output hour where it is first written
    QList<int> joinAt(nH, -1);            // hour -> the placement joined at its top
    QList<QJsonObject> merged(nH);
    int continuations = 0, joins = 0;
    for (int g = outH; g < nH; ++g) {
        const int pi = coverAt[g];
        if (pi < 0 || !written[g]) continue;
        const Pl& q = pls[pi];
        // Its root is the slot it starts in (the grid's first week maps onto the second: Sunday 00:00 continues "6-23"),
        // or the first hour it was joined into.
        int root = effRoot.value(pi, -1);
        const qint64 rootG = outHour(floorDiv(q.pub.startMs, kHourMs));
        if (root < 0 && rootG >= outH && rootG < nH && written[int(rootG)]) root = int(rootG);
        const qint64 h0 = qint64(g) * kHourMs;
        if (root >= 0) {
            merged[g] = QJsonObject{{"isContinuation", true}, {"rootSlotId", slotIdOf(root)}, {"carrySec", msToSec(qMin(q.pub.endMs - h0, kHourMs))},
                                    {"title", q.item.title}, {"source", QStringLiteral("masterplay")}};
            effRoot.insert(pi, root);
            ++continuations;
        } else if (q.pub.endMs - h0 >= run.minItemMs) {
            joinAt[g] = pi;
            effRoot.insert(pi, g);
            ++joins;
        }
    }

    // ---- event starts: hard, native, capped or soft -----------------------------------------------------------------
    QHash<int, int> laidPos;
    for (int j = 0; j < run.laid.size(); ++j) laidPos.insert(run.laid[j], j);
    QMap<QString, int> startRules;
    for (int idx : run.laid) {
        Pl& q = pls[idx];
        if (!q.eventFirst) continue;
        const qint64 g = floorDiv(q.pub.startMs, kHourMs);
        const qint64 h0 = g * kHourMs;
        const qint64 late = q.pub.startMs - q.anchorMs;
        const int j = laidPos.value(idx);
        const int prev = j > 0 ? run.laid[j - 1] : -1;
        const bool inSlotPred = prev >= 0 && (floorDiv(pls[prev].pub.startMs, kHourMs) == g || (g >= 0 && g < nH && joinAt[int(g)] == prev));
        const int et = (q.eventType == 1 && run.maxLateMs <= 0) ? 0 : q.eventType;
        const QString policy = et == 0 ? QStringLiteral("truncate") : QStringLiteral("finish_previous");
        auto hard = [&](qint64 atMs, const QString& pol, bool withLate) {
            q.item.timingMode = QStringLiteral("hard_start");
            q.item.fixedOffsetSec = msToSec(atMs - h0);
            q.item.extra["conflictPolicy"] = pol;
            if (withLate) q.item.extra["maxLateSec"] = o.floatingSkipMaxLateSec;
        };
        if (late == 0) { q.rule = QStringLiteral("onTime"); hard(q.anchorMs, policy, et == 1); }
        else if (floorDiv(q.anchorMs, kHourMs) == g && inSlotPred) { q.rule = QStringLiteral("native"); hard(q.anchorMs, policy, et == 1); }
        else if (q.capped) { q.rule = QStringLiteral("capped"); hard(q.pub.startMs, QStringLiteral("truncate"), false); }
        else q.rule = QStringLiteral("soft");
        const qint64 aDay = floorDiv(q.anchorMs, kDayMs) * kDayMs;
        QJsonObject an{{"time", clock(q.anchorMs - aDay)}, {"eventType", q.eventType}, {"start", q.rule}};
        if (late > 0) an["lateSec"] = msToSec(late);
        if (q.podStart) an["pod"] = q.pod;
        q.mp["anchor"] = an;
    }

    // ---- slots ------------------------------------------------------------------------------------------------------
    const QDate today = QDateTime::fromMSecsSinceEpoch(o.nowMs, tz).date();
    auto hourStart = [&](int g) {
        const tl::Day& day = run.seq[g / 24].day;
        const QDate d = day.isDate() ? day.date : today.addDays((day.dow - sched::scheduleDow(today) + 7) % 7);
        return sched::RunlistBuilder::hourStartUtcMs(d, g % 24, tz);
    };
    QList<int> slotIndex(nH, -1);
    QList<QList<int>> itemsAt(nH);
    for (int idx : run.laid) {
        Pl& q = pls[idx];
        const qint64 g = floorDiv(q.pub.startMs, kHourMs);
        if (g < outH || g >= nH) continue;
        if (written[int(g)]) { itemsAt[int(g)] << idx; q.pub.slotId = slotIdOf(int(g)); }
        else q.pub.state = QStringLiteral("filtered");
    }
    auto finalItem = [&](const Pl& q) {
        sched::SlotItem it = q.item;
        QJsonObject mp = q.mp;
        if (!q.before.isEmpty()) mp["instructions"] = q.before;
        if (!q.after.isEmpty()) mp["after"] = q.after;
        it.extra["masterplay"] = mp;
        for (const QJsonValue& c : q.cues) it.cgOverlays.append(c);
        it.cgOnStart << q.onStart;
        it.cgOnEnd << q.onEnd;
        QStringList info;   // !Info lines before it are its note (design §6.9)
        for (int bi : q.beforeIdx)
            if (pls[bi].pub.kind == QLatin1String("info") && !pls[bi].infoText.isEmpty()) info << pls[bi].infoText;
        if (!info.isEmpty()) it.note = info.join(QStringLiteral("; "));
        return it;
    };
    int hardStarts = 0, items = 0, empty = 0, loopSlots = 0;
    for (int g = outH; g < nH; ++g) {
        if (!written[g]) continue;
        const tl::DayTimeline& t = run.seq[g / 24];
        sched::HourSlot s;
        s.id = slotIdOf(g);
        s.channelId = o.channelId;
        s.hour = g % 24;
        s.dayOfWeek = t.day.dow;
        if (t.day.isDate()) s.date = t.day.date;
        s.isLooping = run.loopHours.contains(g);
        s.randomizeAds = false;      // the playlist's order is the order
        s.keepProgramOrder = true;
        s.mergedEvent = merged[g];
        if (joinAt[g] >= 0) {
            const Pl& q = pls[joinAt[g]];
            sched::SlotItem it = finalItem(q);
            it.id += QStringLiteral("~j");
            it.inPointSec += msToSec(qint64(g) * kHourMs - q.pub.startMs);
            it.timingMode = QStringLiteral("soft_start");
            it.fixedOffsetSec = -1.0;
            it.extra.remove(QStringLiteral("conflictPolicy"));
            it.extra.remove(QStringLiteral("maxLateSec"));
            QJsonObject mp = it.extra.value(QStringLiteral("masterplay")).toObject();
            mp.remove(QStringLiteral("anchor"));
            mp.remove(QStringLiteral("instructions"));
            mp["joinSec"] = it.inPointSec;
            it.extra["masterplay"] = mp;
            s.items << it;
            if (s.isLooping) {   // RunlistBuilder would replay the joined programme with the loop
                s.isLooping = false;
                p.notes << QStringLiteral("%1 joins '%2' (its start hour is not written) and so cannot loop: after one pass of the loop the rest of the hour "
                                          "is left to the fill policy")
                               .arg(s.id, q.item.title);
            }
        }
        if (s.isLooping) ++loopSlots;
        for (int idx : itemsAt[g]) {
            s.items << finalItem(pls[idx]);
            if (pls[idx].item.isHardStart()) ++hardStarts;
        }
        items += int(s.items.size());
        if (s.items.isEmpty()) ++empty;
        // Provenance: the events in force during the hour.
        QJsonArray evs;
        QString title;
        qint64 best = -1;   // the title is the Name= of the event holding most of the hour (ties: the later one)
        for (const auto& kw : winsAt[g]) {
            const tl::Window& w = run.seq[kw.first].windows[kw.second];
            const qint64 w0 = qint64(kw.first) * kDayMs + qint64(w.startSec) * 1000, w1 = qint64(kw.first) * kDayMs + qint64(w.endSec) * 1000;
            const qint64 share = qMin(w1, qint64(g + 1) * kHourMs) - qMax(w0, qint64(g) * kHourMs);
            const int wx = run.winIndex[kw.first][kw.second];
            const bool emptyEvent = wx >= 0 && !run.wins[wx].virt && !run.wins[wx].merged && !run.wins[wx].started && !run.wins[wx].breakLive;   // airs nothing
            if (share >= best && !emptyEvent) { best = share; title = w.name; }
            QJsonObject ev{{"line", w.line}, {"stamp", w.stamp.raw}, {"day", run.seq[kw.first].day.label()}, {"start", clock(qint64(w.startSec) * 1000)},
                           {"eventType", w.eventType}, {"playMode", w.playMode}};
            if (w.file.compare(p.file, Qt::CaseInsensitive) != 0) ev["file"] = w.file;
            if (w.carry) { ev["carry"] = true; ev["eventTime"] = clock(qint64(w.eventSec) * 1000); ev["carryDays"] = w.carryDays; }
            if (!w.name.isEmpty()) ev["name"] = w.name;
            if (emptyEvent) ev["empty"] = true;
            if (wx >= 0 && run.wins[wx].looped) ev["loops"] = true;
            if (wx >= 0 && run.wins[wx].breakLive) ev["breakLive"] = true;
            if (wx >= 0 && !run.wins[wx].unplaced.isEmpty() && floorDiv(qint64(kw.first) * kDayMs + qint64(w.startSec) * 1000, kHourMs) == g)
                ev["unplaced"] = run.wins[wx].unplaced;
            evs.append(ev);
        }
        QJsonObject prov{{"file", p.file}, {"sha256", p.sha256}, {"events", evs}};
        if (!covered[g]) prov["uncovered"] = true;
        s.showBlock = QJsonObject{{"masterplay", prov}};
        if (!title.isEmpty()) { s.showBlock["title"] = title; s.blockType = QStringLiteral("show"); }
        slotIndex[g] = int(p.hourSlots.size());
        p.hourSlots << s;
        p.hourStartMs << hourStart(g);
    }
    for (int g = outH; g < nH; ++g) {
        if (slotIndex[g] < 0) continue;
        const int next = (g + 1 < nH) ? g + 1 : (run.grid ? outH : -1);
        p.nextIndex << (next >= 0 ? slotIndex[next] : -1);
    }
    for (const Pl& x : pls) p.placements << x.pub;

    // ---- the seams: the grid's week wrap, the end of a date range ---------------------------------------------------
    if (!pls.isEmpty()) {
        const int lastCover = run.laid.isEmpty() ? -1 : run.laid.last();
        const bool spillsEnd = lastCover >= 0 && pls[lastCover].pub.endMs > run.seqEnd;
        if (run.grid) {
            const int first = outH < nH ? coverAt[outH] : -1;
            const qint64 a = first >= 0 ? pls[first].pub.endMs - run.outStart : 0, e = spillsEnd ? pls[lastCover].pub.endMs - run.seqEnd : 0;
            if (a != e)
                p.notes << QStringLiteral("the week does not repeat exactly at the Saturday to Sunday seam (an event started late): Sunday 00:00 "
                                          "follows the first laid week (%1 s carried in), Saturday ends %2 s past midnight")
                               .arg(msToSec(a))
                               .arg(msToSec(e));
        } else if (spillsEnd) {
            const Pl& q = pls[lastCover];
            p.notes << QStringLiteral("'%1' runs %2 s past the end of the range (into %3-0, which this import does not write): it is cut at "
                                      "midnight unless that hour continues it")
                           .arg(q.item.title)
                           .arg(msToSec(q.pub.endMs - run.seqEnd))
                           .arg(o.to.addDays(1).toString(Qt::ISODate));
        }
    }

    // ---- hours summary ------------------------------------------------------------------------------------------------
    int coveredOut = 0, filtered = 0;
    for (int g = outH; g < nH; ++g) {
        if (covered[g]) ++coveredOut;
        const tl::Day& day = run.seq[g / 24].day;
        const bool keep = (o.weekdays.isEmpty() || o.weekdays.contains(day.dow)) && (o.hours.isEmpty() || o.hours.contains(g % 24));
        if (!keep) ++filtered;
        QJsonObject h{{"id", slotIdOf(g)}, {"written", written[g]}, {"covered", covered[g]}, {"plannedSec", plannedSec[g]},
                      {"events", int(winsAt[g].size())}};
        if (!keep) h["filtered"] = true;
        if (slotIndex[g] >= 0) {
            const sched::HourSlot& s = p.hourSlots[slotIndex[g]];
            int hard = 0;
            for (const sched::SlotItem& it : s.items) if (it.isHardStart()) ++hard;
            h["items"] = int(s.items.size());
            h["hard"] = hard;
            h["hourStartMs"] = double(p.hourStartMs[slotIndex[g]]);
            if (s.isLooping) h["loop"] = true;
        }
        if (!merged[g].isEmpty())
            h["continuation"] = QJsonObject{{"rootSlotId", merged[g].value("rootSlotId")}, {"carrySec", merged[g].value("carrySec")}};
        if (joinAt[g] >= 0) h["join"] = true;
        p.hours.append(h);
    }

    // ---- validation ---------------------------------------------------------------------------------------------------
    int vOk = 0, vErrors = 0, vWarnings = 0;
    if (o.validate) {
        sched::ValidatorOptions vo;
        vo.media = mediaLookup();
        vo.checkFilesExist = o.validateFiles;
        for (int i = 0; i < p.hourSlots.size(); ++i) {
            vo.build = p.buildOptions(i);
            const sched::HourSlot& s = p.hourSlots[i];
            QJsonObject v = sched::ScheduleValidator::validate(s, p.hourStartMs[i], vo).toJson();
            // ScheduleValidator judges the slot's own items: its "overfill ... will be cut" and "hour is empty" do not hold for
            // an hour the next one continues or one a carried programme covers. Kept as info, worded for the plan.
            // An item-less continuation is laid as a whole empty hour (RunlistBuilder: gap 3600): its gap is what the carried
            // programme leaves.
            const bool carriedOnly = s.isContinuation() && s.items.isEmpty();
            const double carry = qBound(0.0, s.mergedEvent.value("carrySec").toDouble(), 3600.0);
            const QString carried = QStringLiteral("'%1' carried in from %2").arg(s.mergedEvent.value("title").toString(), s.mergedEvent.value("rootSlotId").toString());
            QJsonArray issues;
            int errors = 0, warnings = 0;
            for (const QJsonValue& iv : v.value("issues").toArray()) {
                QJsonObject is = iv.toObject();
                const QString code = is.value("code").toString();
                if (code == QLatin1String("overfill") && vo.build.continuesIntoNextHour) {
                    is["severity"] = QStringLiteral("info");
                    is["message"] = is.value("message").toString() + QStringLiteral(" (the next hour continues it: nothing is cut at the top of the hour)");
                } else if (code == QLatin1String("item_overruns_hour") && s.isLooping) {
                    is["severity"] = QStringLiteral("info");   // the hour loop cuts its last copy at the hour end, as planned
                } else if (carriedOnly && (code == QLatin1String("underfill") || code == QLatin1String("gap"))) {
                    continue;   // replaced by the one row below
                }
                if (is.value("severity").toString() == QLatin1String("error")) ++errors;
                else if (is.value("severity").toString() == QLatin1String("warning")) ++warnings;
                issues.append(is);
            }
            if (carriedOnly) {
                const double gap = 3600.0 - carry;
                v["gapSeconds"] = gap;
                if (gap <= 30.0) {
                    issues.append(QJsonObject{{"code", "gap"}, {"severity", "info"}, {"itemId", ""}, {"message", QStringLiteral("the hour is covered by %1").arg(carried)}});
                } else {
                    issues.append(QJsonObject{{"code", "underfill"}, {"severity", "warning"}, {"itemId", ""},
                                              {"message", QStringLiteral("%1 s unfilled after %2 ends (fill policy will cover it at playout)").arg(gap, 0, 'f', 1).arg(carried)}});
                    ++warnings;
                }
            }
            v["issues"] = issues;
            v["errors"] = errors;
            v["warnings"] = warnings;
            if (v.value("ok").toBool()) ++vOk;
            vErrors += errors;
            vWarnings += warnings;
            p.validation.append(v);
        }
    }
    // A carried programme keeps airing while the executor has something queued behind it (composePlaylist "continuation");
    // with nothing in this hour and the next one it may start its emergency loop instead (scheduleexecutor.cpp push()).
    for (int i = 0; i < p.hourSlots.size(); ++i) {
        const sched::HourSlot& s = p.hourSlots[i];
        const int n = p.nextIndex[i];
        if (s.isContinuation() && s.items.isEmpty() && (n < 0 || p.hourSlots[n].items.isEmpty()))
            p.notes << QStringLiteral("%1 is covered by '%2' carried in from %3, but neither it nor the next hour holds an item: with nothing "
                                      "queued for two hours the executor may start its emergency loop over the programme")
                           .arg(s.id, s.mergedEvent.value("title").toString(), s.mergedEvent.value("rootSlotId").toString());
    }

    // ---- on air: written hours overlapping [now, now + 2 h] -------------------------------------------------------------
    for (qint64 t = sched::RunlistBuilder::hourStartUtcMsForTime(o.nowMs, tz); t <= o.nowMs + 2 * kHourMs; t += kHourMs) {
        QDate d;
        int h = 0;
        sched::RunlistBuilder::hourStartUtcMsForTime(t, tz, &d, &h);
        const QString id = run.grid ? sched::HourSlot::templateId(sched::scheduleDow(d), h) : sched::HourSlot::dateId(d, h);
        if (p.indexOf(id) >= 0 && !p.onAirSlots.contains(id)) p.onAirSlots << id;
    }

    // ---- reports: missing / ambiguous media, folders, ingest, unsupported, overlays, pods, acts, dropped ---------------
    res::Report rep;
    QMap<QString, res::Report> perFile;   // diagnostics need the file of each use
    QSet<QString> seenUse, seenFolder;
    QMap<QString, QJsonObject> ingestRows;
    struct Row { QString name, catalog, code, why; int count = 0; QJsonArray lines; QSet<QString> seen; };
    QMap<QString, Row> unsup;
    struct ActRow { QJsonObject row; QJsonArray lines; QSet<QString> seen; };
    QMap<QString, ActRow> actRows;
    QMap<QString, int> droppedBy, kindCount, podItems;
    QHash<QString, int> podFirst;   // "<win>|<copy>|<pod>" -> the first pod item
    for (int idx : run.laid) {
        const Pl& q = pls[idx];
        if (q.pod < 0 || !q.pub.output) continue;
        const QString k = QStringLiteral("%1|%2|%3").arg(q.winIdx).arg(q.copy).arg(q.pod);
        podItems[k] += 1;
        if (!podFirst.contains(k)) podFirst.insert(k, idx);
    }
    int placed = 0, nUnsupported = 0, folderPicks = 0, liveItems = 0;
    auto useOf = [&](const Pl& x) {
        res::Use u;
        u.file = x.pub.file;
        u.line = x.pub.line;
        u.col = 1;
        const tl::Window& w = run.seq[x.pub.day].windows[x.pub.window];
        u.eventLine = w.line;
        u.event = w.stamp.raw;
        return u;
    };
    for (const Pl& x : pls) {
        if (!x.pub.output) continue;
        kindCount[x.pub.kind] += 1;
        if (x.pub.state == QLatin1String("placed") || x.pub.state == QLatin1String("filtered")) {
            if (x.pub.state == QLatin1String("placed")) ++placed;
            if (x.pub.kind == QLatin1String("folder")) ++folderPicks;
            if (x.pub.kind == QLatin1String("live")) ++liveItems;
        }
        if (x.pub.state == QLatin1String("dropped")) ++droppedBy[x.pub.reason];
        const QString useKey = x.pub.file.toCaseFolded() + QLatin1Char('#') + QString::number(x.pub.line);
        if (x.hasRes && x.r.kind == QLatin1String("file") && !seenUse.contains(useKey)) {
            seenUse.insert(useKey);
            const res::Use u = useOf(x);
            rep.add(x.r, u);
            perFile[x.pub.file].add(x.r, u);
            if (!x.r.resolved() && o.missing == QLatin1String("ingest") && !ingestRows.contains(x.r.mapped.toCaseFolded())) {
                QJsonObject row{{"path", x.r.path}, {"mapped", x.r.mapped}};
                if (x.r.fileExists >= 0) row["exists"] = x.r.fileExists == 1;
                ingestRows.insert(x.r.mapped.toCaseFolded(), row);
            }
        }
        if (x.hasFolder && x.folderLine && x.pub.reason != QLatin1String("searchTerm") && !seenFolder.contains(useKey)) {   // (a search term is MP020, not MP019)
            seenFolder.insert(useKey);
            const res::Use u = useOf(x);
            rep.addFolder(x.fc, u);
            perFile[x.pub.file].addFolder(x.fc, u);
        }
        if (x.pub.state == QLatin1String("kept") && x.pub.reason != QLatin1String("breakLive") && x.pub.reason != QLatin1String("osdOff")) {
            ++nUnsupported;
            const InstrInfo* info = instructionInfo(x.pub.name);
            const QString rk = x.pub.name + QLatin1Char('|') + x.pub.reason;
            Row& r = unsup[rk];
            if (r.name.isEmpty()) {
                r.name = x.pub.name.isEmpty() ? QStringLiteral("(unnamed)") : x.pub.name;
                r.catalog = info ? supportName(info->support) : QStringLiteral("unknown");
                r.code = x.pub.reason == QLatin1String("osdUnread") ? QStringLiteral("MP006") : info ? QStringLiteral("MP022") : QStringLiteral("MP011");
                r.why = x.pub.reason == QLatin1String("osdUnread") ? QStringLiteral("the layer's file or image could not be used")
                        : !x.why.isEmpty()                         ? x.why
                                                                   : QStringLiteral("unknown instruction");
            }
            ++r.count;
            if (!r.seen.contains(useKey)) {
                r.seen.insert(useKey);
                if (r.lines.size() < kMaxListed) {
                    QJsonObject l{{"line", x.pub.line}};
                    if (!x.pub.file.isEmpty()) l["file"] = x.pub.file;
                    r.lines.append(l);
                }
            }
        }
        if (x.act >= 0) {
            const QString fk = x.pub.file.toCaseFolded() + QLatin1Char('|') + (x.r.resolved() ? x.r.mediaId : x.r.mapped.toCaseFolded());
            ActRow& a = actRows[fk];
            if (a.row.isEmpty()) {
                a.row = QJsonObject{{"title", x.r.resolved() ? x.r.media.title : logs::titleOf(logs::fileOf(x.r.mapped))}, {"path", x.r.mapped}, {"mode", x.actMode}};
                if (x.r.resolved()) a.row["mediaId"] = x.r.mediaId;
                if (!x.pub.file.isEmpty()) a.row["file"] = x.pub.file;
            }
            if (!a.seen.contains(useKey) && a.lines.size() < kMaxListed) {
                a.seen.insert(useKey);
                a.lines.append(QJsonObject{{"line", x.pub.line}, {"act", x.act}, {"state", x.pub.state}});
            }
        }
        if (x.pub.kind == QLatin1String("break") && (x.pub.state == QLatin1String("mapped") || x.pub.reason == QLatin1String("breakLive"))) {
            const QString k = QStringLiteral("%1|%2|%3").arg(x.winIdx).arg(x.copy).arg(x.pod);
            const qint64 dayIdx = std::clamp<qint64>(floorDiv(x.atMs, kDayMs), 0, qint64(nDays) - 1);
            const QString mode = x.pub.reason == QLatin1String("breakLive") ? QStringLiteral("live")
                                 : o.breaks == QLatin1String("live") ? QStringLiteral("asScheduled") : o.breaks;   // (live: only break-mode events)
            QJsonObject row{{"line", x.pub.line}, {"pod", x.pod}, {"mode", mode}, {"items", podItems.value(k)}, {"day", run.seq[int(dayIdx)].day.label()}};
            if (!x.pub.file.isEmpty()) row["file"] = x.pub.file;
            if (x.copy > 0) row["copy"] = x.copy;
            if (const std::optional<BreakSpec> bs = breakSpec(run.seq[x.pub.day].windows[x.pub.window].items[x.pub.item].entry); bs && !bs->schedTime.isEmpty())
                row["schedTime"] = bs->schedTime;
            if (podFirst.contains(k)) {
                const Pl& f = pls[podFirst.value(k)];
                const qint64 d0 = floorDiv(f.pub.startMs, kDayMs) * kDayMs;
                row["at"] = clock(f.pub.startMs - d0);
                if (!f.pub.slotId.isEmpty()) row["slotId"] = f.pub.slotId;
            }
            p.pods.append(row);
        }
        const bool timedKind = x.pub.kind == QLatin1String("media") || x.pub.kind == QLatin1String("folder") || x.pub.kind == QLatin1String("live");
        if (timedKind && x.pub.state == QLatin1String("dropped") && x.pub.reason != QLatin1String("wrap") && !x.noProvenance) {
            const qint64 at = x.pub.startMs >= 0 ? x.pub.startMs : x.atMs;
            const qint64 dayIdx = std::clamp<qint64>(floorDiv(at, kDayMs), 0, qint64(nDays) - 1);
            QJsonObject d{{"line", x.pub.line}, {"reason", x.pub.reason}, {"kind", x.pub.kind}, {"day", run.seq[int(dayIdx)].day.label()},
                          {"at", clock(at - dayIdx * kDayMs)}};
            d["path"] = x.hasFolder ? x.fc.mapped : x.r.mapped;
            if (!x.pub.file.isEmpty()) d["file"] = x.pub.file;
            if (x.r.resolved()) d["title"] = x.r.media.title;
            if (x.pub.durationSec >= 0) d["durationSec"] = x.pub.durationSec;
            if (x.copy > 0) d["copy"] = x.copy;
            p.dropped.append(d);
        }
    }
    p.missing = rep.missing();
    p.ambiguous = rep.ambiguous();
    p.folders = rep.folders();
    for (const QJsonObject& row : std::as_const(ingestRows)) p.ingest.append(row);
    for (auto it = unsup.cbegin(); it != unsup.cend(); ++it)
        p.unsupported.append(QJsonObject{{"name", it->name}, {"catalog", it->catalog}, {"code", it->code}, {"count", it->count}, {"why", it->why}, {"lines", it->lines}});
    for (auto it = actRows.cbegin(); it != actRows.cend(); ++it) {
        QJsonObject r = it->row;
        r["lines"] = it->lines;
        p.acts.append(r);
    }
    int overlaySpans = 0;
    for (const Run::Span& sp : spans) {
        int outItems = 0;
        QString firstSlot, lastSlot;
        for (int idx : sp.items) {
            if (!pls[idx].pub.output) continue;
            ++outItems;
            if (firstSlot.isEmpty()) firstSlot = pls[idx].pub.slotId;
            if (!pls[idx].pub.slotId.isEmpty()) lastSlot = pls[idx].pub.slotId;
        }
        if (outItems == 0) continue;
        ++overlaySpans;
        const Pl& l = pls[sp.pl];
        const Osd& d = l.osd;
        QJsonObject r{{"kind", d.kind}, {"role", d.role}, {"id", d.id}, {"line", l.pub.line}, {"items", outItems}, {"end", sp.end},
                      {"mode", d.kind == QLatin1String("clock") ? QStringLiteral("identity") : o.osd == QLatin1String("hooks") ? QStringLiteral("hook") : QStringLiteral("cue")}};
        if (!l.pub.file.isEmpty()) r["file"] = l.pub.file;
        if (!firstSlot.isEmpty()) r["firstSlot"] = firstSlot;
        if (!lastSlot.isEmpty()) r["lastSlot"] = lastSlot;
        if (!d.text.isEmpty()) r["text"] = d.text;
        if (!d.filePath.isEmpty()) r["filePath"] = d.filePath;
        p.overlays.append(r);
    }
    if (o.osd == QLatin1String("hooks") && overlaySpans > 0)
        p.notes << QStringLiteral("osd hooks: the layers are written as cgOnStart / cgOnEnd command strings, which the engine does not run yet (it reads a "
                                  "string as an identity graphic name; command-object hooks are MP-17): they show nothing on air, use osd auto for cues");

    // ---- diagnostics: parser, timeline, resolver, importer -----------------------------------------------------------
    for (const Diagnostic& d : doc.diagnostics) {
        tl::Note n;
        n.diag = d;
        n.file = docPath;
        p.diagnostics.append(n.toJson());
    }
    for (const tl::Note& n : b.notes()) p.diagnostics.append(n.toJson());
    for (auto it = perFile.cbegin(); it != perFile.cend(); ++it) {
        for (const Diagnostic& d : it->diagnostics()) {
            tl::Note n;
            n.diag = d;
            n.file = it.key();
            p.diagnostics.append(n.toJson());
        }
    }
    for (int wx = 0; wx < run.wins.size(); ++wx) {
        const Win& W = run.wins[wx];
        if (W.merged || W.virt || !W.started || W.looped || W.breakLive || W.anchorMs < run.outStart || W.anchorMs >= run.seqEnd) continue;   // (empty: noted when laid)
        const tl::Window& w = run.seq[W.day].windows[W.win];
        if (w.playMode == 2 || w.playMode == 4) continue;
        qint64 next = -1;   // the next event's start (a carry that is not this event also ends it, at midnight)
        int nextType = 0;   // ... and how it starts (that carry: as exact)
        auto typeOf = [&](const Win& N) { return N.virt ? 0 : run.seq[N.day].windows[N.win].eventType; };
        for (int k = wx + 1; k < run.wins.size(); ++k) {
            if (run.wins[k].merged || (!run.wins[k].virt && !run.wins[k].started && !run.wins[k].breakLive)) continue;   // an empty event cuts nothing
            next = run.wins[k].virt ? run.wins[k].startMs : run.wins[k].anchorMs;
            nextType = typeOf(run.wins[k]);
            break;
        }
        if (next < 0 && run.grid)   // the grid's last event runs into the week's first one
            for (const Win& F : std::as_const(run.wins))
                if (!F.merged && !F.virt && (F.started || F.breakLive) && F.anchorMs >= run.outStart) { next = F.anchorMs + 7 * kDayMs; nextType = typeOf(F); break; }
        if (next < 0) continue;
        qint64 content = 0;
        bool skip = false;
        for (int pi : W.pls) {
            const Pl& x = pls[pi];
            if ((x.pub.state == QLatin1String("kept") && x.timed) || (x.pub.state == QLatin1String("dropped") && kContentDrops.contains(x.pub.reason))) {
                skip = true;
                break;
            }
            content += x.contentMs;
        }
        if (skip) continue;
        const qint64 span = next - W.startMs;
        const QString evd = w.eventDurationSec > 0 ? QStringLiteral(" (EventDuration says %1)").arg(mmss(secToMs(w.eventDurationSec))) : QString();
        const qint64 nextDay = floorDiv(next, kDayMs) * kDayMs;
        const int nt = (nextType == 1 && run.maxLateMs <= 0) ? 0 : nextType;
        const QString overrun = nt == 2 ? QStringLiteral("that event (floating play all) starts after it")
                                : nt == 1 ? QStringLiteral("the item playing then finishes (by at most %1 s), the rest is skipped").arg(o.floatingSkipMaxLateSec)
                                          : QStringLiteral("what is still playing then is cut");
        if (span - content > qMax<qint64>(600000, span / 4))
            run.note(QStringLiteral("MP016"), w.file, w.line, 1,
                     QStringLiteral("the event's content (%1) is %2 shorter than its window (until the next event at %3)%4: the rest is left to the fill "
                                    "policy (and the emergency loop through hours with nothing queued)")
                         .arg(mmss(content), mmss(span - content), clock(next - nextDay), evd),
                     run.seq[W.day].day.label());
        else if (content - span > qMax<qint64>(60000, span / 20))
            run.note(QStringLiteral("MP016"), w.file, w.line, 1,
                     QStringLiteral("the event's content (%1) runs %2 past the next event at %3%4: %5")
                         .arg(mmss(content), mmss(content - span), clock(next - nextDay), evd, overrun),
                     run.seq[W.day].day.label());
    }
    for (const tl::Note& n : run.notes) p.diagnostics.append(n.toJson());
    p.sources = b.sourcesJson();

    // ---- stats ----------------------------------------------------------------------------------------------------------
    int events = 0, carries = 0, windows = 0, loopingEvents = 0, unrolled = 0;
    for (const Win& W : run.wins) {
        const qint64 at = W.anchorMs >= 0 ? W.anchorMs : qint64(W.day) * kDayMs;
        if (at < run.outStart || at >= run.seqEnd) continue;
        ++windows;
        if (W.anchorMs >= 0) ++events;
        else if (W.virt) ++carries;
        if (W.looped) ++loopingEvents;
        unrolled += W.unrolled;
    }
    for (int idx : run.laid) {
        const Pl& q = pls[idx];
        if (q.eventFirst && q.pub.startMs >= run.outStart && q.pub.startMs < run.seqEnd) startRules[q.rule] += 1;
    }
    QJsonObject dropObj, ruleObj, kindObj;
    for (auto it = droppedBy.cbegin(); it != droppedBy.cend(); ++it) dropObj[it.key()] = it.value();
    for (auto it = startRules.cbegin(); it != startRules.cend(); ++it) ruleObj[it.key()] = it.value();
    for (const QString& k : kKinds) kindObj[k] = kindCount.value(k);
    int diagE = 0, diagW = 0, diagI = 0;
    for (const QJsonValue& v : std::as_const(p.diagnostics)) {
        const QString s = v.toObject().value("severity").toString();
        if (s == QLatin1String("error")) ++diagE;
        else if (s == QLatin1String("warning")) ++diagW;
        else ++diagI;
    }
    double plannedTotal = 0;
    for (int g = outH; g < nH; ++g) if (written[g]) plannedTotal += plannedSec[g];
    p.stats = QJsonObject{
        {"days", nDays - run.outFirst},
        {"windows", windows},
        {"events", events},
        {"carries", carries},
        {"placements", kindObj},
        {"placed", placed},
        {"dropped", dropObj},
        {"starts", ruleObj},
        {"hardStarts", hardStarts},
        {"continuations", continuations},
        {"joins", joins},
        {"slots", QJsonObject{{"written", int(p.hourSlots.size())}, {"empty", empty}, {"covered", coveredOut}, {"uncovered", nH - outH - coveredOut},
                              {"filtered", filtered}, {"onAir", int(p.onAirSlots.size())}, {"looping", loopSlots}}},
        {"items", items},
        {"plannedSec", plannedTotal},
        {"resolver", rep.stats()},
        {"validation", QJsonObject{{"slots", int(p.validation.size())}, {"ok", vOk}, {"errors", vErrors}, {"warnings", vWarnings}}},
        {"loops", QJsonObject{{"events", loopingEvents}, {"unrolledLines", unrolled}}},
        {"folderPicks", folderPicks},
        {"liveItems", liveItems},
        {"overlays", overlaySpans},
        {"pods", int(p.pods.size())},
        {"acts", int(p.acts.size())},
        {"unsupported", nUnsupported},
        {"diagnostics", QJsonObject{{"error", diagE}, {"warning", diagW}, {"info", diagI}}},
    };
    return p;
}

// =====================================================================================================================
// Apply / undo / history (MP-09)

namespace {

const QString kImportsKey = QStringLiteral("masterplay.imports");
QString backupKey(const QString& id) { return QStringLiteral("masterplay.backup.") + id; }

/// The zone hour starts are read in: the import's option, else the channel's, else UTC (as plan() does).
QTimeZone zoneFor(const sched::ScheduleStore* store, const QString& channelId, const QString& option) {
    if (!option.isEmpty()) {
        const QTimeZone z(option.toUtf8());
        if (z.isValid()) return z;
    }
    if (store && !channelId.isEmpty()) {
        bool found = false;
        const sched::ChannelProfile ch = store->channel(channelId, &found);
        if (found) return ch.tz();
    }
    return QTimeZone::utc();
}

QJsonArray readImports(const sched::ScheduleStore* store) { return store->settingJson(kImportsKey).value("imports").toArray(); }
bool writeImports(sched::ScheduleStore* store, const QJsonArray& a) { return store->setSettingJson(kImportsKey, QJsonObject{{"imports", a}}); }
int indexOfImport(const QJsonArray& a, const QString& id) {
    for (int i = 0; i < a.size(); ++i)
        if (a.at(i).toObject().value("id").toString() == id) return i;
    return -1;
}
/// Newest kMaxImports entries; only the newest kMaxBackups keep a backup (a cleared key is "": settingJson gives {}).
void pruneImports(sched::ScheduleStore* store, QJsonArray& a) {
    while (a.size() > Importer::kMaxImports) {
        store->setSetting(backupKey(a.last().toObject().value("id").toString()), QString());
        a.removeLast();
    }
    for (int i = Importer::kMaxBackups; i < a.size(); ++i) {
        QJsonObject e = a.at(i).toObject();
        if (!e.value("backup").toBool()) continue;
        store->setSetting(backupKey(e.value("id").toString()), QString());
        e["backup"] = false;
        a[i] = e;
    }
}
QString isoUtc(qint64 ms) { return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::utc()).toString(Qt::ISODateWithMs); }

} // namespace

ApplyOptions ApplyOptions::fromJson(const QJsonObject& o) {
    ApplyOptions a;
    a.importId = o.value("importId").toString().trimmed();
    a.allowOnAir = o.value("allowOnAir").toBool(false);
    a.force = o.value("force").toBool(false);
    a.nowMs = qint64(o.value("nowMs").toDouble(0));
    a.by = o.value("by").toString();
    return a;
}

QJsonObject ApplyResult::toJson() const {
    QJsonObject o{{"ok", ok()},
                  {"status", status},
                  {"importId", importId},
                  {"channel", channelId},
                  {"written", QJsonArray::fromStringList(written)},
                  {"created", QJsonArray::fromStringList(created)},
                  {"backedUp", QJsonArray::fromStringList(backedUp)},
                  {"restored", QJsonArray::fromStringList(restored)},
                  {"deleted", QJsonArray::fromStringList(deleted)},
                  {"unchanged", QJsonArray::fromStringList(unchanged)},
                  {"conflicts", QJsonArray::fromStringList(conflicts)},
                  {"failed", QJsonArray::fromStringList(failed)},
                  {"onAirSlots", QJsonArray::fromStringList(onAirSlots)},
                  {"entry", entry}};
    if (!error.isEmpty()) o["error"] = error;
    return o;
}

QString Importer::slotDigest(const QJsonObject& slotJson) {
    QJsonObject c = slotJson;
    c.remove(QStringLiteral("updatedAt"));   // saveSlot stamps it: not part of the content
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(c).toJson(QJsonDocument::Compact), QCryptographicHash::Sha1).toHex().left(16));
}

QStringList Importer::onAirAmong(const QStringList& ids, const QTimeZone& tz, qint64 nowMs) {
    QStringList out;
    for (qint64 t = sched::RunlistBuilder::hourStartUtcMsForTime(nowMs, tz); t <= nowMs + 2 * kHourMs; t += kHourMs) {
        QDate d;
        int h = 0;
        sched::RunlistBuilder::hourStartUtcMsForTime(t, tz, &d, &h);
        for (const QString& id : {sched::HourSlot::templateId(sched::scheduleDow(d), h), sched::HourSlot::dateId(d, h)})
            if (ids.contains(id) && !out.contains(id)) out << id;
    }
    return out;
}

QJsonArray Importer::imports(int limit) const {
    if (!m_store || !m_store->isOpen()) return {};
    const QJsonArray a = readImports(m_store);
    if (limit <= 0 || limit >= a.size()) return a;
    QJsonArray o;
    for (int i = 0; i < limit; ++i) o.append(a.at(i));
    return o;
}

QJsonObject Importer::backup(const QString& importId) const {
    if (!m_store || !m_store->isOpen() || importId.isEmpty()) return {};
    return m_store->settingJson(backupKey(importId));
}

ApplyResult Importer::apply(const Plan& plan, const ApplyOptions& a) const {
    ApplyResult r;
    r.channelId = plan.options.channelId;
    auto refuse = [&r](int status, const QString& why) {
        r.status = status;
        r.error = why;
        return r;
    };
    if (!m_store || !m_store->isOpen()) return refuse(500, QStringLiteral("no schedule store"));
    if (!plan.ok()) return refuse(400, QStringLiteral("the plan has errors: ") + plan.errors.join(QStringLiteral("; ")));
    if (r.channelId.isEmpty()) return refuse(400, QStringLiteral("the plan names no channel"));
    bool found = false;
    m_store->channel(r.channelId, &found);
    if (!found) return refuse(400, QStringLiteral("unknown channel '%1'").arg(r.channelId));
    if (plan.hourSlots.isEmpty()) return refuse(400, QStringLiteral("the plan writes no slot"));
    const QString& ch = r.channelId;
    const qint64 now = a.nowMs > 0 ? a.nowMs : QDateTime::currentMSecsSinceEpoch();

    // ---- on air: the plan's list (taken when planned) plus the rule again now -------------------------------------------
    QStringList ids;
    for (const sched::HourSlot& s : plan.hourSlots) ids << s.id;
    for (const QString& id : plan.onAirSlots)
        if (ids.contains(id) && !r.onAirSlots.contains(id)) r.onAirSlots << id;
    for (const QString& id : onAirAmong(ids, zoneFor(m_store, ch, plan.options.timeZone), now))
        if (!r.onAirSlots.contains(id)) r.onAirSlots << id;
    if (!r.onAirSlots.isEmpty() && !a.allowOnAir)
        return refuse(409, QStringLiteral("%1 slot(s) on air or due within two hours (%2): nothing was written; pass allowOnAir to write them")
                               .arg(r.onAirSlots.size())
                               .arg(r.onAirSlots.join(QStringLiteral(", "))));

    // ---- the import id ---------------------------------------------------------------------------------------------------
    QJsonArray hist = readImports(m_store);
    auto used = [&](const QString& id) { return indexOfImport(hist, id) >= 0 || !m_store->setting(backupKey(id)).isEmpty(); };
    QString id = a.importId.trimmed();
    if (!id.isEmpty()) {
        static const QRegularExpression rx(QStringLiteral("^[A-Za-z0-9_.-]{1,64}$"));
        if (!rx.match(id).hasMatch()) return refuse(400, QStringLiteral("bad import id '%1' (letters, digits, '_', '.', '-'; at most 64)").arg(id));
        if (used(id)) return refuse(409, QStringLiteral("import id '%1' is already used").arg(id));
    } else {
        const QString stamp = QDateTime::fromMSecsSinceEpoch(now, QTimeZone::utc()).toString(QStringLiteral("yyyyMMdd'T'HHmmsszzz"));
        for (int n = 0; id.isEmpty() || used(id); ++n)
            id = QStringLiteral("mp%1-%2").arg(stamp, shortHash(QStringLiteral("%1|%2|%3").arg(plan.sha256, ch).arg(n)).left(6));
    }
    r.importId = id;

    // ---- backup: what the store holds now, and what is about to be written ----------------------------------------------
    QList<sched::HourSlot> out;
    QJsonObject before, written;
    QJsonArray order;
    QStringList replaced, created;
    for (const sched::HourSlot& s : plan.hourSlots) {
        sched::HourSlot w = s;
        w.channelId = ch;
        QJsonObject prov = w.showBlock.value("masterplay").toObject();
        prov["importId"] = id;
        w.showBlock["masterplay"] = prov;
        bool had = false;
        const sched::HourSlot cur = m_store->slot(ch, s.id, &had);
        before[s.id] = had ? QJsonValue(cur.toJson()) : QJsonValue(QJsonValue::Null);
        (had ? replaced : created) << s.id;
        // What slot() will give back: fromJson of the stored JSON, id and channel set (replaced by the read-back below).
        sched::HourSlot back = sched::HourSlot::fromJson(w.toJson());
        back.channelId = ch;
        back.id = s.id;
        written[s.id] = slotDigest(back.toJson());
        order.append(s.id);
        out << w;
    }
    QJsonObject bk{{"importId", id}, {"channel", ch}, {"at", isoUtc(now)}, {"file", plan.file}, {"sha256", plan.sha256},
                   {"order", order}, {"slots", before}, {"written", written}};
    if (!m_store->setSettingJson(backupKey(id), bk)) return refuse(500, QStringLiteral("could not write the backup: nothing was written"));

    // ---- history first ("applying"), so a crash half-way still leaves an undoable record --------------------------------
    QJsonObject entry{{"id", id},
                      {"at", isoUtc(now)},
                      {"atMs", double(now)},
                      {"by", a.by},
                      {"file", plan.file},
                      {"sha256", plan.sha256},
                      {"channel", ch},
                      {"mode", plan.options.mode},
                      {"slots", int(out.size())},
                      {"slotIds", QJsonArray::fromStringList(ids)},
                      {"created", QJsonArray::fromStringList(created)},
                      {"replaced", QJsonArray::fromStringList(replaced)},
                      {"options", plan.options.toJson()},
                      {"state", QStringLiteral("applying")},
                      {"backup", true}};
    if (plan.options.from.isValid()) entry["from"] = plan.options.from.toString(Qt::ISODate);
    if (plan.options.to.isValid()) entry["to"] = plan.options.to.toString(Qt::ISODate);
    if (!r.onAirSlots.isEmpty()) entry["onAir"] = QJsonArray::fromStringList(r.onAirSlots);   // allowOnAir was given
    hist.prepend(entry);
    pruneImports(m_store, hist);
    if (!writeImports(m_store, hist)) {
        m_store->setSetting(backupKey(id), QString());
        return refuse(500, QStringLiteral("could not write the import history: nothing was written"));
    }

    // ---- the slots ----------------------------------------------------------------------------------------------------
    for (const sched::HourSlot& w : std::as_const(out)) {
        if (!m_store->saveSlot(w)) {
            r.failed << w.id;
            break;
        }
        r.written << w.id;
        (replaced.contains(w.id) ? r.backedUp : r.created) << w.id;
        written[w.id] = slotDigest(m_store->slot(ch, w.id).toJson());   // the digest undo compares against
    }
    bk["written"] = written;
    m_store->setSettingJson(backupKey(id), bk);

    hist = readImports(m_store);
    const int at = indexOfImport(hist, id);
    entry["state"] = r.failed.isEmpty() ? QStringLiteral("applied") : QStringLiteral("partial");
    entry["written"] = int(r.written.size());
    if (at >= 0) {
        hist[at] = entry;
        writeImports(m_store, hist);
    }
    r.entry = entry;
    if (!r.written.isEmpty() && a.afterSave) a.afterSave();
    if (!r.failed.isEmpty())
        return refuse(500, QStringLiteral("saving slot %1 failed after %2 slot(s); undo '%3' puts them back").arg(r.failed.first()).arg(r.written.size()).arg(id));
    return r;
}

ApplyResult Importer::undo(const QString& importId, const ApplyOptions& a) const {
    ApplyResult r;
    r.importId = importId;
    auto refuse = [&r](int status, const QString& why) {
        r.status = status;
        r.error = why;
        return r;
    };
    if (!m_store || !m_store->isOpen()) return refuse(500, QStringLiteral("no schedule store"));
    QJsonArray hist = readImports(m_store);
    const int at = indexOfImport(hist, importId);
    if (importId.isEmpty() || at < 0) return refuse(404, QStringLiteral("unknown import '%1'").arg(importId));
    QJsonObject entry = hist.at(at).toObject();
    r.entry = entry;
    r.channelId = entry.value("channel").toString();
    const QString& ch = r.channelId;
    if (entry.value("state").toString() == QLatin1String("undone")) return refuse(409, QStringLiteral("import '%1' is already undone").arg(importId));
    const QJsonObject bk = backup(importId);
    if (!entry.value("backup").toBool() || bk.isEmpty())
        return refuse(409, QStringLiteral("the backup of import '%1' is gone (only the newest %2 imports keep one)").arg(importId).arg(kMaxBackups));
    const qint64 now = a.nowMs > 0 ? a.nowMs : QDateTime::currentMSecsSinceEpoch();
    const QJsonObject slotsBefore = bk.value("slots").toObject(), written = bk.value("written").toObject();

    // ---- what changes: unchanged / conflicts / the rest ------------------------------------------------------------------
    QStringList change;
    for (const QJsonValue& v : bk.value("order").toArray()) {
        const QString id = v.toString();
        bool had = false;
        const sched::HourSlot cur = m_store->slot(ch, id, &had);
        const QString is = had ? slotDigest(cur.toJson()) : QStringLiteral("absent");
        const QJsonValue b = slotsBefore.value(id);
        const QString was = b.isObject() ? slotDigest(b.toObject()) : QStringLiteral("absent");
        if (is == was) { r.unchanged << id; continue; }
        if (is != written.value(id).toString()) {   // edited (or removed) since the import
            r.conflicts << id;
            if (!a.force) continue;
        }
        change << id;
    }
    r.onAirSlots = onAirAmong(change, zoneFor(m_store, ch, entry.value("options").toObject().value("timeZone").toString()), now);
    if (!r.onAirSlots.isEmpty() && !a.allowOnAir)
        return refuse(409, QStringLiteral("%1 slot(s) to restore are on air or due within two hours (%2): nothing was changed; pass allowOnAir to "
                                          "restore them")
                               .arg(r.onAirSlots.size())
                               .arg(r.onAirSlots.join(QStringLiteral(", "))));

    // ---- restore ------------------------------------------------------------------------------------------------------
    for (const QString& id : std::as_const(change)) {
        const QJsonValue b = slotsBefore.value(id);
        if (b.isObject()) {
            sched::HourSlot s = sched::HourSlot::fromJson(b.toObject());
            s.channelId = ch;
            s.id = id;
            if (m_store->saveSlot(s)) r.restored << id;
            else r.failed << id;
        } else {
            if (m_store->deleteSlot(ch, id)) r.deleted << id;
            else r.failed << id;
        }
    }
    const bool complete = r.failed.isEmpty() && (a.force || r.conflicts.isEmpty());
    entry["state"] = complete ? QStringLiteral("undone") : QStringLiteral("undoPartial");
    entry["undoneAt"] = isoUtc(now);
    if (!a.by.isEmpty()) entry["undoneBy"] = a.by;
    entry["undo"] = QJsonObject{{"restored", int(r.restored.size())}, {"deleted", int(r.deleted.size())}, {"unchanged", int(r.unchanged.size())},
                                {"conflicts", QJsonArray::fromStringList(r.conflicts)}, {"forced", a.force}, {"failed", QJsonArray::fromStringList(r.failed)}};
    hist = readImports(m_store);
    const int at2 = indexOfImport(hist, importId);
    if (at2 >= 0) {
        hist[at2] = entry;
        writeImports(m_store, hist);
    }
    r.entry = entry;
    if ((!r.restored.isEmpty() || !r.deleted.isEmpty()) && a.afterSave) a.afterSave();
    if (!r.failed.isEmpty()) return refuse(500, QStringLiteral("restoring slot %1 failed; the others were restored").arg(r.failed.first()));
    return r;
}

} // namespace mp::imp
