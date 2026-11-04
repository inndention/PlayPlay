#include "dektecasisink.h"
#include <QDebug>
#include <QElapsedTimer>
#include <QJsonArray>

#ifdef HAS_DEKTEC
#include <DTAPI.h>
#endif

// ---------------------------------------------------------------------------
// Hardware wrapper
// ---------------------------------------------------------------------------

struct DekTecAsiSink::Hw {
#ifdef HAS_DEKTEC
    DtDevice device;
    DtOutpChannel channel;
    bool deviceAttached = false;
    bool channelAttached = false;
    int fifoSize = 0;
    int tsRate = 0;
    qint64 underflows = 0;
    qint64 overflows = 0;
#endif
};

#ifdef HAS_DEKTEC
namespace {
QString dtErr(DTAPI_RESULT r) { return QString::fromLatin1(DtapiResult2Str(r)); }
}
#endif

DekTecAsiSink::DekTecAsiSink(const QString& name, const QJsonObject& options, QObject* parent)
    : PacketSink(name, parent), m_opts(options), m_hw(new Hw)
{
}

DekTecAsiSink::~DekTecAsiSink() {
    stop();
    delete m_hw;
}

bool DekTecAsiSink::isAvailable() {
#ifdef HAS_DEKTEC
    return true;
#else
    return false;
#endif
}

QJsonArray DekTecAsiSink::listDevices() {
    QJsonArray out;
#ifdef HAS_DEKTEC
    DtDeviceDesc descs[16];
    int n = 0;
    if (DtapiDeviceScan(16, n, descs) == DTAPI_OK) {
        for (int i = 0; i < n; ++i) {
            QJsonObject d;
            d["typeNumber"] = descs[i].m_TypeNumber;
            d["serial"] = QString::number(descs[i].m_Serial);
            d["ports"] = descs[i].m_NumPorts;
            d["outputs"] = descs[i].m_NumDtOutpChan;
            d["inputs"] = descs[i].m_NumDtInpChan;
            d["firmware"] = descs[i].m_FirmwareVersion;
            out.append(d);
        }
    }
#endif
    return out;
}

// ---------------------------------------------------------------------------
// Start / stop
// ---------------------------------------------------------------------------

bool DekTecAsiSink::openHardware(QString* err) {
#ifdef HAS_DEKTEC
    const int type = m_opts.value("deviceType").toInt(2154);
    const int index = m_opts.value("deviceIndex").toInt(0);
    const int port = m_opts.value("port").toInt(2);
    DTAPI_RESULT r;
    if (m_opts.contains("serial")) r = m_hw->device.AttachToSerial(m_opts["serial"].toString().toLongLong());
    else r = m_hw->device.AttachToType(type, index);
    if (r != DTAPI_OK) { *err = QString("DtDevice attach (type %1 #%2): %3").arg(type).arg(index).arg(dtErr(r)); return false; }
    m_hw->deviceAttached = true;

    // Make sure the port is an ASI output (the 2154 ports are bidirectional).
    if (m_opts.value("configurePort").toBool(true)) {
        int dir = 0, sub = 0;
        if (m_hw->device.GetIoConfig(port, DTAPI_IOCONFIG_IODIR, dir, sub) == DTAPI_OK && dir != DTAPI_IOCONFIG_OUTPUT) {
            r = m_hw->device.SetIoConfig(port, DTAPI_IOCONFIG_IODIR, DTAPI_IOCONFIG_OUTPUT);
            if (r != DTAPI_OK) qWarning() << "[DekTec] cannot set port" << port << "to output:" << dtErr(r);
            else qInfo() << "[DekTec] port" << port << "switched to output";
        }
        int std = 0;
        if (m_hw->device.GetIoConfig(port, DTAPI_IOCONFIG_IOSTD, std, sub) == DTAPI_OK && std != DTAPI_IOCONFIG_ASI) {
            r = m_hw->device.SetIoConfig(port, DTAPI_IOCONFIG_IOSTD, DTAPI_IOCONFIG_ASI);
            if (r != DTAPI_OK) qWarning() << "[DekTec] cannot set port" << port << "to ASI:" << dtErr(r);
            else qInfo() << "[DekTec] port" << port << "switched to ASI";
        }
    }

    r = m_hw->channel.AttachToPort(&m_hw->device, port);
    if (r != DTAPI_OK) { *err = QString("DtOutpChannel attach port %1: %2").arg(port).arg(dtErr(r)); return false; }
    m_hw->channelAttached = true;

    const int rate = int(m_opts.value("muxrate").toDouble(19392658.0));
    if ((r = m_hw->channel.SetTxControl(DTAPI_TXCTRL_IDLE)) != DTAPI_OK) { *err = "SetTxControl IDLE: " + dtErr(r); return false; }
    if ((r = m_hw->channel.SetTxMode(DTAPI_TXMODE_188, DTAPI_TXSTUFF_MODE_OFF)) != DTAPI_OK) { *err = "SetTxMode: " + dtErr(r); return false; }
    if ((r = m_hw->channel.SetTsRateBps(rate)) != DTAPI_OK) { *err = QString("SetTsRateBps %1: %2").arg(rate).arg(dtErr(r)); return false; }
    m_hw->channel.SetFifoSizeTyp();
    m_hw->channel.GetFifoSize(m_hw->fifoSize);
    m_hw->channel.GetTsRateBps(m_hw->tsRate);
    if ((r = m_hw->channel.ClearFifo()) != DTAPI_OK) { *err = "ClearFifo: " + dtErr(r); return false; }
    int st = 0, latched = 0;
    m_hw->channel.GetFlags(st, latched);
    m_hw->channel.ClearFlags(latched);
    if ((r = m_hw->channel.SetTxControl(DTAPI_TXCTRL_HOLD)) != DTAPI_OK) { *err = "SetTxControl HOLD: " + dtErr(r); return false; }
    qInfo().noquote() << QString("[DekTec] port %1 ASI ready: %2 bps, FIFO %3 bytes").arg(port).arg(m_hw->tsRate).arg(m_hw->fifoSize);
    return true;
#else
    *err = "DekTec support not compiled in";
    return false;
#endif
}

