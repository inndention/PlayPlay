// ST-01 smoke test (docs/plans/starmedia-studio.md §8): the Studio service starts on an ephemeral loopback port and
// answers GET /health in the spot-node shape RenderFarm probes; it refuses other services' ports, a non-loopback bind
// and the Supervisor's data dir before touching a port or the disk; PlayPlayStudio.exe --version prints the version and
// the exe itself serves /health. Never binds a reserved port (the guards are checked through checkOptions only).
// Since ST-04 the service runs on the net/ HTTP server (keep-alive, router, token): the protocol details are covered by
// test_studio_http; the raw requests here send "Connection: close" so the server ends each exchange.
#include <QtTest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QTemporaryDir>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "app/studioservice.h"

namespace {

struct Got {
    int status = 0;
    QByteArray body;
    QByteArray type;
};

Got request(const QByteArray& verb, const QUrl& url, const QByteArray& body = {}) {
    QNetworkAccessManager nam;
    QNetworkRequest rq(url);
    rq.setTransferTimeout(5000);
    if (!body.isEmpty()) rq.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
    QNetworkReply* r = nam.sendCustomRequest(rq, verb, body);
    if (!r->isFinished()) {
        QSignalSpy done(r, &QNetworkReply::finished);
        done.wait(8000);
    }
    Got g;
    g.status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    g.body = r->isOpen() ? r->readAll() : QByteArray();   // QNAM closes the device of a bodiless HEAD reply
    g.type = r->header(QNetworkRequest::ContentTypeHeader).toByteArray();
    delete r;
    return g;
}

// Raw socket exchange: write `chunks` (pausing between them), then read until the server closes. The server runs on
// this thread, so every wait must spin the event loop (qWait / qWaitFor), never block in waitFor*().
QByteArray raw(quint16 port, const QList<QByteArray>& chunks, int pauseMs = 0) {
    QTcpSocket s;
    QByteArray out;
    QObject::connect(&s, &QTcpSocket::readyRead, &s, [&]() { out += s.readAll(); });
    s.connectToHost(QHostAddress::LocalHost, port);
    if (!QTest::qWaitFor([&]() { return s.state() == QAbstractSocket::ConnectedState; }, 3000)) return {};
    for (const QByteArray& c : chunks) {
        s.write(c);
        QTest::qWait(pauseMs ? pauseMs : 1);
    }
    (void)QTest::qWaitFor([&]() { return s.state() == QAbstractSocket::UnconnectedState; }, 5000);   // the answer is checked by the caller
    out += s.readAll();
    return out;
}

int statusOf(const QByteArray& response) {
    const QList<QByteArray> parts = response.left(response.indexOf("\r\n")).split(' ');
    return parts.size() >= 2 ? parts[1].toInt() : 0;
}

} // namespace

class TestStudioSmoke : public QObject {
    Q_OBJECT
    QTemporaryDir m_tmp;
    studio::StudioService m_svc;
    QUrl url(const QString& path) const { return QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(m_svc.port()).arg(path)); }

