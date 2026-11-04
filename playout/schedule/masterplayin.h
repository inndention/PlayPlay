#pragma once
// MasterPlay playlist (.IN) parser and byte-exact serializer (docs/plans/masterplay.md §2 grammar, §9.1 API).
//
// QtCore only: AN-FS compiles this file read-only later, so no GUI, SQL or network types and no PlayPlay schedule
// dependency. The grammar was rebuilt from the PlaylistEditor.exe writer and checked against sample playlists
// and a production playlist.
//
// Model: a Document is one Entry per physical line. Each entry keeps its original bytes and its own terminator, so
// serialize() writes an untouched document back byte for byte (BOM, CRLF / LF per line, trailing TABs, empty values,
// no final newline). An entry marked dirty is re-rendered from its fields by renderEntry(); renderEntry() of a parsed,
// unedited entry gives back exactly its raw text, which is what makes the field model lossless.
//
// MP-02a: classification, stamps, durations, the TAB field model, Document / Event, byte-exact serialize.
// MP-02b: encoding detection (UTF-8 BOM, UTF-16 LE/BE BOM with a code-unit line splitter, strict UTF-8, cp1252),
// the file-level diagnostics, WriteOptions (canonical form, stamp form, encoding / EOL / final-newline overrides),
// semanticDiff() and the diagnostic catalog. MP-03: the instruction catalog and typed accessors live in
// masterplayinstructions.{h,cpp} (also QtCore only; this file needs it, so AN-FS compiles both). parse() runs its
// resolvePlayModes() (the in-band play mode beats the header) and appends its instructionDiagnostics().
//
// Diagnostics the parser emits (design §15; diagnosticCatalog() lists every code with its stage):
//   MP001 warning  unknown line (leading whitespace, '#' / ';' / '//', a stray U+FEFF, "!" alone, stamp + space)
//   MP002 error    bad event stamp (the line is still a header, so its items stay with it)
//   MP003 warning  duplicate event time (same day and time as an earlier header, whatever the stamp form)
//   MP004 info     out-of-order event (sorts before the header above it; ISO stamp order = MasterPlay's: every day,
//                  weekly 1..7, day change, dated, end of list)
//   MP007 warning  legacy dated stamp valid in both DMY and MDY order (legacyDateOrder auto)
//   MP008 warning  items before the first header (preamble)
//   MP009 warning  mixed CRLF / LF terminators (once per file), or a lone CR inside a line (MasterPlay breaks there)
//   MP010 warning  not UTF-8 and no BOM: decoded as cp1252; also bytes not valid in a BOM's encoding
//   MP011 warning  PlayMode / EventType value the editor could not have written
//   MP012 warning  bracket token after a media name that is not a forced duration ("x.mpg[2025-01-01..]"), kept
//   MP013 warning  bad EventDuration / ExpectedDuration / bracket duration
// and, from the instruction catalog (masterplayinstructions.h has the details):
//   MP011 warning  unknown instruction name; bad value or unknown key on an instruction with a typed spec
//   MP013 warning  bad duration in a typed instruction key (showvalue, hidevalue, DefPTime, infoexpectedlength)
//   MP014 warning  header PlayMode disagrees with the in-band mode (the in-band one is used)
#include <QByteArray>
#include <QDate>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QTime>
#include <optional>

