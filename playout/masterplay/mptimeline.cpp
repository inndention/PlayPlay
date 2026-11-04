#include "mptimeline.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <algorithm>
#include <optional>

namespace mp::tl {

namespace {

const char* const kDayNames[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

int floorDiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

QString secText(qint64 sec) {   // "HH:MM:SS"; 86400 is "24:00:00"
    const qint64 s = qMax<qint64>(0, sec);
    return QStringLiteral("%1:%2:%3")
        .arg(s / 3600, 2, 10, QLatin1Char('0'))
        .arg(s / 60 % 60, 2, 10, QLatin1Char('0'))
        .arg(s % 60, 2, 10, QLatin1Char('0'));
}

int parseClockSec(const QString& text) {   // "HH:MM" or "HH:MM:SS" -> second of the day, -1 when it is not one
    const QStringList p = text.trimmed().split(QLatin1Char(':'));
    if (p.size() < 2 || p.size() > 3) return -1;
    int v[3] = {0, 0, 0};
    for (int i = 0; i < p.size(); ++i) {
        bool ok = false;
        v[i] = p[i].toInt(&ok);
        if (!ok || v[i] < 0) return -1;
    }
    if (v[0] > 23 || v[1] > 59 || v[2] > 59) return -1;
    return v[0] * 3600 + v[1] * 60 + v[2];
}

/// The identity of a file for caches and cycle checks: '/' -> '\', "." / ".." folded, case-folded.
QString fileKey(const QString& path) {
    const QString t = path.trimmed();
    if (t.isEmpty()) return {};
    return ini::normalizePath(QDir::cleanPath(QDir::fromNativeSeparators(t))).toCaseFolded();
}

bool isRootedPath(const QString& n) {   // n normalised: "D:\..." or "\\host..."
    return (n.size() >= 3 && n.at(0).isLetter() && n.at(1) == QLatin1Char(':') && n.at(2) == QLatin1Char('\\')) ||
           n.startsWith(QLatin1String("\\\\"));
}

QString folderOf(const QString& file) {   // normalised parent folder, "" for a bare name
    const QString n = ini::normalizePath(file.trimmed());
    const qsizetype i = n.lastIndexOf(QLatin1Char('\\'));
    if (i < 0) return {};
    return n.left(i + 1);
}

/// Drive root ("C:\") or share root ("\\host\share\") of a normalised absolute path.
QString rootOf(const QString& n) {
    if (n.startsWith(QLatin1String("\\\\"))) {
        const qsizetype host = n.indexOf(QLatin1Char('\\'), 2);
        if (host < 0) return n + QLatin1Char('\\');
        const qsizetype share = n.indexOf(QLatin1Char('\\'), host + 1);
        return share < 0 ? n + QLatin1Char('\\') : n.left(share + 1);
    }
    return n.left(3);
}

bool appliesOn(const Stamp& s, const Day& origin) {
    switch (s.kind) {
    case DayKind::EveryDay:
    case DayKind::DayChange: return true;
    case DayKind::Weekly: return s.weekday == origin.weekdayCode();
    case DayKind::Dated: return origin.isDate() && s.date == origin.date;
    default: return false;
    }
}

QString shortHash(const QString& s) {
    return QString::fromLatin1(QCryptographicHash::hash(s.toUtf8(), QCryptographicHash::Sha1).toHex().left(12));
}

/// One candidate instance of an event on a day.
struct Inst {
    int event = -1;
    int sec = 0;          // landing second (after timeShift)
    int rank = 0;         // priority used for ties (0 for all under samePriority)
    int priority = 0;     // the stamp kind's priority
    int entry = 0;        // header index: the file order
};
struct DayInst {
    QList<Inst> kept;                   // by sec, ties resolved
    QList<QPair<Inst, Inst>> dropped;   // (dropped, the one kept at that second)
    int datedSkipped = 0;
};

struct SourceDoc {
    QString path, key, target, why;
    bool ok = false, root = false;
    Document doc;
    int bytes = -1;
    QString sha256;
    QHash<QString, DayInst> days;       // Day::key() -> candidates (the cache)
};

/// One window's expansion.
struct Walk {
    QList<Item>* items = nullptr;
    Day subDay;                         // where the inserting event starts: sub-playlists are looked up there
    int subSec = 0;
    QString noteDay;
    QStringList chain;                  // file keys on the insert path
    bool cut = false, limited = false;
    int cutAt = -1, cutItems = 0, changeDoc = -1;
};

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Days, priorities, options

Day Day::weekday(int dow) {
    Day d;
    d.dow = (dow % 7 + 7) % 7;
    return d;
}

Day Day::of(const QDate& date) {
    Day d;
    d.date = date;
    d.dow = date.isValid() ? date.dayOfWeek() % 7 : -1;   // Qt: 1 Monday .. 7 Sunday
    return d;
}

Day Day::addDays(int n) const {
    if (isDate()) return of(date.addDays(n));
    return isValid() ? weekday(dow + n) : Day();
}

QString Day::label() const {
    if (isDate()) return date.toString(Qt::ISODate);
    return isValid() ? QString::fromLatin1(kDayNames[dow]) : QString();
}

QString Day::key() const {
    if (isDate()) return date.toString(Qt::ISODate);
    return isValid() ? QString::number(dow) : QString();
}

int priorityOf(DayKind k) {
    switch (k) {
    case DayKind::Dated: return 3;
    case DayKind::Weekly: return 2;
    case DayKind::EveryDay: return 1;
    case DayKind::DayChange: return 0;
    default: return -1;
    }
}

QString priorityName(int p) {
    switch (p) {
    case 3: return QStringLiteral("dated");
    case 2: return QStringLiteral("weekly");
    case 1: return QStringLiteral("everyDay");
    case 0: return QStringLiteral("dayChange");
    default: return {};
    }
}

Options Options::fromIni(const ini::MplayIni& ini) {
    Options o;
    o.samePriority = ini.eventsSamePriority;
    if (ini.dayChange.isValid()) o.dayChangeSec = ini.dayChange.msecsSinceStartOfDay() / 1000;
    o.parse.fps = ini.suggestedFps();
    return o;
}

Options Options::fromJson(const QJsonObject& j) {
    Options o;
    if (j.contains("maxDepth")) o.maxDepth = std::clamp(j.value("maxDepth").toInt(o.maxDepth), 0, 8);
    if (j.contains("maxItems")) o.maxItems = std::clamp(j.value("maxItems").toInt(o.maxItems), 100, 1000000);
    if (j.contains("subPlaylists")) o.expandSubPlaylists = j.value("subPlaylists").toString().compare(QLatin1String("off"), Qt::CaseInsensitive) != 0;
    if (j.contains("expandSubPlaylists")) o.expandSubPlaylists = j.value("expandSubPlaylists").toBool(o.expandSubPlaylists);
    o.followChangePlaylist = j.value("followChangePlaylist").toBool(o.followChangePlaylist);
    o.samePriority = j.value("samePriority").toBool(o.samePriority);
    if (j.value("dayChange").isString()) {
        const int s = parseClockSec(j.value("dayChange").toString());
        if (s >= 0) o.dayChangeSec = s;
    } else if (j.contains("dayChangeSec")) {
        o.dayChangeSec = std::clamp(j.value("dayChangeSec").toInt(0), 0, kDaySec - 1);
    }
    if (j.contains("timeShiftMin")) o.timeShiftMin = std::clamp(j.value("timeShiftMin").toInt(0), -1440, 1440);
    if (j.contains("fps")) o.parse.fps = std::clamp(j.value("fps").toInt(o.parse.fps), 1, 120);
    const QString order = j.value("legacyDateOrder").toString().trimmed().toLower();
    if (order == u"auto" || order == u"dmy" || order == u"mdy") o.parse.legacyDateOrder = order;
    return o;
}

QJsonObject Options::toJson() const {
    return QJsonObject{{"maxDepth", maxDepth}, {"maxItems", maxItems}, {"subPlaylists", expandSubPlaylists ? "expand" : "off"},
                       {"followChangePlaylist", followChangePlaylist}, {"samePriority", samePriority},
                       {"dayChange", secText(dayChangeSec)}, {"timeShiftMin", timeShiftMin}, {"fps", parse.fps},
                       {"legacyDateOrder", parse.legacyDateOrder}};
}

// ---------------------------------------------------------------------------------------------------------------------
// Readers

ReadFn fileReader(const QList<ini::PathRule>& rules, qint64 maxBytes) {
    struct State {
        QList<ini::PathRule> rules;
        qint64 max = 0;
        QMutex lock;
        QHash<QString, bool> roots;   // case-folded root -> reachable
    };
    auto st = std::make_shared<State>();
    st->rules = ini::normalizeRules(rules);
    st->max = maxBytes > 0 ? maxBytes : 16 * 1024 * 1024;
    return [st](const QString& target, const QString& fromFile) -> ReadResult {
        ReadResult r;
        const QString n = ini::normalizePath(target.trimmed());
        r.path = n;
        if (n.isEmpty()) { r.why = QStringLiteral("no playlist named"); return r; }
        QString path;
        if (isRootedPath(n)) {
            path = ini::mapPath(n, st->rules);
        } else {
            const QString folder = folderOf(fromFile);
            if (folder.isEmpty() || !isRootedPath(folder)) {
                r.why = QStringLiteral("a relative name, and the including playlist's folder is not known");
                return r;
            }
            const QString drive = folder.left(2);   // "\x.in" is rooted on the including file's drive
            path = n.startsWith(QLatin1Char('\\')) ? (folder.startsWith(QLatin1String("\\\\")) ? rootOf(folder).chopped(1) + n : drive + n)
                                                    : folder + n;
        }
        path = ini::normalizePath(QDir::cleanPath(QDir::fromNativeSeparators(path)));
        r.path = path;
        if (!isRootedPath(path)) { r.why = QStringLiteral("not an absolute path after the path rules"); return r; }
        const QString root = rootOf(path), rootKey = root.toCaseFolded();
        bool known = false, reachable = true;
        {
            QMutexLocker l(&st->lock);
            if (st->roots.contains(rootKey)) { known = true; reachable = st->roots.value(rootKey); }
        }
        if (!known) {   // may block on a dead UNC host; once per root
            reachable = QFileInfo::exists(root);
            QMutexLocker l(&st->lock);
            st->roots.insert(rootKey, reachable);
        }
        if (!reachable) { r.why = QStringLiteral("root unreachable: ") + root; return r; }
        const QFileInfo fi(path);
        if (!fi.exists()) { r.why = QStringLiteral("not found"); return r; }
        if (fi.isDir()) { r.why = QStringLiteral("is a folder"); return r; }
        if (fi.size() > st->max) { r.why = QStringLiteral("too large (%1 bytes)").arg(fi.size()); return r; }
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) { r.why = QStringLiteral("unreadable: ") + f.errorString(); return r; }
        r.bytes = f.read(st->max + 1);
        if (r.bytes.size() > st->max) { r.bytes.clear(); r.why = QStringLiteral("too large"); return r; }
        r.ok = true;
        return r;
    };
}

ReadFn memoryReader(const QHash<QString, QByteArray>& files) {
    QHash<QString, QPair<QString, QByteArray>> byKey;
    for (auto it = files.cbegin(); it != files.cend(); ++it) byKey.insert(fileKey(it.key()), {it.key(), it.value()});
    return [byKey](const QString& target, const QString& fromFile) -> ReadResult {
        ReadResult r;
        r.path = target.trimmed();
        if (r.path.isEmpty()) { r.why = QStringLiteral("no playlist named"); return r; }
        QStringList tries{fileKey(r.path)};
        const QString n = ini::normalizePath(r.path);
        if (!isRootedPath(n) && !folderOf(fromFile).isEmpty()) tries << fileKey(folderOf(fromFile) + n);
        for (const QString& k : tries) {
            const auto it = byKey.constFind(k);
            if (it == byKey.cend()) continue;
            r.ok = true;
            r.path = it->first;
            r.bytes = it->second;
            return r;
        }
        r.why = QStringLiteral("not found");
        return r;
    };
}

// ---------------------------------------------------------------------------------------------------------------------
// JSON views

QJsonObject Item::toJson() const {
    QJsonObject o{{"line", entry.line}, {"kind", lineKindName(entry.kind)}, {"text", entry.raw}};
    if (depth > 0) { o["file"] = file; o["depth"] = depth; }
    if (parent >= 0) o["parent"] = parent;
    if (children > 0) o["children"] = children;
    if (!group.isEmpty()) o["group"] = group;
    if (viaChange) o["viaChange"] = true;
    if (entry.kind == LineKind::Instruction) {
        const Instruction ins = identify(entry);
        o["instruction"] = ins.name;
    }
    if (!target.isEmpty()) o["target"] = target;
    if (!targetPath.isEmpty()) o["targetPath"] = targetPath;
    if (!status.isEmpty()) o["status"] = status;
    if (!why.isEmpty()) o["why"] = why;
    if (subEventLine > 0) o["subEventLine"] = subEventLine;
    if (subPlayMode > 0) o["subPlayMode"] = subPlayMode;
    if (targetPlayMode > 0) o["targetPlayMode"] = targetPlayMode;
    if (targetLoopFrom >= 0) o["targetLoopFrom"] = targetLoopFrom;
    if (replacedWindows > 0) o["replacedWindows"] = replacedWindows;
    return o;
}

QJsonObject Window::toJson(bool withItems) const {
    QJsonObject o{{"start", secText(startSec)}, {"end", secText(endSec)}, {"startSec", startSec}, {"endSec", endSec},
                  {"file", file}, {"line", line}, {"stamp", stamp.raw}, {"dayKind", dayKindName(stamp.kind)},
                  {"priority", priorityName(priority)}, {"eventTime", secText(eventSec)}, {"eventType", eventType},
                  {"playMode", playMode}, {"playModeFrom", playModeFrom}, {"itemCount", int(items.size())}};
    if (headerPlayMode >= 0) o["headerPlayMode"] = headerPlayMode;
    if (carry) { o["carry"] = true; o["carryDays"] = carryDays; o["offsetSec"] = double(offsetSec); }
    if (!name.isEmpty()) o["name"] = name;
    if (eventDurationSec >= 0) o["eventDurationSec"] = eventDurationSec;
    if (expectedSec >= 0) o["expectedSec"] = expectedSec;
    if (repeatAt >= 0) o["repeatAt"] = repeatAt;
    if (loopFrom >= 0) o["loopFrom"] = loopFrom;
    if (cutAt >= 0) { o["cutAt"] = cutAt; o["cutItems"] = cutItems; }
    if (limited) o["limited"] = true;
    if (changeDepth > 0) o["changeDepth"] = changeDepth;
    if (!via.isEmpty()) o["via"] = via;
    if (withItems) {
        QJsonArray a;
        for (const Item& it : items) a.append(it.toJson());
        o["items"] = a;
    }
    return o;
}

QJsonObject Dropped::toJson() const {
    return QJsonObject{{"file", file}, {"line", line}, {"stamp", stamp}, {"time", secText(sec)}, {"priority", priorityName(priority)},
                       {"kept", QJsonObject{{"line", keptLine}, {"stamp", keptStamp}, {"priority", priorityName(keptPriority)}}}};
}

const Window* DayTimeline::at(int sec) const {
    for (const Window& w : windows)
        if (w.startSec <= sec && sec < w.endSec) return &w;
    return nullptr;
}

QJsonObject DayTimeline::toJson(bool withItems) const {
    QJsonObject o{{"day", day.label()}, {"key", day.key()}, {"dow", day.dow}};
    if (day.isDate()) o["date"] = day.date.toString(Qt::ISODate);
    if (datedSkipped > 0) o["datedSkipped"] = datedSkipped;
    QJsonArray ws, ds;
    for (const Window& w : windows) ws.append(w.toJson(withItems));
    for (const Dropped& x : dropped) ds.append(x.toJson());
    o["windows"] = ws;
    o["dropped"] = ds;
    return o;
}

QJsonObject Note::toJson() const {
    QJsonObject o = diag.toJson();
    o["file"] = file;
    o["days"] = QJsonArray::fromStringList(days);
    return o;
}

// ---------------------------------------------------------------------------------------------------------------------
// Builder

struct Builder::Impl {
    Options o;
    ReadFn read;
    QList<std::shared_ptr<SourceDoc>> docs;   // 0: the root
    QHash<QString, int> byKey;                // fileKey -> docs index
    QHash<QString, int> byRequest;            // case-folded target | including file key -> docs index
    QList<Note> notes;
    QHash<QString, int> noteIndex;
    int reads = 0;
    // totals over build()
    int days = 0, windows = 0, carry = 0, dropped = 0, items = 0, maxDepth = 0, cutItems = 0, limited = 0;
    QHash<QString, int> inserts, changes;

