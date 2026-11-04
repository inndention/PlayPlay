// MasterPlay REST service tests (the lane design notes §10; slice MP-11): the /api/v1/masterplay route table of
// playout/masterplay/mpservice.h driven through MpService::call() the way RestApiServer would dispatch it, against a
// temp ScheduleStore holding the synthetic library of test_mpimport.cpp and the fixtures as a file-system source. Covers:
// the route keys and info / catalog; config validation and round trip; parse (text, source + path, errors); browse
// (quick stats, dated names, sub folders, errors); import dry run -> apply -> history -> undo, the on-air 409, missing =
// ingest enqueue through the host; export (POST, the download bytes and the raw file route); as-run dry run and write
// with the Status.log reasons and the per-source state; resolve; config/from-ini without secrets; links CRUD; mesh reads
// (browse, parse, an import whose sub-playlist is fetched) against a mock AN-FS bridge on 127.0.0.1, with the bearer
// token. Nothing here touches the live stack or the real mesh.
#include <QtTest>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QUrl>
#include <QUrlQuery>
#include <memory>
#include "../../playout/masterplay/mplane.h"
#include "../../playout/masterplay/mpservice.h"
#include "../../playout/schedule/schedulestore.h"

using namespace mp;
using sched::MediaRecord;
using svc::Query;

namespace {

QString S(const char* s) { return QString::fromUtf8(s); }
QString fxDir() { return QDir::toNativeSeparators(QDir(QStringLiteral(MP_FIXTURES)).absolutePath()); }
QString fx(const QString& name) { return QDir::toNativeSeparators(QDir(QStringLiteral(MP_FIXTURES)).filePath(name)); }
QByteArray readAll(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

const qint64 kNow = QDateTime(QDate(2025, 9, 1), QTime(12, 0), QTimeZone::UTC).toMSecsSinceEpoch();   // a Monday, noon UTC
const qint64 kDay = 86400000;

MediaRecord rec(const char* id, const QString& path, const QString& title, const char* type, double dur) {
    MediaRecord m;
    m.id = QString::fromLatin1(id);
    m.path = path;
    m.title = title;
    m.type = QString::fromLatin1(type);
    m.durationSec = dur;
    m.addedAtMs = kNow - 30 * kDay;
    return m;
}
/// The library import_times.in names (test_mpimport.cpp's importLibrary()).
QList<MediaRecord> importLibrary() {
    return {
        rec("movie", S(R"(D:\Media\Prog\Movie.mpg)"), S("Movie"), "program", 5400),
        rec("news", S(R"(D:\Media\Prog\News Hour.mpg)"), S("News Hour"), "news", 2700),
        rec("morning", S(R"(D:\Media\Prog\Morning Show.mpg)"), S("Morning Show"), "program", 1800),
        rec("doc", S(R"(D:\Media\Prog\Doc.mpg)"), S("Doc"), "program", 1500),
        rec("talk", S(R"(D:\Media\Prog\Talk.mpg)"), S("Talk"), "program", 600),
        rec("weather", S(R"(D:\Media\Prog\Weather.mpg)"), S("Weather"), "weather", 300),
        rec("spotA", S(R"(D:\Media\Spots\Spot A.mpg)"), S("Spot A"), "ad", 30),
        rec("spotB", S(R"(D:\Media\Spots\Spot B.mpg)"), S("Spot B"), "ad", 60),
        rec("card", S(R"(D:\Media\Stills\Card.png)"), S("Card"), "image", 10),
    };
}

/// A mock AN-FS bridge: GET /api/browse?path= lists a local folder, GET /api/download?path= serves a local file. It records
/// every request line and the Authorization header it saw.
class MockBridge : public QObject {
public:
    QTcpServer server;
    QStringList requests, auth;
    bool fail404 = false;
    MockBridge() {
        connect(&server, &QTcpServer::newConnection, this, [this]() {
            while (QTcpSocket* s = server.nextPendingConnection()) {
                connect(s, &QTcpSocket::readyRead, this, [this, s]() { handle(s); });
                connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
            }
        });
    }
    bool start() { return server.listen(QHostAddress::LocalHost, 0); }
    QString url() const { return QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()); }
    void handle(QTcpSocket* s) {
        const QByteArray req = s->peek(1 << 20);
        const int end = req.indexOf("\r\n\r\n");
        if (end < 0) return;
        s->read(end + 4);
        const QList<QByteArray> lines = req.left(end).split('\n');
        const QList<QByteArray> parts = lines.value(0).trimmed().split(' ');
        for (const QByteArray& l : lines) if (l.toLower().startsWith("authorization:")) auth << QString::fromUtf8(l.mid(14).trimmed());
        const QUrl url(QString::fromUtf8(parts.value(1)));
        const QUrlQuery q(url);
        requests << url.path() + QLatin1Char('?') + q.toString(QUrl::FullyDecoded);
        int status = 404;
        QByteArray body = "{\"error\":\"not found\"}", type = "application/json";
        const QString path = q.queryItemValue(QStringLiteral("path"), QUrl::FullyDecoded);
        if (!fail404 && url.path() == QLatin1String("/api/browse")) {
            const QDir d(path);
            if (d.exists()) {
                QJsonArray items;
                for (const QFileInfo& fi : d.entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot))
                    items << QJsonObject{{"name", fi.fileName()}, {"path", QDir::toNativeSeparators(fi.absoluteFilePath())}, {"isDirectory", fi.isDir()}, {"size", double(fi.size())},
                                         {"mtime", fi.lastModified().toUTC().toString(Qt::ISODate)}};
                status = 200;
                body = QJsonDocument(QJsonObject{{"currentPath", path}, {"items", items}}).toJson(QJsonDocument::Compact);
            }
        } else if (!fail404 && url.path() == QLatin1String("/api/download")) {
            QFile f(path);
            if (f.open(QIODevice::ReadOnly)) { status = 200; body = f.readAll(); type = "application/octet-stream"; }
        }
        s->write("HTTP/1.1 " + QByteArray::number(status) + (status == 200 ? " OK" : " Not Found") + "\r\nContent-Type: " + type + "\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        s->flush();
        s->disconnectFromHost();
    }
};

struct Fixture {
    QTemporaryDir dir;
    sched::ScheduleStore store;
    std::unique_ptr<svc::MpService> svc;
    int afterSaveCalls = 0;
    QList<QJsonObject> broadcasts, enqueued;
    MockBridge bridge;

