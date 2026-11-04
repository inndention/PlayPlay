#include <QtTest/QtTest>
#include <QBuffer>
#include <QElapsedTimer>
#include <QImageWriter>
#include <QJSEngine>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>
#include "playout/supervisorclient.h"
#include "playout/supervisorapi.h"
#include "playout/ui_ipc/ipctransport.h"

#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <mmsystem.h>
#endif

// perf-app PA-1 (W9): the app's IPC link runs on a worker thread, connects without blocking, flags a Supervisor that
// stops talking (3 s) and pauses the api.* GETs meanwhile. Every check that matters is "the GUI thread keeps turning":
// a 5 ms precise timer on the test's main thread (= the app's GUI thread) records how late each tick fires.
// Pipe names are unique to this process ("pptc-..."); HTTP fakes listen on ephemeral 127.0.0.1 ports. Nothing here
// touches the live IPC name, the live ports or a health port of a real Supervisor (healthPort is 0 or a fake).
// PA-4 (W12 client side): no periodic status query; what the link carries is counted (bytes by kind, queries by reason).
class TestSupervisorClient : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void coalesceKeys();
    void stallWhenServerNeverAnswers();     // (a)
    void highRateKeepsGuiResponsive();      // (b)
    void blockedGuiGetsCoalescedState();    // (b, GUI side busy) never a stall, state coalesced, events kept
    void absentServerNeverBlocks();         // (c)
    void busyServerIsStalledNotLaunched();  // (c, a Supervisor whose loop no longer accepts)
    void pausedApiHoldsGets();              // api.* polling paused while stalled
    void queriesOnlyWhenNeeded();           // PA-4: no 4 s poll; connect / stale / operator queries; runlists by broadcast
    void idleBytesTenMinutes();             // PA-4: bytes per minute with an idle schedule, before vs PS-6 vs PS-6 + PA-4
};

namespace {
QString uniqueName(const char* tag) {
    static int n = 0;
    return QString("pptc-%1-%2-%3").arg(tag).arg(QCoreApplication::applicationPid()).arg(++n);
}

// Lateness of every tick of a 5 ms timer on this thread: how long its event loop was blocked.
struct LagProbe {
    QTimer timer; QElapsedTimer clock; qint64 last = 0; std::vector<qint64> lags;
    LagProbe() {
        timer.setTimerType(Qt::PreciseTimer); timer.setInterval(5);
        QObject::connect(&timer, &QTimer::timeout, [this]() { const qint64 now = clock.elapsed(); lags.push_back(qMax<qint64>(0, now - last - 5)); last = now; });
        reset(); timer.start();
    }
    void reset() { clock.restart(); last = 0; lags.clear(); }
    qint64 worst() const { return lags.empty() ? 0 : *std::max_element(lags.begin(), lags.end()); }
    qint64 p99() const {
        if (lags.empty()) return 0;
        std::vector<qint64> v = lags; std::sort(v.begin(), v.end());
        return v[size_t(qMin<double>(v.size() - 1, v.size() * 0.99))];
    }
    QString summary() const { return QString("%1 ticks, p99 %2 ms, worst %3 ms").arg(lags.size()).arg(p99()).arg(worst()); }
};

SupervisorClient::Options opts(const QString& name) {
    SupervisorClient::Options o;
    o.ipcName = name;
    o.launchIfMissing = false;
    o.restPort = 1;        // never used for REST here
    o.healthPort = 0;      // no /healthz probe unless a test sets a fake one
    o.statusPollMs = 0;    // never from the environment unless a test asks for it
    o.executable = QDir::temp().filePath("pptc-no-such-dir/PlayPlaySupervisor.exe");
    return o;
}

// Bytes one message takes on the pipe (IpcTransport::frame: a 4-byte length + the compact JSON).
qint64 wire(const IpcProtocol::Message& m) { return 4 + m.toJson().size(); }

// A Supervisor stand-in: an IpcTransport server on its own thread, so what it sends never costs the client's GUI loop.
// bytesSent counts every byte it writes (to every connected client), so tests can hold the client's counter against it.
struct FakeSupervisor {
    QThread thread;
    QObject* ctx = nullptr;            // lives on thread
    IpcTransport* server = nullptr;    // lives on thread
    std::atomic<int> received{0}, queries{0}, commands{0}, clients{0};
    std::atomic<bool> streamDone{false};
    std::atomic<qint64> bytesSent{0};
    std::function<void(const QString& clientId)> onQuery;   // thread side; set with setQueryHandler

