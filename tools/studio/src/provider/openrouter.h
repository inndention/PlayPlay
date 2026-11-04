#pragma once
// The OpenRouter client (ST-07a part 1): the model calls of starmedia src/openrouter.ts on top of the Wire (wire.h,
// which owns timeouts, retries, gates, the limit answers and the "never pay twice" rule).
//
// Part 1 (ST-07a): json() (one JSON call, TS json()), chat() with function tools, the web-search server tool,
// json_schema / json_object structured output and the TS JSON salvage, getJson() for GETs.
// Part 2 (ST-07b): generateImages() (the answer is parsed, its base64 decoded and the files written on a worker pool,
// never on the event loop), generateVideo() (submit -> onAccepted, which the caller persists BEFORE the first poll ->
// poll -> content), resumeVideo() (polling a job a restart found accepted), speak(), transcribe() / transcribeLong()
// (16-bit WAV split into pieces, segments joined with their time offsets), embed(), rerank(), generationCost() and
// credits() (TS generateImages ... creditsRemaining).
//
// Results carry the cost OpenRouter reports (usage.cost, dollars) both as the double it sent and as money::Micro
// (fromUsd), so the spend gate books integers. Callbacks follow the Wire rules: exactly one runs, queued on the
// context's thread, none when the context is gone (a context that dies in the middle of a video or a long
// transcription also stops it: no further request is sent for it).
//
// Money: every paid POST (images, videos, audio) goes through the Wire's one-shot path, so it is never sent twice. A
// video job that was accepted and then failed, timed out or was stopped reports `accepted` and its job id
// (VideoError): money may be owed, and the caller reconciles (generationCost / the provider's job) instead of
// submitting again.
#include "core/money.h"
#include "provider/wire.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

class QThreadPool;

