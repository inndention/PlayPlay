#include <QtTest/QtTest>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QMutex>
#include <QRandomGenerator>
#include <QThread>
#include <atomic>
#include <functional>
#include <thread>
#include "playout/ui_ipc/ipctransport.h"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

Q_DECLARE_METATYPE(IpcProtocol::Message)

// PS-5 (W8): binary preview frames and preview subscriptions.
//  - frame codec: binary body (version byte, JSON head, raw JPEG) and the JSON fallback (pre-PS-5 shape);
//  - negotiation through real transports: new <-> new goes binary, everything that did not announce gets JSON;
//  - a mixed relay (the Supervisor's MSG_PREVIEW_FRAME code) with a new engine, a pre-PS-5 engine (raw JSON writer),
//    a binary UI, a JSON UI (today's app) and a raw pre-PS-5 reader that must never meet the flag;
//  - preview subscriptions (keys + fps cap);
//  - a relay bench: 100 frames/s through an in-process server on its own thread, relay-thread CPU cycles per frame for
//    the pre-PS-5 JSON/base64 path vs the binary path.
// Every pipe name is unique to this process ("ppib-..."); nothing here touches the live IPC name.
class TestIpcBinary : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { qRegisterMetaType<IpcProtocol::Message>(); }
    void frameCodec();
    void previewPassTable();
    void transportBinaryRoundTrip();
    void mixedPeersRelay();
    void newEngineOldSupervisor();
    void receiverSafety();
    void previewSubscription();
    void benchRelay();
};