    bool start(const QString& name) {
        thread.setObjectName("fake-supervisor");
        thread.start();
        ctx = new QObject;
        ctx->moveToThread(&thread);
        bool ok = false;
        QMetaObject::invokeMethod(ctx, [&]() {
            server = new IpcTransport(IpcTransport::ServerRole, ctx);
            QObject::connect(server, &IpcTransport::messageReceived, ctx, [this](const IpcProtocol::Message& m, const QString& client) {
                ++received;
                if (m.type == IpcProtocol::MSG_UI_QUERY_STATUS) { ++queries; if (onQuery) onQuery(client); }
                else if (m.type == IpcProtocol::MSG_UI_COMMAND) ++commands;
            });
            QObject::connect(server, &IpcTransport::clientConnected, ctx, [this](const QString&) { ++clients; });
            ok = server->startServer(name);
        }, Qt::BlockingQueuedConnection);
        return ok;
    }
    void setQueryHandler(std::function<void(const QString&)> h) {
        QMetaObject::invokeMethod(ctx, [this, h]() { onQuery = h; }, Qt::BlockingQueuedConnection);
    }
    // Thread side: to every connected client / to one, never skipped, counted.
    void sendAll(const IpcProtocol::Message& m) { bytesSent += wire(m) * server->clientIds().size(); server->broadcast(m, IpcTransport::Normal); }
    void sendOne(const QString& client, const IpcProtocol::Message& m) { if (server->sendTo(client, m, IpcTransport::Normal)) bytesSent += wire(m); }
    void broadcast(const IpcProtocol::Message& m) {
        QMetaObject::invokeMethod(ctx, [this, m]() { sendAll(m); }, Qt::QueuedConnection);
    }
    // fn every ms on the fake's thread (a precise timer), until the fake goes.
    void every(int ms, std::function<void()> fn) {
        QMetaObject::invokeMethod(ctx, [this, ms, fn]() {
            auto* t = new QTimer(ctx); t->setTimerType(Qt::PreciseTimer); t->setInterval(ms);
            QObject::connect(t, &QTimer::timeout, ctx, fn);
            t->start();
        }, Qt::BlockingQueuedConnection);
    }
    // ratePerSec messages a second for durationMs (kept up by elapsed time, whatever the timer granularity).
    void stream(int ratePerSec, int durationMs, std::function<IpcProtocol::Message(int)> gen) {
        streamDone = false;
        QMetaObject::invokeMethod(ctx, [this, ratePerSec, durationMs, gen]() {
            auto* timer = new QTimer(ctx);
            timer->setTimerType(Qt::PreciseTimer);
            timer->setInterval(2);
            auto clock = std::make_shared<QElapsedTimer>(); clock->start();
            auto sent = std::make_shared<int>(0);
            const int total = ratePerSec * durationMs / 1000;
            QObject::connect(timer, &QTimer::timeout, ctx, [this, timer, clock, sent, total, ratePerSec, gen]() {
                const int due = int(qMin<qint64>(total, clock->elapsed() * ratePerSec / 1000 + 1));
                while (*sent < due) sendAll(gen((*sent)++));   // never skipped: counts are exact
                if (*sent >= total) { timer->stop(); timer->deleteLater(); streamDone = true; }
            });
            timer->start();
        }, Qt::QueuedConnection);
    }
    // Thread side: no more timers, no more answers (a test's handler may refer to its own locals).
    void quiet() { qDeleteAll(ctx->findChildren<QTimer*>()); onQuery = nullptr; }
    void silence() { QMetaObject::invokeMethod(ctx, [this]() { quiet(); }, Qt::BlockingQueuedConnection); }
    ~FakeSupervisor() {
        if (ctx) {
            QMetaObject::invokeMethod(ctx, [this]() { quiet(); delete server; server = nullptr; }, Qt::BlockingQueuedConnection);
            ctx->deleteLater();   // deleted when the thread finishes
        }
        thread.quit();
        thread.wait();
    }
};

// Tiny HTTP/1.1 server: answers every request with {"path": "...", "n": <count>} (or a fixed body), Connection: close.
struct FakeHttp : QObject {
    QTcpServer server;
    QStringList hits;   // "GET /api/v1/status"
    QByteArray fixedBody;
    FakeHttp() {
        server.listen(QHostAddress::LocalHost, 0);
        QObject::connect(&server, &QTcpServer::newConnection, this, [this]() {
            while (QTcpSocket* s = server.nextPendingConnection()) {
                auto buf = std::make_shared<QByteArray>();
                QObject::connect(s, &QTcpSocket::readyRead, s, [this, s, buf]() {
                    buf->append(s->readAll());
                    const int end = int(buf->indexOf("\r\n\r\n"));
                    if (end < 0) return;
                    const QList<QByteArray> lines = buf->left(end).split('\n');
                    const QList<QByteArray> req = lines.value(0).trimmed().split(' ');
                    int len = 0;
                    for (const QByteArray& l : lines) if (l.toLower().startsWith("content-length:")) len = l.mid(15).trimmed().toInt();
                    if (buf->size() < end + 4 + len) return;
                    const QString line = QString::fromLatin1(req.value(0) + ' ' + req.value(1));
                    hits << line;
                    const QByteArray body = fixedBody.isEmpty()
                        ? QJsonDocument(QJsonObject{{"path", QString::fromLatin1(req.value(1))}, {"n", int(hits.size())}}).toJson(QJsonDocument::Compact)
                        : fixedBody;
                    s->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                    s->disconnectFromHost();
                    buf->clear();
                });
                QObject::connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
            }
        });
    }
    quint16 port() const { return server.serverPort(); }
    int count(const QString& prefix) const { return int(std::count_if(hits.begin(), hits.end(), [&](const QString& h) { return h.startsWith(prefix); })); }
};

// The message mix of a busy station, deterministic by index (100 msgs/s at the default rate):
// every 5th a preview (alternating channels), every 50th (+1) an alert, every 100th (+2) a snapshot, every 25th (+3)
// time sync, the rest telemetry alternating channels.
struct Mix {
    QString b64;   // one real 1280x720 JPEG
    enum Kind { Preview, Alert, Snapshot, TimeSync, Telemetry };
    static Kind kind(int i) {
        if (i % 5 == 0) return Preview;
        if (i % 50 == 1) return Alert;
        if (i % 100 == 2) return Snapshot;
        if (i % 25 == 3) return TimeSync;
        return Telemetry;
    }
    static QString chan(int i) { return (i / 5) % 2 ? QStringLiteral("perftest-b") : QStringLiteral("perftest-a"); }
    IpcProtocol::Message operator()(int i) const {
        using namespace IpcProtocol;
        switch (kind(i)) {
        case Preview: return make(MSG_PREVIEW_FRAME, chan(i), QJsonObject{{"jpegBase64", b64}, {"frame", i}, {"width", 1280}, {"height", 720},
                                                                          {"meters", QJsonObject{{"L", -12.0 - (i % 7)}, {"R", -13.0}}}});
        case Alert: return make(MSG_UI_ALERT, QString(), QJsonObject{{"level", "info"}, {"message", QString("alert %1").arg(i)}});
        case Snapshot: return make(MSG_UI_STATUS, QString(), QJsonObject{{"seq", i}, {"channels", QJsonArray{QJsonObject{{"profile", QJsonObject{{"id", "perftest-a"}}}},
                                                                                                          QJsonObject{{"profile", QJsonObject{{"id", "perftest-b"}}}}}}});
        case TimeSync: return makeTimeSync(i, QDateTime::currentMSecsSinceEpoch() * 1000000LL, 0);
        case Telemetry: default: return make(MSG_UI_TELEMETRY, i % 2 ? QStringLiteral("perftest-b") : QStringLiteral("perftest-a"),
                                             QJsonObject{{"frame", i}, {"state", "ON_AIR"}, {"fps", 29.97}});
        }
    }
    // What the client must end up with after n messages.
    struct Expect { int lastTelA = -1, lastTelB = -1, alerts = 0, lastAlert = -1, timeSyncs = 0, previews = 0, lastSnap = -1; };
    static Expect expect(int n) {
        Expect e;
        for (int i = 0; i < n; ++i) switch (kind(i)) {
            case Preview: ++e.previews; break;
            case Alert: ++e.alerts; e.lastAlert = i; break;
            case Snapshot: e.lastSnap = i; break;
            case TimeSync: ++e.timeSyncs; break;
            case Telemetry: (i % 2 ? e.lastTelB : e.lastTelA) = i; break;
        }
        return e;
    }
};

// PA-4: what the Supervisor sends a UI with an idle schedule, shaped like the real thing (two channels; snapshot about
// 4 KB; a runlist of two hours of 40 items each when engines are on air, empty without engines; telemetry about 0.6 KB).
const QStringList kSimChannels = {QStringLiteral("perftest-a"), QStringLiteral("perftest-b")};

QJsonObject simSnapshot(qint64 utcMs) {
    QJsonArray chans;
    for (const QString& id : kSimChannels) {
        const QJsonObject profile{{"id", id}, {"name", "Channel " + id}, {"callLetters", "WPTT"}, {"frameRate", 29.97}, {"width", 1920}, {"height", 1080},
                                  {"audioChannels", 2}, {"prerollSec", 3}, {"armed", true}, {"strict", false}, {"timezone", "America/New_York"},
                                  {"outputs", QJsonArray{QJsonObject{{"type", "ndi"}, {"name", "perftest-" + id}, {"enabled", true}},
                                                         QJsonObject{{"type", "udp"}, {"url", "udp://127.0.0.1:50000?pkt_size=1316"}, {"bitrate", 8000}, {"enabled", false}}}},
                                  {"fill", QJsonObject{{"mode", "loop"}, {"maxFillSec", 600}, {"emergencyPath", "D:/Media/Emergency/stand_by_loop.mp4"}}},
                                  {"identity", QJsonObject{{"brandName", "Perf Test TV"}, {"slateCaptions", QJsonArray{"PERF TEST", "STAND BY"}}}}};
        const QJsonObject executor{{"armed", true}, {"connected", true}, {"emergency", false}, {"currentSlotId", "2026-10-26T14"}, {"hourStartMs", 1761487200000.0},
                                   {"lastPushMs", 1761487201000.0}, {"lastPushReason", "hour_changed"}, {"pushedItems", 40}, {"engineState", "ON_AIR"},
                                   {"onAirRunlistId", "rl-14-03"}, {"onAirTitle", "Programme 3"}, {"driftSec", 0}};
        const QJsonObject process{{"running", true}, {"pid", 4242}, {"status", "running"}, {"connected", true}, {"lastHeartbeatMs", 1761487300000.0}, {"restarts", 0}};
        chans.append(QJsonObject{{"profile", profile}, {"executor", executor}, {"process", process}, {"engineState", "ON_AIR"}, {"hasPreview", true}});
    }
    QJsonArray alerts;
    for (int i = 0; i < 12; ++i)
        alerts.append(QJsonObject{{"channelId", kSimChannels[i % 2]}, {"level", "info"}, {"message", QString("engine connected (start %1)").arg(i)}, {"utcMs", 1761487000000.0 + i}});
    return QJsonObject{{"utcMs", double(utcMs)}, {"uptimeMs", 1234567.0}, {"channels", chans}, {"alerts", alerts},
                       {"health", QJsonObject{{"cpu", 12.5}, {"memMb", 512}, {"disks", QJsonArray{QJsonObject{{"path", "D:/"}, {"freeGb", 812.4}}}}}},
                       {"ntp", QJsonObject{{"offsetMs", 1.5}, {"server", "pool.ntp.org"}}}, {"mux", QJsonObject{{"state", "stopped"}, {"services", QJsonArray()}}}};
}

QJsonObject simHour(const QString& ch, qint64 hourStartMs, int items, const QString& tag) {
    QJsonArray a;
    for (int i = 0; i < items; ++i) {
        const qint64 s = hourStartMs + qint64(i) * 90000;
        a.append(QJsonObject{{"runlistId", QString("rl-%1-%2-%3").arg(ch).arg(hourStartMs / 3600000).arg(i)}, {"title", QString("%1 programme %2").arg(tag).arg(i)},
                             {"path", QString("D:/Media/Programmes/%1/show_%2.mp4").arg(ch).arg(i)}, {"mediaId", QString("m-%1-%2").arg(ch).arg(i)},
                             {"plannedStartMs", double(s)}, {"plannedEndMs", double(s + 90000)}, {"inSec", 0}, {"outSec", 90}, {"duration", 90.0},
                             {"origin", "slot"}, {"type", "file"}, {"category", "programme"}, {"notes", QJsonArray()},
                             {"cues", QJsonArray{QJsonObject{{"atSec", 30}, {"cg", "lower-third"}, {"text", "Perf test caption"}}}}});
    }
    return QJsonObject{{"hourStartMs", double(hourStartMs)}, {"slotId", QString("2026-10-26T%1").arg(hourStartMs / 3600000 % 24)}, {"items", a}, {"notes", QJsonArray()}};
}

QJsonObject simRunlist(const QString& ch, int itemsPerHour, const QString& tag = QStringLiteral("Plan")) {
    const qint64 h = 1761487200000LL;
    return QJsonObject{{"channelId", ch}, {"current", simHour(ch, h, itemsPerHour, tag)}, {"next", simHour(ch, h + 3600000, itemsPerHour, tag)},
                       {"status", QJsonObject{{"channelId", ch}, {"armed", true}, {"connected", itemsPerHour > 0}, {"emergency", false}, {"pushedItems", 2 * itemsPerHour}}}};
}

QJsonObject simTelemetry(const QString& ch, int i) {
    return QJsonObject{{"state", "ON_AIR"}, {"itemId", "rl-14-03"}, {"title", "Plan programme 3"}, {"position", 12.5 + i}, {"duration", 90.0}, {"frame", 100000 + i * 30},
                       {"fps", 29.97}, {"droppedFrames", 0}, {"bufferDepth", 6}, {"channelId", ch},
                       {"outputs", QJsonArray{QJsonObject{{"name", "ndi"}, {"state", "running"}, {"bitrateKbps", 0}}, QJsonObject{{"name", "udp"}, {"state", "running"}, {"bitrateKbps", 8000}}}},
                       {"audio", QJsonObject{{"lufs", -23.4}, {"truePeak", -2.1}, {"channels", 2}}}};
}

// The Supervisor's UI wiring with an idle schedule, `speed` times faster than real time:
//  - ps6 = false (before PS-6): the snapshot to every UI every 2 s; a status query answered with the snapshot, every
//    channel's telemetry and every runlist.
//  - ps6 = true: the snapshot when it changes (never here) and every 10 s; a client's first query gets snapshot +
//    telemetry + runlists, a repeated one the snapshot and only runlists not sent to it since (none: nothing changes).
//  - time sync to every client at 1 Hz; engine telemetry to UIs at 1 Hz per channel when onAir.
struct SimConfig { bool ps6 = true; bool onAir = false; int speed = 40; };
void simulate(FakeSupervisor& f, const SimConfig& cfg) {
    struct State { QSet<QString> ui; bool published = false; qint64 lastPub = 0; QElapsedTimer clock; int tel = 0; };
    auto st = std::make_shared<State>(); st->clock.start();
    const int items = cfg.onAir ? 40 : 0;
    const QJsonObject rlA = simRunlist(kSimChannels[0], items), rlB = simRunlist(kSimChannels[1], items);
    using namespace IpcProtocol;
    auto snapshot = []() { return make(MSG_UI_STATUS, QString(), simSnapshot(1761487300000LL)); };
    f.setQueryHandler([&f, st, cfg, rlA, rlB, snapshot](const QString& client) {
        const bool first = !st->ui.contains(client);
        st->ui.insert(client);
        f.sendOne(client, snapshot());
        if (cfg.ps6 && !first) return;   // repeated query: no runlist changed since this client got them
        if (cfg.onAir) for (const QString& ch : kSimChannels) f.sendOne(client, make(MSG_UI_TELEMETRY, ch, simTelemetry(ch, st->tel)));
        f.sendOne(client, make(MSG_UI_RUNLIST, kSimChannels[0], rlA));
        f.sendOne(client, make(MSG_UI_RUNLIST, kSimChannels[1], rlB));
    });
    const int tickMs = 2000 / cfg.speed, keepaliveMs = 10000 / cfg.speed, secMs = 1000 / cfg.speed;
    f.every(tickMs, [&f, st, cfg, tickMs, keepaliveMs, snapshot]() {
        if (st->ui.isEmpty()) return;
        const qint64 now = st->clock.elapsed();
        if (cfg.ps6 && st->published && now - st->lastPub < keepaliveMs - tickMs / 2) return;   // StatePublisher's keepalive rule
        st->published = true; st->lastPub = now;
        const IpcProtocol::Message m = snapshot();
        for (const QString& c : std::as_const(st->ui)) f.sendOne(c, m);
    });
    f.every(secMs, [&f]() { f.sendAll(makeTimeSync(0, QDateTime::currentMSecsSinceEpoch() * 1000000LL, 0)); });
    if (cfg.onAir) f.every(secMs, [&f, st]() {
        ++st->tel;
        for (const QString& ch : kSimChannels) { const IpcProtocol::Message m = make(MSG_UI_TELEMETRY, ch, simTelemetry(ch, st->tel)); for (const QString& c : std::as_const(st->ui)) f.sendOne(c, m); }
    });
}

// A picture with gradients, a checker and grain, so the JPEG has a realistic size (no QPainter text: no QGuiApplication).
QString makeJpegBase64(int w, int h) {
    QImage img(w, h, QImage::Format_RGB32);
    quint32 seed = 12345;
    for (int y = 0; y < h; ++y) {
        QRgb* line = reinterpret_cast<QRgb*>(img.scanLine(y));
        for (int x = 0; x < w; ++x) {
            seed = seed * 1664525u + 1013904223u;
            const int n = int(seed >> 27);   // 0..31 of grain
            line[x] = qRgb(qBound(0, 20 + x * 200 / w + n, 255), qBound(0, 40 + y * 150 / h + n, 255), qBound(0, 90 + ((x / 40 + y / 40) % 2) * 80 + n, 255));
        }
    }
    QByteArray jpeg; QBuffer b(&jpeg); b.open(QIODevice::WriteOnly);
    if (!img.save(&b, "JPEG", 80)) return {};
    return QString::fromLatin1(jpeg.toBase64());
}

#ifdef Q_OS_WIN
std::atomic<int> g_launchWarnings{0};
QtMessageHandler g_prevHandler = nullptr;
void countingHandler(QtMsgType t, const QMessageLogContext& c, const QString& m) {
    if (m.contains("executable not found") || m.contains("] launched")) ++g_launchWarnings;
    if (g_prevHandler) g_prevHandler(t, c, m);
}
#endif
} // namespace