namespace studio::provider {

struct ChatRequest {
    /// primary first, then fallbacks (more than one is sent as `models`, OpenRouter's fallback list)
    QStringList models;
    QString system;
    /// a string, or an array of content parts ({type:"text"} / {type:"image_url"} ...)
    QJsonValue user = QString();
    std::optional<int> maxTokens;                        // default 4000
    std::optional<double> temperature;
    /// "minimal" | "low" | "medium" | "high" | "xhigh" | "max" | "none" (none = {enabled:false})
    QString reasoningEffort;
    /// the raw OpenRouter `reasoning` object; wins over reasoningEffort
    std::optional<QJsonObject> reasoning;
    struct JsonSchema { QString name; QJsonObject schema; };
    std::optional<JsonSchema> jsonSchema;
    /// "json_schema" (strict structured output, the default) or "json_object" (the schema is in the prompt)
    QString jsonMode = QStringLiteral("json_schema");
    /// the openrouter:web_search server tool
    struct WebSearch { int maxResults = 5; int maxTotal = 0; int maxUses = 0; QString contextSize; };
    std::optional<WebSearch> webSearch;
    /// OpenRouter provider routing preferences (sort, data_collection, max_price ...), merged over require_parameters
    QJsonObject provider;
    AbortSignal signal;
    /// a whole conversation (tool loops): replaces the system + user pair when set
    std::optional<QJsonArray> messages;
    /// function tools (OpenAI format)
    QJsonArray tools;
    /// "auto" | "none" | "required"; sent only with tools
    QString toolChoice;
    qint64 timeoutMs = 600000;
    int retries = 3;
};

struct ToolCall {
    QString id;
    QString name;
    QString arguments;                                   // a JSON text, as the model wrote it
};

struct Citation {
    QString url;
    QString title;
};

struct ChatResult {
    QString text;
    /// the structured answer (jsonSchema requests only): parsed, or salvaged from prose / a cut-off answer; Undefined
    /// when nothing parses
    QJsonValue json = QJsonValue(QJsonValue::Undefined);
    /// json came from salvageJson (the text was not clean JSON; a repaired cut-off answer is only a prefix)
    bool jsonSalvaged = false;
    QString model;
    std::optional<double> costUsd;
    std::optional<money::Micro> costMicro;
    std::optional<qint64> promptTokens, completionTokens, reasoningTokens;
    QList<Citation> citations;
    QString generationId;
    QList<ToolCall> toolCalls;
    QString reasoning;
    QString finishReason;
    /// the assistant message as returned (append it to `messages` to continue a tool loop)
    QJsonObject message;
    int attempts = 0;
    bool http2 = false;
};

/// "```json ... ```" fences removed, trimmed (TS stripFences).
QString stripFences(QStringView s);
/// Models wrap JSON in prose or get cut off mid-array: the text as is, then the outermost {...} / [...] span, then that
/// span closed after its last complete value (TS salvageJson). Undefined when nothing parses.
QJsonValue salvageJson(QStringView text);

/// The /chat/completions body chat() sends (exactly the TS body). `viaProvider`: a local / pool provider (no routing
/// prefs, plugins or fallback list).
QJsonObject chatBody(const ChatRequest& req, bool viaProvider);
/// Reads a /chat/completions answer (TS chat() after the call).
ChatResult parseChat(const QJsonObject& data, const ChatRequest& req);

// ---------------- images ----------------

struct ImageRequest {
    QString model;
    QString prompt;
    QString aspectRatio;
    QString quality;                                     // auto | low | medium | high
    int n = 1;
    QStringList references;                              // URLs or data: URIs (input_references)
    /// client-side estimate, sent as X-Est-Cost when > 0 (a proxy reserves it against the daily cap)
    std::optional<double> estUsd;
    AbortSignal signal;
    safety::SendContext safety;                          // rule Picture only for the plain generate_image tool
    /// where the decoded images go: <outDir>/<baseName>-<i>.<ext> (replaced when present). Empty = kept in memory
    /// (ImageFile::data). baseName must be ids::isSafe(), else the call is refused before anything is sent.
    QString outDir;
    QString baseName = QStringLiteral("image");
    qint64 timeoutMs = 600000;
};

struct ImageFile {
    QString path;                                        // empty when kept in memory
    QString mediaType;                                   // default image/png (TS)
    qint64 size = 0;
    QByteArray sha256;                                   // hex
    QByteArray data;                                     // only without outDir
};

struct ImageResult {
    QList<ImageFile> images;
    std::optional<double> costUsd;
    std::optional<money::Micro> costMicro;
    /// the provider answered (and billed) but an image could not be decoded or written: that image is missing from
    /// `images` and this says why. The cost is still reported: book it.
    QString writeError;
    int attempts = 0;
    bool http2 = false;
};

/// The /images body (TS generateImages).
QJsonObject imageBody(const ImageRequest& req);
/// "png" for image/png, "jpg" for image/jpeg, webp, gif, avif; "bin" otherwise.
QString imageExtension(QStringView mediaType);

// ---------------- video ----------------

struct VideoRequest {
    QString model;
    QString prompt;
    int duration = 0;
    QString resolution;
    QString aspectRatio;
    std::optional<bool> generateAudio;
    QString firstFrameUrl;
    QString lastFrameUrl;
    QString videoRefUrl;
    std::optional<double> estUsd;                        // X-Est-Cost when set and non-zero
    AbortSignal signal;                                  // cancels the submit, the polling and the download
    safety::SendContext safety;
    /// The provider ACCEPTED the job (POST /videos answered): from here on money may be owed. The caller records
    /// `jobId` (jobs::markAccepted through DbWriter::commitThenSend) and calls `proceed(true)` once it is committed;
    /// the first poll is sent only then. proceed(false) (it could not be recorded) polls nothing: the call fails with
    /// VideoError::jobStatus "unrecorded", accepted and the job id. Without onAccepted the polling starts at once.
    /// `proceed` may be called from any thread, exactly once matters (later calls do nothing).
    std::function<void(const QString& jobId, const QString& pollPath, std::function<void(bool)> proceed)> onAccepted;
    /// the job reported `completed`, BEFORE the download (which can still fail): the billed cost when reported
    std::function<void(std::optional<double> costUsd)> onCompleted;
    std::function<void(const QString& status)> onStatus; // every poll's status
    qint64 maxWaitMs = 20 * 60000;
    qint64 pollPendingMs = 4000;                         // wait before a poll while "pending" (TS)
    qint64 pollMs = 6000;                                // ... in any other in-progress status
    qint64 submitTimeoutMs = 180000;
    qint64 pollTimeoutMs = 180000;
    qint64 contentTimeoutMs = 600000;
    qint64 maxBytes = qint64(1) << 30;                   // largest clip accepted
    /// where the clip goes (written on the worker pool, replaced when present); empty = VideoResult::bytes
    QString outPath;
};

struct VideoResult {
    QByteArray bytes;                                    // only without outPath
    QString path;
    qint64 size = 0;
    QString jobId;                                       // the job id (TS: the poll URL when the answer had no id)
    std::optional<double> costUsd;
    std::optional<money::Micro> costMicro;
    int polls = 0;
    /// the clip was downloaded (and billed) but could not be written to outPath: it is in `bytes` instead
    QString writeError;
};

/// A video call that ended without a clip.
struct VideoError {
    WireError error;                                     // the failing request, or a synthesized one (kind Http / Timeout / Cancelled)
    /// the job's final status ("failed", "cancelled", "expired" ...), "timeout", "unrecorded" (onAccepted said no),
    /// "no_job_id"; empty when the failure was a request (submit, poll or download)
    QString jobStatus;
    QString jobId;
    /// the provider accepted the job: money may be owed. Never submit again; reconcile.
    bool accepted = false;
    std::optional<double> costUsd;
    std::optional<money::Micro> costMicro;
    QString message() const { return error.message; }
};

/// The /videos body (TS generateVideo).
QJsonObject videoBody(const VideoRequest& req);
/// TS videoPollPath: an absolute URL stays; a relative one has the API's own path prefix ("/api/v1/videos/x" from
/// OpenRouter, "/v1/videos/x" from a provider base ending in /v1) removed. Empty for a non-string / blank value.
QString videoPollPath(const QJsonValue& u, const QString& base);
/// statuses that mean "still working"; every other status a poll returns is final
bool videoInProgress(QStringView status);

// ---------------- speech / transcription ----------------

struct SpeechRequest {
    QString model;
    QString input;
    QString voice;                                       // not sent with a reference
    QString format = QStringLiteral("mp3");              // mp3 | pcm
    std::optional<double> speed;
    struct Reference { QString audioDataUrl; QString transcript; };
    std::optional<Reference> reference;                  // a voice sample for cloning-capable models
    std::optional<double> estUsd;
    AbortSignal signal;
    safety::SendContext safety;                          // cloned speech: safety.voice (the consent record)
    qint64 timeoutMs = 300000;
    QString outPath;                                     // written on the worker pool; empty = SpeechResult::bytes
};
struct SpeechResult {
    QByteArray bytes;
    QString path;
    qint64 size = 0;
    QString format;
    QString generationId;                                // x-generation-id (generationCost reads the billed cost)
    int attempts = 0;
    /// the audio could not be written to outPath: it is in `bytes` instead
    QString writeError;
};
/// The /audio/speech body (TS speak()).
QJsonObject speechBody(const SpeechRequest& req);
/// Largest voice sample (encoded data: URI length) a cloning request takes (TS 20 MB).
constexpr qint64 kMaxVoiceSample = qint64(20) * 1024 * 1024;

struct TranscribeOptions {
    AbortSignal signal;
    QString language;
    /// "json" | "verbose_json"; empty = verbose_json unless the model is known to reject it. A 400 on verbose_json is
    /// asked once more with json (a 400 is not billed).
    QString responseFormat;
    bool diarize = false;                                // azure / deepgram diarization options with verbose_json
    /// transcribeLong: seconds per WAV piece and the largest non-WAV file sent whole; 0 = limits::sttFor(model)
    int pieceSeconds = 0;
    qint64 maxSingleBytes = 0;
    std::function<void(int piece, int pieces)> onPiece;  // 1-based, before each piece is sent
    qint64 timeoutMs = 600000;
};
struct TranscriptSegment {
    double start = 0;
    double end = 0;
    QString text;
    QJsonValue speaker = QJsonValue(QJsonValue::Undefined);   // a number or a non-empty string when given; else Undefined
};
struct TranscribeResult {
    QString text;
    QList<TranscriptSegment> segments;
    std::optional<double> duration;
    std::optional<double> costUsd;
    money::Micro costMicro = 0;                          // sum of the pieces' reported costs
    QString responseFormat;                              // the format that answered ("json" has no segments)
    int pieces = 0;                                      // requests answered
};
/// The /audio/transcriptions body (TS transcribe()); `audioB64` is the base64 of the audio.
QJsonObject transcribeBody(const QString& model, const QByteArray& audioB64, const QString& format, const QString& responseFormat,
                           const QString& language, bool diarize, bool viaProvider);
/// Reads a transcription answer (TS transcribe() after the call).
TranscribeResult parseTranscription(const QJsonObject& data, const QString& responseFormat);

// ---------------- embeddings / rerank / misc ----------------

struct EmbedResult {
    QList<QVector<double>> vectors;                      // in input order (sorted by `index`)
    std::optional<double> costUsd;
    std::optional<money::Micro> costMicro;
};
struct RerankHit { int index = 0; double score = 0; };
struct RerankResult {
    QList<RerankHit> results;
    std::optional<double> costUsd;
    std::optional<money::Micro> costMicro;
};
struct Credits {
    std::optional<double> total;
    std::optional<double> used;
};

class OpenRouter : public QObject {
    Q_OBJECT
public:
    explicit OpenRouter(Wire& wire, QObject* parent = nullptr);
    ~OpenRouter() override;
    Wire& wire() { return m_wire; }