    bool init(bool meshUp = true) {
        if (!dir.isValid() || !store.open(dir.filePath(S("service.db")))) return false;
        sched::ChannelProfile cp;
        cp.id = S("mptest");
        cp.timezone = S("UTC");
        if (!store.saveChannel(cp)) return false;
        for (const MediaRecord& m : importLibrary()) if (!store.saveMedia(m)) return false;
        if (meshUp && !bridge.start()) return false;
        svc::Host h;
        h.store = &store;
        h.dataDir = dir.path();
        h.nowMs = kNow;
        h.afterSave = [this]() { ++afterSaveCalls; };
        h.broadcast = [this](const QString& type, const QJsonObject& p) { QJsonObject o = p; o["type"] = type; broadcasts << o; };
        h.enqueueIngest = [this](const QString& path, const QString& type, const QJsonObject& meta) { QJsonObject j{{"id", S("job-") + QString::number(enqueued.size() + 1)}, {"path", path}, {"type", type}, {"meta", meta}}; enqueued << j; return j; };
        h.meshConfig = [this]() { return QJsonObject{{"url", bridge.url()}, {"token", "tok"}}; };
        h.meshTimeoutMs = 5000;
        svc = std::make_unique<svc::MpService>(h);
        return true;
    }
    /// The two sources every test uses: "fx" (file system) and "mesh" (through the mock bridge), the fixtures as C:\Sync\NEWS HUB.
    bool configure() {
        QString e;
        const QJsonObject c = svc->setConfig(QJsonObject{
            {"sources", QJsonArray{QJsonObject{{"id", "fx"}, {"label", "fixtures"}, {"read", "fs"}, {"playlistDir", fxDir()}, {"logPath", fx(S("LogFile.log"))}, {"osdDir", fxDir()}, {"tz", "America/New_York"}},
                                   QJsonObject{{"id", "mesh"}, {"label", "mock bridge"}, {"read", "mesh"}, {"node", "n1"}, {"playlistDir", fxDir()}, {"logPath", fx(S("LogFile.log"))}, {"tz", "America/New_York"}}}},
            {"pathMap", QJsonArray{QJsonObject{{"from", S(R"(C:\Sync\NEWS HUB)")}, {"to", fxDir()}, {"origin", "user"}}}},
            {"defaults", QJsonObject{{"timeZone", "UTC"}, {"fps", 30}}},
            {"resolver", QJsonObject{{"hash", false}, {"probe", false}}},
            {"asrun", QJsonObject{{"tz", "America/New_York"}}}}, &e);
        return e.isEmpty() && !c.isEmpty();
    }
    int call(const QString& method, const QString& path, QJsonObject* out, const QJsonObject& body = {}, const Query& q = {}) {
        return svc->callSync(method, QString::fromLatin1(svc::MpService::kPrefix) + path, body, out, q, 60000);
    }
};

QJsonObject importBody(const QString& date, const QString& channel = S("mptest")) {
    return QJsonObject{{"source", "fx"}, {"path", "import_times.in"}, {"channel", channel}, {"mode", "dates"}, {"from", date}, {"to", date}};
}

} // namespace

class TestMpService : public QObject {
    Q_OBJECT
private slots:
    void routesAndInfo() {
        Fixture f;
        QVERIFY(f.init(false));
        const QStringList keys = f.svc->routeKeys();
        for (const char* k : {"GET /api/v1/masterplay", "GET /api/v1/masterplay/catalog", "POST /api/v1/masterplay/parse", "POST /api/v1/masterplay/import",
                              "POST /api/v1/masterplay/import/{id}/undo", "GET /api/v1/masterplay/imports", "GET /api/v1/masterplay/imports/{id}", "POST /api/v1/masterplay/export",
                              "GET /api/v1/masterplay/export.in", "GET /api/v1/masterplay/browse", "POST /api/v1/masterplay/asrun/import", "GET /api/v1/masterplay/asrun/state",
                              "GET /api/v1/masterplay/config", "PUT /api/v1/masterplay/config", "POST /api/v1/masterplay/config/from-ini", "POST /api/v1/masterplay/resolve",
                              "GET /api/v1/masterplay/links", "POST /api/v1/masterplay/links", "DELETE /api/v1/masterplay/links/{id}"})
            QVERIFY2(keys.contains(QLatin1String(k)), k);
        QCOMPARE(keys.size(), 19);
        QCOMPARE(f.svc->routes().size(), 18);   // the download route is raw, not in the JSON table
        QJsonObject o;
        QCOMPARE(f.call("GET", "", &o), 200);
        QCOMPARE(o.value("lane").toString(), S("masterplay"));
        QCOMPARE(o.value("apiVersion").toInt(), kApiVersion);
        QCOMPARE(o.value("routes").toArray().size(), 19);
        QVERIFY(o.value("dryRunByDefault").toBool());
        QCOMPARE(o.value("pending").toInt(), 0);
        QCOMPARE(f.call("GET", "/catalog", &o), 200);
        QVERIFY(o.value("instructions").toArray().size() > 20);
        QCOMPARE(o.value("diagnostics").toArray().size(), diagnosticCatalog().size());
        QCOMPARE(f.call("GET", "/nothing", &o), 0);           // no route
        QCOMPARE(f.call("POST", "/catalog", &o), 0);          // wrong method
        QCOMPARE(f.call("GET", "/export.in", &o), 0);         // raw, not JSON
        QCOMPARE(f.call("POST", "/import/abc/undo/x", &o), 0);
    }

