// MasterPlay export (slice MP-10): see mpexporter.h for the rules.
#include "mpexporter.h"
#include "queryblocks.h"
#include "schedulestore.h"

#include <QHash>
#include <QJsonDocument>
#include <QSet>
#include <algorithm>
#include <cmath>

namespace mp::ex {
namespace {

const QString kMasterplay = QStringLiteral("masterplay");

bool ieq(const QString& a, const QString& b) { return a.compare(b, Qt::CaseInsensitive) == 0; }
bool isUrl(const QString& s) { return s.contains(QLatin1String("://")); }

/// The last path component ("Promos" for "D:\Media\Promos\", "a.mpg" for "D:\x\a.mpg").
QString fileNameOf(const QString& path) {
    qsizetype end = path.size();
    while (end > 0 && (path.at(end - 1) == u'\\' || path.at(end - 1) == u'/')) --end;
    qsizetype start = end;
    while (start > 0 && path.at(start - 1) != u'\\' && path.at(start - 1) != u'/') --start;
    return path.mid(start, end - start);
}
QString baseNameOf(const QString& file) {
    const qsizetype dot = file.lastIndexOf(u'.');
    return dot > 0 ? file.left(dot) : file;
}
QString compact(const QJsonObject& o) { return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)); }
QString numberText(double v) {   // "48", "1.5": MaxAge= the way the editor writes whole hours
    if (std::abs(v - std::round(v)) < 1e-9) return QString::number(qint64(std::round(v)));
    return QString::number(v, 'f', 2);
}
Param param(const char* key, const QString& value) { return Param{QString::fromLatin1(key), value, false}; }

QString stampFormText(StampForm f) { return f == StampForm::Legacy ? QStringLiteral("legacy") : QStringLiteral("iso"); }

// ---- the walk --------------------------------------------------------------------------------------------------

struct Line {
    Entry e;
    QString slotId, itemId;
    bool raw = false;              // written from a provenance line
};
struct PendingNote {               // a Note whose output line is not known yet
    int line = -1;                 // index into Ev::lines (-1: the header)
    Note n;
};
struct Ev {
    Stamp stamp;                   // the day kind, weekday / date and time; raw empty (rendered in the chosen form)
    qint64 dayOrder = 0;           // grid: dow 0..6; dates: julian day; -1 a folded every-day event
    int sec = 0;                   // start second of the day
    QString name;
    int eventType = 0, playMode = 1;
    bool loops = false;            // a hand-made looping hour opened it: "!Repeat" first unless the lines hold one
    bool hasRepeat = false;
    bool provenance = false;       // opened by an imported event
    DayKind srcKind = DayKind::Invalid;   // its original stamp kind (foldEveryDay)
    QString src;                   // "<eventSrc>|<day>": tells an imported event from its neighbours
    QList<Line> lines;
    QList<PendingNote> notes;
    double contentSec = 0;
    QString text() const {         // what the fold compares: the lines as they will be written
        QStringList t;
        for (const Line& l : lines) t << renderEntry(l.e);
        return t.join(u'\n');
    }
};

struct Walk {
    const Options& o;
    ParseOptions po;
    QList<ini::PathRule> reversed; // station -> playlist
    QList<Ev> evs;
    int cur = -1;
    QList<QPair<QString, QJsonObject>> open;   // overlay layers up: (key, cue)
    QString lastSrc;               // extra.masterplay.src of the last item line written (join dedup)
    // stats
    int items = 0, rawLines = 0, canonical = 0, fresh = 0, copies = 0, picks = 0, joins = 0, shows = 0, offs = 0, queries = 0;

    explicit Walk(const Options& opt) : o(opt) {
        po.fps = o.fps;
        for (ini::PathRule r : o.pathMap) { std::swap(r.from, r.to); reversed << r; }
        reversed = ini::normalizeRules(reversed);
    }

    QString reversePath(const QString& p) const {
        if (p.isEmpty() || isUrl(p) || reversed.isEmpty()) return p;
        return ini::mapPath(p, reversed);
    }
    void note(int lineIdx, const QString& code, const QString& message, const QString& slotId, const QString& itemId) {
        PendingNote pn;
        pn.line = lineIdx;
        pn.n.diag = Diagnostic{0, 0, diagnosticInfo(code).severity, code, message};
        pn.n.slotId = slotId;
        pn.n.itemId = itemId;
        evs[cur].notes << pn;
    }
    /// A note on the line about to be appended (or the last one when `next` is false).
    void noteNext(const QString& code, const QString& message, const QString& slotId, const QString& itemId, bool next = true) {
        note(int(evs[cur].lines.size()) - (next ? 0 : 1), code, message, slotId, itemId);
    }
    void add(Line l) {
        l.e.eol = o.eol;
        evs[cur].lines << l;
    }
    Line rawLine(const QString& raw, const QString& slotId, const QString& itemId) const {
        Line l;
        l.e = parseLine(raw, po);
        l.slotId = slotId;
        l.itemId = itemId;
        l.raw = true;
        return l;
    }
    Line made(Entry e, const QString& slotId, const QString& itemId) const {
        Line l;
        l.e = std::move(e);
        l.slotId = slotId;
        l.itemId = itemId;
        return l;
    }

    // ---- days and stamps ----
    qint64 dayOrderOf(const sched::HourSlot& s) const { return s.isDated() ? s.date.toJulianDay() : s.dayOfWeek; }
    Stamp stampFor(const sched::HourSlot& s, qint64 dayOrder, int sec) const {
        Stamp st;
        st.time = QTime(0, 0).addSecs(sec);
        if (s.isDated()) { st.kind = DayKind::Dated; st.date = QDate::fromJulianDay(dayOrder); }
        else { st.kind = DayKind::Weekly; st.weekday = int(dayOrder) + 1; }   // MasterPlay codes 1 Sunday .. 7 Saturday
        return st;
    }
    qint64 previousDay(const sched::HourSlot& s, qint64 dayOrder) const { return s.isDated() ? dayOrder - 1 : (dayOrder + 6) % 7; }

