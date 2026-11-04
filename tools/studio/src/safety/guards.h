#pragma once
// Spot rules and the provider-layer hook (ST-15).
//
// Spot rules: port of StarMedia mcp/spot/safety.ts, the STRICTEST rule. A named public figure is refused in any style
// (the picture rule allows a clearly stylised caricature), and deepfake / look-alike / office-holder / celebrity-photo /
// "real person" wording is refused too. Uploaded media (the user's own photos) is not checked here: only what goes to an
// AI model is. Every AI shot prompt carries kInventedPeople.
//
// The provider hook: checkOutgoing(path, body, ctx) runs on EVERY OpenRouter request body immediately before it is sent
// (after the model-family prompt rewrite, so it reads the prompt exactly as the model will). The OpenRouter client
// (provider/openrouter, ST-07) calls it in its send path and does not send when it refuses: the request never leaves
// the process, nothing is charged and the refusal is the call's error. What it checks:
//   - pictures and video (POST /images, /images/generations, /images/edits, /videos, and /chat/completions whose
//     "modalities" ask for image or video output, /responses with an image_generation tool): checkVisual(prompt as
//     sent, style, rule) where the prompt is promptText(body) (never negative_prompt) with its own people bans
//     neutralised (neutraliseNegatedPeople). Rule Picture = realPersonCheck; rule Spot = the spot rules AND
//     realPersonCheck (strictly stronger than either alone);
//   - cloned speech (POST /audio/speech whose input_references carry audio): ctx.voice must describe the profile, and
//     checkCloneUse re-validates its stored consent at that moment (consent.h), reading the body's text as well as
//     any text the caller passed. No profile = refused;
//   - everything else (text chat, embeddings, STT, preset-voice speech, video polls) passes.
// Fails closed: the real-people lists unavailable → every picture, video and cloned-speech request is refused.
// A refusal is logged (category "safety": path, rule, reason; never the body), so a blocked request can be found later.
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QStringView>
#include <optional>

#include "safety/consent.h"

namespace studio::safety {

/// `reason` is a predicate without a subject ("names a real public figure …"): callers put their own subject first.
struct SafetyHit {
    QString reason;
    QString match;
};

/// Why this text asks for an AI depiction of a real, identifiable person (spot/safety.ts realPeopleProblem); nullopt
/// when it does not. Also reads the text with look-alike letters folded (stricter than the TS, which folded them only
/// for the name lists).
std::optional<SafetyHit> realPeopleProblem(QStringView text);

/// spot/safety.ts REAL_PEOPLE_POLICY.
QString realPeoplePolicy();
/// The message for a refused request: "Refused: <what> <reason>. <policy> Rewrite …".
QString refusal(const SafetyHit& hit, QStringView what);
/// spot/safety.ts INVENTED_PEOPLE: the clause appended to every AI shot prompt.
QString inventedPeopleClause();
/// `prompt` with inventedPeopleClause() appended once (no-op when it is already there).
QString withInventedPeople(const QString& prompt);

enum class VisualRule {
    Picture,   // generate_image / video tools: realPersonCheck (a clearly stylised caricature of a public figure passes)
    Spot,      // spots, films, any brief-driven shot: spot rules + realPersonCheck (no AI depiction of real people at all)
};

struct Verdict {
    bool refuse = false;
    QString code;            // "real_person" | "voice_consent:<refusal code>" | "safety_data_unavailable"; empty when allowed
    QString reason;          // the message for the caller
    QStringList names;       // what triggered it
};

/// The picture / video rule for one prompt as it will be sent.
Verdict checkVisual(QStringView prompt, QStringView style = {}, VisualRule rule = VisualRule::Spot);

/// What the caller knows about a request that the body does not say.
struct SendContext {
    VisualRule rule = VisualRule::Spot;   // the strict rule unless the caller is a plain picture / video tool
    QString style;                        // the requested look (style preset text), read with the prompt
    std::optional<CloneUse> voice;        // cloned speech: the profile as stored and its use
    qint64 nowMs = -1;                    // -1 = the system clock (consent dates)
};

/// What kind of request a path + body is (for the hook and its tests).
enum class RequestKind { Other, Image, Video, CloneSpeech };
RequestKind classify(QStringView path, const QJsonObject& body);
/// The prompt text a model reads in this body: "prompt", "instructions", the text parts of "messages", and "input"
/// (the speech text, or /responses input items). "negative_prompt" is not included: it lists what NOT to draw.
QString promptText(const QJsonObject& body);
/// `text` with the real/identifiable adjectives dropped where the text itself bans people: inside "Avoid: …" lists and
/// right after a negation ("no identifiable people" → "no people", "do not draw identifiable real people"). The hook
/// reads the composed shot prompt, whose style text carries such bans (styles.json "news-broadcast", "interview");
/// without this every shot in those styles would be refused. Names, look-alike, office-holder and celebrity wording are
/// left as they are.
QString neutraliseNegatedPeople(QStringView text);

/// The provider-layer hook: may this body be sent to `path` (e.g. "/images", "/api/v1/videos")? Logs a refusal.
Verdict checkOutgoing(QStringView path, const QJsonObject& body, const SendContext& ctx = {});

} // namespace studio::safety
