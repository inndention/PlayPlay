#include "compose/timeline.h"

#include "core/json.h"

#include <QJsonArray>
#include <QRegularExpression>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace studio::compose {

namespace {

template <typename E, size_t N>
QString nameOf(E v, const std::array<const char*, N>& names) { return QString::fromLatin1(names[size_t(v)]); }
template <typename E, size_t N>
std::optional<E> parseName(QStringView s, const std::array<const char*, N>& names) {
    for (size_t i = 0; i < N; ++i)
        if (s == QLatin1StringView(names[i])) return E(i);
    return std::nullopt;
}

constexpr std::array<const char*, 4> kAspects{"9:16", "16:9", "1:1", "4:5"};
constexpr std::array<const char*, 2> kQualities{"draft", "final"};
constexpr std::array<const char*, 7> kTransitions{"cut", "fade", "dissolve", "dip", "slide", "wipe", "zoom"};
constexpr std::array<const char*, 8> kKenBurns{"auto", "in", "out", "left", "right", "up", "down", "none"};
constexpr std::array<const char*, 3> kKinds{"image", "video", "color"};
constexpr std::array<const char*, 2> kFits{"cover", "contain"};
constexpr std::array<const char*, 4> kTextPositions{"", "top", "center", "bottom"};
constexpr std::array<const char*, 5> kLoudness{"social", "broadcast", "off", "r128", "custom"};
constexpr std::array<const char*, 4> kEases{"linear", "easeIn", "easeOut", "easeInOut"};

// ECMAScript WhiteSpace + LineTerminator: what \s matches and String.prototype.trim() removes.
bool jsSpace(char16_t c) {
    return (c >= 0x09 && c <= 0x0D) || c == 0x20 || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A)
        || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000 || c == 0xFEFF;
}
bool jsControl(char16_t c) { return c <= 0x1F || c == 0x7F; }

QString jsTrim(QStringView s) {
    qsizetype b = 0, e = s.size();
    while (b < e && jsSpace(s[b].unicode())) ++b;
    while (e > b && jsSpace(s[e - 1].unicode())) --e;
    return s.mid(b, e - b).toString();
}

// Extended_Pictographic (Unicode emoji-data.txt), the fallback when PCRE2 lacks the property. The test checks it
// against PCRE2 code point by code point.
constexpr std::array<std::pair<char32_t, char32_t>, 78> kExtPict{{
    {0x00A9, 0x00A9}, {0x00AE, 0x00AE}, {0x203C, 0x203C}, {0x2049, 0x2049}, {0x2122, 0x2122}, {0x2139, 0x2139},
    {0x2194, 0x2199}, {0x21A9, 0x21AA}, {0x231A, 0x231B}, {0x2328, 0x2328}, {0x2388, 0x2388}, {0x23CF, 0x23CF},
    {0x23E9, 0x23F3}, {0x23F8, 0x23FA}, {0x24C2, 0x24C2}, {0x25AA, 0x25AB}, {0x25B6, 0x25B6}, {0x25C0, 0x25C0},
    {0x25FB, 0x25FE}, {0x2600, 0x2605}, {0x2607, 0x2612}, {0x2614, 0x2685}, {0x2690, 0x2705}, {0x2708, 0x2712},
    {0x2714, 0x2714}, {0x2716, 0x2716}, {0x271D, 0x271D}, {0x2721, 0x2721}, {0x2728, 0x2728}, {0x2733, 0x2734},
    {0x2744, 0x2744}, {0x2747, 0x2747}, {0x274C, 0x274C}, {0x274E, 0x274E}, {0x2753, 0x2755}, {0x2757, 0x2757},
    {0x2763, 0x2767}, {0x2795, 0x2797}, {0x27A1, 0x27A1}, {0x27B0, 0x27B0}, {0x27BF, 0x27BF}, {0x2934, 0x2935},
    {0x2B05, 0x2B07}, {0x2B1B, 0x2B1C}, {0x2B50, 0x2B50}, {0x2B55, 0x2B55}, {0x3030, 0x3030}, {0x303D, 0x303D},
    {0x3297, 0x3297}, {0x3299, 0x3299}, {0x1F000, 0x1F0FF}, {0x1F10D, 0x1F10F}, {0x1F12F, 0x1F12F},
    {0x1F16C, 0x1F171}, {0x1F17E, 0x1F17F}, {0x1F18E, 0x1F18E}, {0x1F191, 0x1F19A}, {0x1F1AD, 0x1F1E5},
    {0x1F201, 0x1F20F}, {0x1F21A, 0x1F21A}, {0x1F22F, 0x1F22F}, {0x1F232, 0x1F23A}, {0x1F23C, 0x1F23F},
    {0x1F249, 0x1F3FA}, {0x1F400, 0x1F53D}, {0x1F546, 0x1F64F}, {0x1F680, 0x1F6FF}, {0x1F774, 0x1F77F},
    {0x1F7D5, 0x1F7FF}, {0x1F80C, 0x1F80F}, {0x1F848, 0x1F84F}, {0x1F85A, 0x1F85F}, {0x1F888, 0x1F88F},
    {0x1F8AE, 0x1F8FF}, {0x1F90C, 0x1F93A}, {0x1F93C, 0x1F945}, {0x1F947, 0x1FAFF}, {0x1FC00, 0x1FFFD},
}};

const QRegularExpression& extPictRe() {
    static const QRegularExpression re(QStringLiteral("\\p{Extended_Pictographic}"));
    return re;
}

} // namespace

namespace detail {
bool extPictTable(char32_t cp) {
    for (const auto& [lo, hi] : kExtPict) {
        if (cp < lo) return false;
        if (cp <= hi) return true;
    }
    return false;
}
bool extPictRegexAvailable() { return extPictRe().isValid(); }
} // namespace detail