void DekTecAsiSink::closeHardware() {
#ifdef HAS_DEKTEC
    if (m_hw->channelAttached) {
        m_hw->channel.SetTxControl(DTAPI_TXCTRL_IDLE);
        m_hw->channel.Detach(DTAPI_INSTANT_DETACH);
        m_hw->channelAttached = false;
    }
    if (m_hw->deviceAttached) {
        m_hw->device.Detach();
        m_hw->deviceAttached = false;
    }
#endif
}

bool DekTecAsiSink::start(const AVCodecParameters* video, AVRational videoTimeBase,
                          const AVCodecParameters* audio, AVRational audioTimeBase) {
    if (m_running.load()) return true;
    QString err;
    if (!openHardware(&err)) { setError(err); closeHardware(); return false; }

    QJsonObject muxOpts = m_opts;
    if (!muxOpts.contains("muxrate")) muxOpts["muxrate"] = 19392658.0;
    m_mux = std::make_unique<TsMuxer>();
    if (!m_mux->open(video, videoTimeBase, audio, audioTimeBase, muxOpts,
                     [this](const uint8_t* d, int n) { enqueue(d, n); })) {
        setError("TS mux: " + m_mux->lastError());
        m_mux.reset();
        closeHardware();
        return false;
    }

    m_stop = false;
    m_running = true;
    m_transmitting = false;
    { QMutexLocker l(&m_statMutex); m_stats.connected = true; }
    m_writer = new Writer(this);
    m_writer->start(QThread::TimeCriticalPriority);
    return true;
}

void DekTecAsiSink::stop() {
    if (!m_running.load() && !m_writer) return;
    m_stop = true;
    { QMutexLocker l(&m_qmutex); m_qcond.wakeAll(); }
    if (m_writer) { m_writer->wait(5000); delete m_writer; m_writer = nullptr; }
    { QMutexLocker l(&m_muxMutex); if (m_mux) { m_mux->close(); m_mux.reset(); } }
    { QMutexLocker l(&m_qmutex); m_queue.clear(); m_queuedBytes = 0; }
    closeHardware();
    m_running = false;
    QMutexLocker l(&m_statMutex); m_stats.connected = false;
}

// ---------------------------------------------------------------------------
// Data path
// ---------------------------------------------------------------------------

void DekTecAsiSink::writePacket(const AVPacket* pkt, int streamKind) {
    if (!m_running.load()) return;
    QMutexLocker l(&m_muxMutex);
    if (!m_mux) return;
    if (!m_mux->writePacket(pkt, streamKind)) {
        QMutexLocker s(&m_statMutex); m_stats.dropped++;
    } else {
        QMutexLocker s(&m_statMutex); m_stats.framesIn++;
    }
}