    using JsonDone = std::function<void(const QJsonValue& data, const Response& r)>;
    using Failed = Wire::Failed;

    /// One JSON call (TS json()): POST `body` when given, else GET; the body's string "model" is the quota key and the
    /// provider sub-gate key. An answer that is not JSON fails (Kind::Http with the status, "not JSON").
    quint64 json(const QString& path, const std::optional<QJsonObject>& body, QObject* context, JsonDone done, Failed failed,
                 qint64 timeoutMs = 180000, const AbortSignal& signal = {}, const HeaderList& headers = {}, int retries = 3);
    quint64 getJson(const QString& path, QObject* context, JsonDone done, Failed failed) {
        return json(path, std::nullopt, context, std::move(done), std::move(failed));
    }

    using ChatDone = std::function<void(const ChatResult&)>;
    quint64 chat(const ChatRequest& req, QObject* context, ChatDone done, Failed failed);

    // ---- part 2 (ST-07b) ----

    /// The pool that parses media answers, decodes base64 and writes files (default QThreadPool::globalInstance()).
    /// Never the event loop: a 4-image answer is tens of MB of JSON.
    void setWorkerPool(QThreadPool* pool) { m_pool = pool; }
    /// Operations still running (multi-step calls and worker-pool steps).
    int pending() const { return int(m_ops.size()); }

