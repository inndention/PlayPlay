#ifndef DEKTECSDISINK_H
#define DEKTECSDISINK_H

#include "sinks.h"
#include <QMutex>
#include <QWaitCondition>
#include <deque>
#include <vector>

extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/audio_fifo.h>
}

namespace Dtapi { class DtMxData; }

/**
 * DekTecSdiSink — HD/SD-SDI baseband output with embedded audio on a DekTec
 * card through the DTAPI Matrix API.
 *
 * The Matrix API calls us once per output frame on its own clocked thread.
 * Program frames from the engine wait in a short queue; the callback takes the
 * next one, converts BGRA to 8-bit UYVY 4:2:2 straight into the card's line
 * buffers and feeds 48 kHz PCM from an audio FIFO (so the card's 1601/1602
 * sample cadence is always honoured). If the engine is late the previous
 * picture repeats with silence and an underrun is counted; if it runs ahead
 * the oldest frame is dropped.
 *
 * Options (JSON):
 *   port        physical port, 1-based (default 2)
 *   deviceType / deviceIndex / serial
 *   videoStd    "auto" (from raster + rate) | 1080p29.97 | 1080i59.94 | 1080p25 |
 *               1080i50 | 720p59.94 | 720p50 | 1080p30 | 1080p24 | 1080p23.98 | 1080i60
 *   audioChannels  embedded PCM channels (default 2; up to 16)
 *   configurePort  set the port to output + SDI if it is not (default true)
 */
class DekTecSdiSink : public BasebandSink {
    Q_OBJECT
public:
    DekTecSdiSink(const QString& name, const QJsonObject& options, QObject* parent = nullptr);
    ~DekTecSdiSink() override;

    QString type() const override { return "dektec_sdi"; }
    static bool isAvailable();

    bool start(const Format& fmt) override;
    void stop() override;
    bool isRunning() const override { return m_running.load(); }
    void pushFrame(const MediaFrame& frame) override;

    /// Map a raster/rate to a DTAPI video standard code (-1 if unsupported).
    static int videoStandardFor(int width, int height, const FrameRate& rate, bool interlaced, QString* name = nullptr);
    static int videoStandardFromName(const QString& name);

private:
    struct Hw;
    Hw* m_hw = nullptr;
    QJsonObject m_opts;
    Format m_fmt;
    int m_vidStd = -1;
    bool m_interlaced = false;
    int m_audioChannels = 2;

    // Engine -> callback hand-off
    QMutex m_qmutex;
    std::deque<FramePtr> m_video;          // pending pictures (newest last)
    FramePtr m_lastVideo;                  // repeated on underrun
    AVAudioFifo* m_afifo = nullptr;        // FLTP, m_audioChannels
    static constexpr int kMaxQueue = 4;

    SwsContext* m_sws = nullptr;
    std::vector<uint8_t> m_uyvy;           // one converted frame, packed, width*2 bytes/line
    std::vector<float> m_audioScratch;
    std::vector<float*> m_audioPlanes;

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_transmitting{false};
    bool m_primed = false;                 // hold black until kPrimeFrames are queued (jitter margin)
    static constexpr int kPrimeFrames = 2;
    qint64 m_framesOut = 0, m_underruns = 0, m_overruns = 0, m_callbackErrors = 0;
    qint64 m_lastStatMs = 0;
    // Phase of the card callback relative to the engine's last push (lock error for the output-locked clock)
    qint64 m_lastPushNs = 0; double m_phaseSumNs = 0.0; int m_phaseCount = 0; double m_phaseFrac = 0.5;
    double m_queueSum = 0.0; double m_queueAfterCb = 1.0;   // queue depth measured in the callback, averaged per second

    bool openHardware(QString* err);
    void closeHardware();
    void onMatrixFrame(Dtapi::DtMxData* data);       // called from the Matrix thread
    static void matrixCallback(Dtapi::DtMxData* data, void* opaque);
    void convertVideo(const FramePtr& bgra);
    void publishStats();
};

#endif // DEKTECSDISINK_H