QString aspectName(Aspect a) { return nameOf(a, kAspects); }
QString qualityName(Quality q) { return nameOf(q, kQualities); }
QString transitionName(Transition t) { return nameOf(t, kTransitions); }
QString kenburnsName(KenBurns k) { return nameOf(k, kKenBurns); }
QString clipKindName(ClipKind k) { return nameOf(k, kKinds); }
QString fitName(Fit f) { return nameOf(f, kFits); }
QString textPositionName(TextPosition p) { return nameOf(p, kTextPositions); }
QString loudnessName(Loudness l) { return nameOf(l, kLoudness); }
QString easeName(Ease e) { return nameOf(e, kEases); }
std::optional<Aspect> parseAspect(QStringView s) { return parseName<Aspect>(s, kAspects); }
std::optional<Quality> parseQuality(QStringView s) { return parseName<Quality>(s, kQualities); }
std::optional<Transition> parseTransition(QStringView s) {
    if (s == u"crossfade") return Transition::Fade;               // ads/types.ts Clip.transition.type
    return parseName<Transition>(s, kTransitions);
}
std::optional<KenBurns> parseKenBurns(QStringView s) { return parseName<KenBurns>(s, kKenBurns); }
std::optional<ClipKind> parseClipKind(QStringView s) { return parseName<ClipKind>(s, kKinds); }
std::optional<Fit> parseFit(QStringView s) {
    if (s == u"blur") return Fit::Contain;                         // ads/types.ts Clip.fit
    return parseName<Fit>(s, kFits);
}
std::optional<TextPosition> parseTextPosition(QStringView s) {
    if (s.isEmpty()) return std::nullopt;
    return parseName<TextPosition>(s, kTextPositions);
}
std::optional<Loudness> parseLoudness(QStringView s) {
    if (s == u"web") return Loudness::Social;
    if (s == u"atsc" || s == u"a85") return Loudness::Broadcast;
    if (s == u"ebu") return Loudness::R128;
    const std::optional<Loudness> l = parseName<Loudness>(s, kLoudness);
    return l == Loudness::Custom ? std::nullopt : l;               // a custom target is an object with its numbers
}

LoudnessTarget loudnessPreset(Loudness l) {
    switch (l) {
    case Loudness::Broadcast: return {-24, -2, 7};
    case Loudness::R128: return {-23, -1, 20};
    default: return {-16, -1.5, 11};
    }
}

QString loudnessTargetProblem(const LoudnessTarget& t) {
    const auto in = [](double v, double lo, double hi) { return std::isfinite(v) && v >= lo && v <= hi; };
    if (!in(t.i, -70, -5)) return QStringLiteral("integrated loudness must be -70..-5 LUFS");
    if (!in(t.tp, -9, 0)) return QStringLiteral("true peak must be -9..0 dBTP");
    if (!in(t.lra, 1, 50)) return QStringLiteral("loudness range must be 1..50 LU");
    return {};
}

LoudnessTarget AudioPlan::loudnessTarget() const {
    return loudness == Loudness::Custom && target ? *target : loudnessPreset(loudness);
}
std::optional<Ease> parseEase(QStringView s) { return parseName<Ease>(s, kEases); }

const QVector<Transition>& allTransitions() {
    static const QVector<Transition> v{Transition::Cut, Transition::Fade, Transition::Dissolve, Transition::Dip,
                                       Transition::Slide, Transition::Wipe, Transition::Zoom};
    return v;
}
const QVector<KenBurns>& allKenBurns() {
    static const QVector<KenBurns> v{KenBurns::Auto, KenBurns::In, KenBurns::Out, KenBurns::Left,
                                     KenBurns::Right, KenBurns::Up, KenBurns::Down, KenBurns::None};
    return v;
}
const QVector<Aspect>& allAspects() {
    static const QVector<Aspect> v{Aspect::Vertical, Aspect::Landscape, Aspect::Square, Aspect::Portrait};
    return v;
}

// ---------------------------------------------------------------- numbers

double jsRound(double x) {
    if (!std::isfinite(x)) return x;
    const double f = std::floor(x);
    return (x - f >= 0.5) ? f + 1 : f;                             // x - f is exact for |x| < 2^52
}

QString num(double x) { return json::jsNumber(x); }

// ---------------------------------------------------------------- frame and safe area

FrameSize frameSize(Aspect aspect, Quality quality) {
    const double s = quality == Quality::Draft ? 360 : 1080;
    const auto even = [](double n) { return int(jsRound(n / 2) * 2); };
    switch (aspect) {
    case Aspect::Vertical: return {int(s), even((s * 16) / 9)};
    case Aspect::Landscape: return {even((s * 16) / 9), int(s)};
    case Aspect::Portrait: return {int(s), even((s * 5) / 4)};
    default: return {int(s), int(s)};
    }
}

SafeArea safeArea(Aspect aspect) {
    switch (aspect) {
    case Aspect::Vertical: return {0.08, 0.13, 0.76};
    case Aspect::Portrait: return {0.07, 0.08, 0.86};
    case Aspect::Square: return {0.07, 0.08, 0.88};
    default: return {0.1, 0.1, 0.9};
    }
}

// ---------------------------------------------------------------- text

bool isExtendedPictographic(char32_t cp) {
    if (!detail::extPictRegexAvailable()) return detail::extPictTable(cp);
    const QString s = QString::fromUcs4(&cp, 1);
    return extPictRe().match(s).hasMatch();
}

QString cleanText(QStringView in) {
    // 1) /\p{Extended_Pictographic}|[\u200D\uFE0F]/gu → '' (code point wise)
    QString s;
    s.reserve(in.size());
    const bool useRe = detail::extPictRegexAvailable();
    if (useRe) {
        static const QRegularExpression strip(QStringLiteral("\\p{Extended_Pictographic}|[\\x{200D}\\x{FE0F}]"));
        s = in.toString();
        s.remove(strip);
    } else {
        for (qsizetype i = 0; i < in.size(); ++i) {
            char32_t cp = in[i].unicode();
            qsizetype n = 1;
            if (QChar::isHighSurrogate(cp) && i + 1 < in.size() && in[i + 1].isLowSurrogate()) {
                cp = QChar::surrogateToUcs4(in[i], in[i + 1]);
                n = 2;
            }
            if (cp != 0x200D && cp != 0xFE0F && !detail::extPictTable(cp)) s += in.mid(i, n);
            i += n - 1;
        }
    }
    // 2) runs of [\u0000-\u001f\u007f] → ' ', 3) runs of \s → ' ': together every run of either becomes one space
    QString out;
    out.reserve(s.size());
    bool inRun = false;
    for (const QChar ch : std::as_const(s)) {
        const char16_t c = ch.unicode();
        if (jsControl(c) || jsSpace(c)) {
            if (!inRun) out += QLatin1Char(' ');
            inRun = true;
        } else {
            out += ch;
            inRun = false;
        }
    }
    // 4) trim
    return jsTrim(out);
}