    void configRoundTrip() {
        Fixture f;
        QVERIFY(f.init(false));
        QJsonObject o;
        QCOMPARE(f.call("GET", "/config", &o), 200);
        QVERIFY(o.value("sources").toArray().isEmpty());
        QVERIFY(o.value("pathMap").toArray().isEmpty());
        QVERIFY(o.value("defaults").isObject());
        QVERIFY(o.value("resolver").isObject());
        QVERIFY(o.value("asrun").isObject());
        // bad configs change nothing
        QJsonObject bad;
        QCOMPARE(f.call("PUT", "/config", &bad, QJsonObject{{"sources", QJsonArray{QJsonObject{{"id", "a"}}, QJsonObject{{"id", "A"}}}}}), 400);
        QVERIFY(bad.value("error").toString().contains("used twice"));
        QCOMPARE(f.call("PUT", "/config", &bad, QJsonObject{{"sources", QJsonArray{QJsonObject{{"id", "bad id!"}}}}}), 400);
        QCOMPARE(f.call("PUT", "/config", &bad, QJsonObject{{"sources", QJsonArray{QJsonObject{{"id", "m"}, {"read", "ftp"}}}}}), 400);
        QCOMPARE(f.call("PUT", "/config", &bad, QJsonObject{{"sources", QJsonArray{QJsonObject{{"id", "m"}, {"read", "mesh"}}}}}), 400);   // no node
        QCOMPARE(f.call("PUT", "/config", &bad, QJsonObject{{"sources", QJsonArray{QJsonObject{{"id", "m"}, {"tz", "Mars/Olympus"}}}}}), 400);
        QCOMPARE(f.call("PUT", "/config", &bad, QJsonObject{{"pathMap", QJsonArray{QJsonObject{{"to", "x"}}}}}), 400);
        QCOMPARE(f.call("PUT", "/config", &bad, QJsonObject{{"defaults", QJsonObject{{"mode", "monthly"}}}}), 400);   // weekly is an alias of grid
        QCOMPARE(f.call("PUT", "/config", &bad, QJsonObject{{"asrun", QJsonObject{{"tz", "Nowhere/Void"}}}}), 400);
        QCOMPARE(f.call("PUT", "/config", &bad, QJsonObject{{"resolver", "x"}}), 400);
        QCOMPARE(f.call("GET", "/config", &o), 200);
        QVERIFY(o.value("sources").toArray().isEmpty());
        QVERIFY(f.broadcasts.isEmpty());
        // a good one is stored whole and read back with its defaults
        QVERIFY(f.configure());
        QCOMPARE(f.call("GET", "/config", &o), 200);
        QCOMPARE(o.value("sources").toArray().size(), 2);
        QCOMPARE(o.value("sources").toArray()[1].toObject().value("node").toString(), S("n1"));
        QCOMPARE(o.value("pathMap").toArray().size(), 1);
        QCOMPARE(o.value("defaults").toObject().value("timeZone").toString(), S("UTC"));
        QCOMPARE(o.value("asrun").toObject().value("tz").toString(), S("America/New_York"));
        QVERIFY(!o.value("updatedAt").toString().isEmpty());
        QCOMPARE(f.svc->source("FX").value("id").toString(), S("fx"));   // case-insensitive lookup
        QVERIFY(f.svc->source("nope").isEmpty());
        QCOMPARE(f.broadcasts.size(), 1);
        QCOMPARE(f.broadcasts[0].value("event").toString(), S("config"));
        QCOMPARE(f.store.settingJson("masterplay.config").value("sources").toArray().size(), 2);
        // a channel in the defaults is dropped (the import names it), the rest kept
        QString e;
        f.svc->setConfig(QJsonObject{{"defaults", QJsonObject{{"channel", "main"}, {"missing", "keepPath"}}}}, &e);
        QVERIFY2(e.isEmpty(), qPrintable(e));
        QCOMPARE(f.svc->config().value("defaults").toObject().value("missing").toString(), S("keepPath"));
        QVERIFY(!f.svc->config().value("defaults").toObject().contains("channel"));
    }

    void parseRoute() {
        Fixture f;
        QVERIFY(f.init(false));
        QVERIFY(f.configure());
        const QByteArray bytes = readAll(fx(S("import_times.in")));
        const Document ref = parse(bytes);
        QJsonObject o;
        // pasted text
        QCOMPARE(f.call("POST", "/parse", &o, QJsonObject{{"text", QString::fromUtf8(bytes)}, {"name", "pasted-times.in"}}), 200);
        QCOMPARE(o.value("read").toString(), S("text"));
        QCOMPARE(o.value("name").toString(), S("pasted-times.in"));
        QCOMPARE(o.value("file").toString(), QString());
        QCOMPARE(o.value("events").toArray().size(), ref.events.size());
        QCOMPARE(o.value("stats").toObject().value("events").toInt(), ref.events.size());
        QVERIFY(o.value("roundTrip").toBool());
        QVERIFY(o.value("instructionStats").isObject());
        QCOMPARE(o.value("fps").toInt(), 30);
        QVERIFY(!o.contains("raw"));
        // the source + a relative path, with entries
        QCOMPARE(f.call("POST", "/parse", &o, QJsonObject{{"source", "fx"}, {"path", "import_times.in"}, {"withEntries", true}, {"fps", 25}}), 200);
        QCOMPARE(o.value("read").toString(), S("fs"));
        QCOMPARE(o.value("file").toString(), fx(S("import_times.in")));
        QCOMPARE(o.value("source").toString(), S("fx"));
        QCOMPARE(o.value("bytes").toInt(), bytes.size());
        QCOMPARE(o.value("fps").toInt(), 25);
        QCOMPARE(o.value("events").toArray().size(), ref.events.size());
        QVERIFY(o.value("roundTrip").toBool());
        // an absolute path without a source
        QCOMPARE(f.call("POST", "/parse", &o, QJsonObject{{"path", fx(S("Saturday_0200.in"))}}), 200);
        QVERIFY(o.value("roundTrip").toBool());
        QCOMPARE(o.value("name").toString(), S("Saturday_0200.in"));
        // errors
        QCOMPARE(f.call("POST", "/parse", &o, QJsonObject{}), 400);
        QCOMPARE(f.call("POST", "/parse", &o, QJsonObject{{"text", "x"}, {"path", "y.in"}}), 400);
        QCOMPARE(f.call("POST", "/parse", &o, QJsonObject{{"path", "relative.in"}}), 400);
        QCOMPARE(f.call("POST", "/parse", &o, QJsonObject{{"source", "fx"}, {"path", "..\\outside.in"}}), 400);
        QCOMPARE(f.call("POST", "/parse", &o, QJsonObject{{"source", "nope"}, {"path", "a.in"}}), 404);
        QCOMPARE(f.call("POST", "/parse", &o, QJsonObject{{"source", "fx"}, {"path", "missing.in"}}), 404);
        QVERIFY(o.value("error").toString().contains("missing.in"));
        QCOMPARE(f.svc->pending(), 0);
    }