void TestSupervisorClient::initTestCase() {
    QCoreApplication::setOrganizationName("PlayPlayLaneTest");   // SupervisorApi reads QSettings: never the app's own
    QCoreApplication::setApplicationName("test_supervisorclient");
#ifdef Q_OS_WIN
    timeBeginPeriod(1);   // 1 ms timer resolution, so the lag probe measures the loop and not the 15.6 ms tick
#endif
}

void TestSupervisorClient::cleanupTestCase() {
#ifdef Q_OS_WIN
    timeEndPeriod(1);
#endif
}

void TestSupervisorClient::coalesceKeys() {
    using namespace IpcProtocol;
    using W = SupervisorClientWorker;
    QCOMPARE(W::coalesceKey(make(MSG_PREVIEW_FRAME, "main", {})), QString("pv:main"));
    QCOMPARE(W::coalesceKey(make(MSG_UI_TELEMETRY, "main", QJsonObject{{"frame", 1}, {"state", "ON_AIR"}})), QString("tel:main"));
    QCOMPARE(W::coalesceKey(make(MSG_UI_TELEMETRY, "main", QJsonObject{{"state", "ERROR"}, {"reason", "x"}})), QString());   // state-change event
    QCOMPARE(W::coalesceKey(make(MSG_UI_STATUS, QString(), {})), QString("snap"));
    QCOMPARE(W::coalesceKey(make(MSG_UI_STATUS, "ch2", {})), QString("st:ch2"));
    QCOMPARE(W::coalesceKey(make(MSG_UI_RUNLIST, "ch2", {})), QString("rl:ch2"));
    QCOMPARE(W::coalesceKey(make(MSG_UI_ALERT, "ch2", {})), QString());
    QCOMPARE(W::coalesceKey(make(MSG_UI_ASRUN, "ch2", {})), QString());
    QCOMPARE(W::coalesceKey(make(MSG_UI_SCHEDULE, "ch2", {})), QString());
}

