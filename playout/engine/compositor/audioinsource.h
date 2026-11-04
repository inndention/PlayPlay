#ifndef PLAYPLAY_AUDIOINSOURCE_H
#define PLAYPLAY_AUDIOINSOURCE_H

#include "livesources.h"
#include <condition_variable>
#include <mutex>
#include <thread>

/**
 * AudioInSource — a sound card's line input / microphone (or, with loopback, what a playback device is playing) as an audio-only layer
 * of the mixer, through WASAPI shared mode. The capture thread hands blocks to LiveSourceBase::publishAudio, which resamples to the
 * channel raster and keeps a bounded FIFO (¼ s; on overflow the oldest audio is dropped, so a drifting device clock cannot add delay).
 * spec: { type:"audio_in", device: id | part of the friendly name | "default", loopback: false, label }
 * Layer role "audio": CLEAR ALL of graphics leaves it alone. Gain / mute are the layer's audioGainDb / audioMute (updateLayer).
 */
class AudioInSource : public LiveSourceBase {
    Q_OBJECT
public:
    explicit AudioInSource(const QJsonObject& spec, QObject* parent = nullptr);
    ~AudioInSource() override;
    QString type() const override { return "audio_in"; }
    bool open(const OutputFormat& fmt) override;
    void close() override;
    bool isOpen() const override { return m_open; }
    QJsonObject status() const override;

private:
    QJsonObject m_spec;
    bool m_open = false;
    std::atomic<bool> m_stop{false};
    std::thread m_thread;
    std::mutex m_initMutex;
    std::condition_variable m_initCv;
    int m_initState = 0;                  // 0 pending, 1 capturing, -1 failed (m_error says why)
    QString m_deviceName;
    std::atomic<int> m_devRate{0}, m_devChannels{0};
    std::atomic<qint64> m_blocks{0};
    void captureLoop();
    void finishInit(bool ok, const QString& err);
};

#endif // PLAYPLAY_AUDIOINSOURCE_H
