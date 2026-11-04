#pragma once
// MasterPlay instruction catalog and typed accessors (docs/plans/masterplay.md §2.3 in-band play mode, §2.5 catalogue,
// §6.4-6.9 mapping; slice MP-03). QtCore only, like masterplayin.h: AN-FS compiles the two files together, read-only.
//
// The parser does not need the catalog to parse a line (an unknown name is an ordinary instruction and round-trips
// verbatim). It needs it to diagnose and to map:
//   - identify() names an instruction line. Two-word names ("OSD off", "Set Config", "CountDown To", "CountUp From",
//     "Show TV", "Write Error") are joined from Entry::instr and the first word of Entry::arg, aliases resolve
//     ("Wiederholen" is Repeat), and a path / text head written in the TAB form (a bare first field) or shaped like
//     key=value ("!ShowCrawl Temp=72 today") still reads as the head, so the accessors see the same thing whatever
//     form the writer used.
//   - The *Spec readers turn the instructions the importer maps into typed values: InsertDirectory (TLoadingMode,
//     ToSelect, SubDirs, MaxAge, masks), InsertPlaylist, Break, ShowLogo, ShowCrawl, ShowText, the clocks, Video,
//     LiveStream and Info. They never change the entry, so the line still writes back byte for byte.
//   - resolvePlayModes() applies design §2.3: the first !Normal / !Repeat (!Wiederholen) / !Fillup / !WaitForGPI /
//     !Break / !Package / !InsertPlaylist of an event decides its play mode, and that in-band mode beats the header's
//     cached PlayMode. Lines after !ChangePlaylist never run, so the scan stops there.
//   - instructionDiagnostics() reports, with the catalog severities (parse() runs both and appends these):
//       MP011 warning  an unknown instruction name; a known key with a value the editor could not have written
//                      (LoadingMode=12, SubDirs=3, Layer=top, schedtime=soon, "!CountDown To lunch"); a key outside
//                      the catalog, or a stray bare field, on an instruction with a typed spec (the importer would
//                      drop it). Keys of the unsupported device instructions are not checked: the whole line is
//                      MP022 at import anyway, and the binary's key pools for them are incomplete.
//       MP013 warning  a bad duration in a typed key (showvalue, hidevalue, DefPTime, infoexpectedlength)
//       MP014 warning  the header PlayMode disagrees with the in-band mode (the in-band one is used)
//
// Names and keys match case-insensitively (keys are, design §2.2; names are assumed to be too [UNVERIFIED]).
#include "masterplayin.h"

#include <QJsonArray>
#include <optional>

