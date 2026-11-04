#include "masterplayinstructions.h"

#include <QHash>
#include <QJsonObject>
#include <QRegularExpression>
#include <algorithm>
#include <climits>

namespace mp {

namespace {

// Keys most instructions accept (design §2.5 "common timing keys"): every row knows them.
const QStringList& commonKeys() {
    static const QStringList k{QStringLiteral("show"), QStringLiteral("showvalue"), QStringLiteral("hide"),
                               QStringLiteral("hidevalue"), QStringLiteral("Name"), QStringLiteral("Layer")};
    return k;
}

using G = InstrGroup;
using S = InstrSupport;
using H = HeadKind;

struct Row {
    Instr id;
    const char* name;
    const char* aliases;      // '|'-separated (an alias may hold a space: "Show TV")
    G group;
    S support;
    H head;
    int playMode;
    bool checkKeys;
    const char* keys;         // space-separated; "X*" is a prefix
    const char* evidence;
    const char* summary;
};

// InsertDirectory's writer pool (design §2.5); InsertTopinDirectory takes the same keys.
#define MP_DIR_KEYS "LoadingMode ToSelect SelectRandom Random SubDirs MaxAge MaxAgeDel DefPTime Transparency InvertOrder " \
    "RanClearIfNoneFound RanUpdateAfterPlay LoadSegments Seg* TimeFlag IncludeInComingUp ReplaceWith* TimeOffset Valid "      \
    "SunriseOffset SunsetOffset EW* AutoFill*"
#define MP_CRAWL_KEYS "cfg source maxlines ef al Direction fadetime"

// Design §2.5, in its order. Evidence: SAMPLE / PLANT = seen in files; the rest only in the binary's string pools or
// the manual. checkKeys marks the rows a typed spec reads (their key lists come from the writer pool and match every
// file seen); device rows keep their keys for reference only.
const Row kRows[] = {
    {Instr::Normal, "Normal", "", G::PlayMode, S::Structural, H::None, 1, false, "", "SAMPLE",
     "play once, then stop or fall back to the last repeat loop"},
    {Instr::Repeat, "Repeat", "Wiederholen", G::PlayMode, S::Structural, H::None, 2, false, "", "SAMPLE",
     "everything after it repeats until the next event"},
    {Instr::Fillup, "Fillup", "", G::PlayMode, S::Structural, H::None, 3, false, "", "BIN-PE", "fill-up mode (PlayMode 3) [UNVERIFIED]"},
    {Instr::WaitForGPI, "WaitForGPI", "", G::PlayMode, S::Structural, H::None, 4, false, "", "BIN-PE",
     "break mode: wait for a GPI trigger (PlayMode 4)"},
    {Instr::Break, "Break", "", G::PlayMode, S::Structural, H::None, 4, true,
     "gpicfg # oinfo schedtime clocktriggered cancel gpiproceednonstop expectedtimeunp OutOfOrder type breaktime bc manual", "PLANT",
     "one GPI-triggered break pod: the items after it up to the next !Break"},
    {Instr::Package, "Package", "", G::PlayMode, S::Structural, H::None, 0, false, "", "BIN-PE", "play-mode marker [UNVERIFIED]"},
    {Instr::ChangePlaylist, "ChangePlaylist", "", G::Content, S::Structural, H::Path, -1, false, "", "SAMPLE",
     "switch to another playlist; later lines do not run"},
    {Instr::InsertDirectory, "InsertDirectory", "", G::Content, S::Mapped, H::Path, -1, true, MP_DIR_KEYS, "SAMPLE PLANT",
     "pick N (or all) files from a folder at air time"},
    {Instr::InsertTopinDirectory, "InsertTopinDirectory", "", G::Content, S::Mapped, H::Path, -1, true, MP_DIR_KEYS, "BIN-PE",
     "Topin weather renders from a folder (read as InsertDirectory)"},
    {Instr::InsertTextDirectory, "InsertTextDirectory", "", G::Content, S::Unsupported, H::Path, -1, false, "Random SelectRandom From To Time",
     "BIN-PE", "text-file slides"},
    {Instr::InsertPlaylist, "InsertPlaylist", "", G::Content, S::Mapped, H::Path, 0, true,
     "LoadingMode ToSelect sublfe StartStatistic type bc blm breaktime manual TimeOffset Valid SunriseOffset SunsetOffset ReplaceWith*",
     "PLANT", "play another playlist inline; its breaks and repeats report to the parent"},
    {Instr::InsertFile, "InsertFile", "", G::Content, S::Unsupported, H::Path, -1, false, "", "BIN-PE", "insert a file [UNVERIFIED]"},
    {Instr::ReadPlaylist, "ReadPlaylist", "", G::Content, S::Unsupported, H::Path, -1, false, "", "BIN-PE", "read a playlist [UNVERIFIED]"},
    {Instr::Video, "Video", "", G::Live, S::Mapped, H::Text, -1, false, "AudioJukebox ForceLength NoAudioFadeOnPreviousStill", "SAMPLE",
     "video router / live source for a length (parameter layout [UNVERIFIED])"},
    {Instr::LiveStream, "LiveStream", "", G::Live, S::Mapped, H::None, -1, true, "Path Param", "BIN-PE", "live stream input"},
    {Instr::Switcher, "Switcher", "", G::Live, S::Unsupported, H::None, -1, false, "PVW PGM PST Key KeySelect Logo Take", "BIN-PE",
     "vision mixer control"},
    {Instr::ShowLogo, "ShowLogo", "", G::Osd, S::Mapped, H::Field, -1, true, "f cfg PlayMode Scale ItemID LogoID", "SAMPLE PLANT",
     "persistent logo until !OSD off or a replacement"},
    {Instr::ShowCrawl, "ShowCrawl", "", G::Osd, S::Mapped, H::Text, -1, true, MP_CRAWL_KEYS, "SAMPLE", "scrolling text (<br/> = new line)"},
    {Instr::ShowCrawlFromFile, "ShowCrawlFromFile", "", G::Osd, S::Partial, H::Path, -1, true, MP_CRAWL_KEYS, "BIN-PE",
     "crawl text from a file (snapshot at import, MP028)"},
    {Instr::ShowText, "ShowText", "", G::Osd, S::Mapped, H::Text, -1, true, "activerow cfg", "BIN-PE", "static text"},
    {Instr::ShowTextFromFile, "ShowTextFromFile", "", G::Osd, S::Partial, H::Path, -1, true, "activerow cfg", "BIN-PE",
     "subtitle / text file (txt, rtf, srt, sub, xml)"},
    {Instr::ShowClock, "ShowClock", "", G::Osd, S::Mapped, H::Field, -1, true, "t d dct time cfg", "SAMPLE", "real-time clock / counter"},
    {Instr::RealTimeClock, "RealTimeClock", "", G::Osd, S::Mapped, H::None, -1, true, "cfg", "BIN-PE", "real-time clock"},
    {Instr::CountDownTo, "CountDown To", "", G::Osd, S::Mapped, H::Time, -1, true, "cfg", "BIN-PE", "count down to a time or NextEvent"},
    {Instr::CountUpFrom, "CountUp From", "", G::Osd, S::Unsupported, H::Time, -1, false, "cfg", "BIN-PE", "count up from a time"},
    {Instr::OsdOff, "OSD off", "", G::Osd, S::Mapped, H::None, -1, false, "", "PLANT", "clear the OSD layers (reverse creation order)"},
    {Instr::Overlay, "Overlay", "", G::Osd, S::Partial, H::None, -1, false,
     "Name Function FadeInDuration FadeOutDuration Template Tag* Source* Text* TimeFormat DateFormat VClip XPos YPos Repeat Frequency Audio Crawl_Data",
     "BIN-PE", "PE2 overlay template (mapped when the template id exists, else MP022)"},
    {Instr::ShowCGC, "ShowCGC", "", G::Osd, S::Partial, H::None, -1, false, "cgc resume CGClearFirst CGUnloadLayoutAtFileEnd RmFrameOut",
     "BIN-PE", "Medialooks CG layout (mapped when the template id exists, else MP022)"},
    {Instr::TTS, "TTS", "", G::Device, S::Unsupported, H::None, -1, false, "text name", "SAMPLE", "text to speech"},
    {Instr::DTMF, "DTMF", "", G::Device, S::Unsupported, H::None, -1, false, "Text", "SAMPLE", "send DTMF tones"},
    {Instr::Macro, "Macro", "", G::Device, S::Unsupported, H::None, -1, false, "Instructions File3D", "SAMPLE", "macro of instructions"},
    {Instr::SetConfig, "Set Config", "SetConfig", G::Device, S::Unsupported, H::Option, -1, false, "Name", "BIN-PE",
     "runtime configuration (logo, crawl, SCTE-35, out-of-network indicator ...)"},
    {Instr::SendTo, "SendTo", "", G::Device, S::Unsupported, H::Option, -1, false, "waitforstring Source", "BIN-PE",
     "send a string to COM / MIDI / UDP / FTP / HTTP"},
    {Instr::Parallel, "Parallel", "", G::Device, S::Unsupported, H::Option, -1, false, "PortNo HoldTime", "BIN-PE",
     "parallel / serial GPO bits"},
    {Instr::GPI, "GPI", "", G::Device, S::Unsupported, H::None, -1, false, "", "BIN-PE", "GPI out"},
    {Instr::AMX, "AMX", "", G::Device, S::Unsupported, H::None, -1, false, "", "BIN-PE", "wait for an AMX controller"},
    {Instr::PIP, "PIP", "", G::Device, S::Unsupported, H::None, -1, false, "", "BIN-PE", "Kramer PIP-200"},
    {Instr::VCRControl, "VCRControl", "", G::Device, S::Unsupported, H::Field, -1, false, "VCR# tcin t1", "BIN-PE", "Sony P2 deck"},
    {Instr::AutomationCard, "AutomationCard", "", G::Device, S::Unsupported, H::Field, -1, false,
     "CardNo OutputNo OutputAction DiskID Title Chapter Language SubTitle", "BIN-PE", "DVD / automation card"},
    {Instr::WinTV, "WinTV", "Show TV", G::Device, S::Unsupported, H::Text, -1, false, "Length Vol", "BIN-PE", "TV tuner (MultiZone)"},
    {Instr::ChangeLayout, "ChangeLayout", "", G::Device, S::Unsupported, H::Path, -1, false, "", "BIN-PE", "MultiZone layout (.zfg)"},
    {Instr::Info, "Info", "", G::Info, S::Mapped, H::Text, -1, true,
     "episode created infoexpectedlength ContentType InfoIncludeOnWeb InfoProgramGuide OverUnderEmailNotification", "BIN-PE",
     "memo / EPG info line: the format's comment"},
    // Device / UI command names from the binary tables; some may be menu strings rather than instructions [UNVERIFIED].
    // Known, so not MP011, and unsupported (MP022 at import).
    {Instr::Legacy, "Camera", "", G::Legacy, S::Unsupported, H::Text, -1, false, "", "BIN-PE", "camera control"},
    {Instr::Legacy, "KameraDirect", "", G::Legacy, S::Unsupported, H::Text, -1, false, "", "BIN-PE", "camera control (direct)"},
    {Instr::Legacy, "GPSStations", "", G::Legacy, S::Unsupported, H::Text, -1, false, "", "BIN-PE", "GPS stations"},
    {Instr::Legacy, "Graphic", "", G::Legacy, S::Unsupported, H::Text, -1, false, "", "BIN-PE", "graphic"},
    {Instr::Legacy, "GrassValley", "", G::Legacy, S::Unsupported, H::Text, -1, false, "", "BIN-PE", "Grass Valley device"},
    {Instr::Legacy, "DigiMix", "", G::Legacy, S::Unsupported, H::Text, -1, false, "", "BIN-PE", "DigiMix device"},
    {Instr::Legacy, "MaderSDUS", "", G::Legacy, S::Unsupported, H::Text, -1, false, "", "BIN-PE", "Mader SDUS device"},
    {Instr::Legacy, "Snype", "", G::Legacy, S::Unsupported, H::Text, -1, false, "", "BIN-PE", "Snype device"},
    {Instr::Legacy, "Promter", "", G::Legacy, S::Unsupported, H::Text, -1, false, "", "BIN-PE", "prompter"},
    {Instr::Legacy, "Select", "", G::Legacy, S::Unsupported, H::Text, -1, false, "", "BIN-PE", "select"},
    {Instr::Legacy, "Start", "", G::Legacy, S::Unsupported, H::Text, -1, false, "", "BIN-PE", "start"},
    {Instr::Legacy, "Flip", "", G::Legacy, S::Unsupported, H::Text, -1, false, "", "BIN-PE", "flip text"},
    {Instr::Legacy, "Fliptext", "", G::Legacy, S::Unsupported, H::Text, -1, false, "", "BIN-PE", "flip text"},
    {Instr::Legacy, "BackgroundAudio", "", G::Legacy, S::Unsupported, H::Text, -1, false, "", "BIN-PE", "background audio"},
    {Instr::Legacy, "CueOnly", "", G::Legacy, S::Unsupported, H::Text, -1, false, "", "BIN-PE", "cue only"},
    {Instr::Legacy, "Playback", "", G::Legacy, S::Unsupported, H::Text, -1, false, "", "BIN-PE", "playback"},
    {Instr::Legacy, "Logo", "", G::Legacy, S::Unsupported, H::Text, -1, false, "", "BIN-PE", "logo"},
    {Instr::Legacy, "Remove", "", G::Legacy, S::Unsupported, H::Text, -1, false, "", "BIN-PE", "remove"},
    {Instr::Legacy, "Move", "", G::Legacy, S::Unsupported, H::Text, -1, false, "", "BIN-PE", "move"},
    {Instr::Legacy, "Paste", "", G::Legacy, S::Unsupported, H::Text, -1, false, "", "BIN-PE", "paste"},
    {Instr::Legacy, "Write Error", "", G::Legacy, S::Unsupported, H::Text, -1, false, "", "BIN-MP", "write an error line"},
};
#undef MP_DIR_KEYS
#undef MP_CRAWL_KEYS

QStringList splitList(const char* s, char sep) {
    QStringList out;
    for (const QString& p : QString::fromLatin1(s).split(QLatin1Char(sep), Qt::SkipEmptyParts)) out << p;
    return out;
}

// name / alias (case-folded) -> row; built once.
const QHash<QString, const InstrInfo*>& nameIndex() {
    static const QHash<QString, const InstrInfo*> idx = [] {
        QHash<QString, const InstrInfo*> h;
        for (const InstrInfo& i : instructionCatalog()) {
            h.insert(i.name.toLower(), &i);
            for (const QString& a : i.aliases) h.insert(a.toLower(), &i);
        }
        return h;
    }();
    return idx;
}

bool listHasKey(const QStringList& list, const QString& key) {
    for (const QString& k : list) {
        if (k.endsWith(u'*') ? key.startsWith(QStringView(k).chopped(1), Qt::CaseInsensitive) : k.compare(key, Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

// 1-based column of params[i] in e.raw (0 when out of range). The TAB split of raw has the stamp / path / "!name head"
// in its first segment, except that a space-form key=value head is params[0] itself.
int fieldColumn(const Entry& e, int i) {
    if (i < 0 || i >= e.params.size()) return 0;
    const bool fieldHead = e.kind == LineKind::Instruction && !e.pad.isEmpty() && e.argWasField;
    if (fieldHead && i == 0) return 2 + int(e.instr.size() + e.pad.size());
    const int seg = fieldHead ? i : i + 1;
    qsizetype p = 0;
    for (int k = 0; k < seg; ++k) {
        p = e.raw.indexOf(u'\t', p);
        if (p < 0) return 0;
        ++p;
    }
    return int(p) + 1;
}

// A value that switches an option on: not empty and not "0" (the editor writes "ReplaceWithDirectoryMaxAge=0",
// "MaxAge=" for "off").
bool inEffect(const QString& v) {
    const QString t = v.trimmed();
    return !t.isEmpty() && t != u"0";
}

// Reads an instruction's typed keys and reports values the editor could not have written. Keys are latin-1 literals.
struct Reader {
    const Entry& e;
    const Instruction& ins;
    const ParseOptions& o;
    QList<Diagnostic>* diags;

    QString what() const { return QLatin1Char('!') + ins.name; }
    int index(const char* key) const { return paramIndex(e.params, QString::fromLatin1(key)); }
    QString str(const char* key) const { return e.value(QString::fromLatin1(key)); }
    bool flag(const char* key) const {
        const QString v = str(key).trimmed();
        return v == u"1" || v.compare(u"true", Qt::CaseInsensitive) == 0;
    }
    void report(int col, const char* code, const QString& message) const {
        if (diags) diags->append(Diagnostic{e.line, col, QStringLiteral("warning"), QString::fromLatin1(code), message});
    }
    // One arg() pass: values are file text and may hold "%1".
    void badValue(int i, const char* code, const QString& expected) const {
        const Param& p = e.params.at(i);
        report(fieldColumn(e, i), code, QStringLiteral("%1 %2=%3: expected %4; ignored").arg(what(), p.key, p.value, expected));
    }
    // An integer key: absent or empty gives def; not a number or outside lo..hi is MP011 and gives def.
    int integer(const char* key, int lo, int hi, int def) const {
        const int i = index(key);
        if (i < 0) return def;
        const QString v = e.params.at(i).value.trimmed();
        if (v.isEmpty()) return def;
        bool ok = false;
        const int n = v.toInt(&ok);
        if (ok && n >= lo && n <= hi) return n;
        badValue(i, "MP011", hi == INT_MAX ? QStringLiteral("a whole number from %1").arg(lo) : QStringLiteral("a whole number %1..%2").arg(lo).arg(hi));
        return def;
    }
    double number(const char* key, double def, const QString& expected) const {
        const int i = index(key);
        if (i < 0) return def;
        const QString v = e.params.at(i).value.trimmed();
        if (v.isEmpty()) return def;
        bool ok = false;
        const double n = v.toDouble(&ok);
        if (ok && n >= 0) return n;
        badValue(i, "MP011", expected);
        return def;
    }
    // A duration key (HH:MM:SS, H:MM:SS:FF, frames at o.fps, seconds, may be negative): bad text is MP013.
    double duration(const char* key, double def, bool* has = nullptr) const {
        const int i = index(key);
        if (has) *has = false;
        if (i < 0) return def;
        const QString v = e.params.at(i).value;
        if (v.trimmed().isEmpty()) return def;
        bool ok = false;
        const double d = parseDuration(v, o.fps, &ok);
        if (!ok) { badValue(i, "MP013", QStringLiteral("a duration (HH:MM:SS, H:MM:SS:FF at %1 fps, or seconds)").arg(o.fps)); return def; }
        if (has) *has = true;
        return d;
    }
    // Catalog keys outside `typed` whose value is in effect, as written.
    QList<Param> unmodelled(const QStringList& typed) const {
        QList<Param> out;
        for (int i = 0; i < e.params.size(); ++i) {
            const Param& p = e.params.at(i);
            if (i == ins.headParam || p.bare || !inEffect(p.value)) continue;
            if (listHasKey(typed, p.key) || listHasKey(commonKeys(), p.key) || !listHasKey(ins.info->keys, p.key)) continue;
            out << p;
        }
        return out;
    }
};

// A second of the day from "hh:mm:ss" (frames allowed) or plain seconds; -1 when it is not one.
int timeOfDay(const QString& text, int fps) {
    bool ok = false;
    const double v = parseDuration(text, fps, &ok);
    return ok && v >= 0 && v < 86400 ? int(v) : -1;
}

void readOsdCommon(const Reader& r, OsdCommon& c) {
    c.cfg = r.str("cfg");
    c.layer = r.integer("Layer", 0, INT_MAX, -1);
    c.name = r.str("Name");
    c.timing.show = r.str("show");
    c.timing.hide = r.str("hide");
    c.timing.showValueSec = r.duration("showvalue", 0, &c.timing.hasShowValue);
    c.timing.hideValueSec = r.duration("hidevalue", 0, &c.timing.hasHideValue);
    c.unknownKeys = unknownKeys(r.e);
}

// The typed readers on an identified instruction (the public wrappers identify first; the diagnostics pass reuses
// its own identify()).
std::optional<InsertDirectorySpec> readInsertDirectory(const Entry& e, const Instruction& ins, const ParseOptions& o, QList<Diagnostic>* diags) {
    if (ins.id != Instr::InsertDirectory && ins.id != Instr::InsertTopinDirectory) return std::nullopt;
    const Reader r{e, ins, o, diags};
    InsertDirectorySpec s;
    s.topin = ins.id == Instr::InsertTopinDirectory;
    s.directory = ins.head;
    // "Dir\Name*.ext" (design §6.4: a folder plus filename rules). Only the last component can be a mask.
    const qsizetype sep = qMax(s.directory.lastIndexOf(u'\\'), s.directory.lastIndexOf(u'/'));
    const QString last = s.directory.mid(sep + 1);
    if (last.contains(u'*') || last.contains(u'?')) { s.folder = sep >= 0 ? s.directory.left(sep) : QString(); s.mask = last; }
    else s.folder = s.directory;
    static const QRegularExpression kTerm(QStringLiteral("%[^%\\\\/]+%"));
    s.searchTerm = kTerm.match(s.directory).hasMatch();
    const int lm = r.integer("LoadingMode", 0, 8, -1);
    s.toSelect = r.integer("ToSelect", 0, INT_MAX, 0);
    const int legacyCount = r.integer("SelectRandom", 0, INT_MAX, 0);   // [SAMPLE] "SelectRandom=2": older files
    if (lm >= 0) { s.loadingMode = LoadingMode(lm); s.loadingModeWritten = true; }
    else if (legacyCount > 0) { s.loadingMode = LoadingMode::SelectRandom; s.legacySelectRandom = true; }
    else if (r.flag("Random")) s.loadingMode = LoadingMode::Random;
    if (s.toSelect == 0) s.toSelect = legacyCount;
    s.subDirs = r.integer("SubDirs", 0, 2, 0);
    s.maxAgeHours = r.number("MaxAge", -1, QStringLiteral("hours (empty = no limit)"));
    s.maxAgeDelete = r.flag("MaxAgeDel");
    s.stillSec = r.duration("DefPTime", -1);
    s.invertOrder = r.flag("InvertOrder");
    s.includeInComingUp = r.flag("IncludeInComingUp");
    s.transparency = r.flag("Transparency");
    static const QStringList kTyped{QStringLiteral("LoadingMode"), QStringLiteral("ToSelect"), QStringLiteral("SelectRandom"),
                                    QStringLiteral("Random"), QStringLiteral("SubDirs"), QStringLiteral("MaxAge"), QStringLiteral("MaxAgeDel"),
                                    QStringLiteral("DefPTime"), QStringLiteral("InvertOrder"), QStringLiteral("IncludeInComingUp"),
                                    QStringLiteral("Transparency")};
    s.unmodelled = r.unmodelled(kTyped);
    s.unknownKeys = unknownKeys(e);
    return s;
}

std::optional<InsertPlaylistSpec> readInsertPlaylist(const Entry& e, const Instruction& ins, const ParseOptions& o, QList<Diagnostic>* diags) {
    if (ins.id != Instr::InsertPlaylist) return std::nullopt;
    const Reader r{e, ins, o, diags};
    InsertPlaylistSpec s;
    s.path = ins.head;
    const int lm = r.integer("LoadingMode", 0, 8, -1);
    if (lm >= 0) { s.loadingMode = LoadingMode(lm); s.loadingModeWritten = true; }
    s.toSelect = r.integer("ToSelect", 0, INT_MAX, 0);
    s.timeOffset = r.str("TimeOffset");
    s.backtime = inEffect(s.timeOffset);
    s.subLfe = r.flag("sublfe");
    s.startStatistic = r.flag("StartStatistic");
    s.manual = r.flag("manual");
    s.type = r.str("type");
    s.bc = r.str("bc");
    s.blm = r.str("blm");
    s.breakTime = r.str("breaktime");
    static const QStringList kTyped{QStringLiteral("LoadingMode"), QStringLiteral("ToSelect"), QStringLiteral("TimeOffset"),
                                    QStringLiteral("sublfe"), QStringLiteral("StartStatistic"), QStringLiteral("manual"),
                                    QStringLiteral("type"), QStringLiteral("bc"), QStringLiteral("blm"), QStringLiteral("breaktime")};
    s.unmodelled = r.unmodelled(kTyped);
    s.unknownKeys = unknownKeys(e);
    return s;
}

std::optional<BreakSpec> readBreak(const Entry& e, const Instruction& ins, const ParseOptions& o, QList<Diagnostic>* diags) {
    if (ins.id != Instr::Break) return std::nullopt;
    const Reader r{e, ins, o, diags};
    BreakSpec s;
    s.number = r.integer("#", 0, INT_MAX, -1);
    s.gpiConfig = r.str("gpicfg");
    const int st = r.index("schedtime");
    if (st >= 0 && !e.params.at(st).value.trimmed().isEmpty()) {
        s.schedTime = e.params.at(st).value;
        s.schedSec = timeOfDay(s.schedTime, o.fps);
        if (s.schedSec < 0) r.badValue(st, "MP011", QStringLiteral("a time of day hh:mm:ss (the pod then follows the event start)"));
    }
    s.clockTriggered = r.flag("clocktriggered");
    s.cancel = r.flag("cancel");
    s.gpiProceedNonStop = r.flag("gpiproceednonstop");
    s.outOfOrder = r.flag("OutOfOrder");
    s.manual = r.flag("manual");
    s.type = r.str("type");
    s.oinfo = r.str("oinfo");
    s.breakTime = r.str("breaktime");
    s.bc = r.str("bc");
    s.expectedTimeUnp = r.str("expectedtimeunp");
    s.unknownKeys = unknownKeys(e);
    return s;
}

std::optional<ShowLogoSpec> readShowLogo(const Entry& e, const Instruction& ins, const ParseOptions& o, QList<Diagnostic>* diags) {
    if (ins.id != Instr::ShowLogo) return std::nullopt;
    const Reader r{e, ins, o, diags};
    ShowLogoSpec s;
    readOsdCommon(r, s);
    s.file = r.str("f");
    if (s.file.isEmpty()) s.file = ins.head;   // "!ShowLogo D:\x.png": a path head instead of f=
    s.loopMode = r.str("PlayMode");
    s.scale = r.str("Scale");
    s.itemId = r.str("ItemID");
    s.logoId = r.str("LogoID");
    return s;
}

std::optional<ShowCrawlSpec> readShowCrawl(const Entry& e, const Instruction& ins, const ParseOptions& o, QList<Diagnostic>* diags) {
    if (ins.id != Instr::ShowCrawl && ins.id != Instr::ShowCrawlFromFile) return std::nullopt;
    const Reader r{e, ins, o, diags};
    ShowCrawlSpec s;
    readOsdCommon(r, s);
    if (ins.id == Instr::ShowCrawlFromFile) { s.file = ins.head; s.fromFile = true; }
    else s.text = ins.head;
    const QString source = r.str("source");
    if (!source.trimmed().isEmpty()) { s.file = source; s.fromFile = true; }
    s.maxLines = r.integer("maxlines", 0, INT_MAX, -1);
    s.direction = r.str("Direction");
    s.fadeTime = r.str("fadetime");
    s.ef = r.flag("ef");
    s.al = r.flag("al");
    return s;
}

std::optional<ShowTextSpec> readShowText(const Entry& e, const Instruction& ins, const ParseOptions& o, QList<Diagnostic>* diags) {
    if (ins.id != Instr::ShowText && ins.id != Instr::ShowTextFromFile) return std::nullopt;
    const Reader r{e, ins, o, diags};
    ShowTextSpec s;
    readOsdCommon(r, s);
    if (ins.id == Instr::ShowTextFromFile) { s.file = ins.head; s.fromFile = true; }
    else s.text = ins.head;
    s.activeRow = r.integer("activerow", 0, INT_MAX, -1);
    return s;
}

std::optional<ClockSpec> readClock(const Entry& e, const Instruction& ins, const ParseOptions& o, QList<Diagnostic>* diags) {
    if (ins.id != Instr::ShowClock && ins.id != Instr::RealTimeClock && ins.id != Instr::CountDownTo && ins.id != Instr::CountUpFrom)
        return std::nullopt;
    const Reader r{e, ins, o, diags};
    ClockSpec s;
    readOsdCommon(r, s);
    s.realTime = ins.id == Instr::RealTimeClock;
    s.type = r.integer("t", 0, INT_MAX, -1);
    s.date = r.str("d");
    s.dct = r.str("dct");
    s.time = r.str("time");
    if (ins.id == Instr::CountDownTo || ins.id == Instr::CountUpFrom) {
        const bool down = ins.id == Instr::CountDownTo;
        s.kind = down ? ClockKind::CountDown : ClockKind::CountUp;
        if (down && ins.head.trimmed().compare(u"NextEvent", Qt::CaseInsensitive) == 0) s.toNextEvent = true;
        else {
            s.targetSec = timeOfDay(ins.head, o.fps);
            if (s.targetSec < 0)   // one arg() pass: the head is file text
                r.report(ins.headCol ? ins.headCol : 1, "MP011",
                         QStringLiteral("%1 \"%2\": expected a time of day hh:mm:ss%3; the clock has no target")
                             .arg(r.what(), ins.head, down ? QStringLiteral(" or NextEvent") : QString()));
        }
    }
    return s;
}

std::optional<InfoSpec> readInfo(const Entry& e, const Instruction& ins, const ParseOptions& o, QList<Diagnostic>* diags) {
    if (ins.id != Instr::Info) return std::nullopt;
    const Reader r{e, ins, o, diags};
    InfoSpec s;
    s.text = ins.head;
    s.name = r.str("Name");
    s.episode = r.str("episode");
    s.created = r.str("created");
    s.contentType = r.str("ContentType");
    s.expectedSec = r.duration("infoexpectedlength", -1);
    s.includeOnWeb = r.flag("InfoIncludeOnWeb");
    s.programGuide = r.flag("InfoProgramGuide");
    s.unknownKeys = unknownKeys(e);
    return s;
}

// Value checks of the typed instructions (the readers report as they read).
void validate(const Entry& e, const Instruction& ins, const ParseOptions& o, QList<Diagnostic>* diags) {
    switch (ins.id) {
    case Instr::InsertDirectory:
    case Instr::InsertTopinDirectory: readInsertDirectory(e, ins, o, diags); break;
    case Instr::InsertPlaylist: readInsertPlaylist(e, ins, o, diags); break;
    case Instr::Break: readBreak(e, ins, o, diags); break;
    case Instr::ShowLogo: readShowLogo(e, ins, o, diags); break;
    case Instr::ShowCrawl:
    case Instr::ShowCrawlFromFile: readShowCrawl(e, ins, o, diags); break;
    case Instr::ShowText:
    case Instr::ShowTextFromFile: readShowText(e, ins, o, diags); break;
    case Instr::ShowClock:
    case Instr::RealTimeClock:
    case Instr::CountDownTo:
    case Instr::CountUpFrom: readClock(e, ins, o, diags); break;
    case Instr::Info: readInfo(e, ins, o, diags); break;
    default: break;
    }
}

// The play-mode scan of one event (design §2.3). mode: what the first marker sets (-1 none, or a marker without a
// number); modeAt / repeatAt: item indexes (-1 none).
struct Scan {
    int mode = -1, modeAt = -1, repeatAt = -1;
};
Scan scanEvent(const Document& d, const Event& ev) {
    Scan s;
    for (int k = 0; k < ev.items.size(); ++k) {
        const Entry& e = d.entries.at(ev.items.at(k));
        if (e.kind != LineKind::Instruction) continue;
        const Instruction ins = identify(e);
        if (!ins.known()) continue;
        if (s.modeAt < 0 && ins.info->isMarker()) { s.modeAt = k; s.mode = ins.info->playMode > 0 ? ins.info->playMode : -1; }
        if (s.repeatAt < 0 && ins.id == Instr::Repeat) s.repeatAt = k;
        if (ins.id == Instr::ChangePlaylist) break;   // later lines never run (design §2.7.7)
    }
    return s;
}

} // namespace

// ---- catalog -----------------------------------------------------------------------------------------------------

bool InstrInfo::knowsKey(const QString& key) const { return listHasKey(keys, key) || listHasKey(commonKeys(), key); }

const QList<InstrInfo>& instructionCatalog() {
    static const QList<InstrInfo> list = [] {
        QList<InstrInfo> l;
        for (const Row& r : kRows)
            l.append(InstrInfo{r.id, QString::fromLatin1(r.name), splitList(r.aliases, '|'), r.group, r.support, r.head, r.playMode,
                               splitList(r.keys, ' '), r.checkKeys, QString::fromLatin1(r.evidence), QString::fromLatin1(r.summary)});
        return l;
    }();
    return list;
}

const InstrInfo* instructionInfo(Instr id) {
    for (const InstrInfo& i : instructionCatalog())
        if (i.id == id) return &i;
    return nullptr;
}

const InstrInfo* instructionInfo(const QString& name) { return nameIndex().value(name.simplified().toLower(), nullptr); }

QJsonArray catalogJson() {
    QJsonArray a;
    for (const InstrInfo& i : instructionCatalog()) {
        QJsonObject o{{"name", i.name}, {"aliases", QJsonArray::fromStringList(i.aliases)}, {"group", groupName(i.group)},
                      {"support", supportName(i.support)}, {"head", headKindName(i.head)}, {"keys", QJsonArray::fromStringList(i.keys)},
                      {"checkKeys", i.checkKeys}, {"evidence", i.evidence}, {"summary", i.summary}};
        if (i.isMarker()) o["playMode"] = i.playMode;
        a.append(o);
    }
    return a;
}

QString groupName(InstrGroup g) {
    switch (g) {
    case G::PlayMode: return QStringLiteral("playMode");
    case G::Content: return QStringLiteral("content");
    case G::Live: return QStringLiteral("live");
    case G::Osd: return QStringLiteral("osd");
    case G::Device: return QStringLiteral("device");
    case G::Info: return QStringLiteral("info");
    case G::Legacy: return QStringLiteral("legacy");
    }
    return {};
}

QString supportName(InstrSupport s) {
    switch (s) {
    case S::Structural: return QStringLiteral("structural");
    case S::Mapped: return QStringLiteral("mapped");
    case S::Partial: return QStringLiteral("partial");
    case S::Unsupported: return QStringLiteral("unsupported");
    }
    return {};
}

QString headKindName(HeadKind h) {
    switch (h) {
    case H::None: return QStringLiteral("none");
    case H::Path: return QStringLiteral("path");
    case H::Text: return QStringLiteral("text");
    case H::Time: return QStringLiteral("time");
    case H::Option: return QStringLiteral("option");
    case H::Field: return QStringLiteral("field");
    }
    return {};
}

QString playModeName(int mode) {
    static const char* const kNames[] = {"notDefined", "normal", "repeat", "fillUp", "waitForGpi"};
    return mode >= 0 && mode <= 4 ? QString::fromLatin1(kNames[mode]) : QString();
}

QString loadingModeName(LoadingMode m) {
    static const char* const kNames[] = {"normal", "random", "selectNormal", "selectRandom", "episode", "episodeWeekly",
                                         "pictureSequence", "untilNextEvent", "selectEventNo"};
    const int i = int(m);
    return i >= 0 && i <= 8 ? QString::fromLatin1(kNames[i]) : QString();
}

int InsertDirectorySpec::picks() const {
    return loadingMode == LoadingMode::SelectNormal || loadingMode == LoadingMode::SelectRandom ? qMax(1, toSelect) : 0;
}

QStringList ShowCrawlSpec::lines() const {
    static const QRegularExpression kBr(QStringLiteral("<br\\s*/?>"), QRegularExpression::CaseInsensitiveOption);
    return text.split(kBr);
}

// ---- identify ----------------------------------------------------------------------------------------------------

Instruction identify(const Entry& e) {
    Instruction r;
    if (e.kind != LineKind::Instruction) return r;
    r.name = r.written = e.instr;
    const int argCol = 2 + int(e.instr.size() + e.pad.size());   // where Entry::arg starts in raw
    // Two-word names: "!OSD off", "!Set Config Crawl", "!CountDown To NextEvent", "!Show TV CNN". Only the space form:
    // the parser cut the name at the space, and the second word is the start of the argument.
    if (!e.pad.isEmpty() && !e.arg.isEmpty()) {
        const qsizetype sp = e.arg.indexOf(u' ');
        const qsizetype secondEnd = sp < 0 ? e.arg.size() : sp;
        const QString two = e.instr + QLatin1Char(' ') + e.arg.left(secondEnd);
        if (const InstrInfo* info = nameIndex().value(two.toLower(), nullptr)) {
            r.info = info;
            r.id = info->id;
            r.name = info->name;
            r.written = two;
            r.alias = two.compare(info->name, Qt::CaseInsensitive) != 0;
            qsizetype h = secondEnd;
            while (h < e.arg.size() && e.arg.at(h) == u' ') ++h;
            r.head = e.arg.mid(h);
            if (!r.head.isEmpty()) r.headCol = argCol + int(h);
            return r;
        }
    }
    // One word: the name cuts at the first space, so "!OSD" alone can never hit the "osd off" key.
    const InstrInfo* info = nameIndex().value(e.instr.toLower(), nullptr);
    if (info) {
        r.info = info;
        r.id = info->id;
        r.name = info->name;
        r.alias = e.instr.compare(info->name, Qt::CaseInsensitive) != 0;
    }
    if (!e.arg.isEmpty()) {
        r.head = e.arg;
        r.headCol = argCol;
    } else if (info && !e.params.isEmpty() && info->head != H::None && info->head != H::Field) {
        // A path / text / time / option head written as a TAB field (the parser saw a bare first field), or text that
        // happens to look like key=value ("!ShowCrawl Temp=72 today") and is no key of this instruction.
        const Param& p0 = e.params.first();
        if (e.argWasField && !info->knowsKey(p0.key)) r.head = p0.text();
        else if (e.pad.isEmpty() && p0.bare && !p0.value.isEmpty()) r.head = p0.value;
        if (!r.head.isEmpty()) { r.headParam = 0; r.headCol = fieldColumn(e, 0); }
    }
    return r;
}

// ---- typed readers -----------------------------------------------------------------------------------------------

std::optional<InsertDirectorySpec> insertDirectorySpec(const Entry& e, const ParseOptions& o, QList<Diagnostic>* diags) {
    return readInsertDirectory(e, identify(e), o, diags);
}
std::optional<InsertPlaylistSpec> insertPlaylistSpec(const Entry& e, const ParseOptions& o, QList<Diagnostic>* diags) {
    return readInsertPlaylist(e, identify(e), o, diags);
}
std::optional<BreakSpec> breakSpec(const Entry& e, const ParseOptions& o, QList<Diagnostic>* diags) { return readBreak(e, identify(e), o, diags); }
std::optional<ShowLogoSpec> showLogoSpec(const Entry& e, const ParseOptions& o, QList<Diagnostic>* diags) {
    return readShowLogo(e, identify(e), o, diags);
}
std::optional<ShowCrawlSpec> showCrawlSpec(const Entry& e, const ParseOptions& o, QList<Diagnostic>* diags) {
    return readShowCrawl(e, identify(e), o, diags);
}
std::optional<ShowTextSpec> showTextSpec(const Entry& e, const ParseOptions& o, QList<Diagnostic>* diags) {
    return readShowText(e, identify(e), o, diags);
}
std::optional<ClockSpec> clockSpec(const Entry& e, const ParseOptions& o, QList<Diagnostic>* diags) { return readClock(e, identify(e), o, diags); }
std::optional<InfoSpec> infoSpec(const Entry& e, const ParseOptions& o, QList<Diagnostic>* diags) { return readInfo(e, identify(e), o, diags); }

std::optional<VideoSpec> videoSpec(const Entry& e) {
    const Instruction ins = identify(e);
    if (ins.id != Instr::Video) return std::nullopt;
    VideoSpec s;
    s.source = ins.head;
    s.setOnly = s.source.contains(QStringLiteral("Set only"), Qt::CaseInsensitive);
    auto flag = [&](const char* k) { const QString v = e.value(QString::fromLatin1(k)).trimmed(); return v == u"1" || v.compare(u"true", Qt::CaseInsensitive) == 0; };
    s.forceLength = flag("ForceLength");
    s.audioJukebox = flag("AudioJukebox");
    s.noAudioFadeOnPreviousStill = flag("NoAudioFadeOnPreviousStill");
    return s;
}

std::optional<LiveStreamSpec> liveStreamSpec(const Entry& e) {
    if (identify(e).id != Instr::LiveStream) return std::nullopt;
    return LiveStreamSpec{e.value(QStringLiteral("Path")), e.value(QStringLiteral("Param"))};
}

QString changePlaylistTarget(const Entry& e) {
    const Instruction ins = identify(e);
    return ins.id == Instr::ChangePlaylist ? ins.head : QString();
}

bool isRepeat(const Entry& e) { return identify(e).id == Instr::Repeat; }
bool isOsdOff(const Entry& e) { return identify(e).id == Instr::OsdOff; }

QStringList unknownKeys(const Entry& e) {
    QStringList out;
    const Instruction ins = identify(e);
    if (!ins.known() || !ins.info->checkKeys) return out;
    for (int i = 0; i < e.params.size(); ++i) {
        const Param& p = e.params.at(i);
        if (i == ins.headParam) continue;
        if (p.bare) { if (!p.value.trimmed().isEmpty()) out << p.value; }   // Testfiles.in's trailing TAB is an empty bare field
        else if (!ins.info->knowsKey(p.key)) out << p.key;
    }
    return out;
}

// ---- writers -----------------------------------------------------------------------------------------------------

Entry makeInstruction(const QString& name, const QString& head, const QList<Param>& params) {
    Entry e;
    e.kind = LineKind::Instruction;
    const QString n = name.simplified();
    const qsizetype sp = n.indexOf(u' ');
    e.instr = sp < 0 ? n : n.left(sp);
    QString arg = sp < 0 ? QString() : n.mid(sp + 1);       // "OSD off": instr "OSD", arg "off", as the parser reads it
    if (!head.isEmpty()) arg = arg.isEmpty() ? head : arg + QLatin1Char(' ') + head;
    e.arg = arg;
    e.pad = arg.isEmpty() ? QString() : QStringLiteral(" ");
    e.params = params;
    e.dirty = true;
    e.raw = renderEntry(e);
    return e;
}

Entry makeInstruction(Instr id, const QString& head, const QList<Param>& params) {
    const InstrInfo* info = instructionInfo(id);
    return info ? makeInstruction(info->name, head, params) : Entry();
}

// ---- events ------------------------------------------------------------------------------------------------------

void resolvePlayModes(Document& d) {
    for (Event& ev : d.events) {
        const Scan s = scanEvent(d, ev);
        const int header = d.entries.at(ev.entry).playMode;
        ev.inBandPlayMode = s.mode;
        ev.modeAt = s.modeAt;
        ev.repeatAt = s.repeatAt;
        // The header PlayMode is the editor's cached copy; the in-band instruction is what the player runs (§2.3).
        ev.effectivePlayMode = s.mode > 0 ? s.mode : header > 0 ? header : 1;
    }
}

int loopStart(const Event& ev) {
    if (ev.repeatAt >= 0) return ev.repeatAt + 1;
    return ev.effectivePlayMode == 2 ? 0 : -1;
}

QList<Diagnostic> instructionDiagnostics(const Document& d, const ParseOptions& o) {
    QList<Diagnostic> out;
    auto add = [&out](int line, int col, const char* code, const QString& message) {
        out.append(Diagnostic{line, col, diagnosticInfo(QString::fromLatin1(code)).severity, QString::fromLatin1(code), message});
    };
    for (const Entry& e : d.entries) {
        if (e.kind != LineKind::Instruction) continue;
        const Instruction ins = identify(e);
        if (!ins.known()) {   // one arg() pass: the name is file text
            add(e.line, 2, "MP011",
                QStringLiteral("unknown instruction \"!%1\": kept verbatim; the importer passes it over").arg(ins.written));
            continue;
        }
        validate(e, ins, o, &out);
        if (!ins.info->checkKeys) continue;
        for (int i = 0; i < e.params.size(); ++i) {
            const Param& p = e.params.at(i);
            if (i == ins.headParam) continue;
            if (p.bare) {
                if (!p.value.trimmed().isEmpty())
                    add(e.line, fieldColumn(e, i), "MP011",
                        QStringLiteral("!%1: field \"%2\" is not key=value: kept, not mapped").arg(ins.name, p.value));
            } else if (!ins.info->knowsKey(p.key)) {
                add(e.line, fieldColumn(e, i), "MP011",
                    QStringLiteral("!%1 has no parameter \"%2\" in the catalog: kept, not mapped").arg(ins.name, p.key));
            }
        }
    }
    // MP014: the in-band mode wins (resolvePlayModes); the header's cached PlayMode is reported where it disagrees. A
    // header without a usable PlayMode (absent, 0 "not defined", or MP011) has nothing to disagree with.
    for (const Event& ev : d.events) {
        const Entry& h = d.entries.at(ev.entry);
        if (h.playMode <= 0) continue;
        const Scan s = scanEvent(d, ev);
        if (s.mode <= 0 || s.mode == h.playMode) continue;
        const Entry& m = d.entries.at(ev.items.at(s.modeAt));
        add(h.line, fieldColumn(h, paramIndex(h.params, QStringLiteral("PlayMode"))), "MP014",
            QStringLiteral("header PlayMode=%1 (%2), but the first play-mode instruction \"!%3\" (line %4) sets %5 (%6): the in-band mode is used")
                .arg(QString::number(h.playMode), playModeName(h.playMode), identify(m).written, QString::number(m.line),
                     QString::number(s.mode), playModeName(s.mode)));
    }
    std::stable_sort(out.begin(), out.end(), [](const Diagnostic& a, const Diagnostic& b) { return a.line != b.line ? a.line < b.line : a.col < b.col; });
    return out;
}

QJsonObject instructionStats(const Document& d) {
    int total = 0, known = 0;
    QJsonObject byName, unknownNames, aliases;
    int bySupport[4] = {}, unknownCount = 0;
    QHash<QString, QString> unknownSpelling;   // case-folded -> first spelling seen
    for (const Entry& e : d.entries) {
        if (e.kind != LineKind::Instruction) continue;
        ++total;
        const Instruction ins = identify(e);
        if (!ins.known()) {
            ++unknownCount;
            const QString key = ins.written.toLower();
            const QString shown = unknownSpelling.value(key, ins.written);
            unknownSpelling.insert(key, shown);
            unknownNames[shown] = unknownNames.value(shown).toInt() + 1;
            continue;
        }
        ++known;
        byName[ins.name] = byName.value(ins.name).toInt() + 1;
        if (ins.alias) aliases[ins.written] = aliases.value(ins.written).toInt() + 1;
        ++bySupport[int(ins.info->support)];
    }
    return {{"instructions", total}, {"known", known}, {"unknown", unknownCount}, {"byName", byName}, {"unknownNames", unknownNames},
            {"aliases", aliases},
            {"bySupport", QJsonObject{{"structural", bySupport[0]}, {"mapped", bySupport[1]}, {"partial", bySupport[2]},
                                      {"unsupported", bySupport[3]}, {"unknown", unknownCount}}}};
}

} // namespace mp
