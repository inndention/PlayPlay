#pragma once
// Audio for transcription (ST-17): any audio or video file -> 16 kHz mono 16-bit PCM WAV, split into pieces of at most
// 5 minutes, the form every OpenRouter speech-to-text model accepts. C++ port of StarMedia's
//   - src/payload.ts wavInfo / splitWav / STT_PIECE_SECONDS / STT_MAX_SINGLE (pure WAV header work),
//   - src/limits.ts sttFor (piece length from a model's researched limits),
//   - src/ads/ingest.ts wav16k and mcp/voice/ffmpeg.ts "wav16k" (-vn -ac 1 -ar 16000 -c:a pcm_s16le), done here with
//     libavcodec + libswresample in process instead of OfflineAudioContext or a spawned ffmpeg.
// A piece of 300 s is 9.6 MB (32,000 bytes a second), under every provider's file limit and OpenRouter's 50 MiB body.
// transcribeLong (the provider lane) sends pieces in order and shifts each piece's segments by its offset.
//
// Decoding follows media/avopen.h (local files only, playlists refused, deadline and cancel). Synchronous and
// thread-safe per call: run it on a worker (the *Async variants), never on the event loop.
#include <QByteArray>
#include <QByteArrayView>
#include <QFuture>
#include <QJsonObject>
#include <QString>
#include <atomic>
#include <functional>
#include <optional>
#include <vector>

class QThreadPool;

namespace studio::media {

constexpr int kSttSampleRate = 16000;
constexpr qint64 kSttBytesPerSecond = 32000;          // 16 kHz mono 16-bit
constexpr double kSttPieceSeconds = 300;              // payload.ts STT_PIECE_SECONDS: 5 minutes, the hard maximum here
constexpr qint64 kSttMaxSingle = 24LL * 1024 * 1024;  // payload.ts STT_MAX_SINGLE: largest non-WAV file sent whole

// ---------------------------------------------------------------- WAV bytes (pure)

struct WavInfo { int sampleRate = 0, channels = 0, bitsPerSample = 0; qint64 dataOffset = 0, dataBytes = 0; };
/// A PCM WAV header (RIFF/WAVE, 'fmt ' then 'data'); dataBytes is clamped to what the buffer holds. nullopt otherwise.
std::optional<WavInfo> wavInfo(QByteArrayView b);
/// The canonical 44-byte header of a PCM WAV.
QByteArray wavHeader(qint64 dataBytes, int sampleRate, int channels, int bitsPerSample);
struct WavPiece { QByteArray bytes; double offset = 0; };
/// Pieces of at most `seconds` (whole sample frames), each a valid WAV, with its start time. Non-WAV bytes and a WAV
/// that already fits come back as one piece at offset 0 (splitWav).
std::vector<WavPiece> splitWav(const QByteArray& b, double seconds);

/// limits.ts sttFor: seconds per piece and the largest single file for a model whose researched limits say
/// maxAudioMinutes / maxAudioMB (0 = not known): max(30, min(300, 90% of the MB limit, 95% of the minutes)).
struct SttLimits { double pieceSeconds = kSttPieceSeconds; qint64 maxSingleBytes = kSttMaxSingle; };
SttLimits sttFor(double maxAudioMinutes = 0, double maxAudioMB = 0);

// ---------------------------------------------------------------- decode + resample (libav)

struct Wav16kOptions {
    double maxSeconds = 0;                     // stop after this much audio (0 = all), like ffmpeg -t
    qint64 maxBytes = 256LL * 1024 * 1024;     // refuse instead of holding more than this in memory (~2.3 h)
    /// Pad the start with silence when the audio stream starts after the container (a video whose sound comes in late),
    /// so transcript times line up with the picture. Capped at 30 s.
    bool alignToStart = true;
    int timeoutMs = 10 * 60 * 1000;
    const std::atomic<bool>* cancel = nullptr;
};
struct Wav16k {
    bool ok = false;
    QString error;
    QByteArray wav;                            // header + PCM
    double seconds = 0;                        // length of the WAV
    double leadSilence = 0;                    // seconds of padding added by alignToStart
    QJsonObject toJson() const;                // without the bytes
};
/// The whole file's main audio stream as one 16 kHz mono s16 WAV in memory (short clips; use splitAudio16k for long ones).
Wav16k toWav16k(const QString& path, const Wav16kOptions& o = Wav16kOptions());
QFuture<Wav16k> toWav16kAsync(const QString& path, const Wav16kOptions& o = Wav16kOptions(), QThreadPool* pool = nullptr);

struct SplitOptions {
    double pieceSeconds = kSttPieceSeconds;    // clamped to [1, 300]: no piece is ever longer than 5 minutes
    double maxSeconds = 0;                     // stop after this much audio (0 = all)
    QString baseName = QStringLiteral("audio");// pieces are <outDir>/<baseName>-000.wav, -001 ...
    bool alignToStart = true;                  // as Wav16kOptions
    int timeoutMs = 30 * 60 * 1000;
    const std::atomic<bool>* cancel = nullptr;
    std::function<void(int index, double offset, double seconds)> onPiece;   // after each piece is on disk (worker thread)
};
struct AudioPiece {
    int index = 0;
    QString path;
    double offset = 0;                         // seconds from the start of the audio (index * pieceSeconds)
    double seconds = 0;
    qint64 bytes = 0;                          // file size, header included
};
struct SplitResult {
    bool ok = false;
    QString error;
    std::vector<AudioPiece> pieces;
    double seconds = 0;                        // total audio written
    double pieceSeconds = 0;                   // the piece length used (after clamping)
    double leadSilence = 0;
    QString sourceCodec;
    int sourceRate = 0, sourceChannels = 0;
    QJsonObject toJson() const;
};
/// Decodes the main audio stream of `path`, resamples it to 16 kHz mono s16 and writes it as pieces of exactly
/// pieceSeconds (the last one shorter) into `outDir` (created if needed), one piece in memory at a time; each file is
/// written atomically. Concatenating the pieces' PCM gives exactly what toWav16k gives. All or nothing: on an error or
/// a cancel the pieces this call wrote are removed.
SplitResult splitAudio16k(const QString& path, const QString& outDir, const SplitOptions& o = SplitOptions());
QFuture<SplitResult> splitAudio16kAsync(const QString& path, const QString& outDir, const SplitOptions& o = SplitOptions(),
                                        QThreadPool* pool = nullptr);

} // namespace studio::media