    void browseFs() {
        Fixture f;
        QVERIFY(f.init(false));
        QVERIFY(f.configure());
        QJsonObject o;
        QCOMPARE(f.call("GET", "/browse", &o, {}, Query{{"source", "fx"}}), 200);
        QCOMPARE(o.value("read").toString(), S("fs"));
        QCOMPARE(o.value("source").toString(), S("fx"));
        QCOMPARE(o.value("dir").toString(), fxDir());
        QVERIFY(!o.value("parent").toString().isEmpty());
        const QJsonArray entries = o.value("entries").toArray();
        QVERIFY(entries.size() >= 10);
        int inFiles = 0;
        QJsonObject times;
        for (const QJsonValue& v : entries) {
            const QJsonObject e = v.toObject();
            if (e.value("isDir").toBool()) continue;
            ++inFiles;
            QVERIFY2(e.value("name").toString().endsWith(".in", Qt::CaseInsensitive), qPrintable(e.value("name").toString()));
            QVERIFY(e.value("size").toDouble() > 0);
            QVERIFY(!e.value("mtime").toString().isEmpty());
            if (e.value("name").toString() == S("import_times.in")) times = e;
        }
        QCOMPARE(inFiles, o.value("files").toInt());
        QVERIFY(!times.isEmpty());
        QCOMPARE(times.value("path").toString(), fx(S("import_times.in")));
        QVERIFY(times.value("dated").isNull());
        QCOMPARE(times.value("quick").toObject().value("events").toInt(), parse(readAll(fx(S("import_times.in")))).events.size());
        QCOMPARE(times.value("quick").toObject().value("firstStamp").toString(), S("0000-00-00T00:00:00"));
        // no quick stats on request; a folder with daily names; a sub folder; errors
        QCOMPARE(f.call("GET", "/browse", &o, {}, Query{{"source", "fx"}, {"quick", "0"}}), 200);
        for (const QJsonValue& v : o.value("entries").toArray()) QVERIFY(!v.toObject().contains("quick"));
        QTemporaryDir daily;
        QVERIFY(daily.isValid());
        QDir(daily.path()).mkdir("sub");
        for (const char* n : {"2025-09-08.in", "MART_20250909.in", "notes.txt", "weekly.in", "sub/inner.in"}) { QFile w(daily.filePath(QLatin1String(n))); QVERIFY(w.open(QIODevice::WriteOnly)); w.write("0000-00-00T00:00:00\tPlayMode=1\r\nD:\\x.mpg\r\n"); }
        QCOMPARE(f.call("GET", "/browse", &o, {}, Query{{"dir", QDir::toNativeSeparators(daily.path())}}), 200);
        QCOMPARE(o.value("dirs").toInt(), 1);
        QCOMPARE(o.value("files").toInt(), 3);   // notes.txt is not listed
        QCOMPARE(o.value("dated").toInt(), 2);
        QCOMPARE(o.value("entries").toArray()[0].toObject().value("name").toString(), S("sub"));   // folders first
        QCOMPARE(o.value("entries").toArray()[1].toObject().value("dated").toString(), S("2025-09-08"));
        QCOMPARE(o.value("entries").toArray()[2].toObject().value("dated").toString(), S("2025-09-09"));
        QVERIFY(o.value("entries").toArray()[3].toObject().value("dated").isNull());
        QCOMPARE(o.value("entries").toArray()[1].toObject().value("quick").toObject().value("media").toInt(), 1);
        QCOMPARE(svc::MpService::datedName(S("x_2025.09.08_y.in")), QDate(2025, 9, 8));
        QVERIFY(!svc::MpService::datedName(S("20251340.in")).isValid());
        QVERIFY(!svc::MpService::datedName(S("12025090812.in")).isValid());
        QCOMPARE(f.call("GET", "/browse", &o, {}, Query{{"dir", QDir::toNativeSeparators(daily.path())}, {"source", ""}}), 200);
        QCOMPARE(f.call("GET", "/browse", &o, {}, Query{{"dir", QDir::toNativeSeparators(daily.filePath("nope"))}}), 404);
        QCOMPARE(f.call("GET", "/browse", &o, {}, Query{{"source", "zz"}}), 404);
        QCOMPARE(f.call("GET", "/browse", &o, {}, Query{{"source", "fx"}, {"sub", "..\\.."}}), 400);
        QCOMPARE(f.call("GET", "/browse", &o, {}, Query{}), 400);
        // a sub folder of a source, through the source's folder
        QString e;
        f.svc->setConfig(QJsonObject{{"sources", QJsonArray{QJsonObject{{"id", "d"}, {"playlistDir", QDir::toNativeSeparators(daily.path())}}}}}, &e);
        QVERIFY(e.isEmpty());
        QCOMPARE(f.call("GET", "/browse", &o, {}, Query{{"source", "d"}, {"sub", "sub"}}), 200);
        QCOMPARE(o.value("files").toInt(), 1);
        QCOMPARE(o.value("entries").toArray()[0].toObject().value("name").toString(), S("inner.in"));
    }