Wrapped wrapText(QStringView text, int maxChars, int maxLines) {
    maxChars = std::max(1, maxChars);                              // ".{1,0}" would not even compile in JS
    maxLines = std::max(1, maxLines);
    QStringList words;
    for (const QString& w : cleanText(text).split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        if (w.size() > maxChars)
            for (qsizetype i = 0; i < w.size(); i += maxChars) words << w.mid(i, maxChars);   // w.match(/.{1,n}/g)
        else
            words << w;
    }
    QStringList lines;
    QString cur;
    for (const QString& w : std::as_const(words)) {
        if (cur.isEmpty()) cur = w;
        else if (cur.size() + 1 + w.size() <= maxChars) cur += QLatin1Char(' ') + w;
        else { lines << cur; cur = w; }
    }
    if (!cur.isEmpty()) lines << cur;
    if (lines.size() <= maxLines) return {lines, false};
    QStringList kept = lines.mid(0, maxLines);
    QString last = kept[maxLines - 1];
    while (last.size() > maxChars - 1) {
        const qsizetype sp = last.lastIndexOf(QLatin1Char(' '));
        last = last.left(sp > 0 ? sp : maxChars - 1);
    }
    // .replace(/[\s,.;:]+$/, '')
    qsizetype e = last.size();
    while (e > 0) {
        const char16_t c = last[e - 1].unicode();
        if (jsSpace(c) || c == u',' || c == u'.' || c == u';' || c == u':') --e;
        else break;
    }
    kept[maxLines - 1] = last.left(e) + QChar(0x2026);
    return {kept, true};
}

Fitted fitText(QStringView text, double width, double size, int maxLines) {
    for (const double f : {1.0, 0.86, 0.74}) {
        const double s = jsRound(size * f);
        const int maxChars = int(std::max(6.0, std::floor(width / (s * 0.56))));
        Wrapped w = wrapText(text, maxChars, maxLines);
        if (!w.truncated || f == 0.74) return {w.lines, int(s), w.truncated};
    }
    return {{}, int(size), false};                                 // not reached (the TS has the same dead line)
}

QString hexColor(QStringView c, const QString& fallback) {
    const QString t = jsTrim(c);
    QStringView h(t);
    if (h.startsWith(u'#')) h = h.mid(1);
    if (h.size() != 6 && h.size() != 3) return fallback;
    for (const QChar ch : h) {
        const char16_t u = ch.unicode();
        if (!((u >= '0' && u <= '9') || (u >= 'a' && u <= 'f') || (u >= 'A' && u <= 'F'))) return fallback;
    }
    QString hex;
    if (h.size() == 3) for (const QChar ch : h) { hex += ch; hex += ch; }
    else hex = h.toString();
    return QStringLiteral("0x") + hex.toUpper();
}

double luminance(const QString& hex) {
    bool ok = false;
    const uint v = QStringView(hex).mid(hex.startsWith(QLatin1String("0x")) ? 2 : 0).toUInt(&ok, 16);
    if (!ok) return std::numeric_limits<double>::quiet_NaN();     // parseInt gave NaN in the TS
    double ch[3];
    const uint parts[3] = {(v >> 16) & 255, (v >> 8) & 255, v & 255};
    for (int i = 0; i < 3; ++i) {
        const double s = parts[i] / 255.0;
        ch[i] = s <= 0.03928 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
    }
    return 0.2126 * ch[0] + 0.7152 * ch[1] + 0.0722 * ch[2];
}

QString textOn(const QString& bg) { return luminance(bg) > 0.45 ? QStringLiteral("0x111111") : QStringLiteral("0xFFFFFF"); }

QVector<CaptionCue> cuesForLine(QStringView text, double start, double seconds, int maxWords) {
    const QString clean = cleanText(text);
    if (clean.isEmpty() || !(seconds > 0)) return {};
    maxWords = std::max(1, maxWords);
    // clean.split(/(?<=[,.;:!?—–])\s+/): after cleanText every gap is one space
    static const QString punct = QStringLiteral(",.;:!?\u2014\u2013");
    QStringList phrases;
    qsizetype from = 0;
    for (qsizetype i = 1; i < clean.size(); ++i)
        if (clean[i] == QLatin1Char(' ') && punct.contains(clean[i - 1])) {
            phrases << clean.mid(from, i - from);
            from = i + 1;
        }
    phrases << clean.mid(from);
    QStringList chunks;
    for (const QString& ph : std::as_const(phrases)) {
        if (ph.isEmpty()) continue;
        const QStringList words = ph.split(QLatin1Char(' '));
        for (qsizetype i = 0; i < words.size(); i += maxWords) {
            const QStringList part = words.mid(i, maxWords);
            // avoid a dangling single word: pull it into the previous chunk
            if (part.size() == 1 && !chunks.isEmpty() && i > 0) chunks.last() += QLatin1Char(' ') + part[0];
            else chunks << part.join(QLatin1Char(' '));
        }
    }
    QVector<double> weights;
    double sum = 0;
    for (const QString& ch : std::as_const(chunks)) { weights << std::max(4.0, double(ch.size())); sum += weights.last(); }
    QVector<CaptionCue> cues;
    double t = start;
    for (qsizetype i = 0; i < chunks.size(); ++i) {
        const double d = (seconds * weights[i]) / sum;
        cues.push_back({r3(t), r3(t + d), chunks[i]});
        t += d;
    }
    return cues;
}

// ---------------------------------------------------------------- the model

double Timeline::naturalSeconds() const {
    double n = 0;
    for (const Clip& c : clips) n += std::max(0.3, c.duration);
    if (endCard) n += std::max(0.5, endCard->duration);
    return n;
}