    /// The provenance event record of an hour (showBlock.masterplay.events[]) for an item's eventSrc.
    QJsonObject provEvent(const sched::HourSlot& s, const QString& eventSrc) const {
        const QString file = eventSrc.section(u'#', 0, -2);
        const int line = eventSrc.section(u'#', -1).toInt();
        const QJsonArray arr = s.showBlock.value(kMasterplay).toObject().value(QStringLiteral("events")).toArray();
        for (const QJsonValue& v : arr) {
            const QJsonObject ev = v.toObject();
            if (ev.value(QStringLiteral("line")).toInt() != line) continue;
            const QString f = ev.value(QStringLiteral("file")).toString();
            if (!f.isEmpty() && !file.isEmpty() && !ieq(f, file)) continue;
            return ev;
        }
        return {};
    }

    void openEvent(const sched::HourSlot& s, qint64 dayOrder, int sec, const QString& name, int eventType, int playMode, bool loops, bool prov,
                   DayKind srcKind, const QString& src) {
        Ev ev;
        ev.stamp = stampFor(s, dayOrder, sec);
        ev.dayOrder = dayOrder;
        ev.sec = sec;
        ev.name = name;
        ev.eventType = eventType;
        ev.playMode = playMode;
        ev.loops = loops;
        ev.provenance = prov;
        ev.srcKind = srcKind;
        ev.src = src;
        evs << ev;
        cur = int(evs.size()) - 1;
    }

    /// Opens the event an item starts, when it starts one (the header comment's rules).
    void placeEvent(const sched::HourSlot& s, const sched::SlotItem& it, const QJsonObject& mp, bool cont, bool firstInSlot) {
        const qint64 day = dayOrderOf(s);
        const int hourSec = s.hour * 3600;
        const bool prov = mp.contains(QStringLiteral("raw"));
        if (prov) {
            const QString eventSrc = mp.value(QStringLiteral("eventSrc")).toString();
            const bool opens = mp.contains(QStringLiteral("anchor"));
            if (!opens && cur >= 0) return;                                   // a continuation, a copy, a pod line
            Stamp st;
            parseStamp(mp.value(QStringLiteral("event")).toString(), po, &st);
            const QJsonObject pe = provEvent(s, eventSrc);
            int sec = hourSec;
            qint64 d = day;
            if (opens) {
                sec = st.time.isValid() ? st.secondOfDay() : it.isHardStart() && it.fixedOffsetSec >= 0 ? hourSec + int(it.fixedOffsetSec) : hourSec;
                if (sec >= hourSec + 3600) d = previousDay(s, day);   // the event started the day before (a carry laid from its start)
            }
            const QString src = eventSrc + u'|' + QString::number(d);
            if (opens && cur >= 0 && evs[cur].src == src) return;
            const QJsonObject an = mp.value(QStringLiteral("anchor")).toObject();
            const int type = an.contains(QStringLiteral("eventType")) ? an.value(QStringLiteral("eventType")).toInt()
                             : pe.value(QStringLiteral("eventType")).toInt(0);
            const int mode = pe.value(QStringLiteral("playMode")).toInt(1);
            openEvent(s, d, sec, pe.value(QStringLiteral("name")).toString(), type, mode > 0 ? mode : 1, false, true, st.kind, src);
            return;
        }
        if (it.isHardStart() && it.fixedOffsetSec >= 0) {
            const int sec = hourSec + int(std::lround(it.fixedOffsetSec));
            if (cur >= 0 && evs[cur].dayOrder == day && evs[cur].sec == sec && !evs[cur].provenance) return;
            const bool loops = firstInSlot && !cont && s.isLooping;
            openEvent(s, day, sec, firstInSlot ? s.showBlock.value(QStringLiteral("title")).toString() : QString(),
                      it.extra.value(QStringLiteral("conflictPolicy")).toString() == QLatin1String("finish_previous") ? 1 : 0, loops ? 2 : 1, loops, false,
                      DayKind::Invalid, QString());
            return;
        }
        if (cur < 0 || (!cont && firstInSlot)) {
            const bool loops = firstInSlot && !cont && s.isLooping;
            openEvent(s, day, hourSec, s.showBlock.value(QStringLiteral("title")).toString(), 0, loops ? 2 : 1, loops, false, DayKind::Invalid, QString());
        }
    }