// (a) The pipe connects, the server reads our query and never says anything: stalled ~3 s after the connect, the GUI
// loop never blocks, no query goes out while stalled, /healthz details arrive, the first message clears the stall
// and brings one fresh snapshot request, and stop() is immediate.
void TestSupervisorClient::stallWhenServerNeverAnswers() {
    FakeSupervisor fake;
    const QString name = uniqueName("silent");
    QVERIFY(fake.start(name));
    FakeHttp health;
    health.fixedBody = R"({"name":"supervisor","state":"stall","activity":"REST GET /api/v1/library","lagMs":7200})";
    QVERIFY(health.port() > 0);

    SupervisorClient c;
    QElapsedTimer t; t.start();
    qint64 connectedAt = -1, stalledAt = -1, recoveredAt = -1;
    connect(&c, &SupervisorClient::connectedChanged, &c, [&]() { if (c.isConnected() && connectedAt < 0) connectedAt = t.elapsed(); });
    connect(&c, &SupervisorClient::supervisorStalledChanged, &c, [&]() { (c.supervisorStalled() ? stalledAt : recoveredAt) = t.elapsed(); });
    LagProbe probe;
    SupervisorClient::Options o = opts(name);
    o.healthPort = health.port();
    t.restart(); probe.reset();
    c.start(o);
    const qint64 startCall = t.elapsed();
    QVERIFY2(startCall < 20, qPrintable(QString("start() took %1 ms").arg(startCall)));
    QTRY_VERIFY_WITH_TIMEOUT(connectedAt >= 0, 2000);
    QTRY_COMPARE_WITH_TIMEOUT(fake.queries.load(), 1, 1000);
    QTRY_VERIFY_WITH_TIMEOUT(stalledAt >= 0, 5000);
    const qint64 silentFor = stalledAt - connectedAt;
    qInfo().noquote() << QString("silent server: connected after %1 ms, stalled %2 ms after the connect; GUI loop %3").arg(connectedAt).arg(silentFor).arg(probe.summary());
    QVERIFY2(silentFor >= 2900 && silentFor <= 3500, qPrintable(QString("stalled after %1 ms").arg(silentFor)));
    QVERIFY(c.isConnected());
    QCOMPARE(c.stallInfo().value("reason").toString(), QString("silent"));
    QVERIFY(c.stallInfo().value("silentMs").toLongLong() >= 2900);
    QTRY_COMPARE_WITH_TIMEOUT(c.stallInfo().value("health").toMap().value("activity").toString(), QString("REST GET /api/v1/library"), 2000);
    QCOMPARE(c.stallInfo().value("health").toMap().value("lagMs").toInt(), 7200);
    QCOMPARE(health.count("GET /healthz"), 1);   // one probe in flight at a time, every 2 s

    // Past the old 4 s poll mark: nothing went out while stalled (and since PA-4 there is no poll at all).
    QTest::qWait(qMax<qint64>(0, connectedAt + 5000 - t.elapsed()));
    QCOMPARE(fake.queries.load(), 1);
    QVERIFY2(probe.worst() < 100, qPrintable(probe.summary()));

    // Recovery: any message clears the stall at once; one status query asks for what changed meanwhile.
    fake.broadcast(IpcProtocol::make(IpcProtocol::MSG_UI_ALERT, QString(), QJsonObject{{"level", "warning"}, {"message", "back"}}));
    QTRY_VERIFY_WITH_TIMEOUT(recoveredAt >= 0, 1000);
    QVERIFY(!c.supervisorStalled());
    QVERIFY(c.stallInfo().isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(fake.queries.load(), 2, 1000);
    QCOMPARE(c.queriesSent(SupervisorClient::QueryConnect), quint64(1));
    QCOMPARE(c.queriesSent(SupervisorClient::QueryRecover), quint64(1));
    QCOMPARE(c.queriesSent(), quint64(2));
    QCOMPARE(c.alertsList().size(), 1);
    QCOMPARE(c.alertsList().first().toMap().value("message").toString(), QString("back"));

    // Commands still go out (operator actions); stop() is immediate.
    c.play("perftest");
    QTRY_COMPARE_WITH_TIMEOUT(fake.commands.load(), 1, 1000);
    QElapsedTimer s; s.start();
    c.stop();
    QVERIFY2(s.elapsed() < 500, qPrintable(QString("stop() took %1 ms").arg(s.elapsed())));
    QVERIFY2(probe.worst() < 100, qPrintable(probe.summary()));
}

// (b) 100 messages a second of the real mix (20 previews/s as 1280x720 JPEGs): the GUI loop's p99 lateness stays
// under one 60 Hz frame. Everything arrives: latest telemetry per channel, every alert in order, decoded previews.
void TestSupervisorClient::highRateKeepsGuiResponsive() {
    Mix mix; mix.b64 = makeJpegBase64(1280, 720);
    if (mix.b64.isEmpty()) QSKIP("no JPEG image plugin (set QT_PLUGIN_PATH to the Qt plugins folder)");
    FakeSupervisor fake;
    const QString name = uniqueName("rate");
    QVERIFY(fake.start(name));
    SupervisorClient c;
    QSignalSpy stalled(&c, &SupervisorClient::supervisorStalledChanged);
    c.start(opts(name));
    QTRY_VERIFY_WITH_TIMEOUT(c.isConnected(), 2000);

    const int rate = 100, ms = 3000, n = rate * ms / 1000;
    LagProbe probe;
    fake.stream(rate, ms, mix);
    QTRY_VERIFY_WITH_TIMEOUT(fake.streamDone.load(), ms + 2000);
    const Mix::Expect e = Mix::expect(n);
    QTRY_COMPARE_WITH_TIMEOUT(c.telemetry("perftest-a").value("frame").toInt(), e.lastTelA, 2000);
    QTRY_COMPARE_WITH_TIMEOUT(c.telemetry("perftest-b").value("frame").toInt(), e.lastTelB, 2000);
    QTRY_COMPARE_WITH_TIMEOUT(c.alertsList().size(), e.alerts, 2000);
    const QString gui = probe.summary();
    const SupervisorClientWorker::Stats st = c.ipcStats();
    qInfo().noquote() << QString("100 msg/s for 3 s: GUI loop %1; worker received %2 (time sync %3 kept off the GUI), queued %4, coalesced %5, "
                                 "GUI applied %6, previews shown %7, inbox peak %8")
                             .arg(gui).arg(st.received).arg(st.timeSyncs).arg(st.queued).arg(st.coalesced).arg(c.messagesApplied()).arg(c.previewSerial()).arg(st.inboxPeak);
    QVERIFY2(probe.p99() < 16, qPrintable(gui));
    QCOMPARE(c.alertsList().first().toMap().value("message").toString(), QString("alert %1").arg(e.lastAlert));   // newest first
    QCOMPARE(c.alertsList().last().toMap().value("message").toString(), QString("alert 1"));
    QCOMPARE(c.snapshot().value("seq").toInt(), e.lastSnap);
    QCOMPARE(c.channelIds(), QStringList({"perftest-a", "perftest-b"}));
    QVERIFY(c.previewSerial() > 0);
    QCOMPARE(c.preview("perftest-a").size(), QSize(1280, 720));
    QCOMPARE(c.preview("perftest-b").size(), QSize(1280, 720));
    QVERIFY(c.metersOf("perftest-a").contains("L"));
    QCOMPARE(int(st.received), n);
    QCOMPARE(int(st.timeSyncs), e.timeSyncs);
    QCOMPARE(int(st.badPreviews), 0);
    QCOMPARE(int(st.binaryPreviews), 0);   // the fake sends the JSON/base64 shape (Message without bytes); PA-2's binary path is test_previewdecode
    // PA-4 byte counter: what the client counted (pictures by base64 length) against what the fake wrote on the pipe.
    const qint64 sent = fake.bytesSent.load();
    qInfo().noquote() << QString("byte counter: client %1, pipe %2 (%3 %), previews %4, telemetry %5, snapshots %6, time sync %7")
                             .arg(st.bytes).arg(sent).arg(100.0 * (double(st.bytes) - double(sent)) / double(sent), 0, 'f', 4)
                             .arg(st.previewBytes).arg(st.telemetryBytes).arg(st.snapshotBytes).arg(st.timeSyncBytes);
    QVERIFY2(std::abs(double(st.bytes) - double(sent)) <= 0.001 * double(sent), "the client's byte count is off by more than 0.1 %");
    QCOMPARE(int(st.snapshots), e.lastSnap / 100 + 1);
    QVERIFY(c.messagesApplied() <= quint64(n - e.timeSyncs));
    QCOMPARE(stalled.count(), 0);
    c.stop();
}

// The GUI thread is busy for 1.5 s while 100 msg/s keep coming: that is not a Supervisor stall (the worker hears it),
// state is coalesced to the latest (a handful of entries wait, not 150 messages and 30 decoded pictures), every
// event still arrives in order, and catching up does not block the loop.
void TestSupervisorClient::blockedGuiGetsCoalescedState() {
    Mix mix; mix.b64 = makeJpegBase64(1280, 720);
    if (mix.b64.isEmpty()) QSKIP("no JPEG image plugin (set QT_PLUGIN_PATH to the Qt plugins folder)");
    FakeSupervisor fake;
    const QString name = uniqueName("busygui");
    QVERIFY(fake.start(name));
    SupervisorClient c;
    QSignalSpy stalled(&c, &SupervisorClient::supervisorStalledChanged);
    c.start(opts(name));
    QTRY_VERIFY_WITH_TIMEOUT(c.isConnected(), 2000);

    const int rate = 100, ms = 2500, n = rate * ms / 1000;
    fake.stream(rate, ms, mix);
    QTest::qWait(500);
    const quint64 appliedBefore = c.messagesApplied();
    QThread::msleep(1500);   // the GUI thread does something slow
    LagProbe probe;          // from here: catching up must not block
    QTRY_VERIFY_WITH_TIMEOUT(fake.streamDone.load(), ms + 2000);
    const Mix::Expect e = Mix::expect(n);
    QTRY_COMPARE_WITH_TIMEOUT(c.telemetry("perftest-a").value("frame").toInt(), e.lastTelA, 2000);
    QTRY_COMPARE_WITH_TIMEOUT(c.telemetry("perftest-b").value("frame").toInt(), e.lastTelB, 2000);
    QTRY_COMPARE_WITH_TIMEOUT(c.alertsList().size(), e.alerts, 2000);
    const SupervisorClientWorker::Stats st = c.ipcStats();
    qInfo().noquote() << QString("GUI blocked 1.5 s at 100 msg/s: worker received %1, coalesced %2, inbox peak %3, GUI applied %4 of %5 non-time-sync (before the block %6); catch-up loop %7")
                             .arg(st.received).arg(st.coalesced).arg(st.inboxPeak).arg(c.messagesApplied()).arg(n - e.timeSyncs).arg(appliedBefore).arg(probe.summary());
    QCOMPARE(stalled.count(), 0);
    QVERIFY(st.coalesced > 50);
    QVERIFY2(st.inboxPeak < 50, qPrintable(QString("inbox peak %1").arg(st.inboxPeak)));
    QVERIFY2(probe.worst() < 50, qPrintable(probe.summary()));
    for (int i = 0; i < c.alertsList().size() - 1; ++i) {   // newest first, strictly decreasing: none lost or reordered
        const int a = c.alertsList()[i].toMap().value("message").toString().mid(6).toInt();
        const int b = c.alertsList()[i + 1].toMap().value("message").toString().mid(6).toInt();
        QVERIFY2(a == b + 50, qPrintable(QString("alerts out of order: %1 then %2").arg(a).arg(b)));
    }
    QCOMPARE(c.preview("perftest-a").size(), QSize(1280, 720));
    c.stop();
}

// (c) No Supervisor at all: start() returns at once, the GUI never blocks across the 2 s reconnect retry, nothing is
// "stalled" (absent is not hung), and a launch attempt (missing executable here) does not block either.
void TestSupervisorClient::absentServerNeverBlocks() {
    SupervisorClient c;
    QSignalSpy stalled(&c, &SupervisorClient::supervisorStalledChanged), conn(&c, &SupervisorClient::connectedChanged);
    SupervisorClient::Options o = opts(uniqueName("absent"));
    o.launchIfMissing = true;   // executable does not exist: logged, nothing started
    LagProbe probe;
    QElapsedTimer t; t.start();
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression("supervisor executable not found"));
    c.start(o);
    const qint64 startCall = t.elapsed();
    QTest::qWait(2600);
    qInfo().noquote() << QString("absent server: start() %1 ms; GUI loop %2").arg(startCall).arg(probe.summary());
    QVERIFY2(startCall < 20, qPrintable(QString("start() took %1 ms").arg(startCall)));
    QVERIFY2(probe.worst() < 50, qPrintable(probe.summary()));
    QVERIFY(!c.isConnected());
    QVERIFY(!c.supervisorStalled());
    QVERIFY(!c.launchedByUs());
    QCOMPARE(stalled.count(), 0);
    QElapsedTimer s; s.start();
    c.stop();
    QVERIFY2(s.elapsed() < 200, qPrintable(QString("stop() took %1 ms").arg(s.elapsed())));
}

