#ifndef PLAYPLAY_MEDIAPREVIEW_H
#define PLAYPLAY_MEDIAPREVIEW_H

#include <QObject>
#include <QJsonObject>
#include <QByteArray>
#include <QMutex>
#include <QList>
#include <QSharedPointer>
#include <QThreadPool>
#include <atomic>

/**
 * MediaPreview: frame-accurate stills and storyboard sprites for the segment editor.
 *
 *  - frameJpeg(): the exact frame at a frame number (or the frame covering a time), as a JPEG. Decoders stay
 *    open in a small LRU (one per file), so stepping forward decodes on from the last frame instead of seeking;
 *    a step back or a jump seeks to the keyframe before the target and decodes forward to it. The JPEGs of
 *    recently requested frames are kept, so asking again returns the same bytes at once.
 *  - storyboard(): N evenly spaced keyframe thumbnails tiled into one JPEG sprite (YouTube-style hover
 *    previews and the filmstrip), generated once per file version / size and kept in the cache dir.
 *  - info(): frame rate (rational), frame count, duration, raster, aspect and SMPTE timecode base.
 *
 * Every call here blocks; the Supervisor runs them on pool() / storyboardPool() and replies from there.
 * Thread-safe: one file's decoder is used by one thread at a time (per-decoder lock).
 */
class MediaPreview : public QObject {
    Q_OBJECT
public:
    explicit MediaPreview(const QString& cacheDir, QObject* parent = nullptr);
    ~MediaPreview() override;

    struct Info {
        bool ok = false;
        QString error;
        int fpsNum = 0, fpsDen = 1;
        double durationSec = 0.0;
        qint64 frames = 0;
        int width = 0, height = 0;
        int sarNum = 1, sarDen = 1;
        double startSec = 0.0;          // first presentation time in the file (MPEG-TS often starts > 0)
        bool dropFrame = false;          // 29.97 / 59.94: SMPTE drop-frame timecode
        int tcBase = 0;                  // nominal timecode rate (30 for 29.97, 60 for 59.94)
        QJsonObject toJson() const;
    };
    Info info(const QString& path);

    struct Frame {
        QByteArray jpeg;
        qint64 frame = -1;               // frame number actually returned (0 = first frame)
        double timeSec = 0.0;            // its presentation time, relative to the first frame
        int width = 0, height = 0;       // JPEG size
        QString error;
    };
    /// frameNo >= 0 wins over tSec. width: output width (height follows the display aspect), clamped 64..1920.
    Frame frameJpeg(const QString& path, double tSec, qint64 frameNo, int width);

    /// key: a stable name for this file + n + width (the caller adds the file's size/mtime so a changed file regenerates).
    QJsonObject storyboard(const QString& path, const QString& key, int n, int tileW, QString* error);
    QString cacheDir() const { return m_cacheDir; }

    QThreadPool* pool() { return &m_pool; }                       // frames + info (2 threads)
    QThreadPool* storyboardPool() { return &m_sbPool; }           // sprite generation (1 thread)
    /// Requests waiting or running on pool(); the frame route refuses new ones above a bound (busy, not a stall).
    std::atomic<int> pending{0};

    /// SMPTE timecode for a frame number (drop-frame at 29.97 / 59.94).
    static QString timecode(qint64 frame, int fpsNum, int fpsDen);

private:
    struct Decoder;
    QSharedPointer<Decoder> decoderFor(const QString& path, QString* error);
    QString m_cacheDir;
    QMutex m_cacheMutex;
    QList<QSharedPointer<Decoder>> m_decoders;                    // LRU, most recent first
    QThreadPool m_pool, m_sbPool;
    static constexpr int kMaxDecoders = 4;
};

#endif // PLAYPLAY_MEDIAPREVIEW_H