    void note(const char* code, const QString& file, int line, int col, const QString& message, const QString& day) {
        const QString c = QString::fromLatin1(code);
        const QString k = fileKey(file) + QLatin1Char('|') + QString::number(line) + QLatin1Char('|') + QString::number(col) +
                          QLatin1Char('|') + c + QLatin1Char('|') + message;
        const auto it = noteIndex.constFind(k);
        if (it != noteIndex.cend()) {
            QStringList& ds = notes[*it].days;
            if (!day.isEmpty() && !ds.contains(day)) ds << day;
            return;
        }
        Note n;
        n.diag = Diagnostic{line, col, diagnosticInfo(c).severity, c, message};
        n.file = file;
        if (!day.isEmpty()) n.days << day;
        noteIndex.insert(k, int(notes.size()));
        notes << n;
    }

    /// The document a (non-empty) target names: read once per (target, including file), parsed once per file. A failed
    /// read is a document too (ok false), so the failure is not retried and sourcesJson() lists it.
    int open(const QString& target, int fromDoc) {
        const std::shared_ptr<SourceDoc> from = docs.at(fromDoc);
        const QString req = target.trimmed().toCaseFolded() + QLatin1Char('|') + from->key;
        const auto hit = byRequest.constFind(req);
        if (hit != byRequest.cend()) return *hit;
        ReadResult r;
        if (read) {
            r = read(target, from->path);
            ++reads;
        } else {
            r.path = target.trimmed();
            r.why = QStringLiteral("no reader (sub-playlists cannot be read here)");
        }
        const QString path = r.path.isEmpty() ? target.trimmed() : r.path;
        const QString key = fileKey(path);
        const auto known = byKey.constFind(key);
        if (known != byKey.cend() && (docs.at(*known)->ok || !r.ok)) {   // one document per file
            byRequest.insert(req, *known);
            return *known;
        }
        auto s = std::make_shared<SourceDoc>();
        s->path = path;
        s->key = key;
        s->target = target.trimmed();
        s->ok = r.ok;
        s->why = r.why;
        if (r.ok) {
            s->doc = parse(r.bytes, o.parse);
            s->bytes = int(r.bytes.size());
            s->sha256 = QString::fromLatin1(QCryptographicHash::hash(r.bytes, QCryptographicHash::Sha256).toHex());
        }
        const int idx = int(docs.size());
        docs << s;
        byKey.insert(key, idx);
        byRequest.insert(req, idx);
        return idx;
    }

