#pragma once
// MasterPlay day timelines (the lane design notes §2.7 semantics, §6.1 day timeline, §6.5 !InsertPlaylist; slice
// MP-07): which event is in force when on one target day, and what each event plays once its !InsertPlaylist lines are
// expanded and its !ChangePlaylist cut is applied. MP-08 turns these windows into PlayPlay hour slots.
//
// Days. A Day is a weekday of the weekly grid (dow 0 = Sunday .. 6 = Saturday: PlayPlay's HourSlot day; MasterPlay's
// weekday code is dow + 1) or one calendar date (a dated override import).
//
// Candidates (design §6.1.1). On a day: every-day events, weekly events with that weekday code, the day-change marker
// (0000-00-80, at Options::dayChangeSec: TagesWechsel, default midnight [UNVERIFIED]) and, on a date only, the events
// dated that day. Dated events are never used in the weekly grid (DayTimeline::datedSkipped counts them). End-of-list
// sentinels and MP002 headers are never candidates. Options::timeShiftMin moves every event (a source in another time
// zone); an event pushed over midnight lands on the next / previous day, so its weekday or date moves with it.
//
// Ties (§6.1.2). Of the events starting at the same second the highest priority stays: dated 3 > weekly 2 > every day
// 1 > day change 0 [UNVERIFIED: the design ranks the first three; the marker, whose meaning is unknown, ranks below a
// real every-day event]. Equal priorities (two weekly events at one time, or every event under samePriority, Mplay.INI
// EventsDoesHaveSamePriority=1) keep the first one in the file. Each dropped event is MP015, at its header.
//
// Carry-over (§6.1.3, §2.7.1: the last event of a day runs on past midnight until the next one). When nothing starts
// at 00:00, the day opens with a carry window [0, first start) holding the event in force at the end of the previous
// day: the latest kept event of the nearest earlier day within a week (grid: the week is cyclic, so Sunday continues
// Saturday and a playlist with one weekly event continues last week's), else, on a date, the latest dated event before
// that week. Window::carryDays / offsetSec say how long that event had been running at 00:00.
//
// Windows end where the next one starts (the last at 24:00); EventDuration is informational (MP-08 compares content
// with the window, MP016). A window lists its event's content as Items: the event's own lines, each !InsertPlaylist
// line followed by the target's lines (depth + 1), recursively.
//
// !InsertPlaylist (§6.5 expand, the snapshot mode; "link" expands the same way). The target is read once per Builder
// through the ReadFn (cached by file; reads may block: run the Builder in a worker) and parsed with Options::parse.
// Inlined: the target's event in force at the inserting event's own start (the same rule as a playlist being loaded;
// a carry window therefore expands exactly like its event's own window), or the target's preamble when no event is in
// force (a header-less list) [UNVERIFIED: MasterPlay reads the file at cue time]. Depth is limited to maxDepth (4:
// items at depth 0..4); a deeper !InsertPlaylist, or one naming a file already on its insert chain (case-folded,
// normalised path: A -> B -> a.in), is MP023 and stays unexpanded; an unreadable or unnamed target is MP024. Picking
// modes (LoadingMode / ToSelect / TimeOffset) are the importer's: every line is inlined. Options::maxItems bounds one
// window's expansion (a fan-out of nested inserts): at the cap the rest is not expanded, MP023, Window::limited.
//
// Play mode after expansion (§2.3, §2.7.5: "their breaks and repeats affect the parent"). The first play-mode marker of
// the event decides; when that marker is an !InsertPlaylist whose target was read (expanded, or empty) the target
// decides the same way among its own lines (its first marker, else its event's header PlayMode, absent = normal), so a
// sub-playlist's !Repeat makes the event repeat; an unread target leaves the decision to the event's header. Without
// any marker the header PlayMode decides (absent or 0 = normal). Window::repeatAt is the first !Repeat / !Wiederholen
// at any depth; loopFrom follows it, or is 0 (the target's first line when a target's header decided) for a repeat
// mode without a !Repeat line (Saturday_0200.in) [UNVERIFIED, as MP-03's loopStart()].
//
// !ChangePlaylist (§6.1.5, §2.7.7) ends the event's content where it stands, at any depth: the line is kept (cutAt),
// the lines after it at every level are not (cutItems). Not followed (the default, an unnamed or unreadable target, a
// change cycle, more than maxDepth hops): MP017. With followChangePlaylist the target's own timeline for the same day
// takes over [UNVERIFIED approximation; the switch time depends on durations the timeline does not know]: the target's
// event in force at the window start is appended after the change line (viaChange items: they play after the change,
// in that event's mode, Item::targetPlayMode / targetLoopFrom), and the target's windows starting later replace the
// rest of the day (Window::via names the change line). Every day starts from the root playlist again.
//
// Diagnostics (lane catalog codes, timeline stage, catalog severities) are Notes: a Diagnostic plus the file it points
// into (sub-playlists have their own line numbers) and the days it was found on, de-duplicated across days.
//   MP015 warning  a lower-priority event dropped at the same time
//   MP017 warning  !ChangePlaylist not followed (the content ends there)
//   MP023 error    sub-playlist cycle; also nesting deeper than maxDepth, and the maxItems cap
//   MP024 warning  sub-playlist missing (no name, or the ReadFn could not read it)
// The parser's own diagnostics of each file read are in sourcesJson().
#include "masterplayin.h"
#include "masterplayinstructions.h"
#include "mpini.h"