// A Supervisor whose loop no longer accepts: the pipe exists but its only instance stays busy. The connect gives up
// after its budget on the helper thread (GUI never waits), the client reports "stalled (busy)" instead of starting
// a second Supervisor, and connects normally once a working server is there.
void TestSupervisorClient::busyServerIsStalledNotLaunched() {
#ifndef Q_OS_WIN
    QSKIP("raw named pipes are Windows-only");
#else
    const QString name = uniqueName("busy");
    const std::wstring path = (QStringLiteral("\\\\.\\pipe\\") + name).toStdWString();
    HANDLE srv = CreateNamedPipeW(path.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 0, 0, 0, nullptr);
    QVERIFY(srv != INVALID_HANDLE_VALUE);
    HANDLE occupant = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    QVERIFY(occupant != INVALID_HANDLE_VALUE);

    g_launchWarnings = 0;
    g_prevHandler = qInstallMessageHandler(countingHandler);
    SupervisorClient c;
    SupervisorClient::Options o = opts(name);
    o.launchIfMissing = true;
    o.connectTimeoutMs = 1000;
    LagProbe probe;
    QElapsedTimer t; t.start();
    c.start(o);
    QTRY_VERIFY_WITH_TIMEOUT(c.supervisorStalled(), 3000);
    const qint64 at = t.elapsed();
    QCOMPARE(c.stallInfo().value("reason").toString(), QString("busy"));
    QVERIFY(!c.isConnected());
    QTest::qWait(2500);   // another round of the reconnect timer: still busy, still no launch
    qInstallMessageHandler(g_prevHandler);
    qInfo().noquote() << QString("busy pipe: stalled (busy) after %1 ms; GUI loop %2; launch attempts %3").arg(at).arg(probe.summary()).arg(g_launchWarnings.load());
    QVERIFY(c.supervisorStalled());
    QCOMPARE(g_launchWarnings.load(), 0);
    QVERIFY2(probe.worst() < 50, qPrintable(probe.summary()));

    // The hung one goes away and a working one takes the name: connected, no longer stalled.
    CloseHandle(occupant);
    CloseHandle(srv);
    FakeSupervisor fake;
    QVERIFY(fake.start(name));
    QTRY_VERIFY_WITH_TIMEOUT(c.isConnected(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!c.supervisorStalled(), 1000);
    QVERIFY(c.stallInfo().isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(fake.queries.load(), 1, 1000);
    c.stop();
#endif
}

// While paused (stalled), api.get is held, one request per path, newest 8 callbacks per path; writes go out; on
// resume each held path is fetched once and every held caller gets the answer. shutdown() drops held callbacks.
void TestSupervisorClient::pausedApiHoldsGets() {
    FakeHttp http;
    QVERIFY(http.port() > 0);
    QJSEngine eng;
    auto* api = new SupervisorApi;
    QJSEngine::setObjectOwnership(api, QJSEngine::CppOwnership);
    eng.globalObject().setProperty("api", eng.newQObject(api));   // associates api with the engine (callbacks need it)
    eng.globalObject().setProperty("results", eng.newArray());
    const QJSValue factory = eng.evaluate("(function(tag) { return function(ok, d, s) { results.push(tag + ':' + ok + ':' + s + ':' + (d ? d.path : '')); }; })");
    QVERIFY(factory.isCallable());
    auto cb = [&](const QString& tag) { return factory.call({QJSValue(tag)}); };
    auto results = [&]() { QStringList r; const QJSValue a = eng.globalObject().property("results"); const int n = a.property("length").toInt(); for (int i = 0; i < n; ++i) r << a.property(quint32(i)).toString(); return r; };
    api->setBaseUrl(QString("http://127.0.0.1:%1").arg(http.port()));

    api->get("/status", cb("x"));
    QTRY_COMPARE_WITH_TIMEOUT(results().size(), 1, 3000);
    QCOMPARE(results().first(), QString("x:true:200:/api/v1/status"));

    QSignalSpy pausedSpy(api, &SupervisorApi::pausedChanged);
    api->setPaused(true);
    QCOMPARE(pausedSpy.count(), 1);
    api->get("/status", cb("a"));
    api->get("/status", cb("b"));
    api->get("/other", cb("c"));
    api->post("/cmd", QVariantMap{{"go", true}}, cb("d"));
    QTRY_COMPARE_WITH_TIMEOUT(http.count("POST /api/v1/cmd"), 1, 3000);
    QTest::qWait(200);
    QCOMPARE(http.count("GET "), 1);   // only the unpaused one
    QCOMPARE(api->heldPaths(), 2);
    QCOMPARE(api->heldCallbacks(), 3);
    QCOMPARE(api->pending(), 0);
    for (int i = 0; i < 10; ++i) api->get("/status", cb(QString("e%1").arg(i)));   // a poll timer that keeps firing
    QCOMPARE(api->heldPaths(), 2);
    QCOMPARE(api->heldCallbacks(), 8 + 1);

    api->setPaused(false);
    QTRY_COMPARE_WITH_TIMEOUT(http.count("GET /api/v1/status"), 2, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(http.count("GET /api/v1/other"), 1, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(results().size(), 2 + 8 + 1, 3000);
    const QStringList r = results();
    QVERIFY(r.contains("d:true:200:/api/v1/cmd"));
    QVERIFY(r.contains("c:true:200:/api/v1/other"));
    QVERIFY(r.contains("e9:true:200:/api/v1/status"));
    QVERIFY(!r.contains("a:true:200:/api/v1/status"));   // oldest held callbacks of a busy path were let go
    QCOMPARE(api->heldPaths(), 0);
    QCOMPARE(http.count("GET "), 3);

    api->setPaused(true);
    api->get("/status", cb("f"));
    QCOMPARE(api->heldPaths(), 1);
    api->shutdown();
    QCOMPARE(api->heldPaths(), 0);
    api->setPaused(false);
    QTest::qWait(200);
    QCOMPARE(http.count("GET "), 3);
    delete api;
}

// PA-4: no periodic status query. One at connect; a runlist rebuilt after a schedule edit arrives as the Supervisor
// broadcasts it (no query); a snapshot that stops coming is asked for once per window (time sync goes on, so this is
// not a stall); refresh() asks at once; the old poll comes back only through statusPollMs / PLAYPLAY_STATUS_POLL_MS.
void TestSupervisorClient::queriesOnlyWhenNeeded() {
    using namespace IpcProtocol;
    using SC = SupervisorClient;
    std::atomic<bool> pushing{true};   // before the fake: its handlers read it until the fake is gone
    FakeSupervisor fake;
    const QString name = uniqueName("noquery");
    QVERIFY(fake.start(name));
    fake.setQueryHandler([&fake](const QString& client) {   // a reply as the Supervisor sends one: snapshot + runlists
        fake.sendOne(client, make(MSG_UI_STATUS, QString(), simSnapshot(1)));
        for (const QString& ch : kSimChannels) fake.sendOne(client, make(MSG_UI_RUNLIST, ch, simRunlist(ch, 3)));
    });
    fake.every(200, [&fake]() { fake.sendAll(makeTimeSync(0, QDateTime::currentMSecsSinceEpoch() * 1000000LL, 0)); });
    fake.every(500, [&fake, &pushing]() { if (pushing) fake.sendAll(make(MSG_UI_STATUS, QString(), simSnapshot(QDateTime::currentMSecsSinceEpoch()))); });

    SupervisorClient c;
    c.setActiveChannelId(kSimChannels[0]);
    SC::Options o = opts(name);
    o.snapshotStaleMs = 1500;
    c.start(o);
    QTRY_VERIFY_WITH_TIMEOUT(c.isConnected(), 2000);
    QTRY_COMPARE_WITH_TIMEOUT(fake.queries.load(), 1, 1000);
    auto items = [&c](const QString& ch) { return c.runlistOf(ch).value("current").toMap().value("items").toList(); };
    QTRY_COMPARE_WITH_TIMEOUT(items(kSimChannels[0]).size(), 3, 2000);
    QCOMPARE(c.queriesSent(SC::QueryConnect), quint64(1));

    // Past the old 4 s poll: still the one query; the pushes keep the snapshot fresh.
    QTest::qWait(4500);
    QCOMPARE(fake.queries.load(), 1);
    QVERIFY2(c.ipcStats().snapshots >= 8, qPrintable(QString("snapshots %1").arg(c.ipcStats().snapshots)));
    QVERIFY(!c.supervisorStalled());

    // A schedule edit: the Supervisor rebuilds and broadcasts the runlist; it arrives without a query.
    QSignalSpy sched(&c, &SC::scheduleChanged), rl(&c, &SC::runlistChanged), arl(&c, &SC::activeRunlistChanged);
    fake.broadcast(make(MSG_UI_SCHEDULE, kSimChannels[0], QJsonObject{{"channelId", kSimChannels[0]}, {"slotId", "2026-10-26T14"}, {"_type", "schedule"}}));
    fake.broadcast(make(MSG_UI_RUNLIST, kSimChannels[0], simRunlist(kSimChannels[0], 4, "Edited")));
    QTRY_COMPARE_WITH_TIMEOUT(items(kSimChannels[0]).size(), 4, 2000);
    QCOMPARE(items(kSimChannels[0]).first().toMap().value("title").toString(), QString("Edited programme 0"));
    QCOMPARE(c.activeRunlist().value("current").toMap().value("items").toList().size(), 4);
    QCOMPARE(items(kSimChannels[1]).first().toMap().value("title").toString(), QString("Plan programme 0"));
    QCOMPARE(sched.count(), 1);
    QCOMPARE(sched.first().at(1).toString(), QString("2026-10-26T14"));
    QCOMPARE(rl.count(), 1);
    QCOMPARE(rl.first().at(0).toString(), kSimChannels[0]);
    QCOMPARE(arl.count(), 1);
    QCOMPARE(fake.queries.load(), 1);

    // Snapshots stop, time sync goes on: one query once the window is over; its answer opens a new window. Pushes
    // back: no more.
    pushing = false;
    QElapsedTimer s; s.start();
    QTRY_COMPARE_WITH_TIMEOUT(fake.queries.load(), 2, 3000);
    const qint64 staleAfter = s.elapsed();
    QCOMPARE(c.queriesSent(SC::QueryStale), quint64(1));
    QVERIFY(!c.supervisorStalled());
    QTest::qWait(600);
    QCOMPARE(fake.queries.load(), 2);
    pushing = true;
    QTest::qWait(2500);
    QCOMPARE(fake.queries.load(), 2);
    // The last push landed 0-500 ms before they stopped; the window is 1.5 s from it, checked every 0.5 s.
    QVERIFY2(staleAfter >= 900 && staleAfter <= 2400, qPrintable(QString("stale query after %1 ms").arg(staleAfter)));

    // QML's refresh() after an edit: one query at once.
    c.refresh();
    QTRY_COMPARE_WITH_TIMEOUT(fake.queries.load(), 3, 1000);
    const QVariantMap ic = c.ipcCounters();
    qInfo().noquote() << QString("no poll: queries %1 (connect %2, stale %3 after %4 ms, operator %5, poll %6) in %7 s; received %8 msgs, %9 bytes, %10 snapshots, %11 runlists")
                             .arg(ic.value("queries").toInt()).arg(ic.value("queriesConnect").toInt()).arg(ic.value("queriesStale").toInt()).arg(staleAfter)
                             .arg(ic.value("queriesOperator").toInt()).arg(ic.value("queriesPoll").toInt()).arg(s.elapsed() / 1000 + 5)
                             .arg(ic.value("received").toInt()).arg(ic.value("bytes").toLongLong()).arg(ic.value("snapshots").toInt()).arg(ic.value("runlists").toInt());
    QCOMPARE(ic.value("queries").toInt(), 3);
    QCOMPARE(ic.value("queriesOperator").toInt(), 1);
    QCOMPARE(ic.value("queriesPoll").toInt(), 0);
    QCOMPARE(ic.value("queriesRecover").toInt(), 0);
    QCOMPARE(ic.value("statusPollMs").toInt(), 0);
    QCOMPARE(ic.value("events").toInt(), 1);
    QTRY_COMPARE_WITH_TIMEOUT(c.ipcStats().runlists, quint64(2 + 1 + 2 + 2), 1000);   // connect 2, broadcast 1, stale 2, refresh 2
    c.stop();

    // The rollback knob: PLAYPLAY_STATUS_POLL_MS brings the periodic query back (statusPollMs -1 = from the environment).
    const int before = fake.queries.load();
    qputenv("PLAYPLAY_STATUS_POLL_MS", "300");
    SC::Options po = opts(name);
    po.statusPollMs = -1;
    SupervisorClient p;
    p.start(po);
    SC::Options zo = opts(name);   // an explicit 0 wins over the environment
    SupervisorClient z;
    z.start(zo);
    qunsetenv("PLAYPLAY_STATUS_POLL_MS");
    QCOMPARE(p.statusPollMs(), 300);
    QCOMPARE(z.statusPollMs(), 0);
    QTRY_VERIFY_WITH_TIMEOUT(p.isConnected() && z.isConnected(), 2000);
    QTest::qWait(1650);
    z.stop();
    const quint64 sentByP = p.queriesSent();
    QTRY_VERIFY_WITH_TIMEOUT(quint64(fake.queries.load() - before) >= sentByP + 1, 1000);   // they reached the Supervisor
    p.stop();
    const quint64 polls = p.queriesSent(SC::QueryPoll);
    QVERIFY2(polls >= 4 && polls <= 8, qPrintable(QString("polls %1").arg(polls)));
    QCOMPARE(p.queriesSent(SC::QueryConnect), quint64(1));
    QCOMPARE(z.queriesSent(), quint64(1));
    QCOMPARE(z.queriesSent(SC::QueryPoll), quint64(0));
}

// PA-4 (W12 client side): an idle schedule for 10 simulated minutes (40x real time, 15 s), measured by the app's own
// byte counter. Three app + Supervisor pairs side by side: before (the 4 s poll against a Supervisor from before PS-6),
// PS-6 alone (the app still polls) and PS-6 + PA-4 (no poll). Once without engines (like the lane test Supervisor) and
// once with two channels on air (40-item hours, 1 Hz telemetry). The client's count must match the bytes the fake wrote.
void TestSupervisorClient::idleBytesTenMinutes() {
    using SC = SupervisorClient;
    const int speed = 40, simMinutes = 10;
    const qint64 runMs = qint64(simMinutes) * 60000 / speed;
    struct Run { const char* label; bool ps6; int pollMs; };
    const Run runs[3] = {{"before: 4 s poll, Supervisor before PS-6", false, 4000}, {"PS-6, the app still polls", true, 4000}, {"PS-6 + PA-4, no poll", true, 0}};
    qInfo().noquote() << QString("payloads: snapshot %1 B, runlist %2 B empty / %3 B on air, telemetry %4 B, time sync %5 B")
                             .arg(wire(IpcProtocol::make(IpcProtocol::MSG_UI_STATUS, QString(), simSnapshot(1761487300000LL))))
                             .arg(wire(IpcProtocol::make(IpcProtocol::MSG_UI_RUNLIST, kSimChannels[0], simRunlist(kSimChannels[0], 0))))
                             .arg(wire(IpcProtocol::make(IpcProtocol::MSG_UI_RUNLIST, kSimChannels[0], simRunlist(kSimChannels[0], 40))))
                             .arg(wire(IpcProtocol::make(IpcProtocol::MSG_UI_TELEMETRY, kSimChannels[0], simTelemetry(kSimChannels[0], 1))))
                             .arg(wire(IpcProtocol::makeTimeSync(0, QDateTime::currentMSecsSinceEpoch() * 1000000LL, 0)));
    for (const bool onAir : {false, true}) {
        QElapsedTimer t;
        qint64 connectedAt[3] = {-1, -1, -1};   // before the clients: their connectedChanged handlers write here
        std::vector<std::unique_ptr<FakeSupervisor>> fakes;
        std::vector<std::unique_ptr<SupervisorClient>> clients;   // after fakes: stopped and gone first
        std::vector<SC::Options> options;
        for (const Run& r : runs) {
            fakes.push_back(std::make_unique<FakeSupervisor>());
            const QString name = uniqueName("idle");
            QVERIFY(fakes.back()->start(name));
            simulate(*fakes.back(), SimConfig{r.ps6, onAir, speed});
            SC::Options o = opts(name);
            o.statusPollMs = r.pollMs / speed;
            o.snapshotStaleMs = SC::Options().snapshotStaleMs / speed;
            options.push_back(o);
            clients.push_back(std::make_unique<SupervisorClient>());
        }
        t.start();
        for (int i = 0; i < 3; ++i) {
            SupervisorClient* c = clients[size_t(i)].get();
            connect(c, &SC::connectedChanged, c, [c, i, &t, &connectedAt]() { if (c->isConnected() && connectedAt[i] < 0) connectedAt[i] = t.elapsed(); });
            c->start(options[size_t(i)]);
        }
        QTRY_VERIFY_WITH_TIMEOUT(connectedAt[0] >= 0 && connectedAt[1] >= 0 && connectedAt[2] >= 0, 3000);
        QTest::qWait(int(runMs));
        const qint64 frozenAt = t.elapsed();
        for (auto& f : fakes) f->silence();   // no more sends; what is on its way still lands
        QTest::qWait(400);
        double kbMin[3] = {};
        for (int i = 0; i < 3; ++i) {
            SupervisorClient* c = clients[size_t(i)].get();
            const SupervisorClientWorker::Stats st = c->ipcStats();
            const qint64 sent = fakes[size_t(i)]->bytesSent.load();
            const double minutes = double(frozenAt - connectedAt[i]) * speed / 60000.0;
            kbMin[i] = double(st.bytes) / 1024.0 / minutes;
            auto per = [minutes](quint64 b) { return QString::number(double(b) / 1024.0 / minutes, 'f', 1); };
            qInfo().noquote() << QString("%1, %2: %3 KB/min (snapshots %4, runlists %5, telemetry %6, time sync %7) over %8 simulated min; "
                                         "%9 snapshots, %10 runlists; queries %11 (connect %12, poll %13, stale %14); pipe %15 bytes, counted %16")
                                     .arg(onAir ? "on air" : "no engines", runs[i].label, QString::number(kbMin[i], 'f', 1), per(st.snapshotBytes), per(st.runlistBytes),
                                          per(st.telemetryBytes), per(st.timeSyncBytes), QString::number(minutes, 'f', 2))
                                     .arg(st.snapshots).arg(st.runlists).arg(c->queriesSent()).arg(c->queriesSent(SC::QueryConnect))
                                     .arg(c->queriesSent(SC::QueryPoll)).arg(c->queriesSent(SC::QueryStale)).arg(sent).arg(st.bytes);
            QVERIFY2(std::abs(double(st.bytes) - double(sent)) <= 0.001 * double(sent) + 64, "the client's byte count does not match the pipe");
            QVERIFY(!c->supervisorStalled());
        }
        const double drop = 100.0 * (1.0 - kbMin[2] / kbMin[0]), dropPs6 = 100.0 * (1.0 - kbMin[2] / kbMin[1]);
        qInfo().noquote() << QString("%1: PS-6 + PA-4 is %2 % below before, %3 % below PS-6 alone").arg(onAir ? "on air" : "no engines")
                                 .arg(drop, 0, 'f', 1).arg(dropPs6, 0, 'f', 1);
        QVERIFY2(drop >= 75.0, qPrintable(QString("only %1 % less than before").arg(drop, 0, 'f', 1)));   // target: about 80 %
        QVERIFY(kbMin[2] < kbMin[1]);
        QCOMPARE(clients[2]->queriesSent(), quint64(1));                // the connect, nothing else
        QCOMPARE(clients[2]->queriesSent(SC::QueryStale), quint64(0));   // the keepalive always came in time
        for (int i = 0; i < 2; ++i) {                                     // 10 min / 4 s = 150 polls
            const quint64 polls = clients[size_t(i)]->queriesSent(SC::QueryPoll);
            QVERIFY2(polls >= 130 && polls <= 160, qPrintable(QString("%1 polls").arg(polls)));
        }
        for (auto& c : clients) c->stop();
    }
}

QTEST_GUILESS_MAIN(TestSupervisorClient)
#include "test_supervisorclient.moc"