    /// Candidates of one document on one day: instances landing there, sorted, ties resolved. Returned by value: the
    /// cache may rehash while a caller still looks at an earlier day.
    DayInst kept(int docIdx, const Day& day) {
        const std::shared_ptr<SourceDoc> s = docs.at(docIdx);
        const QString dk = day.key();
        const auto it = s->days.constFind(dk);
        if (it != s->days.cend()) return *it;
        DayInst di;
        QList<Inst> all;
        const int shift = std::clamp(o.timeShiftMin, -1440, 1440) * 60;
        for (int i = 0; i < s->doc.events.size(); ++i) {
            const Entry& h = s->doc.header(s->doc.events.at(i));
            const int pr = priorityOf(h.stamp.kind);
            if (pr < 0) continue;
            if (h.stamp.kind == DayKind::Dated && !day.isDate()) { ++di.datedSkipped; continue; }
            const int base = h.stamp.kind == DayKind::DayChange ? std::clamp(o.dayChangeSec, 0, kDaySec - 1) : h.stamp.secondOfDay();
            const int total = base + shift;
            const int delta = floorDiv(total, kDaySec);
            if (!appliesOn(h.stamp, day.addDays(-delta))) continue;
            all << Inst{i, total - delta * kDaySec, o.samePriority ? 0 : pr, pr, s->doc.events.at(i).entry};
        }
        std::stable_sort(all.begin(), all.end(), [](const Inst& a, const Inst& b) {
            if (a.sec != b.sec) return a.sec < b.sec;
            if (a.rank != b.rank) return a.rank > b.rank;
            return a.entry < b.entry;
        });
        for (const Inst& in : all) {
            if (!di.kept.isEmpty() && di.kept.last().sec == in.sec) di.dropped.append(qMakePair(in, di.kept.last()));
            else di.kept << in;
        }
        s->days.insert(dk, di);
        return di;
    }