#include <QDate>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <functional>
#include <memory>

namespace mp::tl {

constexpr int kDaySec = 86400;

// ---------------------------------------------------------------------------------------------------------------------
// Days and priorities

struct Day {
    int dow = -1;                  // 0 Sunday .. 6 Saturday (a date's weekday too)
    QDate date;                    // set for a dated day

    static Day weekday(int dow);   // any int, taken modulo 7
    static Day of(const QDate& date);
    bool isDate() const { return date.isValid(); }
    bool isValid() const { return isDate() || (dow >= 0 && dow <= 6); }
    int weekdayCode() const { return isValid() ? dow + 1 : 0; }   // MasterPlay: 1 Sunday .. 7 Saturday
    Day addDays(int n) const;      // grid: cyclic (Sunday - 1 = Saturday)
    QString label() const;         // "Sun" .. "Sat", or "2025-09-10"
    QString key() const;           // "0" .. "6" (the HourSlot id's day), or "2025-09-10" (a dated slot id's prefix)
    bool operator==(const Day& o) const { return dow == o.dow && date == o.date; }
};

/// 3 dated, 2 weekly, 1 every day, 0 day change; -1 for kinds that are never candidates.
int priorityOf(DayKind k);
QString priorityName(int p);       // dated | weekly | everyDay | dayChange | ""

// ---------------------------------------------------------------------------------------------------------------------
// Options

struct Options {
    int maxDepth = 4;              // !InsertPlaylist nesting (items at depth 0..maxDepth); also ChangePlaylist hops. 0..8
    int maxItems = 20000;          // items one window may expand to (a fan-out bound); 100..1,000,000
    bool expandSubPlaylists = true;// false: !InsertPlaylist lines stay unexpanded (status "off"), nothing is read
    bool followChangePlaylist = false;
    bool samePriority = false;     // Mplay.INI EventsDoesHaveSamePriority=1: ties keep the first event in the file
    int dayChangeSec = 0;          // TagesWechsel: where a 0000-00-80 event starts (its stamp time is not used)
    int timeShiftMin = 0;          // moves every event; -1440..1440
    ParseOptions parse;            // sub-playlists and ChangePlaylist targets are parsed with these