double Timeline::totalSeconds() const { return r3(targetSeconds ? *targetSeconds : naturalSeconds()); }

namespace {

// compose_video localMedia() resolves to an absolute path and stats it. Without I/O the same promise is: an ABSOLUTE
// LOCAL path, on Windows a drive path (C:\ or C:/), elsewhere a rooted one (/x), because these go to ffmpeg's -i
// unprobed. Refused:
//   - on Windows a rooted path without a drive (\x, /x): it is on the CURRENT drive, so the Studio (probe, staging)
//     and ffmpeg (cwd = the work dir, maybe temp on C:) can read two different files, the reason relative names are
//     refused too;
//   - URLs and other schemes, and ffmpeg protocol syntax that dodges a plain scheme check ("subfile,,start,0,end,9,,:
//     http://..."; ffmpeg special-cases the "subfile," prefix and the inner URL may be any protocol);
//   - "-" (stdin: the join would hang until its timeout) and other relative names (stills are staged from the process
//     cwd, but clips and sound open against the work dir, so a relative name means two different files);
//   - UNC and device paths: \\host\share reaches the network (SMB / NTLM), \\?\ and \\.\ reach devices;
//   - DOS device names in any folder (NUL, CON, COM1 ... open a device on older Windows, whatever the extension).
QString pathProblem(const QString& p) {
    if (p.trimmed().isEmpty()) return QStringLiteral("no file given");
    for (const QChar ch : p)
        if (ch.unicode() < 0x20 || ch.unicode() == 0x7F) return QStringLiteral("control characters in the file name");
    const QString notLocal = QStringLiteral("only local files are accepted (got \"%1\")").arg(p.left(40));
    static const QRegularExpression scheme(QStringLiteral("^[a-zA-Z][a-zA-Z0-9+.,_-]+:"));   // 2+ characters: "C:" is a drive
    const auto sep = [](QChar ch) { return ch == u'/' || ch == u'\\'; };
    if (scheme.match(p).hasMatch() || (p.size() >= 2 && sep(p[0]) && sep(p[1]))) return notLocal;
#ifdef Q_OS_WIN
    if (sep(p[0])) return QStringLiteral("needs an absolute local path with a drive letter (got \"%1\")").arg(p.left(40));
#endif
    static const QRegularExpression absolute(QStringLiteral("^(?:[a-zA-Z]:)?[\\\\/]"));
    if (!absolute.match(p).hasMatch()) return QStringLiteral("needs an absolute local path (got \"%1\")").arg(p.left(40));
    static const QRegularExpression device(QStringLiteral("^(?:con|prn|aux|nul|com[0-9]|lpt[0-9]|conin\\$|conout\\$)$"),
                                           QRegularExpression::CaseInsensitiveOption);
    QString stem = p.mid(std::max(p.lastIndexOf(u'/'), p.lastIndexOf(u'\\')) + 1).section(u'.', 0, 0);
    while (stem.endsWith(u' ')) stem.chop(1);                      // Windows drops trailing spaces: "nul .png" is NUL
    if (device.match(stem).hasMatch()) return notLocal;
    return {};
}

bool finite(double x) { return std::isfinite(x); }

void checkPlacement(QStringList& errs, const QString& at, const AudioPlacement& a) {
    if (const QString p = pathProblem(a.path); !p.isEmpty()) errs << at + QStringLiteral(".path: ") + p;
    if (a.start && !(finite(*a.start) && *a.start >= 0)) errs << at + QStringLiteral(".start: must be >= 0");
    if (!(finite(a.tempo) && a.tempo > 0)) errs << at + QStringLiteral(".tempo: must be > 0");
    if (!(finite(a.maxSeconds) && a.maxSeconds >= 0)) errs << at + QStringLiteral(".maxSeconds: must be >= 0");
    if (!(finite(a.volume) && a.volume >= 0 && a.volume <= 16)) errs << at + QStringLiteral(".volume: must be 0..16");
    if (!(finite(a.from) && a.from >= 0)) errs << at + QStringLiteral(".from: must be >= 0");
}

} // namespace