    void noteTies(int docIdx, const DayInst& di, const QString& day) {
        const std::shared_ptr<SourceDoc> s = docs.at(docIdx);
        for (const auto& p : di.dropped) {
            const Entry& dh = s->doc.header(s->doc.events.at(p.first.event));
            const Entry& kh = s->doc.header(s->doc.events.at(p.second.event));
            const QString why = o.samePriority || p.first.priority == p.second.priority
                                    ? QStringLiteral("equal priority: the first event in the file is kept")
                                    : QStringLiteral("a higher priority (%1 over %2) wins").arg(priorityName(p.second.priority), priorityName(p.first.priority));
            note("MP015", s->path, dh.line, 1,
                 QStringLiteral("event \"%1\" dropped: \"%2\" (line %3) starts at the same time; %4")
                     .arg(dh.stamp.raw, kh.stamp.raw, QString::number(kh.line), why),
                 day);
        }
    }

    struct Carry {
        Inst inst;
        int days = 0;
    };
    /// The event in force at the end of the day before `day`: the latest kept event of the nearest earlier day within a
    /// week (the grid week is cyclic: 7 days back is the same weekday), else on a date the latest dated event before.
    std::optional<Carry> carryInto(int docIdx, const Day& day) {
        for (int k = 1; k <= 7; ++k) {
            const DayInst p = kept(docIdx, day.addDays(-k));
            if (!p.kept.isEmpty()) return Carry{p.kept.last(), k};
        }
        if (!day.isDate()) return std::nullopt;
        const std::shared_ptr<SourceDoc> s = docs.at(docIdx);
        const int shift = std::clamp(o.timeShiftMin, -1440, 1440) * 60;
        const QDate limit = day.date.addDays(-7);
        std::optional<Carry> best;
        QDate bestDate;
        for (int i = 0; i < s->doc.events.size(); ++i) {
            const Entry& h = s->doc.header(s->doc.events.at(i));
            if (h.stamp.kind != DayKind::Dated) continue;
            const int total = h.stamp.secondOfDay() + shift;
            const int delta = floorDiv(total, kDaySec);
            const QDate land = h.stamp.date.addDays(delta);
            const int sec = total - delta * kDaySec;
            if (land >= limit) continue;   // nearer ones were candidates of the days above
            if (best && (land < bestDate || (land == bestDate && sec <= best->inst.sec))) continue;   // equal: the first wins
            best = Carry{Inst{i, sec, 3, 3, s->doc.events.at(i).entry}, int(land.daysTo(day.date))};
            bestDate = land;
        }
        return best;
    }