namespace mp {

/// Utf8Bom is what MasterPlay 6.x writes. Detection order: UTF-8 BOM, UTF-16 LE / BE BOM (MasterPlay offers UTF-16
/// for as-run logs), strict UTF-8 without a BOM, then cp1252 for anything else (older or hand-made files, MP010).
enum class Encoding { Utf8Bom, Utf8, Ansi1252, Utf16LE, Utf16BE };
/// Stamp form of a header that is rendered: as parsed (its raw text while set), ISO "0000-00-0NThh:mm:ss" or the legacy
/// "0N/00/0000 hh:mm:ss" the older playlist editor writes.
enum class StampForm { Keep, Iso, Legacy };
enum class LineKind { Blank, Header, Instruction, Media, Unknown };
/// Event day from the stamp: 0000-00-00 every day, 0000-00-0N weekly (N = 1 Sunday .. 7 Saturday), YYYY-MM-DD one
/// date, 0000-00-80 the "use day change time" marker, 9999-99-99 the end-of-list sentinel.
enum class DayKind { EveryDay, Weekly, Dated, DayChange, EndOfList, Invalid };

/// One TAB-separated field. Values are raw (no quoting, no escaping) and split on the first '=' only; a field without
/// '=' is bare (key empty, text in value). Keys compare case-insensitively on read and keep the writer's case.
struct Param {
    QString key, value;
    bool bare = false;
    QString text() const { return bare ? value : key + QLatin1Char('=') + value; }
};

/// Ordered parameter lists: duplicate keys are kept in order, lookups take the first match (as Delphi's
/// TStringList.Values does).
int paramIndex(const QList<Param>& ps, const QString& key);
QString paramValue(const QList<Param>& ps, const QString& key, const QString& def = {});
QStringList paramValues(const QList<Param>& ps, const QString& key);
/// Replaces the first match (keeping its key spelling) or appends key=value.
void setParam(QList<Param>& ps, const QString& key, const QString& value);
Param parseField(const QString& field);

struct Stamp {
    DayKind kind = DayKind::Invalid;
    int weekday = 0;              // Weekly: the MasterPlay code, 1 Sunday .. 7 Saturday (qtDayOfWeek() converts)
    QDate date;                   // Dated
    QTime time;
    bool legacy = false;          // "NN/NN/NNNN hh:mm:ss" (weekday code or day first) instead of ISO
    bool ambiguous = false;       // legacy dated stamp that is a valid date in both DMY and MDY order (MP007)
    QString raw;                  // as written; renderEntry() writes it while it is set
    bool isValid() const { return kind != DayKind::Invalid; }
    int secondOfDay() const { return time.isValid() ? time.msecsSinceStartOfDay() / 1000 : 0; }
};

struct Diagnostic {
    int line = 0, col = 0;        // 1-based; col 0 = the whole line
    QString severity;             // error | warning | info
    QString code;                 // MPnnn (design §15)
    QString message;
    QJsonObject toJson() const;
};

struct Entry {
    int line = 0;                 // 1-based physical line (a leading BOM is not a line)
    LineKind kind = LineKind::Blank;
    QString raw;                  // decoded text without the terminator
    QByteArray bytes;             // the original bytes without the terminator: written back verbatim while !dirty
    QByteArray eol;               // terminator as text: "\r\n", "\n" or "" (only the last line may have none); serialize()
                                  // encodes it in the document's encoding
    QList<Param> params;          // TAB fields: after the stamp (header), the name / head (instruction), the path (media)

    // Header: <stamp> *(TAB field). params are the truth for rendering; these are read views filled by the parser.
    Stamp stamp;
    int playMode = -1;            // PlayMode= (-1 absent): 0 not defined, 1 normal, 2 repeat, 3 fill up, 4 wait for GPI
    int eventType = 0;            // EventType= (absent = 0 exact): 1 floating skip, 2 floating play all
    double eventDurationSec = -1; // EventDuration= (editor's computed length, may exceed 24 h)
    double expectedSec = -1;      // ExpectedDuration=
    QString name;                 // Name=

    // Instruction: "!" instr [pad (arg | params[0])] *(TAB field). The name ends at the first SPACE or TAB; a head
    // written after spaces is the argument unless it looks like key=value ("!ShowClock t=0"), then it is params[0].
    QString instr;                // name as written, without '!' ("OSD" for "!OSD off": arg "off"; identify() joins them)
    QString pad;                  // the spaces between the name and the head ("" = TAB form or name only)
    QString arg;                  // head argument (space form): a folder, a playlist, the crawl text ...
    bool argWasField = false;     // the space-form head was key=value and is params[0]