    void importDryRunApplyUndo() {
        Fixture f;
        QVERIFY(f.init(false));
        QVERIFY(f.configure());
        QJsonObject o;
        // errors before anything is read
        QCOMPARE(f.call("POST", "/import", &o, QJsonObject{{"source", "fx"}, {"path", "import_times.in"}}), 400);           // no channel
        QVERIFY(o.value("error").toString().contains("channel required"));
        QCOMPARE(f.call("POST", "/import", &o, importBody("2025-09-08", "ghost")), 400);                                      // unknown channel
        QCOMPARE(f.call("POST", "/import", &o, QJsonObject{{"source", "fx"}, {"path", "import_times.in"}, {"channel", "mptest"}, {"mode", "dates"}}), 400);   // dates without a range
        QCOMPARE(f.call("POST", "/import", &o, QJsonObject{{"source", "fx"}, {"path", "nope.in"}, {"channel", "mptest"}}), 404);
        // dry run by default: nothing written, the plan with its slots
        const quint64 rev = f.store.revision();
        QCOMPARE(f.call("POST", "/import", &o, importBody("2025-09-08")), 200);
        QVERIFY(o.value("dryRun").toBool());
        QCOMPARE(o.value("source").toObject().value("read").toString(), S("fs"));
        QJsonObject plan = o.value("plan").toObject();
        QCOMPARE(plan.value("channel").toString(), S("mptest"));
        QCOMPARE(plan.value("mode").toString(), S("dates"));
        QCOMPARE(plan.value("slots").toObject().size(), 24);
        QCOMPARE(plan.value("slotOrder").toArray().size(), 24);
        QVERIFY(plan.value("onAirSlots").toArray().isEmpty());
        QVERIFY(plan.value("errors").toArray().isEmpty());
        QCOMPARE(plan.value("options").toObject().value("nowMs").toDouble(), double(kNow));
        QVERIFY(!plan.contains("placements"));
        QCOMPARE(f.store.revision(), rev);
        QCOMPARE(f.store.templateSlots("mptest").size(), 0);
        QVERIFY(f.afterSaveCalls == 0 && f.broadcasts.size() == 1);   // the config broadcast only
        // slots left out, placements in; a client clock is ignored
        QJsonObject b = importBody("2025-09-08");
        b["withSlots"] = false;
        b["withPlacements"] = true;
        b["options"] = QJsonObject{{"nowMs", 1.0}, {"missing", "ingest"}};
        QCOMPARE(f.call("POST", "/import", &o, b), 200);
        plan = o.value("plan").toObject();
        QVERIFY(!plan.contains("slots"));
        QVERIFY(plan.value("placements").toArray().size() > 20);
        QCOMPARE(plan.value("options").toObject().value("nowMs").toDouble(), double(kNow));
        QCOMPARE(plan.value("options").toObject().value("missing").toString(), S("ingest"));
        QCOMPARE(plan.value("ingest").toArray().size(), 1);   // D:\Media\Missing\Gone.mpg
        // on air: the day holding now -> 409, nothing written
        QJsonObject today = importBody("2025-09-01");
        today["dryRun"] = false;
        QCOMPARE(f.call("POST", "/import", &o, today), 409);
        QVERIFY2(o.value("error").toString().contains("on air") || o.value("error").toString().contains("allowOnAir"), qPrintable(o.value("error").toString()));
        QCOMPARE(f.store.revision(), rev);
        QCOMPARE(f.afterSaveCalls, 0);
        // apply a week ahead (missing = ingest with file probes: the missing file is listed, not found on disk, so skipped)
        QJsonObject apply = importBody("2025-09-08");
        apply["dryRun"] = false;
        apply["by"] = "test";
        apply["options"] = QJsonObject{{"missing", "ingest"}};
        apply["resolver"] = QJsonObject{{"probe", true}};
        QCOMPARE(f.call("POST", "/import", &o, apply), 200);
        QVERIFY(!o.value("dryRun").toBool());
        QVERIFY(o.value("ok").toBool());
        const QString importId = o.value("importId").toString();
        QVERIFY(importId.startsWith("mp"));
        QCOMPARE(o.value("written").toArray().size(), 24);
        QCOMPARE(o.value("created").toArray().size(), 24);
        QCOMPARE(o.value("entry").toObject().value("by").toString(), S("test"));
        QVERIFY(o.value("plan").isObject() && !o.value("plan").toObject().contains("slots"));
        QCOMPARE(o.value("ingest").toObject().value("requested").toInt(), 1);
        QCOMPARE(o.value("ingest").toObject().value("enqueued").toArray().size(), 0);
        QCOMPARE(o.value("ingest").toObject().value("skipped").toArray().size(), 1);
        QCOMPARE(o.value("ingest").toObject().value("skipped").toArray()[0].toObject().value("why").toString(), S("not found"));
        QVERIFY(f.enqueued.isEmpty());
        QCOMPARE(f.afterSaveCalls, 1);
        QVERIFY(f.store.revision() > rev);
        bool found = false;
        const sched::HourSlot s9 = f.store.slot("mptest", "2025-09-08-9", &found);
        QVERIFY(found);
        QVERIFY(!s9.items.isEmpty());
        QCOMPARE(s9.showBlock.value("masterplay").toObject().value("importId").toString(), importId);
        QCOMPARE(f.broadcasts.last().value("event").toString(), S("applied"));
        QCOMPARE(f.broadcasts.last().value("importId").toString(), importId);
        QCOMPARE(f.broadcasts.last().value("written").toInt(), 24);
        // a plan whose missing file is found on disk goes to the ingest manager
        QTemporaryDir media;
        QVERIFY(media.isValid());
        { QFile w(media.filePath("Gone.mpg")); QVERIFY(w.open(QIODevice::WriteOnly)); w.write("x"); }
        QJsonObject again = importBody("2025-09-15");
        again["dryRun"] = false;
        again["options"] = QJsonObject{{"missing", "ingest"}};
        again["pathMap"] = QJsonArray{QJsonObject{{"from", S(R"(D:\Media\Missing)")}, {"to", QDir::toNativeSeparators(media.path())}}};
        again["resolver"] = QJsonObject{{"probe", true}};
        QCOMPARE(f.call("POST", "/import", &o, again), 200);
        QCOMPARE(o.value("ingest").toObject().value("enqueued").toArray().size(), 1);
        QCOMPARE(f.enqueued.size(), 1);
        QCOMPARE(f.enqueued[0].value("path").toString(), QDir::toNativeSeparators(media.filePath("Gone.mpg")));
        QCOMPARE(f.enqueued[0].value("meta").toObject().value("origin").toString(), S("masterplay"));
        QCOMPARE(f.enqueued[0].value("meta").toObject().value("importId").toString(), o.value("importId").toString());
        const QString secondId = o.value("importId").toString();
        // history
        QCOMPARE(f.call("GET", "/imports", &o, {}, Query{{"limit", "1"}}), 200);
        QCOMPARE(o.value("imports").toArray().size(), 1);
        QCOMPARE(o.value("imports").toArray()[0].toObject().value("id").toString(), secondId);
        QCOMPARE(f.call("GET", "/imports", &o), 200);
        QCOMPARE(o.value("count").toInt(), 2);
        QCOMPARE(f.call("GET", "/imports/" + importId, &o), 200);
        QCOMPARE(o.value("entry").toObject().value("state").toString(), S("applied"));
        QVERIFY(o.value("undoable").toBool());
        QCOMPARE(o.value("backup").toObject().value("slots").toInt(), 24);
        QCOMPARE(f.call("GET", "/imports/nope", &o), 404);
        QCOMPARE(f.call("GET", "", &o), 200);
        QCOMPARE(o.value("imports").toInt(), 1);   // the newest entry only
        // undo
        const int saves = f.afterSaveCalls;
        QCOMPARE(f.call("POST", "/import/" + importId + "/undo", &o, QJsonObject{{"by", "test"}}), 200);
        QCOMPARE(o.value("deleted").toArray().size(), 24);
        QCOMPARE(o.value("restored").toArray().size(), 0);
        QCOMPARE(f.afterSaveCalls, saves + 1);
        f.store.slot("mptest", "2025-09-08-9", &found);
        QVERIFY(!found);
        QCOMPARE(f.broadcasts.last().value("event").toString(), S("undone"));
        QCOMPARE(f.call("POST", "/import/" + importId + "/undo", &o), 409);   // already undone
        QCOMPARE(f.call("POST", "/import/nope/undo", &o), 404);
        QCOMPARE(f.call("GET", "/imports/" + importId, &o), 200);
        QCOMPARE(o.value("entry").toObject().value("state").toString(), S("undone"));
        QCOMPARE(f.svc->pending(), 0);
    }

