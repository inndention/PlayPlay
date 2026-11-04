// MasterPlay Supervisor wiring tests (the lane design notes §10; slice MP-12): playout/masterplay/mpsupervisor.cpp's
// laneext registrar driven the way SupervisorCore drives it (laneext::installAll on a Context) against a real RestApiServer
// on an ephemeral loopback port and a temp ScheduleStore. Covers: the module is registered under "masterplay"; installAll
// mounts every /api/v1/masterplay route (JSON and the raw download) so RestApiServer::routeKeys() lists them, the station
// copyRoutes carries them over; HTTP GET /api/v1/masterplay answers the lane info over the wire, a POST /import dry run
// answers from the worker, the raw export.in route answers a file, the bearer token is enforced; hostFromContext maps the
// context (ingest enqueue through a real IngestManager, alerts, broadcast); installOn without a store installs nothing.
// Nothing here touches the live stack (port 0, no Supervisor, temp dirs only).
#include <QtTest>
#include <QDir>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimeZone>
#include "../../playout/masterplay/mplane.h"
#include "../../playout/masterplay/mpservice.h"
#include "../../playout/schedule/schedulestore.h"
#include "../../playout/supervisor/ingestmanager.h"
#include "../../playout/supervisor/laneext.h"
#include "../../playout/supervisor/restapiserver.h"

namespace mp { namespace svc {
Host hostFromContext(const laneext::Context& ctx);
MpService* installOn(const laneext::Context& ctx);
} }

using namespace mp;

namespace {

QString S(const char* s) { return QString::fromUtf8(s); }

struct HttpReply {
    int status = 0;
    QByteArray headers, body;
};

/// One HTTP/1.1 request on a fresh loopback socket. The server runs on this same thread, so the wait spins the event
/// loop (QTest::qWait) instead of blocking on the socket; at most 10 s for the full reply (Content-Length or close).
HttpReply http(quint16 port, const QByteArray& method, const QByteArray& path, const QByteArray& body = QByteArray(), const QByteArray& extraHeaders = QByteArray()) {
    HttpReply r;
    QTcpSocket s;
    s.connectToHost(QHostAddress::LocalHost, port);
    QElapsedTimer t; t.start();
    while (s.state() != QAbstractSocket::ConnectedState && t.elapsed() < 3000) QTest::qWait(10);
    if (s.state() != QAbstractSocket::ConnectedState) return r;
    QByteArray req = method + " " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n" + extraHeaders;
    if (!body.isEmpty()) req += "Content-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n";
    req += "\r\n" + body;
    s.write(req);
    QByteArray all;
    while (t.elapsed() < 10000) {
        QTest::qWait(10);
        all += s.readAll();
        const int hdrEnd = all.indexOf("\r\n\r\n");
        if (hdrEnd >= 0) {
            const QByteArray hdr = all.left(hdrEnd).toLower();
            const int cl = hdr.indexOf("content-length:");
            if (cl >= 0) {
                const int len = hdr.mid(cl + 15, hdr.indexOf("\r\n", cl) - cl - 15).trimmed().toInt();
                if (all.size() - hdrEnd - 4 >= len) break;
            }
        }
        if (s.state() != QAbstractSocket::ConnectedState && s.bytesAvailable() == 0) break;
    }
    all += s.readAll();
    const int hdrEnd = all.indexOf("\r\n\r\n");
    if (hdrEnd < 0) return r;
    r.headers = all.left(hdrEnd);
    r.body = all.mid(hdrEnd + 4);
    r.status = r.headers.mid(9, 3).toInt();
    return r;
}

QJsonObject json(const QByteArray& b) { return QJsonDocument::fromJson(b).object(); }
/// MpService has no Q_OBJECT (no moc), so count the SupervisorCore stand-in's service children by dynamic_cast.
int serviceChildren(const QObject& parent) {
    int n = 0;
    for (QObject* c : parent.children()) if (dynamic_cast<svc::MpService*>(c)) ++n;
    return n;
}

/// A Supervisor stand-in: temp store with one channel, a REST server on port 0, the laneext context SupervisorCore builds.
struct Fixture {
    QTemporaryDir dir;
    sched::ScheduleStore store;
    std::unique_ptr<RestApiServer> rest;
    std::unique_ptr<IngestManager> ingest;
    QObject parent;
    QList<QJsonObject> broadcasts;
    QStringList alerts;
    laneext::Context ctx;