    // Media: path [bracket] *(TAB field)
    QString path;                 // as written (drive, forward slashes, UNC, bare name, mp://)
    QString bracketRaw;           // playlist-forced duration after the name, "[00:00:10;00]"; "" when none
    double forcedDurationSec = -1;

    bool dirty = false;           // re-render from the fields on serialize (set it after editing; for a header whose
                                  // stamp changed, also clear stamp.raw)

    QString value(const QString& key, const QString& def = {}) const { return paramValue(params, key, def); }
    QStringList values(const QString& key) const { return paramValues(params, key); }
    bool has(const QString& key) const { return paramIndex(params, key) >= 0; }
};

struct Event {
    int entry = -1;               // the header's index in Document::entries
    int end = -1;                 // one past the event's last entry (the next header, or entries.size())
    QList<int> items;             // instruction and media entries in order (blank and unknown lines are not items)
    // Filled by resolvePlayModes() (masterplayinstructions.h), which parse() runs:
    int effectivePlayMode = 1;    // the in-band mode when there is one, else the header PlayMode (absent or 0 = normal)
    int inBandPlayMode = -1;      // what the first play-mode instruction sets (1..4); -1 none, or !InsertPlaylist /
                                  // !Package (the sub-playlist decides). Differs from a header PlayMode: MP014
    int modeAt = -1;              // index into items of that first play-mode instruction (-1: the header decides)
    int repeatAt = -1;            // index into items of the first !Repeat / !Wiederholen (the loop is what follows it)
};

struct ParseOptions {
    int fps = 30;                         // frames in bracket durations and show/hide values (NTSC plant default)
    QString legacyDateOrder = "auto";     // dated legacy stamps: auto (DMY, MDY when the second number > 12) | dmy | mdy
};

/// The defaults write a parsed document back byte for byte: untouched lines from their original bytes, dirty ones
/// rendered from their fields, in the document's encoding, BOM and per-line terminators.
struct WriteOptions {
    /// Re-render every line from its fields, not only dirty ones (original bytes are not used; blank lines lose their
    /// spaces and TABs, a lone CR becomes a space). Instruction heads keep their own form: MasterPlay itself writes
    /// both "!ShowLogo f=" and "!ShowLogo<TAB>f=", and "!Video " with its space.
    bool canonical = false;
    StampForm stamps = StampForm::Keep;   // form of the headers that are rendered (dirty ones, or all when canonical)
    /// Target encoding (BOM included: Utf8Bom / Utf16LE / Utf16BE have one). Unset = the document's. Untouched lines
    /// keep their text: they are decoded and re-encoded when the encoding changes.
    std::optional<Encoding> encoding;
    QByteArray eol;                       // "" = each line's own terminator; "\r\n" or "\n" = that one on every line
    std::optional<bool> finalNewline;     // unset = as parsed; true adds a terminator to the last line, false drops it
    /// What MasterPlay 6.x writes: every line re-rendered, ISO stamps, UTF-8 with a BOM, CRLF, a final newline. Set
    /// stamps = StampForm::Legacy for the older playlist editor's form.
    static WriteOptions canonicalForm();
};

struct Document {
    Encoding encoding = Encoding::Utf8Bom;
    QList<Entry> entries;
    QList<Event> events;
    QList<int> preamble;          // instruction / media entries before the first header (MP008)
    QList<Diagnostic> diagnostics; // sorted by line, then column

