#ifndef FFMPEGMUXSINK_H
#define FFMPEGMUXSINK_H

#include "sinks.h"
#include <QThread>
#include <QWaitCondition>
#include <deque>

extern "C" {
#include <libavformat/avformat.h>
}

/**
 * FfmpegMuxSink — muxes encoded packets to a URL with libavformat.
 *
 *   udp://239.1.1.1:1234      MPEG-TS (CBR with null padding when muxrate set)
 *   srt://host:port           MPEG-TS (needs an FFmpeg built with libsrt)
 *   rtmp://host/app/key       FLV (H.264 + AAC only)
 *   file path / file://       container by extension (.ts, .mp4, .mkv, .flv)
 *
 * Writing happens on the sink's own thread behind a bounded packet queue, so a
 * slow network can never stall the encoder or the frame clock.
 *
 * Options (JSON): the TsMuxer TS options (muxrate, pcrPeriodMs, patPeriodSec, sdtPeriodSec, serviceId,
 * serviceName, provider, serviceType, pmtPid, videoPid, audioPid, tsId, networkId, tablesVersion,
 * pesPayloadSize) plus pktSize, latencyMs (SRT), rtmpLive.
 */
class FfmpegMuxSink : public PacketSink {
    Q_OBJECT
public:
    FfmpegMuxSink(const QString& name, const QString& url, const QJsonObject& options = {},
                  QObject* parent = nullptr);
    ~FfmpegMuxSink() override;

    QString type() const override { return "ffmpeg"; }
    QString url() const { return m_url; }
    void setDrainOnStop(bool drain) { m_drainOnStop = drain; }

    bool start(const AVCodecParameters* video, AVRational videoTimeBase,
               const AVCodecParameters* audio, AVRational audioTimeBase) override;
    void stop() override;
    bool isRunning() const override { return m_running.load(); }
    void writePacket(const AVPacket* pkt, int streamKind) override;

private:
    class Writer : public QThread {
    public:
        explicit Writer(FfmpegMuxSink* s) : m_s(s) {}
        void run() override { m_s->writeLoop(); }
    private:
        FfmpegMuxSink* m_s;
    };

    QString m_url;
    QJsonObject m_opts;
    AVFormatContext* m_fmt = nullptr;
    int m_videoIdx = -1;
    int m_audioIdx = -1;
    AVRational m_videoTb{1, 30};
    AVRational m_audioTb{1, 48000};
    bool m_headerWritten = false;
    bool m_drainOnStop = false;

    QMutex m_qmutex;
    QWaitCondition m_qcond;
    std::deque<AVPacket*> m_queue;
    static constexpr int kMaxQueue = 600;   // ~5 s of packets at typical rates

    Writer* m_writer = nullptr;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stop{false};

    void writeLoop();
    void closeFormat();
    QString guessFormatName() const;
};

#endif // FFMPEGMUXSINK_H
