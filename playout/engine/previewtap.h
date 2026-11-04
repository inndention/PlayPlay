#ifndef PREVIEWTAP_H
#define PREVIEWTAP_H

#include <QObject>
#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <vector>
#include "mediaframe.h"
#include "compositor/source.h"

struct SwsContext;

/**
 * PreviewEncoder — turns a BGRA frame into a confidence-monitor JPEG.
 *
 * Scaling is area-averaging (swscale SWS_AREA): every source pixel counts, so lower thirds,
 * tickers and timecode stay readable at thumbnail size instead of breaking up the way a
 * nearest-neighbour pick does. The picture keeps its aspect ratio inside `maxW` x `maxH`.
 * Not thread-safe: one encoder per worker thread (the scaler context is cached).
 */
class PreviewEncoder {
public:
    PreviewEncoder() = default;
    ~PreviewEncoder();
    PreviewEncoder(const PreviewEncoder&) = delete;
    PreviewEncoder& operator=(const PreviewEncoder&) = delete;
    /// Empty on failure. `outW` / `outH` receive the encoded size.
    QByteArray encode(const FramePtr& frame, int maxW, int maxH, int quality, int* outW = nullptr, int* outH = nullptr);
    /// Size that fits `srcW` x `srcH` into `maxW` x `maxH` keeping the aspect ratio (even numbers).
    static QSize fit(int srcW, int srcH, int maxW, int maxH);
private:
    SwsContext* m_sws = nullptr;
    int m_srcW = 0, m_srcH = 0, m_srcFmt = -1, m_dstW = 0, m_dstH = 0;
};

/**
 * PreviewTap — JPEG snapshots of program output for confidence monitoring in the UIs
 * (multiview tiles, channel cards, web dashboard).
 *
 * Two profiles: `base` runs all the time (every channel tile), `boost` runs while a UI is
 * watching this channel closely (the On Air page's PGM monitor renews it every few seconds),
 * then the tap falls back to base on its own. The clock thread hands over a reference to the
 * composited frame on the profile's cadence; a worker scales and encodes it. Only the newest
 * pending frame is kept, so a slow consumer never backs up the engine.
 */
class PreviewTap : public QThread {
    Q_OBJECT
public:
    struct Profile {
        int width = 480, height = 270;     // bounding box; the picture keeps its aspect ratio
        double fps = 10.0;                 // pictures per second (capped at the channel rate)
        int quality = 75;                  // JPEG 10..95
        static Profile fromJson(const QJsonObject& o, const Profile& fallback);
        QJsonObject toJson() const;
    };

    explicit PreviewTap(QObject* parent = nullptr);
    ~PreviewTap() override;

    /// The channel's nominal rate, used to turn fps into a frame cadence.
    void setChannelFps(double fps);
    void setBaseProfile(const Profile& p);
    void setBoostProfile(const Profile& p);
    /// Run the boost profile for the next `seconds` (0 = back to base now). Any thread.
    void boost(int seconds);
    /// Legacy: base profile from a size and a frame cadence.
    void configure(int width, int height, int everyNFrames, int jpegQuality = 70);
    void setEnabled(bool on);
    bool isEnabled() const { return m_enabled.load(); }
    bool boosted() const;
    QJsonObject status() const;

    /// Clock thread: non-blocking.
    void submit(const FramePtr& bgra, qint64 frameNumber);
    QByteArray lastJpeg() const { QMutexLocker l(&m_outMutex); return m_lastJpeg; }

signals:
    void frameReady(const QByteArray& jpeg, int width, int height, qint64 frameNumber);

protected:
    void run() override;

private:
    std::atomic<bool> m_enabled{false};
    std::atomic<bool> m_stop{false};
    std::atomic<qint64> m_boostUntilMs{0};
    double m_channelFps = 29.97;

    mutable QMutex m_inMutex;
    QWaitCondition m_inCond;
    Profile m_base, m_boost;
    FramePtr m_pending;
    Profile m_pendingProfile;
    qint64 m_pendingFrame = 0;
    qint64 m_lastSubmitFrame = -1000000;

    mutable QMutex m_outMutex;
    QByteArray m_lastJpeg;
    std::atomic<qint64> m_encoded{0};
    std::atomic<int> m_lastEncodeUs{0};

    int cadence(const Profile& p) const;
};

