#ifndef PLAYPLAY_SINKS_H
#define PLAYPLAY_SINKS_H

#include <QObject>
#include <QString>
#include <QJsonObject>
#include <QMutex>
#include <atomic>
#include "../mediaframe.h"
#include "../../core/framerate.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/packet.h>
}

/**
 * Output sink interfaces.
 *
 *   BasebandSink  takes uncompressed program output (BGRA picture + PCM) once
 *                 per frame: NDI, SDI, preview taps.
 *   PacketSink    takes encoded packets from an EncoderStage: muxers for TS/FLV
 *                 (UDP, SRT, RTMP, file) and the DekTec ASI port.
 *
 * Both are driven from the engine's frame-clock thread and must return quickly;
 * anything that may block (network, hardware FIFO) belongs on the sink's own
 * thread behind a bounded queue.
 */

struct SinkStats {
    qint64 framesIn = 0;
    qint64 framesOut = 0;
    qint64 dropped = 0;
    qint64 bytesOut = 0;
    QString lastError;
    bool connected = false;
    QJsonObject extra;       // sink-specific metrics (FIFO load, TX rate, ...)
};

class BasebandSink : public QObject {
    Q_OBJECT
public:
    struct Format {
        int width = 1920;
        int height = 1080;
        FrameRate rate = FrameRate::ntsc();
        int sampleRate = 48000;
        int channels = 2;
    };

    explicit BasebandSink(const QString& name, QObject* parent = nullptr) : QObject(parent), m_name(name) {}
    ~BasebandSink() override = default;

    QString name() const { return m_name; }
    virtual QString type() const = 0;
    virtual bool start(const Format& fmt) = 0;
    virtual void stop() = 0;
    virtual bool isRunning() const = 0;
    /// Called once per output frame. Must not block.
    virtual void pushFrame(const MediaFrame& frame) = 0;
    SinkStats stats() const { QMutexLocker l(&m_statMutex); return m_stats; }

signals:
    void sinkError(const QString& name, const QString& message);

protected:
    void setError(const QString& msg) {
        { QMutexLocker l(&m_statMutex); m_stats.lastError = msg; }
        emit sinkError(m_name, msg);
    }
    QString m_name;
    mutable QMutex m_statMutex;
    SinkStats m_stats;
};

class PacketSink : public QObject {
    Q_OBJECT
public:
    explicit PacketSink(const QString& name, QObject* parent = nullptr) : QObject(parent), m_name(name) {}
    ~PacketSink() override = default;

    QString name() const { return m_name; }
    virtual QString type() const = 0;

    /// Encoder parameters are final when this is called; the sink can write headers.
    virtual bool start(const AVCodecParameters* video, AVRational videoTimeBase,
                       const AVCodecParameters* audio, AVRational audioTimeBase) = 0;
    virtual void stop() = 0;
    virtual bool isRunning() const = 0;
    /// `pkt` is owned by the caller; sink must av_packet_ref() if it keeps it.
    /// streamKind: 0 = video, 1 = audio. Timestamps are in the given time bases.
    virtual void writePacket(const AVPacket* pkt, int streamKind) = 0;
    SinkStats stats() const { QMutexLocker l(&m_statMutex); return m_stats; }

signals:
    void sinkError(const QString& name, const QString& message);

protected:
    void setError(const QString& msg) {
        { QMutexLocker l(&m_statMutex); m_stats.lastError = msg; }
        emit sinkError(m_name, msg);
    }
    QString m_name;
    mutable QMutex m_statMutex;
    SinkStats m_stats;
};

#endif // PLAYPLAY_SINKS_H
