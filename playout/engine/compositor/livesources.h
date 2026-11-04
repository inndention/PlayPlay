#ifndef PLAYPLAY_LIVESOURCES_H
#define PLAYPLAY_LIVESOURCES_H

#include "source.h"
#include <QJsonArray>
#include <QMutex>
#include <QThread>
#include <QWaitCondition>
#include <atomic>
#include <memory>
#include <vector>

extern "C" {
#include <libavutil/audio_fifo.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

/**
 * LiveSourceBase — shared frame-synchroniser behaviour for inputs that arrive on
 * their own clock (NDI, SDI, later DeckLink): newest picture wins, audio goes
 * through a bounded FIFO (¼ s) with drain-on-overflow, and status counters.
 *
 * Threading (PE-03, docs/plans/perf-engine.md): video() / audio() / status() run on the engine's clock thread and
 * must never wait for the receive thread. Each piece of shared state has its own lock, and no lock is ever held
 * across a sleep, an SDK call, resampling, a string build or a frame free:
 *   m_videoMutex     the newest-picture slot: held for a FramePtr copy / swap only
 *   m_audioMutex     the audio FIFO: held for one FIFO read or write only (resampling happens before it)
 *   m_mutex          status text and subclass state (NDI connected name, spec): held for a copy / assignment only
 *   m_producerMutex  resampler + scratch: receive side only, the clock thread never takes it
 * Off switch for A/B runs: PLAYPLAY_LIVESRC_LEGACY_LOCK=1 (read when the source is created) brings back the old
 * shared-lock behaviour, including the NDI search sleep under m_mutex, so the stall can be reproduced on purpose.
 */
class LiveSourceBase : public LayerSource {
    Q_OBJECT
public:
    explicit LiveSourceBase(QObject* parent = nullptr) : LayerSource(parent) {}
    ~LiveSourceBase() override;

    FramePtr video(qint64 frame, double posSec) override;
    bool audio(const FramePtr& dst, qint64 frame, double posSec) override;
    bool hasAudio() const override { return m_wantAudio; }
    QSize nativeSize() const override;
    QJsonObject status() const override;
    /// A picture has arrived since open (any thread).
    bool hasPicture() const { QMutexLocker l(&m_videoMutex); return m_latest != nullptr; }
    /// Throw away buffered sound (a warm monitor receiver handed to program starts with fresh audio). Touches only the
    /// FIFO, never the receive side's resampler, so the caller (clock thread on a take) never waits for a conversion.
    void dropBufferedAudio();
    /// PLAYPLAY_LIVESRC_LEGACY_LOCK=1: this source uses the pre-PE-03 shared lock (A/B measurement only).
    bool legacyLocking() const { return m_legacyLock; }

protected:
    OutputFormat m_fmt;
    bool m_wantAudio = true;
    bool m_fillRaster = true;             // false: place at the input's own size

    // Producer side (receive thread / card callback)
    void publishVideo(const FramePtr& bgra);
    /// Interleaved or planar float input at `rate` Hz with `channels`; resampled to the raster format.
    void publishAudio(const float* const* planes, bool planar, int channels, int rate, int nSamples);
    void setConnected(bool on);
    /// (Re)create the FIFO and drop the resampler. Called by open() before the producer starts.
    void resetAudio();
    void setStatusText(const QString& text);
    /// Drop the held picture (close); the frame is freed outside the slot lock.
    void clearVideo();

    const bool m_legacyLock = qEnvironmentVariableIntValue("PLAYPLAY_LIVESRC_LEGACY_LOCK") == 1;
    mutable QMutex m_mutex;               // status text + subclass state
    QString m_statusText;                 // guarded by m_mutex

    mutable QMutex m_videoMutex;          // guards the four fields below
    FramePtr m_latest;
    QSize m_nativeSize;
    qint64 m_lastVideoMs = 0;
    bool m_servedLatest = false;

    mutable QMutex m_audioMutex;          // guards the FIFO contents (the pointer changes only in resetAudio, under both locks)
    AVAudioFifo* m_afifo = nullptr;
    QMutex m_producerMutex;               // guards the resampler + scratch below (receive side only)
    SwrContext* m_swr = nullptr;
    int m_swrInChannels = 0, m_swrInRate = 0;
    std::vector<float> m_audioScratch;
    std::vector<float*> m_audioPlanes;
    std::vector<const uint8_t*> m_audioInPtrs;

    std::atomic<bool> m_connected{false};
    std::atomic<qint64> m_videoIn{0}, m_videoRepeats{0}, m_videoDropped{0}, m_audioOverflow{0}, m_audioUnderrun{0};
    std::atomic<qint64> m_audioFramesIn{0}, m_audioSamplesIn{0}, m_lastAudioMs{0}, m_audioInvalid{0}, m_audioConvertErrors{0};
    std::atomic<int> m_audioInputRate{0}, m_audioInputChannels{0};
    std::atomic<float> m_audioInputPeak{0};
    std::unique_ptr<FramePool> m_pool;    // native-size BGRA pool for incoming pictures (producer side)
};

/**
 * NdiSource — receives an NDI stream (video as BGRA, audio as float planar).
 * spec: { type:"ndi", name: "MACHINE (Source)" | substring, url: "ip:port",
 *         groups, extraIps, bandwidth: highest|lowest, audio: true,
 *         fillRaster: true, recvName }
 */
class NdiSource : public LiveSourceBase {
    Q_OBJECT
public:
    explicit NdiSource(const QJsonObject& spec, QObject* parent = nullptr);
    ~NdiSource() override;
    QString type() const override { return "ndi"; }
    bool open(const OutputFormat& fmt) override;
    void close() override;
    bool isOpen() const override { return m_open; }
    void update(const QJsonObject& params) override;
    QJsonObject status() const override;
    static bool isAvailable();
    /// Enumerate visible NDI sources (blocking up to `waitMs`).
    static QStringList discover(int waitMs = 1000, const QString& groups = QString(), const QString& extraIps = QString());
    /// Full scan: [{type:"ndi", name, url, host, ip, port}] — url is the NDI url_address (ip:port) when the finder knows it.
    static QJsonArray discoverFull(int waitMs = 1500, const QString& groups = QString(), const QString& extraIps = QString());

private:
    struct Impl; Impl* m_impl = nullptr;
    QJsonObject m_spec;                   // guarded by m_mutex once the receive thread runs (update() may change it)
    bool m_open = false;
    std::atomic<bool> m_stop{false};
    QMutex m_stopMutex;                   // the receive loop's search pause waits on m_stopCv, so close() ends it at once
    QWaitCondition m_stopCv;
    class Worker;
    Worker* m_worker = nullptr;
    void receiveLoop();
    void waitForStop(int ms);
};

/**
 * DekTecSdiInSource — HD/SD-SDI input with de-embedded audio through the DTAPI
 * Matrix API (an input row). The card calls back once per received frame.
 * spec: { type:"sdi_in", port (1-based), deviceType, deviceIndex, serial,
 *         videoStd: "auto"|"1080i59.94"|..., audioChannels: 2, fillRaster: true }
 */
class DekTecSdiInSource : public LiveSourceBase {
    Q_OBJECT
public:
    explicit DekTecSdiInSource(const QJsonObject& spec, QObject* parent = nullptr);
    ~DekTecSdiInSource() override;
    QString type() const override { return "dektec_sdi_in"; }
    bool open(const OutputFormat& fmt) override;
    void close() override;
    bool isOpen() const override { return m_open; }
    QJsonObject status() const override;
    static bool isAvailable();

private:
    struct Hw; Hw* m_hw = nullptr;
    QJsonObject m_spec;
    bool m_open = false;
    int m_vidStd = -1, m_inWidth = 0, m_inHeight = 0, m_audioChannels = 2;
    bool m_interlaced = false;
    SwsContext* m_sws = nullptr;
    std::vector<float> m_pcm;
    std::vector<const float*> m_pcmPlanes;
    std::atomic<qint64> m_noSignal{0}, m_wrongStd{0};
    void onMatrixFrame(void* dtMxData);
    static void matrixCallback(void* data, void* opaque);
};

/// HtmlSource factory (always built): a page in the shared headless Chromium-family browser (browserhost.h).
/// open() fails with a precise error when no browser is installed or no frame arrives in firstFrameTimeoutMs.
std::unique_ptr<LayerSource> createHtmlSource(const QJsonObject& spec, QString* error);

#endif // PLAYPLAY_LIVESOURCES_H