QStringList Timeline::validate() const {
    QStringList errs;
    if (clips.isEmpty() && !endCard) errs << QStringLiteral("nothing to render: no clips");
    if (clips.size() > kMaxClips) errs << QStringLiteral("too many clips (%1, at most %2)").arg(clips.size()).arg(kMaxClips);
    if (!(finite(fps) && fps >= 1 && fps <= 120)) errs << QStringLiteral("fps: must be 1..120");
    if (!(finite(transitionSeconds) && transitionSeconds > 0)) errs << QStringLiteral("transitionSeconds: must be > 0");
    for (qsizetype i = 0; i < clips.size(); ++i) {
        const Clip& c = clips[i];
        const QString at = QStringLiteral("clips[%1]").arg(i);
        if (!(finite(c.duration) && c.duration > 0)) errs << at + QStringLiteral(".duration: must be > 0");
        if (c.kind != ClipKind::Color) {
            if (const QString p = pathProblem(c.src); !p.isEmpty()) errs << at + QStringLiteral(".src: ") + p;
        }
        if (!c.fallbackImage.isEmpty())
            if (const QString p = pathProblem(c.fallbackImage); !p.isEmpty()) errs << at + QStringLiteral(".fallbackImage: ") + p;
        if (!(finite(c.srcStart) && c.srcStart >= 0)) errs << at + QStringLiteral(".srcStart: must be >= 0");
        if (!(finite(c.speed) && c.speed >= 0.25 && c.speed <= 4)) errs << at + QStringLiteral(".speed: must be 0.25..4");
        if (!(finite(c.audioGain) && c.audioGain >= 0 && c.audioGain <= 16)) errs << at + QStringLiteral(".audioGain: must be 0..16");
        if (!(finite(c.focusX) && finite(c.focusY) && c.focusX >= 0 && c.focusX <= 1 && c.focusY >= 0 && c.focusY <= 1))
            errs << at + QStringLiteral(".focus: x and y must be 0..1");
        if (c.move) {
            for (const ClipPose& p : {c.move->from, c.move->to})
                if (!(finite(p.x) && finite(p.y) && finite(p.z))) { errs << at + QStringLiteral(".move: not a number"); break; }
        }
        if (c.transitionSeconds && !(finite(*c.transitionSeconds) && *c.transitionSeconds > 0))
            errs << at + QStringLiteral(".transitionSeconds: must be > 0");
        if (c.audio) checkPlacement(errs, at + QStringLiteral(".audio"), *c.audio);
    }
    if (audio.voiceover) checkPlacement(errs, QStringLiteral("audio.voiceover"), *audio.voiceover);
    for (qsizetype i = 0; i < audio.events.size(); ++i) checkPlacement(errs, QStringLiteral("audio.events[%1]").arg(i), audio.events[i]);
    if (audio.music) {
        if (const QString p = pathProblem(audio.music->path); !p.isEmpty()) errs << QStringLiteral("audio.music.path: ") + p;
        if (audio.music->volume && !finite(*audio.music->volume)) errs << QStringLiteral("audio.music.volume: not a number");
    }
    if (!(finite(audio.fadeIn) && audio.fadeIn >= 0 && finite(audio.fadeOut) && audio.fadeOut >= 0))
        errs << QStringLiteral("audio.fadeIn / fadeOut: must be >= 0");
    if (audio.loudness == Loudness::Custom) {
        if (!audio.target) errs << QStringLiteral("audio.loudness: a custom target needs its numbers {i, tp, lra}");
        else if (const QString p = loudnessTargetProblem(*audio.target); !p.isEmpty()) errs << QStringLiteral("audio.loudness: ") + p;
    }
    for (qsizetype i = 0; i < captions.size(); ++i) {
        const CaptionCue& c = captions[i];
        if (!(finite(c.start) && finite(c.end) && c.start >= 0)) errs << QStringLiteral("captions[%1]: not a time").arg(i);
        else if (!(c.end > c.start)) errs << QStringLiteral("captions[%1]: end must be after start.").arg(i);
    }
    if (endCard) {
        if (!(finite(endCard->duration) && endCard->duration > 0)) errs << QStringLiteral("endCard.duration: must be > 0");
        if (!endCard->logo.isEmpty())
            if (const QString p = pathProblem(endCard->logo); !p.isEmpty()) errs << QStringLiteral("endCard.logo: ") + p;
        if (endCard->audio) checkPlacement(errs, QStringLiteral("endCard.audio"), *endCard->audio);
    }
    if (targetSeconds && !(finite(*targetSeconds) && *targetSeconds >= 0.2 && *targetSeconds <= kMaxSeconds))
        errs << QStringLiteral("targetSeconds: must be 0.2..%1").arg(kMaxSeconds);
    if (const double n = naturalSeconds(); finite(n) && n > kMaxSeconds)
        errs << QStringLiteral("the clips add up to %1 s, more than %2 s").arg(n).arg(kMaxSeconds);
    if (posterAt && !(finite(*posterAt) && *posterAt >= 0)) errs << QStringLiteral("posterAt: must be >= 0");
    return errs;
}

// ---------------------------------------------------------------- JSON

namespace {

QString sub(const QString& at, const char* key) { return at + QLatin1Char('.') + QLatin1String(key); }

class Reader {
public:
    QString err;
    void fail(const QString& at, const QString& what) { if (err.isEmpty()) err = at + QStringLiteral(": ") + what; }
    static bool absent(const QJsonValue& v) { return v.isUndefined() || v.isNull(); }

    std::optional<double> number(const QJsonObject& o, const char* key, const QString& at) {
        const QJsonValue v = o.value(QLatin1StringView(key));
        if (absent(v)) return std::nullopt;
        if (!v.isDouble() || !std::isfinite(v.toDouble())) { fail(sub(at, key), QStringLiteral("not a number")); return std::nullopt; }
        return v.toDouble();
    }
    std::optional<QString> string(const QJsonObject& o, const char* key, const QString& at) {
        const QJsonValue v = o.value(QLatin1StringView(key));
        if (absent(v)) return std::nullopt;
        if (!v.isString()) { fail(sub(at, key), QStringLiteral("not a string")); return std::nullopt; }
        return v.toString();
    }
    std::optional<bool> boolean(const QJsonObject& o, const char* key, const QString& at) {
        const QJsonValue v = o.value(QLatin1StringView(key));
        if (absent(v)) return std::nullopt;
        if (!v.isBool()) { fail(sub(at, key), QStringLiteral("not true or false")); return std::nullopt; }
        return v.toBool();
    }
    std::optional<QJsonObject> object(const QJsonObject& o, const char* key, const QString& at) {
        const QJsonValue v = o.value(QLatin1StringView(key));
        if (absent(v)) return std::nullopt;
        if (!v.isObject()) { fail(sub(at, key), QStringLiteral("not an object")); return std::nullopt; }
        return v.toObject();
    }
    std::optional<QJsonArray> array(const QJsonObject& o, const char* key, const QString& at) {
        const QJsonValue v = o.value(QLatin1StringView(key));
        if (absent(v)) return std::nullopt;
        if (!v.isArray()) { fail(sub(at, key), QStringLiteral("not an array")); return std::nullopt; }
        return v.toArray();
    }
    template <typename E>
    std::optional<E> name(const QJsonObject& o, const char* key, const QString& at, std::optional<E> (*parse)(QStringView)) {
        const std::optional<QString> s = string(o, key, at);
        if (!s) return std::nullopt;
        const std::optional<E> e = parse(*s);
        if (!e) fail(sub(at, key), QStringLiteral("unknown value \"%1\"").arg(s->left(40)));
        return e;
    }
    template <typename T>
    void set(T& dst, const std::optional<T>& v) { if (v) dst = *v; }