    void exportRoutes() {
        Fixture f;
        QVERIFY(f.init(false));
        QVERIFY(f.configure());
        QJsonObject o;
        QJsonObject apply = importBody("2025-09-08");
        apply["dryRun"] = false;
        QCOMPARE(f.call("POST", "/import", &o, apply), 200);
        // errors
        QCOMPARE(f.call("POST", "/export", &o, QJsonObject{}), 400);
        QCOMPARE(f.call("POST", "/export", &o, QJsonObject{{"channel", "ghost"}}), 400);
        QCOMPARE(f.call("POST", "/export", &o, QJsonObject{{"channel", "mptest"}, {"mode", "dates"}}), 400);
        QCOMPARE(f.call("POST", "/export", &o, QJsonObject{{"channel", "mptest"}, {"mode", "dates"}, {"from", "2025-09-08"}, {"to", "2025-09-01"}}), 400);
        QCOMPARE(f.call("POST", "/export", &o, QJsonObject{{"channel", "mptest"}, {"encoding", "ebcdic"}}), 400);
        // the dated day back out
        QCOMPARE(f.call("POST", "/export", &o, QJsonObject{{"channel", "mptest"}, {"mode", "dates"}, {"from", "2025-09-08"}}), 200);
        QVERIFY(o.value("ok").toBool());
        QCOMPARE(o.value("filename").toString(), S("mptest-2025-09-08.in"));
        QCOMPARE(o.value("slots").toInt(), 24);
        const QString text = o.value("text").toString();
        QVERIFY(text.contains("2025-09-08T08:30:00"));
        QVERIFY(text.contains(S(R"(D:\Media\Prog\News Hour.mpg)")));
        QVERIFY(!o.contains("bytesBase64"));
        const Document back = parse(text.toUtf8());
        QVERIFY(back.events.size() >= 8);
        // the bytes, with the BOM, in legacy ANSI style
        QCOMPARE(f.call("POST", "/export", &o, QJsonObject{{"channel", "mptest"}, {"mode", "dates"}, {"from", "2025-09-08"}, {"to", "2025-09-08"}, {"dateStyle", "legacy"}, {"encoding", "ansi"}, {"eol", "lf"}, {"withText", false}, {"download", true}}), 200);
        QVERIFY(!o.contains("text"));
        const QByteArray bytes = QByteArray::fromBase64(o.value("bytesBase64").toString().toLatin1());
        QVERIFY(!bytes.startsWith("\xEF\xBB\xBF"));
        QVERIFY(bytes.contains("08/09/2025 08:30:00"));
        QVERIFY(!bytes.contains("\r\n"));
        QCOMPARE(o.value("bytes").toInt(), bytes.size());
        // ?download=1 on the POST works the same
        QCOMPARE(f.call("POST", "/export", &o, QJsonObject{{"channel", "mptest"}, {"mode", "dates"}, {"from", "2025-09-08"}}, Query{{"download", "1"}}), 200);
        QVERIFY(QByteArray::fromBase64(o.value("bytesBase64").toString().toLatin1()).startsWith("\xEF\xBB\xBF"));
        // the grid (empty for this channel) exports nothing but is fine
        QCOMPARE(f.call("POST", "/export", &o, QJsonObject{{"channel", "mptest"}}), 200);
        QCOMPARE(o.value("filename").toString(), S("mptest-grid.in"));
        QCOMPARE(o.value("slots").toInt(), 0);
        // the raw file route: query options, a file with its name
        svc::FileReply fr;
        bool done = false;
        QEventLoop loop;
        f.svc->exportFile(Query{{"channel", "mptest"}, {"mode", "dates"}, {"from", "2025-09-08"}, {"token", "secret"}}, [&](const svc::FileReply& r) { fr = r; done = true; loop.quit(); });
        QTimer::singleShot(30000, &loop, &QEventLoop::quit);
        if (!done) loop.exec();
        QVERIFY(done);
        QCOMPARE(fr.status, 200);
        QCOMPARE(fr.filename, S("mptest-2025-09-08.in"));
        QVERIFY(fr.bytes.startsWith("\xEF\xBB\xBF"));
        QCOMPARE(fr.bytes, text.toUtf8().prepend("\xEF\xBB\xBF"));
        done = false;
        f.svc->exportFile(Query{{"channel", "ghost"}}, [&](const svc::FileReply& r) { fr = r; done = true; loop.quit(); });
        QTimer::singleShot(30000, &loop, &QEventLoop::quit);
        if (!done) loop.exec();
        QVERIFY(done);
        QCOMPARE(fr.status, 400);
        QCOMPARE(fr.contentType, QByteArray("application/json"));
        QVERIFY(QJsonDocument::fromJson(fr.bytes).object().value("error").toString().contains("ghost"));
    }

    void asRunImport() {
        Fixture f;
        QVERIFY(f.init(false));
        QVERIFY(f.configure());
        QJsonObject o;
        // the source's log by default; Status.log found next to it; dry run writes nothing
        QCOMPARE(f.call("POST", "/asrun/import", &o, QJsonObject{{"source", "fx"}}), 200);
        QVERIFY(o.value("dryRun").toBool());
        QCOMPARE(o.value("channel").toString(), S("mp-fx"));
        QCOMPARE(o.value("tz").toString(), S("America/New_York"));
        QCOMPARE(o.value("source").toObject().value("file").toString(), fx(S("LogFile.log")));
        QCOMPARE(o.value("rows").toObject().value("played").toInt(), 473);
        QCOMPARE(o.value("rows").toObject().value("missed").toInt(), 262);
        QCOMPARE(o.value("rows").toObject().value("commands").toInt(), 0);
        QCOMPARE(o.value("wouldInsert").toInt(), 735);
        QCOMPARE(o.value("inserted").toInt(), 0);
        QVERIFY(o.value("status").toObject().value("attached").toInt() > 0);
        QCOMPARE(o.value("status").toObject().value("file").toString(), fx(S("Status.log")));
        QVERIFY(o.value("status").toObject().value("report").toObject().contains("filesNotFound"));
        QVERIFY(o.value("stats").toObject().value("airings").toInt() > 0);
        QCOMPARE(o.value("sample").toArray().size(), 20);
        QVERIFY(!o.contains("state"));
        QVERIFY(f.svc->asRunState().isEmpty());
        // written: 735 rows, the state recorded; a second run inserts nothing
        QCOMPARE(f.call("POST", "/asrun/import", &o, QJsonObject{{"source", "fx"}, {"dryRun", false}, {"sample", 2}, {"by", "test"}}), 200);
        QVERIFY(!o.value("dryRun").toBool());
        QCOMPARE(o.value("inserted").toInt(), 735);
        QCOMPARE(o.value("duplicates").toInt(), 0);
        QCOMPARE(o.value("sample").toArray().size(), 2);
        QCOMPARE(o.value("state").toObject().value("inserted").toInt(), 735);
        QCOMPARE(o.value("state").toObject().value("by").toString(), S("test"));
        QVERIFY(!o.value("state").toObject().value("lastStamp").toString().isEmpty());
        QCOMPARE(f.svc->asRunState().value("fx").toObject().value("channel").toString(), S("mp-fx"));
        QCOMPARE(f.broadcasts.last().value("event").toString(), S("asrun"));
        QCOMPARE(f.call("GET", "/asrun/state", &o), 200);
        QCOMPARE(o.value("state").toObject().value("fx").toObject().value("inserted").toInt(), 735);
        QCOMPARE(f.call("POST", "/asrun/import", &o, QJsonObject{{"source", "fx"}, {"dryRun", false}}), 200);
        QCOMPARE(o.value("inserted").toInt(), 0);
        QCOMPARE(o.value("duplicates").toInt(), 735);
        // pasted text with a range, commands, another channel, statusText
        const QByteArray log = readAll(fx(S("LogFile.log")));
        QCOMPARE(f.call("POST", "/asrun/import", &o, QJsonObject{{"text", QString::fromUtf8(log)}, {"statusText", QString::fromUtf8(readAll(fx(S("Status.log"))))}, {"includeCommands", true},
                                                                  {"channelAs", "mp-x"}, {"tz", "UTC"}, {"from", "2025-06-19"}, {"to", "2025-06-19T23:59:59"}}), 200);
        QCOMPARE(o.value("channel").toString(), S("mp-x"));
        QCOMPARE(o.value("tz").toString(), S("UTC"));
        QVERIFY(o.value("rows").toObject().value("commands").toInt() > 0);
        QVERIFY(o.value("outOfRange").toInt() > 0);
        QCOMPARE(o.value("status").toObject().value("file").toString(), S("statusText"));
        // errors
        QCOMPARE(f.call("POST", "/asrun/import", &o, QJsonObject{{"text", "x"}, {"tz", "Nowhere/Void"}}), 400);
        QCOMPARE(f.call("POST", "/asrun/import", &o, QJsonObject{{"text", "x"}, {"from", "yesterday"}}), 400);
        QCOMPARE(f.call("POST", "/asrun/import", &o, QJsonObject{{"text", "x"}, {"from", "2025-06-20"}, {"to", "2025-06-19"}}), 400);
        QCOMPARE(f.call("POST", "/asrun/import", &o, QJsonObject{{"source", "fx"}, {"statusPath", "Nope.log"}}), 404);
        QCOMPARE(f.call("POST", "/asrun/import", &o, QJsonObject{{"path", "LogFile.log"}}), 400);   // relative without a source
        QCOMPARE(f.call("POST", "/asrun/import", &o, QJsonObject{}), 400);
    }

