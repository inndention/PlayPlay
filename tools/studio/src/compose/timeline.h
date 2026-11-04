#pragma once
// Composition model (ST-18a): what the composer renders. One C++ model for the two StarMedia render contracts:
//   - mcp/spot/compose.ts ComposeSpec: scenes (stills with Ken Burns moves, video clips, colour cards) + transitions +
//     burned-in captions in the platform safe area + voice-over and music with ducking + a brand end card, of an EXACT
//     length (the spot pipeline, ST-19);
//   - src/ads/types.ts Timeline / Clip / AudioPlan (L92-135): trimmed and sped-up source clips, a subject focus for the
//     reframe, keyframed camera moves across stills, the clip's own sound, timed audio events, master fades (the film's
//     per-sequence assembly, ST-22).
// A Timeline is a list of Clips (each holds its slot of the timeline; the transition of clip i is INTO clip i+1 and
// borrows from both sides, as in compose.ts), an AudioPlan, caption cues, an optional end card and an optional exact
// length. compose/filtergraph.h turns it into ffmpeg steps; the renderer (ST-18b) runs them.
//
// The text helpers (cleanText, wrapText, fitText, hexColor, cuesForLine) and frameSize / safeArea are ports of
// compose.ts with the same arithmetic in the same order (doubles, Math.round = jsRound), so the filtergraph strings are
// byte-identical to the TS ones; test_studio_compose checks them against compose-golden.json, recorded from the real
// compose.ts (golden/gen-compose-golden.mjs).
//
// Pure value types and functions: no I/O, no clocks; safe on any thread.
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QStringView>
#include <QVector>
#include <optional>

