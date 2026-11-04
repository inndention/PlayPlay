#pragma once
// Real-people guard (ST-15): ONE public-figure check for every Studio module, plus the picture / video rule built on
// it. Port of StarMedia mcp/media/realpeople.ts; the name lists, look-alike letters and the picture rule's patterns
// come from the embedded tools/studio/data/realpeople.json (ST-25 exported them from that file), the rest is code here.
//
//   publicFiguresIn(text) / findPublicFigure(text)  the public figure(s) a free text names (spot briefs and shots,
//       picture prompts, voice consent statements and transcripts, music prompts)
//   publicFigureInName(name) / looksLikePublicFigure(name)  strict check of a NAME field (voice profile / speaker
//       names): every list, any case, also inside a longer name
//   realPersonCheck(text, style)  the rule for generated pictures and video
//
// FAIL CLOSED: when the embedded data does not load (missing, wrong version, fails its schema) or one of the patterns
// does not compile, every check here answers "this is a real person" (realPersonCheck refuses, the name checks report
// kUnavailable). Nothing runs without the lists; the provider hook (guards.h) and the voice rules (consent.h) inherit
// that. status() says why.
//
// Lists (normalised by normName): FULL names match anywhere as whole words; STAGE names (single names people go by)
// too. AMBIGUOUS names that are also ordinary words or surnames ("Trump", "Drake", "The Rock") match in free text only
// when Capitalised, so "a trump card" and "climbing the rock" pass; a NAME field matches them in any case.
// "King Charles spaniel" is a dog. A heuristic over the prompt text: it cannot see faces in reference images.
//
// Thread-safe: the lists are parsed once into an immutable snapshot that every call shares.
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QStringView>

namespace studio::safety {

/// What the name checks return when the safety lists are not available (fail closed: treated as a match).
inline constexpr QStringView kUnavailable = u"(safety lists unavailable)";

/// True when the lists and patterns loaded; `error` says why not. Every check fails closed while this is false.
bool listsReady(QString* error = nullptr);

/// Text as the guard reads it: NFKC (full-width letters, ligatures, styled maths letters unified), invisible format
/// characters removed (\p{Cf}: zero-width spaces and joiners, soft hyphens, bidi marks; U+034F, U+115F, U+1160,
/// U+3164; variation selectors), and Cyrillic / Greek look-alike letters folded to Latin.
QString foldText(QStringView s);
/// foldText, then accents removed (NFKD minus U+0300-036F), lower case, every run of non-[a-z0-9] one space, trimmed.
QString normName(QStringView s);

/// Every public figure a free text names: full names (list order), then stage names, then Capitalised ambiguous
/// names; no duplicates. `fullNamesOnly` stops after the full names. Lists unavailable: {kUnavailable}.
QStringList publicFiguresIn(QStringView text, bool fullNamesOnly = false);
/// The first of publicFiguresIn(); empty when none.
QString findPublicFigure(QStringView text, bool fullNamesOnly = false);
/// The public figure a NAME field is or contains (every list, any case); empty when none.
QString publicFigureInName(QStringView name);
inline bool looksLikePublicFigure(QStringView name) { return !publicFigureInName(name).isEmpty(); }

struct RealPersonVerdict {
    bool refuse = false;
    QString reason;          // a message the calling model / user can act on (empty when not refused)
    QStringList names;       // the names / phrases that triggered it (strong first, then weak)
    bool stylized = false;   // the requested look is clearly non-photographic
};

/// The picture / video rule: a well-known public figure, a title followed by a name ("Senator Jane Doe"), or explicit
/// real-person wording (celebrity, look-alike, deepfake, "likeness of") is refused unless the look is clearly
/// non-photographic (cartoon, caricature, illustration ...). A plain "Firstname Lastname" (a common given name) is
/// refused only when the prompt also asks for a photographic look. `style` is the requested look, read with the text.
RealPersonVerdict realPersonCheck(QStringView text, QStringView style = {});

namespace testing {
/// Tests only: replace the lists with `data` (an object shaped like realpeople.json; an empty object simulates a data
/// file that did not load) until resetData(). Never called outside tests.
void setData(const QJsonObject& data);
void resetData();
} // namespace testing

} // namespace studio::safety
