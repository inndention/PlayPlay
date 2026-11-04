#pragma once
// MasterPlay export (the lane design notes §7, §6.10 round-trip payload; slice MP-10): PlayPlay hour slots back to
// a .IN playlist. The input is a list of sched::HourSlot (the weekly template, or dated slots), the output the file's
// bytes, the mp::Document behind them, diagnostics and stats. Pure: no store, file or clock access; the store helpers
// at the end only collect slots.
//
// Events. The slots are walked in time order (the grid cyclically, from its first hour that is not a continuation,
// so Saturday night runs on into Sunday). A header opens:
//   - at an imported event's first placed item (extra.masterplay.anchor): at the event's own time, which is the stamp
//     in extra.masterplay.event (a pod's schedtime, a floating event laid late and a carry keep the event where the
//     playlist had it), on the slot's day, or the day before when that time lies past the slot's hour;
//   - at a hard start without provenance (a hand-made item): at hour + fixedOffsetSec, unless an event already opened
//     there;
//   - at the top of an hour that is not a continuation when its first item has no provenance, or when nothing is
//     open yet (a range that starts inside a programme, a carry whose root hour is not exported).
// Continuation hours (mergedEvent.isContinuation) open nothing: their items join the open event. Everything else
// joins the open event too. The header is "<stamp>[<TAB>EventType=N][<TAB>Name=...]<TAB>EventDuration=HH:MM:SS<TAB>
// PlayMode=N", the editor's field order: EventType from the provenance anchor, else 1 for extra.conflictPolicy
// finish_previous; Name from the provenance event (showBlock.masterplay.events), else showBlock.title for the first
// event of a hand-made hour; EventDuration the items' planned length (a looping event: the time to the next event of
// its day); PlayMode from the provenance event, else 2 for a looping hour (then "!Repeat" is its first line unless the
// provenance already holds one), else 1. Grid headers are weekly stamps (0000-00-0<dow+1>Thh:mm:ss), dated slots dated
// ones; foldEveryDay turns an imported every-day event that came out identical on all seven weekdays back into one
// 0000-00-00 event. dateStyle legacy writes the older playlist editor's "0N/00/0000 hh:mm:ss" / "DD/MM/YYYY hh:mm:ss".
// Output order is MasterPlay's: every day, weekly 1..7, dated.
//
// Items. Every item is one of:
//   untouched  it carries extra.masterplay.raw and the line still describes it: a media line's file name, forced
//              length (the bracket) and Name= match the item (an act keeps its line whether or not the library had
//              segments), an !InsertDirectory still has its folder, SubDirs and number of picks, a live line still is
//              a live item. With rawLines (the default) the original line is written byte for byte, which makes
//              MasterPlay -> PlayPlay -> MasterPlay lossless for untouched content (design §6.10). Edits the .IN cannot
//              see (a cue removed, a transition) are not detected.
//   edited     it has provenance but the line no longer describes it: rendered from its fields (below).
//   new        no provenance: rendered from its fields.
// The provenance lines of an item (extra.masterplay.instructions / after: OSD, breaks, info, unsupported, structural
// and dropped lines, in the playlist's order) are written raw around it in every class, so an edit keeps the format's
// instructions; lines a later event start carried over (carriedFrom) go to the event they came from. Loop copies
// (extra.masterplay.copy > 0) write nothing: the loop is written once with its !Repeat. The picks of a folder line
// (extra.masterplay.folder.pick 0..N-1) that follow each other in order are one group: the line is written once with
// ToSelect=<group size> (raw when the group is the whole line, from pick 0). Picks whose earlier picks are gone
// (deleted, or an item put between) are a group of their own, written as a folder line of their own (MP031). A join
// (extra.masterplay.joinSec: a programme repeated at the top of an unwritten root hour) writes nothing when the
// programme is the line just written, else its line from the start (MP031; the join offset is not an in point).
//
// Rendering from fields (design §7):
//   media        the path through the reversed pathMap (the import's rules, station -> playlist), "[HH:MM:SS;FF]"
//                when an out point forces the length (with an in point: out - in, so the planned length holds; an in
//                point has no .IN form: MP031, the line plays its first seconds / from the start), "actinout=N" for a
//                segment item through Options::segmentIndex (none: MP031), "Name=" when the title is not the file's
//                base name.
//   folderBlock  "!InsertDirectory <folder>[\<mask>]" with LoadingMode from the provenance mode, else 0 (order name,
//                a target length: omitted), 1 (random, a target length), 2 (name, one pick) or 3 (random / even, one
//                pick); ToSelect=N for pick blocks (consecutive identical blocks of one event merge into N with
//                mergeFolderPicks); SubDirs=1 when recursive; MaxAge=<days*24>; filename starts_with / ends_with rules
//                become the Name*.ext mask; other rules are MP031.
//   live         "!Video <source>" (the provenance source, else the path: ndi:// and other non-URL sources), or
//                "!LiveStream<TAB>Path=<url>" for a URL [UNVERIFIED params].
//   queryBlock   the picks Options::queryPicks gives (MP-11: the executor's cache for the current / next hour) as
//                media lines; none: "!Info<TAB>Name=query block <label> not exported" and MP030.
//   cues         a new item's cgOverlays open layers: "!ShowLogo<TAB>f=<image><TAB>Layer=1" (bug; 2 for other roles),
//                "!ShowCrawl <text>" (template ticker), "!ShowText <title>" (lower_third) before the item; a layer the
//                next new item no longer carries writes "!OSD off" (MasterPlay clears every layer) and the layers it
//                still carries are shown again. cgOnStart "clock" is "!ShowClock t=0", cgOnEnd "clock" or a hide hook
//                "!OSD off" after the item; the importer's JSON show hooks map like cues; other hook names are
//                identity graphics with no .IN form (MP031). Untouched and edited items carry their layers in their
//                provenance lines, so their cues only update what is open.
//   note         a new item's note is "!Info<TAB>Name=<note>" before it (the importer made !Info the next item's note).
//   cg / gap     no .IN form: left out, MP031.
//
// Diagnostics (mp::Diagnostic with the output line, plus the slot and item): MP030 query block not exported, MP031
// (info) something approximated or left out, MP032 (warning) characters the target encoding cannot hold (cp1252:
// written as '?'; serialize()'s lossyLines). Errors (nothing written): slots of the wrong kind for the mode, bad
// options.
//
// THREADING: pure computation on the caller's thread; the store helpers read the store under its own mutex.
#include "masterplayin.h"
#include "masterplayinstructions.h"
#include "mpini.h"
#include "schedulemodel.h"