    using ImageDone = std::function<void(const ImageResult&)>;
    quint64 generateImages(const ImageRequest& req, QObject* context, ImageDone done, Failed failed);

    using VideoDone = std::function<void(const VideoResult&)>;
    using VideoFailed = std::function<void(const VideoError&)>;
    /// submit -> onAccepted (persist) -> poll until completed -> onCompleted -> content (TS generateVideo).
    quint64 generateVideo(const VideoRequest& req, QObject* context, VideoDone done, VideoFailed failed);
    /// Polls an accepted job and downloads its clip, without submitting (a restart found it in `polling`). `pollPath`
    /// may be empty (/videos/<jobId>). onAccepted is not called.
    quint64 resumeVideo(const VideoRequest& req, const QString& jobId, const QString& pollPath, QObject* context, VideoDone done,
                        VideoFailed failed);

    using SpeechDone = std::function<void(const SpeechResult&)>;
    quint64 speak(const SpeechRequest& req, QObject* context, SpeechDone done, Failed failed);

    using TranscribeDone = std::function<void(const TranscribeResult&)>;
    /// `partial` holds what the pieces answered before the failure (their cost is owed).
    using TranscribeFailed = std::function<void(const WireError&, const TranscribeResult& partial)>;
    /// One request (TS transcribe()).
    quint64 transcribe(const QString& model, const QByteArray& audio, const QString& format, const TranscribeOptions& opts, QObject* context,
                       TranscribeDone done, TranscribeFailed failed);
    /// Audio of any length (TS transcribeLong): 16-bit WAV in pieces, sent one after another in order, segments shifted
    /// by each piece's offset; other formats whole when small enough, else refused before sending (413).
    quint64 transcribeLong(const QString& model, const QByteArray& audio, const QString& format, const TranscribeOptions& opts,
                           QObject* context, TranscribeDone done, TranscribeFailed failed);