    std::optional<Carry> inForce(int docIdx, const Day& day, int sec, const QString& noteDay) {
        const DayInst di = kept(docIdx, day);
        noteTies(docIdx, di, noteDay);
        for (qsizetype i = di.kept.size() - 1; i >= 0; --i)
            if (di.kept.at(i).sec <= sec) return Carry{di.kept.at(i), 0};
        return carryInto(docIdx, day);
    }

    Window makeWindow(int docIdx, const Inst& in, int start, int carryDays, int changeDepth) {
        const std::shared_ptr<SourceDoc> s = docs.at(docIdx);
        const Entry& h = s->doc.header(s->doc.events.at(in.event));
        Window w;
        w.startSec = start;
        w.file = s->path;
        w.eventIndex = in.event;
        w.line = h.line;
        w.stamp = h.stamp;
        w.priority = in.priority;
        w.eventSec = in.sec;
        w.carry = carryDays > 0;
        w.carryDays = carryDays;
        w.offsetSec = w.carry ? qint64(carryDays) * kDaySec - in.sec + start : 0;
        w.eventType = h.eventType;
        w.name = h.name;
        w.eventDurationSec = h.eventDurationSec;
        w.expectedSec = h.expectedSec;
        w.headerPlayMode = h.playMode;
        w.changeDepth = changeDepth;
        return w;
    }

    // ---- expansion -------------------------------------------------------------------------------------------------

    void walk(Walk& c, int docIdx, const QList<int>& entries, int depth, int parent, const QString& group) {
        const std::shared_ptr<SourceDoc> s = docs.at(docIdx);
        for (const int idx : entries) {
            if (c.cut) { ++c.cutItems; continue; }   // later lines never run (design §2.7.7)
            if (c.limited) return;
            const Entry& e = s->doc.entries.at(idx);
            if (c.items->size() >= o.maxItems) {
                c.limited = true;
                note("MP023", s->path, e.line, 1,
                     QStringLiteral("expansion stopped at %1 items (maxItems): nested !InsertPlaylist lines fan out too far; the rest of "
                                    "the event is not expanded").arg(o.maxItems),
                     c.noteDay);
                return;
            }
            Item it;
            it.entry = e;
            it.file = s->path;
            it.depth = depth;
            it.parent = parent;
            it.group = group;
            if (e.kind == LineKind::Instruction) {
                const Instruction ins = identify(e);
                it.instr = ins.id;
                if (ins.id == Instr::ChangePlaylist) {
                    it.target = ins.head.trimmed();
                    it.status = QStringLiteral("notFollowed");
                    c.items->append(it);
                    c.cutAt = int(c.items->size()) - 1;
                    c.changeDoc = docIdx;
                    c.cut = true;
                    continue;
                }
                if (ins.id == Instr::InsertPlaylist) {
                    const std::optional<InsertPlaylistSpec> spec = insertPlaylistSpec(e, o.parse);
                    it.target = spec ? spec->path.trimmed() : ins.head.trimmed();
                    const int me = int(c.items->size());
                    c.items->append(it);
                    insert(c, docIdx, me, depth, group, ins.headCol > 0 ? ins.headCol : 2);
                    (*c.items)[me].children = int(c.items->size()) - me - 1;
                    continue;
                }
            }
            c.items->append(it);
        }
    }