    void resolveAndFromIni() {
        Fixture f;
        QVERIFY(f.init(false));
        QVERIFY(f.configure());
        QJsonObject o;
        QCOMPARE(f.call("POST", "/resolve", &o, QJsonObject{}), 400);
        QCOMPARE(f.call("POST", "/resolve", &o, QJsonObject{{"paths", QJsonArray{}}}), 400);
        QCOMPARE(f.call("POST", "/resolve", &o, QJsonObject{{"paths", QJsonArray{S(R"(D:\Media\Prog\Movie.mpg)"), S(R"(P:\Prog\Talk.mpg[00:05:00;00])"), S(R"(D:\Nope\Gone.mpg)"), "mp://in1"}},
                                                             {"pathMap", QJsonArray{QJsonObject{{"from", "P:\\"}, {"to", "D:\\Media\\"}}}}}), 200);
        const QJsonArray r = o.value("results").toArray();
        QCOMPARE(r.size(), 4);
        QCOMPARE(r[0].toObject().value("mediaId").toString(), S("movie"));
        QCOMPARE(r[0].toObject().value("resolvedBy").toString(), S("path"));
        QCOMPARE(r[1].toObject().value("mediaId").toString(), S("talk"));
        QCOMPARE(r[1].toObject().value("mapped").toString(), S(R"(D:\Media\Prog\Talk.mpg)"));
        QCOMPARE(r[1].toObject().value("forcedDurationSec").toDouble(), 300.0);
        QCOMPARE(r[2].toObject().value("resolvedBy").toString(), S("none"));
        QCOMPARE(r[3].toObject().value("kind").toString(), S("live"));
        QCOMPARE(o.value("stats").toObject().value("resolved").toInt(), 2);
        QCOMPARE(o.value("stats").toObject().value("missing").toInt(), 1);
        QCOMPARE(o.value("stats").toObject().value("liveOrUrl").toInt(), 1);
        QCOMPARE(o.value("library").toInt(), 9);
        QCOMPARE(o.value("pathMap").toArray().size(), 1);
        // from-ini: the sample INI as text and by path; never the stored password
        const QByteArray ini = readAll(fx(S("mplay_sample.ini")));
        QVERIFY(ini.contains("redacted"));
        QCOMPARE(f.call("POST", "/config/from-ini", &o, QJsonObject{{"text", QString::fromLatin1(ini)}}), 200);
        QVERIFY(o.value("source").isObject());
        QCOMPARE(o.value("source").toObject().value("read").toString(), S("fs"));
        QVERIFY(o.value("pathMap").isArray());
        QVERIFY(o.value("ini").toObject().value("isMplayIni").toBool());
        QCOMPARE(o.value("input").toObject().value("read").toString(), S("text"));
        QVERIFY(o.value("exists").isObject());
        const QByteArray dumped = QJsonDocument(o).toJson(QJsonDocument::Compact);
        QVERIFY(!dumped.contains("redacted"));
        QVERIFY(!dumped.contains("ApprovalUsers"));
        QCOMPARE(f.call("POST", "/config/from-ini", &o, QJsonObject{{"source", "fx"}, {"path", "mplay_sample.ini"}}), 200);
        QCOMPARE(o.value("input").toObject().value("file").toString(), fx(S("mplay_sample.ini")));
        for (const QJsonValue& v : o.value("pathMap").toArray()) QVERIFY(v.toObject().contains("exists"));
        QCOMPARE(f.call("POST", "/config/from-ini", &o, QJsonObject{{"text", "[Other]\r\nx=1\r\n"}}), 400);
        QCOMPARE(f.call("POST", "/config/from-ini", &o, QJsonObject{{"source", "fx"}, {"path", "nope.ini"}}), 404);
    }

