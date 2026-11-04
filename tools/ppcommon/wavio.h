#ifndef WAVIO_H
#define WAVIO_H

#include <QByteArray>
#include <QString>
#include <QtGlobal>

#include <cstddef>
#include <vector>

/**
 * wavio: PCM WAV read/write and the speech levelling of scripts/spot_tts.py, ported so the in-house TTS (SAPI, Kokoro)
 * airs at exactly the level the Python path does:
 *  - trim()       = trim_wav     (L401-423): drop leading/trailing silence under -42 dBFS, keep 80 ms margins.
 *  - normalize()  = normalize_wav (L445-476): RMS of the voiced 50 ms blocks to -19 dBFS (gain capped at +24 dB, skipped
 *                   under 0.5 dB), peaks over -1.5 dBFS handled by the limiter instead of clipping.
 *  - limitPeaks() = limit_peaks  (L426-442): per 2 ms block the gain that keeps the block under the ceiling, applied one
 *                   block early, ~60 ms release, interpolated between block centres. One deliberate difference: the
 *                   interpolated gain is capped at the block's own need, because interpolating towards a quieter
 *                   neighbour's higher gain could lift a peak slightly past the ceiling in the Python version.
 * Samples are kept as interleaved 16-bit PCM (what every PlayPlay WAV is); readFile() also accepts 8/24/32-bit PCM and
 * 32/64-bit float (incl. WAVE_FORMAT_EXTENSIBLE) converted to 16-bit, and writeFile() always writes 16-bit PCM.
 * QtCore only.
 */
namespace wavio {

struct Wav {
    int sampleRate = 0;
    int channels = 0;
    std::vector<qint16> samples;              // interleaved
    int sourceBits = 16;                      // what the file held before conversion
    bool sourceFloat = false;
    qint64 frames() const { return channels > 0 ? qint64(samples.size()) / channels : 0; }
    double durationSec() const { return sampleRate > 0 ? double(frames()) / sampleRate : 0.0; }
    bool isPcm16() const { return sourceBits == 16 && !sourceFloat; }
};

bool parse(const QByteArray& bytes, Wav* out, QString* err = nullptr);
QByteArray serialize(const Wav& w);           // canonical 44-byte header, 16-bit PCM
bool readFile(const QString& path, Wav* out, QString* err = nullptr);
/// Atomic (QSaveFile): a crash never leaves a half-written WAV for the next pipeline step.
bool writeFile(const QString& path, const Wav& w, QString* err = nullptr);
/// Header only (no samples loaded): sample rate, channels and frames of a WAV of any supported format.
bool info(const QString& path, int* sampleRate, int* channels, qint64* frames, QString* err = nullptr);
/// wav_duration(): seconds, 0 when unreadable.
double fileDurationSec(const QString& path, int* sampleRate = nullptr);

/// Clip = np.clip(x, -1, 1) * 32767 (spot_music.py); PeakScale = divide by the peak when it exceeds 1 (write_wav()).
enum class FloatMode { Clip, PeakScale };
Wav fromFloat(const float* x, size_t n, int sampleRate, int channels, FloatMode mode = FloatMode::Clip);
std::vector<float> toFloat(const Wav& w);     // s / 32768

/// Returns true when samples were removed.
bool trim(Wav& w, double thresholdDb = -42.0, int keepMs = 80);
/// Returns the gain applied in dB (0 when nothing changed).
double normalize(Wav& w, double targetDb = -19.0, double ceilingDb = -1.5);
/// `rate` = values per second as laid out in `y` (sampleRate x channels for interleaved audio, as spot_tts.py passes).
void limitPeaks(double* y, size_t n, int rate, double ceiling);
/// The level normalize() measures: mean power of the 50 ms blocks above -50 dBFS, in dB (-inf when none).
double voicedRmsDb(const Wav& w);
double peakDbfs(const Wav& w);                // -inf for silence

/// In place, like the Python functions: non-16-bit files are left alone. trimFile: true when the file was rewritten.
bool trimFile(const QString& path, double thresholdDb = -42.0, int keepMs = 80, QString* err = nullptr);
double normalizeFile(const QString& path, double targetDb = -19.0, double ceilingDb = -1.5, QString* err = nullptr);

}   // namespace wavio

#endif // WAVIO_H