    /// samePriority (EventsDoesHaveSamePriority), dayChangeSec (TagesWechsel when valid), parse.fps (suggestedFps()).
    static Options fromIni(const ini::MplayIni& ini);
    /// {maxDepth, maxItems, subPlaylists: "expand" | "link" | "off", followChangePlaylist, samePriority,
    ///  dayChange: "HH:MM[:SS]" (or dayChangeSec), timeShiftMin, fps, legacyDateOrder}; absent keys keep defaults,
    ///  numbers are clamped to their ranges.
    static Options fromJson(const QJsonObject& o);
    QJsonObject toJson() const;
};

// ---------------------------------------------------------------------------------------------------------------------
// Reading sub-playlists

struct ReadResult {
    bool ok = false;
    QByteArray bytes;
    QString path;                  // what was read (after path rules): the file's identity for caches and cycles
    QString why;                   // when !ok: "not found", "root unreachable", ...
};
/// target: the !InsertPlaylist / !ChangePlaylist head as written. fromFile: the path of the playlist holding that line
/// (the root file as given to the Builder, else a ReadResult::path), for relative names.
using ReadFn = std::function<ReadResult(const QString& target, const QString& fromFile)>;

/// Reads from the file system: absolute targets (drive or UNC) go through the path rules (ini::mapPath, the MP-05 /
/// MP-06 rules), relative ones are taken against fromFile's folder. A root (drive or \\host\share) found missing is
/// remembered, so a dead host costs one probe. Files over maxBytes are refused. BLOCKS on unreachable UNC hosts: call
/// it from a worker. The returned function may be shared by several threads.
ReadFn fileReader(const QList<ini::PathRule>& rules = {}, qint64 maxBytes = 16 * 1024 * 1024);
/// Reads from memory (tests, pasted text): files by name, compared as normalised, case-folded paths; relative targets
/// are also tried against fromFile's folder.
ReadFn memoryReader(const QHash<QString, QByteArray>& files);

// ---------------------------------------------------------------------------------------------------------------------
// Results

/// One line of a window's content.
struct Item {
    Entry entry;                   // the line (a copy: Qt containers share their data)
    QString file;                  // the playlist it is in (the root file, a sub-playlist or a ChangePlaylist target)
    int depth = 0;                 // 0: the event's own line; n: n !InsertPlaylist / followed !ChangePlaylist levels down
    int parent = -1;               // index in Window::items of the !InsertPlaylist (or followed !ChangePlaylist) above it
    int children = 0;              // !InsertPlaylist / followed !ChangePlaylist: the items after it that came from its
                                   // target (all depths): items [index + 1, index + children]
    QString group;                 // "" at depth 0; a stable id of the insert chain above it (design §6.5 subGroup)
    bool viaChange = false;        // came in through a followed !ChangePlaylist (plays after the change)
    Instr instr = Instr::Unknown;  // identify(entry).id for instruction lines

    // !InsertPlaylist / !ChangePlaylist lines
    QString target;                // the head as written
    QString targetPath;            // ReadResult::path ("" when nothing was read)
    /// !InsertPlaylist: expanded | empty (read, nothing to inline) | missing (MP024) | cycle (MP023) | depth (MP023) |
    /// off (expandSubPlaylists false). !ChangePlaylist: followed | notFollowed (MP017).
    QString status;
    QString why;                   // missing / notFollowed: the reason
    int subEventLine = 0;          // expanded: the target event's header line (0: its preamble was inlined)
    int subPlayMode = 0;           // expanded: that event's header PlayMode (absent or 0 = 1); the target's fallback mode
    int targetPlayMode = 0;        // followed change: the play mode of the target event appended after it
    int targetLoopFrom = -1;       // followed change: index in Window::items where that event's loop starts (-1 none)
    int replacedWindows = 0;       // followed change: windows of the day it replaced

    bool isInsert() const { return instr == Instr::InsertPlaylist; }
    bool isChange() const { return instr == Instr::ChangePlaylist; }
    /// {line, kind, text, file? (depth > 0), depth?, parent?, children?, group?, viaChange?, instruction?, target?,
    ///  targetPath?, status?, why?, subEventLine?, subPlayMode?, targetPlayMode?, targetLoopFrom?, replacedWindows?}
    QJsonObject toJson() const;
};

/// One event in force on a day, from startSec to endSec.
struct Window {
    int startSec = 0, endSec = kDaySec;   // seconds of the day, [start, end)
    QString file;                  // the playlist of the event (the root, or a followed ChangePlaylist target)
    int eventIndex = -1;           // into that document's events
    int line = 0;                  // its header line
    Stamp stamp;                   // its stamp (raw as written)
    int priority = 0;              // priorityOf(stamp.kind)
    int eventSec = 0;              // its start second on its own day (after timeShift; day change: dayChangeSec)
    bool carry = false;            // carries an event that started on an earlier day (startSec 0)
    int carryDays = 0;             // ... that many days earlier (1 yesterday; 7 the same weekday last week)
    qint64 offsetSec = 0;          // how long the event had been running at startSec (carry only)
    int eventType = 0;             // 0 exact, 1 floating skip, 2 floating play all
    QString name;                  // Name=
    double eventDurationSec = -1, expectedSec = -1;
    int headerPlayMode = -1;       // the header's PlayMode (-1 absent)
    int playMode = 1;              // after expansion (see the header comment)
    QString playModeFrom;          // inBand (the event's own marker) | sub (a target's marker or header) | header | default
    QList<Item> items;             // the content after expansion and the ChangePlaylist cut
    int repeatAt = -1;             // index of the first !Repeat (any depth, before the cut)
    int loopFrom = -1;             // first index of the loop; -1 when nothing loops
    int cutAt = -1;                // index of the !ChangePlaylist that ended the content (-1 none)
    int cutItems = 0;              // lines after it (all levels) that never run
    bool limited = false;          // Options::maxItems stopped the expansion (MP023)
    int changeDepth = 0;           // 0: the root playlist; n: reached through n followed ChangePlaylists
    QString via;                   // "<file>#<line>" of the followed !ChangePlaylist that brought this window in