    bool init(bool withIngest = false) {
        if (!dir.isValid() || !store.open(dir.filePath(S("sup.db")))) return false;
        sched::ChannelProfile cp;
        cp.id = S("mptest");
        cp.timezone = S("UTC");
        if (!store.saveChannel(cp)) return false;
        rest = std::make_unique<RestApiServer>(nullptr);
        if (!rest->start(0)) return false;
        if (withIngest) ingest = std::make_unique<IngestManager>(&store, dir.path(), dir.filePath(S("uploads")));
        ctx.parent = &parent;
        ctx.rest = rest.get();
        ctx.store = &store;
        ctx.ingest = ingest.get();
        ctx.dataDir = dir.path();
        ctx.restPort = 0;                                   // no afterSave POST to a real Supervisor
        ctx.restToken = []() { return QString(); };
        ctx.alert = [this](const QString& ch, const QString& level, const QString& msg) { alerts << ch + "|" + level + "|" + msg; };
        ctx.broadcast = [this](const QString& type, const QJsonObject& p) { QJsonObject o = p; o["type"] = type; broadcasts << o; };
        return true;
    }
    quint16 port() const { return rest->port(); }
};

} // namespace

class TestMpSupervisor : public QObject {
    Q_OBJECT
private slots:
    void registered() {
        QVERIFY(laneext::registered().contains(S("masterplay")));
    }

    void installAllMountsRoutes() {
        Fixture f;
        QVERIFY(f.init());
        QVERIFY(f.rest->routeKeys().filter(S("/masterplay")).isEmpty());
        const QStringList installed = laneext::installAll(f.ctx);
        QVERIFY2(installed.contains(S("masterplay")), qPrintable(installed.join(',')));
        const QStringList keys = f.rest->routeKeys();
        for (const char* k : {"GET /api/v1/masterplay", "GET /api/v1/masterplay/catalog", "POST /api/v1/masterplay/parse", "POST /api/v1/masterplay/import",
                              "POST /api/v1/masterplay/import/{id}/undo", "GET /api/v1/masterplay/imports", "GET /api/v1/masterplay/imports/{id}", "POST /api/v1/masterplay/export",
                              "GET /api/v1/masterplay/browse", "POST /api/v1/masterplay/asrun/import", "GET /api/v1/masterplay/asrun/state",
                              "GET /api/v1/masterplay/config", "PUT /api/v1/masterplay/config", "POST /api/v1/masterplay/config/from-ini", "POST /api/v1/masterplay/resolve",
                              "GET /api/v1/masterplay/links", "POST /api/v1/masterplay/links", "DELETE /api/v1/masterplay/links/{id}"})
            QVERIFY2(keys.contains(QLatin1String(k)), k);
        QCOMPARE(keys.filter(S("/api/v1/masterplay")).size(), 18);       // the JSON table; export.in is a raw route
        // The service lives under the SupervisorCore stand-in (one child, the MpService).
        QCOMPARE(serviceChildren(f.parent), 1);
        // The station listener copies the table (supervisorcore.cpp): the lane routes travel with it.
        RestApiServer station(nullptr);
        station.copyRoutes(*f.rest);
        QCOMPARE(station.routeKeys().filter(S("/api/v1/masterplay")).size(), 18);
    }

