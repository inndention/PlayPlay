#include <QtTest/QtTest>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QSemaphore>
#include <QThread>
#include <atomic>
#include <thread>
#include "playout/ui_ipc/ipctransport.h"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>

Q_DECLARE_METATYPE(IpcProtocol::Message)

// PS-1 (W7, PE-20): IPC backpressure and async connect.
// Two kinds of peer that stop reading:
//  - a raw Win32 pipe end that is never read: a process that is frozen entirely (suspended, a debugger, a WER dump) or
//    a non-Qt reader;
//  - a real IpcTransport on a thread whose event loop is parked (ParkedPeer): a hung app, engine or Supervisor. Qt's
//    Windows pipe reader keeps reading on the thread pool while the owner's loop is stuck, so this is the live case;
//    without a read limit it would drain the sender into its own memory and the sender's caps would never trigger.
// Every pipe name is unique to this process ("ppbp-..."); nothing here touches the live IPC name.
class TestIpcBackpressure : public QObject {
    Q_OBJECT

private slots:
    void decideTable();
    void defaultPriorityByType();
    void droppableCappedForNonReadingClient();
    void hardCapDisconnectsNonReadingClients();
    void peerCapsOverride();
    void oversizeLengthHeaderCloses();
    void lengthLimitIsExact();
    void clientRoleCapsWithNonReadingServer();
    void stockQtReaderDrainsWhileStuck();
    void hungQtClientBoundsBothSides();
    void hungQtServerBoundsEngine();
    void disconnectHandsOutWhatArrived();
    void asyncConnectAbsentServerNeverBlocks();
    void asyncConnectBusyServerNeverBlocks();
    void asyncConnectRoundTrip();
    void asyncConnectCancelled();
    void broadcastAndSetSend();
};