private slots:
    void initTestCase() {
        QVERIFY(m_tmp.isValid());
        studio::ServiceOptions opt;
        opt.port = 0;
        opt.wsPort = 0;                                  // never the default 7761 in a test
        opt.dataDir = m_tmp.filePath(QStringLiteral("svc"));
        QString err;
        QVERIFY2(m_svc.start(opt, &err), qPrintable(err));
        QVERIFY(m_svc.wsPort() > 0);
        QVERIFY(QFileInfo::exists(m_svc.tokenPath()));
        QVERIFY(m_svc.isListening());
        QVERIFY(m_svc.port() > 0);
        QVERIFY(QDir(opt.dataDir).exists());
        QString again;
        QVERIFY(!m_svc.start(opt, &again));              // a second start is refused, the listener stays
        QVERIFY(m_svc.isListening());
    }

    void healthShape() {
        for (const QString& path : {QStringLiteral("/health"), QStringLiteral("/health?probe=1")}) {
            const Got g = request("GET", url(path));
            QCOMPARE(g.status, 200);
            QVERIFY(g.type.startsWith("application/json"));
            QJsonParseError pe;
            const QJsonObject o = QJsonDocument::fromJson(g.body, &pe).object();
            QCOMPARE(pe.error, QJsonParseError::NoError);
            QCOMPARE(o.value("ok").toBool(), true);
            QCOMPARE(o.value("service").toString(), QStringLiteral("playplay-studio"));
            QCOMPARE(o.value("version").toString(), QStringLiteral(PLAYPLAY_STUDIO_VERSION));
            // RenderFarm reads capable (bool) and queue{running, queued, max}; a skeleton is honestly not capable.
            QVERIFY(o.value("capable").isBool());
            QCOMPARE(o.value("capable").toBool(), false);
            const QJsonObject q = o.value("queue").toObject();
            QVERIFY(q.contains("running") && q.contains("queued") && q.contains("max"));
            QVERIFY(o.value("uptimeMs").toDouble() >= 0);
        }
        QCOMPARE(m_svc.health().value("service").toString(), studio::serviceName());
    }

    void routes() {
        const Got home = request("GET", url("/"));
        QCOMPARE(home.status, 200);
        QVERIFY(home.type.startsWith("text/html"));
        QVERIFY(home.body.contains("PlayPlayStudio " PLAYPLAY_STUDIO_VERSION));
        QVERIFY(!home.body.contains("{{version}}"));

        const Got missing = request("GET", url("/nope"));
        QCOMPARE(missing.status, 404);
        QCOMPARE(QJsonDocument::fromJson(missing.body).object().value("ok").toBool(true), false);

        QCOMPARE(request("POST", url("/health"), "{}").status, 405);
        const Got head = request("HEAD", url("/health"));
        QCOMPARE(head.status, 200);
        QVERIFY(head.body.isEmpty());
    }

    void rawProtocol() {
        // A head split over two writes is answered once it is complete; "Connection: close" is honoured.
        const QByteArray split = raw(m_svc.port(), {"GET /health HTTP/1.1\r\nHo", "st: 127.0.0.1\r\nConnection: close\r\n\r\n"}, 150);
        QCOMPARE(statusOf(split), 200);
        QVERIFY(split.contains("Connection: close"));
        QCOMPARE(statusOf(raw(m_svc.port(), {"HELLO\r\n\r\n"})), 400);
        QCOMPARE(statusOf(raw(m_svc.port(), {"GET health HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"})), 400);
        // A head that never ends is cut off at 16 KiB instead of buffering forever.
        const QByteArray huge = "GET /health HTTP/1.1\r\nX-Pad: " + QByteArray(20 * 1024, 'a');
        QCOMPARE(statusOf(raw(m_svc.port(), {huge})), 431);

        // A small declared body is read before the answer (closing on unread data makes Windows send a RST that can eat
        // the reply): nothing comes back while the body is still arriving, then one complete 405.
        {
            QTcpSocket s;
            QByteArray got;
            QObject::connect(&s, &QTcpSocket::readyRead, &s, [&]() { got += s.readAll(); });
            s.connectToHost(QHostAddress::LocalHost, m_svc.port());
            QVERIFY(QTest::qWaitFor([&]() { return s.state() == QAbstractSocket::ConnectedState; }, 3000));
            s.write("POST /health HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\nContent-Length: 10\r\n\r\n01234");
            QTest::qWait(250);
            QVERIFY2(got.isEmpty(), got.constData());
            s.write("56789");
            QVERIFY(QTest::qWaitFor([&]() { return s.state() == QAbstractSocket::UnconnectedState; }, 5000));
            got += s.readAll();
            QCOMPARE(statusOf(got), 405);
            QVERIFY(got.contains("Allow: GET, HEAD"));
            QVERIFY(got.endsWith("}"));                               // the JSON body arrived whole
        }
        QCOMPARE(statusOf(raw(m_svc.port(), {"GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\ncontent-length: 3\r\n\r\n", "abc"}, 100)), 200);
        QCOMPARE(statusOf(raw(m_svc.port(), {"POST /x HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: abc\r\n\r\n"})), 400);
        QCOMPARE(statusOf(raw(m_svc.port(), {"POST /x HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n"})), 400);
        // Routing comes before the body: an unknown route is refused at once, and a large unread body closes the
        // connection after the answer.
        QCOMPARE(statusOf(raw(m_svc.port(), {"POST /x HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 2000000\r\n\r\n"})), 404);
        // Token routes refuse without the token; the token file opens them.
        QCOMPARE(request("GET", url("/api/v1/status")).status, 401);
        QString terr;
        const QString token = studio::net::Auth::readTokenFile(m_svc.tokenPath(), &terr);
        QVERIFY2(!token.isEmpty(), qPrintable(terr));
        {
            QNetworkAccessManager nam;
            QNetworkRequest rq(url("/api/v1/status"));
            rq.setRawHeader("Authorization", "Bearer " + token.toLatin1());
            QNetworkReply* r = nam.get(rq);
            QSignalSpy done(r, &QNetworkReply::finished);
            QVERIFY(done.wait(8000));
            QCOMPARE(r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
            const QJsonObject st = QJsonDocument::fromJson(r->readAll()).object();
            QCOMPARE(st.value("service").toString(), QStringLiteral("playplay-studio"));
            QVERIFY(st.value("http").toObject().value("requests").toDouble() > 0);
            QVERIFY(st.value("ws").toObject().value("listening").toBool());
            delete r;
        }
        // The service still answers afterwards.
        QCOMPARE(request("GET", url("/health")).status, 200);
    }

    void guards() {
        // The environment overrides below are undone on every exit, a failed QVERIFY included, so the exe tests that
        // follow (whose children inherit the environment) never run with them.
        const bool hadAppData = qEnvironmentVariableIsSet("APPDATA"), hadData = qEnvironmentVariableIsSet("PLAYPLAY_STUDIO_DATA");
        const QByteArray savedAppData = qgetenv("APPDATA"), savedData = qgetenv("PLAYPLAY_STUDIO_DATA");
        const auto restoreEnv = qScopeGuard([&]() {
            if (hadAppData) qputenv("APPDATA", savedAppData); else qunsetenv("APPDATA");
            if (hadData) qputenv("PLAYPLAY_STUDIO_DATA", savedData); else qunsetenv("PLAYPLAY_STUDIO_DATA");
        });

        for (quint16 p : {443, 7700, 7701, 7709, 7710, 7711, 7797, 7798, 7799, 8080, 8775, 8899, 7827, 7833, 9000, 9001, 9999, 11434}) {
            QVERIFY2(studio::reservedPort(p), qPrintable(QString::number(p)));
            studio::ServiceOptions o;
            o.port = p;
            o.dataDir = m_tmp.filePath(QStringLiteral("g"));
            QVERIFY(studio::checkOptions(o).contains(QString::number(p)));
        }
        QVERIFY(!studio::reservedPort(studio::kDefaultRestPort));
        QVERIFY(!studio::reservedPort(studio::kDefaultWsPort));
        QVERIFY(!studio::reservedPort(7770));
        QVERIFY(!studio::reservedPort(7771));
        QVERIFY(!studio::reservedPort(0));

        // Non-loopback bind: refused by start() itself, before a port is bound or a dir is made.
        studio::StudioService other;
        studio::ServiceOptions any;
        any.port = 0;
        any.bind = QHostAddress(QHostAddress::Any);
        any.dataDir = m_tmp.filePath(QStringLiteral("any"));
        QString err;
        QVERIFY(!other.start(any, &err));
        QVERIFY(err.contains(QStringLiteral("loopback")));
        QVERIFY(!other.isListening());
        QVERIFY(!QDir(any.dataDir).exists());
        any.bind = QHostAddress(QHostAddress::LocalHostIPv6);
        QVERIFY(studio::checkOptions(any).isEmpty());

        // The Supervisor's data tree (%APPDATA%/PlayPlay): APPDATA is pointed at the temp dir so a broken guard could
        // only ever write there.
        qputenv("APPDATA", QDir::toNativeSeparators(m_tmp.filePath(QStringLiteral("appdata"))).toLocal8Bit());
        const QString live = m_tmp.filePath(QStringLiteral("appdata/PlayPlay/PLAY PLAY/supervisor/studio"));
        QVERIFY(studio::insideSupervisorData(live));
        QVERIFY(studio::insideSupervisorData(QDir::toNativeSeparators(m_tmp.filePath(QStringLiteral("appdata/playplay")))));
        QVERIFY(!studio::insideSupervisorData(m_tmp.filePath(QStringLiteral("appdata/PlayPlayStudio"))));
        studio::ServiceOptions inLive;
        inLive.port = 0;
        inLive.dataDir = live;
        QVERIFY(!other.start(inLive, &err));
        QVERIFY(err.contains(QStringLiteral("Supervisor")));
        QVERIFY(!QDir(live).exists());

#ifdef Q_OS_WIN
        // Another name for the same tree must not get past the guard: a junction (no admin rights needed; it points
        // into the temp dir, so nothing outside it is reachable) and an 8.3 short name ("PLAY PLAY" has one when the
        // volume generates them).
        const QString tree = m_tmp.filePath(QStringLiteral("appdata/PlayPlay/PLAY PLAY"));
        QVERIFY(QDir().mkpath(tree));
        const QString link = m_tmp.filePath(QStringLiteral("alias"));
        QProcess mk;
        mk.start(QStringLiteral("cmd.exe"), {QStringLiteral("/c"), QStringLiteral("mklink"), QStringLiteral("/J"), QDir::toNativeSeparators(link),
                                             QDir::toNativeSeparators(m_tmp.filePath(QStringLiteral("appdata/PlayPlay")))});
        if (mk.waitForFinished(10000) && mk.exitStatus() == QProcess::NormalExit && mk.exitCode() == 0 && QFileInfo::exists(link)) {
            const auto dropLink = qScopeGuard([&]() { QDir().rmdir(link); });   // removes the junction, never its target
            QVERIFY(studio::insideSupervisorData(link));
            QVERIFY(studio::insideSupervisorData(link + QStringLiteral("/PLAY PLAY/supervisor/studio")));
            inLive.dataDir = link + QStringLiteral("/studio");
            QVERIFY(!other.start(inLive, &err));
            QVERIFY(!QFileInfo::exists(inLive.dataDir));
        } else {
            qWarning("mklink /J failed (%s); junction case not checked", mk.readAllStandardError().constData());
        }
        const std::wstring longName = QDir::toNativeSeparators(tree).toStdWString();
        wchar_t shortBuf[1024] = {};
        const DWORD n = GetShortPathNameW(longName.c_str(), shortBuf, DWORD(std::size(shortBuf)));
        const QString shortName = QString::fromWCharArray(shortBuf, int(n < std::size(shortBuf) ? n : 0));
        if (!shortName.isEmpty() && shortName.compare(QString::fromStdWString(longName), Qt::CaseInsensitive) != 0)
            QVERIFY2(studio::insideSupervisorData(shortName + QStringLiteral("\\studio")), qPrintable(shortName));
        else
            qWarning("no 8.3 short name for %s; short-name case not checked", qPrintable(tree));
#endif

        // Data dir default: PLAYPLAY_STUDIO_DATA wins.
        qputenv("PLAYPLAY_STUDIO_DATA", m_tmp.filePath(QStringLiteral("envdata")).toLocal8Bit());
        QCOMPARE(studio::defaultDataDir(), QDir::cleanPath(m_tmp.filePath(QStringLiteral("envdata"))));
    }

    void appIdentity() {
        // The exe's organization keeps QStandardPaths / QSettings out of the Supervisor's tree. Read-only: nothing here
        // creates a directory.
        studio::setAppIdentity();
        QCOMPARE(QCoreApplication::organizationName(), QStringLiteral("PlayPlayStudio"));
        QCOMPARE(QCoreApplication::applicationName(), QStringLiteral("PlayPlayStudio"));
        QCOMPARE(QCoreApplication::applicationVersion(), studio::version());
        const QString appData = qEnvironmentVariable("APPDATA");
        if (appData.isEmpty()) QSKIP("APPDATA is not set");
        QVERIFY(studio::insideSupervisorData(appData + QStringLiteral("/PlayPlay/PlayPlayStudio")));   // the old identity
        for (auto loc : {QStandardPaths::AppDataLocation, QStandardPaths::AppLocalDataLocation, QStandardPaths::AppConfigLocation}) {
            const QString p = QStandardPaths::writableLocation(loc);
            QVERIFY(!p.isEmpty());
            QVERIFY2(!studio::insideSupervisorData(p), qPrintable(p));
        }
    }

    void exeVersion() {
        QProcess p;
        p.start(QStringLiteral(STUDIO_EXE_PATH), {QStringLiteral("--version")});
        QVERIFY2(p.waitForFinished(15000), qPrintable(p.errorString()));
        QCOMPARE(p.exitCode(), 0);
        QVERIFY2(p.readAllStandardOutput().contains(PLAYPLAY_STUDIO_VERSION), "version missing from --version output");

        QProcess help;
        help.start(QStringLiteral(STUDIO_EXE_PATH), {QStringLiteral("--help")});
        QVERIFY(help.waitForFinished(15000));
        QCOMPARE(help.exitCode(), 0);
        QVERIFY(help.readAllStandardOutput().contains("--port"));

        // Bad arguments exit 2 with the reason on stderr (never a message box, never a listener).
        for (const QStringList& args : {QStringList{QStringLiteral("--port"), QStringLiteral("70000")}, QStringList{QStringLiteral("--bogus")},
                                        QStringList{QStringLiteral("--bind"), QStringLiteral("not-an-address")}}) {
            QProcess bad;
            bad.start(QStringLiteral(STUDIO_EXE_PATH), args);
            QVERIFY(bad.waitForFinished(15000));
            QCOMPARE(bad.exitCode(), 2);
            QVERIFY(!bad.readAllStandardError().isEmpty());
        }
    }

    void exeServesHealth() {
        const QString data = m_tmp.filePath(QStringLiteral("exe"));
        QProcess p;
        p.setProcessChannelMode(QProcess::SeparateChannels);
        p.start(QStringLiteral(STUDIO_EXE_PATH), {QStringLiteral("--port"), QStringLiteral("0"), QStringLiteral("--data"), data});
        QVERIFY2(p.waitForStarted(10000), qPrintable(p.errorString()));
        QByteArray out;
        QElapsedTimer t;
        t.start();
        while (!out.contains('\n') && t.elapsed() < 15000 && p.state() == QProcess::Running) {
            if (p.waitForReadyRead(250)) out += p.readAllStandardOutput();
        }
        const QRegularExpressionMatch m = QRegularExpression(QStringLiteral("^LISTENING http://127\\.0\\.0\\.1:(\\d+) ws=ws://127\\.0\\.0\\.1:(\\d+) data=(.+)$"),
                                                             QRegularExpression::MultilineOption).match(QString::fromLocal8Bit(out).trimmed());
        if (!m.hasMatch()) {
            p.kill();
            p.waitForFinished(5000);
            QFAIL(qPrintable(QStringLiteral("no LISTENING line; stdout=%1 stderr=%2").arg(QString::fromLocal8Bit(out), QString::fromLocal8Bit(p.readAllStandardError()))));
        }
        const int port = m.captured(1).toInt();
        QVERIFY(port > 0);
        QVERIFY(QDir(data).exists());
        const Got g = request("GET", QUrl(QStringLiteral("http://127.0.0.1:%1/health").arg(port)));
        p.kill();                                        // our own child, started above
        p.waitForFinished(5000);
        QCOMPARE(g.status, 200);
        QCOMPARE(QJsonDocument::fromJson(g.body).object().value("service").toString(), QStringLiteral("playplay-studio"));
    }

    void cleanupTestCase() {
        m_svc.stop();
        QVERIFY(!m_svc.isListening());
        QCOMPARE(m_svc.port(), quint16(0));
    }
};

QTEST_GUILESS_MAIN(TestStudioSmoke)
#include "test_studio_smoke.moc"