    // ---- overlays ----
    static QString cueKey(const QJsonObject& c) {
        const QString id = c.value(QStringLiteral("id")).toString();
        if (!id.isEmpty()) return id;
        return c.value(QStringLiteral("filePath")).toString() + u'|' + c.value(QStringLiteral("template")).toString(c.value(QStringLiteral("templateId")).toString())
               + u'|' + compact(c.value(QStringLiteral("params")).toObject());
    }
    static QList<QPair<QString, QJsonObject>> cuesOf(const sched::SlotItem& it) {
        QList<QPair<QString, QJsonObject>> out;
        for (const QJsonValue& v : it.cgOverlays) {
            const QJsonObject c = v.toObject();
            if (!c.isEmpty()) out << qMakePair(cueKey(c), c);
        }
        return out;
    }
    /// The .IN line that shows a cue (or a show hook's source); an empty entry when it has none.
    Entry cueLine(const QJsonObject& c, QString* why) const {
        const QString file = c.value(QStringLiteral("filePath")).toString(c.value(QStringLiteral("path")).toString());
        const QString role = c.value(QStringLiteral("role")).toString();
        const QString tpl = c.value(QStringLiteral("template")).toString(c.value(QStringLiteral("templateId")).toString(c.value(QStringLiteral("kind")).toString()));
        const QJsonObject params = c.value(QStringLiteral("params")).toObject();
        if (!file.isEmpty()) {
            const int layer = (role.isEmpty() || role == QLatin1String("bug")) ? 1 : 2;
            return makeInstruction(Instr::ShowLogo, {}, {param("f", reversePath(file)), param("Layer", QString::number(layer))});
        }
        auto text = [&](const char* key) { return params.value(QString::fromLatin1(key)).toString(c.value(QString::fromLatin1(key)).toString()); };
        if (tpl == QLatin1String("ticker")) {
            QString t = text("text");
            if (t.isEmpty()) t = text("title");
            t.replace(QStringLiteral("\r\n"), QStringLiteral("<br/>")).replace(u'\n', QStringLiteral("<br/>"));
            return makeInstruction(Instr::ShowCrawl, t.simplified());
        }
        if (tpl == QLatin1String("lower_third")) {
            QString t = text("title");
            if (t.isEmpty()) t = text("text");
            return makeInstruction(Instr::ShowText, t.simplified());
        }
        *why = tpl.isEmpty() ? QStringLiteral("a cue without an image or a ticker / lower_third template")
                             : QStringLiteral("cue template \"%1\"").arg(tpl);
        return Entry();
    }
    bool isOpen(const QString& key) const {
        for (const auto& p : open) if (p.first == key) return true;
        return false;
    }
    /// A new item's layers: "!OSD off" when a layer it no longer carries is up, then the shows it needs.
    void overlaysFor(const sched::SlotItem& it, const QString& slotId) {
        const auto cues = cuesOf(it);
        bool closing = false;
        for (const auto& p : open) {
            bool carried = false;
            for (const auto& c : cues) if (c.first == p.first) carried = true;
            if (!carried) closing = true;
        }
        if (closing) { add(made(makeInstruction(Instr::OsdOff), slotId, it.id)); open.clear(); ++offs; }
        for (const auto& c : cues) {
            if (isOpen(c.first)) continue;
            QString why;
            Entry e = cueLine(c.second, &why);
            if (e.kind == LineKind::Instruction) { add(made(e, slotId, it.id)); ++shows; }
            else noteNext(QStringLiteral("MP031"), QStringLiteral("%1 has no .IN form: the layer is not written").arg(why), slotId, it.id);
            open << c;   // up on this item either way
        }
        for (const QString& h : it.cgOnStart) {
            const QString t = h.trimmed();
            if (t.isEmpty()) continue;
            if (ieq(t, QStringLiteral("clock"))) { add(made(makeInstruction(Instr::ShowClock, {}, {param("t", QStringLiteral("0"))}), slotId, it.id)); ++shows; continue; }
            if (t.startsWith(u'{')) {
                const QJsonObject hook = QJsonDocument::fromJson(t.toUtf8()).object();
                if (hook.value(QStringLiteral("action")).toString() == QLatin1String("show")) {
                    QJsonObject src = hook.value(QStringLiteral("source")).toObject();
                    src[QStringLiteral("role")] = hook.value(QStringLiteral("role"));
                    QString why;
                    Entry e = cueLine(src, &why);
                    if (e.kind == LineKind::Instruction) { add(made(e, slotId, it.id)); ++shows; continue; }
                }
                noteNext(QStringLiteral("MP031"), QStringLiteral("cgOnStart hook %1 has no .IN form").arg(t.left(60)), slotId, it.id);
                continue;
            }
            noteNext(QStringLiteral("MP031"), QStringLiteral("cgOnStart \"%1\" is an identity graphic with no .IN form").arg(t), slotId, it.id);
        }
    }
    void overlaysAfter(const sched::SlotItem& it, const QString& slotId) {
        for (const QString& h : it.cgOnEnd) {
            const QString t = h.trimmed();
            const bool hide = ieq(t, QStringLiteral("clock"))
                              || (t.startsWith(u'{') && QJsonDocument::fromJson(t.toUtf8()).object().value(QStringLiteral("action")).toString() == QLatin1String("hide"));
            if (!hide) continue;
            add(made(makeInstruction(Instr::OsdOff), slotId, it.id));
            open.clear();
            ++offs;
            break;
        }
    }
    void syncOverlays(const sched::SlotItem& it) { open = cuesOf(it); }   // its provenance lines carry the layers

    // ---- rendering from fields ----
    void mediaLine(const sched::SlotItem& it, const QString& slotId, const QString& path) {
        Entry e;
        e.kind = LineKind::Media;
        e.path = reversePath(path);
        const double in = qMax(it.inPointSec, 0.0), out = it.outPointSec;
        if (out > in) {
            // The bracket forces a length from the file's start. With an in point the planned length (out - in) is
            // kept so the event's timing holds; the first out - in seconds play instead of in..out (MP031).
            const double len = out - in;
            e.bracketRaw = formatBracket(len, o.fps);
            e.forcedDurationSec = len;
            if (in > 0)
                noteNext(QStringLiteral("MP031"), QStringLiteral("in point %1 s has no .IN form: the line plays its first %2 s instead of %1..%3 s").arg(in).arg(len).arg(out),
                         slotId, it.id);
        } else if (in > 0)
            noteNext(QStringLiteral("MP031"), QStringLiteral("in point %1 s has no .IN form: the line plays from the start").arg(in), slotId, it.id);
        if (!it.segmentId.isEmpty()) {
            const int idx = o.segmentIndex ? o.segmentIndex(it.mediaId, it.segmentId) : -1;
            if (idx >= 0) e.params << param("actinout", QString::number(idx));
            else noteNext(QStringLiteral("MP031"), QStringLiteral("segment \"%1\" of %2 is not in the library's segments: the whole file plays").arg(it.segmentId, fileNameOf(path)), slotId, it.id);
        }
        const QString title = it.title.trimmed();
        if (!title.isEmpty() && !ieq(title, baseNameOf(fileNameOf(path)))) e.params << param("Name", title);
        e.dirty = true;
        e.raw = renderEntry(e);
        add(made(e, slotId, it.id));
    }