namespace {
using IpcProtocol::Message;

QString uniqueName(const char* tag) {
    static int n = 0;
    return QString("ppib-%1-%2-%3").arg(tag).arg(QCoreApplication::applicationPid()).arg(++n);
}
std::wstring pipePath(const QString& name) { return (QStringLiteral("\\\\.\\pipe\\") + name).toStdWString(); }
HANDLE openRawClient(const QString& name) {
    return CreateFileW(pipePath(name).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
}
HANDLE createRawServer(const QString& name) {
    return CreateNamedPipeW(pipePath(name).c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                            1, 1 << 20, 1 << 20, 0, nullptr);
}
QByteArray rawFrame(quint32 len, const QByteArray& body) { QByteArray f(reinterpret_cast<const char*>(&len), 4); f += body; return f; }
QByteArray jsonFrame(const Message& m) { const QByteArray b = m.toJson(); return rawFrame(quint32(b.size()), b); }
// On a helper thread while this thread's loop keeps running: a synchronous pipe write can wait for the reader, and the
// reader (a transport) lives on this thread.
void writeRawSync(HANDLE h, const QByteArray& bytes) {
    std::atomic<bool> done{false};
    std::thread t([h, bytes, &done]() { DWORD n = 0; WriteFile(h, bytes.constData(), DWORD(bytes.size()), &n, nullptr); done = true; });
    QElapsedTimer w; w.start();
    while (!done.load() && w.elapsed() < 10000) QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    t.join();
}

// A JPEG-shaped blob (SOI ... EOI): the transport never looks inside.
QByteArray fakeJpeg(int bytes, quint32 seed) {
    QByteArray b(bytes, Qt::Uninitialized);
    QRandomGenerator g(seed);
    for (int i = 0; i < bytes; ++i) b[i] = char(g.generate() & 0xFF);
    b[0] = char(0xFF); b[1] = char(0xD8); b[bytes - 2] = char(0xFF); b[bytes - 1] = char(0xD9);
    return b;
}
QJsonObject meters() { QJsonObject m; m["l"] = -12.5; m["r"] = -13.0; m["peakL"] = -3.2; return m; }

// What engine_main sent before PS-5.
Message oldPreview(const QString& ch, const QByteArray& jpeg, const QString& input = {}) {
    QJsonObject p;
    if (!input.isEmpty()) p["input"] = input;
    p["jpegBase64"] = QString::fromLatin1(jpeg.toBase64());
    p["width"] = 640; p["height"] = 360;
    return IpcProtocol::make(IpcProtocol::MSG_PREVIEW_FRAME, ch, p);
}
// What engine_main sends since PS-5.
Message newPreview(const QString& ch, const QByteArray& jpeg, const QString& input = {}) {
    QJsonObject p;
    if (!input.isEmpty()) p["input"] = input;
    p["width"] = 640; p["height"] = 360;
    return IpcProtocol::makePreviewFrame(ch, jpeg, p);
}

// The Supervisor's MSG_PREVIEW_FRAME relay before PS-5 (supervisorcore.cpp) ...
void relayOld(IpcTransport* t, const QSet<QString>& ui, const Message& msg, QHash<QString, QByteArray>& store) {
    const QByteArray jpeg = QByteArray::fromBase64(msg.payload.value("jpegBase64").toString().toLatin1());
    const QString input = msg.payload.value("input").toString();
    const QString key = input.isEmpty() ? msg.channelId : msg.channelId + ":" + input;
    store[key] = jpeg;
    if (input.isEmpty()) t->sendTo(ui, msg, IpcTransport::Droppable);
    else { Message rel = msg; rel.channelId = key; t->sendTo(ui, rel, IpcTransport::Droppable); }
}
// ... and since PS-5 (same lines as supervisorcore.cpp's case MSG_PREVIEW_FRAME).
void relayNew(IpcTransport* t, const QSet<QString>& ui, const Message& msg, QHash<QString, QByteArray>& store) {
    Message rel = msg;
    const QByteArray jpeg = IpcProtocol::liftPreview(rel);
    const QString input = rel.payload.value("input").toString();
    const QString key = input.isEmpty() ? rel.channelId : rel.channelId + ":" + input;
    store[key] = jpeg;
    rel.channelId = key;
    t->sendTo(ui, rel, IpcTransport::Droppable);
}

// A raw pipe client that reads frames the pre-PS-5 way on its own thread (PeekNamedPipe, never blocks on ReadFile).
struct RawOldReader {
    HANDLE h = INVALID_HANDLE_VALUE;
    std::atomic<bool> stop{false};
    std::atomic<int> previews{0}, frames{0}, flagged{0}, badJson{0}, caps{0};
    QMutex mu; QList<QByteArray> jpegs; QStringList keys;
    std::thread th;
    void start() {
        th = std::thread([this]() {
            QByteArray buf;
            while (!stop.load()) {
                DWORD avail = 0;
                if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) break;
                if (avail == 0) { Sleep(1); continue; }
                QByteArray chunk(int(avail), Qt::Uninitialized); DWORD n = 0;
                if (!ReadFile(h, chunk.data(), avail, &n, nullptr)) break;
                buf.append(chunk.constData(), int(n));
                while (buf.size() >= 4) {
                    quint32 len = 0; memcpy(&len, buf.constData(), 4);
                    if (len & 0x80000000u) { ++flagged; buf.clear(); break; }   // a pre-PS-5 reader would wait for 2 GB here
                    if (buf.size() < 4 + int(len)) break;
                    const QJsonDocument d = QJsonDocument::fromJson(buf.mid(4, int(len)));
                    buf.remove(0, 4 + int(len));
                    ++frames;
                    if (!d.isObject()) { ++badJson; continue; }
                    const QJsonObject o = d.object();
                    const int type = o.value("type").toInt();
                    if (type == IpcProtocol::MSG_IPC_CAPS) ++caps;
                    if (type != IpcProtocol::MSG_PREVIEW_FRAME) continue;
                    QMutexLocker lk(&mu);
                    jpegs << QByteArray::fromBase64(o.value("payload").toObject().value("jpegBase64").toString().toLatin1());
                    keys << o.value("channelId").toString();
                    ++previews;
                }
            }
        });
    }
    ~RawOldReader() { stop = true; if (th.joinable()) th.join(); if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
};

// Runs code on a thread with its own event loop (the relay of the bench).
class LoopThread {
public:
    LoopThread() { m_ctx = new QObject; m_ctx->moveToThread(&m_thread); m_thread.start(); run([this]() { m_tid = GetCurrentThreadId(); }); }
    ~LoopThread() { run([this]() { delete m_ctx; m_ctx = nullptr; }); m_thread.quit(); m_thread.wait(); }
    void run(const std::function<void()>& fn) { QMetaObject::invokeMethod(m_ctx, fn, Qt::BlockingQueuedConnection); }
    quint64 cycles() const {   // CPU cycles this thread has run (QueryThreadCycleTime: precise, unlike GetThreadTimes' 15.6 ms ticks)
        HANDLE th = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, m_tid);
        ULONG64 c = 0; if (th) { QueryThreadCycleTime(th, &c); CloseHandle(th); }
        return c;
    }
    double cpuMs() const {
        HANDLE th = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, m_tid);
        FILETIME a, b, k, u; double ms = 0;
        if (th && GetThreadTimes(th, &a, &b, &k, &u)) {
            ULARGE_INTEGER ki, ui; ki.LowPart = k.dwLowDateTime; ki.HighPart = k.dwHighDateTime; ui.LowPart = u.dwLowDateTime; ui.HighPart = u.dwHighDateTime;
            ms = double(ki.QuadPart + ui.QuadPart) / 10000.0;
        }
        if (th) CloseHandle(th);
        return ms;
    }
private:
    QThread m_thread; QObject* m_ctx = nullptr; DWORD m_tid = 0;
};
}