void DekTecAsiSink::enqueue(const uint8_t* data, int size) {
    QMutexLocker l(&m_qmutex);
    if (m_queuedBytes + size > kMaxQueuedBytes) {
        // The card is not draining (cable? rate mismatch?): drop oldest, keep going.
        while (!m_queue.empty() && m_queuedBytes + size > kMaxQueuedBytes) {
            m_queuedBytes -= m_queue.front().size(); m_queue.pop_front();
        }
        QMutexLocker s(&m_statMutex); m_stats.dropped++;
    }
    m_queue.emplace_back(reinterpret_cast<const char*>(data), size);
    m_queuedBytes += size;
    m_qcond.wakeOne();
}

void DekTecAsiSink::pollStats() {
#ifdef HAS_DEKTEC
    int load = 0, st = 0, latched = 0;
    m_hw->channel.GetFifoLoad(load);
    m_hw->channel.GetFlags(st, latched);
    if (latched) {
        if (latched & DTAPI_TX_FIFO_UFL) { m_hw->underflows++; qWarning() << "[DekTec] FIFO underflow on" << m_name; }
        if (latched & DTAPI_TX_FIFO_OVF) { m_hw->overflows++; }
        m_hw->channel.ClearFlags(latched);
    }
    QMutexLocker s(&m_statMutex);
    m_stats.extra["fifoLoadBytes"] = load;
    m_stats.extra["fifoLoadPct"] = m_hw->fifoSize > 0 ? 100.0 * load / m_hw->fifoSize : 0.0;
    m_stats.extra["fifoSize"] = m_hw->fifoSize;
    m_stats.extra["tsRate"] = m_hw->tsRate;
    m_stats.extra["underflows"] = double(m_hw->underflows);
    m_stats.extra["overflows"] = double(m_hw->overflows);
    m_stats.extra["transmitting"] = m_transmitting.load();
    m_stats.extra["queuedBytes"] = double(m_queuedBytes);
#endif
}

void DekTecAsiSink::writeLoop() {
#ifdef HAS_DEKTEC
    const int rate = m_hw->tsRate > 0 ? m_hw->tsRate : int(m_opts.value("muxrate").toDouble(19392658.0));
    const double prefillMs = m_opts.value("prefillMs").toDouble(400.0);
    const qint64 prefillBytes = qint64(rate / 8.0 * prefillMs / 1000.0);
    qint64 written = 0;
    QElapsedTimer statTimer; statTimer.start();

    while (!m_stop.load()) {
        QByteArray chunk;
        {
            QMutexLocker l(&m_qmutex);
            while (m_queue.empty() && !m_stop.load()) m_qcond.wait(&m_qmutex, 50);
            if (m_stop.load()) break;
            chunk = std::move(m_queue.front());
            m_queue.pop_front();
            m_queuedBytes -= chunk.size();
        }

        // Back-pressure: never overfill the hardware FIFO (the card drains it at the TS rate).
        for (;;) {
            int load = 0;
            m_hw->channel.GetFifoLoad(load);
            if (m_hw->fifoSize <= 0 || load + chunk.size() < m_hw->fifoSize * 3 / 4) break;
            if (m_stop.load()) return;
            QThread::usleep(1000);
        }

        DTAPI_RESULT r = m_hw->channel.Write(chunk.data(), chunk.size());
        if (r != DTAPI_OK) {
            setError("Write: " + dtErr(r));
            QMutexLocker s(&m_statMutex); m_stats.dropped++;
            continue;
        }
        written += chunk.size();
        { QMutexLocker s(&m_statMutex); m_stats.framesOut++; m_stats.bytesOut += chunk.size(); }

        if (!m_transmitting.load() && written >= prefillBytes) {
            r = m_hw->channel.SetTxControl(DTAPI_TXCTRL_SEND);
            if (r != DTAPI_OK) setError("SetTxControl SEND: " + dtErr(r));
            else { m_transmitting = true; qInfo() << "[DekTec]" << m_name << "transmitting after" << written << "bytes prefill"; }
        }
        if (statTimer.elapsed() >= 500) { pollStats(); statTimer.restart(); }
    }
#endif
}