    void folderLine(const sched::SlotItem& it, const QJsonObject& mp, int n, const QString& slotId) {
        const sched::FolderBlock fb = sched::FolderBlock::fromJson(it.folder());
        QString starts, ends;
        for (const QJsonValue& v : fb.cond.rules) {
            const QJsonObject r = v.toObject();
            const QString field = r.value(QStringLiteral("field")).toString(), op = r.value(QStringLiteral("op")).toString();
            const QString value = r.value(QStringLiteral("value")).toString();
            if (field == QLatin1String("filename") && op == QLatin1String("starts_with") && starts.isEmpty()) starts = value;
            else if (field == QLatin1String("filename") && op == QLatin1String("ends_with") && ends.isEmpty()) ends = value;
            else noteNext(QStringLiteral("MP031"), QStringLiteral("folder rule %1 %2 has no .IN form").arg(field, op), slotId, it.id);
        }
        QString head = reversePath(fb.folder);
        if (!starts.isEmpty() || !ends.isEmpty()) {
            while (head.endsWith(u'\\') || head.endsWith(u'/')) head.chop(1);
            head += u'\\' + starts + u'*' + ends;
        }
        int mode = -1;
        const QString pm = mp.value(QStringLiteral("folder")).toObject().value(QStringLiteral("loadingMode")).toString();
        for (int m = 0; m <= 8 && !pm.isEmpty(); ++m)
            if (loadingModeName(LoadingMode(m)) == pm) mode = m;
        const bool named = fb.order == QLatin1String("name") || fb.order == QLatin1String("date") || fb.order == QLatin1String("size") || fb.order == QLatin1String("added");
        if (mode < 0) mode = fb.targetSec > 0 ? (named ? 0 : 1) : (named ? 2 : 3);
        QList<Param> ps;
        if (mode != 0) ps << param("LoadingMode", QString::number(mode));
        if (mode == 2 || mode == 3) ps << param("ToSelect", QString::number(n));
        if (fb.recursive) ps << param("SubDirs", QStringLiteral("1"));
        if (fb.cond.maxAgeDays > 0) ps << param("MaxAge", numberText(fb.cond.maxAgeDays * 24.0));
        if (fb.cond.minAgeDays > 0) noteNext(QStringLiteral("MP031"), QStringLiteral("minAgeDays has no .IN form"), slotId, it.id);
        add(made(makeInstruction(Instr::InsertDirectory, head, ps), slotId, it.id));
    }

    void liveLine(const sched::SlotItem& it, const QString& slotId) {
        QString url = it.liveSource.value(QStringLiteral("url")).toString();
        if (url.isEmpty()) url = it.path;
        if (url.isEmpty()) url = it.liveSourceId;
        if (url.isEmpty()) { noteNext(QStringLiteral("MP031"), QStringLiteral("live item without a source: left out"), slotId, it.id); return; }
        if (isUrl(url) && !url.startsWith(QLatin1String("ndi://"), Qt::CaseInsensitive))
            add(made(makeInstruction(Instr::LiveStream, {}, {param("Path", url)}), slotId, it.id));
        else add(made(makeInstruction(Instr::Video, url), slotId, it.id));
    }

    /// The picks' planned seconds when picks were written, -1 when the !Info line stands in.
    double queryLine(const sched::HourSlot& s, const sched::SlotItem& it) {
        ++queries;
        const QList<sched::SlotItem> picks = o.queryPicks ? o.queryPicks(s, it) : QList<sched::SlotItem>();
        if (!picks.isEmpty()) {
            double sec = 0;
            for (const sched::SlotItem& p : picks) {
                if (p.path.isEmpty()) continue;
                mediaLine(p, s.id, p.path);
                sec += p.effectiveDurationSec();
            }
            return sec;
        }
        QString label = it.title.trimmed();
        if (label.isEmpty()) label = it.lockLabel.trimmed();
        if (label.isEmpty()) label = sched::QueryBlock::fromJson(it.query()).label();
        noteNext(QStringLiteral("MP030"), QStringLiteral("query block \"%1\" has no .IN form and no picks were given: an !Info line marks its place").arg(label), s.id, it.id);
        add(made(makeInstruction(Instr::Info, {}, {param("Name", QStringLiteral("query block %1 not exported").arg(label))}), s.id, it.id));
        return -1;
    }

    /// The item's own line(s) from its fields. Returns the seconds the lines stand for when that is not the item's own
    /// planned length (query picks, or 0 for an item left out), else -1.
    double canonicalLines(const sched::HourSlot& s, const sched::SlotItem& it, const QJsonObject& mp, int n) {
        if (!it.query().isEmpty() && it.folder().isEmpty()) return queryLine(s, it);
        if (!it.folder().isEmpty()) { folderLine(it, mp, n, s.id); return -1; }
        if (it.type == QLatin1String("live")) { liveLine(it, s.id); return -1; }
        if (it.type == QLatin1String("cg") || it.type == QLatin1String("gap")) {
            noteNext(QStringLiteral("MP031"), QStringLiteral("%1 item \"%2\" has no .IN form: left out").arg(it.type, it.title), s.id, it.id, false);
            return 0;
        }
        if (it.path.isEmpty()) {
            noteNext(QStringLiteral("MP031"), QStringLiteral("item \"%1\" (%2) has no path: left out").arg(it.title, it.mediaId.isEmpty() ? QStringLiteral("no media") : it.mediaId), s.id, it.id, false);
            return 0;
        }
        mediaLine(it, s.id, it.path);
        return -1;
    }

    /// The provenance lines around an item: before (instructions) and after. carried receives the lines a later event
    /// start moved here (they belong to the event they came from).
    void provLines(const QJsonArray& a, const QString& slotId, const QString& itemId, QList<Line>* carried, QList<Line>* out) const {
        for (const QJsonValue& v : a) {
            const QJsonObject l = v.toObject();
            const QString raw = l.value(QStringLiteral("raw")).toString();
            if (raw.isEmpty() && !l.contains(QStringLiteral("raw"))) continue;
            Line ln = rawLine(raw, slotId, itemId);
            if (l.contains(QStringLiteral("carriedFrom")) && carried) *carried << ln;
            else *out << ln;
        }
    }