void TestIpcBinary::frameCodec() {
    using namespace IpcProtocol;
    const QByteArray jpeg = fakeJpeg(110 * 1024, 1);
    QJsonObject p; p["width"] = 640; p["height"] = 360; p["frame"] = 1234.0; p["meters"] = meters();
    const Message m = makePreviewFrame("main", jpeg, p);
    QCOMPARE(m.binaryKey, QStringLiteral("jpegBase64"));
    QVERIFY(!m.payload.contains("jpegBase64"));

    // Binary frame: flag set, length = body, decodes to the same message.
    const QByteArray bin = encodeFrame(m, true);
    quint32 hdr = 0; memcpy(&hdr, bin.constData(), 4);
    QVERIFY(hdr & kBinaryFrameFlag);
    QCOMPARE(qint64(hdr & kFrameLengthMask), qint64(bin.size() - 4));
    QCOMPARE(int(static_cast<unsigned char>(bin.at(4))), kBinaryFrameVersion);
    QCOMPARE(binaryBody(m), bin.mid(4));
    Message d;
    QCOMPARE(decodeBinaryBody(bin.constData() + 4, bin.size() - 4, &d), BinaryDecode::Ok);
    QCOMPARE(d.type, MSG_PREVIEW_FRAME);
    QCOMPARE(d.channelId, QStringLiteral("main"));
    QCOMPARE(d.timestampNs, m.timestampNs);
    QCOMPARE(d.payload, p);
    QCOMPARE(d.binary, jpeg);
    QCOMPARE(d.binaryKey, QStringLiteral("jpegBase64"));

    // JSON fallback: no flag, and the same JSON object the pre-PS-5 engine produced (base64 under jpegBase64).
    const QByteArray js = encodeFrame(m, false);
    memcpy(&hdr, js.constData(), 4);
    QVERIFY(!(hdr & kBinaryFrameFlag));
    QCOMPARE(qint64(hdr), qint64(js.size() - 4));
    QJsonObject oldShape; {
        QJsonObject op = p; op["jpegBase64"] = QString::fromLatin1(jpeg.toBase64());
        Message o = make(MSG_PREVIEW_FRAME, "main", op); o.timestampNs = m.timestampNs;
        oldShape = QJsonDocument::fromJson(o.toJson()).object();
    }
    QJsonParseError pe{};
    const QJsonDocument fallbackDoc = QJsonDocument::fromJson(js.mid(4), &pe);
    QCOMPARE(pe.error, QJsonParseError::NoError);
    QCOMPARE(fallbackDoc.object(), oldShape);
    QCOMPARE(m.toJson(), js.mid(4));   // toJson() is the fallback
    const Message back = Message::fromJson(js.mid(4));
    QCOMPARE(previewJpeg(back), jpeg);
    QVERIFY(back.binary.isEmpty());
    qInfo().noquote() << QString("110 KB JPEG: binary frame %1 bytes, JSON/base64 frame %2 bytes (%3 %)")
                             .arg(bin.size()).arg(js.size()).arg(100.0 * bin.size() / js.size(), 0, 'f', 1);
    QVERIFY(bin.size() < js.size() * 0.80);

    // Escaping in the hand-built fallback: quotes, backslashes, non-ASCII, an empty payload.
    {
        Message e = makePreviewFrame(QString::fromUtf8("ma\"in\\ \xC3\xA9:in\n3"), QByteArray("\xFF\xD8xyz\xFF\xD9", 7), QJsonObject());
        const QJsonDocument ed = QJsonDocument::fromJson(e.toJson(), &pe);
        QCOMPARE(pe.error, QJsonParseError::NoError);
        QCOMPARE(ed.object().value("channelId").toString(), e.channelId);
        QCOMPARE(ed.object().value("payload").toObject().size(), 1);
        QCOMPARE(previewJpeg(Message::fromJson(e.toJson())), e.binary);
        Message e2; QCOMPARE(decodeBinaryBody(binaryBody(e).constData(), binaryBody(e).size(), &e2), BinaryDecode::Ok);
        QCOMPARE(e2.channelId, e.channelId);
    }
    // A message without bytes is unchanged: the same frame both ways.
    {
        const Message a = make(MSG_UI_ALERT, "main", QJsonObject{{"level", "warning"}, {"message", "x"}});
        QCOMPARE(encodeFrame(a, true), encodeFrame(a, false));
        memcpy(&hdr, encodeFrame(a, true).constData(), 4);
        QVERIFY(!(hdr & kBinaryFrameFlag));
    }
    // Bad bodies: unknown version (skippable), short, head longer than the body, head not JSON.
    QByteArray v2 = bin.mid(4); v2[0] = char(2);
    QCOMPARE(decodeBinaryBody(v2.constData(), v2.size(), &d), BinaryDecode::UnknownVersion);
    QCOMPARE(decodeBinaryBody("\x01\x00", 2, &d), BinaryDecode::Malformed);
    QByteArray longHead = bin.mid(4); const quint32 big = 0x7FFFFFF0u; memcpy(longHead.data() + 1, &big, 4);
    QCOMPARE(decodeBinaryBody(longHead.constData(), longHead.size(), &d), BinaryDecode::Malformed);
    QByteArray notJson = bin.mid(4); notJson[5] = 'x';
    QCOMPARE(decodeBinaryBody(notJson.constData(), notJson.size(), &d), BinaryDecode::Malformed);

    // inlineBinary / liftPreview are inverses on the payload.
    Message in = m; inlineBinary(in);
    QVERIFY(in.binary.isEmpty());
    QCOMPARE(in.payload.value("jpegBase64").toString(), QString::fromLatin1(jpeg.toBase64()));
    Message lifted = oldPreview("main", jpeg, "in3");
    QCOMPARE(liftPreview(lifted), jpeg);
    QVERIFY(!lifted.payload.contains("jpegBase64"));
    QCOMPARE(lifted.payload.value("input").toString(), QStringLiteral("in3"));
    QCOMPARE(liftPreview(lifted), jpeg);   // idempotent
    // Wire values: appended after MSG_SUP_ALIVE (23).
    QCOMPARE(int(MSG_IPC_CAPS), 24);
    QCOMPARE(int(MSG_UI_PREVIEW_SUBSCRIBE), 25);
}