    bool hasBom() const;
    bool finalNewline() const;    // the last line has a terminator
    QString eolStyle() const;     // crlf | lf | mixed | "" (no terminator at all)
    QByteArray defaultEol() const; // what a new line takes: the majority terminator, CRLF on a tie
    const Entry& header(const Event& ev) const { return entries.at(ev.entry); }
    /// {lines, blank, headers, instructions, media, unknown, events, preamble, items, byDayKind{}, byWeekday{"1".."7"},
    ///  byInstruction{name: n}, diagnostics{error, warning, info, byCode{MPnnn: n}}}
    QJsonObject stats() const;
    /// {encoding, bom, eol, finalNewline, stats, events[{line, stamp, playMode, eventType, name, durationSec,
    ///  expectedSec, effectivePlayMode, inBandPlayMode, repeatAt, items[]}], preamble[], diagnostics[]} (+ raw text and
    ///  unknown lines withRaw). Instruction items carry the catalog "name" ("OSD off", "Repeat" for !Wiederholen) and
    ///  "unknown": true when the catalog has no such instruction.
    QJsonObject toJson(bool withRaw = false) const;
};

Document parse(const QByteArray& bytes, const ParseOptions& o = {});
/// Byte-identical to the input for an untouched document and default options; dirty entries are rendered and encoded
/// in the target encoding. Every line but the last gets a terminator even when an edit left it without one.
/// lossyLines receives the 1-based output lines holding characters the target encoding cannot represent (cp1252:
/// written as '?').
QByteArray serialize(const Document& doc, const WriteOptions& w = {}, QList<int>* lossyLines = nullptr);
/// Classifies and parses one line (no terminator). Diagnostics go to diags with the given line number.
Entry parseLine(const QString& text, const ParseOptions& o = {}, QList<Diagnostic>* diags = nullptr, int line = 0);
/// The line text from the entry's fields. Equal to raw for a parsed entry under the default options; TAB / CR / LF
/// inside a field become spaces so an edit can never add a field or a line. w.stamps picks the header stamp form,
/// w.canonical empties blank lines.
QString renderEntry(const Entry& e, const WriteOptions& w = {});
/// "" when both documents mean the same: the same headers (day, time and fields, whatever the stamp form), items
/// (instruction name, argument and fields, whatever the head form; media path, forced duration and fields) and unknown
/// lines, in the same order. Blank lines, encoding, BOM, terminators and line numbers do not count. Otherwise the first
/// difference, with both line numbers.
QString semanticDiff(const Document& a, const Document& b);

bool parseStamp(const QString& text, const ParseOptions& o, Stamp* out, QString* why = nullptr);
/// ISO or legacy text of a parsed stamp (legacy dated = DD/MM/YYYY). The end-of-list sentinel keeps the time it was
/// written with, which is not validated; an invalid stamp gives back its raw text.
QString formatStamp(const Stamp& s, bool iso);
/// HH:MM:SS (hours may exceed 23), HH:MM:SS followed by ';' ':' or '.' and frames at fps, H:MM:SS:FF, a leading '-',
/// or plain seconds ("10", "2.5"). ok = false (and 0 returned) for anything else, including "".
double parseDuration(const QString& text, int fps, bool* ok = nullptr);
QString formatDuration(double sec, bool frames = false, int fps = 30, QChar frameSep = QLatin1Char(';'));
QString formatBracket(double sec, int fps = 30);   // "[HH:MM:SS;FF]"

/// Weekday codes: 1 Sunday .. 7 Saturday. Qt's dayOfWeek() is 1 Monday .. 7 Sunday. 0 / "" outside the range.
int qtDayOfWeek(int code);
int weekdayCode(int qtDayOfWeek);
QString weekdayName(int code);

QString encodingName(Encoding e);   // utf8bom | utf8 | cp1252 | utf16le | utf16be
/// The names above, case-insensitive, plus "ansi" / "windows-1252" for cp1252; nullopt for anything else.
std::optional<Encoding> encodingFromName(const QString& name);
QString stampFormName(StampForm f); // keep | iso | legacy
std::optional<StampForm> stampFormFromName(const QString& name);
QString lineKindName(LineKind k);   // blank | header | instruction | media | unknown
QString dayKindName(DayKind k);     // everyDay | weekly | dated | dayChange | endOfList | invalid

/// Design §15: every MPnnn code of the lane with its default severity and the stage that reports it (parser, catalog,
/// resolver, timeline, importer, exporter). Sorted by code.
struct DiagnosticInfo {
    QString code, severity, stage, title;
};
const QList<DiagnosticInfo>& diagnosticCatalog();
DiagnosticInfo diagnosticInfo(const QString& code);   // empty fields for an unknown code

} // namespace mp