namespace studio::compose {

// ---------------------------------------------------------------- enums (TS string names)

enum class Aspect { Vertical, Landscape, Square, Portrait };          // "9:16" "16:9" "1:1" "4:5"
enum class Quality { Draft, Final };                                  // draft = 360 px short side, final = 1080 px
enum class Transition { Cut, Fade, Dissolve, Dip, Slide, Wipe, Zoom };
enum class KenBurns { Auto, In, Out, Left, Right, Up, Down, None };
enum class ClipKind { Image, Video, Color };
enum class Fit { Cover, Contain };                                    // contain = whole picture on a blurred fill ("blur" in ads/types.ts)
enum class TextPosition { Auto, Top, Center, Bottom };                // Auto: center on colour cards, top elsewhere
/// social -16 LUFS (web; "web" is accepted) / broadcast -24 LUFS (compose.ts; ATSC A/85, "atsc" accepted) / off /
/// r128 -23 LUFS (EBU R128, "ebu" accepted) / custom (AudioPlan::target; JSON "loudness": {"i", "tp", "lra"})
enum class Loudness { Social, Broadcast, Off, R128, Custom };
enum class Ease { Linear, EaseIn, EaseOut, EaseInOut };

QString aspectName(Aspect a);
QString qualityName(Quality q);
QString transitionName(Transition t);
QString kenburnsName(KenBurns k);
QString clipKindName(ClipKind k);
QString fitName(Fit f);
QString textPositionName(TextPosition p);   // "" for Auto
QString loudnessName(Loudness l);
QString easeName(Ease e);
std::optional<Aspect> parseAspect(QStringView s);
std::optional<Quality> parseQuality(QStringView s);
std::optional<Transition> parseTransition(QStringView s);   // also ads/types.ts "crossfade" (= fade)
std::optional<KenBurns> parseKenBurns(QStringView s);
std::optional<ClipKind> parseClipKind(QStringView s);
std::optional<Fit> parseFit(QStringView s);                 // "cover" | "contain" | "blur"
std::optional<TextPosition> parseTextPosition(QStringView s);
std::optional<Loudness> parseLoudness(QStringView s);       // a name or alias; never Custom (that needs the numbers)
std::optional<Ease> parseEase(QStringView s);

const QVector<Transition>& allTransitions();                // compose.ts TRANSITIONS order
const QVector<KenBurns>& allKenBurns();                     // compose.ts KENBURNS order
const QVector<Aspect>& allAspects();                        // compose.ts ASPECTS order

// ---------------------------------------------------------------- numbers (JS semantics)

/// Math.round: half rounds up (towards +inf), exact for every double.
double jsRound(double x);
inline double r2(double x) { return jsRound(x * 100) / 100; }
inline double r3(double x) { return jsRound(x * 1000) / 1000; }
/// String(x) for a number: shortest round-trip digits in ECMAScript layout (core/json.h jsNumber).
QString num(double x);

// ---------------------------------------------------------------- frame and safe area

struct FrameSize { int w = 0, h = 0; };
/// Output frame size: draft = 360 px short side (fast previews and tests), final = 1080 px short side; even sizes.
FrameSize frameSize(Aspect aspect, Quality quality = Quality::Final);

/// Safe areas as fractions of the frame. Vertical feeds (Reels / TikTok / Shorts) cover the bottom ~20 % and the top
/// ~10 % with UI, so captions sit above the bottom UI band; 16:9 follows broadcast title-safe (10 % inset).
struct SafeArea { double marginX = 0, headTop = 0, capBottom = 0; };
SafeArea safeArea(Aspect aspect);

// ---------------------------------------------------------------- text

/// Clean text for drawing: no control characters, no emoji (Extended_Pictographic, ZWJ, VS16; most fonts lack them),
/// single spaces, trimmed. Whitespace is ECMAScript's \s (NBSP, U+2000-200A, U+2028/2029, U+3000, BOM ...; not U+0085).
QString cleanText(QStringView s);
/// Whether code point `cp` is Unicode Extended_Pictographic (PCRE2's property table; a built-in table when this PCRE2
/// has no such property).
bool isExtendedPictographic(char32_t cp);

struct Wrapped { QStringList lines; bool truncated = false; };
/// Greedy word wrap at maxChars UTF-16 units (longer words are cut into pieces); the last line ends in "…" when the
/// text does not fit in maxLines.
Wrapped wrapText(QStringView text, int maxChars, int maxLines);

struct Fitted { QStringList lines; int size = 0; bool truncated = false; };
/// Fit text in a box `width` px wide: shrink the font (100 / 86 / 74 %) before truncating (a glyph ~0.56 em wide).
Fitted fitText(QStringView text, double width, double size, int maxLines);

/// "#rrggbb" / "#rgb" (with or without "#", trimmed) → "0xRRGGBB"; anything else → fallback.
QString hexColor(QStringView c, const QString& fallback);
/// WCAG relative luminance of a "0xRRGGBB" colour.
double luminance(const QString& hex);
/// Text colour that reads on `bg`: "0x111111" on light backgrounds (luminance > 0.45), else "0xFFFFFF".
QString textOn(const QString& bg);

// ---------------------------------------------------------------- the model

struct CaptionCue { double start = 0, end = 0; QString text; };

/// Caption cues for one narration line spoken from `start` for `seconds`: chunks of <= maxWords words (split at
/// punctuation first; a dangling single word joins the chunk before), timed by character count.
QVector<CaptionCue> cuesForLine(QStringView text, double start, double seconds, int maxWords = 6);

/// A camera pose across a still: centre of the crop as fractions of the (cover-fitted) frame, zoom 1 = the whole
/// frame, 1.5 = 50 % tighter (ads/types.ts ClipMove). Zoom is clamped to [1, 10] (zoompan's range), x/y to [0, 1].
struct ClipPose { double x = 0.5, y = 0.5, z = 1; };
struct ClipMove { ClipPose from, to; Ease ease = Ease::Linear; };

/// One piece of sound placed on the timeline.
struct AudioPlacement {
    QString path;                    // local file
    /// Seconds from the start of the clip (clip audio, default 0.15) or of the video (voice-over, default 0; events 0).
    std::optional<double> start;
    double tempo = 1;                // speed-up 1..2 (atempo), re-stamped by sample count
    double maxSeconds = 0;           // cut after this many seconds (after tempo) with a 0.15 s fade; <= 0.2 = no cut
    double volume = 1;
    double from = 0;                 // skip this much of the recording first (ads AudioPlan events "from")
};

struct Clip {
    QString id;                      // free label for the caller (film shot id ...); not rendered
    ClipKind kind = ClipKind::Color;
    QString src;                     // local still or clip (Image / Video)
    QString color;                   // #rrggbb background of colour cards (and of the fallback card)
    QString accent;                  // accent bar at the bottom of a colour card
    double duration = 0;             // seconds this clip holds the timeline (its slot; at least 0.3)
    QString text;                    // on-screen headline drawn on this clip
    TextPosition textPosition = TextPosition::Auto;
    KenBurns kenburns = KenBurns::Auto;
    std::optional<ClipMove> move;    // a keyframed camera move across the focus crop; replaces kenburns (stills)
    Fit fit = Fit::Cover;
    std::optional<Transition> transition;          // INTO the next clip; unset = the timeline's default
    std::optional<double> transitionSeconds;       // overlap of that transition; unset = the timeline's
    std::optional<AudioPlacement> audio;           // clip audio (its narration line), placed from the clip start
    QString fallbackImage;           // used when a video clip turns out unreadable
    // Video clips (ads/types.ts Clip):
    double srcStart = 0;             // in-point in the source (seconds)
    double speed = 1;                // playback speed (0.25..4)
    double audioGain = 0;            // > 0: play the clip's own sound at this gain (speed 1 only)
    double focusX = 0.5, focusY = 0.5;  // subject position in the source (0..1): the cover crop centres on it
};

struct EndCard {
    QString brand, headline, cta, url;
    QString logo;                    // local image
    QStringList colors;              // [background, accent, text?]
    double duration = 3;             // at least 0.5 s
    std::optional<AudioPlacement> audio;   // placed from the card's start (default 0.15 s)
};

struct MusicBed {
    QString path;                    // looped, trimmed to the length, faded in 0.3 s and out min(1.5 s, length / 4)
    std::optional<double> volume;    // 0..1, default 0.22
    bool duck = true;                // sidechain-compress the music under the voice
};

/// What ffmpeg's loudnorm aims at: integrated loudness (LUFS), true-peak ceiling (dBTP), loudness range (LU).
struct LoudnessTarget {
    double i = -16, tp = -1.5, lra = 11;
    bool operator==(const LoudnessTarget& o) const { return i == o.i && tp == o.tp && lra == o.lra; }
};
/// The preset targets: social -16 / -1.5 / 11 and broadcast -24 / -2 / 7 (both compose.ts), r128 -23 / -1 / 20 (R128
/// sets no range; 20 LU keeps a measured second pass linear for ordinary programme material). Off and Custom give
/// the social numbers (Custom's own live in AudioPlan::target).
LoudnessTarget loudnessPreset(Loudness l);
/// loudnorm's accepted ranges (I -70..-5, TP -9..0, LRA 1..50): "" when `t` fits, else what is wrong.
QString loudnessTargetProblem(const LoudnessTarget& t);

struct AudioPlan {
    std::optional<AudioPlacement> voiceover;   // one narration for the whole video
    QVector<AudioPlacement> events;            // timed audio events, start from the start of the video
    std::optional<MusicBed> music;
    Loudness loudness = Loudness::Social;
    std::optional<LoudnessTarget> target;      // Loudness::Custom only
    double fadeIn = 0, fadeOut = 0;            // master fades of the mix (seconds; 0 = none)

