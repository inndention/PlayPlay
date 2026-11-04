#ifndef NULLSINK_H
#define NULLSINK_H

#include "sinks.h"

/// Counts packets and discards them. Used by tests and as a dry-run output.
class NullPacketSink : public PacketSink {
    Q_OBJECT
public:
    explicit NullPacketSink(const QString& name = "null", QObject* parent = nullptr) : PacketSink(name, parent) {}
    QString type() const override { return "null"; }
    bool start(const AVCodecParameters*, AVRational, const AVCodecParameters*, AVRational) override {
        m_running = true; QMutexLocker l(&m_statMutex); m_stats.connected = true; return true;
    }
    void stop() override { m_running = false; }
    bool isRunning() const override { return m_running.load(); }
    void writePacket(const AVPacket* pkt, int streamKind) override {
        QMutexLocker l(&m_statMutex);
        m_stats.framesIn++; m_stats.framesOut++; m_stats.bytesOut += pkt->size;
        if (streamKind == 0) ++videoPackets; else ++audioPackets;
    }
    std::atomic<qint64> videoPackets{0};
    std::atomic<qint64> audioPackets{0};
private:
    std::atomic<bool> m_running{false};
};

/// Counts baseband frames and discards them.
class NullBasebandSink : public BasebandSink {
    Q_OBJECT
public:
    explicit NullBasebandSink(const QString& name = "null", QObject* parent = nullptr) : BasebandSink(name, parent) {}
    QString type() const override { return "null"; }
    // connected like NullPacketSink: otherwise the engine's 1 Hz output alarm reports a dry-run sink as "not connected"
    bool start(const Format&) override { m_running = true; QMutexLocker l(&m_statMutex); m_stats.connected = true; return true; }
    void stop() override { m_running = false; QMutexLocker l(&m_statMutex); m_stats.connected = false; }
    bool isRunning() const override { return m_running.load(); }
    void pushFrame(const MediaFrame& f) override {
        QMutexLocker l(&m_statMutex);
        m_stats.framesIn++; m_stats.framesOut++;
        if (f.audio) audioSamples += f.audio->nb_samples;
        lastFrameNumber = f.frameNumber;
    }
    std::atomic<qint64> audioSamples{0};
    std::atomic<qint64> lastFrameNumber{-1};
private:
    std::atomic<bool> m_running{false};
};

#endif // NULLSINK_H