    /// groupSec: the planned seconds of the item (a folder line: of all its picks). basePick: the provenance pick
    /// number this group starts at (> 0: the line's earlier picks are gone).
    void item(const sched::HourSlot& s, const sched::SlotItem& it, const QJsonObject& mp, int n, bool cont, bool firstInSlot, double groupSec, int basePick) {
        const bool prov = mp.contains(QStringLiteral("raw"));
        QList<Line> carried, before, after;
        provLines(mp.value(QStringLiteral("instructions")).toArray(), s.id, it.id, &carried, &before);
        provLines(mp.value(QStringLiteral("after")).toArray(), s.id, it.id, nullptr, &after);
        const int was = cur;
        placeEvent(s, it, mp, cont, firstInSlot);
        if (!carried.isEmpty()) {
            if (was >= 0) for (Line& l : carried) { l.e.eol = o.eol; evs[was].lines << l; }
            else before = carried + before;
        }
        for (const Line& l : before) { add(l); if (isRepeat(l.e)) evs[cur].hasRepeat = true; }
        ++items;
        bool isUntouched = prov && o.rawLines && Exporter::untouched(it, po);
        if (isUntouched && !it.folder().isEmpty())   // its line still holds every pick, from the first
            isUntouched = basePick == 0 && mp.value(QStringLiteral("folder")).toObject().value(QStringLiteral("picks")).toInt(1) == n;
        double sec = -1;
        if (isUntouched) {
            add(rawLine(mp.value(QStringLiteral("raw")).toString(), s.id, it.id));
            ++rawLines;
            syncOverlays(it);
        } else if (prov) {
            sched::SlotItem x = it;
            if (mp.contains(QStringLiteral("joinSec"))) {
                noteNext(QStringLiteral("MP031"), QStringLiteral("\"%1\" joined this hour in progress: written from its start").arg(it.title), s.id, it.id);
                x.inPointSec = qMax(0.0, x.inPointSec - mp.value(QStringLiteral("joinSec")).toDouble());   // the hour's offset, not an edit
            }
            if (basePick > 0)
                noteNext(QStringLiteral("MP031"), QStringLiteral("the first %1 pick(s) of folder line \"%2\" are gone or no longer follow it: the rest is written as a line of its own")
                                                      .arg(basePick).arg(fileNameOf(sched::FolderBlock::fromJson(it.folder()).folder)), s.id, it.id);
            sec = canonicalLines(s, x, mp, n);
            ++canonical;
            syncOverlays(it);
        } else {
            overlaysFor(it, s.id);
            const QString note = it.note.trimmed();
            if (!note.isEmpty()) add(made(makeInstruction(Instr::Info, {}, {param("Name", note)}), s.id, it.id));
            sec = canonicalLines(s, it, mp, n);
            overlaysAfter(it, s.id);
            ++fresh;
        }
        evs[cur].contentSec += sec >= 0 ? sec : groupSec;
        for (const Line& l : after) add(l);
        lastSrc = mp.value(QStringLiteral("src")).toString();
    }

    void slot(const sched::HourSlot& s) {
        const bool cont = s.isContinuation();
        bool firstInSlot = true;
        const int size = int(s.items.size());
        for (int i = 0; i < size;) {
            const sched::SlotItem& it = s.items[i];
            const QJsonObject mp = it.extra.value(kMasterplay).toObject();
            if (mp.value(QStringLiteral("copy")).toInt() > 0) { ++copies; ++i; continue; }
            int n = 1, basePick = 0;
            if (!it.folder().isEmpty()) {
                const QJsonObject fo = mp.value(QStringLiteral("folder")).toObject();
                if (!fo.isEmpty()) {
                    // The line's picks that follow this one in order form one group. A group starting at a pick > 0
                    // lost its earlier picks (deleted, or an item put between): it is a folder line of its own, with
                    // its own ToSelect (never raw, MP031), so no pick vanishes silently.
                    basePick = fo.value(QStringLiteral("pick")).toInt();
                    const QString src = mp.value(QStringLiteral("src")).toString(), ev = mp.value(QStringLiteral("eventSrc")).toString();
                    while (i + n < size) {
                        const QJsonObject m2 = s.items[i + n].extra.value(kMasterplay).toObject();
                        if (m2.value(QStringLiteral("src")).toString() != src || m2.value(QStringLiteral("eventSrc")).toString() != ev
                            || m2.value(QStringLiteral("folder")).toObject().value(QStringLiteral("pick")).toInt() != basePick + n || m2.value(QStringLiteral("copy")).toInt() > 0)
                            break;
                        ++n;
                    }
                } else if (o.mergeFolderPicks && sched::FolderBlock::fromJson(it.folder()).targetSec <= 0) {
                    while (i + n < size) {
                        const sched::SlotItem& x = s.items[i + n];
                        if (x.folder() != it.folder() || x.extra.contains(kMasterplay) || x.isHardStart() || x.cgOverlays != it.cgOverlays || !x.note.trimmed().isEmpty()) break;
                        ++n;
                    }
                }
            }
            if (mp.contains(QStringLiteral("joinSec")) && !lastSrc.isEmpty() && mp.value(QStringLiteral("src")).toString() == lastSrc) { ++joins; i += n; continue; }
            double groupSec = 0;
            for (int k = 0; k < n; ++k) {
                const sched::SlotItem& x = s.items[i + k];
                groupSec += x.folder().isEmpty() && x.type != QLatin1String("live") ? x.effectiveDurationSec() : x.durationSec;
            }
            item(s, it, mp, n, cont, firstInSlot, groupSec, basePick);
            picks += n - 1;
            firstInSlot = false;
            i += n;
        }
    }
};

int kindRank(DayKind k) { return k == DayKind::EveryDay ? 0 : k == DayKind::Weekly ? 1 : 2; }

} // namespace