    int durationSec() const { return endSec - startSec; }
    /// {start, end ("HH:MM:SS"), startSec, endSec, file, line, stamp, dayKind, priority, eventTime, eventType, playMode,
    ///  playModeFrom, headerPlayMode?, carry?, carryDays?, offsetSec?, name?, eventDurationSec?, expectedSec?,
    ///  repeatAt?, loopFrom?, cutAt?, cutItems?, limited?, changeDepth?, via?, itemCount, items[] (withItems)}
    QJsonObject toJson(bool withItems = true) const;
};

/// An event dropped by a tie (MP015).
struct Dropped {
    QString file;
    int line = 0, eventIndex = -1, sec = 0, priority = 0;
    QString stamp;                 // as written
    int keptLine = 0, keptPriority = 0;
    QString keptStamp;
    QJsonObject toJson() const;    // {file, line, stamp, time, priority, kept{line, stamp, priority}}
};

struct DayTimeline {
    Day day;
    /// Ordered and contiguous from the first window to 24:00. Empty when no event is in force at any time of the day;
    /// the first window starts after 00:00 only when nothing was in force before it (a date before every event).
    QList<Window> windows;
    QList<Dropped> dropped;
    int datedSkipped = 0;          // grid days: dated events not used (they belong to date imports)

    const Window* at(int sec) const;   // the window in force at a second of the day, nullptr when none
    /// {day, key, dow, date?, datedSkipped?, windows[], dropped[]}
    QJsonObject toJson(bool withItems = true) const;
};

/// A timeline diagnostic: the Diagnostic (line / col in `file`, catalog severity), the file, the days it was seen on.
struct Note {
    Diagnostic diag;
    QString file;
    QStringList days;              // Day::label()s in first-seen order
    QJsonObject toJson() const;    // Diagnostic::toJson() + {file, days[]}
};

// ---------------------------------------------------------------------------------------------------------------------
// Builder

/// Builds day timelines of one playlist. Sub-playlists and change targets are read through the ReadFn at most once per
/// distinct (target, including file) and parsed once per file; per-day candidate lists are cached, so a week or a date
/// range costs little more than one day. Not thread-safe; run it in a worker when the ReadFn may block.
class Builder {
public:
    /// rootFile: where the root was read from ("" for pasted text: relative sub-playlist names cannot be read then).
    Builder(const Document& root, const QString& rootFile = {}, const Options& o = {}, ReadFn read = {});
    Builder(Builder&&) noexcept;
    Builder& operator=(Builder&&) noexcept;
    ~Builder();

    const Options& options() const;
    DayTimeline build(const Day& day);
    QList<DayTimeline> week();                                    // Sunday .. Saturday
    QList<DayTimeline> dates(const QDate& from, const QDate& to); // at most 366 days; empty when to < from

    const QList<Note>& notes() const;          // in first-seen order
    QJsonArray notesJson() const;
    /// [{path, target?, ok, why?, root?, bytes?, sha256?, encoding?, events?, items?, diagnostics?{error, warning, info}}]:
    /// the root first, then every file read (or tried) in order.
    QJsonArray sourcesJson() const;
    int reads() const;                         // ReadFn calls
    /// {days, windows, carry, dropped, items, maxDepth, preambleItems, inserts{expanded, empty, missing, cycle, depth,
    ///  off}, changes{followed, notFollowed}, cutItems, limited, reads, sources, notes}: totals over every build().
    QJsonObject stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mp::tl