void TestIpcBinary::previewPassTable() {
    using S = IpcTransport::PreviewSub;
    S all;
    S some; some.all = false; some.keys = QStringList{"main", "ch2:*"};
    S none; none.all = false;
    S capped; capped.maxFps = 5;   // 200 ms, 170 ms with the 15 % allowance
    struct Row { const char* name; S sub; QString key; qint64 now, last; bool pass; };
    const QList<Row> rows = {
        {"all, first", all, "main", 1000, -1, true},
        {"all, no cap, back to back", all, "main", 1000, 999, true},
        {"exact key", some, "main", 1000, -1, true},
        {"other key", some, "main:in3", 1000, -1, false},
        {"prefix key", some, "ch2:in1", 1000, -1, true},
        {"prefix itself", some, "ch2:", 1000, -1, true},
        {"prefix does not match the channel", some, "ch2", 1000, -1, false},
        {"empty subscription", none, "main", 1000, -1, false},
        {"cap: first", capped, "main", 1000, -1, true},
        {"cap: too soon", capped, "main", 1100, 1000, false},
        {"cap: just inside the allowance", capped, "main", 1170, 1000, true},
        {"cap: due", capped, "main", 1200, 1000, true},
    };
    for (const Row& r : rows) QVERIFY2(IpcTransport::previewPass(r.sub, r.key, r.now, r.last) == r.pass, r.name);
}

void TestIpcBinary::transportBinaryRoundTrip() {
    const QString name = uniqueName("rt");
    IpcTransport server(IpcTransport::ServerRole);
    QVERIFY(server.acceptsBinary());   // the Supervisor's default
    QVERIFY(server.startServer(name));
    QSignalSpy srvMsgs(&server, &IpcTransport::messageReceived), conn(&server, &IpcTransport::clientConnected);
    IpcTransport client(IpcTransport::ClientRole);
    QVERIFY(!client.acceptsBinary());   // the app's default
    client.setAcceptBinary(true);
    QSignalSpy cliMsgs(&client, &IpcTransport::messageReceived);
    client.connectToServerAsync(name, 2000);
    QTRY_VERIFY_WITH_TIMEOUT(client.isConnected(), 3000);
    QTRY_COMPARE_WITH_TIMEOUT(conn.count(), 1, 3000);
    const QString cid = conn.at(0).at(0).toString();
    QTRY_VERIFY_WITH_TIMEOUT(client.peerAcceptsBinary(), 3000);       // the server's answer
    QTRY_VERIFY_WITH_TIMEOUT(server.peerAcceptsBinary(cid), 3000);    // the client's caps
    QCOMPARE(srvMsgs.count(), 0);   // caps never reach the owner
    QCOMPARE(cliMsgs.count(), 0);

    const QByteArray j1 = fakeJpeg(90000, 2), j2 = fakeJpeg(120000, 3);
    QVERIFY(client.send(newPreview("perftest", j1)));
    QVERIFY(client.send(IpcProtocol::make(IpcProtocol::MSG_UI_ALERT, "perftest", QJsonObject{{"message", "after"}})));
    QTRY_COMPARE_WITH_TIMEOUT(srvMsgs.count(), 2, 3000);
    Message got = srvMsgs.at(0).at(0).value<Message>();
    QCOMPARE(got.type, IpcProtocol::MSG_PREVIEW_FRAME);
    QCOMPARE(got.binary, j1);
    QVERIFY(!got.payload.contains("jpegBase64"));
    QCOMPARE(got.payload.value("width").toInt(), 640);
    QCOMPARE(srvMsgs.at(1).at(0).value<Message>().payload.value("message").toString(), QStringLiteral("after"));   // order kept

    QVERIFY(server.sendTo(cid, newPreview("perftest", j2, "in1")));
    QTRY_COMPARE_WITH_TIMEOUT(cliMsgs.count(), 1, 3000);
    got = cliMsgs.at(0).at(0).value<Message>();
    QCOMPARE(got.binary, j2);
    QCOMPARE(got.payload.value("input").toString(), QStringLiteral("in1"));
    QCOMPARE(client.stats().value("binarySent").toDouble(), 1.0);
    QCOMPARE(client.stats().value("binaryReceived").toDouble(), 1.0);
    QCOMPARE(server.stats().value("binarySent").toDouble(), 1.0);
    QCOMPARE(server.stats().value("binaryReceived").toDouble(), 1.0);
    QCOMPARE(server.stats().value("binaryFallbacks").toDouble(), 0.0);

    // The client stops accepting (re-announced): the server falls back to JSON for it; the owner still gets bytes it can read.
    client.setAcceptBinary(false);
    QTRY_VERIFY_WITH_TIMEOUT(!server.peerAcceptsBinary(cid), 3000);
    QVERIFY(server.sendTo(cid, newPreview("perftest", j1)));
    QTRY_COMPARE_WITH_TIMEOUT(cliMsgs.count(), 2, 3000);
    got = cliMsgs.at(1).at(0).value<Message>();
    QVERIFY(got.binary.isEmpty());
    QCOMPARE(IpcProtocol::previewJpeg(got), j1);
    QCOMPARE(server.stats().value("binaryFallbacks").toDouble(), 1.0);

    // A reconnect starts over: the new connection negotiates again.
    client.setAcceptBinary(true);
    client.disconnect();
    client.connectToServerAsync(name, 2000);
    QTRY_VERIFY_WITH_TIMEOUT(client.isConnected() && client.peerAcceptsBinary(), 3000);
}