// ---- Options ------------------------------------------------------------------------------------------------------

Options Options::fromJson(const QJsonObject& j, QStringList* errors) {
    Options o;
    auto bad = [&](const QString& m) { if (errors) *errors << m; };
    o.channelId = j.value(QStringLiteral("channel")).toString(j.value(QStringLiteral("channelId")).toString());
    if (j.contains(QStringLiteral("mode"))) {
        const QString m = j.value(QStringLiteral("mode")).toString();
        if (m == QLatin1String("grid") || m == QLatin1String("dates")) o.mode = m;
        else bad(QStringLiteral("mode must be grid or dates"));
    }
    if (j.contains(QStringLiteral("dateStyle"))) {
        const auto f = stampFormFromName(j.value(QStringLiteral("dateStyle")).toString());
        if (f && *f != StampForm::Keep) o.dateStyle = *f;
        else bad(QStringLiteral("dateStyle must be iso or legacy"));
    }
    if (j.contains(QStringLiteral("encoding"))) {
        const auto e = encodingFromName(j.value(QStringLiteral("encoding")).toString());
        if (e) o.encoding = *e;
        else bad(QStringLiteral("encoding must be utf8bom, utf8, ansi, utf16le or utf16be"));
    }
    if (j.contains(QStringLiteral("eol"))) {
        const QString e = j.value(QStringLiteral("eol")).toString().toLower();
        if (e == QLatin1String("crlf") || e == QLatin1String("\r\n")) o.eol = QByteArrayLiteral("\r\n");
        else if (e == QLatin1String("lf") || e == QLatin1String("\n")) o.eol = QByteArrayLiteral("\n");
        else bad(QStringLiteral("eol must be crlf or lf"));
    }
    auto flag = [&](const char* key, bool* v) {
        const QString k = QString::fromLatin1(key);
        if (!j.contains(k)) return;
        if (j.value(k).isBool()) *v = j.value(k).toBool();
        else bad(QStringLiteral("%1 must be true or false").arg(k));
    };
    flag("finalNewline", &o.finalNewline);
    flag("rawLines", &o.rawLines);
    flag("foldEveryDay", &o.foldEveryDay);
    flag("mergeFolderPicks", &o.mergeFolderPicks);
    if (j.contains(QStringLiteral("fps"))) {
        const int f = j.value(QStringLiteral("fps")).toInt();
        if (f >= 1 && f <= 120) o.fps = f;
        else bad(QStringLiteral("fps must be 1..120"));
    }
    if (j.contains(QStringLiteral("pathMap"))) {
        if (!j.value(QStringLiteral("pathMap")).isArray()) bad(QStringLiteral("pathMap must be an array of {from, to}"));
        for (const QJsonValue& v : j.value(QStringLiteral("pathMap")).toArray()) {
            const ini::PathRule r = ini::PathRule::fromJson(v.toObject());
            if (!r.from.isEmpty() && !r.to.isEmpty()) o.pathMap << r;
        }
    }
    return o;
}

QJsonObject Options::toJson() const {
    QJsonArray rules;
    for (const ini::PathRule& r : pathMap) rules.append(r.toJson());
    return QJsonObject{{"channel", channelId}, {"mode", mode}, {"dateStyle", stampFormText(dateStyle)}, {"encoding", encodingName(encoding)},
                       {"eol", eol == "\n" ? QStringLiteral("lf") : QStringLiteral("crlf")}, {"finalNewline", finalNewline}, {"rawLines", rawLines},
                       {"foldEveryDay", foldEveryDay}, {"mergeFolderPicks", mergeFolderPicks}, {"fps", fps}, {"pathMap", rules}};
}

QStringList Options::check() const {
    QStringList e;
    if (mode != QLatin1String("grid") && mode != QLatin1String("dates")) e << QStringLiteral("mode must be grid or dates");
    if (dateStyle == StampForm::Keep) e << QStringLiteral("dateStyle must be iso or legacy");
    if (eol != "\r\n" && eol != "\n") e << QStringLiteral("eol must be crlf or lf");
    if (fps < 1 || fps > 120) e << QStringLiteral("fps must be 1..120");
    return e;
}

// ---- Note / Result ------------------------------------------------------------------------------------------------

QJsonObject Note::toJson() const {
    QJsonObject o = diag.toJson();
    if (!slotId.isEmpty()) o["slotId"] = slotId;
    if (!itemId.isEmpty()) o["itemId"] = itemId;
    return o;
}

QJsonObject Result::toJson(bool withText) const {
    QJsonArray ds;
    for (const Note& n : diagnostics) ds.append(n.toJson());
    QJsonArray es;
    for (const QString& e : errors) es.append(e);
    QJsonObject o{{"ok", ok()}, {"filename", filename}, {"stats", stats}, {"diagnostics", ds}, {"errors", es}};
    if (withText) o["text"] = text;
    return o;
}

// ---- Exporter -----------------------------------------------------------------------------------------------------

Exporter::Exporter(const Options& o) : m_o(o) {}