    void insert(Walk& c, int docIdx, int me, int depth, const QString& group, int col) {
        const std::shared_ptr<SourceDoc> s = docs.at(docIdx);
        auto item = [&c, me]() -> Item& { return (*c.items)[me]; };   // by index: walk() appends to the list
        const int line = item().entry.line;
        const QString target = item().target;
        if (!o.expandSubPlaylists) { item().status = QStringLiteral("off"); return; }
        if (target.isEmpty()) {
            item().status = QStringLiteral("missing");
            item().why = QStringLiteral("no playlist named");
            note("MP024", s->path, line, col, QStringLiteral("!InsertPlaylist names no playlist: nothing is inserted"), c.noteDay);
            return;
        }
        if (depth + 1 > o.maxDepth) {
            item().status = QStringLiteral("depth");
            note("MP023", s->path, line, col,
                 QStringLiteral("!InsertPlaylist \"%1\" nests deeper than %2 levels: not expanded (a cycle through other names?)")
                     .arg(target, QString::number(o.maxDepth)),
                 c.noteDay);
            return;
        }
        const int sub = open(target, docIdx);
        const std::shared_ptr<SourceDoc> t = docs.at(sub);
        item().targetPath = t->path;
        if (!t->ok) {
            item().status = QStringLiteral("missing");
            item().why = t->why;
            note("MP024", s->path, line, col,
                 QStringLiteral("sub-playlist \"%1\" could not be read (%2): nothing is inserted, the hour fill takes over").arg(target, t->why),
                 c.noteDay);
            return;
        }
        if (c.chain.contains(t->key)) {
            item().status = QStringLiteral("cycle");
            QStringList names;
            for (const QString& k : c.chain) {
                const int di = byKey.value(k, -1);
                names << (di >= 0 ? QFileInfo(docs.at(di)->path).fileName() : k);
            }
            names << QFileInfo(t->path).fileName();
            note("MP023", s->path, line, col,
                 QStringLiteral("sub-playlist cycle: %1; \"%2\" is not expanded again").arg(names.join(QStringLiteral(" -> ")), target), c.noteDay);
            return;
        }
        // The target's event in force where the inserting event starts; a header-less list inlines its preamble.
        QList<int> entries;
        int subMode = 1, subLine = 0;
        if (const std::optional<Carry> inf = inForce(sub, c.subDay, c.subSec, c.noteDay)) {
            const Event& ev = t->doc.events.at(inf->inst.event);
            entries = ev.items;
            subLine = t->doc.header(ev).line;
            const int hm = t->doc.header(ev).playMode;
            subMode = hm > 0 ? hm : 1;
        } else {
            entries = t->doc.preamble;
        }
        item().subEventLine = subLine;
        item().subPlayMode = subMode;
        item().status = entries.isEmpty() ? QStringLiteral("empty") : QStringLiteral("expanded");
        const QString childGroup = shortHash(group + QLatin1Char('|') + s->key + QLatin1Char('#') + QString::number(line) + QLatin1Char('|') + t->key);
        c.chain << t->key;
        walk(c, sub, entries, depth + 1, me, childGroup);
        c.chain.removeLast();
    }

    struct Decision {
        int mode = 1;
        QString from;
        int implicitLoop = -1;
    };
    /// The first play-mode marker among the items of one level [begin, end) decides; an !InsertPlaylist whose target was
    /// read (expanded or empty) hands the decision to its target's lines, whose fallback is that target event's header
    /// mode. fallbackLoop: where an implicit loop (repeat without a !Repeat line) starts at this level.
    Decision decide(const QList<Item>& items, int begin, int end, int depth, int fallback, const QString& fallbackFrom, int fallbackLoop) {
        for (int i = begin; i < end; ++i) {
            const Item& it = items.at(i);
            if (it.depth != depth || it.viaChange || it.entry.kind != LineKind::Instruction) continue;
            const InstrInfo* info = instructionInfo(it.instr);
            if (!info || !info->isMarker()) continue;
            if (info->playMode > 0) return Decision{info->playMode, depth == 0 ? QStringLiteral("inBand") : QStringLiteral("sub"), -1};
            if (it.isInsert() && (it.status == u"expanded" || it.status == u"empty"))   // read: the target decides
                return decide(items, i + 1, i + 1 + it.children, depth + 1, it.subPlayMode > 0 ? it.subPlayMode : 1, QStringLiteral("sub"),
                              it.children > 0 ? i + 1 : -1);
            break;   // an unread target (or !Package) would decide; what it says is not known: the fallback
        }
        return Decision{fallback, fallbackFrom, fallback == 2 ? fallbackLoop : -1};
    }

    /// Expands one window's event. Returns the docs index of the playlist holding the !ChangePlaylist (-1 none).
    int expand(Window& w, int docIdx, const Day& eventDay, const QString& noteDay) {
        const std::shared_ptr<SourceDoc> s = docs.at(docIdx);
        const Event& ev = s->doc.events.at(w.eventIndex);
        Walk c;
        c.items = &w.items;
        c.subDay = eventDay;
        c.subSec = w.eventSec;
        c.noteDay = noteDay;
        c.chain << s->key;
        walk(c, docIdx, ev.items, 0, -1, QString());
        w.cutAt = c.cutAt;
        w.cutItems = c.cutItems;
        w.limited = c.limited;
        const int hm = w.headerPlayMode;
        const Decision dec = decide(w.items, 0, int(w.items.size()), 0, hm > 0 ? hm : 1,
                                    hm > 0 ? QStringLiteral("header") : QStringLiteral("default"), 0);
        w.playMode = dec.mode;
        w.playModeFrom = dec.from;
        for (int i = 0; i < w.items.size(); ++i)
            if (w.items.at(i).instr == Instr::Repeat && !w.items.at(i).viaChange) { w.repeatAt = i; break; }
        w.loopFrom = w.repeatAt >= 0 ? w.repeatAt + 1 : (w.playMode == 2 ? dec.implicitLoop : -1);
        return c.changeDoc;
    }