void TestIpcBinary::mixedPeersRelay() {
    const QString name = uniqueName("mixed");
    IpcTransport server(IpcTransport::ServerRole);
    QVERIFY(server.startServer(name));
    QSet<QString> ui;
    QHash<QString, QByteArray> store;
    int relayed = 0;
    connect(&server, &IpcTransport::messageReceived, &server, [&](const Message& m, const QString& cid) {
        if (m.type == IpcProtocol::MSG_UI_QUERY_STATUS) ui.insert(cid);
        if (m.type == IpcProtocol::MSG_PREVIEW_FRAME) { relayNew(&server, ui, m, store); ++relayed; }
    });

    // UIs: a binary UI, today's app (JSON), a raw pre-PS-5 reader.
    IpcTransport newUi(IpcTransport::ClientRole), oldUi(IpcTransport::ClientRole);
    newUi.setAcceptBinary(true);
    QSignalSpy newUiMsgs(&newUi, &IpcTransport::messageReceived), oldUiMsgs(&oldUi, &IpcTransport::messageReceived);
    for (IpcTransport* t : {&newUi, &oldUi}) { t->connectToServerAsync(name, 2000); QTRY_VERIFY_WITH_TIMEOUT(t->isConnected(), 3000); }
    QVERIFY(newUi.send(IpcProtocol::make(IpcProtocol::MSG_UI_QUERY_STATUS, "", {})));
    QVERIFY(oldUi.send(IpcProtocol::make(IpcProtocol::MSG_UI_QUERY_STATUS, "", {})));
    RawOldReader raw;
    raw.h = openRawClient(name);
    QVERIFY(raw.h != INVALID_HANDLE_VALUE);
    writeRawSync(raw.h, jsonFrame(IpcProtocol::make(IpcProtocol::MSG_UI_QUERY_STATUS, "", {})));
    raw.start();
    QTRY_COMPARE_WITH_TIMEOUT(ui.size(), 3, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(newUi.peerAcceptsBinary(), 3000);

    // Engines: a PS-5 engine (binary, since the server accepts) and a pre-PS-5 engine (raw JSON writer, no caps).
    IpcTransport newEngine(IpcTransport::ClientRole);
    newEngine.connectToServerAsync(name, 2000);
    QTRY_VERIFY_WITH_TIMEOUT(newEngine.isConnected() && newEngine.peerAcceptsBinary(), 3000);
    HANDLE oldEngine = openRawClient(name);
    QVERIFY(oldEngine != INVALID_HANDLE_VALUE);

    const int N = 20;
    QList<QByteArray> sentNew, sentOld;
    for (int i = 0; i < N; ++i) {
        sentNew << fakeJpeg(60000 + i * 100, 100 + i);
        sentOld << fakeJpeg(50000 + i * 100, 200 + i);
        QVERIFY(newEngine.send(newPreview("perftest", sentNew.last())));
        writeRawSync(oldEngine, jsonFrame(oldPreview("perftest-old", sentOld.last(), "in3")));
    }
    QTRY_COMPARE_WITH_TIMEOUT(relayed, 2 * N, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(newUiMsgs.count(), 2 * N, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(oldUiMsgs.count(), 2 * N, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(raw.previews.load(), 2 * N, 5000);

    // Every UI got every picture, byte for byte, under the relay key; each in its own form.
    auto check = [&](const QList<QByteArray>& jpegs, const QStringList& keys, const char* who) {
        int iNew = 0, iOld = 0;
        for (int i = 0; i < jpegs.size(); ++i) {
            if (keys.at(i) == QLatin1String("perftest")) QVERIFY2(jpegs.at(i) == sentNew.at(iNew++), who);
            else { QCOMPARE(keys.at(i), QStringLiteral("perftest-old:in3")); QVERIFY2(jpegs.at(i) == sentOld.at(iOld++), who); }
        }
        QCOMPARE(iNew, N); QCOMPARE(iOld, N);
    };
    QList<QByteArray> nj, oj; QStringList nk, ok;
    for (const auto& a : newUiMsgs) { const Message m = a.at(0).value<Message>(); QVERIFY(!m.binary.isEmpty()); QVERIFY(!m.payload.contains("jpegBase64")); nj << m.binary; nk << m.channelId; }
    for (const auto& a : oldUiMsgs) { const Message m = a.at(0).value<Message>(); QVERIFY(m.binary.isEmpty()); QVERIFY(m.payload.contains("jpegBase64")); oj << IpcProtocol::previewJpeg(m); ok << m.channelId; }
    check(nj, nk, "binary UI");
    check(oj, ok, "JSON UI");
    { QMutexLocker lk(&raw.mu); check(raw.jpegs, raw.keys, "raw pre-PS-5 reader"); }
    QCOMPARE(store.value("perftest"), sentNew.last());
    QCOMPARE(store.value("perftest-old:in3"), sentOld.last());

    // The old reader never met the flag nor anything new (it never sent caps, so the server never sent it any).
    QCOMPARE(raw.flagged.load(), 0);
    QCOMPARE(raw.badJson.load(), 0);
    QCOMPARE(raw.caps.load(), 0);
    QCOMPARE(raw.frames.load(), 2 * N);
    // ... and the pre-PS-5 engine got nothing at all from the server.
    DWORD avail = 0;
    QVERIFY(PeekNamedPipe(oldEngine, nullptr, 0, nullptr, &avail, nullptr));
    QCOMPARE(int(avail), 0);
    CloseHandle(oldEngine);

    const QJsonObject st = server.stats();
    QCOMPARE(st.value("binaryReceived").toDouble(), double(N));
    QCOMPARE(st.value("binarySent").toDouble(), double(2 * N));        // the binary UI
    QCOMPARE(st.value("binaryFallbacks").toDouble(), double(4 * N));   // the JSON UI and the raw reader
}

void TestIpcBinary::newEngineOldSupervisor() {
    // A pre-PS-5 Supervisor = a raw server end that parses JSON frames. The PS-5 client's caps arrive as an ordinary JSON
    // message of an unknown type (24) that such a Supervisor ignores; with no answer, previews go out as JSON/base64.
    const QString name = uniqueName("oldsup");
    HANDLE srv = createRawServer(name);
    QVERIFY(srv != INVALID_HANDLE_VALUE);
    IpcTransport engine(IpcTransport::ClientRole);
    engine.connectToServerAsync(name, 2000);
    QTRY_VERIFY_WITH_TIMEOUT(engine.isConnected(), 3000);
    const QByteArray jpeg = fakeJpeg(70000, 9);
    QVERIFY(engine.send(newPreview("perftest", jpeg)));
    QVERIFY(!engine.peerAcceptsBinary());

    QByteArray buf; QList<QJsonObject> got; std::atomic<int> gotN{0}; QElapsedTimer t; t.start();
    std::thread rd([&]() {
        while (gotN.load() < 2 && t.elapsed() < 5000) {
            DWORD avail = 0;
            if (!PeekNamedPipe(srv, nullptr, 0, nullptr, &avail, nullptr)) break;
            if (!avail) { Sleep(1); continue; }
            QByteArray c(int(avail), Qt::Uninitialized); DWORD n = 0;
            if (!ReadFile(srv, c.data(), avail, &n, nullptr)) break;
            buf.append(c.constData(), int(n));
            while (buf.size() >= 4) {
                quint32 len = 0; memcpy(&len, buf.constData(), 4);
                if (len & 0x80000000u) { got << QJsonObject{{"flagged", true}}; ++gotN; buf.clear(); break; }
                if (buf.size() < 4 + int(len)) break;
                got << QJsonDocument::fromJson(buf.mid(4, int(len))).object();
                ++gotN;
                buf.remove(0, 4 + int(len));
            }
        }
    });
    // keep this thread's loop running while the reader works (the transport writes from it)
    QTRY_VERIFY_WITH_TIMEOUT(t.elapsed() > 5000 || gotN.load() >= 2, 6000);
    rd.join();
    QCOMPARE(got.size(), 2);
    QCOMPARE(got.at(0).value("type").toInt(), int(IpcProtocol::MSG_IPC_CAPS));   // first on the connection
    QCOMPARE(got.at(0).value("payload").toObject().value("binaryFrames").toInt(), 0);   // engines do not accept binary
    QCOMPARE(got.at(1).value("type").toInt(), int(IpcProtocol::MSG_PREVIEW_FRAME));
    QVERIFY(!got.at(1).contains("flagged"));
    QCOMPARE(QByteArray::fromBase64(got.at(1).value("payload").toObject().value("jpegBase64").toString().toLatin1()), jpeg);
    QCOMPARE(engine.stats().value("binaryFallbacks").toDouble(), 1.0);
    engine.disconnect();
    CloseHandle(srv);
}

void TestIpcBinary::receiverSafety() {
    // A raw server writes straight to a client transport: binary frames it never asked for, a newer version, a broken
    // body and an oversized flagged header.
    const QString name = uniqueName("safety");
    HANDLE srv = createRawServer(name);
    QVERIFY(srv != INVALID_HANDLE_VALUE);
    IpcTransport app(IpcTransport::ClientRole);   // accept off, like today's app
    QSignalSpy msgs(&app, &IpcTransport::messageReceived);
    app.connectToServerAsync(name, 2000);
    QTRY_VERIFY_WITH_TIMEOUT(app.isConnected(), 3000);

    const QByteArray jpeg = fakeJpeg(80000, 5);
    const QByteArray bin = IpcProtocol::encodeFrame(newPreview("perftest", jpeg), true);
    QByteArray v2 = bin; v2[4] = char(2);
    QByteArray broken = bin; broken[4 + 5] = '#';
    const QByteArray alert = jsonFrame(IpcProtocol::make(IpcProtocol::MSG_UI_ALERT, "perftest", QJsonObject{{"message", "still here"}}));
    std::thread w([&]() { DWORD n = 0; const QByteArray all = bin + v2 + broken + alert; WriteFile(srv, all.constData(), DWORD(all.size()), &n, nullptr); });
    QTRY_COMPARE_WITH_TIMEOUT(msgs.count(), 2, 3000);
    w.join();
    const Message m0 = msgs.at(0).at(0).value<Message>();
    QVERIFY(m0.binary.isEmpty());   // inlined for an owner that never accepted binary
    QCOMPARE(QByteArray::fromBase64(m0.payload.value("jpegBase64").toString().toLatin1()), jpeg);
    QCOMPARE(msgs.at(1).at(0).value<Message>().payload.value("message").toString(), QStringLiteral("still here"));
    QCOMPARE(app.stats().value("unknownFrames").toDouble(), 2.0);   // v2 and the broken body skipped, link kept
    QVERIFY(app.isConnected());

    // A flagged header above the frame limit closes the link like any other oversized header.
    QSignalSpy state(&app, &IpcTransport::connectionStateChanged);
    std::thread w2([&]() { DWORD n = 0; const QByteArray f = rawFrame(0x80000000u | quint32((64 << 20) + 1), QByteArray(16, 'x')); WriteFile(srv, f.constData(), DWORD(f.size()), &n, nullptr); });
    QTRY_VERIFY_WITH_TIMEOUT(!app.isConnected(), 3000);
    w2.join();
    QCOMPARE(app.stats().value("oversizeCloses").toDouble(), 1.0);
    CloseHandle(srv);
}

void TestIpcBinary::previewSubscription() {
    const QString name = uniqueName("sub");
    IpcTransport server(IpcTransport::ServerRole);
    QVERIFY(server.startServer(name));
    QSignalSpy conn(&server, &IpcTransport::clientConnected);
    IpcTransport subUi(IpcTransport::ClientRole), plainUi(IpcTransport::ClientRole);
    subUi.setAcceptBinary(true);
    QHash<QString, int> subGot, plainGot;
    connect(&subUi, &IpcTransport::messageReceived, this, [&](const Message& m, const QString&) { if (m.type == IpcProtocol::MSG_PREVIEW_FRAME) ++subGot[m.channelId]; });
    connect(&plainUi, &IpcTransport::messageReceived, this, [&](const Message& m, const QString&) { if (m.type == IpcProtocol::MSG_PREVIEW_FRAME) ++plainGot[m.channelId]; });
    for (IpcTransport* t : {&subUi, &plainUi}) { t->connectToServerAsync(name, 2000); QTRY_VERIFY_WITH_TIMEOUT(t->isConnected(), 3000); }
    QTRY_COMPARE_WITH_TIMEOUT(conn.count(), 2, 3000);
    const QSet<QString> ui{conn.at(0).at(0).toString(), conn.at(1).at(0).toString()};
    QVERIFY(subUi.subscribePreviews(false, {"perftest", "cam:*"}, 5));
    const auto subscribed = [&server]() {
        for (const auto& v : server.stats().value("peers").toArray()) if (v.toObject().contains("previewSub")) return true;
        return false;
    };
    QTRY_VERIFY_WITH_TIMEOUT(subscribed(), 3000);

    // 100 previews a second for 2 s, rotating over three keys (~33 fps each).
    const QByteArray jpeg = fakeJpeg(20000, 11);
    const QStringList keys{"perftest", "perftest:in1", "cam:a"};
    int sent = 0;
    QTimer gen; gen.setTimerType(Qt::PreciseTimer); gen.setInterval(10);
    connect(&gen, &QTimer::timeout, this, [&]() { server.sendTo(ui, newPreview(keys.at(sent % 3), jpeg), IpcTransport::Droppable); ++sent; });
    QElapsedTimer t; t.start(); gen.start();
    QTRY_VERIFY_WITH_TIMEOUT(t.elapsed() >= 2000, 3000);
    gen.stop();
    QTest::qWait(200);
    const int perKey = sent / 3;
    qInfo().noquote() << QString("sent %1 (%2 per key) in %3 ms; subscribed UI: perftest %4, cam:a %5, perftest:in1 %6; plain UI: %7 / %8 / %9; skips %10")
                             .arg(sent).arg(perKey).arg(t.elapsed()).arg(subGot.value("perftest")).arg(subGot.value("cam:a")).arg(subGot.value("perftest:in1"))
                             .arg(plainGot.value("perftest")).arg(plainGot.value("cam:a")).arg(plainGot.value("perftest:in1"))
                             .arg(server.stats().value("subscriptionSkips").toDouble());
    QCOMPARE(subGot.value("perftest:in1"), 0);   // not subscribed
    for (const char* k : {"perftest", "cam:a"}) {
        QVERIFY2(subGot.value(k) >= 8 && subGot.value(k) <= 13, k);   // 5 fps over ~2 s
    }
    QVERIFY(plainGot.value("perftest") >= perKey - 1);   // no subscription = everything (today)
    QVERIFY(plainGot.value("perftest:in1") >= perKey - 1);
    QVERIFY(server.stats().value("subscriptionSkips").toDouble() > perKey);

    // Back to everything, uncapped.
    subGot.clear();
    QVERIFY(subUi.subscribePreviews(true));
    QTest::qWait(100);
    for (int i = 0; i < 30; ++i) server.sendTo(ui, newPreview(keys.at(i % 3), jpeg), IpcTransport::Droppable);
    QTRY_COMPARE_WITH_TIMEOUT(subGot.value("perftest") + subGot.value("perftest:in1") + subGot.value("cam:a"), 30, 3000);
}

void TestIpcBinary::benchRelay() {
    // 100 frames/s (110 KB JPEGs) for 3 s: engine -> server (own thread, the relay) -> UI. Relay-thread CPU per frame.
    struct Mode { const char* name; bool newEngine, serverAccepts, uiAccepts, newRelay; };
    const QList<Mode> modes = {
        {"pre-PS-5 (JSON/base64 both ways)", false, false, false, false},
        {"PS-5 binary (engine and UI binary)", true, true, true, true},
        {"PS-5 mixed (binary engine, JSON UI = today's app)", true, true, false, true},
    };
    const int frames = 300, bytes = 110 * 1024;
    QList<QByteArray> pics; for (int i = 0; i < 8; ++i) pics << fakeJpeg(bytes, 500 + i);
    QList<double> cyclesPerFrame;
    for (const Mode& md : modes) {
        const QString name = uniqueName("bench");
        LoopThread relay;
        IpcTransport* server = nullptr;
        QSet<QString>* ui = new QSet<QString>;
        QHash<QString, QByteArray>* store = new QHash<QString, QByteArray>;
        std::atomic<int> relayed{0};
        relay.run([&]() {
            server = new IpcTransport(IpcTransport::ServerRole);
            server->setAcceptBinary(md.serverAccepts);
            QVERIFY(server->startServer(name));
            QObject::connect(server, &IpcTransport::messageReceived, server, [&, server, ui, store, md](const Message& m, const QString& cid) {
                if (m.type == IpcProtocol::MSG_UI_QUERY_STATUS) { ui->insert(cid); return; }
                if (m.type != IpcProtocol::MSG_PREVIEW_FRAME) return;
                if (md.newRelay) relayNew(server, *ui, m, *store); else relayOld(server, *ui, m, *store);
                ++relayed;
            });
        });
        IpcTransport uiT(IpcTransport::ClientRole), engine(IpcTransport::ClientRole);
        uiT.setAcceptBinary(md.uiAccepts);
        int uiGot = 0; qint64 uiBytes = 0;
        connect(&uiT, &IpcTransport::messageReceived, this, [&](const Message& m, const QString&) {
            if (m.type == IpcProtocol::MSG_PREVIEW_FRAME) { ++uiGot; uiBytes += IpcProtocol::previewJpeg(m).size(); }
        });
        uiT.connectToServerAsync(name, 2000);
        engine.connectToServerAsync(name, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(uiT.isConnected() && engine.isConnected(), 3000);
        QVERIFY(uiT.send(IpcProtocol::make(IpcProtocol::MSG_UI_QUERY_STATUS, "", {})));
        QTRY_COMPARE_WITH_TIMEOUT(engine.peerAcceptsBinary(), md.serverAccepts, 3000);
        QTest::qWait(150);
        const auto registered = [&relay, ui]() { bool n = false; relay.run([&n, ui]() { n = ui->size() == 1; }); return n; };
        QTRY_VERIFY_WITH_TIMEOUT(registered(), 3000);

        int sent = 0; qint64 sendNs = 0;
        QTimer gen; gen.setTimerType(Qt::PreciseTimer); gen.setInterval(10);
        connect(&gen, &QTimer::timeout, this, [&]() {
            if (sent >= frames) { gen.stop(); return; }
            const QByteArray& j = pics.at(sent % pics.size());
            const QString input = (sent % 4 == 0) ? QString() : QString("in%1").arg(sent % 4);
            QElapsedTimer s; s.start();
            // what engine_main does per picture: before PS-5 it base64-encoded into the JSON payload
            engine.send(md.newEngine ? newPreview("perftest", j, input) : oldPreview("perftest", j, input));
            sendNs += s.nsecsElapsed();
            ++sent;
        });
        const quint64 c0 = relay.cycles(); const double cpu0 = relay.cpuMs();
        QElapsedTimer wall; wall.start();
        gen.start();
        QTRY_VERIFY_WITH_TIMEOUT(sent >= frames && relayed.load() >= frames && uiGot >= frames - 5, 15000);
        QTest::qWait(100);
        const quint64 c1 = relay.cycles(); const double cpu1 = relay.cpuMs();
        const double cpf = double(c1 - c0) / qMax(1, relayed.load());
        cyclesPerFrame << cpf;
        QJsonObject st; relay.run([&]() { st = server->stats(); });
        qInfo().noquote() << QString("%1: %2 frames in %3 ms, relayed %4, UI got %5 (%6 MB of JPEG); relay thread %7 Mcycles/frame "
                                     "(%8 ms CPU total, GetThreadTimes), engine send %9 us/frame; server binaryReceived %10 binarySent %11 fallbacks %12 dropped %13")
                                 .arg(md.name).arg(sent).arg(wall.elapsed()).arg(relayed.load()).arg(uiGot).arg(uiBytes / 1048576.0, 0, 'f', 1)
                                 .arg(cpf / 1e6, 0, 'f', 3).arg(cpu1 - cpu0, 0, 'f', 1).arg(sendNs / 1000.0 / qMax(1, sent), 0, 'f', 1)
                                 .arg(st.value("binaryReceived").toDouble()).arg(st.value("binarySent").toDouble())
                                 .arg(st.value("binaryFallbacks").toDouble()).arg(st.value("dropped").toDouble());
        if (md.newEngine && md.serverAccepts) QCOMPARE(st.value("binaryReceived").toDouble(), double(frames));
        if (md.uiAccepts) QCOMPARE(st.value("binarySent").toDouble(), double(relayed.load()) - st.value("dropped").toDouble());
        uiT.disconnect(); engine.disconnect();
        relay.run([&]() { delete server; server = nullptr; delete ui; delete store; });
    }
    const double ratio = cyclesPerFrame.at(0) / cyclesPerFrame.at(1);
    qInfo().noquote() << QString("relay CPU per frame: pre-PS-5 / binary = %1x; pre-PS-5 / mixed = %2x")
                             .arg(ratio, 0, 'f', 1).arg(cyclesPerFrame.at(0) / cyclesPerFrame.at(2), 0, 'f', 1);
    QVERIFY2(ratio >= 5.0, qPrintable(QString("relay CPU only %1x lower").arg(ratio, 0, 'f', 2)));
    QVERIFY2(cyclesPerFrame.at(2) < cyclesPerFrame.at(0), "the mixed path must not cost more than the old one");
}

QTEST_GUILESS_MAIN(TestIpcBinary)
#include "test_ipcbinary.moc"