bool Exporter::untouched(const sched::SlotItem& item, const ParseOptions& po) {
    const QJsonObject mp = item.extra.value(kMasterplay).toObject();
    const QString raw = mp.value(QStringLiteral("raw")).toString();
    if (raw.isEmpty()) return false;
    const Entry e = parseLine(raw, po);
    auto liveSame = [&] {
        const QJsonObject live = mp.value(QStringLiteral("live")).toObject();
        return item.type == QLatin1String("live") && !live.isEmpty() && live.value(QStringLiteral("path")).toString() == item.path;
    };
    if (e.kind == LineKind::Media) {
        if (!item.folder().isEmpty() || !item.query().isEmpty()) return false;
        if (item.type == QLatin1String("live")) return liveSame();
        if (item.path.isEmpty() || !ieq(fileNameOf(e.path), fileNameOf(item.path))) return false;
        if (e.forcedDurationSec >= 0) {
            if (item.inPointSec > 0 || std::abs(item.outPointSec - e.forcedDurationSec) > 0.001) return false;
        } else if (item.outPointSec > 0 || item.inPointSec > 0) return false;
        const QString name = e.value(QStringLiteral("Name")).trimmed();
        if (!name.isEmpty() && name != item.title.trimmed()) return false;
        if (!e.has(QStringLiteral("actinout")) && !item.segmentId.isEmpty()) return false;
        return true;
    }
    if (e.kind == LineKind::Instruction) {
        const Instruction ins = identify(e);
        if (ins.id == Instr::InsertDirectory || ins.id == Instr::InsertTopinDirectory) {
            if (item.folder().isEmpty() || !mp.contains(QStringLiteral("folder"))) return false;
            const std::optional<InsertDirectorySpec> spec = insertDirectorySpec(e, po);
            if (!spec) return false;
            const sched::FolderBlock fb = sched::FolderBlock::fromJson(item.folder());
            const QString dir = spec->mask.isEmpty() ? spec->directory : spec->folder;
            return ieq(fileNameOf(dir), fileNameOf(fb.folder)) && fb.recursive == spec->recursive();
        }
        if (ins.id == Instr::Video || ins.id == Instr::LiveStream) return liveSame();
    }
    return false;
}

Result Exporter::run(const QList<sched::HourSlot>& input) const {
    Result r;
    r.errors = m_o.check();
    const bool dates = m_o.mode == QLatin1String("dates");
    QList<sched::HourSlot> sorted = input;   // not "slots": a Qt keyword macro (QT_NO_KEYWORDS is off)
    for (const sched::HourSlot& s : sorted) {
        if (dates && !s.isDated()) r.errors << QStringLiteral("dates export given the template slot %1").arg(s.id);
        if (!dates && (s.isDated() || s.dayOfWeek < 0 || s.dayOfWeek > 6)) r.errors << QStringLiteral("grid export given the dated slot %1").arg(s.id);
        if (s.hour < 0 || s.hour > 23) r.errors << QStringLiteral("slot %1: bad hour").arg(s.id);
    }
    r.errors.removeDuplicates();
    const QString ch = m_o.channelId.isEmpty() ? QStringLiteral("export") : m_o.channelId;
    if (!r.ok()) return r;
    std::stable_sort(sorted.begin(), sorted.end(), [dates](const sched::HourSlot& a, const sched::HourSlot& b) {
        const qint64 da = dates ? a.date.toJulianDay() : a.dayOfWeek, db = dates ? b.date.toJulianDay() : b.dayOfWeek;
        return da != db ? da < db : a.hour < b.hour;
    });

    // The walk: the grid cyclically from its first hour that is not a continuation.
    Walk w(m_o);
    const int n = int(sorted.size());
    int start = 0;
    if (!dates)
        for (int i = 0; i < n; ++i)
            if (!sorted[i].isContinuation()) { start = i; break; }
    for (int k = 0; k < n; ++k) w.slot(sorted[(start + k) % n]);

    // Loops of hand-made hours, EventDuration.
    for (Ev& ev : w.evs) {
        if (ev.loops && !ev.hasRepeat) {
            Line l;
            l.e = makeInstruction(Instr::Repeat);
            l.e.eol = m_o.eol;
            l.slotId = ev.lines.isEmpty() ? QString() : ev.lines.first().slotId;
            ev.lines.prepend(l);
            for (PendingNote& pn : ev.notes) if (pn.line >= 0) ++pn.line;
            ev.playMode = 2;
            ev.hasRepeat = true;
        }
    }
    QList<double> durations;
    for (int i = 0; i < w.evs.size(); ++i) {
        const Ev& ev = w.evs[i];
        double d = ev.contentSec;
        if (ev.playMode == 2) {
            int next = -1;
            for (const Ev& x : w.evs)
                if (x.dayOrder == ev.dayOrder && x.sec > ev.sec && (next < 0 || x.sec < next)) next = x.sec;
            if (next > 0) d = next - ev.sec;
        }
        durations << d;
    }

    // foldEveryDay: an imported every-day event that came out the same on all seven weekdays.
    int folded = 0;
    QList<Ev> evs;
    QList<double> durs;
    if (!dates && m_o.foldEveryDay) {
        QHash<QString, QList<int>> groups;   // sec|text -> event indexes
        QStringList order;
        for (int i = 0; i < w.evs.size(); ++i) {
            const Ev& ev = w.evs[i];
            if (!ev.provenance || ev.srcKind != DayKind::EveryDay) continue;
            const QString key = QString::number(ev.sec) + u'|' + ev.name + u'|' + QString::number(ev.eventType) + u'|' + QString::number(ev.playMode) + u'\n' + ev.text();
            if (!groups.contains(key)) order << key;
            groups[key] << i;
        }
        QSet<int> drop;
        QList<int> keep;
        for (const QString& key : order) {
            const QList<int>& g = groups[key];
            QSet<qint64> days;
            for (int i : g) days.insert(w.evs[i].dayOrder);
            if (days.size() != 7) continue;
            for (int i : g) drop.insert(i);
            keep << g.first();
            folded += int(g.size()) - 1;
        }
        for (int i : keep) {
            Ev ev = w.evs[i];
            ev.stamp.kind = DayKind::EveryDay;
            ev.stamp.weekday = 0;
            ev.dayOrder = -1;
            evs << ev;
            durs << durations[i];
        }
        for (int i = 0; i < w.evs.size(); ++i)
            if (!drop.contains(i)) { evs << w.evs[i]; durs << durations[i]; }
    } else {
        evs = w.evs;
        durs = durations;
    }
    QList<int> idx;
    for (int i = 0; i < evs.size(); ++i) idx << i;
    std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) {
        const Ev& x = evs[a];
        const Ev& y = evs[b];
        const int rx = kindRank(x.stamp.kind), ry = kindRank(y.stamp.kind);
        if (rx != ry) return rx < ry;
        if (x.dayOrder != y.dayOrder) return x.dayOrder < y.dayOrder;
        return x.sec < y.sec;
    });

    // The document.
    Document& d = r.doc;
    d.encoding = m_o.encoding;
    int lines = 0;
    for (int i : idx) {
        const Ev& ev = evs[i];
        Entry h;
        h.kind = LineKind::Header;
        h.stamp = ev.stamp;
        h.stamp.raw.clear();
        if (ev.eventType != 0) h.params << param("EventType", QString::number(ev.eventType));
        if (!ev.name.isEmpty()) h.params << param("Name", ev.name);
        h.params << param("EventDuration", formatDuration(durs[i])) << param("PlayMode", QString::number(ev.playMode));
        h.dirty = true;
        h.eol = m_o.eol;
        h.raw = renderEntry(h, WriteOptions{false, m_o.dateStyle});
        d.entries << h;
        const int headerLine = int(d.entries.size());
        for (const Line& l : ev.lines) { d.entries << l.e; ++lines; }
        for (PendingNote pn : ev.notes) {
            pn.n.diag.line = pn.line < 0 ? headerLine : qMin(headerLine + 1 + pn.line, int(d.entries.size()));
            r.diagnostics << pn.n;
        }
    }
    for (int i = 0; i < d.entries.size(); ++i) d.entries[i].line = i + 1;
    if (!d.entries.isEmpty() && !m_o.finalNewline) d.entries.last().eol.clear();
    {   // Document::events, for callers that inspect the result without re-parsing it
        Event* cur = nullptr;
        for (int i = 0; i < d.entries.size(); ++i) {
            const Entry& e = d.entries[i];
            if (e.kind == LineKind::Header) { Event ev; ev.entry = i; ev.end = i + 1; d.events << ev; cur = &d.events.last(); continue; }
            if (!cur) continue;
            cur->end = i + 1;
            if (e.kind == LineKind::Instruction || e.kind == LineKind::Media) cur->items << i;
        }
        resolvePlayModes(d);
    }

    WriteOptions wo;
    wo.stamps = m_o.dateStyle;
    wo.encoding = m_o.encoding;
    wo.eol = m_o.eol;
    wo.finalNewline = m_o.finalNewline;
    QList<int> lossy;
    r.bytes = serialize(d, wo, &lossy);
    QStringList text;
    for (const Entry& e : d.entries) text << renderEntry(e, wo);
    r.text = text.join(QString::fromLatin1(m_o.eol));
    if (m_o.finalNewline && !d.entries.isEmpty()) r.text += QString::fromLatin1(m_o.eol);
    for (int line : lossy) {
        Note n;
        n.diag = Diagnostic{line, 0, diagnosticInfo(QStringLiteral("MP032")).severity, QStringLiteral("MP032"),
                            QStringLiteral("characters %1 cannot hold were written as '?'").arg(encodingName(m_o.encoding))};
        r.diagnostics << n;
    }
    std::stable_sort(r.diagnostics.begin(), r.diagnostics.end(), [](const Note& a, const Note& b) { return a.diag.line < b.diag.line; });

    // Filename and stats.
    if (dates) {
        QDate from, to;
        for (const sched::HourSlot& s : sorted) {
            if (!from.isValid() || s.date < from) from = s.date;
            if (!to.isValid() || s.date > to) to = s.date;
        }
        r.filename = ch + u'-' + from.toString(Qt::ISODate) + (to != from ? u'_' + to.toString(Qt::ISODate) : QString()) + QStringLiteral(".in");
    } else r.filename = ch + QStringLiteral("-grid.in");
    int errs = 0, warns = 0, infos = 0;
    for (const Note& n : r.diagnostics) {
        if (n.diag.severity == QLatin1String("error")) ++errs;
        else if (n.diag.severity == QLatin1String("warning")) ++warns;
        else ++infos;
    }
    r.stats = QJsonObject{{"slots", n}, {"events", int(evs.size())}, {"folded", folded}, {"items", w.items}, {"lines", int(d.entries.size())},
                          {"raw", w.rawLines}, {"canonical", w.canonical}, {"new", w.fresh},
                          {"skipped", QJsonObject{{"copies", w.copies}, {"picks", w.picks}, {"joins", w.joins}}},
                          {"overlays", QJsonObject{{"shows", w.shows}, {"offs", w.offs}}}, {"queryBlocks", w.queries}, {"lossyLines", int(lossy.size())},
                          {"diagnostics", QJsonObject{{"error", errs}, {"warning", warns}, {"info", infos}}}};
    return r;
}