    void linksCrud() {
        Fixture f;
        QVERIFY(f.init(false));
        QJsonObject o;
        QCOMPARE(f.call("GET", "/links", &o), 200);
        QCOMPARE(o.value("count").toInt(), 0);
        QCOMPARE(f.call("POST", "/links", &o, QJsonObject{{"channel", "mptest"}}), 400);
        QCOMPARE(f.call("POST", "/links", &o, QJsonObject{{"channel", "mptest"}, {"subPath", "x.in"}, {"id", "bad id"}}), 400);
        QCOMPARE(f.call("POST", "/links", &o, QJsonObject{{"channel", "mptest"}, {"subPath", "C:/Sync/NEWS HUB/sub_news.in"}, {"slots", QJsonArray{"4-12", "4-13", 7}}, {"source", "fx"}}), 200);
        QVERIFY(o.value("created").toBool());
        const QJsonObject link = o.value("link").toObject();
        const QString id = link.value("id").toString();
        QVERIFY(id.startsWith("lnk-"));
        QCOMPARE(link.value("subPath").toString(), S(R"(C:\Sync\NEWS HUB\sub_news.in)"));
        QCOMPARE(link.value("slots").toArray().size(), 2);
        QVERIFY(link.value("enabled").toBool());
        QCOMPARE(link.value("source").toString(), S("fx"));
        QVERIFY(!link.value("createdAt").toString().isEmpty());
        QCOMPARE(o.value("links").toArray().size(), 1);
        QCOMPARE(f.broadcasts.last().value("event").toString(), S("links"));
        // update by id, keep the created stamp
        QCOMPARE(f.call("POST", "/links", &o, QJsonObject{{"id", id}, {"channel", "mptest"}, {"subPath", "sub_news.in"}, {"enabled", false}, {"lastMtime", "2025-09-08T10:00:00Z"}}), 200);
        QVERIFY(!o.value("created").toBool());
        QCOMPARE(o.value("links").toArray().size(), 1);
        QCOMPARE(o.value("link").toObject().value("enabled").toBool(), false);
        QCOMPARE(o.value("link").toObject().value("createdAt").toString(), link.value("createdAt").toString());
        QCOMPARE(o.value("link").toObject().value("lastMtime").toString(), S("2025-09-08T10:00:00Z"));
        QCOMPARE(f.call("POST", "/links", &o, QJsonObject{{"id", "manual-1"}, {"channel", "mptest"}, {"subPath", "b.in"}}), 200);
        QCOMPARE(f.svc->links().size(), 2);
        QCOMPARE(f.store.settingJson("masterplay.links").value("links").toArray().size(), 2);
        QCOMPARE(f.call("DELETE", "/links/" + id, &o), 200);
        QCOMPARE(o.value("deleted").toString(), id);
        QCOMPARE(o.value("links").toArray().size(), 1);
        QCOMPARE(f.call("DELETE", "/links/" + id, &o), 404);
        QCOMPARE(f.call("GET", "/links", &o), 200);
        QCOMPARE(o.value("links").toArray()[0].toObject().value("id").toString(), S("manual-1"));
    }

    void meshReads() {
        Fixture f;
        QVERIFY(f.init(true));
        QVERIFY(f.configure());
        QJsonObject o;
        // browse the mock bridge: folders and .in files only, the token sent
        QCOMPARE(f.call("GET", "/browse", &o, {}, Query{{"source", "mesh"}}), 200);
        QCOMPARE(o.value("read").toString(), S("mesh"));
        QCOMPARE(o.value("node").toString(), S("n1"));
        QCOMPARE(o.value("dir").toString(), fxDir());
        QVERIFY(o.value("files").toInt() >= 10);
        for (const QJsonValue& v : o.value("entries").toArray()) {
            const QJsonObject e = v.toObject();
            QVERIFY(e.value("isDir").toBool() || e.value("name").toString().endsWith(".in"));
            QVERIFY(!e.contains("quick"));
        }
        QVERIFY(f.bridge.requests.last().startsWith("/api/browse?"));
        QVERIFY(f.bridge.requests.last().contains("node=n1"));
        QCOMPARE(f.bridge.auth.last(), S("Bearer tok"));
        // a node and an absolute dir without a source
        QCOMPARE(f.call("GET", "/browse", &o, {}, Query{{"dir", fxDir()}, {"node", "n2"}}), 200);
        QVERIFY(f.bridge.requests.last().contains("node=n2"));
        QCOMPARE(f.call("GET", "/browse", &o, {}, Query{{"dir", fxDir() + "\\nope"}, {"node", "n2"}}), 404);
        // parse through the mesh
        QCOMPARE(f.call("POST", "/parse", &o, QJsonObject{{"source", "mesh"}, {"path", "sub_week.in"}}), 200);
        QCOMPARE(o.value("read").toString(), S("mesh"));
        QCOMPARE(o.value("node").toString(), S("n1"));
        QCOMPARE(o.value("file").toString(), fx(S("sub_week.in")));
        QVERIFY(o.value("roundTrip").toBool());
        QVERIFY(f.bridge.requests.last().startsWith("/api/download?"));
        QCOMPARE(f.call("POST", "/parse", &o, QJsonObject{{"source", "mesh"}, {"path", "nope.in"}}), 404);
        // an import whose !InsertPlaylist targets are fetched from the mesh through the path rule
        f.bridge.requests.clear();
        QCOMPARE(f.call("POST", "/import", &o, QJsonObject{{"source", "mesh"}, {"path", "sub_week.in"}, {"channel", "mptest"}, {"mode", "grid"}, {"withSlots", false}}), 200);
        const QJsonObject plan = o.value("plan").toObject();
        QCOMPARE(o.value("source").toObject().value("read").toString(), S("mesh"));
        QVERIFY(plan.value("errors").toArray().isEmpty());
        const QJsonArray sources = plan.value("sources").toArray();
        QVERIFY(sources.size() >= 3);
        QStringList fetched;
        for (const QJsonValue& v : sources) if (v.toObject().value("ok").toBool()) fetched << res::fileName(v.toObject().value("path").toString()).toLower();
        QVERIFY2(fetched.contains("sub_news.in"), qPrintable(fetched.join(' ')));
        QVERIFY2(fetched.contains("sub_news_inner.in"), qPrintable(fetched.join(' ')));
        int downloads = 0;
        for (const QString& r : f.bridge.requests) if (r.startsWith("/api/download?") && r.contains("sub_news")) ++downloads;
        QVERIFY2(downloads >= 2, qPrintable(f.bridge.requests.join('\n')));
        // the bridge down: 502 / 404, never a hang
        f.bridge.fail404 = true;
        QCOMPARE(f.call("POST", "/parse", &o, QJsonObject{{"source", "mesh"}, {"path", "sub_week.in"}}), 404);
        f.bridge.server.close();
        QCOMPARE(f.call("GET", "/browse", &o, {}, Query{{"source", "mesh"}}), 502);
        QVERIFY2(o.value("error").toString().contains("mesh browse failed"), qPrintable(o.value("error").toString()));
        QCOMPARE(f.svc->pending(), 0);
    }
};

QTEST_GUILESS_MAIN(TestMpService)
#include "test_mpservice.moc"