    /// The target loudnorm aims at: the preset, or `target` for Custom.
    LoudnessTarget loudnessTarget() const;
};

/// The render contract. Defaults are compose.ts's.
struct Timeline {
    Aspect aspect = Aspect::Vertical;
    Quality quality = Quality::Final;
    double fps = 30;
    QVector<Clip> clips;
    Transition transition = Transition::Cut;   // default transition between clips that do not set one
    double transitionSeconds = 0.5;            // overlap of non-cut transitions, clamped to [0.2, 1.5]
    AudioPlan audio;
    QVector<CaptionCue> captions;
    std::optional<EndCard> endCard;
    std::optional<double> targetSeconds;       // exact output length; unset = clips + end card
    std::optional<double> posterAt;            // poster frame time; unset = 40 % into the first clip, >= 0.5 s

    /// Sum of the slots (each clip at least 0.3 s, the end card at least 0.5 s).
    double naturalSeconds() const;
    /// r3(targetSeconds or naturalSeconds()).
    double totalSeconds() const;
    /// What a caller-supplied timeline must satisfy before it is rendered (empty = fine): something to render, finite
    /// positive durations, ABSOLUTE LOCAL file paths only (on Windows with a drive letter; no URL or ffmpeg protocol
    /// syntax, no "-", no relative, UNC (\\host\...) or device (\\?\, \\.\, NUL, CON ...) paths), a source for every
    /// still and clip, captions that end after they start, at most kMaxClips clips and kMaxSeconds in all, fps 1..120.
    QStringList validate() const;

    QJsonObject toJson() const;
    /// Accepts the ComposeSpec shape ("scenes" or "clips", "voiceover", "music", "endCard" ...) and the ads/types.ts
    /// fields: "srcStart"/"srcEnd"/"speed" (duration = (srcEnd - srcStart) / speed when "duration" is missing),
    /// "focus" {x,y}, "zoom" {from,to} (a centred push across the focus crop), "move" {from,to,ease} (replaces a top-level
    /// "focus", as in ads; toJson writes the focus of a clip with a move as "move" {..., focus {x,y}}, so every Timeline
    /// round-trips), fit "blur", "audioGain", a transition
    /// object {type: crossfade|dip|..., dur} meaning "how this clip is entered from the previous one", and "audio"
    /// {voiceover|vo, events[], music, loudness, fadeIn, fadeOut} with "gain" / "dur" / "from" on placements.
    /// "loudness" is a name (social|web|broadcast|atsc|r128|ebu|off) or a custom target {"i", "tp"?, "lra"?}.
    /// Wrong types and unknown names are errors ("clips[2].kenburns: unknown value \"spin\"").
    static Timeline fromJson(const QJsonObject& o, QString* error = nullptr);

    /// A whole film fits the model; ONE plan's join is capped (filtergraph.h kMaxJoinInputs / kMaxJoinCommandChars), so
    /// the renderer splits a longer timeline into parts and joins the parts.
    static constexpr int kMaxClips = 2000;
    static constexpr double kMaxSeconds = 4 * 3600.0;
};

QJsonObject toJson(const AudioPlacement& a);
QJsonObject toJson(const Clip& c);
QJsonObject toJson(const EndCard& e);

namespace detail {
bool extPictTable(char32_t cp);       // the built-in Extended_Pictographic table
bool extPictRegexAvailable();         // PCRE2 knows \p{Extended_Pictographic} (10.40+)
} // namespace detail

} // namespace studio::compose