namespace {
QString uniqueName(const char* tag) {
    static int n = 0;
    return QString("ppbp-%1-%2-%3").arg(tag).arg(QCoreApplication::applicationPid()).arg(++n);
}
std::wstring pipePath(const QString& name) { return (QStringLiteral("\\\\.\\pipe\\") + name).toStdWString(); }

// Client end that never reads.
HANDLE openRawClient(const QString& name) {
    return CreateFileW(pipePath(name).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
}
// Server end (instances) that never reads or accepts: clients can still open it right after creation.
HANDLE createRawServer(const QString& name, DWORD instances = 1) {
    return CreateNamedPipeW(pipePath(name).c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                            instances, 0, 0, 0, nullptr);
}

qint64 privateBytes() {
    PROCESS_MEMORY_COUNTERS_EX pmc{}; pmc.cb = sizeof(pmc);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
    return qint64(pmc.PrivateUsage);
}

// Worst gap between ticks of a 5 ms timer = how long this thread's event loop was blocked.
struct LagProbe {
    QTimer timer; QElapsedTimer clock; qint64 last = 0, worst = 0;
    LagProbe() {
        timer.setTimerType(Qt::PreciseTimer); timer.setInterval(5);
        QObject::connect(&timer, &QTimer::timeout, [this]() { const qint64 now = clock.elapsed(); worst = qMax(worst, now - last); last = now; });
        reset(); timer.start();
    }
    void reset() { clock.restart(); last = 0; worst = 0; }
    qint64 lagMs() const { return qMax<qint64>(0, worst - 5); }
};

IpcProtocol::Message previewMsg(int bytes, int frame) {
    QJsonObject p; p["jpegBase64"] = QString(bytes, QLatin1Char('A')); p["frame"] = frame; p["width"] = 1280; p["height"] = 720;
    return IpcProtocol::make(IpcProtocol::MSG_PREVIEW_FRAME, "perftest", p);
}
IpcProtocol::Message bulkMsg(IpcProtocol::MessageType t, int bytes) {
    QJsonObject p; p["blob"] = QString(bytes, QLatin1Char('B'));
    return IpcProtocol::make(t, "perftest", p);
}
qint64 framedSize(const IpcProtocol::Message& m) { return 4 + m.toJson().size(); }

// Raw frame: 4-byte little-endian length + body (written on a helper thread: a synchronous pipe write can wait for
// the reader, and the reader is this thread's event loop).
QByteArray rawFrame(quint32 len, const QByteArray& body) {
    QByteArray f(reinterpret_cast<const char*>(&len), 4); f += body; return f;
}
std::thread writeRaw(HANDLE h, QByteArray bytes) {
    return std::thread([h, bytes]() { DWORD n = 0; WriteFile(h, bytes.constData(), DWORD(bytes.size()), &n, nullptr); });
}

// A Normal message carrying a sequence number (alerts to a UI, as-run from an engine).
IpcProtocol::Message seqMsg(IpcProtocol::MessageType t, int seq, int bytes) {
    QJsonObject p; p["seq"] = seq; p["blob"] = QString(bytes, QLatin1Char('s'));
    return IpcProtocol::make(t, "perftest", p);
}

// An IpcTransport living on its own thread, whose event loop the test can park (a hung app, engine or Supervisor).
// Everything the main thread reads from it is atomic; the transport itself is only touched on its thread.
class ParkedPeer {
public:
    std::atomic<int> previews{0}, ordered{0}, outOfOrder{0}, connections{0}, disconnects{0};
    std::atomic<int> busyUsPerOrdered{0};   // per-message work while catching up (the Supervisor decodes, relays, ...)
    std::atomic<qint64> worstGapMs{0};      // worst gap between 5 ms ticks on this thread since the last unpark

    explicit ParkedPeer(IpcTransport::Role role) {
        m_ctx = new QObject; m_ctx->moveToThread(&m_thread);
        m_thread.start();
        run([this, role]() {
            t = new IpcTransport(role);
            QObject::connect(t, &IpcTransport::messageReceived, t, [this](const IpcProtocol::Message& m, const QString&) { onMessage(m); });
            QObject::connect(t, &IpcTransport::clientConnected, t, [this](const QString&) { ++connections; });
            QObject::connect(t, &IpcTransport::clientDisconnected, t, [this](const QString&) { ++disconnects; });
            QObject::connect(t, &IpcTransport::connectionStateChanged, t, [this](bool up) {   // client role: count lost links only
                if (up && !m_linked) ++connections;
                if (!up && m_linked) ++disconnects;
                m_linked = up;
            });
            auto* tick = new QTimer(t); tick->setTimerType(Qt::PreciseTimer); tick->setInterval(5);
            QObject::connect(tick, &QTimer::timeout, t, [this]() { const qint64 now = m_clock.elapsed(); worstGapMs = qMax<qint64>(worstGapMs, now - m_last); m_last = now; });
            m_clock.start(); m_last = 0; tick->start();
        });
    }
    ~ParkedPeer() {
        unpark();
        run([this]() { delete t; t = nullptr; });
        m_thread.quit(); m_thread.wait();
        delete m_ctx;
    }
    template <class F> void run(F f) { QMetaObject::invokeMethod(m_ctx, std::move(f), Qt::BlockingQueuedConnection); }
    // Returns once the thread's loop is stuck; until unpark() only Qt's background pipe reader runs for it.
    void park() {
        QMetaObject::invokeMethod(m_ctx, [this]() {
            m_isParked.release();
            m_gate.acquire();
            m_last = m_clock.elapsed(); worstGapMs = 0;   // the parked stretch itself is not a catch-up turn
        }, Qt::QueuedConnection);
        m_isParked.acquire();
        m_parked = true;
    }
    void unpark() { if (m_parked) { m_parked = false; m_gate.release(); } }
    qint64 worstTurnMs() const { return qMax<qint64>(0, worstGapMs - 5); }

    IpcTransport* t = nullptr;

private:
    void onMessage(const IpcProtocol::Message& m) {
        if (m.type == IpcProtocol::MSG_PREVIEW_FRAME) { ++previews; return; }
        if (!m.payload.contains("seq")) return;
        const int seq = m.payload.value("seq").toInt();
        if (seq != m_nextSeq) ++outOfOrder;
        m_nextSeq = seq + 1;
        ++ordered;
        if (const int us = busyUsPerOrdered.load()) { QElapsedTimer b; b.start(); while (b.nsecsElapsed() < qint64(us) * 1000) {} }
    }
    QThread m_thread;
    QObject* m_ctx = nullptr;
    QSemaphore m_gate, m_isParked;
    bool m_parked = false;
    QElapsedTimer m_clock; qint64 m_last = 0;   // this thread only
    int m_nextSeq = 0;                          // this thread only
    bool m_linked = false;                      // this thread only
};
}

void TestIpcBackpressure::decideTable() {
    using V = IpcTransport::Verdict;
    const IpcTransport::Caps caps;   // production defaults
    QCOMPARE(caps.softBytes, qint64(4) << 20);
    QCOMPARE(caps.hardBytes, qint64(64) << 20);
    QCOMPARE(IpcTransport::kDefaultMaxMessageBytes, qint64(64) << 20);
    struct Row { qint64 queued; IpcTransport::Priority p; V v; };
    const Row rows[] = {
        {0,                       IpcTransport::Droppable, V::Send},
        {(4 << 20) - 1,           IpcTransport::Droppable, V::Send},
        {4 << 20,                 IpcTransport::Droppable, V::Drop},
        {4 << 20,                 IpcTransport::Normal,    V::Send},       // Normal is never skipped
        {(64LL << 20) - 1,        IpcTransport::Normal,    V::Send},
        {64LL << 20,              IpcTransport::Normal,    V::Disconnect},
        {64LL << 20,              IpcTransport::Droppable, V::Disconnect},
        {qint64(5) << 30,         IpcTransport::Normal,    V::Disconnect},
    };
    for (const Row& r : rows) QCOMPARE(int(IpcTransport::decide(r.queued, r.p, caps)), int(r.v));
    IpcTransport::Caps off; off.softBytes = 0; off.hardBytes = 0;   // caps off = today's unbounded behaviour
    QCOMPARE(int(IpcTransport::decide(qint64(1) << 40, IpcTransport::Droppable, off)), int(V::Send));

    // By role: the server cuts off a client at the hard cap; a client (engine, app) never cuts its own link (no engine
    // reconnect until PS-3), it only skips Droppable messages past the soft cap.
    const IpcTransport::Caps srv = IpcTransport::defaultCaps(IpcTransport::ServerRole), cli = IpcTransport::defaultCaps(IpcTransport::ClientRole);
    QCOMPARE(srv.softBytes, qint64(4) << 20); QCOMPARE(srv.hardBytes, qint64(64) << 20);
    QCOMPARE(cli.softBytes, qint64(4) << 20); QCOMPARE(cli.hardBytes, qint64(0));
    QCOMPARE(IpcTransport(IpcTransport::ClientRole).caps().hardBytes, qint64(0));
    QCOMPARE(IpcTransport(IpcTransport::ServerRole).caps().hardBytes, qint64(64) << 20);
    QCOMPARE(int(IpcTransport::decide(qint64(1) << 30, IpcTransport::Normal, cli)), int(V::Send));
    QCOMPARE(int(IpcTransport::decide(qint64(4) << 20, IpcTransport::Droppable, cli)), int(V::Drop));
    QCOMPARE(IpcTransport::kDefaultReadBufferBytes, qint64(1) << 20);
}

void TestIpcBackpressure::defaultPriorityByType() {
    using namespace IpcProtocol;
    QCOMPARE(IpcTransport::defaultPriority(MSG_PREVIEW_FRAME), IpcTransport::Droppable);
    QCOMPARE(IpcTransport::defaultPriority(MSG_UI_TELEMETRY), IpcTransport::Droppable);
    QCOMPARE(IpcTransport::defaultPriority(MSG_TIME_SYNC), IpcTransport::Droppable);
    for (MessageType t : {MSG_PLAYLIST_PUSH, MSG_TRANSPORT_COMMAND, MSG_CONFIG_UPDATE, MSG_HEARTBEAT, MSG_STATE_CHANGE, MSG_ASRUN_LOG,
                          MSG_ENGINE_READY, MSG_ENGINE_FATAL, MSG_UI_COMMAND, MSG_UI_QUERY_STATUS, MSG_UI_STATUS, MSG_UI_ALERT,
                          MSG_OUTPUT_CONFIG, MSG_CG_COMMAND, MSG_UI_SCHEDULE, MSG_UI_ASRUN, MSG_UI_RUNLIST})
        QCOMPARE(IpcTransport::defaultPriority(t), IpcTransport::Normal);
}

void TestIpcBackpressure::droppableCappedForNonReadingClient() {
    IpcTransport server(IpcTransport::ServerRole);
    const QString name = uniqueName("drop");
    QVERIFY(server.startServer(name));
    QSignalSpy conn(&server, &IpcTransport::clientConnected), disc(&server, &IpcTransport::clientDisconnected);
    HANDLE raw = openRawClient(name);
    QVERIFY(raw != INVALID_HANDLE_VALUE);
    QTRY_COMPARE_WITH_TIMEOUT(conn.count(), 1, 3000);
    const QString id = conn.at(0).at(0).toString();

    // A program monitor at ~100 KB per picture, 500 pictures/s offered for 1.5 s (75 MB) to a client that reads nothing.
    const IpcProtocol::Message frame = previewMsg(100 * 1024, 1);
    const qint64 frameBytes = framedSize(frame);
    const qint64 mem0 = privateBytes();
    qint64 peakQueued = 0, peakMem = mem0, worstCallMs = 0; int sent = 0, refused = 0;
    QTimer producer; producer.setInterval(10);
    connect(&producer, &QTimer::timeout, this, [&]() {
        for (int i = 0; i < 5; ++i) {
            QElapsedTimer c; c.start();
            (server.sendTo(id, frame) ? sent : refused)++;
            worstCallMs = qMax(worstCallMs, c.elapsed());
            peakQueued = qMax(peakQueued, server.queuedBytes(id));
        }
        peakMem = qMax(peakMem, privateBytes());
    });
    LagProbe probe;
    producer.start();
    QTest::qWait(1500);
    producer.stop();
    const qint64 lag = probe.lagMs();

    qInfo().noquote() << QString("droppable: sent %1, skipped %2, peak queued %3 KB (soft cap %4 KB), peak private +%5 MB, loop lag %6 ms, worst send %7 ms")
                             .arg(sent).arg(refused).arg(peakQueued / 1024).arg(server.caps().softBytes / 1024)
                             .arg((peakMem - mem0) >> 20).arg(lag).arg(worstCallMs);
    QVERIFY2(refused > 0, "previews must be skipped once the client is 4 MB behind");
    QVERIFY(sent > 0);
    QVERIFY2(peakQueued < server.caps().softBytes + frameBytes, "queue bounded by soft cap + one frame");
    QVERIFY2(peakMem - mem0 < (qint64(24) << 20), "memory follows the queue, not the offered 75 MB");
    QVERIFY2(lag < 50, qPrintable(QString("loop lag %1 ms").arg(lag)));
    QVERIFY(worstCallMs < 20);
    QCOMPARE(disc.count(), 0);                                   // droppable traffic alone never disconnects
    QCOMPARE(server.stats().value("dropped").toDouble(), double(refused));
    // Normal messages still go out while the client is behind (they are what the hard cap bounds).
    QVERIFY(server.sendTo(id, IpcProtocol::makeTransportCommand("perftest", "play", {})));

    CloseHandle(raw);
    QTRY_COMPARE_WITH_TIMEOUT(disc.count(), 1, 3000);
    QVERIFY(server.clientIds().isEmpty());
}

void TestIpcBackpressure::hardCapDisconnectsNonReadingClients() {
    IpcTransport server(IpcTransport::ServerRole);
    const QString name = uniqueName("hard");
    QVERIFY(server.startServer(name));
    QSignalSpy conn(&server, &IpcTransport::clientConnected), disc(&server, &IpcTransport::clientDisconnected);
    HANDLE rawA = openRawClient(name);
    QVERIFY(rawA != INVALID_HANDLE_VALUE);
    QTRY_COMPARE_WITH_TIMEOUT(conn.count(), 1, 3000);
    HANDLE rawB = openRawClient(name);
    QVERIFY(rawB != INVALID_HANDLE_VALUE);
    QTRY_COMPARE_WITH_TIMEOUT(conn.count(), 2, 3000);
    const QStringList ids = server.clientIds();

    // Normal 1 MB messages (runlists) broadcast to two clients that never read, until both are cut off.
    const IpcProtocol::Message big = bulkMsg(IpcProtocol::MSG_UI_RUNLIST, 1 << 20);
    const qint64 bigBytes = framedSize(big);
    const qint64 mem0 = privateBytes();
    qint64 peakQueued = 0, peakMem = mem0, worstCallMs = 0; int rounds = 0;
    // One broadcast per tick: serializing 1 MB of JSON costs ~7 ms by itself, and the probe should see the transport,
    // not the test's own producer.
    QTimer producer; producer.setInterval(5);
    connect(&producer, &QTimer::timeout, this, [&]() {
        QElapsedTimer c; c.start();
        server.broadcast(big);
        worstCallMs = qMax(worstCallMs, c.elapsed());
        ++rounds;
        for (const QString& id : ids) peakQueued = qMax(peakQueued, server.queuedBytes(id));
        peakMem = qMax(peakMem, privateBytes());
    });
    LagProbe probe;
    producer.start();
    QTRY_COMPARE_WITH_TIMEOUT(disc.count(), 2, 15000);
    producer.stop();
    const qint64 lag = probe.lagMs();
    const qint64 hard = server.caps().hardBytes;

    qInfo().noquote() << QString("hard cap: %1 broadcasts, peak queued %2 MB per client (cap %3 MB), peak private +%4 MB for two clients, loop lag %5 ms, worst broadcast %6 ms")
                             .arg(rounds).arg(peakQueued >> 20).arg(hard >> 20).arg((peakMem - mem0) >> 20).arg(lag).arg(worstCallMs);
    QVERIFY2(peakQueued < hard + bigBytes, "a client never holds more than the hard cap + one message");
    QVERIFY2(peakMem - mem0 < 2 * (hard + bigBytes) + (qint64(32) << 20), "server memory bounded by the caps");
    QVERIFY2(lag < 50, qPrintable(QString("loop lag %1 ms").arg(lag)));
    QCOMPARE(server.stats().value("slowDisconnects").toDouble(), 2.0);
    QVERIFY(server.clientIds().isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(privateBytes() - mem0 < (qint64(48) << 20), 3000);   // the queues are freed

    // The server keeps serving well-behaved clients.
    IpcTransport client(IpcTransport::ClientRole);
    QSignalSpy got(&client, &IpcTransport::messageReceived);
    client.connectToServerAsync(name, 2000);
    QTRY_VERIFY_WITH_TIMEOUT(client.isConnected(), 3000);
    QTRY_COMPARE_WITH_TIMEOUT(conn.count(), 3, 3000);
    QVERIFY(server.sendTo(conn.at(2).at(0).toString(), IpcProtocol::makeTransportCommand("perftest", "play", {})));
    QTRY_COMPARE_WITH_TIMEOUT(got.count(), 1, 3000);
    CloseHandle(rawA); CloseHandle(rawB);
}

void TestIpcBackpressure::peerCapsOverride() {
    // The Supervisor's engine links: no hard cap for that one client (the heartbeat watchdog handles a frozen engine);
    // every other client keeps the transport's caps. Small caps keep the test quick.
    IpcTransport server(IpcTransport::ServerRole);
    IpcTransport::Caps small; small.softBytes = 1 << 20; small.hardBytes = 4 << 20;
    server.setCaps(small);
    const QString name = uniqueName("peercaps");
    QVERIFY(server.startServer(name));
    QSignalSpy conn(&server, &IpcTransport::clientConnected), disc(&server, &IpcTransport::clientDisconnected);
    HANDLE engine = openRawClient(name);
    QVERIFY(engine != INVALID_HANDLE_VALUE);
    QTRY_COMPARE_WITH_TIMEOUT(conn.count(), 1, 3000);
    HANDLE ui = openRawClient(name);
    QVERIFY(ui != INVALID_HANDLE_VALUE);
    QTRY_COMPARE_WITH_TIMEOUT(conn.count(), 2, 3000);
    const QString engineId = conn.at(0).at(0).toString(), uiId = conn.at(1).at(0).toString();
    IpcTransport::Caps noHard = small; noHard.hardBytes = 0;
    QVERIFY(server.setPeerCaps(engineId, noHard));
    QVERIFY(!server.setPeerCaps("nobody", noHard));

    const IpcProtocol::Message big = bulkMsg(IpcProtocol::MSG_PLAYLIST_PUSH, 1 << 20);
    for (int i = 0; i < 10; ++i) { server.broadcast(big); QTest::qWait(5); }
    QTRY_COMPARE_WITH_TIMEOUT(disc.count(), 1, 3000);
    QCOMPARE(disc.at(0).at(0).toString(), uiId);                          // the UI client was cut off at 4 MB
    QVERIFY(server.clientIds().contains(engineId));                       // the engine link was not
    QVERIFY(server.queuedBytes(engineId) > small.hardBytes);
    QVERIFY(!server.sendTo(engineId, previewMsg(1000, 1)));               // still skips Droppable past its soft cap
    QVERIFY(server.sendTo(engineId, IpcProtocol::makeTransportCommand("perftest", "play", {})));
    CloseHandle(engine); CloseHandle(ui);
}

void TestIpcBackpressure::oversizeLengthHeaderCloses() {
    IpcTransport server(IpcTransport::ServerRole);
    const QString name = uniqueName("oversize");
    QVERIFY(server.startServer(name));
    QSignalSpy conn(&server, &IpcTransport::clientConnected), disc(&server, &IpcTransport::clientDisconnected),
               msgs(&server, &IpcTransport::messageReceived);
    const qint64 mem0 = privateBytes();

    // A bogus 4 GB header (0xFFFFFFFF: the old 32-bit "4 + len" check wrapped to 3 and then read 4 GB).
    HANDLE raw = openRawClient(name);
    QVERIFY(raw != INVALID_HANDLE_VALUE);
    QTRY_COMPARE_WITH_TIMEOUT(conn.count(), 1, 3000);
    std::thread w = writeRaw(raw, rawFrame(0xFFFFFFFFu, QByteArray(16, 'x')));
    QTRY_COMPARE_WITH_TIMEOUT(disc.count(), 1, 3000);
    CloseHandle(raw);   // also releases the writer if it still waits
    w.join();

    // Just over the 64 MB limit: closed as well, without waiting for 64 MB to arrive.
    HANDLE raw2 = openRawClient(name);
    QVERIFY(raw2 != INVALID_HANDLE_VALUE);
    QTRY_COMPARE_WITH_TIMEOUT(conn.count(), 2, 3000);
    std::thread w2 = writeRaw(raw2, rawFrame(quint32((64 << 20) + 1), QByteArray(64, '{')));
    QTRY_COMPARE_WITH_TIMEOUT(disc.count(), 2, 3000);
    CloseHandle(raw2);
    w2.join();

    QCOMPARE(msgs.count(), 0);
    QCOMPARE(server.stats().value("oversizeCloses").toDouble(), 2.0);
    QVERIFY2(privateBytes() - mem0 < (qint64(16) << 20), "nothing was allocated for the bogus lengths");

    // A large legitimate message (8 MB, under the limit) still arrives intact.
    IpcTransport client(IpcTransport::ClientRole);
    client.connectToServerAsync(name, 2000);
    QTRY_VERIFY_WITH_TIMEOUT(client.isConnected(), 3000);
    QJsonArray list; for (int i = 0; i < 8; ++i) { QJsonObject it; it["assetId"] = QString::number(i); it["blob"] = QString(1 << 20, QLatin1Char('p')); list << it; }
    QVERIFY(client.send(IpcProtocol::makePlaylistPush("perftest", list)));
    QTRY_COMPARE_WITH_TIMEOUT(msgs.count(), 1, 5000);
    const auto m = msgs.at(0).at(0).value<IpcProtocol::Message>();
    QCOMPARE(m.type, IpcProtocol::MSG_PLAYLIST_PUSH);
    QCOMPARE(m.payload.value("playlist").toArray().size(), 8);
    QCOMPARE(m.payload.value("playlist").toArray().at(7).toObject().value("blob").toString().size(), 1 << 20);
}

void TestIpcBackpressure::lengthLimitIsExact() {
    IpcTransport server(IpcTransport::ServerRole);
    const qint64 limit = 1 << 20;
    server.setMaxMessageBytes(limit);
    const QString name = uniqueName("limit");
    QVERIFY(server.startServer(name));
    QSignalSpy conn(&server, &IpcTransport::clientConnected), disc(&server, &IpcTransport::clientDisconnected),
               msgs(&server, &IpcTransport::messageReceived);
    HANDLE raw = openRawClient(name);
    QVERIFY(raw != INVALID_HANDLE_VALUE);
    QTRY_COMPARE_WITH_TIMEOUT(conn.count(), 1, 3000);

    // Exactly at the limit: accepted (valid JSON padded with whitespace to the byte).
    QByteArray body = IpcProtocol::makeHeartbeat("perftest", 7, 2, true).toJson();
    body += QByteArray(int(limit) - body.size(), ' ');
    QByteArray bytes = rawFrame(quint32(limit), body);
    // One byte over the limit: the connection closes.
    bytes += rawFrame(quint32(limit + 1), QByteArray(8, ' '));
    std::thread w = writeRaw(raw, bytes);
    QTRY_COMPARE_WITH_TIMEOUT(disc.count(), 1, 5000);
    QCOMPARE(msgs.count(), 1);
    const auto m = msgs.at(0).at(0).value<IpcProtocol::Message>();
    QCOMPARE(m.type, IpcProtocol::MSG_HEARTBEAT);
    QCOMPARE(m.channelId, QString("perftest"));
    CloseHandle(raw);
    w.join();

    // The send side refuses a frame the receiver would reject, instead of getting the link closed.
    IpcTransport client(IpcTransport::ClientRole);
    client.setMaxMessageBytes(limit);
    client.connectToServerAsync(name, 2000);
    QTRY_VERIFY_WITH_TIMEOUT(client.isConnected(), 3000);
    QVERIFY(!client.send(bulkMsg(IpcProtocol::MSG_ASRUN_LOG, int(limit))));
    QVERIFY(client.isConnected());
}

void TestIpcBackpressure::clientRoleCapsWithNonReadingServer() {
    // An engine whose Supervisor is frozen entirely (raw server end, never read): previews stop queueing at the soft
    // cap; Normal messages keep queueing (by default a client never cuts its own link: an engine has no reconnect until
    // PS-3). The hard-cap mechanism itself works in the client role when a caller sets one.
    const QString name = uniqueName("client");
    HANDLE rs = createRawServer(name);
    QVERIFY(rs != INVALID_HANDLE_VALUE);
    IpcTransport client(IpcTransport::ClientRole);
    QSignalSpy state(&client, &IpcTransport::connectionStateChanged);
    client.connectToServerAsync(name, 2000);
    QTRY_VERIFY_WITH_TIMEOUT(client.isConnected(), 3000);
    QCOMPARE(client.caps().hardBytes, qint64(0));

    const IpcProtocol::Message frame = previewMsg(100 * 1024, 2);
    const qint64 frameBytes = framedSize(frame);
    qint64 peakQueued = 0; int sent = 0, refused = 0;
    LagProbe probe;
    for (int i = 0; i < 300; ++i) {   // 30 MB offered
        (client.send(frame) ? sent : refused)++;
        peakQueued = qMax(peakQueued, client.queuedBytes());
        if (i % 20 == 0) QCoreApplication::processEvents();
    }
    QVERIFY(refused > 0);
    QVERIFY2(peakQueued < client.caps().softBytes + frameBytes, "engine-side preview queue bounded by the soft cap");
    QVERIFY(client.isConnected());

    // Messages that cannot be skipped (as-run) are kept, and the link stays up without a hard cap.
    const IpcProtocol::Message asrun = bulkMsg(IpcProtocol::MSG_ASRUN_LOG, 1 << 20);
    const qint64 asrunBytes = framedSize(asrun);
    for (int i = 0; i < 12; ++i) { QVERIFY(client.send(asrun)); QCoreApplication::processEvents(); }
    QVERIFY(client.isConnected());
    QVERIFY(client.queuedBytes() >= 12 * asrunBytes);

    // With a hard cap set, the client drops the link once that much cannot be delivered.
    IpcTransport::Caps capped = client.caps(); capped.hardBytes = qint64(24) << 20;
    client.setCaps(capped);
    for (int i = 0; i < 40 && client.isConnected(); ++i) {
        client.send(asrun);
        peakQueued = qMax(peakQueued, client.queuedBytes());
        QCoreApplication::processEvents();
    }
    QTRY_VERIFY_WITH_TIMEOUT(!client.isConnected(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(!state.isEmpty() && state.last().at(0).toBool() == false, 3000);
    QVERIFY2(peakQueued < capped.hardBytes + asrunBytes, "client memory bounded by its hard cap + one message");
    QCOMPARE(client.stats().value("slowDisconnects").toDouble(), 1.0);
    QVERIFY(!client.send(IpcProtocol::makeHeartbeat("perftest", 1, 0, true)));
    QVERIFY(probe.lagMs() < 200);   // this loop runs the producer inline; it only has to stay sane
    CloseHandle(rs);
}

void TestIpcBackpressure::stockQtReaderDrainsWhileStuck() {
    // Control for the two tests below: a plain QLocalSocket (no read limit) on a parked thread keeps reading in the
    // background, so the sender sees no backlog at all and the stuck process grows instead. This is what the transport's
    // setReadBufferSize prevents; raw pipe ends that never read do not show it.
    IpcTransport server(IpcTransport::ServerRole);
    const QString name = uniqueName("stock");
    QVERIFY(server.startServer(name));
    QSignalSpy conn(&server, &IpcTransport::clientConnected);
    ParkedPeer peer(IpcTransport::ClientRole);
    bool ok = false;
    peer.run([&]() { auto* s = new QLocalSocket(peer.t); s->connectToServer(name); ok = s->waitForConnected(2000); });
    QVERIFY(ok);
    QTRY_COMPARE_WITH_TIMEOUT(conn.count(), 1, 3000);
    const QString id = conn.at(0).at(0).toString();
    peer.park();
    const qint64 mem0 = privateBytes();
    const IpcProtocol::Message big = bulkMsg(IpcProtocol::MSG_UI_RUNLIST, 1 << 20);
    for (int i = 0; i < 24; ++i) { QVERIFY(server.sendTo(id, big)); QTest::qWait(2); }
    QTRY_VERIFY_WITH_TIMEOUT(server.queuedBytes(id) < (qint64(2) << 20), 3000);   // all of it left the sender
    const qint64 grew = privateBytes() - mem0;
    qInfo().noquote() << QString("stock QLocalSocket on a stuck thread: 24 MB sent, sender queue %1 KB, process private +%2 MB")
                             .arg(server.queuedBytes(id) / 1024).arg(grew >> 20);
    QVERIFY2(grew > (qint64(16) << 20), "the stuck reader holds what was sent");
    peer.unpark();
}

void TestIpcBackpressure::hungQtClientBoundsBothSides() {
    // The Supervisor (this thread) and a UI client whose event loop is stuck (a real IpcTransport on a parked thread).
    // Without a read limit Qt's pipe reader would keep draining the Supervisor into the hung client's memory: the
    // Supervisor's queue would stay ~0, nothing would be skipped, nobody would be cut off.
    IpcTransport server(IpcTransport::ServerRole);
    const QString name = uniqueName("hungui");
    QVERIFY(server.startServer(name));
    QSignalSpy conn(&server, &IpcTransport::clientConnected), disc(&server, &IpcTransport::clientDisconnected);
    ParkedPeer ui(IpcTransport::ClientRole);
    bool ok = false;
    ui.run([&]() { ok = ui.t->connectToServer(name); });
    QVERIFY(ok);
    QTRY_COMPARE_WITH_TIMEOUT(conn.count(), 1, 3000);
    const QString id = conn.at(0).at(0).toString();

    // (a) Previews, ~100 KB at 500/s for 1.5 s (75 MB offered), while the UI is stuck.
    ui.park();
    const IpcProtocol::Message frame = previewMsg(100 * 1024, 1);
    const qint64 frameBytes = framedSize(frame);
    const qint64 mem0 = privateBytes();
    qint64 peakQueued = 0, peakMem = mem0; int sent = 0, refused = 0;
    QTimer producer; producer.setInterval(10);
    connect(&producer, &QTimer::timeout, this, [&]() {
        for (int i = 0; i < 5; ++i) { (server.sendTo(id, frame) ? sent : refused)++; peakQueued = qMax(peakQueued, server.queuedBytes(id)); }
        peakMem = qMax(peakMem, privateBytes());
    });
    LagProbe probe;
    producer.start();
    QTest::qWait(1500);
    producer.stop();
    const qint64 lagA = probe.lagMs();
    qInfo().noquote() << QString("hung Qt UI, previews: sent %1, skipped %2, Supervisor queue peak %3 KB, process private +%4 MB, loop lag %5 ms")
                             .arg(sent).arg(refused).arg(peakQueued / 1024).arg((peakMem - mem0) >> 20).arg(lagA);
    QVERIFY2(refused > 0, "the sender must see the hung reader's backlog and skip previews");
    QVERIFY2(peakQueued < server.caps().softBytes + frameBytes, "Supervisor queue bounded by the soft cap");
    QVERIFY2(peakMem - mem0 < (qint64(24) << 20), "neither side holds the offered 75 MB");
    QVERIFY2(lagA < 50, qPrintable(QString("loop lag %1 ms").arg(lagA)));
    QCOMPARE(disc.count(), 0);
    ui.unpark();
    QTRY_VERIFY_WITH_TIMEOUT(server.queuedBytes(id) == 0 && ui.previews.load() == sent, 10000);   // what was sent arrives

    // (b) Catching up: 6000 Normal messages (~12 MB) queued while stuck, 200 us of work each once it runs again.
    // Everything arrives in order, and no loop turn of the recovering thread is long.
    ui.busyUsPerOrdered = 200;
    ui.park();
    const int n = 6000;
    for (int i = 0; i < n; ++i) QVERIFY(server.sendTo(id, seqMsg(IpcProtocol::MSG_UI_ALERT, i, 2000)));
    const qint64 queuedWhileStuck = server.queuedBytes(id);
    QVERIFY2(queuedWhileStuck > (qint64(8) << 20), qPrintable(QString("the backlog stays with the sender (%1 KB queued)").arg(queuedWhileStuck / 1024)));
    QElapsedTimer rec; rec.start();
    ui.unpark();
    QTRY_COMPARE_WITH_TIMEOUT(ui.ordered.load(), n, 20000);
    const qint64 recoveryMs = rec.elapsed(), worstTurn = ui.worstTurnMs();
    qInfo().noquote() << QString("hung Qt UI, catch-up: %1 KB queued at the Supervisor while stuck; %2 messages handed out in %3 ms, worst loop turn %4 ms")
                             .arg(queuedWhileStuck / 1024).arg(n).arg(recoveryMs).arg(worstTurn);
    QCOMPARE(ui.outOfOrder.load(), 0);
    QVERIFY2(worstTurn < 50, qPrintable(QString("worst loop turn while catching up %1 ms").arg(worstTurn)));
    ui.busyUsPerOrdered = 0;

    // (c) Messages that cannot be skipped (1 MB runlists) while stuck: cut off at the hard cap, memory bounded.
    ui.park();
    const IpcProtocol::Message big = bulkMsg(IpcProtocol::MSG_UI_RUNLIST, 1 << 20);
    const qint64 bigBytes = framedSize(big);
    const qint64 mem1 = privateBytes();
    peakQueued = 0; peakMem = mem1;
    QTimer bulk; bulk.setInterval(5);
    connect(&bulk, &QTimer::timeout, this, [&]() {
        server.broadcast(big);
        peakQueued = qMax(peakQueued, server.queuedBytes(id));
        peakMem = qMax(peakMem, privateBytes());
    });
    bulk.start();
    QTRY_COMPARE_WITH_TIMEOUT(disc.count(), 1, 15000);
    bulk.stop();
    qInfo().noquote() << QString("hung Qt UI, runlists: cut off with %1 MB queued, process private +%2 MB")
                             .arg(peakQueued >> 20).arg((peakMem - mem1) >> 20);
    QVERIFY2(peakQueued < server.caps().hardBytes + bigBytes, "queue bounded by the hard cap");
    QVERIFY2(peakMem - mem1 < server.caps().hardBytes + (qint64(24) << 20), "memory bounded by the hard cap, not by what was offered");
    ui.unpark();
    QTRY_COMPARE_WITH_TIMEOUT(ui.disconnects.load(), 1, 5000);
}

void TestIpcBackpressure::hungQtServerBoundsEngine() {
    // An engine (this thread) and a Supervisor whose event loop is stuck (the 10-25 hang; a real IpcTransport server
    // on a parked thread). The engine sees the backlog: previews and telemetry are skipped, its queue and the process
    // stay bounded, the link stays up, and every Normal message (as-run) arrives in order once the Supervisor runs.
    ParkedPeer sup(IpcTransport::ServerRole);
    const QString name = uniqueName("hungsup");
    bool ok = false;
    sup.run([&]() { ok = sup.t->startServer(name); });
    QVERIFY(ok);
    IpcTransport engine(IpcTransport::ClientRole);
    QVERIFY(engine.connectToServer(name));
    QTRY_COMPARE_WITH_TIMEOUT(sup.connections.load(), 1, 3000);

    sup.busyUsPerOrdered = 200;
    sup.park();
    const IpcProtocol::Message frame = previewMsg(100 * 1024, 3);
    const qint64 frameBytes = framedSize(frame);
    const qint64 mem0 = privateBytes();
    qint64 peakQueued = 0, peakMem = mem0; int sent = 0, refused = 0, asrun = 0, asrunRefused = 0;
    QTimer producer; producer.setInterval(10);
    connect(&producer, &QTimer::timeout, this, [&]() {
        for (int i = 0; i < 5; ++i) (engine.send(frame) ? sent : refused)++;
        if (!engine.send(seqMsg(IpcProtocol::MSG_ASRUN_LOG, asrun++, 500))) ++asrunRefused;
        peakQueued = qMax(peakQueued, engine.queuedBytes());
        peakMem = qMax(peakMem, privateBytes());
    });
    LagProbe probe;
    producer.start();
    QTest::qWait(1500);
    producer.stop();
    const qint64 lag = probe.lagMs();
    qInfo().noquote() << QString("hung Qt Supervisor: engine sent %1 previews, skipped %2, queue peak %3 KB, process private +%4 MB, loop lag %5 ms")
                             .arg(sent).arg(refused).arg(peakQueued / 1024).arg((peakMem - mem0) >> 20).arg(lag);
    QCOMPARE(asrunRefused, 0);   // Normal messages are never skipped
    QVERIFY2(refused > 0, "the engine must see the hung Supervisor's backlog and skip previews");
    QVERIFY2(peakQueued < engine.caps().softBytes + frameBytes + (qint64(256) << 10), "engine queue bounded by the soft cap (+ small Normal traffic)");
    QVERIFY2(peakMem - mem0 < (qint64(24) << 20), "neither side holds the offered 75 MB");
    QVERIFY2(lag < 50, qPrintable(QString("loop lag %1 ms").arg(lag)));
    QVERIFY(engine.isConnected());

    sup.unpark();
    QTRY_COMPARE_WITH_TIMEOUT(sup.ordered.load(), asrun, 10000);
    QCOMPARE(sup.outOfOrder.load(), 0);
    QVERIFY2(sup.worstTurnMs() < 50, qPrintable(QString("worst Supervisor loop turn while catching up %1 ms").arg(sup.worstTurnMs())));
    QTRY_COMPARE_WITH_TIMEOUT(engine.queuedBytes(), qint64(0), 5000);
    QVERIFY(engine.send(frame));   // caught up: previews flow again
    QVERIFY(engine.isConnected());
    QCOMPARE(sup.disconnects.load(), 0);
}

void TestIpcBackpressure::disconnectHandsOutWhatArrived() {
    // An engine that writes its last messages and exits at once: they are handed out before clientDisconnected, even
    // when one readyRead may hand out only one message (slice 0) and the rest was re-queued.
    IpcTransport server(IpcTransport::ServerRole);
    server.setReadSliceMs(0);
    const QString name = uniqueName("leave");
    QVERIFY(server.startServer(name));
    QSignalSpy conn(&server, &IpcTransport::clientConnected), msgs(&server, &IpcTransport::messageReceived);
    int atDisconnect = -1;
    connect(&server, &IpcTransport::clientDisconnected, this, [&]() { atDisconnect = msgs.count(); });
    HANDLE raw = openRawClient(name);
    QVERIFY(raw != INVALID_HANDLE_VALUE);
    QTRY_COMPARE_WITH_TIMEOUT(conn.count(), 1, 3000);
    const int n = 300;
    QByteArray bytes;
    for (int i = 0; i < n; ++i) { const QByteArray b = seqMsg(IpcProtocol::MSG_ASRUN_LOG, i, 100).toJson(); bytes += rawFrame(quint32(b.size()), b); }
    std::thread w = writeRaw(raw, bytes);
    w.join();
    CloseHandle(raw);   // gone before this thread's loop ran again
    QTRY_VERIFY_WITH_TIMEOUT(atDisconnect >= 0, 5000);
    QCOMPARE(atDisconnect, n);
    for (int i = 0; i < n; ++i) QCOMPARE(msgs.at(i).at(0).value<IpcProtocol::Message>().payload.value("seq").toInt(), i);
    QVERIFY(server.stats().value("readYields").toDouble() >= 1);
}

void TestIpcBackpressure::asyncConnectAbsentServerNeverBlocks() {
    IpcTransport client(IpcTransport::ClientRole);
    QSignalSpy failed(&client, &IpcTransport::connectFailed), state(&client, &IpcTransport::connectionStateChanged);
    LagProbe probe;
    QElapsedTimer t; t.start();
    client.connectToServerAsync(uniqueName("absent"), 3000);
    const qint64 callMs = t.elapsed();
    QVERIFY2(callMs < 20, qPrintable(QString("connectToServerAsync took %1 ms").arg(callMs)));
    QVERIFY(client.isConnecting());
    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 2000);
    QVERIFY(failed.at(0).at(0).toString().contains("not found"));
    QVERIFY(!client.isConnecting());
    QVERIFY(!client.isConnected());
    QCOMPARE(state.count(), 0);
    QVERIFY(!client.send(IpcProtocol::makeHeartbeat("perftest", 1, 0, true)));
    QVERIFY2(probe.lagMs() < 50, qPrintable(QString("loop lag %1 ms").arg(probe.lagMs())));
}

void TestIpcBackpressure::asyncConnectBusyServerNeverBlocks() {
    // A server whose loop is hung: its only pipe instance is taken and it never offers another one. Qt's blocking
    // connect sits in WaitNamedPipe (5 s per round) here; the async connect keeps the loop running and times out.
    const QString name = uniqueName("busy");
    HANDLE rs = createRawServer(name, 1);
    QVERIFY(rs != INVALID_HANDLE_VALUE);
    HANDLE occupant = openRawClient(name);
    QVERIFY(occupant != INVALID_HANDLE_VALUE);
    HANDLE probeOpen = openRawClient(name);
    QCOMPARE(probeOpen, INVALID_HANDLE_VALUE);
    QCOMPARE(GetLastError(), DWORD(ERROR_PIPE_BUSY));

    IpcTransport client(IpcTransport::ClientRole);
    QSignalSpy failed(&client, &IpcTransport::connectFailed), state(&client, &IpcTransport::connectionStateChanged);
    LagProbe probe;
    QElapsedTimer t; t.start();
    client.connectToServerAsync(name, 1500);
    QVERIFY(t.elapsed() < 20);
    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 5000);
    const qint64 took = t.elapsed();
    qInfo().noquote() << QString("busy server: connectFailed after %1 ms (\"%2\"), loop lag %3 ms")
                             .arg(took).arg(failed.at(0).at(0).toString()).arg(probe.lagMs());
    QVERIFY(took >= 1400 && took < 3500);
    QVERIFY(failed.at(0).at(0).toString().contains("busy"));
    QVERIFY2(probe.lagMs() < 50, qPrintable(QString("loop lag %1 ms").arg(probe.lagMs())));
    QCOMPARE(state.count(), 0);
    CloseHandle(occupant);
    CloseHandle(rs);
}

void TestIpcBackpressure::asyncConnectRoundTrip() {
    IpcTransport server(IpcTransport::ServerRole);
    const QString name = uniqueName("rt");
    QVERIFY(server.startServer(name));
    QSignalSpy conn(&server, &IpcTransport::clientConnected), disc(&server, &IpcTransport::clientDisconnected),
               srvMsgs(&server, &IpcTransport::messageReceived);
    IpcTransport client(IpcTransport::ClientRole);
    QSignalSpy state(&client, &IpcTransport::connectionStateChanged), cliMsgs(&client, &IpcTransport::messageReceived),
               failed(&client, &IpcTransport::connectFailed);
    client.connectToServerAsync(name, 2000);
    QTRY_COMPARE_WITH_TIMEOUT(state.count(), 1, 3000);
    QCOMPARE(state.at(0).at(0).toBool(), true);
    QVERIFY(client.isConnected());
    QVERIFY(!client.isConnecting());
    QCOMPARE(failed.count(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(conn.count(), 1, 3000);
    const QString id = conn.at(0).at(0).toString();

    QVERIFY(client.send(IpcProtocol::makeHeartbeat("perftest", 42, 3, true)));
    QVERIFY(client.send(IpcProtocol::makeAsRunLog("perftest", "START", "asset-1", 43)));
    QTRY_COMPARE_WITH_TIMEOUT(srvMsgs.count(), 2, 3000);
    QCOMPARE(srvMsgs.at(0).at(0).value<IpcProtocol::Message>().type, IpcProtocol::MSG_HEARTBEAT);
    QCOMPARE(srvMsgs.at(0).at(1).toString(), id);
    QCOMPARE(srvMsgs.at(1).at(0).value<IpcProtocol::Message>().payload.value("assetId").toString(), QString("asset-1"));

    QVERIFY(server.sendTo(id, IpcProtocol::makeTransportCommand("perftest", "skip", {})));
    server.broadcast(IpcProtocol::makeTimeSync(1, 2, 3));
    QTRY_COMPARE_WITH_TIMEOUT(cliMsgs.count(), 2, 3000);
    QCOMPARE(cliMsgs.at(0).at(0).value<IpcProtocol::Message>().payload.value("action").toString(), QString("skip"));
    QCOMPARE(cliMsgs.at(1).at(0).value<IpcProtocol::Message>().type, IpcProtocol::MSG_TIME_SYNC);
    QCOMPARE(cliMsgs.at(1).at(1).toString(), QString("server"));

    // The server going away is reported like any other disconnect.
    server.disconnect();
    QTRY_VERIFY_WITH_TIMEOUT(!client.isConnected(), 3000);
    QCOMPARE(state.last().at(0).toBool(), false);
}

void TestIpcBackpressure::asyncConnectCancelled() {
    // (a) cancelled while the worker waits on a busy pipe: no signal, no crash.
    const QString busy = uniqueName("cancel");
    HANDLE rs = createRawServer(busy, 1);
    HANDLE occupant = openRawClient(busy);
    QVERIFY(rs != INVALID_HANDLE_VALUE && occupant != INVALID_HANDLE_VALUE);
    {
        IpcTransport client(IpcTransport::ClientRole);
        QSignalSpy failed(&client, &IpcTransport::connectFailed), state(&client, &IpcTransport::connectionStateChanged);
        client.connectToServerAsync(busy, 3000);
        QVERIFY(client.isConnecting());
        client.disconnect();
        QVERIFY(!client.isConnecting());
        QTest::qWait(400);
        QCOMPARE(failed.count(), 0);
        QCOMPARE(state.count(), 0);
        auto* gone = new IpcTransport(IpcTransport::ClientRole);   // destroyed mid-attempt
        gone->connectToServerAsync(busy, 3000);
        delete gone;
        QTest::qWait(400);
    }
    CloseHandle(occupant);
    CloseHandle(rs);

    // (b) cancelled after the worker may already have opened a live server's pipe: the handle is closed, never leaked
    // (the server sees every such client leave again).
    IpcTransport server(IpcTransport::ServerRole);
    const QString name = uniqueName("cancel-live");
    QVERIFY(server.startServer(name));
    QSignalSpy conn(&server, &IpcTransport::clientConnected), disc(&server, &IpcTransport::clientDisconnected);
    for (int i = 0; i < 5; ++i) {
        IpcTransport client(IpcTransport::ClientRole);
        QSignalSpy state(&client, &IpcTransport::connectionStateChanged);
        client.connectToServerAsync(name, 2000);
        QThread::msleep(30);   // let the worker open the pipe before the cancel
        client.disconnect();
        QTest::qWait(50);
        QCOMPARE(state.count(), 0);
    }
    QTRY_COMPARE_WITH_TIMEOUT(disc.count(), conn.count(), 3000);
    QVERIFY(server.clientIds().isEmpty());

    // (c) a newer connect supersedes a pending one and wins.
    IpcTransport client(IpcTransport::ClientRole);
    QSignalSpy state(&client, &IpcTransport::connectionStateChanged), failed(&client, &IpcTransport::connectFailed);
    client.connectToServerAsync(uniqueName("absent"), 2000);
    client.connectToServerAsync(name, 2000);
    QTRY_VERIFY_WITH_TIMEOUT(client.isConnected(), 3000);
    QTest::qWait(100);
    QCOMPARE(failed.count(), 0);
    QCOMPARE(state.count(), 1);
}

void TestIpcBackpressure::broadcastAndSetSend() {
    IpcTransport server(IpcTransport::ServerRole);
    const QString name = uniqueName("fan");
    QVERIFY(server.startServer(name));
    QSignalSpy conn(&server, &IpcTransport::clientConnected);
    IpcTransport c1(IpcTransport::ClientRole), c2(IpcTransport::ClientRole), c3(IpcTransport::ClientRole);
    QSignalSpy m1(&c1, &IpcTransport::messageReceived), m2(&c2, &IpcTransport::messageReceived), m3(&c3, &IpcTransport::messageReceived);
    for (IpcTransport* c : {&c1, &c2, &c3}) c->connectToServerAsync(name, 2000);
    QTRY_COMPARE_WITH_TIMEOUT(conn.count(), 3, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(c1.isConnected() && c2.isConnected() && c3.isConnected(), 3000);

    server.broadcast(IpcProtocol::makeStateChange("perftest", "ON_AIR", "test"));
    QTRY_VERIFY_WITH_TIMEOUT(m1.count() == 1 && m2.count() == 1 && m3.count() == 1, 3000);

    // Which server-side id belongs to which client is not known here: send to two ids, exactly two clients get it.
    const QSet<QString> two = {conn.at(0).at(0).toString(), conn.at(2).at(0).toString()};
    QCOMPARE(server.sendTo(two, previewMsg(1000, 9), IpcTransport::Droppable), 2);
    QCOMPARE(server.sendTo(QSet<QString>{"nobody"}, previewMsg(10, 9)), 0);
    QTRY_COMPARE_WITH_TIMEOUT(m1.count() + m2.count() + m3.count(), 5, 3000);
    QTest::qWait(100);
    QCOMPARE(m1.count() + m2.count() + m3.count(), 5);
    QVERIFY(!server.sendTo(QString("nobody"), previewMsg(10, 9)));
}

QTEST_GUILESS_MAIN(TestIpcBackpressure)
#include "test_ipcbackpressure.moc"
