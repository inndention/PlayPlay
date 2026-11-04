#ifndef DECKLINKSINK_H
#define DECKLINKSINK_H

#include "sinks.h"
#include <QMutex>
#include <QJsonArray>
#include <deque>
#include <vector>
#include <atomic>

extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/audio_fifo.h>
}

/**
 * DeckLinkSink — SDI/HDMI baseband output on a Blackmagic DeckLink card through the
 * DeckLink SDK (COM) in scheduled-playback mode.
 *
 * The card pulls: every time it finishes showing a frame it calls us back and we schedule
 * the next one from a short queue of program frames, with that frame's share of 48 kHz PCM
 * (the 1601/1602 cadence at 29.97 comes from an exact sample counter). If the engine is
 * late the last picture repeats with silence and an underrun is counted; if it runs ahead
 * the oldest queued frame is dropped. Frames go to the card as 8-bit BGRA (the card does the
 * YUV conversion, and BGRA carries the key for fill+key); cards that refuse BGRA for the mode
 * get 8-bit YUV converted here.
 *
 * Options (JSON):
 *   deviceIndex    0-based DeckLink device (default 0)
 *   deviceName     pick the first device whose display name contains this text (overrides index)
 *   videoMode      "auto" (from raster + rate) or a DeckLink mode name: 1080i59.94, 1080p29.97,
 *                  720p59.94, 1080i50, 1080p25, 2160p29.97 ...
 *   interlaced     with videoMode auto: prefer the interlaced mode for the raster/rate (default
 *                  true for 1080 at 25/29.97/30, else false)
 *   audioChannels  embedded PCM channels; rounded up to 2, 8 or 16 (default 2)
 *   keyer          "off" (default) | "internal" | "external" (fill + key on the second connector)
 *   keyLevel       0-255 (default 255)
 *   prerollFrames  frames scheduled before playback starts (default 3)
 */
class DeckLinkSink : public BasebandSink {
    Q_OBJECT
public:
    DeckLinkSink(const QString& name, const QJsonObject& options, QObject* parent = nullptr);
    ~DeckLinkSink() override;

    QString type() const override { return "decklink"; }
    /// True when the DeckLink driver is installed (the COM iterator can be created).
    static bool isAvailable();
    /// Installed DeckLink devices: [{index, name, model, outputs:[mode names]}]. Empty without a driver.
    static QJsonArray listDevices();

    bool start(const Format& fmt) override;
    void stop() override;
    bool isRunning() const override { return m_running.load(); }
    void pushFrame(const MediaFrame& frame) override;

    /// Called on the DeckLink callback thread.
    void onFrameCompleted(void* completedFrame, int result);
    void onPlaybackStopped();

private:
    struct Hw;
    Hw* m_hw = nullptr;
    QJsonObject m_opts;
    Format m_fmt;
    int m_audioChannels = 2;         // channels sent to the card (2/8/16)
    int m_srcChannels = 2;           // channels in the engine's audio frames
    bool m_bgra = true;              // false: 8-bit YUV via swscale

    QMutex m_qmutex;
    std::deque<FramePtr> m_video;
    FramePtr m_lastVideo;
    AVAudioFifo* m_afifo = nullptr;
    static constexpr int kMaxQueue = 4;

    SwsContext* m_sws = nullptr;
    std::vector<float> m_audioScratch;
    std::vector<float*> m_audioPlanes;
    std::vector<int32_t> m_interleaved;

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopping{false};
    qint64 m_frameIndex = 0;         // frames scheduled so far
    qint64 m_audioSamples = 0;       // samples scheduled so far (48 kHz timeline)
    qint64 m_frameDuration = 1001, m_timeScale = 30000;
    qint64 m_underruns = 0, m_overruns = 0, m_late = 0, m_dropped = 0, m_framesOut = 0;
    qint64 m_lastStatMs = 0;

    bool openHardware(QString* err);
    void closeHardware();
    bool scheduleNext();              // m_qmutex held
    int samplesForFrame(qint64 n) const;
    void publishStats();
};

#endif // DECKLINKSINK_H