    AudioPlacement placement(const QJsonObject& o, const QString& at) {
        AudioPlacement a;
        if (const auto p = string(o, "path", at)) a.path = *p;
        else fail(at + QStringLiteral(".path"), QStringLiteral("missing"));
        a.start = number(o, "start", at);
        set(a.tempo, number(o, "tempo", at));
        set(a.maxSeconds, number(o, "maxSeconds", at));
        set(a.maxSeconds, number(o, "dur", at));                   // ads AudioPlan events
        set(a.volume, number(o, "volume", at));
        set(a.volume, number(o, "gain", at));                      // ads AudioPlan
        set(a.from, number(o, "from", at));
        return a;
    }
    MusicBed music(const QJsonObject& o, const QString& at) {
        MusicBed m;
        if (const auto p = string(o, "path", at)) m.path = *p;
        else fail(at + QStringLiteral(".path"), QStringLiteral("missing"));
        m.volume = number(o, "volume", at);
        if (const auto g = number(o, "gain", at)) m.volume = g;
        set(m.duck, boolean(o, "duck", at));
        return m;
    }
    ClipPose pose(const QJsonObject& o, const QString& at) {
        ClipPose p;
        set(p.x, number(o, "x", at));
        set(p.y, number(o, "y", at));
        set(p.z, number(o, "z", at));
        return p;
    }
    /// "loudness": a preset name, or a custom target {"i", "tp"?, "lra"?} (missing tp / lra: the R128 ones)
    void loudness(const QJsonObject& o, const QString& at, AudioPlan& a) {
        if (const QJsonValue v = o.value(QLatin1String("loudness")); v.isObject()) {
            const QJsonObject lo = v.toObject();
            const QString at2 = sub(at, "loudness");
            LoudnessTarget lt = loudnessPreset(Loudness::R128);
            if (const auto i = number(lo, "i", at2)) lt.i = *i;
            else fail(at2 + QStringLiteral(".i"), QStringLiteral("missing"));
            set(lt.tp, number(lo, "tp", at2));
            set(lt.lra, number(lo, "lra", at2));
            a.loudness = Loudness::Custom;
            a.target = lt;
            return;
        }
        set(a.loudness, name<Loudness>(o, "loudness", at, parseLoudness));
    }
};

} // namespace