namespace mp {

/// Every instruction of design §2.5. Legacy covers the device / UI command names from the binary tables (several catalog
/// rows share it); Unknown is anything else (MP011, kept verbatim).
enum class Instr {
    Unknown,
    Normal, Repeat, Fillup, WaitForGPI, Break, Package,                              // play-mode markers
    ChangePlaylist, InsertDirectory, InsertTopinDirectory, InsertTextDirectory, InsertPlaylist, InsertFile, ReadPlaylist,
    Video, LiveStream, Switcher,
    ShowLogo, ShowCrawl, ShowCrawlFromFile, ShowText, ShowTextFromFile, ShowClock, RealTimeClock, CountDownTo, CountUpFrom,
    OsdOff, Overlay, ShowCGC,
    TTS, DTMF, Macro, SetConfig, SendTo, Parallel, GPI, AMX, PIP, VCRControl, AutomationCard, WinTV, ChangeLayout,
    Info,
    Legacy,
};
enum class InstrGroup { PlayMode, Content, Live, Osd, Device, Info, Legacy };
/// How the importer treats it (design §6.2-6.9): structural (play mode, pods, playlist switch), mapped to a PlayPlay
/// construct, partly mapped (a snapshot or a template match), or kept in extra.masterplay and reported (MP022).
enum class InstrSupport { Structural, Mapped, Partial, Unsupported };
/// What the head (the text after the name, up to the first TAB) holds.
enum class HeadKind { None, Path, Text, Time, Option, Field };

struct InstrInfo {
    Instr id = Instr::Unknown;
    QString name;              // as the editor writes it: "Repeat", "OSD off", "Set Config"
    QStringList aliases;       // other spellings of the same instruction: "Wiederholen", "SetConfig", "Show TV"
    InstrGroup group = InstrGroup::Legacy;
    InstrSupport support = InstrSupport::Unsupported;
    HeadKind head = HeadKind::None;
    /// In-band play mode it sets: 1 normal, 2 repeat, 3 fill up, 4 wait for GPI (break mode). 0: it decides the mode
    /// without a number of its own (!InsertPlaylist, !Package: the sub-playlist does). -1: not a play-mode marker.
    int playMode = -1;
    /// Parameter keys from the writer / reader pools; "ReplaceWith*" is a prefix. The common timing keys (show,
    /// showvalue, hide, hidevalue, Name, Layer) are accepted on every row.
    QStringList keys;
    bool checkKeys = false;    // a typed spec reads it: keys outside the list are MP011
    QString evidence;          // SAMPLE / PLANT: seen in files; BIN-PE / BIN-MP / MAN: binary tables or the manual only
    QString summary;
    bool isMarker() const { return playMode >= 0; }
    bool knowsKey(const QString& key) const;
};

const QList<InstrInfo>& instructionCatalog();
const InstrInfo* instructionInfo(Instr id);              // the first row of that id; nullptr for Unknown
const InstrInfo* instructionInfo(const QString& name);   // by name or alias, case-insensitive ("osd off", "Wiederholen")
/// [{name, aliases[], group, support, head, playMode (markers only), keys[], checkKeys, evidence, summary}]
QJsonArray catalogJson();
QString groupName(InstrGroup g);       // playMode | content | live | osd | device | info | legacy
QString supportName(InstrSupport s);   // structural | mapped | partial | unsupported
QString headKindName(HeadKind h);      // none | path | text | time | option | field
QString playModeName(int mode);        // notDefined | normal | repeat | fillUp | waitForGpi ("" outside 0..4)

/// An instruction line read through the catalog.
struct Instruction {
    Instr id = Instr::Unknown;
    const InstrInfo* info = nullptr;  // the catalog row; nullptr when unknown
    QString name;              // the catalog spelling, or the name as written when unknown ("" for a non-instruction)
    QString written;           // the name as written, both words of a two-word name: "Wiederholen", "osd Off"
    bool alias = false;        // written under an alias, not the catalog name (a case difference is not an alias)
    /// The head argument: Entry::arg (minus a two-word name's second word), or the first field when the head was
    /// written in the TAB form (a bare field) or looks like key=value but is no key of this instruction.
    QString head;
    int headCol = 0;           // 1-based column of the head in Entry::raw (0 = no head)
    int headParam = -1;        // index in Entry::params the head came from (-1 = Entry::arg or none); not a parameter
    bool known() const { return info != nullptr; }
};
Instruction identify(const Entry& e);

/// TLoadingMode, PlaylistEditor's folder and sub-playlist picking modes (design §6.4).
enum class LoadingMode { Normal, Random, SelectNormal, SelectRandom, Episode, EpisodeWeekly, PictureSequence, UntilNextEvent, SelectEventNo };
/// normal | random | selectNormal | selectRandom | episode | episodeWeekly | pictureSequence | untilNextEvent | selectEventNo
QString loadingModeName(LoadingMode m);

/// !InsertDirectory and !InsertTopinDirectory (Topin weather renders, read the same way).
struct InsertDirectorySpec {
    bool topin = false;
    QString directory;         // the head as written: a folder, maybe ending in a mask (Dir\Name*.ext) or holding %...%
    QString folder, mask;      // directory split at the last separator when the last component holds '*' or '?'
    bool searchTerm = false;   // holds a %...% token: a filename search term ([VER] 274, MP020) or a SubstituteTable name
    LoadingMode loadingMode = LoadingMode::Normal;
    bool loadingModeWritten = false;   // a valid LoadingMode= (else from SelectRandom= / Random=, or Normal)
    int toSelect = 0;          // ToSelect=, or the older SelectRandom=N; 0 = not given
    bool legacySelectRandom = false;   // SelectRandom=N with no LoadingMode= (older files): SelectRandom, N picks
    int subDirs = 0;           // SubDirs= 0 this folder only, 1 recursive, 2 one file per subfolder
    double maxAgeHours = -1;   // MaxAge= in hours (empty or absent: no limit)
    bool maxAgeDelete = false; // MaxAgeDel=1
    double stillSec = -1;      // DefPTime= still display time
    bool invertOrder = false, includeInComingUp = false, transparency = false;
    /// Other catalog keys in effect (value not empty and not "0"): ReplaceWith*, EW*, Seg*, LoadSegments, TimeFlag,
    /// TimeOffset, Valid, Sunrise/SunsetOffset, AutoFill*, Ran*. The importer approximates or reports them (MP021).
    QList<Param> unmodelled;
    QStringList unknownKeys;   // keys outside the catalog (MP011)
    /// Items one block yields: max(1, toSelect) for SelectNormal / SelectRandom; 0 = the whole folder (or time-based).
    int picks() const;
    bool recursive() const { return subDirs > 0; }
};

struct InsertPlaylistSpec {
    QString path;              // the sub-playlist (.in) as written
    LoadingMode loadingMode = LoadingMode::Normal;
    bool loadingModeWritten = false;
    int toSelect = 0;
    QString timeOffset;        // TimeOffset= "subtract length from event" ([MAN]); value format [UNVERIFIED]
    bool backtime = false;     // timeOffset in effect (not empty, not "0"): the block is back-timed (design §6.5)
    bool subLfe = false, startStatistic = false, manual = false;   // sublfe=, StartStatistic=, manual=
    QString type, bc, blm, breakTime;
    QList<Param> unmodelled;   // Valid, Sunrise/SunsetOffset, ReplaceWith* in effect
    QStringList unknownKeys;
    bool untilNextEvent() const { return loadingMode == LoadingMode::UntilNextEvent; }
};

/// One GPI break pod: the items after it, up to the next !Break or the event end (design §2.7.4, §6.6).
struct BreakSpec {
    int number = -1;           // #= break number (-1 absent)
    QString gpiConfig;         // gpicfg=
    QString schedTime;         // schedtime= as written
    int schedSec = -1;         // schedtime as a second of the day; -1 absent or not understood (the pod then follows the event start)
    bool clockTriggered = false, cancel = false, gpiProceedNonStop = false, outOfOrder = false, manual = false;
    QString type, oinfo, breakTime, bc, expectedTimeUnp;
    QStringList unknownKeys;
};

/// show= / hide= say when ("at item", "in time from now", "in time from show"), showvalue= / hidevalue= how far
/// (H:MM:SS:FF, may be negative: -0:00:10:00 runs 10 s early). Design §2.5.
struct OsdTiming {
    QString show, hide;
    double showValueSec = 0, hideValueSec = 0;
    bool hasShowValue = false, hasHideValue = false;
};
struct OsdCommon {
    QString cfg;               // cfg= .osd file (position, colours, font: MP-05 reads it)
    int layer = -1;            // Layer= (-1 absent); 6 and above go to CG2 ([VER] 170)
    QString name;              // Name=
    OsdTiming timing;
    QStringList unknownKeys;
    bool cg2() const { return layer >= 6; }
};
struct ShowLogoSpec : OsdCommon {
    QString file;              // f= image (or a path head when f= is missing)
    QString loopMode;          // PlayMode= (PE2 logo loop mode; values [UNVERIFIED])
    QString scale, itemId, logoId;
};
/// !ShowCrawl and !ShowCrawlFromFile.
struct ShowCrawlSpec : OsdCommon {
    QString text;              // the head text; "<br/>" starts a new line
    QString file;              // the !ShowCrawlFromFile head, or source= (a text file)
    bool fromFile = false;     // the text comes from file: the importer snapshots it (MP028)
    int maxLines = -1;
    QString direction, fadeTime;
    bool ef = false, al = false;
    QStringList lines() const; // text split at <br/> (also <br> and <br />)
};
/// !ShowText and !ShowTextFromFile (txt, rtf, srt, sub, xml).
struct ShowTextSpec : OsdCommon {
    QString text, file;
    bool fromFile = false;
    int activeRow = -1;        // activerow=
};
enum class ClockKind { Clock, CountDown, CountUp };
/// !ShowClock, !RealTimeClock, !CountDown To <time> / NextEvent, !CountUp From <time>.
struct ClockSpec : OsdCommon {
    ClockKind kind = ClockKind::Clock;
    bool realTime = false;     // !RealTimeClock
    int type = -1;             // t= (ShowClock display type; values [UNVERIFIED])
    QString date, dct, time;   // d=, dct=, time=
    int targetSec = -1;        // CountDown To / CountUp From <time>: a second of the day
    bool toNextEvent = false;  // CountDown To NextEvent
};
struct VideoSpec {
    QString source;            // the head: live source / router text; empty for a bare "!Video "
    bool setOnly = false;      // "Set only" in the head: switch the router without playing (MP022 at import)
    bool forceLength = false, audioJukebox = false, noAudioFadeOnPreviousStill = false;
};
struct LiveStreamSpec {
    QString path, param;       // Path=, Param=
};
/// !Info: the format's comment / EPG line.
struct InfoSpec {
    QString text;              // the head text
    QString name, episode, created, contentType;
    double expectedSec = -1;   // infoexpectedlength=
    bool includeOnWeb = false, programGuide = false;   // InfoIncludeOnWeb=, InfoProgramGuide=
    QStringList unknownKeys;
};

// Typed readers: nullopt when the entry is not that instruction. Values the editor could not have written are left at
// their defaults and reported to diags (MP011 / MP013, the same diagnostics parse() gives); o.fps reads frames.
std::optional<InsertDirectorySpec> insertDirectorySpec(const Entry& e, const ParseOptions& o = {}, QList<Diagnostic>* diags = nullptr);
std::optional<InsertPlaylistSpec> insertPlaylistSpec(const Entry& e, const ParseOptions& o = {}, QList<Diagnostic>* diags = nullptr);
std::optional<BreakSpec> breakSpec(const Entry& e, const ParseOptions& o = {}, QList<Diagnostic>* diags = nullptr);
std::optional<ShowLogoSpec> showLogoSpec(const Entry& e, const ParseOptions& o = {}, QList<Diagnostic>* diags = nullptr);
std::optional<ShowCrawlSpec> showCrawlSpec(const Entry& e, const ParseOptions& o = {}, QList<Diagnostic>* diags = nullptr);
std::optional<ShowTextSpec> showTextSpec(const Entry& e, const ParseOptions& o = {}, QList<Diagnostic>* diags = nullptr);
std::optional<ClockSpec> clockSpec(const Entry& e, const ParseOptions& o = {}, QList<Diagnostic>* diags = nullptr);
std::optional<VideoSpec> videoSpec(const Entry& e);
std::optional<LiveStreamSpec> liveStreamSpec(const Entry& e);
std::optional<InfoSpec> infoSpec(const Entry& e, const ParseOptions& o = {}, QList<Diagnostic>* diags = nullptr);
QString changePlaylistTarget(const Entry& e);   // the !ChangePlaylist head ("" for a bare one or another line)
bool isRepeat(const Entry& e);                  // !Repeat or !Wiederholen
bool isOsdOff(const Entry& e);
/// Keys of an instruction with a typed spec that are not in its catalog row, plus stray non-empty bare fields (shown
/// as written). Empty for other instructions and for non-instruction lines.
QStringList unknownKeys(const Entry& e);

/// A new instruction line (dirty, no terminator: set eol, e.g. Document::defaultEol(), when it is not the last line).
/// A two-word name is split as the parser would read it. A head is written in the space form ("!InsertDirectory D:\x");
/// without one, fields follow the name after a TAB, as MasterPlay writes "!ShowLogo<TAB>f=" and "!Break<TAB>gpicfg=".
/// A text head shaped like key=value re-parses as a field; identify() still reads it as the head.
Entry makeInstruction(const QString& name, const QString& head = {}, const QList<Param>& params = {});
Entry makeInstruction(Instr id, const QString& head = {}, const QList<Param>& params = {});   // the catalog spelling; Unknown: a blank entry

/// Fills every event's inBandPlayMode, modeAt, repeatAt and effectivePlayMode from its items (parse() calls it; call it
/// again after editing instruction lines).
void resolvePlayModes(Document& d);
/// First item index of the event's loop: after its !Repeat, 0 when the play mode is repeat without a !Repeat line
/// (Saturday_0200.in: the editor shows the whole event looping [UNVERIFIED]), -1 when nothing loops. It equals
/// items.size() for a !Repeat on the last line (an empty loop).
int loopStart(const Event& ev);
/// The MP011 / MP013 / MP014 diagnostics above for a whole document, in line order (parse() appends them).
QList<Diagnostic> instructionDiagnostics(const Document& d, const ParseOptions& o = {});
/// {instructions, known, unknown, byName{catalog name: n}, unknownNames{written: n}, aliases{written: n},
///  bySupport{structural, mapped, partial, unsupported, unknown}}
QJsonObject instructionStats(const Document& d);

} // namespace mp
