#pragma once
// Request-size discipline (ST-07a): a port of starmedia src/payload.ts. OpenRouter refuses any request body over 50 MiB
// (measured 2026-09-28: 413 "Request body of N bytes exceeds the maximum allowed size of 52428800 bytes" on /images,
// /videos, /audio/*, /embeddings), /videos over 31 MiB, and model providers accept far less per inline image or audio
// file. So media is shrunk before it is sent, long audio goes in pieces, and anything still too big is refused before a
// single byte is uploaded (Wire::request answers 413 itself), with a message that says what to do.
//
// The constants are the TS ones. They are also exported into tools/studio/data/model-limits.json (`payload`, ST-25);
// test_studio_openrouter compares the two, so a change on the TS side shows up as a failing test, not as a silent drift.
// Image shrinking (shrinkImage, a canvas in the browser) is not here: the Studio does it with QImage on the worker pool
// (ST-07b / media).
#include <QByteArray>
#include <QByteArrayView>
#include <QList>
#include <QString>
#include <QStringView>
#include <optional>

namespace studio::provider::payload {

constexpr qint64 kMB = 1024 * 1024;
/// OpenRouter's request-body cap.
constexpr qint64 kOpenRouterMaxBody = 50 * kMB;
/// What we allow ourselves (headroom for the JSON around the media).
constexpr qint64 kSafeBody = 48 * kMB;
/// /videos has a smaller cap: 32,505,856 bytes (31 MiB), measured 2026-09-28.
constexpr qint64 kVideosMaxBody = 32505856;
constexpr qint64 kVideosSafeBody = 29 * kMB;

struct BodyCap { qint64 cap = kOpenRouterMaxBody; qint64 safe = kSafeBody; };
/// The request-body cap OpenRouter applies to a path, and the budget we keep under it (TS bodyCap). `path` is an API
/// path ("/videos", "/videos/abc/content") or a full URL (its origin and an /api/v1 prefix are ignored).
BodyCap bodyCap(QStringView path);

/// base64 size of n bytes.
constexpr qint64 b64Size(qint64 n) { return (n + 2) / 3 * 4; }
/// "1.5 MB" below 10 MiB, "12 MB" from there (JS (n / MB).toFixed(n < 10 MB ? 1 : 0), ties rounded up as toFixed does).
QString fmtMB(qint64 n);

struct ImageLimit {
    int maxSide = 1536;          // longest side in pixels
    qint64 maxBytes = 3 * kMB;   // bytes of the encoded image
};
/// references and first frames: large enough for any model to see detail, small enough for every provider
constexpr ImageLimit kReferenceImage{1536, 3 * kMB};
constexpr ImageLimit kFirstFrame{1920, 4 * kMB};

/// audio pieces sent to transcription: 5 minutes of 16 kHz mono WAV ≈ 9.6 MB, under every provider's file limit
constexpr int kSttPieceSeconds = 300;
/// largest non-WAV audio file sent in one piece (most STT providers cap uploads at 25 MB)
constexpr qint64 kSttMaxSingle = 24 * kMB;
/// pieces for uploads to a web host (publisher/store.php)
constexpr qint64 kUploadChunk = 8 * kMB;

/// Keeps the total of a set of inline media under the request budget: drops items from the END (callers order them by
/// importance) until the rest fits (TS fitBudget). Returns how many to keep, from the front.
int fitBudget(const QList<qint64>& sizes, qint64 budget = kSafeBody - 2 * kMB);

// ---------------- audio ----------------

struct WavInfo {
    int sampleRate = 0;
    int channels = 0;
    int bitsPerSample = 0;
    qint64 dataOffset = 0;
    qint64 dataBytes = 0;        // the data chunk, cut to what the buffer holds
};
/// A PCM WAV header (RIFF/WAVE, 'fmt ' then 'data'), as TS wavInfo reads it; nullopt when it is not one.
std::optional<WavInfo> wavInfo(QByteArrayView b);

struct WavPiece {
    QByteArray bytes;            // a complete WAV (44-byte header + data)
    double offset = 0;           // its start in the whole file, seconds
};
/// Splits a PCM WAV into pieces of at most `seconds` (each a valid WAV) with each piece's start time (TS splitWav). A
/// buffer that is not a WAV, or short enough, comes back as one piece at offset 0.
QList<WavPiece> splitWav(const QByteArray& b, double seconds);

} // namespace studio::provider::payload