    /// Follows the window's !ChangePlaylist when allowed; replaces the windows after `i`. False (and MP017) otherwise.
    bool follow(QList<Window>& ws, int i, int changeDoc, const Day& day, int changeDepth, const QStringList& changeChain) {
        Window& w = ws[i];
        Item& ch = w.items[w.cutAt];
        const QString file = ch.file;
        const int line = ch.entry.line;
        const Instruction ins = identify(ch.entry);
        const int col = ins.headCol > 0 ? ins.headCol : 2;
        const QString label = day.label();
        auto refuse = [&](const QString& why, const QString& message) {
            Item& c2 = ws[i].items[ws[i].cutAt];
            c2.status = QStringLiteral("notFollowed");
            c2.why = why;
            note("MP017", file, line, col, message, label);
            return false;
        };
        const QString target = ch.target;
        const QString after = w.cutItems > 0 ? QStringLiteral("; %1 line(s) after it never run").arg(w.cutItems) : QString();
        if (target.isEmpty())
            return refuse(QStringLiteral("no playlist named"), QStringLiteral("!ChangePlaylist names no playlist: the event's content ends here") + after);
        if (!o.followChangePlaylist)
            return refuse(QStringLiteral("followChangePlaylist is off"),
                          QStringLiteral("!ChangePlaylist \"%1\" not followed (followChangePlaylist is off): the event's content ends here").arg(target) + after);
        if (changeDepth + 1 > o.maxDepth)
            return refuse(QStringLiteral("more than %1 ChangePlaylist hops").arg(o.maxDepth),
                          QStringLiteral("!ChangePlaylist \"%1\" not followed: more than %2 hops").arg(target, QString::number(o.maxDepth)) + after);
        const int sub = open(target, changeDoc >= 0 ? changeDoc : 0);
        const std::shared_ptr<SourceDoc> t = docs.at(sub);
        ws[i].items[ws[i].cutAt].targetPath = t->path;
        if (!t->ok)
            return refuse(QStringLiteral("missing: ") + t->why,
                          QStringLiteral("!ChangePlaylist \"%1\" not followed: it could not be read (%2)").arg(target, t->why) + after);
        if (changeChain.contains(t->key))
            return refuse(QStringLiteral("cycle"),
                          QStringLiteral("!ChangePlaylist \"%1\" not followed: the change chain comes back to a playlist already followed").arg(target) + after);
        DayTimeline x = buildDay(sub, day, changeDepth + 1, changeChain + QStringList{t->key});
        Window& w2 = ws[i];
        Item& c2 = w2.items[w2.cutAt];
        c2.status = QStringLiteral("followed");
        c2.why.clear();
        const int base = int(w2.items.size()), cutAt = w2.cutAt;
        const QString followGroup = shortHash(c2.group + QLatin1Char('|') + fileKey(file) + QLatin1Char('#') + QString::number(line) + QLatin1Char('|') + t->key);
        const int changeItemDepth = c2.depth;
        if (const Window* xin = x.at(w2.startSec)) {
            c2.targetPlayMode = xin->playMode;
            c2.targetLoopFrom = xin->loopFrom >= 0 ? base + xin->loopFrom : -1;
            QList<Item> add = xin->items;
            for (Item& a : add) {
                a.depth += changeItemDepth + 1;
                a.parent = a.parent >= 0 ? a.parent + base : cutAt;
                a.group = a.group.isEmpty() ? followGroup : shortHash(followGroup + QLatin1Char('|') + a.group);
                a.viaChange = true;
                if (a.isChange() && a.targetLoopFrom >= 0) a.targetLoopFrom += base;
            }
            w2.items += add;
            w2.items[cutAt].children = int(add.size());
        }
        const int replaced = int(ws.size()) - i - 1;
        ws.erase(ws.begin() + i + 1, ws.end());
        const QString via = file + QLatin1Char('#') + QString::number(line);
        for (const Window& xw : x.windows) {
            if (xw.startSec <= ws[i].startSec) continue;
            Window cw = xw;
            if (cw.via.isEmpty()) cw.via = via;
            ws << cw;
        }
        ws[i].items[cutAt].replacedWindows = replaced;
        ws[i].endSec = i + 1 < ws.size() ? ws[i + 1].startSec : kDaySec;
        return true;
    }

    DayTimeline buildDay(int docIdx, const Day& day, int changeDepth, const QStringList& changeChain) {
        DayTimeline t;
        t.day = day;
        const QString label = day.label();
        const DayInst di = kept(docIdx, day);
        noteTies(docIdx, di, label);
        t.datedSkipped = di.datedSkipped;
        const std::shared_ptr<SourceDoc> s = docs.at(docIdx);
        for (const auto& p : di.dropped) {
            const Entry& dh = s->doc.header(s->doc.events.at(p.first.event));
            const Entry& kh = s->doc.header(s->doc.events.at(p.second.event));
            t.dropped << Dropped{s->path, dh.line, p.first.event, p.first.sec, p.first.priority, dh.stamp.raw, kh.line, p.second.priority, kh.stamp.raw};
        }
        QList<Window> ws;
        if (di.kept.isEmpty() || di.kept.first().sec > 0)
            if (const std::optional<Carry> c = carryInto(docIdx, day)) ws << makeWindow(docIdx, c->inst, 0, c->days, changeDepth);
        for (const Inst& in : di.kept) ws << makeWindow(docIdx, in, in.sec, 0, changeDepth);
        for (int i = 0; i < ws.size(); ++i) ws[i].endSec = i + 1 < ws.size() ? ws[i + 1].startSec : kDaySec;
        for (int i = 0; i < ws.size(); ++i) {
            const Day eventDay = ws[i].carry ? day.addDays(-ws[i].carryDays) : day;
            const int changeDoc = expand(ws[i], docIdx, eventDay, label);
            if (ws[i].cutAt < 0) continue;
            if (follow(ws, i, changeDoc, day, changeDepth, changeChain)) break;   // the target's windows are complete
        }
        t.windows = ws;
        return t;
    }