Timeline Timeline::fromJson(const QJsonObject& o, QString* error) {
    Reader r;
    Timeline t;
    const QString top = QStringLiteral("timeline");
    r.set(t.aspect, r.name<Aspect>(o, "aspect", top, parseAspect));
    r.set(t.quality, r.name<Quality>(o, "quality", top, parseQuality));
    r.set(t.fps, r.number(o, "fps", top));
    r.set(t.transition, r.name<Transition>(o, "transition", top, parseTransition));
    r.set(t.transitionSeconds, r.number(o, "transitionSeconds", top));
    t.targetSeconds = r.number(o, "targetSeconds", top);
    t.posterAt = r.number(o, "posterAt", top);

    const char* clipsKey = o.contains(QLatin1String("clips")) ? "clips" : "scenes";
    QVector<std::optional<std::pair<Transition, std::optional<double>>>> entered;   // ads "transition {type, dur}"
    if (const auto arr = r.array(o, clipsKey, top)) {
        for (qsizetype i = 0; i < arr->size() && r.err.isEmpty(); ++i) {
            const QString at = QStringLiteral("%1[%2]").arg(QLatin1StringView(clipsKey)).arg(i);
            if (!arr->at(i).isObject()) { r.fail(at, QStringLiteral("not an object")); break; }
            const QJsonObject s = arr->at(i).toObject();
            Clip c;
            if (const auto id = r.string(s, "id", at)) c.id = *id;
            if (const auto k = r.name<ClipKind>(s, "kind", at, parseClipKind)) c.kind = *k;
            else if (s.contains(QLatin1String("image"))) c.kind = ClipKind::Image;   // compose_video scene shape
            else if (s.contains(QLatin1String("video"))) c.kind = ClipKind::Video;
            // compose.ts requires kind. Without it a clip is a colour card, which has no source, and validate() does not
            // look at a card's src: {"src": "C:/a.png"} alone would render a dark card with no error.
            else if (!Reader::absent(s.value(QLatin1String("src")))) r.fail(sub(at, "kind"), QStringLiteral("missing (a clip with a src is \"image\" or \"video\")"));
            if (const auto v = r.string(s, "src", at)) c.src = *v;
            else if (const auto im = r.string(s, "image", at)) c.src = *im;
            else if (const auto vi = r.string(s, "video", at)) c.src = *vi;
            if (const auto v = r.string(s, "color", at)) c.color = *v;
            if (const auto v = r.string(s, "accent", at)) c.accent = *v;
            r.set(c.srcStart, r.number(s, "srcStart", at));
            r.set(c.speed, r.number(s, "speed", at));
            if (const auto d = r.number(s, "duration", at)) c.duration = *d;
            else if (const auto e = r.number(s, "srcEnd", at)) c.duration = (*e - c.srcStart) / (c.speed != 0 ? c.speed : 1);
            else r.fail(at + QStringLiteral(".duration"), QStringLiteral("missing"));
            if (const auto v = r.string(s, "text", at)) c.text = *v;
            if (const auto p = r.string(s, "textPosition", at); p && !p->isEmpty()) {
                if (const auto tp = parseTextPosition(*p)) c.textPosition = *tp;
                else r.fail(at + QStringLiteral(".textPosition"), QStringLiteral("unknown value \"%1\"").arg(p->left(40)));
            }
            r.set(c.kenburns, r.name<KenBurns>(s, "kenburns", at, parseKenBurns));
            r.set(c.fit, r.name<Fit>(s, "fit", at, parseFit));
            if (const auto f = r.object(s, "focus", at)) {
                r.set(c.focusX, r.number(*f, "x", at + QStringLiteral(".focus")));
                r.set(c.focusY, r.number(*f, "y", at + QStringLiteral(".focus")));
            }
            if (const auto m = r.object(s, "move", at)) {
                ClipMove mv;
                if (const auto f = r.object(*m, "from", at + QStringLiteral(".move"))) mv.from = r.pose(*f, at + QStringLiteral(".move.from"));
                if (const auto f = r.object(*m, "to", at + QStringLiteral(".move"))) mv.to = r.pose(*f, at + QStringLiteral(".move.to"));
                r.set(mv.ease, r.name<Ease>(*m, "ease", at + QStringLiteral(".move"), parseEase));
                c.move = mv;
                // ads: "when set it replaces focus + zoom", so a top-level focus is dropped. The C++ model applies both
                // (focus crop, then the move across it); toJson writes such a focus as move.focus, read back here.
                c.focusX = c.focusY = 0.5;
                if (const auto f = r.object(*m, "focus", at + QStringLiteral(".move"))) {
                    r.set(c.focusX, r.number(*f, "x", at + QStringLiteral(".move.focus")));
                    r.set(c.focusY, r.number(*f, "y", at + QStringLiteral(".move.focus")));
                }
            } else if (const auto z = r.object(s, "zoom", at)) {
                ClipMove mv;                                       // a centred push across the focus-cropped frame
                r.set(mv.from.z, r.number(*z, "from", at + QStringLiteral(".zoom")));
                r.set(mv.to.z, r.number(*z, "to", at + QStringLiteral(".zoom")));
                if (mv.from.z != 1 || mv.to.z != 1) c.move = mv;
            }
            std::optional<std::pair<Transition, std::optional<double>>> in;
            const QJsonValue tv = s.value(QLatin1String("transition"));
            if (tv.isObject()) {
                const QJsonObject to = tv.toObject();
                const QString at2 = at + QStringLiteral(".transition");
                if (const auto ty = r.name<Transition>(to, "type", at2, parseTransition)) in = std::make_pair(*ty, r.number(to, "dur", at2));
                else if (r.err.isEmpty()) r.fail(at2 + QStringLiteral(".type"), QStringLiteral("missing"));
            } else {
                c.transition = r.name<Transition>(s, "transition", at, parseTransition);
            }
            entered.push_back(in);
            c.transitionSeconds = r.number(s, "transitionSeconds", at);
            if (const auto a = r.object(s, "audio", at)) c.audio = r.placement(*a, at + QStringLiteral(".audio"));
            else if (const auto vo = r.string(s, "voiceover", at)) c.audio = AudioPlacement{*vo, 0.0};   // compose_video
            if (const auto v = r.string(s, "fallbackImage", at)) c.fallbackImage = *v;
            r.set(c.audioGain, r.number(s, "audioGain", at));
            t.clips.push_back(c);
        }
    }
    // "how this clip is entered from the previous one" is the previous clip's transition INTO the next
    for (qsizetype i = 1; i < entered.size() && i < t.clips.size(); ++i)
        if (entered[i]) {
            t.clips[i - 1].transition = entered[i]->first;
            if (entered[i]->second) t.clips[i - 1].transitionSeconds = entered[i]->second;
        }

    // ComposeSpec top level ...
    if (const auto v = r.object(o, "voiceover", top)) t.audio.voiceover = r.placement(*v, top + QStringLiteral(".voiceover"));
    if (const auto m = r.object(o, "music", top)) t.audio.music = r.music(*m, top + QStringLiteral(".music"));
    r.loudness(o, top, t.audio);
    // ... and an ads-style AudioPlan
    if (const auto a = r.object(o, "audio", top)) {
        const QString at = QStringLiteral("audio");
        if (const auto v = r.object(*a, "voiceover", at)) t.audio.voiceover = r.placement(*v, at + QStringLiteral(".voiceover"));
        else if (const auto vo = r.object(*a, "vo", at)) t.audio.voiceover = r.placement(*vo, at + QStringLiteral(".vo"));
        if (const auto ev = r.array(*a, "events", at)) {
            for (qsizetype i = 0; i < ev->size(); ++i) {
                const QString e = QStringLiteral("audio.events[%1]").arg(i);
                if (!ev->at(i).isObject()) { r.fail(e, QStringLiteral("not an object")); break; }
                t.audio.events.push_back(r.placement(ev->at(i).toObject(), e));
            }
        }
        if (const auto m = r.object(*a, "music", at)) t.audio.music = r.music(*m, at + QStringLiteral(".music"));
        r.loudness(*a, at, t.audio);
        r.set(t.audio.fadeIn, r.number(*a, "fadeIn", at));
        r.set(t.audio.fadeOut, r.number(*a, "fadeOut", at));
    }
    if (const auto caps = r.array(o, "captions", top)) {
        for (qsizetype i = 0; i < caps->size(); ++i) {
            const QString at = QStringLiteral("captions[%1]").arg(i);
            if (!caps->at(i).isObject()) { r.fail(at, QStringLiteral("not an object")); break; }
            const QJsonObject c = caps->at(i).toObject();
            CaptionCue cue;
            if (const auto v = r.number(c, "start", at)) cue.start = *v; else r.fail(at + QStringLiteral(".start"), QStringLiteral("missing"));
            if (const auto v = r.number(c, "end", at)) cue.end = *v; else r.fail(at + QStringLiteral(".end"), QStringLiteral("missing"));
            if (const auto v = r.string(c, "text", at)) cue.text = *v;
            t.captions.push_back(cue);
        }
    }
    if (const auto e = r.object(o, "endCard", top)) {
        const QString at = QStringLiteral("endCard");
        EndCard ec;
        if (const auto v = r.string(*e, "brand", at)) ec.brand = *v;
        if (const auto v = r.string(*e, "headline", at)) ec.headline = *v;
        if (const auto v = r.string(*e, "cta", at)) ec.cta = *v;
        if (const auto v = r.string(*e, "url", at)) ec.url = *v;
        if (const auto v = r.string(*e, "logo", at)) ec.logo = *v;
        if (const auto cols = r.array(*e, "colors", at)) {
            for (qsizetype i = 0; i < cols->size(); ++i) {
                if (!cols->at(i).isString()) { r.fail(QStringLiteral("endCard.colors[%1]").arg(i), QStringLiteral("not a string")); break; }
                ec.colors << cols->at(i).toString();
            }
        }
        r.set(ec.duration, r.number(*e, "duration", at));
        if (const auto a = r.object(*e, "audio", at)) ec.audio = r.placement(*a, at + QStringLiteral(".audio"));
        t.endCard = ec;
    }
    if (error) *error = r.err;
    return t;
}

