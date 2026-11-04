#ifndef DEKTECASISINK_H
#define DEKTECASISINK_H

#include "sinks.h"
#include "tsmuxer.h"
#include <QThread>
#include <QWaitCondition>
#include <QByteArray>
#include <deque>
#include <memory>

/**
 * DekTecAsiSink — DVB-ASI transport-stream output on a DekTec card (DTA-2154).
 *
 * Packets from the EncoderStage are multiplexed by TsMuxer into a constant-rate
 * TS (null-stuffed at `muxrate`), and the bytes are written into the card's
 * output FIFO on a dedicated thread. The card clocks the ASI at exactly the
 * TS rate, so the mux rate *is* the on-air rate: keep both equal.
 *
 * Options (JSON):
 *   port         physical port, 1-based (default 2: the DTA-2154's factory output)
 *   deviceType   DekTec type number (default 2154) / deviceIndex / serial
 *   muxrate      TS rate in bps (default 19392658, ATSC 8-VSB)
 *   prefillMs    FIFO fill before transmit starts (default 400 ms)
 *   configurePort  set the port to output + ASI if it is not (default true)
 *   + TsMuxer options (serviceId, pids, serviceName, ...)
 *
 * Health: FIFO load %, latched underflow/overflow counts and TX rate are
 * reported in SinkStats::extra and surface in engine telemetry.
 */
class DekTecAsiSink : public PacketSink {
    Q_OBJECT
public:
    DekTecAsiSink(const QString& name, const QJsonObject& options, QObject* parent = nullptr);
    ~DekTecAsiSink() override;

    QString type() const override { return "dektec_asi"; }
    static bool isAvailable();
    static QJsonArray listDevices();     // for the UI: {typeNumber, serial, ports}

    bool start(const AVCodecParameters* video, AVRational videoTimeBase,
               const AVCodecParameters* audio, AVRational audioTimeBase) override;
    void stop() override;
    bool isRunning() const override { return m_running.load(); }
    void writePacket(const AVPacket* pkt, int streamKind) override;

private:
    class Writer : public QThread {
    public:
        explicit Writer(DekTecAsiSink* s) : m_s(s) {}
        void run() override { m_s->writeLoop(); }
    private:
        DekTecAsiSink* m_s;
    };

    QJsonObject m_opts;
    std::unique_ptr<TsMuxer> m_mux;
    QMutex m_muxMutex;

    QMutex m_qmutex;
    QWaitCondition m_qcond;
    std::deque<QByteArray> m_queue;
    qint64 m_queuedBytes = 0;
    static constexpr qint64 kMaxQueuedBytes = 16 * 1024 * 1024;

    Writer* m_writer = nullptr;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_transmitting{false};

    struct Hw;                 // DTAPI objects, only in the .cpp (keeps DTAPI.h out of other units)
    Hw* m_hw = nullptr;

    void enqueue(const uint8_t* data, int size);
    void writeLoop();
    bool openHardware(QString* err);
    void closeHardware();
    void pollStats();
};

#endif // DEKTECASISINK_H
