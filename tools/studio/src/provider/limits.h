#pragma once
// Per-model input limits (ST-07a): a port of starmedia src/limits.ts over the embedded data file model-limits.json
// (ST-25: MODEL_LIMITS, PLATFORM_LIMITS, RETIRED, REQUEST_CAPS). What the live catalog reports (context, output,
// reference-image count, video durations ...) is merged with a researched table of limits the API does not expose
// (size of an input image, prompt length, text per speech request, audio size / length for transcription). Requests
// are shaped to these limits before they are sent.
//
// The TS functions take a ModelInfo (the catalog entry, ST-08); these take the model id, which is all they read from
// it, and an empty id stands for "no model" (the TS undefined). limitsFor() takes the catalog's own values as a JSON
// object until the catalog module exists.
//
// Fails closed where it matters: when the data file cannot be loaded (data::load logs it once) nothing is "researched",
// so every function answers the TS fallback for an unknown model, which is the conservative default for each (2,400
// chars per speech request, 300 s STT pieces, no prompt trimming, the global body caps). videoRules() is code, not data.
#include "provider/payload.h"

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QStringView>
#include <optional>

namespace studio::provider::limits {

/// The researched entry {id, name, provider, limits{...}, sources[], confidence, notes} for a model id, else the entry
/// of the id without ":free" (":free" variants share the paid model's limits); {} when there is none.
QJsonObject researched(QStringView id);
/// PLATFORM_LIMITS: [{what, limit, source}] (the ⚙ → Model limits page).
QJsonObject platformLimits();

/// Everything known about a model's input limits (TS limitsFor): the researched limits, overridden by every non-null,
/// non-empty member of `catalog` (the catalog's live values: contextTokens, maxOutputTokens, maxInputImages, durations,
/// resolutions, aspects, frameImages, generatesAudio, voices, voiceCloning), plus sources[] and confidence ("api" when
/// nothing is researched).
QJsonObject limitsFor(QStringView id, const QJsonObject& catalog = {});

/// How to shrink a reference / first-frame image for this model (never above the safe defaults of payload.h).
payload::ImageLimit imageLimitFor(QStringView id, bool firstFrame);

/// Whole-request caps some providers impose on top of OpenRouter's, in bytes (Sourceful Riverflow: 4.5 MB); nullopt
/// when none applies.
std::optional<qint64> requestCapFor(QStringView id);

/// Why a model OpenRouter still lists must not be used (its provider shut it down); empty when it is fine.
QString retiredReason(QStringView id);

/// Provider rules on duration × resolution the catalog does not express (Veo 3.1: 1080p / 4K need 8 s; Hailuo 2.3:
/// 1080p only for 6 s, 10 s only at 768p).
struct VideoFit {
    int seconds = 0;
    QString resolution;
    bool operator==(const VideoFit& o) const { return seconds == o.seconds && resolution == o.resolution; }
};
VideoFit videoRules(QStringView id, VideoFit fit, const QList<int>& durations, const QStringList& resolutions);

/// Characters of text per speech request (narration is split at sentence ends to fit).
int ttsCharsFor(QStringView id, int fallback = 2400);
/// Longest prompt a model accepts (characters); nullopt when unknown.
std::optional<int> promptCharsFor(QStringView id);
/// A prompt trimmed to the model's limit at a sentence or word boundary (unchanged when it fits or the limit is unknown).
QString fitPrompt(const QString& prompt, QStringView id);

/// Transcription pieces for this model: seconds per piece and the largest single file.
struct SttLimits { int pieceSeconds = payload::kSttPieceSeconds; qint64 maxSingleBytes = payload::kSttMaxSingle; };
SttLimits sttFor(QStringView id);

/// Models that answer response_format=verbose_json with 400 (the researched notes say so).
bool rejectsVerboseJson(QStringView id);

/// The reason the data file did not load (empty when it did).
QString loadError();

} // namespace studio::provider::limits