/**
 * InputMonitor — live pictures of the switcher's inputs (the On Air page's input bus) without
 * putting them on air.
 *
 * The UI leases a set of inputs: [{key:"in1", source:{type:"ndi",name:...}, quality:"low"|"high"}].
 * Each one gets its own receiver, opened off the clock thread:
 *   low   NDI at the lowest bandwidth (the sender's proxy stream, made for multiviewers), no audio,
 *         320x180 at ~5 fps.
 *   high  the preview-bus input: full bandwidth with audio, 480x270 at ~10 fps. It is a WARM source:
 *         a take of the same input hands this receiver straight to the program bus (takeWarm), so the
 *         cut is immediate and never shows a black frame while a fresh receiver connects.
 * Pictures leave as frameReady(key, jpeg) and the Supervisor relays them as "<channel>:<key>".
 * A lease that is not renewed (UI closed or crashed) drops every receiver after `leaseSec`.
 *
 * Allowed: ndi, stream/udp (network pulls), and generator types (bars, color, slate, image) for tests.
 * Not allowed: SDI inputs (a second receiver on a card port would fight the take), line-ins, html, files.
 *
 * Threads: apply / tick / takeWarm on the clock thread; status() and frames from anywhere.
 */
class InputMonitor : public QObject {
    Q_OBJECT
public:
    explicit InputMonitor(QObject* parent = nullptr);
    ~InputMonitor() override;

    void setFormat(const OutputFormat& fmt);
    /// Close sources off the caller's thread (the engine's retire path). Default: a detached thread.
    void setRetireHandler(std::function<void(std::unique_ptr<LayerSource>)> h) { m_retire = std::move(h); }

    /// Clock thread. Replaces the leased set; entries whose source and quality are unchanged keep their receiver.
    void apply(const QJsonArray& inputs, int leaseSec, qint64 frame);
    /// Clock thread, once per frame: adopt opened receivers, pull pictures, hand them to the encoder.
    void tick(qint64 frame);
    /// Clock thread: an open, picture-carrying, full-quality receiver of `spec`, moved out (the entry reopens later).
    std::unique_ptr<LayerSource> takeWarm(const QJsonObject& spec);
    /// [{key, type, quality, state: opening|open|failed|waiting, connected, error, frames, fps}] (any thread).
    QJsonArray status() const;
    int count() const;

    static bool allowedType(const QString& type);
    /// Library specs may carry only {ndiName} / {url} / {port}: fill name and type the way PlayoutEngine::Item::fromJson does.
    static QJsonObject normalize(const QJsonObject& source);
    /// Same physical input, whatever the receive options (bandwidth, audio, receiver name).
    static bool sameSource(const QJsonObject& a, const QJsonObject& b);
    /// The spec actually opened for a quality (NDI bandwidth / audio / receiver name).
    static QJsonObject openSpec(const QJsonObject& source, bool high);

signals:
    void frameReady(const QString& key, const QByteArray& jpeg, int width, int height);

private:
    struct Entry {
        QString key;
        QJsonObject source;
        bool high = false;
        std::unique_ptr<LayerSource> src;   // open receiver (clock thread only)
        qint64 openedFrame = 0;
        quint64 openingGen = 0;             // != 0 while a receiver is being opened
        qint64 retryAtMs = 0;               // failed: next attempt
        QString error;
        qint64 frames = 0;
        qint64 lastFrameMs = 0;
        bool openedHigh = false;            // the quality `src` was opened with
    };
    struct Opened { QString key; quint64 gen; std::unique_ptr<LayerSource> src; QString error; bool high; };
    struct Inbox {
        QMutex mutex;
        std::vector<Opened> done;
        bool alive = true;
    };
    struct Job { FramePtr frame; int w, h, q; };

    OutputFormat m_fmt;
    std::vector<Entry> m_entries;
    std::shared_ptr<Inbox> m_inbox = std::make_shared<Inbox>();
    std::atomic<quint64> m_gen{0};
    qint64 m_leaseUntilMs = 0;
    std::function<void(std::unique_ptr<LayerSource>)> m_retire;

    // encoder worker
    std::thread m_worker;
    QMutex m_jobMutex;
    QWaitCondition m_jobCond;
    QHash<QString, Job> m_jobs;                 // newest per key
    bool m_stop = false;

    mutable QMutex m_statusMutex;
    QJsonArray m_status;

    void startOpen(Entry& e);
    void retire(std::unique_ptr<LayerSource> s);
    void publishStatus();
    void workerLoop();
    Entry* find(const QString& key);
};

#endif // PREVIEWTAP_H