QList<sched::HourSlot> Exporter::gridSlots(const sched::ScheduleStore& store, const QString& channelId) {
    return store.templateSlots(channelId);
}

QList<sched::HourSlot> Exporter::dateSlots(const sched::ScheduleStore& store, const QString& channelId, const QDate& from, const QDate& to) {
    QList<sched::HourSlot> out;
    if (!from.isValid() || !to.isValid() || to < from || from.daysTo(to) > 366) return out;
    for (QDate d = from; d <= to; d = d.addDays(1))
        for (int h = 0; h < 24; ++h) {
            sched::HourSlot s = store.effectiveSlot(channelId, d, h);
            s.id = sched::HourSlot::dateId(d, h);
            s.channelId = channelId;
            s.date = d;
            s.dayOfWeek = -1;
            s.hour = h;
            out << s;
        }
    return out;
}

std::function<int(const QString&, const QString&)> Exporter::segmentIndexFrom(const sched::ScheduleStore* store) {
    return [store](const QString& mediaId, const QString& segmentId) {
        if (!store || mediaId.isEmpty()) return -1;
        bool found = false;
        const sched::MediaRecord m = store->media(mediaId, &found);
        if (!found) return -1;
        for (int i = 0; i < m.segments.size(); ++i)
            if (m.segments.at(i).toObject().value(QStringLiteral("id")).toString() == segmentId) return i;
        return -1;
    };
}

} // namespace mp::ex