    void accumulate(const DayTimeline& t) {
        ++days;
        dropped += int(t.dropped.size());
        for (const Window& w : t.windows) {
            ++windows;
            if (w.carry) ++carry;
            if (w.limited) ++limited;
            cutItems += w.cutItems;
            items += int(w.items.size());
            for (const Item& it : w.items) {
                maxDepth = qMax(maxDepth, it.depth);
                if (it.isInsert() && !it.status.isEmpty()) ++inserts[it.status];
                else if (it.isChange() && !it.status.isEmpty()) ++changes[it.status];
            }
        }
    }
};

Builder::Builder(const Document& root, const QString& rootFile, const Options& o, ReadFn read) : d(std::make_unique<Impl>()) {
    d->o = o;
    d->o.maxDepth = std::clamp(o.maxDepth, 0, 8);
    d->o.maxItems = std::clamp(o.maxItems, 100, 1000000);
    d->o.timeShiftMin = std::clamp(o.timeShiftMin, -1440, 1440);
    d->o.dayChangeSec = std::clamp(o.dayChangeSec, 0, kDaySec - 1);
    d->read = std::move(read);
    auto s = std::make_shared<SourceDoc>();
    s->path = rootFile.trimmed();
    s->key = fileKey(s->path);   // "" for pasted text: no target ever has that key, so nothing matches it

    s->ok = true;
    s->root = true;
    s->doc = root;
    d->docs << s;
    d->byKey.insert(s->key, 0);
}

Builder::Builder(Builder&&) noexcept = default;
Builder& Builder::operator=(Builder&&) noexcept = default;
Builder::~Builder() = default;

const Options& Builder::options() const { return d->o; }

DayTimeline Builder::build(const Day& day) {
    if (!day.isValid()) return {};
    DayTimeline t = d->buildDay(0, day, 0, QStringList{d->docs.at(0)->key});
    d->accumulate(t);
    return t;
}

QList<DayTimeline> Builder::week() {
    QList<DayTimeline> out;
    for (int dow = 0; dow < 7; ++dow) out << build(Day::weekday(dow));
    return out;
}

QList<DayTimeline> Builder::dates(const QDate& from, const QDate& to) {
    QList<DayTimeline> out;
    if (!from.isValid() || !to.isValid() || to < from) return out;
    const qint64 n = std::min<qint64>(from.daysTo(to) + 1, 366);
    for (qint64 i = 0; i < n; ++i) out << build(Day::of(from.addDays(i)));
    return out;
}

const QList<Note>& Builder::notes() const { return d->notes; }

QJsonArray Builder::notesJson() const {
    QJsonArray a;
    for (const Note& n : d->notes) a.append(n.toJson());
    return a;
}

QJsonArray Builder::sourcesJson() const {
    QJsonArray a;
    for (const std::shared_ptr<SourceDoc>& s : d->docs) {
        QJsonObject o{{"path", s->path}, {"ok", s->ok}};
        if (s->root) o["root"] = true;
        if (!s->target.isEmpty()) o["target"] = s->target;
        if (!s->why.isEmpty()) o["why"] = s->why;
        if (s->ok) {
            if (s->bytes >= 0) { o["bytes"] = s->bytes; o["sha256"] = s->sha256; }
            o["encoding"] = encodingName(s->doc.encoding);
            o["events"] = int(s->doc.events.size());
            int items = int(s->doc.preamble.size());
            for (const Event& ev : s->doc.events) items += int(ev.items.size());
            o["items"] = items;
            int e = 0, w = 0, i = 0;
            for (const Diagnostic& x : s->doc.diagnostics) {
                if (x.severity == u"error") ++e;
                else if (x.severity == u"warning") ++w;
                else ++i;
            }
            o["diagnostics"] = QJsonObject{{"error", e}, {"warning", w}, {"info", i}};
        }
        a.append(o);
    }
    return a;
}

int Builder::reads() const { return d->reads; }

QJsonObject Builder::stats() const {
    auto counts = [](const QHash<QString, int>& h, const QStringList& keys) {
        QJsonObject o;
        for (const QString& k : keys) o[k] = h.value(k);
        return o;
    };
    return QJsonObject{
        {"days", d->days}, {"windows", d->windows}, {"carry", d->carry}, {"dropped", d->dropped}, {"items", d->items},
        {"maxDepth", d->maxDepth}, {"preambleItems", int(d->docs.at(0)->doc.preamble.size())},
        {"inserts", counts(d->inserts, {"expanded", "empty", "missing", "cycle", "depth", "off"})},
        {"changes", counts(d->changes, {"followed", "notFollowed"})}, {"cutItems", d->cutItems}, {"limited", d->limited},
        {"reads", d->reads}, {"sources", int(d->docs.size())}, {"notes", int(d->notes.size())}};
}

} // namespace mp::tl