    void overTheWire() {
        Fixture f;
        QVERIFY(f.init());
        laneext::installAll(f.ctx);
        // Info route, synchronous.
        HttpReply r = http(f.port(), "GET", "/api/v1/masterplay");
        QCOMPARE(r.status, 200);
        QJsonObject o = json(r.body);
        QCOMPARE(o.value("lane").toString(), S("masterplay"));
        QCOMPARE(o.value("apiVersion").toInt(), kApiVersion);
        QCOMPARE(o.value("routes").toArray().size(), 19);
        QVERIFY(o.value("dryRunByDefault").toBool());
        // Catalog.
        r = http(f.port(), "GET", "/api/v1/masterplay/catalog");
        QCOMPARE(r.status, 200);
        QVERIFY(json(r.body).value("instructions").toArray().size() > 20);
        // An async worker route: a dry-run import of pasted text answers 200 with a plan and writes nothing.
        const QByteArray body = QJsonDocument(QJsonObject{{"text", S("0000-00-00T00:00:00\tName=Wire\tEventDuration=01:00:00\tPlayMode=1\r\n!Repeat\r\nD:\\Media\\Prog\\Movie.mpg\t[00:30:00]\r\nD:\\Media\\Prog\\Talk.mpg\t[00:30:00]\r\n")}, {"name", "wire.in"}, {"channel", "mptest"},
                                                          {"mode", "dates"}, {"from", "2031-01-05"}, {"to", "2031-01-05"},
                                                          {"options", QJsonObject{{"missing", "keepPath"}, {"timeZone", "UTC"}}}}).toJson(QJsonDocument::Compact);
        const quint64 rev = f.store.revision();
        r = http(f.port(), "POST", "/api/v1/masterplay/import", body);
        QCOMPARE(r.status, 200);
        o = json(r.body);
        QVERIFY2(o.value("dryRun").toBool(), qPrintable(QString::fromUtf8(r.body.left(300))));
        QCOMPARE(o.value("plan").toObject().value("slots").toObject().size(), 24);
        QCOMPARE(f.store.revision(), rev);                         // a dry run writes nothing
        QCOMPARE(f.store.templateSlots(S("mptest")).size(), 0);
        // {id} segment routes dispatch through the trie.
        r = http(f.port(), "GET", "/api/v1/masterplay/imports/nope");
        QCOMPARE(r.status, 404);
        // The raw download route owns its socket and answers a file (an error body when nothing was planned is still a file reply).
        r = http(f.port(), "GET", "/api/v1/masterplay/export.in?channel=mptest&mode=dates&from=2031-01-05&to=2031-01-05");
        QVERIFY2(r.status == 200 || r.status >= 400, qPrintable(QString::number(r.status)));
        QVERIFY(r.headers.toLower().contains("content-type:"));
        // Unknown route under the prefix.
        r = http(f.port(), "GET", "/api/v1/masterplay/nothing");
        QCOMPARE(r.status, 404);
    }

    void bearerTokenEnforced() {
        Fixture f;
        QVERIFY(f.init());
        laneext::installAll(f.ctx);
        f.rest->setAuthToken(S("secret"));
        QCOMPARE(http(f.port(), "GET", "/api/v1/masterplay").status, 401);
        QCOMPARE(http(f.port(), "GET", "/api/v1/masterplay", QByteArray(), "Authorization: Bearer secret\r\n").status, 200);
    }

    void hostMapping() {
        Fixture f;
        QVERIFY(f.init(true));
        svc::Host h = svc::hostFromContext(f.ctx);
        QCOMPARE(h.store, &f.store);
        QCOMPARE(h.dataDir, f.dir.path());
        QVERIFY(h.restToken);
        QVERIFY(h.enqueueIngest);
        QVERIFY(!h.afterSave);                                  // the service default
        QVERIFY(!h.meshConfig);                                 // the store's mesh.* settings, read per call
        QCOMPARE(h.nowMs, qint64(0));                           // the wall clock
        h.alert(S("mptest"), S("warn"), S("hello"));
        QCOMPARE(f.alerts, QStringList{S("mptest|warn|hello")});
        h.broadcast(S("masterplay"), QJsonObject{{"x", 1}});
        QCOMPARE(f.broadcasts.size(), 1);
        QCOMPARE(f.broadcasts[0].value("type").toString(), S("masterplay"));
        // enqueueIngest reaches the IngestManager with origin "masterplay".
        const QString missing = f.dir.filePath(S("missing.mpg"));
        const QJsonObject job = h.enqueueIngest(missing, S("program"), QJsonObject{{"importId", "i1"}});
        QVERIFY2(!job.isEmpty(), "enqueue answered nothing");
        QCOMPARE(job.value("origin").toString(), S("masterplay"));
        QCOMPARE(job.value("path").toString(), QDir::toNativeSeparators(missing));
        // Without an IngestManager the hook is unset (the service reports the missing list only).
        laneext::Context noIng = f.ctx;
        noIng.ingest = nullptr;
        QVERIFY(!svc::hostFromContext(noIng).enqueueIngest);
    }

    void installOnRefusesWithoutStore() {
        Fixture f;
        QVERIFY(f.init());
        laneext::Context c = f.ctx;
        c.store = nullptr;
        QVERIFY(svc::installOn(c) == nullptr);
        QVERIFY(f.rest->routeKeys().filter(S("/masterplay")).isEmpty());
        c = f.ctx;
        c.rest = nullptr;
        QVERIFY(svc::installOn(c) == nullptr);
        QCOMPARE(serviceChildren(f.parent), 0);
    }
};

QTEST_MAIN(TestMpSupervisor)
#include "test_mpsupervisor.moc"