#include <QByteArray>
#include <QDate>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <functional>

namespace sched { class ScheduleStore; }

namespace mp::ex {

struct Options {
    QString channelId;                         // names the file and the stats ("" = "export")
    QString mode = QStringLiteral("grid");     // grid (weekly template slots "D-H") | dates (dated slots "YYYY-MM-DD-H")
    StampForm dateStyle = StampForm::Iso;      // iso (MasterPlay 6.x) | legacy (the older playlist editor)
    Encoding encoding = Encoding::Utf8Bom;     // utf8bom (MasterPlay 6.x) | ansi (cp1252) | utf8 | utf16le | utf16be
    QByteArray eol = QByteArrayLiteral("\r\n");
    bool finalNewline = true;
    bool rawLines = true;                      // untouched imported items write their original line
    bool foldEveryDay = true;                  // grid: an imported every-day event identical on all 7 days -> 0000-00-00
    bool mergeFolderPicks = true;              // consecutive identical one-pick folder blocks -> ToSelect=N
    int fps = 30;                              // frames in brackets
    /// The import's path rules (playlist path -> station path); the exporter applies them reversed. Empty: paths as
    /// the library has them.
    QList<ini::PathRule> pathMap;
    /// The picks of a query block item (MP-11 passes the executor's cache for the current / next hour); none, or an
    /// empty list: MP030 and an !Info line.
    std::function<QList<sched::SlotItem>(const sched::HourSlot&, const sched::SlotItem&)> queryPicks;
    /// actinout=N of a segment item: the index of segmentId in that media's segments (-1 unknown).
    std::function<int(const QString& mediaId, const QString& segmentId)> segmentIndex;

    /// {channel|channelId, mode, dateStyle (iso|legacy), encoding (encodingFromName names), eol (crlf|lf), finalNewline,
    ///  rawLines, foldEveryDay, mergeFolderPicks, fps, pathMap[{from,to,...}]}. Absent keys keep defaults; bad values
    ///  keep the default and are reported in errors.
    static Options fromJson(const QJsonObject& o, QStringList* errors = nullptr);
    QJsonObject toJson() const;
    QStringList check() const;                 // problems that make an export impossible
};

/// A diagnostic with where it came from. diag.line is the 1-based line of the output file (0 = none).
struct Note {
    Diagnostic diag;
    QString slotId, itemId;
    QJsonObject toJson() const;                // Diagnostic::toJson() + {slotId?, itemId?}
};

struct Result {
    QByteArray bytes;                          // the file (BOM, encoding, terminators as the options say)
    QString text;                              // the same as text
    Document doc;                              // the entries written (parse(bytes) gives the same document)
    QString filename;                          // "<channel>-grid.in" | "<channel>-<from>[_<to>].in"
    QList<Note> diagnostics;                   // MP030 / MP031 / MP032, in line order
    QStringList errors;                        // nothing was written
    QJsonObject stats;                         // {slots, events, folded, items, lines, raw, canonical, new, skipped{copies, picks, joins},
                                               //  overlays{shows, offs}, queryBlocks, lossyLines, diagnostics{error, warning, info}}
    bool ok() const { return errors.isEmpty(); }
    /// {ok, filename, channel, mode, dateStyle, encoding, stats, diagnostics[], errors[], text (withText)}
    QJsonObject toJson(bool withText = false) const;
};

class Exporter {
public:
    explicit Exporter(const Options& o = {});
    const Options& options() const { return m_o; }

    /// The playlist of these slots (any order; sorted by day and hour). Grid mode takes template slots, dates mode
    /// dated ones; a slot of the other kind is an error.
    Result run(const QList<sched::HourSlot>& input) const;

    /// The channel's template slots (grid mode).
    static QList<sched::HourSlot> gridSlots(const sched::ScheduleStore& store, const QString& channelId);
    /// The slot in force for every hour of [from, to] (a dated override, else the template), re-stamped as that
    /// date's slot (dates mode). Empty when to < from or the range is over 366 days. One effectiveSlot() read per hour
    /// under the store mutex (up to 8784 for a year): call it from a worker, never on the Supervisor's event-loop
    /// thread for more than a few days (MP-11).
    static QList<sched::HourSlot> dateSlots(const sched::ScheduleStore& store, const QString& channelId, const QDate& from, const QDate& to);
    /// A segmentIndex over the store's media table (the store must outlive the function).
    static std::function<int(const QString&, const QString&)> segmentIndexFrom(const sched::ScheduleStore* store);

    /// Is the item still what its provenance line says (the header comment's rule)? false for items without a line.
    static bool untouched(const sched::SlotItem& item, const ParseOptions& po = {});

private:
    Options m_o;
};

} // namespace mp::ex