    using EmbedDone = std::function<void(const EmbedResult&)>;
    quint64 embed(const QString& model, const QStringList& input, QObject* context, EmbedDone done, Failed failed, const AbortSignal& signal = {});
    using RerankDone = std::function<void(const RerankResult&)>;
    quint64 rerank(const QString& model, const QString& query, const QStringList& documents, int topN, QObject* context, RerankDone done,
                   Failed failed, const AbortSignal& signal = {});

    /// Best-effort billed cost of a generation id (TTS returns raw bytes): nullopt on any failure (TS generationCost).
    using CostDone = std::function<void(std::optional<double> usd, std::optional<money::Micro> micro)>;
    quint64 generationCost(const QString& id, QObject* context, CostDone done);
    /// Best effort: nullopt on any failure (TS creditsRemaining).
    using CreditsDone = std::function<void(std::optional<Credits>)>;
    quint64 credits(QObject* context, CreditsDone done);

    struct Op;                                           // one multi-step call (openrouter.cpp)

private:
    struct Link {                                        // how an abort listener (any thread) finds this object
        std::mutex m;
        OpenRouter* self = nullptr;
    };
    std::shared_ptr<Op> newOp(QObject* context);
    void endOp(const std::shared_ptr<Op>& op);
    QThreadPool* pool() const;
    void watchAbort(const std::shared_ptr<Op>& op);
    void aborted(quint64 id);
    void videoSubmitted(const std::shared_ptr<Op>& op, const Response& r);
    void accepted(quint64 id, bool ok, const QJsonObject& job);
    void videoPoll(const std::shared_ptr<Op>& op, const QJsonObject& status);
    void videoFetch(const std::shared_ptr<Op>& op, qint64 delayMs);
    void videoDownload(const std::shared_ptr<Op>& op, const QJsonObject& status);
    void videoFail(const std::shared_ptr<Op>& op, VideoError e);
    void sttPiece(const std::shared_ptr<Op>& op);
    void sttFail(const std::shared_ptr<Op>& op, const WireError& e);
    void sttSend(const std::shared_ptr<Op>& op, const QByteArray& audio, const QString& format, const QString& want,
                 std::function<void(const TranscribeResult&)> then);

    Wire& m_wire;
    QThreadPool* m_pool = nullptr;
    std::shared_ptr<Link> m_link;
    QHash<quint64, std::shared_ptr<Op>> m_ops;
    quint64 m_nextOp = 0;
};

} // namespace studio::provider