QJsonObject toJson(const AudioPlacement& a) {
    QJsonObject o{{"path", a.path}};
    if (a.start) o.insert(QStringLiteral("start"), *a.start);
    if (a.tempo != 1) o.insert(QStringLiteral("tempo"), a.tempo);
    if (a.maxSeconds != 0) o.insert(QStringLiteral("maxSeconds"), a.maxSeconds);
    if (a.volume != 1) o.insert(QStringLiteral("volume"), a.volume);
    if (a.from != 0) o.insert(QStringLiteral("from"), a.from);
    return o;
}

QJsonObject toJson(const Clip& c) {
    QJsonObject o{{"kind", clipKindName(c.kind)}, {"duration", c.duration}};
    if (!c.id.isEmpty()) o.insert(QStringLiteral("id"), c.id);
    if (!c.src.isEmpty()) o.insert(QStringLiteral("src"), c.src);
    if (!c.color.isEmpty()) o.insert(QStringLiteral("color"), c.color);
    if (!c.accent.isEmpty()) o.insert(QStringLiteral("accent"), c.accent);
    if (!c.text.isEmpty()) o.insert(QStringLiteral("text"), c.text);
    if (c.textPosition != TextPosition::Auto) o.insert(QStringLiteral("textPosition"), textPositionName(c.textPosition));
    if (c.kenburns != KenBurns::Auto) o.insert(QStringLiteral("kenburns"), kenburnsName(c.kenburns));
    // a focus beside a move goes INSIDE the move: fromJson drops a top-level focus when a move is set (ads semantics)
    const bool focused = c.focusX != 0.5 || c.focusY != 0.5;
    const QJsonObject focus{{"x", c.focusX}, {"y", c.focusY}};
    if (c.move) {
        const auto pose = [](const ClipPose& p) { return QJsonObject{{"x", p.x}, {"y", p.y}, {"z", p.z}}; };
        QJsonObject mv{{"from", pose(c.move->from)}, {"to", pose(c.move->to)}, {"ease", easeName(c.move->ease)}};
        if (focused) mv.insert(QStringLiteral("focus"), focus);
        o.insert(QStringLiteral("move"), mv);
    } else if (focused) {
        o.insert(QStringLiteral("focus"), focus);
    }
    if (c.fit != Fit::Cover) o.insert(QStringLiteral("fit"), fitName(c.fit));
    if (c.transition) o.insert(QStringLiteral("transition"), transitionName(*c.transition));
    if (c.transitionSeconds) o.insert(QStringLiteral("transitionSeconds"), *c.transitionSeconds);
    if (c.audio) o.insert(QStringLiteral("audio"), toJson(*c.audio));
    if (!c.fallbackImage.isEmpty()) o.insert(QStringLiteral("fallbackImage"), c.fallbackImage);
    if (c.srcStart != 0) o.insert(QStringLiteral("srcStart"), c.srcStart);
    if (c.speed != 1) o.insert(QStringLiteral("speed"), c.speed);
    if (c.audioGain != 0) o.insert(QStringLiteral("audioGain"), c.audioGain);
    return o;
}

QJsonObject toJson(const EndCard& e) {
    QJsonObject o{{"duration", e.duration}};
    if (!e.brand.isEmpty()) o.insert(QStringLiteral("brand"), e.brand);
    if (!e.headline.isEmpty()) o.insert(QStringLiteral("headline"), e.headline);
    if (!e.cta.isEmpty()) o.insert(QStringLiteral("cta"), e.cta);
    if (!e.url.isEmpty()) o.insert(QStringLiteral("url"), e.url);
    if (!e.logo.isEmpty()) o.insert(QStringLiteral("logo"), e.logo);
    if (!e.colors.isEmpty()) o.insert(QStringLiteral("colors"), QJsonArray::fromStringList(e.colors));
    if (e.audio) o.insert(QStringLiteral("audio"), toJson(*e.audio));
    return o;
}

QJsonObject Timeline::toJson() const {
    QJsonObject o{{"aspect", aspectName(aspect)}, {"quality", qualityName(quality)}, {"fps", fps},
                  {"transition", transitionName(transition)}, {"transitionSeconds", transitionSeconds}};
    QJsonArray cs;
    for (const Clip& c : clips) cs.append(compose::toJson(c));
    o.insert(QStringLiteral("clips"), cs);
    QJsonObject a;
    if (audio.loudness == Loudness::Custom && audio.target)
        a.insert(QStringLiteral("loudness"), QJsonObject{{"i", audio.target->i}, {"tp", audio.target->tp}, {"lra", audio.target->lra}});
    else
        a.insert(QStringLiteral("loudness"), loudnessName(audio.loudness == Loudness::Custom ? Loudness::Social : audio.loudness));
    if (audio.voiceover) a.insert(QStringLiteral("voiceover"), compose::toJson(*audio.voiceover));
    if (!audio.events.isEmpty()) {
        QJsonArray ev;
        for (const AudioPlacement& e : audio.events) ev.append(compose::toJson(e));
        a.insert(QStringLiteral("events"), ev);
    }
    if (audio.music) {
        QJsonObject m{{"path", audio.music->path}, {"duck", audio.music->duck}};
        if (audio.music->volume) m.insert(QStringLiteral("volume"), *audio.music->volume);
        a.insert(QStringLiteral("music"), m);
    }
    if (audio.fadeIn != 0) a.insert(QStringLiteral("fadeIn"), audio.fadeIn);
    if (audio.fadeOut != 0) a.insert(QStringLiteral("fadeOut"), audio.fadeOut);
    o.insert(QStringLiteral("audio"), a);
    if (!captions.isEmpty()) {
        QJsonArray cc;
        for (const CaptionCue& c : captions) cc.append(QJsonObject{{"start", c.start}, {"end", c.end}, {"text", c.text}});
        o.insert(QStringLiteral("captions"), cc);
    }
    if (endCard) o.insert(QStringLiteral("endCard"), compose::toJson(*endCard));
    if (targetSeconds) o.insert(QStringLiteral("targetSeconds"), *targetSeconds);
    if (posterAt) o.insert(QStringLiteral("posterAt"), *posterAt);
    return o;
}

} // namespace studio::compose
